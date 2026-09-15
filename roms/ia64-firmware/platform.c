/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The platform half of the firmware: guest state decoded from the
 * NVRAM defaults record, the SAL procedure set and dispatcher, PCI
 * config-space access, the CPU register / SAL-handoff assembly bridge,
 * and AP bring-up.  Together with efi_memmap.c and platform_tables.c
 * this is the producer side of the plan's milestone-6
 * SALEFIHANDOFF-shaped platform boundary; firmware.c retains the EFI
 * core (allocator, events, protocol database, services).
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-efi-types.h"
#include "fw-acpi.h"
#include "fw-memmap.h"
#include "fw-storage.h"
#include "fw-platform-layout.h"
#include "linker-symbols.h"
#include "fw-platform-handoff.h"

typedef struct {
    UINT64 Status;
    UINT64 Value0;
    UINT64 Value1;
    UINT64 Value2;
} SAL_RETURN_VALUE;

#define SAL_STATUS_SUCCESS          0
#define SAL_STATUS_INVALID_ARGUMENT ((UINT64)-2)
#define SAL_STATUS_ERROR            ((UINT64)-3)
#define SAL_STATUS_NO_INFORMATION   ((UINT64)-5)
#define SAL_STATUS_NOT_IMPLEMENTED  ((UINT64)-1)
#define SAL_STATUS_INSUFFICIENT_SCRATCH ((UINT64)-9)
#define SAL_SET_VECTORS             0x01000000ULL
#define SAL_GET_STATE_INFO          0x01000001ULL
#define SAL_GET_STATE_INFO_SIZE     0x01000002ULL
#define SAL_CLEAR_STATE_INFO        0x01000003ULL
#define SAL_MC_RENDEZ               0x01000004ULL
#define SAL_MC_SET_PARAMS           0x01000005ULL
#define SAL_REGISTER_PHYSICAL_ADDR  0x01000006ULL
#define SAL_CACHE_FLUSH             0x01000008ULL
#define SAL_CACHE_INIT              0x01000009ULL
#define SAL_PCI_CONFIG_READ         0x01000010ULL
#define SAL_PCI_CONFIG_WRITE        0x01000011ULL
#define SAL_FREQ_BASE               0x01000012ULL
#define SAL_PHYSICAL_ID_INFO        0x01000013ULL
#define SAL_UPDATE_PAL              0x01000020ULL
#define SAL_FREQ_BASE_PLATFORM      0
#define PLATFORM_BASE_FREQUENCY     100000000ULL
#define SAL_UPDATE_PAL_WRITE_FAILURE ((UINT64)-10)

#define SAL_VECTOR_OS_MCA           0
#define SAL_VECTOR_OS_INIT          1
#define SAL_VECTOR_OS_BOOT_RENDEZ   2
#define SAL_VECTOR_COUNT            3
#define SAL_VECTOR_LENGTH_MASK      0xffffffffULL
#define SAL_VECTOR_CHECKSUM_VALID   (1ULL << 32)
#define SAL_VECTOR_LENGTH_RESERVED_MASK \
    ((((1ULL << 7) - 1) << 33) | (0xffffULL << 48))

#define SAL_PHYSICAL_ENTITY_PAL_PROC 0

#define SAL_MC_PARAM_RENDEZ_INT     1
#define SAL_MC_PARAM_RENDEZ_WAKEUP  2
#define SAL_MC_PARAM_CPE_INT        3
#define SAL_MC_PARAM_COUNT          4

#define SAL_MC_PARAM_MECHANISM_INT  1
#define SAL_MC_PARAM_MECHANISM_MEM  2
#define SAL_MC_OPTION_MASK          0x3ULL

#define SAL_STATE_TYPE_MCA          0
#define SAL_STATE_TYPE_INIT         1
#define SAL_STATE_TYPE_CMC          2
#define SAL_STATE_TYPE_CPE          3
#define SAL_STATE_TYPE_DECONFIG     4
#define SAL_ERROR_RECORD_HEADER_SIZE 40
#define SAL_ERROR_SECTION_HEADER_SIZE 24
#define SAL_ERROR_RECORD_MIN_SIZE \
    (SAL_ERROR_RECORD_HEADER_SIZE + SAL_ERROR_SECTION_HEADER_SIZE)

typedef struct {
    UINT64 Psr;
    UINT64 Rsc;
    UINT64 Dcr;
    UINT64 Iva;
    UINT64 Pta;
    UINT64 Sp;
    UINT64 Bsp;
    UINT64 BspStore;
    UINT64 Rr[8];
    UINT64 Pkr[16];
} IA64_SAL_HANDOFF_PROBE;

#define SAL_RR_PREFERRED_PAGE_SHIFT  12U
#define SAL_RR_FIRST_RID             0x1000U
#define SAL_PTA_DISABLED_VALUE       (15ULL << 2)
#define SAL_RR_VALUE(Rid) \
    (((UINT64)(Rid) << 8) | ((UINT64)SAL_RR_PREFERRED_PAGE_SHIFT << 2))
#define SAL_BACKING_STORE_BASE       (mCpuAssistBase + IA64_FW_EARLY_RSE_OFFSET)
#define SAL_BACKING_STORE_END        (mCpuAssistBase + IA64_FW_EARLY_RSE_END_OFFSET)
#define IA64_REGION6_BASE             0xC000000000000000ULL

static IA64_SAL_HANDOFF_PROBE mSalHandoffProbe;
extern UINT64 mResetFloatingPointDisableBits;
extern UINTN mRuntimePciConfigEcam;
extern BOOLEAN mVirtualAddressMapApplied;
extern UINTN fw_sal_handoff_probe(EFI_HANDLE ImageHandle,
                                  EFI_SYSTEM_TABLE *SystemTable);
void fw_set_mem(VOID *Buffer, UINTN Size, UINT8 Value);

UINT64                        mGuestRamSize = FW_LOW_RAM_LIMIT;
/* ACPI staging base; placement decided in efi_init_memory_map (quirk). */
UINT64                        mAcpiRegionBase = FW_LOW_ACPI_ISLAND_BASE;
UINT64                        mGuestLowRamEnd = FW_LOW_RAM_LIMIT;
static UINTN                  mProcessorCount = 1;
static UINTN                  mSocketCount = 1;
static UINTN                  mCoresPerSocket = 1;
static UINTN                  mThreadsPerCore = 1;
/* What the flash stage probed: installed DRAM and the core chipset. */
static UINT64                 mChipsetProbed = IA64_FW_CHIPSET_DERIVE;
/* Application processors that have entered the shadow (firmware_ap_main). */
static volatile UINT64        mApCheckins;

extern char __fw_image_start[];
extern char _start[];

/*
 * The PS/2 controller: an i8042 answers the controller self-test (command
 * AAh) with 55h on its data port; open bus does not.  Probed once, on the
 * first question, since the console and pointer code ask repeatedly.
 */
static UINT8 mI8042Present = 2;

/*
 * The ACPI fixed-hardware block.  On the 460GX board it is the 82468GX
 * IFB's, which this firmware programs to A00h below, and the FADT names the
 * IFB's SMI command port and the RST_CNT reset register at CF9h as the
 * vendor FADT does; the SCI (ISA IRQ 9) reaches the PID on input 49, hence
 * the MADT override.  The zx1 machine keeps its stand-in block at 2000h with
 * a reset register inside it until the zx1 firmware work shows the real
 * one.
 */
UINT64 fw_acpi_pm_io_base(void)
{
    return fw_platform_is_460gx() ? IA64_460GX_ACPI_PM_IO_BASE
                                  : IA64_ACPI_PM_IO_BASE;
}

UINT64 fw_acpi_reset_port(void)
{
    return fw_platform_is_460gx()
        ? IA64_460GX_RESET_CONTROL_PORT
        : IA64_ACPI_PM_IO_BASE + IA64_ACPI_PM_RESET_OFFSET;
}

UINT8 fw_acpi_reset_value(void)
{
    return fw_platform_is_460gx() ? IA64_460GX_RESET_CONTROL_VALUE
                                  : IA64_ACPI_PM_RESET_VALUE;
}

BOOLEAN fw_acpi_sci_override(UINT32 *Gsi, UINT16 *Flags)
{
    if (!fw_platform_is_460gx()) {
        return 0;
    }
    *Gsi = IA64_460GX_SCI_GSI;
    *Flags = IA64_460GX_SCI_ISO_FLAGS;
    return 1;
}

/*
 * The vendor firmware's chipset-init pokes for the IFB's ACPI block, in its
 * order (bios130.BIN 0x2c7a80): ACPI Enable off, ACPI Base A00h, ACPI Enable
 * on.  The block's SCI_EN stays clear: the OS raises it through the SMI
 * command port, as on the real board.
 */
/*
 * Give the 460GX's expander ports their bus numbers, as POST does before it
 * scans them: each port claims configuration cycles for the bus range
 * [BUSNO, SUBNO] (SSDM 2.3.1).  The ports sit on bus CBN (programmed to EEh
 * before this runs) at the device numbers of Table 2-1: 12h and 13h are
 * the two WXBs, 14h the GXB.  Port 10h is the compatibility bus, bus 0
 * whatever its pair says.  The numbers are the ones this firmware's DSDT
 * reports for the roots; the chipset's PCIS windows stay unprogrammed so
 * the DRAM band keeps the layout the memory map describes.
 */
void fw_platform_init_expander_ports(void)
{
    static const struct {
        UINT8 Device;
        UINT8 Bus;
    } ports[] = {
        { 0x12, IA64_460GX_WXB0_BUS },
        { 0x13, IA64_460GX_WXB1_BUS },
        { 0x14, IA64_460GX_GXB_BUS },
    };
    UINTN i;

    if (!fw_platform_is_460gx()) {
        return;
    }
    for (i = 0; i < FW_ARRAY_SIZE(ports); i++) {
        pci_config_write_value(0, IA64_460GX_CBN_BUS, ports[i].Device, 0,
                               0x48, 1, ports[i].Bus);
        pci_config_write_value(0, IA64_460GX_CBN_BUS, ports[i].Device, 0,
                               0x49, 1, ports[i].Bus);
    }
}

void fw_platform_init_south_bridge(void)
{
    if (!fw_platform_is_460gx()) {
        return;
    }
    pci_config_write_value(0, 0, IA64_460GX_IFB_SLOT,
                           IA64_460GX_IFB_LPC_FUNCTION, 0x44, 1, 0);
    pci_config_write_value(0, 0, IA64_460GX_IFB_SLOT,
                           IA64_460GX_IFB_LPC_FUNCTION, 0x40, 4,
                           IA64_460GX_ACPI_PM_IO_BASE);
    pci_config_write_value(0, 0, IA64_460GX_IFB_SLOT,
                           IA64_460GX_IFB_LPC_FUNCTION, 0x44, 1, 1);
}

