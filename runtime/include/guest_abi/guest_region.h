#pragma once

// Guest addresses are named by their PAL (RMCP01) address. Which address that identity has
// depends on the region the game was translated from, which runtime/game supplies at link
// time, so one compiled runtime serves every region.
//
//   MKW_GADDR(80274EA8 /*mkw:801A9E84*/)       address of that identity in the running region
//   MKW_GADDR_CT(80274EA8 /*mkw:801A9E84*/)    same, as a constant expression; translated code only
//
// guest_versions.inc maps PAL ranges to the other regions. A PAL address outside every range
// of a region does not compile.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace GuestRegion {

enum class Id : uint8_t {
    Pal = 0,
    NtscU = 1,
    NtscJ = 2,
};
inline constexpr size_t kCount = 3;

struct Row {
    uint32_t address[kCount];

    constexpr uint32_t In(Id region) const { return address[static_cast<size_t>(region)]; }
};

struct Range {
    Id region;
    uint32_t first;
    uint32_t last;
    int32_t delta;
};

inline constexpr Range kRanges[] = {
#define MKW_REGION_RANGE(region, first, last, delta) {Id::region, 0x##first##u, 0x##last##u, delta},
#include "guest_abi/guest_versions.inc"
#undef MKW_REGION_RANGE
};
inline constexpr size_t kRangeCount = sizeof(kRanges) / sizeof(kRanges[0]);

constexpr bool Precedes(Id region, uint32_t address, const Range& range) {
    return region < range.region || (region == range.region && address < range.first);
}

constexpr bool RangesAreOrdered() {
    for (size_t i = 0; i < kRangeCount; ++i) {
        const Range& range = kRanges[i];
        if (range.region == Id::Pal || range.first > range.last) {
            return false;
        }
        if (i != 0 && !Precedes(kRanges[i - 1].region, kRanges[i - 1].last, range)) {
            return false;
        }
    }
    return true;
}
static_assert(RangesAreOrdered(), "guest_versions.inc ranges must be sorted by region and PAL address without overlaps");

struct Mapping {
    bool found;
    uint32_t address;
};

constexpr Mapping Map(uint32_t pal, Id region) {
    if (region == Id::Pal) {
        return {true, pal};
    }
    size_t lo = 0;
    size_t hi = kRangeCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (Precedes(region, pal, kRanges[mid])) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo == 0) {
        return {false, 0};
    }
    const Range& range = kRanges[lo - 1];
    if (range.region != region || pal > range.last) {
        return {false, 0};
    }
    return {true, static_cast<uint32_t>(static_cast<int64_t>(pal) + range.delta)};
}

constexpr Row RowOf(uint32_t pal) {
    const Mapping ntscU = Map(pal, Id::NtscU);
    const Mapping ntscJ = Map(pal, Id::NtscJ);
    // OPUC: single-region build (ROUPAF). Addresses outside the MKWii region table map to themselves.
    return ntscU.found && ntscJ.found ? Row{{pal, ntscU.address, ntscJ.address}} : Row{{pal, pal, pal}};
}

} // namespace GuestRegion

// Defined by runtime/game for the translated region.
extern "C" const uint8_t kMkwGuestRegion;

namespace GuestRegion {

inline Id Current() noexcept { return static_cast<Id>(kMkwGuestRegion); }

template <uint32_t Pal>
inline uint32_t Address() noexcept {
    constexpr Row row = RowOf(Pal);
    if constexpr (row.address[1] == Pal && row.address[2] == Pal) {
        return Pal;
    } else {
        return row.In(Current());
    }
}

// Folds to false at compile time for a constant target that no region uses for Pal.
template <uint32_t Pal>
inline bool Is(uint32_t target) noexcept {
    constexpr Row row = RowOf(Pal);
    return (target == row.address[0] || target == row.address[1] || target == row.address[2]) &&
           target == row.In(Current());
}

[[noreturn]] void FatalUnknownAddress(uint32_t pal) noexcept;

// For hand-written registrations, which name the PAL address.
inline uint32_t Translate(uint32_t pal) noexcept {
    const Mapping mapping = Map(pal, Current());
    if (!mapping.found) {
        FatalUnknownAddress(pal);
    }
    return mapping.address;
}

struct Facts {
    const char* gameId;
    char letter;
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

inline constexpr Facts kFacts[kCount] = {
    // OPUC1 PAL: game code 'ROUP', __ArenaLo 0x806A29C0 (OSInit 8026b410), VI tv format 1 (PAL) as MKWii PAL.
    {"ROUPAF", 'P', 0x524F5550u, 1u, 0x806A29C0u, "EUR", "RVL-001(EUR)", "LEH", "PAL", "EU"},
    {"RMCE01", 'E', 0x524D4345u, 0u, 0x80394E00u, "USA", "RVL-001(USA)", "LU", "NTSC", "US"},
    {"RMCJ01", 'J', 0x524D434Au, 0u, 0x80398B00u, "JPN", "RVL-001(JPN)", "LJ", "NTSC", "JP"},
};

inline const Facts& CurrentFacts() noexcept { return kFacts[static_cast<size_t>(Current())]; }

} // namespace GuestRegion

#define MKW_GADDR(pal) (::GuestRegion::Address<0x##pal##u>())

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

#if defined(MKW_TRANSLATED_GUEST_REGION)
#define MKW_GADDR_CT(pal) \
    (::GuestRegion::RowOf(0x##pal##u).In(static_cast<::GuestRegion::Id>(MKW_TRANSLATED_GUEST_REGION)))
#endif
