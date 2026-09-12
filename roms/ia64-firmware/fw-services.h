/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal services shared by independently compiled firmware modules.
 */

#ifndef IA64_FIRMWARE_FW_SERVICES_H
#define IA64_FIRMWARE_FW_SERVICES_H

#include "fw-base.h"
#include "fw-efi-types.h"
#include "hw/ia64/ia64_vpc_abi.h"

#define IA64_PSR_AC     (1ULL << 3)
#define IA64_PSR_DT     (1ULL << 17)
#define IA64_PSR_DFL    (1ULL << 18)
#define IA64_PSR_DFH    (1ULL << 19)
#define IA64_PSR_RT     (1ULL << 27)
#define IA64_PSR_CPL_MASK (3ULL << 32)
#define IA64_PSR_IT     (1ULL << 36)
#define IA64_PSR_BN     (1ULL << 44)

#define IA64_DCR_LC     (1ULL << 2)
#define IA64_PSR_IC (1ULL << 13)
#define IA64_PSR_I  (1ULL << 14)

BOOLEAN fw_platform_is_460gx(void);
BOOLEAN fw_platform_is_zx1(void);
extern char __fw_ivt[];
#define SAL_IVT_BASE ((UINTN)__fw_ivt)

#define FW_MAX_CPUS IA64_VPC_MAX_CPUS
#define EFI_PAGE_SIZE 0x1000U

#define UART_RBR      0x00U
#define UART_THR      0x00U
#define UART_IER      0x01U
#define UART_FCR      0x02U
#define UART_LCR      0x03U
#define UART_MCR      0x04U
#define UART_LSR      0x05U
#define UART_MSR      0x06U
#define UART_LSR_DR   0x01U
#define UART_LSR_THRE 0x20U
#define UART_LCR_DLAB 0x80U
#define UART_MCR_DTR  0x01U
#define UART_MCR_RTS  0x02U
#define UART_MCR_LOOP 0x10U

/*
 * ITC rate, set at boot from PAL_FREQ_RATIOS: the ITC counts processor
 * clocks (800 MHz on Merced, 1.6 GHz on the Itanium 2 models).  20 (200 MHz)
 * only until fw_init_itc_rate() has run.
 */
extern UINT64 fw_itc_ticks_per_100ns;
#define FW_ITC_TICKS_PER_100NS fw_itc_ticks_per_100ns
void fw_pal_freq_ratios(UINT64 *Processor, UINT64 *Bus, UINT64 *Itc);
void fw_init_itc_rate(void);
#define FW_ITC_TICKS_PER_MICROSECOND (FW_ITC_TICKS_PER_100NS * 10ULL)
#define FW_ITC_TICKS_PER_SECOND (FW_ITC_TICKS_PER_100NS * 10000000ULL)

#define PS2_DATA_PORT        (IA64_PCI_IO_BASE + 0x60U)
#define PS2_STATUS_PORT      (IA64_PCI_IO_BASE + 0x64U)
#define PS2_STATUS_OBF       0x01U
#define PS2_CMD_READ_MODE             0x20U
#define PS2_CMD_WRITE_MODE            0x60U
#define PS2_CMD_KBD_ENABLE            0xAEU
#define PS2_KBD_CMD_ENABLE_SCAN       0xF4U
#define PS2_KBD_ACK                   0xFAU
#define PS2_MODE_KBD_INT              0x01U
#define PS2_MODE_MOUSE_INT            0x02U
#define PS2_MODE_SYS                  0x04U
#define PS2_MODE_KCC                  0x40U
#define PS2_STATUS_IBF       0x02U
#define PS2_STATUS_MOUSE_OBF 0x20U

#define TPL_CALLBACK   8U
#define TPL_NOTIFY     16U
#define EVT_NOTIFY_WAIT 0x00000100U
#define EVT_TIMER       0x80000000U
#define EVT_NOTIFY_SIGNAL 0x00000200U

#define EFI_RESET_COLD                0U
#define EFI_RESET_WARM                1U
#define EFI_RESET_SHUTDOWN            2U
#define EFI_RESET_PLATFORM_SPECIFIC   3U

#define TIMER_CANCEL   0U
#define TIMER_PERIODIC 1U
#define TIMER_RELATIVE 2U

void fw_copy_mem(VOID *destination, const VOID *source, UINTN length);
void fw_copy_mem_fast(VOID *Destination, const VOID *Source,
                      UINTN Length);
BOOLEAN fw_handoff_ide_dma_enabled(void);
UINT64 fw_read_itc(void);
volatile UINT8 *fw_uart_reg(UINTN offset);
UINT64 fw_read_psr(void);
UINT64 fw_read_cpuid3(void);
UINT64 fw_read_ivr(void);
void fw_write_eoi(void);
void fw_flush_instruction_cache(VOID *start, UINTN bytes);
UINT64 fw_handoff_debug_port_base(void);
BOOLEAN fw_handoff_i8042_enabled(void);
VOID *fw_system_table(VOID);

