/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SMBIOS 2.7 table generation, published through the EFI configuration
 * table.  Guest RAM/topology facts come through the fw_guest_*
 * accessors.
 */

#include "fw-base.h"
#include "fw-services.h"

/* SMBIOS 2.7 structures published through the UEFI configuration table. */
typedef struct {
    UINT8  AnchorString[4];
    UINT8  Checksum;
    UINT8  Length;
    UINT8  MajorVersion;
    UINT8  MinorVersion;
    UINT16 MaxStructureSize;
    UINT8  EntryPointRevision;
    UINT8  FormattedArea[5];
    UINT8  IntermediateAnchorString[5];
    UINT8  IntermediateChecksum;
    UINT16 StructureTableLength;
    UINT32 StructureTableAddress;
    UINT16 NumberOfStructures;
    UINT8  BcdRevision;
} __attribute__((packed)) SMBIOS_ENTRY_POINT_21;

typedef struct {
    UINT8  Type;
    UINT8  Length;
    UINT16 Handle;
} __attribute__((packed)) SMBIOS_STRUCTURE_HEADER;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  Vendor;
    UINT8  BiosVersion;
    UINT16 BiosStartingAddressSegment;
    UINT8  BiosReleaseDate;
    UINT8  BiosRomSize;
    UINT64 BiosCharacteristics;
    UINT8  BiosCharacteristicsExtensionBytes[2];
    UINT8  SystemBiosMajorRelease;
    UINT8  SystemBiosMinorRelease;
    UINT8  EmbeddedControllerMajorRelease;
    UINT8  EmbeddedControllerMinorRelease;
} __attribute__((packed)) SMBIOS_TYPE0_BIOS_INFORMATION;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  Manufacturer;
    UINT8  ProductName;
    UINT8  Version;
    UINT8  SerialNumber;
    UINT8  Uuid[16];
    UINT8  WakeUpType;
    UINT8  SkuNumber;
    UINT8  Family;
} __attribute__((packed)) SMBIOS_TYPE1_SYSTEM_INFORMATION;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  Manufacturer;
    UINT8  Product;
    UINT8  Version;
    UINT8  SerialNumber;
    UINT8  AssetTag;
    UINT8  FeatureFlags;
    UINT8  LocationInChassis;
    UINT16 ChassisHandle;
    UINT8  BoardType;
    UINT8  ContainedObjectHandleCount;
} __attribute__((packed)) SMBIOS_TYPE2_BASEBOARD_INFORMATION;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  Manufacturer;
    UINT8  ChassisType;
    UINT8  Version;
    UINT8  SerialNumber;
    UINT8  AssetTag;
    UINT8  BootUpState;
    UINT8  PowerSupplyState;
    UINT8  ThermalState;
    UINT8  SecurityStatus;
    UINT32 OemDefined;
    UINT8  Height;
    UINT8  NumberOfPowerCords;
    UINT8  ContainedElementCount;
    UINT8  ContainedElementRecordLength;
    UINT8  SkuNumber;
} __attribute__((packed)) SMBIOS_TYPE3_SYSTEM_ENCLOSURE;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  SocketDesignation;
    UINT8  ProcessorType;
    UINT8  ProcessorFamily;
    UINT8  ProcessorManufacturer;
    UINT32 ProcessorId[2];
    UINT8  ProcessorVersion;
    UINT8  Voltage;
    UINT16 ExternalClock;
    UINT16 MaxSpeed;
    UINT16 CurrentSpeed;
    UINT8  Status;
    UINT8  ProcessorUpgrade;
    UINT16 L1CacheHandle;
    UINT16 L2CacheHandle;
    UINT16 L3CacheHandle;
    UINT8  SerialNumber;
    UINT8  AssetTag;
    UINT8  PartNumber;
    UINT8  CoreCount;
    UINT8  CoreEnabled;
    UINT8  ThreadCount;
    UINT16 ProcessorCharacteristics;
    UINT16 ProcessorFamily2;
} __attribute__((packed)) SMBIOS_TYPE4_PROCESSOR_INFORMATION;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  Location;
    UINT8  Use;
    UINT8  ErrorCorrection;
    UINT32 MaximumCapacity;
    UINT16 MemoryErrorInformationHandle;
    UINT16 NumberOfMemoryDevices;
    UINT64 ExtendedMaximumCapacity;
} __attribute__((packed)) SMBIOS_TYPE16_PHYSICAL_MEMORY_ARRAY;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT16 PhysicalMemoryArrayHandle;
    UINT16 MemoryErrorInformationHandle;
    UINT16 TotalWidth;
    UINT16 DataWidth;
    UINT16 Size;
    UINT8  FormFactor;
    UINT8  DeviceSet;
    UINT8  DeviceLocator;
    UINT8  BankLocator;
    UINT8  MemoryType;
    UINT16 TypeDetail;
    UINT16 Speed;
    UINT8  Manufacturer;
    UINT8  SerialNumber;
    UINT8  AssetTag;
    UINT8  PartNumber;
    UINT8  Attributes;
    UINT32 ExtendedSize;
    UINT16 ConfiguredMemoryClockSpeed;
} __attribute__((packed)) SMBIOS_TYPE17_MEMORY_DEVICE;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT32 StartingAddress;
    UINT32 EndingAddress;
    UINT16 MemoryArrayHandle;
    UINT8  PartitionWidth;
    UINT64 ExtendedStartingAddress;
    UINT64 ExtendedEndingAddress;
} __attribute__((packed)) SMBIOS_TYPE19_MEMORY_ARRAY_MAPPED_ADDRESS;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
    UINT8  Reserved[6];
    UINT8  BootStatus;
} __attribute__((packed)) SMBIOS_TYPE32_SYSTEM_BOOT_INFORMATION;

