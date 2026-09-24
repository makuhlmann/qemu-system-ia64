/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * EFI protocol/structure typedefs shared by the firmware modules.
 */

#ifndef IA64_FIRMWARE_FW_EFI_TYPES_H
#define IA64_FIRMWARE_FW_EFI_TYPES_H

#include "fw-base.h"

/* --- EFI Unicode Collation Protocol ---------------------------------------- */

typedef struct _EFI_UNICODE_COLLATION_PROTOCOL EFI_UNICODE_COLLATION_PROTOCOL;

struct _EFI_UNICODE_COLLATION_PROTOCOL {
    INTN (*StriColl)(EFI_UNICODE_COLLATION_PROTOCOL *This,
                     CHAR16 *String1, CHAR16 *String2);
    BOOLEAN (*MetaiMatch)(EFI_UNICODE_COLLATION_PROTOCOL *This,
                          CHAR16 *String, CHAR16 *Pattern);
    VOID (*StrLwr)(EFI_UNICODE_COLLATION_PROTOCOL *This, CHAR16 *String);
    VOID (*StrUpr)(EFI_UNICODE_COLLATION_PROTOCOL *This, CHAR16 *String);
    VOID (*FatToStr)(EFI_UNICODE_COLLATION_PROTOCOL *This,
                     UINTN FatSize, CHAR8 *Fat, CHAR16 *String);
    BOOLEAN (*StrToFat)(EFI_UNICODE_COLLATION_PROTOCOL *This,
                        CHAR16 *String, UINTN FatSize, CHAR8 *Fat);
    CHAR8 *SupportedLanguages;
};

typedef struct {
    UINT64                  Signature;
    EFI_PHYSICAL_ADDRESS    EfiSystemTableBase;
    UINT32                  Crc32;
    UINT32                  Reserved;
} EFI_SYSTEM_TABLE_POINTER;

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* --- EFI Simple Text Output Protocol -------------------------------------- */
typedef EFI_STATUS (*EFI_TEXT_RESET)(VOID *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (*EFI_TEXT_STRING)(VOID *This, CHAR16 *String);
typedef EFI_STATUS (*EFI_TEXT_TEST_STRING)(VOID *This, CHAR16 *String);
typedef EFI_STATUS (*EFI_TEXT_QUERY_MODE)(VOID *This, UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
typedef EFI_STATUS (*EFI_TEXT_SET_MODE)(VOID *This, UINTN ModeNumber);
typedef EFI_STATUS (*EFI_TEXT_SET_ATTRIBUTE)(VOID *This, UINTN Attribute);
typedef EFI_STATUS (*EFI_TEXT_CLEAR_SCREEN)(VOID *This);
typedef EFI_STATUS (*EFI_TEXT_SET_CURSOR_POSITION)(VOID *This, UINTN Column, UINTN Row);
typedef EFI_STATUS (*EFI_TEXT_ENABLE_CURSOR)(VOID *This, BOOLEAN Enable);

typedef struct {
    UINT32                          MaxMode;
    UINT32                          Mode;
    INT32                           Attribute;
    INT32                           CursorColumn;
    INT32                           CursorRow;
    BOOLEAN                         CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUT_PROTOCOL {
    EFI_TEXT_RESET                  Reset;
    EFI_TEXT_STRING                 OutputString;
    EFI_TEXT_TEST_STRING            TestString;
    EFI_TEXT_QUERY_MODE             QueryMode;
    EFI_TEXT_SET_MODE               SetMode;
    EFI_TEXT_SET_ATTRIBUTE          SetAttribute;
    EFI_TEXT_CLEAR_SCREEN           ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION    SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR          EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE        *Mode;
} EFI_SIMPLE_TEXT_OUT_PROTOCOL;

/* --- EFI Simple Text Input Protocol ---------------------------------------- */

#define EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID { 0x387477c1, 0x69c7, 0x11d2, \
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                        BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                 EFI_INPUT_KEY *Key);
    EFI_EVENT   WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* --- EFI Simple Text Input Ex Protocol ------------------------------------ */

#define EFI_SHIFT_STATE_VALID    0x80000000U
#define EFI_RIGHT_SHIFT_PRESSED  0x00000001U
#define EFI_LEFT_SHIFT_PRESSED   0x00000002U
#define EFI_RIGHT_CONTROL_PRESSED 0x00000004U
#define EFI_LEFT_CONTROL_PRESSED 0x00000008U
#define EFI_RIGHT_ALT_PRESSED    0x00000010U
#define EFI_LEFT_ALT_PRESSED     0x00000020U
#define EFI_RIGHT_LOGO_PRESSED   0x00000040U
#define EFI_LEFT_LOGO_PRESSED    0x00000080U
#define EFI_MENU_KEY_PRESSED     0x00000100U
#define EFI_TOGGLE_STATE_VALID   0x80U
#define EFI_KEY_STATE_EXPOSED    0x40U
#define EFI_SCROLL_LOCK_ACTIVE   0x01U
#define EFI_NUM_LOCK_ACTIVE      0x02U
#define EFI_CAPS_LOCK_ACTIVE     0x04U

typedef UINT8 EFI_KEY_TOGGLE_STATE;

typedef struct {
    UINT32 KeyShiftState;
    EFI_KEY_TOGGLE_STATE KeyToggleState;
} EFI_KEY_STATE;

typedef struct {
    EFI_INPUT_KEY Key;
    EFI_KEY_STATE KeyState;
} EFI_KEY_DATA;

typedef EFI_STATUS (*EFI_KEY_NOTIFY_FUNCTION)(EFI_KEY_DATA *KeyData);

typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                        BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStrokeEx)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                  EFI_KEY_DATA *KeyData);
    EFI_EVENT   WaitForKeyEx;
    EFI_STATUS (*SetState)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                           EFI_KEY_TOGGLE_STATE *KeyToggleState);
    EFI_STATUS (*RegisterKeyNotify)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                    EFI_KEY_DATA *KeyData,
                                    EFI_KEY_NOTIFY_FUNCTION KeyNotificationFunction,
                                    VOID **NotifyHandle);
    EFI_STATUS (*UnregisterKeyNotify)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                      VOID *NotificationHandle);
};