volatile UINT8 *ps2_reg(UINTN address);
UINT8 ps2_read_status(void);
BOOLEAN ps2_write_command(UINT8 command);
BOOLEAN ps2_write_data(UINT8 data);
BOOLEAN ps2_keyboard_raw_push(UINT8 data);

EFI_STATUS bs_install_protocol(EFI_HANDLE *handle, void *protocol,
                               UINTN interface_type, VOID *interface);
EFI_STATUS bs_uninstall_protocol(EFI_HANDLE handle, void *protocol,
                                 VOID *interface);
EFI_STATUS bs_locate_protocol(void *protocol, VOID *registration,
                              VOID **interface);
EFI_STATUS bs_create_event(UINT32 type, UINTN notify_tpl,
                           EFI_EVENT_NOTIFY notify_function,
                           VOID *notify_context, EFI_EVENT *event);
EFI_STATUS bs_signal_event(EFI_EVENT event);
EFI_STATUS bs_close_event(EFI_EVENT event);
EFI_STATUS bs_set_timer(EFI_EVENT event, UINTN type, UINT64 trigger_time);
extern EFI_TPL mCurrentTpl;
EFI_TPL bs_raise_tpl(EFI_TPL new_tpl);
VOID bs_restore_tpl(EFI_TPL old_tpl);
EFI_STATUS bs_stall(UINTN microseconds);
EFI_STATUS bs_allocate_pool(EFI_MEMORY_TYPE pool_type, UINTN size,
                            VOID **buffer);
EFI_STATUS bs_free_pool(VOID *buffer);

BOOLEAN fw_graphics_present(VOID);
UINT64 fw_graphics_bar_length(VOID);
UINT64 fw_graphics_framebuffer_base(VOID);
UINT64 fw_graphics_framebuffer_size(VOID);
UINT32 fw_graphics_pixels_per_scan_line(VOID);
EFI_STATUS fw_graphics_reset_current_mode(BOOLEAN redraw_text);
EFI_STATUS fw_graphics_set_uga_mode(UINT32 horizontal, UINT32 vertical,
                                    UINT32 color_depth, UINT32 refresh_rate);
EFI_HANDLE fw_graphics_handle(VOID);
BOOLEAN fw_protocol_interface_installed(EFI_HANDLE handle, VOID *protocol,
                                        VOID **interface);

extern const UINT8 mDevicePathProtocolGuid[16];

UINT64 fw_guest_ram_size(void);
UINT64 fw_guest_low_ram_end(void);
BOOLEAN fw_zx1_iova_hole_active(void);
UINTN fw_guest_processor_count(void);
UINTN fw_guest_socket_count(void);
UINTN fw_guest_cores_per_socket(void);
UINTN fw_guest_threads_per_core(void);
UINTN fw_guest_high_ram_count(void);
UINT64 fw_guest_high_ram_base(UINTN Index);
UINT64 fw_guest_high_ram_end(UINTN Index);

void uart_puts(const char *s);
void uart_put_hex64(UINT64 value);
BOOLEAN ranges_overlap(UINT64 a_base, UINT64 a_size,
                       UINT64 b_base, UINT64 b_size);
EFI_STATUS rs_convert_pointer_value(UINTN *Address);
BOOLEAN efi_find_allocation_overlap(UINT64 Start, UINT64 End,
                                    UINT64 *FirstEnd, UINT64 *LastStart);
UINT64 efi_memory_type_allocation_granularity(EFI_MEMORY_TYPE Type);
UINT8 table_checksum8(const void *buf, UINTN len);

/* platform.c */
void fw_platform_decode_topology(void);
UINTN fw_handoff_processor_count(void);
extern UINTN fw_call_efi_entry(UINTN (*Entry)(EFI_HANDLE, EFI_SYSTEM_TABLE *),
                               EFI_HANDLE ImageHandle,
                               EFI_SYSTEM_TABLE *SystemTable,
                               UINT64 SavedPsr,
                               UINT64 EntryPsrLow);
BOOLEAN sal_set_vectors_selftest(void);
BOOLEAN sal_state_info_selftest(void);
BOOLEAN sal_cache_services_selftest(void);
BOOLEAN sal_mc_rendez_selftest(void);
BOOLEAN sal_mc_set_params_selftest(void);
BOOLEAN sal_freq_base_selftest(void);
BOOLEAN sal_physical_services_selftest(void);
BOOLEAN sal_update_pal_selftest(void);
BOOLEAN sal_pci_config_selftest(void);
BOOLEAN sal_proc_dispatch_selftest(void);
BOOLEAN sal_loader_handoff_selftest(void);
BOOLEAN efi_entry_handoff_selftest(void);
void prepare_sal_loader_handoff(void);
UINT64 sal_loader_psr_low(void);
UINT64 fw_read_rsc(void);
void fw_restore_rsc(UINT64 rsc);
void fw_restore_psr(UINT64 psr);
UINT64 fw_guest_high_ram_total(void);

/* smbios.c */
void smbios_init_table(void);
BOOLEAN smbios_table_integrity_selftest(void);
UINTN fw_smbios_entry_point_address(void);

typedef struct {
    BOOLEAN in_use;
    EFI_HANDLE handle;
    UINT8 guid[16];
    VOID *interface;
    UINT64 modification_generation;
} EFI_PROTOCOL_RECORD;

