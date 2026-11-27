// Enemy AI view (not in the game). See ai_view.h. Facts used here, vault entities/enemies.md:
//   EnemyManager *(0x8068A1E8): +0x38 encountered groups, an intrusive list {first, last} of nodes {next, prev} at
//   group + 8; group +0x10 member list (node at enemy + 0x98), +0x18 enemy_locate row, +0x20 range flags.
//   EnemyFactory *(0x8068A608): +0x1C EnemyBaseData table {+8 row count, +0xC rows (0x5C: +0 enemy_id, +0x08 radius)}, +0x124 working EnemyAIData rows
//   (0x98, crowd-scaled).
//   Enemy: +0xAC enemy_id, +0xB4 ai_type, +0xBC flags, +0xC4 attack kind, +0xE0 perception bits, +0x144 dir to the
//   player, +0x154 distance, +0x1C8 home, +0x214 / +0x218 / +0x23C timers; state id *(*(*(*(e+0x3C))+0x48)+0x10);
//   position *(*(*(e+0xC))+0x54)+0x10 as for every CharaObj.
#include "ai_view.h"

#include "ai_view_font.h"
#include "ai_view_names.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>

extern "C" int g_gxFrameCount;  // hle/gx/gx_copy.cpp, counted per GXCopyDisp

namespace OpucAiView {
namespace {

constexpr uint32_t kEnemyManagerPtr = 0x8068A1E8;
constexpr uint32_t kEnemyFactoryPtr = 0x8068A608;
constexpr uint32_t kOn = 0xFFE25A, kOff = 0x9FB4C4;

bool GuestPtr(uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }
uint32_t Deref(uint32_t p) { return GuestPtr(p) ? Memory::Read32(p) : 0; }
float F32(uint32_t at) { return Memory::ReadFloat32(at); }
int32_t S32(uint32_t at) { return static_cast<int32_t>(Memory::Read32(at)); }
std::u16string U(std::string_view s) { return std::u16string(s.begin(), s.end()); }

Settings g_settings;
std::mutex g_mutex;  // the snapshot and the log (game thread writes, overlay reads)
uint32_t g_frame = 0;

}  // namespace

Settings& Get() { return g_settings; }

bool Trace() {
    static const bool on = [] {
        const char* v = std::getenv("OPUC_TRACE_AI");
        return v && *v && *v != '0';
    }();
    return on;
}

namespace {

struct Mob {
    uint32_t addr = 0, group = 0;
    int enemyId = -1, aiType = -1, state = -1, attackKind = -1;
    uint32_t flags = 0, perception = 0, events = 0;
    float pos[3] = {}, home[3] = {}, dist = 0, radius = 0;
    int32_t timer = 0, lockout = 0, freeze = 0;
    uint32_t aiRow = 0;
    uint32_t stateSince = 0;  // g_frame when the state was entered (as seen by the snapshot)
};

struct Event {
    uint32_t frame, enemy;
    std::string text;
    int count = 1;
};

struct CondResult {
    uint32_t frame = 0;
    bool result = false;
    float chance = -1, roll = -1;  // a roll seen while the cond ran
};

struct Group {
    uint32_t addr = 0, row = 0;
    bool encountered = false;
    uint32_t rangeFlags = 0;  // group +0x20 byte (801b3998): 0x40 encount, 0x20 setup, 0x10 chase, 0x08 return
    float center[3] = {}, encount = 0, setup = 0, chase = 0, dist = 0;
    int members = 0;
};

std::vector<Group> g_groups;
std::map<uint32_t, uint32_t> g_lastGroupFlags;
std::vector<Mob> g_mobs;
std::map<uint32_t, Mob> g_last;                                      // enemy -> last snapshot (state changes)
std::deque<Event> g_log;                                              // newest at the back
std::map<uint32_t, std::deque<std::pair<uint32_t, int>>> g_history;  // enemy -> (frame, state) changes
std::map<uint32_t, uint32_t> g_condEventFrame;                      // enemy -> frame of its last cond event
std::map<uint32_t, std::map<uint32_t, CondResult>> g_conds;          // enemy -> cond fn -> last result
uint32_t g_player = 0;
float g_playerPos[3] = {};

// The cond being evaluated (set by the cond wrappers, read by OnRand).
uint32_t g_condEnemy = 0, g_condFn = 0;
float g_condRoll = -1;

void PushEvent(uint32_t enemy, std::string text) {
    if (Trace()) RT_LOG(RT_TAG_HLE) << "[opuc] ai: " << std::hex << enemy << std::dec << " " << text << std::endl;
    for (auto it = g_log.rbegin(); it != g_log.rend() && g_frame - it->frame < 600; ++it)
        if (it->enemy == enemy) {
            if (it->text == text) {  // the same decision again: count it
                ++it->count;
                it->frame = g_frame;
                return;
            }
            break;
        }
    g_log.push_back({g_frame, enemy, std::move(text)});
    while (g_log.size() > 200) g_log.pop_front();
}

void ReadPos(uint32_t chara, float* out) {
    const uint32_t post = Deref(Deref(Deref(chara + 0xC)) + 0x54);
    if (!GuestPtr(post)) return;
    for (int k = 0; k < 3; ++k) out[k] = F32(post + 0x10 + 4 * k);
}

int StateOf(uint32_t e) {
    const uint32_t cur = Deref(Deref(Deref(e + 0x3C)) + 0x48);
    return GuestPtr(cur) ? S32(cur + 0x10) : -1;
}

Mob ReadMob(uint32_t e, uint32_t group) {
    Mob m;
    m.addr = e;
    m.group = group;
    m.enemyId = S32(e + 0xAC);
    m.aiType = S32(e + 0xB4);
    m.flags = Memory::Read32(e + 0xBC);
    m.attackKind = S32(e + 0xC4);
    m.events = Memory::Read32(e + 0xD8);
    // +0xE0 is cleared at the end of EnemyObj::Update (copied to +0x108), so the last frame's bits are at +0x108.
    m.perception = Memory::Read32(e + 0x108);
    m.dist = F32(e + 0x154);
    for (int k = 0; k < 3; ++k) m.home[k] = F32(e + 0x1C8 + 4 * k);
    m.timer = S32(e + 0x214);
    m.lockout = S32(e + 0x218);
    m.freeze = S32(e + 0x23C);
    m.state = StateOf(e);
    ReadPos(e, m.pos);
    if (const uint32_t ef = Deref(kEnemyFactoryPtr)) {
        const uint32_t base = Deref(ef + 0x1C), rows = Deref(ef + 0x124);
        // +0x1C is a table object {?, vtable, row count, rows*}; a row starts with its enemy_id, and the rows are not
        // in id order (index 19 holds id 17 on Fst).
        const uint32_t brows = GuestPtr(base) ? Deref(base + 0xC) : 0;
        const int count = GuestPtr(brows) ? std::min(S32(base + 8), 64) : 0;
        for (int k = 0; k < count; ++k)
            if (S32(brows + k * 0x5C) == m.enemyId) {
                m.radius = F32(brows + k * 0x5C + 0x08);
                break;
            }
        if (m.radius == 0 && m.enemyId >= 0 && m.enemyId < count) m.radius = F32(brows + m.enemyId * 0x5C + 0x08);  // no row with the id
        if (GuestPtr(rows) && m.aiType >= 0 && m.aiType < 128) m.aiRow = rows + m.aiType * 0x98;
    }
    return m;
}

// Walks the encountered groups; called once per frame on the game thread.
void Snapshot() {
    ++g_frame;
    std::vector<Mob> mobs;
    std::vector<Group> groups;
    const uint32_t em = Deref(kEnemyManagerPtr);
    if (GuestPtr(em)) {
        // The leader (CharaManager *(0x8068A18C) +0x88); EnemyManager +0x04 is not a CharaObj with the posture chain.
        g_player = Deref(Deref(0x8068A18C) + 0x88);
        if (GuestPtr(g_player)) ReadPos(g_player, g_playerPos);
        for (uint32_t list : {0x30u, 0x38u}) {
            int guard = 0;
            for (uint32_t gn = Deref(em + list); GuestPtr(gn) && guard < 64; gn = Deref(gn), ++guard) {
                Group g;
                g.addr = gn - 8;
                g.encountered = list == 0x38;
                g.row = Deref(g.addr + 0x18);
                g.rangeFlags = Memory::Read8(g.addr + 0x20);
                g.members = S32(g.addr + 0x98);
                if (GuestPtr(g.row)) {
                    for (int k = 0; k < 3; ++k) g.center[k] = F32(g.row + 4 * k);
                    g.encount = F32(g.row + 0x10);
                    g.setup = F32(g.row + 0x14);
                    g.chase = F32(g.row + 0x18);
                    const float dx = g.center[0] - g_playerPos[0], dz = g.center[2] - g_playerPos[2];
                    g.dist = std::sqrt(dx * dx + dz * dz);
                }
                groups.push_back(g);
                if (!g.encountered) continue;
                int inner = 0;
                for (uint32_t mn = Deref(g.addr + 0x10); GuestPtr(mn) && inner < 16; mn = Deref(mn), ++inner)
                    mobs.push_back(ReadMob(mn - 0x98, g.addr));
            }
        }
    }
    if (Trace() && GuestPtr(em)) {
        static bool dumped = false;
        if (!dumped && std::getenv("OPUC_AI_DUMP_TRANSITIONS")) {
            dumped = true;
            // EnemyAction::RunTransitions 801b6d90: lists at 805ECC20 + kind * 0x130 + state * 8, node {next, prev,
            // cond, target, pickState}.
            for (uint32_t kind = 0; kind < 2; ++kind)
                for (uint32_t st = 0; st < 0x26; ++st) {
                    std::string line;
                    char b[32];
                    for (uint32_t n = Memory::Read32(0x805ECC20 + kind * 0x130 + st * 8), k = 0; GuestPtr(n) && k < 32;
                         n = Memory::Read32(n), ++k) {
                        std::snprintf(b, sizeof(b), " %08X>%d", Memory::Read32(n + 8), S32(n + 0xC));
                        line += b;
                    }
                    if (!line.empty())
                        RT_LOG(RT_TAG_HLE) << "[opuc] ai: list " << kind << " state " << std::hex << st << std::dec << ":" << line << std::endl;
                }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Group& g : groups) {
        auto it = g_lastGroupFlags.find(g.addr);
        const uint32_t before = it == g_lastGroupFlags.end() ? ~0u : it->second;
        if (before == ~0u && !(g.rangeFlags & 0x7C) && !g.encountered) {
            g_lastGroupFlags[g.addr] = 0;  // a far, sleeping group: no event
        } else if (before != (g.rangeFlags & 0x7C)) {
            char b[160];
            std::snprintf(b, sizeof(b), "group %s flags %02X (centre %.1f %.1f %.1f, d %.1f; encount %.1f setup %.1f chase %.1f)",
                          g.encountered ? "out" : "waiting", g.rangeFlags & 0x7C, g.center[0], g.center[1], g.center[2],
                          g.dist, g.encount, g.setup, g.chase);
            PushEvent(g.addr, b);
            g_lastGroupFlags[g.addr] = g.rangeFlags & 0x7C;
        }
    }
    g_groups = std::move(groups);
    std::map<uint32_t, Mob> next;
    for (Mob& m : mobs) {
        auto it = g_last.find(m.addr);
        if (it == g_last.end()) {
            m.stateSince = g_frame;
            g_history[m.addr].push_back({g_frame, m.state});
            char b[192];
            std::snprintf(b, sizeof(b), "appears: enemy %d ai %d, %s at %.1f %.1f %.1f, r %.2f near %.2f atk0 %.2f/%.2f%%",
                          m.enemyId, m.aiType, MobStateName(m.state), m.pos[0], m.pos[1], m.pos[2], m.radius,
                          GuestPtr(m.aiRow) ? F32(m.aiRow) : -1.f, GuestPtr(m.aiRow) ? F32(m.aiRow + 0x24) : -1.f,
                          GuestPtr(m.aiRow) ? F32(m.aiRow + 0x28) : -1.f);
            PushEvent(m.addr, b);
        } else if (it->second.state != m.state) {
            m.stateSince = g_frame;
            auto& h = g_history[m.addr];
            h.push_back({g_frame, m.state});
            while (h.size() > 64) h.pop_front();
            char b[128];
            std::snprintf(b, sizeof(b), "%s(%X) -> %s(%X) (d %.2f, kind %d, bits %08X)", MobStateName(it->second.state),
                          it->second.state & 0xFF, MobStateName(m.state), m.state & 0xFF, m.dist, m.attackKind, m.perception);
            if (g_frame - g_condEventFrame[m.addr] > 2) PushEvent(m.addr, b);
        } else {
            m.stateSince = it->second.stateSince;
        }
        next[m.addr] = m;
    }
    for (auto& [addr, m] : g_last)
        if (!next.count(addr)) {
            PushEvent(addr, "gone");
            g_history.erase(addr);
            g_conds.erase(addr);
        }
    g_last = std::move(next);
    g_mobs = std::move(mobs);
}

OpucDev::Row Switch(std::u16string label, std::u16string desc, bool& v) {
    OpucDev::Row r;
    r.label = std::move(label);
    r.desc = std::move(desc);
    r.value = v ? u"ON" : u"OFF";
    r.rgb = v ? kOn : kOff;
    bool* p = &v;
    r.toggle = [p](const CpuContext&) { *p = !*p; };
    return r;
}

std::vector<OpucDev::Row> Rows(const CpuContext&) {
    Settings& s = g_settings;
    std::vector<OpucDev::Row> rows;
    rows.push_back(Switch(u"View", u"Not in the game: show what each enemy\u2019s AI looks at and picks.", s.view));
    rows.push_back(Switch(u"Mob rings", u"Near, attack and sidestep ranges and the 45-degree facing cone at a mob\u2019s feet; "
                          u"a ring fills while the player is inside it.", s.mobRings));
    rows.push_back(Switch(u"Group rings", u"A group\u2019s encount, setup and chase ranges; lit while the player is inside.", s.groupRings));
    rows.push_back(Switch(u"Boss rings", u"The boss or crew stage\u2019s approach, orbit, speed and evade layers, and the "
                          u"range of the attack slot being scored.", s.bossRings));
    rows.push_back(Switch(u"Target lines", u"A line from each enemy to the player, coloured by its state; the home point.", s.lines));
    rows.push_back(Switch(u"Labels", u"The state, AI type and attack kind above each enemy.", s.labels));
    rows.push_back(Switch(u"Through walls", u"Draw the rings and lines over walls (no depth test).", s.throughWalls));
    rows.push_back(Switch(u"Inspector", u"The nearest mob\u2019s state, perception, transitions and rolls.", s.inspector));
    rows.push_back(Switch(u"Planner", u"The boss or crew planner: behaviour scores, attack slot checks, stages and conditions.", s.planner));
    rows.push_back(Switch(u"Decision log", u"Every state change, condition result and roll, newest first.", s.log));
    rows.push_back(Switch(u"Timeline", u"Each enemy\u2019s state over the last 10 seconds.", s.timeline));
    return rows;
}

}  // namespace

std::shared_ptr<OpucDev::Page> AiPage() {
    auto p = std::make_shared<OpucDev::Page>();
    p->title = u"Enemy AI view";
    p->help = u"A: switch";
    p->rows = Rows;
    return p;
}

void OnRand(uint32_t value) {
    if (g_condFn) g_condRoll = 100.f * static_cast<float>(value) / 4294967296.f;
}

// --- Mob transition conditions ----------------------------------------------------------------------------------
// EnemyAction::RunTransitions(kind, enemy) 801b6d90 calls each node's cond as bool cond(r3 = enemy); kind 1 lists at
// 805ECD50 + state * 8, node {next, prev, cond, target, pickState}; the first nonzero cond wins (target -1: stay;
// a target blocked by +0xD0 is skipped). The cond pointers are mostly 4-byte thunks; names from vault
// entities/enemies.md sec. 16. chance: >= 0 the EnemyAIData offset of the roll's chance, -2 the notice kind's chance
// (+0x28 / +0x30 / +0x38 by +0xE0 bit 0x08 / 0x04 / 0x02000000, 0 meaning 100), -3 a fixed 50 %, -1 no roll.
namespace {

struct CondInfo { uint32_t addr; const char* name; int chance; };
constexpr CondInfo kConds[] = {
    {0x801B6A6C, "NoticeFromStand", -1}, {0x801B6A70, "NearHome(+84)", -1},   {0x801BA8E0, "RollChase(+48)", 0x48},
    {0x801B6A74, "BodyOverlap", -1},     {0x801B6A78, "JumpRoll", -1},        {0x801B6A84, "RollSubmerge(+68)", 0x68},
    {0x801B6A88, "RollEvade(+74)", 0x74}, {0x801B6A8C, "RollEvadeVsAttack(+78)", 0x78}, {0x801B6A90, "OutOfChase", -1},
    {0x801B6A94, "PlayerNear", -1},      {0x801B6AA0, "NoticeRandom", -2},    {0x801B6AA4, "Airborne", -1},
    {0x801B6AB0, "FrontalAttackSeen", -1}, {0x801B6ABC, "E0 bit31", -1},      {0x801B6AC8, "D8 0x04000000", -1},
    {0x801BA9B8, "CircleDone", -1},      {0x801B6AD4, "Overlap>1s", -1},      {0x801BA9F4, "AlertToNotice", -1},
    {0x801BAAB8, "AlertToLockOn", -3},   {0x801B6AD8, "InSidestepRange", -1}, {0x801B6AE4, "DoneAirborne", -1},
    {0x801B6B4C, "TransformBoss", -1},   {0x801B6B58, "MotionDone", -1},      {0x801B6BA0, "Timer0", -1},
    {0x801B6BB0, "FarFromHome(+88)", -1}, {0x801B6BB4, "HitLaunched", -1},    {0x801B6BC0, "Dead", -1},
    {0x801B6BCC, "BeastSubmerge", -1},   {0x801B6C20, "BeastAttack", -1},     {0x801B6C74, "PlayerTouching", -1},
    {0x801B6C80, "PlayerFar(2x+0C)", -1}, {0x801B6C84, "ResumeChase", -1},    {0x801B6C88, "AtHome", -1},
    {0x801B6C8C, "PlayerInGroupChase", -1}, {0x801B6C90, "PlayerNotNear", -1},   {0x801B6CA4, "DamageOver", -1},
    {0x801B6CB8, "JumpLanded", -1},      {0x801B6CE0, "JumpToAirAttack", -1}, {0x801B6CEC, "Landed", -1},
    {0x801B6CF8, "LandDone>Stand", -1},  {0x801B6CFC, "LandDone>Alert", -1},  {0x801B6D00, "LandDone>LockOn", -1},
    {0x801B6D04, "LandDone>Chase", -1},
};

const CondInfo* FindCond(uint32_t addr) {
    for (const CondInfo& c : kConds)
        if (c.addr == addr) return &c;
    return nullptr;
}

constexpr uint32_t kTransitionLists = 0x805ECD50;

int TargetOf(int state, uint32_t cond) {
    if (state < 0 || state >= 0x26) return -2;
    for (uint32_t n = Memory::Read32(kTransitionLists + state * 8), k = 0; GuestPtr(n) && k < 32; n = Memory::Read32(n), ++k)
        if (Memory::Read32(n + 8) == cond) return S32(n + 0xC);
    return -2;
}

float ChanceOf(const CondInfo& ci, uint32_t enemy, uint32_t e0) {
    if (ci.chance == -3) return 50.f;
    if (ci.chance == -1) return -1.f;
    const uint32_t ef = Deref(kEnemyFactoryPtr), rows = ef ? Deref(ef + 0x124) : 0;
    const int ai = S32(enemy + 0xB4);
    if (!GuestPtr(rows) || ai < 0 || ai >= 128) return -1.f;
    const uint32_t row = rows + ai * 0x98;
    if (ci.chance >= 0) return F32(row + ci.chance);
    const uint32_t off = (e0 & 0x08000000) ? 0x28 : (e0 & 0x04000000) ? 0x30 : (e0 & 0x02000000) ? 0x38 : 0;
    if (!off) return -1.f;
    const float v = F32(row + off);
    return off == 0x28 && v == 0.f ? 100.f : v;
}

}  // namespace

void CondHook(CpuContext* ctx, uint32_t addr) {
    if (!g_settings.view && !Trace()) {
        GuestFunctions::NextTranslation(addr)(ctx);
        return;
    }
    const uint32_t enemy = ctx->gpr[3];
    const uint32_t e0 = Memory::Read32(enemy + 0xE0);
    g_condEnemy = enemy;
    g_condFn = addr;
    g_condRoll = -1;
    GuestFunctions::NextTranslation(addr)(ctx);
    const bool result = ctx->gpr[3] != 0;
    const float roll = g_condRoll;
    g_condFn = 0;
    g_condEnemy = 0;
    const CondInfo* ci = FindCond(addr);
    const float chance = ci ? ChanceOf(*ci, enemy, e0) : -1.f;
    std::lock_guard<std::mutex> lock(g_mutex);
    CondResult& r = g_conds[enemy][addr];
    r.frame = g_frame;
    r.result = result;
    r.roll = roll;
    r.chance = chance;
    if (result) {
        const int state = StateOf(enemy), target = TargetOf(state, addr);
        if (target == -1) return;  // "stay" entries (body overlap) fire every frame
        char b[160];
        if (roll >= 0)
            std::snprintf(b, sizeof(b), "%s: %s yes, roll %.2f < %.2f%% -> %s", MobStateName(state), ci ? ci->name : "?",
                          roll, chance, MobStateName(target));
        else
            std::snprintf(b, sizeof(b), "%s: %s yes -> %s", MobStateName(state), ci ? ci->name : "?", MobStateName(target));
        PushEvent(enemy, b);
        g_condEventFrame[enemy] = g_frame;
    }
}

// --- Planner (bosses, CPU crew) -----------------------------------------------------------------------------------
// UtilAi (0x18C): +0 AiFsm*, +0xC UtilAiStage* (0x594: +0xC stage, +0x10 stage block {flags, mov*, atk*[16]}, +0x98
// recover delay, +0xA0 latched slot), +0x14 owner CharaObj, +0x18 target, ctx at +0x1C (+4 self, +8 target, +0xC
// has-target, +0x20 distance, +0x34 yaw delta, 0x10000 = 360 degrees), +0x5C best score, +0x60 best node.
// UtilAi::Tick 8000d41c per frame (CharaAi::Update 80027434); scorers f1 = score(r3 node, r4 ctx), the node's weight
// (+0x18) or 0; attack scorer nodes keep their slot at +0x1C. Running behaviour id *(*(*(UtilAi + 0) + 4) + 0x18).
namespace {

struct ScoreEntry {
    uint32_t node = 0, fn = 0, frame = 0;
    float weight = 0, score = 0;
    int slot = -1;
};

struct Planner {
    uint32_t util = 0, owner = 0, target = 0, stagePtr = 0, block = 0, bestNode = 0;
    int stage = -1, latched = -1, running = -1;
    int32_t recover = 0;
    float dist = 0, yawDeg = 0, bestScore = 0;
    bool hasTarget = false;
    std::vector<ScoreEntry> scores;
    uint32_t frame = 0;
    float pos[3] = {}, targetPos[3] = {};
    uint32_t lastPress = 0, lastPressFrame = 0;
    uint32_t conds = 0;  // UtilAiStage +0x560 cond records (0x2C each)
    int condCount = 0;   // +0x564
    float hpPct = -1;    // the owner's HP %
    int profile = -1;    // UtilAiStage +0x17C scale profile (the bit a cond row's profile mask tests)
};

std::map<uint32_t, Planner> g_planners;
uint32_t g_curUtil = 0;  // the UtilAi whose Tick is running
std::map<uint32_t, uint32_t> g_scorerCalls;  // fn -> calls (trace)
std::map<uint32_t, std::vector<ScoreEntry>> g_tickScores;
std::map<uint32_t, int32_t> g_stageSeen;  // UtilAiStage -> stage + 1 last logged

const char* ScorerName(uint32_t fn, int slot, float weight) {
    switch (fn) {
        case 0x800133A8: return "0x1000 (own move state 7)";
        case 0x800138C0: return "0x2000 phase motion";
        case 0x80011CEC: return "0 recover wait";
        case 0x80012DC0: return "0x10 approach";
        case 0x80012010: return "0x11 orbit";
        case 0x80013E94: return "0x30 wander gate";
        case 0x80014ECC: return "0x40 approach kind";
        case 0x80014190: return "0x50 evade";
        case 0x800154F0: return "0xC0 slot 4 variant";
        case 0x80015E64: return "0xD0 slot 5 variant";
        case 0x80018FD8: return weight > 0.82f ? "0x110 C follow-up 7" : "0x100 C follow-up 6";
        case 0x80019378: return "0x38 particular band";
        case 0x80019624: return "0x39 particular react";
        case 0x800181F8: {
            static char b[8][24];
            const int s = slot & 7;
            std::snprintf(b[s], sizeof(b[s]), "slot %d attack", slot);
            return b[s];
        }
        default: return "?";
    }
}

}  // namespace

void ScorerHook(CpuContext* ctx, uint32_t fn) {
    const uint32_t node = ctx->gpr[3], c = ctx->gpr[4];
    GuestFunctions::NextTranslation(fn)(ctx);
    if (!g_settings.view && !Trace()) return;
    ++g_scorerCalls[fn];
    const uint32_t util = g_curUtil ? g_curUtil : Deref(c);
    if (!GuestPtr(util)) return;
    ScoreEntry e;
    e.node = node;
    e.fn = fn;
    e.weight = F32(node + 0x18);
    e.slot = fn == 0x800181F8 ? S32(node + 0x1C) : -1;
    e.score = static_cast<float>(ctx->fpr[1].d);
    e.frame = g_frame;
    g_tickScores[util].push_back(e);
}

// The owner's HP %: PO = **(chara + 0x20), PS = **(*(PO + 0x3C) + 0x34), HP PlayStatus at PS + 0x40 (+0 int* current,
// +4 int* max) (vault entities/player-locomotion.md).
uint32_t HpStatus(uint32_t chara) {
    const uint32_t po = Deref(Deref(chara + 0x20));
    const uint32_t ps = Deref(Deref(Deref(po + 0x3C) + 0x34));
    return GuestPtr(ps) ? Deref(ps + 0x40) : 0;
}

float HpPercent(uint32_t chara, int32_t* curOut = nullptr, int32_t* maxOut = nullptr) {
    const uint32_t hp = HpStatus(chara);
    if (!GuestPtr(hp)) return -1;
    const uint32_t c = Deref(hp), m = Deref(hp + 4);
    if (!GuestPtr(c) || !GuestPtr(m)) return -1;
    const int32_t cur = S32(c), mx = S32(m);
    if (curOut) *curOut = cur;
    if (maxOut) *maxOut = mx;
    return mx > 0 ? 100.f * cur / mx : -1;
}

// A cond row (ai/{code}_cond.atb, 0x2C, the Character editor's field order): +0 profile mask, +4 stage bits
// (0xFFFFFFFF any), +8 compare kind, +0xC op, +0x10 value, +0x14 compare arg, +0x18 action, +0x1C / +0x20 / +0x24
// action args, +0x28 extra flags.
struct CondRow {
    uint32_t profile, stages, kind, op, action, arg0;
    int32_t value;
};

CondRow ReadCondRow(uint32_t at) {
    return {Memory::Read32(at), Memory::Read32(at + 4), Memory::Read32(at + 8), Memory::Read32(at + 0xC),
            Memory::Read32(at + 0x18), Memory::Read32(at + 0x1C), S32(at + 0x10)};
}

// Kind 1 (HP %) evaluated with the owner's HP now: 1 true, 0 false, -1 not evaluable here.
int EvalNow(const CondRow& r, float hpPct) {
    if (r.op == 6) return 1;
    if (r.op == 7) return 0;
    if (r.kind != 1 || hpPct < 0) return -1;
    const float v = hpPct, c = static_cast<float>(r.value);
    switch (r.op) {
        case 0: return v == c;
        case 1: return v != c;
        case 2: return v > c;
        case 3: return v >= c;
        case 4: return v < c;
        case 5: return v <= c;
        default: return -1;
    }
}

std::string CondText(const CondRow& r, int index) {
    char b[160];
    std::string stages;
    if (r.stages == 0xFFFFFFFFu) stages = "any";
    else
        for (int k = 0; k < 16; ++k)
            if (r.stages & (1u << k)) stages += (stages.empty() ? "" : ",") + std::to_string(k);
    std::snprintf(b, sizeof(b), "#%d [stage %s, profiles %X] if %s %s %d -> %s(%u)", index, stages.c_str(), r.profile,
                  CondKindName(r.kind), CondOpName(r.op), r.value, CondActionName(r.action), r.arg0);
    return b;
}

void PlannerTickHook(CpuContext* ctx) {
    const uint32_t util = ctx->gpr[3];
    const bool on = g_settings.view || Trace();
    if (on) g_tickScores[util].clear();
    g_curUtil = util;
    GuestFunctions::NextTranslation(0x8000D41C)(ctx);
    g_curUtil = 0;
    if (Trace() && (g_frame % 600) == 0) {
        std::string w;
        for (auto& [f, n] : g_scorerCalls) w += " " + std::to_string(n) + "@" + std::to_string(f);
        RT_LOG(RT_TAG_HLE) << "[opuc] ai: scorer calls" << w << std::endl;
    }
    if (!on || !GuestPtr(util)) return;
    Planner p;
    p.util = util;
    p.frame = g_frame;
    p.owner = Deref(util + 0x14);
    p.target = Deref(util + 0x18);
    const uint32_t c = util + 0x1C;
    p.hasTarget = Memory::Read8(c + 0xC) != 0;
    p.dist = F32(c + 0x20);
    p.yawDeg = static_cast<float>(S32(c + 0x34)) * 360.f / 65536.f;
    p.bestScore = F32(util + 0x5C);
    p.bestNode = Deref(util + 0x60);
    p.stagePtr = Deref(util + 0xC);
    if (GuestPtr(p.stagePtr)) {
        p.conds = Deref(p.stagePtr + 0x560);
        p.condCount = GuestPtr(p.conds) ? std::clamp(S32(p.stagePtr + 0x564), 0, 32) : 0;
        p.stage = S32(p.stagePtr + 0xC);
        p.profile = S32(p.stagePtr + 0x17C);
        p.block = Deref(p.stagePtr + 0x10);
        p.recover = S32(p.stagePtr + 0x98);
        p.latched = S32(p.stagePtr + 0xA0);
    }
    p.hpPct = HpPercent(p.owner);
    // OPUC_AI_TEST_BOSS_HP=<percent>: the boss's HP is set to that share once (PlayStatus::SetCurrent 804d8da0, so the
    // game is told), for reaching its HP conds in a test run.
    static const int testHp = [] { const char* v = std::getenv("OPUC_AI_TEST_BOSS_HP"); return v ? std::atoi(v) : 0; }();
    static std::map<uint32_t, bool> testDone;
    if (testHp > 0 && !testDone[util] && p.hasTarget && g_frame > 300) {
        int32_t cur = 0, mx = 0;
        if (HpPercent(p.owner, &cur, &mx) > testHp && mx > 0) {
            testDone[util] = true;
            const int32_t next = mx * testHp / 100;
            OpucGuest::Call(*ctx, 0x804D8DA0, {HpStatus(p.owner), static_cast<uint32_t>(next), 0});
            RT_LOG(RT_TAG_HLE) << "[opuc] ai: test: boss HP " << cur << " -> " << next << " of " << mx << std::endl;
        }
    }
    if (Trace() && !g_planners.count(util) && p.condCount) {
        for (int i = 0; i < p.condCount; ++i)
            RT_LOG(RT_TAG_HLE) << "[opuc] ai: cond row " << CondText(ReadCondRow(p.conds + i * 0x2C), i) << std::endl;
    }
    const uint32_t fsm = Deref(util), beh = Deref(fsm + 4);
    p.running = GuestPtr(beh) ? S32(beh + 0x18) : -1;
    // The global group (0x1000 / 0x2000) is scored every tick, the behaviour list only when UtilAi +0x68 is set
    // (about every third tick): each node keeps its last score.
    if (auto old = g_planners.find(util); old != g_planners.end()) p.scores = old->second.scores;
    for (const ScoreEntry& e : g_tickScores[util]) {
        auto it = std::find_if(p.scores.begin(), p.scores.end(), [&](const ScoreEntry& o) { return o.node == e.node; });
        if (it != p.scores.end()) *it = e;
        else p.scores.push_back(e);
    }
    ReadPos(p.owner, p.pos);
    if (GuestPtr(p.target)) ReadPos(p.target, p.targetPos);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_planners.find(util);
    if (it != g_planners.end()) {
        p.lastPress = it->second.lastPress;
        p.lastPressFrame = it->second.lastPressFrame;
        if (Trace() && it->second.bestNode != p.bestNode && GuestPtr(p.bestNode)) {
            std::string w;
            char t[16];
            for (uint32_t o = 0; o < 0x24; o += 4) { std::snprintf(t, sizeof(t), " %08X", Memory::Read32(p.bestNode + o)); w += t; }
            RT_LOG(RT_TAG_HLE) << "[opuc] ai: best node " << std::hex << p.bestNode << std::dec << w << std::endl;
        }
        if (it->second.running != p.running) {
            char b[128];
            std::snprintf(b, sizeof(b), "planner: %s -> %s (best %.2f, d %.2f, yaw %.0f)",
                          PlannerBehaviourName(static_cast<uint32_t>(it->second.running)),
                          PlannerBehaviourName(static_cast<uint32_t>(p.running)), p.bestScore, p.dist, p.yawDeg);
            PushEvent(p.owner, b);
        }
    } else {
        char b[96];
        std::snprintf(b, sizeof(b), "planner seen: UtilAi %08X stage %d", util, p.stage);
        PushEvent(p.owner, b);
    }
    g_planners[util] = std::move(p);
}

void PlannerEventHook(CpuContext* ctx, uint32_t fn) {
    const uint32_t r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    GuestFunctions::NextTranslation(fn)(ctx);
    if (!g_settings.view && !Trace()) return;
    char b[128];
    uint32_t who = 0;
    switch (fn) {
        case 0x800117C4:  // UtilAiStage::SetStage(stage*, n)
            if (g_stageSeen[r3] == static_cast<int32_t>(r4) + 1) return;  // the same stage again
            g_stageSeen[r3] = static_cast<int32_t>(r4) + 1;
            std::snprintf(b, sizeof(b), "stage -> %d (UtilAiStage %08X)", static_cast<int32_t>(r4), r3);
            who = r3;
            for (auto& [u, p] : g_planners) {
                if (p.stagePtr != r3) continue;
                who = p.owner;
                // the SetMoveStage cond row that leads from the old stage to this one
                const float hp = HpPercent(p.owner);
                for (int i = 0; i < p.condCount; ++i) {
                    const CondRow row = ReadCondRow(p.conds + i * 0x2C);
                    if (row.action != 4 || row.arg0 != r4) continue;
                    if (p.stage >= 0 && row.stages != 0xFFFFFFFFu && !(row.stages & (1u << (p.stage & 31)))) continue;
                    std::snprintf(b, sizeof(b), "stage %d -> %d by cond #%d: %s %s %d (HP now %.0f%%)", p.stage,
                                  static_cast<int32_t>(r4), i, CondKindName(row.kind), CondOpName(row.op), row.value, hp);
                    break;
                }
            }
            break;
        case 0x800106B8:  // UtilAi::EvalAdvCond(chara, util, eventKind, arg, entry)
            if (ctx->gpr[5] > 255) return;  // not an event kind (other call shapes)
            std::snprintf(b, sizeof(b), "cond event kind %d arg %d", static_cast<int32_t>(ctx->gpr[5]),
                          static_cast<int32_t>(ctx->gpr[6]));
            who = r3;
            break;
        case 0x80016384:  // attack Enter: r3 behaviour (+0x1C UtilAi, +0x20 slot)
        case 0x80017E48: {
            const uint32_t util = Deref(r3 + 0x1C);
            who = Deref(util + 0x14);
            std::snprintf(b, sizeof(b), "slot %d attack %s", S32(r3 + 0x20), fn == 0x80016384 ? "starts" : "ends");
            break;
        }
        default: return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    PushEvent(who, b);
}

void EmitInputHook(CpuContext* ctx) {
    const uint32_t util = ctx->gpr[3], bits = ctx->gpr[4];
    GuestFunctions::NextTranslation(0x8000EDD0)(ctx);
    if (!g_settings.view && !Trace()) return;
    const uint32_t v = GuestPtr(bits) ? Memory::Read32(bits) : 0;
    if (!v) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_planners.find(util);
    if (it == g_planners.end()) return;
    if (it->second.lastPress != v || g_frame - it->second.lastPressFrame > 30) {
        char b[64];
        std::snprintf(b, sizeof(b), "presses %s%s%s%s(%08X)", (v & 0x40) ? "C+" : "", (v & 0x10) ? "A " : "",
                      (v & 0x02) ? "Shake " : "", (v & 0x52) ? "" : "pad ", v);
        PushEvent(it->second.owner, b);
    }
    it->second.lastPress = v;
    it->second.lastPressFrame = g_frame;
}

// --- In-game marks (ALPrim3D) -------------------------------------------------------------------------------------
// ALPrim3D::CreateWithVertexCap(cap) 803a2240 binds the prim to the screen current at creation (*(0x8068B2D0));
// each frame: ClearVertices(ds) 8003355c, BeginAdd(ds, type) 803ea4e4 (type 0 = GX triangles), per vertex
// ALVertexListGC::AddVertexColorF(list, pos*, nrm*, rgba*, uv0..3*) 803dea58 on list = **(ds + 0x2C), EndAdd(ds)
// 803a2534; ds = *(node + 0x78). Node vtbl: +0xE4 SetEnable, +0x14C ZCompare, +0x150 ZUpdate, +0x154 alpha test,
// +0x160 blend, +0x178 cull, +0x138 SetAlpha(f1). Release: intf (node + 0x20) vtbl+0x10 KillDeferred. As the fishing
// line (FishingLine::RebuildPrim 80033368). AddPosition has no bounds check: never more than cap vertices.
namespace {

constexpr uint32_t kCreatePrim = 0x803A2240, kClearVertices = 0x8003355C, kBeginAdd = 0x803EA4E4,
                   kEndAdd = 0x803A2534, kAddVertexColorF = 0x803DEA58;
constexpr uint32_t kCurrentScreen = 0x8068B2D0, kCameraRoot = 0x8068A470;
constexpr uint32_t kNormal = 0x80680C40, kDummyUv = 0x8068B490;  // r13 - 0x4A10
constexpr uint32_t kPrimCap = 10000;
constexpr int kSegments = 32;
constexpr float kPi = 3.14159265f;

struct Vtx { float x, y, z; uint32_t rgba; };

struct Prim {
    uint32_t node = 0, camera = 0;
    std::vector<Vtx> verts;
};
Prim g_prim;
uint32_t g_hookFrame = ~0u;

uint32_t FieldCamera() { return Deref(Deref(Deref(kCameraRoot) + 0x14) + 0x24); }

uint32_t VCall(const CpuContext& c, uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> more = {},
               std::initializer_list<double> f = {}) {
    const uint32_t fn = Memory::Read32(Memory::Read32(obj + 4) + slot);
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), more.begin(), more.end());
    CpuContext cc = OpucGuest::MakeCallContext(c);
    for (size_t i = 0; i < a.size(); ++i) cc.gpr[3 + i] = a[i];
    uint32_t k = 1;
    for (double d : f) cc.fpr[k++].d = d;
    CpuContextScope scope(&cc);
    InvokeIndirectCpu(fn, &cc);
    return cc.gpr[3];
}

uint32_t Rgba(uint32_t rgb, uint8_t a) { return (rgb << 8) | a; }

void Tri(std::vector<Vtx>& v, const float* a, const float* b, const float* c, uint32_t col) {
    if (v.size() + 3 > kPrimCap) return;
    v.push_back({a[0], a[1], a[2], col});
    v.push_back({b[0], b[1], b[2], col});
    v.push_back({c[0], c[1], c[2], col});
}

void Quad(std::vector<Vtx>& v, const float* a, const float* b, const float* c, const float* d, uint32_t col) {
    Tri(v, a, b, c, col);
    Tri(v, a, c, d, col);
}

// A flat band around (x, y, z) between r - w / 2 and r + w / 2; the arc from a0 over `sweep` radians.
void Band(std::vector<Vtx>& v, float x, float y, float z, float r, float w, uint32_t col, float a0 = 0,
          float sweep = 2 * kPi) {
    if (r <= 0) return;
    const float ri = std::max(0.f, r - w / 2), ro = r + w / 2;
    const int n = std::max(4, static_cast<int>(kSegments * sweep / (2 * kPi)));
    for (int i = 0; i < n; ++i) {
        const float t0 = a0 + sweep * i / n, t1 = a0 + sweep * (i + 1) / n;
        const float p0[3] = {x + ri * std::sin(t0), y, z + ri * std::cos(t0)};
        const float p1[3] = {x + ro * std::sin(t0), y, z + ro * std::cos(t0)};
        const float p2[3] = {x + ro * std::sin(t1), y, z + ro * std::cos(t1)};
        const float p3[3] = {x + ri * std::sin(t1), y, z + ri * std::cos(t1)};
        Quad(v, p0, p1, p2, p3, col);
    }
}

void Fan(std::vector<Vtx>& v, float x, float y, float z, float r, uint32_t col, float a0 = 0, float sweep = 2 * kPi) {
    if (r <= 0) return;
    const int n = std::max(4, static_cast<int>(kSegments * sweep / (2 * kPi)));
    const float c[3] = {x, y, z};
    for (int i = 0; i < n; ++i) {
        const float t0 = a0 + sweep * i / n, t1 = a0 + sweep * (i + 1) / n;
        const float p0[3] = {x + r * std::sin(t0), y, z + r * std::cos(t0)};
        const float p1[3] = {x + r * std::sin(t1), y, z + r * std::cos(t1)};
        Tri(v, c, p0, p1, col);
    }
}

// A line as a cross of two ribbons (one flat, one upright), visible from any side.
void Line(std::vector<Vtx>& v, const float* a, const float* b, float w, uint32_t col) {
    const float dx = b[0] - a[0], dz = b[2] - a[2], len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-3f) return;
    const float sx = -dz / len * w / 2, sz = dx / len * w / 2;
    const float a0[3] = {a[0] + sx, a[1], a[2] + sz}, a1[3] = {a[0] - sx, a[1], a[2] - sz};
    const float b0[3] = {b[0] + sx, b[1], b[2] + sz}, b1[3] = {b[0] - sx, b[1], b[2] - sz};
    Quad(v, a0, b0, b1, a1, col);
    const float u0[3] = {a[0], a[1] + w / 2, a[2]}, u1[3] = {a[0], a[1] - w / 2, a[2]};
    const float w0[3] = {b[0], b[1] + w / 2, b[2]}, w1[3] = {b[0], b[1] - w / 2, b[2]};
    Quad(v, u0, w0, w1, u1, col);
}

// The yaw (radians, 0 = +z) a CharaObj faces: its world matrix z column (posture +0x80, row-major 3x4).
float FacingYaw(uint32_t chara) {
    const uint32_t post = Deref(Deref(Deref(chara + 0xC)) + 0x54);
    if (!GuestPtr(post)) return 0;
    return std::atan2(F32(post + 0x88), F32(post + 0xA8));
}

// Labels: camera-facing stroke text (ai_view_font.h). The field camera's view matrix (ALCamera +0x104, world to view,
// looking down -z) gives the camera's right, up and back axes in world space as its rows.
struct Cam {
    float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, back[3] = {0, 0, 1};
    bool ok = false;
};
Cam g_cam;

Cam ReadCam() {
    Cam c;
    const uint32_t cam = FieldCamera();
    if (!GuestPtr(cam)) return c;
    for (int k = 0; k < 3; ++k) {
        c.right[k] = F32(cam + 0x104 + 4 * k);
        c.up[k] = F32(cam + 0x114 + 4 * k);
        c.back[k] = F32(cam + 0x124 + 4 * k);
    }
    c.ok = true;
    return c;
}

void PlanePoint(float* out, const float* o, float x, float y, float z) {
    for (int k = 0; k < 3; ++k) out[k] = o[k] + g_cam.right[k] * x + g_cam.up[k] * y + g_cam.back[k] * z;
}

// Text centred above `anchor`, `scale` world units per font grid unit.
void Text(std::vector<Vtx>& v, const float* anchor, const std::string& text, uint32_t rgb, float scale = 0.032f) {
    if (!g_cam.ok || text.empty()) return;
    const float width = (6.f * text.size() - 2.f) * scale;
    float o[3];
    PlanePoint(o, anchor, -width / 2, 0, 0);
    float a[3], b[3], c[3], d[3];
    PlanePoint(a, o, -scale, -scale, 0);
    PlanePoint(b, o, width + scale, -scale, 0);
    PlanePoint(c, o, width + scale, 7 * scale, 0);
    PlanePoint(d, o, -scale, 7 * scale, 0);
    Quad(v, a, b, c, d, Rgba(0x101418, 0x90));
    const float w = 0.45f * scale;
    for (size_t i = 0; i < text.size(); ++i) {
        const char* g = GlyphStrokes(text[i]);
        for (; g[0] && g[1] && g[2] && g[3]; g += 4) {
            const float x0 = (6.f * i + (g[0] - '0')) * scale, y0 = (g[1] - '0') * scale;
            const float x1 = (6.f * i + (g[2] - '0')) * scale, y1 = (g[3] - '0') * scale;
            const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-5f) continue;
            const float nx = -dy / len * w, ny = dx / len * w, ex = dx / len * w * 0.5f, ey = dy / len * w * 0.5f;
            float p0[3], p1[3], p2[3], p3[3];
            PlanePoint(p0, o, x0 + nx - ex, y0 + ny - ey, 0.02f);
            PlanePoint(p1, o, x1 + nx + ex, y1 + ny + ey, 0.02f);
            PlanePoint(p2, o, x1 - nx + ex, y1 - ny + ey, 0.02f);
            PlanePoint(p3, o, x0 - nx - ex, y0 - ny - ey, 0.02f);
            Quad(v, p0, p1, p2, p3, Rgba(rgb, 0xFF));
        }
    }
}