typedef struct {
    SMBIOS_STRUCTURE_HEADER Hdr;
} __attribute__((packed)) SMBIOS_TYPE127_END_OF_TABLE;

#define SMBIOS_TABLE_MAX_SIZE        4096U

static SMBIOS_ENTRY_POINT_21   mSmbiosEntryPoint;
static UINT8                   mSmbiosTable[SMBIOS_TABLE_MAX_SIZE];
static UINT16                  mSmbiosTableLength;
static UINT16                  mSmbiosStructureCount;
static UINT16                  mSmbiosMaxStructureSize;

FW_STATIC_ASSERT(sizeof(SMBIOS_ENTRY_POINT_21) == 31,
                 smbios_entry_point_21_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_STRUCTURE_HEADER) == 4,
                 smbios_structure_header_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE0_BIOS_INFORMATION) == 24,
                 smbios_type0_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE1_SYSTEM_INFORMATION) == 27,
                 smbios_type1_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE2_BASEBOARD_INFORMATION) == 15,
                 smbios_type2_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE3_SYSTEM_ENCLOSURE) == 22,
                 smbios_type3_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE4_PROCESSOR_INFORMATION) == 42,
                 smbios_type4_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE16_PHYSICAL_MEMORY_ARRAY) == 23,
                 smbios_type16_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE17_MEMORY_DEVICE) == 34,
                 smbios_type17_v27_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE19_MEMORY_ARRAY_MAPPED_ADDRESS) == 31,
                 smbios_type19_v27_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE32_SYSTEM_BOOT_INFORMATION) == 11,
                 smbios_type32_size);
FW_STATIC_ASSERT(sizeof(SMBIOS_TYPE127_END_OF_TABLE) == 4,
                 smbios_type127_size);

static UINTN smbios_ascii_len(const CHAR8 *Str)
{
    UINTN Len = 0;

    if (Str == NULL) {
        return 0;
    }
    while (Str[Len] != '\0') {
        Len++;
    }
    return Len;
}

static BOOLEAN smbios_append_bytes(const VOID *Data, UINTN Size)
{
    if (Data == NULL || Size > sizeof(mSmbiosTable) - mSmbiosTableLength) {
        return 0;
    }
    fw_copy_mem(&mSmbiosTable[mSmbiosTableLength], Data, Size);
    mSmbiosTableLength = (UINT16)(mSmbiosTableLength + Size);
    return 1;
}

static BOOLEAN smbios_append_byte(UINT8 Value)
{
    if (mSmbiosTableLength >= sizeof(mSmbiosTable)) {
        return 0;
    }
    mSmbiosTable[mSmbiosTableLength++] = Value;
    return 1;
}

