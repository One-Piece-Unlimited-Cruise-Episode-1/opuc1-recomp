#pragma once

// OPUC1 is a single-region (PAL, ROUPAF) build: a guest address is the address in main.dol.
//
//   MKW_GADDR(80274EA8 /*mkw:801A9E84*/)       guest function or data address used by HLE code
//   MKW_GADDR_CT(80274EA8 /*mkw:801A9E84*/)    same; spelled separately where a constant expression is needed
//
// The /*mkw:...*/ comment records the upstream address tools/rebind-runtime.mjs mapped it from.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace GuestRegion {

struct Facts {
    const char* gameId;
    uint32_t gameCode;
    uint32_t viTvFormat;
    uint32_t mem1ArenaLo;
    // setting.txt defaults for a new NAND
    const char* area;
    const char* model;
    const char* productCode;
    const char* video;
    const char* gameRegion;
};

// OPUC1 PAL: game code 'ROUP', __ArenaLo 0x806A29C0 (OSInit 8026b410), VI tv format 1 (PAL).
inline constexpr Facts kFacts = {"ROUPAF", 0x524F5550u, 1u, 0x806A29C0u, "EUR", "RVL-001(EUR)", "LEH", "PAL", "EU"};

inline const Facts& CurrentFacts() noexcept { return kFacts; }

} // namespace GuestRegion

#define MKW_GADDR(pal) (0x##pal##u)
#define MKW_GADDR_CT(pal) (0x##pal##u)

// OPUC port markers written by tools/rebind-runtime.mjs.
// OPUC_UNBOUND: an upstream MKWii HLE registration with no OPUC counterpart yet. Compiles to nothing, so the
// translated SDK code at the OPUC address runs instead. The translator's registration regex does not match it.
#ifndef OPUC_UNBOUND
#define OPUC_UNBOUND(...)
#endif
// OPUC_TODO: an upstream MKWii guest address with no OPUC counterpart yet. Many sites are namespace-scope
// constants that are evaluated during static initialization, so the marker cannot be fatal on evaluation. It
// yields a poison guest address 0xF0xxxxxx (low 24 bits of the MKWii address) in an unmapped region: the first
// load/store/call through it faults, and a crash naming 0xF0xxxxxx means "unported MKWii 0x80xxxxxx".
namespace OpucTodo {
[[noreturn]] inline void Fatal(const char* what, uint32_t mkwAddress) noexcept {
    std::fprintf(stderr, "[opuc] FATAL: runtime reached an unported MKWii %s 0x%08X (see projects/opuc1/rebind_report.md)\n",
                 what, mkwAddress);
    std::fflush(stderr);
    std::abort();
}
inline constexpr uint32_t kPoisonBase = 0xF0000000u;
template <uint32_t Mkw> constexpr uint32_t Address() noexcept { return kPoisonBase | (Mkw & 0x00FFFFFFu); }
inline bool IsPoison(uint32_t address) noexcept { return (address & 0xFF000000u) == kPoisonBase; }
}
#define OPUC_TODO(kind, mkw) OPUC_TODO_##kind(mkw)
#define OPUC_TODO_MKW_GADDR(mkw) (::OpucTodo::Address<0x##mkw##u>())
#define OPUC_TODO_MKW_GADDR_CT(mkw) (::OpucTodo::Address<0x##mkw##u>())

