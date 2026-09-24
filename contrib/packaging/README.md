# Packaging build scripts

Self-contained, performance-optimized release builds of `qemu-system-ia64`. The
scripts package the source tree they live in. They build in their own trees under
`build-packaging/.work-*` (removed after each build), never touch `build/`, and place
every finished artifact in the unified output folder **`build-packaging/dist/`**.
`build-packaging/` is ignored by git (`/build-*/`).

| Script | Output (in `build-packaging/dist/`) | Notes |
|---|---|---|
| `build-firmware.sh` | `qemu-system-ia64-firmware_<ver>.zip` | IA-64 EFI/SAL/PAL firmware. Also leaves the raw blob in `build-packaging/firmware/` for the other builds to consume. |
| `build-windows.sh` | `qemu-system-ia64_<ver>_win64.zip` | MinGW-w64 cross build in an ephemeral fedora:44 podman container. Fully self-contained (every non-system DLL bundled), ships the `qemu-img`/`qemu-io`/… utilities. GTK and SDL displays. |
| `deb/ubuntu-{22,24,26}.04.sh` | `..._ubuntuXX.04_amd64.deb` | Per-release `.deb`, built in an ephemeral podman container. Deps resolve seamlessly via apt. |
| `deb/debian-{11,12,13}.sh` | `..._debianNN_amd64.deb` | Same, for Debian. |
| `build-all.sh` | all of the above | firmware → Windows → every `.deb`; one failing does not abort the rest. |

## Quick start

From the top of the source tree:

```bash
contrib/packaging/build-firmware.sh       # firmware zip + build-packaging/firmware/ia64-firmware.bin  (run first)
contrib/packaging/build-windows.sh        # Windows zip
contrib/packaging/deb/ubuntu-24.04.sh     # one .deb
contrib/packaging/build-all.sh            # firmware + Windows + all 6 .deb releases
# everything lands in build-packaging/dist/
```

`build-packaging/firmware/ia64-firmware.bin` must exist before the Windows/`.deb`
builds (their containers have no IA-64 toolchain); run `build-firmware.sh` first
(`build-all.sh` does this automatically).

## Source location

The scripts build the tree they are part of (two levels above this directory),
including uncommitted changes. `QEMU_SRC` points them at another checkout instead.
The package version counts commits, so the checkout must not be shallow; the scripts
warn if it is (`git fetch --unshallow` fixes it).

## Cleanup and logs

Every build removes its work/temp trees on exit — **success or failure** — leaving only
the finished products and logs. Each build tees its output to
`build-packaging/logs/<name>.log`, and on failure Meson's logs are salvaged there too,
so a failure can still be diagnosed after the scratch is gone. `KEEP_WORK=1` (or
`INCREMENTAL=1`) preserves the work tree.

Install a `.deb` (apt resolves the dependencies):

```bash
sudo apt install ./build-packaging/dist/qemu-system-ia64_<ver>_ubuntu24.04_amd64.deb
qemu-system-ia64 -M zx1 -display gtk \
  -bios /usr/lib/qemu-system-ia64/share/qemu/ia64-firmware.bin
```

`-M zx1` (the default, Itanium 2) suits Windows Server 2003 and XP build 3790;
use `-M 460gx` (original Itanium) for XP build 2002 and earlier.

## Options (environment variables)

| Var | Default | Effect |
|---|---|---|
| `JOBS` | `nproc` | Parallel build jobs. |
| `LTO` | `1` | `--enable-lto`. Set `0` for a much faster build / lower memory. |
| `STRIP` | `1` | Strip installed binaries (smaller package). `0` keeps symbols. |
| `NATIVE` | `0` | `1` adds `-march=native` and `-Doptimization=3`. **Not portable** — the binary only runs on the build host's CPU generation. |
| `INCREMENTAL` | `0` | `1` reuses the work tree for a faster rebuild (skips a clean configure and the cleanup). |
| `KEEP_WORK` | `0` | `1` keeps the work/temp trees after a build (for debugging). |
| `QEMU_SRC` | *(this tree)* | Path to another source checkout to package. |
| `PKG_OUT` | `build-packaging` | Output root: `dist/`, `firmware/`, `logs/` and the work trees. |
| `BASE_IMAGE` | *(per wrapper)* | `.deb` builds: override the container image (e.g. a different registry/tag). |

