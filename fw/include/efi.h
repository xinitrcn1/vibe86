/*
 * efi.h: the UEFI types this firmware implements and its images use.
 *
 * Written from the specifications, not copied from an implementation:
 *   [UEFI]  UEFI Forum, "Unified Extensible Firmware Interface (UEFI)
 *           Specification", version 2.10, August 2022.
 *   [PI]    UEFI Forum, "UEFI Platform Initialization (PI) Specification",
 *           version 1.8, March 2023 (the HOBs and firmware volumes: pi.h).
 *   [CSM]   Intel, "Intel Platform Innovation Framework for EFI
 *           Compatibility Support Module Specification", version 0.98, 2007
 *           (EFI_LEGACY_REGION2_PROTOCOL is from PI 1.8 Vol. 5 ch. 15).
 *
 * The images are built for x86_64-unknown-windows (PE32+, the Microsoft x64
 * calling convention that UEFI 2.10 2.3.4 prescribes), so EFIAPI is the
 * compiler's default convention and needs no attribute.
 */
#ifndef EFI_H
#define EFI_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define EFIAPI
#define IN
#define OUT
#define OPTIONAL
#define CONST const

typedef uint8_t BOOLEAN;
typedef int64_t INTN;
typedef uint64_t UINTN;
typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef char CHAR8;
typedef uint16_t CHAR16;	/* UCS-2 (built with -fshort-wchar: L"" works) */
typedef void VOID;
typedef UINTN EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef UINTN EFI_TPL;
typedef UINT64 EFI_LBA;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

#define TRUE	1
#define FALSE	0

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8 Data4[8];
} EFI_GUID;

/* status codes [UEFI] Appendix D */
#define EFI_ERR			(1ull << 63)
#define EFI_SUCCESS		0
#define EFI_LOAD_ERROR		(EFI_ERR | 1)
#define EFI_INVALID_PARAMETER	(EFI_ERR | 2)
#define EFI_UNSUPPORTED		(EFI_ERR | 3)
#define EFI_BAD_BUFFER_SIZE	(EFI_ERR | 4)
#define EFI_BUFFER_TOO_SMALL	(EFI_ERR | 5)
#define EFI_NOT_READY		(EFI_ERR | 6)
#define EFI_DEVICE_ERROR	(EFI_ERR | 7)
#define EFI_WRITE_PROTECTED	(EFI_ERR | 8)
#define EFI_OUT_OF_RESOURCES	(EFI_ERR | 9)
#define EFI_VOLUME_CORRUPTED	(EFI_ERR | 10)
#define EFI_VOLUME_FULL		(EFI_ERR | 11)
#define EFI_NO_MEDIA		(EFI_ERR | 12)
#define EFI_MEDIA_CHANGED	(EFI_ERR | 13)
#define EFI_NOT_FOUND		(EFI_ERR | 14)
#define EFI_ACCESS_DENIED	(EFI_ERR | 15)
#define EFI_NO_RESPONSE		(EFI_ERR | 16)
#define EFI_NO_MAPPING		(EFI_ERR | 17)
#define EFI_TIMEOUT		(EFI_ERR | 18)
#define EFI_NOT_STARTED		(EFI_ERR | 19)
#define EFI_ALREADY_STARTED	(EFI_ERR | 20)
#define EFI_ABORTED		(EFI_ERR | 21)
#define EFI_ICMP_ERROR		(EFI_ERR | 22)
#define EFI_TFTP_ERROR		(EFI_ERR | 23)
#define EFI_PROTOCOL_ERROR	(EFI_ERR | 24)
#define EFI_INCOMPATIBLE_VERSION (EFI_ERR | 25)
#define EFI_SECURITY_VIOLATION	(EFI_ERR | 26)
#define EFI_CRC_ERROR		(EFI_ERR | 27)
#define EFI_END_OF_MEDIA	(EFI_ERR | 28)
#define EFI_END_OF_FILE		(EFI_ERR | 31)
#define EFI_INVALID_LANGUAGE	(EFI_ERR | 32)
#define EFI_COMPROMISED_DATA	(EFI_ERR | 33)
#define EFI_WARN_UNKNOWN_GLYPH	1
#define EFI_ERROR(s)		((INTN)(s) < 0)

/* task priority levels [UEFI] 7.1 */
#define TPL_APPLICATION	4
#define TPL_CALLBACK	8
#define TPL_NOTIFY	16
#define TPL_HIGH_LEVEL	31

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

#define EFI_2_100_SYSTEM_TABLE_REVISION	((2 << 16) | 100)
#define EFI_SYSTEM_TABLE_SIGNATURE	0x5453595320494249ull	/* "IBI SYST" */
#define EFI_BOOT_SERVICES_SIGNATURE	0x56524553544f4f42ull	/* "BOOTSERV" */
#define EFI_RUNTIME_SERVICES_SIGNATURE	0x56524553544e5552ull	/* "RUNTSERV" */

