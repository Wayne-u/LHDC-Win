#include <ntddk.h>
#include <wdf.h>
#include <bthdef.h>
#include <bthioctl.h>
#include <bthddi.h>
#include <sdpnode.h>
#include <bthsdpddi.h>
#include <initguid.h>
#include <bthguid.h>
#include <lhdc_transport.h>
#include "trace.h"
#include "transport.tmh"

typedef struct DEVICE_CONTEXT DEVICE_CONTEXT;
typedef struct CHANNEL_CONTEXT {
    DEVICE_CONTEXT* DeviceContext;
    WDFQUEUE Queue;
    L2CAP_CHANNEL_HANDLE Handle;
    L2CAP_CHANNEL_HANDLE DisconnectHandle;
    LHDC_INFO Info;
    BOOLEAN Opening;
} CHANNEL_CONTEXT;
struct DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET Target;
    WDFQUEUE Queue;
    WDFSPINLOCK Lock;
    WDFWAITLOCK CloseLock;
    WDFWORKITEM DisconnectWork;
    BTH_PROFILE_DRIVER_INTERFACE Profile;
    CHANNEL_CONTEXT Channels[LHDC_CHANNEL_COUNT];
    BOOLEAN Ready;
    BOOLEAN SessionClosing;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)
typedef struct REQUEST_CONTEXT {
    CHANNEL_CONTEXT* Channel;
    BRB Brb;
    ULONG Ioctl;
    PVOID Output;
    ULONG Capacity;
    ULONG TxBytes;
} REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_CONTEXT, GetRequestContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DriverCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY D0Entry;
EVT_WDF_DEVICE_D0_EXIT D0Exit;
EVT_WDF_DEVICE_FILE_CREATE FileCreate;
EVT_WDF_FILE_CLEANUP FileCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL IoControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RouteControl;
EVT_WDF_IO_QUEUE_IO_STOP IoStop;
EVT_WDF_REQUEST_COMPLETION_ROUTINE BrbComplete;
EVT_WDF_WORKITEM DisconnectWorker;
NTSTATUS LhdcCreateAudioChild(WDFDEVICE parent);

