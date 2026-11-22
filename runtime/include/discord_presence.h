#pragma once

#include <cstdint>
#include <string>

// Discord Rich Presence over the local Discord IPC socket/pipe. The runtime sets a basic presence at startup
// (Initialize). SetClient / SetActivity / Reset were driven by Dolphin's /dev/dolphin device, which OPUC never
// opens; they currently have no caller.
namespace DiscordPresence {

struct Activity {
    std::string details;
    std::string state;
    std::string largeImageKey;
    std::string largeImageText;
    std::string smallImageKey;
    std::string smallImageText;
    int64_t startTimestamp = 0;
    int64_t endTimestamp = 0;
    uint32_t partySize = 0;
    uint32_t partyMax = 0;
};

void Initialize(const std::string& basicClientId, const std::string& basicTitle);
void SetClient(const std::string& clientId);
void SetActivity(Activity activity);
void Reset();
void Shutdown();

} // namespace DiscordPresence
