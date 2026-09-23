.. _system-target-ia64:

IA-64 System emulator
=====================

QEMU's experimental IA-64 system target provides the ``ia64-vpc`` machine for
Itanium guest bring-up.  It uses TCG and a project-owned EFI firmware; KVM and
IA-64 user-mode emulation are not provided.

Machine profile
---------------

``ia64-vpc`` defaults to one Madison-class CPU, 2 GiB of RAM, an
ATI-compatible PCI display, an e1000 network adapter, LSI53C895A SCSI storage,
ICH9 AHCI, OHCI/UHCI USB, and PS/2 input.  One to eight CPUs are supported, and
MTTCG can be selected with ``-accel tcg,thread=multi``.  The machine also
provides local SAPIC and I/O SAPIC interrupt controllers, ACPI tables, RTC,
watchdog, NVRAM, serial I/O, and the firmware debug port.

The available CPU generation names are ``merced``, ``madison`` and
``montecito``.  Madison is the default: an Itanium 2 has the widest
operating-system compatibility across Itanium's lifespan.

Building and running
--------------------

The firmware requires an ``ia64-linux-gnu-*`` ELF cross toolchain in
``PATH``::

  ./configure --target-list=ia64-softmmu
  ninja -C build qemu-system-ia64 \
      roms/ia64-firmware/ia64-firmware.bin

For a serial-only emulator without the built-in display, network, input,
storage, or USB device groups, configure with
``--with-devices-ia64=minimal``.  The default build includes all of those
groups.

A typical optical-media boot is::

  build/qemu-system-ia64 \
      -machine ia64-vpc,nvram=/path/to/guest.nvram \
      -bios build/roms/ia64-firmware/ia64-firmware.bin \
      -drive file=/path/to/media.iso,media=cdrom,format=raw,readonly=on \
      -display gtk

Machine properties
------------------

``i8042=on|off``
  Enable or disable the PS/2 controller.  The default is ``on``.  With
  ``i8042=off``, the machine supplies a USB keyboard and absolute tablet when
  default devices are enabled.

``firmware-ide-dma=on|off``
  Select firmware CMD646 bus-master DMA.  The default is ``on``.

``firmware-console=serial|vga``
  Select the primary console advertised by HCDP.  The default is ``vga``.

``nvram=none|PATH``
  Select the file that keeps the board's settings across runs.  On ``460gx``
  it is an image of the firmware flash.  On ``zx1`` it is an image of the PDH
  battery-backed SRAM, followed by the settings tokens that the BMC keeps.
  ``none`` (the default; ``auto`` is accepted too) keeps them for one run
  only.  Use a separate file for each VM.  ``scripts/ia64-nvram.py``
  identifies a file and converts an older one.

``alat=zero|full``
  Select the ALAT model.  ``zero`` is the default.  The full model is not
  currently SMP-safe and is rejected with more than one vCPU.

Firmware and boot media
-----------------------

The firmware implements EFI boot and runtime services, PAL/SAL entry points,
PE/COFF and EBC loading, FAT and UDF filesystems, El Torito boot, storage and
input protocols, graphics output, and a pre-boot shell.  Press F2, F12, or
Delete during the startup window to enter the shell.  Installed systems can
be attached with an ordinary ``-drive`` option.

``-debug-port`` connects the transport described by the ACPI DBGP table to a
QEMU character backend.  For example::

  -debug-port tcp::4444,server=on,wait=on,nodelay=on

EFI network boot is not implemented; the default e1000 device is available to
the operating system after boot.

Validation
----------

The project-owned tests cover the CPU architecture, machine wiring, firmware
services, storage layouts, input, and SMP behavior.  See
:doc:`../devel/testing/ia64` for the exact commands and test-authoring rules.
