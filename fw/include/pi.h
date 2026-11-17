/*
 * pi.h: the Platform Initialization structures the firmware's phases share
 * (UEFI Forum, "UEFI Platform Initialization Specification" 1.8, Vol. 3
 * "Shared Architectural Elements": firmware volumes ch. 3, firmware file
 * sections ch. 2.1, HOBs ch. 4-5), and the PE32+ headers the image loaders
 * read (Microsoft, "PE Format", learn.microsoft.com, 2024).
 */
#ifndef PI_H
#define PI_H

#include "efi.h"

/* ------------------------------------------------------------ firmware volumes */

#define EFI_FVH_SIGNATURE	0x4856465f	/* "_FVH" */

typedef struct {
	UINT32 NumBlocks;
	UINT32 Length;
} EFI_FV_BLOCK_MAP_ENTRY;

typedef struct {
	UINT8 ZeroVector[16];
	EFI_GUID FileSystemGuid;
	UINT64 FvLength;
	UINT32 Signature;
	UINT32 Attributes;
	UINT16 HeaderLength;
	UINT16 Checksum;
	UINT16 ExtHeaderOffset;
	UINT8 Reserved[1];
	UINT8 Revision;
	EFI_FV_BLOCK_MAP_ENTRY BlockMap[1];
} EFI_FIRMWARE_VOLUME_HEADER;

typedef struct {
	EFI_GUID Name;
	union {
		struct {
			UINT8 Header;
			UINT8 File;
		} Checksum;
		UINT16 Checksum16;
	} IntegrityCheck;
	UINT8 Type;
	UINT8 Attributes;
	UINT8 Size[3];
	UINT8 State;
} EFI_FFS_FILE_HEADER;

#define EFI_FV_FILETYPE_RAW			0x01
#define EFI_FV_FILETYPE_FREEFORM		0x02
#define EFI_FV_FILETYPE_SECURITY_CORE		0x03
#define EFI_FV_FILETYPE_PEI_CORE		0x04
#define EFI_FV_FILETYPE_DXE_CORE		0x05
#define EFI_FV_FILETYPE_DRIVER			0x07
#define EFI_FV_FILETYPE_APPLICATION		0x09
#define EFI_FV_FILETYPE_FFS_PAD			0xF0

typedef struct {
	UINT8 Size[3];
	UINT8 Type;
} EFI_COMMON_SECTION_HEADER;

#define EFI_SECTION_PE32		0x10
#define EFI_SECTION_USER_INTERFACE	0x15
#define EFI_SECTION_RAW			0x19

#define FV_SIZE24(a)	((UINT32)(a)[0] | (UINT32)(a)[1] << 8 | (UINT32)(a)[2] << 16)

/* EFI_FIRMWARE_FILE_SYSTEM2_GUID */
#define EFI_FIRMWARE_FILE_SYSTEM2_GUID EFI_GUID_DEF(0x8c8ce578, 0x8a3d, 0x4f1c, 0x99, 0x35, 0x89, 0x61, 0x85, 0xc3, 0x2d, 0xd3)

/* ------------------------------------------------------------ HOBs */

#define EFI_HOB_TYPE_HANDOFF		0x0001
#define EFI_HOB_TYPE_MEMORY_ALLOCATION	0x0002
#define EFI_HOB_TYPE_RESOURCE_DESCRIPTOR 0x0003
#define EFI_HOB_TYPE_GUID_EXTENSION	0x0004
#define EFI_HOB_TYPE_FV			0x0005
#define EFI_HOB_TYPE_CPU		0x0006
#define EFI_HOB_TYPE_END_OF_HOB_LIST	0xFFFF

typedef struct {
	UINT16 HobType;
	UINT16 HobLength;
	UINT32 Reserved;
} EFI_HOB_GENERIC_HEADER;

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	UINT32 Version;
	UINT32 BootMode;
	EFI_PHYSICAL_ADDRESS EfiMemoryTop;
	EFI_PHYSICAL_ADDRESS EfiMemoryBottom;
	EFI_PHYSICAL_ADDRESS EfiFreeMemoryTop;
	EFI_PHYSICAL_ADDRESS EfiFreeMemoryBottom;
	EFI_PHYSICAL_ADDRESS EfiEndOfHobList;
} EFI_HOB_HANDOFF_INFO_TABLE;
#define EFI_HOB_HANDOFF_TABLE_VERSION	0x0009
#define BOOT_WITH_FULL_CONFIGURATION	0x00