/* ------------------------------------------------------------ memory */

typedef enum {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiUnacceptedMemoryType,
	EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
} EFI_ALLOCATE_TYPE;

#define EFI_MEMORY_UC		0x1ull
#define EFI_MEMORY_WC		0x2ull
#define EFI_MEMORY_WT		0x4ull
#define EFI_MEMORY_WB		0x8ull
#define EFI_MEMORY_UCE		0x10ull
#define EFI_MEMORY_WP		0x1000ull
#define EFI_MEMORY_RP		0x2000ull
#define EFI_MEMORY_XP		0x4000ull
#define EFI_MEMORY_RUNTIME	0x8000000000000000ull

#define EFI_MEMORY_DESCRIPTOR_VERSION	1
typedef struct {
	UINT32 Type;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS VirtualStart;
	UINT64 NumberOfPages;
	UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_PAGE_SIZE		4096
#define EFI_PAGE_SHIFT		12
#define EFI_SIZE_TO_PAGES(s)	(((s) + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT)

/* ------------------------------------------------------------ time */

typedef struct {
	UINT16 Year;
	UINT8 Month, Day, Hour, Minute, Second, Pad1;
	UINT32 Nanosecond;
	INT16 TimeZone;
	UINT8 Daylight, Pad2;
} EFI_TIME;
#define EFI_UNSPECIFIED_TIMEZONE 0x07FF

typedef struct {
	UINT32 Resolution;
	UINT32 Accuracy;
	BOOLEAN SetsToZero;
} EFI_TIME_CAPABILITIES;

/* ------------------------------------------------------------ device paths [UEFI] 10 */

typedef struct {
	UINT8 Type;
	UINT8 SubType;
	UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define HARDWARE_DEVICE_PATH	0x01
#define   HW_PCI_DP		0x01
#define   HW_MEMMAP_DP		0x03
#define   HW_VENDOR_DP		0x04
#define ACPI_DEVICE_PATH	0x02
#define   ACPI_DP		0x01
#define MESSAGING_DEVICE_PATH	0x03
#define   MSG_USB_DP		0x05
#define   MSG_UART_DP		0x0E
#define   MSG_VENDOR_DP		0x0A
#define MEDIA_DEVICE_PATH	0x04
#define   MEDIA_HARDDRIVE_DP	0x01
#define   MEDIA_VENDOR_DP	0x03
#define   MEDIA_FILEPATH_DP	0x04
#define   MEDIA_PIWG_FW_FILE_DP	0x06
#define   MEDIA_PIWG_FW_VOL_DP	0x07
#define BBS_DEVICE_PATH		0x05
#define END_DEVICE_PATH_TYPE	0x7F
#define   END_ENTIRE_DEVICE_PATH_SUBTYPE	0xFF
#define   END_INSTANCE_DEVICE_PATH_SUBTYPE	0x01

#pragma pack(push, 1)
typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT8 Function;
	UINT8 Device;
} PCI_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT32 HID;
	UINT32 UID;
} ACPI_HID_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT8 ParentPortNumber;
	UINT8 InterfaceNumber;
} USB_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	EFI_GUID Guid;
} VENDOR_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT32 Reserved;
	UINT64 BaudRate;
	UINT8 DataBits, Parity, StopBits;
} UART_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT32 MemoryType;
	EFI_PHYSICAL_ADDRESS StartingAddress;
	EFI_PHYSICAL_ADDRESS EndingAddress;
} MEMMAP_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	CHAR16 PathName[1];
} FILEPATH_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	EFI_GUID FvFileName;
} MEDIA_FW_VOL_FILEPATH_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	EFI_GUID FvName;
} MEDIA_FW_VOL_DEVICE_PATH;

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT32 PartitionNumber;
	UINT64 PartitionStart;
	UINT64 PartitionSize;
	UINT8 Signature[16];
	UINT8 MBRType;
	UINT8 SignatureType;
} HARDDRIVE_DEVICE_PATH;
#pragma pack(pop)

#define EISA_PNP_ID(id)		((UINT32)(((id) << 16) | 0x41D0))

/* ------------------------------------------------------------ events */

#define EVT_TIMER				0x80000000
#define EVT_RUNTIME				0x40000000
#define EVT_NOTIFY_WAIT				0x00000100
#define EVT_NOTIFY_SIGNAL			0x00000200
#define EVT_SIGNAL_EXIT_BOOT_SERVICES		0x00000201
#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE	0x60000202

typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, VOID *Context);

typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;

/* ------------------------------------------------------------ protocol handler services */

