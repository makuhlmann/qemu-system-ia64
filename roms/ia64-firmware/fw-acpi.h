/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SAL System Table and ACPI/HCDP/DBGP table structures (the guest-visible
 * platform-table scaffolds), shared by platform_tables.c and firmware.c.
 */

#ifndef IA64_FIRMWARE_FW_ACPI_H
#define IA64_FIRMWARE_FW_ACPI_H

#include "fw-base.h"
#include "fw-efi-types.h"
#include "fw-platform-layout.h"

#define EFI_SIGNATURE_32(a,b,c,d) \
    (((UINT32)(a)<<0)|((UINT32)(b)<<8)|((UINT32)(c)<<16)|((UINT32)(d)<<24))

#define SAL_REVISION_3_0             0x0300U
#define SAL_REVISION_3_2             0x0320U
#define IA64_CPUID3_FAMILY_SHIFT     24U
#define IA64_CPUID3_FAMILY_MASK      0xffU
#define IA64_CPUID3_FAMILY_MERCED    0x07U
/*
 * ITR(0) as published in the SST: a 1 MB translation at the image shadow
 * base - exactly the firmware-context identity window the emulator models
 * (rework D11; the OS purges this register with these VA/size values).
 */
#define SAL_TR_PAGE_SHIFT            20U
#define SAL_TR_ENCODED_PAGE_SIZE     (SAL_TR_PAGE_SHIFT << 2)

typedef struct {
    UINT64 Base;
    UINT64 End;
} FW_RAM_RANGE;



/* --- SAL + ACPI platform table scaffolds ---------------------------------- */

typedef struct {
    UINT8  Type;
    UINT8  Reserved0[7];
    UINT64 PalProc;
    UINT64 SalProc;
    UINT64 SalGp;
    UINT8  Reserved1[16];
} __attribute__((packed)) IA64_SAL_ENTRYPOINT_DESCRIPTOR;

typedef struct {
    UINT8 Type;
    UINT8 Features;
    UINT8 Reserved[14];
} __attribute__((packed)) IA64_SAL_PLATFORM_FEATURES_DESCRIPTOR;

typedef struct {
    UINT8  Type;
    UINT8  RegisterType;
    UINT8  RegisterNumber;
    UINT8  Reserved0[5];
    UINT64 VirtualAddress;
    UINT64 EncodedPageSize;
    UINT64 Reserved1;
} __attribute__((packed)) IA64_SAL_TR_DESCRIPTOR;

typedef struct {
    UINT8  Type;
    UINT8  Mechanism;
    UINT8  Reserved[6];
    UINT64 Vector;
} __attribute__((packed)) IA64_SAL_AP_WAKE_DESCRIPTOR;

/*
 * SAL spec 3.2.3 type-1 memory descriptor.  Windows' HAL requires the SST
 * to describe the PAL code, SAL code, SAL data and firmware ROM spaces
 * (REGULAR_MEMORY usages 1/4/5 and FIRMWARE_CODE usage 0): it maps each
 * region and derives its virtual SAL/PAL entry points from them; a missing
 * region fails HalpInitSalPal and with it HalInitSystem phase 1 (0x61).
 */
typedef struct {
    UINT8  Type;
    UINT8  NeedVaReg;
    UINT8  CurrentAttribute;
    UINT8  PageAccessRights;
    UINT8  SupportedAttributes;
    UINT8  Reserved0;
    UINT8  MemoryType;
    UINT8  MemoryUsage;
    UINT64 PhysicalAddress;
    UINT32 Length;              /* in 4 KiB pages */
    UINT8  Reserved1[4];
    UINT8  OemReserved[8];
} __attribute__((packed)) IA64_SAL_MEMORY_DESCRIPTOR;

#define SAL_MEM_TYPE_REGULAR        0
#define SAL_MEM_TYPE_FIRMWARE_CODE  4
#define SAL_MEM_USAGE_PAL_CODE      1
#define SAL_MEM_USAGE_SAL_CODE      4
#define SAL_MEM_USAGE_SAL_DATA      5
#define SAL_MEM_USAGE_FW_SAL_PAL    0

