// Calling guest (translated) functions from OPUC native code with arguments.
//
// A native hook runs inside a guest call: `ctx` is the caller's register file and r1 its stack. GuestCall copies
// that context, loads r3.. with the arguments and calls the guest address through the dispatcher, so the callee
// builds its frame below the caller's r1 exactly as a `bl` from the hooked function would. The caller's registers
// are untouched; the result is the callee's r3 (f1 for GuestCallF).
#pragma once

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace OpucGuest {

// Room left between the caller's r1 and the callee's frame: a hooked function may have stored into its own red
// zone before the native ran, and scratch data (strings) may sit in this gap (ScratchBelow).
inline constexpr uint32_t kCallGap = 0x400u;

inline CpuContext MakeCallContext(const CpuContext& caller) {
    CpuContext c = caller;
    c.gpr[1] = (caller.gpr[1] - kCallGap) & ~0xFu;
    // The PowerPC EABI back chain: the callee's prologue stores r1 at 0(r1).
    Memory::Write32(c.gpr[1], caller.gpr[1]);
    return c;
}

inline uint32_t Call(const CpuContext& caller, uint32_t address, std::initializer_list<uint32_t> args = {},
                     std::initializer_list<double> fargs = {}) {
    CpuContext c = MakeCallContext(caller);
    uint32_t r = 3;
    for (uint32_t a : args) c.gpr[r++] = a;
    uint32_t f = 1;
    for (double d : fargs) c.fpr[f++].d = d;
    CpuContextScope scope(&c);
    InvokeIndirectCpu(address, &c);
    return c.gpr[3];
}

// The base translation of `address`, bypassing a native registered at the same address (for a wrapper calling
// the function it wraps). Arguments as Call.
inline uint32_t CallBase(const CpuContext& caller, uint32_t address, std::initializer_list<uint32_t> args = {}) {
    CpuContext c = MakeCallContext(caller);
    uint32_t r = 3;
    for (uint32_t a : args) c.gpr[r++] = a;
    CpuContextScope scope(&c);
    GuestFunctions::BaseTranslation(address)(&c);
    return c.gpr[3];
}

// Scratch guest memory in the gap between the caller's r1 and the callee frame Call builds (kCallGap bytes; the
// top 0x40 stay free). Valid only for the duration of one Call made with the same caller context.
inline uint32_t ScratchBelow(const CpuContext& caller, uint32_t size) {
    const uint32_t top = caller.gpr[1] - 0x40u;
    return (top - size) & ~0x7u;
}

// Writes a big-endian UTF-16 string (the game's wchar_t) with its terminator; returns the bytes written.
inline uint32_t WriteWide(uint32_t at, std::u16string_view text) {
    uint32_t o = 0;
    for (char16_t ch : text) { Memory::Write16(at + o, static_cast<uint16_t>(ch)); o += 2; }
    Memory::Write16(at + o, 0);
    return o + 2;
}

inline std::u16string ReadWide(uint32_t at, uint32_t maxChars = 256) {
    std::u16string s;
    for (uint32_t i = 0; i < maxChars; ++i) {
        const uint16_t ch = Memory::Read16(at + 2 * i);
        if (!ch) break;
        s.push_back(static_cast<char16_t>(ch));
    }
    return s;
}

} // namespace OpucGuest