typedef enum { EFI_NATIVE_INTERFACE } EFI_INTERFACE_TYPE;
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL	0x01
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL		0x02
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL		0x04
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER	0x08
#define EFI_OPEN_PROTOCOL_BY_DRIVER		0x10
#define EFI_OPEN_PROTOCOL_EXCLUSIVE		0x20

typedef struct {
	EFI_HANDLE AgentHandle;
	EFI_HANDLE ControllerHandle;
	UINT32 Attributes;
	UINT32 OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;

typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown, EfiResetPlatformSpecific } EFI_RESET_TYPE;

typedef struct {
	EFI_GUID VendorGuid;
	VOID *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
	UINT64 Length;
	union {
		EFI_PHYSICAL_ADDRESS DataBlock;
		EFI_PHYSICAL_ADDRESS ContinuationPointer;
	} Union;
} EFI_CAPSULE_BLOCK_DESCRIPTOR;

typedef struct {
	EFI_GUID CapsuleGuid;
	UINT32 HeaderSize;
	UINT32 Flags;
	UINT32 CapsuleImageSize;
} EFI_CAPSULE_HEADER;

/* ------------------------------------------------------------ runtime services [UEFI] 8 */

typedef struct {
	EFI_TABLE_HEADER Hdr;
	EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Capabilities);
	EFI_STATUS (EFIAPI *SetTime)(EFI_TIME *Time);
	EFI_STATUS (EFIAPI *GetWakeupTime)(BOOLEAN *Enabled, BOOLEAN *Pending, EFI_TIME *Time);
	EFI_STATUS (EFIAPI *SetWakeupTime)(BOOLEAN Enable, EFI_TIME *Time);
	EFI_STATUS (EFIAPI *SetVirtualAddressMap)(UINTN MemoryMapSize, UINTN DescriptorSize,
	    UINT32 DescriptorVersion, EFI_MEMORY_DESCRIPTOR *VirtualMap);
	EFI_STATUS (EFIAPI *ConvertPointer)(UINTN DebugDisposition, VOID **Address);
	EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
	    UINT32 *Attributes, UINTN *DataSize, VOID *Data);
	EFI_STATUS (EFIAPI *GetNextVariableName)(UINTN *VariableNameSize, CHAR16 *VariableName,
	    EFI_GUID *VendorGuid);
	EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid,
	    UINT32 Attributes, UINTN DataSize, VOID *Data);
	EFI_STATUS (EFIAPI *GetNextHighMonotonicCount)(UINT32 *HighCount);
	VOID (EFIAPI *ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus,
	    UINTN DataSize, VOID *ResetData);
	EFI_STATUS (EFIAPI *UpdateCapsule)(EFI_CAPSULE_HEADER **CapsuleHeaderArray,
	    UINTN CapsuleCount, EFI_PHYSICAL_ADDRESS ScatterGatherList);
	EFI_STATUS (EFIAPI *QueryCapsuleCapabilities)(EFI_CAPSULE_HEADER **CapsuleHeaderArray,
	    UINTN CapsuleCount, UINT64 *MaximumCapsuleSize, EFI_RESET_TYPE *ResetType);
	EFI_STATUS (EFIAPI *QueryVariableInfo)(UINT32 Attributes, UINT64 *MaximumVariableStorageSize,
	    UINT64 *RemainingVariableStorageSize, UINT64 *MaximumVariableSize);
} EFI_RUNTIME_SERVICES;

#define EFI_VARIABLE_NON_VOLATILE		0x1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS		0x2
#define EFI_VARIABLE_RUNTIME_ACCESS		0x4

/* ------------------------------------------------------------ boot services [UEFI] 7 */

