#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

if [ "$#" -ne 7 ]; then
    echo "usage: $0 BIN RAW ELF MAP SECTIONS SOURCE_DIR DEPFILE" >&2
    exit 2
fi

OUT_BIN="$1"
OUT_RAW="$2"
FW_ELF="$3"
FW_MAP="$4"
FW_SECTIONS="$5"
SRC_DIR="$6"
DEPFILE="$7"
OUT_DIR="$(dirname "$OUT_BIN")"
INCLUDE_DIR="${SRC_DIR}/../../include"
MANIFEST="${SRC_DIR}/firmware.sources"

AS="${AS:-ia64-linux-gnu-as}"
CC="${CC:-ia64-linux-gnu-gcc}"
LD="${LD:-ia64-linux-gnu-ld}"
OBJCOPY="${OBJCOPY:-ia64-linux-gnu-objcopy}"
SIZE="${SIZE:-ia64-linux-gnu-size}"
IASL="${IASL:-iasl}"
LIBGCC="$("$CC" -print-libgcc-file-name)"

set --
LINKER_SCRIPT=
DEPFILES=
ASL_SOURCES=
ACPI_SIZES_H="${OUT_DIR}/ia64-fw-acpi-aml.h"

# Compile the ACPI table sources first: the generated .inc/.h fragments are
# included by firmware.c, so the AML byte arrays can never drift from the
# .asl sources again.  -on -oi keep integer literals as typed opcodes
# (Linux 2.4's ACPI CA rejects ZeroOp/OneOp *package elements* -- see
# plans/status.md 2.3).
: > "$ACPI_SIZES_H"
while IFS= read -r source || [ -n "$source" ]; do
    case "$source" in
        *.asl) ;;
        *) continue ;;
    esac
    source_path="${SRC_DIR}/${source}"
    stem="${source##*/}"
    stem="${stem%.*}"
    aml="${OUT_DIR}/ia64-fw-${stem}.aml"
    inc="${OUT_DIR}/ia64-fw-${stem}.inc"
    "$IASL" -p "${OUT_DIR}/ia64-fw-${stem}" -on -oi "$source_path" \
        > "${OUT_DIR}/ia64-fw-${stem}.iasl.log" 2>&1 || {
        command cat "${OUT_DIR}/ia64-fw-${stem}.iasl.log" >&2
        echo "iasl failed for $source (set IASL= to a working binary)" >&2
        exit 2
    }
    # Strip the 36-byte SDT header: the firmware builds and patches its own.
    od -An -v -tx1 -j36 "$aml" | \
        awk '{ for (i = 1; i <= NF; i++) printf "0x%s,", $i; print "" }' \
        > "$inc"
    aml_size=$(( $(wc -c < "$aml") - 36 ))
    guard="$(printf '%s' "$stem" | tr 'a-z-' 'A-Z_')"
    printf '#define FW_%s_AML_SIZE %su\n' "$guard" "$aml_size" \
        >> "$ACPI_SIZES_H"
    ASL_SOURCES="${ASL_SOURCES} ${source_path}"
done < "$MANIFEST"

while IFS= read -r source || [ -n "$source" ]; do
    case "$source" in
        ''|'#'*)
            continue
            ;;
    esac

    source_path="${SRC_DIR}/${source}"
    stem="${source##*/}"
    stem="${stem%.*}"
    object="${OUT_DIR}/ia64-fw-${stem}.o"

    case "$source" in
        *.S)
            # Via $CC so the C preprocessor runs: entry.S includes the shared
            # ABI header.  Dependencies tracked like the C objects.
            object_depfile="${object}.d"
            "$CC" -nostdinc -nostdlib -I"$INCLUDE_DIR" \
                -MMD -MP -MF "$object_depfile" -MT "$OUT_BIN" \
                -c -o "$object" "$source_path"
            set -- "$@" "$object"
            DEPFILES="${DEPFILES} ${object_depfile}"
            ;;
        *.c)
            object_depfile="${object}.d"
            "$CC" -O2 -fno-builtin -ffreestanding -nostdinc -nostdlib \
                -G 0 -mno-sdata -fno-stack-protector -fno-common \
                -fno-optimize-sibling-calls -fno-pic \
                -I"$INCLUDE_DIR" -I"$OUT_DIR" \
                -Wall -Wextra -Wno-unused-parameter \
                -MMD -MP -MF "$object_depfile" -MT "$OUT_BIN" \
                -c -o "$object" "$source_path"
            set -- "$@" "$object"
            DEPFILES="${DEPFILES} ${object_depfile}"
            ;;
        *.asl)
            # Compiled in the first pass above.
            ;;
        *.lds)
            LINKER_SCRIPT="$source_path"
            ;;
        *)
            echo "unsupported firmware manifest entry: $source" >&2
            exit 2
            ;;
    esac
