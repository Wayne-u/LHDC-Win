#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <devpkey.h>
#include <lhdc_pcm.h>
/* One audio function belongs to this specific Bluetooth Audio Sink devnode. */
NTSTATUS LhdcCreateAudioChild(WDFDEVICE parent)
{
    PWDFDEVICE_INIT init=WdfPdoInitAllocate(parent);
    WDFDEVICE child;
    UNICODE_STRING id,instance,description,location,container;
    WDF_DEVICE_PNP_CAPABILITIES capabilities;
    WDF_DEVICE_PROPERTY_DATA property;
    DEVPROPTYPE propertyType;
    GUID containerId;
    ULONG required;
    NTSTATUS status;
    if (init==NULL) return STATUS_INSUFFICIENT_RESOURCES;
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
    status=WdfFdoAddStaticChild(parent,child);
    if (!NT_SUCCESS(status)) WdfObjectDelete(child);
    return status;
fail:
    if (init!=NULL) WdfDeviceInitFree(init);
    return status;
}
