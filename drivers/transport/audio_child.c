#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <devpkey.h>
#include <lhdc_pcm.h>
#include "audio_child.h"
/* One audio function belongs to this specific Bluetooth Audio Sink devnode. */
NTSTATUS LhdcCreateAudioChild(WDFCHILDLIST list,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER identification,PWDFDEVICE_INIT init)
{
    WDFDEVICE parent=WdfChildListGetDevice(list);
    WDFDEVICE child;
    UNICODE_STRING id,instance,description,location,container;
    WDF_DEVICE_PNP_CAPABILITIES capabilities;
    WDF_DEVICE_PROPERTY_DATA property;
    DEVPROPTYPE propertyType;
    GUID containerId;
    ULONG required;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(identification);
    RtlInitUnicodeString(&id,LHDC_AUDIO_HARDWARE_ID);
    RtlInitUnicodeString(&instance,L"Audio0");
    RtlInitUnicodeString(&description,L"Enco X4 LHDC Audio");
    RtlInitUnicodeString(&location,L"Bluetooth A2DP");
    status=WdfPdoInitAssignDeviceID(init,&id);
    if (!NT_SUCCESS(status)) goto fail;
    status=WdfPdoInitAddHardwareID(init,&id);
    if (!NT_SUCCESS(status)) goto fail;
    status=WdfPdoInitAssignInstanceID(init,&instance);
    if (!NT_SUCCESS(status)) goto fail;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property,&DEVPKEY_Device_ContainerId);
    status=WdfDeviceQueryPropertyEx(parent,&property,sizeof(containerId),&containerId,&required,&propertyType);
    if (!NT_SUCCESS(status)) goto fail;
    if (propertyType!=DEVPROP_TYPE_GUID || required!=sizeof(containerId)) { status=STATUS_DEVICE_DATA_ERROR; goto fail; }
    status=RtlStringFromGUID(&containerId,&container);
    if (!NT_SUCCESS(status)) goto fail;
    status=WdfPdoInitAssignContainerID(init,&container);
    RtlFreeUnicodeString(&container);
    if (!NT_SUCCESS(status)) goto fail;
    status=WdfPdoInitAddDeviceText(init,&description,&location,0x409);
    if (!NT_SUCCESS(status)) goto fail;
    WdfPdoInitSetDefaultLocale(init,0x409);
    status=WdfDeviceCreate(&init,WDF_NO_OBJECT_ATTRIBUTES,&child);
    if (!NT_SUCCESS(status)) goto fail;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&capabilities);
    capabilities.Removable=WdfFalse;
    capabilities.EjectSupported=WdfFalse;
    capabilities.UniqueID=WdfFalse;
    capabilities.Address=0;
    capabilities.UINumber=0;
    WdfDeviceSetPnpCapabilities(child,&capabilities);
    return STATUS_SUCCESS;
fail:
    return status;
}
NTSTATUS LhdcSetAudioPresent(WDFDEVICE parent,BOOLEAN present)
{
    LHDC_AUDIO_DESCRIPTION description;
    WDFCHILDLIST list=WdfFdoGetDefaultChildList(parent);
    RtlZeroMemory(&description,sizeof(description));
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&description.Header,sizeof(description));
    if (present) {
        NTSTATUS status=WdfChildListAddOrUpdateChildDescriptionAsPresent(list,&description.Header,NULL);
        return status==STATUS_OBJECT_NAME_EXISTS?STATUS_SUCCESS:status;
    }
    /* Scanning an empty bus also works before the first child has appeared. */
    WdfChildListBeginScan(list);
    WdfChildListEndScan(list);
    return STATUS_SUCCESS;
}
