/** @file
  DroidVM memory pool SSDT generator.

  crosvm announces each of its memory pools -- the slabs of guest-physical address space
  the host can reach, which the GPU stack allocates from -- as a `no-map` reserved-memory
  node carrying `compatible = "droidvm,pool"`. A guest that reads the device tree finds
  its pool there. A guest that does not -- Windows -- needs the same information in the
  ACPI namespace, which is what this driver builds.

  Every pool found becomes one device under \_SB:

    Device (PL00) {
      Name (_HID, "DRVM0001")                        // shared, unless the node overrides it
      Name (_UID, "gpu_guest")                       // the pool name, from droidvm,pool-name
      Name (_STR, Unicode ("DroidVM pool: gpu_guest"))
      Name (_STA, 0x0F)
      Name (_CRS, ResourceTemplate () { QWordMemory (... Cacheable ...) })
      Name (_DSD, ...)                               // growable pools only
    }

  So Windows enumerates ACPI\DRVM0001\gpu_guest, and one provider driver bound to
  ACPI\DRVM0001 serves every pool, telling them apart by _UID. A pool that needs its own
  driver instead sets `droidvm,acpi-hid` in the device tree and gets that as its _HID,
  with DRVM0001 kept as _CID so the shared provider still picks it up when the private
  driver is not installed. Windows matches an INF on ACPI\<_HID> and ACPI\<_CID> only --
  the instance id never takes part -- which is why a private driver needs a private _HID.

  The scan is generic on purpose: adding a pool in crosvm must not require a firmware
  change. Nothing here names an individual pool.

  This driver does not touch the restricted DMA pool (ACPI\RDMA0000), which
  GunyahRestrictedDmaPoolAcpiDxe publishes from a different node and which existing
  Windows drivers already bind. The two are separate: the restricted DMA pool is a bounce
  buffer, these pools hold the data itself.

  Copyright (c) 2026, DroidVM contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/AmlLib/AmlLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FdtClient.h>

#define DROIDVM_POOL_COMPAT      "droidvm,pool"
#define DROIDVM_POOL_SHARED_HID  "DRVM0001"

#define SSDT_OEM_ID        "ARMLTD"
#define SSDT_OEM_TABLE_ID  "DVMPOOL"
#define SSDT_OEM_REVISION  1

//
// A cap on how many pools one SSDT describes. Well above any plausible configuration --
// there are four today -- and here only so that a corrupt device tree cannot make this
// loop forever.
//
#define MAX_POOLS  32

//
// Longest pool name accepted. The name becomes the ACPI _UID and therefore part of the
// Windows device instance id, so it is kept short and to characters that survive there.
//
#define MAX_POOL_NAME_LEN  31

//
// _DSD device properties UUID, per ACPI 6.4 s6.2.5:
// daffd814-6eba-4d8c-8a91-bc9bbf4aa301
//
STATIC CONST EFI_GUID  mDsdDevicePropertyGuid = {
  0xdaffd814, 0x6eba, 0x4d8c, { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 }
};

STATIC CONST CHAR8  mHexDigits[] = "0123456789ABCDEF";

typedef struct {
  CONST CHAR8    *Name;
  CONST CHAR8    *Hid;
  UINT64         Base;
  UINT64         Size;
  BOOLEAN        HasPoolId;
  UINT64         PoolId;
  BOOLEAN        HasStepSize;
  UINT64         StepSize;
  BOOLEAN        HasPreAllocSize;
  UINT64         PreAllocSize;
} DROIDVM_POOL;

/**
  Read a device tree property holding a single integer, as either a 32- or 64-bit cell.

  crosvm writes some of these as u32 (`droidvm,pool-id`) and some as u64 (the sizes), so
  accept both rather than making the caller know which.
**/
STATIC
EFI_STATUS
GetPoolInteger (
  IN  FDT_CLIENT_PROTOCOL  *FdtClient,
  IN  INT32                Node,
  IN  CONST CHAR8          *Name,
  OUT UINT64               *Value
  )
{
  EFI_STATUS  Status;
  CONST VOID  *Prop;
  UINT32      Len;

  Status = FdtClient->GetNodeProperty (FdtClient, Node, Name, &Prop, &Len);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Len == sizeof (UINT32)) {
    *Value = SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)Prop));
  } else if (Len == sizeof (UINT64)) {
    *Value = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Prop));
  } else {
    DEBUG ((DEBUG_ERROR, "GunyahPool: property %a has length %u, not 4 or 8\n", Name, Len));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Read a device tree property holding a single NUL-terminated string.

  Returns a pointer into the device tree, which outlives this driver, or NULL if the
  property is absent or is not a well-formed string.
**/
STATIC
CONST CHAR8 *
GetPoolString (
  IN FDT_CLIENT_PROTOCOL  *FdtClient,
  IN INT32                Node,
  IN CONST CHAR8          *Name
  )
{
  EFI_STATUS  Status;
  CONST VOID  *Prop;
  UINT32      Len;

  Status = FdtClient->GetNodeProperty (FdtClient, Node, Name, &Prop, &Len);
  if (EFI_ERROR (Status) || (Len == 0)) {
    return NULL;
  }

  if (((CONST CHAR8 *)Prop)[Len - 1] != '\0') {
    return NULL;
  }

  return (CONST CHAR8 *)Prop;
}

/**
  Is this a name that can be an ACPI _UID, and survive being part of a Windows device
  instance id?
**/
STATIC
BOOLEAN
IsValidPoolName (
  IN CONST CHAR8  *Name
  )
{
  UINTN  Index;
  UINTN  Len;

  Len = AsciiStrLen (Name);
  if ((Len == 0) || (Len > MAX_POOL_NAME_LEN)) {
    return FALSE;
  }

  for (Index = 0; Index < Len; Index++) {
    CHAR8  Char = Name[Index];

    if (((Char >= 'a') && (Char <= 'z')) ||
        ((Char >= 'A') && (Char <= 'Z')) ||
        ((Char >= '0') && (Char <= '9')) ||
        (Char == '_') || (Char == '-'))
    {
      continue;
    }

    return FALSE;
  }

  return TRUE;
}

/**
  Is this a well-formed ACPI id: a four character vendor id followed by four hex digits?
**/
STATIC
BOOLEAN
IsValidAcpiId (
  IN CONST CHAR8  *Id
  )
{
  UINTN  Index;

  if (AsciiStrLen (Id) != 8) {
    return FALSE;
  }

  for (Index = 0; Index < 4; Index++) {
    if ((Id[Index] < 'A') || (Id[Index] > 'Z')) {
      return FALSE;
    }
  }

  for (Index = 4; Index < 8; Index++) {
    CHAR8  Char = Id[Index];

    if (((Char < '0') || (Char > '9')) && ((Char < 'A') || (Char > 'F'))) {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Read one `droidvm,pool` node into @Pool. Returns EFI_NOT_FOUND if the node does not
  describe a usable pool, in which case the caller skips it and keeps going: one
  malformed node must not cost the others their ACPI devices.
**/
STATIC
EFI_STATUS
ParsePoolNode (
  IN  FDT_CLIENT_PROTOCOL  *FdtClient,
  IN  INT32                Node,
  OUT DROIDVM_POOL         *Pool
  )
{
  EFI_STATUS  Status;
  CONST VOID  *Prop;
  UINT32      Len;
  UINT64      Value;

  ZeroMem (Pool, sizeof (*Pool));

  //
  // `reg` as <address size>, both 64-bit: /reserved-memory carries
  // #address-cells = <2> and #size-cells = <2> in every tree crosvm emits. The protocol
  // gives no way to read a parent's cells, so this is an assumption rather than a check;
  // a tree that broke it would be caught by the zero/overflow tests below.
  //
  Status = FdtClient->GetNodeProperty (FdtClient, Node, "reg", &Prop, &Len);
  if (EFI_ERROR (Status) || (Len < 2 * sizeof (UINT64))) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: pool node without a usable `reg` (%r)\n", Status));
    return EFI_NOT_FOUND;
  }

  Pool->Base = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Prop));
  Pool->Size = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Prop + 1));

  //
  // `reg` covers the pre-shared floor, which for a growable pool is smaller than the window
  // the guest may grow into: the Gunyah resource manager on android14-6.1 refuses to start a
  // VM whose reserved-memory node describes a range no memparcel matches, and before boot only
  // the floor is one. The window's size travels beside it, and is absent on a fully pre-shared
  // pool because there the floor IS the window -- which is also every tree built before this.
  //
  if (!EFI_ERROR (GetPoolInteger (FdtClient, Node, "droidvm,pool-size", &Value)) &&
      (Value >= Pool->Size))
  {
    Pool->Size = Value;
  }

  if ((Pool->Base == 0) || (Pool->Size == 0) ||
      (Pool->Base > MAX_UINT64 - Pool->Size))
  {
    DEBUG ((
      DEBUG_ERROR,
      "GunyahPool: pool at 0x%lx size 0x%lx is not a usable range\n",
      Pool->Base,
      Pool->Size
      ));
    return EFI_NOT_FOUND;
  }

  //
  // The name is what tells one pool from another, so a node without a usable one is not
  // worth publishing: the guest could not ask for it by name anyway.
  //
  Pool->Name = GetPoolString (FdtClient, Node, "droidvm,pool-name");
  if (Pool->Name == NULL) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: pool at 0x%lx has no droidvm,pool-name\n", Pool->Base));
    return EFI_NOT_FOUND;
  }

  if (!IsValidPoolName (Pool->Name)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: pool name \"%a\" cannot be an ACPI _UID\n", Pool->Name));
    return EFI_NOT_FOUND;
  }

  //
  // A pool with its own Windows driver overrides the shared _HID. A malformed override is
  // reported and ignored rather than dropping the pool: the shared provider is still a
  // useful thing to fall back to.
  //
  Pool->Hid = GetPoolString (FdtClient, Node, "droidvm,acpi-hid");
  if ((Pool->Hid != NULL) && !IsValidAcpiId (Pool->Hid)) {
    DEBUG ((
      DEBUG_ERROR,
      "GunyahPool: %a: droidvm,acpi-hid \"%a\" is not an ACPI id, using " DROIDVM_POOL_SHARED_HID "\n",
      Pool->Name,
      Pool->Hid
      ));
    Pool->Hid = NULL;
  }

  if (Pool->Hid == NULL) {
    Pool->Hid = DROIDVM_POOL_SHARED_HID;
  }

  //
  // Growable pools carry three more numbers a driver cannot work out from `reg`: which
  // pool it is in the host's table, the granularity a grant must be asked for in, and
  // where the part that is backed at boot ends. Absent on a fully pre-shared pool.
  //
  if (!EFI_ERROR (GetPoolInteger (FdtClient, Node, "droidvm,pool-id", &Value))) {
    Pool->HasPoolId = TRUE;
    Pool->PoolId    = Value;
  }

  if (!EFI_ERROR (GetPoolInteger (FdtClient, Node, "droidvm,step-size", &Value))) {
    Pool->HasStepSize = TRUE;
    Pool->StepSize    = Value;
  }

  if (!EFI_ERROR (GetPoolInteger (FdtClient, Node, "droidvm,pre-alloc-size", &Value))) {
    Pool->HasPreAllocSize = TRUE;
    Pool->PreAllocSize    = Value;
  }

  return EFI_SUCCESS;
}

