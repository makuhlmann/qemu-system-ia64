# qemu-system-ia64

Experimental QEMU full-system emulation target for IA-64/Itanium guests. Forked from [syunnPC/qemu-system-ia64](https://github.com/syunnPC/qemu-system-ia64
)

**DISCLAIMER: This fork's ia64 implementation is mostly written using AI LLMs.** Testing and validation is done both automated and by hand. If you encounter any issues or bugs, feel free to report them: https://github.com/makuhlmann/qemu-system-ia64/issues

## Quick-Start

Ready-to-run builds of this emulator are available under Releases: https://github.com/makuhlmann/qemu-system-ia64/releases

Automatic experimental builds of the [develop branch](https://github.com/makuhlmann/qemu-system-ia64/tree/develop) are available under Actions: https://github.com/makuhlmann/qemu-system-ia64/actions

To launch qemu, run:

`qemu-system-ia64 -machine ia64-vpc`

Notice that not much will happen when you run it this way. You will need to attach some disks to boot from.

### Disks and ISOs

To create a new empty hard drive image, use the qemu-img command (on Linux available via the qemu-utils package):

`qemu-img create -f qcow2 hdd.img 16G`

To attach the drive, append: `-drive file=hdd.img,format=qcow2`

To attach an ISO image, append: `-drive file=disc.iso,media=cdrom,format=raw,readonly=on`

#### Disk controllers

Most operating systems work well with the default LSI53C895A SCSI controller. However some need alternatives such as an CMD646 IDE or ICH9 AHCI (SATA) controller. They can be enabled via a modified machine flag:

IDE: `-machine ia64-vpc,ide=on`

AHCI: `-machine ia64-vpc,ahci=on`

Note: Disks still attach to SCSI by default. You need to set the interface accordingly by adding `,if=ide` to the -drive parameter. Example: `-drive file=hdd.img,format=qcow2,if=ide`. And yes: `ide` is the correct value for AHCI as well.

#### NVRAM

On Itanium systems, the EFI stores boot parameters in an NVRAM storage, separate from the disk image. Normally this will create one single file in the bios rom directory, however when installing multiple operating systems, this may be problematic when they overwrite each other's boot entries.

To specify a unique NVRAM file, simply add an nvram-setting to the machine flag: `-machine ia64-vpc,nvram=nvram.bin` - the file will be created automatically once an OS or the EFI writes to NVRAM.

### Modify the machine type

#### CPU

The emulator supports three different Itanium CPU type arguments:

- `-cpu merced` - The original Itanium CPU, required by early Operating Systems only
- `-cpu madison` - A later Itanium 2 CPU with near universal compatibility - Default if no CPU is specified
- `-cpu montecito` - A later Itanium 2 9000 Series CPU, intended for more modern Operating Systems

To use multiple CPUs or CPU-Cores for better performance, append `-accel tcg,thread=multi -smp 2` as an argument. Change the 2 to the desired CPU / Core count.

#### Memory

To change the amount of memory from the default of 1 GiB, use the `-m` flag, such as `-m 2G` for 2 GiB or `-m 512M` for 512 MiB. Note: some operating systems may not work properly at certain RAM sizes. See the [Supported Operating Systems](https://github.com/makuhlmann/qemu-system-ia64/wiki/Supported-Operating-Systems) Wiki page for more details.

#### Input devices

By default, a PS/2 controller and peripherals are attached. Some operating systems work better with it, but you may want to disable it for better mouse control via the modified machine flag: `-machine ia64-vpc,i8042=off`

#### Networking

By default, the board's Intel® 8255x 10/100 Mbps Ethernet Controller is attached to the machine in user mode: an 82550 (`model=i82550`) on `zx1` and an 82559 (`model=i82559c`) on `460gx`. A guest installed with an earlier release, which attached an 82557 (`model=i82557b`), finds a new network adapter; `-nic user,model=i82557b` keeps the old one. You can change it to a different type depending on needs (such as for newer operating systems). These are other tested models confirmed to work:

- Intel® 82543GC Gigabit Ethernet Controller: `-nic model=e1000-82543gc`
- Intel® 82545EM Gigabit Ethernet Controller: `-nic model=e1000-82545em`

To disable networking, use `-nic none`

#### Graphics

By default, QEMU will show a GTK window when launched, letting you see the graphical output of the guest. Alternatively you can use the more basic SDL output as well (`-display sdl`) or have no output at all (`-display none`), which might be useful for server systems.

The default graphics card depends on the machine: an ATI Rage 128 GL (AGP) on `-M 460gx` and an ATI Mach64 3D Rage (PCI) on `-M zx1`, both with rudimentary 2D acceleration. `vga=rage128` selects the ATI Rage 128 Pro (AGP) and `vga=rage128gl` the Rage 128 GL, for example `-machine zx1,vga=rage128`. To attach a Rage 128 without its AGP capability (may be needed for some guests), add `agp=off` to the machine flag: `-machine 460gx,agp=off`.

To fall back to a standard VGA capable graphics card, use the flag `-vga std` flag.

## Compatibility and Performance

To run machines at a "reasonable" speed, you will need a performant x86-64 based CPU.

In tests using an AMD Ryzen 7 3700X, the 7-Zip 9.20 (IA-64) benchmark scores about 75 MIPS per core - roughly equivalent to the performance of a 1997 Intel Pentium MMX @ 133 MHz. Using a modern desktop or server CPU with `-smp` enabled is strongly advised for decent performance.

To see which guest operating systems are supported, check out the [Supported Operating Systems](https://github.com/makuhlmann/qemu-system-ia64/wiki/Supported-Operating-Systems) Wiki page. Known issues are documented there as well.


## Build

Configure and build the IA-64 target with GTK:

```sh
./configure --enable-gtk
ninja -C build qemu-system-ia64 roms/ia64-firmware/ia64-firmware.bin
```

The firmware build requires an IA-64 ELF cross toolchain named `ia64-linux-gnu-*` in `PATH`.

### Performance builds

For guest-performance measurements, use an IA-64-only build without the compiler hardening passes that add substantial overhead to TCG helper calls.
Build it in its own directory, and keep the default `build/` for development and correctness testing:

```sh
mkdir build-perf
cd build-perf
../configure --target-list=ia64-softmmu \
  --enable-lto \
  --disable-qom-cast-debug \
  --disable-stack-protector \
  --extra-cflags='-fno-stack-protector -fzero-call-used-regs=skip -ftrivial-auto-var-init=uninitialized' \
  -Doptimization=2 \
  --enable-gtk
ninja qemu-system-ia64 roms/ia64-firmware/ia64-firmware.bin
```

Set the optimization level with `-Doptimization=`, not with `-O` in `--extra-cflags`, so that only one level reaches the compiler.
`-Doptimization=3` together with `--extra-cflags='-march=native -mtune=native ...'` produces a faster binary that only runs on hosts of the same processor generation.

The commands elsewhere in this file assume the default `build/` directory.
With a second build tree, pass its path to `ninja -C`, to `meson test -C`, and to the binary and firmware arguments.

For profile-guided optimization, use a dedicated build directory and the same compiler for both stages:

```sh
mkdir build-pgo
cd build-pgo
../configure --target-list=ia64-softmmu \
  --enable-lto \
  --disable-qom-cast-debug \
  --disable-stack-protector \
  --extra-cflags='-fno-stack-protector -fzero-call-used-regs=skip -ftrivial-auto-var-init=uninitialized' \
  -Doptimization=2 \
  --enable-gtk \
  -Db_pgo=generate
ninja qemu-system-ia64 roms/ia64-firmware/ia64-firmware.bin
```

Run representative guest workloads with the instrumented binary.
The training set should cover boot, integer, floating-point, SIMD, MMU-intensive, and RSE-intensive code paths; a guest boot to the desktop plus the microprogram suites covers all of them.
Then consume the generated profiles:

```sh
meson configure -Db_pgo=use
ninja qemu-system-ia64 roms/ia64-firmware/ia64-firmware.bin
```

Profiles are tied to the exact build tree, compiler, and source revision; do not copy stale profile data between builds.

On hosts that all support the x86-64-v3 ISA level, add `--x86-version=3` to a separate comparison build.
This changes the minimum host ISA for QEMU and its C helpers; TCG-generated guest code still selects host vector features at runtime.

### Windows x86_64 cross build

Install a MinGW-w64 cross compiler and the usual build tools (Debian/Ubuntu: `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 build-essential meson ninja-build pkg-config python3 python3-venv curl zstd flex bison`), plus the IA-64 cross toolchain for the firmware.

`scripts/fetch-win64-deps.sh` downloads the pinned MSYS2 MinGW64 libraries (SDL2, glib, pixman, libslirp, and their dependencies) from `repo.msys2.org`, verifies each SHA-256, assembles a sysroot, and prints its path.
GTK is not part of the pinned manifest; the Windows build uses SDL.
`repo.msys2.org` rolls its package files, so when a pinned URL disappears, update the version and hash in the script from the current MSYS2 repository in the same commit.

```sh
WIN_SYSROOT="$(./scripts/fetch-win64-deps.sh)"
HOST_PKG_CONFIG="$(command -v pkg-config)"
mkdir -p build-win64
(
  cd build-win64
  PKG_CONFIG="$HOST_PKG_CONFIG" \
  PKG_CONFIG_LIBDIR="$WIN_SYSROOT/mingw64/lib/pkgconfig" \
  PKG_CONFIG_SYSROOT_DIR="$WIN_SYSROOT" \
  ../configure \
    --cross-prefix=x86_64-w64-mingw32- \
    --host-cc=gcc \
    --python=/usr/bin/python3 \
    --target-list=ia64-softmmu \
    --without-default-features \
    --enable-system \
    --enable-tcg \
    --enable-pixman \
    --enable-fdt=internal \
    --enable-sdl \
    --enable-slirp \
    --enable-vnc \
    --disable-docs \
    --disable-werror
)
ninja -C build-win64 qemu-system-ia64.exe qemu-system-ia64w.exe \
  roms/ia64-firmware/ia64-firmware.bin
```

To make the result relocatable, copy beside the executables: the runtime DLLs from `$WIN_SYSROOT/mingw64/bin` (`SDL2.dll libglib-2.0-0.dll libiconv-2.dll libintl-8.dll libpcre2-8-0.dll libpixman-1-0.dll libslirp-0.dll zlib1.dll`), `libwinpthread-1.dll` from `x86_64-w64-mingw32-gcc -print-file-name=libwinpthread-1.dll`, the built `ia64-firmware.bin`, and the `pc-bios` ROMs and keymaps the machine loads. Each run loads one graphics ROM and one network ROM depending on the selected devices, so bundle all of them to keep every parameter combination working: `vgabios-ati.bin` (default ATI graphics), `vgabios-stdvga.bin` (`-vga std`), `vgabios-mach64.bin` (`vga=mach64`), `pxe-eepro100.rom` (the default 8255x NICs), `efi-e1000.rom` (`e1000`-family NICs such as `e1000-82543gc`/`e1000-82545em`), and `efi-e1000e.rom` (`e1000e` NIC).


### Console and debugging

Use `-serial stdio` to view serial output.
The `-debug-port` option publishes the guest debug transport described by the ACPI DBGP table; for example, `-debug-port tcp::4444,server=on,wait=on,nodelay=on`.

It is this project's own option and is unrelated to QEMU's `-debugcon`.

Three logging categories are useful when bringing up a guest:

- `-d guest_errors` prints decoded guest debug output, including the assertion and `DbgPrint` text produced by Windows checked builds
- `-d ia64_fault` logs rare or fatal IA-64 fault classes (illegal operation, NaT consumption, unaligned reference, privileged operation, break) and excludes routine TLB, paging, and external-interrupt activity
- `-d ia32_fault` logs IA-32 execution-layer faults and instruction intercepts with EIP, opcode bytes, and registers

## Tests

Run the behavior-oriented IA-64 unit, TCG, and machine tests after building:

```sh
build/pyvenv/bin/meson test -C build --suite ia64 --print-errorlogs
build/pyvenv/bin/meson test -C build --suite qtest-ia64 --print-errorlogs
build/pyvenv/bin/meson test -C build --suite func-ia64 --print-errorlogs
build/pyvenv/bin/meson test -C build --suite func-ia64-thorough --print-errorlogs
```

Use the build-local Meson shown above.

It is the same version selected by QEMU's configure process; a host `meson` of another version may be unable to read `build/meson-private/build.dat`.

Plain `meson test` from the source directory is not valid because the Meson build data lives under `build`.

The TCG registry currently contains more than 1000 architectural microprograms divided between core, memory/NaT, floating-point, RSE, MMU, interruption, and PAL groups.
Each group run reports its own case count, so that total can be re-derived after adding cases.
Machine tests cover platform wiring and display behavior.

The functional suite builds project-owned EFI applications and boots them from deterministic FAT, GPT, MBR, El Torito, and UDF media. It also exercises the firmware shell through PS/2, USB, and serial input, including direct application execution and NVRAM persistence across restarts.

## Legal disclaimer

This repository does not include third-party operating system images, disk images, firmware images, machine ROM dumps, proprietary firmware blobs, or operating system binaries.

Guest operating system images, firmware, installation media, and other third-party materials must be supplied by users under their own applicable licenses.

This project is an independent experimental QEMU IA-64 system emulation project. It is not affiliated with, endorsed by, sponsored by, or supported by Intel, HPE, the QEMU Project or Microsoft Corporation.

QEMU is used as the upstream base for this fork. QEMU as a whole is licensed under the GNU General Public License, version 2. See the license files in this repository for details.

Microsoft and Windows are trademarks of the Microsoft group of companies.

All other product names, project names, company names, and trademarks are the property of their respective owners.