BOOLEAN fw_handoff_i8042_enabled(void)
{
    if (mI8042Present == 2) {
        volatile UINT8 *status = (volatile UINT8 *)(UINTN)PS2_STATUS_PORT;
        volatile UINT8 *data = (volatile UINT8 *)(UINTN)PS2_DATA_PORT;
        UINTN limit;

        mI8042Present = 0;
        for (limit = 0; limit < 100000 && (*status & PS2_STATUS_IBF); limit++) {
        }
        if ((*status & PS2_STATUS_IBF) == 0) {
            /* Drain stale output before asking, then wait for the answer. */
            for (limit = 0; limit < 16 && (*status & PS2_STATUS_OBF); limit++) {
                (void)*data;
            }
            *status = 0xaa;
            for (limit = 0; limit < 100000; limit++) {
                if (*status & PS2_STATUS_OBF) {
                    mI8042Present = *data == 0x55;
                    break;
                }
            }
        }
    }
    return mI8042Present != 0;
}

/*
 * The debug UART: a 16550 at the board's debug port (COM2 on the 460GX
 * board, the machine's memory-mapped window on zx1) keeps what is written
 * to its scratch register (offset 7); open bus keeps nothing.  Zero means
 * no debug port.
 */
UINT64 fw_handoff_debug_port_base(void)
{
    static UINT64 base = ~0ULL;

    if (base == ~0ULL) {
        UINT64 window = fw_platform_is_460gx()
            ? LEGACY_IO_BASE + IA64_460GX_COM2_IO_BASE
            : IA64_DEBUG_UART_BASE;
        volatile UINT8 *scratch = (volatile UINT8 *)(UINTN)(window + 7);

        *scratch = 0xa5;
        base = *scratch == 0xa5 ? window : 0;
        if (base != 0) {
            *scratch = 0x5a;
            base = *scratch == 0x5a ? window : 0;
        }
    }
    return base;
}

/*
 * The console UART and the debug port as ACPI describes them: the 460GX
 * board's are legacy I/O ports (System I/O GAS, the port number); the zx1
 * machine's are memory-mapped (System Memory GAS, the address).
 */
BOOLEAN fw_console_uart_io_port(UINT64 *Port)
{
    if (!fw_platform_is_460gx()) {
        return 0;
    }
    *Port = IA64_460GX_COM1_IO_BASE;
    return 1;
}

BOOLEAN fw_debug_port_io_port(UINT64 *Port)
{
    if (!fw_platform_is_460gx() || fw_handoff_debug_port_base() == 0) {
        return 0;
    }
    *Port = IA64_460GX_COM2_IO_BASE;
    return 1;
}

void fw_platform_set_probed(UINT64 RamSize, UINT64 Chipset)
{
    mGuestRamSize = RamSize & ~0xfffULL;
    mGuestLowRamEnd = mGuestRamSize > FW_LOW_RAM_LIMIT ? FW_LOW_RAM_LIMIT
                                                       : mGuestRamSize;
    mChipsetProbed = Chipset;
}


/* FW_RAM_RANGE lives in fw-acpi.h. */

static FW_RAM_RANGE           mGuestHighRam[FW_HIGH_RAM_RANGE_MAX];
static UINTN                  mGuestHighRamCount;

/* The firmware defaults record the machine seeds into the NVRAM store. */
static const IA64NvramDefaults *fw_nvram_defaults(void)
{
    const IA64NvramDefaults *defaults =
        (const IA64NvramDefaults *)(fw_nvram_image() +
                                    IA64_NVRAM_DEFAULTS_OFFSET);

    if (defaults->Magic != IA64_NVRAM_DEFAULTS_MAGIC ||
        defaults->Version == 0 ||
        defaults->Version > IA64_NVRAM_DEFAULTS_VERSION) {
        return 0;
    }
    return defaults;
}

BOOLEAN fw_handoff_vga_console_primary(void)
{
    const IA64NvramDefaults *defaults = fw_nvram_defaults();

    return defaults != 0 && defaults->ConsolePolicy == IA64_FW_CONSOLE_VGA;
}

UINT64 fw_handoff_map_quirk_disable(void)
{
    const IA64NvramDefaults *defaults = fw_nvram_defaults();

    return defaults != 0 ? (defaults->MapQuirkDisable & IA64_FW_QUIRK_ALL) : 0;
}

BOOLEAN fw_handoff_ide_dma_enabled(void)
{
    const IA64NvramDefaults *defaults = fw_nvram_defaults();

    return defaults == 0 || defaults->IdeDmaEnabled != 0;
}

UINT16 fw_handoff_boot_timeout(void)
{
    const IA64NvramDefaults *defaults = fw_nvram_defaults();

    return defaults != 0 ? (UINT16)defaults->BootTimeout
                         : IA64_FW_BOOT_TIMEOUT_WAIT_FOREVER;
}


UINT64 fw_guest_low_ram_end(void)
{
    return mGuestRamSize > FW_LOW_RAM_LIMIT ? FW_LOW_RAM_LIMIT
                                            : mGuestRamSize;
}

UINT64 fw_guest_ram_size(void)
{
    return mGuestRamSize;
}

static void fw_add_guest_high_ram_range(UINT64 Base, UINT64 Limit,
                                        UINT64 *Remaining)
{
    FW_RAM_RANGE *range;
    UINT64 size;
    UINT64 end;

    if (mGuestHighRamCount >= FW_HIGH_RAM_RANGE_MAX ||
        Remaining == NULL || *Remaining == 0 || Limit <= Base) {
        return;
    }

    size = *Remaining < Limit - Base ? *Remaining : Limit - Base;
    end = Base + size;

    range = &mGuestHighRam[mGuestHighRamCount++];
    range->Base = Base;
    range->End = end;
    *Remaining -= size;
}

void fw_init_guest_high_ram_ranges(UINT64 RamSize)
{
    UINT64 remaining;
    UINTN i;

    mGuestHighRamCount = 0;
    for (i = 0; i < FW_HIGH_RAM_RANGE_MAX; i++) {
        mGuestHighRam[i].Base = 0;
        mGuestHighRam[i].End = 0;
    }

    /*
     * Match real 460GX: low DRAM is contiguous from 0 to the PCI/MMIO aperture
     * (mGuestLowRamEnd), and anything displaced by the top-of-memory gap is
     * remapped ABOVE 4 GiB.  There is no sub-4 GiB DRAM island above the
     * aperture.
     *
     * The zx1 machine additionally carves the 1 GiB SBA "safe IOVA space" hole
     * out of the low band (fw_zx1_iova_hole_active(): zx1 with RAM past the
     * aperture), so its low band holds IA64_SBA_IOVA_SIZE fewer bytes and that
     * much more DRAM is displaced above 4 GiB.  In that regime mGuestLowRamEnd
     * is the aperture, so subtracting the hole size is exact.  (Keep this in
     * lockstep with ia64_vpc_map_ram() in hw/ia64/ia64_base.c and
     * efi_add_low_ram_band() above.)
     */
    {
        UINT64 low_band = mGuestLowRamEnd;

        if (fw_zx1_iova_hole_active()) {
            low_band -= IA64_SBA_IOVA_SIZE;
        }
        remaining = RamSize > low_band ? RamSize - low_band : 0;
    }
    fw_add_guest_high_ram_range(FW_FIRMWARE_ADDRESS_SPACE_END,
                                ~0ULL, &remaining);
}

UINTN fw_guest_processor_count(void)
{
    return mProcessorCount;
}

UINTN fw_guest_socket_count(void)
{
    return mSocketCount;
}

UINTN fw_guest_cores_per_socket(void)
{
    return mCoresPerSocket;
}

UINTN fw_guest_threads_per_core(void)
{
    return mThreadsPerCore;
}

UINTN fw_guest_high_ram_count(void)
{
    return mGuestHighRamCount;
}

UINT64 fw_guest_high_ram_base(UINTN Index)
{
    return Index < mGuestHighRamCount ? mGuestHighRam[Index].Base : 0;
}

UINT64 fw_guest_high_ram_end(UINTN Index)
{
    return Index < mGuestHighRamCount ? mGuestHighRam[Index].End : 0;
}

UINT64 fw_guest_high_ram_total(void)
{
    UINT64 total = 0;
    UINTN i;

    for (i = 0; i < mGuestHighRamCount; i++) {
        total += mGuestHighRam[i].End - mGuestHighRam[i].Base;
    }
    return total;
}

UINT64 fw_boot_stack_top(void)
{
    UINT64 low_ram_end;

    /*
     * The entry trampoline initially uses the minimum-machine stack.  Only
     * move it after validating the machine handoff, since this function is
     * itself called on that bootstrap stack.
     */
    low_ram_end = fw_guest_low_ram_end();
    if (low_ram_end < IA64_FW_LOW_RAM_MIN) {
        return IA64_FW_LOW_RAM_MIN;
    }
    return low_ram_end & ~(IA64_EFI_MEMORY_ALIGN - 1U);
}







/* IDE bus-master DMA policy; moves to the NVRAM defaults block next. */


/*
 * The PAL_PROC address PAL handed the boot processor at SALE_ENTRY (GR34),
 * stored by start_after_pal.
 */
UINT64 mFwResetPalProc;

/*
 * What the PAL emulation's firmware assists need to know about this image
 * (IA64_PAL_FIRMWARE_REGISTER, hw/ia64/ia64_vpc_abi.h): the IVT, the image
 * window a firmware context reaches identity-mapped, the SAL runtime stubs
 * and dispatch block, and the CPU-assist region.  The boot processor fills
 * it before it releases the others; each processor registers it itself.
 */
static UINT64 mFwRegistration[IA64_FW_REGISTRATION_SIZE / 8]
    __attribute__((aligned(16)));

UINT64 fw_pal_call_at(UINT64 Entry, UINT64 Index, UINT64 Arg1, UINT64 Arg2,
                      UINT64 Arg3, UINT64 *Results);
UINT64 fw_pal_stacked_call_at(UINT64 Entry, UINT64 Index, UINT64 Arg1,
                              UINT64 Arg2, UINT64 Arg3, UINT64 *Results);

#define FW_PAL_COPY_INFO 0x01e
#define FW_PAL_COPY_PAL  0x100

/*
 * PAL_PROC in RAM: PAL's copy of itself in the image's first page, what
 * every PAL call of this firmware and the SAL system table use.
 */
UINT64 mFwPalProc;

/*
 * SAL 3.2.3 step 9: copy PAL into RAM.  The boot processor asks PAL for the
 * buffer it needs (PAL_COPY_INFO) and has it copied (PAL_COPY_PAL, processor
 * 0); every other processor makes the same call with processor 1, which
 * installs the copy's entry in that processor without copying again.  If
 * PAL refuses, the firmware keeps calling the PAL_PROC it got at reset.
 */
