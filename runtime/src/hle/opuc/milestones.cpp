// OPUC boot milestones and once-per-second status, read by tools/status.mjs.
// "[opuc] milestone <name>" lines, one per name per run. Each marker sits in an HLE whose first call proves it:
//   main        first GXInit: only ALSystemGC::InitGraphicsAndHandlers (803ee2f4/803ee478/803ee5f0) calls it,
//               and that runs inside ALSystem::Main (803f03d8)
//   first-draw  first GXBegin
//   frame       first GXCopyDisp (EFB copied to the XFB that VI presents)
//   disc-read   first DVDReadPrio / DVDReadAsyncPrio served from the disc root
// "[opuc] status t=<s> copies=<n> (+<per second>) proj=<ptr> state=<id>" every second after "main":
//   ProjProcess = *(0x8068A2B0) (r13-0x5bf0, ProjProcess::Construct 80100e48), its ALTask = *(ProjProcess+0);
//   ALTask current state id at task+0x64
//   (ALTask::Run 8040d530). Vault: entities/boot-flow (ProjProcess state machine).
// "[opuc] dvd read <path> off=<o> len=<n>" the first time each disc file is read.
#include "hle/opuc/milestones.h"

#include "memory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>

extern "C" bool g_opucTraceNative = false;

namespace {
std::atomic<uint64_t> g_copyDisp{0};

// Native-call site histogram (racy by design: diagnostics only).
constexpr uint32_t kSites = 4096;
uint32_t g_siteLr[kSites];
uint32_t g_siteHits[kSites];

void PrintHotSites() {
    uint32_t lr[kSites], hits[kSites];
    for (uint32_t i = 0; i < kSites; ++i) { lr[i] = g_siteLr[i]; hits[i] = g_siteHits[i]; g_siteHits[i] = 0; }
    for (int k = 0; k < 5; ++k) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < kSites; ++i) if (hits[i] > hits[best]) best = i;
        if (hits[best] == 0) break;
        std::fprintf(stderr, "[opuc]   native site lr=0x%08X calls=%u\n", lr[best], hits[best]);
        hits[best] = 0;
    }
}

uint32_t TryRead32(uint32_t address) {
    try {
        return Memory::Read32(address);
    } catch (...) {
        return 0xFFFFFFFFu;
    }
}

void StatusThread() {
    const auto start = std::chrono::steady_clock::now();
    uint64_t lastCopies = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto t = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
        const uint64_t copies = g_copyDisp.load(std::memory_order_relaxed);
        const uint32_t proj = TryRead32(0x8068A2B0u);
        // ProjProcess+0 points at its ALTask (ProjProcess::Construct 80100e48: lwz r3,0(r28); ALTask::Start).
        const uint32_t task = (proj >= 0x80000000u && proj < 0x81800000u) ? TryRead32(proj) : 0u;
        const uint32_t state = (task >= 0x80000000u && task < 0x81800000u) ? TryRead32(task + 0x64u) : 0xFFFFFFFFu;
        // Byte r13-0x68b9 (0x806895E7): ALPadWii converts buttons only while it is non-zero (803e9074).
        const uint32_t padGate = TryRead32(0x806895E4u) & 0xFFu;
        std::fprintf(stderr, "[opuc] status t=%llds copies=%llu (+%llu) proj=0x%08X state=0x%X padgate=%u\n",
                     static_cast<long long>(t.count()), static_cast<unsigned long long>(copies),
                     static_cast<unsigned long long>(copies - lastCopies), proj, state, padGate);
        std::fflush(stderr);
        lastCopies = copies;
        if (g_opucTraceNative) PrintHotSites();
    }
}
} // namespace

extern "C" void OpucMilestone(const char* name) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(name).second) return;
    std::fprintf(stderr, "[opuc] milestone %s\n", name);
    std::fflush(stderr);
    if (std::string(name) == "main") {
        const char* trace = std::getenv("OPUC_TRACE_NATIVE");
        g_opucTraceNative = trace && trace[0] == '1';
        std::thread(StatusThread).detach();
    }
}

extern "C" void OpucNativeHit(uint32_t lr) {
    const uint32_t i = (lr >> 2) & (kSites - 1);
    g_siteLr[i] = lr;
    ++g_siteHits[i];
}

extern "C" void OpucCountCopyDisp() { g_copyDisp.fetch_add(1, std::memory_order_relaxed); }

extern "C" void OpucTraceDvdRead(const char* path, uint32_t offset, uint32_t length) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(path ? path : "?").second) return;
    std::fprintf(stderr, "[opuc] dvd read %s off=0x%X len=0x%X\n", path ? path : "?", offset, length);
    std::fflush(stderr);
}
