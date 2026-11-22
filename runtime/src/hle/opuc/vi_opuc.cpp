// OPUC-only VI HLE: SDK VI functions OPUC calls that upstream (MKWii) never bound.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "runtime_log.h"

// VIGetScanMode (80284c70): reads VI register 0xCC00206C. The runtime presents progressive frames, but the game's
// render mode decides field handling; report VI_INTERLACE (0) like a PAL console on composite until a game path
// needs otherwise (Dolphin default for ROUPAF is interlaced 50 Hz). ^[OPUC-VI-01]
extern "C" uint32_t VIGetScanMode_OPUC() {
    return 0u; // VI_INTERLACE
}
PPC_NATIVE_OVERRIDE(80284C70, VIGetScanMode_OPUC, uint32_t, (), ());