BOOLEAN fw_platform_install_pal(UINT64 Processor, UINT64 ResetPalProc)
{
    UINT64 results[3];
    UINT64 buffer = (UINTN)fw_pal_buffer;

    if (ResetPalProc == 0) {
        return 0;
    }
    if (Processor == 0) {
        mFwPalProc = ResetPalProc;
        if (fw_pal_call_at(ResetPalProc, FW_PAL_COPY_INFO, 0, 0, 0,
                           results) != 0 ||
            results[0] > IA64_FW_PAL_BUFFER_SIZE || results[1] == 0 ||
            (buffer & (results[1] - 1)) != 0) {
            return 0;
        }
    }
    if (fw_pal_stacked_call_at(ResetPalProc, FW_PAL_COPY_PAL, buffer,
                               IA64_FW_PAL_BUFFER_SIZE, Processor,
                               results) != 0 ||
        results[0] >= IA64_FW_PAL_BUFFER_SIZE) {
        return 0;
    }
    if (Processor == 0) {
        mFwPalProc = buffer + results[0];
    }
    return 1;
}

BOOLEAN fw_platform_register_processor(UINT64 ResetPalProc)
{
    return ResetPalProc != 0 &&
           fw_pal_call_at(ResetPalProc, IA64_PAL_FIRMWARE_REGISTER,
                          (UINTN)mFwRegistration, sizeof(mFwRegistration),
                          0, NULL) == 0;
}

BOOLEAN fw_platform_register_firmware(UINT64 CpuAssistBase)
{
    mFwRegistration[IA64_FW_REGISTRATION_MAGIC_OFF / 8] =
        IA64_FW_REGISTRATION_MAGIC;
    mFwRegistration[IA64_FW_REGISTRATION_IMAGE_BASE_OFF / 8] =
        (UINTN)__fw_image_start;
    mFwRegistration[IA64_FW_REGISTRATION_IMAGE_SIZE_OFF / 8] =
        IA64_FW_IDENTITY_WINDOW_SIZE;
    mFwRegistration[IA64_FW_REGISTRATION_IVT_OFF / 8] = (UINTN)__fw_ivt;
    mFwRegistration[IA64_FW_REGISTRATION_SAL_ENTRY_OFF / 8] =
        (UINTN)sal_runtime_entry;
    mFwRegistration[IA64_FW_REGISTRATION_SAL_RETURN_OFF / 8] =
        (UINTN)sal_runtime_return;
    mFwRegistration[IA64_FW_REGISTRATION_SAL_BLOCK_OFF / 8] =
        (UINTN)sal_dispatch_block;
    mFwRegistration[IA64_FW_REGISTRATION_ASSIST_OFF / 8] = CpuAssistBase;
    return fw_platform_register_processor(mFwResetPalProc);
}

/*
 * Release the application processors, which the flash stage parks until
 * the shadow's data is ready, and count them as they check in.  Real SAL
 * rendezvouses its processors the same way (SAL 3.2.3); the wait is
 * bounded, so a machine with one processor moves on after 20 ms.
 */
static void fw_platform_rendezvous_processors(void)
{
    volatile UINT64 *mailbox = (volatile UINT64 *)(UINTN)IA64_FW_SHADOW_MAILBOX;
    UINT64 deadline;
    UINT64 seen;

    mailbox[1] = mGuestRamSize;
    __asm__ volatile ("mf;;" : : : "memory");
    /* The shadow's reset entry; its first page is PAL's buffer. */
    mailbox[0] = (UINT64)(UINTN)_start;
    __asm__ volatile ("mf;;" : : : "memory");

    deadline = fw_read_itc() + 200000ULL * fw_itc_ticks_per_100ns;
    do {
        seen = mApCheckins;
        while (fw_read_itc() < deadline && mApCheckins == seen) {
            __asm__ volatile ("hint @pause" : : : "memory");
        }
    } while (mApCheckins != seen);

    mProcessorCount = 1 + (UINTN)mApCheckins;
    if (mProcessorCount > FW_MAX_CPUS) {
        mProcessorCount = FW_MAX_CPUS;
    }
}

/*
 * The package geometry from PAL_LOGICAL_TO_PHYSICAL (SDM vol. 2 11.10.3):
 * threads per core in bits 16-31 and cores per package in bits 32-47 of
 * its first return.  Processors that do not implement it (Itanium and
 * Itanium 2) have one core and one thread per package.
 */
static void fw_platform_decode_package_topology(void)
{
    UINT64 status, info;

    mSocketCount = mProcessorCount;
    mCoresPerSocket = 1;
    mThreadsPerCore = 1;

    status = fw_pal_logical_to_physical(&info);
    if (status == 0) {
        UINTN threads = (info >> 16) & 0xffff;
        UINTN cores = (info >> 32) & 0xffff;

        if (threads != 0 && cores != 0 && threads * cores <= FW_MAX_CPUS) {
            mThreadsPerCore = threads;
            mCoresPerSocket = cores;
            mSocketCount = (mProcessorCount + threads * cores - 1) /
                           (threads * cores);
        }
    }
    if (mSocketCount == 0) {
        mSocketCount = 1;
    }
}


UINT64 fw_ap_stack_top(UINT64 ProcessorId)
{
    if (ProcessorId == 0 || ProcessorId >= FW_MAX_CPUS) {
        return fw_boot_stack_top();
    }
    return fw_boot_stack_top() - ProcessorId * FW_AP_STACK_SIZE;
}

UINT64 fw_system_table_pointer_base(UINT64 LowRamEnd,
                                           UINT64 BootStackBase,
                                           UINT64 BootStackTop)
{
    UINT64 base;

    if (LowRamEnd <= FW_LOW_IMAGE_END + FW_SYSTEM_TABLE_POINTER_SIZE) {
        return 0;
    }

    base = (LowRamEnd - 1U) & ~(FW_SYSTEM_TABLE_POINTER_ALIGN - 1U);
    if (base < BootStackTop &&
        base + FW_SYSTEM_TABLE_POINTER_SIZE > BootStackBase) {
        base = (BootStackBase - FW_SYSTEM_TABLE_POINTER_SIZE) &
               ~(FW_SYSTEM_TABLE_POINTER_ALIGN - 1U);
    }
    if (base < mCpuAssistBase + IA64_FW_CPU_ASSIST_SIZE &&
        base + FW_SYSTEM_TABLE_POINTER_SIZE > mCpuAssistBase) {
        base = (mCpuAssistBase - FW_SYSTEM_TABLE_POINTER_SIZE) &
               ~(FW_SYSTEM_TABLE_POINTER_ALIGN - 1U);
    }
    /* When ACPI staging sits below the CPU-assist region, step below it. */
    if (!fw_map_quirk_enabled(IA64_FW_QUIRK_ACPI_LOW_ISLAND) &&
        base < ACPI_RECLAIM_END &&
        base + FW_SYSTEM_TABLE_POINTER_SIZE > ACPI_RECLAIM_BASE) {
        base = (ACPI_RECLAIM_BASE - FW_SYSTEM_TABLE_POINTER_SIZE) &
               ~(FW_SYSTEM_TABLE_POINTER_ALIGN - 1U);
    }
    /* And below the RAM-top image shadow (its span is machine-reserved). */
    if (base < (UINT64)(UINTN)__fw_image_start + IA64_FW_IMAGE_SPAN &&
        base + FW_SYSTEM_TABLE_POINTER_SIZE > (UINT64)(UINTN)__fw_image_start) {
        base = ((UINT64)(UINTN)__fw_image_start -
                FW_SYSTEM_TABLE_POINTER_SIZE) &
               ~(FW_SYSTEM_TABLE_POINTER_ALIGN - 1U);
    }
    if (base <= FW_LOW_IMAGE_END ||
        base + FW_SYSTEM_TABLE_POINTER_SIZE > LowRamEnd) {
        return 0;
    }
    return base;
}

/*
 * SAL revision advertised in the SST, chosen from the processor this machine
 * is impersonating.  See the SAL_REVISION_* definitions for the rationale and
 * the hardware cross-check.  It also selects the procedure set: a call that
 * post-dates the advertised revision must not be offered.
 */
/*
 * The core-chipset personality the machine selected via -machine chipset=,
 * or IA64_FW_CHIPSET_DERIVE for an old handoff (or the default) that leaves
 * the choice to the CPU family.
 */
static UINT64 fw_platform_chipset_profile(void)
{
    return mChipsetProbed;
}

/*
 * Platform personality (rework phase 3): Merced machines model the 460GX
 * (i2000/SDV) and everything else the E8870 (SR870BH2).  The -machine
 * chipset= option (handoff version 14+) overrides this CPU-family default;
 * chipset=zx1 selects the HP zx1 (rx2600/zx2000/zx6000) profile.
 */
BOOLEAN fw_platform_is_zx1(void)
{
    return fw_platform_chipset_profile() == IA64_FW_CHIPSET_ZX1;
}

BOOLEAN fw_platform_is_460gx(void)
{
    UINT64 profile = fw_platform_chipset_profile();
    UINT64 family;

    if (profile == IA64_FW_CHIPSET_460GX) {
        return 1;
    }
    if (profile == IA64_FW_CHIPSET_ZX1) {
        return 0;
    }

    family = (fw_read_cpuid3() >> IA64_CPUID3_FAMILY_SHIFT) &
             IA64_CPUID3_FAMILY_MASK;
    return family == IA64_CPUID3_FAMILY_MERCED;
}

/*
 * True when the SBA "safe IOVA space" DRAM hole is carved for this boot: the
 * zx1 machine, with installed RAM past the PCI aperture so there is already
 * displaced above-4-GiB RAM and the low band fills to the aperture regardless.
 * Only in that regime does the hole leave mGuestLowRamEnd and the firmware's
 * aperture-relative self-placement untouched, keeping the QEMU RAM map and the
 * firmware EFI/high-RAM ranges trivially consistent.  A guest at or below the
 * aperture uses the contiguous 460gx-identical layout (no hole).  Keep the
 * predicate identical to the `remaining > IA64_LOW_RAM_LIMIT` gate in
 * ia64_vpc_map_ram().
 */
BOOLEAN fw_zx1_iova_hole_active(void)
{
    return fw_platform_is_zx1() && fw_guest_ram_size() > FW_LOW_RAM_LIMIT;
}

UINT16 fw_sal_revision(void)
{
    UINT64 family = (fw_read_cpuid3() >> IA64_CPUID3_FAMILY_SHIFT) &
                    IA64_CPUID3_FAMILY_MASK;

    return family == IA64_CPUID3_FAMILY_MERCED ? SAL_REVISION_3_0 :
                                                 SAL_REVISION_3_2;
}

static SAL_RETURN_VALUE sal_proc_entry(UINT64 Index, UINT64 Arg1, UINT64 Arg2,
                                        UINT64 Arg3, UINT64 Arg4, UINT64 Arg5,
                                        UINT64 Arg6, UINT64 Arg7);

UINT64 fw_sal_proc_function_entry(void)
{
    return fw_function_entry((UINTN)sal_proc_entry);
}

typedef struct {
    UINT64 HandlerAddr1;
    UINT64 Gp1;
    UINT64 HandlerLen1;
    UINT64 HandlerAddr2;
    UINT64 Gp2;
    UINT64 HandlerLen2;
    BOOLEAN Valid;
} SAL_VECTOR_REGISTRATION;