## Prerequisites

- **IA-64 firmware toolchain** (`build-firmware.sh` only) — `ia64-linux-gnu-*` on
  PATH. Fedora packages it as `gcc-ia64-linux-gnu` and `binutils-ia64-linux-gnu`;
  on other hosts, PATH shims that forward into a Fedora podman container work too
  (a container named `fedora` is started if it exists but is stopped).
- **Windows build** — `podman` and `zip`. The MinGW-w64 toolchain and the GTK stack
  are installed inside the fedora:44 container by `win-in-container.sh`.
- **`.deb` builds** — `podman`. Everything else is installed inside the container.

Meson is not needed on PATH — QEMU's `configure` bootstraps its own into the work
tree's `pyvenv/`.

## "Self-contained" — what that means here

- **Windows**: fully self-contained. All bundled DLLs are resolved from the QEMU
  binaries' import tables (3-pass sweep); only OS-provided system DLLs are left out.
- **Debian/Ubuntu `.deb`**: the recommended distribution form. Runtime dependencies
  are generated per release by `dpkg-shlibdeps` and resolved by apt; see the pipeline
  section below for the package layout.

## `.deb` pipeline (`deb/`)

Each release has a thin wrapper (`ubuntu-XX.04.sh` / `debian-NN.sh`) that spins up an
ephemeral podman container, installs the build dependencies, and runs
`in-container.sh` — which does a curated build (GTK+VTE, SDL, VNC, audio, slirp,
curl, USB passthrough; everything else off), stages a minimal payload, injects the
prebuilt firmware, computes `Depends:` with `dpkg-shlibdeps`, and emits the `.deb`.
`lib-deb.sh` holds the shared build-dep list and the `podman run` orchestration.

- **Supported:** Ubuntu 22.04 / 24.04 / 26.04 and Debian 11 / 12 / 13. (Ubuntu 20.04
  is out — its glib 2.64 and Python 3.8 are below QEMU 11's minimums of glib 2.66 /
  Python 3.9.)
- The container script adapts to each release automatically: it probes the compiler
  for supported cflags, disables gnutls where its version is too old, and lets
  `dpkg-shlibdeps` generate the correct per-release runtime `Depends:` (handling the
  t64 library rename). No per-release dependency lists.
- **Version:** `1:11.0.50+makuhlmann<N>` where `N = git rev-list --count HEAD`, since
  the fork keeps QEMU's version at 11.0.50.
- **Package layout:** ships only the `qemu-system-ia64` binary + firmware/ROMs in a
  private `/usr/lib/qemu-system-ia64/` tree with a `/usr/bin` symlink, so it never
  collides with the distro's own qemu packages; `qemu-img` comes from the recommended
  `qemu-utils`.
- **Other distros:** copy a wrapper and change `BASE_IMAGE` (and `DEV_PACKAGES` if a
  name differs). The container does the rest.

## Pitfalls

- `win-in-container.sh` must pass `--target-list=ia64-softmmu`. Without it,
  `--enable-system` builds every QEMU target and the build seems to never end.
- `win-in-container.sh` runs plain `ninja`, so the firmware target builds in the container
  too. Keep `acpica-tools` (for `iasl`) and the IA-64 cross packages in its `dnf` list; the
  build replaces that blob with the shared one afterwards.
- The `.deb` container has no IA-64 toolchain. It builds only `ninja qemu-system-ia64` and
  injects `build-packaging/firmware/ia64-firmware.bin`; `ninja install` would fail on the
  firmware target.
- `--without-default-features` turns every optional feature off. Enable each wanted feature
  explicitly; `-Dcurses=enabled` needs `-Diconv=enabled` as well.
- Older releases are handled inside `in-container.sh`, not with per-release lists: it probes
  each performance cflag, provides a TOML parser (`python3-tomli` or pip), builds from a
  writable copy of `/src` (old pip writes `qemu.egg-info` into the source), and disables
  gnutls below 3.7.5.