/**
  Add one pool as a device under @ScopeNode.

  Every failure here is an AML generation failure, which means out of memory. The caller
  abandons the whole table rather than installing a partial one.
**/
STATIC
EFI_STATUS
AddPoolDevice (
  IN CONST DROIDVM_POOL      *Pool,
  IN UINTN                   Index,
  IN AML_OBJECT_NODE_HANDLE  ScopeNode
  )
{
  EFI_STATUS              Status;
  AML_OBJECT_NODE_HANDLE  DeviceNode;
  AML_OBJECT_NODE_HANDLE  CrsNode;
  AML_OBJECT_NODE_HANDLE  DsdNode;
  AML_OBJECT_NODE_HANDLE  DsdPackageNode;
  CHAR8                   DeviceName[5];
  CHAR16                  Description[64];

  //
  // An ACPI name is exactly four characters. The pool name cannot be it -- that is what
  // _UID and _STR are for -- so the devices are simply numbered. Index is below MAX_POOLS
  // and so always fits in the two hex digits.
  //
  DeviceName[0] = 'P';
  DeviceName[1] = 'L';
  DeviceName[2] = mHexDigits[(Index >> 4) & 0xF];
  DeviceName[3] = mHexDigits[Index & 0xF];
  DeviceName[4] = '\0';

  Status = AmlCodeGenDevice (DeviceName, ScopeNode, &DeviceNode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameString ("_HID", Pool->Hid, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Only when the pool asked for an _HID of its own: an ACPI id must not appear as both
  // the _HID and the _CID of one device.
  //
  if (AsciiStrCmp (Pool->Hid, DROIDVM_POOL_SHARED_HID) != 0) {
    Status = AmlCodeGenNameString ("_CID", DROIDVM_POOL_SHARED_HID, DeviceNode, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  //
  // The _UID is the pool name, and is what makes ACPI\DRVM0001\gpu_guest and
  // ACPI\DRVM0001\gfx_host two distinct Windows devices.
  //
  Status = AmlCodeGenNameString ("_UID", Pool->Name, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeSPrintAsciiFormat (
    Description,
    sizeof (Description),
    "DroidVM pool: %a",
    Pool->Name
    );
  Status = AmlCodeGenNameUnicodeString ("_STR", Description, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameInteger ("_STA", 0x0F, DeviceNode, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenNameResourceTemplate ("_CRS", DeviceNode, &CrsNode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Cacheable, to match how the host maps the same pages. On AArch64 a mismatch between
  // the two mappings of one physical range is not a performance question, it is stale
  // data. The range is 64-bit because the pools sit above guest RAM, past 4 GiB.
  //
  Status = AmlCodeGenRdQWordMemory (
             TRUE,                          // IsResourceConsumer
             TRUE,                          // IsPosDecode
             TRUE,                          // IsMinFixed
             TRUE,                          // IsMaxFixed
             AmlMemoryCacheable,            // Cacheable
             TRUE,                          // IsReadWrite
             0,                             // AddressGranularity
             Pool->Base,                    // AddressMinimum
             Pool->Base + Pool->Size - 1,   // AddressMaximum
             0,                             // AddressTranslation
             Pool->Size,                    // RangeLength
             0,                             // ResourceSourceIndex
             NULL,                          // ResourceSource
             AmlAddressRangeMemory,         // MemoryRangeType
             TRUE,                          // IsTypeStatic
             CrsNode,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Pool->HasPoolId && !Pool->HasStepSize && !Pool->HasPreAllocSize) {
    return EFI_SUCCESS;
  }

  Status = AmlCodeGenNamePackage ("_DSD", DeviceNode, &DsdNode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlAddDeviceDataDescriptorPackage (
             &mDsdDevicePropertyGuid,
             DsdNode,
             &DsdPackageNode
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The property names are the device tree ones, unchanged, so that a driver ported from
  // the device tree side looks for the same strings.
  //
  if (Pool->HasPoolId) {
    Status = AmlAddNameIntegerPackage ("droidvm,pool-id", Pool->PoolId, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Pool->HasStepSize) {
    Status = AmlAddNameIntegerPackage ("droidvm,step-size", Pool->StepSize, DsdPackageNode);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Pool->HasPreAllocSize) {
    Status = AmlAddNameIntegerPackage (
               "droidvm,pre-alloc-size",
               Pool->PreAllocSize,
               DsdPackageNode
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InstallPoolSsdt (
  VOID
  )
{
  EFI_STATUS                   Status;
  FDT_CLIENT_PROTOCOL          *FdtClient;
  EFI_ACPI_TABLE_PROTOCOL      *AcpiTableProtocol;
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  AML_ROOT_NODE_HANDLE         RootNode;
  AML_OBJECT_NODE_HANDLE       ScopeNode;
  UINTN                        TableKey;
  UINTN                        Count;
  UINTN                        Index;
  INT32                        Node;
  CONST CHAR8                  *SeenNames[MAX_POOLS];
  DROIDVM_POOL                 Pool;

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&FdtClient);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: FdtClient not available (%r)\n", Status));
    return Status;
  }

  Status = AmlCodeGenDefinitionBlock (
             "SSDT",
             SSDT_OEM_ID,
             SSDT_OEM_TABLE_ID,
             SSDT_OEM_REVISION,
             &RootNode
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: AmlCodeGenDefinitionBlock failed %r\n", Status));
    return Status;
  }

  Status = AmlCodeGenScope ("_SB_", RootNode, &ScopeNode);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: AmlCodeGenScope failed %r\n", Status));
    goto FreeTree;
  }

  Count = 0;
  for (Status = FdtClient->FindCompatibleNode (FdtClient, DROIDVM_POOL_COMPAT, &Node);
       !EFI_ERROR (Status) && (Count < MAX_POOLS);
       Status = FdtClient->FindNextCompatibleNode (FdtClient, DROIDVM_POOL_COMPAT, Node, &Node))
  {
    if (EFI_ERROR (ParsePoolNode (FdtClient, Node, &Pool))) {
      continue;
    }

    //
    // Two devices sharing an _HID must not share a _UID: Windows would build the same
    // instance id twice and the second device would fail to start. The symptom -- one
    // pool intermittently missing -- is a long way from the cause, so it is caught here.
    //
    for (Index = 0; Index < Count; Index++) {
      if (AsciiStrCmp (SeenNames[Index], Pool.Name) == 0) {
        break;
      }
    }

    if (Index < Count) {
      DEBUG ((DEBUG_ERROR, "GunyahPool: duplicate pool name \"%a\", skipped\n", Pool.Name));
      continue;
    }

    Status = AddPoolDevice (&Pool, Count, ScopeNode);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "GunyahPool: %a: AML generation failed %r\n", Pool.Name, Status));
      goto FreeTree;
    }

    DEBUG ((
      DEBUG_INFO,
      "GunyahPool: %a -> ACPI\\%a\\%a, base 0x%lx size 0x%lx\n",
      Pool.Name,
      Pool.Hid,
      Pool.Name,
      Pool.Base,
      Pool.Size
      ));

    SeenNames[Count] = Pool.Name;
    Count++;
  }

  if (Count == MAX_POOLS) {
    DEBUG ((
      DEBUG_ERROR,
      "GunyahPool: reached the %u pool cap; anything past it has no ACPI device\n",
      MAX_POOLS
      ));
  }

  if (Count == 0) {
    DEBUG ((DEBUG_INFO, "GunyahPool: no " DROIDVM_POOL_COMPAT " nodes\n"));
    Status = EFI_NOT_FOUND;
    goto FreeTree;
  }

  Status = AmlSerializeDefinitionBlock (RootNode, &Table);
  AmlDeleteTree (RootNode);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: AmlSerializeDefinitionBlock failed %r\n", Status));
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTableProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: ACPI table protocol not available %r\n", Status));
    FreePool (Table);
    return Status;
  }

  Status = AcpiTableProtocol->InstallAcpiTable (AcpiTableProtocol, Table, Table->Length, &TableKey);
  FreePool (Table);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: InstallAcpiTable failed %r\n", Status));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "GunyahPool: SSDT installed (TableKey=0x%lx) describing %u pool(s)\n",
    TableKey,
    (UINT32)Count
    ));

  return EFI_SUCCESS;

FreeTree:
  AmlDeleteTree (RootNode);
  return Status;
}

EFI_STATUS
EFIAPI
GunyahPoolAcpiDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = InstallPoolSsdt ();
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    DEBUG ((DEBUG_ERROR, "GunyahPool: SSDT install failed %r\n", Status));
  }

  //
  // A VM with no pools is a normal configuration, and a firmware that cannot describe
  // them is still a firmware that can boot. Never fail the entry point.
  //
  return EFI_SUCCESS;
}