typedef struct {
    UINT64 Mechanism;
    UINT64 Value;
    UINT64 Timeout;
    UINT64 Options;
    BOOLEAN Valid;
} SAL_MC_PARAM_REGISTRATION;

static SAL_VECTOR_REGISTRATION mSalVectors[SAL_VECTOR_COUNT];
static SAL_MC_PARAM_REGISTRATION mSalMcParams[SAL_MC_PARAM_COUNT];
static UINT64 mSalPalProcPhysicalAddress __attribute__((used));

static SAL_RETURN_VALUE sal_return(UINT64 Status, UINT64 Value0,
                                   UINT64 Value1, UINT64 Value2)
{
    SAL_RETURN_VALUE Ret;

    Ret.Status = Status;
    Ret.Value0 = Value0;
    Ret.Value1 = Value1;
    Ret.Value2 = Value2;
    return Ret;
}

static BOOLEAN sal_vector_length_cs_valid(UINT64 LengthCs)
{
    if ((LengthCs & SAL_VECTOR_CHECKSUM_VALID) == 0) {
        return 1;
    }

    if ((LengthCs & SAL_VECTOR_LENGTH_RESERVED_MASK) != 0) {
        return 0;
    }

    return (LengthCs & SAL_VECTOR_LENGTH_MASK) != 0 &&
           (LengthCs & 0xfU) == 0;
}

