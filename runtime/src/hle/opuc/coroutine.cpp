// OPUC engine coroutines on host fibers.
//
// The Aqualead engine runs cooperative coroutines (ALScheduler::RunLogicTick 803c0f7c..803c1008,
// ALObject::ResumeThread 80382454) that share one guest stack region and copy their stack image in and out:
//   80342114  set the shared stack top (r13-0x4E14) = buf + size; ALSystem::Main passes a 0x4000-byte buffer
//   803dff34  create(co, entry, arg): builds a 0x1A0-byte first frame {r3=arg, r4, PC=entry, LR=0x80342090, ...}
//             and copies it into the coroutine's buffer (co+8 buffer, co+0xC capacity, co+0x10 size)
//   80342124  resume(co): copies the image to top - size, sets r13-0x4E10 = top - size, copies r13-0x4E04 (the
//             switch) into r13-0x4E0C and calls through r13-0x4E0C;
//             when the coroutine yields, it copies [r13-0x4E10, top) back into the coroutine
//   80342090  finish: set the done byte of the current coroutine *(r13-0x4E08) to 1 (create cleared it at
//             803e0100), then yield forever
//   803dfc94  THE SWITCH: save PC/LR/CR/XER/GPRs/FPRs/r2/FPSCR in a 0x1A0 frame on the current stack, swap r1
//             with r13-0x4E10, restore the other side's frame and `bctr` to its saved PC.
// Translated C++ cannot jump into the middle of a function, so only the switch is native: each coroutine is a host
// fiber with its own CpuContext. The game still copies the guest stack bytes exactly as on hardware, so guest
// stack memory above r1 is preserved; the register state lives in the fiber instead of the 0x1A0 frame. A yielding
// coroutine leaves a marker frame {token, ..., 'OPCO'} where the real switch would have left its frame, and that
// frame travels with the stack image; resume finds the fiber again from it. Vault: entities/main-dol-layout.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "fiber_manager.h"
#include "host_context.h"
#include "runtime_log.h"

#include <cstdlib>
#include <memory>
#include <vector>

