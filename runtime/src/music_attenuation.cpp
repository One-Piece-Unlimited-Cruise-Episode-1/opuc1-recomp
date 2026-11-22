// OPUC: upstream music ducking (Windows media session via C++/WinRT, nw4r SoundPlayer volume hooks at MKWii
// addresses) is not ported. LLVM-MinGW ships no C++/WinRT headers and OPUC's nw4r::snd sits at other addresses.
// The settings overlay still stores the volume sliders; nothing scales the game's mix yet (STATUS.md M8).
#include "music_attenuation.h"

#include <atomic>

namespace MusicAttenuation {
namespace {
std::atomic<bool> g_enabled{false};
std::atomic<float> g_music{1.0f}, g_sfx{1.0f}, g_ui{1.0f}, g_voices{1.0f};
} // namespace

void SetEnabled(bool enabled) noexcept { g_enabled.store(enabled); }
void SetMusicVolume(float volume) noexcept { g_music.store(volume); }
void SetSoundEffectsVolume(float volume) noexcept { g_sfx.store(volume); }
void SetUiVolume(float volume) noexcept { g_ui.store(volume); }
void SetVoicesVolume(float volume) noexcept { g_voices.store(volume); }
bool IsExternalMediaPlaying() noexcept { return false; }
bool IsMediaControlAvailable() noexcept { return false; }
bool IsMediaControlInitializationComplete() noexcept { return true; }
void TickGuest() noexcept {}
void SetSoundPlayerVolume(uint32_t, float) {}
} // namespace MusicAttenuation
