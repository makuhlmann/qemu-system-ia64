# qemu-system-ia64 — notes for coding agents

This is a QEMU fork with an IA-64 (Itanium) system target. Its goal is to run IA-64
editions of Windows, Linux and other IA-64 operating systems, on two emulated platforms:

- **`-M zx1`** (default; alias `ia64-vpc`): the HP zx1 board ("Longs Peak", as in the
  rx2600 / zx2000 / zx6000), Itanium 2. For Server 2003 and up, as well as most Linux and
  HP-UX operating systems.
- **`-M 460gx`**: the Intel 460GX SDV (re-badged by HP as the i2000), original Itanium
  ("Merced"). For XP build 2600, certain Windows beta versions, as well as older Linux,
  HP-UX and the pre-release AIX operating system.

The code is the reference. Start here: `target/ia64/` (CPU), `hw/ia64/` (machines:
`longspeak.c`, `sdv.c`, `ia64_base.c`), `roms/ia64-firmware/` (our EFI/SAL/PAL firmware),
`tests/unit/ia64/` (microprogram tests), `tests/qtest/ia64-vpc-test.c`,
`tests/functional/ia64/`, `contrib/packaging/` (release packages). `-M <machine>,help`
lists the machine options.

## Build and test

- The firmware needs an IA-64 cross toolchain (`ia64-linux-gnu-gcc`/`binutils`) and `iasl`.
  Fedora packages them; `.github/workflows/ci.yml` has the full package list.
- `./configure --target-list=ia64-softmmu && ninja -C build` builds QEMU and the firmware.
- The regression gate must be fully green before every commit:
  `build/pyvenv/bin/meson test -C build --suite ia64 --suite qtest-ia64 --suite func-ia64`
- Every change to CPU or PAL behaviour needs a microprogram test.
- ACPI, PCI or firmware-table changes, and RSE, MMU or interrupt-routing changes, can pass
  the gate and still break a guest. They need a guest run (XP 2600 and Server 2003,
  uniprocessor and `-smp 2`). If you cannot do that run, say so.

## Rules

- Work on `develop`, or on a topic branch that the maintainer merges into it. `main` is the
  release branch and takes `develop` through merges.
  Changes from the original upstream repository are generally reviewed and ported.
- Do not open a pull request unless the maintainer asks for one.
- Model what the real hardware does. Do not add workarounds for problems that real
  hardware would also have, and do not change behaviour by detecting which guest runs.
- No proprietary binaries: no vendor option ROMs, BIOS or firmware images, and no tables
  copied out of retail images.
- Base hardware and firmware behaviour on official documentation and specifications
  whenever possible: the Intel Itanium SDM, the SAL and EFI specifications, chipset and
  board manuals, vendor firmware documentation. They are not part of this repository, so
  ask the user to provide them rather than relying on secondary sources.
- Some comments and commit messages cite `WSRV03/…` or `WXPSP1/…` paths. These refer to
  reference material that is not part of this repository. Never copy or translate code
  from it.

## Comments and commit messages

- Write a comment only for a reason, a constraint, a quirk of the hardware, firmware or
  guest, or a citation (vendor document and section, firmware address, `path:line`). Do not
  repeat what the code says.
- Commit subject: `<area>: <verb> what changed`, for example
  `hw/ia64: added the Longs Peak PDH clock`. Body: about three to eight lines on what
  changed and why, with the evidence. Leave out test results, file lists and the story of
  the investigation.
