// OPUC sound: the Wii Remote speaker gate, and a trace of every game sound start.
//
// Remote speaker gate (2026-11-28). Se::PlayId 801aa1e8 reads the sound's user flags from sound/snd_all.atb (row =
// id & 0xFFFF). 18 rows carry 0x08, "also on the Wii Remote speaker": 0xE7D-0xE7F, 0xFDB, 0xFDE, 0xFE0-0xFE2, 0xFE8,
// 0xFEA-0xFED (field object hits), 0x1561 ("Saving in progress" loop), 0x1562 ("Save complete"), 0x1573, 0x1579,
// 0x157A. For them PlayId calls Se::SpawnVoice 801aab64 with r8 = 1, and SpawnVoice returns -2 without starting
// anything, TV output included, while the "remote 0 speaker ready" byte 0x8068A5E0 is 0 (vault entities/boot-flow,
// Sound). Only SoundSe::OnRemoteSpeakerSetup 801ab6d8 sets it, after nw4r RemoteSpeaker reached state 4 (WPAD
// speaker ON, then PLAY). The recompilation has no Wii Remote, so the setup never completes and those sounds were
// silent (the pickaxe on rocks, the save). The wrapper on SoundSe::Update 801a930c (once a frame; it also clears the
// byte when WPADProbe reports no remote) sets the byte after each update. The sounds then start as on hardware: their
// 0x10 flag puts them on the TV's main line; the remote line's volume (SetRemoteOutVolume from the never-run setup)
// stays 0. OPUC_REMOTE_SPEAKER_READY=0 restores the old behaviour.
//
// OPUC_TRACE_SE=1: one log line per sound start, with the caller and SpawnVoice's result (the ring slot, or -2 for a
// refused start; a failed nw4r StartSound still returns the slot).
#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>

namespace {
constexpr uint32_t kRemoteSpeakerReady = 0x8068A5E0;  // r13 - 0x58C0, u8
}

extern "C" void OpucSe_SoundSeUpdate(CpuContext* ctx) {
    static const bool force = [] {
        const char* v = std::getenv("OPUC_REMOTE_SPEAKER_READY");
        return !(v && v[0] == '0');
    }();
    GuestFunctions::NextTranslation(0x801A930C)(ctx);
    if (force && Memory::Read8(kRemoteSpeakerReady) == 0) {
        Memory::Write8(kRemoteSpeakerReady, 1);
        static bool logged = false;
        if (!logged) {
            logged = true;
            RT_LOG(RT_TAG_HLE) << "[opuc] sound: Wii Remote speaker marked ready (the 0x08 sounds play on the TV)" << std::endl;
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x801A930C, OpucSe_SoundSeUpdate, "OpucSe_SoundSeUpdate");

extern "C" void OpucSe_SpawnVoice(CpuContext* ctx) {
    static const bool trace = std::getenv("OPUC_TRACE_SE") != nullptr;
    const uint32_t id = ctx->gpr[3], lr = ctx->lr, remote = ctx->gpr[8];
    GuestFunctions::NextTranslation(0x801AAB64)(ctx);
    if (trace)
        RT_LOGF(RT_TAG_HLE, "[opuc] se start id %08X (sound %u)%s caller %08X -> %08X\n", id, id & 0xFFFFu,
                remote ? " +remote" : "", lr, ctx->gpr[3]);
}
REGISTER_NATIVE_WRAPPER_AS(0x801AAB64, OpucSe_SpawnVoice, "OpucSe_SpawnVoice");