namespace {

constexpr uint32_t kSwapFramePtrAddr = 0x8068B090u;  // r13-0x4E10: frame pointer of the switched-out side
constexpr uint32_t kFrameSize = 0x1A0u;
constexpr uint32_t kMarker = 0x4F50434Fu;            // 'OPCO' at frame+8 (where the real frame keeps the PC)
constexpr std::size_t kCoroutineHostStack = 1u << 20;

struct Coroutine {
    uint32_t token = 0;
    void* fiber = nullptr;
    void* resumer = nullptr;
    uint32_t entryPc = 0;
    uint32_t finishPc = 0;
    CpuContext ctx{};
};

std::vector<std::unique_ptr<Coroutine>> g_coroutines;  // index = token - 1
Coroutine* g_running = nullptr;                         // coroutine whose fiber runs now (nullptr: none)

void SwitchKeepingCpuContext(void* target) {
    CpuContext* const ambient = TryGetCpuContext();
    const uint32_t fpControl = MkwGetHostFpControl();
    HostContext::Switch(target);
    g_currentCpuContext = ambient;
    if (ambient) {
        MkwApplyHostNiMode(ambient->fpscr);
    } else {
        MkwRestoreHostMxcsr(fpControl);
    }
}

void CoroutineMain(void* arg) {
    auto* co = static_cast<Coroutine*>(arg);
    {
        CpuContextScope scope(&co->ctx);
        InvokeIndirectCpu(co->entryPc, &co->ctx);
        // The entry returned through the LR the first frame set: 80342090 marks the coroutine done and yields
        // forever, so this call never returns.
        InvokeIndirectCpu(co->finishPc, &co->ctx);
    }
    RT_LOG(RT_TAG_OS) << "[opuc] coroutine " << co->token << " finish stub returned" << std::endl;
    std::abort();
}

Coroutine* CreateFromFirstFrame(uint32_t frame, const CpuContext& caller) {
    auto co = std::make_unique<Coroutine>();
    co->token = static_cast<uint32_t>(g_coroutines.size() + 1);
    co->ctx = caller;  // FPSCR, GQRs, HIDs, MSR as the scheduler has them
    CpuContext& c = co->ctx;
    c.gpr[3] = Memory::Read32(frame + 0x00);
    c.gpr[4] = Memory::Read32(frame + 0x04);
    co->entryPc = Memory::Read32(frame + 0x08);
    co->finishPc = Memory::Read32(frame + 0x0C);
    c.lr = co->finishPc;
    c.ctr = co->entryPc;
    c.cr = Memory::Read32(frame + 0x10);
    c.xer = Memory::Read32(frame + 0x14);
    for (int r = 11; r <= 31; ++r) c.gpr[r] = Memory::Read32(frame + 0x18 + 4u * static_cast<uint32_t>(r - 11));
    c.gpr[0] = Memory::Read32(frame + 0x6C);
    for (int f = 14; f <= 31; ++f) {
        const uint32_t at = frame + 0x70 + 0x10u * static_cast<uint32_t>(f - 14);
        c.fpr[f].raw = (static_cast<uint64_t>(Memory::Read32(at)) << 32) | Memory::Read32(at + 4);
    }
    c.gpr[2] = Memory::Read32(frame + 0x190);
    c.fpscr = Memory::Read32(frame + 0x19C);
    c.gpr[1] = frame + kFrameSize;
    co->fiber = HostContext::Create(kCoroutineHostStack, CoroutineMain, co.get());
    if (!co->fiber) {
        RT_LOG(RT_TAG_OS) << "[opuc] coroutine fiber creation failed" << std::endl;
        std::abort();
    }
    RT_LOG(RT_TAG_OS) << "[opuc] coroutine " << co->token << " created: entry 0x" << std::hex << co->entryPc
                      << " arg 0x" << c.gpr[3] << " stack 0x" << c.gpr[1] << std::dec << std::endl;
    g_coroutines.push_back(std::move(co));
    return g_coroutines.back().get();
}

} // namespace

extern "C" void OpucCoroutineSwitch(CpuContext* ctx) {
    if (g_running) {
        // Yield: leave the marker frame where the real switch would put its 0x1A0 frame, publish it in
        // r13-0x4E10 (resume copies [frame, top) into the coroutine) and go back to whoever resumed us.
        Coroutine* co = g_running;
        const uint32_t frame = ctx->gpr[1] - kFrameSize;
        Memory::Write32(frame + 0x00, co->token);
        Memory::Write32(frame + 0x08, kMarker);
        Memory::Write32(kSwapFramePtrAddr, frame);
        void* back = co->resumer;
        g_running = nullptr;
        Fiber::GuestFiberManager::ExchangeCurrentThreadHostFiber(back);
        SwitchKeepingCpuContext(back);
        return;  // resumed: guest stack image is back in place, registers never left this fiber
    }

    // Resume: r13-0x4E10 points at the frame of the coroutine to switch in.
    const uint32_t frame = Memory::Read32(kSwapFramePtrAddr);
    Coroutine* co = nullptr;
    if (Memory::Read32(frame + 0x08) == kMarker) {
        const uint32_t token = Memory::Read32(frame + 0x00);
        if (token != 0 && token <= g_coroutines.size()) co = g_coroutines[token - 1].get();
    }
    if (!co) co = CreateFromFirstFrame(frame, *ctx);

    co->resumer = HostContext::Current();
    g_running = co;
    Fiber::GuestFiberManager::ExchangeCurrentThreadHostFiber(co->fiber);
    SwitchKeepingCpuContext(co->fiber);
    // The coroutine yielded (it set r13-0x4E10 and g_running = nullptr); resume continues at 80342180.
}
PPC_NATIVE_OVERRIDE_VOID(803DFC94, OpucCoroutineSwitch, (CpuContext* ctx), (ctx));
