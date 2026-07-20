# NovaFermi

An independent out-of-tree Linux kernel module (DRM/KMS) for NVIDIA Fermi-architecture GPUs, specifically the GeForce GT 430 (GF108 chip).

## Goal

Full support for NVIDIA Fermi cards on modern Linux kernels (6.x+), fixing key power-management (reclocking) gaps that remain unaddressed in the existing nouveau driver for this generation of hardware.

## Design principles

- **Isolation from nouveau.** The driver is written as a standalone module built on the standard Linux Kernel DRM API, not as a fork of the nouveau codebase.
- **Reference-only approach.** The nouveau and envytools codebases are used strictly as a specification and reference for MMIO registers, VBIOS table structures, and the I2C bus algorithms. No code is copied directly.
- **Portability.** The build is based on DKMS, ensuring the driver is portable across distributions (Arch, Ubuntu, Mint, Fedora).

## Target hardware

| Parameter | Value |
|---|---|
| Card | GeForce GT 430 |
| Chip | GF108 (Fermi) |
| Vendor ID | 0x10de |
| Device ID | 0x0de1 |

## Source layout

- `nv_fermi_drv.h` -- shared macros, register defines, data structures (`nv_fermi_priv`, `nv_dcb_header`), function prototypes.
- `core.c` -- module init/exit, PCI driver registration, probe/remove.
- `vbios.c` -- VBIOS parsing: expansion ROM read, BIT table, DCB table.
- `i2c.c` -- GPIO/I2C engine (in progress).
- `Makefile` -- Kbuild build file, links `core.o vbios.o i2c.o` into `nv_fermi_drv.ko`.

## Current status

The project is currently in Phase 3 (see the table below).

### Phase 1 -- project skeleton and hardware detection (complete)

- Repository structure and Makefile for building on Arch Linux.
- PCI ID registration for the device (10de:0de1).
- Module load/unload functions with dmesg logging.
- Confirmed device takeover from the stock kernel driver.

### Phase 2 -- card memory access: MMIO and BAR (complete)

- Device activation via `pci_enable_device()`.
- Region reservation via `pci_request_regions()`.
- Mapping of BAR0 (registers) and BAR1 (video memory) via `pci_ioremap_bar()`.
- Basic register read/write macros (`ioread32`/`iowrite32`).
- Reading and decoding `NV_PMC_BOOT_0`, confirming the chipset ID (0xC1 = GF108).

### Phase 3 -- VBIOS parser and I2C bus (in progress)

- VBIOS location and read via `pci_map_rom()`, with option ROM signature validation.
- BIT (BIOS Information Table) parser: signature search, header parsing, enumeration of all subsystem tokens, header checksum check.
- DCB (Device Configuration Table) location via the BIT token `i`, with a legacy-pointer fallback.
- DCB header parsing (version, header size, entry count and size).
- In progress: pinning down the exact offsets of the GPIO and I2C tables inside the DCB header (version-dependent, verified via dmesg hex-dumps before any decoding is trusted), and I2C/SMBus engine initialization.

### Phase 4 -- power and clock management (reclocking) (not started)

Extracting PSTATE performance profiles, writing a PWM voltage-control driver, PLL clock control code, a basic in-kernel power-management daemon.

### Phase 5 -- graphics output and Mesa Gallium3D (not started)

DRM/KMS subsystem integration for display init and video output management, GEM/TTM memory manager for framebuffer allocation, a minimal userspace Mesa Gallium3D state tracker.

## Building

Requires the headers for your currently running kernel to be installed.

```bash
git clone <repository-url>
cd nv-fermi-drv
make
```

## Loading the module

```bash
sudo make load
```

If the card is already claimed by the stock driver (nouveau), unbind it first:

```bash
echo "0000:XX:00.0" | sudo tee /sys/bus/pci/drivers/nouveau/unbind
echo "0000:XX:00.0" | sudo tee /sys/bus/pci/drivers/nv_fermi_drv/bind
```

Find the device's bus address with `lspci -nn | grep 10de`.

## Viewing logs

```bash
sudo make log
```

or directly:

```bash
sudo dmesg -w | grep nv_fermi_drv
```

## Unloading the module

```bash
sudo make unload
```

## Warning

This project is at an early development stage and works with undocumented NVIDIA hardware via direct MMIO and VBIOS access. The current phases (1-3) are limited to read operations and do not change the card's power or clock state. Starting with Phase 4, the driver will write to registers controlling voltage and frequency -- use at that stage will come with additional warnings and safety guards.

## License

GPL-2.0