typedef struct {
    UINT32 Signature;
    UINT32 Length;
    UINT16 Revision;
    UINT16 EntryCount;
    UINT8  Checksum;
    UINT8  Reserved0[7];
    UINT16 SalAVersion;
    UINT16 SalBVersion;
    UINT8  OemId[32];
    UINT8  ProductId[32];
    UINT8  Reserved1[8];
    IA64_SAL_ENTRYPOINT_DESCRIPTOR Entrypoint;
    IA64_SAL_MEMORY_DESCRIPTOR MemoryDescriptors[4];
    IA64_SAL_PLATFORM_FEATURES_DESCRIPTOR PlatformFeatures;
    IA64_SAL_TR_DESCRIPTOR TranslationRegister;
    IA64_SAL_AP_WAKE_DESCRIPTOR ApWake;
} __attribute__((packed)) IA64_SAL_SYSTEM_TABLE;

typedef struct {
    UINT32 Signature;
    UINT32 Length;
    UINT8  Revision;
    UINT8  Checksum;
    UINT8  OemId[6];
    UINT8  OemTableId[8];
    UINT32 OemRevision;
    UINT32 CreatorId;
    UINT32 CreatorRevision;
} __attribute__((packed)) ACPI_SDT_HEADER;

typedef struct {
    UINT8  SpaceId;
    UINT8  BitWidth;
    UINT8  BitOffset;
    UINT8  Reserved;
    UINT32 AddressLow;
    UINT32 AddressHigh;
} __attribute__((packed)) ACPI_GENERIC_ADDRESS;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT32 FirmwareCtrl;
    UINT32 Dsdt;
    UINT8  Model;
    UINT8  PreferredProfile;
    UINT16 SciInterrupt;
    UINT32 SmiCommand;
    UINT8  AcpiEnable;
    UINT8  AcpiDisable;
    UINT8  S4BiosRequest;
    UINT8  PStateControl;
    UINT32 Pm1aEventBlock;
    UINT32 Pm1bEventBlock;
    UINT32 Pm1aControlBlock;
    UINT32 Pm1bControlBlock;
    UINT32 Pm2ControlBlock;
    UINT32 PmTimerBlock;
    UINT32 Gpe0Block;
    UINT32 Gpe1Block;
    UINT8  Pm1EventLength;
    UINT8  Pm1ControlLength;
    UINT8  Pm2ControlLength;
    UINT8  PmTimerLength;
    UINT8  Gpe0BlockLength;
    UINT8  Gpe1BlockLength;
    UINT8  Gpe1Base;
    UINT8  CstControl;
    UINT16 C2Latency;
    UINT16 C3Latency;
    UINT16 FlushSize;
    UINT16 FlushStride;
    UINT8  DutyOffset;
    UINT8  DutyWidth;
    UINT8  DayAlarm;
    UINT8  MonthAlarm;
    UINT8  Century;
    UINT16 BootFlags;
    UINT8  Reserved0;
    UINT32 Flags;
    ACPI_GENERIC_ADDRESS ResetRegister;
    UINT8  ResetValue;
    UINT8  Reserved1[3];
    UINT64 XFirmwareCtrl;
    UINT64 XDsdt;
    ACPI_GENERIC_ADDRESS XPm1aEventBlock;
    ACPI_GENERIC_ADDRESS XPm1bEventBlock;
    ACPI_GENERIC_ADDRESS XPm1aControlBlock;
    ACPI_GENERIC_ADDRESS XPm1bControlBlock;
    ACPI_GENERIC_ADDRESS XPm2ControlBlock;
    ACPI_GENERIC_ADDRESS XPmTimerBlock;
    ACPI_GENERIC_ADDRESS XGpe0Block;
    ACPI_GENERIC_ADDRESS XGpe1Block;
} __attribute__((packed)) ACPI_FADT;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT64 Entry[8];
} __attribute__((packed)) ACPI_XSDT;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT32 Entry[8];
} __attribute__((packed)) ACPI_RSDT;

typedef struct {
    UINT8  Signature[8];
    UINT8  Checksum;
    UINT8  OemId[6];
    UINT8  Revision;
    UINT32 RsdtAddress;
    UINT32 Length;
    UINT64 XsdtAddress;
    UINT8  ExtendedChecksum;
    UINT8  Reserved[3];
} __attribute__((packed)) ACPI_RSDP;