void BuildMob(std::vector<Vtx>& v, const Mob& m, int index) {
    if (m.state == 0xB || m.state == 0xA || !GuestPtr(m.aiRow)) return;
    const float x = m.pos[0], y = m.pos[1] + 0.06f, z = m.pos[2], r = m.radius;
    const uint32_t a = m.aiRow, bits = m.perception;
    if (g_settings.mobRings) {
        struct RingDef { float radius; uint32_t bit, rgb; };
        const RingDef rings[] = {
            {F32(a + 0x00), 0x40000000, 0x9FB4C4},  // near (lock-on)
            {F32(a + 0x0C), 0x00200000, 0x6AB0FF},  // sidestep
            {F32(a + 0x24), 0x08000000, 0xFF4A4A},  // attack kind 0
            {F32(a + 0x2C), 0x04000000, 0xFF9A3C},  // kind 1
            {F32(a + 0x34), 0x02000000, 0xFFE25A},  // kind 2
        };
        for (const RingDef& d : rings) {
            if (d.radius <= 0 || d.radius > 40) continue;  // 8000-unit "always" ranges are not drawn
            const bool in = (bits & d.bit) != 0;
            Band(v, x, y, z, r + d.radius, in ? 0.2f : 0.1f, Rgba(d.rgb, in ? 0xFF : 0xB0));
            if (in) Fan(v, x, y - 0.01f, z, r + d.radius, Rgba(d.rgb, 0x30));
        }
        // the 45-degree facing cone of the attack request (EnemyObj::UpdatePerception, 8068C23C)
        const float reach = r + std::max({F32(a + 0x24), F32(a + 0x2C), F32(a + 0x34), 1.f});
        if (reach < 40) {
            const float yaw = FacingYaw(m.addr);
            Fan(v, x, y + 0.01f, z, std::min(reach, 12.f), Rgba(0xFFFFFF, (bits & 0x10000000) ? 0x50 : 0x22),
                yaw - kPi / 4, kPi / 2);
        }
    }
    if (g_settings.labels && m.dist < 30) {
        char b[48];
        if (m.attackKind >= 0 && m.attackKind <= 2)
            std::snprintf(b, sizeof(b), "%s AI%d K%d", MobStateName(m.state), m.aiType, m.attackKind);
        else
            std::snprintf(b, sizeof(b), "%s AI%d", MobStateName(m.state), m.aiType);
        const float at[3] = {x, m.pos[1] + 1.9f + 0.28f * (index % 4), z};  // staggered: mobs bunch up
        Text(v, at, b, MobStateRgb(m.state));
    }
    if (g_settings.lines && GuestPtr(g_player)) {
        const float from[3] = {x, m.pos[1] + 1.0f, z}, to[3] = {g_playerPos[0], g_playerPos[1] + 1.0f, g_playerPos[2]};
        if (m.dist < 40) Line(v, from, to, 0.05f, Rgba(MobStateRgb(m.state), 0xD0));
        const float home[3] = {m.home[0], m.home[1] + 0.06f, m.home[2]};
        Band(v, home[0], home[1], home[2], 0.35f, 0.08f, Rgba(0xFFFFFF, 0x80));
        if (bits & 0x00080000) {  // out of chase range: the leash back home
            const float h[3] = {m.home[0], m.home[1] + 0.5f, m.home[2]};
            Line(v, from, h, 0.04f, Rgba(0xFFFFFF, 0x90));
        }
    }
}