/* --- EFI Table Header ----------------------------------------------------- */
typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

/* Simple Text Output/Input(+Ex) typedefs live in fw-efi-types.h. */

/* --- EFI Runtime Services Table ------------------------------------------- */
#define EFI_RUNTIME_SERVICES_REVISION  0x0001000a

typedef struct {
    EFI_TABLE_HEADER   Hdr;
    /* Runtime service entry points. */
    UINTN              GetTime;
    UINTN              SetTime;
    UINTN              GetWakeupTime;
    UINTN              SetWakeupTime;
    UINTN              SetVirtualAddressMap;
    UINTN              ConvertPointer;
    UINTN              GetVariable;
    UINTN              GetNextVariableName;
    UINTN              SetVariable;
    UINTN              GetNextHighMonotonicCount;
    UINTN              ResetSystem;
    UINTN              QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

/* --- EFI Boot Services Table ---------------------------------------------- */
#define EFI_BOOT_SERVICES_REVISION   0x0001000a

#define TPL_APPLICATION 4U
#define TPL_HIGH_LEVEL  31U

#define EVT_RUNTIME                       0x40000000U
#define EVT_RUNTIME_CONTEXT               0x20000000U
#define EVT_SIGNAL_EXIT_BOOT_SERVICES     0x00000201U
#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE 0x60000202U

typedef EFI_TPL (*EFI_RAISE_TPL)(EFI_TPL NewTpl);
typedef VOID (*EFI_RESTORE_TPL)(EFI_TPL OldTpl);

#define EFI_MEMORY_TYPE_OS_RESERVED_MIN 0x80000000U

typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                          UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (*EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(UINTN *MemoryMapSize,
                                          EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                          UINTN *MapKey,
                                          UINTN *DescriptorSize,
                                          UINT32 *DescriptorVersion);
typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
typedef EFI_STATUS (*EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (*EFI_CREATE_EVENT)(UINT32 Type, UINTN NotifyTpl,
                                        EFI_EVENT_NOTIFY NotifyFunction,
                                        VOID *NotifyContext,
                                        EFI_EVENT *Event);
typedef EFI_STATUS (*EFI_SET_TIMER)(EFI_EVENT Event, UINTN Type, UINT64 TriggerTime);
typedef EFI_STATUS (*EFI_WAIT_FOR_EVENT)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
typedef EFI_STATUS (*EFI_SIGNAL_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (*EFI_CLOSE_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (*EFI_CHECK_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (*EFI_INSTALL_PROTOCOL_INTERFACE)(EFI_HANDLE *Handle,
                                                      void *Protocol, UINTN InterfaceType,
                                                      VOID *Interface);
typedef EFI_STATUS (*EFI_REINSTALL_PROTOCOL_INTERFACE)(EFI_HANDLE Handle,
                                                        void *Protocol, VOID *OldInterface,
                                                        VOID *NewInterface);
typedef EFI_STATUS (*EFI_UNINSTALL_PROTOCOL_INTERFACE)(EFI_HANDLE Handle,
                                                        void *Protocol, VOID *Interface);
typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle, void *Protocol, VOID **Interface);
typedef EFI_STATUS (*EFI_REGISTER_PROTOCOL_NOTIFY)(void *Protocol, EFI_EVENT Event,
                                                    VOID **Registration);
typedef EFI_STATUS (*EFI_LOCATE_HANDLE)(UINTN SearchType, void *Protocol, VOID *SearchKey,
                                         UINTN *BufferSize, EFI_HANDLE *Buffer);
typedef EFI_STATUS (*EFI_LOCATE_DEVICE_PATH)(void *Protocol, void **DevicePath,
                                              EFI_HANDLE *Device);
typedef EFI_STATUS (*EFI_INSTALL_CONFIGURATION_TABLE)(void *Guid, VOID *Table);
typedef EFI_STATUS (*EFI_IMAGE_LOAD)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
                                      void *DevicePath, VOID *SourceBuffer, UINTN SourceSize,
                                      EFI_HANDLE *ImageHandle);
typedef EFI_STATUS (*EFI_IMAGE_START)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize,
                                       CHAR16 **ExitData);
typedef EFI_STATUS (*EFI_EXIT)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus,
                                UINTN ExitDataSize, CHAR16 *ExitData);
typedef EFI_STATUS (*EFI_IMAGE_UNLOAD)(EFI_HANDLE ImageHandle);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (*EFI_GET_NEXT_MONOTONIC_COUNT)(UINT64 *Count);
typedef EFI_STATUS (*EFI_STALL)(UINTN Microseconds);
typedef EFI_STATUS (*EFI_SET_WATCHDOG_TIMER)(UINTN Timeout, UINT64 WatchdogCode,
                                              UINTN DataSize, CHAR16 *WatchdogData);
typedef EFI_STATUS (*EFI_CONNECT_CONTROLLER)(EFI_HANDLE ControllerHandle,
                                              EFI_HANDLE *DriverImageHandle,
                                              void *RemainingDevicePath,
                                              BOOLEAN Recursive);
typedef EFI_STATUS (*EFI_DISCONNECT_CONTROLLER)(EFI_HANDLE ControllerHandle,
                                                 EFI_HANDLE DriverImageHandle,
                                                 EFI_HANDLE ChildHandle);
typedef EFI_STATUS (*EFI_OPEN_PROTOCOL)(EFI_HANDLE Handle, void *Protocol,
                                         VOID **Interface, EFI_HANDLE AgentHandle,
                                         EFI_HANDLE ControllerHandle,
                                         UINT32 Attributes);
typedef EFI_STATUS (*EFI_CLOSE_PROTOCOL)(EFI_HANDLE Handle, void *Protocol,
                                          EFI_HANDLE AgentHandle,
                                          EFI_HANDLE ControllerHandle);
typedef struct {
    EFI_HANDLE AgentHandle;
    EFI_HANDLE ControllerHandle;
    UINT32     Attributes;
    UINT32     OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
typedef EFI_STATUS (*EFI_OPEN_PROTOCOL_INFORMATION)(EFI_HANDLE Handle,
                                                     void *Protocol,
                                                     EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer,
                                                     UINTN *EntryCount);
typedef EFI_STATUS (*EFI_PROTOCOLS_PER_HANDLE)(EFI_HANDLE Handle,
                                                void ***ProtocolBuffer,
                                                UINTN *ProtocolBufferCount);
typedef EFI_STATUS (*EFI_LOCATE_HANDLE_BUFFER)(UINTN SearchType, void *Protocol,
                                                VOID *SearchKey, UINTN *NoHandles,
                                                EFI_HANDLE **Buffer);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(void *Protocol, VOID *Registration,
                                           VOID **Interface);

#define EFI_LOCATE_ALL_HANDLES        0
#define EFI_LOCATE_BY_REGISTER_NOTIFY 1

typedef EFI_STATUS (*EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)(EFI_HANDLE *Handle, ...);
typedef EFI_STATUS (*EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)(EFI_HANDLE Handle, ...);
typedef EFI_STATUS (*EFI_CALCULATE_CRC32)(VOID *Data, UINTN DataSize,
                                           UINT32 *Crc32);
typedef VOID (*EFI_COPY_MEM)(VOID *Destination, VOID *Source, UINTN Length);
typedef VOID (*EFI_SET_MEM)(VOID *Buffer, UINTN Size, UINT8 Value);
typedef EFI_STATUS (*EFI_CREATE_EVENT_EX)(UINT32 Type, UINTN NotifyTpl,
                                           EFI_EVENT_NOTIFY NotifyFunction,
                                           VOID *NotifyContext,
                                           void *EventGroup, EFI_EVENT *Event);

typedef struct {
    EFI_TABLE_HEADER                    Hdr;
    EFI_RAISE_TPL                       RaiseTPL;
    EFI_RESTORE_TPL                     RestoreTPL;
    EFI_ALLOCATE_PAGES                  AllocatePages;
    EFI_FREE_PAGES                      FreePages;
    EFI_GET_MEMORY_MAP                  GetMemoryMap;
    EFI_ALLOCATE_POOL                   AllocatePool;
    EFI_FREE_POOL                       FreePool;
    EFI_CREATE_EVENT                    CreateEvent;
    EFI_SET_TIMER                       SetTimer;
    EFI_WAIT_FOR_EVENT                  WaitForEvent;
    EFI_SIGNAL_EVENT                    SignalEvent;
    EFI_CLOSE_EVENT                     CloseEvent;
    EFI_CHECK_EVENT                     CheckEvent;
    EFI_INSTALL_PROTOCOL_INTERFACE      InstallProtocolInterface;
    EFI_REINSTALL_PROTOCOL_INTERFACE    ReinstallProtocolInterface;
    EFI_UNINSTALL_PROTOCOL_INTERFACE    UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL                 HandleProtocol;
    VOID                               *Reserved;
    EFI_REGISTER_PROTOCOL_NOTIFY        RegisterProtocolNotify;
    EFI_LOCATE_HANDLE                   LocateHandle;
    EFI_LOCATE_DEVICE_PATH              LocateDevicePath;
    EFI_INSTALL_CONFIGURATION_TABLE     InstallConfigurationTable;
    EFI_IMAGE_LOAD                      LoadImage;
    EFI_IMAGE_START                     StartImage;
    EFI_EXIT                            Exit;
    EFI_IMAGE_UNLOAD                    UnloadImage;
    EFI_EXIT_BOOT_SERVICES              ExitBootServices;
    EFI_GET_NEXT_MONOTONIC_COUNT        GetNextMonotonicCount;
    EFI_STALL                           Stall;
    EFI_SET_WATCHDOG_TIMER              SetWatchdogTimer;
    EFI_CONNECT_CONTROLLER              ConnectController;
    EFI_DISCONNECT_CONTROLLER           DisconnectController;
    EFI_OPEN_PROTOCOL                   OpenProtocol;
    EFI_CLOSE_PROTOCOL                  CloseProtocol;
    EFI_OPEN_PROTOCOL_INFORMATION       OpenProtocolInformation;
    EFI_PROTOCOLS_PER_HANDLE            ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER            LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL                 LocateProtocol;
    EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES InstallMultipleProtocolInterfaces;
    EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES UninstallMultipleProtocolInterfaces;
    EFI_CALCULATE_CRC32                 CalculateCrc32;
    EFI_COPY_MEM                        CopyMem;
    EFI_SET_MEM                         SetMem;
    EFI_CREATE_EVENT_EX                 CreateEventEx;
} EFI_BOOT_SERVICES;

/* --- EFI Configuration Table ---------------------------------------------- */
typedef struct {
    UINT8   VendorGuid[16];
    UINTN   VendorTable;
} EFI_CONFIGURATION_TABLE;

/* --- EFI System Table ----------------------------------------------------- */
#define EFI_SYSTEM_TABLE_REVISION  ((1 << 16) | 10)

typedef struct {
    EFI_TABLE_HEADER                Hdr;
    CHAR16                         *FirmwareVendor;
    UINT32                          FirmwareRevision;
    EFI_HANDLE                      ConsoleInHandle;
    VOID                           *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUT_PROTOCOL   *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    VOID                           *StdErr;
    EFI_RUNTIME_SERVICES           *RuntimeServices;
    EFI_BOOT_SERVICES              *BootServices;
    UINTN                           NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE        *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    UINT32              Revision;
    EFI_HANDLE          ParentHandle;
    EFI_SYSTEM_TABLE   *SystemTable;
    EFI_HANDLE          DeviceHandle;
    void               *FilePath;
    VOID               *Reserved;
    UINT32              LoadOptionsSize;
    VOID               *LoadOptions;
    VOID               *ImageBase;
    UINT64              ImageSize;
    UINT32              ImageCodeType;
    UINT32              ImageDataType;
    EFI_STATUS          (*Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
typedef struct _EFI_DISK_IO_PROTOCOL EFI_DISK_IO_PROTOCOL;

typedef EFI_STATUS (*EFI_BLOCK_RESET)(EFI_BLOCK_IO_PROTOCOL *This,
                                       BOOLEAN ExtendedVerification);
typedef EFI_STATUS (*EFI_BLOCK_READ)(EFI_BLOCK_IO_PROTOCOL *This,
                                      UINT32 MediaId, UINT64 Lba,
                                      UINTN BufferSize, VOID *Buffer);
typedef EFI_STATUS (*EFI_BLOCK_WRITE)(EFI_BLOCK_IO_PROTOCOL *This,
                                       UINT32 MediaId, UINT64 Lba,
                                       UINTN BufferSize, VOID *Buffer);
typedef EFI_STATUS (*EFI_BLOCK_FLUSH)(EFI_BLOCK_IO_PROTOCOL *This);

typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32  BlockSize;
    UINT32  IoAlign;
    UINT64  LastBlock;
} EFI_BLOCK_IO_MEDIA;

struct _EFI_BLOCK_IO_PROTOCOL {
    UINT64              Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_BLOCK_RESET     Reset;
    EFI_BLOCK_READ      ReadBlocks;
    EFI_BLOCK_WRITE     WriteBlocks;
    EFI_BLOCK_FLUSH     FlushBlocks;
};

typedef struct _EFI_DRIVER_BINDING_PROTOCOL EFI_DRIVER_BINDING_PROTOCOL;

typedef EFI_STATUS (*EFI_DRIVER_BINDING_SUPPORTED)(
    EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    VOID *RemainingDevicePath);
typedef EFI_STATUS (*EFI_DRIVER_BINDING_START)(
    EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    VOID *RemainingDevicePath);
typedef EFI_STATUS (*EFI_DRIVER_BINDING_STOP)(
    EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    UINTN NumberOfChildren, EFI_HANDLE *ChildHandleBuffer);

struct _EFI_DRIVER_BINDING_PROTOCOL {
    EFI_DRIVER_BINDING_SUPPORTED Supported;
    EFI_DRIVER_BINDING_START Start;
    EFI_DRIVER_BINDING_STOP Stop;
    UINT32 Version;
    EFI_HANDLE ImageHandle;
    EFI_HANDLE DriverBindingHandle;
};

typedef struct _EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL
    EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL;

struct _EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL {
    EFI_STATUS (*GetDriver)(EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL *This,
                            EFI_HANDLE ControllerHandle,
                            EFI_HANDLE *DriverImageHandle);
    EFI_STATUS (*GetDriverPath)(EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL *This,
                                EFI_HANDLE ControllerHandle,
                                VOID **DriverImagePath);
    EFI_STATUS (*DriverLoaded)(EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL *This,
                               EFI_HANDLE ControllerHandle,
                               VOID *DriverImagePath,
                               EFI_HANDLE DriverImageHandle);
};

typedef struct _EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL
    EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL;

struct _EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL {
    EFI_STATUS (*GetDriver)(
        EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL *This,
        EFI_HANDLE *DriverImageHandle);
};

typedef struct _EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL
    EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL;

struct _EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL {
    UINT32 (*GetVersion)(EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL *This);
};

typedef struct _EFI_LOAD_FILE_PROTOCOL EFI_LOAD_FILE_PROTOCOL;

struct _EFI_LOAD_FILE_PROTOCOL {
    EFI_STATUS (*LoadFile)(EFI_LOAD_FILE_PROTOCOL *This, VOID *FilePath,
                           BOOLEAN BootPolicy, UINTN *BufferSize,
                           VOID *Buffer);
};

typedef struct _EFI_COMPONENT_NAME_PROTOCOL EFI_COMPONENT_NAME_PROTOCOL;

typedef EFI_STATUS (*EFI_COMPONENT_NAME_GET_DRIVER_NAME)(
    EFI_COMPONENT_NAME_PROTOCOL *This, CHAR8 *Language,
    CHAR16 **DriverName);
typedef EFI_STATUS (*EFI_COMPONENT_NAME_GET_CONTROLLER_NAME)(
    EFI_COMPONENT_NAME_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    EFI_HANDLE ChildHandle, CHAR8 *Language, CHAR16 **ControllerName);

struct _EFI_COMPONENT_NAME_PROTOCOL {
    EFI_COMPONENT_NAME_GET_DRIVER_NAME GetDriverName;
    EFI_COMPONENT_NAME_GET_CONTROLLER_NAME GetControllerName;
    CHAR8 *SupportedLanguages;
};

/* --- EFI Debug Support Table --------------------------------------------- */

/* EFI_SYSTEM_TABLE_POINTER lives in fw-efi-types.h. */

#define EFI_DEBUG_IMAGE_INFO_UPDATE_IN_PROGRESS 0x01U
#define EFI_DEBUG_IMAGE_INFO_TABLE_MODIFIED     0x02U
#define EFI_DEBUG_IMAGE_INFO_TYPE_NORMAL        0x01U

typedef struct {
    UINT32                    ImageInfoType;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImageProtocolInstance;
    EFI_HANDLE                ImageHandle;
} EFI_DEBUG_IMAGE_INFO_NORMAL;

typedef union {
    UINT32                      *ImageInfoType;
    EFI_DEBUG_IMAGE_INFO_NORMAL *NormalImage;
} EFI_DEBUG_IMAGE_INFO;

typedef struct {
    volatile UINT32       UpdateStatus;
    UINT32                TableSize;
    EFI_DEBUG_IMAGE_INFO *EfiDebugImageInfoTable;
} EFI_DEBUG_IMAGE_INFO_TABLE_HEADER;

#endif /* IA64_FIRMWARE_FW_EFI_TYPES_H */
