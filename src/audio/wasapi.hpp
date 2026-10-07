#pragma once
#include "windows_audio.hpp"
#include <filesystem>
namespace lhdc {
void probe_headphone_microphone(unsigned seconds);
void inspect_audio_devices(bool capture=false);
void inspect_playback_ready(const std::string& endpoint,unsigned rate,unsigned bits);
void render_wav(const std::string& endpoint,const std::filesystem::path& input,unsigned repeats=1);
}
