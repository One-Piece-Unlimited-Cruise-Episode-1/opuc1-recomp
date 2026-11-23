#include "input_bindings.h"

#include <atomic>

namespace InputBindings {
namespace {
std::atomic<bool> g_inputBlocked{false};
} // namespace

void SetInputBlocked(bool blocked) noexcept {
    g_inputBlocked.store(blocked, std::memory_order_relaxed);
}

bool InputBlocked() noexcept {
    return g_inputBlocked.load(std::memory_order_relaxed);
}

} // namespace InputBindings