typedef struct EFI_BOOT_SERVICES {
	EFI_TABLE_HEADER Hdr;
	EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL NewTpl);
	VOID (EFIAPI *RestoreTPL)(EFI_TPL OldTpl);
	EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
	    UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
	EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
	EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
	    UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
	EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
	EFI_STATUS (EFIAPI *FreePool)(VOID *Buffer);
	EFI_STATUS (EFIAPI *CreateEvent)(UINT32 Type, EFI_TPL NotifyTpl, EFI_EVENT_NOTIFY NotifyFunction,
	    VOID *NotifyContext, EFI_EVENT *Event);
	EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime);
	EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
	EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT Event);
	EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
	EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT Event);
	EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *Handle, EFI_GUID *Protocol,
	    EFI_INTERFACE_TYPE InterfaceType, VOID *Interface);
	EFI_STATUS (EFIAPI *ReinstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol,
	    VOID *OldInterface, VOID *NewInterface);
	EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol,
	    VOID *Interface);
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
	VOID *Reserved;
	EFI_STATUS (EFIAPI *RegisterProtocolNotify)(EFI_GUID *Protocol, EFI_EVENT Event,
	    VOID **Registration);
	EFI_STATUS (EFIAPI *LocateHandle)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol,
	    VOID *SearchKey, UINTN *BufferSize, EFI_HANDLE *Buffer);
	EFI_STATUS (EFIAPI *LocateDevicePath)(EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath,
	    EFI_HANDLE *Device);
	EFI_STATUS (EFIAPI *InstallConfigurationTable)(EFI_GUID *Guid, VOID *Table);
	EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
	    EFI_DEVICE_PATH_PROTOCOL *DevicePath, VOID *SourceBuffer, UINTN SourceSize,
	    EFI_HANDLE *ImageHandle);
	EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
	EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus, UINTN ExitDataSize,
	    CHAR16 *ExitData);
	EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
	EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
	EFI_STATUS (EFIAPI *GetNextMonotonicCount)(UINT64 *Count);
	EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
	EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize,
	    CHAR16 *WatchdogData);
	EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
	    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath, BOOLEAN Recursive);
	EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE ControllerHandle,
	    EFI_HANDLE DriverImageHandle, EFI_HANDLE ChildHandle);
	EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface,
	    EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
	EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
	    EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
	EFI_STATUS (EFIAPI *OpenProtocolInformation)(EFI_HANDLE Handle, EFI_GUID *Protocol,
	    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer, UINTN *EntryCount);
	EFI_STATUS (EFIAPI *ProtocolsPerHandle)(EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer,
	    UINTN *ProtocolBufferCount);
	EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol,
	    VOID *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer);
	EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
	EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *Handle, ...);
	EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE Handle, ...);
	EFI_STATUS (EFIAPI *CalculateCrc32)(VOID *Data, UINTN DataSize, UINT32 *Crc32);
	VOID (EFIAPI *CopyMem)(VOID *Destination, VOID *Source, UINTN Length);
	VOID (EFIAPI *SetMem)(VOID *Buffer, UINTN Size, UINT8 Value);
	EFI_STATUS (EFIAPI *CreateEventEx)(UINT32 Type, EFI_TPL NotifyTpl, EFI_EVENT_NOTIFY NotifyFunction,
	    const VOID *NotifyContext, const EFI_GUID *EventGroup, EFI_EVENT *Event);
} EFI_BOOT_SERVICES;

/* ------------------------------------------------------------ console protocols [UEFI] 12 */

