# edk2-gunyah

UEFI Firmware for crosvm + gunyah virtualization platform

## Quick build

Install the arm64 cross compiler toolchain (e.g. `aarch64-linux-gnu-gcc`) and iasl(e.g. `acpica-tools`).

- Build **Release** Variant:
```bash
./build.sh
```

- Build **Debug** Variant:
```bash
EDK2_TARGET=DEBUG ./build.sh
```

Artifacts named `edk2-gunyah.fd`.

## Running

```bash
/apex/com.android.virt/bin/crosvm run \
    --mem 4096 \
    --cpus 1 \
    --protected-vm-without-firmware \
    --no-balloon \
    --disable-sandbox \
    --block disk.img,lock=false \
    edk2-gunyah.fd
```

## Tested platforms

### SoC

- **Qualcomm Snapdragon 8 Elite Gen 5** (SM8850)
- **Qualcomm Snapdragon 8 Elite** (SM8750)

### Devices

- **Xiaomi Redmi K90 Pro Max** (codename "myron", with SM8850)
- **Lenovo Y700 Gen4** (codename "tb322fc", with SM8750)

## Differents from ArmVirt

This repo is based on ArmVirtPkg, with some patch for crosvm + gunyah.

- ARM64 boot header

  crosvm requires text_offset to be 0 KiB (0x0), ArmVirtQemuKernel uses 512 KiB (0x80000).

- Memory Base

  ArmVirt uses 0x0, crosvm + gunyah uses 0x80000000.

- NS16550A Serial UART

  ArmVirt uses the PL011, crosvm uses NS16550A, `PatchedSerialPortLib16550` removed PCI supports.

- ~~PCIe generic CAM~~

  ~~ArmVirt uses `pci-host-ecam-generic`, crosvm uses `pci-host-cam-generic`.~~

  Custom [crosvm](http://github.com/Droid-VM/crosvm) patch to support ECAM mode, your can use `-DPCI_CAM_MODE=FALSE` flag to build this firmware with PCIe ECAM mode

- PL030 Real Time Clock

  ArmVirt uses pl031, crosvm uses pl030.

- VirtioLib

  ArmVirt's VirtioLib lack of swiotlb supports.

- VirtioGpu

  crosvm's VirtioGpu wants 2 queues.

- VirtioInput

  crosvm's VirtioInput wants 2 queues.

- VirtioScsi

  crosvm's VirtioScsi need initialize all queues (controlq, eventq, requestq) before setting DRIVER_OK.

  must limit LUNs to 1 (default to 8) to avoid crosvm returning duplicate disk handles for the same target.

- SMBIOS

  ArmVirt get SMBIOS table from QEMU, crosvm doesn't have SMBIOS.

## License

See the repository `LICENSE` file at the project root for licensing details.
