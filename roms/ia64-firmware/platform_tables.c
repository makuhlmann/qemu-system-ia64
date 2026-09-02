/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Guest platform tables: the SAL System Table (SST_) and every ACPI table
 * (FACS/FADT/DSDT/SSDT/XSDT/RSDT/RSDP/MADT/MCFG/SRAT/SLIT/HCDP/DBGP), their
 * placement in the ACPI reclaim window, the SSDT byte-patching helpers, and
 * the table integrity selftest.  Extracted verbatim from firmware.c
 * (Phase 1 milestone 4 of plans/firmware-rework-plan.md); the split of
 * efi_init_platform_tables into per-table builders is the follow-up step.
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-acpi.h"
#include "fw-memmap.h"
#include "fw-storage.h"
#include "fw-platform-handoff.h"
#include "fw-platform-layout.h"
#include "linker-symbols.h"
#include "ia64-fw-acpi-aml.h"

static IA64_SAL_SYSTEM_TABLE   mSalSystemTable;
static ACPI_FADT               mFadt;
static ACPI_XSDT               mXsdt;
static ACPI_RSDT               mRsdt;
static ACPI_RSDP               mRsdp;
static ACPI_FACS               mFacs __attribute__((aligned(64)));
/*
 * Source: dsdt-pci-root.asl (compiled with iasl -on).
 *
 * AML body for \_SB.PCI0 with _HID/_CID, _CRS windows (including the
 * parent window for the UART child), and _PRT entries routing the fixed
 * root-bus PCI INTx pins to IOSAPIC GSIs 16..19.
 *
 * The root bridge _HID must be PNP0A03 (conventional PCI), not PNP0A08:
 * some guest OS installers validate every ancestor device of the install
 * disk against a fixed hardware-compatibility list that predates PCI
 * Express and only recognizes *PNP0A03 root bridges, and a string _CID
 * is not matched in its wildcard form there.  The emulated root bus is
 * conventional PCI, so PNP0A03 is also the accurate identifier.
 */
static ACPI_DSDT               mDsdt = {
    .Aml = {
        /* Generated from dsdt-pci-root.asl by build_firmware.sh. */
#include "ia64-fw-dsdt-pci-root.inc"
    },
};
static ACPI_SSDT               mSsdt = {
    .Aml = {
        /*
         * Generated from ssdt-platform-devices.asl by build_firmware.sh
         * (iasl -on -oi keeps every patchable value an AML BytePrefix
         * object).
         *
         * Scope (\_SB) contains CPU0..CPU7 with patchable CxEN _STA values.
         * Scope (\_SB.PCI0) carries P2EN, UAR0 (PNP0501, GSI 4), PS2K and
         * PS2M gated on P2EN.
         */
#include "ia64-fw-ssdt-platform-devices.inc"
    },
};
/*
 * The zx1-profile AML: PCI0 nested inside the HP zx1 SBA IOC (HWP0001) so
 * Linux sba_iommu binds, and the SSDT re-scoped to \_SB.SBA0.PCI0.  Copied
 * over mDsdt.Aml / mSsdt.Aml at table build when fw_platform_is_zx1(); the flat
 * 460gx AML above stays the static default.
 */
static const UINT8 mDsdtZx1Aml[] = {
#include "ia64-fw-dsdt-pci-root-zx1.inc"
};
static const UINT8 mSsdtZx1Aml[] = {
#include "ia64-fw-ssdt-platform-devices-zx1.inc"
};
FW_STATIC_ASSERT(sizeof(mDsdtZx1Aml) <= sizeof(((ACPI_DSDT *)0)->Aml),
                 dsdt_zx1_fits);
FW_STATIC_ASSERT(sizeof(mSsdtZx1Aml) <= sizeof(((ACPI_SSDT *)0)->Aml),
                 ssdt_zx1_fits);
/*
 * The guest-visible AML length of the published DSDT/SSDT (header + active AML
 * body), set from the chipset profile at table build.  The header Length,
 * checksum and integrity selftest all use these rather than sizeof(ACPI_DSDT),
 * since the struct is sized to the larger variant.
 */
static UINT32 mAcpiDsdtLength;
static UINT32 mAcpiSsdtLength;
/*
 * The patchable SSDT bytes are byte-encoded Name objects (NameOp, 4-char
 * name, BytePrefix, value).  Locate them by name instead of by raw offset
 * so an AML edit cannot silently shift the patch targets.
 */
static const UINT8 mSsdtCpuEnabledNames[IA64_VPC_MAX_CPUS][4] = {
    { 'C', '0', 'E', 'N' },
    { 'C', '1', 'E', 'N' },
    { 'C', '2', 'E', 'N' },
    { 'C', '3', 'E', 'N' },
    { 'C', '4', 'E', 'N' },
    { 'C', '5', 'E', 'N' },
    { 'C', '6', 'E', 'N' },
    { 'C', '7', 'E', 'N' },
};
static const UINT8 mSsdtPs2EnabledName[4] = { 'P', '2', 'E', 'N' };

static UINT8 *acpi_ssdt_named_byte(ACPI_SSDT *Ssdt, const UINT8 Name[4])
{
    UINTN i;

    for (i = 0; i + 6U < sizeof(Ssdt->Aml); i++) {
        if (Ssdt->Aml[i] == 0x08U &&
            Ssdt->Aml[i + 1U] == Name[0] &&
            Ssdt->Aml[i + 2U] == Name[1] &&
            Ssdt->Aml[i + 3U] == Name[2] &&
            Ssdt->Aml[i + 4U] == Name[3] &&
            Ssdt->Aml[i + 5U] == 0x0aU) {
            return &Ssdt->Aml[i + 6U];
        }
    }
    return NULL;
}

static BOOLEAN acpi_ssdt_set_named_byte(ACPI_SSDT *Ssdt,
                                        const UINT8 Name[4], UINT8 Value)
{
    UINT8 *value = acpi_ssdt_named_byte(Ssdt, Name);

    if (value == NULL) {
        return 0;
    }
    *value = Value;
    return 1;
}

static BOOLEAN acpi_ssdt_named_byte_is(ACPI_SSDT *Ssdt,
                                       const UINT8 Name[4], UINT8 Value)
{
    const UINT8 *value = acpi_ssdt_named_byte(Ssdt, Name);

    return value != NULL && *value == Value;
}
static ACPI_MCFG               mMcfg;
static ACPI_MADT               mMadt;
static ACPI_SRAT               mSrat;
static ACPI_SLIT               mSlit;
static ACPI_HCDP               mHcdp;
static ACPI_DBGP               mDbgp;
static ACPI_RSDP              *mAcpiRsdp;
static ACPI_XSDT              *mAcpiXsdt;
static ACPI_RSDT              *mAcpiRsdt;
static ACPI_FADT              *mAcpiFadt;
static ACPI_FACS              *mAcpiFacs;
static ACPI_DSDT              *mAcpiDsdt;
static ACPI_SSDT              *mAcpiSsdt;
static ACPI_MADT              *mAcpiMadt;
static ACPI_MCFG              *mAcpiMcfg;
static ACPI_SRAT              *mAcpiSrat;
static ACPI_SLIT              *mAcpiSlit;
static ACPI_HCDP              *mAcpiHcdp;
static ACPI_DBGP              *mAcpiDbgp;
static UINTN                   mAcpiTableEnd;