void BuildGroup(std::vector<Vtx>& v, const Group& g) {
    if (!g_settings.groupRings || !GuestPtr(g.row) || g.dist > 60) return;
    const float y = g.center[1] + 0.05f;
    struct RingDef { float radius; uint32_t bit, rgb; };
    const RingDef rings[] = {{g.encount, 0x40, 0x66CC88}, {g.setup, 0x20, 0x44AACC}, {g.chase, 0x10, 0x8866CC}};
    for (const RingDef& d : rings) {
        if (d.radius <= 0.2f) continue;
        const bool in = (g.rangeFlags & d.bit) != 0;
        Band(v, g.center[0], y, g.center[2], d.radius, in ? 0.3f : 0.15f, Rgba(d.rgb, in ? 0xFF : 0x90));
    }
}

// The boss / crew stage's mov layers (ai/{code}_mov.atb row, record 0x58: +0x04 / +0x08 orbit inner / outer, +0x0C
// approach kind, +0x10 approach angle (deg), +0x14 approach distance, +0x1C evade mode, +0x20 evade angle, +0x24
// evade distance, +0x3C / +0x40 speed near / far) in the Character editor's colours, and the latched or best attack
// slot's range band and facing cone (atk record: +0x18 / +0x1C range min / max, +0x20 facing deg).
void BuildPlanner(std::vector<Vtx>& v, const Planner& p) {
    if (!GuestPtr(p.owner)) return;
    const float x = p.pos[0], y = p.pos[1] + 0.06f, z = p.pos[2];
    const float yaw = FacingYaw(p.owner);
    if (g_settings.bossRings && GuestPtr(p.block)) {
        const uint32_t mov = Deref(p.block + 4);
        if (GuestPtr(mov)) {
            const int mode = S32(mov + 0x00), kind = S32(mov + 0x0C), evade = S32(mov + 0x1C);
            if (mode == 1 || mode == 2) {
                Band(v, x, y, z, F32(mov + 0x04), 0.08f, Rgba(0x66CC88, 0xC0));
                Band(v, x, y, z, F32(mov + 0x08), 0.08f, Rgba(0x44AA44, 0xC0));
            }
            if (kind >= 1 && kind <= 3 && F32(mov + 0x14) > 0) {
                const float d = F32(mov + 0x14);
                Band(v, x, y, z, d, 0.08f, Rgba(0xDDAA66, 0xC0));
                const float ang = S32(mov + 0x10) * kPi / 180;
                if (ang > 0) Fan(v, x, y + 0.01f, z, d, Rgba(0xDDAA66, 0x40), yaw - ang, 2 * ang);
            }
            const float sn = F32(mov + 0x3C), sf = F32(mov + 0x40);
            if (sn > 0 && sn < 60) Band(v, x, y, z, sn, 0.05f, Rgba(0x44AACC, 0xA0));
            if (sf > 0 && sf < 60) Band(v, x, y, z, sf, 0.05f, Rgba(0x3388AA, 0xA0));
            if (evade != 0 && F32(mov + 0x24) > 0) {
                const float d = F32(mov + 0x24), ang = S32(mov + 0x20) * kPi / 180;
                Band(v, x, y, z, d, 0.08f, Rgba(0x8866CC, 0xC0));
                if (ang > 0) Fan(v, x, y + 0.01f, z, d, Rgba(0x8866CC, 0x40), yaw - ang, 2 * ang);
            }
        }
        int slot = p.latched >= 0 && p.latched < 16 ? p.latched : -1;
        if (slot < 0)
            for (const ScoreEntry& e : p.scores)
                if (e.node == p.bestNode && e.slot >= 0) slot = e.slot;
        if (slot >= 0) {
            const uint32_t atk = Deref(p.block + 8 + 4 * slot);
            if (GuestPtr(atk)) {
                const float mn = F32(atk + 0x18), mx = F32(atk + 0x1C);
                const float ang = S32(atk + 0x20) * kPi / 180;
                if (mx > 0 && mx < 60) {
                    const float mid = (mn + mx) / 2, w = std::max(0.08f, mx - mn);
                    Band(v, x, y + 0.02f, z, mid, w, Rgba(0xFF4A4A, 0x50), ang > 0 ? yaw - ang : 0, ang > 0 ? 2 * ang : 2 * kPi);
                    Band(v, x, y + 0.02f, z, mx, 0.08f, Rgba(0xFF4A4A, 0xE0));
                }
            }
        }
    }
    if (g_settings.labels) {
        char b[64];
        int slot = -1;
        for (const ScoreEntry& e : p.scores)
            if (e.node == p.bestNode) slot = e.slot;
        if (slot >= 0)
            std::snprintf(b, sizeof(b), "ST%d %s S%d", p.stage, PlannerBehaviourName(static_cast<uint32_t>(p.running)), slot);
        else
            std::snprintf(b, sizeof(b), "ST%d %s", p.stage, PlannerBehaviourName(static_cast<uint32_t>(p.running)));
        const float at[3] = {x, p.pos[1] + 3.6f, z};
        Text(v, at, b, 0xFFE25A, 0.05f);
    }
    if (g_settings.lines && GuestPtr(p.target)) {
        const float from[3] = {x, p.pos[1] + 1.2f, z}, to[3] = {p.targetPos[0], p.targetPos[1] + 1.2f, p.targetPos[2]};
        uint32_t rgb = 0x9FB4C4;
        if (p.running >= 0x80 && p.running <= 0x110) rgb = 0xFF4A4A;
        else if (p.running == 0x10 || p.running == 0x11 || p.running == 0x40) rgb = 0xFF9A3C;
        else if (p.running == 0x50) rgb = 0x6AB0FF;
        Line(v, from, to, 0.07f, Rgba(rgb, 0xD0));
    }
}