static NTSTATUS SendSdpIoctl(DEVICE_CONTEXT* ctx, ULONG code, PVOID input,
    ULONG inputSize, PVOID output, ULONG outputSize)
{
    WDF_MEMORY_DESCRIPTOR inDescriptor, outDescriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDescriptor, input, inputSize);
    if (output != NULL) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDescriptor, output, outputSize);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    return WdfIoTargetSendIoctlSynchronously(ctx->Target, NULL, code,
        &inDescriptor, output != NULL ? &outDescriptor : NULL, &options, NULL);
}
static BOOLEAN RecordHasProtocol(BTHDDI_SDP_PARSE_INTERFACE* parser, PUCHAR record, ULONG size, USHORT psm)
{
    PSDP_TREE_ROOT_NODE tree = NULL;
    PSDP_NODE protocols = NULL;
    PLIST_ENTRY head, link;
    NTSTATUS status;
    BOOLEAN l2cap = FALSE, protocol = FALSE;
    status = parser->SdpConvertStreamToTree(record, size, &tree, 'cdhL');
    if (!NT_SUCCESS(status)) return FALSE;
    status = parser->SdpFindAttributeInTree(tree, 0x0004, &protocols);
    if (!NT_SUCCESS(status) || protocols->hdr.Type != SDP_TYPE_SEQUENCE) goto done;
    head = &protocols->u.sequence.Link;
    for (link = head->Flink; link != head; link = link->Flink) {
        PSDP_NODE sequence = CONTAINING_RECORD(link, SDP_NODE, hdr.Link);
        PSDP_NODE uuid, parameter;
        PLIST_ENTRY children;
        if (sequence->hdr.Type != SDP_TYPE_SEQUENCE) goto done;
        children = &sequence->u.sequence.Link;
        if (IsListEmpty(children)) goto done;
        uuid = CONTAINING_RECORD(children->Flink, SDP_NODE, hdr.Link);
        if (uuid->hdr.SpecificType != SDP_ST_UUID16) continue;
        if (uuid->u.uuid16 == psm) protocol = TRUE;
        if (uuid->u.uuid16 != 0x0100) continue;
        if (uuid->hdr.Link.Flink == children) goto done;
        parameter = CONTAINING_RECORD(uuid->hdr.Link.Flink, SDP_NODE, hdr.Link);
        if (parameter->hdr.SpecificType == SDP_ST_UINT16 && parameter->u.uint16 == psm) l2cap = TRUE;
    }
done:
    parser->SdpFreeTree(tree);
    return l2cap && protocol;
}
static NTSTATUS VerifySdpPsm(DEVICE_CONTEXT* ctx, USHORT psm)
{
    BTH_SDP_CONNECT connect = {0};
    BTH_SDP_DISCONNECT disconnect = {0};
    BTH_SDP_SERVICE_ATTRIBUTE_SEARCH_REQUEST search = {0};
    BTH_SDP_STREAM_RESPONSE sizeResponse = {0};
    PBTH_SDP_STREAM_RESPONSE response = NULL;
    BTHDDI_SDP_PARSE_INTERFACE parser;
    NTSTATUS status, disconnectStatus;
    ULONG allocation, elementSize = 0;
    PUCHAR element = NULL, previous = NULL;
    BOOLEAN found = FALSE;
    RtlZeroMemory(&parser, sizeof(parser));
    connect.bthAddress = ctx->Channels[0].Info.RemoteAddress;
    connect.requestTimeout = SDP_REQUEST_TO_MIN;
    status = SendSdpIoctl(ctx, IOCTL_BTH_SDP_CONNECT, &connect, sizeof(connect), &connect, sizeof(connect));
    if (!NT_SUCCESS(status)) return status;
    search.hConnection = connect.hConnection;
    search.uuids[0].uuidType = SDP_ST_UUID16;
    search.uuids[0].u.uuid16 = psm == LHDC_AVCTP_PSM ? 0x110c : 0x110b;
    search.range[0].minAttribute = 0x0004;
    search.range[0].maxAttribute = 0x0004;
    status = SendSdpIoctl(ctx, IOCTL_BTH_SDP_SERVICE_ATTRIBUTE_SEARCH,
        &search, sizeof(search), &sizeResponse, sizeof(sizeResponse));
    if (!NT_SUCCESS(status)) goto done;
    if (sizeResponse.requiredSize == 0 || sizeResponse.requiredSize > 65535) { status = STATUS_DEVICE_DATA_ERROR; goto done; }
    allocation = FIELD_OFFSET(BTH_SDP_STREAM_RESPONSE, response) + sizeResponse.requiredSize;
    response = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocation, 'cdhL');
    if (response == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    status = SendSdpIoctl(ctx, IOCTL_BTH_SDP_SERVICE_ATTRIBUTE_SEARCH,
        &search, sizeof(search), response, allocation);
    if (!NT_SUCCESS(status)) goto done;
    if (response->responseSize != response->requiredSize || response->responseSize > sizeResponse.requiredSize) { status = STATUS_DEVICE_DATA_ERROR; goto done; }
    status = WdfFdoQueryForInterface(ctx->Device, &GUID_BTHDDI_SDP_PARSE_INTERFACE,
        (PINTERFACE)&parser, sizeof(parser), BTHDDI_SDP_PARSE_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status)) goto done;
    do {
        parser.SdpGetNextElement(response->response, response->responseSize, previous, &element, &elementSize);
        if (elementSize == 0) break;
        if (RecordHasProtocol(&parser, element, elementSize, psm)) { found = TRUE; break; }
        previous = element;
    } while (element != NULL);
    status = found ? STATUS_SUCCESS : STATUS_DEVICE_PROTOCOL_ERROR;
done:
    if (parser.Interface.InterfaceDereference != NULL) parser.Interface.InterfaceDereference(parser.Interface.Context);
    if (response != NULL) ExFreePoolWithTag(response, 'cdhL');
    disconnect.hConnection = connect.hConnection;
    disconnectStatus = SendSdpIoctl(ctx, IOCTL_BTH_SDP_DISCONNECT, &disconnect, sizeof(disconnect), NULL, 0);
    if (!NT_SUCCESS(disconnectStatus)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_TRANSPORT, "SDP disconnect failed=%!STATUS!", disconnectStatus);
        if (NT_SUCCESS(status)) status = disconnectStatus;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_TRANSPORT, "SDP PSM=0x%04x validation=%!STATUS!", psm, status);
    return status;
}

