/** @file

  Accept a dedicated runtime-shared Gunyah memory pool and make it the only
  ordinary memory exposed to the subsequently booted Linux kernel.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>

#include <Guid/FdtHob.h>
#include <IndustryStandard/Virtio.h>
#include <Library/ArmHvcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/VirtioLib.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/VirtioDevice.h>

#define VIRTIO_SUBSYSTEM_GUNYAH_ACCEPT  60

#define VGA_REQUEST_QUEUE     0
#define VGA_COMPLETION_QUEUE  1
#define VGA_POOL_QUEUE        2
#define VGA_QUEUE_COUNT       3
#define VGA_OP_ACCEPT         1
#define VGP_OP_SHARE          1

#define GH_HCALL_MSGQ_SEND  0xC600801B
#define GH_HCALL_MSGQ_RECV  0xC600801C
#define GH_MSGQ_TX_PUSH     BIT0
#define GH_ERROR_OK         0

#define GH_RM_RPC_API           0x21
#define GH_RM_RPC_TYPE_REQUEST  0x01
#define GH_RM_RPC_TYPE_REPLY    0x02
#define GH_RM_RPC_TYPE_MASK     0x03
#define GH_RM_RPC_MEM_ACCEPT    0x51000011

#define GH_RM_MEM_TYPE_NORMAL                 0
#define GH_RM_TRANS_TYPE_SHARE                2
#define GH_RM_MEM_ACCEPT_MAP_IPA_CONTIGUOUS   BIT4
#define GH_RM_MEM_ACCEPT_DONE                 BIT7

#define GH_RM_MSGQ_MSG_SIZE  240
#define PRELOAD_TIMEOUT_US   (30 * 1000 * 1000)
#define PRELOAD_POLL_US      100
#define PRELOAD_MIN_SIZE     (2 * 1024 * 1024)

#define LINUX_EIO     5
#define LINUX_EINVAL  22

#pragma pack (1)
typedef struct {
  UINT32    RequestId;
  UINT32    Operation;
  UINT32    Handle;
  UINT32    Flags;
  UINT64    GuestAddress;
  UINT64    Size;
} VGA_REQUEST;

typedef struct {
  UINT32    RequestId;
  INT32     Result;
} VGA_COMPLETION;

typedef struct {
  UINT32    RequestId;
  UINT32    Operation;
  UINT32    PoolId;
  UINT32    Flags;
  UINT64    Offset;
  UINT64    Length;
} VGP_REQUEST;

typedef struct {
  UINT32    RequestId;
  INT32     Result;
  UINT64    Extra;
} VGP_RESPONSE;

typedef struct {
  VGA_REQUEST       AcceptRequest;
  VGA_COMPLETION    AcceptCompletion;
  VGP_REQUEST       PoolRequest;
  VGP_RESPONSE      PoolResponse;
} PRELOAD_SHARED_BUFFERS;
#pragma pack ()

STATIC_ASSERT (sizeof (VGA_REQUEST) == 32, "invalid accept request size");
STATIC_ASSERT (sizeof (VGA_COMPLETION) == 8, "invalid accept completion size");
STATIC_ASSERT (sizeof (VGP_REQUEST) == 32, "invalid pool request size");
STATIC_ASSERT (sizeof (VGP_RESPONSE) == 16, "invalid pool response size");

typedef struct {
  VRING      Ring;
  VOID       *RingMapping;
  BOOLEAN    Initialized;
  BOOLEAN    Submitted;
  UINT16     UsedSlot;
} PRELOAD_QUEUE;

typedef struct {
  VOID      *Fdt;
  INT32     Node;
  UINT64    Base;
  UINT64    Size;
  UINT32    PoolId;
  UINT64    TxCapability;
  UINT64    RxCapability;
} PRELOAD_POOL;

typedef struct {
  VOID       *Fdt;
  UINT64     Base;
  UINT64     Size;
  EFI_EVENT  ReadyToBootEvent;
} PRELOAD_LINUX_MEMORY;

STATIC BOOLEAN  mPreloadAttempted;
STATIC UINT16   mRpcSequence = 1;
STATIC PRELOAD_LINUX_MEMORY  mLinuxMemory;

STATIC
UINT64
ReadBe64 (
  IN CONST VOID  *Buffer
  )
{
  return SwapBytes64 (ReadUnaligned64 (Buffer));
}

STATIC
UINT32
ReadBe32 (
  IN CONST VOID  *Buffer
  )
{
  return SwapBytes32 (ReadUnaligned32 (Buffer));
}

STATIC
VOID
WriteBe64 (
  OUT VOID   *Buffer,
  IN  UINT64 Value
  )
{
  WriteUnaligned64 (Buffer, SwapBytes64 (Value));
}

STATIC
EFI_STATUS
GetFirmwareFdt (
  OUT VOID  **Fdt
  )
{
  VOID  *Hob;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return EFI_NOT_FOUND;
  }

  *Fdt = (VOID *)(UINTN)*(CONST UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((*Fdt == NULL) || (FdtCheckHeader (*Fdt) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidatePoolInMemoryNode (
  IN CONST VOID  *Fdt,
  IN UINT64      PoolBase,
  IN UINT64      PoolSize
  )
{
  CONST UINT8  *Reg;
  INT32        Length;
  INT32        MemoryNode;
  INT32        Offset;
  UINT64       PoolEnd;

  if (PoolBase > (MAX_UINT64 - PoolSize)) {
    return EFI_COMPROMISED_DATA;
  }

  PoolEnd   = PoolBase + PoolSize;
  MemoryNode = FdtPathOffset (Fdt, "/memory");
  if (MemoryNode < 0) {
    return EFI_NOT_FOUND;
  }

  Reg = FdtGetProp (Fdt, MemoryNode, "reg", &Length);
  if ((Reg == NULL) || (Length <= 0) || ((Length % 16) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Offset = 0; Offset < Length; Offset += 16) {
    UINT64  Base;
    UINT64  Size;

    Base = ReadBe64 (Reg + Offset);
    Size = ReadBe64 (Reg + Offset + sizeof (UINT64));
    if ((Base <= PoolBase) && (Size <= (MAX_UINT64 - Base)) &&
        ((Base + Size) >= PoolEnd))
    {
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
FindPreloadPool (
  OUT PRELOAD_POOL  *Pool
  )
{
  STATIC CONST CHAR8  PoolName[] = "edk2_preload";
  CONST UINT8         *Property;
  EFI_STATUS          Status;
  INT32               Length;
  INT32               ReservedNode;
  INT32               Node;
  INT32               MatchNode;
  INT32               RmNode;
  UINTN               Matches;
  UINT64              PreAlloc;
  UINT64              Step;

  ZeroMem (Pool, sizeof (*Pool));
  Status = GetFirmwareFdt (&Pool->Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ReservedNode = FdtPathOffset (Pool->Fdt, "/reserved-memory");
  if (ReservedNode < 0) {
    return EFI_NOT_FOUND;
  }

  Matches   = 0;
  MatchNode = -1;
  FdtForEachSubnode (Node, Pool->Fdt, ReservedNode) {
    Property = FdtGetProp (Pool->Fdt, Node, "droidvm,pool-name", &Length);
    if ((Property != NULL) && (Length == sizeof (PoolName)) &&
        (CompareMem (Property, PoolName, sizeof (PoolName)) == 0))
    {
      MatchNode = Node;
      Matches++;
    }
  }

  if (Matches == 0) {
    return EFI_NOT_FOUND;
  }

  if (Matches != 1) {
    return EFI_COMPROMISED_DATA;
  }

  Property = FdtGetProp (Pool->Fdt, MatchNode, "reg", &Length);
  if ((Property == NULL) || (Length != 16)) {
    return EFI_COMPROMISED_DATA;
  }

  Pool->Base = ReadBe64 (Property);
  Pool->Size = ReadBe64 (Property + sizeof (UINT64));
  // Gunyah accepts a contiguous range in folio-sized units.  The crosvm
  // pool validator guarantees the same 2 MiB granularity; requiring a
  // power-of-two size here would reject valid sizes such as 2050 MiB.
  if ((Pool->Size < PRELOAD_MIN_SIZE) ||
      ((Pool->Size & (PRELOAD_MIN_SIZE - 1)) != 0) ||
      ((Pool->Base & (PRELOAD_MIN_SIZE - 1)) != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Property = FdtGetProp (Pool->Fdt, MatchNode, "no-map", &Length);
  if (Property == NULL) {
    return EFI_COMPROMISED_DATA;
  }

  Property = FdtGetProp (Pool->Fdt, MatchNode, "droidvm,pre-alloc-size", &Length);
  if ((Property == NULL) || (Length != sizeof (UINT64))) {
    return EFI_COMPROMISED_DATA;
  }

  PreAlloc = ReadBe64 (Property);
  Property = FdtGetProp (Pool->Fdt, MatchNode, "droidvm,step-size", &Length);
  if ((Property == NULL) || (Length != sizeof (UINT64))) {
    return EFI_COMPROMISED_DATA;
  }

  Step = ReadBe64 (Property);
  Property = FdtGetProp (Pool->Fdt, MatchNode, "droidvm,pool-id", &Length);
  if ((Property == NULL) || (Length != sizeof (UINT32)) ||
      (PreAlloc != 0) || (Step != Pool->Size))
  {
    return EFI_COMPROMISED_DATA;
  }

  Pool->PoolId = ReadBe32 (Property);
  Status       = ValidatePoolInMemoryNode (Pool->Fdt, Pool->Base, Pool->Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RmNode = FdtNodeOffsetByCompatible (
             Pool->Fdt,
             -1,
             "gunyah-resource-manager"
             );
  if (RmNode < 0) {
    return EFI_NOT_FOUND;
  }

  Property = FdtGetProp (Pool->Fdt, RmNode, "reg", &Length);
  if ((Property == NULL) || (Length < 16)) {
    return EFI_COMPROMISED_DATA;
  }

  Pool->TxCapability = ReadBe64 (Property);
  Pool->RxCapability = ReadBe64 (Property + sizeof (UINT64));
  Pool->Node         = MatchNode;

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: pool=%u base=0x%lx size=0x%lx tx=%lu rx=%lu\n",
    Pool->PoolId,
    Pool->Base,
    Pool->Size,
    Pool->TxCapability,
    Pool->RxCapability
    ));
  return EFI_SUCCESS;
}

STATIC
UINTN
GunyahMessageQueueSend (
  IN UINT64      Capability,
  IN CONST VOID  *Buffer,
  IN UINTN       Size
  )
{
  ARM_HVC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = GH_HCALL_MSGQ_SEND;
  Args.Arg1 = (UINTN)Capability;
  Args.Arg2 = Size;
  Args.Arg3 = (UINTN)Buffer;
  Args.Arg4 = GH_MSGQ_TX_PUSH;
  ArmCallHvc (&Args);
  return Args.Arg0;
}

STATIC
UINTN
GunyahMessageQueueReceive (
  IN  UINT64  Capability,
  OUT VOID    *Buffer,
  IN  UINTN   BufferSize,
  OUT UINTN   *ReceivedSize
  )
{
  ARM_HVC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = GH_HCALL_MSGQ_RECV;
  Args.Arg1 = (UINTN)Capability;
  Args.Arg2 = (UINTN)Buffer;
  Args.Arg3 = BufferSize;
  ArmCallHvc (&Args);
  if (Args.Arg0 == GH_ERROR_OK) {
    *ReceivedSize = Args.Arg1;
  }

  return Args.Arg0;
}

STATIC
EFI_STATUS
GunyahMemAccept (
  IN CONST PRELOAD_POOL  *Pool,
  IN UINT32              Handle
  )
{
  UINT64      TxWords[64 / sizeof (UINT64)];
  UINT64      RxWords[GH_RM_MSGQ_MSG_SIZE / sizeof (UINT64)];
  UINT8       *TxBuffer;
  UINT8       *RxBuffer;
  UINT8       *Cursor;
  UINTN       Offset;
  UINTN       ReceivedSize;
  UINTN       Result;
  UINTN       Attempt;
  UINT16      Sequence;

  TxBuffer = (UINT8 *)TxWords;
  RxBuffer = (UINT8 *)RxWords;
  ZeroMem (TxBuffer, sizeof (TxWords));
  Cursor   = TxBuffer;
  Offset   = 0;
  Sequence = ++mRpcSequence;

  Cursor[Offset++] = GH_RM_RPC_API;
  Cursor[Offset++] = GH_RM_RPC_TYPE_REQUEST;
  WriteUnaligned16 (Cursor + Offset, Sequence);
  Offset += sizeof (UINT16);
  WriteUnaligned32 (Cursor + Offset, GH_RM_RPC_MEM_ACCEPT);
  Offset += sizeof (UINT32);

  WriteUnaligned32 (Cursor + Offset, Handle);
  Offset += sizeof (UINT32);
  Cursor[Offset++] = GH_RM_MEM_TYPE_NORMAL;
  Cursor[Offset++] = GH_RM_TRANS_TYPE_SHARE;
  Cursor[Offset++] = GH_RM_MEM_ACCEPT_MAP_IPA_CONTIGUOUS |
                     GH_RM_MEM_ACCEPT_DONE;
  Cursor[Offset++] = 0;
  WriteUnaligned32 (Cursor + Offset, 0); // validate_label
  Offset += sizeof (UINT32);
  WriteUnaligned32 (Cursor + Offset, 0); // empty ACL descriptor
  Offset += sizeof (UINT32);
  WriteUnaligned16 (Cursor + Offset, 1); // one SGL entry
  Offset += sizeof (UINT16);
  WriteUnaligned16 (Cursor + Offset, 0); // map_vmid
  Offset += sizeof (UINT16);
  WriteUnaligned64 (Cursor + Offset, Pool->Base);
  Offset += sizeof (UINT64);
  WriteUnaligned64 (Cursor + Offset, Pool->Size);
  Offset += sizeof (UINT64);
  WriteUnaligned16 (Cursor + Offset, 0); // empty memory attribute descriptor
  Offset += sizeof (UINT16);
  WriteUnaligned16 (Cursor + Offset, 0);
  Offset += sizeof (UINT16);

  Result = GunyahMessageQueueSend (
             Pool->TxCapability,
             TxBuffer,
             Offset
             );
  if (Result != GH_ERROR_OK) {
    DEBUG ((DEBUG_ERROR, "GunyahPreload: MSGQ_SEND failed: %u\n", Result));
    return EFI_DEVICE_ERROR;
  }

  for (Attempt = 0; Attempt < (PRELOAD_TIMEOUT_US / PRELOAD_POLL_US); Attempt++) {
    ReceivedSize = 0;
    Result       = GunyahMessageQueueReceive (
                     Pool->RxCapability,
                     RxBuffer,
                     sizeof (RxWords),
                     &ReceivedSize
                     );
    if ((Result == GH_ERROR_OK) && (ReceivedSize >= 12) &&
        ((RxBuffer[1] & GH_RM_RPC_TYPE_MASK) == GH_RM_RPC_TYPE_REPLY) &&
        (ReadUnaligned16 (RxBuffer + 2) == Sequence) &&
        (ReadUnaligned32 (RxBuffer + 4) == GH_RM_RPC_MEM_ACCEPT))
    {
      UINT32  ErrorCode;

      ErrorCode = ReadUnaligned32 (RxBuffer + 8);
      if (ErrorCode != 0) {
        DEBUG ((
          DEBUG_ERROR,
          "GunyahPreload: MEM_ACCEPT handle=0x%x rejected: RM error=0x%x\n",
          Handle,
          ErrorCode
          ));
        return EFI_ACCESS_DENIED;
      }

      DEBUG ((
        DEBUG_INFO,
        "GunyahPreload: MEM_ACCEPT handle=0x%x base=0x%lx size=0x%lx succeeded\n",
        Handle,
        Pool->Base,
        Pool->Size
        ));
      return EFI_SUCCESS;
    }

    gBS->Stall (PRELOAD_POLL_US);
  }

  DEBUG ((DEBUG_ERROR, "GunyahPreload: MEM_ACCEPT reply timed out\n"));
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
InitializeQueue (
  IN VIRTIO_DEVICE_PROTOCOL  *VirtIo,
  IN UINT16                  QueueIndex,
  OUT PRELOAD_QUEUE          *Queue
  )
{
  EFI_STATUS  Status;
  UINT16      QueueSize;
  UINT64      RingBaseShift;

  Status = VirtIo->SetQueueSel (VirtIo, QueueIndex);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = VirtIo->GetQueueNumMax (VirtIo, &QueueSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (QueueSize < 2) {
    return EFI_UNSUPPORTED;
  }

  Status = VirtioRingInit (VirtIo, QueueSize, &Queue->Ring);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Queue->Initialized = TRUE;
  Status             = VirtioRingMap (
                         VirtIo,
                         &Queue->Ring,
                         &RingBaseShift,
                         &Queue->RingMapping
                         );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = VirtIo->SetQueueNum (VirtIo, QueueSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = VirtIo->SetQueueAlign (VirtIo, EFI_PAGE_SIZE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return VirtIo->SetQueueAddress (VirtIo, &Queue->Ring, RingBaseShift);
}

STATIC
VOID
ReleaseQueue (
  IN VIRTIO_DEVICE_PROTOCOL  *VirtIo,
  IN OUT PRELOAD_QUEUE       *Queue
  )
{
  if (!Queue->Initialized) {
    return;
  }

  if (Queue->RingMapping != NULL) {
    VirtIo->UnmapSharedBuffer (VirtIo, Queue->RingMapping);
  }

  VirtioRingUninit (VirtIo, &Queue->Ring);
  ZeroMem (Queue, sizeof (*Queue));
}

STATIC
EFI_STATUS
SubmitQueue (
  IN VIRTIO_DEVICE_PROTOCOL  *VirtIo,
  IN UINT16                  QueueIndex,
  IN OUT PRELOAD_QUEUE       *Queue,
  IN UINT16                  HeadDescriptor
  )
{
  UINT16      NextAvailable;
  EFI_STATUS  Status;

  if (Queue->Submitted) {
    return EFI_ALREADY_STARTED;
  }

  *Queue->Ring.Avail.Flags = VRING_AVAIL_F_NO_INTERRUPT;
  NextAvailable            = *Queue->Ring.Avail.Idx;
  Queue->UsedSlot          = *Queue->Ring.Used.Idx;
  Queue->Ring.Avail.Ring[NextAvailable % Queue->Ring.QueueSize] =
    HeadDescriptor;
  MemoryFence ();
  *Queue->Ring.Avail.Idx = NextAvailable + 1;
  MemoryFence ();
  Status = VirtIo->SetQueueNotify (VirtIo, QueueIndex);
  if (!EFI_ERROR (Status)) {
    Queue->Submitted = TRUE;
  }

  return Status;
}

STATIC
BOOLEAN
QueueHasCompleted (
  IN PRELOAD_QUEUE  *Queue
  )
{
  BOOLEAN  Completed;

  if (!Queue->Submitted) {
    return FALSE;
  }

  MemoryFence ();
  Completed = *Queue->Ring.Used.Idx != Queue->UsedSlot;
  MemoryFence ();
  return Completed;
}

STATIC
EFI_STATUS
FinishQueue (
  IN OUT PRELOAD_QUEUE  *Queue,
  OUT UINT32            *UsedLength OPTIONAL
  )
{
  volatile CONST VRING_USED_ELEM  *Used;

  if (!QueueHasCompleted (Queue)) {
    return EFI_NOT_READY;
  }

  Used = &Queue->Ring.Used.UsedElem[Queue->UsedSlot % Queue->Ring.QueueSize];
  if (Used->Id != 0) {
    return EFI_DEVICE_ERROR;
  }

  if (UsedLength != NULL) {
    *UsedLength = Used->Len;
  }

  Queue->Submitted = FALSE;
  return EFI_SUCCESS;
}

STATIC
INT32
FindPreloadNode (
  IN VOID  *Fdt
  )
{
  STATIC CONST CHAR8  PoolName[] = "edk2_preload";
  CONST VOID          *Property;
  INT32               Length;
  INT32               Node;
  INT32               ReservedNode;

  ReservedNode = FdtPathOffset (Fdt, "/reserved-memory");
  if (ReservedNode < 0) {
    return ReservedNode;
  }

  FdtForEachSubnode (Node, Fdt, ReservedNode) {
    Property = FdtGetProp (Fdt, Node, "droidvm,pool-name", &Length);
    if ((Property != NULL) && (Length == sizeof (PoolName)) &&
        (CompareMem (Property, PoolName, sizeof (PoolName)) == 0))
    {
      return Node;
    }
  }

  return -1;
}

STATIC
EFI_STATUS
ReserveConventionalRange (
  IN UINT64  Start,
  IN UINT64  End,
  IN OUT UINT64  *ReservedPages
  )
{
  EFI_PHYSICAL_ADDRESS  Address;
  EFI_STATUS            Status;
  UINT64                Pages64;
  UINTN                 Pages;

  if (Start >= End) {
    return EFI_SUCCESS;
  }

  Pages64 = (End - Start) >> EFI_PAGE_SHIFT;
  if ((Pages64 == 0) || (Pages64 > MAX_UINTN)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Address = Start;
  Pages   = (UINTN)Pages64;
  Status  = gBS->AllocatePages (
                   AllocateAddress,
                   EfiBootServicesData,
                   Pages,
                   &Address
                   );
  if (!EFI_ERROR (Status)) {
    *ReservedPages += Pages64;
  }

  return Status;
}

STATIC
EFI_STATUS
ReserveConventionalMemoryOutsidePool (
  IN UINT64  PoolBase,
  IN UINT64  PoolSize
  )
{
  EFI_MEMORY_DESCRIPTOR  *Descriptor;
  EFI_MEMORY_DESCRIPTOR  *MemoryMap;
  EFI_STATUS             Status;
  UINT8                  *Walker;
  UINT64                 DescriptorEnd;
  UINT64                 DescriptorSizeBytes;
  UINT64                 PoolEnd;
  UINT64                 ReservedPages;
  UINTN                  DescriptorSize;
  UINTN                  MapKey;
  UINTN                  MemoryMapSize;
  UINT32                 DescriptorVersion;

  PoolEnd          = PoolBase + PoolSize;
  MemoryMap        = NULL;
  MemoryMapSize    = 0;
  DescriptorSize   = 0;
  ReservedPages    = 0;
  DescriptorVersion = 0;
  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  if ((DescriptorSize == 0) || (MemoryMapSize > (MAX_UINTN - 8 * DescriptorSize))) {
    return EFI_OUT_OF_RESOURCES;
  }

  MemoryMapSize += 8 * DescriptorSize;
  Status         = gBS->AllocatePool (
                          EfiBootServicesData,
                          MemoryMapSize,
                          (VOID **)&MemoryMap
                          );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  for (Walker = (UINT8 *)MemoryMap;
       Walker < ((UINT8 *)MemoryMap + MemoryMapSize);
       Walker += DescriptorSize)
  {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)Walker;
    if (Descriptor->Type != EfiConventionalMemory) {
      continue;
    }

    if (Descriptor->NumberOfPages >
        RShiftU64 (MAX_UINT64 - Descriptor->PhysicalStart, EFI_PAGE_SHIFT))
    {
      Status = EFI_COMPROMISED_DATA;
      goto Done;
    }

    DescriptorSizeBytes = LShiftU64 (Descriptor->NumberOfPages, EFI_PAGE_SHIFT);
    DescriptorEnd       = Descriptor->PhysicalStart + DescriptorSizeBytes;
    if ((DescriptorEnd <= PoolBase) || (Descriptor->PhysicalStart >= PoolEnd)) {
      Status = ReserveConventionalRange (
                 Descriptor->PhysicalStart,
                 DescriptorEnd,
                 &ReservedPages
                 );
    } else {
      Status = ReserveConventionalRange (
                 Descriptor->PhysicalStart,
                 MIN (DescriptorEnd, PoolBase),
                 &ReservedPages
                 );
      if (!EFI_ERROR (Status)) {
        Status = ReserveConventionalRange (
                   MAX (Descriptor->PhysicalStart, PoolEnd),
                   DescriptorEnd,
                   &ReservedPages
                   );
      }
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "GunyahPreload: failed to reserve ordinary range [0x%lx, 0x%lx): %r\n",
        Descriptor->PhysicalStart,
        DescriptorEnd,
        Status
        ));
      goto Done;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: withheld 0x%lx pages of ordinary memory from EFI stub\n",
    ReservedPages
    ));

Done:
  // On success the scratch buffer must remain allocated until ExitBootServices.
  // Freeing an ordinary-memory buffer here would recreate ConventionalMemory
  // outside the accepted pool after that memory was deliberately withheld.
  if (EFI_ERROR (Status)) {
    gBS->FreePool (MemoryMap);
  }

  return Status;
}

STATIC
EFI_STATUS
SetLinuxUsableMemoryRange (
  IN VOID    *Fdt,
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  UINT8  Range[2 * sizeof (UINT64)];
  INT32  ChosenNode;

  ChosenNode = FdtPathOffset (Fdt, "/chosen");
  if (ChosenNode < 0) {
    return EFI_NOT_FOUND;
  }

  WriteBe64 (Range, Base);
  WriteBe64 (Range + sizeof (UINT64), Size);
  if (FdtSetProp (
        Fdt,
        ChosenNode,
        "linux,usable-memory-range",
        Range,
        sizeof (Range)
        ) != 0)
  {
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: capped Linux usable memory to [0x%lx, 0x%lx)\n",
    Base,
    Base + Size
    ));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DisableRestrictedDmaForLinux (
  IN VOID  *Fdt
  )
{
  CONST VOID  *MemoryRegion;
  CONST VOID  *Phandle;
  INT32       Length;
  INT32       PciNode;
  INT32       PoolNode;

  PoolNode = FdtNodeOffsetByCompatible (Fdt, -1, "restricted-dma-pool");
  if (PoolNode < 0) {
    return EFI_NOT_FOUND;
  }

  Phandle = FdtGetProp (Fdt, PoolNode, "phandle", &Length);
  if ((Phandle == NULL) || (Length != sizeof (UINT32))) {
    return EFI_COMPROMISED_DATA;
  }

  PciNode = FdtPathOffset (Fdt, "/pci");
  if (PciNode < 0) {
    return EFI_NOT_FOUND;
  }

  MemoryRegion = FdtGetProp (Fdt, PciNode, "memory-region", &Length);
  if ((MemoryRegion == NULL) || (Length != sizeof (UINT32)) ||
      (ReadBe32 (MemoryRegion) != ReadBe32 (Phandle)))
  {
    return EFI_COMPROMISED_DATA;
  }

  if (FdtDelProp (Fdt, PciNode, "memory-region") != 0) {
    return EFI_DEVICE_ERROR;
  }

  PoolNode = FdtNodeOffsetByCompatible (Fdt, -1, "restricted-dma-pool");
  if ((PoolNode < 0) || (FdtDelNode (Fdt, PoolNode) != 0)) {
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: removed Linux restricted DMA pool; PCI will use direct DMA\n"
    ));
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
PublishAcceptedMemoryOnly (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8       Reg[2 * sizeof (UINT64)];
  EFI_STATUS  Status;
  INT32       MemoryNode;
  INT32       PoolNode;

  (VOID)Context;
  gBS->CloseEvent (Event);
  mLinuxMemory.ReadyToBootEvent = NULL;

  PoolNode = FindPreloadNode (mLinuxMemory.Fdt);
  if ((PoolNode < 0) || (FdtDelNode (mLinuxMemory.Fdt, PoolNode) != 0)) {
    DEBUG ((DEBUG_ERROR, "GunyahPreload: failed to delete reserved-memory node\n"));
    return;
  }

  MemoryNode = FdtPathOffset (mLinuxMemory.Fdt, "/memory");
  if (MemoryNode < 0) {
    DEBUG ((DEBUG_ERROR, "GunyahPreload: /memory disappeared before ReadyToBoot\n"));
    return;
  }

  WriteBe64 (Reg, mLinuxMemory.Base);
  WriteBe64 (Reg + sizeof (UINT64), mLinuxMemory.Size);
  if (FdtSetProp (mLinuxMemory.Fdt, MemoryNode, "reg", Reg, sizeof (Reg)) != 0) {
    DEBUG ((DEBUG_ERROR, "GunyahPreload: failed to isolate Linux /memory\n"));
    return;
  }

  Status = SetLinuxUsableMemoryRange (
             mLinuxMemory.Fdt,
             mLinuxMemory.Base,
             mLinuxMemory.Size
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPreload: cannot cap Linux usable memory: %r\n", Status));
    return;
  }

  Status = DisableRestrictedDmaForLinux (mLinuxMemory.Fdt);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPreload: cannot disable restricted DMA: %r\n", Status));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: Linux memory isolated to accepted [0x%lx, 0x%lx)\n",
    mLinuxMemory.Base,
    mLinuxMemory.Base + mLinuxMemory.Size
    ));
}

STATIC
EFI_STATUS
ArmAcceptedMemoryForLinux (
  IN PRELOAD_POOL  *Pool
  )
{
  EFI_STATUS  Status;

  ZeroMem (&mLinuxMemory, sizeof (mLinuxMemory));
  mLinuxMemory.Fdt  = Pool->Fdt;
  mLinuxMemory.Base = Pool->Base;
  mLinuxMemory.Size = Pool->Size;
  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             PublishAcceptedMemoryOnly,
             NULL,
             &mLinuxMemory.ReadyToBootEvent
             );
  if (EFI_ERROR (Status)) {
    ZeroMem (&mLinuxMemory, sizeof (mLinuxMemory));
    return Status;
  }

  Status = gBS->FreePages (
                  Pool->Base,
                  EFI_SIZE_TO_PAGES ((UINTN)Pool->Size)
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mLinuxMemory.ReadyToBootEvent);
    ZeroMem (&mLinuxMemory, sizeof (mLinuxMemory));
    return Status;
  }

  Status = ReserveConventionalMemoryOutsidePool (Pool->Base, Pool->Size);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mLinuxMemory.ReadyToBootEvent);
    ZeroMem (&mLinuxMemory, sizeof (mLinuxMemory));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: entire accepted pool armed for Linux direct DMA\n"
    ));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SubmitAcceptBuffer (
  IN VIRTIO_DEVICE_PROTOCOL     *VirtIo,
  IN OUT PRELOAD_QUEUE          *Queue,
  IN EFI_PHYSICAL_ADDRESS       DeviceAddress
  )
{
  DESC_INDICES  Indices;

  VirtioPrepare (&Queue->Ring, &Indices);
  VirtioAppendDesc (
    &Queue->Ring,
    DeviceAddress,
    sizeof (VGA_REQUEST),
    VRING_DESC_F_WRITE,
    &Indices
    );
  return SubmitQueue (VirtIo, VGA_REQUEST_QUEUE, Queue, Indices.HeadDescIdx);
}

STATIC
EFI_STATUS
SubmitCompletion (
  IN VIRTIO_DEVICE_PROTOCOL     *VirtIo,
  IN OUT PRELOAD_QUEUE          *Queue,
  IN EFI_PHYSICAL_ADDRESS       DeviceAddress
  )
{
  DESC_INDICES  Indices;

  VirtioPrepare (&Queue->Ring, &Indices);
  VirtioAppendDesc (
    &Queue->Ring,
    DeviceAddress,
    sizeof (VGA_COMPLETION),
    0,
    &Indices
    );
  return SubmitQueue (VirtIo, VGA_COMPLETION_QUEUE, Queue, Indices.HeadDescIdx);
}

STATIC
EFI_STATUS
SubmitPoolRequest (
  IN VIRTIO_DEVICE_PROTOCOL     *VirtIo,
  IN OUT PRELOAD_QUEUE          *Queue,
  IN EFI_PHYSICAL_ADDRESS       RequestDeviceAddress,
  IN EFI_PHYSICAL_ADDRESS       ResponseDeviceAddress
  )
{
  DESC_INDICES  Indices;

  VirtioPrepare (&Queue->Ring, &Indices);
  VirtioAppendDesc (
    &Queue->Ring,
    RequestDeviceAddress,
    sizeof (VGP_REQUEST),
    VRING_DESC_F_NEXT,
    &Indices
    );
  VirtioAppendDesc (
    &Queue->Ring,
    ResponseDeviceAddress,
    sizeof (VGP_RESPONSE),
    VRING_DESC_F_WRITE,
    &Indices
    );
  return SubmitQueue (VirtIo, VGA_POOL_QUEUE, Queue, Indices.HeadDescIdx);
}

STATIC
INT32
HandleAcceptRequest (
  IN CONST PRELOAD_POOL  *Pool,
  IN CONST VGA_REQUEST   *Request
  )
{
  EFI_STATUS  Status;

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: request id=%u op=%u handle=0x%x base=0x%lx size=0x%lx\n",
    Request->RequestId,
    Request->Operation,
    Request->Handle,
    Request->GuestAddress,
    Request->Size
    ));

  if ((Request->Operation != VGA_OP_ACCEPT) || (Request->Handle == 0) ||
      (Request->GuestAddress != Pool->Base) || (Request->Size != Pool->Size))
  {
    return -LINUX_EINVAL;
  }

  Status = GunyahMemAccept (Pool, Request->Handle);
  return EFI_ERROR (Status) ? -LINUX_EIO : 0;
}

STATIC
EFI_STATUS
RunQueueTransaction (
  IN VIRTIO_DEVICE_PROTOCOL       *VirtIo,
  IN CONST PRELOAD_POOL           *Pool,
  IN OUT PRELOAD_QUEUE            *Queues,
  IN OUT PRELOAD_SHARED_BUFFERS   *Buffers,
  IN EFI_PHYSICAL_ADDRESS         BufferDeviceAddress
  )
{
  EFI_STATUS  Status;
  UINTN       Elapsed;
  UINT32      UsedLength;
  INT32       AcceptResult;
  BOOLEAN     AcceptHandled;
  BOOLEAN     CompletionDone;
  BOOLEAN     PoolDone;

  ZeroMem (Buffers, sizeof (*Buffers));
  Buffers->PoolRequest.RequestId = 1;
  Buffers->PoolRequest.Operation = VGP_OP_SHARE;
  Buffers->PoolRequest.PoolId    = Pool->PoolId;
  Buffers->PoolRequest.Offset    = 0;
  Buffers->PoolRequest.Length    = Pool->Size;

  Status = SubmitAcceptBuffer (
             VirtIo,
             &Queues[VGA_REQUEST_QUEUE],
             BufferDeviceAddress + OFFSET_OF (PRELOAD_SHARED_BUFFERS, AcceptRequest)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SubmitPoolRequest (
             VirtIo,
             &Queues[VGA_POOL_QUEUE],
             BufferDeviceAddress + OFFSET_OF (PRELOAD_SHARED_BUFFERS, PoolRequest),
             BufferDeviceAddress + OFFSET_OF (PRELOAD_SHARED_BUFFERS, PoolResponse)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AcceptResult  = -LINUX_EIO;
  AcceptHandled = FALSE;
  CompletionDone = FALSE;
  PoolDone      = FALSE;

  for (Elapsed = 0; Elapsed < PRELOAD_TIMEOUT_US; Elapsed += PRELOAD_POLL_US) {
    if (!AcceptHandled && QueueHasCompleted (&Queues[VGA_REQUEST_QUEUE])) {
      Status = FinishQueue (&Queues[VGA_REQUEST_QUEUE], &UsedLength);
      if (EFI_ERROR (Status) || (UsedLength < sizeof (VGA_REQUEST))) {
        return EFI_DEVICE_ERROR;
      }

      AcceptResult = HandleAcceptRequest (Pool, &Buffers->AcceptRequest);
      Buffers->AcceptCompletion.RequestId = Buffers->AcceptRequest.RequestId;
      Buffers->AcceptCompletion.Result    = AcceptResult;
      Status = SubmitCompletion (
                 VirtIo,
                 &Queues[VGA_COMPLETION_QUEUE],
                 BufferDeviceAddress +
                 OFFSET_OF (PRELOAD_SHARED_BUFFERS, AcceptCompletion)
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }

      AcceptHandled = TRUE;
    }

    if (AcceptHandled && !CompletionDone &&
        QueueHasCompleted (&Queues[VGA_COMPLETION_QUEUE]))
    {
      Status = FinishQueue (&Queues[VGA_COMPLETION_QUEUE], NULL);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      CompletionDone = TRUE;
    }

    if (!PoolDone && QueueHasCompleted (&Queues[VGA_POOL_QUEUE])) {
      Status = FinishQueue (&Queues[VGA_POOL_QUEUE], &UsedLength);
      if (EFI_ERROR (Status) || (UsedLength < sizeof (VGP_RESPONSE)) ||
          (Buffers->PoolResponse.RequestId != Buffers->PoolRequest.RequestId))
      {
        return EFI_DEVICE_ERROR;
      }

      PoolDone = TRUE;
      DEBUG ((
        DEBUG_INFO,
        "GunyahPreload: pool SHARE response=%d\n",
        Buffers->PoolResponse.Result
        ));
    }

    if (PoolDone && (Buffers->PoolResponse.Result != 0) &&
        (!AcceptHandled || CompletionDone))
    {
      return EFI_DEVICE_ERROR;
    }

    if (AcceptHandled && CompletionDone && PoolDone) {
      if ((AcceptResult == 0) && (Buffers->PoolResponse.Result == 0)) {
        return EFI_SUCCESS;
      }

      return EFI_DEVICE_ERROR;
    }

    gBS->Stall (PRELOAD_POLL_US);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
PromotePreloadPool (
  IN VIRTIO_DEVICE_PROTOCOL  *VirtIo
  )
{
  PRELOAD_POOL            Pool;
  PRELOAD_QUEUE           Queues[VGA_QUEUE_COUNT];
  PRELOAD_SHARED_BUFFERS  *Buffers;
  EFI_PHYSICAL_ADDRESS    BufferDeviceAddress;
  VOID                    *BufferMapping;
  UINT8                   DeviceStatus;
  UINT64                  Features;
  EFI_STATUS              Status;
  EFI_STATUS              TransactionStatus;
  UINTN                   Index;
  BOOLEAN                 BufferAllocated;
  BOOLEAN                 BufferMapped;

  Status = FindPreloadPool (&Pool);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "GunyahPreload: dedicated pool unavailable: %r\n", Status));
    return Status;
  }

  ZeroMem (Queues, sizeof (Queues));
  Buffers          = NULL;
  BufferMapping    = NULL;
  BufferAllocated  = FALSE;
  BufferMapped     = FALSE;
  DeviceStatus     = 0;

  Status = VirtIo->SetDeviceStatus (VirtIo, 0);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  DeviceStatus = VSTAT_ACK;
  Status       = VirtIo->SetDeviceStatus (VirtIo, DeviceStatus);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  DeviceStatus |= VSTAT_DRIVER;
  Status        = VirtIo->SetDeviceStatus (VirtIo, DeviceStatus);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  Status = VirtIo->SetPageSize (VirtIo, EFI_PAGE_SIZE);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  Status = VirtIo->GetDeviceFeatures (VirtIo, &Features);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  Features &= VIRTIO_F_VERSION_1 | VIRTIO_F_IOMMU_PLATFORM;
  if (VirtIo->Revision >= VIRTIO_SPEC_REVISION (1, 0, 0)) {
    Status = Virtio10WriteFeatures (VirtIo, Features, &DeviceStatus);
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
  }

  for (Index = 0; Index < VGA_QUEUE_COUNT; Index++) {
    Status = InitializeQueue (VirtIo, (UINT16)Index, &Queues[Index]);
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
  }

  Status = VirtIo->AllocateSharedPages (VirtIo, 1, (VOID **)&Buffers);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  BufferAllocated = TRUE;
  Status          = VirtioMapAllBytesInSharedBuffer (
                      VirtIo,
                      VirtioOperationBusMasterCommonBuffer,
                      Buffers,
                      EFI_PAGE_SIZE,
                      &BufferDeviceAddress,
                      &BufferMapping
                      );
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  BufferMapped = TRUE;
  if (VirtIo->Revision < VIRTIO_SPEC_REVISION (1, 0, 0)) {
    Features &= ~(UINT64)(VIRTIO_F_VERSION_1 | VIRTIO_F_IOMMU_PLATFORM);
    Status    = VirtIo->SetGuestFeatures (VirtIo, Features);
    if (EFI_ERROR (Status)) {
      goto Failed;
    }
  }

  DeviceStatus |= VSTAT_DRIVER_OK;
  Status        = VirtIo->SetDeviceStatus (VirtIo, DeviceStatus);
  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  TransactionStatus = RunQueueTransaction (
                        VirtIo,
                        &Pool,
                        Queues,
                        Buffers,
                        BufferDeviceAddress
                        );
  if (EFI_ERROR (TransactionStatus)) {
    Status = TransactionStatus;
    goto Failed;
  }

  Status = ArmAcceptedMemoryForLinux (&Pool);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "GunyahPreload: cannot arm accepted-only Linux memory: %r\n",
      Status
      ));
    goto Failed;
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPreload: accepted [0x%lx, 0x%lx); Linux publication deferred to ReadyToBoot\n",
    Pool.Base,
    Pool.Base + Pool.Size
    ));
  goto Done;

Failed:
  DeviceStatus |= VSTAT_FAILED;
  VirtIo->SetDeviceStatus (VirtIo, DeviceStatus);
  DEBUG ((DEBUG_ERROR, "GunyahPreload: preload failed: %r\n", Status));

Done:
  // Stop DMA before returning the restricted-DMA-pool pages to the allocator.
  VirtIo->SetDeviceStatus (VirtIo, 0);
  if (BufferMapped) {
    VirtIo->UnmapSharedBuffer (VirtIo, BufferMapping);
  }

  if (BufferAllocated) {
    VirtIo->FreeSharedPages (VirtIo, 1, Buffers);
  }

  for (Index = 0; Index < VGA_QUEUE_COUNT; Index++) {
    ReleaseQueue (VirtIo, &Queues[Index]);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GunyahPreloadDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  VIRTIO_DEVICE_PROTOCOL  *VirtIo;
  EFI_STATUS              Status;

  if (mPreloadAttempted) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gVirtioDeviceProtocolGuid,
                  (VOID **)&VirtIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = (VirtIo->SubSystemDeviceId == VIRTIO_SUBSYSTEM_GUNYAH_ACCEPT) ?
           EFI_SUCCESS : EFI_UNSUPPORTED;
  gBS->CloseProtocol (
         ControllerHandle,
         &gVirtioDeviceProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GunyahPreloadDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  VIRTIO_DEVICE_PROTOCOL  *VirtIo;
  EFI_STATUS              Status;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gVirtioDeviceProtocolGuid,
                  (VOID **)&VirtIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mPreloadAttempted = TRUE;
  Status            = PromotePreloadPool (VirtIo);
  if (EFI_ERROR (Status)) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gVirtioDeviceProtocolGuid,
           This->DriverBindingHandle,
           ControllerHandle
           );
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GunyahPreloadDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  return gBS->CloseProtocol (
                ControllerHandle,
                &gVirtioDeviceProtocolGuid,
                This->DriverBindingHandle,
                ControllerHandle
                );
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  mDriverBinding = {
  GunyahPreloadDriverBindingSupported,
  GunyahPreloadDriverBindingStart,
  GunyahPreloadDriverBindingStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
GunyahPreloadDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mDriverBinding,
           ImageHandle,
           NULL,
           NULL
           );
}
