// Copyright (c) Microsoft Corporation. All Rights Reserved.
// Derived from Simple Audio Sample, licensed under MS-PL.
#pragma once
#include "connection.h"
#include "format_config.h"
static KSDATARANGE SpeakerTopoPinDataRangesBridge[] = {
    {sizeof(KSDATARANGE),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
     STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
};
static PKSDATARANGE SpeakerTopoPinDataRangePointersBridge[] = {&SpeakerTopoPinDataRangesBridge[0]};
static PCPIN_DESCRIPTOR SpeakerTopoMiniportPins[] = {
    {0,0,0,nullptr,{0,nullptr,0,nullptr,SIZEOF_ARRAY(SpeakerTopoPinDataRangePointersBridge),
     SpeakerTopoPinDataRangePointersBridge,KSPIN_DATAFLOW_IN,KSPIN_COMMUNICATION_NONE,&KSCATEGORY_AUDIO,nullptr,0}},
    {0,0,0,nullptr,{0,nullptr,0,nullptr,SIZEOF_ARRAY(SpeakerTopoPinDataRangePointersBridge),
     SpeakerTopoPinDataRangePointersBridge,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_NONE,&KSNODETYPE_HEADPHONES,nullptr,0}}
};
// Software mode leaves gain to Windows. Absolute mode exposes one master
// control and lets acknowledged AVRCP commands apply the gain at the earbud.
static PCCONNECTION_DESCRIPTOR SpeakerTopoMiniportConnections[] = {
    {PCFILTER_NODE,KSPIN_TOPO_WAVEOUT_SOURCE,PCFILTER_NODE,KSPIN_TOPO_LINEOUT_DEST}
};
static PCPROPERTY_ITEM SpeakerVolumeProperties[] = {
    {&KSPROPSETID_Audio,KSPROPERTY_AUDIO_VOLUMELEVEL,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcVolume}
};
static PCPROPERTY_ITEM SpeakerMuteProperties[] = {
    {&KSPROPSETID_Audio,KSPROPERTY_AUDIO_MUTE,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcVolume}
};
DEFINE_PCAUTOMATION_TABLE_PROP(SpeakerVolumeAutomation,SpeakerVolumeProperties);
DEFINE_PCAUTOMATION_TABLE_PROP(SpeakerMuteAutomation,SpeakerMuteProperties);
static PCNODE_DESCRIPTOR SpeakerVolumeNodes[] = {
    {0,&SpeakerVolumeAutomation,&KSNODETYPE_VOLUME,&KSAUDFNAME_MASTER_VOLUME},
    {0,&SpeakerMuteAutomation,&KSNODETYPE_MUTE,&KSAUDFNAME_MASTER_MUTE}
};
static PCCONNECTION_DESCRIPTOR SpeakerVolumeConnections[] = {
    {PCFILTER_NODE,KSPIN_TOPO_WAVEOUT_SOURCE,0,1},
    {0,0,1,1},
    {1,0,PCFILTER_NODE,KSPIN_TOPO_LINEOUT_DEST}
};
static PCPROPERTY_ITEM PropertiesSpeakerTopoFilter[] = {
    {&KSPROPSETID_LHDC_CONNECTION,LHDC_CONNECTION_STATUS,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcConnection},
    {&KSPROPSETID_Jack,KSPROPERTY_JACK_CONTAINERID,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcJack},
    {&KSPROPSETID_Jack,KSPROPERTY_JACK_DESCRIPTION,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcJack},
    {&KSPROPSETID_Jack,KSPROPERTY_JACK_DESCRIPTION2,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcJack},
    {&KSPROPSETID_Jack,KSPROPERTY_JACK_DESCRIPTION3,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcJackConfig}
};
static PCEVENT_ITEM SpeakerJackEvents[] = {
    {&KSEVENTSETID_PinCapsChange,KSEVENT_PINCAPS_JACKINFOCHANGE,
     PCEVENT_ITEM_FLAG_ENABLE|PCEVENT_ITEM_FLAG_BASICSUPPORT,EventHandler_LhdcJack}
};
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationSpeakerTopoFilter,PropertiesSpeakerTopoFilter,SpeakerJackEvents);
static PCFILTER_DESCRIPTOR SpeakerTopoMiniportFilterDescriptor = {
    0,&AutomationSpeakerTopoFilter,sizeof(PCPIN_DESCRIPTOR),SIZEOF_ARRAY(SpeakerTopoMiniportPins),
    SpeakerTopoMiniportPins,sizeof(PCNODE_DESCRIPTOR),0,nullptr,
    SIZEOF_ARRAY(SpeakerTopoMiniportConnections),SpeakerTopoMiniportConnections,0,nullptr
};
inline void LhdcConfigureVolumeTopology() {
    if (LhdcHardwareVolume()) {
        SpeakerTopoMiniportFilterDescriptor.NodeCount=SIZEOF_ARRAY(SpeakerVolumeNodes);
        SpeakerTopoMiniportFilterDescriptor.Nodes=SpeakerVolumeNodes;
        SpeakerTopoMiniportFilterDescriptor.ConnectionCount=SIZEOF_ARRAY(SpeakerVolumeConnections);
        SpeakerTopoMiniportFilterDescriptor.Connections=SpeakerVolumeConnections;
    }
}
