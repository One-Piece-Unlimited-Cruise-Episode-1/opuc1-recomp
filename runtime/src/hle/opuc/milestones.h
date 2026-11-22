#pragma once
#include <atomic>
#include <cstdint>
// Prints "[opuc] milestone <name>" to stderr the first time a name is reached (see milestones.cpp).
extern "C" void OpucMilestone(const char* name);
// Counted by the GXCopyDisp HLE; reported once per second by the status thread.
extern "C" void OpucCountCopyDisp();
// First read of each disc file (DVDReadPrio HLE).
extern "C" void OpucTraceDvdRead(const char* path, uint32_t offset, uint32_t length);
// Per-call-site guard so hot HLEs (GXBegin) pay one relaxed load after the first call.
#define OPUC_MILESTONE(name)                                                       \
    do {                                                                           \
        static std::atomic<bool> opucMilestoneSeen{false};                          \
        if (!opucMilestoneSeen.load(std::memory_order_relaxed) &&                   \
            !opucMilestoneSeen.exchange(true)) {                                    \
            OpucMilestone(name);                                                   \
        }                                                                          \
    } while (0)
