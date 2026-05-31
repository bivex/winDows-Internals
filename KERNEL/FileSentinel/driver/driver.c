#include <fltKernel.h>
#include "sentinel.h"

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);
NTSTATUS DriverUnload(FLT_FILTER_UNLOAD_FLAGS);
NTSTATUS InstanceSetup(PCFLT_RELATED_OBJECTS, FLT_INSTANCE_SETUP_FLAGS,
                       ULONG, FLT_INSTANCE_SETUP_FLAGS);
FLT_PREOP_CALLBACK_STATUS PreCreate(PFLT_CALLBACK_DATA, PCFLT_RELATED_OBJECTS, PVOID *);
FLT_PREOP_CALLBACK_STATUS PreWrite(PFLT_CALLBACK_DATA, PCFLT_RELATED_OBJECTS, PVOID *);
FLT_PREOP_CALLBACK_STATUS PreSetInfo(PFLT_CALLBACK_DATA, PCFLT_RELATED_OBJECTS, PVOID *);

NTSTATUS DispatchCreate(PDEVICE_OBJECT, PIRP);
NTSTATUS DispatchClose(PDEVICE_OBJECT, PIRP);
NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT, PIRP);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
PFLT_FILTER   g_Filter       = NULL;
PDEVICE_OBJECT g_DeviceObject = NULL;
UNICODE_STRING g_SymLinkName  = { 0 };

// Pending event queue (simple single-slot for now)
typedef struct _PENDING_EVENT {
    SENTINEL_MESSAGE Message;
    BOOLEAN          Valid;
    KEVENT           Completed;
    SENTINEL_VERDICT Verdict;
} PENDING_EVENT;

PENDING_EVENT g_PendingEvent = { 0 };
KMUTEX        g_EventLock    = { 0 };

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
    NULL, NULL, NULL, NULL, NULL, NULL,
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
    UNICODE_STRING devName;
    PSECURITY_DESCRIPTOR sd;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING portName;

    UNREFERENCED_PARAMETER(RegistryPath);

    // Initialize synchronization
    KeInitializeMutex(&g_EventLock, 0);
    g_PendingEvent.Valid = FALSE;

    // Create control device object
    RtlInitUnicodeString(&devName, SENTINEL_DEVICE_NAME);
    status = IoCreateDevice(DriverObject, 0, &devName,
                            FILE_DEVICE_UNKNOWN, 0, FALSE, &g_DeviceObject);
    if (!NT_SUCCESS(status))
        return status;

    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    // Create symbolic link for usermode access
    RtlInitUnicodeString(&g_SymLinkName, SENTINEL_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&g_SymLinkName, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        return status;
    }

    // Set up IRP dispatch routines for the control device
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = DispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]           = DispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]  = DispatchDeviceControl;

    // Register the minifilter
    status = FltRegisterFilter(DriverObject, &FilterReg, &g_Filter);
    if (!NT_SUCCESS(status))
        goto fail_sym;

    // Start filtering
    status = FltStartFiltering(g_Filter);
    if (!NT_SUCCESS(status))
        goto fail_filter;

    DbgPrint("FileSentinel: loaded, altitude=%ws\n", SENTINEL_ALTITUDE);
    return STATUS_SUCCESS;

fail_filter:
    FltUnregisterFilter(g_Filter);
fail_sym:
    IoDeleteSymbolicLink(&g_SymLinkName);
    IoDeleteDevice(g_DeviceObject);
    return status;
}

// ===========================================================================
// Unload
// ===========================================================================
NTSTATUS DriverUnload(FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    if (g_Filter)
        FltUnregisterFilter(g_Filter);
    if (g_SymLinkName.Buffer)
        IoDeleteSymbolicLink(&g_SymLinkName);
    if (g_DeviceObject)
        IoDeleteDevice(g_DeviceObject);

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
// IRP Dispatch: Create / Close
// ===========================================================================
NTSTATUS DispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    DbgPrint("FileSentinel: usermode client connected\n");
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    DbgPrint("FileSentinel: usermode client disconnected\n");
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// ===========================================================================
// IRP Dispatch: DeviceControl
// ===========================================================================
NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = STATUS_SUCCESS;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen  = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG retLen = 0;

    switch (code) {
    case IOCTL_SENTINEL_GET_EVENT:
        // Usermode polls for pending event
        if (outLen < sizeof(SENTINEL_MESSAGE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KeWaitForMutexObject(&g_EventLock, Executive, KernelMode, FALSE, NULL);
        if (g_PendingEvent.Valid) {
            RtlCopyMemory(buffer, &g_PendingEvent.Message, sizeof(SENTINEL_MESSAGE));
            retLen = sizeof(SENTINEL_MESSAGE);
        } else {
            status = STATUS_NO_MORE_ENTRIES;
        }
        KeReleaseMutex(&g_EventLock, FALSE);
        break;

    case IOCTL_SENTINEL_REPLY_EVENT:
        // Usermode sends verdict
        if (inLen < sizeof(SENTINEL_REPLY)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KeWaitForMutexObject(&g_EventLock, Executive, KernelMode, FALSE, NULL);
        if (g_PendingEvent.Valid) {
            PSENTINEL_REPLY reply = (PSENTINEL_REPLY)buffer;
            g_PendingEvent.Verdict = reply->Verdict;
            g_PendingEvent.Valid = FALSE;
            KeSetEvent(&g_PendingEvent.Completed, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseMutex(&g_EventLock, FALSE);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = retLen;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// ===========================================================================
// Helper: queue event and wait for usermode verdict (called from callbacks.c)
// ===========================================================================
NTSTATUS SentinelQueueEvent(PSENTINEL_MESSAGE Msg, SENTINEL_VERDICT *Verdict)
{
    KeWaitForMutexObject(&g_EventLock, Executive, KernelMode, FALSE, NULL);

    if (g_PendingEvent.Valid) {
        // Already a pending event — allow this one (don't block)
        KeReleaseMutex(&g_EventLock, FALSE);
        *Verdict = SentinelVerdict_Allow;
        return STATUS_SUCCESS;
    }

    KeInitializeEvent(&g_PendingEvent.Completed, NotificationEvent, FALSE);
    RtlCopyMemory(&g_PendingEvent.Message, Msg, sizeof(SENTINEL_MESSAGE));
    g_PendingEvent.Valid = TRUE;
    g_PendingEvent.Verdict = SentinelVerdict_Allow;

    KeReleaseMutex(&g_EventLock, FALSE);

    // Wait for usermode to reply (timeout 5 seconds)
    LARGE_INTEGER timeout;
    timeout.QuadPart = -50000000LL; // 5 seconds in 100ns units
    NTSTATUS status = KeWaitForSingleObject(&g_PendingEvent.Completed, Executive,
                                            KernelMode, FALSE, &timeout);

    if (status == STATUS_TIMEOUT) {
        // Timed out — cancel the event
        KeWaitForMutexObject(&g_EventLock, Executive, KernelMode, FALSE, NULL);
        g_PendingEvent.Valid = FALSE;
        KeReleaseMutex(&g_EventLock, FALSE);
        *Verdict = SentinelVerdict_Allow;
        return STATUS_TIMEOUT;
    }

    KeWaitForMutexObject(&g_EventLock, Executive, KernelMode, FALSE, NULL);
    *Verdict = g_PendingEvent.Verdict;
    KeReleaseMutex(&g_EventLock, FALSE);
    return STATUS_SUCCESS;
}
