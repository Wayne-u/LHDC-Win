#pragma once
#include <lhdc_connection.h>
NTSTATUS PropertyHandler_LhdcConnection(PPCPROPERTY_REQUEST request);
NTSTATUS PropertyHandler_LhdcJack(PPCPROPERTY_REQUEST request);
NTSTATUS EventHandler_LhdcJack(PPCEVENT_REQUEST request);
NTSTATUS LhdcLoadJackContainer(PDEVICE_OBJECT device,GUID* container);