typedef struct {
    UINT32 Signature;
    UINT32 Length;
    UINT32 HardwareSignature;
    UINT32 FirmwareWakingVector;
    UINT32 GlobalLock;
    UINT32 Flags;
    UINT64 XFirmwareWakingVector;
    UINT8  Version;
    UINT8  Reserved0[3];
    UINT32 OspmFlags;
    UINT8  Reserved1[24];
} __attribute__((packed)) ACPI_FACS;

/*
 * The DSDT/SSDT AML buffers are sized to the larger of the two chipset-profile
 * variants (the nested zx1 DSDT is bigger than the flat 460gx one).  The flat
 * AML is the static initializer, so the 460gx/default table is byte-identical
 * to before; the zx1 profile copies its AML in at runtime and sets the header
 * Length/checksum from the active AML size, so the unused tail is never
 * guest-visible.  Keep these in lockstep with the FW_*_AML_SIZE asserts in
 * firmware.c.
 */
typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT8 Aml[1182];
} __attribute__((packed)) ACPI_DSDT;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT8 Aml[506];
} __attribute__((packed)) ACPI_SSDT;

typedef struct {
    UINT64 BaseAddress;
    UINT16 PciSegmentGroup;
    UINT8  StartBusNumber;
    UINT8  EndBusNumber;
    UINT32 Reserved;
} __attribute__((packed)) ACPI_MCFG_ALLOCATION;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT64 Reserved;
    ACPI_MCFG_ALLOCATION Allocation[1];
} __attribute__((packed)) ACPI_MCFG;

typedef struct {
    UINT8  Type;
    UINT8  Length;
    UINT8  ProcessorId;
    UINT8  Id;
    UINT8  Eid;
    UINT8  Reserved[3];
    UINT32 Flags;
} __attribute__((packed)) ACPI_MADT_LSAPIC;

typedef struct {
    UINT8  Type;
    UINT8  Length;
    UINT8  Id;
    UINT8  Reserved;
    UINT32 GsiBase;
    UINT64 Address;
} __attribute__((packed)) ACPI_MADT_IOSAPIC;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT32 LocalApicAddr;
    UINT32 Flags;
    /* ACPI 2.0 Errata C entries omit the later UID extension fields. */
    ACPI_MADT_LSAPIC Lsapic[FW_MAX_CPUS];
    ACPI_MADT_IOSAPIC Iosapic;
} __attribute__((packed)) ACPI_MADT;

typedef struct {
    UINT8  Type;
    UINT8  Length;
    UINT8  ProximityDomain;
    UINT8  ApicId;
    UINT32 Flags;
    UINT8  LsapicEid;
    UINT8  Reserved[7];
} __attribute__((packed)) ACPI_SRAT_PROCESSOR_AFFINITY;

typedef struct {
    UINT8  Type;
    UINT8  Length;
    UINT32 ProximityDomain;
    UINT16 Reserved0;
    UINT32 BaseAddrLow;
    UINT32 BaseAddrHigh;
    UINT32 LengthLow;
    UINT32 LengthHigh;
    UINT32 Reserved1;
    UINT32 Flags;
    UINT64 Reserved2;
} __attribute__((packed)) ACPI_SRAT_MEMORY_AFFINITY;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT32 TableRevision;
    UINT64 Reserved;
    ACPI_SRAT_MEMORY_AFFINITY Memory[FW_MEMORY_AFFINITY_MAX];
    ACPI_SRAT_PROCESSOR_AFFINITY Processor[FW_MAX_CPUS];
} __attribute__((packed)) ACPI_SRAT;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT64 Localities;
    UINT8  Entry[1];
} __attribute__((packed)) ACPI_SLIT;

typedef struct {
    UINT8  Type;
    UINT8  Bits;
    UINT8  Parity;
    UINT8  StopBits;
    UINT8  PciSegment;
    UINT8  PciBus;
    UINT8  PciDevice;
    UINT8  PciFunction;
    UINT64 Baud;
    ACPI_GENERIC_ADDRESS BaseAddress;
    UINT16 PciDeviceId;
    UINT16 PciVendorId;
    UINT32 GlobalInterrupt;
    UINT32 ClockRate;
    UINT8  PciProgrammingInterface;
    UINT8  Flags;
    UINT16 ConOutIndex;
    UINT32 Reserved;
} __attribute__((packed)) HCDP_UART_DESCRIPTOR;