static BOOLEAN smbios_append_string_set(const CHAR8 * const *Strings,
                                        UINTN StringCount)
{
    UINTN i;
    UINTN Len;

    if (StringCount == 0) {
        return smbios_append_byte(0) && smbios_append_byte(0);
    }

    for (i = 0; i < StringCount; i++) {
        Len = smbios_ascii_len(Strings[i]);
        if (!smbios_append_bytes(Strings[i], Len) ||
            !smbios_append_byte(0)) {
            return 0;
        }
    }
    return smbios_append_byte(0);
}

static BOOLEAN smbios_append_structure(const VOID *Formatted,
                                       UINTN FormattedSize,
                                       const CHAR8 * const *Strings,
                                       UINTN StringCount)
{
    UINTN Start;
    UINTN StructureSize;

    if (FormattedSize < sizeof(SMBIOS_STRUCTURE_HEADER) ||
        FormattedSize > 0xffU) {
        return 0;
    }

    Start = mSmbiosTableLength;
    if (!smbios_append_bytes(Formatted, FormattedSize) ||
        !smbios_append_string_set(Strings, StringCount)) {
        return 0;
    }

    StructureSize = mSmbiosTableLength - Start;
    if (StructureSize > mSmbiosMaxStructureSize) {
        mSmbiosMaxStructureSize = (UINT16)StructureSize;
    }
    mSmbiosStructureCount++;
    return 1;
}

static void smbios_header_init(SMBIOS_STRUCTURE_HEADER *Header,
                               UINT8 Type, UINTN Length, UINT16 Handle)
{
    Header->Type = Type;
    Header->Length = (UINT8)Length;
    Header->Handle = Handle;
}

static BOOLEAN smbios_build_type0(void)
{
    static const CHAR8 * const Strings[] = {
        "QEMU",
        "ia64-firmware",
        "01/01/2026",
    };
    SMBIOS_TYPE0_BIOS_INFORMATION T;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 0, sizeof(T), 0x0000);
    T.Vendor = 1;
    T.BiosVersion = 2;
    T.BiosStartingAddressSegment = 0xe800;
    T.BiosReleaseDate = 3;
    T.BiosRomSize = 0;
    T.BiosCharacteristics = 0x08;
    T.BiosCharacteristicsExtensionBytes[1] = 0x18;
    T.SystemBiosMajorRelease = 0xff;
    T.SystemBiosMinorRelease = 0xff;
    T.EmbeddedControllerMajorRelease = 0xff;
    T.EmbeddedControllerMinorRelease = 0xff;
    return smbios_append_structure(&T, sizeof(T), Strings,
                                   FW_ARRAY_SIZE(Strings));
}

static BOOLEAN smbios_build_type1(void)
{
    static const CHAR8 * const Strings[] = {
        "QEMU",
        "IA-64 Virtual Platform",
        "1.0",
        "0",
        "IA64-VPC",
        "Virtual Machine",
    };
    SMBIOS_TYPE1_SYSTEM_INFORMATION T;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 1, sizeof(T), 0x0100);
    T.Manufacturer = 1;
    T.ProductName = 2;
    T.Version = 3;
    T.SerialNumber = 4;
    T.WakeUpType = 0x06;
    T.SkuNumber = 5;
    T.Family = 6;
    return smbios_append_structure(&T, sizeof(T), Strings,
                                   FW_ARRAY_SIZE(Strings));
}

static BOOLEAN smbios_build_type2(void)
{
    static const CHAR8 * const Strings[] = {
        "QEMU",
        "IA-64 Virtual Board",
        "1.0",
        "0",
        "0",
        "Mainboard",
    };
    SMBIOS_TYPE2_BASEBOARD_INFORMATION T;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 2, sizeof(T), 0x0200);
    T.Manufacturer = 1;
    T.Product = 2;
    T.Version = 3;
    T.SerialNumber = 4;
    T.AssetTag = 5;
    T.FeatureFlags = 0x01;
    T.LocationInChassis = 6;
    T.ChassisHandle = 0x0300;
    T.BoardType = 0x0a;
    T.ContainedObjectHandleCount = 0;
    return smbios_append_structure(&T, sizeof(T), Strings,
                                   FW_ARRAY_SIZE(Strings));
}