typedef struct {
	UINT16 ScanCode;
	CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* scan codes [UEFI] Table 12-2 */
#define SCAN_NULL	0x00
#define SCAN_UP		0x01
#define SCAN_DOWN	0x02
#define SCAN_RIGHT	0x03
#define SCAN_LEFT	0x04
#define SCAN_HOME	0x05
#define SCAN_END	0x06
#define SCAN_INSERT	0x07
#define SCAN_DELETE	0x08
#define SCAN_PAGE_UP	0x09
#define SCAN_PAGE_DOWN	0x0A
#define SCAN_F1		0x0B
#define SCAN_F2		0x0C
#define SCAN_F3		0x0D
#define SCAN_F4		0x0E
#define SCAN_F5		0x0F
#define SCAN_F6		0x10
#define SCAN_F7		0x11
#define SCAN_F8		0x12
#define SCAN_F9		0x13
#define SCAN_F10	0x14
#define SCAN_F11	0x15
#define SCAN_F12	0x16
#define SCAN_ESC	0x17
#define CHAR_NULL		0x0000
#define CHAR_BACKSPACE		0x0008
#define CHAR_TAB		0x0009
#define CHAR_LINEFEED		0x000A
#define CHAR_CARRIAGE_RETURN	0x000D

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
	EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
	EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
	INT32 MaxMode;
	INT32 Mode;
	INT32 Attribute;
	INT32 CursorColumn;
	INT32 CursorRow;
	BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
	EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
	EFI_STATUS (EFIAPI *TestString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
	EFI_STATUS (EFIAPI *QueryMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
	    UINTN *Columns, UINTN *Rows);
	EFI_STATUS (EFIAPI *SetMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
	EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
	EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
	EFI_STATUS (EFIAPI *SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column,
	    UINTN Row);
	EFI_STATUS (EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);
	SIMPLE_TEXT_OUTPUT_MODE *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

#define EFI_BLACK		0x00
#define EFI_BLUE		0x01
#define EFI_GREEN		0x02
#define EFI_CYAN		0x03
#define EFI_RED			0x04
#define EFI_MAGENTA		0x05
#define EFI_BROWN		0x06
#define EFI_LIGHTGRAY		0x07
#define EFI_DARKGRAY		0x08
#define EFI_LIGHTBLUE		0x09
#define EFI_LIGHTGREEN		0x0A
#define EFI_LIGHTCYAN		0x0B
#define EFI_LIGHTRED		0x0C
#define EFI_LIGHTMAGENTA	0x0D
#define EFI_YELLOW		0x0E
#define EFI_WHITE		0x0F
#define EFI_TEXT_ATTR(fg, bg)	((fg) | ((bg) << 4))

/* box drawing [UEFI] 12.4 (Unicode code points the consoles map to CP437) */
#define BOXDRAW_HORIZONTAL		0x2500
#define BOXDRAW_VERTICAL		0x2502
#define BOXDRAW_DOWN_RIGHT		0x250C
#define BOXDRAW_DOWN_LEFT		0x2510
#define BOXDRAW_UP_RIGHT		0x2514
#define BOXDRAW_UP_LEFT			0x2518
#define BOXDRAW_DOUBLE_HORIZONTAL	0x2550
#define BOXDRAW_DOUBLE_VERTICAL		0x2551
#define BOXDRAW_DOUBLE_DOWN_RIGHT	0x2554
#define BOXDRAW_DOUBLE_DOWN_LEFT	0x2557
#define BOXDRAW_DOUBLE_UP_RIGHT		0x255A
#define BOXDRAW_DOUBLE_UP_LEFT		0x255D
#define BLOCKELEMENT_FULL_BLOCK		0x2588
#define BLOCKELEMENT_LIGHT_SHADE	0x2591
#define ARROW_UP			0x2191
#define ARROW_DOWN			0x2193

typedef struct {
	INT32 RelativeMovementX, RelativeMovementY, RelativeMovementZ;
	BOOLEAN LeftButton, RightButton;
} EFI_SIMPLE_POINTER_STATE;

typedef struct {
	UINT64 ResolutionX, ResolutionY, ResolutionZ;
	BOOLEAN LeftButton, RightButton;
} EFI_SIMPLE_POINTER_MODE;

typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN ExtendedVerification);
	EFI_STATUS (EFIAPI *GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *State);
	EFI_EVENT WaitForInput;
	EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;

/* ------------------------------------------------------------ GOP [UEFI] 12.9 */

typedef struct {
	UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
	UINT8 Blue, Green, Red, Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
	EfiBltVideoFill,
	EfiBltVideoToBltBuffer,
	EfiBltBufferToVideo,
	EfiBltVideoToVideo,
	EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber,
	    UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
	EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber);
	EFI_STATUS (EFIAPI *Blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
	    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer, EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
	    UINTN SourceX, UINTN SourceY, UINTN DestinationX, UINTN DestinationY, UINTN Width,
	    UINTN Height, UINTN Delta);
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ------------------------------------------------------------ loaded image [UEFI] 9 */

typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	EFI_DEVICE_PATH_PROTOCOL *FilePath;
	VOID *Reserved;
	UINT32 LoadOptionsSize;
	VOID *LoadOptions;
	VOID *ImageBase;
	UINT64 ImageSize;
	EFI_MEMORY_TYPE ImageCodeType;
	EFI_MEMORY_TYPE ImageDataType;
	EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;
#define EFI_LOADED_IMAGE_PROTOCOL_REVISION 0x1000

/* ------------------------------------------------------------ system table [UEFI] 4.3 */

struct EFI_SYSTEM_TABLE {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	EFI_RUNTIME_SERVICES *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
};

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_ENTRY_POINT)(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

/* ------------------------------------------------------------ block I/O [UEFI] 13.9 */

typedef struct {
	UINT32 MediaId;
	BOOLEAN RemovableMedia;
	BOOLEAN MediaPresent;
	BOOLEAN LogicalPartition;
	BOOLEAN ReadOnly;
	BOOLEAN WriteCaching;
	UINT32 BlockSize;
	UINT32 IoAlign;
	EFI_LBA LastBlock;
	EFI_LBA LowestAlignedLba;
	UINT32 LogicalBlocksPerPhysicalBlock;
	UINT32 OptimalTransferLengthGranularity;
} EFI_BLOCK_IO_MEDIA;

#define EFI_BLOCK_IO_PROTOCOL_REVISION3 0x0002001F
typedef struct EFI_BLOCK_IO_PROTOCOL {
	UINT64 Revision;
	EFI_BLOCK_IO_MEDIA *Media;
	EFI_STATUS (EFIAPI *Reset)(struct EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification);
	EFI_STATUS (EFIAPI *ReadBlocks)(struct EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
	    UINTN BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *WriteBlocks)(struct EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
	    UINTN BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *FlushBlocks)(struct EFI_BLOCK_IO_PROTOCOL *This);
} EFI_BLOCK_IO_PROTOCOL;

typedef struct EFI_DISK_IO_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *ReadDisk)(struct EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Offset,
	    UINTN BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *WriteDisk)(struct EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Offset,
	    UINTN BufferSize, VOID *Buffer);
} EFI_DISK_IO_PROTOCOL;

/* ------------------------------------------------------------ file system [UEFI] 13.4-13.5 */