typedef struct {
    UINT8  Interconnect;
    UINT8  Reserved;
    UINT16 Length;
    UINT8  Segment;
    UINT8  Bus;
    UINT8  Device;
    UINT8  Function;
    UINT16 DeviceId;
    UINT16 VendorId;
    UINT32 AcpiInterrupt;
    UINT64 MmioTranslation;
    UINT64 IoPortTranslation;
    UINT8  Flags;
    UINT8  Translation;
} __attribute__((packed)) HCDP_PCI_INTERFACE;

typedef struct {
    UINT8  Count;
} __attribute__((packed)) HCDP_VGA_DESCRIPTOR;

typedef struct {
    UINT8  Type;
    UINT8  Flags;
    UINT16 Length;
    UINT16 EfiIndex;
    HCDP_PCI_INTERFACE Pci;
    HCDP_VGA_DESCRIPTOR Vga;
} __attribute__((packed)) HCDP_DEVICE_DESCRIPTOR;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT32 EntryCount;
    HCDP_UART_DESCRIPTOR Uart[1];
    HCDP_DEVICE_DESCRIPTOR Device[1];
} __attribute__((packed)) ACPI_HCDP;

typedef struct {
    ACPI_SDT_HEADER Hdr;
    UINT8 InterfaceType;
    UINT8 Reserved[3];
    ACPI_GENERIC_ADDRESS BaseAddress;
} __attribute__((packed)) ACPI_DBGP;

typedef struct _EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL;

typedef enum {
    EfiPciWidthUint8,
    EfiPciWidthUint16,
    EfiPciWidthUint32,
    EfiPciWidthUint64,
    EfiPciWidthFifoUint8,
    EfiPciWidthFifoUint16,
    EfiPciWidthFifoUint32,
    EfiPciWidthFifoUint64,
    EfiPciWidthFillUint8,
    EfiPciWidthFillUint16,
    EfiPciWidthFillUint32,
    EfiPciWidthFillUint64,
    EfiPciWidthMaximum
} EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH;

typedef enum {
    EfiPciOperationBusMasterRead,
    EfiPciOperationBusMasterWrite,
    EfiPciOperationBusMasterCommonBuffer,
    EfiPciOperationBusMasterRead64,
    EfiPciOperationBusMasterWrite64,
    EfiPciOperationBusMasterCommonBuffer64,
    EfiPciOperationMaximum
} EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION;

typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_POLL_IO_MEM)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
    UINT64 Address, UINT64 Mask, UINT64 Value, UINT64 Delay,
    UINT64 *Result);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
    UINT64 Address, UINTN Count, VOID *Buffer);

typedef struct {
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM Read;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM Write;
} EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_ACCESS;

typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_COPY_MEM)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
    UINT64 DestAddress, UINT64 SrcAddress, UINTN Count);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_MAP)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION Operation,
    VOID *HostAddress, UINTN *NumberOfBytes,
    EFI_PHYSICAL_ADDRESS *DeviceAddress, VOID **Mapping);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_UNMAP)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, VOID *Mapping);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_ALLOCATE_BUFFER)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **HostAddress,
    UINT64 Attributes);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_FREE_BUFFER)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, UINTN Pages, VOID *HostAddress);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_FLUSH)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GET_ATTRIBUTES)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, UINT64 *Supports,
    UINT64 *Attributes);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_SET_ATTRIBUTES)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, UINT64 Attributes,
    UINT64 *ResourceBase, UINT64 *ResourceLength);
typedef EFI_STATUS (*EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_CONFIGURATION)(
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This, VOID **Resources);

struct _EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL {
    EFI_HANDLE ParentHandle;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_POLL_IO_MEM PollMem;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_POLL_IO_MEM PollIo;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_ACCESS Mem;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_ACCESS Io;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_ACCESS Pci;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_COPY_MEM CopyMem;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_MAP Map;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_UNMAP Unmap;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_ALLOCATE_BUFFER AllocateBuffer;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_FREE_BUFFER FreeBuffer;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_FLUSH Flush;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GET_ATTRIBUTES GetAttributes;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_SET_ATTRIBUTES SetAttributes;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_CONFIGURATION Configuration;
    UINT32 SegmentNumber;
};

typedef struct _EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
typedef EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH EFI_PCI_IO_PROTOCOL_WIDTH;
typedef EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION EFI_PCI_IO_PROTOCOL_OPERATION;

#define EFI_PCI_IO_PASS_THROUGH_BAR 0xffU

typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_POLL_IO_MEM)(
    EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
    UINT8 BarIndex, UINT64 Offset, UINT64 Mask, UINT64 Value,
    UINT64 Delay, UINT64 *Result);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_IO_MEM)(
    EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
    UINT8 BarIndex, UINT64 Offset, UINTN Count, VOID *Buffer);

typedef struct {
    EFI_PCI_IO_PROTOCOL_IO_MEM Read;
    EFI_PCI_IO_PROTOCOL_IO_MEM Write;
} EFI_PCI_IO_PROTOCOL_ACCESS;

typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_CONFIG)(
    EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
    UINT32 Offset, UINTN Count, VOID *Buffer);

typedef struct {
    EFI_PCI_IO_PROTOCOL_CONFIG Read;
    EFI_PCI_IO_PROTOCOL_CONFIG Write;
} EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS;

typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_COPY_MEM)(
    EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
    UINT8 DestBarIndex, UINT64 DestOffset, UINT8 SrcBarIndex,
    UINT64 SrcOffset, UINTN Count);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_MAP)(
    EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_OPERATION Operation,
    VOID *HostAddress, UINTN *NumberOfBytes,
    EFI_PHYSICAL_ADDRESS *DeviceAddress, VOID **Mapping);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_UNMAP)(
    EFI_PCI_IO_PROTOCOL *This, VOID *Mapping);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_ALLOCATE_BUFFER)(
    EFI_PCI_IO_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **HostAddress,
    UINT64 Attributes);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_FREE_BUFFER)(
    EFI_PCI_IO_PROTOCOL *This, UINTN Pages, VOID *HostAddress);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_FLUSH)(EFI_PCI_IO_PROTOCOL *This);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_GET_LOCATION)(
    EFI_PCI_IO_PROTOCOL *This, UINTN *SegmentNumber, UINTN *BusNumber,
    UINTN *DeviceNumber, UINTN *FunctionNumber);

typedef enum {
    EfiPciIoAttributeOperationGet,
    EfiPciIoAttributeOperationSet,
    EfiPciIoAttributeOperationEnable,
    EfiPciIoAttributeOperationDisable,
    EfiPciIoAttributeOperationSupported,
    EfiPciIoAttributeOperationMaximum
} EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION;

typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_ATTRIBUTES)(
    EFI_PCI_IO_PROTOCOL *This,
    EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Operation,
    UINT64 Attributes, UINT64 *Result);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_GET_BAR_ATTRIBUTES)(
    EFI_PCI_IO_PROTOCOL *This, UINT8 BarIndex, UINT64 *Supports,
    VOID **Resources);
typedef EFI_STATUS (*EFI_PCI_IO_PROTOCOL_SET_BAR_ATTRIBUTES)(
    EFI_PCI_IO_PROTOCOL *This, UINT64 Attributes, UINT8 BarIndex,
    UINT64 *Offset, UINT64 *Length);

struct _EFI_PCI_IO_PROTOCOL {
    EFI_PCI_IO_PROTOCOL_POLL_IO_MEM PollMem;
    EFI_PCI_IO_PROTOCOL_POLL_IO_MEM PollIo;
    EFI_PCI_IO_PROTOCOL_ACCESS Mem;
    EFI_PCI_IO_PROTOCOL_ACCESS Io;
    EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS Pci;
    EFI_PCI_IO_PROTOCOL_COPY_MEM CopyMem;
    EFI_PCI_IO_PROTOCOL_MAP Map;
    EFI_PCI_IO_PROTOCOL_UNMAP Unmap;
    EFI_PCI_IO_PROTOCOL_ALLOCATE_BUFFER AllocateBuffer;
    EFI_PCI_IO_PROTOCOL_FREE_BUFFER FreeBuffer;
    EFI_PCI_IO_PROTOCOL_FLUSH Flush;
    EFI_PCI_IO_PROTOCOL_GET_LOCATION GetLocation;
    EFI_PCI_IO_PROTOCOL_ATTRIBUTES Attributes;
    EFI_PCI_IO_PROTOCOL_GET_BAR_ATTRIBUTES GetBarAttributes;
    EFI_PCI_IO_PROTOCOL_SET_BAR_ATTRIBUTES SetBarAttributes;
    UINT64 RomSize;
    VOID *RomImage;
};

