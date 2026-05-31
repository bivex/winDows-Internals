#include <fltKernel.h>
#include "sentinel.h"

// ---------------------------------------------------------------------------
// Helper: send operation to usermode and wait for verdict
// ---------------------------------------------------------------------------
static SENTINEL_VERDICT QueryUsermode(
    SENTINEL_MSG_TYPE Type,
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects)
{
    SENTINEL_MESSAGE msg = { 0 };
    SENTINEL_REPLY reply = { 0 };
    ULONG replyLen = sizeof(reply);
    NTSTATUS status;

    if (!g_ClientPort)
        return SentinelVerdict_Allow;  // no usermode client — passthrough

    // Fill message
    msg.Type            = Type;
    msg.ProcessId       = (ULONG)PsGetCurrentProcessId();
    msg.ThreadId        = (ULONG)PsGetCurrentThreadId();

    // Get full path
    if (FltObjects && FltObjects->FileObject && FltObjects->FileObject->FileName.Buffer) {
        ULONG len = FltObjects->FileObject->FileName.Length / sizeof(WCHAR);
        if (len >= SENTINEL_MAX_PATH)
            len = SENTINEL_MAX_PATH - 1;
        RtlCopyMemory(msg.FilePath,
                       FltObjects->FileObject->FileName.Buffer,
                       len * sizeof(WCHAR));
        msg.FilePath[len] = L'\0';
    }

    // Send synchronously — blocks until usermode replies
    status = FltSendMessage(g_Filter, &g_ClientPort,
                            &msg, sizeof(msg),
                            &reply, &replyLen, NULL);
    if (!NT_SUCCESS(status))
        return SentinelVerdict_Allow;  // timeout/error — passthrough

    return reply.Verdict;
}

// ===========================================================================
// PreCreate — intercept file open/create
// ===========================================================================
FLT_PREOP_CALLBACK_STATUS PreCreate(
    PFLT_CALLBACK_DATA   Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID                *CompletionContext)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    // Only interested in writes / creates
    ACCESS_MASK access = Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    ULONG options = Data->Iopb->Parameters.Create.Options;
    ULONG disposition = (options >> 24) & 0xFF;

    BOOLEAN wantsWrite = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    BOOLEAN isCreate   = (disposition == FILE_CREATE || disposition == FILE_SUPERSEDE ||
                          disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF);

    if (!wantsWrite && !isCreate)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    SENTINEL_VERDICT v = QueryUsermode(SentinelMsg_FileCreate, Data, FltObjects);

    if (v == SentinelVerdict_Deny) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        return FLT_PREOP_COMPLETE;  // BLOCKED
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

// ===========================================================================
// PreWrite — intercept file write
// ===========================================================================
FLT_PREOP_CALLBACK_STATUS PreWrite(
    PFLT_CALLBACK_DATA   Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID                *CompletionContext)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    SENTINEL_VERDICT v = QueryUsermode(SentinelMsg_FileWrite, Data, FltObjects);

    if (v == SentinelVerdict_Deny) {
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        return FLT_PREOP_COMPLETE;  // BLOCKED
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

// ===========================================================================
// PreSetInfo — intercept rename / delete
// ===========================================================================
FLT_PREOP_CALLBACK_STATUS PreSetInfo(
    PFLT_CALLBACK_DATA   Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID                *CompletionContext)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    FILE_INFORMATION_CLASS infoClass = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;

    if (infoClass == FileDispositionInformation ||
        infoClass == FileDispositionInformationEx) {
        SENTINEL_VERDICT v = QueryUsermode(SentinelMsg_FileDelete, Data, FltObjects);
        if (v == SentinelVerdict_Deny) {
            Data->IoStatus.Status = STATUS_ACCESS_DENIED;
            return FLT_PREOP_COMPLETE;
        }
    }

    if (infoClass == FileRenameInformation ||
        infoClass == FileRenameInformationEx) {
        SENTINEL_VERDICT v = QueryUsermode(SentinelMsg_FileRename, Data, FltObjects);
        if (v == SentinelVerdict_Deny) {
            Data->IoStatus.Status = STATUS_ACCESS_DENIED;
            return FLT_PREOP_COMPLETE;
        }
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}
