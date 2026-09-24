/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Boot policy: BootOrder/Boot#### evaluation, BootCurrent maintenance,
 * the disk fallback boot, and the boot-shell console services.
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-boot-shell.h"
#include "fw-storage.h"
#include "fw-fs.h"

/* --- Updated boot path using FAT + Block I/O ----------------------------- */

void fw_boot_option_name(UINT16 Option, CHAR16 Name[9])
{
    static const CHAR16 hex[] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
    };

    Name[0] = 'B';
    Name[1] = 'o';
    Name[2] = 'o';
    Name[3] = 't';
    Name[4] = hex[(Option >> 12) & 0xf];
    Name[5] = hex[(Option >> 8) & 0xf];
    Name[6] = hex[(Option >> 4) & 0xf];
    Name[7] = hex[Option & 0xf];
    Name[8] = 0;
}

UINTN fw_load_option_description_size(const UINT8 *Option,
                                      UINTN OptionSize)
{
    UINTN offset = sizeof(UINT32) + sizeof(UINT16);

    while (offset + sizeof(CHAR16) <= OptionSize) {
        CHAR16 ch;

        fw_copy_mem(&ch, (VOID *)(Option + offset), sizeof(ch));
        offset += sizeof(ch);
        if (ch == 0) {
            return offset - (sizeof(UINT32) + sizeof(UINT16));
        }
    }
    return 0;
}

static EFI_STATUS fw_set_boot_current(UINT16 OptionNumber)
{
    static CHAR16 name[] = {
        'B', 'o', 'o', 't', 'C', 'u', 'r', 'r', 'e', 'n', 't', 0
    };

    return rs_set_variable(name, (void *)mEfiGlobalVariableGuid,
                           EFI_VARIABLE_BOOTSERVICE_ACCESS |
                           EFI_VARIABLE_RUNTIME_ACCESS,
                           sizeof(OptionNumber), &OptionNumber);
}

static void fw_clear_boot_current(void)
{
    static CHAR16 name[] = {
        'B', 'o', 'o', 't', 'C', 'u', 'r', 'r', 'e', 'n', 't', 0
    };

    (void)rs_set_variable(name, (void *)mEfiGlobalVariableGuid, 0, 0, NULL);
}

/*
 * The firmware's own "Removable Media Boot" option names the boot device by a
 * fixed-shape path -- a SCSI or ATAPI node, then a CD-ROM node, then the
 * file -- and LoadImage resolves it only when the boot Block I/O handle's path
 * is a prefix of it and that handle carries the file system.  On a
 * partitioned disk the file system is on a partition handle, and an AHCI
 * handle's path has a SATA node, so there LoadImage finds nothing.  EFI 1.10
 * 3.4.1.1 has the firmware find the file system on the removable device, so
 * look for the option's file on each file system of the boot device.
 */