typedef struct {
    UINT8  Descriptor;
    UINT16 Length;
    UINT8  ResourceType;
    UINT8  GeneralFlags;
    UINT8  TypeSpecificFlags;
    UINT64 AddressSpaceGranularity;
    UINT64 AddressRangeMinimum;
    UINT64 AddressRangeMaximum;
    UINT64 AddressTranslationOffset;
    UINT64 AddressLength;
} __attribute__((packed)) ACPI_QWORD_ADDRESS_DESCRIPTOR;

typedef struct {
    UINT8 Descriptor;
    UINT8 Checksum;
} __attribute__((packed)) ACPI_END_TAG_DESCRIPTOR;

typedef struct {
    ACPI_QWORD_ADDRESS_DESCRIPTOR Bus;
    ACPI_QWORD_ADDRESS_DESCRIPTOR Io;
    ACPI_QWORD_ADDRESS_DESCRIPTOR Mem;
    ACPI_END_TAG_DESCRIPTOR End;
} __attribute__((packed)) FW_PCI_ROOT_BRIDGE_RESOURCES;

typedef struct {
    ACPI_QWORD_ADDRESS_DESCRIPTOR Address;
    ACPI_END_TAG_DESCRIPTOR End;
} __attribute__((packed)) FW_PCI_BAR_RESOURCES;

FW_STATIC_ASSERT(sizeof(ACPI_SDT_HEADER) == 36, acpi_sdt_header_size);
FW_STATIC_ASSERT(sizeof(IA64_SAL_ENTRYPOINT_DESCRIPTOR) == 48,
                 sal_entrypoint_descriptor_size);
FW_STATIC_ASSERT(sizeof(IA64_SAL_PLATFORM_FEATURES_DESCRIPTOR) == 16,
                 sal_platform_features_descriptor_size);
FW_STATIC_ASSERT(sizeof(IA64_SAL_TR_DESCRIPTOR) == 32,
                 sal_tr_descriptor_size);
FW_STATIC_ASSERT(sizeof(IA64_SAL_AP_WAKE_DESCRIPTOR) == 16,
                 sal_ap_wake_descriptor_size);
FW_STATIC_ASSERT(sizeof(IA64_SAL_MEMORY_DESCRIPTOR) == 32,
                 sal_memory_descriptor_size);
FW_STATIC_ASSERT(sizeof(IA64_SAL_SYSTEM_TABLE) == 336,
                 sal_system_table_size);
FW_STATIC_ASSERT(__builtin_offsetof(IA64_SAL_SYSTEM_TABLE,
                                    TranslationRegister) == 288,
                 sal_tr_descriptor_offset);
FW_STATIC_ASSERT(__builtin_offsetof(IA64_SAL_SYSTEM_TABLE, ApWake) == 320,
                 sal_ap_wake_descriptor_offset);
FW_STATIC_ASSERT(FW_BOOT_STACK_SIZE >=
                 IA64_EFI_MIN_STACK_BYTES,
                 efi_boot_stack_capacity);
FW_STATIC_ASSERT(FW_AP_STACK_SIZE >=
                 IA64_EFI_MIN_STACK_BYTES,
                 efi_ap_stack_capacity);
FW_STATIC_ASSERT(IA64_FW_SAL_RUNTIME_END_OFFSET <=
                 IA64_FW_DEBUG_CONTEXT_OFFSET,
                 sal_debug_context_disjoint);
FW_STATIC_ASSERT(IA64_FW_DEBUG_CONTEXT_SIZE <=
                 IA64_FW_DEBUG_CONTEXT_STRIDE,
                 debug_context_stride_capacity);
FW_STATIC_ASSERT(IA64_FW_DEBUG_CONTEXT_END_OFFSET <=
                 IA64_FW_DEBUG_STACK_OFFSET,
                 debug_context_stack_disjoint);
FW_STATIC_ASSERT(IA64_FW_DEBUG_STACK_END_OFFSET <= IA64_FW_EARLY_RSE_OFFSET,
                 debug_stack_rse_disjoint);
FW_STATIC_ASSERT(IA64_FW_EARLY_RSE_END_OFFSET <= IA64_FW_BOOT_STACK_OFFSET,
                 early_rse_boot_stack_disjoint);
