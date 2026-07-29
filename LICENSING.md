# Licensing

Everything in this repository is **BSD-2-Clause-Patent**, matching upstream EDK2.
It is the one DroidVM repository that does not default to the GPL, and that is
deliberate.

## Why not GPL like the rest

- EDK2 chose BSD-2-Clause-Patent so firmware can ship in devices. Putting GPL
  code into a platform package works against the only thing this repository
  produces.
- Contributions to EDK2 must be BSD-2-Clause-Patent, so GPL here would block
  the upstreaming that the other repositories go out of their way to allow.
- The files DroidVM added here already declare BSD-2-Clause-Patent. Changing
  them now would contradict a choice this project already made, not record one.
- Authorship is spread across several people who hold copyright in their own
  files by name. No one of them can relicense the others' work.

## What came from where

Most of `GunyahPkg/` is EDK2 code under a Gunyah name: the VirtioFsDxe,
VirtioGpuDxe, VirtioKeyboardDxe and VirtioScsiDxe trees, PlatformBootManagerLib,
the PCI host-bridge libraries, PrePi and the .dsc/.fdf/.dec files are renamed
copies or close derivatives of OvmfPkg, ArmVirtPkg, ArmPlatformPkg and
MdeModulePkg. They keep their upstream copyright lines.

Two more came from neither EDK2 nor DroidVM and must be left alone:

- `GunyahPkg/Drivers/SimpleFbDxe/` — DuoWoA / AlohaWoA (Windows-on-ARM project)
- `GunyahPkg/Drivers/SmbiosPlatformDxe/` — edk2-platforms RaspberryPi lineage;
  its .inf still says "SMBIOS Table for the RaspberryPi platform"

Upstream EDK2 itself is a submodule at `edk2/`, untouched. DroidVM's changes to
it live as patches under `patches/edk2/` and are applied at build time; those
patch files embed upstream context, so they are derivative of EDK2 too.

## Contributing

See `CONTRIBUTING.md`. Sign-off is required; there is no CLA.