void ReleasePrim(const CpuContext& c) {
    if (!g_prim.node) return;
    if (g_prim.camera == FieldCamera()) {
        uint32_t intf = Memory::Read32(g_prim.node + 0x20);
        if (!intf) {
            VCall(c, g_prim.node, 0x18);
            intf = Memory::Read32(g_prim.node + 0x20);
        }
        if (GuestPtr(intf)) {
            const uint32_t fn = Memory::Read32(Memory::Read32(intf + 4) + 0x10);
            OpucGuest::Call(c, fn, {intf});
        }
        if (Trace()) RT_LOG(RT_TAG_HLE) << "[opuc] ai: prim released" << std::endl;
    } else if (Trace()) {
        RT_LOG(RT_TAG_HLE) << "[opuc] ai: prim dropped with its field" << std::endl;
    }
    g_prim.node = 0;
}

void Flush(const CpuContext& c) {
    const uint32_t node = g_prim.node;
    const uint32_t ds = Memory::Read32(node + 0x78);
    if (!GuestPtr(ds)) return;
    VCall(c, node, 0x14C, {g_settings.throughWalls ? 0u : 1u});
    OpucGuest::Call(c, kClearVertices, {ds});
    const std::vector<Vtx>& v = g_prim.verts;
    VCall(c, node, 0xE4, {v.empty() ? 0u : 1u});
    if (v.empty()) return;
    OpucGuest::Call(c, kBeginAdd, {ds, 0});
    const uint32_t list = Deref(Deref(ds + 0x2C));
    if (!GuestPtr(list)) return;
    // One guest call per vertex; the position and colour go in scratch memory below the caller's stack.
    const uint32_t pos = OpucGuest::ScratchBelow(c, 0x40), rgba = pos + 0x10;
    CpuContext cc = OpucGuest::MakeCallContext(c);
    CpuContextScope scope(&cc);
    const CpuContext base = cc;
    const size_t n = std::min<size_t>(v.size(), kPrimCap);
    for (size_t i = 0; i < n; ++i) {
        const Vtx& t = v[i];
        Memory::WriteFloat32(pos + 0, t.x);
        Memory::WriteFloat32(pos + 4, t.y);
        Memory::WriteFloat32(pos + 8, t.z);
        Memory::WriteFloat32(rgba + 0, ((t.rgba >> 24) & 0xFF) / 255.f);
        Memory::WriteFloat32(rgba + 4, ((t.rgba >> 16) & 0xFF) / 255.f);
        Memory::WriteFloat32(rgba + 8, ((t.rgba >> 8) & 0xFF) / 255.f);
        Memory::WriteFloat32(rgba + 12, (t.rgba & 0xFF) / 255.f);
        cc = base;
        cc.gpr[3] = list;
        cc.gpr[4] = pos;
        cc.gpr[5] = kNormal;
        cc.gpr[6] = rgba;
        cc.gpr[7] = cc.gpr[8] = cc.gpr[9] = cc.gpr[10] = kDummyUv;
        InvokeIndirectCpu(kAddVertexColorF, &cc);
    }
    OpucGuest::Call(c, kEndAdd, {ds});
}

}  // namespace

