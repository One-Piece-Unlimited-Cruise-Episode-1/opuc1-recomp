// PC fishing (ported from the Play tab's play/fishing.js; not in the game): the controls the virtual Wii Remote
// (wiimote_opuc.cpp) sends while the player fishes, the assist HUD and the PC PlayGuide pictures
// (fishing_pc.cpp, pc_pictures.cpp).
#pragma once

#include "fishing_motion.h"

#include <vector>

namespace OpucFishing {

// The keyboard / mouse state of one 60 Hz frame (the Play tab's Fishing key group, default keys).
struct PadIn {
    bool cast = false;      // left mouse / J: cast (hold: power, release: throw) / reel (hold)
    bool hookEdge = false;  // right mouse / L pressed: hook
    bool quitEdge = false;  // Space / Backspace pressed: reel in / stop fishing
    float sx = 0, sy = 0;   // the move keys (A / D lean, W raise) as the stick
    double mouseDx = 0, mouseDy = 0;  // mouse movement this frame, radians (Remote input)
};

// What the remote sends instead while fishing owns the controls.
struct PadOut {
    bool active = false;  // fishing owns the controls this frame
    bool A = false;       // the A button as the game should read it
    bool B = false;       // a B press (reel the line in)
    bool right = false;   // a D-pad Right press (Unequip: put the rod away)
};

// Called once per KPADRead on the game thread. Fills `samples` (oldest first) with n accelerometer samples while
// fishing; returns out.active.
bool Frame(const PadIn& in, int n, std::vector<V3>& samples, PadOut& out);

// The Dev menu's Fishing switches (Play tab pauseext.js fishingPage; dev_pages.cpp).
struct DevSwitches {
    bool autoHook = false;         // a real bite is hooked by itself (also OPUC_FISHING_AUTOHOOK)
    bool lineNeverBreaks = false;  // line HP stays above 5 %, a long cast or run does not snap the line
    bool alwaysBite = false;       // the bite rolls succeed, any of the spot's fish bites at any distance
    bool remoteGraph = false;      // the overlay draws the game's WiiMotion swing ring
};
DevSwitches& Dev();

// The game's WiiMotion as the Remote graph shows it (copied on the game thread after each CharaFishing::Update).
struct GraphSnapshot {
    bool active = false;
    float ring[60] = {};  // |acc| per sample, oldest first
    double power = 0;     // AnalyzeSwing power * 0.25 (0 when quiet >= 12)
    int quiet = 0;
    int gesture = 0;      // the published gesture byte (WiiMotion +0x1320)
};
GraphSnapshot Graph();

}  // namespace OpucFishing