static BOOLEAN fw_is_removable_media_path(const FW_DEVICE_PATH_NODE *FilePath,
                                          UINTN Size)
{
    const UINT8 *a = (const UINT8 *)FilePath;
    const UINT8 *b = (const UINT8 *)&mOpticalSetupLoaderDevicePath;
    UINTN i;

    if (Size != sizeof(mOpticalSetupLoaderDevicePath)) {
        return 0;
    }
    for (i = 0; i < Size; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static EFI_STATUS fw_load_removable_media_image(FW_DEVICE_PATH_NODE *FilePath,
                                                EFI_HANDLE *Image)
{
    static UINT8 full_path[256];
    FW_DEVICE_PATH_NODE *file_node;
    EFI_HANDLE volume;
    EFI_STATUS st = EFI_NOT_FOUND;
    UINTN i;

    file_node = (FW_DEVICE_PATH_NODE *)fw_loaded_image_file_path(FilePath);
    if (file_node == NULL || file_node->Type != 0x04 ||
        file_node->SubType != 0x04) {
        return EFI_NOT_FOUND;
    }
    for (i = 0; (volume = fw_boot_media_file_system(i)) != NULL; i++) {
        if (fw_build_file_device_path(volume, file_node, full_path,
                                      sizeof(full_path)) != EFI_SUCCESS) {
            continue;
        }
        st = mBootServices.LoadImage(1, mImageHandle, full_path,
                                     NULL, 0, Image);
        if (st == EFI_SUCCESS) {
            break;
        }
    }
    return st;
}

static EFI_STATUS boot_image_from_load_option(UINT16 OptionNumber,
                                              const UINT8 *Option,
                                              UINTN OptionSize)
{
    UINT32 attributes;
    UINT16 file_path_list_length;
    UINTN description_size;
    UINTN file_path_offset;
    UINTN optional_data_offset;
    FW_DEVICE_PATH_NODE *file_path;
    VOID *optional_data;
    VOID *load_options = NULL;
    EFI_HANDLE image = NULL;
    EFI_STATUS st;

    if (Option == NULL ||
        OptionSize < sizeof(UINT32) + sizeof(UINT16) + sizeof(CHAR16)) {
        return EFI_LOAD_ERROR;
    }

    fw_copy_mem(&attributes, (VOID *)Option, sizeof(attributes));
    fw_copy_mem(&file_path_list_length,
                (VOID *)(Option + sizeof(UINT32)),
                sizeof(file_path_list_length));
    if ((attributes & 0x00000001U) == 0) {
        return EFI_NOT_FOUND;
    }

    description_size = fw_load_option_description_size(Option, OptionSize);
    if (description_size == 0) {
        return EFI_LOAD_ERROR;
    }
    file_path_offset = sizeof(UINT32) + sizeof(UINT16) + description_size;
    optional_data_offset = file_path_offset + file_path_list_length;
    if (file_path_list_length < sizeof(FW_DEVICE_PATH_NODE) ||
        optional_data_offset > OptionSize) {
        return EFI_LOAD_ERROR;
    }

    file_path = (FW_DEVICE_PATH_NODE *)(VOID *)(Option + file_path_offset);
    if (fw_device_path_size(file_path) != file_path_list_length) {
        return EFI_LOAD_ERROR;
    }

    uart_puts("Boot Manager:        trying Boot");
    uart_put_hex64(OptionNumber);
    uart_puts("\r\n");

    /* The built-in EFI shell is a Boot#### whose file path is a lone
     * HW_VENDOR node with our shell GUID (mirroring the sample's
     * "EFI Shell [Built-in]" entry).  It has no image to load — hand off
     * to the interactive shell directly. */
    if (fw_device_path_is_internal_shell(file_path)) {
        (void)fw_set_boot_current(OptionNumber);
        fw_boot_shell_run();
        return EFI_SUCCESS;
    }

    st = mBootServices.LoadImage(1, mImageHandle, file_path,
                                 NULL, 0, &image);
    if (st == EFI_NOT_FOUND &&
        fw_is_removable_media_path(file_path, file_path_list_length)) {
        st = fw_load_removable_media_image(file_path, &image);
    }
    if (st != EFI_SUCCESS) {
        return st;
    }

    if (optional_data_offset < OptionSize) {
        optional_data = (VOID *)(Option + optional_data_offset);
        st = fw_copy_loaded_image_load_options(
            image, optional_data, (UINT32)(OptionSize - optional_data_offset),
            &load_options);
        if (st != EFI_SUCCESS) {
            (void)mBootServices.UnloadImage(image);
            return st;
        }
    }

    st = fw_set_boot_current(OptionNumber);
    if (st != EFI_SUCCESS) {
        if (load_options != NULL) {
            (void)fw_release_loaded_image_load_options(image, load_options);
        }
        (void)mBootServices.UnloadImage(image);
        return st;
    }

    (void)mBootServices.SetWatchdogTimer(300, 0, 0, NULL);
    mSalLoaderHandoffPending = 1;
    st = mBootServices.StartImage(image, NULL, NULL);
    mSalLoaderHandoffPending = 0;
    (void)mBootServices.SetWatchdogTimer(0, 0, 0, NULL);

    uart_puts("Boot Manager:        StartImage returned status=0x");
    uart_put_hex64(st);
    uart_puts("\r\n");

    if (!mBootServicesExited) {
        if (st != EFI_SUCCESS) {
            fw_clear_boot_current();
        }
        if (load_options != NULL) {
            (void)fw_release_loaded_image_load_options(image, load_options);
        }
        (void)mBootServices.UnloadImage(image);
    }
    return st;
}

EFI_STATUS fw_boot_image_from_boot_option(UINT16 OptionNumber)
{
    /* Do not retain a large firmware stack frame across StartImage(). */
    static UINT8 option[NVRAM_VAR_DATA_MAX];
    CHAR16 name[9];
    UINTN option_size = sizeof(option);
    UINT32 attributes = 0;
    EFI_STATUS st;

    fw_boot_option_name(OptionNumber, name);
    st = rs_get_variable(name, (void *)mEfiGlobalVariableGuid,
                         &attributes, &option_size, option);
    if (st != EFI_SUCCESS) {
        return st;
    }
    if ((attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0) {
        return EFI_LOAD_ERROR;
    }
    return boot_image_from_load_option(OptionNumber, option, option_size);
}

/*
 * BootNext names the option for this boot only.  Delete it, then boot that
 * option; the boot manager does this before the BootOrder list and before
 * the menu, whatever the Timeout (as the EFI 1.10 sample boot manager does).
 * EFI_NOT_FOUND: BootNext is not set.
 */
static EFI_STATUS __attribute__((noinline)) fw_boot_next_option(void)
{
    static CHAR16 boot_next_name[] = {
        'B', 'o', 'o', 't', 'N', 'e', 'x', 't', 0
    };
    UINT16 boot_next;
    UINTN boot_next_size = sizeof(boot_next);
    UINT32 attributes = 0;

    if (rs_get_variable(boot_next_name, (void *)mEfiGlobalVariableGuid,
                        &attributes, &boot_next_size, &boot_next) !=
            EFI_SUCCESS ||
        boot_next_size != sizeof(boot_next) ||
        (attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0) {
        return EFI_NOT_FOUND;
    }
    /* A one-shot request is consumed before it is used. */
    (void)rs_set_variable(boot_next_name, (void *)mEfiGlobalVariableGuid,
                          0, 0, NULL);
    (void)fw_console_clear();
    return fw_boot_image_from_boot_option(boot_next);
}

/* Keep address-taken buffers in a bounded IA-64 register-stack frame. */
EFI_STATUS __attribute__((noinline)) boot_image_from_boot_order(void)
{
    static CHAR16 boot_order_name[] = {
        'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0
    };
    UINT16 order[16];
    UINTN order_size = sizeof(order);
    UINT32 attributes = 0;
    EFI_STATUS st;
    EFI_STATUS last;
    UINTN i;

    last = fw_boot_next_option();
    if (last == EFI_SUCCESS || mBootServicesExited) {
        return last;
    }

    st = rs_get_variable(boot_order_name, (void *)mEfiGlobalVariableGuid,
                         &attributes, &order_size, order);
    if (st != EFI_SUCCESS) {
        return st;
    }
    if ((attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0 ||
        (order_size % sizeof(UINT16)) != 0) {
        return EFI_LOAD_ERROR;
    }

    for (i = 0; i < order_size / sizeof(UINT16); i++) {
        last = fw_boot_image_from_boot_option(order[i]);
        if (last == EFI_SUCCESS || mBootServicesExited) {
            return last;
        }
    }
    return last;
}

EFI_STATUS __attribute__((noinline)) boot_image_from_disk(void)
{
    static CHAR16 boot_path[] = {
        '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\',
        'B', 'O', 'O', 'T', 'I', 'A', '6', '4', '.', 'E', 'F', 'I', 0
    };
    FAT_DIR_ENTRY boot_entry;
    static UINT8 full_path[256];
    VOID *file_buf = NULL;
    UINT32 file_size = 0;
    EFI_HANDLE image = NULL;
    EFI_STATUS st;

    uart_puts("Block I/O: locating \\EFI\\BOOT\\BOOTIA64.EFI...\r\n");
    st = fw_fat_lookup(boot_path, &boot_entry);
    if (st != EFI_SUCCESS) {
        uart_puts("Block I/O: BOOTIA64.EFI not found\r\n");
        return st;
    }
    if (boot_entry.size == 0) {
        uart_puts("Block I/O: BOOTIA64.EFI is empty\r\n");
        return EFI_LOAD_ERROR;
    }

    /*
     * Stage the file in a tracked allocation: it bounds the read to the
     * directory entry's size, and it keeps the staging window out of the
     * conventional memory that LoadImage() places the loaded image in.
     */
    st = bs_allocate_pool(EfiBootServicesData, boot_entry.size, &file_buf);
    if (st != EFI_SUCCESS) {
        uart_puts("Block I/O: BOOTIA64.EFI staging failed\r\n");
        return st;
    }

    st = fw_fat_read_file_entry(&boot_entry, file_buf, &file_size);
    if (st != EFI_SUCCESS || file_size != boot_entry.size) {
        uart_puts("Block I/O: BOOTIA64.EFI read failed\r\n");
        (void)bs_free_pool(file_buf);
        return st != EFI_SUCCESS ? st : EFI_DEVICE_ERROR;
    }

    uart_puts("Block I/O: BOOTIA64.EFI loaded\r\n");

    if (mDefaultFatVolume == NULL || !mDefaultFatVolume->valid) {
        (void)bs_free_pool(file_buf);
        return EFI_NOT_FOUND;
    }
    st = fw_build_file_device_path(
        mDefaultFatVolume->handle,
        (FW_DEVICE_PATH_NODE *)&mBootFullDevicePath.FileHeader,
        full_path, sizeof(full_path));
    if (st != EFI_SUCCESS) {
        (void)bs_free_pool(file_buf);
        return st;
    }
    st = mBootServices.LoadImage(1, mImageHandle, full_path,
                                 file_buf, file_size, &image);
    (void)bs_free_pool(file_buf);
    if (st != EFI_SUCCESS) {
        uart_puts("Block I/O: LoadImage failed\r\n");
        return st;
    }

    /* Generic optical boot: launch with empty load options (OS-agnostic). */
    mSalLoaderHandoffPending = 1;
    st = mBootServices.StartImage(image, NULL, NULL);
    mSalLoaderHandoffPending = 0;
    uart_puts("Block I/O: StartImage returned\r\n");
    if (!mBootServicesExited) {
        (void)mBootServices.UnloadImage(image);
    }
    return st;
}

/* --- Interactive boot menu ------------------------------------------------ */

/*
 * A keyboard-navigable boot manager, in the spirit of the Intel EFI sample's
 * bootmgr: it lists the active Boot#### options in BootOrder order plus the
 * built-in EFI Shell and Boot Maintenance actions, honours the EFI Timeout
 * variable with a visible countdown that auto-boots the highlighted default,
 * and hands a chosen option to the existing boot engine
 * (fw_boot_image_from_boot_option).  Built entirely on SimpleTextOut cursor/
 * attribute control and SimpleTextIn ReadKeyStroke -- no new platform service.
 */

#define FW_MENU_MAX          20U
#define FW_MENU_DESC_MAX     58U

#define FW_MENU_ATTR_NORMAL   0x07U  /* light grey on black           */
#define FW_MENU_ATTR_SELECTED 0x70U  /* black on light grey (reverse) */
#define FW_MENU_ATTR_HEADER   0x0eU  /* yellow on black (emphasis)    */

#define FW_MENU_KIND_BOOT   0U
#define FW_MENU_KIND_MAINT  2U

typedef struct {
    UINT16 Option;
    UINT8  Kind;
    CHAR16 Desc[FW_MENU_DESC_MAX + 1U];
} FW_MENU_ENTRY;

static void fw_boot_maint_run(void);

static void fw_menu_attr(UINTN Attr)
{
    (void)mConOutProto.SetAttribute(&mConOutProto, Attr);
}

static void fw_menu_at(UINTN Col, UINTN Row)
{
    (void)mConOutProto.SetCursorPosition(&mConOutProto, Col, Row);
}

static void fw_menu_put_char16(const CHAR16 *Str)
{
    (void)mConOutProto.OutputString(&mConOutProto, (CHAR16 *)Str);
}

static void fw_menu_put_uint(UINTN Value)
{
    CHAR8 buf[12];
    UINTN i = sizeof(buf) - 1U;

    buf[i] = 0;
    do {
        buf[--i] = (CHAR8)('0' + (Value % 10U));
        Value /= 10U;
    } while (Value != 0U && i > 0U);
    efi_conout_ascii(&buf[i]);
}

static void fw_menu_copy_desc(const UINT8 *Option, UINTN OptionSize,
                              CHAR16 *Dst)
{
    UINTN off = sizeof(UINT32) + sizeof(UINT16);
    UINTN i = 0;

    while (off + sizeof(CHAR16) <= OptionSize && i < FW_MENU_DESC_MAX) {
        CHAR16 ch;

        fw_copy_mem(&ch, (VOID *)(Option + off), sizeof(ch));
        off += sizeof(ch);
        if (ch == 0) {
            break;
        }
        Dst[i++] = ch;
    }
    Dst[i] = 0;
}

static UINT16 fw_menu_read_timeout(void)
{
    static CHAR16 name[] = { 'T', 'i', 'm', 'e', 'o', 'u', 't', 0 };
    /*
     * With no "Timeout" variable the sample waits for the user indefinitely
     * (0xFFFF) rather than auto-booting: an installed OS writes the variable
     * alongside its Boot#### entry, and a user can always set one, so the
     * wait-forever default only applies to a bare machine with nothing to run.
     * The platform can override that fallback via the firmware-boot-timeout
     * machine property (default 0xFFFF); the functional tests set a short
     * finite value so bare media boots without an interactive menu selection.
     */
    UINT16 timeout = fw_handoff_boot_timeout();
    UINTN size = sizeof(timeout);
    UINT32 attr = 0;

    if (rs_get_variable(name, (void *)mEfiGlobalVariableGuid,
                        &attr, &size, &timeout) != EFI_SUCCESS ||
        size != sizeof(timeout)) {
        timeout = fw_handoff_boot_timeout();
    }
    return timeout;
}

static UINTN fw_menu_build(FW_MENU_ENTRY *Entries)
{
    static CHAR16 order_name[] = {
        'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0
    };
    static const CHAR16 shell_label[] = {
        'E', 'F', 'I', ' ', 'S', 'h', 'e', 'l', 'l', ' ',
        '[', 'B', 'u', 'i', 'l', 't', '-', 'i', 'n', ']', 0
    };
    static const CHAR16 maint_label[] = {
        'B', 'o', 'o', 't', ' ', 'o', 'p', 't', 'i', 'o', 'n', ' ',
        'm', 'a', 'i', 'n', 't', 'e', 'n', 'a', 'n', 'c', 'e', ' ',
        'm', 'e', 'n', 'u', 0
    };
    static UINT8 option[NVRAM_VAR_DATA_MAX];
    UINT16 order[16];
    UINTN order_size = sizeof(order);
    UINT32 attr = 0;
    UINTN n = 0;

    if (rs_get_variable(order_name, (void *)mEfiGlobalVariableGuid,
                        &attr, &order_size, order) == EFI_SUCCESS &&
        (order_size % sizeof(UINT16)) == 0) {
        UINTN i;

        for (i = 0; i < order_size / sizeof(UINT16) &&
                    n < FW_MENU_MAX - 2U; i++) {
            CHAR16 name[9];
            UINTN opt_size = sizeof(option);
            UINT32 opt_attr = 0;
            UINT32 load_attr = 0;

            fw_boot_option_name(order[i], name);
            if (rs_get_variable(name, (void *)mEfiGlobalVariableGuid,
                                &opt_attr, &opt_size, option) != EFI_SUCCESS ||
                opt_size < sizeof(UINT32) + sizeof(UINT16)) {
                continue;
            }
            fw_copy_mem(&load_attr, option, sizeof(load_attr));
            if ((load_attr & 0x00000001U) == 0) {
                continue;  /* LOAD_OPTION_ACTIVE clear: not shown */
            }
            Entries[n].Option = order[i];
            Entries[n].Kind = FW_MENU_KIND_BOOT;
            fw_menu_copy_desc(option, opt_size, Entries[n].Desc);
            if (Entries[n].Desc[0] == 0) {
                fw_copy_mem(Entries[n].Desc, name, sizeof(name));
            }
            n++;
        }
    }

    /* The built-in EFI shell is a genuine Boot#### (Boot00FF, provided by the
     * firmware variable table): selecting it runs the normal boot-option engine
     * exactly like the sample, which then hands off to the interactive shell
     * via its vendor-GUID device path. */
    Entries[n].Option = 0x00ffU;
    Entries[n].Kind = FW_MENU_KIND_BOOT;
    fw_copy_mem(Entries[n].Desc, shell_label, sizeof(shell_label));
    n++;
    Entries[n].Kind = FW_MENU_KIND_MAINT;
    fw_copy_mem(Entries[n].Desc, maint_label, sizeof(maint_label));
    n++;
    return n;
}

/* Column at which option rows and the footer text begin. */
#define FW_MENU_COL 4U

/* First option row in a maintenance screen.  The maintenance banner and header
 * each end in "\n\n", so options start two rows below the header (row 5),
 * whereas the boot-manager prompt has no trailing blank line (options at 4). */
#define FW_MAINT_TOP 5U

/* Clear the screen and draw the standard header plus a prompt line. */
static void fw_menu_frame(const CHAR8 *Prompt)
{
    (void)fw_console_clear();
    fw_console_set_cursor_visible(0);
    fw_menu_attr(FW_MENU_ATTR_HEADER);
    fw_menu_at(0, 0);
    efi_conout_ascii("EFI Boot Manager ver 1.10 [1.0]");
    /* The sample leaves the emphasis attribute active, so the prompt line
     * inherits the header's colour -- keep it yellow to match. */
    fw_menu_at(0, 2);
    efi_conout_ascii(Prompt);
    fw_menu_attr(FW_MENU_ATTR_NORMAL);
}

/* The arrow-key help line, drawn at the given row.  The up/down glyphs are
 * written as their Unicode code points; the console's CP437 mapping renders
 * them from the 8x16 font (codes 0x18/0x19). */
static void fw_menu_footer(UINTN Row)
{
    static const CHAR16 up[] = { 0x2191U, 0 };
    static const CHAR16 down[] = { 0x2193U, 0 };

    fw_menu_attr(FW_MENU_ATTR_NORMAL);
    fw_menu_at(FW_MENU_COL, Row);
    efi_conout_ascii("Use ");
    fw_menu_put_char16(up);
    efi_conout_ascii(" and ");
    fw_menu_put_char16(down);
    efi_conout_ascii(" to change option(s). Use Enter to select an option");
}

/* Option rows are drawn as a fixed-width field, matching the sample's %-.64s
 * format: the text is space-padded so the selection is a full-width bar. */
#define FW_MENU_FIELD 64U

/* Emit (FW_MENU_FIELD - Used) trailing spaces to fill the option field. */
static void fw_menu_pad_field(UINTN Used)
{
    static CHAR8 spaces[FW_MENU_FIELD + 1U];

    if (spaces[0] == 0) {
        UINTN i;

        for (i = 0; i < FW_MENU_FIELD; i++) {
            spaces[i] = ' ';
        }
        spaces[FW_MENU_FIELD] = 0;
    }
    if (Used < FW_MENU_FIELD) {
        efi_conout_ascii(spaces + Used);
    }
}

/* Draw one CHAR16 option row at FW_MENU_COL, padded, reverse if selected. */
static void fw_menu_option_row(UINTN Row, const CHAR16 *Desc, BOOLEAN Selected)
{
    UINTN len = 0;

    while (Desc[len]) {
        len++;
    }
    fw_menu_at(FW_MENU_COL, Row);
    fw_menu_attr(Selected ? FW_MENU_ATTR_SELECTED : FW_MENU_ATTR_NORMAL);
    fw_menu_put_char16(Desc);
    fw_menu_pad_field(len);
    fw_menu_attr(FW_MENU_ATTR_NORMAL);
}

/* Draw one CHAR8 option row at FW_MENU_COL, padded, reverse if selected. */
static void fw_menu_field8(UINTN Row, const CHAR8 *Text, BOOLEAN Selected)
{
    UINTN len = 0;

    while (Text[len]) {
        len++;
    }
    fw_menu_at(FW_MENU_COL, Row);
    fw_menu_attr(Selected ? FW_MENU_ATTR_SELECTED : FW_MENU_ATTR_NORMAL);
    efi_conout_ascii(Text);
    fw_menu_pad_field(len);
    fw_menu_attr(FW_MENU_ATTR_NORMAL);
}

static void fw_menu_render(const FW_MENU_ENTRY *Entries, UINTN Count,
                           UINTN Selected, INTN SecondsLeft)
{
    UINTN i;

    fw_menu_frame("Please select a boot option");
    for (i = 0; i < Count; i++) {
        fw_menu_option_row(4 + i, Entries[i].Desc, i == Selected);
    }
    /* Two blank rows separate the last option from the footer (sample). */
    fw_menu_footer(6 + Count);

    fw_menu_at(FW_MENU_COL, 7 + Count);
    if (SecondsLeft >= 0) {
        efi_conout_ascii("Default boot selection will be booted in ");
        fw_menu_put_uint((UINTN)SecondsLeft);
        efi_conout_ascii(" seconds  ");
    } else {
        efi_conout_ascii("                                            "
                         "                    ");
    }
}

static void fw_menu_activate(const FW_MENU_ENTRY *Entry)
{
    (void)fw_console_clear();
    if (Entry->Kind == FW_MENU_KIND_MAINT) {
        fw_boot_maint_run();
    } else {
        (void)fw_boot_image_from_boot_option(Entry->Option);
    }
}

void fw_boot_menu_run(void)
{
    static FW_MENU_ENTRY entries[FW_MENU_MAX];
    UINTN count;
    UINTN selected = 0;
    UINT16 timeout;
    INTN seconds_left;
    BOOLEAN counting;
    BOOLEAN dirty = 1;

    /*
     * BootNext first, before the menu and whatever the Timeout: with the
     * default Timeout (0xFFFF, wait for the user) the menu alone would never
     * boot it.  If that boot fails or its image returns, the menu follows.
     */
    (void)fw_boot_next_option();
    if (mBootServicesExited) {
        return;
    }

    count = fw_menu_build(entries);
    if (count == 0) {
        return;
    }
    timeout = fw_menu_read_timeout();
    if (timeout == 0) {
        /* Timeout 0 means boot immediately, without a menu; use the full
         * engine so the whole BootOrder is honoured. */
        (void)boot_image_from_boot_order();
        return;
    }
    seconds_left = (timeout == 0xffffU) ? -1 : (INTN)timeout;
    counting = (seconds_left >= 0);

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN sub;
        BOOLEAN got_key = 0;

        if (dirty) {
            fw_menu_render(entries, count, selected,
                           counting ? seconds_left : -1);
            dirty = 0;
        }

        /* Poll ~1 second (100 x 10 ms), reacting to the first key. */
        for (sub = 0; sub < 100U; sub++) {
            if (fw_console_read_key(&key) == EFI_SUCCESS) {
                got_key = 1;
                break;
            }
            (void)bs_stall(10000U);
        }

        if (got_key) {
            counting = 0;   /* any key cancels the countdown */
            dirty = 1;
            /* Sample key map: arrows or '^'/'V'/'v' move (clamped, no wrap);
             * Enter or Space selects; there is no Esc/exit key. */
            if (key.ScanCode == EFI_SCAN_UP || key.UnicodeChar == '^') {
                if (selected > 0) {
                    selected--;
                }
            } else if (key.ScanCode == EFI_SCAN_DOWN ||
                       key.UnicodeChar == 'V' || key.UnicodeChar == 'v') {
                if (selected + 1U < count) {
                    selected++;
                }
            } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n' ||
                       key.UnicodeChar == ' ') {
                fw_menu_activate(&entries[selected]);
                /* Only reached if the boot failed or an action returned:
                 * rebuild (entries may have changed) and re-render. */
                count = fw_menu_build(entries);
                if (selected >= count) {
                    selected = 0;
                }
            }
        } else if (counting) {
            seconds_left--;
            if (seconds_left <= 0) {
                /* Countdown expired with no interaction: auto-boot the default
                 * via the full BootNext/BootOrder engine.  If it returns, every
                 * option failed -- fall back to showing the menu. */
                (void)fw_console_clear();
                (void)boot_image_from_boot_order();
                count = fw_menu_build(entries);
                if (selected >= count) {
                    selected = 0;
                }
                counting = 0;
            }
            dirty = 1;
        }
    }
}

/* --- Boot maintenance ----------------------------------------------------- */

/*
 * The Boot Maintenance Manager mirrors the Intel EFI 1.10 sample: a persistent
 * banner, a Main Menu of operations, and sub-menus that edit the NVRAM boot
 * configuration.  Each screen re-draws the banner (fw_maint_frame); the
 * operation sub-menus append the "Save Settings to NVRAM"/"Help"/"Exit"
 * actions and offer to save on exit when there are unsaved changes.
 */

/* Draw the persistent maintenance banner plus a per-screen operation header. */
/* Banner (always emphasised) plus a per-screen header in HeaderAttr.  Most
 * maintenance headers are emphasised (%E in the sample); the console-device
 * headers use the plain screen attribute. */
static void fw_maint_frame_ex(const CHAR8 *Header, UINTN HeaderAttr)
{
    (void)fw_console_clear();
    fw_console_set_cursor_visible(0);
    fw_menu_attr(FW_MENU_ATTR_HEADER);
    fw_menu_at(0, 0);
    efi_conout_ascii("EFI Boot Maintenance Manager ver 1.10 [1.0]");
    fw_menu_attr(HeaderAttr);
    fw_menu_at(0, 2);
    efi_conout_ascii(Header);
    fw_menu_attr(FW_MENU_ATTR_NORMAL);
}

static void fw_maint_frame(const CHAR8 *Header)
{
    fw_maint_frame_ex(Header, FW_MENU_ATTR_HEADER);
}

/* Block until a key is available, then return it. */
static void fw_maint_key(EFI_INPUT_KEY *Key)
{
    while (fw_console_read_key(Key) != EFI_SUCCESS) {
        (void)bs_stall(10000U);
    }
}

/* Show a prompt at the given row and wait for Y/N (Esc counts as No). */
static BOOLEAN fw_maint_confirm(const CHAR8 *Prompt, UINTN Row)
{
    EFI_INPUT_KEY key;

    fw_menu_attr(FW_MENU_ATTR_NORMAL);
    fw_menu_at(FW_MENU_COL, Row);
    efi_conout_ascii(Prompt);
    fw_console_set_cursor_visible(1);
    for (;;) {
        fw_maint_key(&key);
        if (key.UnicodeChar == 'Y' || key.UnicodeChar == 'y') {
            fw_console_set_cursor_visible(0);
            return 1;
        }
        if (key.UnicodeChar == 'N' || key.UnicodeChar == 'n' ||
            key.ScanCode == EFI_SCAN_ESC) {
            fw_console_set_cursor_visible(0);
            return 0;
        }
    }
}

/* Show a status line at the given row and wait for any key. */
static void fw_maint_status(const CHAR8 *Message, UINTN Row)
{
    EFI_INPUT_KEY key;

    fw_menu_attr(FW_MENU_ATTR_NORMAL);
    fw_menu_at(FW_MENU_COL, Row);
    efi_conout_ascii(Message);
    fw_maint_key(&key);
}

/* Render Count CHAR8 items at rows 4.., Selected shown in reverse video. */
static void fw_maint_list(const CHAR8 *Header, const CHAR8 *const *Items,
                          UINTN Count, UINTN Selected)
{
    UINTN i;

    fw_maint_frame(Header);
    for (i = 0; i < Count; i++) {
        fw_menu_field8(FW_MAINT_TOP + i, Items[i], i == Selected);
    }
    fw_menu_footer(FW_MAINT_TOP + 1U + Count);
}

/* Run a simple selectable CHAR8 list; return the chosen index or Count on Esc. */
static UINTN fw_maint_choose(const CHAR8 *Header, const CHAR8 *const *Items,
                             UINTN Count)
{
    UINTN sel = 0;
    BOOLEAN dirty = 1;

    for (;;) {
        EFI_INPUT_KEY key;

        if (dirty) {
            fw_maint_list(Header, Items, Count, sel);
            dirty = 0;
        }
        fw_maint_key(&key);
        if (key.ScanCode == EFI_SCAN_UP) {
            if (sel > 0) {
                sel--;
            }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            if (sel + 1U < Count) {
                sel++;
            }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_ESC) {
            return Count;
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            return sel;
        }
    }
}

/* Set Auto Boot TimeOut: a sub-menu to set or clear the Timeout variable. */
static void fw_maint_help(const CHAR8 *Header, const CHAR8 *const *Lines,
                          UINTN Count);

static void fw_maint_set_timeout(void)
{
    static const CHAR8 *const items[] = {
        "Set Timeout Value",
        "Delete/Disable Timeout",
        "Help",
        "Exit",
    };
    static const CHAR8 *const help[] = {
        "\"Set Timeout Value\" sets how many seconds the boot manager waits",
        "before booting the default selection.  \"Delete/Disable Timeout\"",
        "removes the timeout so the menu waits for a key; a value of 65535",
        "also disables the automatic boot.",
    };
    static CHAR16 name[] = { 'T', 'i', 'm', 'e', 'o', 'u', 't', 0 };
    static const UINT32 nv = EFI_VARIABLE_NON_VOLATILE |
                             EFI_VARIABLE_BOOTSERVICE_ACCESS |
                             EFI_VARIABLE_RUNTIME_ACCESS;

    for (;;) {
        UINTN choice = fw_maint_choose(
            "Set Auto Boot Timeout. Select an Option", items, 4);

        if (choice == 0) {
            UINT16 current = fw_menu_read_timeout();
            UINT32 value = 0;
            BOOLEAN entered = 0;

            fw_maint_frame("Change Auto Boot TimeOut value");
            fw_menu_at(FW_MENU_COL, 4);
            efi_conout_ascii("Current TimeOut is : ");
            fw_menu_put_uint(current);
            efi_conout_ascii(" seconds");
            fw_menu_at(FW_MENU_COL, 6);
            efi_conout_ascii("New TimeOut in seconds (<= 65535) is : ");
            fw_console_set_cursor_visible(1);
            for (;;) {
                EFI_INPUT_KEY key;

                fw_maint_key(&key);
                if (key.ScanCode == EFI_SCAN_ESC) {
                    entered = 0;
                    break;
                }
                if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
                    break;
                }
                if (key.UnicodeChar >= '0' && key.UnicodeChar <= '9' &&
                    value * 10U + (UINT32)(key.UnicodeChar - '0') <= 65535U) {
                    CHAR8 echo[2];

                    value = value * 10U + (UINT32)(key.UnicodeChar - '0');
                    entered = 1;
                    echo[0] = (CHAR8)key.UnicodeChar;
                    echo[1] = 0;
                    efi_conout_ascii(echo);
                }
            }
            fw_console_set_cursor_visible(0);
            if (entered) {
                UINT16 t = (UINT16)value;

                (void)rs_set_variable(name, (void *)mEfiGlobalVariableGuid,
                                      nv, sizeof(t), &t);
            }
        } else if (choice == 1) {
            (void)rs_set_variable(name, (void *)mEfiGlobalVariableGuid,
                                  0, 0, NULL);
            fw_maint_frame("Set Auto Boot Timeout. Select an Option");
            fw_maint_status("'Timeout' variable cleared. Press any key...", 4);
        } else if (choice == 2) {
            fw_maint_help("Timeout Menu Help Screen", help, 4U);
        } else {
            return;
        }
    }
}

static const UINT32 FW_VAR_NV_BS_RT =
    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
    EFI_VARIABLE_RUNTIME_ACCESS;

static CHAR16 fw_boot_order_name[] = {
    'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0
};

/* Remove one option number from BootOrder, preserving the order of the rest. */
static void fw_menu_bootorder_remove(UINT16 Option)
{
    UINT16 order[128];
    UINT16 kept[128];
    UINTN order_size = sizeof(order);
    UINT32 attr = 0;
    UINTN i;
    UINTN n;
    UINTN m = 0;

    if (rs_get_variable(fw_boot_order_name, (void *)mEfiGlobalVariableGuid,
                        &attr, &order_size, order) != EFI_SUCCESS ||
        (order_size % sizeof(UINT16)) != 0) {
        return;
    }
    n = order_size / sizeof(UINT16);
    for (i = 0; i < n; i++) {
        if (order[i] != Option) {
            kept[m++] = order[i];
        }
    }
    (void)rs_set_variable(fw_boot_order_name, (void *)mEfiGlobalVariableGuid,
                          FW_VAR_NV_BS_RT, m * sizeof(UINT16), kept);
}

/* Append one option number to the end of BootOrder. */
static void fw_menu_bootorder_append(UINT16 Option)
{
    UINT16 order[128];
    UINTN order_size = sizeof(order) - sizeof(UINT16);
    UINT32 attr = 0;
    UINTN n = 0;

    if (rs_get_variable(fw_boot_order_name, (void *)mEfiGlobalVariableGuid,
                        &attr, &order_size, order) == EFI_SUCCESS &&
        (order_size % sizeof(UINT16)) == 0) {
        n = order_size / sizeof(UINT16);
    }
    order[n++] = Option;
    (void)rs_set_variable(fw_boot_order_name, (void *)mEfiGlobalVariableGuid,
                          FW_VAR_NV_BS_RT, n * sizeof(UINT16), order);
}

/* Read a line of CHAR16 into Buf (echoing); Enter ends it, Esc returns 0. */
static UINTN fw_menu_read_line(CHAR16 *Buf, UINTN Cap)
{
    UINTN len = 0;

    if (Cap == 0) {
        return 0;
    }
    Buf[0] = 0;
    fw_console_set_cursor_visible(1);       /* show the cursor while typing */
    for (;;) {
        EFI_INPUT_KEY key;

        if (fw_console_read_key(&key) != EFI_SUCCESS) {
            (void)bs_stall(10000U);
            continue;
        }
        if (key.ScanCode == EFI_SCAN_ESC) {
            Buf[0] = 0;
            fw_console_set_cursor_visible(0);
            return 0;
        }
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            Buf[len] = 0;
            efi_conout_ascii("\r\n");
            fw_console_set_cursor_visible(0);
            return len;
        }
        if ((key.UnicodeChar == 0x08 || key.UnicodeChar == 0x7f) && len > 0) {
            len--;
            efi_conout_ascii("\b \b");
            continue;
        }
        if (key.UnicodeChar >= 0x20 && len + 1U < Cap) {
            CHAR16 echo[2];

            Buf[len++] = key.UnicodeChar;
            echo[0] = key.UnicodeChar;
            echo[1] = 0;
            fw_menu_put_char16(echo);
        }
    }
}

/* Lowest Boot#### number not currently defined. */
static UINT16 fw_menu_free_boot_number(void)
{
    static UINT8 probe[NVRAM_VAR_DATA_MAX];
    UINT32 n;

    for (n = 0; n < 0x10000U; n++) {
        CHAR16 name[9];
        UINTN size = sizeof(probe);
        UINT32 attr = 0;

        fw_boot_option_name((UINT16)n, name);
        if (rs_get_variable(name, (void *)mEfiGlobalVariableGuid,
                            &attr, &size, probe) != EFI_SUCCESS) {
            return (UINT16)n;
        }
    }
    return 0;
}

static CHAR16 fw_boot_next_name[] = {
    'B', 'o', 'o', 't', 'N', 'e', 'x', 't', 0
};

/* Render a boot-option list followed by a blank line and the special action
 * rows; the row at index Selected (spanning both) is drawn in reverse video. */
static void fw_maint_ops_render(const CHAR8 *Header,
                                const FW_MENU_ENTRY *Entries, UINTN BootCount,
                                const CHAR8 *const *Specials, UINTN SpecCount,
                                UINTN Selected)
{
    UINTN i;
    UINTN row = FW_MAINT_TOP;

    fw_maint_frame(Header);
    for (i = 0; i < BootCount; i++, row++) {
        fw_menu_option_row(row, Entries[i].Desc, i == Selected);
    }
    row++;                                  /* blank line before the actions */
    for (i = 0; i < SpecCount; i++, row++) {
        fw_menu_field8(row, Specials[i], (BootCount + i) == Selected);
    }
    fw_menu_footer(row + 1U);
}

/* Row at which a sub-menu draws its confirm/status prompts. */
static UINTN fw_maint_msg_row(UINTN BootCount, UINTN SpecCount)
{
    UINTN row = FW_MAINT_TOP + BootCount + 1U + SpecCount + 2U;

    return (row > 23U) ? 23U : row;
}

/* Write BootOrder from the option numbers in a (possibly reordered) list. */
static void fw_maint_save_order(const FW_MENU_ENTRY *Entries, UINTN BootCount)
{
    UINT16 old[128];
    UINT16 neworder[128];
    UINTN old_size = sizeof(old);
    UINT32 attr = 0;
    UINTN n_old;
    UINTN i;
    UINTN vidx = 0;
    UINTN m = 0;

    /*
     * Only active, readable options appear in Entries[]; the live BootOrder may
     * also hold inactive or momentarily unreadable Boot#### numbers.  Splice the
     * reordered visible options back into the slots they occupied, leaving the
     * hidden entries in place, so a reorder never silently drops them.
     */
    if (rs_get_variable(fw_boot_order_name, (void *)mEfiGlobalVariableGuid,
                        &attr, &old_size, old) != EFI_SUCCESS ||
        (old_size % sizeof(UINT16)) != 0) {
        for (i = 0; i < BootCount && i < 128U; i++) {
            neworder[i] = Entries[i].Option;
        }
        (void)rs_set_variable(fw_boot_order_name,
                              (void *)mEfiGlobalVariableGuid, FW_VAR_NV_BS_RT,
                              i * sizeof(UINT16), neworder);
        return;
    }
    n_old = old_size / sizeof(UINT16);
    for (i = 0; i < n_old && m < 128U; i++) {
        BOOLEAN visible = 0;
        UINTN j;

        for (j = 0; j < BootCount; j++) {
            if (Entries[j].Option == old[i]) {
                visible = 1;
                break;
            }
        }
        if (visible && vidx < BootCount) {
            neworder[m++] = Entries[vidx++].Option;
        } else {
            neworder[m++] = old[i];
        }
    }
    (void)rs_set_variable(fw_boot_order_name, (void *)mEfiGlobalVariableGuid,
                          FW_VAR_NV_BS_RT, m * sizeof(UINT16), neworder);
}

/* Draw a help screen of CHAR8 lines and wait for a key. */
static void fw_maint_help(const CHAR8 *Header, const CHAR8 *const *Lines,
                          UINTN Count)
{
    UINTN i;

    fw_maint_frame(Header);
    for (i = 0; i < Count; i++) {
        fw_menu_at(FW_MENU_COL, 4 + i);
        efi_conout_ascii(Lines[i]);
    }
    fw_maint_status("Press any key to Continue: ", 4U + Count + 1U);
}

/* Change Boot Order: reorder BootOrder with the U/D keys, save on request. */
static void fw_maint_change_order(void)
{
    static const CHAR8 *const spec[] = {
        "Save Settings to NVRAM", "Help", "Exit",
    };
    static const CHAR8 *const help[] = {
        "Use the up and down arrow keys to highlight a boot option.",
        "Press 'U'/'u' to move it up, or 'D'/'d' to move it down.",
        "Select \"Save Settings to NVRAM\" to store the new order,",
        "then \"Exit\" to return to the Main Menu.",
    };
    static FW_MENU_ENTRY entries[FW_MENU_MAX];
    UINTN count = fw_menu_build(entries);
    UINTN boot_count = (count >= 2U) ? count - 2U : 0U;
    UINTN total = boot_count + 3U;
    UINTN mrow = fw_maint_msg_row(boot_count, 3U);
    UINTN selected = 0;
    BOOLEAN dirty = 1;
    BOOLEAN changed = 0;

    for (;;) {
        EFI_INPUT_KEY key;

        if (dirty) {
            fw_maint_ops_render("Change boot order.  Select an Operation",
                                entries, boot_count, spec, 3U, selected);
            dirty = 0;
        }
        fw_maint_key(&key);
        if (key.ScanCode == EFI_SCAN_UP) {
            if (selected > 0) { selected--; }   /* clamp, no wrap */
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            if (selected + 1U < total) { selected++; }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_ESC) {
            break;
        } else if (selected < boot_count &&
                   (key.UnicodeChar == 'U' || key.UnicodeChar == 'u')) {
            if (selected == 0) {
                fw_maint_status("Selected option top of list. Cannot move up. "
                                "Press any key...", mrow);
            } else {
                FW_MENU_ENTRY tmp = entries[selected];
                entries[selected] = entries[selected - 1U];
                entries[selected - 1U] = tmp;
                selected--;
                changed = 1;
            }
            dirty = 1;
        } else if (selected < boot_count &&
                   (key.UnicodeChar == 'D' || key.UnicodeChar == 'd')) {
            if (selected + 1U >= boot_count) {
                fw_maint_status("Selected option bottom of list. Cannot move "
                                "down. Press any key...", mrow);
            } else {
                FW_MENU_ENTRY tmp = entries[selected];
                entries[selected] = entries[selected + 1U];
                entries[selected + 1U] = tmp;
                selected++;
                changed = 1;
            }
            dirty = 1;
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            if (selected == boot_count) {
                fw_maint_save_order(entries, boot_count);
                changed = 0;
            } else if (selected == boot_count + 1U) {
                fw_maint_help("Boot Order Menu Help Screen", help, 4U);
            } else if (selected == boot_count + 2U) {
                break;                          /* Exit */
            }
            /* Enter on a boot option is a no-op; use U/D to reorder. */
            dirty = 1;
        }
    }
    if (changed &&
        fw_maint_confirm("NVRAM Not updated. Save NVRAM? "
                         "[Y to save, N to ignore]", mrow)) {
        fw_maint_save_order(entries, boot_count);
    }
}

/* Manage BootNext: stage a one-shot BootNext (or clear it), save on request. */
static void fw_maint_bootnext(void)
{
    static const CHAR8 *const spec[] = {
        "Reset BootNext Setting", "Save Settings to NVRAM", "Help", "Exit",
    };
    static const CHAR8 *const help[] = {
        "Highlight a boot option and press Enter or 'B'/'b' to make it the",
        "one-shot 'BootNext' selection.  Press 'R'/'r' or select",
        "\"Reset BootNext Setting\" to clear it.  \"Save Settings to NVRAM\"",
        "commits the change; \"Exit\" returns to the Main Menu.",
    };
    static FW_MENU_ENTRY entries[FW_MENU_MAX];
    UINTN count = fw_menu_build(entries);
    UINTN boot_count = (count >= 2U) ? count - 2U : 0U;
    UINTN total = boot_count + 4U;
    UINTN mrow = fw_maint_msg_row(boot_count, 4U);
    UINTN selected = 0;
    BOOLEAN dirty = 1;
    BOOLEAN changed = 0;
    BOOLEAN has_next = 0;
    UINT16 next = 0;

    for (;;) {
        EFI_INPUT_KEY key;

        if (dirty) {
            fw_maint_ops_render("Manage BootNext setting.  Select an Operation",
                                entries, boot_count, spec, 4U, selected);
            dirty = 0;
        }
        fw_maint_key(&key);
        if (key.ScanCode == EFI_SCAN_UP) {
            if (selected > 0) { selected--; }   /* clamp, no wrap */
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            if (selected + 1U < total) { selected++; }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_ESC) {
            break;
        } else if ((key.UnicodeChar == 'R' || key.UnicodeChar == 'r') ||
                   (selected == boot_count &&
                    (key.UnicodeChar == '\r' || key.UnicodeChar == '\n'))) {
            has_next = 0;
            changed = 1;
            fw_maint_status("'BootNext' variable cleared. Press any key...",
                            mrow);
            dirty = 1;
        } else if (selected < boot_count &&
                   (key.UnicodeChar == 'B' || key.UnicodeChar == 'b' ||
                    key.UnicodeChar == '\r' || key.UnicodeChar == '\n')) {
            if (fw_maint_confirm("Enter selected Boot Option as 'BootNext' "
                                 "[Y-Yes N-No]: ", mrow)) {
                next = entries[selected].Option;
                has_next = 1;
                changed = 1;
            }
            dirty = 1;
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            if (selected == boot_count + 1U) {           /* Save */
                if (has_next) {
                    (void)rs_set_variable(fw_boot_next_name,
                                          (void *)mEfiGlobalVariableGuid,
                                          FW_VAR_NV_BS_RT, sizeof(next), &next);
                } else {
                    (void)rs_set_variable(fw_boot_next_name,
                                          (void *)mEfiGlobalVariableGuid,
                                          0, 0, NULL);
                }
                changed = 0;
            } else if (selected == boot_count + 2U) {    /* Help */
                fw_maint_help("BootNext Menu Help Screen", help, 4U);
            } else {                                     /* Exit */
                break;
            }
            dirty = 1;
        }
    }
    if (changed &&
        fw_maint_confirm("NVRAM Not updated. Save NVRAM? "
                         "[Y to save, N to ignore]", mrow)) {
        if (has_next) {
            (void)rs_set_variable(fw_boot_next_name,
                                  (void *)mEfiGlobalVariableGuid,
                                  FW_VAR_NV_BS_RT, sizeof(next), &next);
        } else {
            (void)rs_set_variable(fw_boot_next_name,
                                  (void *)mEfiGlobalVariableGuid, 0, 0, NULL);
        }
    }
}

/* --- Maintenance file browser (Boot from a File / Add a Boot Option) ------ */

#define FW_BROWSE_MAX 15U

typedef struct {
    CHAR16 Name[64];
    BOOLEAN IsDir;
} FW_BROWSE_ENTRY;

/* TRUE if Name ends in ".EFI" (case-insensitive). */
static BOOLEAN fw_name_is_efi(const CHAR16 *Name)
{
    UINTN len = 0;
    const CHAR16 *p;

    while (Name[len]) {
        len++;
    }
    if (len < 4U) {
        return 0;
    }
    p = Name + (len - 4U);
    return p[0] == '.' &&
           (p[1] == 'E' || p[1] == 'e') &&
           (p[2] == 'F' || p[2] == 'f') &&
           (p[3] == 'I' || p[3] == 'i');
}

/* Append "\<Name>" to an absolute path (empty Path denotes the volume root). */
static void fw_path_push(CHAR16 *Path, UINTN Cap, const CHAR16 *Name)
{
    UINTN len = 0;
    UINTN i;

    while (Path[len]) {
        len++;
    }
    if (len + 1U < Cap) {
        Path[len++] = '\\';
    }
    for (i = 0; Name[i] && len + 1U < Cap; i++) {
        Path[len++] = Name[i];
    }
    Path[len] = 0;
}

/* Drop the last "\<component>" from an absolute path. */
static void fw_path_pop(CHAR16 *Path)
{
    UINTN len = 0;

    while (Path[len]) {
        len++;
    }
    while (len > 0U && Path[len - 1U] != '\\') {
        len--;
    }
    if (len > 0U) {
        len--;
    }
    Path[len] = 0;
}

/* Read a directory handle, keeping sub-directories and *.EFI files only. */
static UINTN fw_browse_read_dir(EFI_FILE_HANDLE Dir, FW_BROWSE_ENTRY *Out,
                                UINTN Max)
{
    static UINT8 buf[512];
    UINTN n = 0;

    for (;;) {
        UINTN size = sizeof(buf);
        FW_EFI_FILE_INFO *info = (FW_EFI_FILE_INFO *)(VOID *)buf;
        BOOLEAN is_dir;
        UINTN i;

        if (Dir->Read(Dir, &size, buf) != EFI_SUCCESS || size == 0) {
            break;
        }
        if (info->FileName[0] == '.' &&
            (info->FileName[1] == 0 ||
             (info->FileName[1] == '.' && info->FileName[2] == 0))) {
            continue;
        }
        is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        if (!is_dir && !fw_name_is_efi(info->FileName)) {
            continue;
        }
        if (n >= Max) {
            break;
        }
        for (i = 0; i < 63U && info->FileName[i]; i++) {
            Out[n].Name[i] = info->FileName[i];
        }
        Out[n].Name[i] = 0;
        Out[n].IsDir = is_dir;
        n++;
    }
    return n;
}

/* Build a boot option for VolHandle:FullPath and either boot it or save it. */
static void fw_browse_make_option(EFI_HANDLE VolHandle, const CHAR16 *FullPath,
                                  BOOLEAN BootNow)
{
    static UINT8 node[2U * sizeof(FW_DEVICE_PATH_NODE) + 2U * 128U];
    static UINT8 full_path[256];
    static UINT8 option[NVRAM_VAR_DATA_MAX];
    FW_DEVICE_PATH_NODE *hdr = (FW_DEVICE_PATH_NODE *)node;
    UINTN pathlen = 0;
    UINTN dp_size;
    EFI_STATUS st;

    while (FullPath[pathlen]) {
        pathlen++;
    }
    if (pathlen == 0U || pathlen > 120U) {
        return;
    }

    hdr->Type = 0x04U;      /* MEDIA_DEVICE_PATH  */
    hdr->SubType = 0x04U;   /* MEDIA_FILEPATH_DP  */
    hdr->Length = (UINT16)(sizeof(FW_DEVICE_PATH_NODE) + (pathlen + 1U) * 2U);
    fw_copy_mem(node + sizeof(FW_DEVICE_PATH_NODE), FullPath,
                (pathlen + 1U) * 2U);
    {
        FW_DEVICE_PATH_NODE *end =
            (FW_DEVICE_PATH_NODE *)(VOID *)(node + hdr->Length);

        end->Type = 0x7fU;
        end->SubType = 0xffU;
        end->Length = (UINT16)sizeof(FW_DEVICE_PATH_NODE);
    }
    st = fw_build_file_device_path(VolHandle, hdr, full_path,
                                   sizeof(full_path));
    if (st != EFI_SUCCESS) {
        fw_maint_status("Could not build the device path.  Press any key...",
                        20);
        return;
    }
    dp_size = fw_device_path_size((FW_DEVICE_PATH_NODE *)(VOID *)full_path);

    if (BootNow) {
        EFI_HANDLE image = NULL;

        st = fw_load_image(1, full_path, &image);
        if (st == EFI_SUCCESS) {
            (void)fw_set_watchdog_timeout(300U);
            fw_set_sal_loader_handoff_pending(1);
            st = fw_start_image(image);
            fw_set_sal_loader_handoff_pending(0);
            (void)fw_set_watchdog_timeout(0U);
        }
        if (st != EFI_SUCCESS) {
            fw_maint_status("Load failed.  Press any key to continue...", 20);
        }
        return;
    }

    {
        CHAR16 desc[FW_MENU_DESC_MAX + 1U];
        UINTN desclen;
        UINTN off = 0;
        UINT16 num;
        CHAR16 name[9];

        fw_maint_frame("Add a Boot Option");
        fw_menu_at(FW_MENU_COL, 4);
        efi_conout_ascii("Enter New Description: ");
        desclen = fw_menu_read_line(desc, sizeof(desc) / sizeof(desc[0]));
        if (desclen == 0U) {
            return;
        }

        {
            UINT32 active = 0x00000001U;
            UINT16 fpll = (UINT16)dp_size;

            fw_copy_mem(option + off, &active, sizeof(active));
            off += sizeof(active);
            fw_copy_mem(option + off, &fpll, sizeof(fpll));
            off += sizeof(fpll);
        }
        fw_copy_mem(option + off, desc, (desclen + 1U) * 2U);
        off += (desclen + 1U) * 2U;
        fw_copy_mem(option + off, full_path, dp_size);
        off += dp_size;

        num = fw_menu_free_boot_number();
        fw_boot_option_name(num, name);
        st = rs_set_variable(name, (void *)mEfiGlobalVariableGuid,
                             FW_VAR_NV_BS_RT, off, option);
        if (st == EFI_SUCCESS) {
            fw_menu_bootorder_append(num);
        }
        fw_maint_status(st == EFI_SUCCESS ?
                        "Boot option saved.  Press any key..." :
                        "Failed to save the boot option.  Press any key...", 6);
    }
}

/* Draw the directory listing (entries, then ".." if not root, then Exit). */
static void fw_browse_render(const FW_BROWSE_ENTRY *Entries, UINTN N,
                             BOOLEAN HasDotDot, UINTN Selected)
{
    UINTN i;
    UINTN row = FW_MAINT_TOP;
    UINTN idx;

    fw_maint_frame("Select file or change to new directory:");
    for (i = 0; i < N; i++, row++) {
        UINTN len = 7U;                     /* the "<DIR>  "/"       " prefix */
        UINTN j;

        for (j = 0; Entries[i].Name[j]; j++) {
            len++;
        }
        fw_menu_at(FW_MENU_COL, row);
        fw_menu_attr(i == Selected ? FW_MENU_ATTR_SELECTED :
                     FW_MENU_ATTR_NORMAL);
        efi_conout_ascii(Entries[i].IsDir ? "<DIR>  " : "       ");
        fw_menu_put_char16(Entries[i].Name);
        fw_menu_pad_field(len);
        fw_menu_attr(FW_MENU_ATTR_NORMAL);
    }
    row++;
    idx = N;
    if (HasDotDot) {
        fw_menu_field8(row, "[ .. ]  Up one directory", idx == Selected);
        row++;
        idx++;
    }
    fw_menu_field8(row, "Exit", idx == Selected);
    fw_menu_footer(row + 2U);
}

/* Walk one volume's directory tree, letting the user pick an *.EFI file. */
static void fw_browse_volume(EFI_HANDLE VolHandle, BOOLEAN BootNow)
{
    static FW_BROWSE_ENTRY entries[FW_BROWSE_MAX];
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_HANDLE root = NULL;
    CHAR16 path[224];

    if (bs_handle_protocol(VolHandle, (void *)mSimpleFileSystemProtocolGuid,
                           (void **)&sfs) != EFI_SUCCESS || sfs == NULL) {
        return;
    }
    if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS || root == NULL) {
        return;
    }
    path[0] = 0;

    for (;;) {
        EFI_FILE_HANDLE dir = root;
        BOOLEAN opened = 0;
        BOOLEAN has_dotdot = (path[0] != 0);
        UINTN n;
        UINTN total;
        UINTN selected = 0;
        BOOLEAN dirty = 1;
        BOOLEAN descend = 0;

        if (path[0] != 0 &&
            root->Open(root, &dir, path, EFI_FILE_MODE_READ, 0) !=
                EFI_SUCCESS) {
            path[0] = 0;
            continue;
        }
        opened = (path[0] != 0);
        n = fw_browse_read_dir(dir, entries, FW_BROWSE_MAX);
        if (opened) {
            (void)dir->Close(dir);
        }
        total = n + (has_dotdot ? 2U : 1U);

        while (!descend) {
            EFI_INPUT_KEY key;

            if (dirty) {
                fw_browse_render(entries, n, has_dotdot, selected);
                dirty = 0;
            }
            fw_maint_key(&key);
            if (key.ScanCode == EFI_SCAN_UP) {
                if (selected > 0) { selected--; }   /* clamp, no wrap */
                dirty = 1;
            } else if (key.ScanCode == EFI_SCAN_DOWN) {
                if (selected + 1U < total) { selected++; }
                dirty = 1;
            } else if (key.ScanCode == EFI_SCAN_ESC) {
                (void)root->Close(root);
                return;
            } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
                if (selected < n) {
                    if (entries[selected].IsDir) {
                        fw_path_push(path, sizeof(path) / sizeof(path[0]),
                                     entries[selected].Name);
                        descend = 1;
                    } else {
                        CHAR16 full[256];
                        UINTN i;

                        for (i = 0; path[i]; i++) {
                            full[i] = path[i];
                        }
                        full[i] = 0;
                        fw_path_push(full, sizeof(full) / sizeof(full[0]),
                                     entries[selected].Name);
                        fw_browse_make_option(VolHandle, full, BootNow);
                        if (!BootNow) {
                            (void)root->Close(root);
                            return;
                        }
                        dirty = 1;
                    }
                } else if (has_dotdot && selected == n) {
                    fw_path_pop(path);
                    descend = 1;
                } else {
                    (void)root->Close(root);
                    return;
                }
            }
        }
    }
}