done < "$MANIFEST"

if [ -z "$LINKER_SCRIPT" ]; then
    echo "firmware manifest does not name a linker script" >&2
    exit 2
fi

"$LD" -nostdlib -static -T "$LINKER_SCRIPT" -Map="$FW_MAP" \
    -o "$FW_ELF" "$@" "$LIBGCC"
"$OBJCOPY" -O binary "$FW_ELF" "${OUT_BIN}.raw"
"$SIZE" -A "$FW_ELF" > "$FW_SECTIONS"

# Self-relocation fixup table (rework phase 2.2): link twice more at shifted
# bases, derive the table from the binary diffs, prove it by reconstruction,
# and inject it into the reserved .fw_fixups region.  See fw-fixups.py.
FW_ALT1_DELTA=0x80000
FW_ALT2_DELTA=0x200000
"$LD" -nostdlib -static -T "$LINKER_SCRIPT" \
    --defsym FW_LINK_BASE=$((0x100000 + FW_ALT1_DELTA)) \
    -o "${FW_ELF}.alt1" "$@" "$LIBGCC"
"$LD" -nostdlib -static -T "$LINKER_SCRIPT" \
    --defsym FW_LINK_BASE=$((0x100000 + FW_ALT2_DELTA)) \
    -o "${FW_ELF}.alt2" "$@" "$LIBGCC"
"$OBJCOPY" -O binary "${FW_ELF}.alt1" "${OUT_BIN}.alt1"
"$OBJCOPY" -O binary "${FW_ELF}.alt2" "${OUT_BIN}.alt2"
FIXUPS_VA="$("${NM:-ia64-linux-gnu-nm}" "$FW_ELF" | \
    awk '$3 == "__fw_fixups_start" { print "0x" $1 }')"
if [ -z "$FIXUPS_VA" ]; then
    echo "__fw_fixups_start not found in $FW_ELF" >&2
    exit 2
fi
python3 "${SRC_DIR}/fw-fixups.py" "${OUT_BIN}.raw" \
    "${OUT_BIN}.alt1" "$FW_ALT1_DELTA" \
    "${OUT_BIN}.alt2" "$FW_ALT2_DELTA" \
    "$(( FIXUPS_VA - 0x100000 ))" "$OUT_RAW"
rm -f "${OUT_BIN}.raw" "${OUT_BIN}.alt1" "${OUT_BIN}.alt2" \
    "${FW_ELF}.alt1" "${FW_ELF}.alt2"

# The shipped image is a flash image: the flat body plus a FIT and the reset
# pointer block, mapped by the machine to end at 4 GiB (see fw-flash.py).
python3 "${SRC_DIR}/fw-flash.py" "$OUT_RAW" "$FW_ELF" "$OUT_BIN"

{
    for dependency in $DEPFILES; do
        command cat "$dependency"
    done
    echo "$OUT_BIN: $MANIFEST $LINKER_SCRIPT $SRC_DIR/build_firmware.sh" \
        "$SRC_DIR/fw-fixups.py" "$SRC_DIR/fw-flash.py" "$ASL_SOURCES"
} > "$DEPFILE"