static BOOLEAN smbios_build_type3(void)
{
    static const CHAR8 * const Strings[] = {
        "QEMU",
        "1.0",
        "0",
        "0",
        "IA64-VPC",
    };
    SMBIOS_TYPE3_SYSTEM_ENCLOSURE T;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 3, sizeof(T), 0x0300);
    T.Manufacturer = 1;
    T.ChassisType = 0x01;
    T.Version = 2;
    T.SerialNumber = 3;
    T.AssetTag = 4;
    T.BootUpState = 0x03;
    T.PowerSupplyState = 0x03;
    T.ThermalState = 0x03;
    T.SecurityStatus = 0x02;
    T.SkuNumber = 5;
    return smbios_append_structure(&T, sizeof(T), Strings,
                                   FW_ARRAY_SIZE(Strings));
}

static BOOLEAN smbios_build_type4_socket(UINTN SocketIndex)
{
    CHAR8 SocketName[] = "CPU 0";
    const CHAR8 * const Strings[] = {
        SocketName,
        "QEMU",
        "IA-64",
        "0",
        "0",
        "0",
    };
    SMBIOS_TYPE4_PROCESSOR_INFORMATION T;
    UINTN threads_per_socket = fw_guest_cores_per_socket() * fw_guest_threads_per_core();
    UINTN first_processor = SocketIndex * threads_per_socket;
    UINTN enabled_threads = 0;
    UINTN enabled_cores;

    if (SocketIndex >= fw_guest_socket_count() || SocketIndex >= 10) {
        return 0;
    }
    SocketName[4] = (CHAR8)('0' + SocketIndex);
    if (first_processor < fw_guest_processor_count()) {
        enabled_threads = fw_guest_processor_count() - first_processor;
        if (enabled_threads > threads_per_socket) {
            enabled_threads = threads_per_socket;
        }
    }
    enabled_cores = (enabled_threads + fw_guest_threads_per_core() - 1U) /
                    fw_guest_threads_per_core();

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 4, sizeof(T),
                       0x0400U + (UINT16)SocketIndex);
    T.SocketDesignation = 1;
    T.ProcessorType = 0x03;
    T.ProcessorFamily = 0x82;
    T.ProcessorManufacturer = 2;
    T.ProcessorVersion = 3;
    T.Status = enabled_threads != 0 ? 0x41 : 0;
    T.ProcessorUpgrade = 0x01;
    T.L1CacheHandle = 0xffff;
    T.L2CacheHandle = 0xffff;
    T.L3CacheHandle = 0xffff;
    T.SerialNumber = 4;
    T.AssetTag = 5;
    T.PartNumber = 6;
    T.CoreCount = (UINT8)fw_guest_cores_per_socket();
    T.CoreEnabled = (UINT8)enabled_cores;
    T.ThreadCount = (UINT8)threads_per_socket;
    T.ProcessorCharacteristics = 0x0004 |
        (fw_guest_cores_per_socket() > 1 ? 0x0008 : 0) |
        (fw_guest_threads_per_core() > 1 ? 0x0010 : 0);
    T.ProcessorFamily2 = 0x0082;
    return smbios_append_structure(&T, sizeof(T), Strings,
                                   FW_ARRAY_SIZE(Strings));
}

static BOOLEAN smbios_build_type4(void)
{
    UINTN i;

    for (i = 0; i < fw_guest_socket_count(); i++) {
        if (!smbios_build_type4_socket(i)) {
            return 0;
        }
    }
    return 1;
}

static BOOLEAN smbios_build_type16(void)
{
    SMBIOS_TYPE16_PHYSICAL_MEMORY_ARRAY T;
    UINT64 SizeKb;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 16, sizeof(T), 0x1000);
    T.Location = 0x03;
    T.Use = 0x03;
    T.ErrorCorrection = 0x03;
    SizeKb = fw_guest_ram_size() / 1024U;
    if (SizeKb < 0x80000000ULL) {
        T.MaximumCapacity = (UINT32)SizeKb;
        T.ExtendedMaximumCapacity = 0;
    } else {
        T.MaximumCapacity = 0x80000000U;
        T.ExtendedMaximumCapacity = fw_guest_ram_size();
    }
    T.MemoryErrorInformationHandle = 0xfffe;
    T.NumberOfMemoryDevices = 1;
    return smbios_append_structure(&T, sizeof(T), NULL, 0);
}