/* Enumerate the file-system volumes, let the user pick one, then browse it. */
static void fw_maint_browse(BOOLEAN BootNow)
{
    EFI_HANDLE *handles = NULL;
    UINTN vol_count = 0;
    const CHAR8 *volhdr = BootNow ? "Boot From a File.  Select a Volume" :
                          "Add a Boot Option.  Select a Volume";

    if (bs_locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL,
                                (void *)mSimpleFileSystemProtocolGuid, NULL,
                                &vol_count, &handles) != EFI_SUCCESS ||
        vol_count == 0 || handles == NULL) {
        fw_maint_frame(volhdr);
        fw_maint_status("No file-system volume is available.  Press any "
                        "key...", 4);
        return;
    }
    if (vol_count == 1U) {
        fw_browse_volume(handles[0], BootNow);
        return;
    }

    for (;;) {
        UINTN selected = 0;
        BOOLEAN dirty = 1;
        UINTN total = vol_count + 1U;

        for (;;) {
            EFI_INPUT_KEY key;
            UINTN i;
            UINTN row = FW_MAINT_TOP;

            if (dirty) {
                fw_maint_frame(volhdr);
                for (i = 0; i < vol_count; i++, row++) {
                    UINTN len = 30U;        /* "Removable Media Boot [Volume ]" */
                    UINTN v = i;

                    do {
                        len++;
                        v /= 10U;
                    } while (v);
                    fw_menu_at(FW_MENU_COL, row);
                    fw_menu_attr(i == selected ? FW_MENU_ATTR_SELECTED :
                                 FW_MENU_ATTR_NORMAL);
                    efi_conout_ascii("Removable Media Boot [Volume ");
                    fw_menu_put_uint(i);
                    efi_conout_ascii("]");
                    fw_menu_pad_field(len);
                    fw_menu_attr(FW_MENU_ATTR_NORMAL);
                }
                row++;
                fw_menu_field8(row, "Exit", vol_count == selected);
                fw_menu_footer(row + 2U);
                dirty = 0;
            }
            fw_maint_key(&key);
            if (key.ScanCode == EFI_SCAN_UP) {
                if (selected > 0) { selected--; }   /* clamp, no wrap */
                dirty = 1;
            } else if (key.ScanCode == EFI_SCAN_DOWN) {
                if (selected + 1U < total) { selected++; }
                dirty = 1;
            } else if (key.ScanCode == EFI_SCAN_ESC) {
                return;
            } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
                if (selected >= vol_count) {
                    return;
                }
                fw_browse_volume(handles[selected], BootNow);
                dirty = 1;
            }
        }
    }
}

