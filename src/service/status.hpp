#pragma once
namespace lhdc {
bool adaptive_bitrate_enabled();
void publish_active_bitrate(unsigned kbps);
void inspect_audio_service();
}