static const UINT8 gEfiAcpi20TableGuid[16] = {
    0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
    0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
};
static const UINT8 gEfiAcpi10TableGuid[16] = {
    0x30, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
    0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

static void init_sdt_header(ACPI_SDT_HEADER *hdr, UINT32 sig, UINT32 len)
{
    UINTN i;
    hdr->Signature = sig;
    hdr->Length = len;
    hdr->Revision = 1;
    hdr->Checksum = 0;
    /*
     * IA-64 Linux acpi_get_sysname() selects the platform machvec from the
     * XSDT OEM ID: it wants the hpzx1 machvec (which registers sba_iommu and
     * routes DMA through the IOC) versus the generic "dig" machvec (swiotlb
     * only, no IOMMU).  It compares the 6-byte OEM ID for an *exact* "HP"
     * (strcmp), so the field must be NUL-padded "HP\0\0\0\0" -- a space-padded
     * "HP    " does NOT match and the guest falls back to "dig".  The 460gx
     * profile keeps QEMU.
     */
    {
        static const UINT8 oem_hp[6] = { 'H', 'P', 0, 0, 0, 0 };
        static const UINT8 oem_qemu[6] = { 'Q', 'E', 'M', 'U', ' ', ' ' };
        const UINT8 *oem = fw_platform_is_zx1() ? oem_hp : oem_qemu;

        for (i = 0; i < 6; i++) {
            hdr->OemId[i] = oem[i];
        }
    }
    for (i = 0; i < 8; i++) {
        hdr->OemTableId[i] = "IA64VMSR"[i];
    }
    hdr->OemRevision = 1;
    hdr->CreatorId = EFI_SIGNATURE_32('Q', 'E', 'M', 'U');
    hdr->CreatorRevision = 1;
}

static ACPI_GENERIC_ADDRESS acpi_gas(UINT8 space_id, UINT8 width,
                                     UINT64 address)
{
    ACPI_GENERIC_ADDRESS gas;

    gas.SpaceId = space_id;
    gas.BitWidth = width;
    gas.BitOffset = 0;
    gas.Reserved = 0;
    gas.AddressLow = (UINT32)address;
    gas.AddressHigh = (UINT32)(address >> 32);
    return gas;
}

static ACPI_GENERIC_ADDRESS acpi_system_memory_gas(UINT8 width, UINT64 address)
{
    return acpi_gas(ACPI_GAS_SYSTEM_MEMORY, width, address);
}

static ACPI_GENERIC_ADDRESS acpi_system_io_gas(UINT8 width, UINT64 address)
{
    return acpi_gas(ACPI_GAS_SYSTEM_IO, width, address);
}

static UINT64 acpi_gas_address(const ACPI_GENERIC_ADDRESS *gas)
{
    return ((UINT64)gas->AddressHigh << 32) | gas->AddressLow;
}

static BOOLEAN acpi_gas_matches(const ACPI_GENERIC_ADDRESS *gas,
                                UINT8 space_id, UINT8 width, UINT64 address)
{
    return gas->SpaceId == space_id &&
           gas->BitWidth == width &&
           gas->BitOffset == 0 &&
           gas->Reserved == 0 &&
           acpi_gas_address(gas) == address;
}

static void acpi_srat_init_memory_affinity(
    ACPI_SRAT_MEMORY_AFFINITY *Memory, UINT64 Base, UINT64 End,
    BOOLEAN Enabled)
{
    UINT64 length = End > Base ? End - Base : 0;

    fw_set_mem(Memory, sizeof(*Memory), 0);
    Memory->Type = 1;
    Memory->Length = sizeof(*Memory);
    Memory->ProximityDomain = 0;
    Memory->BaseAddrLow = (UINT32)Base;
    Memory->BaseAddrHigh = (UINT32)(Base >> 32);
    Memory->LengthLow = (UINT32)length;
    Memory->LengthHigh = (UINT32)(length >> 32);
    Memory->Flags = Enabled && length != 0 ? 1 : 0;
}

static UINT64 acpi_srat_memory_base(
    const ACPI_SRAT_MEMORY_AFFINITY *Memory)
{
    return (UINT64)Memory->BaseAddrLow |
           ((UINT64)Memory->BaseAddrHigh << 32);
}

static UINT64 acpi_srat_memory_length(
    const ACPI_SRAT_MEMORY_AFFINITY *Memory)
{
    return (UINT64)Memory->LengthLow |
           ((UINT64)Memory->LengthHigh << 32);
}

static UINTN acpi_align_up(UINTN value, UINTN align)
{
    return (value + align - 1U) & ~(align - 1U);
}

static void acpi_use_static_tables(void)
{
    UINTN end;

    mAcpiRsdp = &mRsdp;
    mAcpiXsdt = &mXsdt;
    mAcpiRsdt = &mRsdt;
    mAcpiFadt = &mFadt;
    mAcpiFacs = &mFacs;
    mAcpiDsdt = &mDsdt;
    mAcpiSsdt = &mSsdt;
    mAcpiMadt = &mMadt;
    mAcpiMcfg = &mMcfg;
    mAcpiSrat = &mSrat;
    mAcpiSlit = &mSlit;
    mAcpiHcdp = &mHcdp;
    mAcpiDbgp = &mDbgp;

    end = (UINTN)&mRsdp + sizeof(mRsdp);
    if ((UINTN)&mFacs + sizeof(mFacs) > end) {
        end = (UINTN)&mFacs + sizeof(mFacs);
    }
    if ((UINTN)&mDsdt + sizeof(mDsdt) > end) {
        end = (UINTN)&mDsdt + sizeof(mDsdt);
    }
    if ((UINTN)&mSsdt + sizeof(mSsdt) > end) {
        end = (UINTN)&mSsdt + sizeof(mSsdt);
    }
    if ((UINTN)&mFadt + sizeof(mFadt) > end) {
        end = (UINTN)&mFadt + sizeof(mFadt);
    }
    if ((UINTN)&mXsdt + sizeof(mXsdt) > end) {
        end = (UINTN)&mXsdt + sizeof(mXsdt);
    }
    if ((UINTN)&mRsdt + sizeof(mRsdt) > end) {
        end = (UINTN)&mRsdt + sizeof(mRsdt);
    }
    if ((UINTN)&mMadt + sizeof(mMadt) > end) {
        end = (UINTN)&mMadt + sizeof(mMadt);
    }
    if ((UINTN)&mMcfg + sizeof(mMcfg) > end) {
        end = (UINTN)&mMcfg + sizeof(mMcfg);
    }
    if ((UINTN)&mSrat + sizeof(mSrat) > end) {
        end = (UINTN)&mSrat + sizeof(mSrat);
    }
    if ((UINTN)&mSlit + sizeof(mSlit) > end) {
        end = (UINTN)&mSlit + sizeof(mSlit);
    }
    if ((UINTN)&mHcdp + sizeof(mHcdp) > end) {
        end = (UINTN)&mHcdp + sizeof(mHcdp);
    }
    if ((UINTN)&mDbgp + sizeof(mDbgp) > end) {
        end = (UINTN)&mDbgp + sizeof(mDbgp);
    }
    mAcpiTableEnd = end;
}

static BOOLEAN acpi_assign_reclaim_tables(void)
{
    UINTN cursor = ACPI_RECLAIM_BASE;

    mAcpiFacs = (ACPI_FACS *)acpi_align_up(cursor, 64);
    cursor = (UINTN)mAcpiFacs + sizeof(*mAcpiFacs);
    if (cursor > ACPI_RECLAIM_TABLE_BASE) {
        acpi_use_static_tables();
        return 0;
    }

    /*
     * FACS is writable firmware/OS handshake state and must survive ACPI
     * S1-S3.  Give it a dedicated IA-64-sized EfiACPIMemoryNVS descriptor;
     * all reclaimable boot-time ACPI tables start at the next 8 KiB boundary.
     */
    cursor = ACPI_RECLAIM_TABLE_BASE;
    mAcpiRsdp = (ACPI_RSDP *)acpi_align_up(cursor, 16);
    cursor = (UINTN)mAcpiRsdp + sizeof(*mAcpiRsdp);
    mAcpiDsdt = (ACPI_DSDT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiDsdt + sizeof(*mAcpiDsdt);
    mAcpiSsdt = (ACPI_SSDT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiSsdt + sizeof(*mAcpiSsdt);
    mAcpiFadt = (ACPI_FADT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiFadt + sizeof(*mAcpiFadt);
    mAcpiXsdt = (ACPI_XSDT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiXsdt + sizeof(*mAcpiXsdt);
    mAcpiRsdt = (ACPI_RSDT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiRsdt + sizeof(*mAcpiRsdt);
    mAcpiMadt = (ACPI_MADT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiMadt + sizeof(*mAcpiMadt);
    mAcpiMcfg = (ACPI_MCFG *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiMcfg + sizeof(*mAcpiMcfg);
    mAcpiSrat = (ACPI_SRAT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiSrat + sizeof(*mAcpiSrat);
    mAcpiSlit = (ACPI_SLIT *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiSlit + sizeof(*mAcpiSlit);
    mAcpiHcdp = (ACPI_HCDP *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiHcdp + sizeof(*mAcpiHcdp);
    mAcpiDbgp = (ACPI_DBGP *)acpi_align_up(cursor, 8);
    cursor = (UINTN)mAcpiDbgp + sizeof(*mAcpiDbgp);
    mAcpiTableEnd = cursor;

    if (mAcpiTableEnd < ACPI_RECLAIM_BASE ||
        mAcpiTableEnd > ACPI_RECLAIM_END) {
        acpi_use_static_tables();
        return 0;
    }
    return 1;
}

static void acpi_publish_reclaim_tables(void)
{
    fw_copy_mem(mAcpiRsdp, &mRsdp, sizeof(mRsdp));
    fw_copy_mem(mAcpiFacs, &mFacs, sizeof(mFacs));
    fw_copy_mem(mAcpiDsdt, &mDsdt, sizeof(mDsdt));
    fw_copy_mem(mAcpiSsdt, &mSsdt, sizeof(mSsdt));
    fw_copy_mem(mAcpiFadt, &mFadt, sizeof(mFadt));
    fw_copy_mem(mAcpiXsdt, &mXsdt, sizeof(mXsdt));
    fw_copy_mem(mAcpiRsdt, &mRsdt, sizeof(mRsdt));
    fw_copy_mem(mAcpiMadt, &mMadt, sizeof(mMadt));
    fw_copy_mem(mAcpiMcfg, &mMcfg, sizeof(mMcfg));
    fw_copy_mem(mAcpiSrat, &mSrat, sizeof(mSrat));
    fw_copy_mem(mAcpiSlit, &mSlit, sizeof(mSlit));
    fw_copy_mem(mAcpiHcdp, &mHcdp, sizeof(mHcdp));
    fw_copy_mem(mAcpiDbgp, &mDbgp, sizeof(mDbgp));
}

static BOOLEAN acpi_published_range_valid(const VOID *Table, UINTN Size,
                                          UINTN Align)
{
    UINTN addr = (UINTN)Table;
    UINTN end = addr + Size;

    return Table != NULL &&
           Align != 0 &&
           (addr & (Align - 1U)) == 0 &&
           end >= addr &&
           addr >= ACPI_RECLAIM_BASE &&
           end <= ACPI_RECLAIM_END &&
           end <= mAcpiTableEnd;
}

static void efi_init_sal_system_table(void)
{
    UINTN i;

    mSalSystemTable.Signature = EFI_SIGNATURE_32('S', 'S', 'T', '_');
    mSalSystemTable.Length = sizeof(mSalSystemTable);
    mSalSystemTable.Revision = fw_sal_revision();
    mSalSystemTable.EntryCount = 8;
    mSalSystemTable.Checksum = 0;
    for (i = 0; i < sizeof(mSalSystemTable.Reserved0); i++) {
        mSalSystemTable.Reserved0[i] = 0;
    }
    mSalSystemTable.SalAVersion = 0x0100;
    mSalSystemTable.SalBVersion = 0x0100;
    /*
     * Reference-platform identity per personality (rework D13): the SDV /
     * i2000 for the 460GX profile, the SR870BH2 for the E8870 profile.
     */
    {
        const char *oem = "Intel Corp.";
        const char *product = fw_platform_is_460gx() ? "SDV460GX"
                                                     : "SR870BH2";

        for (i = 0; i < sizeof(mSalSystemTable.OemId); i++) {
            mSalSystemTable.OemId[i] = oem[i] != 0 ? (UINT8)oem[i] : 0;
            if (oem[i] == 0) {
                break;
            }
        }
        for (; i < sizeof(mSalSystemTable.OemId); i++) {
            mSalSystemTable.OemId[i] = 0;
        }
        for (i = 0; i < sizeof(mSalSystemTable.ProductId); i++) {
            mSalSystemTable.ProductId[i] =
                product[i] != 0 ? (UINT8)product[i] : 0;
            if (product[i] == 0) {
                break;
            }
        }
        for (; i < sizeof(mSalSystemTable.ProductId); i++) {
            mSalSystemTable.ProductId[i] = 0;
        }
    }
    for (i = 0; i < sizeof(mSalSystemTable.Reserved1); i++) {
        mSalSystemTable.Reserved1[i] = 0;
    }
    mSalSystemTable.Entrypoint.Type = 0;
    for (i = 0; i < sizeof(mSalSystemTable.Entrypoint.Reserved0); i++) {
        mSalSystemTable.Entrypoint.Reserved0[i] = 0;
    }
    mSalSystemTable.Entrypoint.PalProc = (UINTN)pal_proc_entry;
    /*
     * SAL_PROC points at the mode-agnostic runtime stub (entry.S): the
     * machine re-enters the C dispatcher physically so the OS may call
     * SAL_PROC in virtual mode (SAL spec 3.1) even though the firmware
     * itself is linked at fixed physical addresses.  The handshake block
     * next to the stub carries the C entry, GP and a firmware-owned
     * physical stack/backing store for that re-entry.
     */
    mSalSystemTable.Entrypoint.SalProc = (UINTN)sal_runtime_entry;
    mSalSystemTable.Entrypoint.SalGp = fw_current_gp();
    for (i = 0; i < sizeof(mSalSystemTable.Entrypoint.Reserved1); i++) {
        mSalSystemTable.Entrypoint.Reserved1[i] = 0;
    }
    {
        volatile UINT64 *block = (volatile UINT64 *)(UINTN)sal_dispatch_block;

        block[0] = fw_sal_proc_function_entry();
        block[1] = fw_current_gp();
        block[2] = FW_SAL_PHYS_STACK_TOP;
        block[3] = FW_SAL_PHYS_BSTORE_BASE;
    }
    {
        IA64_SAL_MEMORY_DESCRIPTOR *md = mSalSystemTable.MemoryDescriptors;
        UINTN image_base = (UINTN)pal_proc_entry & ~0xFFFULL;
        UINTN data_start = (UINTN)&__runtime_data_start;
        UINTN image_end = ((UINTN)&_end + 0xFFFULL) & ~0xFFFULL;
        UINTN n;

        for (n = 0; n < 4; n++) {
            md[n].Type = 1;
            md[n].NeedVaReg = 0;
            md[n].CurrentAttribute = 0;
            md[n].PageAccessRights = 0;
            md[n].SupportedAttributes = 0;
            md[n].Reserved0 = 0;
            for (i = 0; i < sizeof(md[n].Reserved1); i++) {
                md[n].Reserved1[i] = 0;
            }
            for (i = 0; i < sizeof(md[n].OemReserved); i++) {
                md[n].OemReserved[i] = 0;
            }
        }
        /* PAL code: the boot page holding PAL_PROC. */
        md[0].MemoryType = SAL_MEM_TYPE_REGULAR;
        md[0].MemoryUsage = SAL_MEM_USAGE_PAL_CODE;
        md[0].PhysicalAddress = image_base;
        md[0].Length = 1;
        /* SAL code: the page holding the SAL_PROC runtime stubs. */
        md[1].MemoryType = SAL_MEM_TYPE_REGULAR;
        md[1].MemoryUsage = SAL_MEM_USAGE_SAL_CODE;
        md[1].PhysicalAddress = (UINTN)sal_runtime_entry & ~0xFFFULL;
        md[1].Length = 1;
        /* SAL data: the firmware's runtime data (SST, SAL state). */
        md[2].MemoryType = SAL_MEM_TYPE_REGULAR;
        md[2].MemoryUsage = SAL_MEM_USAGE_SAL_DATA;
        md[2].PhysicalAddress = data_start;
        md[2].Length = (UINT32)((image_end - data_start) >> 12);
        /* Firmware ROM space (SAL_UPDATE_PAL mapping target). */
        md[3].MemoryType = SAL_MEM_TYPE_FIRMWARE_CODE;
        md[3].MemoryUsage = SAL_MEM_USAGE_FW_SAL_PAL;
        md[3].PhysicalAddress = image_base;
        md[3].Length = (UINT32)((image_end - image_base) >> 12);
    }
    mSalSystemTable.PlatformFeatures.Type = 2;
    /* SAL spec platform-feature bit 0: bus lock, a 460GX-era feature. */
    mSalSystemTable.PlatformFeatures.Features =
        fw_platform_is_460gx() ? 0x01U : 0x00U;
    for (i = 0; i < sizeof(mSalSystemTable.PlatformFeatures.Reserved); i++) {
        mSalSystemTable.PlatformFeatures.Reserved[i] = 0;
    }
    mSalSystemTable.TranslationRegister.Type = 3;
    mSalSystemTable.TranslationRegister.RegisterType = 0;
    mSalSystemTable.TranslationRegister.RegisterNumber = 0;
    for (i = 0;
         i < sizeof(mSalSystemTable.TranslationRegister.Reserved0); i++) {
        mSalSystemTable.TranslationRegister.Reserved0[i] = 0;
    }
    /*
     * Truthful ITR(0) (rework D11): name the firmware's actual identity
     * mapping - the 1 MB window at the image shadow base - instead of a
     * fictitious VA 0.
     */
    mSalSystemTable.TranslationRegister.VirtualAddress =
        (UINT64)(UINTN)__fw_image_start;
    mSalSystemTable.TranslationRegister.EncodedPageSize =
        SAL_TR_ENCODED_PAGE_SIZE;
    mSalSystemTable.TranslationRegister.Reserved1 = 0;
    mSalSystemTable.ApWake.Type = 5;
    mSalSystemTable.ApWake.Mechanism = 0;
    for (i = 0; i < sizeof(mSalSystemTable.ApWake.Reserved); i++) {
        mSalSystemTable.ApWake.Reserved[i] = 0;
    }
    mSalSystemTable.ApWake.Vector = 0xff;
    mSalSystemTable.Checksum =
        table_checksum8(&mSalSystemTable, sizeof(mSalSystemTable));

}

/*
 * Where the primary VGA adapter lives.  Neither machine puts it on the first
 * root: on zx1 the AGP graphics sits behind the Mercury (LBA) bridge on
 * IA64_MERCURY_BUS at slot IA64_MERCURY_VGA_SLOT, and on the i2000 it sits
 * behind the GXB expander on IA64_460GX_GXB_BUS at device
 * IA64_460GX_GXB_VGA_SLOT.  The PCDP/HCDP VGA console descriptor and the id
 * probe use these.
 */
static UINT8 fw_vga_pci_bus(void)
{
    return fw_platform_is_zx1() ? (UINT8)IA64_MERCURY_BUS :
                                  (UINT8)IA64_460GX_GXB_BUS;
}

static UINT8 fw_vga_pci_device(void)
{
    return fw_platform_is_zx1() ? (UINT8)IA64_MERCURY_VGA_SLOT :
                                  (UINT8)IA64_460GX_GXB_VGA_SLOT;
}

static void efi_init_acpi_tables(void)
{
    UINTN i;
    BOOLEAN vga_primary = fw_handoff_vga_console_primary();
    UINT32 vga_id = (UINT32)pci_config_read_value(0, fw_vga_pci_bus(), fw_vga_pci_device(), 0, 0, 4);
    UINT64 debug_port_base = fw_handoff_debug_port_base();
    BOOLEAN debug_port_present = debug_port_base != 0;
    BOOLEAN is_460gx = fw_platform_is_460gx();
    UINTN acpi_entries = 6U + (is_460gx ? 0U : 1U) +
                         (debug_port_present ? 1U : 0U);
    UINT32 xsdt_length = 36 + (UINT32)acpi_entries * 8U;
    UINT32 rsdt_length = 36 + (UINT32)acpi_entries * 4U;

    mFacs.Signature = EFI_SIGNATURE_32('F', 'A', 'C', 'S');
    mFacs.Length = sizeof(mFacs);
    mFacs.HardwareSignature = 0;
    mFacs.FirmwareWakingVector = 0;
    mFacs.GlobalLock = 0;
    mFacs.Flags = 0;
    mFacs.XFirmwareWakingVector = 0;
    mFacs.Version = 1;
    for (i = 0; i < sizeof(mFacs.Reserved0); i++) {
        mFacs.Reserved0[i] = 0;
    }
    mFacs.OspmFlags = 0;
    for (i = 0; i < sizeof(mFacs.Reserved1); i++) {
        mFacs.Reserved1[i] = 0;
    }

    /*
     * Select the DSDT AML for the chipset profile.  The flat 460gx AML is the
     * static initializer; the zx1 profile copies in its nested HWP0001 DSDT.
     * The header Length and checksum cover only the active AML body, so the
     * unused tail of the (max-sized) buffer is never guest-visible.
     */
    if (fw_platform_is_zx1()) {
        fw_copy_mem(mDsdt.Aml, mDsdtZx1Aml, sizeof(mDsdtZx1Aml));
        mAcpiDsdtLength = sizeof(ACPI_SDT_HEADER) + FW_DSDT_PCI_ROOT_ZX1_AML_SIZE;
    } else {
        mAcpiDsdtLength = sizeof(ACPI_SDT_HEADER) + FW_DSDT_PCI_ROOT_AML_SIZE;
    }
    init_sdt_header(&mDsdt.Hdr, EFI_SIGNATURE_32('D', 'S', 'D', 'T'),
                    mAcpiDsdtLength);
    mDsdt.Hdr.Revision = 2;
    mDsdt.Hdr.Checksum = table_checksum8(&mDsdt, mAcpiDsdtLength);

    init_sdt_header(&mFadt.Hdr, EFI_SIGNATURE_32('F', 'A', 'C', 'P'),
                    sizeof(mFadt));
    mFadt.Hdr.Revision = 3;
    mFadt.FirmwareCtrl = (UINT32)(UINTN)mAcpiFacs;
    mFadt.Dsdt = (UINT32)(UINTN)mAcpiDsdt;
    mFadt.Model = 0;
    mFadt.PreferredProfile = 4;
    mFadt.SciInterrupt = ACPI_SCI_IRQ;
    mFadt.SmiCommand = 0;
    mFadt.AcpiEnable = 0;
    mFadt.AcpiDisable = 0;
    mFadt.S4BiosRequest = 0;
    mFadt.PStateControl = 0;
    mFadt.Pm1aEventBlock = ACPI_PM_IO_BASE + ACPI_PM1_EVT_OFFSET;
    mFadt.Pm1bEventBlock = 0;
    mFadt.Pm1aControlBlock = ACPI_PM_IO_BASE + ACPI_PM1_CNT_OFFSET;
    mFadt.Pm1bControlBlock = 0;
    mFadt.Pm2ControlBlock = 0;
    mFadt.PmTimerBlock = ACPI_PM_IO_BASE + ACPI_PM_TMR_OFFSET;
    mFadt.Gpe0Block = 0;
    mFadt.Gpe1Block = 0;
    mFadt.Pm1EventLength = 4;
    mFadt.Pm1ControlLength = 2;
    mFadt.Pm2ControlLength = 0;
    mFadt.PmTimerLength = 4;
    mFadt.Gpe0BlockLength = 0;
    mFadt.Gpe1BlockLength = 0;
    mFadt.Gpe1Base = 0;
    mFadt.CstControl = 0;
    mFadt.C2Latency = 0;
    mFadt.C3Latency = 0;
    mFadt.FlushSize = 0;
    mFadt.FlushStride = 0;
    mFadt.DutyOffset = 0;
    mFadt.DutyWidth = 0;
    mFadt.DayAlarm = 0;
    mFadt.MonthAlarm = 0;
    mFadt.Century = 0;
    mFadt.BootFlags = 0;
    mFadt.Reserved0 = 0;
    mFadt.Flags = ACPI_FADT_FLAG_WBINVD |
                  ACPI_FADT_FLAG_SLP_BUTTON |
                  ACPI_FADT_FLAG_RESET_REG_SUP |
                  ACPI_FADT_FLAG_SW_CPU_SLP;
    mFadt.ResetRegister.SpaceId = ACPI_GAS_SYSTEM_IO;
    mFadt.ResetRegister.BitWidth = 8;
    mFadt.ResetRegister.BitOffset = 0;
    mFadt.ResetRegister.Reserved = 0;
    mFadt.ResetRegister.AddressLow =
        ACPI_PM_IO_BASE + ACPI_PM_RESET_OFFSET;
    mFadt.ResetRegister.AddressHigh = 0;
    mFadt.ResetValue = ACPI_PM_RESET_VALUE;
    for (i = 0; i < sizeof(mFadt.Reserved1); i++) {
        mFadt.Reserved1[i] = 0;
    }
    mFadt.XFirmwareCtrl = (UINT64)(UINTN)mAcpiFacs;
    mFadt.XDsdt = (UINT64)(UINTN)mAcpiDsdt;
    mFadt.XPm1aEventBlock =
        acpi_system_io_gas(32, ACPI_PM_IO_BASE + ACPI_PM1_EVT_OFFSET);
    mFadt.XPm1bEventBlock = acpi_system_memory_gas(0, 0);
    mFadt.XPm1aControlBlock =
        acpi_system_io_gas(16, ACPI_PM_IO_BASE + ACPI_PM1_CNT_OFFSET);
    mFadt.XPm1bControlBlock = acpi_system_memory_gas(0, 0);
    mFadt.XPm2ControlBlock = acpi_system_memory_gas(0, 0);
    mFadt.XPmTimerBlock =
        acpi_system_io_gas(32, ACPI_PM_IO_BASE + ACPI_PM_TMR_OFFSET);
    mFadt.XGpe0Block = acpi_system_memory_gas(0, 0);
    mFadt.XGpe1Block = acpi_system_memory_gas(0, 0);
    mFadt.Hdr.Checksum = table_checksum8(&mFadt, sizeof(mFadt));

    /*
     * Select the SSDT AML for the chipset profile before patching enable bytes
     * (the zx1 SSDT re-scopes its devices under \_SB.SBA0.PCI0).  The CxEN/P2EN
     * NameOp bytes exist in both variants, so the by-name patch below is
     * profile-agnostic.
     */
    if (fw_platform_is_zx1()) {
        fw_copy_mem(mSsdt.Aml, mSsdtZx1Aml, sizeof(mSsdtZx1Aml));
        mAcpiSsdtLength = sizeof(ACPI_SDT_HEADER) +
                          FW_SSDT_PLATFORM_DEVICES_ZX1_AML_SIZE;
    } else {
        mAcpiSsdtLength = sizeof(ACPI_SDT_HEADER) +
                          FW_SSDT_PLATFORM_DEVICES_AML_SIZE;
    }
    {
        UINTN cpu;

        for (cpu = 0; cpu < FW_ARRAY_SIZE(mSsdtCpuEnabledNames); cpu++) {
            acpi_ssdt_set_named_byte(&mSsdt, mSsdtCpuEnabledNames[cpu],
                                     fw_guest_processor_count() > cpu ? 0x0fU : 0);
        }
    }
    acpi_ssdt_set_named_byte(&mSsdt, mSsdtPs2EnabledName,
                             fw_handoff_i8042_enabled() ? 0x0fU : 0);
    init_sdt_header(&mSsdt.Hdr, EFI_SIGNATURE_32('S', 'S', 'D', 'T'),
                    mAcpiSsdtLength);
    mSsdt.Hdr.Revision = 2;
    mSsdt.Hdr.Checksum = table_checksum8(&mSsdt, mAcpiSsdtLength);

    init_sdt_header(&mXsdt.Hdr, EFI_SIGNATURE_32('X', 'S', 'D', 'T'),
                    xsdt_length);
    {
        UINTN xe = 0;

        mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiFadt;
        mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiMadt;
        mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiSrat;
        mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiSlit;
        mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiHcdp;
        /* No MCFG on the 460GX profile: config space is SAL_PCI_CONFIG. */
        if (!is_460gx) {
            mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiMcfg;
        }
        mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiSsdt;
        if (debug_port_present) {
            mXsdt.Entry[xe++] = (UINT64)(UINTN)mAcpiDbgp;
        }
        while (xe < sizeof(mXsdt.Entry) / sizeof(mXsdt.Entry[0])) {
            mXsdt.Entry[xe++] = 0;
        }
    }
    mXsdt.Hdr.Checksum = table_checksum8(&mXsdt, mXsdt.Hdr.Length);

    init_sdt_header(&mRsdt.Hdr, EFI_SIGNATURE_32('R', 'S', 'D', 'T'),
                    rsdt_length);
    {
        UINTN re = 0;

        mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiFadt;
        mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiMadt;
        mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiSrat;
        mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiSlit;
        mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiHcdp;
        if (!is_460gx) {
            mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiMcfg;
        }
        mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiSsdt;
        if (debug_port_present) {
            mRsdt.Entry[re++] = (UINT32)(UINTN)mAcpiDbgp;
        }
        while (re < sizeof(mRsdt.Entry) / sizeof(mRsdt.Entry[0])) {
            mRsdt.Entry[re++] = 0;
        }
    }
    mRsdt.Hdr.Checksum = table_checksum8(&mRsdt, mRsdt.Hdr.Length);

    init_sdt_header(&mMcfg.Hdr, EFI_SIGNATURE_32('M', 'C', 'F', 'G'),
                    sizeof(mMcfg));
    mMcfg.Hdr.Revision = 1;
    mMcfg.Reserved = 0;
    mMcfg.Allocation[0].BaseAddress = PCI_CONFIG_ECAM_BASE;
    mMcfg.Allocation[0].PciSegmentGroup = 0;
    mMcfg.Allocation[0].StartBusNumber = 0;
    mMcfg.Allocation[0].EndBusNumber =
        (UINT8)(PCI_CONFIG_ECAM_SIZE / 0x100000U - 1U);
    mMcfg.Allocation[0].Reserved = 0;
    mMcfg.Hdr.Checksum = table_checksum8(&mMcfg, sizeof(mMcfg));

    init_sdt_header(&mMadt.Hdr, EFI_SIGNATURE_32('A', 'P', 'I', 'C'), sizeof(mMadt));
    mMadt.Hdr.Revision = 2;
    mMadt.LocalApicAddr = (UINT32)FW_LOCAL_SAPIC_BASE;
    /*
     * PCAT_COMPAT.  This platform presents a PC/AT legacy interrupt space:
     * the i8042 sits at 0x60/0x64 driving IOSAPIC pins 1 and 12, the UART is
     * on pin 4, the SCI on pin 9, and pins 0..15 are the ISA IRQ numbers
     * identity-mapped, exactly as on the Merced-era platforms this machine
     * models (460GX + PIIX4E, whose south bridge really does carry the 8259
     * pair an APIC-mode OS is told to mask).
     *
     * The bit is what tells an OS that pins 0..15 *are* the ISA IRQs.  Linux
     * IA-64 keys its entire legacy-IRQ setup off it: iosapic_init()
     * (arch/ia64/kernel/iosapic.c) programs redirection entries for pins
     * 0..15 only `if ((base_irq == 0) && pcat_compat)`, and the PS/2 driver
     * asks for isa_irq_to_vector(1) unconditionally
     * (include/asm-ia64/keyboard.h).  With the bit clear no ISA pin is ever
     * routed, pc_keyb's send_data() never sees the ACK its interrupt handler
     * is supposed to post, and the guest reports
     * "keyboard: Timeout - AT keyboard not present?(ed)" and takes no input.
     * The kernel considers firmware that leaves it clear on such a platform
     * broken and hard-codes 1 for CONFIG_ITANIUM builds; Debian's kernel is
     * built CONFIG_MCKINLEY, so it believes us.
     *
     * Windows does not need it - it routes the i8042 from PS2K/PS2M's ACPI
     * _CRS instead - and is unaffected either way.
     */
    mMadt.Flags = ACPI_MADT_FLAG_PCAT_COMPAT;
    for (i = 0; i < FW_MAX_CPUS; i++) {
        mMadt.Lsapic[i].Type = 7;
        mMadt.Lsapic[i].Length = sizeof(mMadt.Lsapic[i]);
        mMadt.Lsapic[i].ProcessorId = i;
        mMadt.Lsapic[i].Id = i;
        mMadt.Lsapic[i].Eid = 0;
        mMadt.Lsapic[i].Reserved[0] = 0;
        mMadt.Lsapic[i].Reserved[1] = 0;
        mMadt.Lsapic[i].Reserved[2] = 0;
        mMadt.Lsapic[i].Flags = i < fw_guest_processor_count() ? 1 : 0;
    }
    mMadt.Iosapic.Type = 6;
    mMadt.Iosapic.Length = sizeof(mMadt.Iosapic);
    mMadt.Iosapic.Id = 0;
    mMadt.Iosapic.Reserved = 0;
    mMadt.Iosapic.GsiBase = 0;
    mMadt.Iosapic.Address = IOSAPIC_BASE;
    mMadt.Hdr.Checksum = table_checksum8(&mMadt, sizeof(mMadt));

    init_sdt_header(&mSrat.Hdr, EFI_SIGNATURE_32('S', 'R', 'A', 'T'),
                    sizeof(mSrat));
    mSrat.Hdr.Revision = 1;
    mSrat.TableRevision = 1;
    mSrat.Reserved = 0;
    acpi_srat_init_memory_affinity(&mSrat.Memory[0], 0, mGuestLowRamEnd, 1);
    for (i = 0; i < FW_HIGH_RAM_RANGE_MAX; i++) {
        if (i < fw_guest_high_ram_count()) {
            acpi_srat_init_memory_affinity(&mSrat.Memory[i + 1U],
                                           fw_guest_high_ram_base(i),
                                           fw_guest_high_ram_end(i), 1);
        } else {
            acpi_srat_init_memory_affinity(&mSrat.Memory[i + 1U], 0, 0, 0);
        }
    }
    for (i = 0; i < FW_MAX_CPUS; i++) {
        UINTN j;

        mSrat.Processor[i].Type = 0;
        mSrat.Processor[i].Length = sizeof(mSrat.Processor[i]);
        mSrat.Processor[i].ProximityDomain = 0;
        mSrat.Processor[i].ApicId = mMadt.Lsapic[i].Id;
        mSrat.Processor[i].Flags = i < fw_guest_processor_count() ? 1 : 0;
        mSrat.Processor[i].LsapicEid = mMadt.Lsapic[i].Eid;
        for (j = 0; j < sizeof(mSrat.Processor[i].Reserved); j++) {
            mSrat.Processor[i].Reserved[j] = 0;
        }
    }
    mSrat.Hdr.Checksum = table_checksum8(&mSrat, sizeof(mSrat));

    init_sdt_header(&mSlit.Hdr, EFI_SIGNATURE_32('S', 'L', 'I', 'T'),
                    sizeof(mSlit));
    mSlit.Hdr.Revision = 1;
    mSlit.Localities = 1;
    mSlit.Entry[0] = 10;
    mSlit.Hdr.Checksum = table_checksum8(&mSlit, sizeof(mSlit));

    init_sdt_header(&mHcdp.Hdr, EFI_SIGNATURE_32('H', 'C', 'D', 'P'),
                    sizeof(mHcdp));
    mHcdp.Hdr.Revision = 3;
    /* Only the fixed-length type 0/1 UART descriptors are counted here. */
    mHcdp.EntryCount = 1;
    mHcdp.Uart[0].Type = 0;
    mHcdp.Uart[0].Bits = 8;
    mHcdp.Uart[0].Parity = 1;
    mHcdp.Uart[0].StopBits = 1;
    mHcdp.Uart[0].PciSegment = 0;
    mHcdp.Uart[0].PciBus = 0;
    mHcdp.Uart[0].PciDevice = 0;
    mHcdp.Uart[0].PciFunction = 0;
    mHcdp.Uart[0].Baud = 115200;
    mHcdp.Uart[0].BaseAddress.SpaceId = 0;
    mHcdp.Uart[0].BaseAddress.BitWidth = 8;
    mHcdp.Uart[0].BaseAddress.BitOffset = 0;
    mHcdp.Uart[0].BaseAddress.Reserved = 0;
    mHcdp.Uart[0].BaseAddress.AddressLow = (UINT32)IA64_UART_BASE;
    mHcdp.Uart[0].BaseAddress.AddressHigh =
        (UINT32)(IA64_UART_BASE >> 32);
    /* With the PCI flag clear, these fields carry ACPI _HID and _UID. */
    mHcdp.Uart[0].PciDeviceId =
        (UINT16)HCDP_UART_ACPI_HID_PNP0501;
    mHcdp.Uart[0].PciVendorId =
        (UINT16)(HCDP_UART_ACPI_HID_PNP0501 >> 16);
    mHcdp.Uart[0].GlobalInterrupt = 4;
    mHcdp.Uart[0].ClockRate = HCDP_UART_PSEUDO_CLOCK_RATE;
    mHcdp.Uart[0].PciProgrammingInterface = 0x02;
    mHcdp.Uart[0].Flags =
        HCDP_UART_FLAG_ACTIVE_LOW | HCDP_UART_FLAG_INTERRUPT |
        (vga_primary ? 0 : HCDP_UART_FLAG_PRIMARY_CONSOLE);
    mHcdp.Uart[0].ConOutIndex = HCDP_CONOUT_UART_INDEX;
    mHcdp.Uart[0].Reserved = 0;
    mHcdp.Device[0].Type = HCDP_DEVICE_TYPE_VGA_CONSOLE;
    mHcdp.Device[0].Flags =
        vga_primary ? HCDP_DEVICE_FLAG_PRIMARY_CONSOLE : 0;
    mHcdp.Device[0].Length = sizeof(mHcdp.Device[0]);
    mHcdp.Device[0].EfiIndex = HCDP_CONOUT_VGA_INDEX;
    mHcdp.Device[0].Pci.Interconnect = HCDP_PCI_INTERFACE_TYPE;
    mHcdp.Device[0].Pci.Reserved = 0;
    mHcdp.Device[0].Pci.Length = sizeof(mHcdp.Device[0].Pci);
    mHcdp.Device[0].Pci.Segment = 0;
    mHcdp.Device[0].Pci.Bus = fw_vga_pci_bus();
    mHcdp.Device[0].Pci.Device = fw_vga_pci_device();
    mHcdp.Device[0].Pci.Function = 0;
    mHcdp.Device[0].Pci.DeviceId = (UINT16)(vga_id >> 16);
    mHcdp.Device[0].Pci.VendorId = (UINT16)vga_id;
    mHcdp.Device[0].Pci.AcpiInterrupt = 0;
    mHcdp.Device[0].Pci.MmioTranslation = 0;
    mHcdp.Device[0].Pci.IoPortTranslation = LEGACY_IO_BASE;
    mHcdp.Device[0].Pci.Flags = 0;
    mHcdp.Device[0].Pci.Translation = HCDP_PCI_TRANSLATE_IOPORT;
    mHcdp.Device[0].Vga.Count = 0;
    mHcdp.Hdr.Checksum = table_checksum8(&mHcdp, sizeof(mHcdp));

    init_sdt_header(&mDbgp.Hdr, EFI_SIGNATURE_32('D', 'B', 'G', 'P'),
                    sizeof(mDbgp));
    mDbgp.InterfaceType = ACPI_DBGP_INTERFACE_16550_FULL;
    mDbgp.Reserved[0] = 0;
    mDbgp.Reserved[1] = 0;
    mDbgp.Reserved[2] = 0;
    mDbgp.BaseAddress = acpi_system_memory_gas(8, debug_port_base);
    mDbgp.Hdr.Checksum = table_checksum8(&mDbgp, sizeof(mDbgp));

    for (i = 0; i < 8; i++) {
        mRsdp.Signature[i] = "RSD PTR "[i];
    }
    {
        /* NUL-padded "HP" selects the hpzx1 machvec (see init_sdt_header). */
        static const UINT8 oem_hp[6] = { 'H', 'P', 0, 0, 0, 0 };
        static const UINT8 oem_qemu[6] = { 'Q', 'E', 'M', 'U', ' ', ' ' };
        const UINT8 *oem = fw_platform_is_zx1() ? oem_hp : oem_qemu;

        for (i = 0; i < 6; i++) {
            mRsdp.OemId[i] = oem[i];
        }
    }
    mRsdp.Checksum = 0;
    mRsdp.Revision = 2;
    mRsdp.RsdtAddress = (UINT32)(UINTN)mAcpiRsdt;
    mRsdp.Length = sizeof(mRsdp);
    mRsdp.XsdtAddress = (UINT64)(UINTN)mAcpiXsdt;
    mRsdp.ExtendedChecksum = 0;
    for (i = 0; i < 3; i++) {
        mRsdp.Reserved[i] = 0;
    }
    mRsdp.Checksum = table_checksum8(&mRsdp, 20);
    mRsdp.ExtendedChecksum = table_checksum8(&mRsdp, sizeof(mRsdp));

    acpi_publish_reclaim_tables();
    smbios_init_table();
}

static void efi_publish_config_tables(void)
{
    UINTN i;


    for (i = 0; i < 16; i++) {
        mConfigTables[PLATFORM_TABLE_ACPI20].VendorGuid[i] =
            gEfiAcpi20TableGuid[i];
    }
    mConfigTables[PLATFORM_TABLE_ACPI20].VendorTable = (UINTN)mAcpiRsdp;
    for (i = 0; i < 16; i++) {
        mConfigTables[PLATFORM_TABLE_ACPI10].VendorGuid[i] =
            gEfiAcpi10TableGuid[i];
    }
    mConfigTables[PLATFORM_TABLE_ACPI10].VendorTable = (UINTN)mAcpiRsdp;
    for (i = 0; i < 16; i++) {
        mConfigTables[PLATFORM_TABLE_SAL].VendorGuid[i] =
            gEfiSalSystemTableGuid[i];
    }
    mConfigTables[PLATFORM_TABLE_SAL].VendorTable = (UINTN)&mSalSystemTable;
    for (i = 0; i < 16; i++) {
        mConfigTables[PLATFORM_TABLE_HCDP].VendorGuid[i] =
            gEfiHcdpTableGuid[i];
    }
    mConfigTables[PLATFORM_TABLE_HCDP].VendorTable = (UINTN)mAcpiHcdp;
    for (i = 0; i < 16; i++) {
        mConfigTables[PLATFORM_TABLE_SMBIOS].VendorGuid[i] =
            gEfiSmbiosTableGuid[i];
    }
    mConfigTables[PLATFORM_TABLE_SMBIOS].VendorTable =
        fw_smbios_entry_point_address();
    for (i = 0; i < 16; i++) {
        mConfigTables[PLATFORM_TABLE_DEBUG_IMAGE].VendorGuid[i] =
            mDebugImageInfoTableGuid[i];
    }
    mConfigTables[PLATFORM_TABLE_DEBUG_IMAGE].VendorTable =
        (UINTN)&mDebugImageInfoHeader;

    mSystemTable.NumberOfTableEntries = PLATFORM_TABLE_INITIAL;
    mSystemTable.ConfigurationTable = mConfigTables;
}

void efi_init_platform_tables(void)
{
    (void)acpi_assign_reclaim_tables();
    efi_init_sal_system_table();
    efi_init_acpi_tables();
    efi_publish_config_tables();
    fw_platform_publish_tables(&mSalSystemTable, mAcpiRsdp);
}

static BOOLEAN acpi_sdt_integrity_valid(const ACPI_SDT_HEADER *Hdr,
                                        UINT32 Signature, UINT32 Length)
{
    return Hdr->Signature == Signature &&
           Hdr->Length == Length &&
           table_checksum8(Hdr, Length) == 0;
}

static BOOLEAN acpi_has_bytes(const UINT8 *Haystack, UINTN HaystackLen,
                              const UINT8 *Needle, UINTN NeedleLen)
{
    UINTN i;
    UINTN j;

    if (NeedleLen == 0 || NeedleLen > HaystackLen) {
        return 0;
    }

    for (i = 0; i <= HaystackLen - NeedleLen; i++) {
        for (j = 0; j < NeedleLen; j++) {
            if (Haystack[i + j] != Needle[j]) {
                break;
            }
        }
        if (j == NeedleLen) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN acpi_dsdt_has_bytes(const UINT8 *Needle, UINTN NeedleLen)
{
    return mAcpiDsdt != NULL &&
           acpi_has_bytes(mAcpiDsdt->Aml, sizeof(mAcpiDsdt->Aml),
                          Needle, NeedleLen);
}

static BOOLEAN acpi_ssdt_has_bytes(const UINT8 *Needle, UINTN NeedleLen)
{
    return mAcpiSsdt != NULL &&
           acpi_has_bytes(mAcpiSsdt->Aml, sizeof(mAcpiSsdt->Aml),
                          Needle, NeedleLen);
}

BOOLEAN __attribute__((noinline)) acpi_table_integrity_selftest(void)
{
    static const UINT8 pci0_name[] = { 'P', 'C', 'I', '0' };
    static const UINT8 s5_name[] = { '_', 'S', '5', '_' };
    static const UINT8 uar0_name[] = { 'U', 'A', 'R', '0' };
    static const UINT8 hid_pci_root[] = "PNP0A03";
    static const UINT8 cid_pci[] = "PNP0A03";
    static const UINT8 hid_uart[] = "PNP0501";
    static const UINT8 ps2_enabled[] = { 'P', '2', 'E', 'N' };
    static const UINT8 sta_name[] = { '_', 'S', 'T', 'A' };
    static const UINT8 crs_name[] = { '_', 'C', 'R', 'S' };
    static const UINT8 prt_name[] = { '_', 'P', 'R', 'T' };
    /* The zx1 profile nests PCI0 inside the SBA0 (HWP0001) IOC device. */
    static const UINT8 sba0_name[] = { 'S', 'B', 'A', '0' };
    static const UINT8 legacy_vga_memory[] = {
        0x87, 0x17, 0x00, 0x00, 0x0c, 0x03,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x0a, 0x00,
        0xff, 0xff, 0x0b, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00,
    };
    UINT32 vga_id = (UINT32)pci_config_read_value(0, fw_vga_pci_bus(), fw_vga_pci_device(), 0, 0, 4);
    UINT8 hcdp_uart_flags =
        HCDP_UART_FLAG_ACTIVE_LOW | HCDP_UART_FLAG_INTERRUPT |
        (fw_handoff_vga_console_primary() ?
         0 : HCDP_UART_FLAG_PRIMARY_CONSOLE);
    UINTN i;
    UINT64 debug_port_base = fw_handoff_debug_port_base();
    BOOLEAN debug_port_present = debug_port_base != 0;
    BOOLEAN is_460gx = fw_platform_is_460gx();
    UINTN acpi_entries = 6U + (is_460gx ? 0U : 1U) +
                         (debug_port_present ? 1U : 0U);
    UINT32 xsdt_length = 36 + (UINT32)acpi_entries * 8U;
    UINT32 rsdt_length = 36 + (UINT32)acpi_entries * 4U;

    if (mSalSystemTable.Signature != EFI_SIGNATURE_32('S', 'S', 'T', '_') ||
        mSalSystemTable.Length != sizeof(mSalSystemTable) ||
        mSalSystemTable.Revision != fw_sal_revision() ||
        mSalSystemTable.EntryCount != 8 ||
        mSalSystemTable.MemoryDescriptors[0].Type != 1 ||
        mSalSystemTable.MemoryDescriptors[0].MemoryUsage !=
            SAL_MEM_USAGE_PAL_CODE ||
        mSalSystemTable.MemoryDescriptors[1].MemoryUsage !=
            SAL_MEM_USAGE_SAL_CODE ||
        mSalSystemTable.MemoryDescriptors[2].MemoryUsage !=
            SAL_MEM_USAGE_SAL_DATA ||
        mSalSystemTable.MemoryDescriptors[3].MemoryType !=
            SAL_MEM_TYPE_FIRMWARE_CODE ||
        mSalSystemTable.MemoryDescriptors[0].Length == 0 ||
        mSalSystemTable.MemoryDescriptors[1].Length == 0 ||
        mSalSystemTable.MemoryDescriptors[2].Length == 0 ||
        mSalSystemTable.MemoryDescriptors[3].Length == 0 ||
        mSalSystemTable.Entrypoint.SalProc !=
            (UINT64)(UINTN)sal_runtime_entry ||
        (UINT64)(UINTN)sal_runtime_entry !=
            (UINT64)(UINTN)&__runtime_code_start ||
        mSalSystemTable.Entrypoint.Type != 0 ||
        mSalSystemTable.PlatformFeatures.Type != 2 ||
        mSalSystemTable.TranslationRegister.Type != 3 ||
        mSalSystemTable.TranslationRegister.RegisterType != 0 ||
        mSalSystemTable.TranslationRegister.RegisterNumber != 0 ||
        mSalSystemTable.TranslationRegister.VirtualAddress !=
            (UINT64)(UINTN)__fw_image_start ||
        mSalSystemTable.TranslationRegister.EncodedPageSize !=
            SAL_TR_ENCODED_PAGE_SIZE ||
        mSalSystemTable.TranslationRegister.Reserved1 != 0 ||
        mSalSystemTable.ApWake.Type != 5 ||
        mSalSystemTable.ApWake.Mechanism != 0 ||
        mSalSystemTable.ApWake.Vector != 0xff ||
        table_checksum8(&mSalSystemTable, sizeof(mSalSystemTable)) != 0) {
        return 0;
    }

    if (!acpi_published_range_valid(mAcpiRsdp, sizeof(*mAcpiRsdp), 16) ||
        !acpi_published_range_valid(mAcpiFacs, sizeof(*mAcpiFacs), 64) ||
        !acpi_published_range_valid(mAcpiDsdt, sizeof(*mAcpiDsdt), 8) ||
        !acpi_published_range_valid(mAcpiSsdt, sizeof(*mAcpiSsdt), 8) ||
        !acpi_published_range_valid(mAcpiFadt, sizeof(*mAcpiFadt), 8) ||
        !acpi_published_range_valid(mAcpiXsdt, sizeof(*mAcpiXsdt), 8) ||
        !acpi_published_range_valid(mAcpiRsdt, sizeof(*mAcpiRsdt), 8) ||
        !acpi_published_range_valid(mAcpiMadt, sizeof(*mAcpiMadt), 8) ||
        !acpi_published_range_valid(mAcpiMcfg, sizeof(*mAcpiMcfg), 8) ||
        !acpi_published_range_valid(mAcpiSrat, sizeof(*mAcpiSrat), 8) ||
        !acpi_published_range_valid(mAcpiSlit, sizeof(*mAcpiSlit), 8) ||
        !acpi_published_range_valid(mAcpiHcdp, sizeof(*mAcpiHcdp), 8) ||
        !acpi_published_range_valid(mAcpiDbgp, sizeof(*mAcpiDbgp), 8)) {
        return 0;
    }

    if (table_checksum8(mAcpiRsdp, 20) != 0 ||
        table_checksum8(mAcpiRsdp, sizeof(*mAcpiRsdp)) != 0 ||
        mAcpiRsdp->Revision != 2 ||
        mAcpiRsdp->Length != sizeof(*mAcpiRsdp) ||
        mAcpiRsdp->RsdtAddress != (UINT32)(UINTN)mAcpiRsdt ||
        mAcpiRsdp->XsdtAddress != (UINT64)(UINTN)mAcpiXsdt) {
        return 0;
    }

    if (!acpi_sdt_integrity_valid(&mAcpiDsdt->Hdr,
                                  EFI_SIGNATURE_32('D', 'S', 'D', 'T'),
                                  mAcpiDsdtLength) ||
        !acpi_sdt_integrity_valid(&mAcpiFadt->Hdr,
                                  EFI_SIGNATURE_32('F', 'A', 'C', 'P'),
                                  sizeof(*mAcpiFadt)) ||
        !acpi_sdt_integrity_valid(&mAcpiXsdt->Hdr,
                                  EFI_SIGNATURE_32('X', 'S', 'D', 'T'),
                                  xsdt_length) ||
        !acpi_sdt_integrity_valid(&mAcpiRsdt->Hdr,
                                  EFI_SIGNATURE_32('R', 'S', 'D', 'T'),
                                  rsdt_length) ||
        !acpi_sdt_integrity_valid(&mAcpiSsdt->Hdr,
                                  EFI_SIGNATURE_32('S', 'S', 'D', 'T'),
                                  mAcpiSsdtLength) ||
        !acpi_sdt_integrity_valid(&mAcpiMadt->Hdr,
                                  EFI_SIGNATURE_32('A', 'P', 'I', 'C'),
                                  sizeof(*mAcpiMadt)) ||
        !acpi_sdt_integrity_valid(&mAcpiSrat->Hdr,
                                  EFI_SIGNATURE_32('S', 'R', 'A', 'T'),
                                  sizeof(*mAcpiSrat)) ||
        !acpi_sdt_integrity_valid(&mAcpiSlit->Hdr,
                                  EFI_SIGNATURE_32('S', 'L', 'I', 'T'),
                                  sizeof(*mAcpiSlit)) ||
        !acpi_sdt_integrity_valid(&mAcpiHcdp->Hdr,
                                  EFI_SIGNATURE_32('H', 'C', 'D', 'P'),
                                  sizeof(*mAcpiHcdp)) ||
        !acpi_sdt_integrity_valid(&mAcpiDbgp->Hdr,
                                  EFI_SIGNATURE_32('D', 'B', 'G', 'P'),
                                  sizeof(*mAcpiDbgp)) ||
        !acpi_sdt_integrity_valid(&mAcpiMcfg->Hdr,
                                  EFI_SIGNATURE_32('M', 'C', 'F', 'G'),
                                  sizeof(*mAcpiMcfg))) {
        return 0;
    }

    if (!acpi_dsdt_has_bytes(pci0_name, sizeof(pci0_name)) ||
        !acpi_dsdt_has_bytes(s5_name, sizeof(s5_name)) ||
        !acpi_dsdt_has_bytes(hid_pci_root, sizeof(hid_pci_root) - 1) ||
        !acpi_dsdt_has_bytes(cid_pci, sizeof(cid_pci) - 1) ||
        !acpi_dsdt_has_bytes(crs_name, sizeof(crs_name)) ||
        !acpi_dsdt_has_bytes(legacy_vga_memory,
                             sizeof(legacy_vga_memory)) ||
        !acpi_dsdt_has_bytes(prt_name, sizeof(prt_name))) {
        return 0;
    }
    /* The zx1 DSDT must carry the SBA0 IOC that encloses PCI0 (HWP0001). */
    if (fw_platform_is_zx1() &&
        !acpi_dsdt_has_bytes(sba0_name, sizeof(sba0_name))) {
        return 0;
    }

    if (!acpi_ssdt_has_bytes(uar0_name, sizeof(uar0_name)) ||
        !acpi_ssdt_has_bytes(hid_uart, sizeof(hid_uart) - 1) ||
        !acpi_ssdt_has_bytes(ps2_enabled, sizeof(ps2_enabled)) ||
        !acpi_ssdt_has_bytes(sta_name, sizeof(sta_name)) ||
        !acpi_ssdt_named_byte_is(mAcpiSsdt, mSsdtPs2EnabledName,
                                 fw_handoff_i8042_enabled() ? 0x0fU : 0) ||
        !acpi_ssdt_has_bytes(crs_name, sizeof(crs_name))) {
        return 0;
    }
    for (i = 0; i < FW_MAX_CPUS; i++) {
        UINT8 cpu_name[4] = { 'C', 'P', 'U', (UINT8)('0' + i) };

        if (!acpi_ssdt_has_bytes(cpu_name, sizeof(cpu_name)) ||
            !acpi_ssdt_named_byte_is(mAcpiSsdt, mSsdtCpuEnabledNames[i],
                                     i < fw_guest_processor_count() ? 0x0fU : 0)) {
            return 0;
        }
    }

    if (mAcpiFacs->Signature != EFI_SIGNATURE_32('F', 'A', 'C', 'S') ||
        mAcpiFacs->Length != sizeof(*mAcpiFacs) ||
        mAcpiFadt->XFirmwareCtrl != (UINT64)(UINTN)mAcpiFacs ||
        mAcpiFadt->XDsdt != (UINT64)(UINTN)mAcpiDsdt ||
        mAcpiFadt->SciInterrupt != ACPI_SCI_IRQ ||
        mAcpiFadt->Pm1aEventBlock !=
            ACPI_PM_IO_BASE + ACPI_PM1_EVT_OFFSET ||
        mAcpiFadt->Pm1aControlBlock !=
            ACPI_PM_IO_BASE + ACPI_PM1_CNT_OFFSET ||
        mAcpiFadt->PmTimerBlock !=
            ACPI_PM_IO_BASE + ACPI_PM_TMR_OFFSET ||
        !acpi_gas_matches(&mAcpiFadt->XPm1aEventBlock,
                          ACPI_GAS_SYSTEM_IO, 32, ACPI_PM_IO_BASE) ||
        !acpi_gas_matches(&mAcpiFadt->XPm1aControlBlock,
                          ACPI_GAS_SYSTEM_IO, 16,
                          ACPI_PM_IO_BASE + ACPI_PM1_CNT_OFFSET) ||
        !acpi_gas_matches(&mAcpiFadt->XPmTimerBlock,
                          ACPI_GAS_SYSTEM_IO, 32,
                          ACPI_PM_IO_BASE + ACPI_PM_TMR_OFFSET) ||
        !acpi_gas_matches(&mAcpiFadt->ResetRegister,
                          ACPI_GAS_SYSTEM_IO, 8,
                          ACPI_PM_IO_BASE + ACPI_PM_RESET_OFFSET) ||
        mAcpiFadt->ResetValue != ACPI_PM_RESET_VALUE ||
        (mAcpiFadt->Flags & ACPI_FADT_FLAG_PWR_BUTTON) != 0 ||
        (mAcpiFadt->Flags & ACPI_FADT_FLAG_RESET_REG_SUP) == 0 ||
        (mAcpiFadt->Flags & ACPI_FADT_FLAG_SW_CPU_SLP) == 0) {
        return 0;
    }

    {
        BOOLEAN is_460gx = fw_platform_is_460gx();
        UINTN e = 5;

        if (mAcpiXsdt->Entry[0] != (UINT64)(UINTN)mAcpiFadt ||
            mAcpiXsdt->Entry[1] != (UINT64)(UINTN)mAcpiMadt ||
            mAcpiXsdt->Entry[2] != (UINT64)(UINTN)mAcpiSrat ||
            mAcpiXsdt->Entry[3] != (UINT64)(UINTN)mAcpiSlit ||
            mAcpiXsdt->Entry[4] != (UINT64)(UINTN)mAcpiHcdp ||
            (!is_460gx &&
             mAcpiXsdt->Entry[e] != (UINT64)(UINTN)mAcpiMcfg)) {
            return 0;
        }
        if (!is_460gx) {
            e++;
        }
        if (mAcpiXsdt->Entry[e] != (UINT64)(UINTN)mAcpiSsdt ||
            mAcpiXsdt->Entry[e + 1U] !=
                (debug_port_present ? (UINT64)(UINTN)mAcpiDbgp : 0)) {
            return 0;
        }

        e = 5;
        if (mAcpiRsdt->Entry[0] != (UINT32)(UINTN)mAcpiFadt ||
            mAcpiRsdt->Entry[1] != (UINT32)(UINTN)mAcpiMadt ||
            mAcpiRsdt->Entry[2] != (UINT32)(UINTN)mAcpiSrat ||
            mAcpiRsdt->Entry[3] != (UINT32)(UINTN)mAcpiSlit ||
            mAcpiRsdt->Entry[4] != (UINT32)(UINTN)mAcpiHcdp ||
            (!is_460gx &&
             mAcpiRsdt->Entry[e] != (UINT32)(UINTN)mAcpiMcfg)) {
            return 0;
        }
        if (!is_460gx) {
            e++;
        }
        if (mAcpiRsdt->Entry[e] != (UINT32)(UINTN)mAcpiSsdt ||
            mAcpiRsdt->Entry[e + 1U] !=
                (debug_port_present ? (UINT32)(UINTN)mAcpiDbgp : 0)) {
            return 0;
        }
    }

    if (mAcpiDbgp->InterfaceType != ACPI_DBGP_INTERFACE_16550_FULL ||
        mAcpiDbgp->Reserved[0] != 0 ||
        mAcpiDbgp->Reserved[1] != 0 ||
        mAcpiDbgp->Reserved[2] != 0 ||
        (debug_port_present &&
         !acpi_gas_matches(&mAcpiDbgp->BaseAddress,
                           ACPI_GAS_SYSTEM_MEMORY, 8, debug_port_base))) {
        return 0;
    }

    if (mAcpiMcfg->Reserved != 0 ||
        mAcpiMcfg->Allocation[0].BaseAddress != PCI_CONFIG_ECAM_BASE ||
        mAcpiMcfg->Allocation[0].PciSegmentGroup != 0 ||
        mAcpiMcfg->Allocation[0].StartBusNumber != 0 ||
        mAcpiMcfg->Allocation[0].EndBusNumber !=
            (UINT8)(PCI_CONFIG_ECAM_SIZE / 0x100000U - 1U) ||
        mAcpiMcfg->Allocation[0].Reserved != 0) {
        return 0;
    }

    if (mAcpiMadt->Iosapic.Type != 6 ||
        mAcpiMadt->Iosapic.Length != 16 ||
        mAcpiMadt->Iosapic.Address != IOSAPIC_BASE ||
        mAcpiSlit->Localities != 1 ||
        mAcpiSlit->Entry[0] != 10 ||
        mAcpiHcdp->EntryCount != 1 ||
        mAcpiHcdp->Uart[0].Type != 0 ||
        mAcpiHcdp->Uart[0].Bits != 8 ||
        mAcpiHcdp->Uart[0].Parity != 1 ||
        mAcpiHcdp->Uart[0].StopBits != 1 ||
        mAcpiHcdp->Uart[0].PciSegment != 0 ||
        mAcpiHcdp->Uart[0].PciBus != 0 ||
        mAcpiHcdp->Uart[0].PciDevice != 0 ||
        mAcpiHcdp->Uart[0].PciFunction != 0 ||
        mAcpiHcdp->Uart[0].Baud != 115200 ||
        !acpi_gas_matches(&mAcpiHcdp->Uart[0].BaseAddress,
                          ACPI_GAS_SYSTEM_MEMORY, 8, IA64_UART_BASE) ||
        mAcpiHcdp->Uart[0].PciDeviceId !=
            (UINT16)HCDP_UART_ACPI_HID_PNP0501 ||
        mAcpiHcdp->Uart[0].PciVendorId !=
            (UINT16)(HCDP_UART_ACPI_HID_PNP0501 >> 16) ||
        mAcpiHcdp->Uart[0].GlobalInterrupt != 4 ||
        mAcpiHcdp->Uart[0].ClockRate != HCDP_UART_PSEUDO_CLOCK_RATE ||
        mAcpiHcdp->Uart[0].PciProgrammingInterface != 0x02 ||
        mAcpiHcdp->Uart[0].Flags != hcdp_uart_flags ||
        mAcpiHcdp->Uart[0].ConOutIndex != HCDP_CONOUT_UART_INDEX ||
        mAcpiHcdp->Uart[0].Reserved != 0 ||
        mAcpiHcdp->Device[0].Type != HCDP_DEVICE_TYPE_VGA_CONSOLE ||
        mAcpiHcdp->Device[0].Flags !=
            (fw_handoff_vga_console_primary() ?
             HCDP_DEVICE_FLAG_PRIMARY_CONSOLE : 0) ||
        mAcpiHcdp->Device[0].Length != sizeof(mAcpiHcdp->Device[0]) ||
        mAcpiHcdp->Device[0].EfiIndex != HCDP_CONOUT_VGA_INDEX ||
        mAcpiHcdp->Device[0].Pci.Interconnect !=
            HCDP_PCI_INTERFACE_TYPE ||
        mAcpiHcdp->Device[0].Pci.Reserved != 0 ||
        mAcpiHcdp->Device[0].Pci.Length !=
            sizeof(mAcpiHcdp->Device[0].Pci) ||
        mAcpiHcdp->Device[0].Pci.Segment != 0 ||
        mAcpiHcdp->Device[0].Pci.Bus != fw_vga_pci_bus() ||
        mAcpiHcdp->Device[0].Pci.Device != fw_vga_pci_device() ||
        mAcpiHcdp->Device[0].Pci.Function != 0 ||
        mAcpiHcdp->Device[0].Pci.DeviceId != (UINT16)(vga_id >> 16) ||
        mAcpiHcdp->Device[0].Pci.VendorId != (UINT16)vga_id) {
        return 0;
    }
    for (i = 0; i < FW_MAX_CPUS; i++) {
        UINT32 expected_flags = i < fw_guest_processor_count() ? 1 : 0;

        if (mAcpiMadt->Lsapic[i].Type != 7 ||
            mAcpiMadt->Lsapic[i].Length != 12 ||
            mAcpiMadt->Lsapic[i].ProcessorId != i ||
            mAcpiMadt->Lsapic[i].Id != i ||
            mAcpiMadt->Lsapic[i].Eid != 0 ||
            mAcpiMadt->Lsapic[i].Flags != expected_flags ||
            mAcpiSrat->Processor[i].Length !=
                sizeof(mAcpiSrat->Processor[i]) ||
            mAcpiSrat->Processor[i].ApicId != i ||
            mAcpiSrat->Processor[i].LsapicEid != 0 ||
            mAcpiSrat->Processor[i].Flags != expected_flags) {
            return 0;
        }
    }
    for (i = 0; i < FW_MEMORY_AFFINITY_MAX; i++) {
        const ACPI_SRAT_MEMORY_AFFINITY *memory = &mAcpiSrat->Memory[i];

        if (memory->Type != 1 ||
            memory->Length != sizeof(*memory) ||
            memory->Reserved0 != 0 ||
            memory->Reserved1 != 0 ||
            memory->Reserved2 != 0) {
            return 0;
        }
        if (i == 0) {
            if (memory->Flags != 1 ||
                acpi_srat_memory_base(memory) != 0 ||
                acpi_srat_memory_length(memory) != mGuestLowRamEnd) {
                return 0;
            }
        } else if (i <= fw_guest_high_ram_count()) {
            UINT64 range_base = fw_guest_high_ram_base(i - 1U);
            UINT64 range_end = fw_guest_high_ram_end(i - 1U);

            if (memory->Flags != 1 ||
                acpi_srat_memory_base(memory) != range_base ||
                acpi_srat_memory_length(memory) != range_end - range_base) {
                return 0;
            }
        } else if (memory->Flags != 0 ||
                   acpi_srat_memory_base(memory) != 0 ||
                   acpi_srat_memory_length(memory) != 0) {
            return 0;
        }
    }

    return 1;
}