static void fw_maint_add_entry(void)
{
    fw_maint_browse(0);
}

/* Commit a list of staged option-number deletions to NVRAM and BootOrder. */
static void fw_maint_commit_deletes(const UINT16 *Pending, UINTN Count)
{
    UINTN i;

    for (i = 0; i < Count; i++) {
        CHAR16 name[9];

        fw_boot_option_name(Pending[i], name);
        (void)rs_set_variable(name, (void *)mEfiGlobalVariableGuid, 0, 0, NULL);
        fw_menu_bootorder_remove(Pending[i]);
    }
}

/* Delete Boot Option(s): stage per-option or all deletions, commit on save. */
static void fw_maint_delete_entry(void)
{
    static const CHAR8 *const spec[] = {
        "Delete All Boot Options", "Save Settings to NVRAM", "Help", "Exit",
    };
    static const CHAR8 *const help[] = {
        "Highlight a boot option and press Enter or 'D'/'d' to remove it,",
        "answering the confirmation.  Press 'A'/'a' or select \"Delete All",
        "Boot Options\" to remove every option.  \"Save Settings to NVRAM\"",
        "commits the deletions; \"Exit\" returns to the Main Menu.",
    };
    static FW_MENU_ENTRY entries[FW_MENU_MAX];
    UINT16 pending[FW_MENU_MAX];
    UINTN pend_count = 0;
    UINTN count = fw_menu_build(entries);
    UINTN boot_count = (count >= 2U) ? count - 2U : 0U;
    UINTN selected = 0;
    BOOLEAN dirty = 1;

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN total = boot_count + 4U;
        UINTN mrow = fw_maint_msg_row(boot_count, 4U);

        if (selected >= total) {
            selected = total - 1U;
        }
        if (dirty) {
            fw_maint_ops_render("Delete Boot Option(s).  Select an Option",
                                entries, boot_count, spec, 4U, selected);
            dirty = 0;
        }
        fw_maint_key(&key);
        if (key.ScanCode == EFI_SCAN_UP) {
            if (selected > 0) { selected--; }   /* clamp, no wrap */
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            if (selected + 1U < total) { selected++; }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_ESC) {
            break;
        } else if ((key.UnicodeChar == 'A' || key.UnicodeChar == 'a') ||
                   (selected == boot_count &&
                    (key.UnicodeChar == '\r' || key.UnicodeChar == '\n'))) {
            if (boot_count > 0U &&
                fw_maint_confirm("Delete ALL of above Boot Options "
                                 "[Y-Yes N-No]: ", mrow)) {
                UINTN i;

                for (i = 0; i < boot_count && pend_count < FW_MENU_MAX; i++) {
                    pending[pend_count++] = entries[i].Option;
                }
                boot_count = 0;
                selected = 0;
            }
            dirty = 1;
        } else if (selected < boot_count &&
                   (key.UnicodeChar == 'D' || key.UnicodeChar == 'd' ||
                    key.UnicodeChar == '\r' || key.UnicodeChar == '\n')) {
            if (fw_maint_confirm("Delete selected Boot Option [Y-Yes N-No]: ",
                                 mrow)) {
                UINTN i;

                if (pend_count < FW_MENU_MAX) {
                    pending[pend_count++] = entries[selected].Option;
                }
                for (i = selected; i + 1U < boot_count; i++) {
                    entries[i] = entries[i + 1U];
                }
                if (boot_count > 0U) {
                    boot_count--;
                }
                if (selected >= boot_count && boot_count > 0U) {
                    selected = boot_count - 1U;
                }
            }
            dirty = 1;
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            if (selected == boot_count + 1U) {
                fw_maint_commit_deletes(pending, pend_count);
                pend_count = 0;
            } else if (selected == boot_count + 2U) {
                fw_maint_help("Delete Menu Help Screen", help, 4U);
            } else {
                break;
            }
            dirty = 1;
        }
    }
    if (pend_count > 0U &&
        fw_maint_confirm("NVRAM Not updated. Save NVRAM? "
                         "[Y to save, N to ignore]",
                         fw_maint_msg_row(boot_count, 4U))) {
        fw_maint_commit_deletes(pending, pend_count);
    }
}