// Once per frame from inside the field update (the enemy group range test and the planner tick call it): the
// snapshot, then the marks rebuilt.
void FrameHook(CpuContext* ctx) {
    const uint32_t frame = static_cast<uint32_t>(g_gxFrameCount);
    if (frame == g_hookFrame) return;
    g_hookFrame = frame;
    // OPUC_AI_TOGGLE_TEST=N: the view flips every N frames (the create / release cycle test).
    static const int toggleEvery = [] { const char* v = std::getenv("OPUC_AI_TOGGLE_TEST"); return v ? std::atoi(v) : 0; }();
    static int toggles = 0;
    if (toggleEvery > 0 && frame % toggleEvery == 0 && toggles < 40) {
        g_settings.view = !g_settings.view;
        ++toggles;
        RT_LOG(RT_TAG_HLE) << "[opuc] ai: toggle " << toggles << " view " << g_settings.view << std::endl;
    }
    const bool marks = g_settings.view && (g_settings.mobRings || g_settings.groupRings || g_settings.bossRings || g_settings.labels ||
                                           g_settings.lines);
    if (!g_settings.view && !Trace()) {
        if (g_prim.node) ReleasePrim(*ctx);
        return;
    }
    Snapshot();
    if (g_prim.node && g_prim.camera != FieldCamera()) {
        if (Trace()) RT_LOG(RT_TAG_HLE) << "[opuc] ai: field camera changed, prim dropped" << std::endl;
        g_prim.node = 0;
    }
    if (!marks) {
        if (g_prim.node) ReleasePrim(*ctx);
        return;
    }
    if (!g_prim.node) {
        const uint32_t cam = FieldCamera();
        if (!GuestPtr(cam) || Deref(Deref(kCurrentScreen) + 0x6C) != cam) return;  // not inside the field screen
        const uint32_t node = OpucGuest::Call(*ctx, kCreatePrim, {kPrimCap});
        if (!GuestPtr(node)) return;
        g_prim.node = node;
        g_prim.camera = cam;
        const uint32_t ds = Memory::Read32(node + 0x78);
        VCall(*ctx, ds, 0x14, {0xB8000});  // draw key, between the trails (0xB7000) and the fishing line (0xBD000)
        VCall(*ctx, node, 0x150, {0});     // Z write off
        VCall(*ctx, node, 0x154, {0});     // alpha test off
        VCall(*ctx, node, 0x160, {0});     // blend SRCALPHA / INVSRCALPHA
        VCall(*ctx, node, 0x178, {0});     // cull none
        VCall(*ctx, node, 0x138, {}, {1.0});
        if (Trace()) RT_LOG(RT_TAG_HLE) << "[opuc] ai: prim " << std::hex << node << " ds " << ds << std::dec << " cap " << kPrimCap << std::endl;
    }
    std::vector<Vtx>& v = g_prim.verts;
    v.clear();
    g_cam = ReadCam();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Group& g : g_groups) BuildGroup(v, g);
        for (size_t i = 0; i < g_mobs.size(); ++i) BuildMob(v, g_mobs[i], static_cast<int>(i));
        for (auto it = g_planners.begin(); it != g_planners.end();) {
            if (g_frame - it->second.frame > 120) { it = g_planners.erase(it); continue; }
            BuildPlanner(v, it->second);
            ++it;
        }
    }
    Flush(*ctx);
}

