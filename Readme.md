# NovaFermi

Independent out-of-tree Linux kernel module (DRM/KMS) for NVIDIA Fermi architecture graphics cards, specifically the GeForce GT 430 (GF108 chip).

## Project Goal

Full support for NVIDIA Fermi cards on modern Linux kernels (6.x+) with fixes for key power management issues (reclocking) that remain unresolved in the existing nouveau driver for this hardware generation.

## Architectural Principles

- **Isolation from Nouveau.** The driver is written as a standalone module based on the standard Linux Kernel DRM API, rather than as a fork of the nouveau codebase.
- **Reference Method.** The nouveau codebase and envytools are used exclusively as a specification and reference for MMIO registers, VBIOS table structures, and I2C bus algorithms. No direct code copying is involved.
- **Portability.** The build is based on DKMS, ensuring driver portability across distributions (Arch, Ubuntu, Mint, Fedora).

## Target Hardware

| Parameter | Value |
|---|---|
| Card | GeForce GT 430 |
| Chip | GF108 (Fermi) |
| Vendor ID | 0x10de |
| Device ID | 0x0de1 |

## Current Status

The project is currently at the Phase 3 stage (see table below). Below is a list of what has already been implemented and verified on real hardware.

### Phase 1 — Project Skeleton and Hardware Detection (Completed)

- Repository structure and Makefile for building under Arch Linux.
- Registration of the device PCI ID (10de:0de1).
- Module load/unload functions with logging to dmesg.
- Device unbinding from the standard kernel driver confirmed.

### Phase 2 — Card Memory Access: MMIO and BAR (Completed)

- Device activation via `pci_enable_device()`.
- Region reservation via `pci_request_regions()`.
- Mapping of BAR0 (registers) and BAR1 (vram) via `pci_ioremap_bar()`.
- Basic register read/write macros (`ioread32`/`iowrite32`).
- Reading and parsing of `NV_PMC_BOOT_0`, confirming chipset ID (0xC1 = GF108).

### Phase 3 — VBIOS Parser and I2C Bus (In Progress)

- Localization and reading of VBIOS via `pci_map_rom()`, verifying the option ROM signature.
- BIT (BIOS Information Table) parser: signature search, header parsing, enumeration of all subsystem tokens.
- Localization of the DCB (Device Configuration Table) via BIT token `i` with a fallback to the legacy pointer.
- Parsing of the DCB header (version, header size, number and size of entries).
- In progress: exact determination of GPIO and I2C table offsets within the DCB header (dependent on the DCB version, verified via hex dump in dmesg before decoding), initialization of the I2C/SMBus engine.

### Phase 4 — Power Management and Clock Speeds (Not Started)

Extraction of PSTATE profiles, driver for the PWM voltage controller, PLL management, kernel-space power-saving daemon.

### Phase 5 — Graphics Output and Mesa Gallium3D (Not Started)

DRM/KMS subsystem, GEM/TTM memory manager, Mesa Gallium3D state tracker in user space.

## Building

Requires installed headers for the current kernel.

```bash
git clone <repository-url>
cd nv-fermi-drv
make
```

## Loading the Module

```bash
sudo make load
```

If the card is already occupied by the standard driver (nouveau), unbind the device first:

```bash
echo "0000:XX:00.0" | sudo tee /sys/bus/pci/drivers/nouveau/unbind
echo "0000:XX:00.0" | sudo tee /sys/bus/pci/drivers/nv_fermi_drv/bind
```

Find the device bus address using `lspci -nn | grep 10de`.

## Viewing Logs

```bash
sudo make log
```

or directly:

```bash
sudo dmesg -w | grep nv_fermi_drv
```

## Unloading the Module

```bash
sudo make unload
```

## Warning

The project is in an early stage of development and interacts with undocumented NVIDIA hardware at the level of direct MMIO and VBIOS access. The current phases (1-3) are limited to read operations and do not make changes to the power or clock frequency state of the card. Starting from Phase 4, the driver will write to registers controlling voltage and frequencies — usage at that stage will be accompanied by additional safety warnings and restrictions.

## License

GPL-2.0