static NTSTATUS SendBrbSync(DEVICE_CONTEXT* ctx, PBRB brb, ULONG size)
{
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, brb, size);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(5));
    return WdfIoTargetSendInternalIoctlOthersSynchronously(ctx->Target, NULL,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, &descriptor, NULL, NULL, &options, NULL);
}
static NTSTATUS CloseChannel(CHANNEL_CONTEXT* slot, L2CAP_CHANNEL_HANDLE expected)
{
    DEVICE_CONTEXT* ctx = slot->DeviceContext;
    struct _BRB_L2CA_CLOSE_CHANNEL brb;
    L2CAP_CHANNEL_HANDLE channel;
    NTSTATUS status = STATUS_SUCCESS;
    WdfWaitLockAcquire(ctx->CloseLock, NULL);
    WdfSpinLockAcquire(ctx->Lock);
    channel = slot->Handle;
    if (expected != NULL && channel != expected) {
        WdfSpinLockRelease(ctx->Lock);
        WdfWaitLockRelease(ctx->CloseLock);
        return STATUS_SUCCESS;
    }
    slot->Info.Connected = FALSE;
    WdfSpinLockRelease(ctx->Lock);
    if (channel != NULL) {
        RtlZeroMemory(&brb, sizeof(brb));
        if (ctx->Profile.BthInitializeBrb == NULL) {
            WdfWaitLockRelease(ctx->CloseLock);
            return STATUS_DEVICE_NOT_READY;
        }
        ctx->Profile.BthInitializeBrb((PBRB)&brb, BRB_L2CA_CLOSE_CHANNEL);
        brb.BtAddress = slot->Info.RemoteAddress;
        brb.ChannelHandle = channel;
        status = SendBrbSync(ctx, (PBRB)&brb, sizeof(brb));
        WdfSpinLockAcquire(ctx->Lock);
        slot->Info.LastNtStatus = status;
        slot->Info.LastBtStatus = brb.Hdr.Status;
        // A failed close retains ownership; reopening must not leak a channel.
        if (NT_SUCCESS(status)) slot->Handle = NULL;
        WdfSpinLockRelease(ctx->Lock);
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_TRANSPORT,
            "Close channel=%u status=%!STATUS! bt=0x%08x", slot->Info.ChannelId, status, brb.Hdr.Status);
    }
    WdfWaitLockRelease(ctx->CloseLock);
    return status;
}
static NTSTATUS CloseAllChannels(DEVICE_CONTEXT* ctx)
{
    NTSTATUS avrcp = CloseChannel(&ctx->Channels[LHDC_CHANNEL_AVRCP - 1], NULL);
    NTSTATUS media = CloseChannel(&ctx->Channels[LHDC_CHANNEL_MEDIA - 1], NULL);
    NTSTATUS signal = CloseChannel(&ctx->Channels[LHDC_CHANNEL_SIGNAL - 1], NULL);
    if (!NT_SUCCESS(avrcp)) return avrcp;
    return NT_SUCCESS(media) ? signal : media;
}
static VOID PurgeQueues(DEVICE_CONTEXT* ctx)
{
    ULONG i;
    WdfIoQueuePurgeSynchronously(ctx->Queue);
    for (i = 0; i < LHDC_CHANNEL_COUNT; ++i) WdfIoQueuePurgeSynchronously(ctx->Channels[i].Queue);
}
static VOID StartQueues(DEVICE_CONTEXT* ctx)
{
    ULONG i;
    for (i = 0; i < LHDC_CHANNEL_COUNT; ++i) WdfIoQueueStart(ctx->Channels[i].Queue);
    WdfIoQueueStart(ctx->Queue);
}
static VOID IndicationCallback(PVOID context, INDICATION_CODE indication, PINDICATION_PARAMETERS parameters)
{
    CHANNEL_CONTEXT* slot = context;
    DEVICE_CONTEXT* ctx = slot->DeviceContext;
    switch (indication) {
    case IndicationAddReference: WdfObjectReference(ctx->Device); break;
    case IndicationReleaseReference: WdfObjectDereference(ctx->Device); break;
    case IndicationRemoteDisconnect:
        WdfSpinLockAcquire(ctx->Lock);
        if (slot->Handle != parameters->ConnectionHandle && !(slot->Handle == NULL && slot->Opening)) {
            WdfSpinLockRelease(ctx->Lock);
            break;
        }
        slot->DisconnectHandle = parameters->ConnectionHandle;
        slot->Info.Connected = FALSE;
        slot->Info.LastNtStatus = STATUS_CONNECTION_DISCONNECTED;
        WdfSpinLockRelease(ctx->Lock);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_TRANSPORT,
            "Remote disconnect channel=%u reason=%u", slot->Info.ChannelId, parameters->Parameters.Disconnect.Reason);
        WdfWorkItemEnqueue(ctx->DisconnectWork);
        break;
    default: break;
    }
}
VOID DisconnectWorker(WDFWORKITEM work)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(WdfWorkItemGetParentObject(work));
    L2CAP_CHANNEL_HANDLE expected;
    ULONG i;
    for (i = LHDC_CHANNEL_COUNT; i > 0; --i) {
        CHANNEL_CONTEXT* slot = &ctx->Channels[i - 1];
        WdfSpinLockAcquire(ctx->Lock);
        expected = slot->DisconnectHandle;
        WdfSpinLockRelease(ctx->Lock);
        // A work item queued for an old connection must never close a new one.
        if (expected != NULL) CloseChannel(slot, expected);
    }
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    WPP_INIT_TRACING(driver, registry);
    WDF_DRIVER_CONFIG_INIT(&config, DeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = DriverCleanup;
    status = WdfDriverCreate(driver, registry, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) WPP_CLEANUP(driver);
    return status;
}
VOID DriverCleanup(WDFOBJECT driver)
{
    WPP_CLEANUP(WdfDriverWdmGetDriverObject(driver));
}
NTSTATUS DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes, requestAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_WORKITEM_CONFIG work;
    DEVICE_CONTEXT* ctx;
    NTSTATUS status;
    ULONG i;
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetExclusive(init, TRUE);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = PrepareHardware;
    pnp.EvtDeviceReleaseHardware = ReleaseHardware;
    pnp.EvtDeviceD0Entry = D0Entry;
    pnp.EvtDeviceD0Exit = D0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, FileCreate, WDF_NO_EVENT_CALLBACK, FileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(init, &files, &attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(init, &requestAttributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    ctx = GetDeviceContext(device);
    ctx->Device = device;
    ctx->Target = WdfDeviceGetIoTarget(device);
    for (i = 0; i < LHDC_CHANNEL_COUNT; ++i) {
        ctx->Channels[i].DeviceContext = ctx;
        ctx->Channels[i].Info.Header.Size = sizeof(LHDC_INFO);
        ctx->Channels[i].Info.Header.Version = LHDC_ABI_VERSION;
        ctx->Channels[i].Info.ChannelId = i + 1;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->Lock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfWaitLockCreate(&attributes, &ctx->CloseLock);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG_INIT(&work, DisconnectWorker);
    work.AutomaticSerialization = FALSE;
    status = WdfWorkItemCreate(&work, &attributes, &ctx->DisconnectWork);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.EvtIoDeviceControl = RouteControl;
    queue.EvtIoStop = IoStop;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, &ctx->Queue);
    if (!NT_SUCCESS(status)) return status;
    for (i = 0; i < LHDC_CHANNEL_COUNT; ++i) {
        WDF_IO_QUEUE_CONFIG_INIT(&queue, WdfIoQueueDispatchSequential);
        queue.EvtIoDeviceControl = IoControl;
        queue.EvtIoStop = IoStop;
        status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, &ctx->Channels[i].Queue);
        if (!NT_SUCCESS(status)) return status;
    }
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_LHDC_TRANSPORT, NULL);
    if (!NT_SUCCESS(status)) return status;
    return LhdcCreateAudioChild(device);
}
NTSTATUS PrepareHardware(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    BTH_DEVICE_INFO peer;
    struct _BRB_GET_LOCAL_BD_ADDR local;
    WDF_MEMORY_DESCRIPTOR output;
    NTSTATUS status;
    ULONG i;
    UNREFERENCED_PARAMETER(raw); UNREFERENCED_PARAMETER(translated);
    RtlZeroMemory(&ctx->Profile, sizeof(ctx->Profile));
    status = WdfFdoQueryForInterface(device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
        (PINTERFACE)&ctx->Profile, sizeof(ctx->Profile), BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(&peer, sizeof(peer));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &peer, sizeof(peer));
    status = WdfIoTargetSendInternalIoctlSynchronously(ctx->Target, NULL,
        IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &output, NULL, NULL);
    if (!NT_SUCCESS(status)) goto fail;
    RtlZeroMemory(&local, sizeof(local));
    ctx->Profile.BthInitializeBrb((PBRB)&local, BRB_HCI_GET_LOCAL_BD_ADDR);
    status = SendBrbSync(ctx, (PBRB)&local, sizeof(local));
    if (!NT_SUCCESS(status)) goto fail;
    for (i = 0; i < LHDC_CHANNEL_COUNT; ++i) {
        ctx->Channels[i].Info.RemoteAddress = peer.address;
        ctx->Channels[i].Info.LocalAddress = local.BtAddress;
    }
    WdfSpinLockAcquire(ctx->Lock);
    ctx->Ready = TRUE;
    ctx->SessionClosing = FALSE;
    WdfSpinLockRelease(ctx->Lock);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_TRANSPORT, "Prepared remote=%I64x", peer.address);
    return STATUS_SUCCESS;
