// OPUC-only GX HLE: SDK GX paths OPUC uses that upstream (MKWii) never exercised.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "hle/gx/gx_internal.h"

#include <dolphin/gx/GXManage.h>

extern "C" void GX__FinishInterruptHandler_8016ed94();

namespace {
// GXSetDrawDoneCallback (80293368) stores the callback here (r13-0x51dc); GX worker batch 2026-11-22.
constexpr uint32_t kGxDrawDoneCallbackAddr = 0x8068ACC4u;
}

// GXSetDrawDone (802930e4): the SDK writes BP 0x45000002 and returns; the GPU's PE "finish" interrupt later runs
// __GXFinishInterruptHandler, which sets the draw-done flag and calls the draw-done callback. OPUC's frame loop
// depends on that callback: ALSystemGC::InitGraphicsAndHandlers (803eeb18) registers 803ee194, which swaps the XFB
// (VISetNextFrameBuffer / VISetBlack(0) / VIFlush) and sets the byte r13-0x6400 (0x80689AA0) that
// ALEngine::RunFrame (803eefb0) spins on with OSSleepTicks(1). No PE interrupt is emulated, so the HLE drains the GX
// work (aurora GXDrawDone) and runs what the interrupt would, synchronously.
extern "C" void GXSetDrawDone_OPUC(CpuContext* ctx) {
    GX_SYNC_INLINE();
    try { Memory::Write8(kGxDrawDoneFlagAddr, 0); } catch (...) {}
    GXDrawDone();
    GX__FinishInterruptHandler_8016ed94();
    uint32_t callback = 0;
    try { callback = Memory::Read32(kGxDrawDoneCallbackAddr); } catch (...) {}
    if (callback != 0) {
        InvokeIndirectCpu(callback, ctx);
    }
}
PPC_NATIVE_OVERRIDE_VOID(802930E4, GXSetDrawDone_OPUC, (CpuContext* ctx), (ctx));
