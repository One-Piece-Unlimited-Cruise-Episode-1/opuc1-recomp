// Window viewport policy: stretch the game's picture to the window, or keep a forced 4:3 / 16:9 picture
// ([video] force_aspect, F10 bar). The base project also widened the camera by patching Mario Kart Wii's
// EGG::Screen records; OPUC has no EGG library, so only the presentation policy is kept here (true widescreen
// for OPUC is open work, STATUS.md).
#include "aurora_events.h"

#include <atomic>

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

namespace {

bool g_widescreen = true;
bool g_forceAspect = false;
std::atomic_bool g_requestedForceAspect{false};
bool g_policyDirty = false;

} // namespace

void UpdateMkwDynamicAspectSurface(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    const bool requestedForceAspect = g_requestedForceAspect.load(std::memory_order_acquire);
    if (g_forceAspect != requestedForceAspect) {
        g_forceAspect = requestedForceAspect;
        g_policyDirty = true;
    }
    if (surfaceWidth == 0 || surfaceHeight == 0 || !g_policyDirty) {
        return;
    }
    if (g_forceAspect && !g_widescreen) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
    } else {
        AuroraSetViewportPolicy(g_forceAspect ? AURORA_VIEWPORT_16_9 : AURORA_VIEWPORT_STRETCH);
        VIUnlockAspectRatio();
    }
    g_policyDirty = false;
}

void SetMkwForceAspect(bool enabled) {
    g_requestedForceAspect.store(enabled, std::memory_order_release);
}

void ConfigureMkwDynamicAspect(bool widescreen, bool forceAspect, uint32_t surfaceWidth, uint32_t surfaceHeight) {
    (void)surfaceWidth;
    (void)surfaceHeight;
    g_widescreen = widescreen;
    g_forceAspect = forceAspect;
    g_requestedForceAspect.store(forceAspect, std::memory_order_relaxed);
    g_policyDirty = true;
}