static BOOLEAN smbios_build_type17(void)
{
    static const CHAR8 * const Strings[] = {
        "DIMM 0",
        "BANK 0",
    };
    SMBIOS_TYPE17_MEMORY_DEVICE T;
    UINT64 SizeMb;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 17, sizeof(T), 0x1100);
    T.PhysicalMemoryArrayHandle = 0x1000;
    T.MemoryErrorInformationHandle = 0xfffe;
    T.TotalWidth = 64;
    T.DataWidth = 64;
    SizeMb = (fw_guest_ram_size() + 0xfffffULL) >> 20;
    if (SizeMb < 0x7fffULL) {
        T.Size = (UINT16)SizeMb;
        T.ExtendedSize = 0;
    } else {
        T.Size = 0x7fff;
        T.ExtendedSize = (UINT32)SizeMb;
    }
    T.FormFactor = 0x09;
    T.DeviceSet = 0;
    T.DeviceLocator = 1;
    T.BankLocator = 2;
    T.MemoryType = 0x07;
    T.TypeDetail = 0x0002;
    T.Speed = 0;
    T.Attributes = 0;
    T.ConfiguredMemoryClockSpeed = 0;
    return smbios_append_structure(&T, sizeof(T), Strings,
                                   FW_ARRAY_SIZE(Strings));
}

static BOOLEAN smbios_build_type19_range(UINTN Index, UINT64 Base, UINT64 End)
{
    SMBIOS_TYPE19_MEMORY_ARRAY_MAPPED_ADDRESS T;
    UINT64 StartKb;
    UINT64 EndKb;

    if (End <= Base || (Base & 0x3ffULL) != 0 || (End & 0x3ffULL) != 0) {
        return 0;
    }

    StartKb = Base / 1024U;
    EndKb = (End - 1U) / 1024U;
    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 19, sizeof(T), 0x1300 + (UINT16)Index);
    if (EndKb < 0xffffffffULL) {
        T.StartingAddress = (UINT32)StartKb;
        T.EndingAddress = (UINT32)EndKb;
        T.ExtendedStartingAddress = 0;
        T.ExtendedEndingAddress = 0;
    } else {
        T.StartingAddress = 0xffffffffU;
        T.EndingAddress = 0xffffffffU;
        T.ExtendedStartingAddress = 0;
        T.ExtendedEndingAddress = End;
    }
    T.MemoryArrayHandle = 0x1000;
    T.PartitionWidth = 1;
    return smbios_append_structure(&T, sizeof(T), NULL, 0);
}

static BOOLEAN smbios_build_type19(void)
{
    UINTN i;

    if (fw_guest_low_ram_end() == 0 ||
        !smbios_build_type19_range(0, 0, fw_guest_low_ram_end())) {
        return 0;
    }
    for (i = 0; i < fw_guest_high_ram_count(); i++) {
        if (!smbios_build_type19_range(i + 1U, fw_guest_high_ram_base(i),
                                       fw_guest_high_ram_end(i))) {
            return 0;
        }
    }
    return 1;
}

static BOOLEAN smbios_build_type32(void)
{
    SMBIOS_TYPE32_SYSTEM_BOOT_INFORMATION T;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 32, sizeof(T), 0x2000);
    T.BootStatus = 0;
    return smbios_append_structure(&T, sizeof(T), NULL, 0);
}

static BOOLEAN smbios_build_type127(void)
{
    SMBIOS_TYPE127_END_OF_TABLE T;

    fw_set_mem(&T, sizeof(T), 0);
    smbios_header_init(&T.Hdr, 127, sizeof(T), 0x7f00);
    return smbios_append_structure(&T, sizeof(T), NULL, 0);
}

static void smbios_entry_point_checksum(void)
{
    mSmbiosEntryPoint.Checksum = 0;
    mSmbiosEntryPoint.IntermediateChecksum = 0;
    mSmbiosEntryPoint.IntermediateChecksum =
        table_checksum8((const UINT8 *)&mSmbiosEntryPoint + 0x10, 0x0f);
    mSmbiosEntryPoint.Checksum =
        table_checksum8(&mSmbiosEntryPoint, sizeof(mSmbiosEntryPoint));
}

