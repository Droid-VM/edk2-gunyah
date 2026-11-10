// SPDX-License-Identifier: BSD-2-Clause-Patent
// GICv3 message-based interrupts for ACPI guests: the GIC node's mbi-ranges / mbi-alias (crosvm
// --mbi-spis) become a GIC MSI Frame in the MADT, which Windows and Linux's ACPI GICv2m driver
// use for PCI MSI/MSI-X (address = frame + 0x40, data = INTID). An ITS the MADT may describe is
// dropped (the frame replaces it) and so is the IORT, which exists only for an ITS here.
#include <Uefi.h>
#include <IndustryStandard/Acpi65.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FdtClient.h>

#define IORT_SIGNATURE  SIGNATURE_32 ('I', 'O', 'R', 'T')

typedef struct {
  UINT64    Base;
  UINT32    SpiBase;
  UINT32    SpiCount;
} MBI_INFO;

STATIC EFI_EVENT  mReadyToBootEvent;

STATIC
EFI_STATUS
GetMbiInfo (
  OUT MBI_INFO  *Info
  )
{
  FDT_CLIENT_PROTOCOL  *Fdt;
  CONST UINT32         *Ranges;
  CONST UINT64         *Alias;
  UINT32               Size;
  INT32                Node;
  EFI_STATUS           Status;

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Fdt->FindCompatibleNode (Fdt, "arm,gic-v3", &Node);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Fdt->GetNodeProperty (Fdt, Node, "mbi-ranges", (CONST VOID **)&Ranges, &Size);
  if (EFI_ERROR (Status) || (Size < 2 * sizeof (UINT32))) {
    return EFI_NOT_FOUND;
  }

  Status = Fdt->GetNodeProperty (Fdt, Node, "mbi-alias", (CONST VOID **)&Alias, &Size);
  if (EFI_ERROR (Status) || (Size != sizeof (UINT64))) {
    return EFI_NOT_FOUND;
  }

  Info->Base     = SwapBytes64 (ReadUnaligned64 (Alias));
  Info->SpiBase  = SwapBytes32 (ReadUnaligned32 (&Ranges[0]));
  Info->SpiCount = SwapBytes32 (ReadUnaligned32 (&Ranges[1]));
  return EFI_SUCCESS;
}

STATIC
EFI_ACPI_SDT_HEADER *
FindTable (
  IN  EFI_ACPI_SDT_PROTOCOL  *Sdt,
  IN  UINT32                 Signature,
  OUT UINTN                  *Key
  )
{
  EFI_ACPI_SDT_HEADER     *Table;
  EFI_ACPI_TABLE_VERSION  Version;
  UINTN                   Index;

  for (Index = 0; !EFI_ERROR (Sdt->GetAcpiTable (Index, &Table, &Version, Key)); Index++) {
    if (Table->Signature == Signature) {
      return Table;
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
ReplaceMadt (
  IN EFI_ACPI_SDT_PROTOCOL    *Sdt,
  IN EFI_ACPI_TABLE_PROTOCOL  *AcpiTable,
  IN CONST MBI_INFO           *Mbi
  )
{
  EFI_ACPI_6_5_GIC_MSI_FRAME_STRUCTURE  *Frame;
  EFI_ACPI_DESCRIPTION_HEADER           *Hdr;
  EFI_ACPI_SDT_HEADER                   *Madt;
  UINT8                                 *New;
  UINT8                                 *Src;
  UINT8                                 *Dst;
  UINT8                                 *End;
  UINTN                                 Key;
  UINTN                                 NewKey;
  UINTN                                 Its;
  UINT32                                Length;
  EFI_STATUS                            Status;

  Madt = FindTable (Sdt, EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE, &Key);
  if (Madt == NULL) {
    return EFI_NOT_FOUND;
  }

  New = AllocateZeroPool (Madt->Length + sizeof (*Frame));
  if (New == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (New, Madt, sizeof (EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER));
  Src = (UINT8 *)Madt + sizeof (EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER);
  End = (UINT8 *)Madt + Madt->Length;
  Dst = New + sizeof (EFI_ACPI_6_5_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER);
  Its = 0;

  while ((Src + 2 <= End) && (Src[1] >= 2) && (Src + Src[1] <= End)) {
    if (Src[0] == EFI_ACPI_6_5_GIC_MSI_FRAME) {
      FreePool (New);
      return EFI_ALREADY_STARTED;
    }

    if (Src[0] == EFI_ACPI_6_5_GIC_ITS) {
      Its++;
    } else {
      CopyMem (Dst, Src, Src[1]);
      Dst += Src[1];
    }

    Src += Src[1];
  }

  // crosvm's MADT has no ITS: the frame is added. A QEMU virt one has one: the frame replaces it.
  Frame                      = (EFI_ACPI_6_5_GIC_MSI_FRAME_STRUCTURE *)Dst;
  Frame->Type                = EFI_ACPI_6_5_GIC_MSI_FRAME;
  Frame->Length              = sizeof (*Frame);
  Frame->GicMsiFrameId       = 0;
  Frame->PhysicalBaseAddress = Mbi->Base;
  Frame->Flags               = EFI_ACPI_6_5_SPI_COUNT_BASE_SELECT;
  Frame->SPICount            = (UINT16)Mbi->SpiCount;
  Frame->SPIBase             = (UINT16)Mbi->SpiBase;
  Dst                       += sizeof (*Frame);

  Length        = (UINT32)(Dst - New);
  Hdr           = (EFI_ACPI_DESCRIPTION_HEADER *)New;
  Hdr->Length   = Length;
  Hdr->Checksum = 0;
  Hdr->Checksum = CalculateCheckSum8 (New, Length);

  Status = AcpiTable->UninstallAcpiTable (AcpiTable, Key);
  if (!EFI_ERROR (Status)) {
    Status = AcpiTable->InstallAcpiTable (AcpiTable, New, Length, &NewKey);
  }

  FreePool (New);
  DEBUG ((
    DEBUG_INFO,
    "%a: GIC MSI frame 0x%lx SPI %u..%u (%u ITS dropped): %r\n",
    __func__,
    Mbi->Base,
    Mbi->SpiBase,
    Mbi->SpiBase + Mbi->SpiCount - 1,
    (UINT32)Its,
    Status
    ));
  return Status;
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  EFI_ACPI_SDT_PROTOCOL    *Sdt;
  MBI_INFO                 Mbi;
  UINTN                    Key;
  EFI_STATUS               Status;

  gBS->CloseEvent (Event);

  if (EFI_ERROR (GetMbiInfo (&Mbi)) || (Mbi.SpiCount == 0)) {
    return;
  }

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAcpiSdtProtocolGuid, NULL, (VOID **)&Sdt)) ||
      EFI_ERROR (gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable)))
  {
    return;
  }

  Status = ReplaceMadt (Sdt, AcpiTable, &Mbi);
  if (EFI_ERROR (Status)) {
    return;
  }

  if (FindTable (Sdt, IORT_SIGNATURE, &Key) != NULL) {
    Status = AcpiTable->UninstallAcpiTable (AcpiTable, Key);
    DEBUG ((DEBUG_INFO, "%a: IORT removed: %r\n", __func__, Status));
  }
}

EFI_STATUS
EFIAPI
GunyahMbiAcpiDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &mReadyToBootEvent);
}
