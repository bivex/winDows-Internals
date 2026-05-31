#include <fltKernel.h>
#include "sentinel.h"

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);
NTSTATUS DriverUnload(FLT_FILTER_UNLOAD_FLAGS);
NTSTATUS InstanceSetup(PCFLT_RELATED_OBJECTS, FLT_INSTANCE_SETUP_FLAGS,
                       ULONG, FLT_INSTANCE_SETUP_FLAGS);
NTSTATUS ConnectNotify(PFLT_PORT, PVOID);
void     DisconnectNotify(PFLT_PORT, PVOID);
NTSTATUS MessageNotify(PFLT_PORT, PVOID, ULONG, PVOID *, PULONG);

FLT_PREOP_CALLBACK_STATUS PreCreate(PFLT_CALLBACK_DATA, PCFLT_RELATED_OBJECTS, PVOID *);
FLT_PREOP_CALLBACK_STATUS PreWrite(PFLT_CALLBACK_DATA, PCFLT_RELATED_OBJECTS, PVOID *);
FLT_PREOP_CALLBACK_STATUS PreSetInfo(PFLT_CALLBACK_DATA, PCFLT_RELATED_OBJECTS, PVOID *);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
PFLT_FILTER  g_Filter  = NULL;
PFLT_PORT    g_ServerPort = NULL;
PFLT_PORT    g_ClientPort = NULL;

// ---------------------------------------------------------------------------
// Operation registration
// ---------------------------------------------------------------------------
const FLT_OPERATION_REGISTRATION Callbacks[] = {
    { IRP_MJ_CREATE,          0, PreCreate,  NULL },
    { IRP_MJ_WRITE,           0, PreWrite,   NULL },
    { IRP_MJ_SET_INFORMATION, 0, PreSetInfo, NULL },
    { IRP_MJ_OPERATION_END }
};

// ---------------------------------------------------------------------------
// Filter registration
// ---------------------------------------------------------------------------
const FLT_CONTEXT_REGISTRATION NoContexts[] = {
    { FLT_CONTEXT_END }
};

const FLT_REGISTRATION FilterReg = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    NoContexts,
    Callbacks,
    DriverUnload,
    InstanceSetup,
    NULL,  // InstanceQueryTeardown
    NULL,  // InstanceTeardownStart
    NULL,  // InstanceTeardownComplete
    NULL,  // GenerateFileName
    NULL,  // NormalizeNameComponent
    NULL,  // NormalizeContextCleanup
    SENTINEL_ALTITUDE,
};

// ===========================================================================
// DriverEntry
// ===========================================================================
NTSTATUS DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING portName;
    PSECURITY_DESCRIPTOR sd;
    OBJECT_ATTRIBUTES oa;

    UNREFERENCED_PARAMETER(RegistryPath);

    // Register the filter
    status = FltRegisterFilter(DriverObject, &FilterReg, &g_Filter);
    if (!NT_SUCCESS(status))
        return status;

    // Create security descriptor: allow Everyone to connect
    sd = NULL;
    status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status))
        goto fail;

    RtlInitUnicodeString(&portName, SENTINEL_PORT_NAME);
    InitializeObjectAttributes(&oa, &portName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, sd);

    // Create communication port
    status = FltCreateCommunicationPort(g_Filter, &g_ServerPort, &oa,
                                        NULL, ConnectNotify, DisconnectNotify,
                                        MessageNotify, 1);
    FltFreeSecurityDescriptor(sd);

    if (!NT_SUCCESS(status))
        goto fail;

    // Start filtering
    status = FltStartFiltering(g_Filter);
    if (!NT_SUCCESS(status))
        goto fail_port;

    DbgPrint("FileSentinel: loaded, altitude=%ws\n", SENTINEL_ALTITUDE);
    return STATUS_SUCCESS;

fail_port:
    FltCloseCommunicationPort(g_ServerPort);
fail:
    FltUnregisterFilter(g_Filter);
    return status;
}

// ===========================================================================
// Unload
// ===========================================================================
NTSTATUS DriverUnload(FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    if (g_ClientPort)
        FltCloseCommunicationPort(g_ClientPort);
    if (g_ServerPort)
        FltCloseCommunicationPort(g_ServerPort);
    if (g_Filter)
        FltUnregisterFilter(g_Filter);

    DbgPrint("FileSentinel: unloaded\n");
    return STATUS_SUCCESS;
}

// ===========================================================================
// Instance setup
// ===========================================================================
NTSTATUS InstanceSetup(
    PCFLT_RELATED_OBJECTS  FltObjects,
    FLT_INSTANCE_SETUP_FLAGS Flags,
    ULONG                  VolumeFolderType,
    FLT_INSTANCE_SETUP_FLAGS VolumeFlags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeFolderType);
    UNREFERENCED_PARAMETER(VolumeFlags);
    return STATUS_SUCCESS;
}

// ===========================================================================
// Communication port callbacks
// ===========================================================================
NTSTATUS ConnectNotify(PFLT_PORT Port, PVOID PortContext)
{
    UNREFERENCED_PARAMETER(PortContext);

    // Only allow one client at a time
    if (g_ClientPort)
        return STATUS_CONNECTION_REFUSED;

    g_ClientPort = Port;
    DbgPrint("FileSentinel: usermode client connected\n");
    return STATUS_SUCCESS;
}

void DisconnectNotify(PFLT_PORT Port, PVOID PortContext)
{
    UNREFERENCED_PARAMETER(PortContext);

    if (g_ClientPort == Port) {
        g_ClientPort = NULL;
        FltCloseCommunicationPort(Port);
    }
    DbgPrint("FileSentinel: usermode client disconnected\n");
}

NTSTATUS MessageNotify(
    PFLT_PORT  Port,
    PVOID      Buffer,
    ULONG      BufferSize,
    PVOID     *ReplyBuffer,
    PULONG     ReplyLength)
{
    UNREFERENCED_PARAMETER(Port);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BufferSize);

    // Usermode sends config/rules here. For now, simple ping.
    static SENTINEL_REPLY reply = { 0 };
    reply.Header.Status = 0;
    reply.Header.MessageId = 0;
    reply.Verdict = SentinelVerdict_Allow;

    *ReplyBuffer = &reply;
    *ReplyLength = sizeof(reply);
    return STATUS_SUCCESS;
}