/* Boot from a File / console-device selection are implemented below. */
static void fw_maint_boot_from_file(void);
static void fw_maint_console_select(const CHAR8 *Header, const CHAR8 *Device);

#define FW_MAINT_BOOTFILE  0U
#define FW_MAINT_ADD       1U
#define FW_MAINT_DELETE    2U
#define FW_MAINT_ORDER     3U
#define FW_MAINT_BOOTNEXT  4U
#define FW_MAINT_TIMEOUT   5U
#define FW_MAINT_CONOUT    6U
#define FW_MAINT_CONIN     7U
#define FW_MAINT_CONERR    8U
#define FW_MAINT_COLDRESET 9U
#define FW_MAINT_EXIT      10U

typedef struct {
    const CHAR8 *Label;
    UINT8 Action;
    UINT8 GapBefore;
} FW_MAINT_ITEM;

/* The sample indents every Main Menu entry by four spaces within the row. */
static const FW_MAINT_ITEM mMaintItems[] = {
    { "    Boot from a File",                     FW_MAINT_BOOTFILE,  0 },
    { "    Add a Boot Option",                    FW_MAINT_ADD,       0 },
    { "    Delete Boot Option(s)",                FW_MAINT_DELETE,    0 },
    { "    Change Boot Order",                    FW_MAINT_ORDER,     0 },
    { "    Manage BootNext setting",              FW_MAINT_BOOTNEXT,  1 },
    { "    Set Auto Boot TimeOut",                FW_MAINT_TIMEOUT,   0 },
    { "    Select Active Console Output Devices", FW_MAINT_CONOUT,    1 },
    { "    Select Active Console Input Devices",  FW_MAINT_CONIN,     0 },
    { "    Select Active Standard Error Devices", FW_MAINT_CONERR,    0 },
    { "    Cold Reset",                           FW_MAINT_COLDRESET, 1 },
    { "    Exit",                                 FW_MAINT_EXIT,      0 },
};