fail:
    ctx->Profile.Interface.InterfaceDereference(ctx->Profile.Interface.Context);
    RtlZeroMemory(&ctx->Profile, sizeof(ctx->Profile));
    return status;
}
NTSTATUS ReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    NTSTATUS status;
    ULONG i;
    UNREFERENCED_PARAMETER(translated);
    WdfSpinLockAcquire(ctx->Lock);
    ctx->Ready = FALSE;
    ctx->SessionClosing = TRUE;
    WdfSpinLockRelease(ctx->Lock);
    PurgeQueues(ctx);
    status = CloseAllChannels(ctx);
    WdfWorkItemFlush(ctx->DisconnectWork);
    WdfWaitLockAcquire(ctx->CloseLock, NULL);
    if (ctx->Profile.Interface.InterfaceDereference != NULL) {
        ctx->Profile.Interface.InterfaceDereference(ctx->Profile.Interface.Context);
        RtlZeroMemory(&ctx->Profile, sizeof(ctx->Profile));
    }
    // Lower hardware removal invalidates any remaining channel handle.
    WdfSpinLockAcquire(ctx->Lock);
    for (i = 0; i < LHDC_CHANNEL_COUNT; ++i) {
        ctx->Channels[i].Handle = NULL;
        ctx->Channels[i].DisconnectHandle = NULL;
    }
    WdfSpinLockRelease(ctx->Lock);
    WdfWaitLockRelease(ctx->CloseLock);
    return status;
}
NTSTATUS D0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    UNREFERENCED_PARAMETER(previous);
    WdfSpinLockAcquire(ctx->Lock);
    ctx->Ready = TRUE;
    ctx->SessionClosing = FALSE;
    WdfSpinLockRelease(ctx->Lock);
    StartQueues(ctx);
    return STATUS_SUCCESS;
}
NTSTATUS D0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(target);
    WdfSpinLockAcquire(ctx->Lock);
    ctx->Ready = FALSE;
    ctx->SessionClosing = TRUE;
    WdfSpinLockRelease(ctx->Lock);
    status = CloseAllChannels(ctx);
    WdfWorkItemFlush(ctx->DisconnectWork);
    return status;
}
VOID FileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(file);
    WdfSpinLockAcquire(ctx->Lock);
    status = ctx->Ready ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) ctx->SessionClosing = FALSE;
    WdfSpinLockRelease(ctx->Lock);
    WdfRequestComplete(request, status);
}
VOID FileCleanup(WDFFILEOBJECT file)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(WdfFileObjectGetDevice(file));
    BOOLEAN ready;
    WdfSpinLockAcquire(ctx->Lock);
    ctx->SessionClosing = TRUE;
    WdfSpinLockRelease(ctx->Lock);
    PurgeQueues(ctx);
    CloseAllChannels(ctx);
    WdfWorkItemFlush(ctx->DisconnectWork);
    WdfSpinLockAcquire(ctx->Lock);
    ready = ctx->Ready;
    WdfSpinLockRelease(ctx->Lock);
    if (ready) StartQueues(ctx);
}
VOID BrbComplete(WDFREQUEST request, WDFIOTARGET target,
    PWDF_REQUEST_COMPLETION_PARAMS params, WDFCONTEXT context)
{
    CHANNEL_CONTEXT* slot = context;
    DEVICE_CONTEXT* ctx = slot->DeviceContext;
    REQUEST_CONTEXT* req = GetRequestContext(request);
    NTSTATUS status = params->IoStatus.Status;
    ULONG_PTR information = 0;
    BOOLEAN remoteGone = FALSE;
    UNREFERENCED_PARAMETER(target);
    WdfSpinLockAcquire(ctx->Lock);
    --slot->Info.PendingRequests;
    slot->Info.LastNtStatus = status;
    slot->Info.LastBtStatus = req->Brb.BrbHeader.Status;
    if (status == STATUS_CANCELLED) ++slot->Info.CancelledRequests;
    if (req->Ioctl == IOCTL_LHDC_OPEN) slot->Opening = FALSE;
    if (NT_SUCCESS(status)) {
        if (req->Ioctl == IOCTL_LHDC_OPEN) {
            struct _BRB_L2CA_OPEN_CHANNEL* open = &req->Brb.BrbL2caOpenChannel;
            slot->Handle = open->ChannelHandle;
            slot->Info.InMtu = open->InResults.Params.Mtu;
            slot->Info.OutMtu = open->OutResults.Params.Mtu;
            remoteGone = slot->DisconnectHandle == open->ChannelHandle;
            slot->Info.Connected = !remoteGone;
            if (remoteGone) status = STATUS_CONNECTION_DISCONNECTED;
        } else if (req->Ioctl == IOCTL_LHDC_SEND) {
            slot->Info.CompletedBytes += req->Brb.BrbL2caAclTransfer.BufferSize;
        } else if (req->Ioctl == IOCTL_LHDC_RECEIVE) {
            LHDC_SDU_HEADER* output = req->Output;
            ULONG bytes = req->Brb.BrbL2caAclTransfer.BufferSize;
            if (bytes > req->Capacity) status = STATUS_DATA_ERROR;
            else {
                output->Header.Size = sizeof(*output) + bytes;
                output->Header.Version = LHDC_ABI_VERSION;
                output->ChannelId = slot->Info.ChannelId;
                output->PayloadSize = bytes;
                information = output->Header.Size;
                slot->Info.ReceivedBytes += bytes;
            }
        }
    }
    slot->Info.LastNtStatus = status;
    WdfSpinLockRelease(ctx->Lock);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_TRANSPORT,
        "BRB channel=%u ioctl=0x%08x status=%!STATUS! bt=0x%08x", slot->Info.ChannelId, req->Ioctl, status, req->Brb.BrbHeader.Status);
    if (remoteGone) WdfWorkItemEnqueue(ctx->DisconnectWork);
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? information : 0);
}
VOID RouteControl(WDFQUEUE queue, WDFREQUEST request, size_t outLength, size_t inLength, ULONG code)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    REQUEST_CONTEXT* req = GetRequestContext(request);
    LHDC_HEADER* header;
    ULONG channelId;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(outLength);
    status = WdfRequestRetrieveInputBuffer(request, sizeof(*header), (PVOID*)&header, NULL);
    if (!NT_SUCCESS(status)) goto fail;
    if (header->Version != LHDC_ABI_VERSION || header->Size != inLength) { status = STATUS_INVALID_PARAMETER; goto fail; }
    if (code == IOCTL_LHDC_OPEN) {
        LHDC_OPEN_INPUT* input = (LHDC_OPEN_INPUT*)header;
        if (inLength != sizeof(*input) || input->Reserved != 0) { status = STATUS_INVALID_PARAMETER; goto fail; }
        channelId = input->ChannelId;
    } else if (code == IOCTL_LHDC_QUERY_INFO || code == IOCTL_LHDC_CLOSE) {
        LHDC_CHANNEL_INPUT* input = (LHDC_CHANNEL_INPUT*)header;
        if (inLength != sizeof(*input) || input->Reserved != 0) { status = STATUS_INVALID_PARAMETER; goto fail; }
        channelId = input->ChannelId;
    } else if (code == IOCTL_LHDC_SEND || code == IOCTL_LHDC_RECEIVE) {
        if (inLength < sizeof(LHDC_SDU_HEADER)) { status = STATUS_INVALID_PARAMETER; goto fail; }
        channelId = ((LHDC_SDU_HEADER*)header)->ChannelId;
    } else { status = STATUS_INVALID_DEVICE_REQUEST; goto fail; }
    if (channelId < LHDC_CHANNEL_SIGNAL || channelId > LHDC_CHANNEL_COUNT) { status = STATUS_INVALID_PARAMETER; goto fail; }
    RtlZeroMemory(req, sizeof(*req));
    req->Channel = &ctx->Channels[channelId - 1];
    req->Ioctl = code;
    status = WdfRequestForwardToIoQueue(request, req->Channel->Queue);
    if (NT_SUCCESS(status)) return;
