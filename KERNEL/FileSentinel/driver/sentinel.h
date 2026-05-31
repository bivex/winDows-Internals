#pragma once

//
// FileSentinel — minifilter driver for file monitoring/blocking
// Shared definitions between kernel driver and usermode service
//

// ---------------------------------------------------------------------------
// Communication port name
// ---------------------------------------------------------------------------
#define SENTINEL_PORT_NAME  L"\\FileSentinelPort"
#define SENTINEL_PORT_USER  L"\\\\.\\FileSentinelPort"

// ---------------------------------------------------------------------------
// Altitude — above FSFilter Anti-Virus, below Defender
// ---------------------------------------------------------------------------
#define SENTINEL_ALTITUDE   L"327000"

// ---------------------------------------------------------------------------
// Message types (kernel -> usermode)
// ---------------------------------------------------------------------------
typedef enum _SENTINEL_MSG_TYPE {
    SentinelMsg_FileCreate   = 1,   // IRP_MJ_CREATE
    SentinelMsg_FileWrite    = 2,   // IRP_MJ_WRITE
    SentinelMsg_FileDelete   = 3,   // IRP_MJ_SET_INFORMATION (FileDispositionInfo)
    SentinelMsg_FileRename   = 4,   // IRP_MJ_SET_INFORMATION (FileRenameInfo)
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
#define SENTINEL_MAX_PATH  520  // UNICODE chars (1040 bytes)

// ---------------------------------------------------------------------------
// Kernel -> Usermode message (no Filter Manager headers — use ReadFile/WriteFile)
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
// Max pending operations (backpressure)
// ---------------------------------------------------------------------------
#define SENTINEL_MAX_PENDING  256