static void fw_boot_maint_run(void)
{
    UINTN count = sizeof(mMaintItems) / sizeof(mMaintItems[0]);
    UINTN selected = 0;
    BOOLEAN dirty = 1;

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN i;
        UINTN row = FW_MAINT_TOP;

        if (dirty) {
            fw_maint_frame("Main Menu. Select an Operation");
            for (i = 0; i < count; i++) {
                if (mMaintItems[i].GapBefore) {
                    row++;
                }
                fw_menu_field8(row, mMaintItems[i].Label, i == selected);
                row++;
            }
            fw_menu_footer(row + 1U);
            dirty = 0;
        }
        fw_maint_key(&key);
        if (key.ScanCode == EFI_SCAN_UP) {
            if (selected > 0) { selected--; }   /* clamp, no wrap */
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            if (selected + 1U < count) { selected++; }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_ESC) {
            return;
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            switch (mMaintItems[selected].Action) {
            case FW_MAINT_BOOTFILE:
                fw_maint_boot_from_file();
                break;
            case FW_MAINT_ADD:
                fw_maint_add_entry();
                break;
            case FW_MAINT_DELETE:
                fw_maint_delete_entry();
                break;
            case FW_MAINT_ORDER:
                fw_maint_change_order();
                break;
            case FW_MAINT_BOOTNEXT:
                fw_maint_bootnext();
                break;
            case FW_MAINT_TIMEOUT:
                fw_maint_set_timeout();
                break;
            case FW_MAINT_CONOUT:
                fw_maint_console_select("Select the Console Output Device(s)",
                                        "VGA Text Console");
                break;
            case FW_MAINT_CONIN:
                fw_maint_console_select("Select the Console Input Device(s)",
                                        "PS/2 Keyboard");
                break;
            case FW_MAINT_CONERR:
                fw_maint_console_select("Select the Standard Error Device",
                                        "VGA Text Console");
                break;
            case FW_MAINT_COLDRESET:
                fw_reset_cold();
                break;
            case FW_MAINT_EXIT:
            default:
                return;
            }
            dirty = 1;
        }
    }
}