void smbios_init_table(void)
{
    BOOLEAN Ok;
    UINTN i;

    fw_set_mem(mSmbiosTable, sizeof(mSmbiosTable), 0);
    fw_set_mem(&mSmbiosEntryPoint, sizeof(mSmbiosEntryPoint), 0);
    mSmbiosTableLength = 0;
    mSmbiosStructureCount = 0;
    mSmbiosMaxStructureSize = 0;

    Ok = smbios_build_type0() &&
         smbios_build_type1() &&
         smbios_build_type2() &&
         smbios_build_type3() &&
         smbios_build_type4() &&
         smbios_build_type16() &&
         smbios_build_type17() &&
         smbios_build_type19() &&
         smbios_build_type32() &&
         smbios_build_type127();

    if (!Ok || (UINTN)mSmbiosTable > 0xffffffffULL) {
        mSmbiosTableLength = 0;
        mSmbiosStructureCount = 0;
        mSmbiosMaxStructureSize = 0;
        return;
    }

    mSmbiosEntryPoint.AnchorString[0] = '_';
    mSmbiosEntryPoint.AnchorString[1] = 'S';
    mSmbiosEntryPoint.AnchorString[2] = 'M';
    mSmbiosEntryPoint.AnchorString[3] = '_';
    mSmbiosEntryPoint.Length = sizeof(mSmbiosEntryPoint);
    mSmbiosEntryPoint.MajorVersion = 2;
    mSmbiosEntryPoint.MinorVersion = 7;
    mSmbiosEntryPoint.MaxStructureSize = mSmbiosMaxStructureSize;
    mSmbiosEntryPoint.EntryPointRevision = 0;
    for (i = 0; i < sizeof(mSmbiosEntryPoint.FormattedArea); i++) {
        mSmbiosEntryPoint.FormattedArea[i] = 0;
    }
    mSmbiosEntryPoint.IntermediateAnchorString[0] = '_';
    mSmbiosEntryPoint.IntermediateAnchorString[1] = 'D';
    mSmbiosEntryPoint.IntermediateAnchorString[2] = 'M';
    mSmbiosEntryPoint.IntermediateAnchorString[3] = 'I';
    mSmbiosEntryPoint.IntermediateAnchorString[4] = '_';
    mSmbiosEntryPoint.StructureTableLength = mSmbiosTableLength;
    mSmbiosEntryPoint.StructureTableAddress = (UINT32)(UINTN)mSmbiosTable;
    mSmbiosEntryPoint.NumberOfStructures = mSmbiosStructureCount;
    mSmbiosEntryPoint.BcdRevision = 0x27;
    smbios_entry_point_checksum();
}

static UINTN smbios_structure_size(const UINT8 *Data, UINTN Remaining)
{
    UINTN Pos;

    if (Remaining < sizeof(SMBIOS_STRUCTURE_HEADER) ||
        Data[1] < sizeof(SMBIOS_STRUCTURE_HEADER) ||
        Data[1] > Remaining) {
        return 0;
    }

    Pos = Data[1];
    while (Pos + 1U < Remaining) {
        if (Data[Pos] == 0 && Data[Pos + 1U] == 0) {
            return Pos + 2U;
        }
        Pos++;
    }
    return 0;
}

static UINT16 smbios_get_u16(const UINT8 *Data, UINTN Offset)
{
    return (UINT16)((UINT16)Data[Offset] |
                    ((UINT16)Data[Offset + 1U] << 8));
}

static UINT32 smbios_get_u32(const UINT8 *Data, UINTN Offset)
{
    return (UINT32)Data[Offset] |
           ((UINT32)Data[Offset + 1U] << 8) |
           ((UINT32)Data[Offset + 2U] << 16) |
           ((UINT32)Data[Offset + 3U] << 24);
}

static UINT64 smbios_get_u64(const UINT8 *Data, UINTN Offset)
{
    return (UINT64)smbios_get_u32(Data, Offset) |
           ((UINT64)smbios_get_u32(Data, Offset + 4U) << 32);
}

static BOOLEAN smbios_type19_range_matches(const UINT8 *Data, UINT64 Base,
                                           UINT64 End)
{
    UINT64 start_kb = Base / 1024U;
    UINT64 end_kb = (End - 1U) / 1024U;

    if (End <= Base || smbios_get_u16(Data, 0x0c) != 0x1000 ||
        Data[0x0e] != 1) {
        return 0;
    }
    if (end_kb < 0xffffffffULL) {
        return smbios_get_u32(Data, 4) == start_kb &&
               smbios_get_u32(Data, 8) == end_kb &&
               smbios_get_u64(Data, 0x0f) == 0 &&
               smbios_get_u64(Data, 0x17) == 0;
    }
    return smbios_get_u32(Data, 4) == 0xffffffffU &&
           smbios_get_u32(Data, 8) == 0xffffffffU &&
           smbios_get_u64(Data, 0x0f) == Base &&
           smbios_get_u64(Data, 0x17) == (End - 1U);
}