fail:
    WdfRequestComplete(request, status);
}
VOID IoControl(WDFQUEUE queue, WDFREQUEST request, size_t outLength, size_t inLength, ULONG code)
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    REQUEST_CONTEXT* req = GetRequestContext(request);
    CHANNEL_CONTEXT* slot = req->Channel;
    LHDC_HEADER* header;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFMEMORY memory;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;
    ULONG brbSize = 0;
    BOOLEAN available;
    WdfSpinLockAcquire(ctx->Lock);
    available = ctx->Ready && !ctx->SessionClosing;
    WdfSpinLockRelease(ctx->Lock);
    if (!available) { status = STATUS_DEVICE_NOT_READY; goto fail; }
    status = WdfRequestRetrieveInputBuffer(request, sizeof(*header), (PVOID*)&header, NULL);
    if (!NT_SUCCESS(status)) goto fail;
    if (header->Version != LHDC_ABI_VERSION || header->Size != inLength) { status = STATUS_INVALID_PARAMETER; goto fail; }
    if (code == IOCTL_LHDC_QUERY_INFO) {
        LHDC_INFO* info;
        if (inLength != sizeof(LHDC_CHANNEL_INPUT)) { status = STATUS_INVALID_PARAMETER; goto fail; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*info), (PVOID*)&info, NULL);
        if (!NT_SUCCESS(status)) goto fail;
        WdfSpinLockAcquire(ctx->Lock); *info = slot->Info; WdfSpinLockRelease(ctx->Lock);
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*info));
        return;
    }
    if (code == IOCTL_LHDC_CLOSE) {
        if (inLength != sizeof(LHDC_CHANNEL_INPUT)) { status = STATUS_INVALID_PARAMETER; goto fail; }
        WdfSpinLockAcquire(ctx->Lock);
        available = slot->Info.ChannelId != LHDC_CHANNEL_SIGNAL ||
            (ctx->Channels[LHDC_CHANNEL_MEDIA - 1].Handle == NULL && !ctx->Channels[LHDC_CHANNEL_MEDIA - 1].Opening);
        WdfSpinLockRelease(ctx->Lock);
        if (!available) { status = STATUS_DEVICE_BUSY; goto fail; }
        status = CloseChannel(slot, NULL);
        WdfRequestComplete(request, status);
        return;
    }
    if (code == IOCTL_LHDC_OPEN) {
        LHDC_OPEN_INPUT* input = (LHDC_OPEN_INPUT*)header;
        struct _BRB_L2CA_OPEN_CHANNEL* open = &req->Brb.BrbL2caOpenChannel;
        if (inLength != sizeof(*input) || input->ExpectedRemoteAddress != slot->Info.RemoteAddress) { status = STATUS_INVALID_PARAMETER; goto fail; }
        WdfSpinLockAcquire(ctx->Lock);
        available = slot->Handle == NULL &&
            (slot->Info.ChannelId != LHDC_CHANNEL_MEDIA || ctx->Channels[LHDC_CHANNEL_SIGNAL - 1].Info.Connected);
        WdfSpinLockRelease(ctx->Lock);
        if (!available) { status = STATUS_DEVICE_BUSY; goto fail; }
        if (slot->Info.ChannelId != LHDC_CHANNEL_MEDIA) {
            status = VerifySdpPsm(ctx, slot->Info.ChannelId == LHDC_CHANNEL_AVRCP ? LHDC_AVCTP_PSM : LHDC_AVDTP_PSM);
            if (!NT_SUCCESS(status)) goto fail;
        }
        WdfSpinLockAcquire(ctx->Lock);
        available = ctx->Ready && !ctx->SessionClosing;
        if (available) {
            slot->DisconnectHandle = NULL;
            slot->Opening = TRUE;
        }
        WdfSpinLockRelease(ctx->Lock);
        if (!available) { status = STATUS_DEVICE_NOT_READY; goto fail; }
        ctx->Profile.BthInitializeBrb(&req->Brb, BRB_L2CA_OPEN_CHANNEL);
        open->BtAddress = slot->Info.RemoteAddress;
        open->Psm = slot->Info.ChannelId == LHDC_CHANNEL_AVRCP ? LHDC_AVCTP_PSM : LHDC_AVDTP_PSM;
        open->ChannelFlags = CF_ROLE_EITHER | CF_LINK_AUTHENTICATED;
        open->ConfigOut.Flags = CFG_MTU;
        open->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
        open->ConfigOut.Mtu.Preferred = L2CAP_DEFAULT_MTU;
        open->ConfigOut.Mtu.Max = LHDC_MAX_SDU;
        open->ConfigIn.Flags = CFG_MTU;
        open->ConfigIn.Mtu = open->ConfigOut.Mtu;
        open->CallbackFlags = CALLBACK_DISCONNECT;
        open->Callback = IndicationCallback;
        open->CallbackContext = slot;
        open->ReferenceObject = WdfDeviceWdmGetDeviceObject(ctx->Device);
        open->IncomingQueueDepth = 10;
        brbSize = sizeof(*open);
    } else if (code == IOCTL_LHDC_SEND || code == IOCTL_LHDC_RECEIVE) {
        LHDC_SDU_HEADER* input = (LHDC_SDU_HEADER*)header;
        struct _BRB_L2CA_ACL_TRANSFER* transfer = &req->Brb.BrbL2caAclTransfer;
        L2CAP_CHANNEL_HANDLE channel;
        WdfSpinLockAcquire(ctx->Lock);
        channel = slot->Info.Connected ? slot->Handle : NULL;
        WdfSpinLockRelease(ctx->Lock);
        if (channel == NULL) { status = STATUS_CONNECTION_DISCONNECTED; goto fail; }
        if (inLength < sizeof(*input) || input->ChannelId != slot->Info.ChannelId) { status = STATUS_INVALID_PARAMETER; goto fail; }
        ctx->Profile.BthInitializeBrb(&req->Brb, BRB_L2CA_ACL_TRANSFER);
        transfer->BtAddress = slot->Info.RemoteAddress;
        transfer->ChannelHandle = channel;
        if (code == IOCTL_LHDC_SEND) {
            if (input->PayloadSize == 0 || input->PayloadSize > slot->Info.OutMtu || input->PayloadSize > LHDC_MAX_SDU || input->PayloadSize != inLength - sizeof(*input)) { status = STATUS_INVALID_BUFFER_SIZE; goto fail; }
            transfer->TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
            transfer->Buffer = (PUCHAR)input + sizeof(*input);
            transfer->BufferSize = input->PayloadSize;
            req->TxBytes = input->PayloadSize;
        } else {
            if (inLength != sizeof(*input) || input->PayloadSize != 0 || outLength < sizeof(*input) + slot->Info.InMtu) { status = STATUS_INVALID_BUFFER_SIZE; goto fail; }
            status = WdfRequestRetrieveOutputBuffer(request, outLength, &req->Output, NULL);
            if (!NT_SUCCESS(status)) goto fail;
            req->Capacity = slot->Info.InMtu;
            // Use the WDF request timer for reads. On the target BthPort build,
            // the BRB timeout field completed immediately instead of waiting.
            transfer->TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
            transfer->Buffer = (PUCHAR)req->Output + sizeof(*input);
            transfer->BufferSize = req->Capacity;
        }
        brbSize = sizeof(*transfer);
    } else { status = STATUS_INVALID_DEVICE_REQUEST; goto fail; }
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = request;
    status = WdfMemoryCreatePreallocated(&attributes, &req->Brb, brbSize, &memory);
    if (!NT_SUCCESS(status)) goto fail;
    status = WdfIoTargetFormatRequestForInternalIoctlOthers(ctx->Target, request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, memory, NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) goto fail;
    WdfRequestSetCompletionRoutine(request, BrbComplete, slot);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,
        code == IOCTL_LHDC_RECEIVE ? WDF_REL_TIMEOUT_IN_SEC(3) : WDF_REL_TIMEOUT_IN_SEC(8));
    WdfSpinLockAcquire(ctx->Lock);
    ++slot->Info.PendingRequests;
    slot->Info.SubmittedBytes += req->TxBytes;
    WdfSpinLockRelease(ctx->Lock);
    if (WdfRequestSend(request, ctx->Target, &options)) return;
    status = WdfRequestGetStatus(request);
    WdfSpinLockAcquire(ctx->Lock);
    --slot->Info.PendingRequests;
    slot->Info.SubmittedBytes -= req->TxBytes;
    WdfSpinLockRelease(ctx->Lock);
fail:
    WdfSpinLockAcquire(ctx->Lock);
    slot->Info.LastNtStatus = status;
    if (code == IOCTL_LHDC_OPEN) slot->Opening = FALSE;
    WdfSpinLockRelease(ctx->Lock);
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_TRANSPORT, "Channel=%u IOCTL=0x%08x failed=%!STATUS!", slot->Info.ChannelId, code, status);
    WdfRequestComplete(request, status);
}
VOID IoStop(WDFQUEUE queue, WDFREQUEST request, ULONG flags)
{
    UNREFERENCED_PARAMETER(queue); UNREFERENCED_PARAMETER(flags);
    // The original user request is the BRB request; cancellation propagates.
    WdfRequestCancelSentRequest(request);
}