static void fw_maint_boot_from_file(void)
{
    fw_maint_browse(1);
}

/*
 * Console-device selection.  The synthetic ia64-vpc console is a single VGA
 * text device (output/error) with a PS/2 keyboard (input), chosen at launch,
 * so each of the three menus lists one device that stays active -- the sample
 * likewise refuses to deselect the last active console.  The list is drawn
 * with the reference's '*'/' ' active marker and the Save/Exit actions.
 */
static void fw_maint_console_select(const CHAR8 *Header, const CHAR8 *Device)
{
    static const CHAR8 *const spec[] = {
        "Save Settings to NVRAM", "Exit",
    };
    UINTN selected = 0;
    UINTN total = 3;
    BOOLEAN active = 1;
    BOOLEAN dirty = 1;

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN i;

        if (dirty) {
            UINTN len = 2U;                 /* the "* "/"  " marker */
            UINTN j;

            /*
             * The console-select header is yellow like every other menu.  The
             * sample's console Header string omits %E, but the menu framework
             * prints it right after PrintBanner(), which leaves the attribute
             * at %E/yellow (its banner never resets to %N), so the header
             * inherits yellow (Maint/menu.c PrintBanner + Print(Menu->Header)).
             */
            fw_maint_frame_ex(Header, FW_MENU_ATTR_HEADER);
            for (j = 0; Device[j]; j++) {
                len++;
            }
            fw_menu_at(FW_MENU_COL, FW_MAINT_TOP);
            fw_menu_attr(selected == 0 ? FW_MENU_ATTR_SELECTED :
                         FW_MENU_ATTR_NORMAL);
            efi_conout_ascii(active ? "* " : "  ");
            efi_conout_ascii(Device);
            fw_menu_pad_field(len);
            fw_menu_attr(FW_MENU_ATTR_NORMAL);
            for (i = 0; i < 2U; i++) {
                fw_menu_field8(FW_MAINT_TOP + 2U + i, spec[i],
                               (1U + i) == selected);
            }
            fw_menu_footer(FW_MAINT_TOP + 5U);
            dirty = 0;
        }
        fw_maint_key(&key);
        if (key.ScanCode == EFI_SCAN_UP) {
            if (selected > 0) { selected--; }   /* clamp, no wrap */
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            if (selected + 1U < total) { selected++; }
            dirty = 1;
        } else if (key.ScanCode == EFI_SCAN_ESC) {
            return;
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            if (selected == 0) {
                if (active) {
                    fw_maint_status("At least one console device must stay "
                                    "active.  Press any key...", 11);
                } else {
                    active = 1;
                }
            } else if (selected == 2) {
                return;
            }
            dirty = 1;
        }
    }
}

