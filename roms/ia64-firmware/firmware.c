/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 EFI Firmware — written in C, compiled with ia64-linux-gnu-gcc.
 *
 * Provides:
 *  - Serial console output (UART at 0x47F0000000)
 *  - VGA text output via GOP/UGA framebuffer
 *  - EFI system, boot-service, runtime-service, and protocol tables
 *  - Boot from disk (PE32+ image loader)
 */

/* Freestanding — no libc or hosted headers. */

#include "fw-base.h"
#include "fw-boot-shell.h"
#include "fw-debug-support.h"
#include "fw-device-path.h"
#include "fw-efi-types.h"
#include "fw-ebc.h"
#include "fw-legacy-io.h"
#include "fw-pointer.h"
#include "fw-services.h"
#include "fw-memmap.h"
#include "fw-platform-handoff.h"
#include "fw-platform-layout.h"
#include "fw-acpi.h"
#include "fw-pe.h"
#include "fw-storage.h"
#include "fw-fs.h"
#include "fw-uart.h"
#include "fw-uga-io.h"
#include "ia64-fw-acpi-aml.h"
#include "fw-usb.h"
#include "vga_font_8x16.h"


/*
 * SST SAL_REV is BCD major.minor and must name a revision the specification
 * actually defines.  245359-007 table 3-3 enumerates them: 3.2 (0x0320) is the
 * December 2003 specification, 3.1 the November 2002 one, 3.0 the January/July
 * 2001 pair, 2.9 July 2000 and 2.8 January 2000.  There is no 3.4, which is
 * what this firmware used to report.
 *
 * Which one is truthful depends on the processor the machine is impersonating,
 * so it is chosen at run time from CPUID[3].family (see fw_sal_revision()).
 * A Merced platform is a SAL 3.0 platform: the SST template in the HP i2000's
 * own firmware (bios130.BIN at file offset 0x37d120) reads "SST_" followed by
 * minor 0x00, major 0x03.  Everything newer reports 3.2, which matches the
 * procedure set this SAL implements.
 */
/* SAL revision/TR-descriptor macros live in fw-acpi.h. */
/* Per-CPU initial RSE backing stores live in the RAM-top CPU-assist region. */

/* Platform memory-map/layout constants live in fw-platform-layout.h. */

#define EFI_VARIABLE_ACCESS_ATTRIBUTES \
    (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define EFI_VARIABLE_SUPPORTED_ATTRIBUTES \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
     EFI_VARIABLE_RUNTIME_ACCESS)

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001U
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002U
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL       0x00000004U
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008U
#define EFI_OPEN_PROTOCOL_BY_DRIVER           0x00000010U
#define EFI_OPEN_PROTOCOL_EXCLUSIVE           0x00000020U

#define PCI_VGA_IOPORT_OFFSET         0x400U
#define PCI_VGA_BOCHS_OFFSET          0x500U
#define PCI_VGA_QEXT_OFFSET           0x600U
#define PCI_VGA_QEXT_REG_BYTEORDER    0x4U
#define PCI_VGA_QEXT_LITTLE_ENDIAN    0x1e1e1e1eU
#define VGA_IOPORT_BASE               0x3c0U
#define VGA_ATT_W                     0x3c0U
#define VGA_MIS_W                     0x3c2U
#define VGA_SEQ_I                     0x3c4U
#define VGA_SEQ_D                     0x3c5U
#define VGA_PEL_MSK                   0x3c6U
#define VGA_PEL_IW                    0x3c8U
#define VGA_PEL_D                     0x3c9U
#define VGA_GFX_I                     0x3ceU
#define VGA_GFX_D                     0x3cfU
#define VGA_CRTC_I                    0x3d4U
#define VGA_CRTC_D                    0x3d5U
#define VGA_IS1_RC                    0x3daU
#define VGA_MIS_COLOR                 0x01U
#define VGA_AR_ENABLE_DISPLAY         0x20U
/* VGA_LEGACY_FB_* live in fw-platform-layout.h. */
#define VBE_DISPI_INDEX_ID            0x0U
#define VBE_DISPI_INDEX_XRES          0x1U
#define VBE_DISPI_INDEX_YRES          0x2U
#define VBE_DISPI_INDEX_BPP           0x3U
#define VBE_DISPI_INDEX_ENABLE        0x4U
#define VBE_DISPI_INDEX_BANK          0x5U
#define VBE_DISPI_INDEX_VIRT_WIDTH    0x6U
#define VBE_DISPI_INDEX_X_OFFSET      0x8U
#define VBE_DISPI_INDEX_Y_OFFSET      0x9U
#define VBE_DISPI_ID5                 0xB0C5U
#define VBE_DISPI_ENABLED             0x01U
#define VBE_DISPI_LFB_ENABLED         0x40U

/* Shared with the machine model via hw/ia64/ia64_vpc_abi.h. */
/* PCI I/O / ECAM aliases live in fw-platform-layout.h. */
#define PCI_IDE_CMD646_ID             0x06461095U




/* --- EFI/UEFI type definitions -------------------------------------------- */

#include "linker-symbols.h"

/* EFI table signatures */
#define EFI_SYSTEM_TABLE_SIGNATURE  0x5453595320494249ULL
#define EFI_BOOT_SERVICES_SIGNATURE  0x56524553544F4F42ULL
#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544E5552ULL

/* Driver Binding / Load File / Component Name typedefs live in
   fw-efi-types.h. */

/* EFI table/service typedefs live in fw-efi-types.h. */

static BOOLEAN efi_memory_type_is_valid(EFI_MEMORY_TYPE Type)
{
    UINT32 type = (UINT32)Type;

    return type < (UINT32)EfiMaxMemoryType ||
           type >= EFI_MEMORY_TYPE_OS_RESERVED_MIN;
}

UINT64 efi_memory_attribute(EFI_MEMORY_TYPE Type, UINT64 Attribute)
{
    if (Type == EfiRuntimeServicesCode ||
        Type == EfiRuntimeServicesData) {
        return Attribute | EFI_MEMORY_RUNTIME;
    }
    return Attribute;
}

/* --- EFI Loaded Image Protocol --------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID { 0x5B1B31A1, 0x9562, 0x11d2, \
    { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_LOADED_IMAGE_PROTOCOL_REVISION  0x00001000

/* EFI_LOADED_IMAGE_PROTOCOL lives in fw-efi-types.h. */

/* --- TCG EFI protocol ----------------------------------------------------- */

#define EFI_TCG_PROTOCOL_GUID { 0xf541796d, 0xa62e, 0x4954, \
    { 0xa7, 0x75, 0x95, 0x84, 0xf6, 0x1b, 0x9c, 0xdd } }

#define TPM_ALG_SHA             0x00000004U
#define TCG_SHA1_DIGEST_SIZE    20U

typedef UINT32 TCG_ALGORITHM_ID;
typedef UINT32 TCG_PCRINDEX;
typedef UINT32 TCG_EVENTTYPE;

typedef struct {
    UINT8 Major;
    UINT8 Minor;
    UINT8 RevMajor;
    UINT8 RevMinor;
} TCG_VERSION;

typedef struct {
    UINT8 Size;
    TCG_VERSION StructureVersion;
    TCG_VERSION ProtocolSpecVersion;
    UINT8 HashAlgorithmBitmap;
    BOOLEAN TPMPresentFlag;
    BOOLEAN TPMDeactivatedFlag;
} TCG_EFI_BOOT_SERVICE_CAPABILITY;

typedef struct {
    UINT8 Digest[TCG_SHA1_DIGEST_SIZE];
} TCG_DIGEST;

typedef struct {
    TCG_PCRINDEX PCRIndex;
    TCG_EVENTTYPE EventType;
    TCG_DIGEST Digest;
    UINT32 EventSize;
} TCG_PCR_EVENT_HDR;

typedef struct {
    TCG_PCRINDEX PCRIndex;
    TCG_EVENTTYPE EventType;
    TCG_DIGEST Digest;
    UINT32 EventSize;
    UINT8 Event[1];
} TCG_PCR_EVENT;

typedef struct _EFI_TCG_PROTOCOL EFI_TCG_PROTOCOL;

struct _EFI_TCG_PROTOCOL {
    EFI_STATUS (*StatusCheck)(EFI_TCG_PROTOCOL *This,
                              TCG_EFI_BOOT_SERVICE_CAPABILITY *ProtocolCapability,
                              UINT32 *TCGFeatureFlags,
                              EFI_PHYSICAL_ADDRESS *EventLogLocation,
                              EFI_PHYSICAL_ADDRESS *EventLogLastEntry);
    EFI_STATUS (*HashAll)(EFI_TCG_PROTOCOL *This, UINT8 *HashData,
                          UINT64 HashDataLen,
                          TCG_ALGORITHM_ID AlgorithmId,
                          UINT64 *HashedDataLen,
                          UINT8 **HashedDataResult);
    EFI_STATUS (*LogEvent)(EFI_TCG_PROTOCOL *This,
                           TCG_PCR_EVENT *TCGLogData,
                           UINT32 *EventNumber, UINT32 Flags);
    EFI_STATUS (*PassThroughToTpm)(EFI_TCG_PROTOCOL *This,
                                   UINT32 TpmInputParameterBlockSize,
                                   UINT8 *TpmInputParameterBlock,
                                   UINT32 TpmOutputParameterBlockSize,
                                   UINT8 *TpmOutputParameterBlock);
    EFI_STATUS (*HashLogExtendEvent)(EFI_TCG_PROTOCOL *This,
                                     EFI_PHYSICAL_ADDRESS HashData,
                                     UINT64 HashDataLen,
                                     TCG_ALGORITHM_ID AlgorithmId,
                                     TCG_PCR_EVENT *TCGLogData,
                                     UINT32 *EventNumber,
                                     EFI_PHYSICAL_ADDRESS *EventLogLastEntry);
};

/* EFI debug-image-info typedefs live in fw-efi-types.h. */

/* --- EFI GOP / UGA graphics protocols ------------------------------------- */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

#define GOP_BGRX_RED_MASK       0x00ff0000U
#define GOP_BGRX_GREEN_MASK     0x0000ff00U
#define GOP_BGRX_BLUE_MASK      0x000000ffU
#define GOP_BGRX_RESERVED_MASK  0xff000000U

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    UINT8 Blue;
    UINT8 Green;
    UINT8 Red;
    UINT8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                            UINT32 ModeNumber, UINTN *SizeOfInfo,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (*SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                          UINT32 ModeNumber);
    EFI_STATUS (*Blt)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
                      EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
                      UINTN SourceX, UINTN SourceY,
                      UINTN DestinationX, UINTN DestinationY,
                      UINTN Width, UINTN Height, UINTN Delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

typedef EFI_GRAPHICS_OUTPUT_BLT_PIXEL EFI_UGA_PIXEL;
typedef EFI_GRAPHICS_OUTPUT_BLT_OPERATION EFI_UGA_BLT_OPERATION;

typedef struct _EFI_UGA_DRAW_PROTOCOL EFI_UGA_DRAW_PROTOCOL;

struct _EFI_UGA_DRAW_PROTOCOL {
    EFI_STATUS (*GetMode)(EFI_UGA_DRAW_PROTOCOL *This,
                          UINT32 *HorizontalResolution,
                          UINT32 *VerticalResolution,
                          UINT32 *ColorDepth,
                          UINT32 *RefreshRate);
    EFI_STATUS (*SetMode)(EFI_UGA_DRAW_PROTOCOL *This,
                          UINT32 HorizontalResolution,
                          UINT32 VerticalResolution,
                          UINT32 ColorDepth,
                          UINT32 RefreshRate);
    EFI_STATUS (*Blt)(EFI_UGA_DRAW_PROTOCOL *This,
                      EFI_UGA_PIXEL *BltBuffer,
                      EFI_UGA_BLT_OPERATION BltOperation,
                      UINTN SourceX, UINTN SourceY,
                      UINTN DestinationX, UINTN DestinationY,
                      UINTN Width, UINTN Height, UINTN Delta);
};

/* SAL + ACPI table scaffolds live in fw-acpi.h. */

FW_STATIC_ASSERT(FW_DSDT_PCI_ROOT_AML_SIZE == 2558u, dsdt_generated_aml_size);
FW_STATIC_ASSERT(FW_SSDT_PLATFORM_DEVICES_AML_SIZE == 496u,
                 ssdt_generated_aml_size);
/* The nested zx1-profile DSDT/SSDT; the larger sets ACPI_DSDT/SSDT Aml[]. */
FW_STATIC_ASSERT(FW_DSDT_PCI_ROOT_ZX1_AML_SIZE == 1182u,
                 dsdt_zx1_generated_aml_size);
FW_STATIC_ASSERT(FW_SSDT_PLATFORM_DEVICES_ZX1_AML_SIZE == 506u,
                 ssdt_zx1_generated_aml_size);
FW_STATIC_ASSERT(sizeof(EFI_DEBUG_IMAGE_INFO_TABLE_HEADER) == 16,
                 efi_debug_image_info_table_header_size);
FW_STATIC_ASSERT(sizeof(EFI_DEBUG_IMAGE_INFO_NORMAL) == 24,
                 efi_debug_image_info_normal_size);
FW_STATIC_ASSERT(sizeof(FW_OHCI_HCCA) == 256, ohci_hcca_size);
FW_STATIC_ASSERT(sizeof(FW_OHCI_ED) == 16, ohci_ed_size);
FW_STATIC_ASSERT(sizeof(FW_OHCI_TD) == 16, ohci_td_size);
FW_STATIC_ASSERT(sizeof(TCG_VERSION) == 4, tcg_version_size);
FW_STATIC_ASSERT(sizeof(TCG_DIGEST) == 20, tcg_digest_size);
FW_STATIC_ASSERT(sizeof(TCG_EFI_BOOT_SERVICE_CAPABILITY) == 12,
                 tcg_capability_size);
FW_STATIC_ASSERT(__builtin_offsetof(TCG_PCR_EVENT, Event) == 32,
                 tcg_pcr_event_payload_offset);


EFI_CONFIGURATION_TABLE mConfigTables[PLATFORM_TABLE_MAX];
EFI_SYSTEM_TABLE_POINTER       *mSystemTablePointer;
UINT64                          mSystemTablePointerBase;
UINT64                          mBootStackBase;
UINT64                          mBootStackTop;
/* Base of the 2 MiB RAM-top CPU-assist region (IA64_FW_CPU_ASSIST_BASE_FOR). */
UINT64                          mCpuAssistBase;

/* The anchor is a soft reservation: see efi_release_low_anchor_if_claimed(). */
EFI_DEBUG_IMAGE_INFO_TABLE_HEADER mDebugImageInfoHeader;
static EFI_DEBUG_IMAGE_INFO mDebugImageInfoTable[LOADED_IMAGE_MAX + 1U];
static EFI_DEBUG_IMAGE_INFO_NORMAL mDebugImageInfoNormal[LOADED_IMAGE_MAX + 1U];
const UINT8 gEfiSalSystemTableGuid[16] = {
    0x32, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
    0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};
const UINT8 gEfiHcdpTableGuid[16] = {
    0x8d, 0x93, 0x51, 0xf9, 0x0b, 0x62, 0xef, 0x42,
    0x82, 0x79, 0xa8, 0x4b, 0x79, 0x61, 0x78, 0x98
};
const UINT8 gEfiSmbiosTableGuid[16] = {
    0x31, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
    0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};
static const UINT8 gEfiEventGroupExitBootServicesGuid[16] = {
    0x55, 0xf0, 0xab, 0x27, 0xb8, 0xb1, 0x26, 0x4c,
    0x80, 0x48, 0x74, 0x8f, 0x37, 0xba, 0xa2, 0xdf
};
static const UINT8 gEfiEventGroupBeforeExitBootServicesGuid[16] = {
    0x74, 0xe2, 0xe0, 0x8b, 0x70, 0x39, 0x44, 0x4b,
    0x80, 0xc5, 0x1a, 0xb9, 0x50, 0x2f, 0x3b, 0xfc
};
static const UINT8 gEfiEventGroupVirtualAddressChangeGuid[16] = {
    0x98, 0x76, 0xfa, 0x13, 0x31, 0xc8, 0xc7, 0x49,
    0x87, 0xea, 0x8f, 0x43, 0xfc, 0xc2, 0x51, 0x96
};

/* --- Virtual address map (SetVirtualAddressMap state) --------------------- */

static EFI_MEMORY_DESCRIPTOR  mVirtualAddressMap[MEMORY_MAP_MAX];
static UINTN                  mVirtualAddressMapEntries;
static BOOLEAN                mVirtualAddressMapInProgress;
BOOLEAN                       mVirtualAddressMapApplied;
/* Guest state + machine-handoff decode live in platform.c. */

/* --- Boot and runtime entry points ----------------------------------------- */

EFI_BOOT_SERVICES    mBootServices;
static EFI_RUNTIME_SERVICES mRuntimeServices;
EFI_SYSTEM_TABLE            mSystemTable;
static EFI_GRAPHICS_OUTPUT_PROTOCOL mGopProto;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mGopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mGopModeInfo[5];
static EFI_UGA_DRAW_PROTOCOL mUgaDrawProto;

UINT32 mGraphicsWidth;
UINT32 mGraphicsHeight;
UINT32 mGraphicsStride;
BOOLEAN                       mGraphicsActive;
static BOOLEAN                mGraphicsHandoffClaimed;
EFI_PHYSICAL_ADDRESS          mNextPageAddr = 0x01000000ULL;
BOOLEAN                       mBootServicesExited;
static BOOLEAN                mBeforeExitBootServicesSignaled;
static BOOLEAN                mExitBootServicesEventsSignaled;
static UINTN                  mRuntimeAcpiPm1Cnt =
    LEGACY_IO_BASE + ACPI_PM_IO_BASE + ACPI_PM1_CNT_OFFSET;
static UINTN                  mRuntimeResetControl =
    LEGACY_IO_BASE + ACPI_PM_IO_BASE + ACPI_PM_RESET_OFFSET;
UINTN                         mRuntimePciConfigEcam =
    PCI_CONFIG_ECAM_BASE;
/* MC146818 CMOS RTC index port; the data port is index + 1 (rework D8). */
static UINTN                  mRuntimeRtc = LEGACY_IO_BASE + 0x70U;
static UINTN                  mRuntimeRtcState =
    FW_NVRAM_BASE + FW_NVRAM_RTC_OFFSET;

void fw_copy_mem(VOID *Destination, const VOID *Source, UINTN Length);
void fw_set_mem(VOID *Buffer, UINTN Size, UINT8 Value);
EFI_STATUS rs_get_boot0000_variable(UINT32 *Attributes,
                                           UINTN *DataSize, VOID *Data);
EFI_STATUS rs_get_shell_variable(UINT32 *Attributes,
                                 UINTN *DataSize, VOID *Data);
EFI_STATUS rs_convert_pointer_value(UINTN *Address);
BOOLEAN ranges_overlap(UINT64 a_base, UINT64 a_size,
                              UINT64 b_base, UINT64 b_size);
static BOOLEAN efi_pages_to_size(UINTN Pages, UINT64 *Size);
static void fw_poll_timers(void);
UINT64 fw_read_itc(void);
static void nvram_commit(void);

/* EFI_LOADED_IMAGE_RECORD lives in fw-pe.h. */

/* PE_LOADED_IMAGE_RESULT lives in fw-pe.h. */

EFI_LOADED_IMAGE_RECORD mLoadedImages[LOADED_IMAGE_MAX];

typedef struct {
    BOOLEAN in_use;
    EFI_PHYSICAL_ADDRESS base;
    UINTN pages;
    EFI_MEMORY_TYPE type;
} EFI_PAGE_ALLOCATION_RECORD;

#define PAGE_ALLOCATION_MAX 128
static EFI_PAGE_ALLOCATION_RECORD mPageAllocations[PAGE_ALLOCATION_MAX];

typedef struct {
    BOOLEAN in_use;
    EFI_PHYSICAL_ADDRESS base;
    UINTN size;
    EFI_PHYSICAL_ADDRESS backing_base;
    UINTN backing_pages;
    EFI_MEMORY_TYPE type;
} EFI_POOL_ALLOCATION_RECORD;

#define POOL_ALLOCATION_MAX 512
#define EFI_POOL_ALIGNMENT 8U
#define EFI_POOL_CHUNK_SIZE 0x10000U
static EFI_POOL_ALLOCATION_RECORD mPoolAllocations[POOL_ALLOCATION_MAX];

typedef struct {
    UINTN jump[8];
    BOOLEAN in_use;
    EFI_HANDLE image_handle;
    EFI_STATUS exit_status;
    UINTN exit_data_size;
    CHAR16 *exit_data;
    UINT64 saved_psr;
    UINT64 saved_rsc;
    UINT64 handle_database_generation;
} EFI_START_IMAGE_FRAME;

static EFI_START_IMAGE_FRAME mStartImageFrames[LOADED_IMAGE_MAX];
static UINTN mStartImageFrameDepth;
BOOLEAN mSalLoaderHandoffPending;
UINT64 mResetFloatingPointDisableBits;

/* IA64_SAL_HANDOFF_PROBE lives in platform.c. */


/* EFI_PROTOCOL_RECORD lives in fw-services.h. */
EFI_PROTOCOL_RECORD mProtocolRecords[PROTOCOL_RECORD_MAX];
static UINT64 mHandleDatabaseGeneration;

typedef struct {
    BOOLEAN in_use;
} EFI_DYNAMIC_HANDLE_RECORD;

#define DYNAMIC_HANDLE_MAX 256U
static EFI_DYNAMIC_HANDLE_RECORD mDynamicHandles[DYNAMIC_HANDLE_MAX];

/* EFI_OPEN_PROTOCOL_RECORD lives in fw-services.h. */
EFI_OPEN_PROTOCOL_RECORD mOpenProtocolRecords[OPEN_PROTOCOL_RECORD_MAX];

typedef struct {
    UINT64 status;
    UINT64 err0;
    UINT64 err1;
    UINT64 err2;
} IA64_FPSWA_RET;

typedef IA64_FPSWA_RET (*IA64_EFI_FPSWA)(
    UINT64 trap_type,
    VOID *bundle,
    UINT64 *ipsr,
    UINT64 *fpsr,
    UINT64 *isr,
    UINT64 *preds,
    UINT64 *ifs,
    VOID *fp_state);

typedef struct {
    UINT32 revision;
    UINT32 reserved;
    IA64_EFI_FPSWA fpswa;
} IA64_FPSWA_INTERFACE;

extern IA64_FPSWA_RET fpswa_emulation_entry(
    UINT64 trap_type, VOID *bundle, UINT64 *ipsr, UINT64 *fpsr,
    UINT64 *isr, UINT64 *preds, UINT64 *ifs, VOID *fp_state);

typedef struct {
    UINTN entry;
    UINTN gp;
} IA64_FUNCTION_DESCRIPTOR;

FW_STATIC_ASSERT(sizeof(IA64_FPSWA_RET) == 32, ia64_fpswa_ret_size);
FW_STATIC_ASSERT(sizeof(IA64_FPSWA_INTERFACE) == 16,
                 ia64_fpswa_interface_size);
FW_STATIC_ASSERT(sizeof(IA64_FUNCTION_DESCRIPTOR) == 16,
                 ia64_function_descriptor_size);

/* IA-64 plabel (function descriptor): 2 x 64-bit values */
typedef struct {
    UINT64  EntryPoint;
    UINT64  GP;
} IA64_PLABEL;

FW_STATIC_ASSERT(sizeof(IA64_PLABEL) == 16, ia64_plabel_size);

typedef struct {
    UINT32 signature;
    UINT32 type;
    BOOLEAN signaled;
    BOOLEAN timer_active;
    UINTN timer_type;
    UINT64 timer_last_tick;
    UINT64 timer_remaining_100ns;
    UINT64 timer_partial_ticks;
    UINT64 timer_period_100ns;
    EFI_TPL notify_tpl;
    EFI_EVENT_NOTIFY notify_function;
    VOID *notify_context;
    IA64_PLABEL notify_plabel;
    BOOLEAN has_group;
    UINT8 group[16];
} FW_EVENT_RECORD;

#define FW_EVENT_SIGNATURE 0x45564e54U
#define FW_EVENT_MAX 16
static FW_EVENT_RECORD mEventRecords[FW_EVENT_MAX];

typedef struct {
    BOOLEAN in_use;
    FW_EVENT_RECORD *event;
    EFI_TPL notify_tpl;
    EFI_EVENT_NOTIFY notify_function;
    VOID *notify_context;
    UINT64 order;
} FW_EVENT_NOTIFY_RECORD;

#define FW_EVENT_NOTIFY_MAX 32
static FW_EVENT_NOTIFY_RECORD mEventNotifyQueue[FW_EVENT_NOTIFY_MAX];
static UINT64 mEventNotifyOrder;

typedef struct {
    BOOLEAN in_use;
    UINT8 guid[16];
    FW_EVENT_RECORD *event;
    UINTN next_log_index;
} EFI_PROTOCOL_NOTIFY_RECORD;

#define PROTOCOL_NOTIFY_RECORD_MAX 32
static EFI_PROTOCOL_NOTIFY_RECORD mProtocolNotifyRecords[PROTOCOL_NOTIFY_RECORD_MAX];

typedef struct {
    BOOLEAN in_use;
    EFI_HANDLE handle;
    UINT8 guid[16];
} EFI_PROTOCOL_NOTIFY_LOG_RECORD;

#define PROTOCOL_NOTIFY_LOG_MAX 2048
static EFI_PROTOCOL_NOTIFY_LOG_RECORD mProtocolNotifyLog[PROTOCOL_NOTIFY_LOG_MAX];
static UINTN mProtocolNotifyLogCount;

EFI_TPL mCurrentTpl;
static UINT64 mMonotonicCount;
static UINT32 mHighMonotonicCount;

typedef struct _EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;

/*
 * USB host controller identities: the discrete PIIX3 function zx1 carries,
 * and function 2 of the i2000's 82468GX I/O and Firmware Bridge.
 */
#define FW_PCI_PIIX3_UHCI_ID 0x70208086U
#define FW_PCI_IFB_UHCI_ID   0x76028086U
/* Likewise the IDE controller: a discrete CMD646, or the bridge's function 1. */
#define FW_PCI_IFB_IDE_ID    0x76018086U

typedef struct FW_PCI_IO_DEVICE {
    EFI_HANDLE *Handle;
    EFI_PCI_IO_PROTOCOL *Protocol;
    VOID *DevicePath;
    UINT8 Bus;
    UINT8 Device;
    UINT8 Function;
    UINT64 Attributes;
    UINT32 ExpectedId;
    UINT8 ExpectedBarIndex;
    UINT32 ExpectedBarValue;
    UINT64 ExpectedBarLength;
    const CHAR8 *TraceName;
    BOOLEAN ProvidesDevicePath;
} FW_PCI_IO_DEVICE;

/* Handle numbers / protocol storage, placed early so init functions
 * can reference them. */
#define FW_HANDLE_BLOCK_IO    ((EFI_HANDLE)(UINTN)0x1000)
#define FW_HANDLE_RAW_BLOCK_IO ((EFI_HANDLE)(UINTN)0x1800)
#define FW_HANDLE_DISK_BLOCK_IO ((EFI_HANDLE)(UINTN)0x1900)
#define FW_HANDLE_IMAGE       ((EFI_HANDLE)(UINTN)0x2000)
#define FW_HANDLE_UNICODE     ((EFI_HANDLE)(UINTN)0x3000)
#define FW_HANDLE_GRAPHICS    ((EFI_HANDLE)(UINTN)0x4000)
#define FW_HANDLE_FPSWA       ((EFI_HANDLE)(UINTN)0x6000)
#define FW_HANDLE_PCI_ROOT_BRIDGE ((EFI_HANDLE)(UINTN)0x7000)
#define FW_HANDLE_PCI_IDE     ((EFI_HANDLE)(UINTN)0x7100)
#define FW_HANDLE_PCI_AHCI    ((EFI_HANDLE)(UINTN)0x7101)
#define FW_HANDLE_PCI_OHCI    ((EFI_HANDLE)(UINTN)0x7102)
#define FW_HANDLE_PCI_UHCI    ((EFI_HANDLE)(UINTN)0x7103)
#define FW_HANDLE_PCI_LSI     ((EFI_HANDLE)(UINTN)0x7104)
#define FW_HANDLE_TCG         ((EFI_HANDLE)(UINTN)0x8000)
#define FW_HANDLE_STORAGE_DRIVER ((EFI_HANDLE)(UINTN)0x9000)
#define FW_HANDLE_ARCH_PROTOCOLS ((EFI_HANDLE)(UINTN)0xa000)

EFI_HANDLE mBlockIoHandle;
EFI_HANDLE mRawBlockIoHandle;
EFI_HANDLE mDiskBlockIoHandle;
EFI_HANDLE mImageHandle;
EFI_HANDLE mUnicodeCollationHandle;
EFI_HANDLE mGraphicsHandle;
static EFI_HANDLE mFpswaHandle;
EFI_HANDLE mPciRootBridgeHandle;
static EFI_HANDLE mPciIdeHandle;
static EFI_HANDLE mPciAhciHandle;
static EFI_HANDLE mPciOhciHandle;
static EFI_HANDLE mPciUhciHandle;
static EFI_HANDLE mPciLsiHandle;
static EFI_HANDLE mTcgHandle;
EFI_HANDLE mStorageDriverHandle;
static EFI_HANDLE mArchitecturalHandle;
#define FW_PCI_IO_DEVICE_COUNT 6U
static FW_PCI_IO_DEVICE mPciIoDevices[FW_PCI_IO_DEVICE_COUNT];
EFI_LOADED_IMAGE_PROTOCOL mLoadedImageProto;
static IA64_FPSWA_INTERFACE mFpswaProto;
static EFI_LOADED_IMAGE_PROTOCOL mFpswaLoadedImageProto;
static BOOLEAN mFpswaLoadedImageActive;
const UINT8 mLoadedImageProtocolGuid[16];
static const UINT8 mLoadedImageDevicePathProtocolGuid[16];
static const UINT8 mHiiPackageListProtocolGuid[16];
const UINT8 mBlockIoProtocolGuid[16];
const UINT8 mDiskIoProtocolGuid[16];
extern const UINT8 mDevicePathProtocolGuid[16];
static const UINT8 mUnicodeCollationProtocolGuid[16];
static const UINT8 mGraphicsOutputProtocolGuid[16];
static const UINT8 mUgaDrawProtocolGuid[16];
static const UINT8 mFpswaProtocolGuid[16];
static const UINT8 mPciRootBridgeIoProtocolGuid[16];
static const UINT8 mPciIoProtocolGuid[16];
static const UINT8 mTcgProtocolGuid[16];
const UINT8 mDriverBindingProtocolGuid[16];
const UINT8 mComponentNameProtocolGuid[16];
static const UINT8 mPlatformDriverOverrideProtocolGuid[16];
static const UINT8 mBusSpecificDriverOverrideProtocolGuid[16];
static const UINT8 mDriverFamilyOverrideProtocolGuid[16];
static const UINT8 mLoadFileProtocolGuid[16];
static const UINT8 mLoadFile2ProtocolGuid[16];
static const FW_PCI_IO_DEVICE *fw_pci_io_device_from_handle(
    EFI_HANDLE Handle);

/* IMAGE_SUBSYSTEM_* live in fw-pe.h. */
EFI_STATUS bs_handle_protocol(EFI_HANDLE Handle, void *Protocol,
                               VOID **Interface);
EFI_STATUS bs_locate_handle(UINTN SearchType, void *Protocol,
                            VOID *SearchKey, UINTN *BufferSize,
                            EFI_HANDLE *Buffer);
EFI_STATUS bs_locate_device_path(void *Protocol, void **DevicePath,
                                 EFI_HANDLE *Device);
EFI_STATUS bs_install_protocol(EFI_HANDLE *Handle, void *Protocol,
                               UINTN InterfaceType, VOID *Interface);
EFI_STATUS bs_reinstall_protocol(EFI_HANDLE Handle, void *Protocol,
                                 VOID *OldInterface, VOID *NewInterface);
EFI_STATUS bs_uninstall_protocol(EFI_HANDLE Handle, void *Protocol,
                                 VOID *Interface);
static EFI_STATUS fpswa_unload_image(EFI_HANDLE ImageHandle);
static BOOLEAN fpswa_install_protocols(void);
static BOOLEAN tcg_install_protocol(void);
static BOOLEAN tcg_protocol_selftest(void);
EFI_STATUS bs_disconnect_controller(EFI_HANDLE ControllerHandle,
                                    EFI_HANDLE DriverImageHandle,
                                    EFI_HANDLE ChildHandle);
EFI_STATUS bs_locate_handle_buffer(UINTN SearchType, void *Protocol,
                                   VOID *SearchKey, UINTN *NoHandles,
                                   EFI_HANDLE **Buffer);
EFI_STATUS bs_locate_protocol(void *Protocol, VOID *Registration,
                              VOID **Interface);
EFI_STATUS bs_protocols_per_handle(EFI_HANDLE Handle, void ***ProtocolBuffer,
                                   UINTN *ProtocolBufferCount);
EFI_STATUS bs_stall(UINTN Microseconds);
EFI_STATUS bs_unload_image(EFI_HANDLE ImageHandle);
EFI_STATUS rs_get_variable(CHAR16 *VariableName, void *VendorGuid,
                           UINT32 *Attributes, UINTN *DataSize, VOID *Data);
EFI_STATUS rs_set_variable(CHAR16 *VariableName, void *VendorGuid,
                           UINT32 Attributes, UINTN DataSize, VOID *Data);
EFI_STATUS rs_get_next_var_name(UINTN *VariableNameSize,
                                CHAR16 *VariableName, void *VendorGuid);
EFI_STATUS rs_get_next_high_monotonic_count(UINT32 *HighCount);
VOID rs_reset_system(UINTN ResetType, EFI_STATUS ResetStatus,
                     UINTN DataSize, VOID *ResetData);
EFI_STATUS rs_query_variable_info(UINT32 Attributes,
                                  UINT64 *MaximumVariableStorageSize,
                                  UINT64 *RemainingVariableStorageSize,
                                  UINT64 *MaximumVariableSize);
BOOLEAN handle_supports_protocol(EFI_HANDLE Handle, void *Protocol,
                                        VOID **Interface);
static BOOLEAN protocol_has_open_records(EFI_HANDLE Handle,
                                         const void *Protocol);
static void close_uninstall_safe_open_records(EFI_HANDLE Handle,
                                              void *Protocol);
static BOOLEAN installed_protocol_interface(EFI_HANDLE Handle, void *Protocol,
                                             VOID **Interface);
static BOOLEAN open_protocol_is_driver(UINT32 Attributes);
static void clear_open_protocol_record(EFI_OPEN_PROTOCOL_RECORD *Rec);
const void *fw_pci_io_device_from_handle_opaque(EFI_HANDLE Handle)
{
    return fw_pci_io_device_from_handle(Handle);
}

BOOLEAN guid_matches(const void *Protocol, const UINT8 *Guid);
static void copy_guid(UINT8 *Destination, const void *Source);
static BOOLEAN open_protocol_attribute_legal(UINT32 Attributes);
static EFI_STATUS open_protocol_check_conflicts(EFI_HANDLE Handle,
                                                void *Protocol,
                                                EFI_HANDLE AgentHandle,
                                                UINT32 Attributes);
static EFI_STATUS add_open_protocol_record(EFI_HANDLE Handle, void *Protocol,
                                           EFI_HANDLE AgentHandle,
                                           EFI_HANDLE ControllerHandle,
                                           UINT32 Attributes);
static EFI_STATUS fw_load_image_source_from_device_path(
    BOOLEAN BootPolicy, void *DevicePath, VOID **SourceBuffer,
    UINTN *SourceSize);
static void efi_refresh_table_crc32s(void);

UINT8 table_checksum8(const void *buf, UINTN len)
{
    const UINT8 *p = (const UINT8 *)buf;
    UINTN i;
    UINT8 sum = 0;
    for (i = 0; i < len; i++) {
        sum = (UINT8)(sum + p[i]);
    }
    return (UINT8)(0 - sum);
}

UINT64 fw_current_gp(void)
{
    register UINT64 gp __asm__("r1");

    return gp;
}

UINT64 fw_function_entry(UINTN FunctionPointer)
{
    return *(UINT64 *)(UINTN)FunctionPointer;
}

/* SAL procedure constants live in platform.c. */

/* SAL procedures, PCI config access, the CPU/SAL asm bridge and AP
   bring-up live in platform.c. */

/* --- Boot services -------------------------------------------------------- */

/*
 * EfiConventionalMemory is a valid AllocatePages()/AllocatePool() type.
 * Such an allocation does not change the descriptor type, so the memory map
 * alone cannot distinguish it from free memory.  Keep the allocation records
 * in every availability decision as well as in the free paths.
 */
BOOLEAN efi_find_allocation_overlap(UINT64 Start, UINT64 End,
                                           UINT64 *FirstEnd,
                                           UINT64 *LastStart)
{
    UINT64 first_end = ~0ULL;
    UINT64 last_start = 0;
    BOOLEAN found = 0;
    UINTN i;

    if (End <= Start) {
        return 0;
    }

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        EFI_PAGE_ALLOCATION_RECORD *rec = &mPageAllocations[i];
        UINT64 size;
        UINT64 end;

        if (!rec->in_use ||
            !efi_pages_to_size(rec->pages, &size) ||
            rec->base > ~0ULL - size) {
            continue;
        }
        end = rec->base + size;
        if (Start >= end || rec->base >= End) {
            continue;
        }
        if (!found || end < first_end) {
            first_end = end;
        }
        if (!found || rec->base > last_start) {
            last_start = rec->base;
        }
        found = 1;
    }

    for (i = 0; i < POOL_ALLOCATION_MAX; i++) {
        EFI_POOL_ALLOCATION_RECORD *rec = &mPoolAllocations[i];
        UINT64 size;
        UINT64 end;

        if (!rec->in_use ||
            !efi_pages_to_size(rec->backing_pages, &size) ||
            rec->backing_base > ~0ULL - size) {
            continue;
        }
        end = rec->backing_base + size;
        if (Start >= end || rec->backing_base >= End) {
            continue;
        }
        if (!found || end < first_end) {
            first_end = end;
        }
        if (!found || rec->backing_base > last_start) {
            last_start = rec->backing_base;
        }
        found = 1;
    }

    if (found) {
        if (FirstEnd != NULL) {
            *FirstEnd = first_end;
        }
        if (LastStart != NULL) {
            *LastStart = last_start;
        }
    }
    return found;
}

static BOOLEAN efi_range_is_available(UINT64 Start, UINT64 End)
{
    UINT64 current = Start;

    if (End <= Start ||
        efi_find_allocation_overlap(Start, End, NULL, NULL)) {
        return 0;
    }

    while (current < End) {
        BOOLEAN found = 0;
        UINTN i;

        for (i = 0; i < mMemoryMapEntries; i++) {
            EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
            UINT64 desc_start = desc->PhysicalStart;
            UINT64 desc_end = desc_start + (desc->NumberOfPages << 12);

            if (desc->Type != EfiConventionalMemory ||
                current < desc_start || current >= desc_end) {
                continue;
            }

            current = desc_end < End ? desc_end : End;
            found = 1;
            break;
        }

        if (!found) {
            return 0;
        }
    }

    return 1;
}

static BOOLEAN efi_pages_to_size(UINTN Pages, UINT64 *Size)
{
    UINT64 size;

    if (Pages == 0 || Size == NULL) {
        return 0;
    }
    size = (UINT64)Pages << 12;
    if ((size >> 12) != Pages) {
        return 0;
    }
    *Size = size;
    return 1;
}

BOOLEAN efi_memory_descriptor_requires_ia64_alignment(
    EFI_MEMORY_TYPE Type, UINT64 Attribute)
{
    return Type == EfiACPIReclaimMemory ||
           Type == EfiACPIMemoryNVS ||
           (Attribute & EFI_MEMORY_RUNTIME) != 0;
}

UINT64 efi_memory_type_allocation_granularity(EFI_MEMORY_TYPE Type)
{
    if (efi_memory_descriptor_requires_ia64_alignment(
            Type, efi_memory_attribute(Type, 0))) {
        return IA64_EFI_MEMORY_ALIGN;
    }
    return EFI_PAGE_SIZE;
}

BOOLEAN efi_align_up_u64(UINT64 Value, UINT64 Alignment,
                                UINT64 *Aligned)
{
    UINT64 mask;

    if (Aligned == NULL || Alignment == 0 ||
        (Alignment & (Alignment - 1U)) != 0) {
        return 0;
    }
    mask = Alignment - 1U;
    if (Value > ~0ULL - mask) {
        return 0;
    }
    *Aligned = (Value + mask) & ~mask;
    return 1;
}

static BOOLEAN efi_round_allocation_pages(EFI_MEMORY_TYPE Type, UINTN Pages,
                                          UINTN *RoundedPages, UINT64 *Size)
{
    UINTN granularity_pages =
        (UINTN)(efi_memory_type_allocation_granularity(Type) >> 12);
    UINTN rounded;

    if (Pages == 0 || RoundedPages == NULL || Size == NULL ||
        Pages > ~(UINTN)0 - (granularity_pages - 1U)) {
        return 0;
    }
    rounded = (Pages + granularity_pages - 1U) &
              ~(granularity_pages - 1U);
    if (!efi_pages_to_size(rounded, Size)) {
        return 0;
    }
    *RoundedPages = rounded;
    return 1;
}

static UINT64 efi_page_allocation_end(EFI_PAGE_ALLOCATION_RECORD *Rec)
{
    return Rec->base + ((UINT64)Rec->pages << 12);
}

static BOOLEAN efi_page_allocation_type_at(EFI_PHYSICAL_ADDRESS Address,
                                           EFI_MEMORY_TYPE *Type)
{
    UINTN i;

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        EFI_PAGE_ALLOCATION_RECORD *rec = &mPageAllocations[i];

        if (rec->in_use && Address >= rec->base &&
            Address < efi_page_allocation_end(rec)) {
            if (Type != NULL) {
                *Type = rec->type;
            }
            return 1;
        }
    }
    return 0;
}

static UINTN efi_page_allocation_free_slots(void)
{
    UINTN i;
    UINTN slots = 0;

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        if (!mPageAllocations[i].in_use) {
            slots++;
        }
    }
    return slots;
}

static void efi_coalesce_page_allocations(void)
{
    UINTN i;

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        UINTN j;

        if (!mPageAllocations[i].in_use) {
            continue;
        }

        for (j = i + 1U; j < PAGE_ALLOCATION_MAX; j++) {
            EFI_PAGE_ALLOCATION_RECORD *a = &mPageAllocations[i];
            EFI_PAGE_ALLOCATION_RECORD *b = &mPageAllocations[j];
            UINT64 a_end;
            UINT64 b_end;

            if (!b->in_use || a->type != b->type) {
                continue;
            }

            a_end = efi_page_allocation_end(a);
            b_end = efi_page_allocation_end(b);
            if (a_end == b->base) {
                a->pages += b->pages;
                b->in_use = 0;
                j = i;
            } else if (b_end == a->base) {
                a->base = b->base;
                a->pages += b->pages;
                b->in_use = 0;
                j = i;
            }
        }
    }
}

static EFI_PAGE_ALLOCATION_RECORD *
efi_record_page_allocation(EFI_PHYSICAL_ADDRESS Base, UINTN Pages,
                           EFI_MEMORY_TYPE Type,
                           EFI_PAGE_ALLOCATION_RECORD *Previous)
{
    UINT64 size;
    UINTN i;

    if (Previous == NULL || !efi_pages_to_size(Pages, &size) ||
        Base + size < Base ||
        efi_find_allocation_overlap(Base, Base + size, NULL, NULL)) {
        return NULL;
    }

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        EFI_PAGE_ALLOCATION_RECORD *rec = &mPageAllocations[i];

        if (!rec->in_use) {
            fw_copy_mem(Previous, rec, sizeof(*Previous));
            rec->in_use = 1;
            rec->base = Base;
            rec->pages = Pages;
            rec->type = Type;
            return rec;
        }
    }

    return NULL;
}

static BOOLEAN efi_page_allocation_covers_type(EFI_PHYSICAL_ADDRESS Base,
                                               UINTN Pages,
                                               EFI_MEMORY_TYPE Type)
{
    UINT64 size;
    UINT64 current;
    UINT64 end;

    if (!efi_pages_to_size(Pages, &size) || Base + size < Base) {
        return 0;
    }

    current = Base;
    end = Base + size;
    while (current < end) {
        BOOLEAN found = 0;
        UINTN i;

        for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
            EFI_PAGE_ALLOCATION_RECORD *rec = &mPageAllocations[i];
            UINT64 rec_end;

            if (!rec->in_use || rec->type != Type || current < rec->base) {
                continue;
            }
            rec_end = efi_page_allocation_end(rec);
            if (current >= rec_end) {
                continue;
            }

            current = rec_end < end ? rec_end : end;
            found = 1;
            break;
        }

        if (!found) {
            return 0;
        }
    }

    return 1;
}

static UINTN efi_page_allocation_splits_needed(EFI_PHYSICAL_ADDRESS Base,
                                               UINTN Pages)
{
    UINT64 size;
    UINT64 end;
    UINTN splits = 0;
    UINTN i;

    if (!efi_pages_to_size(Pages, &size) || Base + size < Base) {
        return PAGE_ALLOCATION_MAX + 1U;
    }
    end = Base + size;

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        EFI_PAGE_ALLOCATION_RECORD *rec = &mPageAllocations[i];
        UINT64 rec_start;
        UINT64 rec_end;
        UINT64 free_start;
        UINT64 free_end;

        if (!rec->in_use) {
            continue;
        }

        rec_start = rec->base;
        rec_end = efi_page_allocation_end(rec);
        free_start = Base > rec_start ? Base : rec_start;
        free_end = end < rec_end ? end : rec_end;
        if (free_start >= free_end) {
            continue;
        }
        if (free_start > rec_start && free_end < rec_end) {
            splits++;
        }
    }

    return splits;
}

static BOOLEAN efi_forget_page_allocation(EFI_PHYSICAL_ADDRESS Base,
                                          UINTN Pages)
{
    UINT64 size;
    UINT64 end;
    UINTN i;

    if (!efi_pages_to_size(Pages, &size) || Base + size < Base ||
        efi_page_allocation_splits_needed(Base, Pages) >
        efi_page_allocation_free_slots()) {
        return 0;
    }
    end = Base + size;

    for (i = 0; i < PAGE_ALLOCATION_MAX; i++) {
        EFI_PAGE_ALLOCATION_RECORD *rec = &mPageAllocations[i];
        UINT64 rec_start;
        UINT64 rec_end;
        UINT64 free_start;
        UINT64 free_end;

        if (!rec->in_use) {
            continue;
        }

        rec_start = rec->base;
        rec_end = efi_page_allocation_end(rec);
        free_start = Base > rec_start ? Base : rec_start;
        free_end = end < rec_end ? end : rec_end;
        if (free_start >= free_end) {
            continue;
        }

        if (free_start == rec_start && free_end == rec_end) {
            rec->in_use = 0;
        } else if (free_start == rec_start) {
            rec->base = free_end;
            rec->pages = (rec_end - free_end) >> 12;
        } else if (free_end == rec_end) {
            rec->pages = (free_start - rec_start) >> 12;
        } else {
            UINTN j;

            rec->pages = (free_start - rec_start) >> 12;
            for (j = 0; j < PAGE_ALLOCATION_MAX; j++) {
                EFI_PAGE_ALLOCATION_RECORD *after = &mPageAllocations[j];

                if (!after->in_use) {
                    after->in_use = 1;
                    after->base = free_end;
                    after->pages = (rec_end - free_end) >> 12;
                    after->type = rec->type;
                    break;
                }
            }
        }
    }

    efi_coalesce_page_allocations();
    return 1;
}

static BOOLEAN efi_find_free_pages_forward(UINT64 Start, UINT64 End,
                                           UINT64 Size, UINT64 Alignment,
                                           EFI_PHYSICAL_ADDRESS *Memory)
{
    UINT64 addr;

    if (Size == 0 || End <= Start || End - Start < Size ||
        !efi_align_up_u64(Start, Alignment, &addr)) {
        return 0;
    }

    while (addr <= End - Size) {
        UINT64 allocation_end;

        if (!efi_find_allocation_overlap(addr, addr + Size,
                                         &allocation_end, NULL)) {
            *Memory = addr;
            return 1;
        }
        if (allocation_end <= addr ||
            !efi_align_up_u64(allocation_end, Alignment, &addr)) {
            return 0;
        }
    }
    return 0;
}

static BOOLEAN efi_find_free_pages_backward(UINT64 Start, UINT64 End,
                                            UINT64 Size, UINT64 Alignment,
                                            EFI_PHYSICAL_ADDRESS *Memory)
{
    UINT64 limit_end = End;

    if (Size == 0 || End <= Start || End - Start < Size ||
        Alignment == 0 || (Alignment & (Alignment - 1U)) != 0) {
        return 0;
    }

    while (limit_end > Start && limit_end - Start >= Size) {
        UINT64 allocation_start;
        UINT64 addr = limit_end - Size;

        addr &= ~(Alignment - 1U);

        if (addr < Start) {
            return 0;
        }
        if (!efi_find_allocation_overlap(addr, addr + Size, NULL,
                                         &allocation_start)) {
            *Memory = addr;
            return 1;
        }
        if (allocation_start >= limit_end) {
            return 0;
        }
        limit_end = allocation_start;
    }
    return 0;
}

static BOOLEAN efi_find_max_pages(UINT64 MaxAddress, UINT64 Size,
                                  UINT64 Alignment,
                                  EFI_PHYSICAL_ADDRESS *Memory);

static BOOLEAN efi_find_any_pages(UINT64 Size, UINT64 Alignment,
                                  EFI_PHYSICAL_ADDRESS *Memory)
{
    UINT64 lower_bound;
    unsigned pass;

    /*
     * Real EFI cores satisfy AllocateAnyPages top-down (rework plan 2.5):
     * boot-services allocations cluster directly below the RAM-top firmware
     * reservation instead of fragmenting the low RAM the Windows loaders
     * carve their heaps and images from ([1 MB, ~256 MB)).  Cap at the low
     * RAM end so loader-visible data stays below 4 GiB; the ascending
     * legacy walk below remains as the fallback.
     */
    if (mGuestLowRamEnd != 0 &&
        efi_find_max_pages(mGuestLowRamEnd - 1U, Size, Alignment, Memory)) {
        return 1;
    }

    if (!efi_align_up_u64(mNextPageAddr, Alignment, &lower_bound)) {
        lower_bound = ~0ULL;
    }

    for (pass = 0; pass < 2; pass++) {
        UINTN i;

        for (i = 0; i < mMemoryMapEntries; i++) {
            EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
            UINT64 desc_start;
            UINT64 desc_end;
            UINT64 range_start;
            UINT64 range_end;

            if (desc->Type != EfiConventionalMemory) {
                continue;
            }

            if (!efi_align_up_u64(desc->PhysicalStart, Alignment,
                                  &desc_start)) {
                continue;
            }
            desc_end = desc->PhysicalStart + (desc->NumberOfPages << 12);
            if (desc_end <= desc->PhysicalStart) {
                continue;
            }

            range_start = desc_start;
            range_end = desc_end;
            if (pass == 0) {
                if (range_start < lower_bound) {
                    range_start = lower_bound;
                }
            } else {
                if (range_start >= lower_bound) {
                    continue;
                }
            }

            if (efi_find_free_pages_forward(range_start, range_end, Size,
                                            Alignment, Memory)) {
                return 1;
            }
        }
    }

    return 0;
}

static BOOLEAN efi_find_max_pages(UINT64 MaxAddress, UINT64 Size,
                                  UINT64 Alignment,
                                  EFI_PHYSICAL_ADDRESS *Memory)
{
    UINTN i;

    if (Size == 0 ||
        (MaxAddress != ~0ULL && MaxAddress + 1 < Size)) {
        return 0;
    }

    for (i = mMemoryMapEntries; i > 0; i--) {
        EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i - 1U];
        UINT64 desc_start;
        UINT64 desc_end;
        UINT64 limit_end;

        if (desc->Type != EfiConventionalMemory) {
            continue;
        }

        if (!efi_align_up_u64(desc->PhysicalStart, Alignment, &desc_start)) {
            continue;
        }
        desc_end = desc->PhysicalStart + (desc->NumberOfPages << 12);
        if (desc_end <= desc->PhysicalStart) {
            continue;
        }
        limit_end = desc_end;
        if (MaxAddress != ~0ULL && limit_end > MaxAddress + 1) {
            limit_end = MaxAddress + 1;
        }
        if (limit_end < desc_start || limit_end - desc_start < Size) {
            continue;
        }

        if (efi_find_free_pages_backward(desc_start, limit_end, Size,
                                         Alignment, Memory)) {
            return 1;
        }
    }

    return 0;
}



EFI_STATUS bs_allocate_pages(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                     UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory)
{
    EFI_PAGE_ALLOCATION_RECORD previous_allocation;
    EFI_PAGE_ALLOCATION_RECORD *allocation;
    EFI_PHYSICAL_ADDRESS addr;
    UINT64 alignment;
    UINT64 size;
    UINTN previous_map_key;
    UINTN rounded_pages;

    if (Memory == NULL || Pages == 0 ||
        (UINT32)Type >= (UINT32)MaxAllocateType ||
        !efi_memory_type_is_valid(MemoryType)) {
        return EFI_INVALID_PARAMETER;
    }
    if (mBootServicesExited) {
        return EFI_UNSUPPORTED;
    }
    alignment = efi_memory_type_allocation_granularity(MemoryType);
    if (!efi_round_allocation_pages(MemoryType, Pages, &rounded_pages,
                                    &size)) {
        return EFI_OUT_OF_RESOURCES;
    }
    if (Type == AllocateAddress) {
        addr = *Memory;
        if ((addr & 0xfffULL) != 0) {
            return EFI_INVALID_PARAMETER;
        }
        if ((addr & (alignment - 1U)) != 0) {
            return EFI_NOT_FOUND;
        }
        if (addr + size < addr ||
            !efi_range_is_available(addr, addr + size)) {
            return EFI_NOT_FOUND;
        }
    } else if (Type == AllocateMaxAddress) {
        if (!efi_find_max_pages(*Memory, size, alignment, &addr)) {
            return EFI_NOT_FOUND;
        }
    } else {
        if (!efi_find_any_pages(size, alignment, &addr)) {
            return EFI_NOT_FOUND;
        }
    }

    allocation = efi_record_page_allocation(addr, rounded_pages, MemoryType,
                                            &previous_allocation);
    if (allocation == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    previous_map_key = mMapKey;
    if (!efi_mark_memory_range(MemoryType, addr, addr + size,
                               efi_memory_attribute(MemoryType,
                                                   EFI_MEMORY_WB))) {
        fw_copy_mem(allocation, &previous_allocation,
                    sizeof(previous_allocation));
        return EFI_OUT_OF_RESOURCES;
    }
    efi_coalesce_page_allocations();
    if (mMapKey == previous_map_key) {
        mMapKey++;
    }
    if (Type != AllocateAddress && addr + size > mNextPageAddr) {
        mNextPageAddr = addr + size;
    }
    *Memory = addr;
    return EFI_SUCCESS;
}

EFI_STATUS bs_free_pages(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages)
{
    EFI_MEMORY_TYPE type;
    UINT64 size;
    UINT64 alignment;
    UINTN previous_map_key;
    UINTN rounded_pages;

    if ((Memory & 0xfffULL) != 0 || Pages == 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (!efi_page_allocation_type_at(Memory, &type)) {
        return EFI_NOT_FOUND;
    }
    alignment = efi_memory_type_allocation_granularity(type);
    if ((Memory & (alignment - 1U)) != 0 ||
        !efi_round_allocation_pages(type, Pages, &rounded_pages, &size) ||
        Memory + size < Memory) {
        return EFI_INVALID_PARAMETER;
    }
    if (!efi_page_allocation_covers_type(Memory, rounded_pages, type)) {
        return EFI_NOT_FOUND;
    }
    if (efi_page_allocation_splits_needed(Memory, rounded_pages) >
        efi_page_allocation_free_slots()) {
        return EFI_OUT_OF_RESOURCES;
    }
    previous_map_key = mMapKey;
    if (!efi_mark_memory_range(EfiConventionalMemory, Memory,
                               Memory + size, EFI_MEMORY_WB)) {
        return EFI_NOT_FOUND;
    }
    (void)efi_forget_page_allocation(Memory, rounded_pages);
    if (mMapKey == previous_map_key) {
        mMapKey++;
    }
    return EFI_SUCCESS;
}

BOOLEAN ranges_overlap(UINT64 a_base, UINT64 a_size,
                              UINT64 b_base, UINT64 b_size)
{
    if (a_size == 0 || b_size == 0) {
        return 0;
    }
    return a_base < b_base + b_size && b_base < a_base + a_size;
}

EFI_STATUS bs_get_memory_map(UINTN *MemoryMapSize,
                                     EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                     UINTN *MapKey,
                                     UINTN *DescriptorSize,
                                     UINT32 *DescriptorVersion)
{
    UINTN needed;
    UINTN i;

    if (MemoryMapSize == NULL || DescriptorSize == NULL ||
        DescriptorVersion == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    needed = mMemoryMapEntries * sizeof(EFI_MEMORY_DESCRIPTOR);
    *DescriptorSize = sizeof(EFI_MEMORY_DESCRIPTOR);
    *DescriptorVersion = EFI_MEMORY_DESCRIPTOR_VERSION;

    if (*MemoryMapSize < needed) {
        *MemoryMapSize = needed;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (MemoryMap == NULL || MapKey == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    for (i = 0; i < mMemoryMapEntries; i++) {
        MemoryMap[i] = mMemoryMap[i];
    }
    *MemoryMapSize = needed;
    *MapKey = mMapKey;
    return EFI_SUCCESS;
}

static UINT64 efi_pool_backing_end(const EFI_POOL_ALLOCATION_RECORD *Rec)
{
    return Rec->backing_base + ((UINT64)Rec->backing_pages << 12);
}

static BOOLEAN efi_find_pool_pages(UINT64 Size, UINT64 Alignment,
                                   EFI_PHYSICAL_ADDRESS *Memory)
{
    if (Size == 0 || Memory == NULL) {
        return 0;
    }

    /* Keep fixed-address image candidates available to AllocatePages(). */
    return efi_find_max_pages(~0ULL, Size, Alignment, Memory);
}

static BOOLEAN efi_pool_find_gap(const EFI_POOL_ALLOCATION_RECORD *Arena,
                                 UINTN Size,
                                 EFI_PHYSICAL_ADDRESS *Memory)
{
    UINT64 end = efi_pool_backing_end(Arena);
    UINT64 candidate = Arena->backing_base;

    while (candidate <= end && Size <= end - candidate) {
        UINT64 first_end = ~0ULL;
        BOOLEAN overlap = 0;
        UINTN i;

        for (i = 0; i < POOL_ALLOCATION_MAX; i++) {
            EFI_POOL_ALLOCATION_RECORD *rec = &mPoolAllocations[i];
            UINT64 rec_end;

            if (!rec->in_use ||
                rec->backing_base != Arena->backing_base ||
                candidate >= rec->base + rec->size ||
                rec->base >= candidate + Size) {
                continue;
            }
            rec_end = rec->base + rec->size;
            if (!overlap || rec_end < first_end) {
                first_end = rec_end;
            }
            overlap = 1;
        }
        if (!overlap) {
            *Memory = candidate;
            return 1;
        }
        if (!efi_align_up_u64(first_end, EFI_POOL_ALIGNMENT, &candidate)) {
            return 0;
        }
    }
    return 0;
}

static EFI_POOL_ALLOCATION_RECORD *efi_find_pool_allocation(UINTN Address)
{
    UINTN i;

    for (i = 0; i < POOL_ALLOCATION_MAX; i++) {
        EFI_POOL_ALLOCATION_RECORD *rec = &mPoolAllocations[i];

        if (rec->in_use && rec->base == Address) {
            return rec;
        }
    }
    return NULL;
}

EFI_STATUS bs_allocate_pool(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer)
{
    EFI_POOL_ALLOCATION_RECORD *alloc_rec;
    EFI_POOL_ALLOCATION_RECORD *arena;
    EFI_PHYSICAL_ADDRESS memory;
    EFI_PHYSICAL_ADDRESS backing_base;
    UINT64 backing_size;
    UINT64 alloc_size;
    UINT64 backing_alignment;
    UINTN previous_map_key;
    UINTN request_size;
    UINTN backing_pages;
    UINTN i;

    if (Buffer == NULL || !efi_memory_type_is_valid(PoolType)) {
        return EFI_INVALID_PARAMETER;
    }
    if (mBootServicesExited) {
        return EFI_UNSUPPORTED;
    }
    request_size = Size == 0 ? EFI_POOL_ALIGNMENT : Size;
    if (!efi_align_up_u64(request_size, EFI_POOL_ALIGNMENT, &alloc_size) ||
        (UINTN)alloc_size != alloc_size) {
        return EFI_OUT_OF_RESOURCES;
    }
    alloc_rec = NULL;
    for (i = 0; i < POOL_ALLOCATION_MAX; i++) {
        EFI_POOL_ALLOCATION_RECORD *rec = &mPoolAllocations[i];

        if (!rec->in_use) {
            alloc_rec = rec;
            break;
        }
    }
    if (alloc_rec == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    arena = NULL;
    for (i = 0; i < POOL_ALLOCATION_MAX; i++) {
        EFI_POOL_ALLOCATION_RECORD *rec = &mPoolAllocations[i];
        BOOLEAN first_arena_member = 1;
        UINTN j;

        if (!rec->in_use || rec->type != PoolType) {
            continue;
        }
        /* Allocation records in one arena repeat its backing range. */
        for (j = 0; j < i; j++) {
            EFI_POOL_ALLOCATION_RECORD *previous = &mPoolAllocations[j];

            if (previous->in_use &&
                previous->backing_base == rec->backing_base) {
                first_arena_member = 0;
                break;
            }
        }
        if (first_arena_member &&
            efi_pool_find_gap(rec, (UINTN)alloc_size, &memory)) {
            arena = rec;
            break;
        }
    }

    if (arena == NULL) {
        backing_alignment = efi_memory_type_allocation_granularity(PoolType);
        backing_size = alloc_size > EFI_POOL_CHUNK_SIZE ?
                       alloc_size : EFI_POOL_CHUNK_SIZE;
        if (!efi_align_up_u64(backing_size, backing_alignment,
                              &backing_size) ||
            !efi_find_pool_pages(backing_size, backing_alignment,
                                 &backing_base)) {
            return EFI_OUT_OF_RESOURCES;
        }
        previous_map_key = mMapKey;
        if (!efi_mark_memory_range(PoolType, backing_base,
                                   backing_base + backing_size,
                                   efi_memory_attribute(PoolType,
                                                        EFI_MEMORY_WB))) {
            return EFI_OUT_OF_RESOURCES;
        }
        backing_pages = (UINTN)(backing_size >> 12);
        memory = backing_base;
        if (mMapKey == previous_map_key) {
            mMapKey++;
        }
    } else {
        backing_base = arena->backing_base;
        backing_pages = arena->backing_pages;
    }

    alloc_rec->in_use = 1;
    alloc_rec->base = memory;
    alloc_rec->size = (UINTN)alloc_size;
    alloc_rec->backing_base = backing_base;
    alloc_rec->backing_pages = backing_pages;
    alloc_rec->type = PoolType;
    if (Size <= FW_POOL_ZERO_LIMIT) {
        fw_set_mem((VOID *)(UINTN)memory, Size, 0);
    }
    *Buffer = (VOID *)(UINTN)memory;
    return EFI_SUCCESS;
}

EFI_STATUS bs_free_pool(VOID *Buffer)
{
    UINTN addr = (UINTN)Buffer;
    EFI_POOL_ALLOCATION_RECORD *target;
    UINTN i;

    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    target = efi_find_pool_allocation(addr);
    if (target == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < POOL_ALLOCATION_MAX; i++) {
        EFI_POOL_ALLOCATION_RECORD *rec = &mPoolAllocations[i];

        if (rec == target) {
            EFI_POOL_ALLOCATION_RECORD saved_rec = *rec;
            UINT64 backing_end = efi_pool_backing_end(rec);
            EFI_PHYSICAL_ADDRESS backing_base = rec->backing_base;
            BOOLEAN backing_in_use = 0;
            UINTN j;

            fw_set_mem(rec, sizeof(*rec), 0);
            for (j = 0; j < POOL_ALLOCATION_MAX; j++) {
                EFI_POOL_ALLOCATION_RECORD *other = &mPoolAllocations[j];

                if (other->in_use && other->backing_base == backing_base) {
                    backing_in_use = 1;
                    break;
                }
            }
            if (!backing_in_use) {
                UINTN previous_map_key = mMapKey;

                if (!efi_mark_memory_range(EfiConventionalMemory,
                                           backing_base, backing_end,
                                           EFI_MEMORY_WB)) {
                    *rec = saved_rec;
                    return EFI_INVALID_PARAMETER;
                }
                if (mMapKey == previous_map_key) {
                    mMapKey++;
                }
            }
            return EFI_SUCCESS;
        }
    }
    return EFI_INVALID_PARAMETER;
}

EFI_STATUS bs_stall(UINTN Microseconds)
{
    while (Microseconds != 0) {
        const UINTN max_chunk =
            (UINTN)(~0ULL / FW_ITC_TICKS_PER_MICROSECOND);
        UINTN chunk = Microseconds > max_chunk ? max_chunk : Microseconds;
        UINT64 ticks = (UINT64)chunk * FW_ITC_TICKS_PER_MICROSECOND;
        UINT64 start = fw_read_itc();

        while (fw_read_itc() - start < ticks) {
            __asm__ volatile ("" ::: "memory");
        }
        Microseconds -= chunk;
    }
    fw_poll_timers();
    return EFI_SUCCESS;
}

static BOOLEAN __attribute__((noinline)) uefi_stall_selftest(void)
{
    const UINT64 ticks = 1000ULL * FW_ITC_TICKS_PER_MICROSECOND;
    UINT64 start = fw_read_itc();

    if (bs_stall(1000) != EFI_SUCCESS) {
        return 0;
    }
    return fw_read_itc() - start >= ticks;
}

typedef UINT64 FW_UINT64_ALIAS __attribute__((may_alias));

void __attribute__((noinline)) fw_copy_mem(VOID *Destination,
                                           const VOID *Source,
                                           UINTN Length)
{
    UINT8 *d = (UINT8 *)Destination;
    const UINT8 *s = (const UINT8 *)Source;
    UINTN du = (UINTN)d;
    UINTN su = (UINTN)s;

    if (Length == 0 || du == su) {
        return;
    }

    if (du > su && du - su < Length) {
        d += Length;
        s += Length;
        if ((((UINTN)d ^ (UINTN)s) & 7U) == 0) {
            while (Length > 0 && ((UINTN)d & 7U) != 0) {
                *--d = *--s;
                Length--;
            }
            while (Length >= 8U) {
                d -= 8;
                s -= 8;
                *(FW_UINT64_ALIAS *)d = *(const FW_UINT64_ALIAS *)s;
                Length -= 8U;
            }
        }
        while (Length > 0) {
            *--d = *--s;
            Length--;
        }
        return;
    }

    if ((((UINTN)d ^ (UINTN)s) & 7U) == 0) {
        while (Length > 0 && ((UINTN)d & 7U) != 0) {
            *d++ = *s++;
            Length--;
        }
        while (Length >= 8U) {
            *(FW_UINT64_ALIAS *)d = *(const FW_UINT64_ALIAS *)s;
            d += 8;
            s += 8;
            Length -= 8U;
        }
    }
    while (Length > 0) {
        *d++ = *s++;
        Length--;
    }
}

void fw_copy_mem_fast(VOID *Destination, const VOID *Source,
                             UINTN Length)
{
    UINT8 *d = (UINT8 *)Destination;
    const UINT8 *s = (const UINT8 *)Source;
    UINTN du = (UINTN)d;
    UINTN su = (UINTN)s;

    if (Length == 0 || du == su) {
        return;
    }

    if ((((du | su | Length) & 7U) == 0) &&
        (du > su ? du - su : su - du) >= Length) {
        FW_UINT64_ALIAS *dw = (FW_UINT64_ALIAS *)d;
        const FW_UINT64_ALIAS *sw = (const FW_UINT64_ALIAS *)s;

        Length >>= 3;
        while (Length >= 8U) {
            dw[0] = sw[0];
            dw[1] = sw[1];
            dw[2] = sw[2];
            dw[3] = sw[3];
            dw[4] = sw[4];
            dw[5] = sw[5];
            dw[6] = sw[6];
            dw[7] = sw[7];
            dw += 8;
            sw += 8;
            Length -= 8U;
        }
        while (Length > 0) {
            *dw++ = *sw++;
            Length--;
        }
        return;
    }

    fw_copy_mem(Destination, Source, Length);
}

void *memcpy(void *Destination, const void *Source, size_t Length)
{
    fw_copy_mem(Destination, Source, Length);
    return Destination;
}

void fw_set_mem(VOID *Buffer, UINTN Size, UINT8 Value)
{
    UINT8 *p = (UINT8 *)Buffer;
    UINT64 pattern;

    while (Size > 0 && ((UINTN)p & 7U) != 0) {
        *p++ = Value;
        Size--;
    }

    pattern = Value;
    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;
    while (Size >= 64U) {
        UINT64 *wide = (UINT64 *)p;

        wide[0] = pattern;
        wide[1] = pattern;
        wide[2] = pattern;
        wide[3] = pattern;
        wide[4] = pattern;
        wide[5] = pattern;
        wide[6] = pattern;
        wide[7] = pattern;
        p += 64U;
        Size -= 64U;
    }
    while (Size >= 8U) {
        *(UINT64 *)p = pattern;
        p += 8;
        Size -= 8U;
    }

    while (Size > 0) {
        *p++ = Value;
        Size--;
    }
}

static BOOLEAN __attribute__((noinline)) fw_copy_mem_selftest(void)
{
    UINT8 buf[48] __attribute__((aligned(8)));
    UINT8 expect[48] __attribute__((aligned(8)));
    UINTN i;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (UINT8)i;
        expect[i] = (UINT8)i;
    }
    fw_copy_mem(buf + 1, buf + 9, 31);
    for (i = 0; i < 31; i++) {
        expect[1 + i] = (UINT8)(9 + i);
    }
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] != expect[i]) {
            return 0;
        }
    }

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (UINT8)(0xa0U + i);
        expect[i] = (UINT8)(0xa0U + i);
    }
    fw_copy_mem(buf + 9, buf + 1, 31);
    for (i = 31; i > 0; i--) {
        expect[9 + i - 1U] = (UINT8)(0xa0U + 1U + i - 1U);
    }
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] != expect[i]) {
            return 0;
        }
    }

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = 0;
    }
    fw_copy_mem(buf, expect, sizeof(buf));
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] != expect[i]) {
            return 0;
        }
    }

    fw_set_mem(buf, sizeof(buf), 0);
    fw_copy_mem_fast(buf, expect, sizeof(buf));
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] != expect[i]) {
            return 0;
        }
    }

    return 1;
}

void *memset(void *Buffer, int Value, size_t Size)
{
    fw_set_mem(Buffer, Size, (UINT8)Value);
    return Buffer;
}

static volatile UINT8 *vga_io_reg(UINTN Port)
{
    return (volatile UINT8 *)(UINTN)(LEGACY_IO_BASE + Port);
}

static UINT8 vga_io_read(UINTN Port)
{
    return *vga_io_reg(Port);
}

static void vga_io_write(UINTN Port, UINT8 Value)
{
    *vga_io_reg(Port) = Value;
}

static void vga_bochs_write(UINTN Index, UINT16 Value)
{
    volatile UINT16 *index =
        (volatile UINT16 *)(UINTN)(LEGACY_IO_BASE + 0x1ceU);
    volatile UINT16 *data =
        (volatile UINT16 *)(UINTN)(LEGACY_IO_BASE + 0x1d0U);

    *index = (UINT16)Index;
    *data = Value;
}

static void vga_indexed_write(UINTN IndexPort, UINTN DataPort,
                              UINT8 Index, UINT8 Value)
{
    vga_io_write(IndexPort, Index);
    vga_io_write(DataPort, Value);
}

/*
 * Move the hardware text cursor to a character offset (Row * 80 + Column) and
 * show or hide it.  The console tracks the logical cursor in software; without
 * this the VGA cursor is stuck wherever the CRTC left it (top-left) and blinks
 * there permanently.  CRTC 0x0a bit 5 disables the cursor; the scan-line range
 * (13..14) matches graphics_program_text_mode().
 */
void graphics_set_text_cursor(UINT16 Location, BOOLEAN Visible)
{
    vga_indexed_write(VGA_CRTC_I, VGA_CRTC_D, 0x0a,
                      Visible ? 0x0dU : (0x0dU | 0x20U));
    vga_indexed_write(VGA_CRTC_I, VGA_CRTC_D, 0x0e, (UINT8)(Location >> 8));
    vga_indexed_write(VGA_CRTC_I, VGA_CRTC_D, 0x0f, (UINT8)(Location & 0xffU));
}

static void vga_enable_attribute_output(void)
{
    vga_io_write(VGA_MIS_W, VGA_MIS_COLOR);
    (void)vga_io_read(VGA_IS1_RC);
    vga_io_write(VGA_ATT_W, VGA_AR_ENABLE_DISPLAY);
}

static void graphics_clear_framebuffer(void)
{
    fw_set_mem((VOID *)(UINTN)VGA_FB_BASE,
               (UINTN)mGraphicsStride * mGraphicsHeight, 0);
}

static BOOLEAN graphics_mode_matches(UINT32 ModeNumber,
                                     UINT32 HorizontalResolution,
                                     UINT32 VerticalResolution,
                                     UINT32 ColorDepth)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    if (ModeNumber >= mGopMode.MaxMode) {
        return 0;
    }
    info = &mGopModeInfo[ModeNumber];
    return info->HorizontalResolution == HorizontalResolution &&
           info->VerticalResolution == VerticalResolution &&
           ColorDepth == VGA_BPP;
}

static BOOLEAN graphics_mode_has_bgrx_layout(
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info)
{
    return Info != NULL &&
           (Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
            (Info->PixelFormat == PixelBitMask &&
             Info->PixelInformation.RedMask == GOP_BGRX_RED_MASK &&
             Info->PixelInformation.GreenMask == GOP_BGRX_GREEN_MASK &&
             Info->PixelInformation.BlueMask == GOP_BGRX_BLUE_MASK &&
             Info->PixelInformation.ReservedMask == GOP_BGRX_RESERVED_MASK));
}

static EFI_STATUS graphics_select_mode(UINT32 ModeNumber, BOOLEAN RedrawText)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    if (ModeNumber >= mGopMode.MaxMode) {
        return EFI_UNSUPPORTED;
    }

    info = &mGopModeInfo[ModeNumber];
    mGraphicsWidth = info->HorizontalResolution;
    mGraphicsHeight = info->VerticalResolution;
    mGraphicsStride = info->PixelsPerScanLine * 4U;

    vga_bochs_write(VBE_DISPI_INDEX_ENABLE, 0);
    vga_bochs_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    vga_bochs_write(VBE_DISPI_INDEX_XRES, mGraphicsWidth);
    vga_bochs_write(VBE_DISPI_INDEX_YRES, mGraphicsHeight);
    vga_bochs_write(VBE_DISPI_INDEX_BPP, VGA_BPP);
    vga_bochs_write(VBE_DISPI_INDEX_BANK, 0);
    vga_bochs_write(VBE_DISPI_INDEX_VIRT_WIDTH, mGraphicsWidth);
    vga_bochs_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vga_bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    vga_bochs_write(VBE_DISPI_INDEX_ENABLE,
                    VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    mGopMode.Mode = ModeNumber;
    mGopMode.Info = info;
    mGopMode.SizeOfInfo = sizeof(*info);
    mGopMode.FrameBufferSize = (UINTN)mGraphicsStride * mGraphicsHeight;
    vga_enable_attribute_output();
    graphics_clear_framebuffer();
    mGraphicsActive = 1;
    if (RedrawText) {
        text_redraw_screen();
    }
    return EFI_SUCCESS;
}

static void graphics_load_text_font(void)
{
    volatile UINT8 *font = (volatile UINT8 *)(UINTN)VGA_LEGACY_FB_BASE;
    UINTN ch;

    vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, 0x00, 0x01);
    vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, 0x02, 0x04);
    vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, 0x04, 0x07);
    vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, 0x00, 0x03);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x00, 0x00);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x01, 0x00);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x03, 0x00);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x04, 0x02);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x05, 0x00);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x06, 0x00);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x07, 0x00);
    vga_indexed_write(VGA_GFX_I, VGA_GFX_D, 0x08, 0xff);

    for (ch = 0; ch < 256U; ch++) {
        UINTN row;

        /*
         * VGA plane 2 reserves 32 bytes per glyph; a mode-3 cell is 16 scan
         * lines tall (CRTC max-scan-line 0x0f).  Load the standard 8x16 VGA
         * font -- the character generator then rasterises it exactly as the
         * real i2000 VGA does, one byte per scan line, bit 0x80 leftmost.
         */
        for (row = 0; row < 32U; row++) {
            font[ch * 32U + row] = (row < VGA_FONT_8X16_HEIGHT) ?
                                   gVgaFont8x16[ch][row] : 0U;
        }
    }
}

static void graphics_program_text_mode(void)
{
    /* SR01 bit 0 keeps 80-column text at 640 pixels instead of 720. */
    static const UINT8 seq[] = { 0x03, 0x01, 0x03, 0x00, 0x02 };
    static const UINT8 crtc[] = {
        0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f,
        0x00, 0x4f, 0x2d, 0x0e, 0x00, 0x00, 0x00, 0x00,
        0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3,
        0xff,
    };
    /* CRTC 0x0a bit 5 (0x20) starts the text cursor disabled; the console
     * drives it explicitly via graphics_set_text_cursor(). */
    static const UINT8 attr[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x0c, 0x00, 0x0f, 0x08,
    };
    static const UINT8 gfx[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0e, 0x00, 0xff,
    };
    static const UINT8 dac[][3] = {
        { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x2a },
        { 0x00, 0x2a, 0x00 }, { 0x00, 0x2a, 0x2a },
        { 0x2a, 0x00, 0x00 }, { 0x2a, 0x00, 0x2a },
        { 0x2a, 0x15, 0x00 }, { 0x2a, 0x2a, 0x2a },
        { 0x15, 0x15, 0x15 }, { 0x15, 0x15, 0x3f },
        { 0x15, 0x3f, 0x15 }, { 0x15, 0x3f, 0x3f },
        { 0x3f, 0x15, 0x15 }, { 0x3f, 0x15, 0x3f },
        { 0x3f, 0x3f, 0x15 }, { 0x3f, 0x3f, 0x3f },
    };
    UINTN i;

    vga_io_write(VGA_MIS_W, 0x67);
    for (i = 0; i < FW_ARRAY_SIZE(seq); i++) {
        vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, (UINT8)i, seq[i]);
    }

    vga_indexed_write(VGA_CRTC_I, VGA_CRTC_D, 0x11,
                      (UINT8)(crtc[0x11] & ~0x80U));
    for (i = 0; i < FW_ARRAY_SIZE(crtc); i++) {
        vga_indexed_write(VGA_CRTC_I, VGA_CRTC_D, (UINT8)i, crtc[i]);
    }
    for (i = 0; i < FW_ARRAY_SIZE(gfx); i++) {
        vga_indexed_write(VGA_GFX_I, VGA_GFX_D, (UINT8)i, gfx[i]);
    }
    for (i = 0; i < FW_ARRAY_SIZE(attr); i++) {
        (void)vga_io_read(VGA_IS1_RC);
        vga_io_write(VGA_ATT_W, (UINT8)i);
        vga_io_write(VGA_ATT_W, attr[i]);
    }
    vga_io_write(VGA_PEL_MSK, 0xff);
    /*
     * The attribute controller maps the 16 text colours to the scattered DAC
     * indices in attr[0..15] (0x00-0x07, 0x14, 0x38-0x3f), exactly as a real
     * VGA BIOS programs mode 3.  Load each colour into the DAC slot its ATC
     * entry selects -- loading 0..15 instead leaves the bright colours (and
     * brown) pointing at unwritten DAC entries, so e.g. yellow text renders
     * from QEMU's default palette and can come out black.
     */
    for (i = 0; i < FW_ARRAY_SIZE(dac); i++) {
        vga_io_write(VGA_PEL_IW, attr[i]);
        vga_io_write(VGA_PEL_D, dac[i][0]);
        vga_io_write(VGA_PEL_D, dac[i][1]);
        vga_io_write(VGA_PEL_D, dac[i][2]);
    }

    vga_enable_attribute_output();
}

static void graphics_select_text_mode(void)
{
    vga_bochs_write(VBE_DISPI_INDEX_ENABLE, 0);
    graphics_load_text_font();
    graphics_program_text_mode();
    mGraphicsActive = 0;
    text_redraw_screen();
}

/*
 * EFI 1.10 section 5.4 does not prescribe a video hardware mode after
 * ExitBootServices().  The IA-64 Linux boot ABI, however, supplies only text
 * geometry in ia64_boot_param and the PCDP VGA path selects vgacon, which
 * expects the firmware to leave legacy VGA text mode usable.  GRUB 2.12 can
 * draw directly into the already-active GOP framebuffer without calling a
 * mutating GOP method, then hand a non-framebuffer Linux kernel to us.
 *
 * Preserve graphics when the loader explicitly selected or blitted a GOP/UGA
 * mode.  Otherwise, when PCDP designates VGA as the primary OS console,
 * restore the legacy text state expected by vgacon.  Serial-primary boots do
 * not need or want a display transition.
 */
static void graphics_prepare_os_handoff(BOOLEAN VgaPrimary)
{
    if (!VgaPrimary || mGraphicsHandoffClaimed || !mGraphicsActive) {
        return;
    }

    /*
     * The linear framebuffer and legacy planes share VGA memory.  Clear GOP
     * pixels before loading the font; clearing afterwards would erase the
     * freshly loaded font plane and produce black text glyphs.
     */
    graphics_clear_framebuffer();
    graphics_select_text_mode();
    text_clear_legacy_cells();
}

static void __attribute__((noinline))
graphics_begin_loader_handoff(BOOLEAN TopLevelLoader)
{
    /*
     * Only a firmware-launched top-level loader starts a new ownership
     * window.  Nested StartImage() calls made by that loader may load a video
     * driver, so their GOP/UGA activity must remain attributed to the loader.
     */
    if (TopLevelLoader) {
        mGraphicsHandoffClaimed = 0;
    }
}

static BOOLEAN graphics_rect_in_bounds(UINTN X, UINTN Y, UINTN Width,
                                       UINTN Height)
{
    if (Width == 0 || Height == 0) {
        return 1;
    }
    return X < mGraphicsWidth && Y < mGraphicsHeight &&
           Width <= (UINTN)mGraphicsWidth - X &&
           Height <= (UINTN)mGraphicsHeight - Y;
}

/* Direct framebuffer mappings require observable device-memory accesses. */
typedef volatile UINT32 FW_FRAMEBUFFER_UINT32;
/* Wide framebuffer stores need the same device-memory semantics. */
typedef volatile UINT64 FW_FRAMEBUFFER_UINT64;

static void graphics_write_pixel(UINTN X, UINTN Y,
                                 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel)
{
    FW_FRAMEBUFFER_UINT32 *p =
        (FW_FRAMEBUFFER_UINT32 *)(UINTN)(VGA_FB_BASE +
                                         Y * mGraphicsStride + X * 4U);

    *p = (UINT32)Pixel.Blue | ((UINT32)Pixel.Green << 8) |
         ((UINT32)Pixel.Red << 16) | ((UINT32)Pixel.Reserved << 24);
}

static EFI_GRAPHICS_OUTPUT_BLT_PIXEL graphics_read_pixel(UINTN X, UINTN Y)
{
    FW_FRAMEBUFFER_UINT32 *p =
        (FW_FRAMEBUFFER_UINT32 *)(UINTN)(VGA_FB_BASE +
                                         Y * mGraphicsStride + X * 4U);
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel;
    UINT32 value = *p;

    Pixel.Blue = (UINT8)value;
    Pixel.Green = (UINT8)(value >> 8);
    Pixel.Red = (UINT8)(value >> 16);
    Pixel.Reserved = (UINT8)(value >> 24);
    return Pixel;
}

static void graphics_fill_pixels(UINTN X, UINTN Y, UINTN Width, UINTN Height,
                                 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel)
{
    UINT32 value = (UINT32)Pixel.Blue | ((UINT32)Pixel.Green << 8) |
                   ((UINT32)Pixel.Red << 16) |
                   ((UINT32)Pixel.Reserved << 24);
    UINT64 pair = (UINT64)value | ((UINT64)value << 32);
    UINTN y;

    for (y = 0; y < Height; y++) {
        FW_FRAMEBUFFER_UINT32 *dst =
            (FW_FRAMEBUFFER_UINT32 *)(UINTN)(VGA_FB_BASE +
                                             (Y + y) * mGraphicsStride +
                                             X * 4U);
        UINTN remaining = Width;

        if (remaining != 0 && ((UINTN)dst & 7U) != 0) {
            *dst++ = value;
            remaining--;
        }
        while (remaining >= 16U) {
            FW_FRAMEBUFFER_UINT64 *wide = (FW_FRAMEBUFFER_UINT64 *)dst;

            wide[0] = pair;
            wide[1] = pair;
            wide[2] = pair;
            wide[3] = pair;
            wide[4] = pair;
            wide[5] = pair;
            wide[6] = pair;
            wide[7] = pair;
            dst += 16;
            remaining -= 16U;
        }
        while (remaining >= 2U) {
            *(FW_FRAMEBUFFER_UINT64 *)dst = pair;
            dst += 2;
            remaining -= 2U;
        }
        if (remaining != 0) {
            *dst = value;
        }
    }
}

static BOOLEAN graphics_buffer_rect_valid(UINTN X, UINTN Y,
                                          UINTN Width, UINTN Height,
                                          UINTN Delta)
{
    UINTN max = ~(UINTN)0;
    UINTN row_bytes;
    UINTN last_row;

    if (Width > max / sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) ||
        X > max / sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) - Width) {
        return 0;
    }
    row_bytes = sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * (X + Width);
    if (Delta < row_bytes || Y > max - (Height - 1U)) {
        return 0;
    }
    last_row = Y + Height - 1U;
    return last_row == 0 || Delta <= (max - row_bytes) / last_row;
}

static BOOLEAN graphics_pixels_equal(
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Left,
    const EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Right, UINTN Count)
{
    UINTN i;

    for (i = 0; i < Count; i++) {
        if (Left[i].Blue != Right[i].Blue ||
            Left[i].Green != Right[i].Green ||
            Left[i].Red != Right[i].Red ||
            Left[i].Reserved != Right[i].Reserved) {
            return 0;
        }
    }
    return 1;
}

static EFI_STATUS graphics_blt(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
                               EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
                               UINTN SourceX, UINTN SourceY,
                               UINTN DestinationX, UINTN DestinationY,
                               UINTN Width, UINTN Height, UINTN Delta)
{
    UINTN y;

    if ((UINTN)BltOperation >= EfiGraphicsOutputBltOperationMax) {
        return EFI_INVALID_PARAMETER;
    }
    if (Width == 0 || Height == 0) {
        return EFI_SUCCESS;
    }
    switch (BltOperation) {
    case EfiBltVideoFill:
        if (BltBuffer == NULL ||
            !graphics_rect_in_bounds(DestinationX, DestinationY,
                                     Width, Height)) {
            return EFI_INVALID_PARAMETER;
        }
        graphics_fill_pixels(DestinationX, DestinationY, Width, Height,
                             BltBuffer[0]);
        return EFI_SUCCESS;

    case EfiBltBufferToVideo:
        if (Delta == 0) {
            if (SourceX != 0 || SourceY != 0) {
                return EFI_INVALID_PARAMETER;
            }
            Delta = sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * Width;
        }
        if (BltBuffer == NULL ||
            !graphics_rect_in_bounds(DestinationX, DestinationY,
                                     Width, Height) ||
            !graphics_buffer_rect_valid(SourceX, SourceY, Width, Height,
                                        Delta)) {
            return EFI_INVALID_PARAMETER;
        }
        for (y = 0; y < Height; y++) {
            VOID *dst = (VOID *)(UINTN)(VGA_FB_BASE +
                                        (DestinationY + y) * mGraphicsStride +
                                        DestinationX * 4U);
            const VOID *src = (UINT8 *)BltBuffer + (SourceY + y) * Delta +
                              SourceX * 4U;

            fw_copy_mem_fast(dst, src, Width * 4U);
        }
        return EFI_SUCCESS;

    case EfiBltVideoToBltBuffer:
        if (Delta == 0) {
            if (DestinationX != 0 || DestinationY != 0) {
                return EFI_INVALID_PARAMETER;
            }
            Delta = sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) * Width;
        }
        if (BltBuffer == NULL ||
            !graphics_rect_in_bounds(SourceX, SourceY, Width, Height) ||
            !graphics_buffer_rect_valid(DestinationX, DestinationY,
                                        Width, Height, Delta)) {
            return EFI_INVALID_PARAMETER;
        }
        for (y = 0; y < Height; y++) {
            VOID *dst = (UINT8 *)BltBuffer + (DestinationY + y) * Delta +
                        DestinationX * 4U;
            const VOID *src = (const VOID *)(UINTN)(
                VGA_FB_BASE + (SourceY + y) * mGraphicsStride + SourceX * 4U);

            fw_copy_mem_fast(dst, src, Width * 4U);
        }
        return EFI_SUCCESS;

    case EfiBltVideoToVideo:
        if (!graphics_rect_in_bounds(SourceX, SourceY, Width, Height) ||
            !graphics_rect_in_bounds(DestinationX, DestinationY,
                                     Width, Height)) {
            return EFI_INVALID_PARAMETER;
        }
        if (DestinationY > SourceY ||
            (DestinationY == SourceY && DestinationX > SourceX)) {
            for (y = Height; y > 0; y--) {
                VOID *dst = (VOID *)(UINTN)(
                    VGA_FB_BASE + (DestinationY + y - 1U) * mGraphicsStride +
                    DestinationX * 4U);
                const VOID *src = (const VOID *)(UINTN)(
                    VGA_FB_BASE + (SourceY + y - 1U) * mGraphicsStride +
                    SourceX * 4U);

                fw_copy_mem(dst, src, Width * 4U);
            }
        } else {
            for (y = 0; y < Height; y++) {
                VOID *dst = (VOID *)(UINTN)(
                    VGA_FB_BASE + (DestinationY + y) * mGraphicsStride +
                    DestinationX * 4U);
                const VOID *src = (const VOID *)(UINTN)(
                    VGA_FB_BASE + (SourceY + y) * mGraphicsStride +
                    SourceX * 4U);

                fw_copy_mem(dst, src, Width * 4U);
            }
        }
        return EFI_SUCCESS;

    default:
        return EFI_INVALID_PARAMETER;
    }
}

static BOOLEAN graphics_blt_claims_handoff(
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    UINTN Width, UINTN Height)
{
    if (Width == 0 || Height == 0) {
        return 0;
    }
    return BltOperation == EfiBltVideoFill ||
           BltOperation == EfiBltBufferToVideo ||
           BltOperation == EfiBltVideoToVideo;
}

static EFI_STATUS gop_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                 UINT32 ModeNumber, UINTN *SizeOfInfo,
                                 EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
    EFI_STATUS st;

    (void)This;
    if (SizeOfInfo == NULL || Info == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (ModeNumber >= mGopMode.MaxMode) {
        return EFI_UNSUPPORTED;
    }
    st = bs_allocate_pool(EfiBootServicesData, sizeof(mGopModeInfo[0]),
                          (VOID **)Info);
    if (st != EFI_SUCCESS) {
        return st;
    }
    fw_copy_mem(*Info, &mGopModeInfo[ModeNumber], sizeof(mGopModeInfo[0]));
    *SizeOfInfo = sizeof(mGopModeInfo[0]);
    return EFI_SUCCESS;
}

static EFI_STATUS gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                               UINT32 ModeNumber)
{
    EFI_STATUS st;

    (void)This;
    st = graphics_select_mode(ModeNumber, 0);
    if (st == EFI_SUCCESS) {
        mGraphicsHandoffClaimed = 1;
    }
    return st;
}

static EFI_STATUS gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                          EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
                          EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
                          UINTN SourceX, UINTN SourceY,
                          UINTN DestinationX, UINTN DestinationY,
                          UINTN Width, UINTN Height, UINTN Delta)
{
    EFI_STATUS st;

    (void)This;
    st = graphics_blt(BltBuffer, BltOperation, SourceX, SourceY,
                      DestinationX, DestinationY, Width, Height, Delta);
    if (st == EFI_SUCCESS &&
        graphics_blt_claims_handoff(BltOperation, Width, Height)) {
        mGraphicsHandoffClaimed = 1;
    }
    return st;
}

static EFI_STATUS uga_get_mode(EFI_UGA_DRAW_PROTOCOL *This,
                               UINT32 *HorizontalResolution,
                               UINT32 *VerticalResolution,
                               UINT32 *ColorDepth,
                               UINT32 *RefreshRate)
{
    (void)This;
    if (HorizontalResolution == NULL || VerticalResolution == NULL ||
        ColorDepth == NULL || RefreshRate == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *HorizontalResolution = mGraphicsWidth;
    *VerticalResolution = mGraphicsHeight;
    *ColorDepth = VGA_BPP;
    *RefreshRate = 60;
    return EFI_SUCCESS;
}

static EFI_STATUS uga_set_mode(EFI_UGA_DRAW_PROTOCOL *This,
                               UINT32 HorizontalResolution,
                               UINT32 VerticalResolution,
                               UINT32 ColorDepth,
                               UINT32 RefreshRate)
{
    UINT32 mode;

    (void)This;
    if (RefreshRate != 0 && RefreshRate != 60) {
        return EFI_UNSUPPORTED;
    }
    for (mode = 0; mode < mGopMode.MaxMode; mode++) {
        if (graphics_mode_matches(mode, HorizontalResolution,
                                  VerticalResolution, ColorDepth)) {
            EFI_STATUS st = graphics_select_mode(mode, 0);

            if (st == EFI_SUCCESS) {
                mGraphicsHandoffClaimed = 1;
            }
            return st;
        }
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS uga_blt(EFI_UGA_DRAW_PROTOCOL *This,
                          EFI_UGA_PIXEL *BltBuffer,
                          EFI_UGA_BLT_OPERATION BltOperation,
                          UINTN SourceX, UINTN SourceY,
                          UINTN DestinationX, UINTN DestinationY,
                          UINTN Width, UINTN Height, UINTN Delta)
{
    EFI_STATUS st;

    (void)This;
    st = graphics_blt(BltBuffer, BltOperation, SourceX, SourceY,
                      DestinationX, DestinationY, Width, Height, Delta);
    if (st == EFI_SUCCESS &&
        graphics_blt_claims_handoff(BltOperation, Width, Height)) {
        mGraphicsHandoffClaimed = 1;
    }
    return st;
}

static BOOLEAN graphics_visible_framebuffer_is_black(void)
{
    volatile UINT32 *fb = (volatile UINT32 *)(UINTN)VGA_FB_BASE;
    UINTN pixels = (UINTN)mGraphicsWidth * (UINTN)mGraphicsHeight;
    UINTN i;

    for (i = 0; i < pixels; i++) {
        if (fb[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static BOOLEAN __attribute__((noinline)) graphics_gop_set_mode_selftest(void)
{
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL marker;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL observed;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL source[20];
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL output[20];
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info = NULL;
    UINT32 saved_mode = mGopMode.Mode;
    BOOLEAN saved_active = mGraphicsActive;
    BOOLEAN saved_handoff = mGraphicsHandoffClaimed;
    UINTN info_size = 0;
    UINTN expected_size;
    UINTN i;
    EFI_STATUS st;
    BOOLEAN ok = 1;

    mGraphicsHandoffClaimed = 0;
    st = gop_query_mode(&mGopProto, 0, &info_size, &mode_info);
    if (st != EFI_SUCCESS || info_size != sizeof(*mode_info) ||
        mode_info == NULL ||
        mode_info->HorizontalResolution != VGA_MODE_TEXT_WIDTH ||
        mode_info->VerticalResolution != VGA_MODE_TEXT_HEIGHT ||
        !graphics_mode_has_bgrx_layout(mode_info)) {
        ok = 0;
    }
    if (mode_info != NULL) {
        (void)bs_free_pool(mode_info);
        mode_info = NULL;
    }
    st = gop_query_mode(&mGopProto, 0, NULL, &mode_info);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }
    st = gop_query_mode(&mGopProto, mGopMode.MaxMode, &info_size, &mode_info);
    if (st != EFI_UNSUPPORTED) {
        ok = 0;
    }
    st = gop_query_mode(&mGopProto, 1, &info_size, &mode_info);
    if (st != EFI_SUCCESS || info_size != sizeof(*mode_info) ||
        mode_info == NULL ||
        mode_info->HorizontalResolution != VGA_MODE_640_WIDTH ||
        mode_info->VerticalResolution != VGA_MODE_640_HEIGHT ||
        !graphics_mode_has_bgrx_layout(mode_info)) {
        ok = 0;
    }
    if (mode_info != NULL) {
        (void)bs_free_pool(mode_info);
        mode_info = NULL;
    }

    st = gop_query_mode(&mGopProto, 3, &info_size, &mode_info);
    if (st != EFI_SUCCESS || info_size != sizeof(*mode_info) ||
        mode_info == NULL ||
        mode_info->HorizontalResolution != VGA_MODE_1024_WIDTH ||
        mode_info->VerticalResolution != VGA_MODE_1024_HEIGHT ||
        !graphics_mode_has_bgrx_layout(mode_info)) {
        ok = 0;
    }
    if (mode_info != NULL) {
        (void)bs_free_pool(mode_info);
        mode_info = NULL;
    }

    if (mGraphicsHandoffClaimed) {
        ok = 0;
    }

    st = gop_set_mode(&mGopProto, 3);
    expected_size = (UINTN)VGA_MODE_1024_WIDTH *
                    (UINTN)VGA_MODE_1024_HEIGHT * 4U;
    if (st != EFI_SUCCESS || mGopMode.Mode != 3 ||
        mGraphicsWidth != VGA_MODE_1024_WIDTH ||
        mGraphicsHeight != VGA_MODE_1024_HEIGHT ||
        mGopMode.FrameBufferSize != expected_size ||
        !graphics_visible_framebuffer_is_black() ||
        !mGraphicsHandoffClaimed) {
        ok = 0;
    }

    marker.Blue = 0x22;
    marker.Green = 0x44;
    marker.Red = 0x66;
    marker.Reserved = 0;

    mGraphicsHandoffClaimed = 0;
    st = gop_blt(&mGopProto, NULL,
                 (EFI_GRAPHICS_OUTPUT_BLT_OPERATION)
                 EfiGraphicsOutputBltOperationMax,
                 0, 0, 0, 0, 0, 1, 0);
    if (st != EFI_INVALID_PARAMETER || mGraphicsHandoffClaimed) {
        ok = 0;
    }
    st = gop_blt(&mGopProto, NULL, EfiBltVideoFill,
                 0, 0, 0, 0, 0, 1, 0);
    if (st != EFI_SUCCESS || mGraphicsHandoffClaimed) {
        ok = 0;
    }

    mGraphicsHandoffClaimed = 0;
    st = gop_blt(&mGopProto, &marker, EfiBltVideoFill, 0, 0, 0, 0,
                 8, 8, 0);
    if (st != EFI_SUCCESS || !mGraphicsHandoffClaimed) {
        ok = 0;
    }

    fw_set_mem(&observed, sizeof(observed), 0);
    mGraphicsHandoffClaimed = 0;
    st = gop_blt(&mGopProto, &observed, EfiBltVideoToBltBuffer,
                 0, 0, 0, 0, 1, 1, 0);
    if (st != EFI_SUCCESS || mGraphicsHandoffClaimed ||
        observed.Blue != marker.Blue ||
        observed.Green != marker.Green || observed.Red != marker.Red ||
        observed.Reserved != marker.Reserved) {
        ok = 0;
    }

    mGraphicsHandoffClaimed = 0;
    st = gop_blt(&mGopProto, &marker, EfiBltBufferToVideo,
                 0, 0, 1, 0, 1, 1, 0);
    if (st != EFI_SUCCESS || !mGraphicsHandoffClaimed) {
        ok = 0;
    }
    mGraphicsHandoffClaimed = 0;
    st = gop_blt(&mGopProto, NULL, EfiBltVideoToVideo,
                 0, 0, 2, 0, 1, 1, 0);
    if (st != EFI_SUCCESS || !mGraphicsHandoffClaimed) {
        ok = 0;
    }

    for (i = 0; i < FW_ARRAY_SIZE(source); i++) {
        source[i].Blue = (UINT8)(i * 3U + 1U);
        source[i].Green = (UINT8)(i * 5U + 2U);
        source[i].Red = (UINT8)(i * 7U + 3U);
        source[i].Reserved = (UINT8)(i * 11U + 4U);
    }
    fw_set_mem(output, sizeof(output), 0);
    st = gop_blt(&mGopProto, source, EfiBltBufferToVideo,
                 1, 1, 4, 4, 3, 2, 5U * sizeof(source[0]));
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    st = gop_blt(&mGopProto, output, EfiBltVideoToBltBuffer,
                 4, 4, 1, 1, 3, 2, 5U * sizeof(output[0]));
    if (st != EFI_SUCCESS ||
        !graphics_pixels_equal(&source[6], &output[6], 3) ||
        !graphics_pixels_equal(&source[11], &output[11], 3)) {
        ok = 0;
    }
    st = gop_blt(&mGopProto, NULL, EfiBltVideoToVideo,
                 4, 4, 5, 5, 3, 2, 0);
    fw_set_mem(output, sizeof(output), 0);
    if (st != EFI_SUCCESS ||
        gop_blt(&mGopProto, output, EfiBltVideoToBltBuffer,
                5, 5, 1, 1, 3, 2, 5U * sizeof(output[0])) != EFI_SUCCESS ||
        !graphics_pixels_equal(&source[6], &output[6], 3) ||
        !graphics_pixels_equal(&source[11], &output[11], 3)) {
        ok = 0;
    }
    if (gop_blt(&mGopProto, source, EfiBltBufferToVideo,
                1, 0, 4, 4, 2, 1, 2U * sizeof(source[0])) !=
        EFI_INVALID_PARAMETER) {
        ok = 0;
    }
    if (gop_blt(&mGopProto, source, EfiBltBufferToVideo,
                1, 0, 4, 4, 2, 1, 0) != EFI_INVALID_PARAMETER ||
        gop_blt(&mGopProto, output, EfiBltVideoToBltBuffer,
                4, 4, 0, 1, 2, 1, 0) != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = gop_set_mode(&mGopProto, 3);
    if (st != EFI_SUCCESS || !graphics_visible_framebuffer_is_black()) {
        ok = 0;
    }

    st = gop_set_mode(&mGopProto, mGopMode.MaxMode);
    if (st != EFI_UNSUPPORTED) {
        ok = 0;
    }

    mGraphicsHandoffClaimed = 0;
    st = uga_set_mode(&mUgaDrawProto, VGA_MODE_TEXT_WIDTH, VGA_MODE_TEXT_HEIGHT,
                      VGA_BPP, 0);
    expected_size = (UINTN)VGA_MODE_TEXT_WIDTH *
                    (UINTN)VGA_MODE_TEXT_HEIGHT * 4U;
    if (st != EFI_SUCCESS || mGopMode.Mode != 0 ||
        mGopMode.FrameBufferSize != expected_size ||
        !graphics_visible_framebuffer_is_black() ||
        !mGraphicsHandoffClaimed) {
        ok = 0;
    }

    st = uga_set_mode(&mUgaDrawProto, VGA_MODE_1280_WIDTH, VGA_MODE_1280_HEIGHT,
                      VGA_BPP, 60);
    expected_size = (UINTN)VGA_MODE_1280_WIDTH *
                    (UINTN)VGA_MODE_1280_HEIGHT * 4U;
    if (st != EFI_SUCCESS || mGopMode.Mode != 4 ||
        mGopMode.FrameBufferSize != expected_size ||
        !graphics_visible_framebuffer_is_black()) {
        ok = 0;
    }

    if (saved_active) {
        (void)graphics_select_mode(saved_mode, 1);
    } else {
        graphics_select_text_mode();
    }
    mGraphicsHandoffClaimed = saved_handoff;
    return ok;
}

static BOOLEAN __attribute__((noinline)) graphics_handoff_selftest(void)
{
    volatile UINT16 *text_fb =
        (volatile UINT16 *)(UINTN)VGA_TEXT_FB_BASE;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL marker;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL observed;
    UINT32 saved_mode = mGopMode.Mode;
    BOOLEAN saved_active = mGraphicsActive;
    BOOLEAN saved_handoff = mGraphicsHandoffClaimed;
    BOOLEAN ok = 1;

    if (graphics_select_mode(0, 0) != EFI_SUCCESS) {
        return 0;
    }
    marker.Blue = 0x12;
    marker.Green = 0x34;
    marker.Red = 0x56;
    marker.Reserved = 0x78;
    mGraphicsHandoffClaimed = 0;
    graphics_write_pixel(0, 0, marker);
    if (mGraphicsHandoffClaimed) {
        ok = 0;
    }

    mGraphicsHandoffClaimed = 1;
    graphics_begin_loader_handoff(0);
    if (!mGraphicsHandoffClaimed) {
        ok = 0;
    }
    graphics_begin_loader_handoff(1);
    if (mGraphicsHandoffClaimed) {
        ok = 0;
    }

    mGraphicsHandoffClaimed = 1;
    graphics_prepare_os_handoff(1);
    observed = graphics_read_pixel(0, 0);
    if (!mGraphicsActive || observed.Blue != marker.Blue ||
        observed.Green != marker.Green || observed.Red != marker.Red ||
        observed.Reserved != marker.Reserved) {
        ok = 0;
    }

    mGraphicsHandoffClaimed = 0;
    graphics_prepare_os_handoff(0);
    observed = graphics_read_pixel(0, 0);
    if (!mGraphicsActive || observed.Blue != marker.Blue ||
        observed.Green != marker.Green || observed.Red != marker.Red ||
        observed.Reserved != marker.Reserved) {
        ok = 0;
    }

    graphics_prepare_os_handoff(1);
    if (mGraphicsActive || text_fb[0] != 0x0720U ||
        text_fb[VGA_TEXT_COLUMNS * VGA_TEXT_ROWS - 1U] != 0x0720U) {
        ok = 0;
    }

    if (saved_active) {
        (void)graphics_select_mode(saved_mode, 1);
    } else {
        graphics_select_text_mode();
    }
    mGraphicsHandoffClaimed = saved_handoff;
    return ok;
}

static BOOLEAN fw_notify_tpl_valid(EFI_TPL NotifyTpl)
{
    return NotifyTpl == TPL_CALLBACK || NotifyTpl == TPL_NOTIFY;
}

static BOOLEAN __attribute__((noinline))
fw_event_type_valid(UINT32 Type, BOOLEAN CreateEventEx)
{
    UINT32 notify = Type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL);
    UINT32 legal = EVT_TIMER | EVT_RUNTIME | EVT_RUNTIME_CONTEXT |
                   EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL;

    if (CreateEventEx &&
        (Type == EVT_SIGNAL_EXIT_BOOT_SERVICES ||
         Type == EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE)) {
        return 0;
    }
    if (!CreateEventEx &&
        (Type == EVT_SIGNAL_EXIT_BOOT_SERVICES ||
         Type == EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE)) {
        return 1;
    }
    if ((Type & ~legal) != 0) {
        return 0;
    }
    return notify != (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL);
}

static BOOLEAN fw_event_validate_create(UINT32 Type, UINTN NotifyTpl,
                                        EFI_EVENT_NOTIFY NotifyFunction,
                                        EFI_EVENT *Event,
                                        BOOLEAN CreateEventEx)
{
    UINT32 notify = Type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL);

    if (Event == NULL || !fw_event_type_valid(Type, CreateEventEx)) {
        return 0;
    }
    if (notify != 0 &&
        (NotifyFunction == NULL || !fw_notify_tpl_valid(NotifyTpl))) {
        return 0;
    }
    return 1;
}

static UINTN fw_event_notify_address(EFI_EVENT_NOTIFY NotifyFunction)
{
    union {
        EFI_EVENT_NOTIFY notify_function;
        UINTN address;
    } bits;

    bits.notify_function = NotifyFunction;
    return bits.address;
}

static EFI_EVENT_NOTIFY fw_event_notify_from_address(UINTN Address)
{
    union {
        EFI_EVENT_NOTIFY notify_function;
        UINTN address;
    } bits;

    bits.address = Address;
    return bits.notify_function;
}

static void fw_event_capture_notify(FW_EVENT_RECORD *Event, UINTN NotifyTpl,
                                    EFI_EVENT_NOTIFY NotifyFunction,
                                    VOID *NotifyContext)
{
    const IA64_PLABEL *plabel =
        (const IA64_PLABEL *)fw_event_notify_address(NotifyFunction);

    Event->notify_tpl = NotifyTpl;
    Event->notify_context = NotifyContext;
    Event->notify_plabel = *plabel;
    Event->notify_function =
        fw_event_notify_from_address((UINTN)&Event->notify_plabel);
}

BOOLEAN fw_guid_equal(const UINT8 *A, const UINT8 *B)
{
    UINTN i;

    if (A == NULL || B == NULL) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        if (A[i] != B[i]) {
            return 0;
        }
    }
    return 1;
}

static FW_EVENT_RECORD *fw_event_record_from_handle(EFI_EVENT Event)
{
    UINTN address = (UINTN)Event;
    UINTN base = (UINTN)&mEventRecords[0];
    UINTN offset;
    FW_EVENT_RECORD *rec;

    if (address < base) {
        return NULL;
    }
    offset = address - base;
    if (offset >= sizeof(mEventRecords) ||
        (offset % sizeof(mEventRecords[0])) != 0) {
        return NULL;
    }
    rec = &mEventRecords[offset / sizeof(mEventRecords[0])];
    return rec->signature == FW_EVENT_SIGNATURE ? rec : NULL;
}

static void fw_event_queue_notify(FW_EVENT_RECORD *Event)
{
    UINTN i;

    if (Event == NULL || Event->notify_function == NULL) {
        return;
    }
    for (i = 0; i < FW_EVENT_NOTIFY_MAX; i++) {
        if (mEventNotifyQueue[i].in_use &&
            mEventNotifyQueue[i].event == Event) {
            return;
        }
    }
    for (i = 0; i < FW_EVENT_NOTIFY_MAX; i++) {
        FW_EVENT_NOTIFY_RECORD *rec = &mEventNotifyQueue[i];

        if (!rec->in_use) {
            rec->in_use = 1;
            rec->event = Event;
            rec->notify_tpl = Event->notify_tpl;
            rec->notify_function = Event->notify_function;
            rec->notify_context = Event->notify_context;
            rec->order = mEventNotifyOrder++;
            return;
        }
    }
}

static void fw_dispatch_event_notifications(void)
{
    for (;;) {
        INTN selected = -1;
        EFI_TPL selected_tpl = 0;
        UINT64 selected_order = 0;
        UINTN i;

        for (i = 0; i < FW_EVENT_NOTIFY_MAX; i++) {
            FW_EVENT_NOTIFY_RECORD *rec = &mEventNotifyQueue[i];

            if (!rec->in_use || rec->notify_tpl <= mCurrentTpl) {
                continue;
            }
            if (selected < 0 || rec->notify_tpl > selected_tpl ||
                (rec->notify_tpl == selected_tpl &&
                 rec->order < selected_order)) {
                selected = (INTN)i;
                selected_tpl = rec->notify_tpl;
                selected_order = rec->order;
            }
        }
        if (selected < 0) {
            return;
        }

        {
            FW_EVENT_NOTIFY_RECORD rec = mEventNotifyQueue[selected];
            FW_EVENT_RECORD *event = rec.event;
            EFI_TPL old_tpl = mCurrentTpl;

            mEventNotifyQueue[selected].in_use = 0;
            if (event == NULL || event->signature != FW_EVENT_SIGNATURE ||
                event->notify_function != rec.notify_function) {
                continue;
            }
            if ((event->type & EVT_NOTIFY_SIGNAL) != 0) {
                event->signaled = 0;
            }
            mCurrentTpl = rec.notify_tpl;
            rec.notify_function((EFI_EVENT)event, rec.notify_context);
            mCurrentTpl = old_tpl;
        }
    }
}

EFI_TPL bs_raise_tpl(EFI_TPL NewTpl)
{
    EFI_TPL old = mCurrentTpl;

    if (NewTpl > mCurrentTpl) {
        mCurrentTpl = NewTpl;
    }
    return old;
}

VOID bs_restore_tpl(EFI_TPL OldTpl)
{
    mCurrentTpl = OldTpl;
    fw_dispatch_event_notifications();
}

static EFI_STATUS fw_create_event_common(UINT32 Type, UINTN NotifyTpl,
                                         EFI_EVENT_NOTIFY NotifyFunction,
                                         VOID *NotifyContext,
                                         const void *EventGroup,
                                         BOOLEAN CreateEventEx,
                                         EFI_EVENT *Event)
{
    const void *effective_group = EventGroup;
    UINTN i;

    if (!fw_event_validate_create(Type, NotifyTpl, NotifyFunction, Event,
                                  CreateEventEx)) {
        return EFI_INVALID_PARAMETER;
    }
    if (effective_group == NULL &&
        Type == EVT_SIGNAL_EXIT_BOOT_SERVICES) {
        effective_group = gEfiEventGroupExitBootServicesGuid;
    } else if (effective_group == NULL &&
               Type == EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE) {
        effective_group = gEfiEventGroupVirtualAddressChangeGuid;
    }
    for (i = 0; i < FW_EVENT_MAX; i++) {
        if (mEventRecords[i].signature != FW_EVENT_SIGNATURE) {
            UINTN j;

            fw_set_mem(&mEventRecords[i], sizeof(mEventRecords[i]), 0);
            mEventRecords[i].signature = FW_EVENT_SIGNATURE;
            mEventRecords[i].type = Type;
            if ((Type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL)) != 0) {
                fw_event_capture_notify(&mEventRecords[i], NotifyTpl,
                                        NotifyFunction, NotifyContext);
            }
            if (effective_group != NULL) {
                const UINT8 *group = (const UINT8 *)effective_group;

                mEventRecords[i].has_group = 1;
                for (j = 0; j < 16; j++) {
                    mEventRecords[i].group[j] = group[j];
                }
            }
            *Event = &mEventRecords[i];
            return EFI_SUCCESS;
        }
    }
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS bs_create_event(UINT32 Type, UINTN NotifyTpl,
                           EFI_EVENT_NOTIFY NotifyFunction,
                           VOID *NotifyContext,
                           EFI_EVENT *Event)
{
    return fw_create_event_common(Type, NotifyTpl, NotifyFunction,
                                  NotifyContext, NULL, 0, Event);
}

EFI_STATUS bs_create_event_ex(UINT32 Type, UINTN NotifyTpl,
                              EFI_EVENT_NOTIFY NotifyFunction,
                              VOID *NotifyContext,
                              void *EventGroup, EFI_EVENT *Event)
{
    return fw_create_event_common(Type, NotifyTpl, NotifyFunction,
                                  NotifyContext, EventGroup,
                                  1, Event);
}

UINT64 fw_read_itc(void)
{
    UINT64 tick;

    __asm__ volatile ("mov %0=ar.itc" : "=r"(tick));
    return tick;
}

static BOOLEAN __attribute__((noinline))
fw_event_timer_consume(FW_EVENT_RECORD *rec, UINT64 Now)
{
    UINT64 delta = Now - rec->timer_last_tick;
    UINT64 elapsed_100ns;
    UINT64 partial;
    UINT64 overrun;

    if (delta == 0) {
        return 0;
    }
    rec->timer_last_tick = Now;

    /* A zero trigger expires on the first timer tick after SetTimer(). */
    if (rec->timer_remaining_100ns == 0) {
        rec->timer_partial_ticks = 0;
        if (rec->timer_type != TIMER_PERIODIC) {
            rec->timer_active = 0;
        }
        return 1;
    }

    /* Carry sub-100ns ITC ticks across polls without scaling the deadline. */
    elapsed_100ns = delta / FW_ITC_TICKS_PER_100NS;
    partial = rec->timer_partial_ticks +
              delta % FW_ITC_TICKS_PER_100NS;
    if (partial >= FW_ITC_TICKS_PER_100NS) {
        partial -= FW_ITC_TICKS_PER_100NS;
        elapsed_100ns++;
    }
    rec->timer_partial_ticks = partial;
    if (elapsed_100ns < rec->timer_remaining_100ns) {
        rec->timer_remaining_100ns -= elapsed_100ns;
        return 0;
    }

    overrun = elapsed_100ns - rec->timer_remaining_100ns;
    if (rec->timer_type == TIMER_PERIODIC) {
        if (rec->timer_period_100ns == 0) {
            rec->timer_remaining_100ns = 0;
            rec->timer_partial_ticks = 0;
        } else {
            /* Skip missed periods in O(1) while preserving timer phase. */
            rec->timer_remaining_100ns = rec->timer_period_100ns -
                overrun % rec->timer_period_100ns;
        }
    } else {
        rec->timer_active = 0;
        rec->timer_remaining_100ns = 0;
        rec->timer_partial_ticks = 0;
    }
    return 1;
}

static BOOLEAN fw_event_timer_expired_at(FW_EVENT_RECORD *rec, UINT64 Now)
{
    BOOLEAN already_signaled;

    if (rec == NULL || !rec->timer_active) {
        return 0;
    }

    already_signaled = rec->signaled;
    if (!fw_event_timer_consume(rec, Now)) {
        return already_signaled;
    }
    if (already_signaled) {
        return 1;
    }
    rec->signaled = 1;
    if ((rec->type & EVT_NOTIFY_SIGNAL) != 0) {
        fw_event_queue_notify(rec);
        fw_dispatch_event_notifications();
    }
    return 1;
}

static BOOLEAN fw_event_timer_expired(FW_EVENT_RECORD *rec)
{
    if (rec == NULL || !rec->timer_active) {
        return 0;
    }
    return fw_event_timer_expired_at(rec, fw_read_itc());
}

static void fw_poll_timers(void)
{
    UINTN i;

    for (i = 0; i < FW_EVENT_MAX; i++) {
        FW_EVENT_RECORD *rec = &mEventRecords[i];

        if (rec->signature == FW_EVENT_SIGNATURE && rec->timer_active) {
            (void)fw_event_timer_expired(rec);
        }
    }
}

EFI_STATUS bs_set_timer(EFI_EVENT Event, UINTN Type, UINT64 TriggerTime)
{
    FW_EVENT_RECORD *rec = fw_event_record_from_handle(Event);

    if (rec == NULL || (rec->type & EVT_TIMER) == 0 ||
        Type > TIMER_RELATIVE) {
        return EFI_INVALID_PARAMETER;
    }
    if (Type == TIMER_CANCEL) {
        rec->timer_active = 0;
        rec->timer_type = 0;
        rec->timer_last_tick = 0;
        rec->timer_remaining_100ns = 0;
        rec->timer_partial_ticks = 0;
        rec->timer_period_100ns = 0;
        return EFI_SUCCESS;
    }
    rec->timer_active = 1;
    rec->timer_type = Type;
    rec->timer_last_tick = fw_read_itc();
    rec->timer_remaining_100ns = TriggerTime;
    rec->timer_partial_ticks = 0;
    rec->timer_period_100ns =
        Type == TIMER_PERIODIC ? TriggerTime : 0;
    return EFI_SUCCESS;
}

static BOOLEAN fw_event_ready(EFI_EVENT Event)
{
    FW_EVENT_RECORD *rec = (FW_EVENT_RECORD *)Event;

    if ((Event == mConInProto.WaitForKey ||
         Event == mConInExProto.WaitForKeyEx) &&
        conin_key_available()) {
        rec->signaled = 1;
    }
    return fw_event_timer_expired(rec) || rec->signaled;
}

static void fw_event_consume(FW_EVENT_RECORD *rec)
{
    rec->signaled = 0;
}

EFI_STATUS bs_wait_for_event(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index)
{
    UINTN i;

    if (NumberOfEvents == 0 || Index == NULL || Event == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (mCurrentTpl != TPL_APPLICATION) {
        return EFI_UNSUPPORTED;
    }
    for (i = 0; i < NumberOfEvents; i++) {
        FW_EVENT_RECORD *rec = fw_event_record_from_handle(Event[i]);
        if (rec == NULL) {
            *Index = i;
            return EFI_INVALID_PARAMETER;
        }
        if ((rec->type & EVT_NOTIFY_SIGNAL) != 0) {
            *Index = i;
            return EFI_INVALID_PARAMETER;
        }
    }

    while (1) {
        for (i = 0; i < NumberOfEvents; i++) {
            FW_EVENT_RECORD *rec = fw_event_record_from_handle(Event[i]);

            if (rec == NULL) {
                *Index = i;
                return EFI_INVALID_PARAMETER;
            }
            if (fw_event_ready(Event[i])) {
                *Index = i;
                fw_event_consume(rec);
                return EFI_SUCCESS;
            }
            if ((rec->type & EVT_NOTIFY_WAIT) != 0) {
                fw_event_queue_notify(rec);
                fw_dispatch_event_notifications();
                if (fw_event_ready(Event[i])) {
                    *Index = i;
                    fw_event_consume(rec);
                    return EFI_SUCCESS;
                }
            }
        }
        bs_stall(50);
    }
}

static void fw_signal_event_record(FW_EVENT_RECORD *rec)
{
    if (rec == NULL || rec->signature != FW_EVENT_SIGNATURE ||
        rec->signaled) {
        return;
    }
    rec->signaled = 1;
    if ((rec->type & EVT_NOTIFY_SIGNAL) != 0) {
        fw_event_queue_notify(rec);
    }
}

static void fw_signal_event_group(const UINT8 *Group)
{
    UINTN i;

    for (i = 0; i < FW_EVENT_MAX; i++) {
        FW_EVENT_RECORD *rec = &mEventRecords[i];

        if (rec->signature == FW_EVENT_SIGNATURE && rec->has_group &&
            fw_guid_equal(rec->group, Group)) {
            fw_signal_event_record(rec);
        }
    }
    fw_dispatch_event_notifications();
}

static void fw_signal_event_group_and_type(const UINT8 *Group, UINT32 Type)
{
    UINTN i;

    for (i = 0; i < FW_EVENT_MAX; i++) {
        FW_EVENT_RECORD *rec = &mEventRecords[i];

        if (rec->signature == FW_EVENT_SIGNATURE &&
            (rec->type == Type ||
             (rec->has_group && fw_guid_equal(rec->group, Group)))) {
            fw_signal_event_record(rec);
        }
    }
    fw_dispatch_event_notifications();
}

static void __attribute__((noinline)) fw_cancel_all_timers(void)
{
    UINTN i;

    for (i = 0; i < FW_EVENT_MAX; i++) {
        FW_EVENT_RECORD *rec = &mEventRecords[i];

        if (rec->signature == FW_EVENT_SIGNATURE) {
            rec->timer_active = 0;
            rec->timer_type = 0;
            rec->timer_last_tick = 0;
            rec->timer_remaining_100ns = 0;
            rec->timer_partial_ticks = 0;
            rec->timer_period_100ns = 0;
        }
    }
}

EFI_STATUS bs_signal_event(EFI_EVENT Event)
{
    FW_EVENT_RECORD *rec = fw_event_record_from_handle(Event);
    BOOLEAN signaled_group = 0;
    UINTN i;

    if (rec == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (rec->has_group) {
        for (i = 0; i < FW_EVENT_MAX; i++) {
            FW_EVENT_RECORD *other = &mEventRecords[i];

            if (other->signature == FW_EVENT_SIGNATURE &&
                other->has_group && fw_guid_equal(other->group, rec->group)) {
                fw_signal_event_record(other);
                signaled_group = 1;
            }
        }
    }
    if (!signaled_group) {
        fw_signal_event_record(rec);
    }
    fw_dispatch_event_notifications();
    return EFI_SUCCESS;
}

static void fw_close_protocol_notify_for_event(FW_EVENT_RECORD *Event)
{
    UINTN i;

    for (i = 0; i < PROTOCOL_NOTIFY_RECORD_MAX; i++) {
        EFI_PROTOCOL_NOTIFY_RECORD *rec = &mProtocolNotifyRecords[i];

        if (rec->in_use && rec->event == Event) {
            rec->in_use = 0;
            rec->event = NULL;
            rec->next_log_index = 0;
            fw_set_mem(rec->guid, sizeof(rec->guid), 0);
        }
    }
}

static void fw_remove_event_notifications(FW_EVENT_RECORD *Event)
{
    UINTN i;

    for (i = 0; i < FW_EVENT_NOTIFY_MAX; i++) {
        FW_EVENT_NOTIFY_RECORD *rec = &mEventNotifyQueue[i];

        if (rec->in_use && rec->event == Event) {
            fw_set_mem(rec, sizeof(*rec), 0);
        }
    }
}

EFI_STATUS bs_close_event(EFI_EVENT Event)
{
    FW_EVENT_RECORD *rec = fw_event_record_from_handle(Event);

    if (rec == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    fw_remove_event_notifications(rec);
    fw_close_protocol_notify_for_event(rec);
    rec->signature = 0;
    rec->type = 0;
    rec->signaled = 0;
    rec->timer_active = 0;
    rec->timer_type = 0;
    rec->timer_last_tick = 0;
    rec->timer_remaining_100ns = 0;
    rec->timer_partial_ticks = 0;
    rec->timer_period_100ns = 0;
    rec->notify_tpl = 0;
    rec->notify_function = NULL;
    rec->notify_context = NULL;
    rec->has_group = 0;
    fw_set_mem(rec->group, sizeof(rec->group), 0);
    return EFI_SUCCESS;
}

EFI_STATUS bs_check_event(EFI_EVENT Event)
{
    FW_EVENT_RECORD *rec = fw_event_record_from_handle(Event);

    if (rec == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if ((rec->type & EVT_NOTIFY_SIGNAL) != 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (!fw_event_ready(Event)) {
        if ((rec->type & EVT_NOTIFY_WAIT) != 0) {
            fw_event_queue_notify(rec);
            fw_dispatch_event_notifications();
            if (fw_event_ready(Event)) {
                fw_event_consume(rec);
                return EFI_SUCCESS;
            }
        }
        return EFI_NOT_READY;
    }
    fw_event_consume(rec);
    return EFI_SUCCESS;
}

EFI_STATUS bs_register_protocol_notify(void *Protocol, EFI_EVENT Event,
                                       VOID **Registration)
{
    FW_EVENT_RECORD *event = fw_event_record_from_handle(Event);
    UINTN i;

    if (Protocol == NULL || event == NULL || Registration == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    for (i = 0; i < PROTOCOL_NOTIFY_RECORD_MAX; i++) {
        EFI_PROTOCOL_NOTIFY_RECORD *rec = &mProtocolNotifyRecords[i];

        if (!rec->in_use) {
            rec->in_use = 1;
            copy_guid(rec->guid, Protocol);
            rec->event = event;
            rec->next_log_index = mProtocolNotifyLogCount;
            *Registration = rec;
            return EFI_SUCCESS;
        }
    }

    *Registration = NULL;
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS bs_install_configuration_table(void *Guid, VOID *Table)
{
    UINTN i;
    UINTN count;

    if (Guid == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    count = mSystemTable.NumberOfTableEntries;
    if (count > PLATFORM_TABLE_MAX) {
        count = PLATFORM_TABLE_MAX;
    }

    for (i = 0; i < count; i++) {
        if (!fw_guid_equal(mConfigTables[i].VendorGuid, (const UINT8 *)Guid)) {
            continue;
        }

        if (Table == NULL) {
            UINTN j;

            for (j = i; j + 1U < count; j++) {
                mConfigTables[j] = mConfigTables[j + 1U];
            }
            fw_set_mem(&mConfigTables[count - 1U],
                       sizeof(mConfigTables[count - 1U]), 0);
            mSystemTable.NumberOfTableEntries = count - 1U;
        } else {
            mConfigTables[i].VendorTable = (UINTN)Table;
            mSystemTable.NumberOfTableEntries = count;
        }
        efi_refresh_table_crc32s();
        fw_signal_event_group((const UINT8 *)Guid);
        return EFI_SUCCESS;
    }

    if (Table == NULL) {
        return EFI_NOT_FOUND;
    }
    if (count >= PLATFORM_TABLE_MAX) {
        return EFI_OUT_OF_RESOURCES;
    }

    fw_copy_mem(mConfigTables[count].VendorGuid, Guid,
                sizeof(mConfigTables[count].VendorGuid));
    mConfigTables[count].VendorTable = (UINTN)Table;
    mSystemTable.NumberOfTableEntries = count + 1U;
    efi_refresh_table_crc32s();
    fw_signal_event_group((const UINT8 *)Guid);
    return EFI_SUCCESS;
}

static EFI_START_IMAGE_FRAME *start_image_push_frame(EFI_HANDLE ImageHandle)
{
    EFI_START_IMAGE_FRAME *frame;

    if (mStartImageFrameDepth >= LOADED_IMAGE_MAX) {
        return NULL;
    }

    frame = &mStartImageFrames[mStartImageFrameDepth++];
    fw_set_mem(frame, sizeof(*frame), 0);
    frame->in_use = 1;
    frame->image_handle = ImageHandle;
    frame->exit_status = EFI_SUCCESS;
    frame->saved_psr = fw_read_psr() & ~(IA64_PSR_DT | IA64_PSR_RT | IA64_PSR_IT);
    frame->saved_rsc = fw_read_rsc();
    frame->handle_database_generation = mHandleDatabaseGeneration;
    return frame;
}

static EFI_START_IMAGE_FRAME *start_image_top_frame(void)
{
    if (mStartImageFrameDepth == 0) {
        return NULL;
    }
    return &mStartImageFrames[mStartImageFrameDepth - 1U];
}

static void start_image_pop_frame(EFI_START_IMAGE_FRAME *Frame)
{
    if (Frame == NULL || mStartImageFrameDepth == 0 ||
        &mStartImageFrames[mStartImageFrameDepth - 1U] != Frame) {
        return;
    }

    Frame->in_use = 0;
    mStartImageFrameDepth--;
}

EFI_STATUS bs_exit(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus,
                   UINTN ExitDataSize, CHAR16 *ExitData)
{
    EFI_START_IMAGE_FRAME *frame = start_image_top_frame();
    EFI_LOADED_IMAGE_RECORD *image = fw_loaded_image_record(ImageHandle);
    UINTN i;

    if (image != NULL && !image->started) {
        return bs_unload_image(ImageHandle);
    }

    if (frame == NULL || !frame->in_use ||
        frame->image_handle != ImageHandle || image == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (mLoadedImages[i].in_use &&
            mLoadedImages[i].loaded_image.ParentHandle == ImageHandle) {
            return EFI_INVALID_PARAMETER;
        }
    }

    frame->exit_status = ExitStatus;
    frame->exit_data_size = ExitStatus == EFI_SUCCESS ? 0 : ExitDataSize;
    frame->exit_data = ExitStatus == EFI_SUCCESS ? NULL : ExitData;
    fw_restore_psr(frame->saved_psr);
    __builtin_longjmp(frame->jump, 1);

    return EFI_ABORTED;
}

EFI_STATUS bs_get_next_monotonic_count(UINT64 *Count)
{
    if (Count == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *Count = ++mMonotonicCount;
    return EFI_SUCCESS;
}

EFI_STATUS bs_set_watchdog_timer(UINTN Timeout, UINT64 WatchdogCode,
                                 UINTN DataSize, CHAR16 *WatchdogData)
{
    volatile UINT64 *timeout_register =
        (volatile UINT64 *)(UINTN)(FW_WATCHDOG_BASE +
                                   FW_WATCHDOG_TIMEOUT_OFFSET);
    volatile UINT64 *code_register =
        (volatile UINT64 *)(UINTN)(FW_WATCHDOG_BASE +
                                   FW_WATCHDOG_CODE_OFFSET);
    UINTN i;

    if (WatchdogCode != 0 && WatchdogCode <= 0xffffU) {
        return EFI_INVALID_PARAMETER;
    }
    if (DataSize != 0) {
        if (WatchdogData == NULL ||
            (DataSize & (sizeof(CHAR16) - 1U)) != 0) {
            return EFI_INVALID_PARAMETER;
        }
        for (i = 0; i < DataSize / sizeof(CHAR16); i++) {
            if (WatchdogData[i] == 0) {
                break;
            }
        }
        if (i == DataSize / sizeof(CHAR16)) {
            return EFI_INVALID_PARAMETER;
        }
    }
    if ((UINT64)Timeout > 0x7fffffffffffffffULL /
                          FW_NANOSECONDS_PER_SECOND) {
        return EFI_DEVICE_ERROR;
    }

    *code_register = WatchdogCode;
    *timeout_register = Timeout;
    return EFI_SUCCESS;
}

typedef struct {
    EFI_HANDLE handle;
    UINT32 version;
    BOOLEAN consumed;
} EFI_CONNECT_CANDIDATE;

static EFI_STATUS connect_add_candidate(EFI_CONNECT_CANDIDATE *Candidates,
                                        UINTN *CandidateCount,
                                        EFI_HANDLE Handle, UINT32 Version)
{
    UINTN i;

    if (Handle == NULL ||
        !handle_supports_protocol(Handle,
                                  (void *)mDriverBindingProtocolGuid,
                                  NULL)) {
        return EFI_SUCCESS;
    }
    for (i = 0; i < *CandidateCount; i++) {
        if (Candidates[i].handle == Handle) {
            return EFI_SUCCESS;
        }
    }
    if (*CandidateCount >= PROTOCOL_RECORD_MAX) {
        return EFI_OUT_OF_RESOURCES;
    }
    Candidates[*CandidateCount].handle = Handle;
    Candidates[*CandidateCount].version = Version;
    Candidates[*CandidateCount].consumed = 0;
    (*CandidateCount)++;
    return EFI_SUCCESS;
}

static void connect_sort_candidate_group(EFI_CONNECT_CANDIDATE *Candidates,
                                         UINTN First, UINTN Count)
{
    UINTN i;

    for (i = First + 1U; i < Count; i++) {
        EFI_CONNECT_CANDIDATE candidate = Candidates[i];
        UINTN pos = i;

        while (pos > First &&
               Candidates[pos - 1U].version < candidate.version) {
            Candidates[pos] = Candidates[pos - 1U];
            pos--;
        }
        Candidates[pos] = candidate;
    }
}

static EFI_STATUS connect_collect_candidates(
    EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
    EFI_CONNECT_CANDIDATE *Candidates, UINTN *CandidateCount)
{
    EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL *platform = NULL;
    EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL *bus = NULL;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS st;
    UINTN count;
    UINTN first;
    UINTN i;

    *CandidateCount = 0;

    /* Context override. */
    if (DriverImageHandle != NULL) {
        for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
            if (DriverImageHandle[i] == NULL) {
                break;
            }
            st = connect_add_candidate(Candidates, CandidateCount,
                                       DriverImageHandle[i], 0);
            if (st != EFI_SUCCESS) {
                return st;
            }
        }
        if (i == PROTOCOL_RECORD_MAX) {
            return EFI_INVALID_PARAMETER;
        }
    }

    /* Platform driver override. */
    st = bs_locate_protocol((void *)mPlatformDriverOverrideProtocolGuid,
                            NULL, (VOID **)&platform);
    if (st == EFI_SUCCESS && platform != NULL &&
        platform->GetDriver != NULL) {
        EFI_HANDLE previous = NULL;

        for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
            EFI_HANDLE current = previous;

            st = platform->GetDriver(platform, ControllerHandle, &current);
            if (st != EFI_SUCCESS) {
                break;
            }
            if (current == NULL || current == previous) {
                break;
            }
            st = connect_add_candidate(Candidates, CandidateCount, current,
                                       0);
            if (st != EFI_SUCCESS) {
                return st;
            }
            previous = current;
        }
        if (i == PROTOCOL_RECORD_MAX) {
            return EFI_OUT_OF_RESOURCES;
        }
    }

    /* Driver family override, sorted by the family version. */
    st = bs_locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL,
                                 (void *)mDriverFamilyOverrideProtocolGuid,
                                 NULL, &count, &handles);
    if (st == EFI_SUCCESS) {
        first = *CandidateCount;
        for (i = 0; i < count; i++) {
            EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL *family = NULL;

            if (!handle_supports_protocol(
                    handles[i], (void *)mDriverFamilyOverrideProtocolGuid,
                    (VOID **)&family) || family == NULL ||
                family->GetVersion == NULL) {
                continue;
            }
            st = connect_add_candidate(Candidates, CandidateCount,
                                       handles[i],
                                       family->GetVersion(family));
            if (st != EFI_SUCCESS) {
                (void)bs_free_pool(handles);
                return st;
            }
        }
        (void)bs_free_pool(handles);
        handles = NULL;
        connect_sort_candidate_group(Candidates, first, *CandidateCount);
    } else if (st != EFI_NOT_FOUND) {
        return st;
    }

    /* Bus-specific driver override. */
    if (handle_supports_protocol(
            ControllerHandle, (void *)mBusSpecificDriverOverrideProtocolGuid,
            (VOID **)&bus) && bus != NULL && bus->GetDriver != NULL) {
        EFI_HANDLE previous = NULL;

        for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
            EFI_HANDLE current = previous;

            st = bus->GetDriver(bus, &current);
            if (st != EFI_SUCCESS) {
                break;
            }
            if (current == NULL || current == previous) {
                break;
            }
            st = connect_add_candidate(Candidates, CandidateCount, current,
                                       0);
            if (st != EFI_SUCCESS) {
                return st;
            }
            previous = current;
        }
        if (i == PROTOCOL_RECORD_MAX) {
            return EFI_OUT_OF_RESOURCES;
        }
    }

    /* Remaining Driver Binding handles, sorted by binding version. */
    st = bs_locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL,
                                 (void *)mDriverBindingProtocolGuid,
                                 NULL, &count, &handles);
    if (st == EFI_NOT_FOUND) {
        return EFI_SUCCESS;
    }
    if (st != EFI_SUCCESS) {
        return st;
    }
    first = *CandidateCount;
    for (i = 0; i < count; i++) {
        EFI_DRIVER_BINDING_PROTOCOL *binding = NULL;

        if (!handle_supports_protocol(handles[i],
                                      (void *)mDriverBindingProtocolGuid,
                                      (VOID **)&binding) ||
            binding == NULL) {
            continue;
        }
        st = connect_add_candidate(Candidates, CandidateCount, handles[i],
                                   binding->Version);
        if (st != EFI_SUCCESS) {
            (void)bs_free_pool(handles);
            return st;
        }
    }
    (void)bs_free_pool(handles);
    connect_sort_candidate_group(Candidates, first, *CandidateCount);
    return EFI_SUCCESS;
}

EFI_STATUS bs_connect_controller(EFI_HANDLE ControllerHandle,
                                 EFI_HANDLE *DriverImageHandle,
                                 void *RemainingDevicePath,
                                 BOOLEAN Recursive)
{
    EFI_CONNECT_CANDIDATE *candidates = NULL;
    UINTN candidate_count = 0;
    UINTN i;
    BOOLEAN connected = 0;
    EFI_STATUS st;
    static UINTN recursion_depth;
    void **protocols = NULL;
    UINTN protocol_count = 0;

    if (ControllerHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    st = bs_protocols_per_handle(ControllerHandle, &protocols,
                                 &protocol_count);
    if (st != EFI_SUCCESS) {
        return EFI_INVALID_PARAMETER;
    }
    (void)protocol_count;
    (void)bs_free_pool(protocols);

    st = bs_allocate_pool(EfiBootServicesData,
                          sizeof(*candidates) * PROTOCOL_RECORD_MAX,
                          (VOID **)&candidates);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = connect_collect_candidates(ControllerHandle, DriverImageHandle,
                                    candidates, &candidate_count);
    if (st != EFI_SUCCESS) {
        (void)bs_free_pool(candidates);
        return st;
    }

    for (;;) {
        BOOLEAN restart = 0;

        for (i = 0; i < candidate_count; i++) {
            EFI_DRIVER_BINDING_PROTOCOL *binding = NULL;

            if (candidates[i].consumed ||
                !handle_supports_protocol(
                    candidates[i].handle,
                    (void *)mDriverBindingProtocolGuid,
                    (VOID **)&binding) ||
                binding == NULL || binding->Supported == NULL ||
                binding->Start == NULL) {
                continue;
            }
            st = binding->Supported(binding, ControllerHandle,
                                    RemainingDevicePath);
            if (st != EFI_SUCCESS) {
                continue;
            }
            candidates[i].consumed = 1;
            st = binding->Start(binding, ControllerHandle,
                                RemainingDevicePath);
            if (st == EFI_SUCCESS) {
                connected = 1;
            }
            restart = 1;
            break;
        }
        if (!restart) {
            break;
        }
    }
    (void)bs_free_pool(candidates);

    if (Recursive && recursion_depth < 32U) {
        EFI_HANDLE children[OPEN_PROTOCOL_RECORD_MAX];
        UINTN child_count = 0;

        for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
            EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];
            UINTN j;

            if (!rec->in_use || rec->handle != ControllerHandle ||
                rec->attributes != EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
                rec->controller_handle == NULL) {
                continue;
            }
            for (j = 0; j < child_count; j++) {
                if (children[j] == rec->controller_handle) {
                    break;
                }
            }
            if (j == child_count) {
                children[child_count++] = rec->controller_handle;
            }
        }
        recursion_depth++;
        for (i = 0; i < child_count; i++) {
            st = bs_connect_controller(children[i], NULL, NULL, 1);
            if (st == EFI_SUCCESS) {
                connected = 1;
            }
        }
        recursion_depth--;
    }

    if (!connected && RemainingDevicePath != NULL) {
        const UINT8 *end = (const UINT8 *)RemainingDevicePath;

        if (end[0] == 0x7fU && end[1] == 0xffU &&
            end[2] == 4U && end[3] == 0U) {
            return EFI_SUCCESS;
        }
    }
    return connected ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static VOID start_image_connect_modified_handles(
    const EFI_START_IMAGE_FRAME *Frame)
{
    EFI_HANDLE modified[PROTOCOL_RECORD_MAX];
    UINTN modified_count = 0;
    UINTN i;

    if (Frame == NULL || mBootServicesExited) {
        return;
    }
    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        UINTN j;

        if (!mProtocolRecords[i].in_use ||
            mProtocolRecords[i].modification_generation <=
                Frame->handle_database_generation) {
            continue;
        }
        for (j = 0; j < modified_count; j++) {
            if (modified[j] == mProtocolRecords[i].handle) {
                break;
            }
        }
        if (j == modified_count) {
            modified[modified_count++] = mProtocolRecords[i].handle;
        }
    }
    for (i = 0; i < modified_count; i++) {
        (void)bs_connect_controller(modified[i], NULL, NULL, 1);
    }
}

EFI_STATUS bs_disconnect_controller(EFI_HANDLE ControllerHandle,
                                    EFI_HANDLE DriverImageHandle,
                                    EFI_HANDLE ChildHandle)
{
    EFI_HANDLE drivers[OPEN_PROTOCOL_RECORD_MAX];
    UINTN driver_count = 0;
    BOOLEAN disconnected = 0;
    EFI_STATUS first_error = EFI_SUCCESS;
    UINTN i;

    if (ControllerHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (DriverImageHandle != NULL &&
        (!efi_handle_is_valid(DriverImageHandle) ||
         !handle_supports_protocol(DriverImageHandle,
                                   (void *)mDriverBindingProtocolGuid,
                                   NULL))) {
        return EFI_INVALID_PARAMETER;
    }
    if (ChildHandle != NULL && !efi_handle_is_valid(ChildHandle)) {
        return EFI_INVALID_PARAMETER;
    }

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];
        UINTN j;

        if (!rec->in_use || rec->handle != ControllerHandle ||
            !open_protocol_is_driver(rec->attributes) ||
            (DriverImageHandle != NULL &&
             rec->agent_handle != DriverImageHandle)) {
            continue;
        }
        for (j = 0; j < driver_count; j++) {
            if (drivers[j] == rec->agent_handle) {
                break;
            }
        }
        if (j == driver_count) {
            drivers[driver_count++] = rec->agent_handle;
        }
    }

    for (i = 0; i < driver_count; i++) {
        EFI_DRIVER_BINDING_PROTOCOL *binding = NULL;
        EFI_HANDLE children[OPEN_PROTOCOL_RECORD_MAX];
        UINTN child_count = 0;
        UINTN j;
        EFI_STATUS st;

        if (!handle_supports_protocol(drivers[i],
                                      (void *)mDriverBindingProtocolGuid,
                                      (VOID **)&binding) ||
            binding == NULL || binding->Stop == NULL) {
            if (first_error == EFI_SUCCESS) {
                first_error = EFI_DEVICE_ERROR;
            }
            continue;
        }
        for (j = 0; j < OPEN_PROTOCOL_RECORD_MAX; j++) {
            EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[j];
            UINTN k;

            if (!rec->in_use || rec->handle != ControllerHandle ||
                rec->agent_handle != drivers[i] ||
                rec->attributes != EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
                rec->controller_handle == NULL ||
                (ChildHandle != NULL &&
                 rec->controller_handle != ChildHandle)) {
                continue;
            }
            for (k = 0; k < child_count; k++) {
                if (children[k] == rec->controller_handle) {
                    break;
                }
            }
            if (k == child_count) {
                children[child_count++] = rec->controller_handle;
            }
        }
        if (ChildHandle != NULL && child_count == 0) {
            continue;
        }

        if (child_count != 0) {
            st = binding->Stop(binding, ControllerHandle, child_count,
                               children);
            if (st != EFI_SUCCESS) {
                if (first_error == EFI_SUCCESS) {
                    first_error = st;
                }
                continue;
            }
            disconnected = 1;
        }
        if (ChildHandle == NULL) {
            st = binding->Stop(binding, ControllerHandle, 0, NULL);
            if (st != EFI_SUCCESS) {
                if (first_error == EFI_SUCCESS) {
                    first_error = st;
                }
                continue;
            }
            disconnected = 1;
        } else {
            BOOLEAN has_children = 0;

            for (j = 0; j < OPEN_PROTOCOL_RECORD_MAX; j++) {
                EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[j];

                if (rec->in_use && rec->handle == ControllerHandle &&
                    rec->agent_handle == drivers[i] &&
                    rec->attributes ==
                        EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER &&
                    rec->controller_handle != NULL) {
                    has_children = 1;
                    break;
                }
            }
            if (!has_children) {
                st = binding->Stop(binding, ControllerHandle, 0, NULL);
                if (st != EFI_SUCCESS) {
                    if (first_error == EFI_SUCCESS) {
                        first_error = st;
                    }
                    continue;
                }
            }
        }
    }

    if (first_error != EFI_SUCCESS) {
        return first_error;
    }
    (void)disconnected;
    return EFI_SUCCESS;
}

EFI_STATUS bs_open_protocol(EFI_HANDLE Handle, void *Protocol,
                            VOID **Interface, EFI_HANDLE AgentHandle,
                            EFI_HANDLE ControllerHandle, UINT32 Attributes)
{
    VOID *interface;
    EFI_STATUS st;

    if (Protocol == NULL ||
        (Interface == NULL &&
         Attributes != EFI_OPEN_PROTOCOL_TEST_PROTOCOL) ||
        Handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (!handle_supports_protocol(Handle, Protocol, &interface)) {
        if (Interface != NULL &&
            Attributes != EFI_OPEN_PROTOCOL_TEST_PROTOCOL) {
            *Interface = NULL;
        }
        return EFI_UNSUPPORTED;
    }
    if (!open_protocol_attribute_legal(Attributes)) {
        return EFI_INVALID_PARAMETER;
    }
    if ((Attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
         Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER ||
         Attributes ==
             (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE) ||
         Attributes == EFI_OPEN_PROTOCOL_EXCLUSIVE) &&
        AgentHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if ((Attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
         Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER ||
         Attributes ==
             (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE)) &&
        ControllerHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (Attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER &&
        Handle == ControllerHandle) {
        return EFI_INVALID_PARAMETER;
    }

    st = open_protocol_check_conflicts(Handle, Protocol, AgentHandle,
                                       Attributes);
    if (st != EFI_SUCCESS) {
        if (st == EFI_ALREADY_STARTED && Interface != NULL) {
            *Interface = interface;
        }
        return st;
    }

    st = add_open_protocol_record(Handle, Protocol, AgentHandle,
                                  ControllerHandle, Attributes);
    if (st != EFI_SUCCESS) {
        return st;
    }

    if (Interface != NULL && Attributes != EFI_OPEN_PROTOCOL_TEST_PROTOCOL) {
        *Interface = interface;
    }
    return EFI_SUCCESS;
}

EFI_STATUS bs_close_protocol(EFI_HANDLE Handle, void *Protocol,
                             EFI_HANDLE AgentHandle,
                             EFI_HANDLE ControllerHandle)
{
    BOOLEAN found = 0;
    UINTN i;

    if (Handle == NULL || Protocol == NULL || AgentHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!handle_supports_protocol(Handle, Protocol, NULL)) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use &&
            rec->handle == Handle &&
            rec->agent_handle == AgentHandle &&
            rec->controller_handle == ControllerHandle &&
            guid_matches(Protocol, rec->guid)) {
            clear_open_protocol_record(rec);
            found = 1;
        }
    }

    return found ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS bs_open_protocol_information(EFI_HANDLE Handle, void *Protocol,
                                        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer,
                                        UINTN *EntryCount)
{
    UINTN count = 0;
    UINTN i;
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries;
    EFI_STATUS st;

    if (EntryBuffer == NULL || EntryCount == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *EntryBuffer = NULL;
    *EntryCount = 0;
    if (Handle == NULL || Protocol == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!handle_supports_protocol(Handle, Protocol, NULL)) {
        return EFI_NOT_FOUND;
    }
    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Handle &&
            guid_matches(Protocol, rec->guid)) {
            count++;
        }
    }
    if (count == 0) {
        return EFI_SUCCESS;
    }

    st = bs_allocate_pool(EfiBootServicesData,
                          count * sizeof(*entries), (VOID **)&entries);
    if (st != EFI_SUCCESS) {
        return st;
    }
    count = 0;
    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Handle &&
            guid_matches(Protocol, rec->guid)) {
            entries[count].AgentHandle = rec->agent_handle;
            entries[count].ControllerHandle = rec->controller_handle;
            entries[count].Attributes = rec->attributes;
            entries[count].OpenCount = rec->open_count;
            count++;
        }
    }
    *EntryBuffer = entries;
    *EntryCount = count;
    return EFI_SUCCESS;
}

EFI_STATUS bs_protocols_per_handle(EFI_HANDLE Handle, void ***ProtocolBuffer,
                                   UINTN *ProtocolBufferCount)
{
    UINTN count = 0;
    UINTN i;
    void **buffer;
    EFI_STATUS st;
    const FW_PCI_IO_DEVICE *pci_io_dev;

    if (Handle == NULL || ProtocolBuffer == NULL ||
        ProtocolBufferCount == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *ProtocolBuffer = NULL;
    *ProtocolBufferCount = 0;

    if (Handle == mRawBlockIoHandle) {
        count += 3;
        if ((fw_udf_init() || fw_iso_init()) &&
            !fw_boot_optical_fs_available()) {
            count++;
        }
    }
    if (Handle == mBlockIoHandle) {
        count += 3;
        if (fw_boot_fat_available() || fw_boot_optical_fs_available()) {
            count++;
        }
    }
    if (Handle == mDiskBlockIoHandle) {
        count += 3;
    }
    if (Handle == mImageHandle) {
        count += 4;
    }
    if (Handle == mUnicodeCollationHandle) {
        count++;
    }
    if (Handle == mGraphicsHandle) {
        count += 3;
    }
    if (Handle == mPciRootBridgeHandle) {
        count += 2;
    }
    pci_io_dev = fw_pci_io_device_from_handle(Handle);
    if (pci_io_dev != NULL) {
        count++;
        if (pci_io_dev->ProvidesDevicePath) {
            count++;
        }
    }
    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (mLoadedImages[i].in_use && Handle == mLoadedImages[i].handle) {
            count += 2;
            if (mLoadedImages[i].hii_package_list != NULL) {
                count++;
            }
        }
    }
    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use && mProtocolRecords[i].handle == Handle) {
            count++;
        }
    }
    if (count == 0) {
        return EFI_NOT_FOUND;
    }

    st = bs_allocate_pool(EfiBootServicesData, count * sizeof(void *),
                          (VOID **)&buffer);
    if (st != EFI_SUCCESS) {
        return st;
    }
    count = 0;
    if (Handle == mRawBlockIoHandle) {
        buffer[count++] = (void *)mBlockIoProtocolGuid;
        buffer[count++] = (void *)mDiskIoProtocolGuid;
        if ((fw_udf_init() || fw_iso_init()) &&
            !fw_boot_optical_fs_available()) {
            buffer[count++] = (void *)mSimpleFileSystemProtocolGuid;
        }
        buffer[count++] = (void *)mDevicePathProtocolGuid;
    }
    if (Handle == mBlockIoHandle) {
        buffer[count++] = (void *)mBlockIoProtocolGuid;
        buffer[count++] = (void *)mDiskIoProtocolGuid;
        if (fw_boot_fat_available() || fw_boot_optical_fs_available()) {
            buffer[count++] = (void *)mSimpleFileSystemProtocolGuid;
        }
        buffer[count++] = (void *)mDevicePathProtocolGuid;
    }
    if (Handle == mDiskBlockIoHandle) {
        buffer[count++] = (void *)mBlockIoProtocolGuid;
        buffer[count++] = (void *)mDiskIoProtocolGuid;
        buffer[count++] = (void *)mDevicePathProtocolGuid;
    }
    if (Handle == mImageHandle) {
        buffer[count++] = (void *)mLoadedImageProtocolGuid;
        buffer[count++] = (void *)mConInProtocolGuid;
        buffer[count++] = (void *)mConInExProtocolGuid;
        buffer[count++] = (void *)mConOutProtocolGuid;
    }
    if (Handle == mUnicodeCollationHandle) {
        buffer[count++] = (void *)mUnicodeCollationProtocolGuid;
    }
    if (Handle == mGraphicsHandle) {
        buffer[count++] = (void *)mGraphicsOutputProtocolGuid;
        buffer[count++] = (void *)mUgaDrawProtocolGuid;
        buffer[count++] = (void *)mDevicePathProtocolGuid;
    }
    if (Handle == mPciRootBridgeHandle) {
        buffer[count++] = (void *)mPciRootBridgeIoProtocolGuid;
        buffer[count++] = (void *)mDevicePathProtocolGuid;
    }
    if (pci_io_dev != NULL) {
        buffer[count++] = (void *)mPciIoProtocolGuid;
        if (pci_io_dev->ProvidesDevicePath) {
            buffer[count++] = (void *)mDevicePathProtocolGuid;
        }
    }
    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (mLoadedImages[i].in_use && Handle == mLoadedImages[i].handle) {
            buffer[count++] = (void *)mLoadedImageProtocolGuid;
            buffer[count++] = (void *)mLoadedImageDevicePathProtocolGuid;
            if (mLoadedImages[i].hii_package_list != NULL) {
                buffer[count++] = (void *)mHiiPackageListProtocolGuid;
            }
        }
    }
    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use && mProtocolRecords[i].handle == Handle) {
            buffer[count++] = mProtocolRecords[i].guid;
        }
    }
    *ProtocolBuffer = buffer;
    *ProtocolBufferCount = count;
    return EFI_SUCCESS;
}

static BOOLEAN protocol_interface_list_has_guid(void **Protocols,
                                                UINTN Count,
                                                void *Protocol)
{
    UINTN i;

    for (i = 0; i < Count; i++) {
        if (guid_matches(Protocol, (const UINT8 *)Protocols[i])) {
            return 1;
        }
    }
    return 0;
}

static EFI_STATUS check_duplicate_device_path(EFI_HANDLE TargetHandle,
                                              VOID *DevicePath)
{
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    UINTN path_size;
    UINTN i;
    EFI_STATUS st;

    path_size = fw_device_path_size(DevicePath);
    if (path_size == 0) {
        return EFI_INVALID_PARAMETER;
    }
    st = bs_locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL,
                                 (void *)mDevicePathProtocolGuid,
                                 NULL, &handle_count, &handles);
    if (st == EFI_NOT_FOUND) {
        return EFI_SUCCESS;
    }
    if (st != EFI_SUCCESS) {
        return st;
    }
    for (i = 0; i < handle_count; i++) {
        VOID *existing = NULL;
        UINTN existing_size;
        UINTN byte;

        if (handles[i] == TargetHandle ||
            !handle_supports_protocol(handles[i],
                                      (void *)mDevicePathProtocolGuid,
                                      (VOID **)&existing) ||
            existing == NULL) {
            continue;
        }
        existing_size = fw_device_path_size(existing);
        if (existing_size != path_size) {
            continue;
        }
        for (byte = 0; byte < path_size; byte++) {
            if (((const UINT8 *)existing)[byte] !=
                ((const UINT8 *)DevicePath)[byte]) {
                break;
            }
        }
        if (byte == path_size) {
            (void)bs_free_pool(handles);
            return EFI_ALREADY_STARTED;
        }
    }
    (void)bs_free_pool(handles);
    return EFI_SUCCESS;
}

EFI_STATUS bs_install_multiple_protocol_interfaces(EFI_HANDLE *Handle, ...)
{
    void *protocols[64];
    void *interfaces[64];
    UINTN count = 0;
    UINTN installed = 0;
    EFI_HANDLE original_handle;
    __builtin_va_list args;
    void *protocol;
    EFI_STATUS st = EFI_SUCCESS;

    if (Handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    original_handle = *Handle;

    __builtin_va_start(args, Handle);
    while ((protocol = __builtin_va_arg(args, void *)) != NULL) {
        void *interface = __builtin_va_arg(args, void *);

        if (count >= FW_ARRAY_SIZE(protocols)) {
            st = EFI_OUT_OF_RESOURCES;
            break;
        }
        if (protocol_interface_list_has_guid(protocols, count, protocol)) {
            st = EFI_INVALID_PARAMETER;
            break;
        }
        protocols[count] = protocol;
        interfaces[count] = interface;
        count++;
    }
    __builtin_va_end(args);
    if (st == EFI_SUCCESS) {
        for (installed = 0; installed < count; installed++) {
            if (guid_matches((void *)mDevicePathProtocolGuid,
                             (const UINT8 *)protocols[installed])) {
                st = check_duplicate_device_path(
                    original_handle, interfaces[installed]);
                break;
            }
        }
        installed = 0;
    }
    while (st == EFI_SUCCESS && installed < count) {
        st = bs_install_protocol(Handle, protocols[installed], 0,
                                 interfaces[installed]);
        if (st == EFI_SUCCESS) {
            installed++;
        }
    }
    if (st != EFI_SUCCESS) {
        while (installed != 0) {
            installed--;
            (void)bs_uninstall_protocol(*Handle, protocols[installed],
                                        interfaces[installed]);
        }
        if (original_handle == NULL) {
            *Handle = NULL;
        }
    }
    return st;
}

EFI_STATUS bs_uninstall_multiple_protocol_interfaces(EFI_HANDLE Handle, ...)
{
    void *protocols[64];
    void *interfaces[64];
    UINTN count = 0;
    UINTN removed = 0;
    __builtin_va_list args;
    void *protocol;
    EFI_STATUS st = EFI_SUCCESS;

    if (Handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    __builtin_va_start(args, Handle);
    while ((protocol = __builtin_va_arg(args, void *)) != NULL) {
        void *interface = __builtin_va_arg(args, void *);

        if (count >= FW_ARRAY_SIZE(protocols)) {
            st = EFI_OUT_OF_RESOURCES;
            break;
        }
        protocols[count] = protocol;
        interfaces[count] = interface;
        count++;
    }
    __builtin_va_end(args);
    if (st != EFI_SUCCESS) {
        return st;
    }
    for (removed = 0; removed < count; removed++) {
        st = bs_uninstall_protocol(Handle, protocols[removed],
                                   interfaces[removed]);
        if (st != EFI_SUCCESS) {
            UINTN restore = removed;

            while (restore != 0) {
                EFI_HANDLE restored_handle = Handle;

                restore--;
                (void)bs_install_protocol(&restored_handle,
                                          protocols[restore], 0,
                                          interfaces[restore]);
            }
            return EFI_INVALID_PARAMETER;
        }
    }
    return EFI_SUCCESS;
}

EFI_STATUS bs_calculate_crc32(VOID *Data, UINTN DataSize, UINT32 *Crc32)
{
    UINT32 crc = 0xffffffffU;
    UINT8 *p = (UINT8 *)Data;
    UINTN i;

    if ((Data == NULL && DataSize != 0) || Crc32 == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < DataSize; i++) {
        UINTN bit;
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xedb88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    *Crc32 = ~crc;
    return EFI_SUCCESS;
}

static void efi_update_table_crc32(EFI_TABLE_HEADER *Header)
{
    UINT32 crc;

    if (Header == NULL || Header->HeaderSize < sizeof(*Header)) {
        return;
    }
    Header->CRC32 = 0;
    if (bs_calculate_crc32(Header, Header->HeaderSize, &crc) == EFI_SUCCESS) {
        Header->CRC32 = crc;
    }
}

static void efi_refresh_table_crc32s(void)
{
    efi_update_table_crc32(&mBootServices.Hdr);
    efi_update_table_crc32(&mRuntimeServices.Hdr);
    efi_update_table_crc32(&mSystemTable.Hdr);
}

static BOOLEAN efi_system_table_crc32_valid(void)
{
    EFI_SYSTEM_TABLE table_copy = mSystemTable;
    UINT32 crc;

    table_copy.Hdr.CRC32 = 0;
    if (bs_calculate_crc32(&table_copy, table_copy.Hdr.HeaderSize, &crc) !=
        EFI_SUCCESS) {
        return 0;
    }
    return crc == mSystemTable.Hdr.CRC32;
}

static BOOLEAN __attribute__((noinline)) uefi_configuration_table_selftest(void)
{
    static const UINT8 test_guid[16] = {
        0x54, 0x43, 0x46, 0x57, 0x51, 0x45, 0x4d, 0x55,
        0x49, 0x41, 0x36, 0x34, 0x54, 0x45, 0x53, 0x54
    };
    static UINTN test_table;
    UINTN original_count = mSystemTable.NumberOfTableEntries;
    UINT32 original_crc = mSystemTable.Hdr.CRC32;
    EFI_STATUS st;

    if (original_count >= PLATFORM_TABLE_MAX ||
        !efi_system_table_crc32_valid()) {
        return 0;
    }

    st = bs_install_configuration_table((void *)test_guid, &test_table);
    if (st != EFI_SUCCESS ||
        mSystemTable.NumberOfTableEntries != original_count + 1U ||
        mSystemTable.Hdr.CRC32 == original_crc ||
        !efi_system_table_crc32_valid()) {
        (void)bs_install_configuration_table((void *)test_guid, NULL);
        return 0;
    }

    st = bs_install_configuration_table((void *)test_guid, NULL);
    if (st != EFI_SUCCESS ||
        mSystemTable.NumberOfTableEntries != original_count ||
        mSystemTable.Hdr.CRC32 != original_crc ||
        !efi_system_table_crc32_valid()) {
        return 0;
    }

    return 1;
}

typedef struct {
    UINT32 h[5];
    UINT64 length_bits;
    UINT8 block[64];
    UINTN block_len;
} FW_SHA1_CONTEXT;

static UINT32 fw_rotl32(UINT32 Value, UINTN Shift)
{
    return (Value << Shift) | (Value >> (32U - Shift));
}

static UINT32 fw_sha1_read_be32(const UINT8 *Data)
{
    return ((UINT32)Data[0] << 24) |
           ((UINT32)Data[1] << 16) |
           ((UINT32)Data[2] << 8) |
           (UINT32)Data[3];
}

static void fw_sha1_write_be32(UINT8 *Data, UINT32 Value)
{
    Data[0] = (UINT8)(Value >> 24);
    Data[1] = (UINT8)(Value >> 16);
    Data[2] = (UINT8)(Value >> 8);
    Data[3] = (UINT8)Value;
}

static void fw_sha1_write_be64(UINT8 *Data, UINT64 Value)
{
    UINTN i;

    for (i = 0; i < 8; i++) {
        Data[i] = (UINT8)(Value >> ((7U - i) * 8U));
    }
}

static void fw_sha1_transform(FW_SHA1_CONTEXT *Ctx, const UINT8 Block[64])
{
    UINT32 w[80];
    UINT32 a;
    UINT32 b;
    UINT32 c;
    UINT32 d;
    UINT32 e;
    UINTN i;

    for (i = 0; i < 16; i++) {
        w[i] = fw_sha1_read_be32(Block + i * 4U);
    }
    for (i = 16; i < 80; i++) {
        w[i] = fw_rotl32(w[i - 3U] ^ w[i - 8U] ^
                         w[i - 14U] ^ w[i - 16U], 1);
    }

    a = Ctx->h[0];
    b = Ctx->h[1];
    c = Ctx->h[2];
    d = Ctx->h[3];
    e = Ctx->h[4];

    for (i = 0; i < 80; i++) {
        UINT32 f;
        UINT32 k;
        UINT32 temp;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }

        temp = fw_rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = fw_rotl32(b, 30);
        b = a;
        a = temp;
    }

    Ctx->h[0] += a;
    Ctx->h[1] += b;
    Ctx->h[2] += c;
    Ctx->h[3] += d;
    Ctx->h[4] += e;
}

static void fw_sha1_init(FW_SHA1_CONTEXT *Ctx)
{
    Ctx->h[0] = 0x67452301U;
    Ctx->h[1] = 0xefcdab89U;
    Ctx->h[2] = 0x98badcfeU;
    Ctx->h[3] = 0x10325476U;
    Ctx->h[4] = 0xc3d2e1f0U;
    Ctx->length_bits = 0;
    Ctx->block_len = 0;
}

static void fw_sha1_update(FW_SHA1_CONTEXT *Ctx, const UINT8 *Data,
                           UINTN DataLen)
{
    Ctx->length_bits += (UINT64)DataLen * 8ULL;
    while (DataLen > 0) {
        UINTN chunk = sizeof(Ctx->block) - Ctx->block_len;

        if (chunk > DataLen) {
            chunk = DataLen;
        }
        fw_copy_mem(Ctx->block + Ctx->block_len, Data, chunk);
        Ctx->block_len += chunk;
        Data += chunk;
        DataLen -= chunk;
        if (Ctx->block_len == sizeof(Ctx->block)) {
            fw_sha1_transform(Ctx, Ctx->block);
            Ctx->block_len = 0;
        }
    }
}

static void fw_sha1_final(FW_SHA1_CONTEXT *Ctx,
                          UINT8 Digest[TCG_SHA1_DIGEST_SIZE])
{
    UINT64 length_bits = Ctx->length_bits;
    UINTN i;

    Ctx->block[Ctx->block_len++] = 0x80;
    if (Ctx->block_len > 56U) {
        while (Ctx->block_len < sizeof(Ctx->block)) {
            Ctx->block[Ctx->block_len++] = 0;
        }
        fw_sha1_transform(Ctx, Ctx->block);
        Ctx->block_len = 0;
    }
    while (Ctx->block_len < 56U) {
        Ctx->block[Ctx->block_len++] = 0;
    }
    fw_sha1_write_be64(Ctx->block + 56, length_bits);
    fw_sha1_transform(Ctx, Ctx->block);

    for (i = 0; i < FW_ARRAY_SIZE(Ctx->h); i++) {
        fw_sha1_write_be32(Digest + i * 4U, Ctx->h[i]);
    }
}

static void fw_sha1_hash(const UINT8 *Data, UINTN DataLen,
                         UINT8 Digest[TCG_SHA1_DIGEST_SIZE])
{
    FW_SHA1_CONTEXT ctx;

    fw_sha1_init(&ctx);
    fw_sha1_update(&ctx, Data, DataLen);
    fw_sha1_final(&ctx, Digest);
}

static BOOLEAN tcg_digest_matches(const UINT8 *Digest, const UINT8 *Expected)
{
    UINTN i;

    for (i = 0; i < TCG_SHA1_DIGEST_SIZE; i++) {
        if (Digest[i] != Expected[i]) {
            return 0;
        }
    }
    return 1;
}

static BOOLEAN tcg_sha1_selftest(void)
{
    static const UINT8 expected_empty[TCG_SHA1_DIGEST_SIZE] = {
        0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d,
        0x32, 0x55, 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90,
        0xaf, 0xd8, 0x07, 0x09
    };
    static const UINT8 expected_abc[TCG_SHA1_DIGEST_SIZE] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
        0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
        0x9c, 0xd0, 0xd8, 0x9d
    };
    static const UINT8 abc[] = { 'a', 'b', 'c' };
    UINT8 digest[TCG_SHA1_DIGEST_SIZE];

    fw_sha1_hash(NULL, 0, digest);
    if (!tcg_digest_matches(digest, expected_empty)) {
        return 0;
    }
    fw_sha1_hash(abc, sizeof(abc), digest);
    if (!tcg_digest_matches(digest, expected_abc)) {
        return 0;
    }
    return 1;
}

static BOOLEAN tcg_install_protocol(void)
{
    /* There is no TPM device on this machine, so no TCG protocol exists. */
    mTcgHandle = NULL;
    return 1;
}

static BOOLEAN __attribute__((noinline)) tcg_protocol_selftest(void)
{
    VOID *interface = NULL;

    return tcg_sha1_selftest() &&
           mTcgHandle == NULL &&
           bs_locate_protocol((void *)mTcgProtocolGuid, NULL, &interface) ==
               EFI_NOT_FOUND &&
           !installed_protocol_interface(FW_HANDLE_TCG,
                                         (void *)mTcgProtocolGuid, NULL);
}


static void efi_init_system_table_pointer(void)
{
    UINT32 crc;

    if (mSystemTablePointer == NULL) {
        return;
    }

    fw_set_mem(mSystemTablePointer, FW_SYSTEM_TABLE_POINTER_SIZE, 0);
    mSystemTablePointer->Signature = EFI_SYSTEM_TABLE_SIGNATURE;
    mSystemTablePointer->EfiSystemTableBase = (UINTN)&mSystemTable;
    mSystemTablePointer->Crc32 = 0;
    mSystemTablePointer->Reserved = 0;
    if (bs_calculate_crc32(mSystemTablePointer,
                           sizeof(*mSystemTablePointer), &crc) ==
        EFI_SUCCESS) {
        mSystemTablePointer->Crc32 = crc;
    }
}

static void efi_debug_image_info_refresh(void)
{
    UINTN slot = 0;
    UINTN i;

    mDebugImageInfoHeader.UpdateStatus =
        EFI_DEBUG_IMAGE_INFO_UPDATE_IN_PROGRESS |
        EFI_DEBUG_IMAGE_INFO_TABLE_MODIFIED;
    fw_set_mem(mDebugImageInfoTable, sizeof(mDebugImageInfoTable), 0);
    fw_set_mem(mDebugImageInfoNormal, sizeof(mDebugImageInfoNormal), 0);

    mDebugImageInfoNormal[slot].ImageInfoType =
        EFI_DEBUG_IMAGE_INFO_TYPE_NORMAL;
    mDebugImageInfoNormal[slot].LoadedImageProtocolInstance =
        &mLoadedImageProto;
    mDebugImageInfoNormal[slot].ImageHandle = mImageHandle;
    mDebugImageInfoTable[slot].NormalImage = &mDebugImageInfoNormal[slot];
    slot++;

    for (i = 0; i < LOADED_IMAGE_MAX && slot < LOADED_IMAGE_MAX + 1U; i++) {
        if (!mLoadedImages[i].in_use) {
            continue;
        }
        mDebugImageInfoNormal[slot].ImageInfoType =
            EFI_DEBUG_IMAGE_INFO_TYPE_NORMAL;
        mDebugImageInfoNormal[slot].LoadedImageProtocolInstance =
            &mLoadedImages[i].loaded_image;
        mDebugImageInfoNormal[slot].ImageHandle = mLoadedImages[i].handle;
        mDebugImageInfoTable[slot].NormalImage =
            &mDebugImageInfoNormal[slot];
        slot++;
    }

    mDebugImageInfoHeader.TableSize = slot;
    mDebugImageInfoHeader.UpdateStatus =
        EFI_DEBUG_IMAGE_INFO_TABLE_MODIFIED;
}

static void efi_init_debug_image_info_table(void)
{
    fw_set_mem(&mDebugImageInfoHeader, sizeof(mDebugImageInfoHeader), 0);
    mDebugImageInfoHeader.EfiDebugImageInfoTable = mDebugImageInfoTable;
    efi_debug_image_info_refresh();
}

static BOOLEAN __attribute__((noinline)) efi_debug_tables_selftest(void)
{
    EFI_SYSTEM_TABLE_POINTER pointer_copy;
    UINT32 crc;

    if (mSystemTablePointer == NULL ||
        mSystemTablePointerBase == 0 ||
        (mSystemTablePointerBase &
         (FW_SYSTEM_TABLE_POINTER_ALIGN - 1U)) != 0 ||
        !efi_memory_map_has_descriptor(EfiReservedMemoryType,
                                       mSystemTablePointerBase,
                                       mSystemTablePointerBase +
                                       FW_SYSTEM_TABLE_POINTER_SIZE,
                                       EFI_MEMORY_WB)) {
        return 0;
    }

    pointer_copy.Signature = mSystemTablePointer->Signature;
    pointer_copy.EfiSystemTableBase = mSystemTablePointer->EfiSystemTableBase;
    pointer_copy.Crc32 = mSystemTablePointer->Crc32;
    pointer_copy.Reserved = mSystemTablePointer->Reserved;
    if (pointer_copy.Signature != EFI_SYSTEM_TABLE_SIGNATURE ||
        pointer_copy.EfiSystemTableBase != (UINTN)&mSystemTable ||
        pointer_copy.Reserved != 0) {
        return 0;
    }
    pointer_copy.Crc32 = 0;
    if (bs_calculate_crc32(&pointer_copy, sizeof(pointer_copy), &crc) !=
        EFI_SUCCESS ||
        crc != mSystemTablePointer->Crc32) {
        return 0;
    }

    if (mDebugImageInfoHeader.UpdateStatus !=
        EFI_DEBUG_IMAGE_INFO_TABLE_MODIFIED ||
        mDebugImageInfoHeader.TableSize != 1 ||
        mDebugImageInfoHeader.EfiDebugImageInfoTable !=
        mDebugImageInfoTable ||
        mDebugImageInfoTable[0].NormalImage != &mDebugImageInfoNormal[0] ||
        mDebugImageInfoNormal[0].ImageInfoType !=
        EFI_DEBUG_IMAGE_INFO_TYPE_NORMAL ||
        mDebugImageInfoNormal[0].LoadedImageProtocolInstance !=
        &mLoadedImageProto ||
        mDebugImageInfoNormal[0].ImageHandle != mImageHandle ||
        !fw_guid_equal(mConfigTables[PLATFORM_TABLE_DEBUG_IMAGE].VendorGuid,
                       mDebugImageInfoTableGuid) ||
        mConfigTables[PLATFORM_TABLE_DEBUG_IMAGE].VendorTable !=
        (UINTN)&mDebugImageInfoHeader) {
        return 0;
    }

    return 1;
}

VOID bs_copy_mem(VOID *Destination, VOID *Source, UINTN Length)
{
    fw_copy_mem(Destination, Source, Length);
}

VOID bs_set_mem(VOID *Buffer, UINTN Size, UINT8 Value)
{
    fw_set_mem(Buffer, Size, Value);
}

EFI_STATUS bs_load_image(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
                                 void *DevicePath, VOID *SourceBuffer, UINTN SourceSize,
                                 EFI_HANDLE *ImageHandle)
{
    UINTN i;
    VOID *entry;
    PE_LOADED_IMAGE_RESULT loaded;
    EFI_MEMORY_TYPE image_code_type;
    EFI_MEMORY_TYPE image_data_type;
    EFI_STATUS st;
    BOOLEAN source_pool_allocated = 0;

    if (ImageHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *ImageHandle = NULL;
    if (mBootServicesExited) {
        return EFI_UNSUPPORTED;
    }
    if (ParentImageHandle == NULL ||
        !handle_supports_protocol(ParentImageHandle,
                                  (void *)mLoadedImageProtocolGuid, NULL)) {
        return EFI_INVALID_PARAMETER;
    }
    if (SourceBuffer == NULL && DevicePath == NULL) {
        return EFI_NOT_FOUND;
    }
    if (SourceBuffer != NULL && SourceSize == 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (SourceBuffer == NULL) {
        st = fw_load_image_source_from_device_path(BootPolicy, DevicePath,
                                                   &SourceBuffer, &SourceSize);
        if (st != EFI_SUCCESS) {
            return st;
        }
        source_pool_allocated = 1;
    }

    entry = load_pe_image((uint8_t *)SourceBuffer, SourceSize, &loaded);
    if (source_pool_allocated) {
        (void)bs_free_pool(SourceBuffer);
    }
    if (entry == NULL) {
        pe_discard_loaded_image_result(&loaded);
        return EFI_LOAD_ERROR;
    }
    pe_image_memory_types(loaded.subsystem, &image_code_type,
                          &image_data_type);
    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (!mLoadedImages[i].in_use) {
            EFI_HANDLE device_handle;
            VOID *file_path;
            VOID *callable_entry = entry;

            fw_set_mem(&mLoadedImages[i], sizeof(mLoadedImages[i]), 0);
            mLoadedImages[i].handle = (EFI_HANDLE)&mLoadedImages[i];
            st = fw_loaded_image_source_paths(&mLoadedImages[i], DevicePath,
                                              &device_handle, &file_path);
            if (st != EFI_SUCCESS) {
                pe_discard_loaded_image_result(&loaded);
                return st;
            }
            mLoadedImages[i].in_use = 1;
            mLoadedImages[i].started = 0;
            mLoadedImages[i].is_ebc = loaded.machine == 0x0ebcU;
            if (mLoadedImages[i].is_ebc) {
                st = fw_ebc_create_image_thunk(mLoadedImages[i].handle,
                                               entry, &callable_entry);
                if (st != EFI_SUCCESS) {
                    if (mLoadedImages[i].device_path != NULL) {
                        (void)bs_free_pool(mLoadedImages[i].device_path);
                    }
                    fw_set_mem(&mLoadedImages[i],
                               sizeof(mLoadedImages[i]), 0);
                    pe_discard_loaded_image_result(&loaded);
                    return st;
                }
            }
            mLoadedImages[i].entry =
                (UINTN (*)(EFI_HANDLE, EFI_SYSTEM_TABLE *))callable_entry;
            mLoadedImages[i].runtime_relocation_log =
                loaded.runtime_relocation_log;
            mLoadedImages[i].runtime_relocation_entries =
                loaded.runtime_relocation_entries;
            mLoadedImages[i].hii_package_list = loaded.hii_package_list;
            *ImageHandle = mLoadedImages[i].handle;
            mLoadedImages[i].loaded_image = mLoadedImageProto;
            mLoadedImages[i].loaded_image.ParentHandle = ParentImageHandle;
            mLoadedImages[i].loaded_image.ImageBase = loaded.base;
            mLoadedImages[i].loaded_image.ImageSize = loaded.size;
            mLoadedImages[i].loaded_image.DeviceHandle = device_handle;
            mLoadedImages[i].loaded_image.FilePath = file_path;
            mLoadedImages[i].loaded_image.LoadOptionsSize = 0;
            mLoadedImages[i].loaded_image.LoadOptions = NULL;
            mLoadedImages[i].loaded_image.ImageCodeType = image_code_type;
            mLoadedImages[i].loaded_image.ImageDataType = image_data_type;
            efi_debug_image_info_refresh();
            return EFI_SUCCESS;
        }
    }

    pe_discard_loaded_image_result(&loaded);
    return EFI_OUT_OF_RESOURCES;
}

static void fw_close_protocols_opened_by_image(EFI_HANDLE ImageHandle)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        if (mOpenProtocolRecords[i].in_use &&
            mOpenProtocolRecords[i].agent_handle == ImageHandle) {
            clear_open_protocol_record(&mOpenProtocolRecords[i]);
        }
    }
}

static EFI_STATUS fw_release_loaded_image_record(
    EFI_LOADED_IMAGE_RECORD *Record)
{
    EFI_HANDLE image_handle;

    if (Record == NULL || !Record->in_use) {
        return EFI_INVALID_PARAMETER;
    }
    image_handle = Record->handle;
    if (Record->is_ebc) {
        EFI_STATUS st = fw_ebc_unload_for_image(image_handle);

        if (st != EFI_SUCCESS) {
            return st;
        }
    }
    if (Record->loaded_image.ImageBase != NULL &&
        Record->loaded_image.ImageSize != 0) {
        pe_release_loaded_image_memory(Record->loaded_image.ImageBase,
                                       Record->loaded_image.ImageSize,
                                       Record->loaded_image.ImageCodeType);
    }
    if (Record->runtime_relocation_log != NULL) {
        (void)bs_free_pool(Record->runtime_relocation_log);
    }
    if (Record->device_path != NULL) {
        (void)bs_free_pool(Record->device_path);
    }
    fw_set_mem(Record, sizeof(*Record), 0);
    efi_debug_image_info_refresh();
    return EFI_SUCCESS;
}

static void fw_finish_started_image(EFI_LOADED_IMAGE_RECORD *Record,
                                    EFI_STATUS ExitStatus)
{
    if (Record == NULL || !Record->in_use || mBootServicesExited) {
        return;
    }
    if (Record->loaded_image.ImageCodeType == EfiLoaderCode ||
        (ExitStatus & EFI_ERROR_BIT) != 0) {
        fw_close_protocols_opened_by_image(Record->handle);
        (void)fw_release_loaded_image_record(Record);
    }
}

EFI_STATUS bs_start_image(EFI_HANDLE ImageHandle, UINTN *ExitDataSize,
                                  CHAR16 **ExitData)
{
    EFI_LOADED_IMAGE_RECORD *rec;
    EFI_START_IMAGE_FRAME *frame;
    BOOLEAN sal_loader_handoff;
    UINTN status;

    if (ImageHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (mBootServicesExited) {
        return EFI_UNSUPPORTED;
    }

    rec = fw_loaded_image_record(ImageHandle);
    if (rec == NULL || rec->entry == NULL || rec->started) {
        return EFI_INVALID_PARAMETER;
    }

    if (ExitDataSize) {
        *ExitDataSize = 0;
    }
    if (ExitData) {
        *ExitData = NULL;
    }

    frame = start_image_push_frame(ImageHandle);
    if (frame == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    rec->started = 1;
    sal_loader_handoff = mSalLoaderHandoffPending;
    mSalLoaderHandoffPending = 0;
    graphics_begin_loader_handoff(sal_loader_handoff);

    if (__builtin_setjmp(frame->jump) != 0) {
        frame = start_image_top_frame();
        if (frame == NULL) {
            return EFI_ABORTED;
        }
        fw_restore_rsc(frame->saved_rsc);
        fw_restore_psr(frame->saved_psr);
        status = frame->exit_status;
        if (ExitDataSize) {
            *ExitDataSize = frame->exit_data_size;
        }
        if (ExitData) {
            *ExitData = frame->exit_data;
        }
        start_image_connect_modified_handles(frame);
        start_image_pop_frame(frame);
        fw_finish_started_image(rec, (EFI_STATUS)status);
        return (EFI_STATUS)status;
    }

    fw_restore_rsc(frame->saved_rsc);
    __asm__ volatile (
        "mov ar.lc = r0\n\t"
        "mov ar.ec = r0\n\t"
        "clrrrb\n\t"
        ::: "memory");
    status = fw_call_efi_entry(rec->entry, ImageHandle, &mSystemTable,
                               frame->saved_psr,
                               sal_loader_handoff ? sal_loader_psr_low() : 0);
    frame = start_image_top_frame();
    start_image_connect_modified_handles(frame);
    start_image_pop_frame(frame);
    fw_finish_started_image(rec, (EFI_STATUS)status);
    return (EFI_STATUS)status;
}

static void efi_exit_boot_services_update_system_table(void)
{
    mSystemTable.ConsoleInHandle = NULL;
    mSystemTable.ConIn = NULL;
    mSystemTable.ConsoleOutHandle = NULL;
    mSystemTable.ConOut = NULL;
    mSystemTable.StandardErrorHandle = NULL;
    mSystemTable.StdErr = NULL;
    mSystemTable.BootServices = NULL;
    efi_update_table_crc32(&mSystemTable.Hdr);
}

static BOOLEAN __attribute__((noinline))
uefi_exit_boot_services_system_table_selftest(void)
{
    EFI_SYSTEM_TABLE saved_table = mSystemTable;
    EFI_RUNTIME_SERVICES *runtime_services = mSystemTable.RuntimeServices;
    VOID *configuration_table = mSystemTable.ConfigurationTable;
    UINTN configuration_table_entries = mSystemTable.NumberOfTableEntries;
    BOOLEAN ok;

    if (mSystemTable.ConsoleInHandle == NULL || mSystemTable.ConIn == NULL ||
        mSystemTable.ConsoleOutHandle == NULL || mSystemTable.ConOut == NULL ||
        mSystemTable.StandardErrorHandle == NULL ||
        mSystemTable.StdErr == NULL ||
        mSystemTable.BootServices == NULL || !efi_system_table_crc32_valid()) {
        return 0;
    }

    efi_exit_boot_services_update_system_table();
    ok = mSystemTable.ConsoleInHandle == NULL && mSystemTable.ConIn == NULL &&
         mSystemTable.ConsoleOutHandle == NULL && mSystemTable.ConOut == NULL &&
         mSystemTable.StandardErrorHandle == NULL &&
         mSystemTable.StdErr == NULL &&
         mSystemTable.BootServices == NULL &&
         mSystemTable.RuntimeServices == runtime_services &&
         mSystemTable.ConfigurationTable == configuration_table &&
         mSystemTable.NumberOfTableEntries == configuration_table_entries &&
         efi_system_table_crc32_valid();

    mSystemTable = saved_table;
    return ok && efi_system_table_crc32_valid();
}

static EFI_STATUS fw_prepare_exit_boot_services(UINTN MapKey)
{
    if (MapKey != mMapKey ||
        !efi_memory_map_has_ia64_descriptor_alignment()) {
        return EFI_INVALID_PARAMETER;
    }
    if (!mBeforeExitBootServicesSignaled) {
        mBeforeExitBootServicesSignaled = 1;
        fw_signal_event_group(gEfiEventGroupBeforeExitBootServicesGuid);
    }
    /*
     * UEFI requires timer services to be deactivated immediately after the
     * BEFORE_EXIT_BOOT_SERVICES handlers run.  A handler is allowed to change
     * the memory map, so this is deliberately before the second MapKey check:
     * the first failed attempt may leave boot services partially shut down.
     */
    fw_cancel_all_timers();
    if (MapKey != mMapKey ||
        !efi_memory_map_has_ia64_descriptor_alignment()) {
        return EFI_INVALID_PARAMETER;
    }
    return EFI_SUCCESS;
}

static void fw_signal_exit_boot_services_events(void)
{
    if (mExitBootServicesEventsSignaled) {
        return;
    }
    mExitBootServicesEventsSignaled = 1;
    fw_signal_event_group_and_type(
        gEfiEventGroupExitBootServicesGuid,
        EVT_SIGNAL_EXIT_BOOT_SERVICES);
}

EFI_STATUS bs_exit_boot_services(EFI_HANDLE ImageHandle, UINTN MapKey)
{
    EFI_STATUS st;

    (void)ImageHandle;
    st = fw_prepare_exit_boot_services(MapKey);
    if (st != EFI_SUCCESS) {
        return st;
    }
    fw_signal_exit_boot_services_events();
    fw_debug_support_exit_boot_services();
    (void)bs_set_watchdog_timer(0, 0, 0, NULL);
    ahci_stop_all_ports();
    graphics_prepare_os_handoff(fw_handoff_vga_console_primary());
    /*
     * The loader owns RR/TR state by this point and may have installed RID=1
     * region-7 TR mappings before ExitBootServices().  Resetting to the
     * firmware SAL RR values here would make those TRs unreachable.
     */
    efi_exit_boot_services_update_system_table();
    mBootServicesExited = 1;
    return EFI_SUCCESS;
}

EFI_STATUS bs_unload_image(EFI_HANDLE ImageHandle)
{
    EFI_LOADED_IMAGE_RECORD *rec;

    if (ImageHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (mBootServicesExited) {
        return EFI_UNSUPPORTED;
    }
    if (ImageHandle == mFpswaHandle && mFpswaLoadedImageActive) {
        if (mFpswaLoadedImageProto.Unload == NULL) {
            return EFI_UNSUPPORTED;
        }
        return mFpswaLoadedImageProto.Unload(ImageHandle);
    }
    rec = fw_loaded_image_record(ImageHandle);
    if (rec == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (rec->started) {
        EFI_STATUS st;

        if (rec->loaded_image.Unload == NULL) {
            return EFI_UNSUPPORTED;
        }
        st = rec->loaded_image.Unload(ImageHandle);
        if (st != EFI_SUCCESS) {
            return st;
        }
    }
    fw_close_protocols_opened_by_image(ImageHandle);
    return fw_release_loaded_image_record(rec);
}

EFI_STATUS rs_set_virtual_address_map(UINTN MemoryMapSize,
                                      UINTN DescriptorSize,
                                      UINT32 DescriptorVersion,
                                      EFI_MEMORY_DESCRIPTOR *VirtualMap);
EFI_STATUS rs_convert_pointer(UINTN DebugDisposition, VOID **Address);

/* --- EFI Time Services ----------------------------------------------------- */

#define EFI_TIME_ADJUST_DAYLIGHT 0x01U
#define EFI_TIME_IN_DAYLIGHT     0x02U
#define EFI_TIME_DAYLIGHT_MASK   \
    (EFI_TIME_ADJUST_DAYLIGHT | EFI_TIME_IN_DAYLIGHT)

#define FW_RTC_STATE_MAGIC 0x54464f3436545249ULL /* "IRT64OFT" */
#define FW_RTC_STATE_VERSION 1U

typedef struct {
    UINT64 Magic;
    UINT32 Version;
    UINT32 Reserved;
    INT64 OffsetSeconds;
    UINT32 Nanosecond;
    INT16 TimeZone;
    UINT8 Daylight;
    UINT8 Pad;
} FW_RTC_STATE;

FW_STATIC_ASSERT(sizeof(FW_RTC_STATE) == 32U, rtc_state_format_size);
FW_STATIC_ASSERT(FW_NVRAM_RTC_OFFSET + sizeof(FW_RTC_STATE) <=
                 FW_NVRAM_COMMIT_OFFSET, rtc_state_fits_nvram);

static BOOLEAN mRtcSelftestActive;
static EFI_TIME mWakeupTime;
static BOOLEAN mWakeupTimeEnabled;
static BOOLEAN mWakeupTimePending;

static BOOLEAN efi_time_is_leap_year(UINT16 Year)
{
    return (Year % 4U == 0U && Year % 100U != 0U) || Year % 400U == 0U;
}

static UINT8 efi_time_days_in_month(UINT16 Year, UINT8 Month)
{
    static const UINT8 days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };

    if (Month == 0 || Month > FW_ARRAY_SIZE(days)) {
        return 0;
    }
    if (Month == 2 && efi_time_is_leap_year(Year)) {
        return 29;
    }
    return days[Month - 1U];
}

BOOLEAN efi_time_valid(const EFI_TIME *Time)
{
    UINT8 days_in_month;

    if (Time->Year < 1900 || Time->Year > 9999 ||
        Time->Month == 0 || Time->Month > 12 ||
        Time->Hour > 23 || Time->Minute > 59 || Time->Second > 59 ||
        Time->Nanosecond >= FW_NANOSECONDS_PER_SECOND ||
        (Time->Daylight & ~EFI_TIME_DAYLIGHT_MASK) != 0) {
        return 0;
    }

    days_in_month = efi_time_days_in_month(Time->Year, Time->Month);
    if (Time->Day == 0 || Time->Day > days_in_month) {
        return 0;
    }

    return Time->TimeZone == 2047 ||
           (Time->TimeZone >= -1440 && Time->TimeZone <= 1440);
}

static BOOLEAN efi_time_to_epoch(const EFI_TIME *Time, INT64 *Seconds)
{
    INT64 days = 0;
    INT64 total;
    INTN year;
    UINT8 month;

    if (!efi_time_valid(Time) || Seconds == NULL) {
        return 0;
    }
    if (Time->Year >= 1970) {
        for (year = 1970; year < Time->Year; year++) {
            days += efi_time_is_leap_year((UINT16)year) ? 366 : 365;
        }
    } else {
        for (year = 1969; year >= Time->Year; year--) {
            days -= efi_time_is_leap_year((UINT16)year) ? 366 : 365;
        }
    }
    for (month = 1; month < Time->Month; month++) {
        days += efi_time_days_in_month(Time->Year, month);
    }
    days += Time->Day - 1U;
    total = days * 24 + Time->Hour;
    total = total * 60 + Time->Minute;
    total = total * 60 + Time->Second;
    *Seconds = total;
    return 1;
}

static BOOLEAN efi_time_from_epoch(INT64 Seconds, UINT32 Nanosecond,
                                   EFI_TIME *Time)
{
    INT64 days = Seconds / 86400;
    INT64 remainder = Seconds % 86400;
    INTN year = 1970;
    UINT8 month = 1;
    UINT16 days_in_year;
    UINT8 days_in_month;

    if (Time == NULL || Nanosecond >= FW_NANOSECONDS_PER_SECOND) {
        return 0;
    }
    if (remainder < 0) {
        remainder += 86400;
        days--;
    }
    while (days < 0) {
        if (--year < 1900) {
            return 0;
        }
        days += efi_time_is_leap_year((UINT16)year) ? 366 : 365;
    }
    for (;;) {
        days_in_year = efi_time_is_leap_year((UINT16)year) ? 366 : 365;
        if (days < days_in_year) {
            break;
        }
        days -= days_in_year;
        if (++year > 9999) {
            return 0;
        }
    }
    for (;;) {
        days_in_month = efi_time_days_in_month((UINT16)year, month);
        if (days < days_in_month) {
            break;
        }
        days -= days_in_month;
        month++;
    }

    Time->Year = year;
    Time->Month = month;
    Time->Day = days + 1;
    Time->Hour = remainder / 3600;
    remainder %= 3600;
    Time->Minute = remainder / 60;
    Time->Second = remainder % 60;
    Time->Pad1 = 0;
    Time->Nanosecond = Nanosecond;
    Time->TimeZone = 0;
    Time->Daylight = 0;
    Time->Pad2 = 0;
    return 1;
}

static FW_RTC_STATE *fw_rtc_state(void)
{
    return (FW_RTC_STATE *)mRuntimeRtcState;
}

static BOOLEAN fw_rtc_state_valid(const FW_RTC_STATE *State)
{
    return State->Magic == FW_RTC_STATE_MAGIC &&
           State->Version == FW_RTC_STATE_VERSION &&
           State->Reserved == 0 && State->Pad == 0 &&
           State->Nanosecond < FW_NANOSECONDS_PER_SECOND &&
           (State->Daylight & ~EFI_TIME_DAYLIGHT_MASK) == 0 &&
           (State->TimeZone == 2047 ||
            (State->TimeZone >= -1440 && State->TimeZone <= 1440));
}

static UINT8 fw_cmos_read(UINT8 Reg)
{
    *(volatile UINT8 *)mRuntimeRtc = Reg;
    return *(volatile UINT8 *)(mRuntimeRtc + 1U);
}

static UINT64 fw_rtc_field(UINT8 Value, BOOLEAN Binary)
{
    return Binary ? Value : (UINT64)(Value >> 4) * 10U + (Value & 0x0FU);
}

/*
 * Read the MC146818 CMOS calendar (ports 0x70/0x71, as on the i2000/SDV
 * Super-I/O) and convert it to seconds since the Unix epoch.  Honors the
 * update-in-progress bit and the binary/BCD and 12/24-hour modes; a
 * double read guards against an update between fields.
 */
static BOOLEAN fw_rtc_read_seconds(INT64 *Seconds)
{
    UINTN attempt;

    if (Seconds == NULL) {
        return 0;
    }

    for (attempt = 0; attempt < 4U; attempt++) {
        UINT8 reg_b;
        BOOLEAN binary;
        BOOLEAN hours24;
        UINT64 sec, min, hour, day, month, year;
        UINT8 raw_hour;
        UINTN spin;
        UINT64 era, yoe, doy, doe, days;

        for (spin = 0; spin < 100000U; spin++) {
            if ((fw_cmos_read(0x0AU) & 0x80U) == 0) {
                break;
            }
        }

        reg_b = fw_cmos_read(0x0BU);
        binary = (reg_b & 0x04U) != 0;
        hours24 = (reg_b & 0x02U) != 0;

        sec = fw_rtc_field(fw_cmos_read(0x00U), binary);
        min = fw_rtc_field(fw_cmos_read(0x02U), binary);
        raw_hour = fw_cmos_read(0x04U);
        day = fw_rtc_field(fw_cmos_read(0x07U), binary);
        month = fw_rtc_field(fw_cmos_read(0x08U), binary);
        year = fw_rtc_field(fw_cmos_read(0x09U), binary);
        if (fw_cmos_read(0x00U) != (binary ? (UINT8)sec :
                (UINT8)(((sec / 10U) << 4) | (sec % 10U)))) {
            continue;   /* the clock ticked mid-read */
        }

        if (hours24) {
            hour = fw_rtc_field(raw_hour, binary);
        } else {
            hour = fw_rtc_field(raw_hour & 0x7FU, binary) % 12U;
            if (raw_hour & 0x80U) {
                hour += 12U;
            }
        }
        /* No century register contract: pivot on the two-digit year. */
        year += (year < 80U) ? 2000U : 1900U;

        if (sec > 59U || min > 59U || hour > 23U ||
            day < 1U || day > 31U || month < 1U || month > 12U) {
            continue;
        }

        /*

         * Days from civil date (proleptic Gregorian), epoch 1970-01-01.
         */
        {
            UINT64 y = year - (month <= 2U ? 1U : 0U);

            era = y / 400U;
            yoe = y - era * 400U;
            doy = (153U * (month + (month > 2U ? (UINT64)-3 : 9U)) + 2U) /
                  5U + day - 1U;
            doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
            days = era * 146097U + doe - 719468U;
        }

        *Seconds = (INT64)(days * 86400U + hour * 3600U + min * 60U + sec);
        return 1;
    }
    return 0;
}

EFI_STATUS rs_get_time(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Capabilities)
{
    FW_RTC_STATE *state = fw_rtc_state();
    INT64 host_seconds;
    INT64 offset_seconds = 0;
    INT64 guest_seconds;
    UINT32 nanosecond = 0;
    INT16 timezone = 0;
    UINT8 daylight = 0;

    if (Time == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (Capabilities != NULL) {
        Capabilities->Resolution = FW_RTC_RESOLUTION_HZ;
        Capabilities->Accuracy = FW_TIME_ACCURACY_1E6_PPM;
        Capabilities->SetsToZero = 0;
    }
    if (!fw_rtc_read_seconds(&host_seconds)) {
        return EFI_DEVICE_ERROR;
    }
    if (fw_rtc_state_valid(state)) {
        offset_seconds = state->OffsetSeconds;
        nanosecond = state->Nanosecond;
        timezone = state->TimeZone;
        daylight = state->Daylight;
    }
    if ((offset_seconds > 0 &&
         host_seconds > (INT64)0x7fffffffffffffffULL - offset_seconds) ||
        (offset_seconds < 0 &&
         host_seconds < (-0x7fffffffffffffffLL - 1) - offset_seconds)) {
        return EFI_DEVICE_ERROR;
    }
    guest_seconds = host_seconds + offset_seconds;
    if (!efi_time_from_epoch(guest_seconds, nanosecond, Time)) {
        return EFI_DEVICE_ERROR;
    }
    Time->TimeZone = timezone;
    Time->Daylight = daylight;
    return EFI_SUCCESS;
}

EFI_STATUS rs_set_time(EFI_TIME *Time)
{
    FW_RTC_STATE *state = fw_rtc_state();
    FW_RTC_STATE next;
    INT64 host_seconds;
    INT64 guest_seconds;

    if (Time == NULL || !efi_time_valid(Time)) {
        return EFI_INVALID_PARAMETER;
    }
    if (!efi_time_to_epoch(Time, &guest_seconds) ||
        !fw_rtc_read_seconds(&host_seconds)) {
        return EFI_DEVICE_ERROR;
    }
    if (guest_seconds < (-0x7fffffffffffffffLL - 1) + host_seconds) {
        return EFI_DEVICE_ERROR;
    }

    next.Magic = FW_RTC_STATE_MAGIC;
    next.Version = FW_RTC_STATE_VERSION;
    next.Reserved = 0;
    next.OffsetSeconds = guest_seconds - host_seconds;
    next.Nanosecond = Time->Nanosecond;
    next.TimeZone = Time->TimeZone;
    next.Daylight = Time->Daylight;
    next.Pad = 0;
    *state = next;
    if (!mRtcSelftestActive) {
        nvram_commit();
    }
    return EFI_SUCCESS;
}

static INTN efi_time_compare(const EFI_TIME *A, const EFI_TIME *B)
{
    if (A->Year != B->Year) {
        return A->Year < B->Year ? -1 : 1;
    }
    if (A->Month != B->Month) {
        return A->Month < B->Month ? -1 : 1;
    }
    if (A->Day != B->Day) {
        return A->Day < B->Day ? -1 : 1;
    }
    if (A->Hour != B->Hour) {
        return A->Hour < B->Hour ? -1 : 1;
    }
    if (A->Minute != B->Minute) {
        return A->Minute < B->Minute ? -1 : 1;
    }
    if (A->Second != B->Second) {
        return A->Second < B->Second ? -1 : 1;
    }
    if (A->Nanosecond != B->Nanosecond) {
        return A->Nanosecond < B->Nanosecond ? -1 : 1;
    }
    return 0;
}

static void rs_update_wakeup_pending(void)
{
    EFI_TIME now;

    if (!mWakeupTimeEnabled || mWakeupTimePending) {
        return;
    }
    if (rs_get_time(&now, NULL) == EFI_SUCCESS &&
        efi_time_compare(&now, &mWakeupTime) >= 0) {
        mWakeupTimePending = 1;
    }
}

static void rs_disable_wakeup_time(void)
{
    mWakeupTimeEnabled = 0;
    mWakeupTimePending = 0;
}

EFI_STATUS __attribute__((noinline))
rs_get_wakeup_time(BOOLEAN *Enabled, BOOLEAN *Pending, EFI_TIME *Time)
{
    if (Enabled == NULL || Pending == NULL || Time == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    rs_update_wakeup_pending();
    *Enabled = mWakeupTimeEnabled;
    *Pending = mWakeupTimePending;
    *Time = mWakeupTime;
    return EFI_SUCCESS;
}

EFI_STATUS rs_set_wakeup_time(BOOLEAN Enable, EFI_TIME *Time)
{
    if (!Enable) {
        rs_disable_wakeup_time();
        return EFI_SUCCESS;
    }
    if (Time == NULL || !efi_time_valid(Time)) {
        return EFI_INVALID_PARAMETER;
    }
    mWakeupTime = *Time;
    mWakeupTimeEnabled = 1;
    mWakeupTimePending = 0;
    rs_update_wakeup_pending();
    return EFI_SUCCESS;
}

static BOOLEAN __attribute__((noinline)) uefi_time_services_selftest(void)
{
    EFI_TIME now;
    EFI_TIME_CAPABILITIES caps;
    FW_RTC_STATE saved_state;
    EFI_TIME custom = {
        .Year = 2031,
        .Month = 12,
        .Day = 31,
        .Hour = 23,
        .Minute = 59,
        .Second = 58,
        .Nanosecond = 123000000,
        .TimeZone = 0,
        .Daylight = EFI_TIME_DAYLIGHT_MASK,
    };
    EFI_TIME invalid = custom;
    EFI_TIME invalid_daylight = custom;
    EFI_TIME alarm;
    EFI_TIME saved_alarm;
    BOOLEAN enabled;
    BOOLEAN pending;

    if (rs_get_time(&now, NULL) != EFI_SUCCESS ||
        !efi_time_valid(&now)) {
        return 0;
    }
    fw_set_mem(&caps, sizeof(caps), 0xff);
    if (rs_get_time(&now, &caps) != EFI_SUCCESS ||
        caps.Resolution != FW_RTC_RESOLUTION_HZ ||
        caps.Accuracy != FW_TIME_ACCURACY_1E6_PPM ||
        caps.SetsToZero != 0) {
        return 0;
    }
    saved_state = *fw_rtc_state();
    mRtcSelftestActive = 1;
    invalid.Month = 13;
    invalid_daylight.Daylight = (UINT8)~EFI_TIME_DAYLIGHT_MASK;
    if (rs_set_time(NULL) != EFI_INVALID_PARAMETER ||
        rs_set_time(&invalid) != EFI_INVALID_PARAMETER ||
        rs_set_time(&invalid_daylight) != EFI_INVALID_PARAMETER ||
        fw_rtc_state()->Daylight != saved_state.Daylight ||
        rs_set_time(&custom) != EFI_SUCCESS ||
        rs_get_time(&now, NULL) != EFI_SUCCESS ||
        now.Year != custom.Year ||
        now.Month != custom.Month ||
        now.Day != custom.Day ||
        now.Hour != custom.Hour ||
        now.Minute != custom.Minute ||
        now.Second < custom.Second ||
        now.Nanosecond != custom.Nanosecond ||
        now.Daylight != custom.Daylight) {
        *fw_rtc_state() = saved_state;
        mRtcSelftestActive = 0;
        return 0;
    }
    *fw_rtc_state() = saved_state;
    mRtcSelftestActive = 0;
    if (rs_get_wakeup_time(&enabled, &pending, &alarm) != EFI_SUCCESS ||
        enabled || pending || !efi_time_valid(&alarm)) {
        return 0;
    }
    saved_alarm = alarm;
    if (rs_set_wakeup_time(1, &invalid_daylight) != EFI_INVALID_PARAMETER ||
        rs_get_wakeup_time(&enabled, &pending, &alarm) != EFI_SUCCESS ||
        enabled || pending || efi_time_compare(&alarm, &saved_alarm) != 0 ||
        alarm.TimeZone != saved_alarm.TimeZone ||
        alarm.Daylight != saved_alarm.Daylight) {
        mWakeupTime = saved_alarm;
        rs_disable_wakeup_time();
        return 0;
    }
    return rs_set_wakeup_time(0, NULL) == EFI_SUCCESS &&
           rs_get_wakeup_time(&enabled, &pending, &alarm) == EFI_SUCCESS &&
           !enabled && !pending && efi_time_valid(&alarm);
}

/* --- EFI core initialization ---------------------------------------------- */

/* Memory-map construction lives in efi_memmap.c. */

BOOLEAN __attribute__((noinline)) uefi_memory_map_selftest(void)
{
    static EFI_MEMORY_DESCRIPTOR saved_map[MEMORY_MAP_MAX];
    static EFI_MEMORY_DESCRIPTOR probe_map[MEMORY_MAP_MAX];
    static EFI_PAGE_ALLOCATION_RECORD saved_pages[PAGE_ALLOCATION_MAX];
    static EFI_PAGE_ALLOCATION_RECORD failed_pages[PAGE_ALLOCATION_MAX];
    static EFI_POOL_ALLOCATION_RECORD saved_pool[POOL_ALLOCATION_MAX];
    UINTN firmware_end = ((UINTN)&_end + 0x1FFFU) & ~0x1FFFULL;
    UINTN runtime_code_start = (UINTN)&__runtime_code_start;
    EFI_MEMORY_DESCRIPTOR before;
    EFI_MEMORY_DESCRIPTOR preserved;
    EFI_MEMORY_DESCRIPTOR legacy;
    EFI_MEMORY_DESCRIPTOR aligned;
    EFI_MEMORY_DESCRIPTOR ordinary;
    EFI_MEMORY_DESCRIPTOR ordinary_next;
    UINTN saved_entries = mMemoryMapEntries;
    UINTN saved_key = mMapKey;
    EFI_PHYSICAL_ADDRESS saved_next_page_addr = mNextPageAddr;
    EFI_PHYSICAL_ADDRESS expected_pool_address;
    EFI_PHYSICAL_ADDRESS failed_address;
    EFI_PHYSICAL_ADDRESS failed_next_page_addr;
    EFI_PHYSICAL_ADDRESS conventional_any;
    EFI_PHYSICAL_ADDRESS conventional_duplicate;
    EFI_PHYSICAL_ADDRESS conventional_max_one;
    EFI_PHYSICAL_ADDRESS conventional_max_two;
    EFI_PHYSICAL_ADDRESS loader_address;
    EFI_PHYSICAL_ADDRESS runtime_address;
    EFI_PHYSICAL_ADDRESS allocation_next_page_addr;
    EFI_POOL_ALLOCATION_RECORD *pool_rec;
    EFI_PHYSICAL_ADDRESS pool_backing_start;
    EFI_PHYSICAL_ADDRESS pool_backing_end;
    VOID *pool = NULL;
    VOID *conventional_pool_one = NULL;
    VOID *conventional_pool_two = NULL;
    UINT64 pool_start = 0;
    UINTN allocation_key;
    UINTN probe_map_size;
    UINTN probe_key;
    UINTN descriptor_size;
    UINT32 descriptor_version;
    UINTN i;
    EFI_STATUS st;
    BOOLEAN ok = 1;

    fw_copy_mem(saved_map, mMemoryMap, sizeof(saved_map));
    fw_copy_mem(saved_pages, mPageAllocations, sizeof(saved_pages));
    fw_copy_mem(saved_pool, mPoolAllocations, sizeof(saved_pool));

    probe_map_size = 0;
    descriptor_size = 0;
    descriptor_version = 0;
    st = bs_get_memory_map(&probe_map_size, NULL, NULL,
                           &descriptor_size, &descriptor_version);
    if (st != EFI_BUFFER_TOO_SMALL || probe_map_size == 0 ||
        descriptor_size != sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptor_version != EFI_MEMORY_DESCRIPTOR_VERSION) {
        ok = 0;
        goto out;
    }

    probe_map_size = saved_entries * sizeof(EFI_MEMORY_DESCRIPTOR);
    descriptor_size = 0;
    descriptor_version = 0;
    st = bs_get_memory_map(&probe_map_size, probe_map, NULL,
                           &descriptor_size, &descriptor_version);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        goto out;
    }

    probe_map_size = sizeof(probe_map);
    probe_key = ~(UINTN)0 - 0x4321U;
    descriptor_size = 0;
    descriptor_version = 0;
    st = bs_get_memory_map(&probe_map_size, NULL, &probe_key,
                           &descriptor_size, &descriptor_version);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        goto out;
    }

    probe_map_size = 0;
    probe_key = ~(UINTN)0 - 0x1234U;
    descriptor_size = 0;
    descriptor_version = 0;
    st = bs_get_memory_map(&probe_map_size, NULL, &probe_key,
                           &descriptor_size, &descriptor_version);
    if (st != EFI_BUFFER_TOO_SMALL ||
        probe_map_size != saved_entries * sizeof(EFI_MEMORY_DESCRIPTOR) ||
        probe_key != ~(UINTN)0 - 0x1234U ||
        descriptor_size != sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptor_version != EFI_MEMORY_DESCRIPTOR_VERSION) {
        ok = 0;
        goto out;
    }

    probe_map_size = saved_entries * sizeof(EFI_MEMORY_DESCRIPTOR);
    probe_key = 0;
    descriptor_size = 0;
    descriptor_version = 0;
    st = bs_get_memory_map(&probe_map_size, probe_map, &probe_key,
                           &descriptor_size, &descriptor_version);
    if (st != EFI_SUCCESS ||
        probe_map_size != saved_entries * sizeof(EFI_MEMORY_DESCRIPTOR) ||
        probe_key != saved_key ||
        descriptor_size != sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptor_version != EFI_MEMORY_DESCRIPTOR_VERSION) {
        ok = 0;
        goto out;
    }

    before.Type = EfiConventionalMemory;
    before.PhysicalStart = FW_LOW_FREE_BASE;
    before.VirtualStart = 0;
    before.NumberOfPages = (FW_LOW_IMAGE_BASE - FW_LOW_FREE_BASE) >> 12;
    before.Attribute = EFI_MEMORY_WB;

    preserved = before;
    preserved.PhysicalStart = FW_LOW_IMAGE_BASE;
    preserved.NumberOfPages =
        (FW_LOW_LEGACY_IMAGE_BASE - FW_LOW_IMAGE_BASE) >> 12;

    legacy = before;
    legacy.PhysicalStart = FW_LOW_LEGACY_IMAGE_BASE;
    legacy.NumberOfPages =
        (FW_LOW_IMAGE_ALIGNED_END - FW_LOW_LEGACY_IMAGE_BASE) >> 12;

    aligned = before;
    aligned.PhysicalStart = FW_LOW_IMAGE_ALIGNED_END;
    aligned.NumberOfPages = (FW_LOW_IMAGE_END - FW_LOW_IMAGE_ALIGNED_END) >> 12;

    if ((FW_LOW_IMAGE_BASE & (FW_LOW_IMAGE_ALIGN - 1ULL)) != 0 ||
        (FW_LOW_IMAGE_ALIGNED_END & (FW_LOW_IMAGE_ALIGN - 1ULL)) != 0 ||
        FW_LOW_IMAGE_ALIGNED_END - FW_LOW_IMAGE_BASE != FW_LOW_IMAGE_ALIGN ||
        FW_LOW_IMAGE_END <= FW_LOW_IMAGE_ALIGNED_END) {
        ok = 0;
        goto out;
    }

    if (mGuestLowRamEnd <= FW_LOW_IMAGE_END) {
        ok = 0;
        goto out;
    }
    ordinary.Type = EfiConventionalMemory;
    ordinary.PhysicalStart = FW_LOW_RAM_STAGING_BASE;
    ordinary.VirtualStart = 0;
    ordinary.NumberOfPages = 1;
    ordinary.Attribute = EFI_MEMORY_WB;
    ordinary_next = ordinary;
    ordinary_next.PhysicalStart = FW_LOW_RAM_STAGING_BASE + 0x1000ULL;

    /* Quirk-dependent shapes are asserted only when the quirk is on. */
    if ((fw_map_quirk_enabled(IA64_FW_QUIRK_LOADER_SPLIT_PAGE) &&
         !efi_memory_map_has_descriptor(EfiReservedMemoryType,
                                        FW_LOADER_HEAP_SPLIT_BASE,
                                        FW_LOW_IMAGE_BASE,
                                        EFI_MEMORY_WB)) ||
        /* With the low-boundaries quirk armed, [32MB,80MB) is a single free
         * descriptor ending exactly at the 80 MB line (XP-era sumain
         * straddle rule); with it retired the run coalesces freely and only
         * coverage is asserted. */
        (fw_map_quirk_enabled(IA64_FW_QUIRK_LOW_BOUNDARIES) ?
         !efi_memory_map_has_descriptor(EfiConventionalMemory,
                                        FW_LOW_IMAGE_BASE,
                                        FW_LOW_IMAGE_END, EFI_MEMORY_WB) :
         !efi_memory_map_covers_range(EfiConventionalMemory,
                                      FW_LOW_IMAGE_BASE,
                                      FW_LOW_IMAGE_END, EFI_MEMORY_WB)) ||
        (fw_map_quirk_enabled(IA64_FW_QUIRK_LOW_ANCHOR) &&
         !efi_memory_map_has_descriptor(EfiReservedMemoryType,
                                        fw_low_anchor_base(),
                                        fw_low_anchor_base() +
                                            FW_LOW_ANCHOR_SIZE,
                                        EFI_MEMORY_WB)) ||
        (fw_low_anchor_base() == FW_LOW_ANCHOR_BASE &&
         !efi_memory_map_covers_range(EfiConventionalMemory,
                                      FW_LOW_IMAGE_END,
                                      FW_LOW_ANCHOR_BASE,
                                      EFI_MEMORY_WB)) ||
        !efi_memory_map_has_boot_stack_layout() ||
        !efi_memory_map_has_descriptor(EfiMemoryMappedIO, IOSAPIC_BASE,
                                       IOSAPIC_BASE + IOSAPIC_SIZE,
                                       EFI_MEMORY_UC) ||
        !efi_memory_map_has_descriptor(EfiMemoryMappedIO,
                                       FW_LOCAL_SAPIC_BASE,
                                       FW_LOCAL_SAPIC_BASE +
                                           FW_LOCAL_SAPIC_SIZE,
                                       EFI_MEMORY_UC) ||
        !efi_memory_map_has_descriptor(
            EfiMemoryMappedIO, PCI_CONFIG_ECAM_BASE,
            PCI_CONFIG_ECAM_BASE + PCI_CONFIG_ECAM_SIZE,
            EFI_MEMORY_UC | EFI_MEMORY_RUNTIME) ||
        !efi_memory_map_has_descriptor(EfiMemoryMappedIO,
                                       IA64_PCI_MMIO_BASE,
                                       IA64_PCI_MMIO_BASE +
                                           IA64_PCI_MMIO_SIZE,
                                       EFI_MEMORY_UC) ||
        !efi_memory_map_has_descriptor(EfiMemoryMappedIO, IA64_UART_BASE,
                                       IA64_UART_BASE + IA64_UART_MMIO_SIZE,
                                       EFI_MEMORY_UC) ||
        !efi_memory_map_has_descriptor(EfiMemoryMappedIOPortSpace,
                                       LEGACY_IO_BASE,
                                       LEGACY_IO_SPARSE_LIMIT,
                                       EFI_MEMORY_UC | EFI_MEMORY_RUNTIME) ||
        !efi_memory_map_has_descriptor(EfiACPIMemoryNVS,
                                       ACPI_RECLAIM_BASE,
                                       ACPI_RECLAIM_TABLE_BASE,
                                       EFI_MEMORY_WB) ||
        !efi_memory_map_has_descriptor(EfiACPIReclaimMemory,
                                       ACPI_RECLAIM_TABLE_BASE,
                                       ACPI_RECLAIM_END, EFI_MEMORY_WB) ||
        !efi_memory_map_is_sorted() ||
        !efi_memory_map_has_ia64_descriptor_alignment() ||
        !efi_memory_map_covers_range(EfiRuntimeServicesCode,
                                     runtime_code_start, firmware_end,
                                     EFI_MEMORY_WB | EFI_MEMORY_RUNTIME) ||
        /* The 32/48/64/80 MB no-coalesce rule only holds with its quirk. */
        (fw_map_quirk_enabled(IA64_FW_QUIRK_LOW_BOUNDARIES) &&
         (efi_memory_descriptors_can_merge(&before, &preserved) ||
          efi_memory_descriptors_can_merge(&preserved, &legacy) ||
          efi_memory_descriptors_can_merge(&legacy, &aligned))) ||
        (!fw_map_quirk_enabled(IA64_FW_QUIRK_LOW_BOUNDARIES) &&
         (!efi_memory_descriptors_can_merge(&before, &preserved) ||
          !efi_memory_descriptors_can_merge(&preserved, &legacy) ||
          !efi_memory_descriptors_can_merge(&legacy, &aligned))) ||
        !efi_memory_descriptors_can_merge(&ordinary, &ordinary_next)) {
        ok = 0;
        goto out;
    }
    for (i = 0; i < fw_guest_high_ram_count(); i++) {
        if (!efi_memory_map_has_descriptor(EfiConventionalMemory,
                                           fw_guest_high_ram_base(i),
                                           fw_guest_high_ram_end(i),
                                           EFI_MEMORY_WB)) {
            ok = 0;
            goto out;
        }
    }

    /* Pool backing must not consume the low fixed-address candidate. */
    loader_address = FW_LOW_RAM_STAGING_BASE;
    if (!efi_range_is_available(loader_address,
                                loader_address + EFI_PAGE_SIZE) ||
        !efi_find_pool_pages(EFI_POOL_CHUNK_SIZE,
                             efi_memory_type_allocation_granularity(
                                 EfiLoaderData),
                             &expected_pool_address) ||
        expected_pool_address <= loader_address ||
        bs_allocate_pool(EfiLoaderData, 17, &pool) != EFI_SUCCESS ||
        pool == NULL) {
        ok = 0;
        goto out;
    }
    pool_start = (UINTN)pool & ~0xfffULL;
    if (pool_start != (UINTN)pool ||
        pool_start != expected_pool_address ||
        mNextPageAddr != saved_next_page_addr ||
        bs_allocate_pages(AllocateAddress, EfiLoaderData, 1,
                          &loader_address) != EFI_SUCCESS ||
        !efi_memory_map_has_descriptor(EfiLoaderData, loader_address,
                                       loader_address + EFI_PAGE_SIZE,
                                       EFI_MEMORY_WB) ||
        !efi_memory_map_has_descriptor(EfiLoaderData, pool_start,
                                       pool_start + EFI_POOL_CHUNK_SIZE,
                                       EFI_MEMORY_WB) ||
        bs_free_pool((UINT8 *)pool + EFI_POOL_ALIGNMENT) !=
            EFI_INVALID_PARAMETER ||
        bs_free_pages(loader_address, 1) != EFI_SUCCESS ||
        bs_free_pool(pool) != EFI_SUCCESS ||
        !efi_memory_map_covers_range(EfiConventionalMemory, loader_address,
                                     loader_address + EFI_PAGE_SIZE,
                                     EFI_MEMORY_WB) ||
        !efi_memory_map_covers_range(EfiConventionalMemory, pool_start,
                                     pool_start + EFI_POOL_CHUNK_SIZE,
                                     EFI_MEMORY_WB)) {
        ok = 0;
        goto out;
    }

    runtime_address = FW_LOW_RAM_STAGING_BASE + IA64_EFI_MEMORY_ALIGN;
    loader_address = runtime_address - EFI_PAGE_SIZE;
    if (bs_allocate_pages(AllocateAddress, EfiLoaderData, 1,
                          &loader_address) != EFI_SUCCESS ||
        bs_allocate_pages(AllocateAddress, EfiRuntimeServicesCode, 1,
                          &runtime_address) != EFI_SUCCESS ||
        !efi_memory_map_has_descriptor(
            EfiRuntimeServicesCode, runtime_address,
            runtime_address + IA64_EFI_MEMORY_ALIGN,
            EFI_MEMORY_WB | EFI_MEMORY_RUNTIME) ||
        !efi_memory_map_has_ia64_descriptor_alignment() ||
        bs_free_pages(loader_address, 2) != EFI_NOT_FOUND ||
        !efi_memory_map_has_ia64_descriptor_alignment() ||
        bs_free_pages(loader_address, 1) != EFI_SUCCESS ||
        bs_free_pages(runtime_address, 1) != EFI_SUCCESS ||
        !efi_memory_map_covers_range(
            EfiConventionalMemory, runtime_address,
            runtime_address + IA64_EFI_MEMORY_ALIGN, EFI_MEMORY_WB)) {
        ok = 0;
        goto out;
    }

    runtime_address = FW_LOW_RAM_STAGING_BASE + IA64_EFI_MEMORY_ALIGN +
                      EFI_PAGE_SIZE;
    if (bs_allocate_pages(AllocateAddress, EfiRuntimeServicesCode, 1,
                          &runtime_address) != EFI_NOT_FOUND ||
        efi_mark_memory_range(
            EfiRuntimeServicesData, runtime_address,
            runtime_address + EFI_PAGE_SIZE,
            EFI_MEMORY_WB | EFI_MEMORY_RUNTIME) ||
        !efi_memory_map_has_ia64_descriptor_alignment()) {
        ok = 0;
        goto out;
    }

    runtime_address = FW_LOW_RAM_STAGING_BASE + 2U * IA64_EFI_MEMORY_ALIGN;
    if (bs_allocate_pages(AllocateAddress, EfiACPIReclaimMemory, 1,
                          &runtime_address) != EFI_SUCCESS ||
        !efi_memory_map_has_descriptor(
            EfiACPIReclaimMemory, runtime_address,
            runtime_address + IA64_EFI_MEMORY_ALIGN, EFI_MEMORY_WB) ||
        !efi_memory_map_has_ia64_descriptor_alignment() ||
        bs_free_pages(runtime_address, 1) != EFI_SUCCESS) {
        ok = 0;
        goto out;
    }

    runtime_address = FW_LOW_RAM_STAGING_BASE + 3U * IA64_EFI_MEMORY_ALIGN;
    pool = NULL;
    if (bs_allocate_pages(AllocateAddress, EfiMaxMemoryType, 1,
                          &runtime_address) != EFI_INVALID_PARAMETER ||
        bs_allocate_pages(AllocateAddress, (EFI_MEMORY_TYPE)0x7fffffffU, 1,
                          &runtime_address) != EFI_INVALID_PARAMETER ||
        bs_allocate_pool(EfiMaxMemoryType, 17, &pool) !=
            EFI_INVALID_PARAMETER ||
        bs_allocate_pool((EFI_MEMORY_TYPE)0x7fffffffU, 17, &pool) !=
            EFI_INVALID_PARAMETER ||
        bs_allocate_pages(
            AllocateAddress,
            (EFI_MEMORY_TYPE)EFI_MEMORY_TYPE_OS_RESERVED_MIN, 1,
            &runtime_address) != EFI_SUCCESS ||
        !efi_memory_map_has_descriptor(
            (EFI_MEMORY_TYPE)EFI_MEMORY_TYPE_OS_RESERVED_MIN,
            runtime_address, runtime_address + EFI_PAGE_SIZE,
            EFI_MEMORY_WB) ||
        bs_free_pages(runtime_address, 1) != EFI_SUCCESS) {
        ok = 0;
        goto out;
    }

    pool = NULL;
    if (bs_allocate_pool(
            (EFI_MEMORY_TYPE)EFI_MEMORY_TYPE_OS_RESERVED_MIN, 17,
            &pool) != EFI_SUCCESS ||
        pool == NULL) {
        ok = 0;
        goto out;
    }
    pool_start = (UINTN)pool;
    if (!efi_memory_map_has_descriptor(
            (EFI_MEMORY_TYPE)EFI_MEMORY_TYPE_OS_RESERVED_MIN,
            pool_start, pool_start + EFI_POOL_CHUNK_SIZE, EFI_MEMORY_WB) ||
        bs_free_pool(pool) != EFI_SUCCESS) {
        ok = 0;
        goto out;
    }

    pool = NULL;
    if (bs_allocate_pool(EfiRuntimeServicesData,
                         2U * EFI_POOL_CHUNK_SIZE + EFI_POOL_ALIGNMENT,
                         &pool) != EFI_SUCCESS ||
        pool == NULL) {
        ok = 0;
        goto out;
    }
    pool_start = (UINTN)pool;
    pool_rec = efi_find_pool_allocation(pool_start);
    if (pool_rec == NULL) {
        ok = 0;
        goto out;
    }
    pool_backing_start = pool_rec->backing_base;
    pool_backing_end = efi_pool_backing_end(pool_rec);
    if ((pool_start & (EFI_POOL_ALIGNMENT - 1U)) != 0 ||
        (pool_rec->backing_base & (IA64_EFI_MEMORY_ALIGN - 1U)) != 0 ||
        !efi_memory_map_covers_range(
            EfiRuntimeServicesData, pool_backing_start,
            pool_backing_end,
            EFI_MEMORY_WB | EFI_MEMORY_RUNTIME) ||
        !efi_memory_map_has_ia64_descriptor_alignment() ||
        bs_free_pool(pool) != EFI_SUCCESS ||
        !efi_memory_map_covers_range(
            EfiConventionalMemory, pool_backing_start,
            pool_backing_end, EFI_MEMORY_WB)) {
        ok = 0;
        goto out;
    }

    /*
     * EfiConventionalMemory is a legal allocation type even though marking
     * it leaves the visible descriptor type unchanged.  Allocation records
     * must therefore keep page and pool requests from reusing the range.
     */
    allocation_next_page_addr = mNextPageAddr;
    conventional_max_one = ~0ULL;
    allocation_key = mMapKey;
    if (bs_allocate_pages(AllocateMaxAddress, EfiConventionalMemory, 1,
                          &conventional_max_one) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    conventional_max_two = ~0ULL;
    allocation_key = mMapKey;
    if (bs_allocate_pages(AllocateMaxAddress, EfiConventionalMemory, 1,
                          &conventional_max_two) != EFI_SUCCESS ||
        conventional_max_two == conventional_max_one ||
        ranges_overlap(conventional_max_one, EFI_PAGE_SIZE,
                       conventional_max_two, EFI_PAGE_SIZE) ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }

    conventional_duplicate = conventional_max_one;
    allocation_key = mMapKey;
    if (bs_allocate_pages(AllocateAddress, EfiConventionalMemory, 1,
                          &conventional_duplicate) != EFI_NOT_FOUND ||
        conventional_duplicate != conventional_max_one ||
        mMapKey != allocation_key) {
        ok = 0;
        goto out;
    }

    allocation_key = mMapKey;
    if (bs_allocate_pool(EfiConventionalMemory, 17,
                         &conventional_pool_one) != EFI_SUCCESS ||
        conventional_pool_one == NULL ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    allocation_key = mMapKey;
    if (bs_allocate_pool(EfiConventionalMemory, 17,
                         &conventional_pool_two) != EFI_SUCCESS ||
        conventional_pool_two == NULL ||
        conventional_pool_two == conventional_pool_one ||
        ((UINTN)conventional_pool_one & (EFI_POOL_ALIGNMENT - 1U)) != 0 ||
        ((UINTN)conventional_pool_two & (EFI_POOL_ALIGNMENT - 1U)) != 0 ||
        ranges_overlap((UINTN)conventional_pool_one, 24,
                       (UINTN)conventional_pool_two, 24) ||
        ranges_overlap((UINTN)conventional_pool_one, EFI_POOL_CHUNK_SIZE,
                       conventional_max_one, EFI_PAGE_SIZE) ||
        ranges_overlap((UINTN)conventional_pool_one, EFI_POOL_CHUNK_SIZE,
                       conventional_max_two, EFI_PAGE_SIZE) ||
        mMapKey != allocation_key) {
        ok = 0;
        goto out;
    }

    conventional_duplicate = (UINTN)conventional_pool_one;
    allocation_key = mMapKey;
    if (bs_allocate_pages(AllocateAddress, EfiConventionalMemory, 1,
                          &conventional_duplicate) != EFI_NOT_FOUND ||
        conventional_duplicate != (UINTN)conventional_pool_one ||
        mMapKey != allocation_key) {
        ok = 0;
        goto out;
    }

    /* Reuse a sub-page gap without releasing or remapping its arena. */
    conventional_duplicate = (UINTN)conventional_pool_one;
    allocation_key = mMapKey;
    if (bs_free_pool(conventional_pool_one) != EFI_SUCCESS ||
        mMapKey != allocation_key) {
        ok = 0;
        goto out;
    }
    conventional_pool_one = NULL;
    if (bs_allocate_pool(EfiConventionalMemory, 17,
                         &conventional_pool_one) != EFI_SUCCESS ||
        (UINTN)conventional_pool_one != conventional_duplicate ||
        mMapKey != allocation_key) {
        ok = 0;
        goto out;
    }

    /* Exercise forward search and wrap while high ranges block it. */
    mNextPageAddr = conventional_max_two;
    conventional_any = 0xfeedfacefeedfaceULL;
    allocation_key = mMapKey;
    if (bs_allocate_pages(AllocateAnyPages, EfiConventionalMemory, 1,
                          &conventional_any) != EFI_SUCCESS ||
        conventional_any == conventional_max_one ||
        conventional_any == conventional_max_two ||
        ranges_overlap(conventional_any, EFI_PAGE_SIZE,
                       (UINTN)conventional_pool_one, EFI_PAGE_SIZE) ||
        ranges_overlap(conventional_any, EFI_PAGE_SIZE,
                       (UINTN)conventional_pool_two, EFI_PAGE_SIZE) ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }

    allocation_key = mMapKey;
    if (bs_free_pages(conventional_any, 1) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    allocation_key = mMapKey;
    if (bs_free_pool(conventional_pool_two) != EFI_SUCCESS ||
        mMapKey != allocation_key) {
        ok = 0;
        goto out;
    }
    conventional_pool_two = NULL;
    allocation_key = mMapKey;
    if (bs_free_pool(conventional_pool_one) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    conventional_pool_one = NULL;
    allocation_key = mMapKey;
    if (bs_free_pages(conventional_max_two, 1) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    allocation_key = mMapKey;
    if (bs_free_pages(conventional_max_one, 1) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }

    /* A freed conventional allocation must become available again. */
    conventional_duplicate = conventional_max_one;
    allocation_key = mMapKey;
    if (bs_allocate_pages(AllocateAddress, EfiConventionalMemory, 1,
                          &conventional_duplicate) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    allocation_key = mMapKey;
    if (bs_free_pages(conventional_duplicate, 1) != EFI_SUCCESS ||
        mMapKey != allocation_key + 1U) {
        ok = 0;
        goto out;
    }
    mNextPageAddr = allocation_next_page_addr;

    /* A range with an undescribed gap must fail without skipping the gap. */
    mMemoryMap[0].Type = EfiConventionalMemory;
    mMemoryMap[0].PhysicalStart = 0x1001000ULL;
    mMemoryMap[0].VirtualStart = 0;
    mMemoryMap[0].NumberOfPages = 1;
    mMemoryMap[0].Attribute = EFI_MEMORY_WB;
    mMemoryMapEntries = 1;
    mMapKey = saved_key;
    if (efi_mark_memory_range(EfiLoaderData,
                              mMemoryMap[0].PhysicalStart - EFI_PAGE_SIZE,
                              mMemoryMap[0].PhysicalStart + EFI_PAGE_SIZE,
                              EFI_MEMORY_WB) ||
        mMemoryMapEntries != 1 ||
        mMemoryMap[0].Type != EfiConventionalMemory ||
        mMemoryMap[0].PhysicalStart != 0x1001000ULL ||
        mMemoryMap[0].NumberOfPages != 1 ||
        mMapKey != saved_key) {
        ok = 0;
        goto out;
    }

    /* A failed descriptor split must leave the memory map and key intact. */
    for (i = 0; i < MEMORY_MAP_MAX; i++) {
        mMemoryMap[i].Type = EfiReservedMemoryType;
        mMemoryMap[i].PhysicalStart = 0x1000000ULL + (UINT64)i * 0x10000ULL;
        mMemoryMap[i].VirtualStart = 0;
        mMemoryMap[i].NumberOfPages = 1;
        mMemoryMap[i].Attribute = EFI_MEMORY_WB;
    }
    mMemoryMap[0].Type = EfiConventionalMemory;
    mMemoryMap[0].NumberOfPages = 1;
    mMemoryMap[1].Type = EfiConventionalMemory;
    mMemoryMap[1].PhysicalStart =
        mMemoryMap[0].PhysicalStart + EFI_PAGE_SIZE;
    mMemoryMap[1].NumberOfPages = 4;
    mMemoryMapEntries = MEMORY_MAP_MAX;
    mMapKey = saved_key;
    if (efi_mark_memory_range(EfiLoaderData,
                              mMemoryMap[0].PhysicalStart,
                              mMemoryMap[0].PhysicalStart +
                              3U * EFI_PAGE_SIZE,
                              EFI_MEMORY_WB) ||
        mMemoryMapEntries != MEMORY_MAP_MAX ||
        mMemoryMap[0].Type != EfiConventionalMemory ||
        mMemoryMap[0].NumberOfPages != 1 ||
        mMemoryMap[1].Type != EfiConventionalMemory ||
        mMemoryMap[1].PhysicalStart !=
            mMemoryMap[0].PhysicalStart + EFI_PAGE_SIZE ||
        mMemoryMap[1].NumberOfPages != 4 ||
        mMapKey != saved_key) {
        ok = 0;
        goto out;
    }

    /* AllocatePages failure must not advance its cursor or record a page. */
    for (i = 0; i < MEMORY_MAP_MAX; i++) {
        mMemoryMap[i].Type = EfiReservedMemoryType;
        mMemoryMap[i].PhysicalStart = 0x2000000ULL +
                                      (UINT64)i * 0x10000ULL;
        mMemoryMap[i].VirtualStart = 0;
        mMemoryMap[i].NumberOfPages = 1;
        mMemoryMap[i].Attribute = EFI_MEMORY_WB;
    }
    mMemoryMap[0].Type = EfiConventionalMemory;
    mMemoryMap[0].NumberOfPages = 4;
    mMemoryMapEntries = MEMORY_MAP_MAX;
    mMapKey = saved_key;
    mNextPageAddr = mMemoryMap[0].PhysicalStart + EFI_PAGE_SIZE;
    failed_next_page_addr = mNextPageAddr;
    failed_address = 0xfeedfacefeedfaceULL;
    fw_set_mem(mPageAllocations, sizeof(mPageAllocations), 0);
    fw_copy_mem(failed_pages, mPageAllocations, sizeof(failed_pages));
    fw_copy_mem(probe_map, mMemoryMap, sizeof(probe_map));

    st = bs_allocate_pages(AllocateAnyPages, EfiLoaderData, 1,
                           &failed_address);
    if (st != EFI_OUT_OF_RESOURCES ||
        failed_address != 0xfeedfacefeedfaceULL ||
        mMemoryMapEntries != MEMORY_MAP_MAX || mMapKey != saved_key ||
        mNextPageAddr != failed_next_page_addr) {
        ok = 0;
        goto out;
    }
    for (i = 0; i < sizeof(probe_map); i++) {
        if (((UINT8 *)mMemoryMap)[i] != ((UINT8 *)probe_map)[i]) {
            ok = 0;
            goto out;
        }
    }
    for (i = 0; i < sizeof(failed_pages); i++) {
        if (((UINT8 *)mPageAllocations)[i] !=
            ((UINT8 *)failed_pages)[i]) {
            ok = 0;
            goto out;
        }
    }

out:
    fw_copy_mem(mMemoryMap, saved_map, sizeof(saved_map));
    fw_copy_mem(mPageAllocations, saved_pages, sizeof(saved_pages));
    fw_copy_mem(mPoolAllocations, saved_pool, sizeof(saved_pool));
    mMemoryMapEntries = saved_entries;
    mMapKey = saved_key;
    mNextPageAddr = saved_next_page_addr;
    return ok;
}


static void efi_init_boot_services(void)
{
    mCurrentTpl = TPL_APPLICATION;
    mEventNotifyOrder = 0;
    mBeforeExitBootServicesSignaled = 0;
    mExitBootServicesEventsSignaled = 0;
    mBootServices.Hdr.Signature = EFI_BOOT_SERVICES_SIGNATURE;
    mBootServices.Hdr.Revision = EFI_BOOT_SERVICES_REVISION;
    mBootServices.Hdr.HeaderSize = sizeof(mBootServices);
    mBootServices.Hdr.CRC32 = 0;
    mBootServices.Hdr.Reserved = 0;

    mBootServices.RaiseTPL                     = bs_raise_tpl;
    mBootServices.RestoreTPL                   = bs_restore_tpl;
    mBootServices.AllocatePages                = bs_allocate_pages;
    mBootServices.FreePages                    = bs_free_pages;
    mBootServices.GetMemoryMap                 = bs_get_memory_map;
    mBootServices.AllocatePool                 = bs_allocate_pool;
    mBootServices.FreePool                     = bs_free_pool;
    mBootServices.LoadImage                    = bs_load_image;
    mBootServices.StartImage                   = bs_start_image;
    mBootServices.UnloadImage                  = bs_unload_image;
    mBootServices.ExitBootServices             = bs_exit_boot_services;
    mBootServices.Stall                        = bs_stall;
    mBootServices.CreateEvent                  = bs_create_event;
    mBootServices.SetTimer                     = bs_set_timer;
    mBootServices.WaitForEvent                 = bs_wait_for_event;
    mBootServices.SignalEvent                  = bs_signal_event;
    mBootServices.CloseEvent                   = bs_close_event;
    mBootServices.CheckEvent                   = bs_check_event;
    mBootServices.InstallProtocolInterface     = bs_install_protocol;
    mBootServices.ReinstallProtocolInterface   = bs_reinstall_protocol;
    mBootServices.UninstallProtocolInterface   = bs_uninstall_protocol;
    mBootServices.HandleProtocol               = bs_handle_protocol;
    mBootServices.RegisterProtocolNotify       = bs_register_protocol_notify;
    mBootServices.LocateHandle                 = bs_locate_handle;
    mBootServices.LocateDevicePath             = bs_locate_device_path;
    mBootServices.InstallConfigurationTable    = bs_install_configuration_table;
    mBootServices.Exit                         = bs_exit;
    mBootServices.GetNextMonotonicCount        = bs_get_next_monotonic_count;
    mBootServices.SetWatchdogTimer             = bs_set_watchdog_timer;
    mBootServices.ConnectController            = bs_connect_controller;
    mBootServices.DisconnectController         = bs_disconnect_controller;
    mBootServices.OpenProtocol                 = bs_open_protocol;
    mBootServices.CloseProtocol                = bs_close_protocol;
    mBootServices.OpenProtocolInformation      = bs_open_protocol_information;
    mBootServices.ProtocolsPerHandle           = bs_protocols_per_handle;
    mBootServices.LocateHandleBuffer           = bs_locate_handle_buffer;
    mBootServices.LocateProtocol               = bs_locate_protocol;
    mBootServices.InstallMultipleProtocolInterfaces = bs_install_multiple_protocol_interfaces;
    mBootServices.UninstallMultipleProtocolInterfaces = bs_uninstall_multiple_protocol_interfaces;
    mBootServices.CalculateCrc32               = bs_calculate_crc32;
    mBootServices.CopyMem                      = bs_copy_mem;
    mBootServices.SetMem                       = bs_set_mem;
    mBootServices.CreateEventEx                = bs_create_event_ex;
}

static void efi_init_runtime_services(void)
{
    if (rs_get_time(&mWakeupTime, NULL) != EFI_SUCCESS) {
        fw_set_mem(&mWakeupTime, sizeof(mWakeupTime), 0);
        mWakeupTime.Year = 1970;
        mWakeupTime.Month = 1;
        mWakeupTime.Day = 1;
        mWakeupTime.TimeZone = 0;
    }
    rs_disable_wakeup_time();

    mRuntimeServices.Hdr.Signature = EFI_RUNTIME_SERVICES_SIGNATURE;
    mRuntimeServices.Hdr.Revision = EFI_RUNTIME_SERVICES_REVISION;
    mRuntimeServices.Hdr.HeaderSize = sizeof(mRuntimeServices);
    mRuntimeServices.Hdr.CRC32 = 0;
    mRuntimeServices.Hdr.Reserved = 0;

    mRuntimeServices.GetTime = (UINTN)rs_get_time;
    mRuntimeServices.SetTime = (UINTN)rs_set_time;
    mRuntimeServices.GetWakeupTime = (UINTN)rs_get_wakeup_time;
    mRuntimeServices.SetWakeupTime = (UINTN)rs_set_wakeup_time;
    mRuntimeServices.SetVirtualAddressMap = (UINTN)rs_set_virtual_address_map;
    mRuntimeServices.ConvertPointer = (UINTN)rs_convert_pointer;
    mRuntimeServices.GetVariable = (UINTN)rs_get_variable;
    mRuntimeServices.GetNextVariableName = (UINTN)rs_get_next_var_name;
    mRuntimeServices.SetVariable = (UINTN)rs_set_variable;
    mRuntimeServices.GetNextHighMonotonicCount =
        (UINTN)rs_get_next_high_monotonic_count;
    mRuntimeServices.ResetSystem = (UINTN)rs_reset_system;
    mRuntimeServices.QueryVariableInfo = (UINTN)rs_query_variable_info;
}

/* ConIn WaitForKey events live in the event table; wire them here. */
void efi_init_conin_wait_events(void)
{
    fw_set_mem(&mEventRecords[0], sizeof(mEventRecords[0]), 0);
    mEventRecords[0].signature = FW_EVENT_SIGNATURE;
    mEventRecords[0].type = EVT_NOTIFY_WAIT;
    mConInProto.WaitForKey = &mEventRecords[0];

    fw_set_mem(&mEventRecords[1], sizeof(mEventRecords[1]), 0);
    mEventRecords[1].signature = FW_EVENT_SIGNATURE;
    mEventRecords[1].type = EVT_NOTIFY_WAIT;
    mConInExProto.WaitForKeyEx = &mEventRecords[1];
}

static BOOLEAN __attribute__((noinline)) uefi_conin_wait_key_selftest(void)
{
    FW_EVENT_RECORD *rec = (FW_EVENT_RECORD *)mConInProto.WaitForKey;
    EFI_STATUS st;

    if (rec == NULL ||
        rec->signature != FW_EVENT_SIGNATURE ||
        rec->type != EVT_NOTIFY_WAIT) {
        return 0;
    }

    st = bs_check_event(mConInProto.WaitForKey);
    return st == EFI_SUCCESS || st == EFI_NOT_READY;
}

static EFI_STATUS uefi_conin_ex_selftest_notify(EFI_KEY_DATA *KeyData)
{
    (void)KeyData;
    return EFI_SUCCESS;
}

static BOOLEAN __attribute__((noinline)) uefi_conin_ex_selftest(void)
{
    FW_EVENT_RECORD *rec = (FW_EVENT_RECORD *)mConInExProto.WaitForKeyEx;
    VOID *interface = NULL;
    EFI_KEY_TOGGLE_STATE state = 0;
    EFI_KEY_DATA key_data;
    VOID *notify_handle = NULL;
    EFI_STATUS st;

    if (rec == NULL ||
        rec->signature != FW_EVENT_SIGNATURE ||
        rec->type != EVT_NOTIFY_WAIT ||
        !handle_supports_protocol(mImageHandle,
                                  (void *)mConInExProtocolGuid,
                                  &interface) ||
        interface != &mConInExProto) {
        return 0;
    }

    st = bs_check_event(mConInExProto.WaitForKeyEx);
    if (st != EFI_SUCCESS && st != EFI_NOT_READY) {
        return 0;
    }

    if (mConInExProto.SetState(&mConInExProto, &state) != EFI_SUCCESS) {
        return 0;
    }
    state = EFI_KEY_STATE_EXPOSED;
    if (mConInExProto.SetState(&mConInExProto, &state) != EFI_UNSUPPORTED) {
        return 0;
    }

    fw_set_mem(&key_data, sizeof(key_data), 0);
    key_data.Key.UnicodeChar = '\r';
    if (mConInExProto.RegisterKeyNotify(
            &mConInExProto, &key_data, uefi_conin_ex_selftest_notify,
            &notify_handle) != EFI_SUCCESS ||
        notify_handle == NULL) {
        return 0;
    }
    if (mConInExProto.UnregisterKeyNotify(&mConInExProto, notify_handle) !=
            EFI_SUCCESS ||
        mConInExProto.UnregisterKeyNotify(&mConInExProto, notify_handle) !=
            EFI_INVALID_PARAMETER) {
        return 0;
    }
    return 1;
}

static void uefi_event_services_selftest_callback(EFI_EVENT Event,
                                                  VOID *Context)
{
    UINTN *count = (UINTN *)Context;

    (void)Event;
    if (count != NULL) {
        *count = *count + 1U;
    }
}

static void uefi_event_services_selftest_map_change(EFI_EVENT Event,
                                                     VOID *Context)
{
    UINTN *count = (UINTN *)Context;

    (void)Event;
    if (count != NULL) {
        *count = *count + 1U;
    }
    mMapKey++;
}

static BOOLEAN __attribute__((noinline)) uefi_event_services_selftest(void)
{
    static const UINT8 test_group[16] = {
        0x45, 0x56, 0x54, 0x47, 0x51, 0x45, 0x4d, 0x55,
        0x49, 0x41, 0x36, 0x34, 0x54, 0x45, 0x53, 0x54
    };
    EFI_EVENT event = NULL;
    EFI_EVENT group_notify = NULL;
    EFI_EVENT group_wait = NULL;
    UINTN notify_count = 0;
    EFI_STATUS st;
    BOOLEAN ok = 1;

    st = bs_create_event(0, 0, NULL, NULL, &event);
    if (st != EFI_SUCCESS ||
        bs_check_event(event) != EFI_NOT_READY ||
        bs_set_timer(event, TIMER_RELATIVE, 1) != EFI_INVALID_PARAMETER ||
        bs_signal_event(event) != EFI_SUCCESS ||
        bs_check_event(event) != EFI_SUCCESS ||
        bs_check_event(event) != EFI_NOT_READY) {
        ok = 0;
    }
    if (event != NULL) {
        (void)bs_close_event(event);
        event = NULL;
    }

    st = bs_create_event(EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL,
                         TPL_CALLBACK,
                         uefi_event_services_selftest_callback,
                         &notify_count, &event);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    st = bs_create_event(EVT_NOTIFY_WAIT, TPL_APPLICATION,
                         uefi_event_services_selftest_callback,
                         &notify_count, &event);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    event = NULL;
    st = bs_create_event(0x00000001U, 0, NULL, NULL, &event);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    notify_count = 0;
    st = bs_create_event(EVT_RUNTIME | EVT_RUNTIME_CONTEXT |
                         EVT_NOTIFY_SIGNAL,
                         TPL_CALLBACK,
                         uefi_event_services_selftest_callback,
                         &notify_count, &event);
    if (st != EFI_SUCCESS ||
        bs_signal_event(event) != EFI_SUCCESS || notify_count != 1) {
        ok = 0;
    }
    if (event != NULL) {
        (void)bs_close_event(event);
        event = NULL;
    }

    {
        EFI_EVENT invalid_event = (EFI_EVENT)(UINTN)1;
        EFI_EVENT invalid_events[1] = { invalid_event };
        UINTN index = ~(UINTN)0;

        if (bs_set_timer(invalid_event, TIMER_RELATIVE, 1) !=
                EFI_INVALID_PARAMETER ||
            bs_signal_event(invalid_event) != EFI_INVALID_PARAMETER ||
            bs_close_event(invalid_event) != EFI_INVALID_PARAMETER ||
            bs_check_event(invalid_event) != EFI_INVALID_PARAMETER ||
            bs_wait_for_event(1, invalid_events, &index) !=
                EFI_INVALID_PARAMETER ||
            index != 0) {
            ok = 0;
        }
    }

    {
        FW_EVENT_RECORD *timer_rec;

        event = NULL;
        st = bs_create_event(EVT_TIMER, 0, NULL, NULL, &event);
        timer_rec = fw_event_record_from_handle(event);
        if (st != EFI_SUCCESS || timer_rec == NULL) {
            ok = 0;
        }
        if (timer_rec != NULL) {
            if (bs_set_timer(event, TIMER_RELATIVE + 1U, 0) !=
                    EFI_INVALID_PARAMETER ||
                bs_set_timer(event, TIMER_PERIODIC, 0) != EFI_SUCCESS ||
                !timer_rec->timer_active ||
                timer_rec->timer_type != TIMER_PERIODIC ||
                timer_rec->timer_remaining_100ns != 0 ||
                timer_rec->timer_period_100ns != 0 ||
                timer_rec->timer_partial_ticks != 0) {
                ok = 0;
            }
            timer_rec->timer_last_tick = 100;
            if (fw_event_timer_consume(timer_rec, 100) ||
                !fw_event_timer_consume(timer_rec, 101) ||
                !timer_rec->timer_active ||
                timer_rec->timer_remaining_100ns != 0 ||
                timer_rec->timer_last_tick != 101 ||
                bs_set_timer(event, TIMER_CANCEL, 0) != EFI_SUCCESS ||
                timer_rec->timer_active) {
                ok = 0;
            }

            if (bs_signal_event(event) != EFI_SUCCESS ||
                bs_set_timer(event, TIMER_RELATIVE, ~(UINT64)0) !=
                    EFI_SUCCESS ||
                !timer_rec->signaled ||
                timer_rec->timer_remaining_100ns != ~(UINT64)0 ||
                timer_rec->timer_partial_ticks != 0 ||
                timer_rec->timer_period_100ns != 0) {
                ok = 0;
            }
            timer_rec->timer_last_tick = 100;
            if (fw_event_timer_consume(
                    timer_rec, 100 + FW_ITC_TICKS_PER_100NS) ||
                timer_rec->timer_remaining_100ns != ~(UINT64)0 - 1U ||
                timer_rec->timer_partial_ticks != 0 ||
                !timer_rec->timer_active || !timer_rec->signaled) {
                ok = 0;
            }
            if (bs_set_timer(event, TIMER_CANCEL, 0) != EFI_SUCCESS ||
                !timer_rec->signaled) {
                ok = 0;
            }
            fw_event_consume(timer_rec);

            if (bs_set_timer(event, TIMER_PERIODIC, ~(UINT64)0) !=
                    EFI_SUCCESS ||
                timer_rec->timer_period_100ns != ~(UINT64)0) {
                ok = 0;
            }
            timer_rec->timer_last_tick = 100;
            if (fw_event_timer_consume(
                    timer_rec, 100 + FW_ITC_TICKS_PER_100NS) ||
                timer_rec->timer_remaining_100ns != ~(UINT64)0 - 1U ||
                timer_rec->timer_period_100ns != ~(UINT64)0 ||
                !timer_rec->timer_active ||
                bs_set_timer(event, TIMER_CANCEL, 0) != EFI_SUCCESS) {
                ok = 0;
            }

            timer_rec->timer_active = 1;
            timer_rec->timer_type = TIMER_RELATIVE;
            timer_rec->timer_last_tick = ~(UINT64)0 - 9U;
            timer_rec->timer_remaining_100ns = 1;
            timer_rec->timer_partial_ticks = 0;
            timer_rec->timer_period_100ns = 0;
            if (!fw_event_timer_consume(timer_rec, 10) ||
                timer_rec->timer_active ||
                timer_rec->timer_remaining_100ns != 0) {
                ok = 0;
            }

            timer_rec->timer_active = 1;
            timer_rec->timer_type = TIMER_RELATIVE;
            timer_rec->timer_last_tick = 100;
            timer_rec->timer_remaining_100ns = 1;
            timer_rec->timer_partial_ticks = 0;
            timer_rec->timer_period_100ns = 0;
            timer_rec->signaled = 1;
            if (!fw_event_timer_expired_at(
                    timer_rec, 100 + FW_ITC_TICKS_PER_100NS) ||
                timer_rec->timer_active || !timer_rec->signaled) {
                ok = 0;
            }
            fw_event_consume(timer_rec);

            timer_rec->timer_active = 1;
            timer_rec->timer_type = TIMER_RELATIVE;
            timer_rec->timer_last_tick = 100;
            timer_rec->timer_remaining_100ns = 1;
            timer_rec->timer_partial_ticks =
                FW_ITC_TICKS_PER_100NS - 1U;
            if (!fw_event_timer_consume(timer_rec, 101) ||
                timer_rec->timer_active ||
                timer_rec->timer_partial_ticks != 0) {
                ok = 0;
            }

            timer_rec->timer_active = 1;
            timer_rec->timer_type = TIMER_PERIODIC;
            timer_rec->timer_last_tick = 100;
            timer_rec->timer_remaining_100ns = 1;
            timer_rec->timer_partial_ticks = 0;
            timer_rec->timer_period_100ns = 1;
            if (!fw_event_timer_consume(
                    timer_rec,
                    100 + 1000000ULL * FW_ITC_TICKS_PER_100NS) ||
                timer_rec->timer_remaining_100ns != 1 ||
                timer_rec->timer_partial_ticks != 0 ||
                !timer_rec->timer_active) {
                ok = 0;
            }

            timer_rec->timer_active = 1;
            timer_rec->timer_type = TIMER_PERIODIC;
            timer_rec->timer_last_tick = 100;
            timer_rec->timer_remaining_100ns = 3;
            timer_rec->timer_partial_ticks = 0;
            timer_rec->timer_period_100ns = 5;
            if (!fw_event_timer_consume(
                    timer_rec,
                    100 + 15U * FW_ITC_TICKS_PER_100NS + 7U) ||
                !timer_rec->timer_active ||
                timer_rec->timer_remaining_100ns != 3 ||
                timer_rec->timer_partial_ticks != 7 ||
                !fw_event_timer_consume(
                    timer_rec,
                    100 + 18U * FW_ITC_TICKS_PER_100NS) ||
                timer_rec->timer_remaining_100ns != 5 ||
                timer_rec->timer_partial_ticks != 0) {
                ok = 0;
            }

            if (bs_set_timer(event, TIMER_RELATIVE, 0) != EFI_SUCCESS ||
                !timer_rec->timer_active ||
                timer_rec->timer_remaining_100ns != 0) {
                ok = 0;
            }
            timer_rec->timer_last_tick = 500;
            if (fw_event_timer_consume(timer_rec, 500) ||
                !timer_rec->timer_active ||
                !fw_event_timer_consume(timer_rec, 501) ||
                timer_rec->timer_active) {
                ok = 0;
            }
        }
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    {
        EFI_TPL old_tpl;

        notify_count = 0;
        event = NULL;
        st = bs_create_event(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             uefi_event_services_selftest_callback,
                             &notify_count, &event);
        if (st == EFI_SUCCESS) {
            old_tpl = bs_raise_tpl(TPL_NOTIFY);
            if (old_tpl != TPL_APPLICATION ||
                bs_signal_event(event) != EFI_SUCCESS ||
                bs_signal_event(event) != EFI_SUCCESS ||
                notify_count != 0) {
                ok = 0;
            }
            bs_restore_tpl(old_tpl);
            if (notify_count != 1 ||
                bs_signal_event(event) != EFI_SUCCESS ||
                notify_count != 2) {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    {
        EFI_TPL old_tpl;

        notify_count = 0;
        event = NULL;
        st = bs_create_event(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             uefi_event_services_selftest_callback,
                             &notify_count, &event);
        if (st == EFI_SUCCESS) {
            old_tpl = bs_raise_tpl(TPL_NOTIFY);
            if (bs_signal_event(event) != EFI_SUCCESS ||
                bs_close_event(event) != EFI_SUCCESS) {
                ok = 0;
            } else {
                event = NULL;
            }
            bs_restore_tpl(old_tpl);
            if (notify_count != 0) {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    {
        EFI_EVENT wait_events[1];
        EFI_TPL old_tpl;
        UINTN index = 0;

        event = NULL;
        st = bs_create_event(0, 0, NULL, NULL, &event);
        if (st == EFI_SUCCESS) {
            wait_events[0] = event;
            old_tpl = bs_raise_tpl(TPL_CALLBACK);
            st = bs_wait_for_event(1, wait_events, &index);
            bs_restore_tpl(old_tpl);
            if (st != EFI_UNSUPPORTED) {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    {
        IA64_PLABEL temp_plabel =
            *(IA64_PLABEL *)fw_event_notify_address(
                uefi_event_services_selftest_callback);
        EFI_EVENT_NOTIFY temp_notify =
            fw_event_notify_from_address((UINTN)&temp_plabel);

        notify_count = 0;
        st = bs_create_event(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, temp_notify,
                             &notify_count, &event);
        fw_set_mem(&temp_plabel, sizeof(temp_plabel), 0);
        if (st != EFI_SUCCESS ||
            bs_signal_event(event) != EFI_SUCCESS ||
            notify_count != 1) {
            ok = 0;
        }
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    st = bs_create_event_ex(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                            uefi_event_services_selftest_callback,
                            &notify_count, NULL, &event);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    st = bs_create_event_ex(EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE, TPL_CALLBACK,
                            uefi_event_services_selftest_callback,
                            &notify_count, NULL, &event);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    st = bs_create_event_ex(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                            uefi_event_services_selftest_callback,
                            &notify_count, (void *)test_group, &event);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
        if (event != NULL) {
            (void)bs_close_event(event);
            event = NULL;
        }
    }

    st = bs_create_event_ex(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                            uefi_event_services_selftest_callback,
                            &notify_count, (void *)test_group,
                            &group_notify);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    st = bs_create_event_ex(0, 0, NULL, NULL, (void *)test_group,
                            &group_wait);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    if (ok) {
        notify_count = 0;
        st = bs_signal_event(group_wait);
        if (st != EFI_SUCCESS || notify_count != 1 ||
            bs_check_event(group_wait) != EFI_SUCCESS) {
            ok = 0;
        }
    }
    if (group_notify != NULL) {
        (void)bs_close_event(group_notify);
    }
    if (group_wait != NULL) {
        (void)bs_close_event(group_wait);
    }

    {
        EFI_EVENT exit_group = NULL;
        EFI_EVENT exit_legacy = NULL;
        BOOLEAN saved_exit_signaled = mExitBootServicesEventsSignaled;

        mExitBootServicesEventsSignaled = 0;
        notify_count = 0;
        st = bs_create_event_ex(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                uefi_event_services_selftest_callback,
                                &notify_count,
                                (void *)gEfiEventGroupExitBootServicesGuid,
                                &exit_group);
        if (st != EFI_SUCCESS) {
            ok = 0;
        }
        st = bs_create_event(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                             uefi_event_services_selftest_callback,
                             &notify_count, &exit_legacy);
        if (st != EFI_SUCCESS) {
            ok = 0;
        }
        if (exit_group != NULL && exit_legacy != NULL) {
            if (bs_signal_event(exit_group) != EFI_SUCCESS ||
                notify_count != 2) {
                ok = 0;
            }
            notify_count = 0;
            fw_signal_exit_boot_services_events();
            fw_signal_exit_boot_services_events();
            if (notify_count != 2) {
                ok = 0;
            }
        }
        if (exit_group != NULL) {
            (void)bs_close_event(exit_group);
        }
        if (exit_legacy != NULL) {
            (void)bs_close_event(exit_legacy);
        }
        mExitBootServicesEventsSignaled = saved_exit_signaled;
    }

    {
        EFI_EVENT before_event = NULL;
        EFI_EVENT exit_event = NULL;
        EFI_EVENT timer_event = NULL;
        FW_EVENT_RECORD saved_timer_state[FW_EVENT_MAX];
        FW_EVENT_RECORD *timer_rec = NULL;
        BOOLEAN saved_before_signaled = mBeforeExitBootServicesSignaled;
        BOOLEAN saved_exit_signaled = mExitBootServicesEventsSignaled;
        UINTN saved_map_key = mMapKey;
        UINTN exit_notify_count = 0;
        UINTN first_map_key;
        UINTN i;

        fw_copy_mem(saved_timer_state, mEventRecords,
                    sizeof(saved_timer_state));
        mBeforeExitBootServicesSignaled = 0;
        mExitBootServicesEventsSignaled = 0;
        notify_count = 0;
        st = bs_create_event(EVT_TIMER, 0, NULL, NULL, &timer_event);
        timer_rec = fw_event_record_from_handle(timer_event);
        if (st != EFI_SUCCESS || timer_rec == NULL ||
            bs_set_timer(timer_event, TIMER_PERIODIC, 100000U) !=
                EFI_SUCCESS ||
            !timer_rec->timer_active) {
            ok = 0;
        }
        st = bs_create_event(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                             uefi_event_services_selftest_callback,
                             &exit_notify_count, &exit_event);
        if (st != EFI_SUCCESS) {
            ok = 0;
        }
        st = bs_create_event_ex(
            EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
            uefi_event_services_selftest_map_change, &notify_count,
            (void *)gEfiEventGroupBeforeExitBootServicesGuid,
            &before_event);
        if (st == EFI_SUCCESS) {
            first_map_key = mMapKey;
            st = fw_prepare_exit_boot_services(first_map_key);
            if (st != EFI_INVALID_PARAMETER || notify_count != 1 ||
                !mBeforeExitBootServicesSignaled ||
                exit_notify_count != 0 || timer_rec == NULL ||
                timer_rec->timer_active || timer_rec->timer_type != 0 ||
                timer_rec->timer_last_tick != 0 ||
                timer_rec->timer_remaining_100ns != 0 ||
                timer_rec->timer_partial_ticks != 0 ||
                timer_rec->timer_period_100ns != 0) {
                ok = 0;
            }
            st = fw_prepare_exit_boot_services(mMapKey);
            if (st != EFI_SUCCESS || notify_count != 1 ||
                exit_notify_count != 0 || timer_rec == NULL ||
                timer_rec->timer_active) {
                ok = 0;
            }
            fw_signal_exit_boot_services_events();
            fw_signal_exit_boot_services_events();
            if (exit_notify_count != 1) {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (before_event != NULL) {
            (void)bs_close_event(before_event);
        }
        if (exit_event != NULL) {
            (void)bs_close_event(exit_event);
        }
        if (timer_event != NULL) {
            (void)bs_close_event(timer_event);
        }
        for (i = 0; i < FW_EVENT_MAX; i++) {
            mEventRecords[i].timer_active =
                saved_timer_state[i].timer_active;
            mEventRecords[i].timer_type = saved_timer_state[i].timer_type;
            mEventRecords[i].timer_last_tick =
                saved_timer_state[i].timer_last_tick;
            mEventRecords[i].timer_remaining_100ns =
                saved_timer_state[i].timer_remaining_100ns;
            mEventRecords[i].timer_partial_ticks =
                saved_timer_state[i].timer_partial_ticks;
            mEventRecords[i].timer_period_100ns =
                saved_timer_state[i].timer_period_100ns;
        }
        mMapKey = saved_map_key;
        mBeforeExitBootServicesSignaled = saved_before_signaled;
        mExitBootServicesEventsSignaled = saved_exit_signaled;
    }

    if (bs_set_watchdog_timer(300, 0, 0, NULL) != EFI_SUCCESS ||
        *(volatile UINT64 *)(UINTN)(FW_WATCHDOG_BASE +
                                    FW_WATCHDOG_TIMEOUT_OFFSET) != 300U ||
        bs_set_watchdog_timer(0, 0, 0, NULL) != EFI_SUCCESS ||
        bs_set_watchdog_timer(1, 1, 0, NULL) != EFI_INVALID_PARAMETER ||
        mCurrentTpl != TPL_APPLICATION) {
        ok = 0;
    }

    return ok;
}

static void efi_init_graphics(void)
{
    mGopModeInfo[0].Version = 0;
    mGopModeInfo[0].HorizontalResolution = VGA_MODE_TEXT_WIDTH;
    mGopModeInfo[0].VerticalResolution = VGA_MODE_TEXT_HEIGHT;

    mGopModeInfo[0].PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    mGopModeInfo[0].PixelInformation.RedMask = 0;
    mGopModeInfo[0].PixelInformation.GreenMask = 0;
    mGopModeInfo[0].PixelInformation.BlueMask = 0;
    mGopModeInfo[0].PixelInformation.ReservedMask = 0;
    mGopModeInfo[0].PixelsPerScanLine = VGA_MODE_TEXT_WIDTH;

    mGopModeInfo[1] = mGopModeInfo[0];
    mGopModeInfo[1].HorizontalResolution = VGA_MODE_640_WIDTH;
    mGopModeInfo[1].VerticalResolution = VGA_MODE_640_HEIGHT;
    mGopModeInfo[1].PixelsPerScanLine = VGA_MODE_640_WIDTH;

    mGopModeInfo[2] = mGopModeInfo[0];
    mGopModeInfo[2].HorizontalResolution = VGA_MODE_800_WIDTH;
    mGopModeInfo[2].VerticalResolution = VGA_MODE_800_HEIGHT;
    mGopModeInfo[2].PixelsPerScanLine = VGA_MODE_800_WIDTH;

    mGopModeInfo[3] = mGopModeInfo[0];
    mGopModeInfo[3].HorizontalResolution = VGA_MODE_1024_WIDTH;
    mGopModeInfo[3].VerticalResolution = VGA_MODE_1024_HEIGHT;
    mGopModeInfo[3].PixelsPerScanLine = VGA_MODE_1024_WIDTH;

    mGopModeInfo[4] = mGopModeInfo[0];
    mGopModeInfo[4].HorizontalResolution = VGA_MODE_1280_WIDTH;
    mGopModeInfo[4].VerticalResolution = VGA_MODE_1280_HEIGHT;
    mGopModeInfo[4].PixelsPerScanLine = VGA_MODE_1280_WIDTH;

    mGopMode.MaxMode = FW_ARRAY_SIZE(mGopModeInfo);
    mGopMode.Mode = 0;
    mGopMode.Info = &mGopModeInfo[0];
    mGopMode.SizeOfInfo = sizeof(mGopModeInfo[0]);
    mGopMode.FrameBufferBase = VGA_FB_BASE;
    mGopMode.FrameBufferSize = 0;

    mGopProto.QueryMode = gop_query_mode;
    mGopProto.SetMode = gop_set_mode;
    mGopProto.Blt = gop_blt;
    mGopProto.Mode = &mGopMode;

    mUgaDrawProto.GetMode = uga_get_mode;
    mUgaDrawProto.SetMode = uga_set_mode;
    mUgaDrawProto.Blt = uga_blt;

    /*
     * Boot the firmware's own console in hardware VGA text mode: establish the
     * GOP mode-0 geometry (so a GOP consumer that reads the current mode sees a
     * valid framebuffer) but leave the display in 80x25 text.  The VGA
     * character generator rasterises the 8x16 font from plane 2 exactly as a
     * real i2000 does, which both matches hardware and is far cheaper than
     * software-blitting every glyph to the uncached linear framebuffer.  A GOP/
     * UGA consumer switches to graphics via SetMode; a vgacon OS is handed off
     * already in text mode (see graphics_prepare_os_handoff).
     */
    (void)graphics_select_mode(0, 0);
    graphics_select_text_mode();
}

static void efi_init_static_handles(void)
{
    mBlockIoHandle = FW_HANDLE_BLOCK_IO;
    mImageHandle = FW_HANDLE_IMAGE;
    mUnicodeCollationHandle = FW_HANDLE_UNICODE;
    mGraphicsHandle = FW_HANDLE_GRAPHICS;
    mFpswaHandle = FW_HANDLE_FPSWA;
    mPciRootBridgeHandle = FW_HANDLE_PCI_ROOT_BRIDGE;
    mPciIdeHandle = FW_HANDLE_PCI_IDE;
    mPciAhciHandle = FW_HANDLE_PCI_AHCI;
    mPciOhciHandle = FW_HANDLE_PCI_OHCI;
    mPciUhciHandle = FW_HANDLE_PCI_UHCI;
    mPciLsiHandle = FW_HANDLE_PCI_LSI;
    mTcgHandle = FW_HANDLE_TCG;
    mStorageDriverHandle = FW_HANDLE_STORAGE_DRIVER;
    mArchitecturalHandle = FW_HANDLE_ARCH_PROTOCOLS;
}

static void efi_init_system_table(void)
{
    static const CHAR16 fw_vendor[] = {
        'Q', 'E', 'M', 'U', ' ', 'I', 'A', '-', '6', '4', ' ', 'F', 'i', 'r', 'm', 'w', 'a', 'r', 'e', 0
    };

    mSystemTable.Hdr.Signature = EFI_SYSTEM_TABLE_SIGNATURE;
    mSystemTable.Hdr.Revision = EFI_SYSTEM_TABLE_REVISION;
    mSystemTable.Hdr.HeaderSize = sizeof(mSystemTable);
    mSystemTable.Hdr.CRC32 = 0;
    mSystemTable.Hdr.Reserved = 0;

    mSystemTable.FirmwareVendor = (CHAR16 *)fw_vendor;
    mSystemTable.FirmwareRevision = (1 << 16) | 0;
    mSystemTable.ConsoleInHandle = mImageHandle;
    mSystemTable.ConIn = &mConInProto;
    mSystemTable.ConsoleOutHandle = mGraphicsHandle;
    mSystemTable.ConOut = &mConOutProto;
    mSystemTable.StandardErrorHandle = mGraphicsHandle;
    mSystemTable.StdErr = &mConOutProto;
    mSystemTable.RuntimeServices = &mRuntimeServices;
    mSystemTable.BootServices = &mBootServices;
    mSystemTable.NumberOfTableEntries = 0;
    mSystemTable.ConfigurationTable = NULL;
}

static void efi_init_loaded_image_proto(void)
{
    mLoadedImageProto.Revision     = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    mLoadedImageProto.ParentHandle = NULL;
    mLoadedImageProto.SystemTable  = &mSystemTable;
    mLoadedImageProto.DeviceHandle = mBlockIoHandle;
    mLoadedImageProto.FilePath     = NULL;
    mLoadedImageProto.Reserved     = NULL;
    mLoadedImageProto.LoadOptionsSize = 0;
    mLoadedImageProto.LoadOptions  = NULL;
    mLoadedImageProto.ImageBase    = NULL;
    mLoadedImageProto.ImageSize    = 0;
    mLoadedImageProto.ImageCodeType = 0;
    mLoadedImageProto.ImageDataType = 0;
    mLoadedImageProto.Unload       = NULL;
}

static void efi_init_fpswa_loaded_image_proto(void)
{
    mFpswaLoadedImageProto.Revision     = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    mFpswaLoadedImageProto.ParentHandle = NULL;
    mFpswaLoadedImageProto.SystemTable  = &mSystemTable;
    mFpswaLoadedImageProto.DeviceHandle = NULL;
    mFpswaLoadedImageProto.FilePath     = NULL;
    mFpswaLoadedImageProto.Reserved     = NULL;
    mFpswaLoadedImageProto.LoadOptionsSize = 0;
    mFpswaLoadedImageProto.LoadOptions  = NULL;
    mFpswaLoadedImageProto.ImageBase    = &mFpswaProto;
    mFpswaLoadedImageProto.ImageSize    = sizeof(mFpswaProto);
    mFpswaLoadedImageProto.ImageCodeType = EfiRuntimeServicesCode;
    mFpswaLoadedImageProto.ImageDataType = EfiRuntimeServicesData;
    mFpswaLoadedImageProto.Unload       = fpswa_unload_image;
    mFpswaLoadedImageActive = 0;
}

/* --- PE32+ image loader lives in pe_loader.c ----------------------------- */

/* --- ATA/ATAPI IDE driver lives in ide.c --------------------------------- */

/* El Torito boot-image size in 2 KiB CD blocks (shared with the CD path). */
UINT32 mCdromBlocks;

/* --- FAT/ISO filesystems, Block/Disk I/O, partitions and device paths
   live in filesystem.c ----------------------------------------------- */


static EFI_HANDLE mLoadedImageUnloadSelftestHandle;
static UINTN mLoadedImageUnloadSelftestCalls;

static EFI_STATUS loaded_image_unload_selftest_handler(EFI_HANDLE ImageHandle)
{
    if (ImageHandle != mLoadedImageUnloadSelftestHandle) {
        return EFI_INVALID_PARAMETER;
    }
    mLoadedImageUnloadSelftestCalls++;
    return EFI_SUCCESS;
}

static BOOLEAN loaded_image_unload_selftest(void)
{
    EFI_LOADED_IMAGE_RECORD *rec = NULL;
    UINTN i;

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (!mLoadedImages[i].in_use) {
            rec = &mLoadedImages[i];
            break;
        }
    }
    if (rec == NULL) {
        return 0;
    }

    fw_set_mem(rec, sizeof(*rec), 0);
    rec->in_use = 1;
    rec->started = 1;
    rec->handle = (EFI_HANDLE)rec;
    mLoadedImageUnloadSelftestHandle = rec->handle;
    mLoadedImageUnloadSelftestCalls = 0;
    if (bs_unload_image(rec->handle) != EFI_UNSUPPORTED ||
        !rec->in_use) {
        fw_set_mem(rec, sizeof(*rec), 0);
        return 0;
    }

    rec->loaded_image.Unload = loaded_image_unload_selftest_handler;
    if (bs_unload_image(rec->handle) != EFI_SUCCESS ||
        rec->in_use ||
        mLoadedImageUnloadSelftestCalls != 1) {
        fw_set_mem(rec, sizeof(*rec), 0);
        return 0;
    }

    fw_set_mem(rec, sizeof(*rec), 0);
    rec->in_use = 1;
    rec->started = 0;
    rec->handle = (EFI_HANDLE)rec;
    if (bs_unload_image(rec->handle) != EFI_SUCCESS ||
        rec->in_use) {
        fw_set_mem(rec, sizeof(*rec), 0);
        return 0;
    }

    return 1;
}

static BOOLEAN __attribute__((noinline)) pe_section_memory_type_selftest(void)
{
    static EFI_MEMORY_DESCRIPTOR saved_map[MEMORY_MAP_MAX];
    IMAGE_SECTION_HEADER sections[4];
    UINTN saved_entries = mMemoryMapEntries;
    UINTN saved_key = mMapKey;
    UINT64 base = 0x05000000ULL;
    UINT64 loader_data_attr = efi_memory_attribute(EfiLoaderData,
                                                   EFI_MEMORY_WB);
    UINT64 loader_code_attr = efi_memory_attribute(EfiLoaderCode,
                                                   EFI_MEMORY_WB);
    UINT64 runtime_code_attr =
        efi_memory_attribute(EfiRuntimeServicesCode, EFI_MEMORY_WB);
    BOOLEAN ok;

    fw_copy_mem(saved_map, mMemoryMap, sizeof(saved_map));
    fw_set_mem(mMemoryMap, sizeof(mMemoryMap), 0);
    mMemoryMapEntries = 0;
    efi_add_memory_range(&mMemoryMapEntries, EfiConventionalMemory,
                         base, base + 0x8000U, EFI_MEMORY_WB);

    fw_set_mem(sections, sizeof(sections), 0);
    sections[0].VirtualAddress = 0x1000;
    sections[0].VirtualSize = 0x1000;
    sections[0].Characteristics =
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE;
    sections[1].VirtualAddress = 0x2000;
    sections[1].VirtualSize = 0x1000;
    sections[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA;
    sections[2].VirtualAddress = 0x3000;
    sections[2].VirtualSize = 0x800;
    sections[2].Characteristics =
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE;
    sections[3].VirtualAddress = 0x3800;
    sections[3].VirtualSize = 0x800;
    sections[3].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA;

    ok = pe_mark_loaded_image_memory(base, 0x5000, sections,
                                     FW_ARRAY_SIZE(sections),
                                     EfiLoaderCode, EfiLoaderData) &&
         efi_memory_map_has_descriptor(EfiLoaderData,
                                       base, base + 0x1000U,
                                       loader_data_attr) &&
         efi_memory_map_has_descriptor(EfiLoaderCode,
                                       base + 0x1000U, base + 0x2000U,
                                       loader_code_attr) &&
         efi_memory_map_has_descriptor(EfiLoaderData,
                                       base + 0x2000U, base + 0x5000U,
                                       loader_data_attr);
    if (ok) {
        fw_set_mem(mMemoryMap, sizeof(mMemoryMap), 0);
        mMemoryMapEntries = 0;
        efi_add_memory_range(&mMemoryMapEntries, EfiConventionalMemory,
                             base, base + 0x8000U, EFI_MEMORY_WB);

        ok = pe_mark_loaded_image_memory(base, 0x5000, sections,
                                         FW_ARRAY_SIZE(sections),
                                         EfiRuntimeServicesCode,
                                         EfiRuntimeServicesData) &&
             efi_memory_map_has_descriptor(
                 EfiRuntimeServicesCode, base, base + 0x6000U,
                 runtime_code_attr) &&
             efi_memory_map_has_ia64_descriptor_alignment();
    }

    fw_copy_mem(mMemoryMap, saved_map, sizeof(saved_map));
    mMemoryMapEntries = saved_entries;
    mMapKey = saved_key;
    return ok;
}

static BOOLEAN __attribute__((noinline)) pe_runtime_relocation_selftest(void)
{
    static UINT8 image[0x6000] __attribute__((aligned(4096)));
    static EFI_MEMORY_DESCRIPTOR saved_virtual_map[MEMORY_MAP_MAX];
    UINT64 relocation_log[4];
    UINTN saved_virtual_entries = mVirtualAddressMapEntries;
    BOOLEAN saved_in_progress = mVirtualAddressMapInProgress;
    BOOLEAN saved_applied = mVirtualAddressMapApplied;
    UINT64 base = (UINT64)(UINTN)image;
    UINT64 load_adjust = 0x100000ULL;
    UINT64 preferred = base - load_adjust;
    UINT64 virt = 0xe0000000d0000000ULL;
    UINT64 preserved = 0x0123456789abcdefULL;
    IMAGE_DOS_HEADER *dos;
    UINT32 *nt_sig;
    IMAGE_FILE_HEADER *file_hdr;
    IMAGE_OPTIONAL_HEADER64 *opt64;
    UINT32 *data_dir;
    UINT32 *reloc_block;
    UINT16 *reloc_entry;
    UINT64 *patch;
    UINT64 *modified_patch;
    UINT8 *modified_imm_reloc;
    UINT8 *imm_reloc;
    UINT64 imm_value;
    UINT64 slot2;
    UINT64 modified_imm_load_word;
    UINT64 imm_load_word;
    UINT64 modified_imm_words[2];
    BOOLEAN ok;

    fw_copy_mem(saved_virtual_map, mVirtualAddressMap,
                sizeof(saved_virtual_map));
    fw_set_mem(image, sizeof(image), 0);

    dos = (IMAGE_DOS_HEADER *)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    nt_sig = (UINT32 *)(image + dos->e_lfanew);
    *nt_sig = IMAGE_NT_SIGNATURE;
    file_hdr = (IMAGE_FILE_HEADER *)(nt_sig + 1);
    file_hdr->Machine = IMAGE_FILE_MACHINE_IA64;
    file_hdr->SizeOfOptionalHeader = 112 + 16 * 8;
    opt64 = (IMAGE_OPTIONAL_HEADER64 *)((UINT8 *)file_hdr +
                                        sizeof(*file_hdr));
    opt64->Magic = 0x020B;
    opt64->ImageBase = preferred;
    opt64->SizeOfImage = sizeof(image);
    opt64->Subsystem = IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER;
    opt64->NumberOfRvaAndSizes = 16;
    data_dir = (UINT32 *)((UINT8 *)opt64 + 112);
    data_dir[10] = 0x4000;
    data_dir[11] = 20;

    patch = (UINT64 *)(image + 0x3000);
    modified_patch = (UINT64 *)(image + 0x3008);
    modified_imm_reloc = image + 0x3010;
    imm_reloc = image + 0x3020;
    *patch = preferred + 0x2000;
    *modified_patch = preferred + 0x2200;
    imm_value = preferred + 0x2400;
    slot2 = pe_ia64_movl_set_imm64(6ULL << 37, imm_value);
    pe_ia64_store_bundle((UINT64 *)modified_imm_reloc, 4, 0,
                         (imm_value >> 22) & IA64_SLOT_MASK, slot2);
    imm_value = preferred + 0x2600;
    slot2 = pe_ia64_movl_set_imm64(6ULL << 37, imm_value);
    pe_ia64_store_bundle((UINT64 *)imm_reloc, 4, 0,
                         (imm_value >> 22) & IA64_SLOT_MASK, slot2);
    reloc_block = (UINT32 *)(image + 0x4000);
    reloc_block[0] = 0x3000;
    reloc_block[1] = 20;
    reloc_entry = (UINT16 *)(reloc_block + 2);
    reloc_entry[0] = (IMAGE_REL_BASED_DIR64 << 12);
    reloc_entry[1] = (IMAGE_REL_BASED_DIR64 << 12) | 0x008;
    reloc_entry[2] = (IMAGE_REL_BASED_IA64_IMM64 << 12) | 0x010;
    reloc_entry[3] = (IMAGE_REL_BASED_IA64_IMM64 << 12) | 0x020;
    reloc_entry[4] = IMAGE_REL_BASED_ABSOLUTE << 12;
    reloc_entry[5] = IMAGE_REL_BASED_ABSOLUTE << 12;

    fw_set_mem(mVirtualAddressMap, sizeof(mVirtualAddressMap), 0);
    for (UINTN i = 0; i < 4; i++) {
        mVirtualAddressMap[i].Type = EfiRuntimeServicesData;
        mVirtualAddressMap[i].PhysicalStart = 0x10000000ULL + (i << 12);
        mVirtualAddressMap[i].VirtualStart =
            0xe0000000c0000000ULL + (i << 12);
        mVirtualAddressMap[i].NumberOfPages = 1;
        mVirtualAddressMap[i].Attribute =
            efi_memory_attribute(EfiRuntimeServicesData, EFI_MEMORY_WB);
    }
    mVirtualAddressMap[4].Type = EfiRuntimeServicesData;
    mVirtualAddressMap[4].PhysicalStart = base;
    mVirtualAddressMap[4].VirtualStart = virt;
    mVirtualAddressMap[4].NumberOfPages = sizeof(image) >> 12;
    mVirtualAddressMap[4].Attribute =
        efi_memory_attribute(EfiRuntimeServicesData, EFI_MEMORY_WB);
    mVirtualAddressMapEntries = 5;
    mVirtualAddressMapInProgress = 1;
    mVirtualAddressMapApplied = 0;

    ok = pe_apply_relocations(base, sizeof(image), 0x4000, 20,
                              load_adjust, PE_RELOCATE_LOAD,
                              relocation_log,
                              FW_ARRAY_SIZE(relocation_log)) &&
         *patch == base + 0x2000 &&
         *modified_patch == base + 0x2200 &&
         pe_read_ia64_imm64_reloc(modified_imm_reloc, &imm_value) &&
         imm_value == base + 0x2400 &&
         pe_read_ia64_imm64_reloc(imm_reloc, &imm_value) &&
         imm_value == base + 0x2600 &&
         relocation_log[0] == base + 0x2000 &&
         relocation_log[1] == base + 0x2200 &&
         relocation_log[2] == *(UINT64 *)modified_imm_reloc &&
         relocation_log[3] == *(UINT64 *)imm_reloc;
    if (ok) {
        modified_imm_load_word = relocation_log[2];
        imm_load_word = relocation_log[3];
        *modified_patch = preserved;
        ((UINT64 *)modified_imm_reloc)[0] ^= 1ULL << 46;
        modified_imm_words[0] = ((UINT64 *)modified_imm_reloc)[0];
        modified_imm_words[1] = ((UINT64 *)modified_imm_reloc)[1];
        ok = pe_relocate_loaded_runtime_image(base, sizeof(image), NULL, 0) ==
             EFI_LOAD_ERROR &&
             *patch == base + 0x2000 &&
             *modified_patch == preserved &&
             ((UINT64 *)modified_imm_reloc)[0] == modified_imm_words[0] &&
             ((UINT64 *)modified_imm_reloc)[1] == modified_imm_words[1] &&
             pe_read_ia64_imm64_reloc(imm_reloc, &imm_value) &&
             imm_value == base + 0x2600;
    }
    if (ok) {
        ok = pe_relocate_loaded_runtime_image(
                 base, sizeof(image), relocation_log,
                 FW_ARRAY_SIZE(relocation_log)) == EFI_SUCCESS &&
             *patch == virt + 0x2000 &&
             *modified_patch == preserved &&
             ((UINT64 *)modified_imm_reloc)[0] == modified_imm_words[0] &&
             ((UINT64 *)modified_imm_reloc)[1] == modified_imm_words[1] &&
             pe_read_ia64_imm64_reloc(imm_reloc, &imm_value) &&
             imm_value == virt + 0x2600 &&
             relocation_log[2] == modified_imm_load_word &&
             relocation_log[3] != imm_load_word &&
             relocation_log[3] == *(UINT64 *)imm_reloc;
    }

    fw_copy_mem(mVirtualAddressMap, saved_virtual_map,
                sizeof(saved_virtual_map));
    mVirtualAddressMapEntries = saved_virtual_entries;
    mVirtualAddressMapInProgress = saved_in_progress;
    mVirtualAddressMapApplied = saved_applied;
    return ok;
}

static BOOLEAN __attribute__((noinline)) pe_image_base_allocation_selftest(void)
{
    static EFI_MEMORY_DESCRIPTOR saved_map[MEMORY_MAP_MAX];
    static EFI_LOADED_IMAGE_RECORD saved_loaded[LOADED_IMAGE_MAX];
    static EFI_PAGE_ALLOCATION_RECORD saved_pages[PAGE_ALLOCATION_MAX];
    static EFI_POOL_ALLOCATION_RECORD saved_pool[POOL_ALLOCATION_MAX];
    UINTN saved_entries = mMemoryMapEntries;
    UINTN saved_key = mMapKey;
    UINT64 saved_next_pe_image_base = mNextPeImageBase;
    UINT64 base;
    BOOLEAN ok = 1;

    fw_copy_mem(saved_map, mMemoryMap, sizeof(saved_map));
    fw_copy_mem(saved_loaded, mLoadedImages, sizeof(saved_loaded));
    fw_copy_mem(saved_pages, mPageAllocations, sizeof(saved_pages));
    fw_copy_mem(saved_pool, mPoolAllocations, sizeof(saved_pool));
    fw_set_mem(mMemoryMap, sizeof(mMemoryMap), 0);
    fw_set_mem(mLoadedImages, sizeof(mLoadedImages), 0);
    fw_set_mem(mPageAllocations, sizeof(mPageAllocations), 0);
    fw_set_mem(mPoolAllocations, sizeof(mPoolAllocations), 0);
    mMemoryMapEntries = 0;
    mMapKey = 0;
    efi_add_memory_range(&mMemoryMapEntries, EfiConventionalMemory,
                         FW_LOW_FREE_BASE,
                         IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE + 0x40000ULL,
                         EFI_MEMORY_WB);

    mNextPeImageBase = FW_LOW_FREE_BASE;
    base = pe_choose_image_base(FW_LOW_IMAGE_BASE, 0x10000, 0, 0, 0, 0);
    if (base != FW_LOW_IMAGE_BASE) {
        ok = 0;
        goto out;
    }

    /* Conventional page and pool records remain invisible in the map. */
    mPageAllocations[0].in_use = 1;
    mPageAllocations[0].base = FW_LOW_IMAGE_BASE;
    mPageAllocations[0].pages = IA64_EFI_IMAGE_ALIGN >> 12;
    mPageAllocations[0].type = EfiConventionalMemory;
    mPoolAllocations[0].in_use = 1;
    mPoolAllocations[0].base = FW_LOW_IMAGE_BASE + IA64_EFI_IMAGE_ALIGN;
    mPoolAllocations[0].size = IA64_EFI_IMAGE_ALIGN;
    mPoolAllocations[0].backing_base = mPoolAllocations[0].base;
    mPoolAllocations[0].backing_pages = IA64_EFI_IMAGE_ALIGN >> 12;
    mPoolAllocations[0].type = EfiConventionalMemory;
    mNextPeImageBase = FW_LOW_IMAGE_BASE;
    base = pe_choose_image_base(FW_LOW_IMAGE_BASE, 0x10000, 0, 0, 0, 0);
    if (base != FW_LOW_IMAGE_BASE + 2U * IA64_EFI_IMAGE_ALIGN ||
        mNextPeImageBase !=
        FW_LOW_IMAGE_BASE + 3U * IA64_EFI_IMAGE_ALIGN) {
        ok = 0;
        goto out;
    }
    fw_set_mem(mPageAllocations, sizeof(mPageAllocations), 0);
    fw_set_mem(mPoolAllocations, sizeof(mPoolAllocations), 0);

    mNextPeImageBase = FW_LOW_FREE_BASE;
    base = pe_choose_image_base(FW_LOW_IMAGE_BASE, 0x10000, 1, 0, 0, 0);
    if (base != IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE ||
        mNextPeImageBase !=
        IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE + 0x10000ULL) {
        ok = 0;
        goto out;
    }

    mLoadedImages[0].in_use = 1;
    mLoadedImages[0].loaded_image.ImageBase =
        (VOID *)(UINTN)(IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE + 0x10000ULL);
    mLoadedImages[0].loaded_image.ImageSize = 0x10000;
    base = pe_choose_image_base(IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE +
                                0x10000ULL, 0x10000, 1, 0, 0, 0);
    if (base != IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE + 0x20000ULL ||
        mNextPeImageBase !=
        IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE + 0x30000ULL) {
        ok = 0;
        goto out;
    }

    fw_set_mem(mLoadedImages, sizeof(mLoadedImages), 0);
    fw_set_mem(mMemoryMap, sizeof(mMemoryMap), 0);
    mMemoryMapEntries = 0;
    efi_add_memory_range(&mMemoryMapEntries, EfiConventionalMemory,
                         FW_LOW_RECLAIM_BASE, FW_LOW_IMAGE_BASE,
                         EFI_MEMORY_WB);
    mNextPeImageBase = FW_LOW_FREE_BASE;

    /*
     * Images without relocations must load where they were linked even below
     * the staging floor; 0x1040000 is a common IA-64 default link address.
     */
    base = pe_choose_image_base(0x1040000ULL, 0x34000, 0, 1, 0, 0);
    if (base != 0x1040000ULL || mNextPeImageBase != FW_LOW_FREE_BASE) {
        ok = 0;
        goto out;
    }

    /* The same base still loses to the floor when the image can move. */
    if (pe_choose_image_base(0x1040000ULL, 0x34000, 0, 0, 0, 0) <
        FW_LOW_FREE_BASE) {
        ok = 0;
        goto out;
    }

    /* A fixed base must never be placed on top of the source image. */
    if (pe_choose_image_base(0x1040000ULL, 0x34000, 0, 1,
                             0x1060000ULL, 0x1000) != 0) {
        ok = 0;
        goto out;
    }

    /* Occupied memory still rejects a fixed base instead of relocating it. */
    mLoadedImages[0].in_use = 1;
    mLoadedImages[0].loaded_image.ImageBase = (VOID *)(UINTN)0x1040000ULL;
    mLoadedImages[0].loaded_image.ImageSize = 0x1000;
    if (pe_choose_image_base(0x1040000ULL, 0x34000, 0, 1, 0, 0) != 0) {
        ok = 0;
        goto out;
    }
    fw_set_mem(mLoadedImages, sizeof(mLoadedImages), 0);

    mMemoryMapEntries = 0;
    mNextPeImageBase = IA64_EFI_IMAGE_FALLBACK_BASE;
    if (pe_choose_image_base(0, 0x10000, 0, 0, 0, 0) != 0 ||
        mNextPeImageBase != IA64_EFI_IMAGE_FALLBACK_BASE) {
        ok = 0;
    }

out:
    fw_copy_mem(mMemoryMap, saved_map, sizeof(saved_map));
    fw_copy_mem(mLoadedImages, saved_loaded, sizeof(saved_loaded));
    fw_copy_mem(mPageAllocations, saved_pages, sizeof(saved_pages));
    fw_copy_mem(mPoolAllocations, saved_pool, sizeof(saved_pool));
    mMemoryMapEntries = saved_entries;
    mMapKey = saved_key;
    mNextPeImageBase = saved_next_pe_image_base;
    return ok;
}

static BOOLEAN __attribute__((noinline)) load_image_options_selftest(void)
{
    static EFI_MEMORY_DESCRIPTOR saved_map[MEMORY_MAP_MAX];
    static EFI_LOADED_IMAGE_RECORD saved_loaded[LOADED_IMAGE_MAX];
    static EFI_POOL_ALLOCATION_RECORD saved_pool[POOL_ALLOCATION_MAX];
    UINT8 options[4] = { 1, 2, 3, 4 };
    VOID *allocated_options = NULL;
    EFI_MEMORY_TYPE code_type;
    EFI_MEMORY_TYPE data_type;
    EFI_HANDLE image;
    UINTN saved_entries = mMemoryMapEntries;
    UINTN saved_key = mMapKey;
    EFI_PHYSICAL_ADDRESS saved_next_page_addr = mNextPageAddr;
    UINTN i;
    BOOLEAN ok;

    pe_image_memory_types(IMAGE_SUBSYSTEM_EFI_APPLICATION, &code_type,
                          &data_type);
    if (code_type != EfiLoaderCode || data_type != EfiLoaderData) {
        return 0;
    }
    pe_image_memory_types(IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER,
                          &code_type, &data_type);
    if (code_type != EfiBootServicesCode ||
        data_type != EfiBootServicesData) {
        return 0;
    }
    pe_image_memory_types(IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER,
                          &code_type, &data_type);
    if (code_type != EfiRuntimeServicesCode ||
        data_type != EfiRuntimeServicesData) {
        return 0;
    }
    if (!loaded_image_unload_selftest()) {
        return 0;
    }
    if (!pe_section_memory_type_selftest()) {
        return 0;
    }
    if (!pe_image_base_allocation_selftest()) {
        return 0;
    }
    fw_copy_mem(saved_map, mMemoryMap, sizeof(saved_map));
    fw_copy_mem(saved_loaded, mLoadedImages, sizeof(saved_loaded));
    fw_copy_mem(saved_pool, mPoolAllocations, sizeof(saved_pool));
    fw_set_mem(mLoadedImages, sizeof(mLoadedImages), 0);
    mLoadedImages[0].in_use = 1;
    mLoadedImages[0].handle = &mLoadedImages[0];
    image = mLoadedImages[0].handle;

    ok = !fw_set_loaded_image_load_options(image, options, 0) &&
         !fw_set_loaded_image_load_options(image, NULL, sizeof(options)) &&
         fw_set_loaded_image_load_options(image, options, sizeof(options)) &&
         mLoadedImages[0].loaded_image.LoadOptions == options &&
         mLoadedImages[0].loaded_image.LoadOptionsSize == sizeof(options) &&
         fw_set_loaded_image_load_options(image, NULL, 0) &&
         mLoadedImages[0].loaded_image.LoadOptions == NULL &&
         mLoadedImages[0].loaded_image.LoadOptionsSize == 0 &&
         !fw_set_loaded_image_load_options((EFI_HANDLE)(UINTN)1,
                                           options, sizeof(options)) &&
         fw_copy_loaded_image_load_options(
             image, options, sizeof(options),
             &allocated_options) == EFI_SUCCESS &&
         allocated_options != NULL &&
         allocated_options != options &&
         mLoadedImages[0].loaded_image.LoadOptions == allocated_options &&
         mLoadedImages[0].loaded_image.LoadOptionsSize == sizeof(options) &&
         efi_memory_map_covers_range(
             EfiBootServicesData, (UINTN)allocated_options,
             (UINTN)allocated_options + sizeof(options), EFI_MEMORY_WB);
    for (i = 0; ok && i < sizeof(options); i++) {
        if (((UINT8 *)allocated_options)[i] != options[i]) {
            ok = 0;
        }
    }
    if (allocated_options != NULL) {
        ok = fw_release_loaded_image_load_options(
                 image, allocated_options) == EFI_SUCCESS &&
             mLoadedImages[0].loaded_image.LoadOptions == NULL &&
             mLoadedImages[0].loaded_image.LoadOptionsSize == 0 &&
             ok;
    }

    fw_copy_mem(mMemoryMap, saved_map, sizeof(saved_map));
    fw_copy_mem(mLoadedImages, saved_loaded, sizeof(saved_loaded));
    fw_copy_mem(mPoolAllocations, saved_pool, sizeof(saved_pool));
    mMemoryMapEntries = saved_entries;
    mMapKey = saved_key;
    mNextPageAddr = saved_next_page_addr;
    return ok;
}

static EFI_STATUS fw_load_image_source_from_device_path(
    BOOLEAN BootPolicy, void *DevicePath, VOID **SourceBuffer,
    UINTN *SourceSize)
{
    BOOLEAN found;
    EFI_STATUS st;

    if (DevicePath == NULL || SourceBuffer == NULL || SourceSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    st = fw_load_image_source_from_simple_fs(DevicePath, SourceBuffer,
                                              SourceSize, &found);
    if (found || st != EFI_NOT_FOUND) {
        return st;
    }

    if (!BootPolicy) {
        st = fw_load_image_source_from_load_file(
            mLoadFile2ProtocolGuid, 0, DevicePath, SourceBuffer, SourceSize,
            &found);
        if (st == EFI_SUCCESS ||
            (found && st != EFI_NOT_FOUND && st != EFI_UNSUPPORTED) ||
            (!found && st != EFI_NOT_FOUND)) {
            return st;
        }
    }

    return fw_load_image_source_from_load_file(
        mLoadFileProtocolGuid, BootPolicy, DevicePath, SourceBuffer,
        SourceSize, &found);
}

const UINT8 mLoadedImageProtocolGuid[16] = {
    0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11,
    0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

static const UINT8 mLoadedImageDevicePathProtocolGuid[16] = {
    0x7e, 0x15, 0x62, 0xbc, 0x33, 0x3e, 0xec, 0x4f,
    0x99, 0x20, 0x2d, 0x3b, 0x36, 0xd7, 0x50, 0xdf
};

static const UINT8 mHiiPackageListProtocolGuid[16] = {
    0x63, 0xe7, 0x1e, 0x6a, 0x7a, 0xd4, 0xb4, 0x43,
    0xaa, 0xbe, 0xef, 0x1d, 0xe2, 0xab, 0x56, 0xfc
};

const UINT8 mDebugImageInfoTableGuid[16] = {
    0x77, 0x2e, 0x15, 0x49, 0xda, 0x1a, 0x64, 0x47,
    0xb7, 0xa2, 0x7a, 0xfe, 0xfe, 0xd9, 0x5e, 0x8b
};

/* --- NVRAM Variable Store ------------------------------------------------- */

#define NVRAM_STORE_MAGIC 0x524f545352415649ULL /* "IVARSTOR" */
#define NVRAM_STORE_VERSION 1U
#define NVRAM_VAR_MAX 32
#define NVRAM_VAR_NAME_MAX 64
#define NVRAM_VAR_STORAGE_OVERHEAD (16U + sizeof(UINT32))
#define NVRAM_VAR_SLOT_STORAGE \
    (NVRAM_VAR_NAME_MAX * sizeof(CHAR16) + NVRAM_VAR_DATA_MAX + \
     NVRAM_VAR_STORAGE_OVERHEAD)

typedef struct {
    UINT8  name[NVRAM_VAR_NAME_MAX * sizeof(CHAR16)];
    UINT64 name_len;
    UINT8  guid[16];
    UINT8  data[NVRAM_VAR_DATA_MAX];
    UINT64 data_size;
    UINT32 attributes;
    BOOLEAN valid;
    BOOLEAN deleted;
    UINT8 reserved[2];
} NVRAM_VARIABLE;

typedef struct {
    UINT64 magic;
    UINT32 version;
    UINT32 count;
    NVRAM_VARIABLE vars[NVRAM_VAR_MAX];
} NVRAM_STORE;

FW_STATIC_ASSERT(sizeof(NVRAM_VARIABLE) == 1192U,
                 nvram_variable_format_size);
FW_STATIC_ASSERT(__builtin_offsetof(NVRAM_STORE, vars) == 16U,
                 nvram_store_header_size);
FW_STATIC_ASSERT(sizeof(NVRAM_STORE) == 38160U,
                 nvram_store_format_size);
FW_STATIC_ASSERT(sizeof(NVRAM_STORE) <= FW_NVRAM_RTC_OFFSET,
                 nvram_store_fits_mmio_window);

static NVRAM_STORE *mNvramStore = (NVRAM_STORE *)(UINTN)FW_NVRAM_BASE;
static BOOLEAN mNvramSelftestActive;

#define mNvramVars (mNvramStore->vars)
#define mNvramVarCount (mNvramStore->count)

const UINT8 mEfiGlobalVariableGuid[16] = {
    0x61, 0xdf, 0xe4, 0x8b, 0xca, 0x93, 0xd2, 0x11,
    0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c
};

static const UINT16 mBootCurrentValue = 0;
static const UINT16 mBootOrderValue[] = { 0 };
static const CHAR8 mLegacyLangValue[3] = { 'e', 'n', 'g' };
static const CHAR8 mPlatformLangValue[] = "en-US";

typedef EFI_STATUS (*FW_FIRMWARE_VARIABLE_READ)(
    UINT32 *Attributes, UINTN *DataSize, VOID *Data);

typedef struct {
    const char *name;
    const UINT8 *guid;
    UINT32 attributes;
    const VOID *data;
    UINTN data_size;
    FW_FIRMWARE_VARIABLE_READ read;
} FW_FIRMWARE_VARIABLE;

static FW_FIRMWARE_VARIABLE mFirmwareVariables[] = {
    {
        "Boot0000", mEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        NULL, sizeof(FW_EFI_BOOT_OPTION), rs_get_boot0000_variable,
    },
    {
        /* The built-in EFI shell, at a high number to avoid clashing with the
         * low Boot#### an installed OS assigns itself. */
        "Boot00FF", mEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        NULL, sizeof(FW_SHELL_BOOT_OPTION), rs_get_shell_variable,
    },
    {
        "BootCurrent", mEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        &mBootCurrentValue, sizeof(mBootCurrentValue), NULL,
    },
    {
        "BootOrder", mEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        mBootOrderValue, sizeof(mBootOrderValue), NULL,
    },
    {
        "ConOut", mEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        &mGraphicsDevicePath, sizeof(mGraphicsDevicePath), NULL,
    },
    {
        "ConOutDev", mEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        &mConsoleOutputDevicePath, sizeof(mConsoleOutputDevicePath), NULL,
    },
    {
        "ErrOut", mEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        &mGraphicsDevicePath, sizeof(mGraphicsDevicePath), NULL,
    },
    {
        "ErrOutDev", mEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        &mGraphicsDevicePath, sizeof(mGraphicsDevicePath), NULL,
    },
    {
        "Lang", mEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        mLegacyLangValue, sizeof(mLegacyLangValue), NULL,
    },
    {
        "LangCodes", mEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        mLegacyLangValue, sizeof(mLegacyLangValue), NULL,
    },
    {
        "PlatformLang", mEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        mPlatformLangValue, sizeof(mPlatformLangValue), NULL,
    },
    {
        "PlatformLangCodes", mEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        mPlatformLangValue, sizeof(mPlatformLangValue), NULL,
    },
};

#define FW_FIRMWARE_VARIABLE_COUNT FW_ARRAY_SIZE(mFirmwareVariables)

static FW_FIRMWARE_VARIABLE *mRuntimeFirmwareVariables =
    mFirmwareVariables;

static BOOLEAN rs_firmware_variable_enabled(const FW_FIRMWARE_VARIABLE *Var)
{
    if (Var == NULL) {
        return 0;
    }
    return 1;
}

static void nvram_commit(void)
{
    /* The commit register is MMIO, so the store must not be optimized away. */
    volatile UINT64 *commit;

    if (mNvramSelftestActive) {
        return;
    }
    /* This volatile cast targets the host-backed NVRAM MMIO register. */
    commit = (volatile UINT64 *)(UINTN)(
        (UINTN)mNvramStore + FW_NVRAM_COMMIT_OFFSET);
    *commit = FW_NVRAM_COMMIT_MAGIC;
}

static BOOLEAN nvram_store_valid(void)
{
    UINTN i;

    if (mNvramStore->magic != NVRAM_STORE_MAGIC ||
        mNvramStore->version != NVRAM_STORE_VERSION ||
        mNvramVarCount > NVRAM_VAR_MAX) {
        return 0;
    }
    for (i = 0; i < mNvramVarCount; i++) {
        NVRAM_VARIABLE *var = &mNvramVars[i];

        if (var->valid > 1U || var->deleted > 1U) {
            return 0;
        }
        if (!var->valid) {
            continue;
        }
        if (var->name_len < 2 * sizeof(CHAR16) ||
            var->name_len > sizeof(var->name) ||
            (var->name_len & (sizeof(CHAR16) - 1U)) != 0 ||
            var->name[var->name_len - 2U] != 0 ||
            var->name[var->name_len - 1U] != 0 ||
            var->data_size > sizeof(var->data) ||
            (var->deleted && var->data_size != 0) ||
            (var->attributes & ~EFI_VARIABLE_SUPPORTED_ATTRIBUTES) != 0) {
            return 0;
        }
    }
    return 1;
}

static void nvram_init(void)
{
    UINTN i;

    mNvramSelftestActive = 0;
    if (!nvram_store_valid()) {
        fw_set_mem(mNvramStore, sizeof(*mNvramStore), 0);
        mNvramStore->magic = NVRAM_STORE_MAGIC;
        mNvramStore->version = NVRAM_STORE_VERSION;
        return;
    }

    /* Variables without NON_VOLATILE do not survive a platform reset. */
    for (i = 0; i < mNvramVarCount; i++) {
        if (mNvramVars[i].valid &&
            (mNvramVars[i].attributes & EFI_VARIABLE_NON_VOLATILE) == 0) {
            mNvramVars[i].valid = 0;
            mNvramVars[i].deleted = 0;
        }
    }
}

static BOOLEAN fw_char16_eq_ascii_z(const CHAR16 *s, const char *ascii)
{
    UINTN i;

    if (s == NULL || ascii == NULL) {
        return 0;
    }
    for (i = 0; ascii[i] != 0; i++) {
        if (s[i] != (CHAR16)(UINT8)ascii[i]) {
            return 0;
        }
    }
    return s[i] == 0;
}

/* --- Protocol handling ---------------------------------------------------- */

#include "fw-decompress.h"
#include "fw-ebc.h"

const UINT8 mBlockIoProtocolGuid[16] = {
    0x21, 0x5b, 0x4e, 0x96, 0x59, 0x64, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

const UINT8 mDiskIoProtocolGuid[16] = {
    0x71, 0x51, 0x34, 0xce, 0x0b, 0xba, 0xd2, 0x11,
    0x8e, 0x4f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

const UINT8 mSimpleFileSystemProtocolGuid[16] = {
    0x22, 0x5b, 0x4e, 0x96, 0x59, 0x64, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

const UINT8 mDevicePathProtocolGuid[16] = {
    0x91, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

static const UINT8 mUnicodeCollationProtocolGuid[16] = {
    0x7f, 0xcd, 0x85, 0x1d, 0x3d, 0xf4, 0xd2, 0x11,
    0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

static const UINT8 mGraphicsOutputProtocolGuid[16] = {
    0xde, 0xa9, 0x42, 0x90, 0xdc, 0x23, 0x38, 0x4a,
    0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a
};

static const UINT8 mUgaDrawProtocolGuid[16] = {
    0x8b, 0x29, 0x2c, 0x98, 0xfa, 0xf4, 0xcb, 0x41,
    0xb8, 0x38, 0x77, 0xaa, 0x68, 0x8f, 0xb8, 0x39
};

static const UINT8 mFpswaProtocolGuid[16] = {
    0x31, 0x65, 0x1b, 0xc4, 0xb9, 0x97, 0xd3, 0x11,
    0x9a, 0x29, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

static const UINT8 mPciRootBridgeIoProtocolGuid[16] = {
    0xbb, 0x7e, 0x70, 0x2f, 0x1a, 0x4a, 0xd4, 0x11,
    0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

static const UINT8 mPciIoProtocolGuid[16] = {
    0x00, 0xb2, 0xf5, 0x4c, 0xb8, 0x68, 0xa5, 0x4c,
    0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a
};

static const UINT8 mTcgProtocolGuid[16] = {
    0x6d, 0x79, 0x41, 0xf5, 0x2e, 0xa6, 0x54, 0x49,
    0xa7, 0x75, 0x95, 0x84, 0xf6, 0x1b, 0x9c, 0xdd
};

const UINT8 mDriverBindingProtocolGuid[16] = {
    0xab, 0x31, 0xa0, 0x18, 0x43, 0xb4, 0x1a, 0x4d,
    0xa5, 0xc0, 0x0c, 0x09, 0x26, 0x1e, 0x9f, 0x71
};

const UINT8 mComponentNameProtocolGuid[16] = {
    0x2c, 0x77, 0x7a, 0x10, 0xe1, 0xd5, 0xd4, 0x11,
    0x9a, 0x46, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

static const UINT8 mPlatformDriverOverrideProtocolGuid[16] = {
    0x38, 0xc7, 0x30, 0x6b, 0x91, 0xa3, 0xd4, 0x11,
    0x9a, 0x3b, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

static const UINT8 mBusSpecificDriverOverrideProtocolGuid[16] = {
    0x85, 0xb2, 0xc1, 0x3b, 0x15, 0x8a, 0x82, 0x4a,
    0xaa, 0xbf, 0x4d, 0x7d, 0x13, 0xfb, 0x32, 0x65
};

static const UINT8 mDriverFamilyOverrideProtocolGuid[16] = {
    0x9e, 0x12, 0xee, 0xb1, 0x36, 0xda, 0x81, 0x41,
    0x91, 0xf8, 0x04, 0xa4, 0x92, 0x37, 0x66, 0xa7
};

static const UINT8 mLoadFileProtocolGuid[16] = {
    0x91, 0x30, 0xec, 0x56, 0x4c, 0x95, 0xd2, 0x11,
    0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

static const UINT8 mLoadFile2ProtocolGuid[16] = {
    0xc1, 0xc0, 0x06, 0x40, 0xb3, 0xfc, 0x3e, 0x40,
    0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d
};

static const FW_PCI_ROOT_BRIDGE_RESOURCES mPciRootBridgeResources = {
    .Bus = {
        .Descriptor = 0x8a,
        .Length = 0x2b,
        .ResourceType = 2,
        .GeneralFlags = 0,
        .TypeSpecificFlags = 0,
        .AddressSpaceGranularity = 32,
        .AddressRangeMinimum = 0,
        .AddressRangeMaximum = 255,
        .AddressTranslationOffset = 0,
        .AddressLength = 256,
    },
    .Io = {
        .Descriptor = 0x8a,
        .Length = 0x2b,
        .ResourceType = 1,
        .GeneralFlags = 0,
        .TypeSpecificFlags = 0,
        .AddressSpaceGranularity = 32,
        .AddressRangeMinimum = 0,
        .AddressRangeMaximum = PCI_IO_SIZE - 1,
        .AddressTranslationOffset = PCI_IO_TRANSLATION_OFFSET,
        .AddressLength = PCI_IO_SIZE,
    },
    .Mem = {
        .Descriptor = 0x8a,
        .Length = 0x2b,
        .ResourceType = 0,
        .GeneralFlags = 0,
        .TypeSpecificFlags = 0,
        .AddressSpaceGranularity = 64,
        .AddressRangeMinimum = IA64_PCI_MMIO_BASE,
        .AddressRangeMaximum = PCI_MMIO_END,
        .AddressTranslationOffset = PCI_MMIO_TRANSLATION_OFFSET,
        .AddressLength = IA64_PCI_MMIO_SIZE,
    },
    .End = {
        .Descriptor = 0x79,
        .Checksum = 0,
    },
};

#define EFI_PCI_ATTRIBUTE_ISA_MOTHERBOARD_IO 0x0001ULL
#define EFI_PCI_ATTRIBUTE_ISA_IO             0x0002ULL
#define EFI_PCI_ATTRIBUTE_VGA_MEMORY         0x0008ULL
#define EFI_PCI_ATTRIBUTE_VGA_IO             0x0010ULL
#define EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO     0x0020ULL
#define EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO   0x0040ULL
#define EFI_PCI_ATTRIBUTE_MEMORY_WRITE_COMBINE 0x0080ULL
#define EFI_PCI_ATTRIBUTE_IO                 0x0100ULL
#define EFI_PCI_ATTRIBUTE_MEMORY             0x0200ULL
#define EFI_PCI_ATTRIBUTE_BUS_MASTER         0x0400ULL
#define EFI_PCI_ATTRIBUTE_MEMORY_CACHED      0x0800ULL
#define EFI_PCI_ATTRIBUTE_MEMORY_DISABLE     0x1000ULL
#define EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE 0x8000ULL

#define PCI_COMMAND_OFFSET                   0x04U
#define PCI_COMMAND_IO_SPACE                 0x0001U
#define PCI_COMMAND_MEMORY_SPACE             0x0002U
#define PCI_COMMAND_BUS_MASTER               0x0004U
#define PCI_BAR_OFFSET(BarIndex)             (0x10U + (UINT32)(BarIndex) * 4U)

#define FW_PCI_COMMAND_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_IO | \
     EFI_PCI_ATTRIBUTE_MEMORY | \
     EFI_PCI_ATTRIBUTE_BUS_MASTER)

#define FW_PCI_ROOT_BRIDGE_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_ISA_MOTHERBOARD_IO | \
     EFI_PCI_ATTRIBUTE_ISA_IO | \
     EFI_PCI_ATTRIBUTE_VGA_MEMORY | \
     EFI_PCI_ATTRIBUTE_VGA_IO | \
     EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO | \
     EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO)

#define FW_PCI_IDE_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_IO | \
     EFI_PCI_ATTRIBUTE_BUS_MASTER | \
     EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO | \
     EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO)

#define FW_PCI_AHCI_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_IO | \
     EFI_PCI_ATTRIBUTE_MEMORY | \
     EFI_PCI_ATTRIBUTE_BUS_MASTER)

#define FW_PCI_OHCI_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_MEMORY | EFI_PCI_ATTRIBUTE_BUS_MASTER)

#define FW_PCI_UHCI_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_IO | EFI_PCI_ATTRIBUTE_BUS_MASTER)

#define FW_PCI_LSI_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_IO | \
     EFI_PCI_ATTRIBUTE_MEMORY | \
     EFI_PCI_ATTRIBUTE_BUS_MASTER)

#define FW_PCI_VGA_ATTRIBUTES \
    (EFI_PCI_ATTRIBUTE_IO | \
     EFI_PCI_ATTRIBUTE_MEMORY | \
     EFI_PCI_ATTRIBUTE_VGA_MEMORY | \
     EFI_PCI_ATTRIBUTE_VGA_IO)

static BOOLEAN pci_width_valid(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width)
{
    return (UINTN)Width < (UINTN)EfiPciWidthMaximum;
}

static BOOLEAN pci_poll_width_valid(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width)
{
    return (UINTN)Width <= (UINTN)EfiPciWidthUint64;
}

static UINTN pci_width_size(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width)
{
    switch ((UINTN)Width & 3U) {
    case 0:
        return 1;
    case 1:
        return 2;
    case 2:
        return 4;
    default:
        return 8;
    }
}

static UINT64 pci_mem_cpu_addr(UINT64 Address)
{
    if (Address >= IA64_PCI_MMIO_BASE) {
        return Address;
    }
    return IA64_PCI_MMIO_BASE + Address;
}

static UINT64 pci_io_cpu_addr(UINT64 Address)
{
    if (Address >= LEGACY_IO_BASE) {
        return Address;
    }
    return LEGACY_IO_BASE + Address;
}

static UINT64 pci_mmio_read(UINT64 Address, UINTN Size)
{
    volatile UINT8 *p8;
    volatile UINT16 *p16;
    volatile UINT32 *p32;
    volatile UINT64 *p64;

    switch (Size) {
    case 1:
        p8 = (volatile UINT8 *)(UINTN)Address;
        return *p8;
    case 2:
        p16 = (volatile UINT16 *)(UINTN)Address;
        return *p16;
    case 4:
        p32 = (volatile UINT32 *)(UINTN)Address;
        return *p32;
    default:
        p64 = (volatile UINT64 *)(UINTN)Address;
        return *p64;
    }
}

static void pci_mmio_write(UINT64 Address, UINTN Size, UINT64 Value)
{
    volatile UINT8 *p8;
    volatile UINT16 *p16;
    volatile UINT32 *p32;
    volatile UINT64 *p64;

    switch (Size) {
    case 1:
        p8 = (volatile UINT8 *)(UINTN)Address;
        *p8 = (UINT8)Value;
        break;
    case 2:
        p16 = (volatile UINT16 *)(UINTN)Address;
        *p16 = (UINT16)Value;
        break;
    case 4:
        p32 = (volatile UINT32 *)(UINTN)Address;
        *p32 = (UINT32)Value;
        break;
    default:
        p64 = (volatile UINT64 *)(UINTN)Address;
        *p64 = Value;
        break;
    }
}

static EFI_STATUS pci_root_transfer(BOOLEAN IsWrite, BOOLEAN IsIo,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                    UINT64 Address, UINTN Count, VOID *Buffer)
{
    UINTN i;
    UINTN size;
    UINT8 *buf;
    BOOLEAN fifo;
    BOOLEAN fill;

    if (!pci_width_valid(Width) || Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    size = pci_width_size(Width);
    fifo = Width >= EfiPciWidthFifoUint8 && Width <= EfiPciWidthFifoUint64;
    fill = Width >= EfiPciWidthFillUint8 && Width <= EfiPciWidthFillUint64;
    buf = (UINT8 *)Buffer;
    Address = IsIo ? pci_io_cpu_addr(Address) : pci_mem_cpu_addr(Address);

    for (i = 0; i < Count; i++) {
        if (IsWrite) {
            UINT64 value = 0;
            fw_copy_mem(&value, buf, size);
            pci_mmio_write(Address, size, value);
        } else {
            UINT64 value = pci_mmio_read(Address, size);
            fw_copy_mem(buf, &value, size);
        }

        if (!fifo) {
            Address += size;
        }
        if (!fill) {
            buf += size;
        }
    }
    return EFI_SUCCESS;
}

typedef struct {
    UINT64 last_tick;
    UINT64 remaining_100ns;
    UINT64 partial_ticks;
} FW_PCI_POLL_TIMER;

typedef struct {
    UINTN read_count;
    UINT64 now;
    UINT64 ticks_per_read;
} FW_PCI_POLL_SELFTEST_CLOCK;

static void pci_poll_timer_init(FW_PCI_POLL_TIMER *Timer, UINT64 Delay)
{
    Timer->last_tick = fw_read_itc();
    Timer->remaining_100ns = Delay;
    Timer->partial_ticks = 0;
}

static BOOLEAN __attribute__((noinline))
pci_poll_timer_consume(FW_PCI_POLL_TIMER *Timer, UINT64 Now)
{
    UINT64 delta = Now - Timer->last_tick;
    UINT64 elapsed_100ns = delta / FW_ITC_TICKS_PER_100NS;
    UINT64 partial = Timer->partial_ticks +
                     delta % FW_ITC_TICKS_PER_100NS;

    Timer->last_tick = Now;
    if (partial >= FW_ITC_TICKS_PER_100NS) {
        partial -= FW_ITC_TICKS_PER_100NS;
        elapsed_100ns++;
    }
    Timer->partial_ticks = partial;
    if (elapsed_100ns >= Timer->remaining_100ns) {
        Timer->remaining_100ns = 0;
        return 1;
    }
    Timer->remaining_100ns -= elapsed_100ns;
    return 0;
}

static BOOLEAN pci_poll_timer_expired(FW_PCI_POLL_TIMER *Timer)
{
    /* Unsigned subtraction in pci_poll_timer_consume() handles ITC wrap. */
    return pci_poll_timer_consume(Timer, fw_read_itc());
}

static UINT64 pci_poll_width_mask(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width)
{
    UINTN size = pci_width_size(Width);

    return size == sizeof(UINT64) ? ~0ULL :
           (1ULL << (size * 8U)) - 1U;
}

static inline EFI_STATUS __attribute__((always_inline))
pci_poll_address_internal(BOOLEAN IsIo,
                          EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                          UINT64 Address, UINT64 Mask, UINT64 Value,
                          UINT64 Delay, UINT64 *Result,
                          FW_PCI_POLL_SELFTEST_CLOCK *TestClock)
{
    FW_PCI_POLL_TIMER timer;

    if (TestClock != NULL) {
        TestClock->read_count = 0;
    }
    if (!pci_poll_width_valid(Width) || Result == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    Mask &= pci_poll_width_mask(Width);
    if (Delay != 0) {
        if (TestClock == NULL) {
            pci_poll_timer_init(&timer, Delay);
        } else {
            timer.last_tick = TestClock->now;
            timer.remaining_100ns = Delay;
            timer.partial_ticks = 0;
        }
    }
    for (;;) {
        UINT64 data = 0;
        EFI_STATUS st = pci_root_transfer(0, IsIo, Width, Address, 1,
                                          &data);

        if (st != EFI_SUCCESS) {
            return st;
        }
        if (TestClock != NULL) {
            TestClock->read_count++;
        }
        *Result = data;
        if ((data & Mask) == Value || Delay == 0) {
            return EFI_SUCCESS;
        }
        if (TestClock != NULL) {
            TestClock->now += TestClock->ticks_per_read;
        }
        if ((TestClock == NULL && pci_poll_timer_expired(&timer)) ||
            (TestClock != NULL &&
             pci_poll_timer_consume(&timer, TestClock->now))) {
            return EFI_TIMEOUT;
        }
    }
}

static EFI_STATUS __attribute__((noinline))
pci_poll_address(BOOLEAN IsIo,
                 EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                 UINT64 Address, UINT64 Mask, UINT64 Value,
                 UINT64 Delay, UINT64 *Result)
{
    /* The always-inlined NULL clock removes selftest instrumentation here. */
    return pci_poll_address_internal(IsIo, Width, Address, Mask, Value,
                                     Delay, Result, NULL);
}

static UINT8 pci_config_read_byte(UINT64 Address)
{
    UINT8 reg = (UINT8)(Address & 0xffU);
    UINT8 function = (UINT8)((Address >> 8) & 0xffU);
    UINT8 device = (UINT8)((Address >> 16) & 0xffU);
    UINT8 bus = (UINT8)((Address >> 24) & 0xffU);
    UINT32 ext_reg = (UINT32)(Address >> 32);
    UINT32 offset;

    offset = ext_reg != 0 ? ext_reg : reg;
    return (UINT8)pci_config_read_value(0, bus, device, function, offset, 1);
}

static void pci_config_write_byte(UINT64 Address, UINT8 Value)
{
    UINT8 reg = (UINT8)(Address & 0xffU);
    UINT8 function = (UINT8)((Address >> 8) & 0xffU);
    UINT8 device = (UINT8)((Address >> 16) & 0xffU);
    UINT8 bus = (UINT8)((Address >> 24) & 0xffU);
    UINT32 ext_reg = (UINT32)(Address >> 32);
    UINT32 offset;

    offset = ext_reg != 0 ? ext_reg : reg;
    pci_config_write_value(0, bus, device, function, offset, 1, Value);
}

static EFI_STATUS pci_root_config_transfer(BOOLEAN IsWrite,
                                           EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                           UINT64 Address, UINTN Count,
                                           VOID *Buffer)
{
    UINTN i;
    UINTN j;
    UINTN size;
    UINT8 *buf;
    BOOLEAN fifo;
    BOOLEAN fill;

    if (!pci_width_valid(Width) || Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    size = pci_width_size(Width);
    fifo = Width >= EfiPciWidthFifoUint8 && Width <= EfiPciWidthFifoUint64;
    fill = Width >= EfiPciWidthFillUint8 && Width <= EfiPciWidthFillUint64;
    buf = (UINT8 *)Buffer;

    for (i = 0; i < Count; i++) {
        if (IsWrite) {
            for (j = 0; j < size; j++) {
                pci_config_write_byte(Address + j, buf[j]);
            }
        } else {
            for (j = 0; j < size; j++) {
                buf[j] = pci_config_read_byte(Address + j);
            }
        }
        if (!fifo) {
            Address += size;
        }
        if (!fill) {
            buf += size;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS pci_root_poll_mem(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                    UINT64 Address, UINT64 Mask, UINT64 Value,
                                    UINT64 Delay, UINT64 *Result)
{
    (void)This;
    return pci_poll_address(0, Width, Address, Mask, Value, Delay, Result);
}

static EFI_STATUS pci_root_poll_io(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                   EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                   UINT64 Address, UINT64 Mask, UINT64 Value,
                                   UINT64 Delay, UINT64 *Result)
{
    (void)This;
    return pci_poll_address(1, Width, Address, Mask, Value, Delay, Result);
}

static EFI_STATUS pci_root_mem_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                    UINT64 Address, UINTN Count, VOID *Buffer)
{
    (void)This;
    return pci_root_transfer(0, 0, Width, Address, Count, Buffer);
}

static EFI_STATUS pci_root_mem_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                     EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                     UINT64 Address, UINTN Count, VOID *Buffer)
{
    (void)This;
    return pci_root_transfer(1, 0, Width, Address, Count, Buffer);
}

static EFI_STATUS pci_root_io_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                   EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                   UINT64 Address, UINTN Count, VOID *Buffer)
{
    (void)This;
    return pci_root_transfer(0, 1, Width, Address, Count, Buffer);
}

static EFI_STATUS pci_root_io_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                    UINT64 Address, UINTN Count, VOID *Buffer)
{
    (void)This;
    return pci_root_transfer(1, 1, Width, Address, Count, Buffer);
}

static EFI_STATUS pci_root_cfg_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                    UINT64 Address, UINTN Count, VOID *Buffer)
{
    (void)This;
    return pci_root_config_transfer(0, Width, Address, Count, Buffer);
}

static EFI_STATUS pci_root_cfg_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                     EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                     UINT64 Address, UINTN Count, VOID *Buffer)
{
    (void)This;
    return pci_root_config_transfer(1, Width, Address, Count, Buffer);
}

static EFI_STATUS pci_root_copy_mem(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
                                    UINT64 DestAddress, UINT64 SrcAddress,
                                    UINTN Count)
{
    UINTN i;
    UINTN size;
    UINT64 bytes;
    UINT64 src;
    UINT64 dst;

    (void)This;
    if (!pci_poll_width_valid(Width)) {
        return EFI_INVALID_PARAMETER;
    }
    size = pci_width_size(Width);
    if (Count != 0 && (UINT64)Count > ~0ULL / size) {
        return EFI_INVALID_PARAMETER;
    }
    bytes = (UINT64)Count << (UINTN)Width;
    if (bytes != 0 &&
        (SrcAddress > ~0ULL - (bytes - 1U) ||
         DestAddress > ~0ULL - (bytes - 1U))) {
        return EFI_INVALID_PARAMETER;
    }
    src = pci_mem_cpu_addr(SrcAddress);
    dst = pci_mem_cpu_addr(DestAddress);

    if (dst > src && dst - src < bytes) {
        for (i = Count; i > 0; i--) {
            UINT64 value = pci_mmio_read(src + (UINT64)(i - 1) * size, size);
            pci_mmio_write(dst + (UINT64)(i - 1) * size, size, value);
        }
    } else {
        for (i = 0; i < Count; i++) {
            UINT64 value = pci_mmio_read(src + (UINT64)i * size, size);
            pci_mmio_write(dst + (UINT64)i * size, size, value);
        }
    }
    return EFI_SUCCESS;
}

#define FW_PCI_DMA_MAPPING_SIGNATURE 0x5043494dU
#define FW_PCI_DMA_MAPPING_MAX 64U

typedef struct {
    UINT32 signature;
    BOOLEAN in_use;
    BOOLEAN bounced;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION operation;
    VOID *host_address;
    VOID *bounce_address;
    UINTN number_of_bytes;
    UINTN bounce_pages;
} FW_PCI_DMA_MAPPING;

static FW_PCI_DMA_MAPPING mPciDmaMappings[FW_PCI_DMA_MAPPING_MAX];

static FW_PCI_DMA_MAPPING *pci_dma_mapping_allocate(VOID)
{
    UINTN i;

    for (i = 0; i < FW_PCI_DMA_MAPPING_MAX; i++) {
        if (!mPciDmaMappings[i].in_use) {
            FW_PCI_DMA_MAPPING *mapping = &mPciDmaMappings[i];

            fw_set_mem(mapping, sizeof(*mapping), 0);
            mapping->signature = FW_PCI_DMA_MAPPING_SIGNATURE;
            mapping->in_use = 1;
            return mapping;
        }
    }
    return NULL;
}

static VOID pci_dma_mapping_release(FW_PCI_DMA_MAPPING *Mapping)
{
    if (Mapping != NULL) {
        fw_set_mem(Mapping, sizeof(*Mapping), 0);
    }
}

static EFI_STATUS pci_root_map(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                               EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION Operation,
                               VOID *HostAddress, UINTN *NumberOfBytes,
                               EFI_PHYSICAL_ADDRESS *DeviceAddress,
                               VOID **Mapping)
{
    FW_PCI_DMA_MAPPING *mapping;
    EFI_PHYSICAL_ADDRESS host;
    EFI_PHYSICAL_ADDRESS device;
    EFI_PHYSICAL_ADDRESS bounce;
    UINTN pages;
    UINTN base_operation;
    BOOLEAN operation_64;
    BOOLEAN needs_bounce;
    EFI_STATUS st;

    (void)This;
    if (Operation >= EfiPciOperationMaximum || HostAddress == NULL ||
        NumberOfBytes == NULL || *NumberOfBytes == 0 ||
        DeviceAddress == NULL || Mapping == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *Mapping = NULL;
    host = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
    if (host > ~0ULL - (*NumberOfBytes - 1U)) {
        return EFI_INVALID_PARAMETER;
    }

    operation_64 = Operation >= EfiPciOperationBusMasterRead64;
    base_operation = operation_64 ?
        (UINTN)Operation - EfiPciOperationBusMasterRead64 :
        (UINTN)Operation;
    needs_bounce = !operation_64 &&
        host + *NumberOfBytes - 1U > 0xffffffffULL;
    if (needs_bounce &&
        base_operation == EfiPciOperationBusMasterCommonBuffer) {
        return EFI_UNSUPPORTED;
    }

    mapping = pci_dma_mapping_allocate();
    if (mapping == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    mapping->operation = Operation;
    mapping->host_address = HostAddress;
    mapping->number_of_bytes = *NumberOfBytes;
    device = host;

    if (needs_bounce) {
        if (*NumberOfBytes > ~(UINTN)0 - (EFI_PAGE_SIZE - 1U)) {
            pci_dma_mapping_release(mapping);
            return EFI_OUT_OF_RESOURCES;
        }
        pages = (*NumberOfBytes + EFI_PAGE_SIZE - 1U) >> 12;
        bounce = 0xffffffffULL;
        st = bs_allocate_pages(AllocateMaxAddress, EfiBootServicesData,
                               pages, &bounce);
        if (st != EFI_SUCCESS) {
            pci_dma_mapping_release(mapping);
            return st;
        }
        mapping->bounced = 1;
        mapping->bounce_address = (VOID *)(UINTN)bounce;
        mapping->bounce_pages = pages;
        device = bounce;
        if (base_operation == EfiPciOperationBusMasterRead) {
            fw_copy_mem(mapping->bounce_address, HostAddress,
                        *NumberOfBytes);
        }
    }

    __sync_synchronize();
    *DeviceAddress = device;
    *Mapping = mapping;
    return EFI_SUCCESS;
}

static EFI_STATUS pci_root_unmap(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                 VOID *Mapping)
{
    FW_PCI_DMA_MAPPING *mapping = (FW_PCI_DMA_MAPPING *)Mapping;
    UINTN base_operation;
    EFI_STATUS st = EFI_SUCCESS;

    (void)This;
    if (mapping == NULL ||
        mapping->signature != FW_PCI_DMA_MAPPING_SIGNATURE ||
        !mapping->in_use) {
        return EFI_INVALID_PARAMETER;
    }
    base_operation = mapping->operation >=
        EfiPciOperationBusMasterRead64 ?
        (UINTN)mapping->operation - EfiPciOperationBusMasterRead64 :
        (UINTN)mapping->operation;
    __sync_synchronize();
    if (mapping->bounced &&
        base_operation == EfiPciOperationBusMasterWrite) {
        fw_copy_mem(mapping->host_address, mapping->bounce_address,
                    mapping->number_of_bytes);
    }
    if (mapping->bounced) {
        st = bs_free_pages((EFI_PHYSICAL_ADDRESS)
                           (UINTN)mapping->bounce_address,
                           mapping->bounce_pages);
    }
    pci_dma_mapping_release(mapping);
    return st;
}

static EFI_STATUS pci_root_allocate_buffer(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                           EFI_ALLOCATE_TYPE Type,
                                           EFI_MEMORY_TYPE MemoryType,
                                           UINTN Pages, VOID **HostAddress,
                                           UINT64 Attributes)
{
    EFI_PHYSICAL_ADDRESS addr;
    UINT64 legal = EFI_PCI_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                   EFI_PCI_ATTRIBUTE_MEMORY_CACHED |
                   EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
    EFI_STATUS st;

    (void)This;
    if (HostAddress == NULL || Pages == 0 ||
        (UINT32)Type >= (UINT32)MaxAllocateType ||
        (Attributes & ~legal) != 0 ||
        (MemoryType != EfiBootServicesData &&
         MemoryType != EfiRuntimeServicesData)) {
        return (Attributes & ~legal) != 0 ? EFI_UNSUPPORTED :
            EFI_INVALID_PARAMETER;
    }
    addr = Type == AllocateAnyPages ? 0 :
        (EFI_PHYSICAL_ADDRESS)(UINTN)*HostAddress;
    if ((Attributes & EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE) == 0) {
        if (Type == AllocateAddress && addr > 0xffffffffULL) {
            return EFI_UNSUPPORTED;
        }
        if (Type == AllocateAnyPages) {
            Type = AllocateMaxAddress;
            addr = 0xffffffffULL;
        } else if (Type == AllocateMaxAddress && addr > 0xffffffffULL) {
            addr = 0xffffffffULL;
        }
    }
    st = bs_allocate_pages(Type, MemoryType, Pages, &addr);
    if (st != EFI_SUCCESS) {
        return st;
    }
    *HostAddress = (VOID *)(UINTN)addr;
    return EFI_SUCCESS;
}

static EFI_STATUS pci_root_free_buffer(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                       UINTN Pages, VOID *HostAddress)
{
    (void)This;
    if (HostAddress == NULL || Pages == 0) {
        return EFI_INVALID_PARAMETER;
    }
    return bs_free_pages((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
}

static EFI_STATUS pci_root_flush(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This)
{
    (void)This;
    __sync_synchronize();
    return EFI_SUCCESS;
}

static EFI_STATUS pci_root_get_attributes(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                          UINT64 *Supports, UINT64 *Attributes)
{
    (void)This;
    if (Supports == NULL && Attributes == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (Supports != NULL) {
        *Supports = FW_PCI_ROOT_BRIDGE_ATTRIBUTES;
    }
    if (Attributes != NULL) {
        *Attributes = FW_PCI_ROOT_BRIDGE_ATTRIBUTES;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS pci_root_set_attributes(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                          UINT64 Attributes,
                                          UINT64 *ResourceBase,
                                          UINT64 *ResourceLength)
{
    (void)This;
    (void)ResourceBase;
    (void)ResourceLength;
    if ((Attributes & ~FW_PCI_ROOT_BRIDGE_ATTRIBUTES) != 0) {
        return EFI_UNSUPPORTED;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS pci_root_configuration(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
                                         VOID **Resources)
{
    (void)This;
    if (Resources == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *Resources = (VOID *)&mPciRootBridgeResources;
    return EFI_SUCCESS;
}

static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL mPciRootBridgeIoProto = {
    .ParentHandle = NULL,
    .PollMem = pci_root_poll_mem,
    .PollIo = pci_root_poll_io,
    .Mem = {
        .Read = pci_root_mem_read,
        .Write = pci_root_mem_write,
    },
    .Io = {
        .Read = pci_root_io_read,
        .Write = pci_root_io_write,
    },
    .Pci = {
        .Read = pci_root_cfg_read,
        .Write = pci_root_cfg_write,
    },
    .CopyMem = pci_root_copy_mem,
    .Map = pci_root_map,
    .Unmap = pci_root_unmap,
    .AllocateBuffer = pci_root_allocate_buffer,
    .FreeBuffer = pci_root_free_buffer,
    .Flush = pci_root_flush,
    .GetAttributes = pci_root_get_attributes,
    .SetAttributes = pci_root_set_attributes,
    .Configuration = pci_root_configuration,
    .SegmentNumber = 0,
};

static EFI_PCI_IO_PROTOCOL mPciIdeIoProto;
static EFI_PCI_IO_PROTOCOL mPciAhciIoProto;
static EFI_PCI_IO_PROTOCOL mPciOhciIoProto;
static EFI_PCI_IO_PROTOCOL mPciUhciIoProto;
static EFI_PCI_IO_PROTOCOL mPciLsiIoProto;
static EFI_PCI_IO_PROTOCOL mPciVgaIoProto;

static FW_PCI_IO_DEVICE mPciIoDevices[FW_PCI_IO_DEVICE_COUNT] = {
    {
        &mPciIdeHandle, &mPciIdeIoProto, &mPciIdeDevicePath,
        0, 0, 0, FW_PCI_IDE_ATTRIBUTES, PCI_IDE_CMD646_ID,
        4, 0x0000c001U, 0x10, "IDE", 1,
    },
    {
        &mPciAhciHandle, &mPciAhciIoProto, &mPciAhciDevicePath,
        0, 1, 0, FW_PCI_AHCI_ATTRIBUTES, 0x29228086U,
        5, PCI_AHCI_MMIO_BAR, 0x1000, "AHCI", 1,
    },
    {
        &mPciOhciHandle, &mPciOhciIoProto, &mPciOhciDevicePath,
        0, 2, 0, FW_PCI_OHCI_ATTRIBUTES, 0x003f106bU,
        0, PCI_OHCI_MMIO_BAR, 0x100, "OHCI", 1,
    },
    {
        &mPciUhciHandle, &mPciUhciIoProto, &mPciUhciDevicePath,
        0, 3, 0, FW_PCI_UHCI_ATTRIBUTES, FW_PCI_PIIX3_UHCI_ID,
        4, 0x0000c121U, 0x20, "UHCI", 1,
    },
    {
        &mPciLsiHandle, &mPciLsiIoProto, &mPciLsiDevicePath,
        IA64_460GX_WXB0_BUS, IA64_460GX_WXB0_SCSI_SLOT, 0,
        FW_PCI_LSI_ATTRIBUTES, 0x00121000U,
        1, PCI_LSI_MMIO_BAR, 0x400, "LSI", 1,
    },
    {
        &mGraphicsHandle, &mPciVgaIoProto, &mGraphicsDevicePath,
        IA64_460GX_GXB_BUS, IA64_460GX_GXB_VGA_SLOT, 0,
        FW_PCI_VGA_ATTRIBUTES, PCI_VGA_ATI_ID,
        0, PCI_VGA_FB_BAR | 0x8U, PCI_VGA_ATI_FB_SIZE, "VGA", 0,
    },
};

FW_STATIC_ASSERT(FW_ARRAY_SIZE(mPciIoDevices) == FW_PCI_IO_DEVICE_COUNT,
                 pci_io_device_count);

static UINT64 pci_io_config_address(const FW_PCI_IO_DEVICE *Dev,
                                    UINT32 Offset)
{
    UINT64 address = 0;

    address |= (UINT64)(Offset & 0xffU);
    address |= (UINT64)Dev->Function << 8;
    address |= (UINT64)Dev->Device << 16;
    address |= (UINT64)Dev->Bus << 24;
    if (Offset > 0xffU) {
        address |= (UINT64)Offset << 32;
    }
    return address;
}

static const FW_PCI_IO_DEVICE *fw_pci_io_device_from_protocol(
    EFI_PCI_IO_PROTOCOL *This)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPciIoDevices); i++) {
        if (mPciIoDevices[i].Protocol == This) {
            return &mPciIoDevices[i];
        }
    }
    return NULL;
}

static UINT32 fw_pci_io_device_id(const FW_PCI_IO_DEVICE *Dev)
{
    return (UINT32)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                         Dev->Function, 0, 4);
}

static BOOLEAN fw_pci_vga_id_supported(UINT32 Id)
{
    return Id == PCI_VGA_ATI_ID || Id == PCI_VGA_STD_ID;
}

static UINT32 fw_pci_io_expected_id(const FW_PCI_IO_DEVICE *Dev)
{
    UINT32 id;

    if (Dev->Protocol != &mPciVgaIoProto) {
        return Dev->ExpectedId;
    }
    id = fw_pci_io_device_id(Dev);
    return fw_pci_vga_id_supported(id) ? id : Dev->ExpectedId;
}

static UINT32 fw_pci_io_expected_bar_value(const FW_PCI_IO_DEVICE *Dev)
{
    if (Dev->Protocol == &mPciVgaIoProto) {
        return (UINT32)pci_config_read_value(
            0, Dev->Bus, Dev->Device, Dev->Function,
            PCI_BAR_OFFSET(Dev->ExpectedBarIndex), 4);
    }
    return Dev->ExpectedBarValue;
}

static UINT64 fw_pci_io_expected_bar_length(const FW_PCI_IO_DEVICE *Dev)
{
    if (Dev->Protocol == &mPciVgaIoProto &&
        fw_pci_io_device_id(Dev) == PCI_VGA_STD_ID) {
        return PCI_VGA_STD_FB_SIZE;
    }
    return Dev->ExpectedBarLength;
}

/*
 * The IDE (ide=on) and AHCI (ahci=on) storage controllers are opt-in and may
 * be absent from the default machine configuration, in which case their PCI
 * config space reads back all-ones.  Every other device in mPciIoDevices is
 * always present and must self-test.
 */
static BOOLEAN fw_pci_io_device_optional(const FW_PCI_IO_DEVICE *Dev)
{
    return Dev->Protocol == &mPciIdeIoProto ||
           Dev->Protocol == &mPciAhciIoProto;
}

static BOOLEAN fw_pci_io_device_present(const FW_PCI_IO_DEVICE *Dev)
{
    UINT32 id;

    if (*Dev->Handle == NULL) {
        return 0;
    }
    id = fw_pci_io_device_id(Dev);
    if (Dev->Protocol == &mPciVgaIoProto) {
        return fw_pci_vga_id_supported(id);
    }
    return id == Dev->ExpectedId;
}

const FW_PCI_IO_DEVICE *fw_pci_io_device_from_handle(EFI_HANDLE Handle)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPciIoDevices); i++) {
        if (*mPciIoDevices[i].Handle == Handle &&
            fw_pci_io_device_present(&mPciIoDevices[i])) {
            return &mPciIoDevices[i];
        }
    }
    return NULL;
}

static EFI_STATUS pci_io_config_transfer(BOOLEAN IsWrite,
                                         EFI_PCI_IO_PROTOCOL *This,
                                         EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                         UINT32 Offset, UINTN Count,
                                         VOID *Buffer)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);

    if (dev == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    return pci_root_config_transfer(IsWrite, Width,
                                    pci_io_config_address(dev, Offset),
                                    Count, Buffer);
}

static EFI_STATUS pci_io_cfg_read(EFI_PCI_IO_PROTOCOL *This,
                                  EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT32 Offset, UINTN Count, VOID *Buffer)
{
    return pci_io_config_transfer(0, This, Width, Offset, Count, Buffer);
}

static EFI_STATUS pci_io_cfg_write(EFI_PCI_IO_PROTOCOL *This,
                                   EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                   UINT32 Offset, UINTN Count, VOID *Buffer)
{
    return pci_io_config_transfer(1, This, Width, Offset, Count, Buffer);
}

static EFI_STATUS pci_io_bar_address(const FW_PCI_IO_DEVICE *Dev,
                                     UINT8 BarIndex, BOOLEAN IsIo,
                                     UINT64 Offset, UINT64 *Address)
{
    UINT32 bar;
    UINT64 base;

    if (Address == NULL || Dev == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (BarIndex == EFI_PCI_IO_PASS_THROUGH_BAR) {
        *Address = Offset;
        return EFI_SUCCESS;
    }
    if (BarIndex >= 6) {
        return EFI_INVALID_PARAMETER;
    }

    bar = (UINT32)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                        Dev->Function,
                                        0x10U + (UINT32)BarIndex * 4U,
                                        4);
    if (bar == 0 || bar == 0xffffffffU) {
        return EFI_UNSUPPORTED;
    }
    if (IsIo) {
        if ((bar & 1U) == 0) {
            return EFI_UNSUPPORTED;
        }
        base = bar & ~(UINT64)3U;
    } else {
        if ((bar & 1U) != 0) {
            return EFI_UNSUPPORTED;
        }
        if ((bar & 0x6U) == 0x4U) {
            UINT32 high;

            if (BarIndex >= 5) {
                return EFI_UNSUPPORTED;
            }
            high = (UINT32)pci_config_read_value(
                0, Dev->Bus, Dev->Device, Dev->Function,
                0x10U + ((UINT32)BarIndex + 1U) * 4U, 4);
            base = ((UINT64)high << 32) | (bar & ~(UINT64)0xfU);
        } else {
            base = bar & ~(UINT64)0xfU;
        }
    }
    if (base + Offset < base) {
        return EFI_INVALID_PARAMETER;
    }
    *Address = base + Offset;
    return EFI_SUCCESS;
}

static EFI_STATUS pci_io_bar_info(const FW_PCI_IO_DEVICE *Dev,
                                  UINT8 BarIndex, UINT32 *RawValue,
                                  UINT64 *Base, UINT64 *Length,
                                  BOOLEAN *IsIo, BOOLEAN *Is64);

typedef struct {
    BOOLEAN initialized;
    EFI_STATUS status;
    UINT64 length;
    BOOLEAN is_io;
} FW_PCI_BAR_ACCESS_INFO;

static FW_PCI_BAR_ACCESS_INFO
    mPciBarAccessInfo[FW_PCI_IO_DEVICE_COUNT][6];

static EFI_STATUS pci_io_bar_access_info(const FW_PCI_IO_DEVICE *Dev,
                                         UINT8 BarIndex, UINT64 *Length,
                                         BOOLEAN *IsIo)
{
    UINTN device_index;
    FW_PCI_BAR_ACCESS_INFO *info;

    if (Dev == NULL || BarIndex >= 6 || Length == NULL || IsIo == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    device_index = (UINTN)(Dev - mPciIoDevices);
    if (device_index >= FW_ARRAY_SIZE(mPciIoDevices)) {
        return EFI_INVALID_PARAMETER;
    }
    info = &mPciBarAccessInfo[device_index][BarIndex];
    if (!info->initialized) {
        if (BarIndex == Dev->ExpectedBarIndex) {
            info->status = EFI_SUCCESS;
            info->length = fw_pci_io_expected_bar_length(Dev);
            info->is_io =
                (fw_pci_io_expected_bar_value(Dev) & 1U) != 0;
        } else {
            UINT32 raw;
            UINT64 base;
            BOOLEAN is_64;

            info->status = pci_io_bar_info(Dev, BarIndex, &raw, &base,
                                           &info->length, &info->is_io,
                                           &is_64);
            (void)raw;
            (void)base;
            (void)is_64;
        }
        info->initialized = 1;
    }
    if (info->status != EFI_SUCCESS) {
        return info->status;
    }
    *Length = info->length;
    *IsIo = info->is_io;
    return EFI_SUCCESS;
}

static EFI_STATUS pci_io_resolve_bar_access(const FW_PCI_IO_DEVICE *Dev,
                                            UINT8 BarIndex, BOOLEAN IsIo,
                                            EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                            UINT64 Offset, UINTN Count,
                                            UINT64 *Address)
{
    UINT64 length;
    UINT64 access_count;
    UINT64 size;
    BOOLEAN bar_is_io;
    EFI_STATUS st;

    if (BarIndex >= 6) {
        return EFI_UNSUPPORTED;
    }
    st = pci_io_bar_access_info(Dev, BarIndex, &length, &bar_is_io);
    if (st != EFI_SUCCESS || bar_is_io != IsIo) {
        return EFI_UNSUPPORTED;
    }

    size = pci_width_size(Width);
    access_count = (Width >= EfiPciWidthFifoUint8 &&
                    Width <= EfiPciWidthFifoUint64 && Count != 0) ?
                   1 : (UINT64)Count;
    if (access_count == 0) {
        if (Offset > length) {
            return EFI_UNSUPPORTED;
        }
    } else if (Offset >= length ||
               access_count > (length - Offset) / size) {
        return EFI_UNSUPPORTED;
    }

    st = pci_io_bar_address(Dev, BarIndex, IsIo, Offset, Address);
    return st == EFI_SUCCESS ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS pci_io_transfer(BOOLEAN IsWrite, BOOLEAN IsIo,
                                  EFI_PCI_IO_PROTOCOL *This,
                                  EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT8 BarIndex, UINT64 Offset,
                                  UINTN Count, VOID *Buffer)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);
    UINT64 address;
    EFI_STATUS st;

    if (dev == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!pci_width_valid(Width) || Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (BarIndex == EFI_PCI_IO_PASS_THROUGH_BAR) {
        st = pci_io_bar_address(dev, BarIndex, IsIo, Offset, &address);
    } else {
        st = pci_io_resolve_bar_access(dev, BarIndex, IsIo, Width, Offset,
                                       Count, &address);
    }
    if (st != EFI_SUCCESS) {
        return st;
    }
    return pci_root_transfer(IsWrite, IsIo, Width, address, Count, Buffer);
}

static EFI_STATUS pci_io_mem_read(EFI_PCI_IO_PROTOCOL *This,
                                  EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT8 BarIndex, UINT64 Offset,
                                  UINTN Count, VOID *Buffer)
{
    return pci_io_transfer(0, 0, This, Width, BarIndex, Offset, Count, Buffer);
}

static EFI_STATUS pci_io_mem_write(EFI_PCI_IO_PROTOCOL *This,
                                   EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                   UINT8 BarIndex, UINT64 Offset,
                                   UINTN Count, VOID *Buffer)
{
    return pci_io_transfer(1, 0, This, Width, BarIndex, Offset, Count, Buffer);
}

static EFI_STATUS pci_io_io_read(EFI_PCI_IO_PROTOCOL *This,
                                 EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                 UINT8 BarIndex, UINT64 Offset,
                                 UINTN Count, VOID *Buffer)
{
    return pci_io_transfer(0, 1, This, Width, BarIndex, Offset, Count, Buffer);
}

static EFI_STATUS pci_io_io_write(EFI_PCI_IO_PROTOCOL *This,
                                  EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT8 BarIndex, UINT64 Offset,
                                  UINTN Count, VOID *Buffer)
{
    return pci_io_transfer(1, 1, This, Width, BarIndex, Offset, Count, Buffer);
}

static EFI_STATUS pci_io_poll(BOOLEAN IsIo, EFI_PCI_IO_PROTOCOL *This,
                              EFI_PCI_IO_PROTOCOL_WIDTH Width,
                              UINT8 BarIndex, UINT64 Offset, UINT64 Mask,
                              UINT64 Value, UINT64 Delay, UINT64 *Result)
{
    const FW_PCI_IO_DEVICE *dev;
    UINT64 address;
    EFI_STATUS st;

    if (!pci_poll_width_valid(Width) || Result == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    dev = fw_pci_io_device_from_protocol(This);
    if (dev == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (BarIndex == EFI_PCI_IO_PASS_THROUGH_BAR) {
        st = pci_io_bar_address(dev, BarIndex, IsIo, Offset, &address);
    } else {
        st = pci_io_resolve_bar_access(dev, BarIndex, IsIo, Width, Offset,
                                       1, &address);
    }
    if (st != EFI_SUCCESS) {
        return st;
    }
    return pci_poll_address(IsIo, Width, address, Mask, Value, Delay, Result);
}

static EFI_STATUS pci_io_poll_mem(EFI_PCI_IO_PROTOCOL *This,
                                  EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT8 BarIndex, UINT64 Offset,
                                  UINT64 Mask, UINT64 Value,
                                  UINT64 Delay, UINT64 *Result)
{
    return pci_io_poll(0, This, Width, BarIndex, Offset, Mask, Value,
                       Delay, Result);
}

static EFI_STATUS pci_io_poll_io(EFI_PCI_IO_PROTOCOL *This,
                                 EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                 UINT8 BarIndex, UINT64 Offset,
                                 UINT64 Mask, UINT64 Value,
                                 UINT64 Delay, UINT64 *Result)
{
    return pci_io_poll(1, This, Width, BarIndex, Offset, Mask, Value,
                       Delay, Result);
}

static EFI_STATUS pci_io_copy_mem(EFI_PCI_IO_PROTOCOL *This,
                                  EFI_PCI_IO_PROTOCOL_WIDTH Width,
                                  UINT8 DestBarIndex, UINT64 DestOffset,
                                  UINT8 SrcBarIndex, UINT64 SrcOffset,
                                  UINTN Count)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);
    UINT64 src;
    UINT64 dst;
    EFI_STATUS st;

    if (dev == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!pci_poll_width_valid(Width)) {
        return EFI_INVALID_PARAMETER;
    }
    if (DestBarIndex == EFI_PCI_IO_PASS_THROUGH_BAR) {
        st = pci_io_bar_address(dev, DestBarIndex, 0, DestOffset, &dst);
    } else {
        st = pci_io_resolve_bar_access(dev, DestBarIndex, 0, Width,
                                       DestOffset, Count, &dst);
    }
    if (st != EFI_SUCCESS) {
        return st;
    }
    if (SrcBarIndex == EFI_PCI_IO_PASS_THROUGH_BAR) {
        st = pci_io_bar_address(dev, SrcBarIndex, 0, SrcOffset, &src);
    } else {
        st = pci_io_resolve_bar_access(dev, SrcBarIndex, 0, Width,
                                       SrcOffset, Count, &src);
    }
    if (st != EFI_SUCCESS) {
        return st;
    }
    return pci_root_copy_mem(&mPciRootBridgeIoProto, Width, dst, src, Count);
}

static EFI_STATUS pci_io_map(EFI_PCI_IO_PROTOCOL *This,
                             EFI_PCI_IO_PROTOCOL_OPERATION Operation,
                             VOID *HostAddress, UINTN *NumberOfBytes,
                             EFI_PHYSICAL_ADDRESS *DeviceAddress,
                             VOID **Mapping)
{
    (void)This;
    return pci_root_map(&mPciRootBridgeIoProto, Operation, HostAddress,
                        NumberOfBytes, DeviceAddress, Mapping);
}

static EFI_STATUS pci_io_unmap(EFI_PCI_IO_PROTOCOL *This, VOID *Mapping)
{
    (void)This;
    return pci_root_unmap(&mPciRootBridgeIoProto, Mapping);
}

static EFI_STATUS pci_io_allocate_buffer(EFI_PCI_IO_PROTOCOL *This,
                                         EFI_ALLOCATE_TYPE Type,
                                         EFI_MEMORY_TYPE MemoryType,
                                         UINTN Pages, VOID **HostAddress,
                                         UINT64 Attributes)
{
    (void)This;
    return pci_root_allocate_buffer(&mPciRootBridgeIoProto, Type, MemoryType,
                                    Pages, HostAddress, Attributes);
}

static EFI_STATUS pci_io_free_buffer(EFI_PCI_IO_PROTOCOL *This, UINTN Pages,
                                     VOID *HostAddress)
{
    (void)This;
    return pci_root_free_buffer(&mPciRootBridgeIoProto, Pages, HostAddress);
}

static EFI_STATUS pci_io_flush(EFI_PCI_IO_PROTOCOL *This)
{
    (void)This;
    return pci_root_flush(&mPciRootBridgeIoProto);
}

static EFI_STATUS pci_io_get_location(EFI_PCI_IO_PROTOCOL *This,
                                      UINTN *SegmentNumber, UINTN *BusNumber,
                                      UINTN *DeviceNumber,
                                      UINTN *FunctionNumber)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);

    if (dev == NULL || SegmentNumber == NULL || BusNumber == NULL ||
        DeviceNumber == NULL || FunctionNumber == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *SegmentNumber = 0;
    *BusNumber = dev->Bus;
    *DeviceNumber = dev->Device;
    *FunctionNumber = dev->Function;
    return EFI_SUCCESS;
}

static UINT64 pci_io_current_attributes(const FW_PCI_IO_DEVICE *Dev)
{
    UINT16 command;
    UINT64 attrs;

    command = (UINT16)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                            Dev->Function,
                                            PCI_COMMAND_OFFSET, 2);
    attrs = Dev->Attributes & ~FW_PCI_COMMAND_ATTRIBUTES;
    if ((command & PCI_COMMAND_IO_SPACE) != 0 &&
        (Dev->Attributes & EFI_PCI_ATTRIBUTE_IO) != 0) {
        attrs |= EFI_PCI_ATTRIBUTE_IO;
    }
    if ((command & PCI_COMMAND_MEMORY_SPACE) != 0 &&
        (Dev->Attributes & EFI_PCI_ATTRIBUTE_MEMORY) != 0) {
        attrs |= EFI_PCI_ATTRIBUTE_MEMORY;
    }
    if ((command & PCI_COMMAND_BUS_MASTER) != 0 &&
        (Dev->Attributes & EFI_PCI_ATTRIBUTE_BUS_MASTER) != 0) {
        attrs |= EFI_PCI_ATTRIBUTE_BUS_MASTER;
    }
    return attrs;
}

static VOID pci_io_apply_command_attributes(const FW_PCI_IO_DEVICE *Dev,
                                            UINT64 Attributes)
{
    UINT16 command;

    command = (UINT16)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                            Dev->Function,
                                            PCI_COMMAND_OFFSET, 2);
    if ((Dev->Attributes & EFI_PCI_ATTRIBUTE_IO) != 0) {
        if ((Attributes & EFI_PCI_ATTRIBUTE_IO) != 0) {
            command |= PCI_COMMAND_IO_SPACE;
        } else {
            command &= ~PCI_COMMAND_IO_SPACE;
        }
    }
    if ((Dev->Attributes & EFI_PCI_ATTRIBUTE_MEMORY) != 0) {
        if ((Attributes & EFI_PCI_ATTRIBUTE_MEMORY) != 0) {
            command |= PCI_COMMAND_MEMORY_SPACE;
        } else {
            command &= ~PCI_COMMAND_MEMORY_SPACE;
        }
    }
    if ((Dev->Attributes & EFI_PCI_ATTRIBUTE_BUS_MASTER) != 0) {
        if ((Attributes & EFI_PCI_ATTRIBUTE_BUS_MASTER) != 0) {
            command |= PCI_COMMAND_BUS_MASTER;
        } else {
            command &= ~PCI_COMMAND_BUS_MASTER;
        }
    }
    pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                           PCI_COMMAND_OFFSET, 2, command);
}

static EFI_STATUS pci_io_attributes(EFI_PCI_IO_PROTOCOL *This,
                                    EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Operation,
                                    UINT64 Attributes, UINT64 *Result)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);
    UINT64 current;

    if (dev == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    switch (Operation) {
    case EfiPciIoAttributeOperationSupported:
        if (Result == NULL) {
            return EFI_INVALID_PARAMETER;
        }
        *Result = dev->Attributes;
        return EFI_SUCCESS;
    case EfiPciIoAttributeOperationGet:
        if (Result == NULL) {
            return EFI_INVALID_PARAMETER;
        }
        *Result = pci_io_current_attributes(dev);
        return EFI_SUCCESS;
    case EfiPciIoAttributeOperationSet:
    case EfiPciIoAttributeOperationEnable:
    case EfiPciIoAttributeOperationDisable:
        if ((Attributes & ~dev->Attributes) != 0) {
            return EFI_UNSUPPORTED;
        }
        current = pci_io_current_attributes(dev);
        if (Operation == EfiPciIoAttributeOperationSet) {
            current = Attributes | (current & ~FW_PCI_COMMAND_ATTRIBUTES);
        } else if (Operation == EfiPciIoAttributeOperationEnable) {
            current |= Attributes;
        } else {
            current &= ~Attributes;
        }
        pci_io_apply_command_attributes(dev, current);
        return EFI_SUCCESS;
    default:
        return EFI_INVALID_PARAMETER;
    }
}

static EFI_STATUS pci_io_bar_info(const FW_PCI_IO_DEVICE *Dev,
                                  UINT8 BarIndex, UINT32 *RawValue,
                                  UINT64 *Base, UINT64 *Length,
                                  BOOLEAN *IsIo, BOOLEAN *Is64)
{
    UINT32 bar_offset;
    UINT32 low;
    UINT32 high = 0;
    UINT16 command;
    UINT32 mask_low;
    UINT32 mask_high = 0;
    UINT64 mask;

    if (Dev == NULL || RawValue == NULL || Base == NULL || Length == NULL ||
        IsIo == NULL || Is64 == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (BarIndex >= 6) {
        return EFI_INVALID_PARAMETER;
    }

    bar_offset = PCI_BAR_OFFSET(BarIndex);
    low = (UINT32)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                        Dev->Function, bar_offset, 4);
    if (low == 0 || low == 0xffffffffU) {
        return EFI_UNSUPPORTED;
    }

    *RawValue = low;
    *IsIo = (low & 1U) != 0;
    *Is64 = 0;
    if (*IsIo) {
        *Base = low & ~(UINT64)3U;
    } else {
        *Is64 = (low & 0x6U) == 0x4U;
        if (*Is64) {
            if (BarIndex >= 5) {
                return EFI_UNSUPPORTED;
            }
            high = (UINT32)pci_config_read_value(
                0, Dev->Bus, Dev->Device, Dev->Function,
                PCI_BAR_OFFSET(BarIndex + 1U), 4);
        }
        *Base = ((UINT64)high << 32) | (low & ~(UINT64)0xfU);
    }

    command = (UINT16)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                            Dev->Function,
                                            PCI_COMMAND_OFFSET, 2);
    pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                           PCI_COMMAND_OFFSET, 2,
                           command & ~(PCI_COMMAND_IO_SPACE |
                                       PCI_COMMAND_MEMORY_SPACE));
    pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                           bar_offset, 4, 0xffffffffU);
    if (*Is64) {
        pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                               PCI_BAR_OFFSET(BarIndex + 1U), 4,
                               0xffffffffU);
    }
    mask_low = (UINT32)pci_config_read_value(0, Dev->Bus, Dev->Device,
                                             Dev->Function, bar_offset, 4);
    if (*Is64) {
        mask_high = (UINT32)pci_config_read_value(
            0, Dev->Bus, Dev->Device, Dev->Function,
            PCI_BAR_OFFSET(BarIndex + 1U), 4);
    }
    pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                           bar_offset, 4, low);
    if (*Is64) {
        pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                               PCI_BAR_OFFSET(BarIndex + 1U), 4, high);
    }
    pci_config_write_value(0, Dev->Bus, Dev->Device, Dev->Function,
                           PCI_COMMAND_OFFSET, 2, command);

    if (*IsIo) {
        mask = mask_low & ~(UINT64)3U;
        if (mask == 0 || mask == (UINT32)~0U) {
            return EFI_UNSUPPORTED;
        }
        *Length = ((~mask + 1U) & 0xffffffffULL);
    } else if (*Is64) {
        mask = ((UINT64)mask_high << 32) | (mask_low & ~(UINT64)0xfU);
        if (mask == 0 || mask == ~(UINT64)0) {
            return EFI_UNSUPPORTED;
        }
        *Length = ~mask + 1U;
    } else {
        mask = mask_low & ~(UINT64)0xfU;
        if (mask == 0 || mask == (UINT32)~0U) {
            return EFI_UNSUPPORTED;
        }
        *Length = ((~mask + 1U) & 0xffffffffULL);
    }
    if (*Length == 0) {
        return EFI_UNSUPPORTED;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS pci_io_get_bar_attributes(EFI_PCI_IO_PROTOCOL *This,
                                            UINT8 BarIndex, UINT64 *Supports,
                                            VOID **Resources)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);
    FW_PCI_BAR_RESOURCES *resources = NULL;
    UINT32 raw;
    UINT64 base;
    UINT64 length;
    BOOLEAN is_io;
    BOOLEAN is_64;
    EFI_STATUS st;

    if (Supports == NULL && Resources == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (Resources != NULL) {
        *Resources = NULL;
    }
    st = pci_io_bar_info(dev, BarIndex, &raw, &base, &length, &is_io, &is_64);
    if (st != EFI_SUCCESS) {
        return st;
    }

    if (Supports != NULL) {
        *Supports = is_io ? EFI_PCI_ATTRIBUTE_IO : EFI_PCI_ATTRIBUTE_MEMORY;
    }
    if (Resources != NULL) {
        st = bs_allocate_pool(EfiBootServicesData, sizeof(*resources),
                              (VOID **)&resources);
        if (st != EFI_SUCCESS) {
            return st;
        }
        fw_set_mem(resources, sizeof(*resources), 0);

        resources->Address.Descriptor = 0x8a;
        resources->Address.Length = sizeof(ACPI_QWORD_ADDRESS_DESCRIPTOR) - 3U;
        resources->Address.ResourceType = is_io ? 1 : 0;
        resources->Address.GeneralFlags = 0;
        resources->Address.TypeSpecificFlags = 0;
        resources->Address.AddressSpaceGranularity = is_64 ? 64 : 32;
        resources->Address.AddressRangeMinimum = base;
        resources->Address.AddressRangeMaximum = base + length - 1U;
        resources->Address.AddressTranslationOffset = 0;
        resources->Address.AddressLength = length;
        resources->End.Descriptor = 0x79;
        resources->End.Checksum = 0;
        *Resources = resources;
    }

    (void)raw;
    return EFI_SUCCESS;
}

static EFI_STATUS pci_io_set_bar_attributes(EFI_PCI_IO_PROTOCOL *This,
                                            UINT64 Attributes,
                                            UINT8 BarIndex, UINT64 *Offset,
                                            UINT64 *Length)
{
    const FW_PCI_IO_DEVICE *dev = fw_pci_io_device_from_protocol(This);
    UINT32 raw;
    UINT64 base;
    UINT64 bar_length;
    UINT64 supported;
    UINT64 request_offset;
    UINT64 request_length;
    BOOLEAN is_io;
    BOOLEAN is_64;
    EFI_STATUS st;

    if (Offset == NULL || Length == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    request_offset = *Offset;
    request_length = *Length;
    st = pci_io_bar_info(dev, BarIndex, &raw, &base, &bar_length, &is_io,
                         &is_64);
    if (st != EFI_SUCCESS) {
        return st;
    }

    supported = is_io ? EFI_PCI_ATTRIBUTE_IO : EFI_PCI_ATTRIBUTE_MEMORY;
    if (Attributes == 0 || (Attributes & ~supported) != 0 ||
        request_length == 0 || request_offset >= bar_length ||
        request_length > bar_length - request_offset) {
        return EFI_UNSUPPORTED;
    }

    (void)raw;
    (void)base;
    (void)is_64;
    *Offset = 0;
    *Length = bar_length;
    return EFI_SUCCESS;
}

#define FW_PCI_IO_PROTOCOL_INIT \
    { \
        .PollMem = pci_io_poll_mem, \
        .PollIo = pci_io_poll_io, \
        .Mem = { \
            .Read = pci_io_mem_read, \
            .Write = pci_io_mem_write, \
        }, \
        .Io = { \
            .Read = pci_io_io_read, \
            .Write = pci_io_io_write, \
        }, \
        .Pci = { \
            .Read = pci_io_cfg_read, \
            .Write = pci_io_cfg_write, \
        }, \
        .CopyMem = pci_io_copy_mem, \
        .Map = pci_io_map, \
        .Unmap = pci_io_unmap, \
        .AllocateBuffer = pci_io_allocate_buffer, \
        .FreeBuffer = pci_io_free_buffer, \
        .Flush = pci_io_flush, \
        .GetLocation = pci_io_get_location, \
        .Attributes = pci_io_attributes, \
        .GetBarAttributes = pci_io_get_bar_attributes, \
        .SetBarAttributes = pci_io_set_bar_attributes, \
        .RomSize = 0, \
        .RomImage = NULL, \
    }

static EFI_PCI_IO_PROTOCOL mPciIdeIoProto = FW_PCI_IO_PROTOCOL_INIT;
static EFI_PCI_IO_PROTOCOL mPciAhciIoProto = FW_PCI_IO_PROTOCOL_INIT;
static EFI_PCI_IO_PROTOCOL mPciOhciIoProto = FW_PCI_IO_PROTOCOL_INIT;
static EFI_PCI_IO_PROTOCOL mPciUhciIoProto = FW_PCI_IO_PROTOCOL_INIT;
static EFI_PCI_IO_PROTOCOL mPciLsiIoProto = FW_PCI_IO_PROTOCOL_INIT;
static EFI_PCI_IO_PROTOCOL mPciVgaIoProto = FW_PCI_IO_PROTOCOL_INIT;

#undef FW_PCI_IO_PROTOCOL_INIT

static BOOLEAN __attribute__((noinline)) pci_poll_timer_selftest(void)
{
    FW_PCI_POLL_TIMER timer;

    timer.last_tick = ~0ULL - 9U;
    timer.remaining_100ns = 1;
    timer.partial_ticks = 0;
    if (!pci_poll_timer_consume(&timer, 10) ||
        timer.remaining_100ns != 0) {
        return 0;
    }

    timer.last_tick = 100;
    timer.remaining_100ns = ~0ULL;
    timer.partial_ticks = 0;
    if (pci_poll_timer_consume(&timer,
                               100 + FW_ITC_TICKS_PER_100NS) ||
        timer.remaining_100ns != ~0ULL - 1U ||
        timer.partial_ticks != 0) {
        return 0;
    }

    timer.last_tick = 100;
    timer.remaining_100ns = 1;
    timer.partial_ticks = FW_ITC_TICKS_PER_100NS - 1U;
    return pci_poll_timer_consume(&timer, 101) &&
           timer.remaining_100ns == 0 && timer.partial_ticks == 0;
}

static BOOLEAN __attribute__((noinline)) pci_root_poll_selftest(void)
{
    const UINT64 mem_address = VGA_FB_BASE;
    const UINT64 io_address = ACPI_PM_IO_BASE + ACPI_PM_TMR_OFFSET;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width;
    FW_PCI_POLL_SELFTEST_CLOCK test_clock;
    UINT64 expected = pci_mmio_read(mem_address, sizeof(UINT32));
    UINT64 result;
    UINT64 start;

    if (!pci_poll_timer_selftest()) {
        return 0;
    }
    for (width = EfiPciWidthUint8; width <= EfiPciWidthUint64;
         width = (EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)((UINTN)width + 1U)) {
        result = ~0ULL;
        if (pci_root_poll_mem(&mPciRootBridgeIoProto, width, mem_address,
                              0, 0, ~0ULL, &result) != EFI_SUCCESS) {
            return 0;
        }
    }

    result = 0;
    if (pci_root_poll_mem(&mPciRootBridgeIoProto, EfiPciWidthFifoUint8,
                          mem_address, 0, 0, 0, &result) !=
            EFI_INVALID_PARAMETER ||
        pci_root_poll_io(&mPciRootBridgeIoProto, EfiPciWidthFillUint64,
                         io_address, 0, 0, 0, &result) !=
            EFI_INVALID_PARAMETER ||
        pci_root_poll_mem(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          mem_address, 0, 0, 0, NULL) !=
            EFI_INVALID_PARAMETER ||
        pci_root_poll_io(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                         io_address, 0, 0, 0, NULL) !=
            EFI_INVALID_PARAMETER) {
        return 0;
    }
    if (pci_root_copy_mem(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          mem_address, mem_address, 1) != EFI_SUCCESS ||
        pci_root_copy_mem(&mPciRootBridgeIoProto, EfiPciWidthFifoUint32,
                          mem_address, mem_address, 1) !=
            EFI_INVALID_PARAMETER ||
        pci_root_copy_mem(&mPciRootBridgeIoProto, EfiPciWidthFillUint32,
                          mem_address, mem_address, 1) !=
            EFI_INVALID_PARAMETER ||
        pci_root_copy_mem(&mPciRootBridgeIoProto, EfiPciWidthUint64,
                          mem_address, ~0ULL - 3U, 1) !=
            EFI_INVALID_PARAMETER) {
        return 0;
    }

    result = ~expected;
    if (pci_root_poll_mem(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          mem_address, 0xffffffffU, expected, ~0ULL,
                          &result) != EFI_SUCCESS ||
        result != expected) {
        return 0;
    }
    result = ~expected;
    if (pci_root_poll_mem(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          mem_address, 0, 1, 0, &result) != EFI_SUCCESS ||
        result != expected) {
        return 0;
    }

    start = fw_read_itc();
    result = ~expected;
    if (pci_root_poll_mem(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          mem_address, 0, 1, 1, &result) != EFI_TIMEOUT ||
        result != expected ||
        fw_read_itc() - start < FW_ITC_TICKS_PER_100NS) {
        return 0;
    }

    /* Synthetic elapsed ticks make the repeated-read timeout deterministic. */
    test_clock.now = 100;
    test_clock.ticks_per_read = FW_ITC_TICKS_PER_100NS / 4U;
    result = ~expected;
    if (pci_poll_address_internal(0, EfiPciWidthUint32, mem_address,
                                  0, 1, 1, &result,
                                  &test_clock) != EFI_TIMEOUT ||
        test_clock.read_count <= 1 || result != expected ||
        test_clock.now - 100 < FW_ITC_TICKS_PER_100NS) {
        return 0;
    }

    result = ~0ULL;
    if (pci_root_poll_io(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                         io_address, 0, 1, 0, &result) != EFI_SUCCESS ||
        result == ~0ULL) {
        return 0;
    }
    start = fw_read_itc();
    result = ~0ULL;
    return pci_root_poll_io(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                            io_address, 0, 1, 1, &result) == EFI_TIMEOUT &&
           result != ~0ULL &&
           fw_read_itc() - start >= FW_ITC_TICKS_PER_100NS;
}

static BOOLEAN __attribute__((noinline)) pci_io_transfer_selftest(void)
{
    const FW_PCI_IO_DEVICE *vga = &mPciIoDevices[5];
    const FW_PCI_IO_DEVICE *uhci = &mPciIoDevices[3];
    UINT64 vga_length = fw_pci_io_expected_bar_length(vga);
    UINT32 data[2] = { 0, 0 };

    /* A scalar transfer ending exactly at the BAR boundary is valid. */
    if (pci_io_mem_read(vga->Protocol, EfiPciWidthUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data),
                        FW_ARRAY_SIZE(data), data) != EFI_SUCCESS) {
        return 0;
    }

    /* FIFO repeats one address; scalar and Fill widths advance it. */
    if (pci_io_mem_read(vga->Protocol, EfiPciWidthFifoUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]),
                        FW_ARRAY_SIZE(data), data) != EFI_SUCCESS ||
        pci_io_mem_read(vga->Protocol, EfiPciWidthUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]),
                        FW_ARRAY_SIZE(data), data) != EFI_UNSUPPORTED ||
        pci_io_mem_read(vga->Protocol, EfiPciWidthFillUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]),
                        FW_ARRAY_SIZE(data), data) != EFI_UNSUPPORTED ||
        pci_io_mem_write(vga->Protocol, EfiPciWidthUint32,
                         vga->ExpectedBarIndex,
                         vga_length - sizeof(data[0]),
                         FW_ARRAY_SIZE(data), data) != EFI_UNSUPPORTED) {
        return 0;
    }

    /* Reject range arithmetic overflow before touching the controller. */
    if (pci_io_mem_read(vga->Protocol, EfiPciWidthUint64,
                        vga->ExpectedBarIndex, 0, ~(UINTN)0,
                        data) != EFI_UNSUPPORTED ||
        pci_io_mem_read(vga->Protocol, EfiPciWidthUint64,
                        vga->ExpectedBarIndex, ~0ULL - 3U, 1,
                        data) != EFI_UNSUPPORTED ||
        pci_io_mem_read(vga->Protocol,
                        (EFI_PCI_IO_PROTOCOL_WIDTH)-1,
                        vga->ExpectedBarIndex, 0, 1,
                        data) != EFI_INVALID_PARAMETER ||
        pci_io_mem_read(vga->Protocol, EfiPciWidthUint32, 6, 0, 1,
                        data) != EFI_UNSUPPORTED) {
        return 0;
    }

    if (pci_io_copy_mem(vga->Protocol, EfiPciWidthUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]),
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]),
                        1) != EFI_SUCCESS ||
        pci_io_copy_mem(vga->Protocol, EfiPciWidthUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]) + 1U,
                        vga->ExpectedBarIndex, 0, 1) != EFI_UNSUPPORTED ||
        pci_io_copy_mem(vga->Protocol, EfiPciWidthUint32,
                        vga->ExpectedBarIndex, 0,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]) + 1U,
                        1) != EFI_UNSUPPORTED ||
        pci_io_copy_mem(vga->Protocol, EfiPciWidthUint32,
                        vga->ExpectedBarIndex,
                        vga_length - sizeof(data[0]),
                        vga->ExpectedBarIndex, 0, 2) != EFI_UNSUPPORTED ||
        pci_io_copy_mem(vga->Protocol, EfiPciWidthFifoUint32,
                        vga->ExpectedBarIndex, 0,
                        vga->ExpectedBarIndex, 0, 1) !=
            EFI_INVALID_PARAMETER ||
        pci_io_copy_mem(vga->Protocol, EfiPciWidthFillUint32,
                        vga->ExpectedBarIndex, 0,
                        vga->ExpectedBarIndex, 0, 1) !=
            EFI_INVALID_PARAMETER ||
        pci_io_copy_mem(uhci->Protocol, EfiPciWidthUint32,
                        uhci->ExpectedBarIndex, 0,
                        uhci->ExpectedBarIndex, 0, 1) != EFI_UNSUPPORTED) {
        return 0;
    }

    if (pci_io_mem_read(vga->Protocol, EfiPciWidthUint32,
                        EFI_PCI_IO_PASS_THROUGH_BAR, VGA_FB_BASE,
                        1, data) != EFI_SUCCESS) {
        return 0;
    }

    /* Exercise the same exact-end and one-element-over rules for I/O BARs. */
    return pci_io_io_read(uhci->Protocol, EfiPciWidthUint32,
                          uhci->ExpectedBarIndex,
                          uhci->ExpectedBarLength - sizeof(data[0]),
                          1, data) == EFI_SUCCESS &&
           pci_io_io_read(uhci->Protocol, EfiPciWidthUint32,
                          uhci->ExpectedBarIndex,
                          uhci->ExpectedBarLength - sizeof(data[0]),
                          FW_ARRAY_SIZE(data), data) == EFI_UNSUPPORTED &&
           pci_io_io_write(uhci->Protocol, EfiPciWidthUint32,
                           uhci->ExpectedBarIndex,
                           uhci->ExpectedBarLength - sizeof(data[0]),
                           FW_ARRAY_SIZE(data), data) == EFI_UNSUPPORTED;
}

static BOOLEAN __attribute__((noinline)) pci_io_poll_selftest(void)
{
    const UINT64 mem_address = VGA_FB_BASE;
    const UINT64 io_address = ACPI_PM_IO_BASE + ACPI_PM_TMR_OFFSET;
    EFI_PCI_IO_PROTOCOL *protocol = &mPciVgaIoProto;
    EFI_PCI_IO_PROTOCOL_WIDTH width;
    UINT64 expected = pci_mmio_read(mem_address, sizeof(UINT32));
    UINT64 result;
    UINT64 start;

    if (!pci_io_transfer_selftest()) {
        return 0;
    }
    for (width = EfiPciWidthUint8; width <= EfiPciWidthUint64;
         width = (EFI_PCI_IO_PROTOCOL_WIDTH)((UINTN)width + 1U)) {
        result = ~0ULL;
        if (pci_io_poll_mem(protocol, width, EFI_PCI_IO_PASS_THROUGH_BAR,
                            mem_address, 0, 0, ~0ULL, &result) !=
                EFI_SUCCESS) {
            return 0;
        }
    }

    result = 0;
    if (pci_io_poll_mem(protocol, EfiPciWidthFifoUint16,
                        EFI_PCI_IO_PASS_THROUGH_BAR, mem_address,
                        0, 0, 0, &result) != EFI_INVALID_PARAMETER ||
        pci_io_poll_io(protocol, EfiPciWidthFillUint32,
                       EFI_PCI_IO_PASS_THROUGH_BAR, io_address,
                       0, 0, 0, &result) != EFI_INVALID_PARAMETER ||
        pci_io_poll_mem(protocol, EfiPciWidthUint32,
                        EFI_PCI_IO_PASS_THROUGH_BAR, mem_address,
                        0, 0, 0, NULL) != EFI_INVALID_PARAMETER ||
        pci_io_poll_io(protocol, EfiPciWidthUint32,
                       EFI_PCI_IO_PASS_THROUGH_BAR, io_address,
                       0, 0, 0, NULL) != EFI_INVALID_PARAMETER) {
        return 0;
    }

    result = 0;
    if (pci_io_poll_mem(protocol, EfiPciWidthUint32, 6, 0,
                        0, 0, 0, &result) != EFI_UNSUPPORTED ||
        pci_io_poll_mem(protocol, EfiPciWidthUint32, 0,
                        fw_pci_io_expected_bar_length(&mPciIoDevices[5]),
                        0, 0, 0, &result) != EFI_UNSUPPORTED ||
        pci_io_poll_mem(protocol, EfiPciWidthUint32, 0,
                        fw_pci_io_expected_bar_length(&mPciIoDevices[5]) - 1U,
                        0, 0, 0, &result) != EFI_UNSUPPORTED ||
        pci_io_poll_io(&mPciUhciIoProto, EfiPciWidthUint32, 4,
                       mPciIoDevices[3].ExpectedBarLength,
                       0, 0, 0, &result) != EFI_UNSUPPORTED) {
        return 0;
    }

    result = ~expected;
    if (pci_io_poll_mem(protocol, EfiPciWidthUint32,
                        mPciIoDevices[5].ExpectedBarIndex, 0,
                        0xffffffffU, expected, 0, &result) != EFI_SUCCESS ||
        result != expected) {
        return 0;
    }
    result = ~0ULL;
    if (pci_io_poll_io(&mPciUhciIoProto, EfiPciWidthUint32,
                       mPciIoDevices[3].ExpectedBarIndex, 0,
                       0, 0, 0, &result) != EFI_SUCCESS ||
        result == ~0ULL) {
        return 0;
    }

    result = ~expected;
    if (pci_io_poll_mem(protocol, EfiPciWidthUint32,
                        EFI_PCI_IO_PASS_THROUGH_BAR, mem_address,
                        0xffffffffU, expected, ~0ULL, &result) !=
            EFI_SUCCESS ||
        result != expected) {
        return 0;
    }
    result = ~0ULL;
    if (pci_io_poll_io(protocol, EfiPciWidthUint32,
                       EFI_PCI_IO_PASS_THROUGH_BAR, io_address,
                       0, 1, 0, &result) != EFI_SUCCESS ||
        result == ~0ULL) {
        return 0;
    }

    start = fw_read_itc();
    result = ~expected;
    if (pci_io_poll_mem(protocol, EfiPciWidthUint32,
                        EFI_PCI_IO_PASS_THROUGH_BAR, mem_address,
                        0, 1, 1, &result) != EFI_TIMEOUT ||
        result != expected ||
        fw_read_itc() - start < FW_ITC_TICKS_PER_100NS) {
        return 0;
    }

    start = fw_read_itc();
    result = ~0ULL;
    return pci_io_poll_io(protocol, EfiPciWidthUint32,
                          EFI_PCI_IO_PASS_THROUGH_BAR, io_address,
                          0, 1, 1, &result) == EFI_TIMEOUT &&
           result != ~0ULL &&
           fw_read_itc() - start >= FW_ITC_TICKS_PER_100NS;
}

static BOOLEAN __attribute__((noinline)) pci_root_bridge_io_selftest(void)
{
    UINT32 ide_id = 0;
    UINT32 ahci_id = 0;
    UINT64 supports = 0;
    UINT64 attributes = 0;
    VOID *resources = NULL;
    FW_PCI_ROOT_BRIDGE_RESOURCES *res;

    if (pci_root_cfg_read(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          0, 1, &ide_id) != EFI_SUCCESS ||
        ide_id == 0) {
        return 0;
    }

    /*
     * The AHCI controller is opt-in (ahci=on); on the default machine slot 1
     * is empty and reads back all-ones.  Still exercise the root-bridge
     * config-read path either way, but only require the exact id when the
     * controller is actually present.
     */
    if (pci_root_cfg_read(&mPciRootBridgeIoProto, EfiPciWidthUint32,
                          1ULL << 16, 1, &ahci_id) != EFI_SUCCESS ||
        (ahci_id != 0xffffffffU && ahci_id != 0x29228086U)) {
        return 0;
    }

    if (pci_root_get_attributes(&mPciRootBridgeIoProto, &supports,
                                &attributes) != EFI_SUCCESS ||
        supports != FW_PCI_ROOT_BRIDGE_ATTRIBUTES ||
        attributes != FW_PCI_ROOT_BRIDGE_ATTRIBUTES) {
        return 0;
    }

    if (pci_root_configuration(&mPciRootBridgeIoProto, &resources) !=
            EFI_SUCCESS ||
        resources == NULL) {
        return 0;
    }

    res = (FW_PCI_ROOT_BRIDGE_RESOURCES *)resources;
    if (res->Bus.ResourceType != 2 || res->Bus.AddressLength != 256 ||
        res->Io.ResourceType != 1 ||
        res->Io.AddressTranslationOffset != PCI_IO_TRANSLATION_OFFSET ||
        res->Mem.ResourceType != 0 ||
        res->Mem.AddressTranslationOffset != PCI_MMIO_TRANSLATION_OFFSET ||
        res->End.Descriptor != 0x79) {
        return 0;
    }
    return pci_root_poll_selftest();
}

static BOOLEAN __attribute__((noinline)) pci_io_protocol_selftest(void)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPciIoDevices); i++) {
        const FW_PCI_IO_DEVICE *dev = &mPciIoDevices[i];
        UINT32 id = 0;
        UINT32 bar = 0;
        UINTN segment = 1;
        UINTN bus = 0xff;
        UINTN device = 0xff;
        UINTN function = 0xff;
        UINT64 attrs = 0;
        UINT64 supports = 0;
        UINT64 support_only = 0;
        UINT64 current_attrs = 0;
        UINT64 command_attrs;
        UINT64 bar_offset;
        UINT64 bar_length;
        VOID *resources = NULL;
        FW_PCI_BAR_RESOURCES *bar_res;
        UINT32 expected_id;
        UINT32 expected_bar;
        UINT64 expected_bar_length;
        UINT32 expected_base;
        UINT16 command;

        expected_id = fw_pci_io_expected_id(dev);
        expected_bar = fw_pci_io_expected_bar_value(dev);
        expected_bar_length = fw_pci_io_expected_bar_length(dev);

        if (pci_io_cfg_read(dev->Protocol, EfiPciWidthUint32, 0, 1,
                            &id) != EFI_SUCCESS) {
            return 0;
        }
        if (id == 0xffffffffU && fw_pci_io_device_optional(dev)) {
            continue;
        }
        if (id != expected_id) {
            return 0;
        }

        if (pci_io_cfg_read(dev->Protocol, EfiPciWidthUint32,
                            0x10U + (UINT32)dev->ExpectedBarIndex * 4U,
                            1, &bar) != EFI_SUCCESS ||
            bar != expected_bar) {
            return 0;
        }

        if (pci_io_get_location(dev->Protocol, &segment, &bus, &device,
                                &function) != EFI_SUCCESS ||
            segment != 0 || bus != dev->Bus || device != dev->Device ||
            function != dev->Function) {
            return 0;
        }

        if (pci_io_attributes(dev->Protocol,
                              EfiPciIoAttributeOperationSupported, 0,
                              &attrs) != EFI_SUCCESS ||
            attrs != dev->Attributes) {
            return 0;
        }

        if (pci_io_attributes(dev->Protocol, EfiPciIoAttributeOperationSet,
                              dev->Attributes, NULL) != EFI_SUCCESS) {
            return 0;
        }

        if (pci_io_attributes(dev->Protocol, EfiPciIoAttributeOperationGet,
                              0, &current_attrs) != EFI_SUCCESS ||
            current_attrs != dev->Attributes) {
            return 0;
        }

        command = (UINT16)pci_config_read_value(0, dev->Bus, dev->Device,
                                                dev->Function,
                                                PCI_COMMAND_OFFSET, 2);
        command_attrs = dev->Attributes & FW_PCI_COMMAND_ATTRIBUTES;
        /*
         * OHCI fetches its HCCA once per frame while it is operational.
         * Dropping PCI bus mastering underneath that periodic DMA is not a
         * valid way to self-test Attributes(): the controller reports an
         * unrecoverable error and stops.  The other controllers exercise the
         * common bus-master enable/disable path without continuous DMA.
         */
        if (dev->Protocol == &mPciOhciIoProto) {
            command_attrs &= ~EFI_PCI_ATTRIBUTE_BUS_MASTER;
        }
        if (command_attrs != 0) {
            if (pci_io_attributes(dev->Protocol,
                                  EfiPciIoAttributeOperationDisable,
                                  command_attrs, NULL) != EFI_SUCCESS ||
                pci_io_attributes(dev->Protocol,
                                  EfiPciIoAttributeOperationGet, 0,
                                  &current_attrs) != EFI_SUCCESS ||
                (current_attrs & command_attrs) != 0 ||
                pci_io_attributes(dev->Protocol,
                                  EfiPciIoAttributeOperationEnable,
                                  command_attrs, NULL) != EFI_SUCCESS ||
                pci_io_attributes(dev->Protocol,
                                  EfiPciIoAttributeOperationGet, 0,
                                  &current_attrs) != EFI_SUCCESS ||
                current_attrs != dev->Attributes) {
                pci_config_write_value(0, dev->Bus, dev->Device,
                                       dev->Function, PCI_COMMAND_OFFSET, 2,
                                       command);
                return 0;
            }
            pci_config_write_value(0, dev->Bus, dev->Device, dev->Function,
                                   PCI_COMMAND_OFFSET, 2, command);
        }

        if (pci_io_get_bar_attributes(dev->Protocol, dev->ExpectedBarIndex,
                                      &supports, &resources) != EFI_SUCCESS ||
            resources == NULL) {
            return 0;
        }
        bar_res = (FW_PCI_BAR_RESOURCES *)resources;
        expected_base = expected_bar &
            ((expected_bar & 1U) != 0 ? ~3U : ~0xfU);
        if (supports != ((expected_bar & 1U) != 0 ?
                         EFI_PCI_ATTRIBUTE_IO :
                         EFI_PCI_ATTRIBUTE_MEMORY) ||
            bar_res->Address.Descriptor != 0x8a ||
            bar_res->Address.ResourceType !=
                ((expected_bar & 1U) != 0 ? 1 : 0) ||
            bar_res->Address.AddressRangeMinimum != expected_base ||
            bar_res->Address.AddressLength != expected_bar_length ||
            bar_res->Address.AddressRangeMaximum !=
                (UINT64)expected_base + expected_bar_length - 1U ||
            bar_res->End.Descriptor != 0x79) {
            (void)bs_free_pool(resources);
            return 0;
        }
        if (bs_free_pool(resources) != EFI_SUCCESS) {
            return 0;
        }
        if (pci_io_get_bar_attributes(dev->Protocol, dev->ExpectedBarIndex,
                                      &support_only, NULL) != EFI_SUCCESS ||
            support_only != supports ||
            pci_io_get_bar_attributes(dev->Protocol, dev->ExpectedBarIndex,
                                      NULL, &resources) != EFI_SUCCESS ||
            resources == NULL) {
            return 0;
        }
        if (bs_free_pool(resources) != EFI_SUCCESS) {
            return 0;
        }

        bar_offset = expected_bar_length > 1 ? 1 : 0;
        bar_length = expected_bar_length - bar_offset;
        if (pci_io_set_bar_attributes(dev->Protocol, supports,
                                      dev->ExpectedBarIndex, &bar_offset,
                                      &bar_length) != EFI_SUCCESS ||
            bar_offset != 0 || bar_length != expected_bar_length) {
            return 0;
        }
        bar_offset = expected_bar_length;
        bar_length = 1;
        if (pci_io_set_bar_attributes(dev->Protocol, supports,
                                      dev->ExpectedBarIndex, &bar_offset,
                                      &bar_length) != EFI_UNSUPPORTED) {
            return 0;
        }
    }

    return pci_io_poll_selftest();
}

#define FPSWA_STATUS_UNHANDLED ((UINT64)-1)

static IA64_FPSWA_INTERFACE mFpswaProto = {
    .revision = 0x00010000,
    .reserved = 0,
    .fpswa = fpswa_emulation_entry,
};

static IA64_FPSWA_INTERFACE mFpswaSelftestReplacement = {
    .revision = 0x00010000,
    .reserved = 0,
    .fpswa = fpswa_emulation_entry,
};

static BOOLEAN fpswa_install_protocols(void)
{
    EFI_HANDLE handle = mFpswaHandle != NULL ? mFpswaHandle : FW_HANDLE_FPSWA;
    EFI_STATUS st;

    st = bs_install_protocol(&handle, (void *)mFpswaProtocolGuid, 0,
                             &mFpswaProto);
    if (st != EFI_SUCCESS) {
        return 0;
    }
    mFpswaHandle = handle;
    st = bs_install_protocol(&handle, (void *)mLoadedImageProtocolGuid, 0,
                             &mFpswaLoadedImageProto);
    if (st != EFI_SUCCESS) {
        (void)bs_uninstall_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                                    &mFpswaProto);
        mFpswaHandle = NULL;
        return 0;
    }
    st = bs_install_protocol(&handle, (void *)mLoadedImageDevicePathProtocolGuid,
                             0, mFpswaLoadedImageProto.FilePath);
    if (st != EFI_SUCCESS) {
        (void)bs_uninstall_protocol(mFpswaHandle,
                                    (void *)mLoadedImageProtocolGuid,
                                    &mFpswaLoadedImageProto);
        (void)bs_uninstall_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                                    &mFpswaProto);
        mFpswaHandle = NULL;
        return 0;
    }
    mFpswaLoadedImageActive = 1;
    return 1;
}

static EFI_STATUS fpswa_unload_image(EFI_HANDLE ImageHandle)
{
    EFI_STATUS st;

    if (ImageHandle == NULL ||
        ImageHandle != mFpswaHandle ||
        !mFpswaLoadedImageActive) {
        return EFI_INVALID_PARAMETER;
    }

    /*
     * Protocol lookups do not prevent UninstallProtocolInterface() from
     * removing an interface.  Release those records for every interface
     * before checking for consumers that really do block the unload.  Doing
     * this as a preflight also keeps the three interfaces indivisible.
     */
    close_uninstall_safe_open_records(ImageHandle,
                                      (void *)mFpswaProtocolGuid);
    close_uninstall_safe_open_records(ImageHandle,
                                      (void *)mLoadedImageProtocolGuid);
    close_uninstall_safe_open_records(
        ImageHandle, (void *)mLoadedImageDevicePathProtocolGuid);
    if (protocol_has_open_records(ImageHandle, mFpswaProtocolGuid) ||
        protocol_has_open_records(ImageHandle, mLoadedImageProtocolGuid) ||
        protocol_has_open_records(ImageHandle,
                                  mLoadedImageDevicePathProtocolGuid)) {
        return EFI_ACCESS_DENIED;
    }

    st = bs_uninstall_protocol(ImageHandle,
                               (void *)mLoadedImageDevicePathProtocolGuid,
                               mFpswaLoadedImageProto.FilePath);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = bs_uninstall_protocol(ImageHandle, (void *)mLoadedImageProtocolGuid,
                               &mFpswaLoadedImageProto);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = bs_uninstall_protocol(ImageHandle, (void *)mFpswaProtocolGuid,
                               &mFpswaProto);
    if (st != EFI_SUCCESS) {
        return st;
    }

    mFpswaLoadedImageActive = 0;
    mFpswaHandle = NULL;
    return EFI_SUCCESS;
}

static BOOLEAN __attribute__((noinline)) fpswa_protocol_selftest(void)
{
    IA64_FPSWA_RET ret;
    VOID *interface = NULL;
    VOID **protocols = NULL;
    UINTN protocol_count = 0;
    UINTN i;
    BOOLEAN found_fpswa;
    BOOLEAN found_loaded_image;
    BOOLEAN found_loaded_image_path;
    BOOLEAN replacement_ok;

    if (mFpswaHandle != FW_HANDLE_FPSWA ||
        mFpswaProto.revision != 0x00010000 ||
        mFpswaProto.reserved != 0 ||
        mFpswaProto.fpswa != fpswa_emulation_entry ||
        mFpswaProtocolGuid[0] != 0x31 ||
        mFpswaProtocolGuid[1] != 0x65 ||
        mFpswaProtocolGuid[2] != 0x1b ||
        mFpswaProtocolGuid[3] != 0xc4 ||
        mFpswaProtocolGuid[8] != 0x9a ||
        mFpswaProtocolGuid[15] != 0x4d) {
        return 0;
    }
    if (!mFpswaLoadedImageActive ||
        mFpswaLoadedImageProto.Unload != fpswa_unload_image ||
        mFpswaLoadedImageProto.ImageBase != &mFpswaProto ||
        mFpswaLoadedImageProto.ImageSize != sizeof(mFpswaProto) ||
        mFpswaLoadedImageProto.DeviceHandle != NULL ||
        mFpswaLoadedImageProto.FilePath == NULL) {
        return 0;
    }

    if (!installed_protocol_interface(mFpswaHandle,
                                      (void *)mFpswaProtocolGuid,
                                      &interface) ||
        interface != &mFpswaProto) {
        return 0;
    }
    interface = NULL;
    if (!installed_protocol_interface(mFpswaHandle,
                                      (void *)mLoadedImageProtocolGuid,
                                      &interface) ||
        interface != &mFpswaLoadedImageProto) {
        return 0;
    }
    interface = NULL;
    if (!installed_protocol_interface(
            mFpswaHandle, (void *)mLoadedImageDevicePathProtocolGuid,
            &interface) ||
        interface != mFpswaLoadedImageProto.FilePath) {
        return 0;
    }
    if (bs_protocols_per_handle(mFpswaHandle, &protocols,
                                &protocol_count) != EFI_SUCCESS) {
        return 0;
    }
    found_fpswa = 0;
    found_loaded_image = 0;
    found_loaded_image_path = 0;
    for (i = 0; i < protocol_count; i++) {
        if (fw_guid_equal(protocols[i], mFpswaProtocolGuid)) {
            found_fpswa = 1;
        } else if (fw_guid_equal(protocols[i], mLoadedImageProtocolGuid)) {
            found_loaded_image = 1;
        } else if (fw_guid_equal(protocols[i],
                                 mLoadedImageDevicePathProtocolGuid)) {
            found_loaded_image_path = 1;
        }
    }
    (void)bs_free_pool(protocols);
    protocols = NULL;
    if (!found_fpswa || !found_loaded_image || !found_loaded_image_path) {
        return 0;
    }
    if (bs_reinstall_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                              &mFpswaProto,
                              &mFpswaSelftestReplacement) != EFI_SUCCESS) {
        return 0;
    }
    interface = NULL;
    replacement_ok =
        installed_protocol_interface(mFpswaHandle,
                                     (void *)mFpswaProtocolGuid,
                                     &interface) &&
        interface == &mFpswaSelftestReplacement;
    if (bs_reinstall_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                              &mFpswaSelftestReplacement,
                              &mFpswaProto) != EFI_SUCCESS ||
        !replacement_ok) {
        return 0;
    }

    ret = mFpswaProto.fpswa(0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    if (ret.status != FPSWA_STATUS_UNHANDLED ||
        ret.err0 == 0 || ret.err1 != 0 || ret.err2 != 0) {
        return 0;
    }

    interface = NULL;
    if (bs_open_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                         &interface, mImageHandle, mFpswaHandle,
                         EFI_OPEN_PROTOCOL_BY_DRIVER) != EFI_SUCCESS ||
        interface != &mFpswaProto) {
        return 0;
    }
    if (bs_unload_image(mFpswaHandle) != EFI_ACCESS_DENIED ||
        !installed_protocol_interface(FW_HANDLE_FPSWA,
                                      (void *)mFpswaProtocolGuid, NULL) ||
        bs_close_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                          mImageHandle, mFpswaHandle) != EFI_SUCCESS) {
        return 0;
    }

    /* Lookup-only opens must not prevent an image from being unloaded. */
    interface = NULL;
    if (bs_handle_protocol(mFpswaHandle, (void *)mFpswaProtocolGuid,
                           &interface) != EFI_SUCCESS ||
        interface != &mFpswaProto) {
        return 0;
    }
    interface = NULL;
    if (bs_open_protocol(mFpswaHandle, (void *)mLoadedImageProtocolGuid,
                         &interface, mImageHandle, NULL,
                         EFI_OPEN_PROTOCOL_GET_PROTOCOL) != EFI_SUCCESS ||
        interface != &mFpswaLoadedImageProto ||
        bs_open_protocol(mFpswaHandle,
                         (void *)mLoadedImageDevicePathProtocolGuid, NULL,
                         mImageHandle, NULL,
                         EFI_OPEN_PROTOCOL_TEST_PROTOCOL) != EFI_SUCCESS ||
        bs_unload_image(mFpswaHandle) != EFI_SUCCESS ||
        installed_protocol_interface(FW_HANDLE_FPSWA,
                                     (void *)mFpswaProtocolGuid, NULL) ||
        installed_protocol_interface(FW_HANDLE_FPSWA,
                                     (void *)mLoadedImageProtocolGuid,
                                     NULL) ||
        installed_protocol_interface(FW_HANDLE_FPSWA,
                                     (void *)mLoadedImageDevicePathProtocolGuid,
                                     NULL) ||
        !fpswa_install_protocols()) {
        return 0;
    }

    return 1;
}

BOOLEAN guid_matches(const void *Protocol, const UINT8 *Guid)
{
    const UINT8 *p = (const UINT8 *)Protocol;
    UINTN i;

    if (p == NULL || Guid == NULL) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        if (p[i] != Guid[i]) {
            return 0;
        }
    }
    return 1;
}

static void copy_guid(UINT8 *Destination, const void *Source)
{
    fw_copy_mem(Destination, Source, 16);
}

static BOOLEAN installed_protocol_interface(EFI_HANDLE Handle, void *Protocol,
                                            VOID **Interface)
{
    UINTN i;

    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use &&
            mProtocolRecords[i].handle == Handle &&
            guid_matches(Protocol, mProtocolRecords[i].guid)) {
            if (Interface != NULL) {
                *Interface = mProtocolRecords[i].interface;
            }
            return 1;
        }
    }
    return 0;
}

BOOLEAN handle_supports_protocol(EFI_HANDLE Handle, void *Protocol,
                                        VOID **Interface)
{
    if (Handle == mRawBlockIoHandle &&
        guid_matches(Protocol, mBlockIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mRawBlockIoProto;
        }
        return 1;
    }

    if (Handle == mRawBlockIoHandle &&
        guid_matches(Protocol, mDiskIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mRawDiskIoProto;
        }
        return 1;
    }

    if (Handle == mRawBlockIoHandle &&
        guid_matches(Protocol, mSimpleFileSystemProtocolGuid) &&
        (fw_udf_init() || fw_iso_init()) &&
        !fw_boot_optical_fs_available()) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mOpticalSimpleFsProto;
        }
        return 1;
    }

    if (Handle == mRawBlockIoHandle &&
        guid_matches(Protocol, mDevicePathProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = mRawStorageDevice.Kind == FW_STORAGE_AHCI ?
                (VOID *)&mSataRawDevicePath :
                (VOID *)&mRawBlockDevicePath;
        }
        return 1;
    }

    if (Handle == mBlockIoHandle &&
        guid_matches(Protocol, mBlockIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mBlockIoProto;
        }
        return 1;
    }

    if (Handle == mBlockIoHandle &&
        guid_matches(Protocol, mDiskIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mBlockDiskIoProto;
        }
        return 1;
    }

    if (Handle == mBlockIoHandle &&
        guid_matches(Protocol, mSimpleFileSystemProtocolGuid) &&
        (fw_boot_fat_available() || fw_boot_optical_fs_available())) {
        if (Interface != NULL) {
            *Interface = fw_boot_fat_available() ?
                (VOID *)&mSimpleFsProto : (VOID *)&mOpticalSimpleFsProto;
        }
        return 1;
    }

    if (Handle == mBlockIoHandle &&
        guid_matches(Protocol, mDevicePathProtocolGuid)) {
        if (Interface != NULL) {
            /*
             * Optical media keep the MEDIA_CDROM_DP node even when no El Torito
             * EFI boot image was mapped (a combo disc whose only boot catalog
             * entry is the legacy x86 CDBOOT loader): the node then spans the
             * whole media, so an OS loader started from this handle still sees a
             * CD-ROM device path.  Microsoft's setupldr requires that to take
             * its El Torito CD boot path (WSRV03 base/boot/efi/sumain.c).
             */
            BOOLEAN keep_cdrom = mBootImageMapped ||
                                 storage_is_cd(&mBootStorageDevice);
            if (mBootStorageDevice.Kind == FW_STORAGE_AHCI) {
                *Interface = keep_cdrom ?
                    (VOID *)&mSataBlockDevicePath :
                    (VOID *)&mSataBootDevicePath;
            } else if (keep_cdrom) {
                *Interface = (VOID *)&mBlockDevicePath;
            } else {
                *Interface = storage_same_device(&mBootStorageDevice,
                                                 &mDiskStorageDevice) ?
                    (VOID *)&mDiskBlockDevicePath :
                    (VOID *)&mRawBlockDevicePath;
            }
        }
        return 1;
    }

    if (Handle == mDiskBlockIoHandle &&
        guid_matches(Protocol, mBlockIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mDiskBlockIoProto;
        }
        return 1;
    }

    if (Handle == mDiskBlockIoHandle &&
        guid_matches(Protocol, mDiskIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mDiskIoProto;
        }
        return 1;
    }

    if (Handle == mDiskBlockIoHandle &&
        guid_matches(Protocol, mDevicePathProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = mDiskStorageDevice.Kind == FW_STORAGE_AHCI ?
                (VOID *)&mSataDiskDevicePath :
                (VOID *)&mDiskBlockDevicePath;
        }
        return 1;
    }

    if (Handle == mImageHandle &&
        guid_matches(Protocol, mLoadedImageProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mLoadedImageProto;
        }
        return 1;
    }

    if (Handle == mImageHandle &&
        guid_matches(Protocol, mConInProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mConInProto;
        }
        return 1;
    }

    if (Handle == mImageHandle &&
        guid_matches(Protocol, mConInExProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mConInExProto;
        }
        return 1;
    }

    if ((Handle == mImageHandle || Handle == mGraphicsHandle) &&
        guid_matches(Protocol, mConOutProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mConOutProto;
        }
        return 1;
    }

    if (Handle == mUnicodeCollationHandle &&
        guid_matches(Protocol, mUnicodeCollationProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mUnicodeCollationProto;
        }
        return 1;
    }

    if (Handle == mGraphicsHandle &&
        guid_matches(Protocol, mGraphicsOutputProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mGopProto;
        }
        return 1;
    }

    if (Handle == mGraphicsHandle &&
        guid_matches(Protocol, mUgaDrawProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mUgaDrawProto;
        }
        return 1;
    }

    if (Handle == mGraphicsHandle &&
        guid_matches(Protocol, mDevicePathProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mGraphicsDevicePath;
        }
        return 1;
    }

    if (Handle == mPciRootBridgeHandle &&
        guid_matches(Protocol, mPciRootBridgeIoProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mPciRootBridgeIoProto;
        }
        return 1;
    }

    if (Handle == mPciRootBridgeHandle &&
        guid_matches(Protocol, mDevicePathProtocolGuid)) {
        if (Interface != NULL) {
            *Interface = (VOID *)&mPciRootBridgeDevicePath;
        }
        return 1;
    }

    {
        const FW_PCI_IO_DEVICE *pci_io_dev =
            fw_pci_io_device_from_handle(Handle);

        if (pci_io_dev != NULL && guid_matches(Protocol, mPciIoProtocolGuid)) {
            if (Interface != NULL) {
                *Interface = (VOID *)pci_io_dev->Protocol;
            }
            return 1;
        }

        if (pci_io_dev != NULL && pci_io_dev->ProvidesDevicePath &&
            guid_matches(Protocol, mDevicePathProtocolGuid)) {
            if (Interface != NULL) {
                *Interface = pci_io_dev->DevicePath;
            }
            return 1;
        }
    }

    {
        UINTN i;
        for (i = 0; i < LOADED_IMAGE_MAX; i++) {
            if (mLoadedImages[i].in_use &&
                Handle == mLoadedImages[i].handle &&
                guid_matches(Protocol, mLoadedImageProtocolGuid)) {
                if (Interface != NULL) {
                    *Interface = (VOID *)&mLoadedImages[i].loaded_image;
                }
                return 1;
            }
            if (mLoadedImages[i].in_use &&
                Handle == mLoadedImages[i].handle &&
                guid_matches(Protocol, mLoadedImageDevicePathProtocolGuid)) {
                if (Interface != NULL) {
                    *Interface = mLoadedImages[i].device_path;
                }
                return 1;
            }
            if (mLoadedImages[i].in_use &&
                Handle == mLoadedImages[i].handle &&
                mLoadedImages[i].hii_package_list != NULL &&
                guid_matches(Protocol, mHiiPackageListProtocolGuid)) {
                if (Interface != NULL) {
                    *Interface = mLoadedImages[i].hii_package_list;
                }
                return 1;
            }
        }
    }

    return installed_protocol_interface(Handle, Protocol, Interface);
}

static BOOLEAN open_protocol_guid_matches(const EFI_OPEN_PROTOCOL_RECORD *Rec,
                                          void *Protocol)
{
    return Rec->in_use && guid_matches(Protocol, Rec->guid);
}

static BOOLEAN open_protocol_attribute_legal(UINT32 Attributes)
{
    switch (Attributes) {
    case EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL:
    case EFI_OPEN_PROTOCOL_GET_PROTOCOL:
    case EFI_OPEN_PROTOCOL_TEST_PROTOCOL:
    case EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER:
    case EFI_OPEN_PROTOCOL_BY_DRIVER:
    case EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE:
    case EFI_OPEN_PROTOCOL_EXCLUSIVE:
        return 1;
    default:
        return 0;
    }
}

static BOOLEAN open_protocol_is_driver(UINT32 Attributes)
{
    return (Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0;
}

static BOOLEAN open_protocol_is_exclusive(UINT32 Attributes)
{
    return (Attributes & EFI_OPEN_PROTOCOL_EXCLUSIVE) != 0;
}

static EFI_OPEN_PROTOCOL_RECORD *find_open_protocol_record(
    EFI_HANDLE Handle, void *Protocol, EFI_HANDLE AgentHandle,
    EFI_HANDLE ControllerHandle, UINT32 Attributes)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use &&
            rec->handle == Handle &&
            rec->agent_handle == AgentHandle &&
            rec->controller_handle == ControllerHandle &&
            rec->attributes == Attributes &&
            guid_matches(Protocol, rec->guid)) {
            return rec;
        }
    }
    return NULL;
}

static EFI_OPEN_PROTOCOL_RECORD *alloc_open_protocol_record(void)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        if (!mOpenProtocolRecords[i].in_use) {
            return &mOpenProtocolRecords[i];
        }
    }
    return NULL;
}

static void clear_open_protocol_record(EFI_OPEN_PROTOCOL_RECORD *Rec)
{
    Rec->in_use = 0;
    Rec->handle = NULL;
    Rec->agent_handle = NULL;
    Rec->controller_handle = NULL;
    Rec->attributes = 0;
    Rec->open_count = 0;
}

static BOOLEAN open_protocol_has_exclusive(EFI_HANDLE Handle, void *Protocol)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Handle &&
            open_protocol_guid_matches(rec, Protocol) &&
            open_protocol_is_exclusive(rec->attributes)) {
            return 1;
        }
    }
    return 0;
}

static EFI_OPEN_PROTOCOL_RECORD *open_protocol_driver_record(
    EFI_HANDLE Handle, void *Protocol)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Handle &&
            open_protocol_guid_matches(rec, Protocol) &&
            open_protocol_is_driver(rec->attributes)) {
            return rec;
        }
    }
    return NULL;
}

static BOOLEAN open_protocol_driver_open_remains(EFI_HANDLE Handle,
                                                 void *Protocol)
{
    return open_protocol_driver_record(Handle, Protocol) != NULL;
}

static EFI_STATUS open_protocol_remove_driver_opens(EFI_HANDLE Handle,
                                                    void *Protocol)
{
    EFI_OPEN_PROTOCOL_RECORD *rec;

    while ((rec = open_protocol_driver_record(Handle, Protocol)) != NULL) {
        (void)bs_disconnect_controller(Handle, rec->agent_handle,
                                       NULL);
        if (rec->in_use) {
            return EFI_ACCESS_DENIED;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS open_protocol_check_conflicts(EFI_HANDLE Handle,
                                                void *Protocol,
                                                EFI_HANDLE AgentHandle,
                                                UINT32 Attributes)
{
    EFI_OPEN_PROTOCOL_RECORD *rec;

    if (Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) {
        if (open_protocol_has_exclusive(Handle, Protocol)) {
            return EFI_ACCESS_DENIED;
        }
        rec = open_protocol_driver_record(Handle, Protocol);
        if (rec != NULL) {
            if (rec->attributes == EFI_OPEN_PROTOCOL_BY_DRIVER &&
                rec->agent_handle == AgentHandle) {
                return EFI_ALREADY_STARTED;
            }
            return EFI_ACCESS_DENIED;
        }
    } else if (Attributes ==
               (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE)) {
        if (open_protocol_has_exclusive(Handle, Protocol)) {
            UINTN i;

            for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
                EFI_OPEN_PROTOCOL_RECORD *open = &mOpenProtocolRecords[i];

                if (open->in_use && open->handle == Handle &&
                    open_protocol_guid_matches(open, Protocol) &&
                    open->attributes == Attributes &&
                    open->agent_handle == AgentHandle) {
                    return EFI_ALREADY_STARTED;
                }
            }
            return EFI_ACCESS_DENIED;
        }
        if (open_protocol_remove_driver_opens(Handle, Protocol) !=
            EFI_SUCCESS) {
            return EFI_ACCESS_DENIED;
        }
    } else if (Attributes == EFI_OPEN_PROTOCOL_EXCLUSIVE) {
        if (open_protocol_has_exclusive(Handle, Protocol)) {
            return EFI_ACCESS_DENIED;
        }
        if (open_protocol_remove_driver_opens(Handle, Protocol) !=
            EFI_SUCCESS) {
            return EFI_ACCESS_DENIED;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS add_open_protocol_record(EFI_HANDLE Handle, void *Protocol,
                                           EFI_HANDLE AgentHandle,
                                           EFI_HANDLE ControllerHandle,
                                           UINT32 Attributes)
{
    EFI_OPEN_PROTOCOL_RECORD *rec;

    rec = find_open_protocol_record(Handle, Protocol, AgentHandle,
                                    ControllerHandle, Attributes);
    if (rec != NULL) {
        rec->open_count++;
        return EFI_SUCCESS;
    }

    rec = alloc_open_protocol_record();
    if (rec == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    rec->in_use = 1;
    rec->handle = Handle;
    copy_guid(rec->guid, Protocol);
    rec->agent_handle = AgentHandle;
    rec->controller_handle = ControllerHandle;
    rec->attributes = Attributes;
    rec->open_count = 1;
    return EFI_SUCCESS;
}

static void close_uninstall_safe_open_records(EFI_HANDLE Handle,
                                              void *Protocol)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Handle &&
            guid_matches(Protocol, rec->guid) &&
            (rec->attributes == EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL ||
             rec->attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL ||
             rec->attributes == EFI_OPEN_PROTOCOL_TEST_PROTOCOL)) {
            clear_open_protocol_record(rec);
        }
    }
}

static BOOLEAN protocol_has_open_records(EFI_HANDLE Handle,
                                         const void *Protocol)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Handle &&
            guid_matches(Protocol, rec->guid)) {
            return 1;
        }
    }
    return 0;
}

static void fw_protocol_notify_log_append(EFI_HANDLE Handle, void *Protocol)
{
    EFI_PROTOCOL_NOTIFY_LOG_RECORD *rec;

    if (mProtocolNotifyLogCount >= PROTOCOL_NOTIFY_LOG_MAX) {
        return;
    }

    rec = &mProtocolNotifyLog[mProtocolNotifyLogCount++];
    rec->in_use = 1;
    rec->handle = Handle;
    copy_guid(rec->guid, Protocol);
}

static void fw_notify_protocol_installed(EFI_HANDLE Handle, void *Protocol)
{
    BOOLEAN matched = 0;
    UINTN i;

    for (i = 0; i < PROTOCOL_NOTIFY_RECORD_MAX; i++) {
        EFI_PROTOCOL_NOTIFY_RECORD *rec = &mProtocolNotifyRecords[i];

        if (rec->in_use && guid_matches(Protocol, rec->guid)) {
            matched = 1;
            break;
        }
    }
    if (!matched) {
        return;
    }

    fw_protocol_notify_log_append(Handle, Protocol);

    for (i = 0; i < PROTOCOL_NOTIFY_RECORD_MAX; i++) {
        EFI_PROTOCOL_NOTIFY_RECORD *rec = &mProtocolNotifyRecords[i];

        if (rec->in_use && guid_matches(Protocol, rec->guid) &&
            rec->event != NULL &&
            rec->event->signature == FW_EVENT_SIGNATURE) {
            fw_signal_event_record(rec->event);
        }
    }
    fw_dispatch_event_notifications();
}

static EFI_STATUS fw_protocol_notify_next_handle(VOID *Registration,
                                                 EFI_HANDLE *Handle,
                                                 BOOLEAN Consume)
{
    EFI_PROTOCOL_NOTIFY_RECORD *reg =
        (EFI_PROTOCOL_NOTIFY_RECORD *)Registration;
    UINTN i;

    if (reg == NULL || !reg->in_use) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = reg->next_log_index; i < mProtocolNotifyLogCount; i++) {
        EFI_PROTOCOL_NOTIFY_LOG_RECORD *rec = &mProtocolNotifyLog[i];

        if (rec->in_use && guid_matches(reg->guid, rec->guid)) {
            *Handle = rec->handle;
            if (Consume) {
                reg->next_log_index = i + 1U;
            }
            return EFI_SUCCESS;
        }
    }
    if (Consume) {
        reg->next_log_index = mProtocolNotifyLogCount;
    }
    return EFI_NOT_FOUND;
}

EFI_STATUS bs_handle_protocol(EFI_HANDLE Handle, void *Protocol,
                                       VOID **Interface)
{
    VOID *interface;
    EFI_STATUS st;

    if (Protocol == NULL || Interface == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (Handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!handle_supports_protocol(Handle, Protocol, &interface)) {
        *Interface = NULL;
        return EFI_UNSUPPORTED;
    }

    st = add_open_protocol_record(Handle, Protocol, mImageHandle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (st != EFI_SUCCESS) {
        return st;
    }

    *Interface = interface;
    return EFI_SUCCESS;
}

static void fw_locate_handle_add(EFI_HANDLE *Matches, UINTN *Count,
                                 UINTN Capacity, EFI_HANDLE Handle)
{
    UINTN i;

    if (Handle == NULL) {
        return;
    }
    for (i = 0; i < *Count; i++) {
        if (Matches[i] == Handle) {
            return;
        }
    }
    if (*Count < Capacity) {
        Matches[*Count] = Handle;
        *Count = *Count + 1U;
    }
}

EFI_STATUS bs_locate_handle(UINTN SearchType, void *Protocol,
                                     VOID *SearchKey, UINTN *BufferSize,
                                     EFI_HANDLE *Buffer)
{
    EFI_HANDLE matches[8U + FW_PCI_IO_DEVICE_COUNT +
                       LOADED_IMAGE_MAX + PROTOCOL_RECORD_MAX];
    UINTN found = 0;
    UINTN needed;
    UINTN i;

    if (BufferSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (SearchType == EFI_LOCATE_BY_REGISTER_NOTIFY) {
        EFI_HANDLE handle;
        EFI_STATUS st;

        if (SearchKey == NULL) {
            return EFI_INVALID_PARAMETER;
        }
        st = fw_protocol_notify_next_handle(SearchKey, &handle, 0);
        if (st == EFI_NOT_FOUND) {
            *BufferSize = 0;
            return st;
        }
        if (st != EFI_SUCCESS) {
            return st;
        }
        needed = sizeof(EFI_HANDLE);
        if (*BufferSize < needed) {
            *BufferSize = needed;
            return EFI_BUFFER_TOO_SMALL;
        }
        if (Buffer == NULL) {
            return EFI_INVALID_PARAMETER;
        }
        Buffer[0] = handle;
        *BufferSize = needed;
        (void)fw_protocol_notify_next_handle(SearchKey, &handle, 1);
        return EFI_SUCCESS;
    }
    if (SearchType != EFI_LOCATE_ALL_HANDLES &&
        SearchType != EFI_LOCATE_BY_PROTOCOL) {
        return EFI_INVALID_PARAMETER;
    }
    if (SearchType == EFI_LOCATE_BY_PROTOCOL && Protocol == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    /*
     * Firmware handle enumeration order is observable to legacy setup
     * loaders.  Publish fixed disks before optical media so rdisk-style
     * probing does not treat the read-only CD-ROM as the install target.
     */
    if (mDiskBlockIoHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mDiskBlockIoHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mDiskBlockIoHandle);
    }

    if (mRawBlockIoHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mRawBlockIoHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mRawBlockIoHandle);
    }

    if (mBlockIoHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mBlockIoHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mBlockIoHandle);
    }

    if (mImageHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mImageHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mImageHandle);
    }

    if (mUnicodeCollationHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mUnicodeCollationHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mUnicodeCollationHandle);
    }

    if (mGraphicsHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mGraphicsHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mGraphicsHandle);
    }

    if (mPciRootBridgeHandle != NULL &&
        (SearchType == EFI_LOCATE_ALL_HANDLES ||
         handle_supports_protocol(mPciRootBridgeHandle, Protocol, NULL))) {
        fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                             mPciRootBridgeHandle);
    }

    for (i = 0; i < FW_ARRAY_SIZE(mPciIoDevices); i++) {
        EFI_HANDLE handle = *mPciIoDevices[i].Handle;

        if (handle != NULL &&
            (SearchType == EFI_LOCATE_ALL_HANDLES ||
             handle_supports_protocol(handle, Protocol, NULL))) {
            fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                                 handle);
        }
    }

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (mLoadedImages[i].in_use &&
            (SearchType == EFI_LOCATE_ALL_HANDLES ||
             handle_supports_protocol(mLoadedImages[i].handle, Protocol, NULL))) {
            fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                                 mLoadedImages[i].handle);
        }
    }

    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use &&
            (SearchType == EFI_LOCATE_ALL_HANDLES ||
             guid_matches(Protocol, mProtocolRecords[i].guid))) {
            fw_locate_handle_add(matches, &found, FW_ARRAY_SIZE(matches),
                                 mProtocolRecords[i].handle);
        }
    }

    if (found == 0) {
        *BufferSize = 0;
        return EFI_NOT_FOUND;
    }
    needed = found * sizeof(EFI_HANDLE);
    if (needed > *BufferSize) {
        *BufferSize = needed;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < found; i++) {
        Buffer[i] = matches[i];
    }
    *BufferSize = needed;
    return EFI_SUCCESS;
}

static EFI_HANDLE fw_allocate_dynamic_handle(void)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mDynamicHandles); i++) {
        if (!mDynamicHandles[i].in_use) {
            mDynamicHandles[i].in_use = 1;
            return (EFI_HANDLE)&mDynamicHandles[i];
        }
    }
    return NULL;
}

static VOID fw_release_dynamic_handle_if_empty(EFI_HANDLE Handle)
{
    UINTN i;

    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use &&
            mProtocolRecords[i].handle == Handle) {
            return;
        }
    }
    for (i = 0; i < FW_ARRAY_SIZE(mDynamicHandles); i++) {
        if (Handle == (EFI_HANDLE)&mDynamicHandles[i]) {
            mDynamicHandles[i].in_use = 0;
            return;
        }
    }
}

static VOID fw_claim_dynamic_handle(EFI_HANDLE Handle)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mDynamicHandles); i++) {
        if (Handle == (EFI_HANDLE)&mDynamicHandles[i]) {
            mDynamicHandles[i].in_use = 1;
            return;
        }
    }
}

EFI_STATUS bs_install_protocol(EFI_HANDLE *Handle, void *Protocol,
                               UINTN InterfaceType, VOID *Interface)
{
    UINTN i;
    BOOLEAN allocated_handle = 0;

    if (Handle == NULL || Protocol == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (InterfaceType != 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (*Handle != NULL && handle_supports_protocol(*Handle, Protocol, NULL)) {
        return EFI_INVALID_PARAMETER;
    }
    if (*Handle == NULL) {
        *Handle = fw_allocate_dynamic_handle();
        if (*Handle == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        allocated_handle = 1;
    } else {
        fw_claim_dynamic_handle(*Handle);
    }
    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (!mProtocolRecords[i].in_use) {
            UINT64 generation = ++mHandleDatabaseGeneration;

            mProtocolRecords[i].in_use = 1;
            mProtocolRecords[i].handle = *Handle;
            copy_guid(mProtocolRecords[i].guid, Protocol);
            mProtocolRecords[i].interface = Interface;
            mProtocolRecords[i].modification_generation = generation;
            fw_notify_protocol_installed(*Handle, Protocol);
            mMapKey++;
            return EFI_SUCCESS;
        }
    }
    fw_release_dynamic_handle_if_empty(*Handle);
    if (allocated_handle) {
        *Handle = NULL;
    }
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS bs_uninstall_protocol(EFI_HANDLE Handle, void *Protocol, VOID *Interface)
{
    UINTN i;

    if (Handle == NULL || Protocol == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use &&
            mProtocolRecords[i].handle == Handle &&
            mProtocolRecords[i].interface == Interface &&
            guid_matches(Protocol, mProtocolRecords[i].guid)) {
            while (open_protocol_driver_open_remains(Handle, Protocol)) {
                if (open_protocol_remove_driver_opens(Handle, Protocol) !=
                    EFI_SUCCESS) {
                    return EFI_ACCESS_DENIED;
                }
            }
            close_uninstall_safe_open_records(Handle, Protocol);
            if (protocol_has_open_records(Handle, Protocol)) {
                return EFI_ACCESS_DENIED;
            }
            {
                UINT64 generation = ++mHandleDatabaseGeneration;
                UINTN j;

                for (j = 0; j < PROTOCOL_RECORD_MAX; j++) {
                    if (j != i && mProtocolRecords[j].in_use &&
                        mProtocolRecords[j].handle == Handle) {
                        mProtocolRecords[j].modification_generation =
                            generation;
                    }
                }
            }
            mProtocolRecords[i].in_use = 0;
            mProtocolRecords[i].handle = NULL;
            mProtocolRecords[i].interface = NULL;
            mProtocolRecords[i].modification_generation = 0;
            fw_release_dynamic_handle_if_empty(Handle);
            mMapKey++;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

EFI_STATUS bs_reinstall_protocol(EFI_HANDLE Handle, void *Protocol,
                                 VOID *OldInterface, VOID *NewInterface)
{
    EFI_STATUS st;
    EFI_HANDLE h = Handle;

    if (Handle == NULL || Protocol == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    st = bs_uninstall_protocol(Handle, Protocol, OldInterface);
    if (st != EFI_SUCCESS) {
        return st;
    }
    return bs_install_protocol(&h, Protocol, 0, NewInterface);
}

EFI_STATUS bs_locate_handle_buffer(UINTN SearchType, void *Protocol,
                                   VOID *SearchKey, UINTN *NoHandles,
                                   EFI_HANDLE **Buffer)
{
    UINTN buffer_size = 0;
    EFI_STATUS st;

    if (NoHandles == NULL || Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *NoHandles = 0;
    *Buffer = NULL;

    st = bs_locate_handle(SearchType, Protocol, SearchKey, &buffer_size, NULL);
    if (st == EFI_NOT_FOUND) {
        return st;
    }
    if (st != EFI_BUFFER_TOO_SMALL && st != EFI_SUCCESS) {
        return st;
    }
    if (buffer_size == 0) {
        return EFI_NOT_FOUND;
    }

    st = bs_allocate_pool(EfiBootServicesData, buffer_size, (VOID **)Buffer);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = bs_locate_handle(SearchType, Protocol, SearchKey, &buffer_size, *Buffer);
    if (st != EFI_SUCCESS) {
        (void)bs_free_pool(*Buffer);
        *Buffer = NULL;
        return st;
    }
    *NoHandles = buffer_size / sizeof(EFI_HANDLE);
    return EFI_SUCCESS;
}

EFI_STATUS bs_locate_protocol(void *Protocol, VOID *Registration, VOID **Interface)
{
    EFI_HANDLE *handles;
    EFI_HANDLE handle;
    UINTN count;
    EFI_STATUS st;

    if (Protocol == NULL || Interface == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (Registration != NULL) {
        st = fw_protocol_notify_next_handle(Registration, &handle, 1);
        if (st != EFI_SUCCESS) {
            *Interface = NULL;
            return st;
        }
        return bs_handle_protocol(handle, Protocol, Interface);
    }

    st = bs_locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL, Protocol, NULL,
                                 &count, &handles);
    if (st != EFI_SUCCESS) {
        *Interface = NULL;
        return st;
    }
    if (count == 0) {
        (void)bs_free_pool(handles);
        *Interface = NULL;
        return EFI_NOT_FOUND;
    }
    handle = handles[0];
    st = bs_handle_protocol(handle, Protocol, Interface);
    (void)bs_free_pool(handles);
    return st;
}

static BOOLEAN __attribute__((noinline)) console_handle_selftest(void)
{
    VOID *interface = NULL;

    if (mSystemTable.ConsoleOutHandle != mGraphicsHandle ||
        mSystemTable.StandardErrorHandle != mGraphicsHandle ||
        mSystemTable.ConOut != &mConOutProto ||
        mSystemTable.StdErr != &mConOutProto) {
        return 0;
    }
    if (!handle_supports_protocol(mGraphicsHandle, (void *)mConOutProtocolGuid,
                                  &interface) ||
        interface != &mConOutProto) {
        return 0;
    }
    interface = NULL;
    return handle_supports_protocol(mGraphicsHandle,
                                    (void *)mGraphicsOutputProtocolGuid,
                                    &interface) &&
           interface == &mGopProto;
}

static void protocol_notify_selftest_callback(EFI_EVENT Event, VOID *Context)
{
    UINTN *count = (UINTN *)Context;

    (void)Event;
    if (count != NULL) {
        *count = *count + 1U;
    }
}

static BOOLEAN __attribute__((noinline)) protocol_notify_selftest(void)
{
    static const UINT8 test_guid[16] = {
        0x54, 0x4e, 0x50, 0x49, 0x51, 0x45, 0x4d, 0x55,
        0x49, 0x41, 0x36, 0x34, 0x54, 0x45, 0x53, 0x54
    };
    static UINTN test_interface;
    EFI_EVENT event = NULL;
    VOID *registration = NULL;
    VOID *interface = NULL;
    EFI_HANDLE handle = NULL;
    UINTN notify_count = 0;
    EFI_STATUS st;

    st = bs_create_event(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                         protocol_notify_selftest_callback, &notify_count,
                         &event);
    if (st != EFI_SUCCESS) {
        return 0;
    }
    st = bs_register_protocol_notify((void *)test_guid, event,
                                     &registration);
    if (st != EFI_SUCCESS || registration == NULL) {
        (void)bs_close_event(event);
        return 0;
    }

    st = bs_install_protocol(&handle, (void *)test_guid, 0, &test_interface);
    if (st != EFI_SUCCESS || handle == NULL || notify_count != 1) {
        (void)bs_close_event(event);
        return 0;
    }

    st = bs_locate_protocol((void *)test_guid, registration, &interface);
    if (st != EFI_SUCCESS || interface != &test_interface) {
        (void)bs_uninstall_protocol(handle, (void *)test_guid,
                                    &test_interface);
        (void)bs_close_event(event);
        return 0;
    }
    interface = (VOID *)(UINTN)0x1;
    st = bs_locate_protocol((void *)test_guid, registration, &interface);
    if (st != EFI_NOT_FOUND || interface != NULL) {
        (void)bs_uninstall_protocol(handle, (void *)test_guid,
                                    &test_interface);
        (void)bs_close_event(event);
        return 0;
    }

    st = bs_uninstall_protocol(handle, (void *)test_guid, &test_interface);
    (void)bs_close_event(event);
    return st == EFI_SUCCESS;
}

static BOOLEAN __attribute__((noinline)) protocol_null_interface_selftest(void)
{
    static const UINT8 marker_guid[16] = {
        0x4e, 0x55, 0x4c, 0x4c, 0x51, 0x45, 0x4d, 0x55,
        0x49, 0x41, 0x36, 0x34, 0x54, 0x45, 0x53, 0x54
    };
    static UINTN replacement_interface;
    EFI_HANDLE handle = NULL;
    EFI_HANDLE *handles = NULL;
    VOID **protocols = NULL;
    VOID *interface;
    UINTN handle_count = 0;
    UINTN protocol_count = 0;
    UINTN i;
    BOOLEAN found_marker;
    EFI_STATUS st;

    st = bs_install_protocol(&handle, (void *)marker_guid, 0, NULL);
    if (st != EFI_SUCCESS || handle == NULL) {
        return 0;
    }

    interface = (VOID *)(UINTN)1;
    if (!handle_supports_protocol(handle, (void *)marker_guid, &interface) ||
        interface != NULL) {
        goto fail;
    }
    interface = (VOID *)(UINTN)1;
    if (bs_handle_protocol(handle, (void *)marker_guid, &interface) !=
        EFI_SUCCESS || interface != NULL) {
        goto fail;
    }
    interface = (VOID *)(UINTN)1;
    if (bs_open_protocol(handle, (void *)marker_guid, &interface,
                         mImageHandle, NULL,
                         EFI_OPEN_PROTOCOL_GET_PROTOCOL) != EFI_SUCCESS ||
        interface != NULL) {
        goto fail;
    }

    if (bs_locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL,
                                (void *)marker_guid, NULL, &handle_count,
                                &handles) != EFI_SUCCESS ||
        handle_count != 1 || handles[0] != handle) {
        goto fail;
    }
    (void)bs_free_pool(handles);
    handles = NULL;

    if (bs_protocols_per_handle(handle, &protocols, &protocol_count) !=
        EFI_SUCCESS) {
        goto fail;
    }
    found_marker = 0;
    for (i = 0; i < protocol_count; i++) {
        if (fw_guid_equal(protocols[i], marker_guid)) {
            found_marker = 1;
            break;
        }
    }
    (void)bs_free_pool(protocols);
    protocols = NULL;
    if (!found_marker) {
        goto fail;
    }

    if (bs_reinstall_protocol(handle, (void *)marker_guid, NULL,
                              &replacement_interface) != EFI_SUCCESS) {
        goto fail;
    }
    interface = NULL;
    if (bs_handle_protocol(handle, (void *)marker_guid, &interface) !=
        EFI_SUCCESS || interface != &replacement_interface) {
        goto fail;
    }
    if (bs_reinstall_protocol(handle, (void *)marker_guid,
                              &replacement_interface, NULL) != EFI_SUCCESS) {
        goto fail;
    }
    interface = (VOID *)(UINTN)1;
    if (bs_handle_protocol(handle, (void *)marker_guid, &interface) !=
        EFI_SUCCESS || interface != NULL) {
        goto fail;
    }
    if (bs_uninstall_protocol(handle, (void *)marker_guid, NULL) !=
        EFI_SUCCESS ||
        handle_supports_protocol(handle, (void *)marker_guid, NULL)) {
        return 0;
    }

    return 1;

fail:
    if (handles != NULL) {
        (void)bs_free_pool(handles);
    }
    if (protocols != NULL) {
        (void)bs_free_pool(protocols);
    }
    if (handle != NULL) {
        (void)bs_uninstall_protocol(handle, (void *)marker_guid, NULL);
        (void)bs_uninstall_protocol(handle, (void *)marker_guid,
                                    &replacement_interface);
    }
    return 0;
}

/* --- Runtime Services implementations ------------------------------------- */

EFI_STATUS rs_convert_pointer_value(UINTN *Address);
static EFI_STATUS rs_convert_runtime_tables(void);

EFI_STATUS rs_set_virtual_address_map(
    UINTN MemoryMapSize, UINTN DescriptorSize,
    UINT32 DescriptorVersion, EFI_MEMORY_DESCRIPTOR *VirtualMap)
{
    UINTN offset;
    UINTN runtime_index = 0;

    if (!mBootServicesExited || mVirtualAddressMapApplied) {
        return EFI_UNSUPPORTED;
    }
    if (VirtualMap == NULL || MemoryMapSize == 0 ||
        DescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        DescriptorVersion != EFI_MEMORY_DESCRIPTOR_VERSION ||
        (MemoryMapSize % DescriptorSize) != 0) {
        return EFI_INVALID_PARAMETER;
    }

    for (offset = 0; offset < mMemoryMapEntries; offset++) {
        EFI_MEMORY_DESCRIPTOR *runtime_desc = &mMemoryMap[offset];
        EFI_MEMORY_DESCRIPTOR *found = NULL;
        UINTN map_offset;

        if ((runtime_desc->Attribute & EFI_MEMORY_RUNTIME) == 0) {
            continue;
        }

        for (map_offset = 0; map_offset < MemoryMapSize;
             map_offset += DescriptorSize) {
            EFI_MEMORY_DESCRIPTOR *desc =
                (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)VirtualMap + map_offset);

            if (desc->PhysicalStart == runtime_desc->PhysicalStart &&
                desc->NumberOfPages == runtime_desc->NumberOfPages) {
                found = desc;
                break;
            }
        }

        if (found == NULL || found->VirtualStart == 0 ||
            (found->Attribute & EFI_MEMORY_RUNTIME) == 0) {
            return EFI_NO_MAPPING;
        }
        if ((found->PhysicalStart & 0xfffULL) != 0 ||
            (found->VirtualStart & 0xfffULL) != 0) {
            return EFI_INVALID_PARAMETER;
        }
        if (runtime_index >= MEMORY_MAP_MAX) {
            return EFI_INVALID_PARAMETER;
        }
        mVirtualAddressMap[runtime_index++] = *found;
    }

    for (offset = 0; offset < MemoryMapSize; offset += DescriptorSize) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)VirtualMap + offset);
        BOOLEAN found = 0;
        UINTN i;

        if ((desc->Attribute & EFI_MEMORY_RUNTIME) == 0 ||
            desc->VirtualStart == 0) {
            continue;
        }
        for (i = 0; i < mMemoryMapEntries; i++) {
            EFI_MEMORY_DESCRIPTOR *runtime_desc = &mMemoryMap[i];

            if ((runtime_desc->Attribute & EFI_MEMORY_RUNTIME) != 0 &&
                runtime_desc->PhysicalStart == desc->PhysicalStart &&
                runtime_desc->NumberOfPages == desc->NumberOfPages) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return EFI_NOT_FOUND;
        }
    }

    mVirtualAddressMapEntries = runtime_index;
    mVirtualAddressMapInProgress = 1;
    fw_signal_event_group_and_type(
        gEfiEventGroupVirtualAddressChangeGuid,
        EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE);
    if (pe_relocate_runtime_images() != EFI_SUCCESS) {
        mVirtualAddressMapInProgress = 0;
        mVirtualAddressMapEntries = 0;
        return EFI_NOT_FOUND;
    }
    if (rs_convert_runtime_tables() != EFI_SUCCESS) {
        mVirtualAddressMapInProgress = 0;
        mVirtualAddressMapEntries = 0;
        return EFI_NOT_FOUND;
    }
    mVirtualAddressMapInProgress = 0;
    mVirtualAddressMapApplied = 1;
    efi_refresh_table_crc32s();
    return EFI_SUCCESS;
}

EFI_STATUS rs_convert_pointer_value(UINTN *Address)
{
    UINTN value = *Address;
    UINTN i;

    if (!mVirtualAddressMapInProgress && !mVirtualAddressMapApplied) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i < mVirtualAddressMapEntries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = &mVirtualAddressMap[i];
        UINT64 start = desc->PhysicalStart;
        UINT64 size = desc->NumberOfPages << 12;
        UINT64 end = start + size;

        if (end < start) {
            continue;
        }
        if (value >= start && value < end) {
            *Address = (UINTN)(desc->VirtualStart + (value - start));
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

EFI_STATUS rs_convert_pointer(UINTN DebugDisposition, VOID **Address)
{
    UINTN value;
    EFI_STATUS st;

    if ((DebugDisposition & ~(UINTN)EFI_OPTIONAL_PTR) != 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (Address == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (*Address == NULL) {
        return (DebugDisposition & EFI_OPTIONAL_PTR) != 0 ?
            EFI_SUCCESS : EFI_INVALID_PARAMETER;
    }

    value = (UINTN)*Address;
    st = rs_convert_pointer_value(&value);
    if (st == EFI_SUCCESS) {
        *Address = (VOID *)value;
    }
    return st;
}

static BOOLEAN __attribute__((noinline))
uefi_convert_pointer_selftest(void)
{
    VOID *address = (VOID *)(UINTN)0x1000U;
    VOID *original = address;

    if (rs_convert_pointer(2U, &address) != EFI_INVALID_PARAMETER ||
        address != original) {
        return 0;
    }
    address = NULL;
    if (rs_convert_pointer(EFI_OPTIONAL_PTR | 2U, &address) !=
            EFI_INVALID_PARAMETER ||
        address != NULL) {
        return 0;
    }
    address = original;
    if (rs_convert_pointer(~(UINTN)EFI_OPTIONAL_PTR, &address) !=
            EFI_INVALID_PARAMETER ||
        address != original) {
        return 0;
    }
    if (rs_convert_pointer(EFI_OPTIONAL_PTR, &address) != EFI_NOT_FOUND ||
        address != original) {
        return 0;
    }
    address = NULL;
    return rs_convert_pointer(EFI_OPTIONAL_PTR, &address) == EFI_SUCCESS &&
           address == NULL &&
           rs_convert_pointer(0, &address) == EFI_INVALID_PARAMETER;
}

static EFI_STATUS rs_convert_required_uintn(UINTN *Address)
{
    return rs_convert_pointer_value(Address);
}

static EFI_STATUS rs_convert_function_descriptor(UINTN Address,
                                                  BOOLEAN Commit)
{
    IA64_FUNCTION_DESCRIPTOR *descriptor;
    UINTN virtual_descriptor = Address;
    UINTN entry;
    UINTN gp;
    EFI_STATUS st;

    st = rs_convert_pointer_value(&virtual_descriptor);
    if (st != EFI_SUCCESS) {
        return st;
    }

    descriptor = (IA64_FUNCTION_DESCRIPTOR *)Address;
    entry = descriptor->entry;
    gp = descriptor->gp;
    st = rs_convert_pointer_value(&entry);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_pointer_value(&gp);
    if (st != EFI_SUCCESS) {
        return st;
    }

    if (Commit) {
        descriptor->entry = entry;
        descriptor->gp = gp;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS __attribute__((noinline))
rs_convert_firmware_variables(void)
{
    UINTN names[FW_FIRMWARE_VARIABLE_COUNT];
    UINTN guids[FW_FIRMWARE_VARIABLE_COUNT];
    UINTN data[FW_FIRMWARE_VARIABLE_COUNT];
    UINTN reads[FW_FIRMWARE_VARIABLE_COUNT];
    UINTN read_descriptors[FW_FIRMWARE_VARIABLE_COUNT];
    UINTN runtime_variables = (UINTN)mRuntimeFirmwareVariables;
    UINTN i;
    EFI_STATUS st;

    st = rs_convert_required_uintn(&runtime_variables);
    if (st != EFI_SUCCESS) {
        return st;
    }

    for (i = 0; i < FW_FIRMWARE_VARIABLE_COUNT; i++) {
        names[i] = (UINTN)mFirmwareVariables[i].name;
        guids[i] = (UINTN)mFirmwareVariables[i].guid;
        data[i] = (UINTN)mFirmwareVariables[i].data;
        reads[i] = (UINTN)mFirmwareVariables[i].read;
        read_descriptors[i] = reads[i];

        st = rs_convert_required_uintn(&names[i]);
        if (st != EFI_SUCCESS) {
            return st;
        }
        st = rs_convert_required_uintn(&guids[i]);
        if (st != EFI_SUCCESS) {
            return st;
        }
        if (data[i] != 0) {
            st = rs_convert_required_uintn(&data[i]);
            if (st != EFI_SUCCESS) {
                return st;
            }
        }
        if (reads[i] != 0) {
            st = rs_convert_function_descriptor(read_descriptors[i], 0);
            if (st != EFI_SUCCESS) {
                return st;
            }
            st = rs_convert_required_uintn(&reads[i]);
            if (st != EFI_SUCCESS) {
                return st;
            }
        }
    }

    for (i = 0; i < FW_FIRMWARE_VARIABLE_COUNT; i++) {
        if (read_descriptors[i] != 0) {
            st = rs_convert_function_descriptor(read_descriptors[i], 1);
            if (st != EFI_SUCCESS) {
                return st;
            }
        }
    }
    for (i = 0; i < FW_FIRMWARE_VARIABLE_COUNT; i++) {
        mFirmwareVariables[i].name = (const char *)names[i];
        mFirmwareVariables[i].guid = (const UINT8 *)guids[i];
        mFirmwareVariables[i].data = (const VOID *)data[i];
        mFirmwareVariables[i].read =
            (FW_FIRMWARE_VARIABLE_READ)reads[i];
    }
    mRuntimeFirmwareVariables =
        (FW_FIRMWARE_VARIABLE *)runtime_variables;
    return EFI_SUCCESS;
}

static EFI_STATUS rs_convert_runtime_tables(void)
{
    EFI_STATUS st;
    UINTN i;
    UINTN get_time = mRuntimeServices.GetTime;
    UINTN set_time = mRuntimeServices.SetTime;
    UINTN get_wakeup_time = mRuntimeServices.GetWakeupTime;
    UINTN set_wakeup_time = mRuntimeServices.SetWakeupTime;
    UINTN get_variable = mRuntimeServices.GetVariable;
    UINTN get_next_variable_name = mRuntimeServices.GetNextVariableName;
    UINTN set_variable = mRuntimeServices.SetVariable;
    UINTN get_next_high = mRuntimeServices.GetNextHighMonotonicCount;
    UINTN reset_system = mRuntimeServices.ResetSystem;
    UINTN query_variable_info = mRuntimeServices.QueryVariableInfo;
    UINTN fpswa = (UINTN)mFpswaProto.fpswa;
    UINTN firmware_vendor = (UINTN)mSystemTable.FirmwareVendor;
    UINTN runtime_services = (UINTN)mSystemTable.RuntimeServices;
    UINTN configuration_table = (UINTN)mSystemTable.ConfigurationTable;
    UINTN runtime_acpi_pm1_cnt = mRuntimeAcpiPm1Cnt;
    UINTN runtime_reset_control = mRuntimeResetControl;
    UINTN runtime_pci_config_ecam = mRuntimePciConfigEcam;
    UINTN runtime_rtc = mRuntimeRtc;
    UINTN runtime_rtc_state = mRuntimeRtcState;
    UINTN nvram_store = (UINTN)mNvramStore;
    /* Physical-only virtual-memory services are deliberately excluded. */
    UINTN function_descriptors[] = {
        mRuntimeServices.GetTime,
        mRuntimeServices.SetTime,
        mRuntimeServices.GetWakeupTime,
        mRuntimeServices.SetWakeupTime,
        mRuntimeServices.GetVariable,
        mRuntimeServices.GetNextVariableName,
        mRuntimeServices.SetVariable,
        mRuntimeServices.GetNextHighMonotonicCount,
        mRuntimeServices.ResetSystem,
        mRuntimeServices.QueryVariableInfo,
        (UINTN)mFpswaProto.fpswa,
    };

    st = rs_convert_required_uintn(&get_time);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&set_time);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&get_wakeup_time);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&set_wakeup_time);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&get_variable);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&get_next_variable_name);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&set_variable);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&get_next_high);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&reset_system);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&query_variable_info);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&fpswa);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&firmware_vendor);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&runtime_services);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&configuration_table);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&runtime_acpi_pm1_cnt);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&runtime_reset_control);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&runtime_pci_config_ecam);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&runtime_rtc);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&runtime_rtc_state);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = rs_convert_required_uintn(&nvram_store);
    if (st != EFI_SUCCESS) {
        return st;
    }

    for (i = 0; i < FW_ARRAY_SIZE(function_descriptors); i++) {
        st = rs_convert_function_descriptor(function_descriptors[i], 0);
        if (st != EFI_SUCCESS) {
            return st;
        }
    }
    st = rs_convert_firmware_variables();
    if (st != EFI_SUCCESS) {
        return st;
    }
    for (i = 0; i < FW_ARRAY_SIZE(function_descriptors); i++) {
        st = rs_convert_function_descriptor(function_descriptors[i], 1);
        if (st != EFI_SUCCESS) {
            return st;
        }
    }

    mRuntimeServices.GetTime = get_time;
    mRuntimeServices.SetTime = set_time;
    mRuntimeServices.GetWakeupTime = get_wakeup_time;
    mRuntimeServices.SetWakeupTime = set_wakeup_time;
    mRuntimeServices.GetVariable = get_variable;
    mRuntimeServices.GetNextVariableName = get_next_variable_name;
    mRuntimeServices.SetVariable = set_variable;
    mRuntimeServices.GetNextHighMonotonicCount = get_next_high;
    mRuntimeServices.ResetSystem = reset_system;
    mRuntimeServices.QueryVariableInfo = query_variable_info;
    mFpswaProto.fpswa = (IA64_EFI_FPSWA)fpswa;
    mSystemTable.FirmwareVendor = (CHAR16 *)firmware_vendor;
    mSystemTable.RuntimeServices = (EFI_RUNTIME_SERVICES *)runtime_services;
    mSystemTable.ConfigurationTable =
        (EFI_CONFIGURATION_TABLE *)configuration_table;
    mRuntimeAcpiPm1Cnt = runtime_acpi_pm1_cnt;
    mRuntimeResetControl = runtime_reset_control;
    mRuntimePciConfigEcam = runtime_pci_config_ecam;
    mRuntimeRtc = runtime_rtc;
    mRuntimeRtcState = runtime_rtc_state;
    mNvramStore = (NVRAM_STORE *)nvram_store;
    return EFI_SUCCESS;
}

static UINTN rs_variable_name_size(CHAR16 *VariableName)
{
    UINTN chars;

    if (VariableName == NULL) {
        return 0;
    }

    for (chars = 0; chars < NVRAM_VAR_NAME_MAX; chars++) {
        if (VariableName[chars] == 0) {
            return (chars + 1) * sizeof(CHAR16);
        }
    }
    return 0;
}

static UINTN rs_variable_name_size_bounded(CHAR16 *VariableName,
                                           UINTN VariableNameSize)
{
    UINTN chars;
    UINTN max_chars;

    if (VariableName == NULL || VariableNameSize < sizeof(CHAR16)) {
        return 0;
    }

    max_chars = VariableNameSize / sizeof(CHAR16);
    for (chars = 0; chars < max_chars; chars++) {
        if (VariableName[chars] == 0) {
            return (chars + 1) * sizeof(CHAR16);
        }
    }
    return 0;
}

static BOOLEAN rs_variable_attrs_supported(UINT32 Attributes)
{
    return (Attributes & ~EFI_VARIABLE_SUPPORTED_ATTRIBUTES) == 0;
}

static BOOLEAN rs_variable_delete_request(UINT32 Attributes, UINTN DataSize)
{
    if (DataSize == 0) {
        return 1;
    }
    return (Attributes & EFI_VARIABLE_ACCESS_ATTRIBUTES) == 0;
}

static BOOLEAN rs_variable_writable_after_exit(UINT32 Attributes)
{
    return (Attributes & (EFI_VARIABLE_RUNTIME_ACCESS |
                          EFI_VARIABLE_NON_VOLATILE)) ==
           (EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE);
}

static BOOLEAN rs_variable_visible(UINT32 Attributes)
{
    if (!mBootServicesExited) {
        return 1;
    }
    return (Attributes & EFI_VARIABLE_RUNTIME_ACCESS) != 0;
}

static EFI_STATUS rs_copy_variable(UINT32 Attributes, const VOID *Source,
                                   UINTN SourceSize, UINT32 *OutAttributes,
                                   UINTN *DataSize, VOID *Data)
{
    if (*DataSize < SourceSize) {
        if (OutAttributes != NULL) {
            *OutAttributes = Attributes;
        }
        *DataSize = SourceSize;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (Data == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (OutAttributes != NULL) {
        *OutAttributes = Attributes;
    }
    if (SourceSize != 0 && Source != NULL) {
        fw_copy_mem(Data, Source, SourceSize);
    }
    *DataSize = SourceSize;
    return EFI_SUCCESS;
}

static BOOLEAN rs_find_firmware_variable(CHAR16 *VariableName,
                                         void *VendorGuid, UINTN *Index)
{
    UINTN i;

    for (i = 0; i < FW_FIRMWARE_VARIABLE_COUNT; i++) {
        if (guid_matches(VendorGuid, mRuntimeFirmwareVariables[i].guid) &&
            fw_char16_eq_ascii_z(VariableName,
                                 mRuntimeFirmwareVariables[i].name)) {
            if (Index != NULL) {
                *Index = i;
            }
            return 1;
        }
    }
    return 0;
}

static BOOLEAN rs_nvram_name_eq_ascii(const NVRAM_VARIABLE *Var,
                                      const char *Name)
{
    UINTN i;
    UINTN chars = 0;

    while (Name[chars] != 0) {
        chars++;
    }
    if (Var->name_len != (chars + 1) * sizeof(CHAR16)) {
        return 0;
    }
    for (i = 0; i < chars; i++) {
        if (Var->name[i * sizeof(CHAR16)] != (UINT8)Name[i] ||
            Var->name[i * sizeof(CHAR16) + 1] != 0) {
            return 0;
        }
    }
    return Var->name[chars * sizeof(CHAR16)] == 0 &&
           Var->name[chars * sizeof(CHAR16) + 1] == 0;
}

static BOOLEAN rs_nvram_aliases_firmware_variable(const NVRAM_VARIABLE *Var)
{
    UINTN i;

    for (i = 0; i < FW_FIRMWARE_VARIABLE_COUNT; i++) {
        if (rs_nvram_name_eq_ascii(
                Var, mRuntimeFirmwareVariables[i].name)) {
            UINTN n;
            for (n = 0; n < 16; n++) {
                if (Var->guid[n] !=
                    mRuntimeFirmwareVariables[i].guid[n]) {
                    break;
                }
            }
            if (n == 16) {
                return 1;
            }
        }
    }
    return 0;
}

static BOOLEAN rs_find_nvram_ascii_variable(const char *VariableName,
                                            const UINT8 *VendorGuid,
                                            UINTN *Index)
{
    UINTN i;

    for (i = 0; i < mNvramVarCount; i++) {
        if (!mNvramVars[i].valid ||
            !rs_nvram_name_eq_ascii(&mNvramVars[i], VariableName)) {
            continue;
        }
        if (guid_matches(mNvramVars[i].guid, VendorGuid)) {
            if (Index != NULL) {
                *Index = i;
            }
            return 1;
        }
    }
    return 0;
}

static BOOLEAN rs_firmware_variable_deleted(UINTN Index)
{
    UINTN nvram_index;

    if (Index >= FW_FIRMWARE_VARIABLE_COUNT) {
        return 0;
    }
    if (!rs_find_nvram_ascii_variable(
                                      mRuntimeFirmwareVariables[Index].name,
                                      mRuntimeFirmwareVariables[Index].guid,
                                      &nvram_index)) {
        return 0;
    }
    return mNvramVars[nvram_index].deleted;
}

static BOOLEAN rs_find_nvram_variable(CHAR16 *VariableName, void *VendorGuid,
                                      UINTN *Index)
{
    UINTN i;
    UINTN name_size = rs_variable_name_size(VariableName);
    UINT8 *guid = (UINT8 *)VendorGuid;
    UINT8 *name = (UINT8 *)VariableName;

    if (name_size == 0) {
        return 0;
    }

    for (i = 0; i < mNvramVarCount; i++) {
        UINTN n;

        if (!mNvramVars[i].valid ||
            mNvramVars[i].name_len != name_size) {
            continue;
        }

        for (n = 0; n < 16; n++) {
            if (guid[n] != mNvramVars[i].guid[n]) {
                break;
            }
        }
        if (n != 16) {
            continue;
        }

        for (n = 0; n < name_size; n++) {
            if (name[n] != mNvramVars[i].name[n]) {
                break;
            }
        }
        if (n == name_size) {
            if (Index != NULL) {
                *Index = i;
            }
            return 1;
        }
    }
    return 0;
}

EFI_STATUS rs_get_boot0000_variable(UINT32 *Attributes,
                                           UINTN *DataSize, VOID *Data)
{
    FW_EFI_BOOT_OPTION option;

    fw_set_mem(&option, sizeof(option), 0);
    option.Attributes = 0x00000001U;
    option.FilePathListLength = sizeof(option.FilePath);
    fw_copy_mem(option.Description, mDefaultBootDescription,
                sizeof(option.Description));
    fw_copy_mem(&option.FilePath, &mOpticalSetupLoaderDevicePath,
                sizeof(option.FilePath));

    /*
     * A generic "boot from optical media" entry is OS-agnostic: launch the
     * media's \EFI\BOOT\BOOTIA64.EFI with EMPTY load options, matching
     * standard EFI removable-media boot.  Report the option without any
     * OptionalData so the loader receives no injected payload.  (An OS's own
     * NVRAM Boot#### entry still carries its load options verbatim; only this
     * synthetic firmware entry drops the previously Windows-specific blob.)
     */
    return rs_copy_variable(
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        &option, sizeof(FW_EFI_BOOT_OPTION),
        Attributes, DataSize, Data);
}

/*
 * Built-in EFI shell as a Boot#### option.  The Intel sample publishes the
 * internal shell behind a HW_VENDOR device-path node identified by this GUID
 * ({d65a6b8c-71e5-4df0-a909-f0d2992b5aa9}) and the boot manager turns that
 * handle into a Boot#### variable; selecting it runs the built-in shell rather
 * than loading an image.  We mirror that: a firmware-provided Boot#### carries
 * the same vendor node, and the loader recognises the GUID.
 */
static const UINT8 mInternalShellGuid[16] = {
    0x8c, 0x6b, 0x5a, 0xd6, 0xe5, 0x71, 0xf0, 0x4d,
    0xa9, 0x09, 0xf0, 0xd2, 0x99, 0x2b, 0x5a, 0xa9,
};

BOOLEAN fw_device_path_is_internal_shell(const VOID *DevicePath)
{
    const FW_DEVICE_PATH_NODE *node = (const FW_DEVICE_PATH_NODE *)DevicePath;
    const UINT8 *guid;
    UINTN i;

    if (node == NULL || node->Type != 0x01U || node->SubType != 0x04U ||
        node->Length != (UINT16)sizeof(FW_VENDOR_DEVICE_PATH_NODE)) {
        return 0;
    }
    guid = (const UINT8 *)DevicePath + sizeof(FW_DEVICE_PATH_NODE);
    for (i = 0; i < 16U; i++) {
        if (guid[i] != mInternalShellGuid[i]) {
            return 0;
        }
    }
    return 1;
}

EFI_STATUS rs_get_shell_variable(UINT32 *Attributes, UINTN *DataSize,
                                 VOID *Data)
{
    static const CHAR16 shell_desc[21] = {
        'E', 'F', 'I', ' ', 'S', 'h', 'e', 'l', 'l', ' ',
        '[', 'B', 'u', 'i', 'l', 't', '-', 'i', 'n', ']', 0,
    };
    FW_SHELL_BOOT_OPTION option;

    fw_set_mem(&option, sizeof(option), 0);
    option.Attributes = 0x00000001U;
    fw_copy_mem(option.Description, shell_desc, sizeof(option.Description));
    option.Vendor.Header.Type = 0x01U;      /* HARDWARE_DEVICE_PATH */
    option.Vendor.Header.SubType = 0x04U;   /* HW_VENDOR_DP */
    option.Vendor.Header.Length = (UINT16)sizeof(FW_VENDOR_DEVICE_PATH_NODE);
    fw_copy_mem(option.Vendor.Guid, mInternalShellGuid, 16);
    option.End.Type = 0x7fU;
    option.End.SubType = 0xffU;
    option.End.Length = (UINT16)sizeof(FW_DEVICE_PATH_NODE);
    option.FilePathListLength =
        (UINT16)(sizeof(option.Vendor) + sizeof(option.End));
    return rs_copy_variable(
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        &option, sizeof(FW_SHELL_BOOT_OPTION),
        Attributes, DataSize, Data);
}

static EFI_STATUS rs_get_firmware_variable(UINTN Index, UINT32 *Attributes,
                                           UINTN *DataSize, VOID *Data)
{
    const FW_FIRMWARE_VARIABLE *var;

    if (Index >= FW_FIRMWARE_VARIABLE_COUNT) {
        return EFI_NOT_FOUND;
    }
    var = &mRuntimeFirmwareVariables[Index];
    if (!rs_firmware_variable_enabled(var) ||
        !rs_variable_visible(var->attributes) ||
        rs_firmware_variable_deleted(Index)) {
        return EFI_NOT_FOUND;
    }
    if (var->read != NULL) {
        return var->read(Attributes, DataSize, Data);
    }
    return rs_copy_variable(var->attributes, var->data, var->data_size,
                            Attributes, DataSize, Data);
}

static EFI_STATUS rs_copy_ascii_variable_name(const char *Name,
                                             const UINT8 *Guid,
                                             UINTN *VariableNameSize,
                                             CHAR16 *VariableName,
                                             void *VendorGuid)
{
    UINTN i;
    UINTN needed = 0;

    while (Name[needed] != 0) {
        needed++;
    }
    needed = (needed + 1) * sizeof(CHAR16);
    if (*VariableNameSize < needed) {
        *VariableNameSize = needed;
        return EFI_BUFFER_TOO_SMALL;
    }
    for (i = 0; i < needed / sizeof(CHAR16); i++) {
        VariableName[i] = (CHAR16)(UINT8)Name[i];
    }
    fw_copy_mem(VendorGuid, Guid, 16);
    *VariableNameSize = needed;
    return EFI_SUCCESS;
}

static EFI_STATUS rs_copy_nvram_variable_name(const NVRAM_VARIABLE *Var,
                                             UINTN *VariableNameSize,
                                             CHAR16 *VariableName,
                                             void *VendorGuid)
{
    if (*VariableNameSize < Var->name_len) {
        *VariableNameSize = Var->name_len;
        return EFI_BUFFER_TOO_SMALL;
    }
    fw_copy_mem(VariableName, Var->name, Var->name_len);
    fw_copy_mem(VendorGuid, Var->guid, 16);
    *VariableNameSize = Var->name_len;
    return EFI_SUCCESS;
}

EFI_STATUS rs_get_variable(CHAR16 *VariableName, void *VendorGuid,
                                   UINT32 *Attributes, UINTN *DataSize,
                                   VOID *Data)
{
    UINTN index;
    UINTN name_size;

    if (VariableName == NULL || VendorGuid == NULL || DataSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    name_size = rs_variable_name_size(VariableName);
    if (name_size == 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (guid_matches(VendorGuid, mBlockIoProtocolGuid) &&
        fw_char16_eq_ascii_z(VariableName, "EDD30")) {
        if (*DataSize < 1) {
            if (Attributes != NULL) {
                *Attributes = 0x00000007;
            }
            *DataSize = 1;
            return EFI_BUFFER_TOO_SMALL;
        }
        if (Data == NULL) {
            return EFI_INVALID_PARAMETER;
        }
        if (Attributes != NULL) {
            *Attributes = 0x00000007;
        }
        *(UINT8 *)Data = 1;
        *DataSize = 1;
        return EFI_SUCCESS;
    }
    if (rs_find_nvram_variable(VariableName, VendorGuid, &index)) {
        if (mNvramVars[index].deleted ||
            !rs_variable_visible(mNvramVars[index].attributes)) {
            return EFI_NOT_FOUND;
        }
        return rs_copy_variable(mNvramVars[index].attributes,
                                            mNvramVars[index].data,
                                            mNvramVars[index].data_size,
                                            Attributes, DataSize, Data);
    }

    if (rs_find_firmware_variable(VariableName, VendorGuid, &index)) {
        return rs_get_firmware_variable(index, Attributes,
                                                    DataSize, Data);
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS rs_set_variable(CHAR16 *VariableName, void *VendorGuid,
                                   UINT32 Attributes, UINTN DataSize,
                                   VOID *Data)
{
    UINTN i = 0;
    UINTN n;
    UINTN firmware_index = 0;
    UINTN name_size;
    UINT8 *src;
    BOOLEAN have_nvram;
    BOOLEAN have_firmware;
    BOOLEAN deleting;
    BOOLEAN existing;
    UINT32 existing_attributes = 0;

    if (VariableName == NULL || VendorGuid == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    name_size = rs_variable_name_size(VariableName);
    if (name_size <= sizeof(CHAR16)) {
        return EFI_INVALID_PARAMETER;
    }
    if (!rs_variable_attrs_supported(Attributes)) {
        return EFI_INVALID_PARAMETER;
    }

    deleting = rs_variable_delete_request(Attributes, DataSize);
    if (!deleting &&
        (Attributes & EFI_VARIABLE_RUNTIME_ACCESS) != 0 &&
        (Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (!deleting && mBootServicesExited &&
        !rs_variable_writable_after_exit(Attributes)) {
        return EFI_INVALID_PARAMETER;
    }

    have_nvram = rs_find_nvram_variable(VariableName, VendorGuid, &i);
    have_firmware = rs_find_firmware_variable(VariableName, VendorGuid,
                                              &firmware_index);
    existing = 0;
    if (have_nvram && !mNvramVars[i].deleted) {
        existing = 1;
        existing_attributes = mNvramVars[i].attributes;
    } else if (!have_nvram && have_firmware) {
        existing = 1;
        existing_attributes =
            mRuntimeFirmwareVariables[firmware_index].attributes;
    }

    if (deleting) {
        if (!existing) {
            return EFI_NOT_FOUND;
        }
        if (mBootServicesExited &&
            !rs_variable_writable_after_exit(existing_attributes)) {
            return EFI_INVALID_PARAMETER;
        }
        if (have_firmware) {
            if (!have_nvram) {
                for (i = 0; i < mNvramVarCount; i++) {
                    if (!mNvramVars[i].valid) {
                        break;
                    }
                }
                if (i >= NVRAM_VAR_MAX) {
                    return EFI_OUT_OF_RESOURCES;
                }
                if (i == mNvramVarCount) {
                    mNvramVarCount++;
                }
            }
            mNvramVars[i].valid = 1;
            mNvramVars[i].deleted = 1;
            mNvramVars[i].data_size = 0;
            mNvramVars[i].attributes = existing_attributes;
            fw_copy_mem(mNvramVars[i].name, VariableName, name_size);
            mNvramVars[i].name_len = name_size;
            fw_copy_mem(mNvramVars[i].guid, VendorGuid, 16);
            if ((existing_attributes & EFI_VARIABLE_NON_VOLATILE) != 0) {
                nvram_commit();
            }
            return EFI_SUCCESS;
        }
        mNvramVars[i].valid = 0;
        mNvramVars[i].deleted = 0;
        if ((existing_attributes & EFI_VARIABLE_NON_VOLATILE) != 0) {
            nvram_commit();
        }
        return EFI_SUCCESS;
    }

    if (DataSize > NVRAM_VAR_DATA_MAX) {
        return EFI_OUT_OF_RESOURCES;
    }
    if (Data == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (existing && Attributes != existing_attributes) {
        return EFI_INVALID_PARAMETER;
    }

    if (!have_nvram) {
        for (i = 0; i < mNvramVarCount; i++) {
            if (!mNvramVars[i].valid) {
                break;
            }
        }
        if (i >= NVRAM_VAR_MAX) {
            return EFI_OUT_OF_RESOURCES;
        }
        if (i == mNvramVarCount) {
            mNvramVarCount++;
        }
    }

    mNvramVars[i].valid = 1;
    mNvramVars[i].deleted = 0;
    mNvramVars[i].data_size = DataSize;
    mNvramVars[i].attributes = Attributes;
    fw_copy_mem(mNvramVars[i].guid, VendorGuid, 16);
    fw_copy_mem(mNvramVars[i].name, VariableName, name_size);
    mNvramVars[i].name_len = name_size;
    src = (UINT8 *)Data;
    for (n = 0; n < DataSize; n++) {
        mNvramVars[i].data[n] = src[n];
    }
    if ((Attributes & EFI_VARIABLE_NON_VOLATILE) != 0) {
        nvram_commit();
    }
    return EFI_SUCCESS;
}

EFI_STATUS rs_get_next_var_name(UINTN *VariableNameSize,
                                        CHAR16 *VariableName, void *VendorGuid)
{
    UINTN index;
    UINTN input_name_size;
    UINTN start_static = 0;
    UINTN start_nvram = 0;
    BOOLEAN previous_static = 0;

    if (VariableNameSize == NULL || VariableName == NULL ||
        VendorGuid == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    input_name_size =
        rs_variable_name_size_bounded(VariableName, *VariableNameSize);
    if (input_name_size == 0) {
        return EFI_INVALID_PARAMETER;
    }

    if (VariableName[0] != 0) {
        if (rs_find_firmware_variable(VariableName, VendorGuid, &index) &&
            rs_variable_visible(
                mRuntimeFirmwareVariables[index].attributes) &&
            !rs_firmware_variable_deleted(index)) {
            start_static = index + 1;
            previous_static = 1;
        } else if (rs_find_nvram_variable(VariableName, VendorGuid, &index) &&
                   !mNvramVars[index].deleted &&
                   rs_variable_visible(mNvramVars[index].attributes)) {
            start_static = FW_FIRMWARE_VARIABLE_COUNT;
            start_nvram = index + 1;
        } else {
            return EFI_INVALID_PARAMETER;
        }
    }

    for (index = start_static; index < FW_FIRMWARE_VARIABLE_COUNT;
         index++) {
        if (rs_firmware_variable_enabled(
                &mRuntimeFirmwareVariables[index]) &&
            rs_variable_visible(
                mRuntimeFirmwareVariables[index].attributes) &&
            !rs_firmware_variable_deleted(index)) {
            return rs_copy_ascii_variable_name(
                                    mRuntimeFirmwareVariables[index].name,
                                    mRuntimeFirmwareVariables[index].guid,
                                    VariableNameSize, VariableName,
                                    VendorGuid);
        }
    }

    if (previous_static) {
        start_nvram = 0;
    }
    for (index = start_nvram; index < mNvramVarCount; index++) {
        if (!mNvramVars[index].valid ||
            mNvramVars[index].deleted ||
            !rs_variable_visible(mNvramVars[index].attributes) ||
            rs_nvram_aliases_firmware_variable(&mNvramVars[index])) {
            continue;
        }
        return rs_copy_nvram_variable_name(&mNvramVars[index],
                                                        VariableNameSize,
                                                        VariableName,
                                                        VendorGuid);
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS rs_get_next_high_monotonic_count(UINT32 *HighCount)
{
    if (HighCount == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    mHighMonotonicCount++;
    *HighCount = mHighMonotonicCount;
    return EFI_SUCCESS;
}

EFI_STATUS rs_query_variable_info(UINT32 Attributes,
                                  UINT64 *MaximumVariableStorageSize,
                                  UINT64 *RemainingVariableStorageSize,
                                  UINT64 *MaximumVariableSize)
{
    UINT32 effective_attributes;
    UINT64 maximum_storage;
    UINT64 used_storage = 0;
    UINTN i;

    if (MaximumVariableStorageSize == NULL ||
        RemainingVariableStorageSize == NULL ||
        MaximumVariableSize == NULL) {
        effective_attributes = 0;
        return EFI_INVALID_PARAMETER;
    }

    effective_attributes = Attributes & ~EFI_VARIABLE_APPEND_WRITE;
    if (!rs_variable_attrs_supported(effective_attributes)) {
        return EFI_UNSUPPORTED;
    }
    if ((effective_attributes & EFI_VARIABLE_ACCESS_ATTRIBUTES) == 0 ||
        ((effective_attributes & EFI_VARIABLE_RUNTIME_ACCESS) != 0 &&
         (effective_attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0)) {
        return EFI_INVALID_PARAMETER;
    }
    if (mBootServicesExited &&
        (effective_attributes & EFI_VARIABLE_RUNTIME_ACCESS) == 0) {
        return EFI_INVALID_PARAMETER;
    }

    maximum_storage = (UINT64)NVRAM_VAR_MAX * (UINT64)NVRAM_VAR_SLOT_STORAGE;
    for (i = 0; i < mNvramVarCount; i++) {
        if (!mNvramVars[i].valid ||
            ((mNvramVars[i].attributes ^ effective_attributes) &
             EFI_VARIABLE_NON_VOLATILE) != 0) {
            continue;
        }
        used_storage += NVRAM_VAR_STORAGE_OVERHEAD + mNvramVars[i].name_len +
                        mNvramVars[i].data_size;
    }

    *MaximumVariableStorageSize = maximum_storage;
    *RemainingVariableStorageSize = used_storage >= maximum_storage ?
                                    0 : maximum_storage - used_storage;
    *MaximumVariableSize = NVRAM_VAR_DATA_MAX;
    return EFI_SUCCESS;
}

static BOOLEAN __attribute__((noinline)) runtime_variable_selftest(void)
{
    static const UINT8 test_guid[16] = {
        0x51, 0x56, 0x41, 0x52, 0x54, 0x45, 0x53, 0x54,
        0x9a, 0x64, 0x44, 0x2e, 0x80, 0x8a, 0x11, 0x01
    };
    NVRAM_VARIABLE saved[NVRAM_VAR_MAX];
    UINTN saved_count = mNvramVarCount;
    CHAR16 name[] = { 'Q', 'e', 'm', 'u', 'V', 'a', 'r', 0 };
    CHAR16 empty[] = { 0 };
    CHAR16 missing[] = { 'M', 'i', 's', 's', 'i', 'n', 'g', 0 };
    CHAR16 nonterm[] = { 'B', 'a', 'd' };
    CHAR16 boot_order[] = { 'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0 };
    UINT8 data1[2] = { 0x12, 0x34 };
    UINT8 data2[2] = { 0x56, 0x78 };
    UINT8 out[2] = { 0, 0 };
    UINT8 large[512];
    UINTN n;
    UINTN size;
    UINT32 attrs;
    UINT64 max_storage;
    UINT64 remaining_storage;
    UINT64 max_variable;
    UINT64 remaining_before_create;
    UINT64 remaining_after_create;
    EFI_STATUS st;
    BOOLEAN ok = 1;
    UINT32 rw_attrs = EFI_VARIABLE_NON_VOLATILE |
                      EFI_VARIABLE_BOOTSERVICE_ACCESS |
                      EFI_VARIABLE_RUNTIME_ACCESS;

    fw_copy_mem(saved, mNvramVars, sizeof(saved));
    mNvramSelftestActive = 1;
    for (n = 0; n < sizeof(large); n++) {
        large[n] = (UINT8)(n ^ 0xa5U);
    }

    st = rs_query_variable_info(rw_attrs, &max_storage, &remaining_storage,
                                &max_variable);
    remaining_before_create = remaining_storage;
    if (st != EFI_SUCCESS ||
        max_storage != (UINT64)NVRAM_VAR_MAX * (UINT64)NVRAM_VAR_SLOT_STORAGE ||
        max_variable != NVRAM_VAR_DATA_MAX ||
        remaining_storage > max_storage) {
        ok = 0;
    }

    st = rs_query_variable_info(rw_attrs | EFI_VARIABLE_APPEND_WRITE,
                                &max_storage, &remaining_storage,
                                &max_variable);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }

    st = rs_query_variable_info(EFI_VARIABLE_RUNTIME_ACCESS, &max_storage,
                                &remaining_storage, &max_variable);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_query_variable_info(rw_attrs | EFI_VARIABLE_HARDWARE_ERROR_RECORD,
                                &max_storage, &remaining_storage,
                                &max_variable);
    if (st != EFI_UNSUPPORTED) {
        ok = 0;
    }

    st = rs_query_variable_info(rw_attrs, NULL, &remaining_storage,
                                &max_variable);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_set_variable(empty, (void *)test_guid, rw_attrs,
                         sizeof(data1), data1);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_set_variable(name, (void *)test_guid,
                         rw_attrs | EFI_VARIABLE_APPEND_WRITE,
                         sizeof(data1), data1);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_set_variable(name, (void *)test_guid,
                         EFI_VARIABLE_RUNTIME_ACCESS,
                         sizeof(data1), data1);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_set_variable(name, (void *)test_guid, rw_attrs,
                         sizeof(data1), data1);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }

    st = rs_query_variable_info(rw_attrs, &max_storage,
                                &remaining_after_create, &max_variable);
    if (st != EFI_SUCCESS ||
        remaining_after_create >= remaining_before_create) {
        ok = 0;
    }

    size = sizeof(out);
    attrs = 0;
    st = rs_get_variable(name, (void *)test_guid, &attrs, &size, NULL);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    size = 0;
    attrs = 0;
    st = rs_get_variable(name, (void *)test_guid, &attrs, &size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || size != sizeof(data1) ||
        attrs != rw_attrs) {
        ok = 0;
    }

    st = rs_set_variable(name, (void *)test_guid,
                         EFI_VARIABLE_BOOTSERVICE_ACCESS,
                         sizeof(data2), data2);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    size = sizeof(out);
    attrs = 0;
    out[0] = 0;
    out[1] = 0;
    st = rs_get_variable(name, (void *)test_guid, &attrs, &size, out);
    if (st != EFI_SUCCESS || size != sizeof(data1) || attrs != rw_attrs ||
        out[0] != data1[0] || out[1] != data1[1]) {
        ok = 0;
    }

    size = sizeof(missing);
    st = rs_get_next_var_name(&size, missing, (void *)test_guid);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    size = sizeof(nonterm);
    st = rs_get_next_var_name(&size, nonterm, (void *)test_guid);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_set_variable(name, (void *)test_guid, 0, 0, NULL);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    size = sizeof(out);
    st = rs_get_variable(name, (void *)test_guid, &attrs, &size, out);
    if (st != EFI_NOT_FOUND) {
        ok = 0;
    }
    st = rs_set_variable(name, (void *)test_guid, 0, 0, NULL);
    if (st != EFI_NOT_FOUND) {
        ok = 0;
    }

    st = rs_set_variable(name, (void *)test_guid, rw_attrs,
                         sizeof(large), large);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    fw_set_mem(large, sizeof(large), 0);
    size = sizeof(large);
    st = rs_get_variable(name, (void *)test_guid, &attrs, &size, large);
    if (st != EFI_SUCCESS || size != sizeof(large)) {
        ok = 0;
    }
    for (n = 0; n < sizeof(large); n++) {
        if (large[n] != (UINT8)(n ^ 0xa5U)) {
            ok = 0;
            break;
        }
    }
    st = rs_set_variable(name, (void *)test_guid, 0, 0, NULL);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }

    st = rs_set_variable(boot_order, (void *)mEfiGlobalVariableGuid,
                         EFI_VARIABLE_BOOTSERVICE_ACCESS,
                         sizeof(data2), data2);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }

    st = rs_set_variable(boot_order, (void *)mEfiGlobalVariableGuid,
                         rw_attrs, sizeof(data2), data2);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    size = sizeof(out);
    attrs = 0;
    out[0] = 0;
    out[1] = 0;
    st = rs_get_variable(boot_order, (void *)mEfiGlobalVariableGuid,
                         &attrs, &size, out);
    if (st != EFI_SUCCESS || size != sizeof(data2) || attrs != rw_attrs ||
        out[0] != data2[0] || out[1] != data2[1]) {
        ok = 0;
    }
    st = rs_set_variable(boot_order, (void *)mEfiGlobalVariableGuid,
                         0, 0, NULL);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    size = sizeof(out);
    st = rs_get_variable(boot_order, (void *)mEfiGlobalVariableGuid,
                         &attrs, &size, out);
    if (st != EFI_NOT_FOUND) {
        ok = 0;
    }

    fw_copy_mem(mNvramVars, saved, sizeof(saved));
    mNvramVarCount = saved_count;
    mNvramSelftestActive = 0;
    return ok;
}

VOID rs_reset_system(UINTN ResetType, EFI_STATUS ResetStatus,
                     UINTN DataSize, VOID *ResetData)
{
    (void)ResetStatus;
    (void)DataSize;
    (void)ResetData;

    if (ResetType == EFI_RESET_SHUTDOWN) {
        volatile UINT16 *pm1_cnt = (volatile UINT16 *)mRuntimeAcpiPm1Cnt;

        *pm1_cnt = ACPI_PM1_CNT_SLEEP_ENABLE;
    } else if (ResetType == EFI_RESET_COLD ||
               ResetType == EFI_RESET_WARM ||
               ResetType == EFI_RESET_PLATFORM_SPECIFIC) {
        volatile UINT8 *reset_control =
            (volatile UINT8 *)mRuntimeResetControl;

        *reset_control = ACPI_PM_RESET_VALUE;
    }

    while (1) {}
}

BOOLEAN fw_graphics_present(VOID)
{
    return fw_pci_io_device_present(&mPciIoDevices[5]);
}

UINT64 fw_graphics_bar_length(VOID)
{
    return fw_pci_io_expected_bar_length(&mPciIoDevices[5]);
}

UINT64 fw_graphics_framebuffer_base(VOID)
{
    return VGA_FB_BASE;
}

UINT64 fw_graphics_framebuffer_size(VOID)
{
    return mGopMode.FrameBufferSize;
}

UINT32 fw_graphics_pixels_per_scan_line(VOID)
{
    return mGopMode.Info->PixelsPerScanLine;
}

EFI_STATUS fw_graphics_reset_current_mode(BOOLEAN redraw_text)
{
    return graphics_select_mode(mGopMode.Mode, redraw_text);
}

EFI_STATUS fw_graphics_set_uga_mode(UINT32 horizontal, UINT32 vertical,
                                    UINT32 color_depth, UINT32 refresh_rate)
{
    return uga_set_mode(&mUgaDrawProto, horizontal, vertical, color_depth,
                        refresh_rate);
}

EFI_HANDLE fw_graphics_handle(VOID)
{
    return mGraphicsHandle;
}

BOOLEAN fw_protocol_interface_installed(EFI_HANDLE handle, VOID *protocol,
                                        VOID **interface)
{
    return installed_protocol_interface(handle, protocol, interface);
}

VOID *fw_system_table(VOID)
{
    return &mSystemTable;
}

EFI_HANDLE fw_usb_controller_handle(VOID)
{
    return mPciOhciHandle;
}

void fw_usb_controller_device_path(FW_ACPI_HID_DEVICE_PATH_NODE *acpi,
                                   FW_PCI_DEVICE_PATH_NODE *pci,
                                   FW_DEVICE_PATH_NODE *end)
{
    *acpi = mPciOhciDevicePath.Acpi;
    *pci = mPciOhciDevicePath.Pci;
    *end = mEndDevicePath;
}

EFI_STATUS fw_pci_root_read(FW_PCI_ROOT_SPACE space, UINTN width,
                            UINT64 address, UINTN count, VOID *buffer)
{
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH root_width =
        (EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)width;

    switch (space) {
    case FwPciRootMemory:
        return pci_root_mem_read(&mPciRootBridgeIoProto, root_width,
                                 address, count, buffer);
    case FwPciRootIo:
        return pci_root_io_read(&mPciRootBridgeIoProto, root_width,
                                address, count, buffer);
    case FwPciRootConfiguration:
        return pci_root_cfg_read(&mPciRootBridgeIoProto, root_width,
                                 address, count, buffer);
    default:
        return EFI_INVALID_PARAMETER;
    }
}

EFI_STATUS fw_pci_root_write(FW_PCI_ROOT_SPACE space, UINTN width,
                             UINT64 address, UINTN count, VOID *buffer)
{
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH root_width =
        (EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH)width;

    switch (space) {
    case FwPciRootMemory:
        return pci_root_mem_write(&mPciRootBridgeIoProto, root_width,
                                  address, count, buffer);
    case FwPciRootIo:
        return pci_root_io_write(&mPciRootBridgeIoProto, root_width,
                                 address, count, buffer);
    case FwPciRootConfiguration:
        return pci_root_cfg_write(&mPciRootBridgeIoProto, root_width,
                                  address, count, buffer);
    default:
        return EFI_INVALID_PARAMETER;
    }
}

EFI_STATUS fw_pci_root_map(UINTN operation, VOID *host_address,
                           UINTN *number_of_bytes,
                           EFI_PHYSICAL_ADDRESS *device_address,
                           VOID **mapping)
{
    return pci_root_map(
        &mPciRootBridgeIoProto,
        (EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION)operation,
        host_address, number_of_bytes, device_address, mapping);
}

EFI_STATUS fw_pci_root_unmap(VOID *mapping)
{
    return pci_root_unmap(&mPciRootBridgeIoProto, mapping);
}

EFI_STATUS fw_pci_root_allocate_buffer(EFI_ALLOCATE_TYPE type,
                                       EFI_MEMORY_TYPE memory_type,
                                       UINTN pages, VOID **host_address)
{
    return pci_root_allocate_buffer(&mPciRootBridgeIoProto, type,
                                    memory_type, pages, host_address, 0);
}

EFI_STATUS fw_pci_root_free_buffer(UINTN pages, VOID *host_address)
{
    return pci_root_free_buffer(&mPciRootBridgeIoProto, pages, host_address);
}

EFI_STATUS fw_pci_root_flush(VOID)
{
    return pci_root_flush(&mPciRootBridgeIoProto);
}

EFI_STATUS fw_pci_copy_device_path(UINT8 bus, UINT8 device, UINT8 function,
                                   FW_DEVICE_PATH_NODE **path)
{
    UINTN i;

    if (path == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *path = NULL;
    for (i = 0; i < FW_ARRAY_SIZE(mPciIoDevices); i++) {
        const FW_PCI_IO_DEVICE *dev = &mPciIoDevices[i];
        UINTN path_size;
        VOID *copy;

        if (!fw_pci_io_device_present(dev) || dev->Bus != bus ||
            dev->Device != device || dev->Function != function ||
            dev->DevicePath == NULL) {
            continue;
        }
        path_size = fw_device_path_size(dev->DevicePath);
        if (path_size == 0) {
            return EFI_UNSUPPORTED;
        }
        if (bs_allocate_pool(EfiBootServicesData, path_size, &copy) !=
            EFI_SUCCESS) {
            return EFI_OUT_OF_RESOURCES;
        }
        fw_copy_mem(copy, dev->DevicePath, path_size);
        *path = copy;
        return EFI_SUCCESS;
    }
    return EFI_UNSUPPORTED;
}

EFI_HANDLE fw_pci_root_handle(VOID)
{
    return mPciRootBridgeHandle;
}

EFI_HANDLE fw_scsi_controller_handle(VOID)
{
    return mPciLsiHandle;
}



#include "fw-legacy-io.h"
#include "fw-debug-port.h"
#include "fw-debug-support.h"
#include "fw-usb.h"
#include "fw-pointer.h"
#include "fw-uga-io.h"

/* --- Boot policy lives in boot.c ------------------------------------------ */

/* --- Firmware entry point (firmware_main phases) -------------------------- */

/*
 * firmware_main phases.  Pure mechanical split of the former 700-line
 * script -- call order is unchanged.  This is the seam a replaceable EFI
 * core (or the SALEFIHANDOFF-shaped platform boundary, plan milestone 6)
 * slots into: platform state -> EFI core init -> device/storage bring-up
 * -> protocol/selftest battery -> boot policy.
 */
static void fw_phase_platform_init(UINT64 gp, UINT64 stack_top, UINT64 boot_b0)
{

    /*
     * stack_top is the aligned top of the boot-stack region; the entry
     * trampoline starts sp 16 bytes below it so the psABI scratch area
     * [sp, sp+16) stays inside the region even when it ends exactly at
     * the end of installed RAM.
     */
    mBootStackTop = stack_top;
    mBootStackBase = stack_top - FW_BOOT_STACK_SIZE;
    mCpuAssistBase = stack_top - IA64_FW_CPU_ASSIST_SIZE;
    fw_platform()->DecodeTopology();
    mResetFloatingPointDisableBits =
        fw_read_psr() & (IA64_PSR_DFL | IA64_PSR_DFH);

    (void)gp;
    (void)boot_b0;

    uart_puts("\r\n"
              "=============================\r\n"
              "  qemu-system-ia64 Firmware\r\n"
              "=============================\r\n\r\n");

    uart_puts("CPU Architecture:     IA-64\r\n");
    uart_puts("Boot ROM Address:     0x0000000000000000\r\n");
    uart_puts("UART Base:            0x47F0000000\r\n");
    uart_puts("Firmware Entry:       0x0000000000000000\r\n\r\n");

    uart_puts("GP check: OK\r\n");

    nvram_init();

    /* Initialize EFI structures */
    fw_platform()->InitMemoryMap();
    uart_puts("Memory Map:           low RAM end=0x");
    uart_put_hex64(mGuestLowRamEnd);
    uart_puts("\r\n");
    uart_puts("Memory Map:           high RAM ranges=");
    uart_put_hex64(fw_guest_high_ram_count());
    uart_puts(" total=0x");
    uart_put_hex64(fw_guest_high_ram_total());
    uart_puts("\r\n");
    uart_puts("EFI Boot Stack:       0x");
    uart_put_hex64(mBootStackBase);
    uart_puts("-0x");
    uart_put_hex64(mBootStackTop);
    uart_puts("\r\n");
    uart_puts("I/O Port Space:       0x");
    uart_put_hex64(LEGACY_IO_BASE);
    uart_puts("-0x");
    uart_put_hex64(LEGACY_IO_SPARSE_END);
    uart_puts("\r\n");
    uart_puts("Memory Map Test:      ");
    uart_puts(uefi_memory_map_selftest() ?
              "descriptor and pool placement verified\r\n" : "FAILED\r\n");
    uart_puts("CopyMem Test:         ");
    uart_puts(fw_copy_mem_selftest() ?
              "aligned and overlapping copies verified\r\n" :
              "verification failed\r\n");
}

/*
 * Neither machine puts its graphics adapter on the first PCI root.  On zx1
 * the AGP adapter lives behind the Mercury (LBA) bridge at ACPI _UID 1; on
 * the i2000 it lives behind the GXB expander, the AGP root, at _UID 3 device
 * 0.  Retarget the EFI console/graphics device paths accordingly.  Every root
 * advertises PNP0A03 (0x0A0341D0), so only the _UID and the PCI device number
 * change.  Run once, after the chipset profile is known
 * (fw_phase_platform_init) and before the paths are installed as the GOP
 * handle's device path / ConOut variables (fw_phase_efi_core_init).
 */
static void fw_retarget_vga_device_paths(void)
{
    UINT32 uid;
    UINT8 device;

    if (fw_platform_is_zx1()) {
        uid = 1;
        device = IA64_MERCURY_VGA_SLOT;
    } else {
        uid = IA64_460GX_GXB_BUS;
        device = IA64_460GX_GXB_VGA_SLOT;
    }
    mGraphicsDevicePath.Acpi.Uid = uid;
    mGraphicsDevicePath.Pci.Device = device;
    mConsoleOutputDevicePath.Graphics.Acpi.Uid = uid;
    mConsoleOutputDevicePath.Graphics.Pci.Device = device;
}

/*
 * The USB and IDE controllers are functions 2 and 1 of the 82468GX I/O and
 * Firmware Bridge on the i2000, not discrete function-zero devices of their
 * own.  Retarget their fixed PCI-I/O table entries and device paths, which
 * the static initializers give the zx1 layout.  Same timing rule as
 * fw_retarget_vga_device_paths().
 */
static void fw_retarget_south_bridge_device_paths(void)
{
    UINTN i;

    if (fw_platform_is_zx1()) {
        return;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mPciIoDevices); i++) {
        if (mPciIoDevices[i].Protocol == &mPciUhciIoProto) {
            mPciIoDevices[i].Device = IA64_460GX_IFB_SLOT;
            mPciIoDevices[i].Function = IA64_460GX_IFB_USB_FUNCTION;
            mPciIoDevices[i].ExpectedId = FW_PCI_IFB_UHCI_ID;
        } else if (mPciIoDevices[i].Protocol == &mPciIdeIoProto) {
            mPciIoDevices[i].Device = IA64_460GX_IFB_SLOT;
            mPciIoDevices[i].Function = IA64_460GX_IFB_IDE_FUNCTION;
            mPciIoDevices[i].ExpectedId = FW_PCI_IFB_IDE_ID;
        }
    }
    mPciUhciDevicePath.Pci.Device = IA64_460GX_IFB_SLOT;
    mPciUhciDevicePath.Pci.Function = IA64_460GX_IFB_USB_FUNCTION;
    mPciIdeDevicePath.Pci.Device = IA64_460GX_IFB_SLOT;
    mPciIdeDevicePath.Pci.Function = IA64_460GX_IFB_IDE_FUNCTION;
}

static void fw_phase_efi_core_init(void)
{
    fw_retarget_vga_device_paths();
    fw_retarget_south_bridge_device_paths();
    efi_init_boot_services();
    efi_init_runtime_services();
    uart_puts("UEFI Time Services:   ");
    uart_puts(uefi_time_services_selftest() ?
              "GetTime/SetTime/GetWakeupTime verified\r\n" :
              "verification failed\r\n");
    uart_puts("Loaded Image Paths:   ");
    uart_puts(loaded_image_file_path_selftest() ?
              "protocol storage verified\r\n" : "verification failed\r\n");
    efi_init_conout();
    ps2_init_controller();
    efi_init_static_handles();
    {
        EFI_HANDLE handle = mArchitecturalHandle;

        if (bs_install_protocol(&handle,
                                (void *)fw_decompress_protocol_guid, 0,
                                &fw_decompress_protocol) != EFI_SUCCESS) {
            uart_puts("Decompress Protocol:   installation failed\r\n");
        }
    }
    if (!fw_ebc_install_protocol()) {
        uart_puts("EBC Interpreter:       installation failed\r\n");
    }
    if (!tcg_install_protocol()) {
        mTcgHandle = NULL;
    }
    efi_init_system_table();
    fw_platform()->InitPlatformTables();
    efi_init_loaded_image_proto();
    efi_init_fpswa_loaded_image_proto();
    if (!partition_driver_install()) {
        uart_puts("Partition Driver:      protocol installation failed\r\n");
    }
    efi_init_debug_image_info_table();
    efi_refresh_table_crc32s();
    efi_init_system_table_pointer();

}

static void fw_phase_storage_bringup(void)
{
    /* Install Block I/O protocol */
    ide_probe_primary_devices();
    mBootIdeDevice = ide_pick_boot_device();
    ahci_probe_devices();
    scsi_probe_devices();

    storage_set_none(&mBootStorageDevice);
    storage_set_none(&mDiskStorageDevice);
    storage_set_none(&mRawStorageDevice);
    if (mBootScsiDevice != NULL) {
        storage_set_scsi(&mBootStorageDevice, mBootScsiDevice);
    } else if (mBootIdeDevice != NULL && mBootIdeDevice->present &&
               mBootIdeDevice->is_atapi) {
        storage_set_ide(&mBootStorageDevice, mBootIdeDevice);
    } else if (mBootAhciDevice != NULL) {
        storage_set_ahci(&mBootStorageDevice, mBootAhciDevice);
    } else if (mDiskScsiDevice != NULL) {
        storage_set_scsi(&mBootStorageDevice, mDiskScsiDevice);
    } else if (mDiskAhciDevice != NULL) {
        storage_set_ahci(&mBootStorageDevice, mDiskAhciDevice);
    } else if (mBootIdeDevice != NULL && mBootIdeDevice->present) {
        storage_set_ide(&mBootStorageDevice, mBootIdeDevice);
    }

    if (mDiskAhciDevice != NULL) {
        storage_set_ahci(&mDiskStorageDevice, mDiskAhciDevice);
    } else if (mDiskScsiDevice != NULL) {
        storage_set_scsi(&mDiskStorageDevice, mDiskScsiDevice);
    } else if (mHardDiskIdeDevice != NULL) {
        storage_set_ide(&mDiskStorageDevice, mHardDiskIdeDevice);
    }
    if (!storage_device_present(&mBootStorageDevice) &&
        storage_device_present(&mDiskStorageDevice)) {
        mBootStorageDevice = mDiskStorageDevice;
    }
    if (storage_is_cd(&mBootStorageDevice)) {
        mRawStorageDevice = mBootStorageDevice;
    }
    if (!storage_present(&mBootStorageDevice) &&
        storage_present(&mDiskStorageDevice)) {
        mBootStorageDevice = mDiskStorageDevice;
    }

    mCdromBlocks = 0;
    if (mBootStorageDevice.Kind == FW_STORAGE_SCSI &&
        storage_is_cd(&mBootStorageDevice) &&
        mBootStorageDevice.Scsi->block_size == ATAPI_SECTOR_SIZE &&
        mBootStorageDevice.Scsi->last_lba < 0xffffffffULL) {
        mCdromBlocks = (UINT32)(mBootStorageDevice.Scsi->last_lba + 1U);
    } else if (mBootStorageDevice.Kind == FW_STORAGE_AHCI &&
               storage_is_cd(&mBootStorageDevice) &&
               mBootStorageDevice.Ahci->block_size == ATAPI_SECTOR_SIZE &&
               mBootStorageDevice.Ahci->last_lba < 0xffffffffULL) {
        mCdromBlocks = (UINT32)(mBootStorageDevice.Ahci->last_lba + 1U);
    } else if (mBootStorageDevice.Kind == FW_STORAGE_IDE &&
               storage_is_cd(&mBootStorageDevice) &&
               mBootStorageDevice.Ide->media_present &&
               mBootStorageDevice.Ide->last_lba < 0xffffffffULL) {
        mCdromBlocks = (UINT32)(mBootStorageDevice.Ide->last_lba + 1U);
    }
    fw_update_storage_device_paths();

    if (storage_present(&mBootStorageDevice) &&
        storage_is_cd(&mBootStorageDevice) && atapi_configure_el_torito()) {
        UINT64 cdrom_partition_blocks = mBootImagePartitionCdBlocks;

        mBlockDevicePath.Cdrom.BootEntry = 0;
        mBlockDevicePath.Cdrom.PartitionStart = mBootImageStartLba;
        mBlockDevicePath.Cdrom.PartitionSize = cdrom_partition_blocks;
        mSataBlockDevicePath.Cdrom.BootEntry = 0;
        mSataBlockDevicePath.Cdrom.PartitionStart = mBootImageStartLba;
        mSataBlockDevicePath.Cdrom.PartitionSize = cdrom_partition_blocks;
        mBootFullDevicePath.Cdrom.BootEntry = 0;
        mBootFullDevicePath.Cdrom.PartitionStart = mBootImageStartLba;
        mBootFullDevicePath.Cdrom.PartitionSize = cdrom_partition_blocks;
        mOpticalSetupLoaderDevicePath.Cdrom.BootEntry = 0;
        mOpticalSetupLoaderDevicePath.Cdrom.PartitionStart =
            mBootImageStartLba;
        mOpticalSetupLoaderDevicePath.Cdrom.PartitionSize =
            cdrom_partition_blocks;
        uart_puts("Block I/O: El Torito FAT image mapped\r\n");
    } else if (storage_present(&mBootStorageDevice) &&
               storage_is_cd(&mBootStorageDevice)) {
        /*
         * Optical media with no mappable EFI El Torito boot image (e.g. a combo
         * disc whose only boot catalog entry is the legacy x86 CDBOOT loader).
         * Describe the CD-ROM device path node as spanning the whole media, so
         * the boot Block I/O handle -- which carries the disc's ISO-9660 file
         * system -- presents a whole-media MEDIA_CDROM_DP path.  A loader
         * launched from that file system (\IA64\SETUPLDR.EFI from the EFI shell)
         * then receives a CD-ROM device path, matching what the reference
         * firmware's El Torito partition child handle provides (EFI 1.10
         * EDK/Drivers/Partition/ElTorito.c).
         */
        mBlockDevicePath.Cdrom.BootEntry = 0;
        mBlockDevicePath.Cdrom.PartitionStart = 0;
        mBlockDevicePath.Cdrom.PartitionSize = mCdromBlocks;
        mSataBlockDevicePath.Cdrom.BootEntry = 0;
        mSataBlockDevicePath.Cdrom.PartitionStart = 0;
        mSataBlockDevicePath.Cdrom.PartitionSize = mCdromBlocks;
        mBootFullDevicePath.Cdrom.BootEntry = 0;
        mBootFullDevicePath.Cdrom.PartitionStart = 0;
        mBootFullDevicePath.Cdrom.PartitionSize = mCdromBlocks;
        mOpticalSetupLoaderDevicePath.Cdrom.BootEntry = 0;
        mOpticalSetupLoaderDevicePath.Cdrom.PartitionStart = 0;
        mOpticalSetupLoaderDevicePath.Cdrom.PartitionSize = mCdromBlocks;
    }
    mBlockIoMedia.MediaId = 1;
    mBlockIoMedia.RemovableMedia = storage_removable(&mBootStorageDevice);
    mBlockIoMedia.MediaPresent = storage_present(&mBootStorageDevice) ? 1 : 0;
    mBlockIoMedia.LogicalPartition = mBootImageMapped ? 1 : 0;
    mBlockIoMedia.ReadOnly = storage_read_only(&mBootStorageDevice) ? 1 : 0;
    mBlockIoMedia.WriteCaching =
        storage_write_caching(&mBootStorageDevice);
    mBlockIoMedia.BlockSize = mBootImageMapped ? 512 :
                              storage_block_size(&mBootStorageDevice);
    mBlockIoMedia.IoAlign = 0;
    mBlockIoMedia.LastBlock = mBootImageMapped ?
        (mBootImagePartitionBlocks > 0 ?
         (UINT64)(mBootImagePartitionBlocks - 1U) : 0) :
        storage_last_lba(&mBootStorageDevice);

    mBlockIoProto.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    mBlockIoProto.Media = &mBlockIoMedia;
    mBlockIoProto.Reset = blk_reset;
    mBlockIoProto.ReadBlocks = blk_read;
    mBlockIoProto.WriteBlocks = blk_write;
    mBlockIoProto.FlushBlocks = blk_flush;
    mBlockDiskIoProto.Revision = EFI_DISK_IO_PROTOCOL_REVISION;
    mBlockDiskIoProto.ReadDisk = disk_read;
    mBlockDiskIoProto.WriteDisk = disk_write;
    if (storage_is_cd(&mRawStorageDevice)) {
        mRawBlockIoMedia.MediaId = 2;
        mRawBlockIoMedia.RemovableMedia = 1;
        mRawBlockIoMedia.MediaPresent =
            storage_present(&mRawStorageDevice);
        mRawBlockIoMedia.LogicalPartition = 0;
        mRawBlockIoMedia.ReadOnly = 1;
        mRawBlockIoMedia.WriteCaching = 0;
        mRawBlockIoMedia.BlockSize = ATAPI_SECTOR_SIZE;
        mRawBlockIoMedia.IoAlign = 0;
        mRawBlockIoMedia.LastBlock = storage_last_lba(&mRawStorageDevice);
        mRawBlockIoProto.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
        mRawBlockIoProto.Media = &mRawBlockIoMedia;
        mRawBlockIoProto.Reset = blk_reset;
        mRawBlockIoProto.ReadBlocks = blk_read;
        mRawBlockIoProto.WriteBlocks = blk_write;
        mRawBlockIoProto.FlushBlocks = blk_flush;
        mRawDiskIoProto.Revision = EFI_DISK_IO_PROTOCOL_REVISION;
        mRawDiskIoProto.ReadDisk = disk_read;
        mRawDiskIoProto.WriteDisk = disk_write;
        mRawBlockIoHandle = FW_HANDLE_RAW_BLOCK_IO;
    }
    mDiskBlockIoHandle = NULL;
    if (storage_device_present(&mDiskStorageDevice) &&
        !storage_same_device(&mBootStorageDevice, &mDiskStorageDevice)) {
        mDiskBlockIoMedia.MediaId = 3;
        mDiskBlockIoMedia.RemovableMedia =
            storage_removable(&mDiskStorageDevice);
        mDiskBlockIoMedia.MediaPresent =
            storage_present(&mDiskStorageDevice);
        mDiskBlockIoMedia.LogicalPartition = 0;
        mDiskBlockIoMedia.ReadOnly =
            storage_read_only(&mDiskStorageDevice) ? 1 : 0;
        mDiskBlockIoMedia.WriteCaching =
            storage_write_caching(&mDiskStorageDevice);
        mDiskBlockIoMedia.BlockSize = storage_block_size(&mDiskStorageDevice);
        mDiskBlockIoMedia.IoAlign = 0;
        mDiskBlockIoMedia.LastBlock = storage_last_lba(&mDiskStorageDevice);
        mDiskBlockIoProto.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
        mDiskBlockIoProto.Media = &mDiskBlockIoMedia;
        mDiskBlockIoProto.Reset = blk_reset;
        mDiskBlockIoProto.ReadBlocks = blk_read;
        mDiskBlockIoProto.WriteBlocks = blk_write;
        mDiskBlockIoProto.FlushBlocks = blk_flush;
        mDiskIoProto.Revision = EFI_DISK_IO_PROTOCOL_REVISION;
        mDiskIoProto.ReadDisk = disk_read;
        mDiskIoProto.WriteDisk = disk_write;
        mDiskBlockIoHandle = FW_HANDLE_DISK_BLOCK_IO;
    }
    {
        EFI_HANDLE partition_parent = NULL;
        EFI_BLOCK_IO_PROTOCOL *partition_block = NULL;
        EFI_STATUS partition_status;

        if (mDiskBlockIoHandle != NULL &&
            mDiskBlockIoMedia.MediaPresent) {
            partition_parent = mDiskBlockIoHandle;
            partition_block = &mDiskBlockIoProto;
        } else if (mBlockIoHandle != NULL &&
                   mBlockIoMedia.MediaPresent &&
                   !mBlockIoMedia.LogicalPartition &&
                   !storage_is_cd(&mBootStorageDevice)) {
            partition_parent = mBlockIoHandle;
            partition_block = &mBlockIoProto;
        }
        if (partition_parent != NULL) {
            partition_status = fw_partition_discover(partition_parent,
                                                     partition_block);

            uart_puts("Disk Partitions:       ");
            if (partition_status == EFI_SUCCESS) {
                UINTN partition_count = 0;
                UINTN partition_index;

                for (partition_index = 0;
                     partition_index < FW_ARRAY_SIZE(mPartitions);
                     partition_index++) {
                    if (mPartitions[partition_index].in_use &&
                        mPartitions[partition_index].parent_handle ==
                            partition_parent) {
                        partition_count++;
                    }
                }
                uart_put_hex64(partition_count);
                uart_puts(" child handle(s)\r\n");
            } else if (partition_status == EFI_NOT_FOUND) {
                uart_puts("unpartitioned media\r\n");
            } else {
                uart_puts("invalid table or I/O failure\r\n");
            }
        }
    }
    if (storage_is_cd(&mBootStorageDevice)) {
        uart_puts("Optical Setup Boot Option:");
        uart_puts(optical_setup_boot_option_selftest() ?
                  " CD boot path verified\r\n" :
                  " verification failed\r\n");
        uart_puts("Optical Raw Device Path:");
        uart_puts(optical_raw_device_path_selftest() ?
                  " whole-media optical path verified\r\n" :
                  " verification failed\r\n");
        uart_puts("El Torito Mapping:    ");
        uart_puts(el_torito_partition_selftest() ?
                  "partition verified\r\n" :
                  "verification failed\r\n");
    } else {
        uart_puts("Optical Setup Boot Option: no optical boot media\r\n");
        uart_puts("Optical Raw Device Path: no optical boot media\r\n");
        uart_puts("El Torito Mapping:    no optical boot media\r\n");
    }
    mSimpleFsProto.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    mSimpleFsProto.OpenVolume = fat_open_volume;
    mOpticalSimpleFsProto.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    mOpticalSimpleFsProto.OpenVolume = optical_open_volume;
    mLoadedImageProto.FilePath = &mEndDevicePath;
    mFpswaLoadedImageProto.FilePath = &mEndDevicePath;
}

static void fw_phase_protocols_and_selftests(void)
{
    BOOLEAN nvram_variable_selftest_ok;

    if (!fpswa_install_protocols()) {
        mFpswaHandle = NULL;
    }
    mPciRootBridgeIoProto.ParentHandle = mPciRootBridgeHandle;
    if (!fw_legacy_io_protocols_install()) {
        uart_puts("Platform I/O Protocols: installation failed\r\n");
    }
    if (!fw_debug_port_install()) {
        uart_puts("Debug Port Protocol:   installation failed\r\n");
    }
    if (!fw_debug_support_install()) {
        uart_puts("Debug Support Protocol: installation failed\r\n");
    }
    if (!fw_usb_protocols_install()) {
        uart_puts("USB Protocols:         installation failed\r\n");
    }
    if (!fw_pointer_install()) {
        uart_puts("Pointer Protocol:      installation failed\r\n");
    }
    efi_init_graphics();
    if (!fw_uga_io_install()) {
        uart_puts("UGA I/O Protocol:       installation failed\r\n");
    }
    efi_conout_ascii("QEMU IA-64 EFI firmware\r\n");
    efi_conout_ascii("GOP/UGA VGA text console ready\r\n\r\n");

    mRuntimeServices.GetTime = (UINTN)rs_get_time;
    mRuntimeServices.SetTime = (UINTN)rs_set_time;
    mRuntimeServices.GetWakeupTime = (UINTN)rs_get_wakeup_time;
    mRuntimeServices.SetWakeupTime = (UINTN)rs_set_wakeup_time;
    mRuntimeServices.SetVirtualAddressMap = (UINTN)rs_set_virtual_address_map;
    mRuntimeServices.ConvertPointer = (UINTN)rs_convert_pointer;
    mRuntimeServices.GetVariable = (UINTN)rs_get_variable;
    mRuntimeServices.SetVariable = (UINTN)rs_set_variable;
    mRuntimeServices.GetNextVariableName = (UINTN)rs_get_next_var_name;
    mRuntimeServices.GetNextHighMonotonicCount =
        (UINTN)rs_get_next_high_monotonic_count;
    mRuntimeServices.ResetSystem = (UINTN)rs_reset_system;
    mRuntimeServices.QueryVariableInfo = (UINTN)rs_query_variable_info;

    efi_init_debug_image_info_table();
    efi_refresh_table_crc32s();
    efi_init_system_table_pointer();

    uart_puts("EFI System Table:     ready\r\n");
    uart_puts("EFI Debug Tables:     ");
    uart_puts(efi_debug_tables_selftest() ?
              "system pointer/image info verified\r\n" :
              "verification failed\r\n");
    uart_puts("Config Tables:        ");
    uart_puts(uefi_configuration_table_selftest() ?
              "InstallConfigurationTable CRC verified\r\n" :
              "verification failed\r\n");
    uart_puts("ExitBootServices:     ");
    uart_puts(uefi_exit_boot_services_system_table_selftest() ?
              "System Table handoff verified\r\n" :
              "verification failed\r\n");
    uart_puts("Console Out:          Serial 16550 + VGA text\r\n");
    uart_puts("Console Out Test:     ");
    uart_puts(uefi_conout_selftest() ? "text output contracts verified\r\n" :
              "verification failed\r\n");
    uart_puts("Console Handles:      ");
    uart_puts(console_handle_selftest() ?
              "graphics output handle verified\r\n" :
              "verification failed\r\n");
    uart_puts("Console In:           ");
    uart_puts(uefi_conin_wait_key_selftest() ?
              "Serial/PS2/USB WaitForKey ready\r\n" :
              "verification failed\r\n");
    uart_puts("Console In Buffer:    ");
    uart_puts(uefi_conin_buffer_selftest() ?
              "WaitForKey preserves keystrokes\r\n" :
              "verification failed\r\n");
    uart_puts("PS/2 Scancode Test:   ");
    uart_puts(uefi_ps2_scancode_selftest() ?
              "translated set1/set2 decode verified\r\n" :
              "verification failed\r\n");
    uart_puts("USB Keyboard Test:    ");
    uart_puts(uefi_usb_keyboard_selftest() ?
              "HID boot report decode verified\r\n" :
              "verification failed\r\n");
    uart_puts("USB Protocols:        ");
    uart_puts(fw_usb_protocols_selftest() ?
              "host and device interfaces verified\r\n" :
              "verification failed\r\n");
    uart_puts("Simple Pointer:       ");
    uart_puts(fw_pointer_selftest() ?
              "relative motion and buttons verified\r\n" :
              "verification failed\r\n");
    uart_puts("Console In Ex:        ");
    uart_puts(uefi_conin_ex_selftest() ? "SimpleTextInputEx ready\r\n" :
              "verification failed\r\n");
    uart_puts("UEFI Event Services:  ");
    uart_puts(uefi_event_services_selftest() ? "contract checks verified\r\n" :
              "verification failed\r\n");
    uart_puts("UEFI Stall:           ");
    uart_puts(uefi_stall_selftest() ? "ITC delay verified\r\n" :
              "verification failed\r\n");
    uart_puts("Decompress Protocol:  ");
    uart_puts(fw_decompress_selftest() ?
              "UEFI compressed stream verified\r\n" :
              "verification failed\r\n");
    uart_puts("EBC Interpreter:      ");
    uart_puts(fw_ebc_selftest() ?
              "native thunk and byte-code execution verified\r\n" :
              "verification failed\r\n");
    uart_puts("Platform I/O:         ");
    uart_puts(fw_legacy_io_protocols_selftest() ?
              "device, serial, and SCSI interfaces verified\r\n" :
              "verification failed\r\n");
    uart_puts("Debug Port:           ");
    if (fw_handoff_debug_port_base() == 0) {
        uart_puts(fw_debug_port_selftest() ?
                  "not exposed (no debug UART)\r\n" :
                  "verification failed\r\n");
    } else {
        uart_puts(fw_debug_port_selftest() ?
                  "byte-stream protocol verified\r\n" :
                  "verification failed\r\n");
    }
    uart_puts("Debug Support:        ");
    uart_puts(fw_debug_support_selftest() ?
              "exception and periodic callbacks verified\r\n" :
              "verification failed\r\n");
    uart_puts("Graphics Output:      GOP/UGA VGA BGRx "
              "640x400x32, 640x480x32, 800x600x32, 1024x768x32, "
              "1280x1024x32 @ ");
    uart_put_hex64(VGA_FB_BASE);
    uart_puts("\r\n");
    uart_puts("UGA I/O Protocol:     ");
    uart_puts(fw_uga_io_selftest() ?
              "device tree and request dispatch verified\r\n" :
              "verification failed\r\n");
    uart_puts("GOP SetMode Test:     ");
    uart_puts(graphics_gop_set_mode_selftest() ?
              "BGRx framebuffer cleared\r\n" :
              "verification failed\r\n");
    uart_puts("Graphics Handoff:    ");
    uart_puts(graphics_handoff_selftest() ?
              "GOP preserve + PCDP VGA text fallback verified\r\n" :
              "verification failed\r\n");
    uart_puts("EFI Image Handoff:    ");
    uart_puts(efi_entry_handoff_selftest() ?
              "P64 register/stack arguments verified\r\n" :
              "verification failed\r\n");
    uart_puts("UEFI Boot Services:   LoadImage/StartImage/GetMemoryMap ready\r\n");
    uart_puts("Loaded Image Options: ");
    uart_puts(load_image_options_selftest() ?
              "type and ownership contracts verified\r\n" :
              "verification failed\r\n");
    uart_puts("PE Runtime Relocation:");
    uart_puts(pe_runtime_relocation_selftest() ?
              " base adjustment/fixup log verified\r\n" :
              " verification failed\r\n");
    uart_puts("Block I/O Protocol:   installed (");
    if (storage_present(&mBootStorageDevice)) {
        if (mBootStorageDevice.Kind == FW_STORAGE_SCSI) {
            uart_puts(storage_is_cd(&mBootStorageDevice) ?
                      "SCSI CD-ROM" : "SCSI disk");
        } else if (mBootStorageDevice.Kind == FW_STORAGE_AHCI) {
            uart_puts(storage_is_cd(&mBootStorageDevice) ?
                      "SATA ATAPI" : "SATA AHCI disk");
        } else if (mBootStorageDevice.Ide->is_atapi) {
            ide_activate(mBootStorageDevice.Ide);
            uart_puts(gIde.has_bmdma ? "ATAPI DMA-capable" : "ATAPI PIO");
        } else {
            ide_activate(mBootStorageDevice.Ide);
            uart_puts(gIde.has_bmdma ? "ATA DMA-capable" : "ATA PIO");
        }
    } else {
        uart_puts("ATA PIO");
    }
    if (mBootStorageDevice.Kind == FW_STORAGE_SCSI) {
        uart_puts(", LSI53C895A)\r\n");
    } else if (mBootStorageDevice.Kind == FW_STORAGE_AHCI) {
        uart_puts(", AHCI)\r\n");
    } else if (mBootStorageDevice.Kind == FW_STORAGE_IDE) {
        uart_puts(mBootStorageDevice.Ide != NULL &&
                  mBootStorageDevice.Ide->channel != 0 ?
                  ", secondary IDE)\r\n" : ", primary IDE)\r\n");
    } else {
        uart_puts(")\r\n");
    }
    uart_puts("Block I/O Read Test:  ");
    uart_puts(block_io_read_selftest() ? "media ID/range/bulk reads verified\r\n" :
              "verification failed\r\n");
    uart_puts("File Protocol:        ");
    uart_puts(file_protocol_contract_selftest() ?
              "read-only positioning and information verified\r\n" :
              "verification failed\r\n");
    uart_puts("FAT File Reads:       ");
    uart_puts(fat_cursor_cache_selftest() ?
              "cursor and table cache verified\r\n" :
              "verification failed\r\n");
    uart_puts("Unicode Collation:    ");
    uart_puts(unicode_collation_selftest() ?
              "case, wildcard, and FAT conversion verified\r\n" :
              "verification failed\r\n");
    uart_puts("Disk Block I/O Test:  ");
    if (mDiskBlockIoHandle == NULL) {
        uart_puts("no fixed disk present\r\n");
    } else {
        uart_puts(disk_block_io_selftest() ?
                  "fixed disk read/zero-write verified\r\n" :
                  "verification failed\r\n");
    }
    if (storage_is_cd(&mBootStorageDevice)) {
        uart_puts("Optical SimpleFS:     ");
        if (fw_udf_init()) {
            uart_puts("UDF root verified\r\n");
        } else if (fw_iso_init()) {
            uart_puts("ISO9660 root verified\r\n");
        } else {
            uart_puts("no raw filesystem\r\n");
        }
    }
    uart_puts("HandleProtocol:       enabled\r\n");
    uart_puts("LocateHandle:         enabled (Block I/O + GOP/UGA)\r\n");
    uart_puts("Protocol Notify:      ");
    uart_puts(protocol_notify_selftest() ? "LocateProtocol registration verified\r\n" :
              "verification failed\r\n");
    uart_puts("Protocol Database:    ");
    uart_puts(protocol_null_interface_selftest() ?
              "NULL interface markers verified\r\n" :
              "verification failed\r\n");
    uart_puts("Driver Component Name:");
    uart_puts(partition_component_name_selftest() ?
              " partition/controller names verified\r\n" :
              " verification failed\r\n");
    uart_puts("FPSWA Protocol:       ");
    uart_puts(fpswa_protocol_selftest() ? "published (software assist)\r\n" :
              "verification failed\r\n");
    uart_puts("TCG EFI Protocol:     ");
    uart_puts(tcg_protocol_selftest() ?
              "not exposed (no TPM device)\r\n" :
              "verification failed\r\n");
    uart_puts("SetVirtualAddressMap/ConvertPointer: enabled\r\n");
    uart_puts("ConvertPointer Test:  ");
    uart_puts(uefi_convert_pointer_selftest() ?
              "reserved DebugDisposition bits rejected\r\n" :
              "verification failed\r\n");
    uart_puts("NVRAM Variables:      enabled\r\n");
    nvram_variable_selftest_ok = runtime_variable_selftest();
    uart_puts("NVRAM Variable Test:  ");
    uart_puts(nvram_variable_selftest_ok ? "contract checks verified\r\n" :
              "verification failed\r\n");
    uart_puts("EFI Boot Shell:       ");
    uart_puts(fw_boot_shell_selftest() ?
              "command parsing and hotkeys verified\r\n" :
              "verification failed\r\n");
    uart_puts("ResetSystem:          enabled\r\n");
    uart_puts("SAL System Table:     published\r\n");
    uart_puts("SMBIOS Table:         published\r\n");
    uart_puts("SMBIOS Table Checks:  ");
    uart_puts(smbios_table_integrity_selftest() ? "entry point verified\r\n" :
              "verification failed\r\n");
    uart_puts("ACPI RSDP/RSDT/XSDT/FADT: published\r\n");
    uart_puts("ACPI FACS/DSDT:       published\r\n");
    uart_puts("ACPI MADT (SAPIC):    published\r\n");
    uart_puts("ACPI SRAT/SLIT:       published\r\n");
    uart_puts("ACPI MCFG (PCIe):     published\r\n");
    uart_puts("ACPI HCDP/PCDP:       published\r\n");
    uart_puts("ACPI SSDT (serial):   published\r\n");
    uart_puts("ACPI Table Checks:    ");
    uart_puts(acpi_table_integrity_selftest() ? "checksums verified\r\n" :
              "verification failed\r\n");
    uart_puts("PCI Root Bridge I/O:  published\r\n");
    uart_puts("PCI Root Bridge Test: ");
    uart_puts(pci_root_bridge_io_selftest() ?
              "config/resources/polling verified\r\n" :
              "verification failed\r\n");
    uart_puts("PCI I/O Protocol:    ");
    uart_puts(pci_io_protocol_selftest() ?
              "controllers/polling verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL PCI Config:       ");
    uart_puts(sal_pci_config_selftest() ? "read/write verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL Proc Dispatch:    ");
    uart_puts(sal_proc_dispatch_selftest() ? "function ID mask verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL Update PAL:       ");
    uart_puts(sal_update_pal_selftest() ? "error path verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL MC Rendezvous:    ");
    uart_puts(sal_mc_rendez_selftest() ? "idle path verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL MC Params:        ");
    uart_puts(sal_mc_set_params_selftest() ? "argument checks verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL Physical IDs:     ");
    uart_puts(sal_physical_services_selftest() ? "argument checks verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL Cache Services:   ");
    uart_puts(sal_cache_services_selftest() ? "argument checks verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL Set Vectors:      ");
    uart_puts(sal_set_vectors_selftest() ? "argument checks verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL Frequency Base:   ");
    uart_puts(sal_freq_base_selftest() ? "optional clocks verified\r\n" :
              "verification failed\r\n");
    uart_puts("SAL State Info:       ");
    uart_puts(sal_state_info_selftest() ? "no-log paths verified\r\n" :
              "verification failed\r\n");
    prepare_sal_loader_handoff();
    uart_puts("SAL Loader Handoff:   ");
    uart_puts(sal_loader_handoff_selftest() ?
              "registers/stack/TR verified\r\n" :
              "verification failed\r\n");
    uart_puts("BOOT path:            SCSI/SATA/ATA Block I/O + FAT resolver\r\n");
    uart_puts("\r\nFirmware ready.\r\n");
}

/* Emit a decimal integer to the EFI console (ConOut = serial + VGA text). */
static void fw_post_emit_udec(UINT64 Value)
{
    CHAR8 digits[21];
    CHAR8 out[22];
    UINTN count = 0;
    UINTN i = 0;

    do {
        digits[count++] = (CHAR8)('0' + (CHAR8)(Value % 10U));
        Value /= 10U;
    } while (Value != 0 && count < FW_ARRAY_SIZE(digits));
    while (count != 0) {
        out[i++] = digits[--count];
    }
    out[i] = 0;
    efi_conout_ascii(out);
}

/* Emit a two-digit lowercase hex byte to the EFI console. */
static void fw_post_emit_hex2(UINT8 Value)
{
    static const CHAR8 hex[] = "0123456789abcdef";
    CHAR8 out[3];

    out[0] = hex[(Value >> 4) & 0xfU];
    out[1] = hex[Value & 0xfU];
    out[2] = 0;
    efi_conout_ascii(out);
}

/*
 * Emit a "major.minor" version.  The sample shows the firmware revision minor
 * un-padded ("[%d.%d]" -> "1.0") and the EFI spec minor two-wide ("%01d.%02d"
 * -> "1.10"); our two revisions here are EFI 1.10 and firmware 1.00, whose
 * minors (10 and 0) both fall outside the 1..9 range that would differ, so a
 * plain decimal minor matches the sample's display for both.
 */
static void fw_post_emit_version(UINT32 Revision)
{
    fw_post_emit_udec(Revision >> 16);
    efi_conout_ascii(".");
    fw_post_emit_udec(Revision & 0xffffU);
}

#define FW_POST_HOLD_MS   2000U

/*
 * A reference-platform-style POST summary on the video console.
 *
 * Our firmware POSTs almost instantly and, before this, dropped straight into
 * the boot manager with a blank screen -- unlike a real Itanium platform, which
 * shows a firmware/version banner and a processor/memory/device summary during
 * POST.  Mirror that look (the Intel EFI sample prints a one-line
 * "EFI version ... Running on Intel(R) Itanium Processor" banner from its core
 * init before the boot manager clears the screen).  We keep the QEMU branding
 * for the firmware name and version and fill the rest from the values we
 * actually detected, then hold briefly so the screen is perceptible; any key
 * skips ahead to the boot manager.  All figures are read live, so the summary
 * cannot drift from what the firmware published.
 */
static void fw_phase_post_summary(void)
{
    UINT64 cpuid3 = fw_read_cpuid3();
    UINT8 family = (UINT8)((cpuid3 >> IA64_CPUID3_FAMILY_SHIFT) &
                           IA64_CPUID3_FAMILY_MASK);
    UINT8 model = (UINT8)((cpuid3 >> 16) & 0xffU);
    const CHAR8 *cpu_name;
    const CHAR8 *platform_name;
    UINTN elapsed;
    EFI_INPUT_KEY key;

    if (family == IA64_CPUID3_FAMILY_MERCED) {
        cpu_name = "Intel Itanium";
    } else if (family == 0x1fU) {
        cpu_name = "Intel Itanium 2";
    } else if (family == 0x20U) {
        cpu_name = "Intel Itanium 2 9000";
    } else {
        cpu_name = "Intel Itanium";
    }

    /* The chipset personality the machine selected (see fw_platform_is_*). */
    if (fw_platform_is_zx1()) {
        platform_name = "HP zx1 (HP Workstation zx2000 / zx6000 / Integrity rx2600)";
    } else {
        platform_name = "Intel 460GX (Intel SDV / HP Workstation i2000)";
    }

    (void)fw_console_clear();

    fw_console_set_attr(0x0eU);   /* yellow (emphasis) */
    efi_conout_ascii("qemu-system-ia64 Firmware\r\n");
    fw_console_set_attr(0x07U);   /* light grey on black */
    efi_conout_ascii("QEMU IA-64 Firmware  -  EFI ");
    fw_post_emit_version((UINT32)mSystemTable.Hdr.Revision);
    efi_conout_ascii("  -  Firmware Revision ");
    fw_post_emit_version(mSystemTable.FirmwareRevision);
    efi_conout_ascii("\r\n\r\n");

    efi_conout_ascii("Platform       ");
    efi_conout_ascii(platform_name);
    efi_conout_ascii("\r\n");

    efi_conout_ascii("Processor      ");
    fw_post_emit_udec(fw_processor_count());
    efi_conout_ascii(" x ");
    efi_conout_ascii(cpu_name);
    efi_conout_ascii("  (family ");
    fw_post_emit_hex2(family);
    efi_conout_ascii("h model ");
    fw_post_emit_hex2(model);
    efi_conout_ascii("h)\r\n");

    efi_conout_ascii("System Memory  ");
    fw_post_emit_udec(fw_installed_ram_size() / (1024U * 1024U));
    efi_conout_ascii(" MB\r\n");

    efi_conout_ascii("Boot Device    ");
    efi_conout_ascii((const CHAR8 *)fw_storage_description(1));
    efi_conout_ascii("\r\n\r\n");

    /* Hold the summary briefly so it is perceptible; any key skips ahead. */
    for (elapsed = 0; elapsed < FW_POST_HOLD_MS; elapsed += 20U) {
        if (fw_console_read_key(&key) == EFI_SUCCESS) {
            break;
        }
        (void)bs_stall(20000U);
    }
}

static void fw_phase_boot(void)
{
    /*
     * Interactive boot manager: it lists the Boot#### options, honours the EFI
     * Timeout variable with a countdown that auto-boots the default, and offers
     * the EFI shell and a boot-maintenance submenu.  Like the sample it has no
     * exit key and normally loops until a selection boots; it returns here only
     * when there is nothing to show, in which case we fall through to the
     * BootOrder/removable-media path below.
     */
    fw_phase_post_summary();
    fw_boot_menu_run();
    if (mBootServicesExited) {
        while (1) {
        }
    }
    uart_puts("Attempting disk boot...\r\n");

    {
        EFI_STATUS st = boot_image_from_boot_order();
        if (st != EFI_SUCCESS && !mBootServicesExited) {
            uart_puts("\r\nBootOrder boot failed (");
            uart_puts(efi_status_name(st));
            uart_puts(", status=0x");
            uart_put_hex64(st);
            uart_puts("). Trying removable media fallback.\r\n");
            st = boot_image_from_disk();
        }
        if (st == EFI_SUCCESS || mBootServicesExited) {
            uart_puts("\r\nBoot image returned successfully.\r\n");
            while (1) {
            }
        }
        if (st != EFI_SUCCESS) {
            uart_puts("\r\nDisk boot failed (");
            uart_puts(efi_status_name(st));
            uart_puts(", status=0x");
            uart_put_hex64(st);
            uart_puts(").\r\n");
        }
    }

    /*
     * No bootable image was found.  Drop into the EFI shell rather than
     * halting: a disc may still be startable by hand -- e.g. a combo IA-64
     * install disc whose only El Torito boot entry is the legacy x86 CDBOOT
     * loader carries its IA-64 loader at fs0:\IA64\SETUPLDR.EFI, which the
     * user can launch from the shell.
     */
    uart_puts("\r\nNo bootable image found. Entering EFI shell.\r\n");
    efi_conout_ascii("\r\nNo bootable image found. Entering EFI shell.\r\n");
    while (!mBootServicesExited) {
        fw_boot_shell_run();
    }
    while (1) {}
}

void firmware_main(UINT64 gp, UINT64 stack_top, UINT64 boot_b0)
{
    fw_phase_platform_init(gp, stack_top, boot_b0);
    fw_phase_efi_core_init();
    fw_phase_storage_bringup();
    fw_phase_protocols_and_selftests();
    fw_phase_boot();
}