#define PROTOCOL_RECORD_MAX 1024

typedef struct {
    BOOLEAN in_use;
    EFI_HANDLE handle;
    UINT8 guid[16];
    EFI_HANDLE agent_handle;
    EFI_HANDLE controller_handle;
    UINT32 attributes;
    UINT32 open_count;
} EFI_OPEN_PROTOCOL_RECORD;

#define OPEN_PROTOCOL_RECORD_MAX 512

#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x00000008U
#define EFI_OPEN_PROTOCOL_BY_DRIVER           0x00000010U

EFI_STATUS bs_calculate_crc32(VOID *Data, UINTN DataSize, UINT32 *Crc32);
EFI_STATUS bs_close_protocol(EFI_HANDLE Handle, void *Protocol,
                             EFI_HANDLE AgentHandle,
                             EFI_HANDLE ControllerHandle);
EFI_STATUS bs_install_multiple_protocol_interfaces(EFI_HANDLE *Handle, ...);
EFI_STATUS bs_open_protocol(EFI_HANDLE Handle, void *Protocol,
                            VOID **Interface, EFI_HANDLE AgentHandle,
                            EFI_HANDLE ControllerHandle, UINT32 Attributes);
EFI_STATUS bs_uninstall_multiple_protocol_interfaces(EFI_HANDLE Handle, ...);
BOOLEAN fw_guid_equal(const UINT8 *A, const UINT8 *B);
const void *fw_pci_io_device_from_handle_opaque(EFI_HANDLE Handle);
BOOLEAN guid_matches(const void *Protocol, const UINT8 *Guid);
BOOLEAN handle_supports_protocol(EFI_HANDLE Handle, void *Protocol,
                                        VOID **Interface);
EFI_STATUS rs_get_boot0000_variable(UINT32 *Attributes,
                                           UINTN *DataSize, VOID *Data);
extern EFI_HANDLE mBlockIoHandle;
extern const UINT8 mBlockIoProtocolGuid[16];
extern EFI_BOOT_SERVICES    mBootServices;
extern const UINT8 mComponentNameProtocolGuid[16];
extern EFI_HANDLE mDiskBlockIoHandle;
extern const UINT8 mDiskIoProtocolGuid[16];
extern const UINT8 mDriverBindingProtocolGuid[16];
extern EFI_HANDLE mGraphicsHandle;
extern EFI_HANDLE mImageHandle;
extern EFI_LOADED_IMAGE_PROTOCOL mLoadedImageProto;
extern const UINT8 mLoadedImageProtocolGuid[16];
extern EFI_OPEN_PROTOCOL_RECORD mOpenProtocolRecords[OPEN_PROTOCOL_RECORD_MAX];
extern EFI_HANDLE mPciRootBridgeHandle;
extern EFI_PROTOCOL_RECORD mProtocolRecords[PROTOCOL_RECORD_MAX];
extern EFI_HANDLE mRawBlockIoHandle;
extern EFI_HANDLE mStorageDriverHandle;
extern EFI_HANDLE mUnicodeCollationHandle;

/* console.c */
extern EFI_SIMPLE_TEXT_OUT_PROTOCOL mConOutProto;
extern EFI_SIMPLE_TEXT_INPUT_PROTOCOL mConInProto;
extern EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mConInExProto;
extern SIMPLE_TEXT_OUTPUT_MODE mConOutMode;
extern EFI_KEY_STATE mConInCurrentKeyState;
void efi_init_conout(void);
void efi_init_conin_wait_events(void);
void ps2_init_controller(void);
BOOLEAN conin_key_available(void);
EFI_STATUS conin_peek_key(EFI_INPUT_KEY *Key);
EFI_STATUS conin_read_key_raw(EFI_INPUT_KEY *Key);
BOOLEAN uefi_conout_selftest(void);
BOOLEAN uefi_conin_buffer_selftest(void);
BOOLEAN uefi_ps2_scancode_selftest(void);
void text_redraw_screen(void);
void text_clear_screen(void);
void text_clear_legacy_cells(void);

extern const UINT8 mConOutProtocolGuid[16];
extern const UINT8 mConInProtocolGuid[16];
extern const UINT8 mConInExProtocolGuid[16];

extern UINT32 mGraphicsWidth;
extern UINT32 mGraphicsHeight;
extern UINT32 mGraphicsStride;
extern BOOLEAN mGraphicsActive;
BOOLEAN fw_data_translation_enabled(void);

/* ohci.c */
BOOLEAN uefi_usb_keyboard_selftest(void);
BOOLEAN usb_keyboard_report_to_key(EFI_INPUT_KEY *Key);
EFI_STATUS usb_keyboard_read_key(EFI_INPUT_KEY *Key);
BOOLEAN usb_keyboard_init(void);

/* unicode_collation.c */
extern EFI_UNICODE_COLLATION_PROTOCOL mUnicodeCollationProto;
BOOLEAN unicode_collation_selftest(void);

#endif /* IA64_FIRMWARE_FW_SERVICES_H */