static BOOLEAN sal_vector_entry_valid(UINT64 Address, UINT64 LengthCs)
{
    if ((Address & 0xfU) != 0) {
        return 0;
    }

    return sal_vector_length_cs_valid(LengthCs);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_set_vectors(UINT64 VectorType, UINT64 PhysAddr1, UINT64 Gp1,
                UINT64 LengthCs1, UINT64 PhysAddr2, UINT64 Gp2,
                UINT64 LengthCs2)
{
    SAL_VECTOR_REGISTRATION *entry;

    if (VectorType >= SAL_VECTOR_COUNT ||
        !sal_vector_entry_valid(PhysAddr1, LengthCs1) ||
        (VectorType == SAL_VECTOR_OS_INIT &&
         ((PhysAddr1 == 0) != (PhysAddr2 == 0) ||
          !sal_vector_entry_valid(PhysAddr2, LengthCs2))) ||
        (VectorType != SAL_VECTOR_OS_INIT &&
         (PhysAddr2 != 0 || Gp2 != 0 || LengthCs2 != 0))) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    entry = &mSalVectors[VectorType];
    entry->HandlerAddr1 = PhysAddr1;
    entry->Gp1 = Gp1;
    entry->HandlerLen1 = LengthCs1;
    entry->HandlerAddr2 = PhysAddr2;
    entry->Gp2 = Gp2;
    entry->HandlerLen2 = LengthCs2;
    entry->Valid = 1;
    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_set_vectors_selftest(void)
{
    SAL_VECTOR_REGISTRATION saved[SAL_VECTOR_COUNT];
    SAL_RETURN_VALUE mca_valid;
    SAL_RETURN_VALUE bad_secondary;
    SAL_RETURN_VALUE bad_type;
    SAL_RETURN_VALUE init_mismatch;
    SAL_RETURN_VALUE init_checksum_valid;
    SAL_RETURN_VALUE bad_checksum_reserved;
    SAL_RETURN_VALUE bad_checksum_length;
    UINT64 length_cs = 0x20U | SAL_VECTOR_CHECKSUM_VALID | (0x80ULL << 40);
    UINTN i;
    BOOLEAN ok;

    for (i = 0; i < SAL_VECTOR_COUNT; i++) {
        saved[i] = mSalVectors[i];
    }

    mca_valid = sal_set_vectors(SAL_VECTOR_OS_MCA, 0x2000, 0x1000, 0,
                                0, 0, 0);
    bad_secondary = sal_set_vectors(SAL_VECTOR_OS_MCA, 0x2000, 0x1000, 0,
                                    0x3000, 0, 0);
    bad_type = sal_set_vectors(3, 0, 0, 0, 0, 0, 0);
    init_mismatch = sal_set_vectors(SAL_VECTOR_OS_INIT, 0, 0, 0,
                                    0x3000, 0, 0);
    init_checksum_valid = sal_set_vectors(SAL_VECTOR_OS_INIT, 0x2000, 0x1000,
                                          length_cs, 0x3000, 0x1000,
                                          length_cs);
    bad_checksum_reserved =
        sal_set_vectors(SAL_VECTOR_OS_BOOT_RENDEZ, 0x2000, 0x1000,
                        length_cs | (1ULL << 33), 0, 0, 0);
    bad_checksum_length =
        sal_set_vectors(SAL_VECTOR_OS_BOOT_RENDEZ, 0x2000, 0x1000,
                        SAL_VECTOR_CHECKSUM_VALID | 0x18U, 0, 0, 0);

    ok = mca_valid.Status == SAL_STATUS_SUCCESS &&
         bad_secondary.Status == SAL_STATUS_INVALID_ARGUMENT &&
         bad_type.Status == SAL_STATUS_INVALID_ARGUMENT &&
         init_mismatch.Status == SAL_STATUS_INVALID_ARGUMENT &&
         init_checksum_valid.Status == SAL_STATUS_SUCCESS &&
         bad_checksum_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
         bad_checksum_length.Status == SAL_STATUS_INVALID_ARGUMENT;

    for (i = 0; i < SAL_VECTOR_COUNT; i++) {
        mSalVectors[i] = saved[i];
    }

    return ok;
}

static BOOLEAN sal_state_type_valid(UINT64 Type)
{
    return Type <= SAL_STATE_TYPE_DECONFIG;
}

static BOOLEAN sal_reserved_args_are_zero(UINT64 Arg1, UINT64 Arg2,
                                          UINT64 Arg3, UINT64 Arg4,
                                          UINT64 Arg5, UINT64 Arg6)
{
    return Arg1 == 0 && Arg2 == 0 && Arg3 == 0 &&
           Arg4 == 0 && Arg5 == 0 && Arg6 == 0;
}

static BOOLEAN sal_interrupt_vector_valid(UINT64 Vector, BOOLEAN AllowZero)
{
    return (AllowZero && Vector == 0) || (Vector >= 0x10 && Vector <= 0xff);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_get_state_info_size(UINT64 Type, UINT64 Reserved1, UINT64 Reserved2,
                        UINT64 Reserved3, UINT64 Reserved4, UINT64 Reserved5,
                        UINT64 Reserved6)
{
    if (!sal_state_type_valid(Type) ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, Reserved6)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    /*
     * Advertise room for the generic record header and one section header.
     * This also accommodates consumers that initialize first-section
     * metadata before requesting a record when none is pending.
     */
    return sal_return(SAL_STATUS_SUCCESS, SAL_ERROR_RECORD_MIN_SIZE,
                      0, 0);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_get_state_info(UINT64 Type, UINT64 Reserved1, UINT64 MemAddr,
                   UINT64 Reserved2, UINT64 Reserved3, UINT64 Reserved4,
                   UINT64 Reserved5)
{
    (void)MemAddr;

    if (!sal_state_type_valid(Type) ||
        Reserved1 != 0 ||
        !sal_reserved_args_are_zero(Reserved2, Reserved3, Reserved4,
                                    Reserved5, 0, 0)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    return sal_return(SAL_STATUS_NO_INFORMATION, 0, 0, 0);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_clear_state_info(UINT64 Type, UINT64 Reserved1, UINT64 Reserved2,
                     UINT64 Reserved3, UINT64 Reserved4, UINT64 Reserved5,
                     UINT64 Reserved6)
{
    if (!sal_state_type_valid(Type) ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, Reserved6)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_state_info_selftest(void)
{
    SAL_RETURN_VALUE size_valid;
    SAL_RETURN_VALUE size_bad_reserved;
    SAL_RETURN_VALUE info_empty;
    SAL_RETURN_VALUE info_bad_type;
    SAL_RETURN_VALUE clear_valid;
    SAL_RETURN_VALUE clear_bad_reserved;

    size_valid = sal_get_state_info_size(SAL_STATE_TYPE_MCA,
                                         0, 0, 0, 0, 0, 0);
    size_bad_reserved = sal_get_state_info_size(SAL_STATE_TYPE_MCA,
                                                0, 0, 0, 0, 1, 0);
    info_empty = sal_get_state_info(SAL_STATE_TYPE_CPE,
                                    0, 0x2000, 0, 0, 0, 0);
    info_bad_type = sal_get_state_info(5, 0, 0x2000, 0, 0, 0, 0);
    clear_valid = sal_clear_state_info(SAL_STATE_TYPE_INIT,
                                       0, 0, 0, 0, 0, 0);
    clear_bad_reserved = sal_clear_state_info(SAL_STATE_TYPE_INIT,
                                              0, 0, 0, 0, 0, 1);

    return size_valid.Status == SAL_STATUS_SUCCESS &&
           size_valid.Value0 == SAL_ERROR_RECORD_MIN_SIZE &&
           size_bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
           info_empty.Status == SAL_STATUS_NO_INFORMATION &&
           info_empty.Value0 == 0 &&
           info_bad_type.Status == SAL_STATUS_INVALID_ARGUMENT &&
           clear_valid.Status == SAL_STATUS_SUCCESS &&
           clear_bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT;
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_cache_flush(UINT64 IorD, UINT64 Reserved1, UINT64 Reserved2,
                UINT64 Reserved3, UINT64 Reserved4, UINT64 Reserved5,
                UINT64 Reserved6)
{
    if (IorD < 1 || IorD > 4 ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, Reserved6)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_cache_init(UINT64 Reserved1, UINT64 Reserved2, UINT64 Reserved3,
               UINT64 Reserved4, UINT64 Reserved5, UINT64 Reserved6,
               UINT64 Reserved7)
{
    if (Reserved7 != 0 ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, Reserved6)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_cache_services_selftest(void)
{
    SAL_RETURN_VALUE flush_valid;
    SAL_RETURN_VALUE flush_bad_type;
    SAL_RETURN_VALUE flush_bad_reserved;
    SAL_RETURN_VALUE init_valid;
    SAL_RETURN_VALUE init_bad_reserved;

    flush_valid = sal_cache_flush(4, 0, 0, 0, 0, 0, 0);
    flush_bad_type = sal_cache_flush(5, 0, 0, 0, 0, 0, 0);
    flush_bad_reserved = sal_cache_flush(1, 0, 1, 0, 0, 0, 0);
    init_valid = sal_cache_init(0, 0, 0, 0, 0, 0, 0);
    init_bad_reserved = sal_cache_init(0, 0, 0, 0, 0, 0, 1);

    return flush_valid.Status == SAL_STATUS_SUCCESS &&
           flush_bad_type.Status == SAL_STATUS_INVALID_ARGUMENT &&
           flush_bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
           init_valid.Status == SAL_STATUS_SUCCESS &&
           init_bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT;
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_mc_rendez(UINT64 Reserved1, UINT64 Reserved2, UINT64 Reserved3,
              UINT64 Reserved4, UINT64 Reserved5, UINT64 Reserved6,
              UINT64 Reserved7)
{
    if (Reserved1 != 0 || Reserved2 != 0 || Reserved3 != 0 ||
        Reserved4 != 0 || Reserved5 != 0 || Reserved6 != 0 ||
        Reserved7 != 0) {
        return sal_return(SAL_STATUS_ERROR, 0, 0, 0);
    }

    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_mc_rendez_selftest(void)
{
    SAL_RETURN_VALUE valid;
    SAL_RETURN_VALUE invalid;

    valid = sal_mc_rendez(0, 0, 0, 0, 0, 0, 0);
    if (valid.Status != SAL_STATUS_SUCCESS ||
        valid.Value0 != 0 || valid.Value1 != 0 || valid.Value2 != 0) {
        return 0;
    }

    invalid = sal_mc_rendez(1, 0, 0, 0, 0, 0, 0);
    return invalid.Status == SAL_STATUS_ERROR &&
           invalid.Value0 == 0 && invalid.Value1 == 0 && invalid.Value2 == 0;
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_mc_set_params(UINT64 ParamType, UINT64 IorM, UINT64 IorMVal,
                  UINT64 Timeout, UINT64 McaOpt, UINT64 Reserved1,
                  UINT64 Reserved2)
{
    SAL_MC_PARAM_REGISTRATION *entry;

    if (Reserved1 != 0 || Reserved2 != 0 ||
        ParamType < SAL_MC_PARAM_RENDEZ_INT ||
        ParamType > SAL_MC_PARAM_CPE_INT) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    if (ParamType == SAL_MC_PARAM_RENDEZ_INT) {
        if (IorM != SAL_MC_PARAM_MECHANISM_INT ||
            !sal_interrupt_vector_valid(IorMVal, 1) ||
            (McaOpt & ~SAL_MC_OPTION_MASK) != 0) {
            return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
        }
    } else if (ParamType == SAL_MC_PARAM_RENDEZ_WAKEUP) {
        if (McaOpt != 0 ||
            (IorM == SAL_MC_PARAM_MECHANISM_INT &&
             !sal_interrupt_vector_valid(IorMVal, 1)) ||
            (IorM == SAL_MC_PARAM_MECHANISM_MEM && (IorMVal & 0x7U) != 0) ||
            (IorM != SAL_MC_PARAM_MECHANISM_INT &&
             IorM != SAL_MC_PARAM_MECHANISM_MEM)) {
            return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
        }
    } else {
        if (IorM != SAL_MC_PARAM_MECHANISM_INT ||
            !sal_interrupt_vector_valid(IorMVal, 1) ||
            McaOpt != 0) {
            return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
        }
    }

    entry = &mSalMcParams[ParamType];
    entry->Mechanism = IorM;
    entry->Value = IorMVal;
    entry->Timeout = Timeout;
    entry->Options = McaOpt;
    entry->Valid = 1;
    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_mc_set_params_selftest(void)
{
    SAL_MC_PARAM_REGISTRATION saved[SAL_MC_PARAM_COUNT];
    SAL_RETURN_VALUE rendez;
    SAL_RETURN_VALUE wake_mem;
    SAL_RETURN_VALUE cpe_deregister;
    SAL_RETURN_VALUE bad_reserved;
    SAL_RETURN_VALUE bad_vector;
    SAL_RETURN_VALUE bad_mem_align;
    SAL_RETURN_VALUE bad_options;
    BOOLEAN ok;
    UINTN i;

    for (i = 0; i < SAL_MC_PARAM_COUNT; i++) {
        saved[i] = mSalMcParams[i];
    }

    rendez = sal_mc_set_params(SAL_MC_PARAM_RENDEZ_INT,
                               SAL_MC_PARAM_MECHANISM_INT, 0xf0, 250,
                               SAL_MC_OPTION_MASK, 0, 0);
    wake_mem = sal_mc_set_params(SAL_MC_PARAM_RENDEZ_WAKEUP,
                                 SAL_MC_PARAM_MECHANISM_MEM, 0x2000, 0,
                                 0, 0, 0);
    cpe_deregister = sal_mc_set_params(SAL_MC_PARAM_CPE_INT,
                                       SAL_MC_PARAM_MECHANISM_INT, 0, 0,
                                       0, 0, 0);
    bad_reserved = sal_mc_set_params(SAL_MC_PARAM_RENDEZ_INT,
                                     SAL_MC_PARAM_MECHANISM_INT, 0x20, 0,
                                     0, 1, 0);
    bad_vector = sal_mc_set_params(SAL_MC_PARAM_CPE_INT,
                                   SAL_MC_PARAM_MECHANISM_INT, 0xf, 0,
                                   0, 0, 0);
    bad_mem_align = sal_mc_set_params(SAL_MC_PARAM_RENDEZ_WAKEUP,
                                      SAL_MC_PARAM_MECHANISM_MEM, 0x2004, 0,
                                      0, 0, 0);
    bad_options = sal_mc_set_params(SAL_MC_PARAM_RENDEZ_INT,
                                    SAL_MC_PARAM_MECHANISM_INT, 0x20, 0,
                                    1ULL << 2, 0, 0);

    ok = rendez.Status == SAL_STATUS_SUCCESS &&
         wake_mem.Status == SAL_STATUS_SUCCESS &&
         cpe_deregister.Status == SAL_STATUS_SUCCESS &&
         bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
         bad_vector.Status == SAL_STATUS_INVALID_ARGUMENT &&
         bad_mem_align.Status == SAL_STATUS_INVALID_ARGUMENT &&
         bad_options.Status == SAL_STATUS_INVALID_ARGUMENT;

    for (i = 0; i < SAL_MC_PARAM_COUNT; i++) {
        mSalMcParams[i] = saved[i];
    }

    return ok;
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_freq_base(UINT64 ClockType, UINT64 Reserved1, UINT64 Reserved2,
              UINT64 Reserved3, UINT64 Reserved4, UINT64 Reserved5,
              UINT64 Reserved6)
{
    if (ClockType > 2 ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, Reserved6)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, (UINT64)-1,
                          (UINT64)-1, 0);
    }

    if (ClockType == SAL_FREQ_BASE_PLATFORM) {
        return sal_return(SAL_STATUS_SUCCESS, PLATFORM_BASE_FREQUENCY,
                          (UINT64)-1, 0);
    }

    return sal_return(SAL_STATUS_SUCCESS, (UINT64)-1, (UINT64)-1, 0);
}

BOOLEAN __attribute__((noinline)) sal_freq_base_selftest(void)
{
    SAL_RETURN_VALUE platform;
    SAL_RETURN_VALUE optional;
    SAL_RETURN_VALUE invalid_type;
    SAL_RETURN_VALUE invalid_reserved;

    platform = sal_freq_base(0, 0, 0, 0, 0, 0, 0);
    optional = sal_freq_base(1, 0, 0, 0, 0, 0, 0);
    invalid_type = sal_freq_base(3, 0, 0, 0, 0, 0, 0);
    invalid_reserved = sal_freq_base(0, 0, 0, 1, 0, 0, 0);

    return platform.Status == SAL_STATUS_SUCCESS &&
           platform.Value0 == PLATFORM_BASE_FREQUENCY &&
           platform.Value1 == (UINT64)-1 &&
           optional.Status == SAL_STATUS_SUCCESS &&
           optional.Value0 == (UINT64)-1 && optional.Value1 == (UINT64)-1 &&
           invalid_type.Status == SAL_STATUS_INVALID_ARGUMENT &&
           invalid_reserved.Status == SAL_STATUS_INVALID_ARGUMENT;
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_physical_id_info(UINT64 Reserved1, UINT64 Reserved2, UINT64 Reserved3,
                     UINT64 Reserved4, UINT64 Reserved5, UINT64 Reserved6,
                     UINT64 Reserved7)
{
    if (Reserved7 != 0 ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, Reserved6)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_register_physical_addr(UINT64 Entity, UINT64 Address, UINT64 Reserved1,
                           UINT64 Reserved2, UINT64 Reserved3,
                           UINT64 Reserved4, UINT64 Reserved5)
{
    if (Entity != SAL_PHYSICAL_ENTITY_PAL_PROC ||
        !sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, Reserved5, 0)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }
    mSalPalProcPhysicalAddress = Address;
    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_physical_services_selftest(void)
{
    UINT64 saved = mSalPalProcPhysicalAddress;
    SAL_RETURN_VALUE id;
    SAL_RETURN_VALUE id_bad_reserved;
    SAL_RETURN_VALUE reg;
    SAL_RETURN_VALUE reg_bad_entity;
    SAL_RETURN_VALUE reg_bad_reserved;
    BOOLEAN ok;

    id = sal_physical_id_info(0, 0, 0, 0, 0, 0, 0);
    id_bad_reserved = sal_physical_id_info(0, 0, 0, 0, 0, 0, 1);
    reg = sal_register_physical_addr(SAL_PHYSICAL_ENTITY_PAL_PROC,
                                     0x2000, 0, 0, 0, 0, 0);
    reg_bad_entity = sal_register_physical_addr(1, 0x2000, 0, 0, 0, 0, 0);
    reg_bad_reserved =
        sal_register_physical_addr(SAL_PHYSICAL_ENTITY_PAL_PROC,
                                   0x2000, 0, 0, 1, 0, 0);

    ok = id.Status == SAL_STATUS_SUCCESS && id.Value0 == 0 &&
         id_bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
         reg.Status == SAL_STATUS_SUCCESS &&
         reg_bad_entity.Status == SAL_STATUS_INVALID_ARGUMENT &&
         reg_bad_reserved.Status == SAL_STATUS_INVALID_ARGUMENT;

    mSalPalProcPhysicalAddress = saved;
    return ok;
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_update_pal(UINT64 ParamBuf, UINT64 ScratchBuf, UINT64 ScratchBufSize,
               UINT64 Reserved1, UINT64 Reserved2, UINT64 Reserved3,
               UINT64 Reserved4)
{
    if (ParamBuf == 0 || (ParamBuf & 0xfU) != 0 ||
        (ScratchBuf == 0 && ScratchBufSize != 0) ||
        Reserved1 != 0 || Reserved2 != 0 ||
        Reserved3 != 0 || Reserved4 != 0) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    /*
     * The VM firmware image is immutable: report the architectural storage
     * write failure instead of advertising the procedure as absent.
     */
    return sal_return(SAL_STATUS_ERROR, SAL_UPDATE_PAL_WRITE_FAILURE, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_update_pal_selftest(void)
{
    SAL_RETURN_VALUE invalid;
    SAL_RETURN_VALUE readonly;

    invalid = sal_update_pal(0x2001, 0, 0, 0, 0, 0, 0);
    if (invalid.Status != SAL_STATUS_INVALID_ARGUMENT) {
        return 0;
    }

    readonly = sal_update_pal(0x2000, 0, 0, 0, 0, 0, 0);
    return readonly.Status == SAL_STATUS_ERROR &&
           readonly.Value0 == SAL_UPDATE_PAL_WRITE_FAILURE &&
           readonly.Value1 == 0 && readonly.Value2 == 0;
}

/*
 * The configuration mechanism follows the chipset.  The 460GX has only the
 * CF8/CFC pair in legacy I/O space (SSDM 2.3.1), so its window is the I/O
 * port space; the zx1 machine keeps the segment-0 ECAM window until its
 * firmware work settles the mechanism.  Either window is described RUNTIME
 * and reached through the SetVirtualAddressMap-converted pointer
 * (mRuntimePciConfigEcam) once the OS has switched to virtual mode.
 */
BOOLEAN fw_pci_config_by_ports(void)
{
    return fw_platform_is_460gx();
}

UINT64 fw_pci_config_window_base(void)
{
    return fw_pci_config_by_ports() ? LEGACY_IO_BASE : PCI_CONFIG_ECAM_BASE;
}

static UINT64 pci_config_cpu_base_for_mode(BOOLEAN Translated)
{
    if (!Translated) {
        return fw_pci_config_window_base();
    }
    if (mVirtualAddressMapApplied) {
        return mRuntimePciConfigEcam;
    }
    return IA64_REGION6_BASE | fw_pci_config_window_base();
}

static UINT64 pci_config_cpu_base(void)
{
    return pci_config_cpu_base_for_mode(fw_data_translation_enabled());
}

static UINT64 pci_config_all_ones(UINTN Size)
{
    if (Size >= 8) {
        return ~(UINT64)0;
    }
    return (1ULL << (Size * 8U)) - 1U;
}

static UINT64 pci_config_ecam_addr_from_base(UINT64 Base, UINT64 Segment,
                                             UINT64 Bus, UINT64 Device,
                                             UINT64 Function, UINT64 Offset)
{
    if (Segment != 0 || Bus > 0xff || Device > 0x1f || Function > 7 ||
        Offset >= 0x1000) {
        return 0;
    }

    /* EFI virtual mappings are page-aligned, not ECAM-aperture-aligned. */
    return Base + (Bus << 20) + (Device << 15) +
           (Function << 12) + Offset;
}

static UINT64 __attribute__((noinline))
pci_config_ecam_addr(UINT64 Segment, UINT64 Bus, UINT64 Device,
                     UINT64 Function, UINT64 Offset)
{
    return pci_config_ecam_addr_from_base(pci_config_cpu_base(), Segment, Bus,
                                          Device, Function, Offset);
}

/*
 * CF8/CFC: the address register takes the type 1 form (enable, bus,
 * device, function, dword register); the data port answers the dword's
 * bytes at CFCh..CFFh.  Only the 256-byte header is reachable this way.
 */
static BOOLEAN pci_config_ports_select(UINT64 Segment, UINT64 Bus,
                                       UINT64 Device, UINT64 Function,
                                       UINT64 Offset, volatile UINT8 **Data)
{
    UINT64 base = pci_config_cpu_base();
    volatile UINT32 *address = (volatile UINT32 *)(UINTN)(base + 0xcf8U);

    if (Segment != 0 || Bus > 0xff || Device > 0x1f || Function > 7 ||
        Offset >= 0x100) {
        return 0;
    }
    *address = 0x80000000U | ((UINT32)Bus << 16) | ((UINT32)Device << 11) |
               ((UINT32)Function << 8) | ((UINT32)Offset & 0xfcU);
    *Data = (volatile UINT8 *)(UINTN)(base + 0xcfcU + (Offset & 3U));
    return 1;
}

UINT64 pci_config_read_value(UINT64 Segment, UINT64 Bus, UINT64 Device,
                                    UINT64 Function, UINT64 Offset,
                                    UINTN Size)
{
    volatile UINT8 *p8;
    volatile UINT16 *p16;
    volatile UINT32 *p32;
    UINT64 addr;

    if (fw_pci_config_by_ports()) {
        volatile UINT8 *data;

        if (!pci_config_ports_select(Segment, Bus, Device, Function, Offset,
                                     &data)) {
            return pci_config_all_ones(Size);
        }
        addr = (UINT64)(UINTN)data;
    } else {
        addr = pci_config_ecam_addr(Segment, Bus, Device, Function, Offset);
        if (addr == 0) {
            return pci_config_all_ones(Size);
        }
    }

    switch (Size) {
    case 1:
        p8 = (volatile UINT8 *)(UINTN)addr;
        return *p8;
    case 2:
        p16 = (volatile UINT16 *)(UINTN)addr;
        return *p16;
    default:
        p32 = (volatile UINT32 *)(UINTN)addr;
        return *p32;
    }
}

void pci_config_write_value(UINT64 Segment, UINT64 Bus, UINT64 Device,
                                   UINT64 Function, UINT64 Offset,
                                   UINTN Size, UINT64 Value)
{
    volatile UINT8 *p8;
    volatile UINT16 *p16;
    volatile UINT32 *p32;
    UINT64 addr;

    if (fw_pci_config_by_ports()) {
        volatile UINT8 *data;

        if (!pci_config_ports_select(Segment, Bus, Device, Function, Offset,
                                     &data)) {
            return;
        }
        addr = (UINT64)(UINTN)data;
    } else {
        addr = pci_config_ecam_addr(Segment, Bus, Device, Function, Offset);
        if (addr == 0) {
            return;
        }
    }

    switch (Size) {
    case 1:
        p8 = (volatile UINT8 *)(UINTN)addr;
        *p8 = (UINT8)Value;
        break;
    case 2:
        p16 = (volatile UINT16 *)(UINTN)addr;
        *p16 = (UINT16)Value;
        break;
    default:
        p32 = (volatile UINT32 *)(UINTN)addr;
        *p32 = (UINT32)Value;
        break;
    }
}

static BOOLEAN __attribute__((noinline))
sal_pci_config_decode(UINT64 Address, UINT64 Size, UINT64 AddressType,
                      UINT64 *Segment, UINT64 *Bus, UINT64 *Device,
                      UINT64 *Function, UINT64 *Offset)
{
    if ((Size != 1 && Size != 2 && Size != 4) ||
        AddressType > 1) {
        return 0;
    }

    if (AddressType == 0) {
        if ((Address >> 32) != 0) {
            return 0;
        }
        *Offset = Address & 0xffU;
        *Function = (Address >> 8) & 0x7U;
        *Device = (Address >> 11) & 0x1fU;
        *Bus = (Address >> 16) & 0xffU;
        *Segment = (Address >> 24) & 0xffU;
    } else {
        if ((Address >> 44) != 0) {
            return 0;
        }
        *Offset = (Address & 0xffU) | (((Address >> 8) & 0xfU) << 8);
        *Function = (Address >> 12) & 0x7U;
        *Device = (Address >> 15) & 0x1fU;
        *Bus = (Address >> 20) & 0xffU;
        *Segment = (Address >> 28) & 0xffffU;
    }

    return ((*Offset & (Size - 1U)) == 0 && *Offset + Size <= 0x1000);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_pci_config_read(UINT64 Address, UINT64 Size, UINT64 AddressType,
                    UINT64 Reserved1, UINT64 Reserved2, UINT64 Reserved3,
                    UINT64 Reserved4)
{
    UINT64 segment;
    UINT64 bus;
    UINT64 device;
    UINT64 function;
    UINT64 offset;
    UINT64 value;

    if (!sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    Reserved4, 0, 0) ||
        !sal_pci_config_decode(Address, Size, AddressType, &segment, &bus,
                               &device, &function, &offset)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    value = pci_config_read_value(segment, bus, device, function, offset,
                                  (UINTN)Size);
    return sal_return(SAL_STATUS_SUCCESS, value, 0, 0);
}

static SAL_RETURN_VALUE __attribute__((noinline))
sal_pci_config_write(UINT64 Address, UINT64 Size, UINT64 Value,
                     UINT64 AddressType, UINT64 Reserved1,
                     UINT64 Reserved2, UINT64 Reserved3)
{
    UINT64 segment;
    UINT64 bus;
    UINT64 device;
    UINT64 function;
    UINT64 offset;

    if (!sal_reserved_args_are_zero(Reserved1, Reserved2, Reserved3,
                                    0, 0, 0) ||
        !sal_pci_config_decode(Address, Size, AddressType, &segment, &bus,
                               &device, &function, &offset)) {
        return sal_return(SAL_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    pci_config_write_value(segment, bus, device, function, offset,
                           (UINTN)Size, Value);
    return sal_return(SAL_STATUS_SUCCESS, 0, 0, 0);
}

BOOLEAN __attribute__((noinline)) sal_pci_config_selftest(void)
{
    SAL_RETURN_VALUE id;
    SAL_RETURN_VALUE id_ext;
    SAL_RETURN_VALUE command;
    SAL_RETURN_VALUE write_status;
    SAL_RETURN_VALUE bad_read_reserved;
    SAL_RETURN_VALUE bad_write_reserved;
    SAL_RETURN_VALUE bad_alignment;
    UINTN saved_runtime_ecam = mRuntimePciConfigEcam;
    BOOLEAN saved_virtual_map_applied = mVirtualAddressMapApplied;
    UINTN virtual_ecam = 0xe0000000d0018000ULL;

    mVirtualAddressMapApplied = 0;
    if (pci_config_cpu_base_for_mode(0) != fw_pci_config_window_base() ||
        pci_config_cpu_base_for_mode(1) !=
            (IA64_REGION6_BASE | fw_pci_config_window_base())) {
        mRuntimePciConfigEcam = saved_runtime_ecam;
        mVirtualAddressMapApplied = saved_virtual_map_applied;
        return 0;
    }
    mRuntimePciConfigEcam = virtual_ecam;
    mVirtualAddressMapApplied = 1;
    if (pci_config_cpu_base_for_mode(1) != virtual_ecam ||
        pci_config_ecam_addr_from_base(virtual_ecam, 0, 0, 4, 0, 0) !=
            virtual_ecam + (4U << 15) ||
        pci_config_ecam_addr_from_base(virtual_ecam, 0, 0, 7, 0, 0) !=
            virtual_ecam + (7U << 15)) {
        mRuntimePciConfigEcam = saved_runtime_ecam;
        mVirtualAddressMapApplied = saved_virtual_map_applied;
        return 0;
    }
    mRuntimePciConfigEcam = saved_runtime_ecam;
    mVirtualAddressMapApplied = saved_virtual_map_applied;

    id = sal_pci_config_read(0, 4, 0, 0, 0, 0, 0);
    if (id.Status != SAL_STATUS_SUCCESS) {
        return 0;
    }

    id_ext = sal_pci_config_read(0, 4, 1, 0, 0, 0, 0);
    if (id_ext.Status != SAL_STATUS_SUCCESS ||
        (UINT32)id_ext.Value0 != (UINT32)id.Value0) {
        return 0;
    }

    command = sal_pci_config_read(4, 2, 0, 0, 0, 0, 0);
    if (command.Status != SAL_STATUS_SUCCESS) {
        return 0;
    }

    write_status = sal_pci_config_write(4, 2, command.Value0, 0, 0, 0, 0);
    bad_read_reserved = sal_pci_config_read(0, 4, 0, 1, 0, 0, 0);
    bad_write_reserved = sal_pci_config_write(4, 2, command.Value0,
                                              0, 0, 1, 0);
    bad_alignment = sal_pci_config_read(1, 2, 0, 0, 0, 0, 0);
    return write_status.Status == SAL_STATUS_SUCCESS &&
           bad_read_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
           bad_write_reserved.Status == SAL_STATUS_INVALID_ARGUMENT &&
           bad_alignment.Status == SAL_STATUS_INVALID_ARGUMENT;
}

static BOOLEAN sal_runtime_state_valid(void)
{
    UINT64 psr = fw_read_psr();
    UINT64 translation = psr & (IA64_PSR_DT | IA64_PSR_RT | IA64_PSR_IT);

    if ((psr & IA64_PSR_CPL_MASK) != 0) {
        return 0;
    }

    return translation == 0 ||
           translation == (IA64_PSR_DT | IA64_PSR_RT | IA64_PSR_IT);
}

static SAL_RETURN_VALUE sal_proc_entry(UINT64 Index, UINT64 Arg1, UINT64 Arg2,
                                       UINT64 Arg3, UINT64 Arg4, UINT64 Arg5,
                                       UINT64 Arg6, UINT64 Arg7)
{
    UINT64 FunctionId = (UINT32)Index;
    SAL_RETURN_VALUE ret;

    if (!sal_runtime_state_valid()) {
        ret = sal_return(SAL_STATUS_ERROR, 0, 0, 0);
        goto out;
    }

    if (FunctionId == SAL_SET_VECTORS) {
        ret = sal_set_vectors(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_GET_STATE_INFO_SIZE) {
        ret = sal_get_state_info_size(Arg1, Arg2, Arg3, Arg4,
                                      Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_GET_STATE_INFO) {
        ret = sal_get_state_info(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_CLEAR_STATE_INFO) {
        ret = sal_clear_state_info(Arg1, Arg2, Arg3, Arg4,
                                   Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_MC_RENDEZ) {
        ret = sal_mc_rendez(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_MC_SET_PARAMS) {
        ret = sal_mc_set_params(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_REGISTER_PHYSICAL_ADDR) {
        ret = sal_register_physical_addr(Arg1, Arg2, Arg3, Arg4,
                                         Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_CACHE_FLUSH) {
        ret = sal_cache_flush(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_CACHE_INIT) {
        ret = sal_cache_init(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_PCI_CONFIG_READ) {
        ret = sal_pci_config_read(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_PCI_CONFIG_WRITE) {
        ret = sal_pci_config_write(Arg1, Arg2, Arg3, Arg4,
                                   Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_FREQ_BASE) {
        ret = sal_freq_base(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    /*
     * SAL_PHYSICAL_ID_INFO arrived with the December 2003 specification
     * (245359-007 revision history, "Added SAL_PHYSICAL_ID_INFO call"), so it
     * exists only on a platform advertising SAL 3.2.  A SAL 3.0 platform --
     * the Merced persona -- must report it as unimplemented rather than
     * offering a call its own SST revision predates.
     */
    if (FunctionId == SAL_PHYSICAL_ID_INFO) {
        if (fw_sal_revision() < SAL_REVISION_3_2) {
            ret = sal_return(SAL_STATUS_NOT_IMPLEMENTED, 0, 0, 0);
            goto out;
        }
        ret = sal_physical_id_info(Arg1, Arg2, Arg3, Arg4,
                                   Arg5, Arg6, Arg7);
        goto out;
    }

    if (FunctionId == SAL_UPDATE_PAL) {
        ret = sal_update_pal(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7);
        goto out;
    }

    ret = sal_return(SAL_STATUS_NOT_IMPLEMENTED, 0, 0, 0);

out:
    return ret;
}

BOOLEAN __attribute__((noinline)) sal_proc_dispatch_selftest(void)
{
    SAL_RETURN_VALUE masked;
    SAL_RETURN_VALUE unimplemented;

    masked = sal_proc_entry(0xfeedface00000000ULL | SAL_FREQ_BASE,
                            SAL_FREQ_BASE_PLATFORM, 0, 0, 0, 0, 0, 0);
    unimplemented = sal_proc_entry(0xfeedface04000000ULL,
                                   0, 0, 0, 0, 0, 0, 0);

    return sal_runtime_state_valid() &&
           masked.Status == SAL_STATUS_SUCCESS &&
           masked.Value0 == PLATFORM_BASE_FREQUENCY &&
           masked.Value1 == (UINT64)-1 &&
           masked.Value2 == 0 &&
           unimplemented.Status == SAL_STATUS_NOT_IMPLEMENTED &&
           unimplemented.Value0 == 0 &&
           unimplemented.Value1 == 0 &&
           unimplemented.Value2 == 0;
}


/* --- UART/VGA-text/ConOut/ConIn console stack lives in console.c --------- */

UINT64 __attribute__((noinline)) fw_read_rsc(void)
{
    UINT64 rsc;

    __asm__ volatile ("mov %0 = ar.rsc" : "=r"(rsc));
    return rsc;
}

void __attribute__((noinline)) fw_restore_rsc(UINT64 rsc)
{
    __asm__ volatile ("mov ar.rsc = %0;;" : : "r"(rsc) : "memory");
}

void fw_restore_psr(UINT64 psr)
{
    __asm__ volatile (
        "rsm psr.ic;;\n\t"
        "srlz.d;;\n\t"
        "movl r14 = 1f;;\n\t"
        "mov cr.ipsr = %0;;\n\t"
        "mov cr.iip = r14\n\t"
        "mov cr.ifs = r0;;\n\t"
        "rfi;;\n\t"
        "1:\n\t"
        "srlz.i;;"
        :
        : "r"(psr)
        : "r14", "memory");
}

extern UINTN fw_call_efi_entry(UINTN (*Entry)(EFI_HANDLE, EFI_SYSTEM_TABLE *),
                               EFI_HANDLE ImageHandle,
                               EFI_SYSTEM_TABLE *SystemTable,
                               UINT64 SavedPsr,
                               UINT64 EntryPsrLow);
extern VOID fw_call_ap_rendezvous(const UINT64 *Descriptor,
                                  UINT64 EntryPsrLow,
                                  UINT64 SavedPsrLow,
                                  UINT64 SavedRsc);
extern VOID fw_prepare_sal_handoff_registers(VOID);
extern UINTN fw_efi_entry_abi_probe(EFI_HANDLE ImageHandle,
                                    EFI_SYSTEM_TABLE *SystemTable);
extern UINTN fw_sal_handoff_probe(EFI_HANDLE ImageHandle,
                                  EFI_SYSTEM_TABLE *SystemTable);

__asm__(
".text\n"
".macro FW_SET_RR address, value\n"
"    movl r14 = \\address\n"
"    movl r15 = \\value\n"
"    ;;\n"
"    mov rr[r14] = r15\n"
"    ;;\n"
".endm\n"
".macro FW_CLEAR_PKR index\n"
"    adds r14 = \\index, r0\n"
"    ;;\n"
"    mov pkr[r14] = r0\n"
"    ;;\n"
".endm\n"
".macro FW_PROBE_RR address\n"
"    movl r16 = \\address\n"
"    ;;\n"
"    mov r15 = rr[r16]\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
".endm\n"
".macro FW_PROBE_PKR index\n"
"    adds r16 = \\index, r0\n"
"    ;;\n"
"    mov r15 = pkr[r16]\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
".endm\n"
".align 16\n"
".global fw_prepare_sal_handoff_registers\n"
".type fw_prepare_sal_handoff_registers, @function\n"
".proc fw_prepare_sal_handoff_registers\n"
"fw_prepare_sal_handoff_registers:\n"
"    rsm psr.ic\n"
"    ;;\n"
"    srlz.d\n"
"    ;;\n"
"    movl r14 = 0x4\n"
"    ;;\n"
"    mov cr.dcr = r14\n"
"    movl r14 = __fw_ivt\n"
"    ;;\n"
"    mov cr.iva = r14\n"
"    movl r14 = 0x3c\n"
"    ;;\n"
"    mov cr.pta = r14\n"
"    ;;\n"
"    FW_SET_RR 0x0000000000000000, 0x100030\n"
"    FW_SET_RR 0x2000000000000000, 0x100130\n"
"    FW_SET_RR 0x4000000000000000, 0x100230\n"
"    FW_SET_RR 0x6000000000000000, 0x100330\n"
"    FW_SET_RR 0x8000000000000000, 0x100430\n"
"    FW_SET_RR 0xa000000000000000, 0x100530\n"
"    FW_SET_RR 0xc000000000000000, 0x100630\n"
"    FW_SET_RR 0xe000000000000000, 0x100730\n"
".irp index, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15\n"
"    FW_CLEAR_PKR \\index\n"
".endr\n"
"    srlz.d\n"
"    ;;\n"
"    srlz.i\n"
"    ;;\n"
"    mov cr.ifa = r0\n"
"    movl r15 = 0x58\n"
"    movl r16 = 0x661\n"
"    ;;\n"
"    mov cr.itir = r15\n"
"    mov r14 = r0\n"
"    ;;\n"
"    itr.i itr[r14] = r16\n"
"    ;;\n"
"    srlz.i\n"
"    ;;\n"
"    mov ar.rsc = r0\n"
"    br.ret.sptk.many b0\n"
".endp fw_prepare_sal_handoff_registers\n"
"\n"
".align 16\n"
".global fw_call_efi_entry\n"
".type fw_call_efi_entry, @function\n"
".proc fw_call_efi_entry\n"
"fw_call_efi_entry:\n"
"    .prologue\n"
"    .save ar.pfs, r37\n"
"    alloc r37 = ar.pfs, 5, 7, 2, 0\n"
"    .save rp, r38\n"
"    mov r38 = b0\n"
"    mov r39 = gp\n"
"    mov r40 = r35\n"
"    mov r41 = sp\n"
"    mov r43 = ar.rsc\n"
"    adds sp = -16, sp\n"
"    ;;\n"
"    adds r14 = 8, sp\n"
"    ;;\n"
"    st8 [sp] = r33\n"
"    st8 [r14] = r34\n"
"    mov r14 = r32\n"
"    ;;\n"
"    ld8 r15 = [r14], 8\n"
"    ;;\n"
"    ld8 gp = [r14]\n"
"    cmp.eq p6, p7 = r36, r0\n"
"    ;;\n"
"(p6) br.cond.sptk.few 3f\n"
"    ;;\n"
"    mov psr.l = r36\n"
"    ;;\n"
"    srlz.i\n"
"    ;;\n"
"3:\n"
"    mov ar.rsc = r0\n"
"    ;;\n"
"    bsw.1\n"
"    ;;\n"
"    mov r44 = r33\n"
"    mov r45 = r34\n"
"    mov b6 = r15\n"
"    br.call.sptk.many b0 = b6\n"
"    ;;\n"
"    mov r42 = r8\n"
"    mov ar.rsc = r43\n"
"    mov sp = r41\n"
"    mov gp = r39\n"
"    rsm psr.ic\n"
"    ;;\n"
"    srlz.d\n"
"    ;;\n"
"    movl r14 = 4f\n"
"    ;;\n"
"    mov cr.ipsr = r40\n"
"    ;;\n"
"    mov cr.iip = r14\n"
"    mov cr.ifs = r0\n"
"    ;;\n"
"    rfi\n"
"    ;;\n"
"4:\n"
"    srlz.i\n"
"    ;;\n"
"    mov r8 = r42\n"
"    mov b0 = r38\n"
"    mov ar.pfs = r37\n"
"    br.ret.sptk.many b0\n"
".endp fw_call_efi_entry\n"
"\n"
".align 16\n"
".global fw_call_ap_rendezvous\n"
".type fw_call_ap_rendezvous, @function\n"
".proc fw_call_ap_rendezvous\n"
"fw_call_ap_rendezvous:\n"
"    .prologue\n"
"    .save ar.pfs, r36\n"
"    alloc r36 = ar.pfs, 4, 5, 0, 0\n"
"    .save rp, r37\n"
"    mov r37 = b0\n"
"    mov r38 = gp\n"
"    mov r39 = r34\n"
"    mov r40 = r35\n"
"    mov r14 = r32\n"
"    ;;\n"
"    ld8 r15 = [r14], 8\n"
"    ;;\n"
"    ld8 gp = [r14]\n"
"    ;;\n"
"    mov psr.l = r33\n"
"    ;;\n"
"    srlz.i\n"
"    ;;\n"
"    mov ar.rsc = r0\n"
"    ;;\n"
"    bsw.1\n"
"    ;;\n"
"    mov b6 = r15\n"
"    ;;\n"
"    br.call.sptk.many b0 = b6\n"
"    ;;\n"
"    rsm psr.ic\n"
"    ;;\n"
"    srlz.d\n"
"    ;;\n"
"    bsw.0\n"
"    ;;\n"
"    mov psr.l = r39\n"
"    ;;\n"
"    srlz.i\n"
"    ;;\n"
"    mov ar.rsc = r40\n"
"    mov gp = r38\n"
"    mov b0 = r37\n"
"    mov ar.pfs = r36\n"
"    ;;\n"
"    br.ret.sptk.many b0\n"
".endp fw_call_ap_rendezvous\n"
"\n"
".align 16\n"
".global fw_efi_entry_abi_probe\n"
".type fw_efi_entry_abi_probe, @function\n"
".proc fw_efi_entry_abi_probe\n"
"fw_efi_entry_abi_probe:\n"
"    alloc r34 = ar.pfs, 2, 1, 0, 0\n"
"    adds r14 = 8, sp\n"
"    ;;\n"
"    ld8 r15 = [sp]\n"
"    ld8 r16 = [r14]\n"
"    ;;\n"
"    xor r15 = r15, r32\n"
"    xor r16 = r16, r33\n"
"    ;;\n"
"    or r15 = r15, r16\n"
"    mov r17 = ar.rsc\n"
"    ;;\n"
"    or r15 = r15, r17\n"
"    ;;\n"
"    cmp.eq p6, p7 = r15, r0\n"
"    ;;\n"
"(p6) adds r8 = 1, r0\n"
"(p7) mov r8 = r0\n"
"    mov ar.pfs = r34\n"
"    br.ret.sptk.many b0\n"
".endp fw_efi_entry_abi_probe\n"
"\n"
".align 16\n"
".global fw_sal_handoff_probe\n"
".type fw_sal_handoff_probe, @function\n"
".proc fw_sal_handoff_probe\n"
"fw_sal_handoff_probe:\n"
"    movl r14 = mSalHandoffProbe\n"
"    ;;\n"
"    mov r15 = psr\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = ar.rsc\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = cr.dcr\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = cr.iva\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = cr.pta\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = sp\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = ar.bsp\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    mov r15 = ar.bspstore\n"
"    ;;\n"
"    st8 [r14] = r15, 8\n"
"    FW_PROBE_RR 0x0000000000000000\n"
"    FW_PROBE_RR 0x2000000000000000\n"
"    FW_PROBE_RR 0x4000000000000000\n"
"    FW_PROBE_RR 0x6000000000000000\n"
"    FW_PROBE_RR 0x8000000000000000\n"
"    FW_PROBE_RR 0xa000000000000000\n"
"    FW_PROBE_RR 0xc000000000000000\n"
"    FW_PROBE_RR 0xe000000000000000\n"
".irp index, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15\n"
"    FW_PROBE_PKR \\index\n"
".endr\n"
"    ;;\n"
"    adds r8 = 1, r0\n"
"    br.ret.sptk.many b0\n"
".endp fw_sal_handoff_probe\n"
".purgem FW_SET_RR\n"
".purgem FW_CLEAR_PKR\n"
".purgem FW_PROBE_RR\n"
".purgem FW_PROBE_PKR\n");

BOOLEAN __attribute__((noinline)) efi_entry_handoff_selftest(void)
{
    return fw_call_efi_entry(fw_efi_entry_abi_probe, mImageHandle,
                             &mSystemTable, fw_read_psr(), 0) == 1;
}

UINT64 sal_loader_psr_low(void)
{
    return IA64_PSR_AC | IA64_PSR_IC |
           mResetFloatingPointDisableBits;
}

void prepare_sal_loader_handoff(void)
{
    fw_prepare_sal_handoff_registers();
}

BOOLEAN __attribute__((noinline)) sal_loader_handoff_selftest(void)
{
    UINT64 expected_psr = sal_loader_psr_low() | IA64_PSR_BN;
    UINTN i;

    fw_set_mem(&mSalHandoffProbe, sizeof(mSalHandoffProbe), 0xff);
    if (fw_call_efi_entry(fw_sal_handoff_probe, mImageHandle, &mSystemTable,
                          fw_read_psr(), sal_loader_psr_low()) != 1) {
        return 0;
    }

    if (mSalHandoffProbe.Psr != expected_psr ||
        mSalHandoffProbe.Rsc != 0 ||
        mSalHandoffProbe.Dcr != IA64_DCR_LC ||
        mSalHandoffProbe.Iva != SAL_IVT_BASE ||
        mSalHandoffProbe.Pta != SAL_PTA_DISABLED_VALUE ||
        mSalHandoffProbe.Sp < mBootStackBase +
                              IA64_EFI_MIN_STACK_BYTES ||
        mSalHandoffProbe.Sp >= mBootStackTop ||
        mSalHandoffProbe.Bsp < SAL_BACKING_STORE_BASE ||
        mSalHandoffProbe.Bsp + IA64_EFI_MIN_BACKING_BYTES >
            SAL_BACKING_STORE_END ||
        mSalHandoffProbe.BspStore < SAL_BACKING_STORE_BASE ||
        mSalHandoffProbe.BspStore > mSalHandoffProbe.Bsp) {
        return 0;
    }

    for (i = 0; i < 8; i++) {
        if (mSalHandoffProbe.Rr[i] !=
            SAL_RR_VALUE(SAL_RR_FIRST_RID + i)) {
            return 0;
        }
    }
    for (i = 0; i < 16; i++) {
        if (mSalHandoffProbe.Pkr[i] != 0) {
            return 0;
        }
    }
    return 1;
}

extern VOID fw_pal_halt_light(VOID);

UINT64 fw_read_ivr(void)
{
    UINT64 vector;

    __asm__ volatile ("mov %0 = cr.ivr;;\n\tsrlz.d;;"
                      : "=r"(vector) : : "memory");
    return vector & 0xffU;
}

void fw_write_eoi(void)
{
    __asm__ volatile ("mov cr.eoi = r0;;\n\tsrlz.d;;" : : : "memory");
}

static void fw_clear_tpr(void)
{
    __asm__ volatile ("mov cr.tpr = r0;;\n\tsrlz.d;;" : : : "memory");
}

static void fw_ap_rendezvous(void)
{
    /* The BSP publishes this registration before issuing the wake IPI. */
    volatile SAL_VECTOR_REGISTRATION *registration =
        &mSalVectors[SAL_VECTOR_OS_BOOT_RENDEZ];
    UINT64 descriptor[2] __attribute__((aligned(16)));
    UINT64 saved_psr;
    UINT64 saved_rsc;

    if (fw_read_ivr() != 0xff) {
        fw_write_eoi();
        return;
    }
    fw_write_eoi();

    __asm__ volatile ("mf;;" : : : "memory");
    if (!registration->Valid || registration->HandlerAddr1 == 0) {
        return;
    }
    descriptor[0] = registration->HandlerAddr1;
    descriptor[1] = registration->Gp1;
    saved_psr = fw_read_psr();
    saved_rsc = fw_read_rsc();
    prepare_sal_loader_handoff();
    fw_call_ap_rendezvous(descriptor, sal_loader_psr_low(),
                          saved_psr, saved_rsc);
}

void firmware_ap_main(UINT64 ProcessorId, UINT64 ResetPalProc)
{
    (void)ProcessorId;

    fw_platform_install_pal(1, ResetPalProc);
    fw_platform_register_processor(ResetPalProc);
    __sync_fetch_and_add(&mApCheckins, 1);
    fw_ap_rendezvous();
    for (;;) {
        /* TPR is scratch on return from OS_BOOT_RENDEZ. */
        fw_clear_tpr();
        fw_pal_halt_light();
        fw_ap_rendezvous();
    }
}

BOOLEAN fw_data_translation_enabled(void)
{
    UINT64 psr = fw_read_psr();

    return (psr & IA64_PSR_DT) != 0;
}


void fw_platform_decode_topology(void)
{
    fw_platform_rendezvous_processors();
    fw_platform_decode_package_topology();
}

static FW_PLATFORM_HANDOFF mPlatformHandoff = {
    .MemDesc = mMemoryMap,
    .MemDescCount = &mMemoryMapEntries,
    .MapKey = &mMapKey,
    .DecodeTopology = fw_platform_decode_topology,
    .InitMemoryMap = efi_init_memory_map,
    .InitPlatformTables = efi_init_platform_tables,
    .SalProcFunctionEntry = fw_sal_proc_function_entry,
};

const FW_PLATFORM_HANDOFF *fw_platform(void)
{
    return &mPlatformHandoff;
}

void fw_platform_publish_tables(VOID *SalSystemTable, VOID *AcpiRsdp)
{
    mPlatformHandoff.SalSystemTable = SalSystemTable;
    mPlatformHandoff.AcpiRsdp = AcpiRsdp;
}
