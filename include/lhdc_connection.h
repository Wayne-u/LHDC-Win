#pragma once
/* Private topology property. The service publishes the Bluetooth ACL state. */
#define LHDC_CONNECTION_ABI_VERSION 1u
static const GUID KSPROPSETID_LHDC_CONNECTION =
    {0xe269c6b7,0x3c5f,0x47af,{0x97,0xe1,0x3e,0x1b,0xb5,0x56,0x8b,0x75}};
enum { LHDC_CONNECTION_STATUS=0 };
typedef struct LHDC_CONNECTION_STATE {
    ULONG Size;
    ULONG Version;
    ULONG Connected;
} LHDC_CONNECTION_STATE;