BOOLEAN __attribute__((noinline)) smbios_table_integrity_selftest(void)
{
    UINTN Offset = 0;
    UINTN Count = 0;
    UINTN MaxSize = 0;
    UINTN Size;
    UINTN Type4Count = 0;
    UINTN Type19Count = 0;
    UINT16 RequiredTypes = 0;

    if (mSmbiosEntryPoint.AnchorString[0] != '_' ||
        mSmbiosEntryPoint.AnchorString[1] != 'S' ||
        mSmbiosEntryPoint.AnchorString[2] != 'M' ||
        mSmbiosEntryPoint.AnchorString[3] != '_' ||
        mSmbiosEntryPoint.Length != sizeof(mSmbiosEntryPoint) ||
        mSmbiosEntryPoint.MajorVersion != 2 ||
        mSmbiosEntryPoint.MinorVersion != 7 ||
        mSmbiosEntryPoint.EntryPointRevision != 0 ||
        mSmbiosEntryPoint.IntermediateAnchorString[0] != '_' ||
        mSmbiosEntryPoint.IntermediateAnchorString[1] != 'D' ||
        mSmbiosEntryPoint.IntermediateAnchorString[2] != 'M' ||
        mSmbiosEntryPoint.IntermediateAnchorString[3] != 'I' ||
        mSmbiosEntryPoint.IntermediateAnchorString[4] != '_' ||
        mSmbiosEntryPoint.StructureTableAddress != (UINT32)(UINTN)mSmbiosTable ||
        mSmbiosEntryPoint.StructureTableLength != mSmbiosTableLength ||
        mSmbiosEntryPoint.NumberOfStructures != mSmbiosStructureCount ||
        mSmbiosEntryPoint.BcdRevision != 0x27 ||
        table_checksum8(&mSmbiosEntryPoint, mSmbiosEntryPoint.Length) != 0 ||
        table_checksum8((const UINT8 *)&mSmbiosEntryPoint + 0x10, 0x0f) != 0) {
        return 0;
    }

    while (Offset < mSmbiosTableLength) {
        const UINT8 *Data = &mSmbiosTable[Offset];
        UINT8 Type = Data[0];
        UINT8 Length = Data[1];

        Size = smbios_structure_size(Data, mSmbiosTableLength - Offset);
        if (Size == 0) {
            return 0;
        }
        if (Size > MaxSize) {
            MaxSize = Size;
        }
        Count++;

        switch (Type) {
        case 0:
            if (Length != sizeof(SMBIOS_TYPE0_BIOS_INFORMATION) ||
                Data[4] != 1 || Data[5] != 2 || Data[8] != 3 ||
                Data[0x13] != 0x18) {
                return 0;
            }
            RequiredTypes |= 0x001;
            break;
        case 1:
            if (Length != sizeof(SMBIOS_TYPE1_SYSTEM_INFORMATION) ||
                Data[4] != 1 || Data[5] != 2 || Data[0x18] != 0x06) {
                return 0;
            }
            RequiredTypes |= 0x002;
            break;
        case 2:
            if (Length != sizeof(SMBIOS_TYPE2_BASEBOARD_INFORMATION) ||
                smbios_get_u16(Data, 0x0b) != 0x0300 ||
                Data[0x0d] != 0x0a) {
                return 0;
            }
            RequiredTypes |= 0x004;
            break;
        case 3:
            if (Length != sizeof(SMBIOS_TYPE3_SYSTEM_ENCLOSURE) ||
                Data[5] != 0x01 || Data[0x0c] != 0x02) {
                return 0;
            }
            RequiredTypes |= 0x008;
            break;
        case 4: {
            UINTN threads_per_socket =
                fw_guest_cores_per_socket() * fw_guest_threads_per_core();
            UINTN first_processor = Type4Count * threads_per_socket;
            UINTN enabled_threads = 0;
            UINTN enabled_cores;
            UINT16 characteristics = 0x0004U |
                (fw_guest_cores_per_socket() > 1 ? 0x0008U : 0) |
                (fw_guest_threads_per_core() > 1 ? 0x0010U : 0);

            if (first_processor < fw_guest_processor_count()) {
                enabled_threads = fw_guest_processor_count() - first_processor;
                if (enabled_threads > threads_per_socket) {
                    enabled_threads = threads_per_socket;
                }
            }
            enabled_cores = (enabled_threads + fw_guest_threads_per_core() - 1U) /
                            fw_guest_threads_per_core();
            if (Length != sizeof(SMBIOS_TYPE4_PROCESSOR_INFORMATION) ||
                Type4Count >= fw_guest_socket_count() ||
                smbios_get_u16(Data, 2) != 0x0400U + Type4Count ||
                Data[4] != 1 ||
                Data[5] != 0x03 || Data[6] != 0x82 ||
                Data[0x18] != (enabled_threads != 0 ? 0x41 : 0) ||
                Data[0x23] != fw_guest_cores_per_socket() ||
                Data[0x24] != enabled_cores ||
                Data[0x25] != threads_per_socket ||
                smbios_get_u16(Data, 0x26) != characteristics ||
                smbios_get_u16(Data, 0x28) != 0x0082) {
                return 0;
            }
            Type4Count++;
            RequiredTypes |= 0x010;
            break;
        }
        case 16:
            if (Length != sizeof(SMBIOS_TYPE16_PHYSICAL_MEMORY_ARRAY) ||
                Data[4] != 0x03 || Data[5] != 0x03 || Data[6] != 0x03 ||
                smbios_get_u16(Data, 0x0d) != 1 ||
                (fw_guest_ram_size() < 0x20000000000ULL &&
                 smbios_get_u32(Data, 7) != fw_guest_ram_size() / 1024U) ||
                (fw_guest_ram_size() >= 0x20000000000ULL &&
                 (smbios_get_u32(Data, 7) != 0x80000000U ||
                  smbios_get_u64(Data, 0x0f) != fw_guest_ram_size()))) {
                return 0;
            }
            RequiredTypes |= 0x020;
            break;
        case 17: {
            UINT64 size_mb = (fw_guest_ram_size() + 0xfffffULL) >> 20;

            if (Length != sizeof(SMBIOS_TYPE17_MEMORY_DEVICE) ||
                smbios_get_u16(Data, 4) != 0x1000 ||
                Data[0x0e] != 0x09 || Data[0x12] != 0x07) {
                return 0;
            }
            if (size_mb < 0x7fffULL) {
                if (smbios_get_u16(Data, 0x0c) != size_mb ||
                    smbios_get_u32(Data, 0x1c) != 0) {
                    return 0;
                }
            } else if (smbios_get_u16(Data, 0x0c) != 0x7fff ||
                       smbios_get_u32(Data, 0x1c) != (UINT32)size_mb) {
                return 0;
            }
            RequiredTypes |= 0x040;
            break;
        }
        case 19:
            if (Length != sizeof(SMBIOS_TYPE19_MEMORY_ARRAY_MAPPED_ADDRESS) ||
                Type19Count > fw_guest_high_ram_count()) {
                return 0;
            }
            if (Type19Count == 0) {
                if (!smbios_type19_range_matches(Data, 0, fw_guest_low_ram_end())) {
                    return 0;
                }
            } else if (!smbios_type19_range_matches(
                           Data, fw_guest_high_ram_base(Type19Count - 1U),
                           fw_guest_high_ram_end(Type19Count - 1U))) {
                return 0;
            }
            Type19Count++;
            RequiredTypes |= 0x080;
            break;
        case 32:
            if (Length != sizeof(SMBIOS_TYPE32_SYSTEM_BOOT_INFORMATION) ||
                Data[0x0a] != 0) {
                return 0;
            }
            RequiredTypes |= 0x100;
            break;
        case 127:
            if (Length != sizeof(SMBIOS_TYPE127_END_OF_TABLE) ||
                Offset + Size != mSmbiosTableLength) {
                return 0;
            }
            RequiredTypes |= 0x200;
            break;
        default:
            break;
        }
        Offset += Size;
    }

    return Offset == mSmbiosTableLength &&
           Count == mSmbiosStructureCount &&
           MaxSize == mSmbiosEntryPoint.MaxStructureSize &&
           Type4Count == fw_guest_socket_count() &&
           Type19Count == 1U + fw_guest_high_ram_count() &&
           RequiredTypes == 0x3ff;
}


UINTN fw_smbios_entry_point_address(void)
{
    return (UINTN)&mSmbiosEntryPoint;
}