FW_STATIC_ASSERT(IA64_FW_BOOT_STACK_OFFSET + IA64_FW_BOOT_STACK_SIZE ==
                 IA64_FW_CPU_ASSIST_SIZE,
                 boot_stack_tops_cpu_assist);
FW_STATIC_ASSERT((IA64_FW_CPU_ASSIST_SIZE & (IA64_FW_LOW_RAM_ALIGN - 1U)) == 0,
                 cpu_assist_alignment);
FW_STATIC_ASSERT(IA64_FW_LOW_RAM_MIN >= IA64_FW_CPU_ASSIST_SIZE,
                 cpu_assist_fits_minimum_ram);
FW_STATIC_ASSERT(IA64_FW_LOW_RAM_MIN >=
                 IA64_FW_CPU_ASSIST_SIZE + FW_ACPI_REGION_SIZE,
                 ram_top_acpi_region_fits_minimum_ram);
FW_STATIC_ASSERT((FW_ACPI_REGION_SIZE & (IA64_FW_LOW_RAM_ALIGN - 1U)) == 0,
                 acpi_region_alignment);
FW_STATIC_ASSERT(IA64_FW_SAL_RUNTIME_SLOT_SIZE / 2U >=
                 IA64_EFI_MIN_BACKING_BYTES,
                 sal_runtime_slot_capacity);
FW_STATIC_ASSERT((IA64_FW_EARLY_RSE_OFFSET & 7U) == 0,
                 sal_backing_store_alignment);
FW_STATIC_ASSERT(IA64_FW_EARLY_RSE_END_OFFSET > IA64_FW_EARLY_RSE_OFFSET,
                 sal_backing_store_order);
FW_STATIC_ASSERT((IA64_FW_EARLY_RSE_END_OFFSET - IA64_FW_EARLY_RSE_OFFSET) /
                 FW_MAX_CPUS >= IA64_EFI_MIN_BACKING_BYTES,
                 sal_backing_store_capacity);
FW_STATIC_ASSERT(sizeof(ACPI_FADT) == 244, acpi_fadt_size);
FW_STATIC_ASSERT(sizeof(ACPI_XSDT) == 100, acpi_xsdt_size);
FW_STATIC_ASSERT(sizeof(ACPI_RSDT) == 68, acpi_rsdt_size);
FW_STATIC_ASSERT(sizeof(ACPI_RSDP) == 36, acpi_rsdp_size);
FW_STATIC_ASSERT(sizeof(ACPI_FACS) == 64, acpi_facs_size);
FW_STATIC_ASSERT(sizeof(ACPI_DSDT) == 1218, acpi_dsdt_size);
FW_STATIC_ASSERT(sizeof(ACPI_SSDT) == 542, acpi_ssdt_size);
FW_STATIC_ASSERT(sizeof(ACPI_MCFG_ALLOCATION) == 16,
                 acpi_mcfg_allocation_size);
FW_STATIC_ASSERT(sizeof(ACPI_MCFG) == 60, acpi_mcfg_size);
FW_STATIC_ASSERT(sizeof(ACPI_MADT_LSAPIC) == 12, acpi_madt_lsapic_size);
FW_STATIC_ASSERT(sizeof(ACPI_MADT_IOSAPIC) == 16, acpi_madt_iosapic_size);
FW_STATIC_ASSERT(sizeof(ACPI_MADT) == 156, acpi_madt_size);
FW_STATIC_ASSERT(sizeof(ACPI_SRAT_PROCESSOR_AFFINITY) == 16,
                 acpi_srat_processor_affinity_size);
FW_STATIC_ASSERT(sizeof(ACPI_SRAT_MEMORY_AFFINITY) == 40,
                 acpi_srat_memory_affinity_size);
FW_STATIC_ASSERT(sizeof(ACPI_SRAT) == 336, acpi_srat_size);
FW_STATIC_ASSERT(sizeof(ACPI_SLIT) == 45, acpi_slit_size);
FW_STATIC_ASSERT(sizeof(ACPI_GENERIC_ADDRESS) == 12, acpi_gas_size);
FW_STATIC_ASSERT(sizeof(HCDP_UART_DESCRIPTOR) == 48, acpi_hcdp_uart_size);
FW_STATIC_ASSERT(sizeof(HCDP_PCI_INTERFACE) == 34, acpi_hcdp_pci_size);
FW_STATIC_ASSERT(sizeof(HCDP_DEVICE_DESCRIPTOR) == 41, acpi_hcdp_device_size);
FW_STATIC_ASSERT(sizeof(ACPI_DBGP) == 52, acpi_dbgp_size);
FW_STATIC_ASSERT(sizeof(ACPI_QWORD_ADDRESS_DESCRIPTOR) == 46,
                 acpi_qword_address_descriptor_size);
