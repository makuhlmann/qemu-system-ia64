/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "ia64-test.h"

static UINT8 loaded_image_guid[16] = IA64_GUID_LOADED_IMAGE;
static UINT8 device_path_guid[16] = IA64_GUID_DEVICE_PATH;
static UINT8 acpi20_guid[16] = IA64_GUID_ACPI20;
static UINT8 pci_root_guid[16] = IA64_GUID_PCI_ROOT_IO;

#define SMOKE_LEGACY_IO_BASE 0x00000ffffc000000ULL
#define SMOKE_SCI_EN_POLLS   1000000U
/* The GXB's function 1, device 14h on the chipset bus (CBN, EEh after POST). */
#define SMOKE_GXB(reg) \
    ((0xeeULL << 24) | (0x14ULL << 16) | (1ULL << 8) | (reg))
#define SMOKE_GXB_ID         0x84ea8086U
#define SMOKE_APERTURE_SIZE  0x10000000ULL

static UINT64 smoke_get(const UINT8 *Address, UINTN Size)
{
    UINT64 value = 0;

    ia64_copy(&value, Address, Size);
    return value;
}

static const UINT8 *smoke_find_fadt(EFI_SYSTEM_TABLE *SystemTable)
{
    const UINT8 *rsdp = NULL;
    const UINT8 *xsdt;
    UINT32 length;
    UINTN i;

    for (i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        if (ia64_bytes_equal(SystemTable->ConfigurationTable[i].VendorGuid,
                             acpi20_guid, sizeof(acpi20_guid))) {
            rsdp = (const UINT8 *)
                SystemTable->ConfigurationTable[i].VendorTable;
        }
    }
    if (rsdp == NULL) {
        return NULL;
    }
    xsdt = (const UINT8 *)(UINTN)smoke_get(rsdp + 24, 8);
    length = smoke_get(xsdt + 4, 4);
    for (i = 36; i + 8 <= length; i += 8) {
        const UINT8 *table = (const UINT8 *)(UINTN)smoke_get(xsdt + i, 8);

        if (ia64_bytes_equal(table, "FACP", 4)) {
            return table;
        }
    }
    return NULL;
}

static BOOLEAN smoke_wait_sci_en(UINT32 Pm1aCnt, UINT16 Value)
{
    volatile UINT16 *cnt =
        (volatile UINT16 *)(UINTN)(SMOKE_LEGACY_IO_BASE + Pm1aCnt);
    UINTN i;

    for (i = 0; i < SMOKE_SCI_EN_POLLS; i++) {
        if ((*cnt & 1U) == Value) {
            return 1;
        }
    }
    return 0;
}

/*
 * What an OS does to take the ACPI hardware from the platform, by the FADT's
 * SMI_CMD, ACPI_ENABLE and ACPI_DISABLE: write ACPI_ENABLE, then wait for
 * SCI_EN; ACPI_DISABLE hands it back.  A zero SMI_CMD means there is nothing
 * to switch.
 */
static BOOLEAN smoke_acpi_enable(EFI_SYSTEM_TABLE *SystemTable)
{
    const UINT8 *fadt = smoke_find_fadt(SystemTable);
    UINT32 smi_cmd;
    UINT32 pm1a_cnt;
    volatile UINT8 *port;

    if (fadt == NULL) {
        return 0;
    }
    smi_cmd = smoke_get(fadt + 48, 4);
    pm1a_cnt = smoke_get(fadt + 64, 4);
    if (smi_cmd == 0) {
        return 1;
    }
    if (smi_cmd >= 0x1000 || pm1a_cnt >= 0x1000) {
        return 0;
    }
    port = (volatile UINT8 *)(UINTN)(SMOKE_LEGACY_IO_BASE + smi_cmd);
    *port = fadt[52];
    if (!smoke_wait_sci_en(pm1a_cnt, 1)) {
        return 0;
    }
    *port = fadt[53];
    return smoke_wait_sci_en(pm1a_cnt, 0);
}

static BOOLEAN smoke_range_free(EFI_SYSTEM_TABLE *SystemTable, UINT64 Base,
                                UINT64 End)
{
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN map_size = 0, key, descriptor_size = 0;
    UINT32 version;
    BOOLEAN ok;
    UINTN i;

    if (bs->GetMemoryMap(&map_size, NULL, &key, &descriptor_size,
                         &version) != EFI_BUFFER_TOO_SMALL ||
        descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        return 0;
    }
    map_size += 4U * descriptor_size;
    if (bs->AllocatePool(EfiLoaderData, map_size, (VOID **)&map) !=
        EFI_SUCCESS) {
        return 0;
    }
    ok = bs->GetMemoryMap(&map_size, map, &key, &descriptor_size,
                          &version) == EFI_SUCCESS;
    for (i = 0; ok && i < map_size / descriptor_size; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)
            ((UINT8 *)map + i * descriptor_size);
        UINT64 end = d->PhysicalStart + (d->NumberOfPages << 12);

        ok = d->PhysicalStart >= End || end <= Base;
    }
    (void)bs->FreePool(map);
    return ok;
}