// --- Panels ---------------------------------------------------------------------------------------------------
namespace {

ImVec4 Rgb(uint32_t rgb, float a = 1) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar;

const Mob* Focus() {
    const Mob* best = nullptr;
    for (const Mob& m : g_mobs)
        if (m.state != 0xB && m.state != 0xA && (!best || m.dist < best->dist)) best = &m;
    return best;
}

void DrawInspector(const Mob& m) {
    ImGui::SetNextWindowPos(ImVec2(10, 110), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (!ImGui::Begin("AI Inspector", nullptr, kPanel)) { ImGui::End(); return; }
    ImGui::TextColored(Rgb(MobStateRgb(m.state)), "%s (0x%X)  %.1f s", MobStateName(m.state), m.state & 0xFF,
                       (g_frame - m.stateSince) / 60.f);
    ImGui::Text("enemy %d  ai %d  r %.2f  d %.2f  kind %d", m.enemyId, m.aiType, m.radius, m.dist, m.attackKind);
    ImGui::Text("timer %d ms  lockout %d  freeze %d ms  flags %08X", m.timer, m.lockout, m.freeze, m.flags);
    std::string bits;
    for (const BitName& b : kPerceptionBits)
        if (m.perception & b.bit) { if (!bits.empty()) bits += ", "; bits += b.name; }
    ImGui::TextWrapped("sees: %s", bits.empty() ? "-" : bits.c_str());
    if (GuestPtr(m.aiRow)) {
        const uint32_t a = m.aiRow;
        ImGui::Text("near %.1f  side %.1f  atk %.1f/%.2f%%  %.1f/%.2f%%  %.1f/%.2f%%  chase roll %.0f%%", F32(a + 0x00),
                    F32(a + 0x0C), F32(a + 0x24), F32(a + 0x28), F32(a + 0x2C), F32(a + 0x30), F32(a + 0x34),
                    F32(a + 0x38), F32(a + 0x48));
    }
    // The transitions out of the current state, in the game's order, with each cond's last result this frame.
    if (m.state >= 0 && m.state < 0x26) {
        ImGui::Separator();
        const auto it = g_conds.find(m.addr);
        for (uint32_t n = Memory::Read32(kTransitionLists + m.state * 8), k = 0; GuestPtr(n) && k < 16;
             n = Memory::Read32(n), ++k) {
            const uint32_t fn = Memory::Read32(n + 8);
            const int target = S32(n + 0xC);
            const CondInfo* ci = FindCond(fn);
            const CondResult* r = nullptr;
            if (it != g_conds.end())
                if (auto jt = it->second.find(fn); jt != it->second.end() && g_frame - jt->second.frame < 3) r = &jt->second;
            char tail[64] = "";
            if (r && r->roll >= 0) std::snprintf(tail, sizeof(tail), "  roll %.2f vs %.2f%%", r->roll, r->chance);
            else if (r && r->chance >= 0) std::snprintf(tail, sizeof(tail), "  (%.2f%%)", r->chance);
            const ImVec4 col = !r ? ImVec4(0.6f, 0.6f, 0.6f, 0.7f) : r->result ? Rgb(kOn) : Rgb(kOff);
            ImGui::TextColored(col, "%-22s -> %-12s %s%s", ci ? ci->name : "?", target < 0 ? "(stay)" : MobStateName(target),
                               !r ? "-" : r->result ? "YES" : "no", tail);
        }
    }
    ImGui::End();
}

void DrawPlanner(const Planner& p, float y) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10, y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    char title[32];
    std::snprintf(title, sizeof(title), "AI Planner %08X", p.util);
    if (!ImGui::Begin(title, nullptr, kPanel)) { ImGui::End(); return; }
    (void)io;
    ImGui::TextColored(Rgb(0xFFE25A), "UtilAi %08X  stage %d  running %s (0x%X)", p.util, p.stage,
                       PlannerBehaviourName(static_cast<uint32_t>(p.running)), p.running);
    ImGui::Text("target %s  d %.2f  yaw %.0f deg  recover %d  latched slot %d", p.hasTarget ? "yes" : "no", p.dist,
                p.yawDeg, p.recover, p.latched);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The stage strip (as the Character editor's stage graph): stages 0..n, the current one filled; n from the highest
    // stage a cond row names.
    {
        int top = std::max(p.stage, 0);
        for (int i = 0; i < p.condCount; ++i) {
            const CondRow r = ReadCondRow(p.conds + i * 0x2C);
            if (r.action == 4 && r.arg0 < 16) top = std::max(top, static_cast<int>(r.arg0));
            if (r.stages != 0xFFFFFFFFu)
                for (int k = 0; k < 16; ++k)
                    if (r.stages & (1u << k)) top = std::max(top, k);
        }
        const ImVec2 o = ImGui::GetCursorScreenPos();
        for (int k = 0; k <= top; ++k) {
            const ImVec2 c(o.x + 12 + 30 * k, o.y + 11);
            if (k < top) dl->AddLine(ImVec2(c.x + 9, c.y), ImVec2(c.x + 21, c.y), IM_COL32(160, 160, 160, 160), 1.5f);
            const bool cur = k == p.stage;
            dl->AddCircleFilled(c, 9, cur ? IM_COL32(80, 192, 128, 255) : IM_COL32(64, 96, 128, 200));
            char t[4];
            std::snprintf(t, sizeof(t), "%d", k);
            dl->AddText(ImVec2(c.x - 3.5f, c.y - 7), IM_COL32(255, 255, 255, 255), t);
        }
        ImGui::Dummy(ImVec2(30.f * (top + 1) + 8, 22));
        ImGui::SameLine();
        ImGui::Text("HP %.0f%%  profile %d  %d cond row(s)", p.hpPct, p.profile, p.condCount);
        for (int i = 0; i < p.condCount; ++i) {
            const CondRow r = ReadCondRow(p.conds + i * 0x2C);
            const bool here = r.stages == 0xFFFFFFFFu || (p.stage >= 0 && (r.stages & (1u << (p.stage & 31))));
            const bool profileOn = p.profile < 0 || p.profile > 31 || (r.profile & (1u << p.profile));
            const int now = EvalNow(r, p.hpPct);
            const ImVec4 col = !here || !profileOn ? ImVec4(0.6f, 0.6f, 0.6f, 0.6f) : now == 1 ? Rgb(kOn) : Rgb(0xDDDDDD);
            ImGui::TextColored(col, "%s%s%s", CondText(r, i).c_str(), now == 1 ? "  (true now)" : now == 0 ? "  (false now)" : "",
                               !profileOn ? "  (not this profile)" : !here ? "  (other stage)" : "");
        }
    }
    for (const ScoreEntry& e : p.scores) {
        const bool best = e.node == p.bestNode && e.score > 0;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float w = 120 * std::min(1.2f, e.score) / 1.2f;
        dl->AddRectFilled(ImVec2(o.x, o.y + 2), ImVec2(o.x + w, o.y + 12),
                          best ? IM_COL32(255, 226, 90, 230) : e.score > 0 ? IM_COL32(119, 170, 170, 200) : IM_COL32(80, 80, 80, 120));
        ImGui::Dummy(ImVec2(124, 14));
        ImGui::SameLine();
        std::string checks;
        if (e.slot >= 0 && GuestPtr(p.block)) {
            const uint32_t atk = Deref(p.block + 8 + 4 * e.slot);
            if (GuestPtr(atk)) {
                const float mn = F32(atk + 0x18), mx = F32(atk + 0x1C);
                const int face = S32(atk + 0x20);
                char b[160];
                std::snprintf(b, sizeof(b), "  [tgt %s] [row %s] [recover %s] [range %.1f..%.1f %s] [face %d %s]",
                              p.hasTarget ? "ok" : "NO", Memory::Read32(atk + 8) ? "ok" : "off", p.recover > 0 ? "WAIT" : "ok",
                              mn, mx, (mn > 0 && p.dist < mn) || (mx > 0 && p.dist >= mx) ? "OUT" : "ok", face,
                              face > 0 && std::fabs(p.yawDeg) > face ? "OUT" : "ok");
                checks = b;
            }
        }
        const float fade = g_frame - e.frame > 10 ? 0.6f : 1.f;  // not scored on this tick
        ImGui::TextColored(best ? Rgb(0xFFE25A) : ImVec4(1, 1, 1, (e.score > 0 ? 1 : 0.5f) * fade), "%.2f / %.2f  %s%s", e.score,
                           e.weight, ScorerName(e.fn, e.slot, e.weight), checks.c_str());
    }
    ImGui::End();
}

void DrawLog() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, 40), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (!ImGui::Begin("AI Log", nullptr, kPanel)) { ImGui::End(); return; }
    int shown = 0;
    for (auto it = g_log.rbegin(); it != g_log.rend() && shown < 12; ++it, ++shown) {
        const float age = (g_frame - it->frame) / 60.f;
        ImGui::TextColored(ImVec4(1, 1, 1, std::max(0.35f, 1 - age / 20)), "%5.1fs %05X %s%s", age, it->enemy & 0xFFFFF,
                           it->text.c_str(), it->count > 1 ? (" x" + std::to_string(it->count)).c_str() : "");
    }
    if (!shown) ImGui::TextDisabled("AI log: no decisions yet");
    ImGui::End();
}

