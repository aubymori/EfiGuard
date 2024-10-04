#pragma once

//
// Minimal version of ntdef.h to avoid a dependency on the WDK
//

// Ignore this file if either ntdef.h or winnt.h has already been included elsewhere
#if !defined(_NTDEF_) && !defined(_WINNT_)

// DebugLib.h (re)defines _DEBUG without checking if it has already been defined. So get it now
#include <Library/DebugLib.h>

// Get the correct CPU and (non-)debug defines for NT from UEFI if we don't have them already
#if defined(MDE_CPU_X64)
	#if !defined(_WIN64)
		#define _WIN64
	#endif
	#if !defined(_AMD64_)
		#define _AMD64_
	#endif
#elif defined(MDE_CPU_IA32)
	#if !defined(_X86_)
		#define _X86_
	#endif
#endif
#if defined(EFI_DEBUG)
	#if !defined(_DEBUG)
		#define _DEBUG
	#endif
	#if !defined(DBG)
		#define DBG		1
	#endif
#endif
#if defined(MDEPKG_NDEBUG)
	#if !defined(NDEBUG)
		#define NDEBUG
	#endif
#endif

// Defines
#define ANYSIZE_ARRAY				1
#define FIELD_OFFSET(Type, Field)	((INT32)(INTN)&(((Type *)0)->Field))
#define MAKELANGID(Primary, Sub)	((((UINT16)(Sub)) << 10) | (UINT16)(Primary))
#define LANG_NEUTRAL				0x00
#define SUBLANG_NEUTRAL				0x00
#define RTL_CONSTANT_STRING(s) \
{ \
	(sizeof(s) - sizeof((s)[0])), \
	(sizeof(s)), \
	(s) \
}
#define LOWORD(l)					((UINT16)(((UINTN)(l)) & 0xffff))
#define HIWORD(l)					((UINT16)((((UINTN)(l)) >> 16) & 0xffff))
#define LOBYTE(w)					((UINT8)(((UINTN)(w)) & 0xff))
#define HIBYTE(w)					((UINT8)((((UINTN)(w)) >> 8) & 0xff))

#define FILE_ATTRIBUTE_READONLY           0x00000001
#define FILE_ATTRIBUTE_HIDDEN             0x00000002
#define FILE_ATTRIBUTE_SYSTEM             0x00000004
#define FILE_ATTRIBUTE_DIRECTORY          0x00000010
#define FILE_ATTRIBUTE_ARCHIVE            0x00000020
#define FILE_ATTRIBUTE_DEVICE             0x00000040
#define FILE_ATTRIBUTE_NORMAL             0x00000080
#define FILE_ATTRIBUTE_TEMPORARY          0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE        0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT      0x00000400
#define FILE_ATTRIBUTE_COMPRESSED         0x00000800
#define FILE_ATTRIBUTE_OFFLINE            0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED          0x00004000
#define FILE_ATTRIBUTE_VIRTUAL            0x00010000

#define FILE_ATTRIBUTE_VALID_FLAGS        0x00007fb7
#define FILE_ATTRIBUTE_VALID_SET_FLAGS    0x000031a7

#define FILE_DIRECTORY_FILE               0x00000001
#define FILE_WRITE_THROUGH                0x00000002
#define FILE_SEQUENTIAL_ONLY              0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING    0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT         0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT      0x00000020
#define FILE_NON_DIRECTORY_FILE           0x00000040
#define FILE_CREATE_TREE_CONNECTION       0x00000080
#define FILE_COMPLETE_IF_OPLOCKED         0x00000100
#define FILE_NO_EA_KNOWLEDGE              0x00000200
#define FILE_OPEN_REMOTE_INSTANCE         0x00000400
#define FILE_RANDOM_ACCESS                0x00000800
#define FILE_DELETE_ON_CLOSE              0x00001000
#define FILE_OPEN_BY_FILE_ID              0x00002000
#define FILE_OPEN_FOR_BACKUP_INTENT       0x00004000
#define FILE_NO_COMPRESSION               0x00008000
#if (NTDDI_VERSION >= NTDDI_WIN7)
#define FILE_OPEN_REQUIRING_OPLOCK        0x00010000
#define FILE_DISALLOW_EXCLUSIVE           0x00020000
#endif /* (NTDDI_VERSION >= NTDDI_WIN7) */
#define FILE_RESERVE_OPFILTER             0x00100000
#define FILE_OPEN_REPARSE_POINT           0x00200000
#define FILE_OPEN_NO_RECALL               0x00400000
#define FILE_OPEN_FOR_FREE_SPACE_QUERY    0x00800000

