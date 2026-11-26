// Enemy AI view (not in the game): shows what each enemy's AI looks at and picks. Field mobs run the EnemyAction
// state machine (vault entities/enemies.md sec. 16), bosses and CPU crew the UtilAi planner (entities/ai-overview.md).
// World-space marks (range rings, target lines) are ALPrim3D nodes in the field scene; the panels (inspector,
// planner, log, timeline) are ImGui windows. Switched from Dev > Debug > Enemy AI view, or OPUC_AI_VIEW=1.
#pragma once

#include "dev_pages.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace OpucAiView {

struct Settings {
    bool view = false;          // master switch
    bool mobRings = true;       // near / attack / sidestep rings and the facing wedge at a mob's feet
    bool groupRings = true;     // a group's encount / setup / chase rings
    bool bossRings = true;      // the boss / crew mov layers and the scored slot's band
    bool lines = true;          // enemy -> player lines, home markers
    bool labels = true;         // text above heads
    bool throughWalls = false;  // Z compare off
    bool inspector = true;      // the focused mob's panel
    bool planner = true;        // the boss / crew planner panel
    bool log = true;            // the decision log
    bool timeline = false;      // state bands over the last seconds
    int focus = -1;             // focused mob: -1 nearest, else the index in the snapshot
};

// The switches (game thread and overlay thread; plain values, read once per frame).
Settings& Get();
bool Trace();  // OPUC_TRACE_AI

// The Dev > Debug > Enemy AI view list.
std::shared_ptr<OpucDev::Page> AiPage();

// The ImGui panels, from OpucDev_DrawOverlays.
void DrawPanels();

// RandMT::NextU32 803b4a70 returned `value` (the existing wrapper in dev_pages.cpp calls this).
void OnRand(uint32_t value);

}  // namespace OpucAiView