void DrawTimeline() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 10), ImGuiCond_Always, ImVec2(0.5f, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (!ImGui::Begin("AI Timeline", nullptr, kPanel)) { ImGui::End(); return; }
    constexpr float w = 480, h = 10;
    constexpr uint32_t span = 600;  // 10 s at 60 Hz
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const Mob& m : g_mobs) {
        auto it = g_history.find(m.addr);
        if (it == g_history.end()) continue;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const auto& h2 = it->second;
        for (size_t i = 0; i < h2.size(); ++i) {
            const uint32_t from = h2[i].first, to = i + 1 < h2.size() ? h2[i + 1].first : g_frame;
            if (to + span < g_frame) continue;
            const uint32_t start = std::max(from, g_frame > span ? g_frame - span : 0u);
            const float x0 = w - (g_frame - start) * w / span, x1 = w - (g_frame - to) * w / span;
            dl->AddRectFilled(ImVec2(o.x + x0, o.y), ImVec2(o.x + x1, o.y + h),
                              ImGui::ColorConvertFloat4ToU32(Rgb(MobStateRgb(h2[i].second))));
        }
        ImGui::Dummy(ImVec2(w, h + 2));
    }
    ImGui::TextDisabled("last 10 s; grey idle, orange closing in, yellow wind-up, red attack, blue evade / air");
    ImGui::End();
}

}  // namespace

void DrawPanels() {
    if (!g_settings.view) return;
    // GXCopyDisp counts the frame before the overlay draws, so the field update of this frame ran at count - 1.
    if (static_cast<uint32_t>(g_gxFrameCount) - g_hookFrame > 1) Snapshot();  // no field update ran this frame
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_settings.inspector)
        if (const Mob* m = Focus()) DrawInspector(*m);
    if (g_settings.planner) {
        float y = g_settings.inspector && Focus() ? 420.f : 110.f;
        for (const auto& [u, p] : g_planners) {
            if (g_frame - p.frame > 120) continue;
            DrawPlanner(p, y);
            y += 90 + 16 * (p.scores.size() + p.condCount);
        }
    }
    if (g_settings.log) DrawLog();
    if (g_settings.timeline) DrawTimeline();
}

// OPUC_AI_VIEW=1 switches the view on at start (tests); OPUC_AI_VIEW=marks without the panels.
namespace {
struct EnvInit {
    EnvInit() {
        if (const char* v = std::getenv("OPUC_AI_VIEW"); v && *v && *v != '0') {
            g_settings.view = true;
            g_settings.timeline = true;
            if (std::string_view(v) == "marks")  // the world marks only (screenshots of the rings)
                g_settings.inspector = g_settings.planner = g_settings.log = g_settings.timeline = false;
        }
    }
} g_envInit;
}  // namespace

}  // namespace OpucAiView


// --- Wrappers -----------------------------------------------------------------------------------------------------
// Mob transition conds (EnemyAction::RunTransitions 801b6d90 calls them through the node pointer).
#define OPUC_AI_COND(name, addr) \
    extern "C" void name(CpuContext* ctx) { OpucAiView::CondHook(ctx, addr); }