#define FILE_SUPERSEDE                    0x00000000
#define FILE_OPEN                         0x00000001
#define FILE_CREATE                       0x00000002
#define FILE_OPEN_IF                      0x00000003
#define FILE_OVERWRITE                    0x00000004
#define FILE_OVERWRITE_IF                 0x00000005
#define FILE_MAXIMUM_DISPOSITION          0x00000005

/* Values for the Attributes member */
#define OBJ_INHERIT             0x00000002
#define OBJ_PERMANENT           0x00000010
#define OBJ_EXCLUSIVE           0x00000020
#define OBJ_CASE_INSENSITIVE    0x00000040
#define OBJ_OPENIF              0x00000080
#define OBJ_OPENLINK            0x00000100
#define OBJ_KERNEL_HANDLE       0x00000200
#define OBJ_FORCE_ACCESS_CHECK  0x00000400
#define OBJ_IGNORE_IMPERSONATED_DEVICEMAP 0x00000800
#define OBJ_DONT_REPARSE        0x00001000
#define OBJ_VALID_ATTRIBUTES    0x00001FF2

typedef struct _OBJECT_ATTRIBUTES64 {
	UINT32 Length;
	UINT64 RootDirectory;
	UINT64 ObjectName;
	UINT32 Attributes;
	UINT64 SecurityDescriptor;
	UINT64 SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef CONST OBJECT_ATTRIBUTES *PCOBJECT_ATTRIBUTES;

/* Helper Macro */
#define InitializeObjectAttributes(p,n,a,r,s) { \
	(p)->Length = sizeof(OBJECT_ATTRIBUTES); \
	(p)->RootDirectory = (UINT64)(r); \
	(p)->Attributes = (UINT32)(a); \
	(p)->ObjectName = (UINT64)(n); \
	(p)->SecurityDescriptor = (UINT64)(s); \
	(p)->SecurityQualityOfService = (UINT64)NULL; \
}

// Typedefs
/* Native API Return Value Macros */
#define NT_SUCCESS(Status)              (((NTSTATUS)(Status)) >= 0)
#define NT_INFORMATION(Status)          ((((ULONG)(Status)) >> 30) == 1)
#define NT_WARNING(Status)              ((((ULONG)(Status)) >> 30) == 2)
#define NT_ERROR(Status)                ((((ULONG)(Status)) >> 30) == 3)
typedef INT32 NTSTATUS;

typedef VOID *HANDLE, **PHANDLE;

#define DELETE                           0x00010000L
#define READ_CONTROL                     0x00020000L
#define WRITE_DAC                        0x00040000L
#define WRITE_OWNER                      0x00080000L
#define SYNCHRONIZE                      0x00100000L
#define STANDARD_RIGHTS_REQUIRED         0x000F0000L
#define STANDARD_RIGHTS_READ             READ_CONTROL
#define STANDARD_RIGHTS_WRITE            READ_CONTROL
#define STANDARD_RIGHTS_EXECUTE          READ_CONTROL
#define STANDARD_RIGHTS_ALL              0x001F0000L
#define SPECIFIC_RIGHTS_ALL              0x0000FFFFL
#define ACCESS_SYSTEM_SECURITY           0x01000000L
#define MAXIMUM_ALLOWED                  0x02000000L
#define GENERIC_READ                     0x80000000L
#define GENERIC_WRITE                    0x40000000L
#define GENERIC_EXECUTE                  0x20000000L
#define GENERIC_ALL                      0x10000000L
typedef UINT32 ACCESS_MASK, *PACCESS_MASK;

typedef struct _IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		VOID* Pointer;
	} DUMMYUNIONNAME;
	UINT64 Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef union _LARGE_INTEGER {
	struct {
		UINT32 LowPart;
		INT32 HighPart;
	} s;
	struct {
		UINT32 LowPart;
		INT32 HighPart;
	} u;
	INT64 QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _UNICODE_STRING {
	UINT16 Length;
	UINT16 MaximumLength;
	CHAR16* Buffer;
} UNICODE_STRING;
typedef UNICODE_STRING *PUNICODE_STRING;
typedef CONST UNICODE_STRING *PCUNICODE_STRING;

/* Interrupt request levels */
#define PASSIVE_LEVEL           0
#define LOW_LEVEL               0
#define APC_LEVEL               1
#define DISPATCH_LEVEL          2
#define CMCI_LEVEL              5
#define PROFILE_LEVEL           27
#define CLOCK1_LEVEL            28
#define CLOCK2_LEVEL            28
#define IPI_LEVEL               29
#define POWER_LEVEL             30
#define HIGH_LEVEL              31
#define CLOCK_LEVEL             CLOCK2_LEVEL
typedef CHAR8 KIRQL;

#endif // !defined(_NTDEF_) && !defined(_WINNT_)
