#pragma once

//
// FileSentinel — minifilter driver for file monitoring/blocking
// Shared definitions between kernel driver and usermode service
//

// ---------------------------------------------------------------------------
// Device name and symlink
// ---------------------------------------------------------------------------
#define SENTINEL_DEVICE_NAME   L"\\Device\\FileSentinel"
#define SENTINEL_SYMLINK_NAME  L"\\??\\FileSentinel"
#define SENTINEL_USER_PATH     L"\\\\.\\FileSentinel"

// ---------------------------------------------------------------------------
// Altitude — above FSFilter Anti-Virus, below Defender
// ---------------------------------------------------------------------------
#define SENTINEL_ALTITUDE   L"327000"

// ---------------------------------------------------------------------------
// IOCTL codes
// ---------------------------------------------------------------------------
#define FILE_DEVICE_SENTINEL  0x8000

#define IOCTL_SENTINEL_GET_EVENT \
    CTL_CODE(FILE_DEVICE_SENTINEL, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_SENTINEL_REPLY_EVENT \
    CTL_CODE(FILE_DEVICE_SENTINEL, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---------------------------------------------------------------------------
// Message types (kernel -> usermode)
// ---------------------------------------------------------------------------
typedef enum _SENTINEL_MSG_TYPE {
    SentinelMsg_FileCreate   = 1,
    SentinelMsg_FileWrite    = 2,
    SentinelMsg_FileDelete   = 3,
    SentinelMsg_FileRename   = 4,
} SENTINEL_MSG_TYPE;

// ---------------------------------------------------------------------------
// Operation result (usermode -> kernel)
// ---------------------------------------------------------------------------
typedef enum _SENTINEL_VERDICT {
    SentinelVerdict_Allow    = 0,
    SentinelVerdict_Deny     = 1,
} SENTINEL_VERDICT;

// ---------------------------------------------------------------------------
// Max path length for messages
// ---------------------------------------------------------------------------
#define SENTINEL_MAX_PATH  520

// ---------------------------------------------------------------------------
// Kernel -> Usermode event
// ---------------------------------------------------------------------------
typedef struct _SENTINEL_MESSAGE {
    SENTINEL_MSG_TYPE  Type;
    unsigned long      ProcessId;
    unsigned long      ThreadId;
    unsigned long      DesiredAccess;
    unsigned long      CreateDisposition;
    wchar_t            FilePath[SENTINEL_MAX_PATH];
} SENTINEL_MESSAGE, *PSENTINEL_MESSAGE;

// ---------------------------------------------------------------------------
// Usermode -> Kernel reply
// ---------------------------------------------------------------------------
typedef struct _SENTINEL_REPLY {
    SENTINEL_VERDICT    Verdict;
} SENTINEL_REPLY, *PSENTINEL_REPLY;

// ---------------------------------------------------------------------------
// Max pending operations
// ---------------------------------------------------------------------------
#define SENTINEL_MAX_PENDING  256
