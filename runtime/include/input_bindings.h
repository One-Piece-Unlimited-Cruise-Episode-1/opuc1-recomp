#pragma once

// Input gate set by the settings overlay: while the F10 bar, a prompt, the startup cover or the controller
// setup wizard is open, game input (the virtual Wii Remote in hle/opuc/wiimote_opuc.cpp and paired real
// Wii Remotes) is held back.
namespace InputBindings {

void SetInputBlocked(bool blocked) noexcept;
bool InputBlocked() noexcept;

} // namespace InputBindings