typedef struct {
	EFI_GUID Name;
	EFI_PHYSICAL_ADDRESS MemoryBaseAddress;
	UINT64 MemoryLength;
	UINT32 MemoryType;	/* EFI_MEMORY_TYPE */
	UINT8 Reserved[4];
} EFI_HOB_MEMORY_ALLOCATION_HEADER;

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	EFI_HOB_MEMORY_ALLOCATION_HEADER AllocDescriptor;
} EFI_HOB_MEMORY_ALLOCATION;

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	EFI_HOB_MEMORY_ALLOCATION_HEADER MemoryAllocationHeader;
	EFI_GUID ModuleName;
	EFI_PHYSICAL_ADDRESS EntryPoint;
} EFI_HOB_MEMORY_ALLOCATION_MODULE;

#define EFI_RESOURCE_SYSTEM_MEMORY	0x00
#define EFI_RESOURCE_MEMORY_MAPPED_IO	0x01
#define EFI_RESOURCE_IO			0x02
#define EFI_RESOURCE_FIRMWARE_DEVICE	0x03
#define EFI_RESOURCE_MEMORY_RESERVED	0x05

#define EFI_RESOURCE_ATTRIBUTE_PRESENT		0x01
#define EFI_RESOURCE_ATTRIBUTE_INITIALIZED	0x02
#define EFI_RESOURCE_ATTRIBUTE_TESTED		0x04
#define EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE	0x400
#define EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE 0x2000

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	EFI_GUID Owner;
	UINT32 ResourceType;
	UINT32 ResourceAttribute;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	UINT64 ResourceLength;
} EFI_HOB_RESOURCE_DESCRIPTOR;

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	EFI_GUID Name;
	/* data follows */
} EFI_HOB_GUID_TYPE;

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	EFI_PHYSICAL_ADDRESS BaseAddress;
	UINT64 Length;
} EFI_HOB_FIRMWARE_VOLUME;

typedef struct {
	EFI_HOB_GENERIC_HEADER Header;
	UINT8 SizeOfMemorySpace;
	UINT8 SizeOfIoSpace;
	UINT8 Reserved[6];
} EFI_HOB_CPU;

/* ------------------------------------------------------------ PE32+ */

#define IMAGE_DOS_SIGNATURE	0x5A4D		/* "MZ" */
#define IMAGE_NT_SIGNATURE	0x00004550	/* "PE\0\0" */
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20B
#define IMAGE_SUBSYSTEM_EFI_APPLICATION		10
#define IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER	11
#define IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER	12
#define IMAGE_DIRECTORY_ENTRY_BASERELOC	5
#define IMAGE_REL_BASED_ABSOLUTE	0
#define IMAGE_REL_BASED_DIR64		10

typedef struct {
	UINT16 e_magic;
	UINT16 e_res[29];
	UINT32 e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct {
	UINT16 Machine;
	UINT16 NumberOfSections;
	UINT32 TimeDateStamp;
	UINT32 PointerToSymbolTable;
	UINT32 NumberOfSymbols;
	UINT16 SizeOfOptionalHeader;
	UINT16 Characteristics;
} IMAGE_FILE_HEADER;

typedef struct {
	UINT32 VirtualAddress;
	UINT32 Size;
} IMAGE_DATA_DIRECTORY;

typedef struct {
	UINT16 Magic;
	UINT8 MajorLinkerVersion, MinorLinkerVersion;
	UINT32 SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
	UINT32 AddressOfEntryPoint;
	UINT32 BaseOfCode;
	UINT64 ImageBase;
	UINT32 SectionAlignment;
	UINT32 FileAlignment;
	UINT16 MajorOperatingSystemVersion, MinorOperatingSystemVersion;
	UINT16 MajorImageVersion, MinorImageVersion;
	UINT16 MajorSubsystemVersion, MinorSubsystemVersion;
	UINT32 Win32VersionValue;
	UINT32 SizeOfImage;
	UINT32 SizeOfHeaders;
	UINT32 CheckSum;
	UINT16 Subsystem;
	UINT16 DllCharacteristics;
	UINT64 SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
	UINT32 LoaderFlags;
	UINT32 NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER64;

typedef struct {
	UINT32 Signature;
	IMAGE_FILE_HEADER FileHeader;
	IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

typedef struct {
	UINT8 Name[8];
	UINT32 VirtualSize;
	UINT32 VirtualAddress;
	UINT32 SizeOfRawData;
	UINT32 PointerToRawData;
	UINT32 PointerToRelocations;
	UINT32 PointerToLinenumbers;
	UINT16 NumberOfRelocations;
	UINT16 NumberOfLinenumbers;
	UINT32 Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct {
	UINT32 VirtualAddress;
	UINT32 SizeOfBlock;
} IMAGE_BASE_RELOCATION;

#endif
