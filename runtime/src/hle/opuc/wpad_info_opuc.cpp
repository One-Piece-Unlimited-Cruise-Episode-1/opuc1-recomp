// OPUC virtual Wii Remote: WPADGetInfoAsync always answers "full battery".
//
// WPADGetInfoAsync(chan, WPADInfo* info, WPADCallback cb) (802c3438) queues output report 0x15 (status request);
// the remote's status report later fills `info` and calls cb(chan, WPAD_ERR_NONE). Two callers:
//   ALPadWii::Read (803e973c)  cb = NULL; info = ALPadWii::aInfo[dwInfoBuf] (two 0x18-byte WPADInfo buffers). On a
//                              0 return it flips dwInfoBuf and waits 0x3C frames before asking again; info.dpd (+0)
//                              is compared with bDpdWanted to turn DPD on / off. The game's low-battery badge
//                              (icon_battery, vault entities/pad-input) is fed from these buffers.
//   HBM getInfoAsync (80496d70) the HOME Menu's battery bars, cb = 80496df4.
// The recompilation has no remote under the translated WPAD/Bluetooth stack (channel 0 is the KPAD-level virtual
// remote of wiimote_opuc.cpp), so the request never completed, the buffers stayed zeroed (battery 0) and the game
// showed the badge. A PC has no battery: channel 0 answers at once with a full remote, as WPADProbe_OPUC reports it:
//   dpd 0 (as the zeroed buffer read before, so ALPadWii's DPD on / off calls are unchanged), speaker 0,
//   attach 1 (Nunchuk), lowBat 0, nearempty 0, battery 4 (four bars, the highest level), led 1 (player 1),
//   protocol 0, firmware 0.
// Channels 1-3 keep the translated WPAD (no controller there). OPUC_TRACE_INPUT=1 logs the first answered request
// and every time the badge opens (AquaDesignerBatteryIcon::Open 8019e120, reached from BatteryWarningWii::Update
// 80252a44 after six 10-s checks in a row read battery <= 1). Test switch OPUC_WPAD_INFO_PASSTHROUGH=1: leave
// channel 0 to the translated WPAD too (the old behaviour, the badge comes back).
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>

namespace {
constexpr uint32_t kWpadGetInfoAsync = 0x802C3438u;
constexpr uint32_t kWpadInfoSize = 0x18u;
constexpr uint8_t kBatteryFull = 4;

bool GuestPtr(uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }
} // namespace

extern "C" void OpucWpad_GetInfoAsync(CpuContext* ctx) {
    const uint32_t chan = ctx->gpr[3], info = ctx->gpr[4], cb = ctx->gpr[5];
    static const bool passthrough = [] { const char* e = std::getenv("OPUC_WPAD_INFO_PASSTHROUGH"); return e && e[0] == '1'; }();
    if (chan != 0 || !GuestPtr(info) || passthrough) {
        GuestFunctions::NextTranslation(kWpadGetInfoAsync)(ctx);
        return;
    }
    for (uint32_t o = 0; o < kWpadInfoSize; o += 4) Memory::Write32(info + o, 0);
    Memory::Write32(info + 0x00, 0);              // dpd
    Memory::Write32(info + 0x04, 0);              // speaker
    Memory::Write32(info + 0x08, 1);              // attach: the Nunchuk
    Memory::Write32(info + 0x0C, 0);              // lowBat
    Memory::Write32(info + 0x10, 0);              // nearempty
    Memory::Write8(info + 0x14, kBatteryFull);    // battery
    Memory::Write8(info + 0x15, 1);               // led: player 1
    static const bool trace = std::getenv("OPUC_TRACE_INPUT") != nullptr;
    static bool logged = false;
    if (trace && !logged) {
        logged = true;
        RT_LOG(RT_TAG_HLE) << "[opuc] WPADGetInfoAsync chan 0: full battery (" << int(kBatteryFull) << ") -> 0x"
                           << std::hex << info << " lr=0x" << ctx->lr << std::dec << std::endl;
    }
    if (cb) OpucGuest::Call(*ctx, cb, {chan, 0});   // WPAD_ERR_NONE, as the status report would deliver it
    ctx->gpr[3] = 0;                                // WPAD_ERR_NONE: request accepted
}
REGISTER_NATIVE_REPLACEMENT_AS(0x802C3438, OpucWpad_GetInfoAsync, "OpucWpad_GetInfoAsync");

// AquaDesignerBatteryIcon::Open (8019e120), called through WinDispInfo::OpenPart (8018ec5c): trace only.
extern "C" void OpucWpad_BatteryIconOpen(CpuContext* ctx) {
    static const bool trace = std::getenv("OPUC_TRACE_INPUT") != nullptr;
    if (trace) RT_LOG(RT_TAG_HLE) << "[opuc] low-battery badge opened (AquaDesignerBatteryIcon::Open, lr=0x" << std::hex
                                  << ctx->lr << std::dec << ")" << std::endl;
    GuestFunctions::NextTranslation(0x8019E120u)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8019E120, OpucWpad_BatteryIconOpen, "OpucWpad_BatteryIconOpen");
