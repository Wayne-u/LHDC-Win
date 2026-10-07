#pragma once
#include <windows.h>
#include "codec/profile.hpp"
namespace lhdc {
Profile service_profile();
void serve_audio(HANDLE stop,unsigned duration_seconds=0);
}
