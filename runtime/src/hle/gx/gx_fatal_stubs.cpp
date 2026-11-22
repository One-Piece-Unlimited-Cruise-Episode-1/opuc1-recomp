// Auto-generated GX fatal stubs
#include "hle_stubs.h"
#include "runtime_log.h"

namespace {
[[noreturn]] void HaltGX(uint32_t addr, const char* name) {
    const char* symbol = name ? name : "<unknown GX symbol>";
    RT_LOGF(RT_TAG_GX,
            "unimplemented GX entry point: %s at guest address 0x%08X.\n"
            "[gx] This graphics call has no Aurora implementation bound to it yet, so the\n"
            "[gx] runtime cannot continue without silently dropping GPU state. Bind it in\n"
            "[gx] runtime/src/hle/gx/ and remove the stub from gx_fatal_stubs.cpp.\n",
            symbol, addr);
    std::fflush(stderr);
    char message[512]{};
    std::snprintf(message, sizeof(message),
                  "%s at guest address 0x%08X has no Aurora implementation bound to it, so the "
                  "runtime stopped rather than keep rendering with missing GPU state.",
                  symbol, addr);
    ShowRuntimeFatalPopup("the game called an unimplemented graphics function", message);
    std::abort();
}
} // namespace

// OPUC: the upstream file registered ~200 MKWii GX addresses as fatal stubs (GX functions MKWii never calls).
// OPUC calls a different GX subset and the stub addresses do not carry over, so no stubs are registered: a GX
// function without an Aurora binding runs translated and reaches Aurora through the write-gather-pipe FIFO decoder.
// HaltGX stays for OPUC-specific stubs added later.
[[maybe_unused]] static void (*const kOpucHaltGx)(uint32_t, const char*) = HaltGX;