OPUC_AI_COND(OpucAi_Cond_801B6A6C, 0x801B6A6C)
OPUC_AI_COND(OpucAi_Cond_801B6A70, 0x801B6A70)
OPUC_AI_COND(OpucAi_Cond_801BA8E0, 0x801BA8E0)
OPUC_AI_COND(OpucAi_Cond_801B6A74, 0x801B6A74)
OPUC_AI_COND(OpucAi_Cond_801B6A78, 0x801B6A78)
OPUC_AI_COND(OpucAi_Cond_801B6A84, 0x801B6A84)
OPUC_AI_COND(OpucAi_Cond_801B6A88, 0x801B6A88)
OPUC_AI_COND(OpucAi_Cond_801B6A8C, 0x801B6A8C)
OPUC_AI_COND(OpucAi_Cond_801B6A90, 0x801B6A90)
OPUC_AI_COND(OpucAi_Cond_801B6A94, 0x801B6A94)
OPUC_AI_COND(OpucAi_Cond_801B6AA0, 0x801B6AA0)
OPUC_AI_COND(OpucAi_Cond_801B6AA4, 0x801B6AA4)
OPUC_AI_COND(OpucAi_Cond_801B6AB0, 0x801B6AB0)
OPUC_AI_COND(OpucAi_Cond_801B6ABC, 0x801B6ABC)
OPUC_AI_COND(OpucAi_Cond_801B6AC8, 0x801B6AC8)
OPUC_AI_COND(OpucAi_Cond_801BA9B8, 0x801BA9B8)
OPUC_AI_COND(OpucAi_Cond_801B6AD4, 0x801B6AD4)
OPUC_AI_COND(OpucAi_Cond_801BA9F4, 0x801BA9F4)
OPUC_AI_COND(OpucAi_Cond_801BAAB8, 0x801BAAB8)
OPUC_AI_COND(OpucAi_Cond_801B6AD8, 0x801B6AD8)
OPUC_AI_COND(OpucAi_Cond_801B6AE4, 0x801B6AE4)
OPUC_AI_COND(OpucAi_Cond_801B6B4C, 0x801B6B4C)
OPUC_AI_COND(OpucAi_Cond_801B6B58, 0x801B6B58)
OPUC_AI_COND(OpucAi_Cond_801B6BA0, 0x801B6BA0)
OPUC_AI_COND(OpucAi_Cond_801B6BB0, 0x801B6BB0)
OPUC_AI_COND(OpucAi_Cond_801B6BB4, 0x801B6BB4)
OPUC_AI_COND(OpucAi_Cond_801B6BC0, 0x801B6BC0)
OPUC_AI_COND(OpucAi_Cond_801B6BCC, 0x801B6BCC)
OPUC_AI_COND(OpucAi_Cond_801B6C20, 0x801B6C20)
OPUC_AI_COND(OpucAi_Cond_801B6C74, 0x801B6C74)
OPUC_AI_COND(OpucAi_Cond_801B6C80, 0x801B6C80)
OPUC_AI_COND(OpucAi_Cond_801B6C84, 0x801B6C84)
OPUC_AI_COND(OpucAi_Cond_801B6C88, 0x801B6C88)
OPUC_AI_COND(OpucAi_Cond_801B6C8C, 0x801B6C8C)
OPUC_AI_COND(OpucAi_Cond_801B6C90, 0x801B6C90)
OPUC_AI_COND(OpucAi_Cond_801B6CA4, 0x801B6CA4)
OPUC_AI_COND(OpucAi_Cond_801B6CB8, 0x801B6CB8)
OPUC_AI_COND(OpucAi_Cond_801B6CE0, 0x801B6CE0)
OPUC_AI_COND(OpucAi_Cond_801B6CEC, 0x801B6CEC)
OPUC_AI_COND(OpucAi_Cond_801B6CF8, 0x801B6CF8)
OPUC_AI_COND(OpucAi_Cond_801B6CFC, 0x801B6CFC)
OPUC_AI_COND(OpucAi_Cond_801B6D00, 0x801B6D00)
OPUC_AI_COND(OpucAi_Cond_801B6D04, 0x801B6D04)
#undef OPUC_AI_COND
REGISTER_NATIVE_WRAPPER_AS(0x801B6A6C, OpucAi_Cond_801B6A6C, "OpucAi_Cond_801B6A6C");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A70, OpucAi_Cond_801B6A70, "OpucAi_Cond_801B6A70");
REGISTER_NATIVE_WRAPPER_AS(0x801BA8E0, OpucAi_Cond_801BA8E0, "OpucAi_Cond_801BA8E0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A74, OpucAi_Cond_801B6A74, "OpucAi_Cond_801B6A74");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A78, OpucAi_Cond_801B6A78, "OpucAi_Cond_801B6A78");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A84, OpucAi_Cond_801B6A84, "OpucAi_Cond_801B6A84");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A88, OpucAi_Cond_801B6A88, "OpucAi_Cond_801B6A88");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A8C, OpucAi_Cond_801B6A8C, "OpucAi_Cond_801B6A8C");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A90, OpucAi_Cond_801B6A90, "OpucAi_Cond_801B6A90");
REGISTER_NATIVE_WRAPPER_AS(0x801B6A94, OpucAi_Cond_801B6A94, "OpucAi_Cond_801B6A94");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AA0, OpucAi_Cond_801B6AA0, "OpucAi_Cond_801B6AA0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AA4, OpucAi_Cond_801B6AA4, "OpucAi_Cond_801B6AA4");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AB0, OpucAi_Cond_801B6AB0, "OpucAi_Cond_801B6AB0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6ABC, OpucAi_Cond_801B6ABC, "OpucAi_Cond_801B6ABC");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AC8, OpucAi_Cond_801B6AC8, "OpucAi_Cond_801B6AC8");
REGISTER_NATIVE_WRAPPER_AS(0x801BA9B8, OpucAi_Cond_801BA9B8, "OpucAi_Cond_801BA9B8");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AD4, OpucAi_Cond_801B6AD4, "OpucAi_Cond_801B6AD4");
REGISTER_NATIVE_WRAPPER_AS(0x801BA9F4, OpucAi_Cond_801BA9F4, "OpucAi_Cond_801BA9F4");
REGISTER_NATIVE_WRAPPER_AS(0x801BAAB8, OpucAi_Cond_801BAAB8, "OpucAi_Cond_801BAAB8");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AD8, OpucAi_Cond_801B6AD8, "OpucAi_Cond_801B6AD8");
REGISTER_NATIVE_WRAPPER_AS(0x801B6AE4, OpucAi_Cond_801B6AE4, "OpucAi_Cond_801B6AE4");
REGISTER_NATIVE_WRAPPER_AS(0x801B6B4C, OpucAi_Cond_801B6B4C, "OpucAi_Cond_801B6B4C");
REGISTER_NATIVE_WRAPPER_AS(0x801B6B58, OpucAi_Cond_801B6B58, "OpucAi_Cond_801B6B58");
REGISTER_NATIVE_WRAPPER_AS(0x801B6BA0, OpucAi_Cond_801B6BA0, "OpucAi_Cond_801B6BA0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6BB0, OpucAi_Cond_801B6BB0, "OpucAi_Cond_801B6BB0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6BB4, OpucAi_Cond_801B6BB4, "OpucAi_Cond_801B6BB4");
REGISTER_NATIVE_WRAPPER_AS(0x801B6BC0, OpucAi_Cond_801B6BC0, "OpucAi_Cond_801B6BC0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6BCC, OpucAi_Cond_801B6BCC, "OpucAi_Cond_801B6BCC");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C20, OpucAi_Cond_801B6C20, "OpucAi_Cond_801B6C20");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C74, OpucAi_Cond_801B6C74, "OpucAi_Cond_801B6C74");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C80, OpucAi_Cond_801B6C80, "OpucAi_Cond_801B6C80");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C84, OpucAi_Cond_801B6C84, "OpucAi_Cond_801B6C84");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C88, OpucAi_Cond_801B6C88, "OpucAi_Cond_801B6C88");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C8C, OpucAi_Cond_801B6C8C, "OpucAi_Cond_801B6C8C");
REGISTER_NATIVE_WRAPPER_AS(0x801B6C90, OpucAi_Cond_801B6C90, "OpucAi_Cond_801B6C90");
REGISTER_NATIVE_WRAPPER_AS(0x801B6CA4, OpucAi_Cond_801B6CA4, "OpucAi_Cond_801B6CA4");
REGISTER_NATIVE_WRAPPER_AS(0x801B6CB8, OpucAi_Cond_801B6CB8, "OpucAi_Cond_801B6CB8");
REGISTER_NATIVE_WRAPPER_AS(0x801B6CE0, OpucAi_Cond_801B6CE0, "OpucAi_Cond_801B6CE0");
REGISTER_NATIVE_WRAPPER_AS(0x801B6CEC, OpucAi_Cond_801B6CEC, "OpucAi_Cond_801B6CEC");
REGISTER_NATIVE_WRAPPER_AS(0x801B6CF8, OpucAi_Cond_801B6CF8, "OpucAi_Cond_801B6CF8");
REGISTER_NATIVE_WRAPPER_AS(0x801B6CFC, OpucAi_Cond_801B6CFC, "OpucAi_Cond_801B6CFC");
REGISTER_NATIVE_WRAPPER_AS(0x801B6D00, OpucAi_Cond_801B6D00, "OpucAi_Cond_801B6D00");
REGISTER_NATIVE_WRAPPER_AS(0x801B6D04, OpucAi_Cond_801B6D04, "OpucAi_Cond_801B6D04");

// The group range test 801b3998 runs for every group every field frame: the per-frame hook (snapshot, marks).
extern "C" void OpucAi_GroupRange(CpuContext* ctx) {
    GuestFunctions::NextTranslation(0x801B3998)(ctx);
    OpucAiView::FrameHook(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x801B3998, OpucAi_GroupRange, "OpucAi_GroupRange");

// Planner: UtilAi::Tick, the scorers, the stage / cond / attack / pad-input events.
extern "C" void OpucAi_PlannerTick(CpuContext* ctx) {
    OpucAiView::PlannerTickHook(ctx);
    OpucAiView::FrameHook(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8000D41C, OpucAi_PlannerTick, "OpucAi_PlannerTick");

#define OPUC_AI_SCORER(name, addr) \
    extern "C" void name(CpuContext* ctx) { OpucAiView::ScorerHook(ctx, addr); }
OPUC_AI_SCORER(OpucAi_Score_800133A8, 0x800133A8)
OPUC_AI_SCORER(OpucAi_Score_800138C0, 0x800138C0)
OPUC_AI_SCORER(OpucAi_Score_80011CEC, 0x80011CEC)
OPUC_AI_SCORER(OpucAi_Score_80012DC0, 0x80012DC0)
OPUC_AI_SCORER(OpucAi_Score_80012010, 0x80012010)
OPUC_AI_SCORER(OpucAi_Score_80013E94, 0x80013E94)
OPUC_AI_SCORER(OpucAi_Score_80014ECC, 0x80014ECC)
OPUC_AI_SCORER(OpucAi_Score_80014190, 0x80014190)
OPUC_AI_SCORER(OpucAi_Score_800181F8, 0x800181F8)
OPUC_AI_SCORER(OpucAi_Score_800154F0, 0x800154F0)
OPUC_AI_SCORER(OpucAi_Score_80015E64, 0x80015E64)
OPUC_AI_SCORER(OpucAi_Score_80018FD8, 0x80018FD8)
OPUC_AI_SCORER(OpucAi_Score_80019378, 0x80019378)
OPUC_AI_SCORER(OpucAi_Score_80019624, 0x80019624)
#undef OPUC_AI_SCORER
REGISTER_NATIVE_WRAPPER_AS(0x800133A8, OpucAi_Score_800133A8, "OpucAi_Score_800133A8");
REGISTER_NATIVE_WRAPPER_AS(0x800138C0, OpucAi_Score_800138C0, "OpucAi_Score_800138C0");
REGISTER_NATIVE_WRAPPER_AS(0x80011CEC, OpucAi_Score_80011CEC, "OpucAi_Score_80011CEC");
REGISTER_NATIVE_WRAPPER_AS(0x80012DC0, OpucAi_Score_80012DC0, "OpucAi_Score_80012DC0");
REGISTER_NATIVE_WRAPPER_AS(0x80012010, OpucAi_Score_80012010, "OpucAi_Score_80012010");
REGISTER_NATIVE_WRAPPER_AS(0x80013E94, OpucAi_Score_80013E94, "OpucAi_Score_80013E94");
REGISTER_NATIVE_WRAPPER_AS(0x80014ECC, OpucAi_Score_80014ECC, "OpucAi_Score_80014ECC");
REGISTER_NATIVE_WRAPPER_AS(0x80014190, OpucAi_Score_80014190, "OpucAi_Score_80014190");
REGISTER_NATIVE_WRAPPER_AS(0x800181F8, OpucAi_Score_800181F8, "OpucAi_Score_800181F8");
REGISTER_NATIVE_WRAPPER_AS(0x800154F0, OpucAi_Score_800154F0, "OpucAi_Score_800154F0");
REGISTER_NATIVE_WRAPPER_AS(0x80015E64, OpucAi_Score_80015E64, "OpucAi_Score_80015E64");
REGISTER_NATIVE_WRAPPER_AS(0x80018FD8, OpucAi_Score_80018FD8, "OpucAi_Score_80018FD8");
REGISTER_NATIVE_WRAPPER_AS(0x80019378, OpucAi_Score_80019378, "OpucAi_Score_80019378");
REGISTER_NATIVE_WRAPPER_AS(0x80019624, OpucAi_Score_80019624, "OpucAi_Score_80019624");

extern "C" void OpucAi_SetStage(CpuContext* ctx) { OpucAiView::PlannerEventHook(ctx, 0x800117C4); }
REGISTER_NATIVE_WRAPPER_AS(0x800117C4, OpucAi_SetStage, "OpucAi_SetStage");
extern "C" void OpucAi_EvalAdvCond(CpuContext* ctx) { OpucAiView::PlannerEventHook(ctx, 0x800106B8); }
REGISTER_NATIVE_WRAPPER_AS(0x800106B8, OpucAi_EvalAdvCond, "OpucAi_EvalAdvCond");
extern "C" void OpucAi_AttackStart(CpuContext* ctx) { OpucAiView::PlannerEventHook(ctx, 0x80016384); }
REGISTER_NATIVE_WRAPPER_AS(0x80016384, OpucAi_AttackStart, "OpucAi_AttackStart");
extern "C" void OpucAi_AttackEnd(CpuContext* ctx) { OpucAiView::PlannerEventHook(ctx, 0x80017E48); }
REGISTER_NATIVE_WRAPPER_AS(0x80017E48, OpucAi_AttackEnd, "OpucAi_AttackEnd");
extern "C" void OpucAi_EmitInput(CpuContext* ctx) { OpucAiView::EmitInputHook(ctx); }
REGISTER_NATIVE_WRAPPER_AS(0x8000EDD0, OpucAi_EmitInput, "OpucAi_EmitInput");