EFI_STATUS fw_console_read_key(EFI_INPUT_KEY *key)
{
    return mConInProto.ReadKeyStroke(&mConInProto, key);
}

EFI_STATUS fw_console_clear(VOID)
{
    return mConOutProto.ClearScreen(&mConOutProto);
}

BOOLEAN fw_boot_services_exited(VOID)
{
    return mBootServicesExited;
}

EFI_STATUS fw_load_image(BOOLEAN boot_policy, VOID *device_path,
                         EFI_HANDLE *image)
{
    return mBootServices.LoadImage(boot_policy, mImageHandle, device_path,
                                   NULL, 0, image);
}

EFI_STATUS fw_start_image(EFI_HANDLE image)
{
    return mBootServices.StartImage(image, NULL, NULL);
}

EFI_STATUS fw_unload_image(EFI_HANDLE image)
{
    return mBootServices.UnloadImage(image);
}

EFI_STATUS fw_set_watchdog_timeout(UINTN timeout_seconds)
{
    return mBootServices.SetWatchdogTimer(timeout_seconds, 0, 0, NULL);
}

void fw_set_sal_loader_handoff_pending(BOOLEAN pending)
{
    mSalLoaderHandoffPending = pending;
}

UINTN fw_partition_count(VOID)
{
    UINTN count = 0;
    UINTN index;

    for (index = 0; index < FW_ARRAY_SIZE(mPartitions); index++) {
        if (mPartitions[index].in_use) {
            count++;
        }
    }
    return count;
}

UINTN fw_processor_count(VOID)
{
    return fw_guest_processor_count();
}

UINT64 fw_installed_ram_size(VOID)
{
    return fw_guest_ram_size();
}

UINT32 fw_graphics_width(VOID)
{
    return mGraphicsWidth;
}

UINT32 fw_graphics_height(VOID)
{
    return mGraphicsHeight;
}

const CHAR8 *fw_storage_description(BOOLEAN boot_device)
{
    FW_STORAGE_DEVICE *device = boot_device ?
        &mBootStorageDevice : &mDiskStorageDevice;

    if (!storage_device_present(device)) {
        return "none";
    }
    if (device->Kind == FW_STORAGE_SCSI) {
        return storage_is_cd(device) ? "SCSI optical" : "SCSI disk";
    }
    if (device->Kind == FW_STORAGE_AHCI) {
        return storage_is_cd(device) ? "SATA optical" : "SATA disk";
    }
    return storage_is_cd(device) ? "IDE optical" : "IDE disk";
}

void fw_reset_cold(VOID)
{
    rs_reset_system(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
}

#include "fw-boot-shell.h"

