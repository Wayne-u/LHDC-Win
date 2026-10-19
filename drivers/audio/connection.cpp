#include "definitions.h"
#include "endpoints.h"
#include "mintopo.h"
#include "connection.h"
#include <initguid.h>
#include <devpkey.h>
#pragma code_seg("PAGE")
NTSTATUS PropertyHandler_LhdcVolume(PPCPROPERTY_REQUEST request) {
    PAGED_CODE();
    return static_cast<CMiniportTopology*>(request->MajorTarget)->PropertyHandlerGeneric(request);
}
NTSTATUS LhdcLoadJackContainer(PDEVICE_OBJECT device,GUID* container) {
    PAGED_CODE();
    ULONG required=0;DEVPROPTYPE type=0;
    const auto status=IoGetDevicePropertyData(device,&DEVPKEY_Device_ContainerId,LOCALE_NEUTRAL,0,
        sizeof(*container),container,&required,&type);
    if (!NT_SUCCESS(status)) return status;
    return type==DEVPROP_TYPE_GUID && required==sizeof(*container)?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR;
}
NTSTATUS PropertyHandler_LhdcConnection(PPCPROPERTY_REQUEST request) {
    PAGED_CODE();
    if (request->Verb&KSPROPERTY_TYPE_BASICSUPPORT)
        return PropertyHandler_BasicSupport(request,KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_BASICSUPPORT,VT_ILLEGAL);
    const auto status=ValidatePropertyParams(request,sizeof(LHDC_CONNECTION_STATE),0);
    if (!NT_SUCCESS(status)) return status;
    auto* miniport=static_cast<CMiniportTopology*>(request->MajorTarget);
    auto* state=static_cast<LHDC_CONNECTION_STATE*>(request->Value);
    if (request->Verb&KSPROPERTY_TYPE_SET) {
        if (state->Size!=sizeof(*state) || state->Version!=LHDC_CONNECTION_ABI_VERSION || state->Connected>1)
            return STATUS_INVALID_PARAMETER;
        const LONG connected=static_cast<LONG>(state->Connected);
        if (InterlockedExchange(&miniport->m_LhdcConnected,connected)!=connected)
            miniport->GenerateEventList(const_cast<GUID*>(&KSEVENTSETID_PinCapsChange),
                KSEVENT_PINCAPS_JACKINFOCHANGE,TRUE,KSPIN_TOPO_LINEOUT_DEST,FALSE,0);
    } else if (request->Verb&KSPROPERTY_TYPE_GET) {
        *state={sizeof(*state),LHDC_CONNECTION_ABI_VERSION,
            static_cast<ULONG>(InterlockedCompareExchange(&miniport->m_LhdcConnected,0,0))};
    } else return STATUS_INVALID_DEVICE_REQUEST;
    request->ValueSize=sizeof(*state);
    return STATUS_SUCCESS;
}
NTSTATUS PropertyHandler_LhdcJack(PPCPROPERTY_REQUEST request) {
    PAGED_CODE();
    if (request->Verb&KSPROPERTY_TYPE_BASICSUPPORT)
        return PropertyHandler_BasicSupport(request,KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,VT_ILLEGAL);
    if (!(request->Verb&KSPROPERTY_TYPE_GET) || request->InstanceSize<sizeof(ULONG) || !request->Instance ||
        *static_cast<ULONG*>(request->Instance)!=KSPIN_TOPO_LINEOUT_DEST) return STATUS_INVALID_PARAMETER;
    const auto id=request->PropertyItem->Id;
    if (id==KSPROPERTY_JACK_CONTAINERID) {
        const auto status=ValidatePropertyParams(request,sizeof(GUID),sizeof(ULONG));
        if (!NT_SUCCESS(status)) return status;
        *static_cast<GUID*>(request->Value)=static_cast<CMiniportTopology*>(request->MajorTarget)->m_LhdcContainer;
        request->ValueSize=sizeof(GUID);
        return STATUS_SUCCESS;
    }
    if (id!=KSPROPERTY_JACK_DESCRIPTION && id!=KSPROPERTY_JACK_DESCRIPTION2) return STATUS_INVALID_DEVICE_REQUEST;
    const ULONG bytes=sizeof(KSMULTIPLE_ITEM)+(id==KSPROPERTY_JACK_DESCRIPTION?sizeof(KSJACK_DESCRIPTION):sizeof(KSJACK_DESCRIPTION2));
    const auto status=ValidatePropertyParams(request,bytes,sizeof(ULONG));
    if (!NT_SUCCESS(status)) return status;
    auto* items=static_cast<KSMULTIPLE_ITEM*>(request->Value);
    items->Size=bytes;items->Count=1;
    if (id==KSPROPERTY_JACK_DESCRIPTION) {
        auto* miniport=static_cast<CMiniportTopology*>(request->MajorTarget);
        const BOOL connected=InterlockedCompareExchange(&miniport->m_LhdcConnected,0,0)!=0;
        *reinterpret_cast<KSJACK_DESCRIPTION*>(items+1)=
            {KSAUDIO_SPEAKER_STEREO,JACKDESC_RGB(0xB3,0xC9,0x8C),
             eConnTypeUnknown,eGeoLocFront,eGenLocPrimaryBox,ePortConnIntegratedDevice,connected};
    } else {
        *reinterpret_cast<KSJACK_DESCRIPTION2*>(items+1)={0,JACKDESC2_PRESENCE_DETECT_CAPABILITY};
    }
    request->ValueSize=bytes;
    return STATUS_SUCCESS;
}
NTSTATUS EventHandler_LhdcJack(PPCEVENT_REQUEST request) {
    PAGED_CODE();
    if (!IsEqualGUIDAligned(*request->EventItem->Set,KSEVENTSETID_PinCapsChange) ||
        request->EventItem->Id!=KSEVENT_PINCAPS_JACKINFOCHANGE) return STATUS_INVALID_PARAMETER;
    switch (request->Verb) {
    case PCEVENT_VERB_SUPPORT:
    case PCEVENT_VERB_REMOVE: return STATUS_SUCCESS;
    case PCEVENT_VERB_ADD:
        if (!request->EventEntry) return STATUS_INVALID_PARAMETER;
        static_cast<CMiniportTopology*>(request->MajorTarget)->AddEventToEventList(request->EventEntry);
        return STATUS_SUCCESS;
    default: return STATUS_INVALID_PARAMETER;
    }
}
#pragma code_seg()
