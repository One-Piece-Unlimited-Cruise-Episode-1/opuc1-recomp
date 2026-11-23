// GameCube PAD library. OPUC reads its controls only through KPAD (the virtual Wii Remote in
// hle/opuc/wiimote_opuc.cpp), so GameCube pad input is not routed to the game: PADInit / PADRead /
// PADControlMotor are not replaced. The game does call PADReset(0xF0000000) once during boot (vault
// entities/pad-input); the translated SDK version would program the SI hardware, so PADReset and
// PADRecalibrate stay answered by aurora's PAD layer.
#include "hle_stubs.h"

#include <atomic>
#include <cstdint>

#include <dolphin/pad.h>

namespace {
std::atomic<bool> g_rumbleEnabled{true};
} // namespace

// Vibration switch shared with paired real Wii Remotes (wiimote/wiimote_host.cpp) and the settings overlay.
extern "C" bool PAD_HLE_RumbleEnabled()
{
    return g_rumbleEnabled.load(std::memory_order_relaxed);
}

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(80287B1C /*mkw:801AF0DC*/, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(80287C24 /*mkw:801AF1E4*/, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));