#define EFI_FILE_MODE_READ	0x0000000000000001ull
#define EFI_FILE_MODE_WRITE	0x0000000000000002ull
#define EFI_FILE_MODE_CREATE	0x8000000000000000ull
#define EFI_FILE_READ_ONLY	0x01
#define EFI_FILE_HIDDEN		0x02
#define EFI_FILE_SYSTEM		0x04
#define EFI_FILE_RESERVED	0x08
#define EFI_FILE_DIRECTORY	0x10
#define EFI_FILE_ARCHIVE	0x20
#define EFI_FILE_PROTOCOL_REVISION	0x00010000

typedef struct EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle,
	    CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
	EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
	EFI_STATUS (EFIAPI *Delete)(struct EFI_FILE_PROTOCOL *This);
	EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *Write)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 *Position);
	EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 Position);
	EFI_STATUS (EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
	    UINTN *BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *SetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
	    UINTN BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *This);
	/* revision 2 (async) members are not provided: Revision says 1 */
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
	    EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION 0x00010000

typedef struct {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	EFI_TIME CreateTime;
	EFI_TIME LastAccessTime;
	EFI_TIME ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[1];
} EFI_FILE_INFO;
#define SIZE_OF_EFI_FILE_INFO	offsetof(EFI_FILE_INFO, FileName)

