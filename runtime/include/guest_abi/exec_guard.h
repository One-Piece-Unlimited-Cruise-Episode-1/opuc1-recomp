#pragma once

#include <cstdint>

namespace TranslatedExecution {

// Diagnostic-only, read by fatal reporters (unmapped access, MMIO, CPU dumps).
// Defined here as `inline thread_local` with a constant initializer, not `extern thread_local`
// in the .cpp: every indirect dispatch scopes this, so an out-of-line ctor/dtor would cost two
// un-inlinable calls plus a register spill each for three instructions of work.
inline thread_local uint32_t g_currentAddress = 0;

// The guest address of the translated function currently running on this thread, or 0.
inline uint32_t CurrentAddress() noexcept {
    return g_currentAddress;
}

class Scope {
public:
    explicit Scope(uint32_t address) noexcept
        : previous_(g_currentAddress) {
        // Address 0 means "nothing new to report" - the enclosing scope's value
        // stays visible, and the unconditional restore below keeps nesting exact.
        if (address != 0) {
            g_currentAddress = address;
        }
    }

    ~Scope() noexcept {
        g_currentAddress = previous_;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    uint32_t previous_ = 0;
};

} // namespace TranslatedExecution
