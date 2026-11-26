// Names for the Enemy AI view. The game stores no AI names: the mob state names are the vault's (entities/enemies.md
// sec. 16 state table), the planner behaviour names the vault's mov table (entities/mov.md, ai-overview.md), the cond
// kinds / ops / actions the Character editor's enums (UI.Opus1.CharacterAi, BossAiAdvCond*).
#pragma once

#include <cstdint>

namespace OpucAiView {

inline const char* MobStateName(int s) {
    switch (s) {
        case 0x00: return "Stand";
        case 0x01: return "Chase";
        case 0x02: return "Circle";
        case 0x03: return "Alert";
        case 0x04: return "Attack";
        case 0x05: return "State5";
        case 0x06: return "Retreat";
        case 0x07: return "Launched";
        case 0x08: return "Launched2";
        case 0x09: return "Down";
        case 0x0A: return "Vanish";
        case 0x0B: return "Remove";
        case 0x0C: return "Chase(C)";
        case 0x0D: return "Stand(D)";
        case 0x0E: return "Sidestep";
        case 0x0F: return "LookAround";
        case 0x10: return "ReturnHome";
        case 0x11: return "Leash";
        case 0x12: return "Leash2";
        case 0x13: return "LockOn";
        case 0x14: return "SlowApproach";
        case 0x15: return "Flinch";
        case 0x16: return "GetUp";
        case 0x17: return "LethalFlinch";
        case 0x18: return "Jump";
        case 0x19: return "Fall";
        case 0x1A: return "Land";
        case 0x1B: return "AirAttack";
        case 0x1C: return "Evade";
        case 0x1D: return "Submerge";
        case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: return "Entry";
        case -1: return "-";
        default: return "?";
    }
}

// The colour family of a mob state for lines, rings and timeline bands (0xRRGGBB).
inline uint32_t MobStateRgb(int s) {
    switch (s) {
        case 0x00: case 0x0D: case 0x06: case 0x0F: case 0x10: case 0x11: case 0x12: return 0x9FB4C4;  // idle / leash: grey
        case 0x03: return 0xFFE25A;                                                                 // wind-up: yellow
        case 0x01: case 0x02: case 0x0C: case 0x13: case 0x14: case 0x0E: return 0xFF9A3C;         // closing in: orange
        case 0x04: case 0x1B: return 0xFF4A4A;                                                      // attacking: red
        case 0x1C: case 0x1D: case 0x18: case 0x19: case 0x1A: return 0x6AB0FF;                    // evade / air: blue
        default: return 0xB070FF;                                                                   // hit / down: purple
    }
}

// Perception bits in EnemyObj +0xE0 (EnemyObj::UpdatePerception 8008cd50).
struct BitName { uint32_t bit; const char* name; };
constexpr BitName kPerceptionBits[] = {
    // An attack request is 0x10000000 (facing within 45 degrees) plus the kind bit: 0x18 / 0x14 / 0x12000000.
    {0x40000000, "near"},      {0x01000000, "touching"}, {0x10000000, "facing"},        {0x08000000, "atk0"},
    {0x04000000, "atk1"},      {0x02000000, "atk2"},     {0x00200000, "sidestep"},      {0x00100000, "chase ok"},
    {0x00800000, "in front"},  {0x00400000, "behind"},   {0x00080000, "out of chase"},  {0x00040000, "submerge ring"},
};

// Planner behaviours (FSM id -> name), selector 800100e0.
inline const char* PlannerBehaviourName(uint32_t id) {
    switch (id) {
        case 0x2000: return "phase motion";
        case 0x1000: return "own move state 7";
        case 0x0000: return "recover wait";
        case 0x0010: return "approach";
        case 0x0011: return "orbit";
        case 0x0030: return "wander gate";
        case 0x0040: return "approach kind";
        case 0x0050: return "evade";
        case 0x00C0: return "slot 4 variant";
        case 0x00C1: return "slot 4 (0xC1)";
        case 0x00D0: return "slot 5 variant";
        case 0x00D1: return "slot 5 (0xD1)";
        case 0x0100: return "C follow-up 6";
        case 0x0110: return "C follow-up 7";
        case 0x0038: return "particular band";
        case 0x0039: return "particular react";
        default: break;
    }
    if (id >= 0x80 && id <= 0xF0 && (id & 0xF) == 0) {
        static const char* kSlots[] = {"slot 0", "slot 1", "slot 2", "slot 3", "slot 4", "slot 5", "slot 6", "slot 7"};
        return kSlots[(id - 0x80) >> 4];
    }
    return "?";
}

// cond rows (ai/{code}_cond.atb, UtilAi::EvalAdvCond 800106b8).
inline const char* CondKindName(uint32_t k) {
    switch (k) {
        case 1: return "HP%";
        case 2: return "progress%";
        case 3: return "has status";
        case 4: return "missing status";
        case 5: return "alive others";
        default: return "?";
    }
}
inline const char* CondOpName(uint32_t op) {
    static const char* kOps[] = {"==", "!=", ">", ">=", "<", "<=", "always", "never"};
    return op < 8 ? kOps[op] : "?";
}
inline const char* CondActionName(uint32_t a) {
    switch (a) {
        case 0: return "QueuePhaseMotion";
        case 2: return "HealToMax";
        case 4: return "SetMoveStage";
        case 5: return "SpawnEffect";
        case 6: return "RequestSlot67";
        case 7: return "QueueRandomEffects";
        case 8: return "SetPoweredForm";
        default: return "?";
    }
}

}  // namespace OpucAiView