typedef struct {
	UINT64 Size;
	BOOLEAN ReadOnly;
	UINT64 VolumeSize;
	UINT64 FreeSpace;
	UINT32 BlockSize;
	CHAR16 VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;
#define SIZE_OF_EFI_FILE_SYSTEM_INFO	offsetof(EFI_FILE_SYSTEM_INFO, VolumeLabel)

/* ------------------------------------------------------------ serial I/O [UEFI] 12.8 */

typedef enum { DefaultParity, NoParity, EvenParity, OddParity, MarkParity, SpaceParity } EFI_PARITY_TYPE;
typedef enum { DefaultStopBits, OneStopBit, OneFiveStopBits, TwoStopBits } EFI_STOP_BITS_TYPE;

typedef struct {
	UINT32 ControlMask;
	UINT32 Timeout;
	UINT64 BaudRate;
	UINT32 ReceiveFifoDepth;
	UINT32 DataBits;
	UINT32 Parity;
	UINT32 StopBits;
} SERIAL_IO_MODE;

typedef struct EFI_SERIAL_IO_PROTOCOL {
	UINT32 Revision;
	EFI_STATUS (EFIAPI *Reset)(struct EFI_SERIAL_IO_PROTOCOL *This);
	EFI_STATUS (EFIAPI *SetAttributes)(struct EFI_SERIAL_IO_PROTOCOL *This, UINT64 BaudRate,
	    UINT32 ReceiveFifoDepth, UINT32 Timeout, EFI_PARITY_TYPE Parity, UINT8 DataBits,
	    EFI_STOP_BITS_TYPE StopBits);
	EFI_STATUS (EFIAPI *SetControl)(struct EFI_SERIAL_IO_PROTOCOL *This, UINT32 Control);
	EFI_STATUS (EFIAPI *GetControl)(struct EFI_SERIAL_IO_PROTOCOL *This, UINT32 *Control);
	EFI_STATUS (EFIAPI *Write)(struct EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
	EFI_STATUS (EFIAPI *Read)(struct EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
	SERIAL_IO_MODE *Mode;
	const EFI_GUID *DeviceTypeGuid;		/* revision 1.1 */
} EFI_SERIAL_IO_PROTOCOL;
#define EFI_SERIAL_IO_PROTOCOL_REVISION	0x00010000
#define EFI_SERIAL_INPUT_BUFFER_EMPTY	0x0100
#define EFI_SERIAL_CLEAR_TO_SEND	0x0010
#define EFI_SERIAL_DATA_SET_READY	0x0020
#define EFI_SERIAL_CARRIER_DETECT	0x0080

/* ------------------------------------------------------------ PCI I/O [UEFI] 14.4 */

typedef enum {
	EfiPciIoWidthUint8, EfiPciIoWidthUint16, EfiPciIoWidthUint32, EfiPciIoWidthUint64,
	EfiPciIoWidthFifoUint8, EfiPciIoWidthFifoUint16, EfiPciIoWidthFifoUint32, EfiPciIoWidthFifoUint64,
	EfiPciIoWidthFillUint8, EfiPciIoWidthFillUint16, EfiPciIoWidthFillUint32, EfiPciIoWidthFillUint64,
	EfiPciIoWidthMaximum
} EFI_PCI_IO_PROTOCOL_WIDTH;

typedef enum {
	EfiPciIoOperationBusMasterRead,
	EfiPciIoOperationBusMasterWrite,
	EfiPciIoOperationBusMasterCommonBuffer,
	EfiPciIoOperationMaximum
} EFI_PCI_IO_PROTOCOL_OPERATION;

typedef enum {
	EfiPciIoAttributeOperationGet,
	EfiPciIoAttributeOperationSet,
	EfiPciIoAttributeOperationEnable,
	EfiPciIoAttributeOperationDisable,
	EfiPciIoAttributeOperationSupported,
	EfiPciIoAttributeOperationMaximum
} EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION;

#define EFI_PCI_IO_ATTRIBUTE_ISA_IO		0x0002
#define EFI_PCI_IO_ATTRIBUTE_VGA_PALETTE_IO	0x0004
#define EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY		0x0008
#define EFI_PCI_IO_ATTRIBUTE_VGA_IO		0x0010
#define EFI_PCI_IO_ATTRIBUTE_IO			0x0100
#define EFI_PCI_IO_ATTRIBUTE_MEMORY		0x0200
#define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER		0x0400
#define EFI_PCI_IO_ATTRIBUTE_EMBEDDED_DEVICE	0x0800
#define EFI_PCI_IO_ATTRIBUTE_VGA_IO_16		0x20000
#define EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE	0x8000

struct EFI_PCI_IO_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_PROTOCOL_IO_MEM)(struct EFI_PCI_IO_PROTOCOL *This,
    EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT8 BarIndex, UINT64 Offset, UINTN Count, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_PCI_IO_PROTOCOL_CONFIG)(struct EFI_PCI_IO_PROTOCOL *This,
    EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, UINTN Count, VOID *Buffer);

typedef struct {
	EFI_PCI_IO_PROTOCOL_IO_MEM Read;
	EFI_PCI_IO_PROTOCOL_IO_MEM Write;
} EFI_PCI_IO_PROTOCOL_ACCESS;

typedef struct {
	EFI_PCI_IO_PROTOCOL_CONFIG Read;
	EFI_PCI_IO_PROTOCOL_CONFIG Write;
} EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS;

typedef struct EFI_PCI_IO_PROTOCOL {
	EFI_STATUS (EFIAPI *PollMem)(struct EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
	    UINT8 BarIndex, UINT64 Offset, UINT64 Mask, UINT64 Value, UINT64 Delay, UINT64 *Result);
	EFI_STATUS (EFIAPI *PollIo)(struct EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
	    UINT8 BarIndex, UINT64 Offset, UINT64 Mask, UINT64 Value, UINT64 Delay, UINT64 *Result);
	EFI_PCI_IO_PROTOCOL_ACCESS Mem;
	EFI_PCI_IO_PROTOCOL_ACCESS Io;
	EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS Pci;
	EFI_STATUS (EFIAPI *CopyMem)(struct EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_WIDTH Width,
	    UINT8 DestBarIndex, UINT64 DestOffset, UINT8 SrcBarIndex, UINT64 SrcOffset, UINTN Count);
	EFI_STATUS (EFIAPI *Map)(struct EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_OPERATION Operation,
	    VOID *HostAddress, UINTN *NumberOfBytes, EFI_PHYSICAL_ADDRESS *DeviceAddress, VOID **Mapping);
	EFI_STATUS (EFIAPI *Unmap)(struct EFI_PCI_IO_PROTOCOL *This, VOID *Mapping);
	EFI_STATUS (EFIAPI *AllocateBuffer)(struct EFI_PCI_IO_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
	    EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **HostAddress, UINT64 Attributes);
	EFI_STATUS (EFIAPI *FreeBuffer)(struct EFI_PCI_IO_PROTOCOL *This, UINTN Pages, VOID *HostAddress);
	EFI_STATUS (EFIAPI *Flush)(struct EFI_PCI_IO_PROTOCOL *This);
	EFI_STATUS (EFIAPI *GetLocation)(struct EFI_PCI_IO_PROTOCOL *This, UINTN *SegmentNumber,
	    UINTN *BusNumber, UINTN *DeviceNumber, UINTN *FunctionNumber);
	EFI_STATUS (EFIAPI *Attributes)(struct EFI_PCI_IO_PROTOCOL *This,
	    EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Operation, UINT64 Attributes, UINT64 *Result);
	EFI_STATUS (EFIAPI *GetBarAttributes)(struct EFI_PCI_IO_PROTOCOL *This, UINT8 BarIndex,
	    UINT64 *Supports, VOID **Resources);
	EFI_STATUS (EFIAPI *SetBarAttributes)(struct EFI_PCI_IO_PROTOCOL *This, UINT64 Attributes,
	    UINT8 BarIndex, UINT64 *Offset, UINT64 *Length);
	UINT64 RomSize;
	VOID *RomImage;
} EFI_PCI_IO_PROTOCOL;

/* ------------------------------------------------------------ legacy region [PI] Vol. 5 ch. 15 */

typedef enum { LegacyRegionDecoded, LegacyRegionNotDecoded, LegacyRegionWriteEnabled,
	LegacyRegionWriteDisabled, LegacyRegionBootLocked, LegacyRegionNotLocked } EFI_LEGACY_REGION_ATTRIBUTE;

typedef struct {
	UINT32 Start;
	UINT32 Length;
	EFI_LEGACY_REGION_ATTRIBUTE Attribute;
	UINT32 Granularity;
} EFI_LEGACY_REGION_DESCRIPTOR;

typedef struct EFI_LEGACY_REGION2_PROTOCOL {
	EFI_STATUS (EFIAPI *Decode)(struct EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length,
	    UINT32 *Granularity, BOOLEAN *On);
	EFI_STATUS (EFIAPI *Lock)(struct EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length,
	    UINT32 *Granularity);
	EFI_STATUS (EFIAPI *BootLock)(struct EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length,
	    UINT32 *Granularity);
	EFI_STATUS (EFIAPI *UnLock)(struct EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length,
	    UINT32 *Granularity);
	EFI_STATUS (EFIAPI *GetInfo)(struct EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 *DescriptorCount,
	    EFI_LEGACY_REGION_DESCRIPTOR **Descriptor);
} EFI_LEGACY_REGION2_PROTOCOL;

/* ------------------------------------------------------------ GUIDs */

#define EFI_GUID_DEF(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
	{ a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }

#define EFI_LOADED_IMAGE_PROTOCOL_GUID EFI_GUID_DEF(0x5B1B31A1, 0x9562, 0x11d2, 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B)
#define EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID EFI_GUID_DEF(0xbc62157e, 0x3e33, 0x4fec, 0x99, 0x20, 0x2d, 0x3b, 0x36, 0xd7, 0x50, 0xdf)
#define EFI_DEVICE_PATH_PROTOCOL_GUID EFI_GUID_DEF(0x09576e91, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID EFI_GUID_DEF(0x387477c1, 0x69c7, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID EFI_GUID_DEF(0x387477c2, 0x69c7, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_SIMPLE_POINTER_PROTOCOL_GUID EFI_GUID_DEF(0x31878c87, 0x0b75, 0x11d5, 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d)
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID EFI_GUID_DEF(0x9042a9de, 0x23dc, 0x4a38, 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a)
#define EFI_BLOCK_IO_PROTOCOL_GUID EFI_GUID_DEF(0x964e5b21, 0x6459, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_DISK_IO_PROTOCOL_GUID EFI_GUID_DEF(0xCE345171, 0xBA0B, 0x11d2, 0x8e, 0x4F, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID EFI_GUID_DEF(0x964e5b22, 0x6459, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_FILE_INFO_ID EFI_GUID_DEF(0x09576e92, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_FILE_SYSTEM_INFO_ID EFI_GUID_DEF(0x09576e93, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b)
#define EFI_SERIAL_IO_PROTOCOL_GUID EFI_GUID_DEF(0xBB25CF6F, 0xF1D4, 0x11D2, 0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0xfd)
#define EFI_PCI_IO_PROTOCOL_GUID EFI_GUID_DEF(0x4cf5b200, 0x68b8, 0x4ca5, 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a)
#define EFI_LEGACY_REGION2_PROTOCOL_GUID EFI_GUID_DEF(0x70101eaf, 0x0085, 0x440c, 0xb3, 0x56, 0x8e, 0xe3, 0x6f, 0xef, 0x24, 0xf0)
#define EFI_GLOBAL_VARIABLE EFI_GUID_DEF(0x8BE4DF61, 0x93CA, 0x11d2, 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C)
#define EFI_EVENT_GROUP_EXIT_BOOT_SERVICES EFI_GUID_DEF(0x27abf055, 0xb1b8, 0x4c26, 0x80, 0x48, 0x74, 0x8f, 0x37, 0xba, 0xa2, 0xdf)
#define EFI_EVENT_GROUP_READY_TO_BOOT EFI_GUID_DEF(0x7ce88fb3, 0x4bd7, 0x4679, 0x87, 0xa8, 0xa8, 0xd8, 0xde, 0xe5, 0x0d, 0x2b)
#define EFI_ACPI_TABLE_GUID EFI_GUID_DEF(0xeb9d2d30, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d)
#define EFI_ACPI_20_TABLE_GUID EFI_GUID_DEF(0x8868e871, 0xe4f1, 0x11d3, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81)
#define SMBIOS_TABLE_GUID EFI_GUID_DEF(0xeb9d2d31, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d)
#define SMBIOS3_TABLE_GUID EFI_GUID_DEF(0xf2fd1544, 0x9794, 0x4a2c, 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94)
#define EFI_PC_ANSI_GUID EFI_GUID_DEF(0xe0c14753, 0xf9be, 0x11d2, 0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d)

#endif