/*
 * The 460GX's GXB (8086:84EA): with its GART SRAM found, the firmware
 * gives it a 256 MB aperture from BAPBASE, on a 256 MB boundary (AGP_BASE
 * bits 27:12 are hardwired, SSDM 7.1), clear of DRAM and of everything else
 * the memory map describes (7.2.3).  A board without it passes.
 */
static BOOLEAN smoke_agp_aperture(EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root = NULL;
    UINT32 id = 0, low = 0, high = 0;
    UINT8 size = 0;
    UINT64 base;

    if (SystemTable->BootServices->LocateProtocol(
            pci_root_guid, NULL, (VOID **)&root) != EFI_SUCCESS ||
        root == NULL ||
        root->Pci.Read(root, EfiPciWidthUint32, SMOKE_GXB(0), 1, &id) !=
            EFI_SUCCESS) {
        return 0;
    }
    if (id != SMOKE_GXB_ID) {
        return 1;
    }
    if (root->Pci.Read(root, EfiPciWidthUint8, SMOKE_GXB(0xa2), 1, &size) !=
            EFI_SUCCESS ||
        root->Pci.Read(root, EfiPciWidthUint32, SMOKE_GXB(0x98), 1, &low) !=
            EFI_SUCCESS ||
        root->Pci.Read(root, EfiPciWidthUint32, SMOKE_GXB(0x9c), 1, &high) !=
            EFI_SUCCESS) {
        return 0;
    }
    base = (((UINT64)high << 32) | low) & ~0xfULL;
    return (size & 0x0fU) == 0x09U && base != 0 &&
           (base & (SMOKE_APERTURE_SIZE - 1U)) == 0 &&
           smoke_range_free(SystemTable, base, base + SMOKE_APERTURE_SIZE);
}

static BOOLEAN system_table_crc_valid(EFI_SYSTEM_TABLE *SystemTable)
{
    UINT8 copy[256];
    UINT32 crc = 0;
    UINT32 expected;
    UINTN size;

    if (SystemTable == NULL || SystemTable->BootServices == NULL ||
        SystemTable->BootServices->CalculateCrc32 == NULL ||
        SystemTable->Hdr.HeaderSize > sizeof(copy) ||
        SystemTable->Hdr.HeaderSize < sizeof(EFI_TABLE_HEADER)) {
        return 0;
    }
    size = SystemTable->Hdr.HeaderSize;
    expected = SystemTable->Hdr.CRC32;
    ia64_copy(copy, SystemTable, size);
    ((EFI_TABLE_HEADER *)copy)->CRC32 = 0;
    return SystemTable->BootServices->CalculateCrc32(copy, size, &crc) ==
               EFI_SUCCESS &&
           crc == expected;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    IA64_TEST_CONTEXT context = {
        .SystemTable = SystemTable,
        .Suite = "smoke",
        .Passed = 0,
        .Failed = 0,
        .DirectUart = 0,
    };
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    VOID *device_path = NULL;
    EFI_STATUS status;

    ia64_test_pass(&context, "entry");
    ia64_test_check(
        &context, "system-table",
        SystemTable != NULL &&
            SystemTable->Hdr.Signature == EFI_SYSTEM_TABLE_SIGNATURE &&
            SystemTable->BootServices != NULL &&
            SystemTable->BootServices->Hdr.Signature ==
                EFI_BOOT_SERVICES_SIGNATURE &&
            SystemTable->RuntimeServices != NULL &&
            SystemTable->RuntimeServices->Hdr.Signature ==
                EFI_RUNTIME_SERVICES_SIGNATURE &&
            system_table_crc_valid(SystemTable),
        EFI_DEVICE_ERROR, "signature-or-crc");

    status = SystemTable->BootServices->HandleProtocol(
        ImageHandle, loaded_image_guid, (VOID **)&loaded);
    ia64_test_check(&context, "loaded-image",
                    status == EFI_SUCCESS && loaded != NULL &&
                        loaded->DeviceHandle != NULL &&
                        loaded->ImageBase != NULL && loaded->ImageSize != 0,
                    status, "handle-protocol");

    if (loaded != NULL && loaded->DeviceHandle != NULL) {
        status = SystemTable->BootServices->HandleProtocol(
            loaded->DeviceHandle, device_path_guid, &device_path);
    } else {
        status = EFI_NOT_FOUND;
    }
    ia64_test_check(&context, "device-path",
                    status == EFI_SUCCESS && device_path != NULL,
                    status, "device-path-protocol");

    ia64_test_check(&context, "acpi-enable", smoke_acpi_enable(SystemTable),
                    EFI_DEVICE_ERROR, "sci-en");

    ia64_test_check(&context, "agp-aperture", smoke_agp_aperture(SystemTable),
                    EFI_DEVICE_ERROR, "bapbase");

    ia64_test_check(&context, "console-output",
                    SystemTable != NULL && SystemTable->ConOut != NULL &&
                        SystemTable->ConOut->OutputString != NULL,
                    EFI_DEVICE_ERROR, "missing-conout");
    ia64_test_done(&context);
    return context.Failed == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS (*efi_entry_descriptor_reference)(EFI_HANDLE, EFI_SYSTEM_TABLE *)
    __attribute__((used)) = efi_main;