FW_STATIC_ASSERT(sizeof(FW_PCI_ROOT_BRIDGE_RESOURCES) == 140,
                 pci_root_bridge_resources_size);
FW_STATIC_ASSERT(sizeof(FW_PCI_BAR_RESOURCES) == 48,
                 pci_bar_resources_size);
FW_STATIC_ASSERT(sizeof(EFI_SYSTEM_TABLE_POINTER) == 24,
                 efi_system_table_pointer_size);
FW_STATIC_ASSERT(__builtin_offsetof(EFI_SYSTEM_TABLE_POINTER, Crc32) == 16,
                 efi_system_table_pointer_crc_offset);
FW_STATIC_ASSERT((LEGACY_IO_BASE & (PCI_IO_SPARSE_SIZE - 1)) == 0,
                 legacy_io_base_sparse_alignment);
FW_STATIC_ASSERT(__builtin_offsetof(HCDP_UART_DESCRIPTOR, Flags) == 41,
                 acpi_hcdp_uart_flags_offset);
FW_STATIC_ASSERT(__builtin_offsetof(HCDP_UART_DESCRIPTOR, ConOutIndex) == 42,
                 acpi_hcdp_uart_conout_index_offset);
FW_STATIC_ASSERT(__builtin_offsetof(HCDP_UART_DESCRIPTOR, Reserved) == 44,
                 acpi_hcdp_uart_reserved_offset);

#define HCDP_UART_FLAG_ACTIVE_LOW       (1u << 1)
#define HCDP_UART_FLAG_PRIMARY_CONSOLE  (1u << 2)
#define HCDP_UART_FLAG_INTERRUPT        (1u << 6)
#define HCDP_UART_ACPI_HID_PNP0501      0x0105d041U
#define HCDP_UART_PSEUDO_CLOCK_RATE     115200U
#define HCDP_CONOUT_VGA_INDEX            0U
#define HCDP_CONOUT_UART_INDEX           1U
#define HCDP_DEVICE_FLAG_PRIMARY_CONSOLE 1u
#define HCDP_DEVICE_TYPE_VGA_CONSOLE    ((1u << 3) | 2u)
#define HCDP_PCI_INTERFACE_TYPE         1u
#define HCDP_PCI_TRANSLATE_MMIO         0x01u
#define HCDP_PCI_TRANSLATE_IOPORT       0x02u

#define PLATFORM_TABLE_ACPI20        0
#define PLATFORM_TABLE_ACPI10        1
#define PLATFORM_TABLE_SAL           2
#define PLATFORM_TABLE_HCDP          3
#define PLATFORM_TABLE_SMBIOS        4
#define PLATFORM_TABLE_DEBUG_IMAGE   5
#define PLATFORM_TABLE_INITIAL       6
#define PLATFORM_TABLE_MAX           16
/* LOADED_IMAGE_MAX lives in fw-pe.h. */

extern EFI_SYSTEM_TABLE mSystemTable;
extern EFI_CONFIGURATION_TABLE mConfigTables[PLATFORM_TABLE_MAX];
extern EFI_DEBUG_IMAGE_INFO_TABLE_HEADER mDebugImageInfoHeader;
extern const UINT8 gEfiSalSystemTableGuid[16];
extern const UINT8 gEfiHcdpTableGuid[16];
extern const UINT8 gEfiSmbiosTableGuid[16];
extern const UINT8 mDebugImageInfoTableGuid[16];

UINT16 fw_sal_revision(void);
BOOLEAN fw_platform_is_460gx(void);
BOOLEAN fw_platform_is_zx1(void);
UINT64 fw_current_gp(void);
UINT64 fw_function_entry(UINTN FunctionPointer);
UINT64 fw_sal_proc_function_entry(void);
BOOLEAN fw_handoff_vga_console_primary(void);

void efi_init_platform_tables(void);
BOOLEAN acpi_table_integrity_selftest(void);

#endif /* IA64_FIRMWARE_FW_ACPI_H */
