// PC fishing controls, ported from the Play tab (programs/apps/play/web/play/fishing.js "Simple input" and the
// Remote input; not in the game). The game's fishing (CharaFishing::Update 80037a60, vault entities/gathering.md
// §1a) reads A and the remote's accelerometer through WiiMotion; a keyboard / mouse player gets:
//   Simple input (default): hold the cast key (left mouse) in the wind-up and a power bar swings 0 -> 1 -> 0; its
//     release plays a rod throw into the accelerometer with A held through it and let go in the follow-through, so
//     the game's own AnalyzeSwing power and cast speed formula decide the cast. The bar's top is set per rod so a
//     full bar lands 1.15 x the line length (the last part of the bar snaps the line). A tap (under 8 frames) throws
//     nothing. The hook key (right mouse) plays the 40 ms tip jerk; hold the cast key to reel; A / D (or the mouse
//     sideways) lean, W raises the rod; Space reels a line in, and in state 0 puts the rod away (D-pad Right,
//     Unequip; the game's B does nothing there).
//   Remote input ([game] fishing_remote_input): the mouse moves the remote (vertical = the tip, a fast flick casts,
//     a sharp upward tug hooks; horizontal = the lean); the hook key with A held in the wind-up plays the rod throw.
// CharaFishing is found by wrapping CharaFishing::Update and caching r3.
#include "fishing_pc.h"

#include "hle_stubs.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace OpucFishing {
namespace {

constexpr uint32_t kCharaFishingUpdate = 0x80037A60;

bool GuestPtr(uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }  // MEM1, MEM2
float F32(uint32_t at) { const uint32_t b = Memory::Read32(at); float f; std::memcpy(&f, &b, 4); return f; }

// What the controls need from the game's CharaFishing (vault entities/gathering.md §1i): +0x1C state, +0x24 step,
// +0x50 line length, +0x8C rod_prm row (+4 cast power).
struct GameState {
    bool valid = false;
    int state = -1, step = -1;
    float linelen = 0;  // line length (m)
    float rodPower = 0; // rod_prm +4 (cast power)
};

uint32_t g_fishing = 0;      // the CharaFishing whose Update ran last
uint64_t g_updateFrame = 0;  // the KPAD frame of that Update
uint64_t g_frame = 0;        // KPAD frames
uint64_t g_updateFramePrev = 0;
std::vector<std::pair<double, double>> g_bands;  // this spot's bite bands (m), for the assist HUD

GameState ReadState() {
    GameState s;
    if (!GuestPtr(g_fishing) || g_frame - g_updateFrame > 2) return s;
    try {
        s.state = static_cast<int>(Memory::Read32(g_fishing + 0x1C));
        s.step = static_cast<int>(Memory::Read32(g_fishing + 0x24));
        s.linelen = F32(g_fishing + 0x50);
        const uint32_t rodRow = Memory::Read32(g_fishing + 0x8C);
        s.rodPower = GuestPtr(rodRow) ? F32(rodRow + 4) : 10.f;
        s.valid = true;
    } catch (...) {
        s.valid = false;
    }
    return s;
}

struct Ctl {
    MotionSource motion;
    bool prevA = false, prevPhys = true;
    int charge = 0, autoRelease = 0;
    bool waitRelease = false;
    double bar = 0, castBar = -1;
    int lastState = -1, lastStep = -1;
    double effortTop = 1;   // the bar's top effort for this rod / line
    float effortFor = -1;   // the line length it was computed for
    bool tilting = false;
    double mouseLean = 0;
    int rightFrames = 0, bFrames = 0;
    uint64_t throwFrame = 0;  // the frame a Simple throw started (a tap throws nothing and gets no message)
    bool trace = false, traceInit = false;
    int autoHookWait = 0;  // frames before the auto-hook may jerk again
} g;

// The top effort: bisected so the top throw lands kSimpleOver x the line length with the fitted flight (water
// 1.5 below the feet ^[inferred: Fst's points; the Sunny's deck is ~10 above the water]).
double EffortTop(float linelen, float rodPower) {
    const double target = linelen * kSimpleOver, dy = -1.5;
    auto dist = [&](double e) { const double v = CastSpeed(SimpleCastPower(e), rodPower); return v ? FlightDistance(v, dy) : 0.0; };
    double lo = kEffortMin, hi = kEffortMax;
    if (dist(lo) >= target) hi = lo;
    else if (dist(hi) <= target) lo = hi;
    for (int k = 0; k < 24 && hi - lo > 1e-3; ++k) { const double m = (lo + hi) / 2; (dist(m) < target ? lo : hi) = m; }
    return hi;
}

// The Simple bar -> predicted landing distance table (Play tab buildCastTable): 21 steps of the bar, each the throw
// through the game's power and speed formulas and the fitted flight.
struct CastStep { double b, dist; };
std::vector<CastStep> g_castTable;
void BuildCastTable(double effortTop, float rodPower) {
    g_castTable.clear();
    for (int i = 0; i <= 20; ++i) {
        const double b = i / 20.0, v = CastSpeed(SimpleCastPower(b * effortTop), rodPower);
        g_castTable.push_back({b, v ? FlightDistance(v, -1.5) : 0.0});
    }
}
double BarOfDist(double d) {
    const auto& T = g_castTable;
    for (size_t i = 1; i < T.size(); ++i)
        if (T[i].dist >= d) {
            const CastStep &a = T[i - 1], &c = T[i];
            const double span = c.dist - a.dist;
            return a.b + (c.b - a.b) * ((d - a.dist) / (span != 0 ? span : 1));
        }
    return (!T.empty() && d <= T[0].dist) ? 0 : 1;
}
double DistOfBar(double b) {
    const auto& T = g_castTable;
    if (T.size() < 2) return 0;
    const double x = std::clamp(b, 0.0, 1.0) * (T.size() - 1);
    const size_t i = std::min(T.size() - 2, static_cast<size_t>(x));
    const CastStep &a = T[i], &c = T[i + 1];
    if (!a.dist || !c.dist) return (x - i < 0.5 ? a : c).dist;
    return a.dist + (c.dist - a.dist) * (x - i);
}

}  // namespace

DevSwitches& Dev() {
    static DevSwitches d;
    return d;
}

namespace {
std::mutex g_graphMutex;
GraphSnapshot g_graph;
}  // namespace

GraphSnapshot Graph() {
    std::lock_guard<std::mutex> lock(g_graphMutex);
    return g_graph;
}

bool Frame(const PadIn& in, int n, std::vector<V3>& samples, PadOut& out) {
    g_frame++;
    if (!g.traceInit) {
        g.traceInit = true;
        g.trace = std::getenv("OPUC_TRACE_FISHING") != nullptr;
        if (std::getenv("OPUC_FISHING_AUTOHOOK")) Dev().autoHook = true;
    }
    out = PadOut{};
    const GameState s = ReadState();
    if (g.trace && g_fishing && g_frame % 120 == 0)
        RT_LOG(RT_TAG_HLE) << "[opuc] fishing frame " << g_frame << " update " << g_updateFrame << " valid " << s.valid
                           << " state " << s.state << " step " << s.step << " cast " << in.cast << std::endl;
    if (!s.valid) {
        if (g_graph.active) { std::lock_guard<std::mutex> lock(g_graphMutex); g_graph.active = false; }
        g.prevA = false; g.charge = 0; g.autoRelease = 0; g.waitRelease = false; g.castBar = -1;
        g.lastState = -1;
        return false;
    }
    out.active = true;
    const bool simple = !RuntimeConfigFile::FishingRemoteInput(false);
    MotionSource& M = g.motion;
    if (s.linelen != g.effortFor && s.linelen > 0) {
        g.effortTop = EffortTop(s.linelen, s.rodPower);
        g.effortFor = s.linelen;
        BuildCastTable(g.effortTop, s.rodPower);
    }

    // The hook jerk (wait / fight), or in Remote input the throw with A held in the wind-up.
    const bool phys = in.cast;
    if (in.hookEdge && !M.Playing()) {
        if (!simple && s.state == 0 && s.step == 3 && phys) { M.Play(ThrowTimeline(kRemoteThrow)); g.autoRelease = ThrowFrames(kRemoteThrow); }
        else if (s.state == 3 || s.state == 4) M.Play(RodJerk());
    }
    // Auto-hook (the Dev menu's Fishing switch, or OPUC_FISHING_AUTOHOOK): the jerk as soon as a real bite dips the
    // float (CharaFishing +0x84 bBiting, set only for bite kind 2 while the dip is above 0). Once per bite: the game
    // publishes a gesture only after its pending countdown, and jerks back to back keep it pending past 28 samples,
    // where WiiMotion overwrites it with 0 (vault entities/pad-input "Publishing").
    if (g.autoHookWait > 0) g.autoHookWait--;
    if (Dev().autoHook && s.state == 3 && !M.Playing() && !g.autoHookWait && Memory::Read8(g_fishing + 0x84) && Memory::Read32(g_fishing + 0x2C) == 0)
        { M.Play(RodJerk()); g.autoHookWait = 40; if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] fishing auto-hook" << std::endl; }
    // The tilt: the move keys (A / D lean, W raise) and, in Simple input, the mouse sideways; in Remote input the
    // mouse moves the remote itself.
    if (simple) g.mouseLean = std::clamp(g.mouseLean * 0.97 + in.mouseDx * 2.5, -1.0, 1.0);
    else M.MouseMove(in.mouseDx, in.mouseDy);
    const float lx = std::clamp(in.sx + (simple ? static_cast<float>(g.mouseLean) : 0.f), -1.f, 1.f);
    const float py = std::max(in.sy, 0.f);
    const bool pushed = std::fabs(lx) > 0.1f || py > 0.1f;
    if (!M.Playing() && !M.MouseOn() && (pushed || g.tilting)) { M.Set(TiltAccel(py, lx)); g.tilting = pushed; }

    // A as the game reads it.
    bool A = phys;
    if (s.state != g.lastState || s.step != g.lastStep) {
        if (s.state == 0 && s.step == 3) { g.charge = 0; g.bar = 0; }
        if (g.trace)
            RT_LOG(RT_TAG_HLE) << "[opuc] fishing state " << s.state << " step " << s.step << " line " << s.linelen
                               << " bar " << g.bar << std::endl;
        g.lastState = s.state; g.lastStep = s.step;
    }
    if (simple && s.state == 0 && s.step == 3 && !g.autoRelease && !g.waitRelease) {
        if (phys) { g.charge++; g.bar = PingPong(static_cast<double>(g.charge) / kChargeFrames); }
        else if (g.prevPhys && g.charge >= kMinCharge) {
            const Throw t = SimpleThrow(g.bar * g.effortTop);
            M.Play(ThrowTimeline(t));
            g.autoRelease = ThrowFrames(t);
            g.castBar = g.bar;
            g.throwFrame = g_frame;
            A = true;
            if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] fishing throw bar " << g.bar << " effort " << g.bar * g.effortTop
                                            << " g " << t.g << " ms " << t.ms << std::endl;
        }
    }
    g.prevPhys = phys;
    if (g.autoRelease > 0) { A = true; if (--g.autoRelease == 0) { A = false; g.waitRelease = true; } }
    else if (g.waitRelease) { if (!phys) g.waitRelease = false; A = false; }
    if (!(s.state == 0 && (s.step == 3 || s.step == 4))) g.castBar = -1;
    g.prevA = A;
    out.A = A;

    // Space / Backspace: with the line out B reels it in; in state 0 (not throwing) D-pad Right puts the rod away.
    if (in.quitEdge) {
        if (s.state == 0 && s.step != 4) g.rightFrames = 3;
        else g.bFrames = 3;
    }
    if (g.rightFrames > 0) { out.right = true; g.rightFrames--; }
    if (g.bFrames > 0) { out.B = true; g.bFrames--; }

    M.Frame(n, samples);
    return true;
}

// The bar of this frame for the assist HUD: -1 when no bar shows.
double CurrentBar() {
    if (g.castBar >= 0) return g.castBar;
    return (g.lastState == 0 && g.lastStep == 3 && g.charge > 0) ? g.bar : -1;
}

}  // namespace OpucFishing

namespace OpucFishing {
namespace {

using OpucGuest::Call;
constexpr uint32_t kFindMenuByName = 0x8007AAAC;  // AquaDesigner::FindMenuByName(const char*)
constexpr uint32_t kWinDispInfoName = 0x805017BE; // "WinDispInfo"
constexpr uint32_t kPushLine = 0x8019BB48;       // AquaDesignerInformation::PushLine(self, fmt, rgba[4], icon)
constexpr uint32_t kRemapMsgId = 0x80102794;     // Project::RemapMsgId(id)
constexpr uint32_t kFindFishPrm = 0x800FABFC;    // ItemDataManage::FindFishPrm(self, itemId)
constexpr uint32_t kItemDataManage = 0x805E91E0;

// A line on the INFORMATION list (left), as the Play tab's assist messages: PushLine prints the text through
// ALText::PrintfW, so '%' is doubled.
void Inform(const CpuContext& c, std::u16string_view text, uint32_t rgb) {
    if (!RuntimeConfigFile::FishingAssist(true)) return;
    const uint32_t win = Call(c, kFindMenuByName, {kWinDispInfoName});
    const uint32_t info = GuestPtr(win) ? Memory::Read32(win + 0x30) : 0;  // apParts[7]
    if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] fishing: inform (win 0x" << std::hex << win << " info 0x" << info << std::dec << ")" << std::endl;
    if (!GuestPtr(info)) return;
    std::u16string t;
    for (char16_t ch : text) { if (ch == u'%') t.push_back(u'%'); t.push_back(ch); }
    const uint32_t buf = OpucGuest::ScratchBelow(c, 0x120);
    OpucGuest::WriteWide(buf, t.substr(0, 120));
    const uint32_t rgba = buf + 0x100;
    const float col[4] = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1.f};
    for (int i = 0; i < 4; ++i) { uint32_t b; std::memcpy(&b, &col[i], 4); Memory::Write32(rgba + 4 * i, b); }
    Call(c, kPushLine, {info, buf, rgba, 0});
}

// Where this spot's fish bite: the union of their distance windows (fish_prm +0x14 / +0x16, cm; both 0 = any)
// above the 5 m bite minimum, in m (Play tab fishing.js biteBands; PickFishSlot 80036254 reads the same rows).
std::vector<std::pair<double, double>> BiteBands(const CpuContext& c, uint32_t work) {
    std::vector<std::pair<double, double>> bands, u;
    const uint32_t row = Memory::Read32(work + 0xE4);
    if (!GuestPtr(row) || static_cast<int32_t>(Memory::Read32(work + 0xE0)) < 0) return u;
    for (int i = 0; i < 5; ++i) {
        const uint32_t id = Memory::Read32(row + 8 + 8 * i);
        if (!id) continue;
        const uint32_t prm = Call(c, kFindFishPrm, {kItemDataManage, Call(c, kRemapMsgId, {id})});
        if (!GuestPtr(prm)) continue;
        const double lo = Memory::Read16(prm + 0x14), range = Memory::Read16(prm + 0x16);
        if (!lo && !range) bands.push_back({5, 200});
        else bands.push_back({std::max(5.0, lo / 100), (lo + range) / 100});
    }
    std::sort(bands.begin(), bands.end());
    for (auto& b : bands) {
        if (!u.empty() && b.first <= u.back().second) u.back().second = std::max(u.back().second, b.second);
        else u.push_back(b);
    }
    return u;
}

// ---- The assist HUD (Play tab fishing.js buildAssist / assistTick / drawAssist; not in the game) ----
// Built from the game's own HUD pieces: two PlayStatus GaugeSP layouts (0xA00458: the cast power, the lean track)
// and one GaugeHP (0xA00457: the running fish's zone), each bound to a StatusGauge (0x74 bytes, here in low memory)
// as AquaDesignerCharaSelect::BindGauges 80190f9c binds them; the float uki.aig 0xA004F8 as the bite-band markers
// and the lean needle, and the rod gauge's digits (font_num_size@10 0xA004F9, point 0xA004FA, "m" 0xA004FB) as the
// predicted distance, as sprites made the way AquaDesignerFishing::BindRodNodes 801a3060 makes its own. All of it
// hangs under the fishing HUD's layout root (WinFish = CharaFishing +8, its AquaDesignerLayout +8), built in its
// screen with the menu scene pushed (AquaDesignerFishing::DataUpdateReady 801a5720), so it hides with the HUD
// (pause, catch scene) and goes with it. A DataUpdateReady with flag 0x80 rebuilds that layout and frees what hangs
// under it, so the assist is released before and rebuilt after it; BeginOrEnd(end) releases it too.
constexpr uint32_t kR13 = 0x8068FEA0;
constexpr uint32_t kLookupObjectByFileId = 0x8032D0D8;  // ALResource::LookupObjectByFileId(parent, fileId)
constexpr uint32_t kPushCurrent = 0x803B4F64, kPopCurrent = 0x803B4F94, kSetCurrentScene = 0x803B5024;
constexpr uint32_t kGaugeConstruct = 0x80184F04, kGaugeDestruct = 0x8018503C, kGaugeBind = 0x801850D0;
constexpr uint32_t kGaugeSetValueBackAndLayout = 0x80185EC0;
constexpr uint32_t kSpriteCreate = 0x803BBCB4, kSetTextureById = 0x803BAEDC, kSetCenterMode = 0x804D2540;
constexpr uint32_t kSetPosXYRef = 0x801942A0, kGetById = 0x803CAFBC, kGetFrameWidth = 0x8017C8B0;
constexpr uint32_t kSetScaleX = 0x80374120, kGetFrameHeight = 0x8017C934, kTextureGetWidth = 0x80350F98;
constexpr uint32_t kBlankTex = 0xA0FFF0;  // a transparent 8 x 8 picture served by pc_pictures.cpp's LoadById hook
constexpr uint32_t kGaugeMem = 0x80002600;  // 3 StatusGauges of 0x74, 0x80 apart (low-memory gap, see dev_menu.cpp)
constexpr uint32_t kGaugeSP = 0xA00458, kGaugeHP = 0xA00457;
constexpr uint32_t kUki = 0xA004F8, kDigits = 0xA004F9, kPoint = 0xA004FA, kMetre = 0xA004FB;
constexpr float kBarW = 200, kLeanW = 160;
constexpr float kX = 192, kY = 396;  // the gauges' roots (Play tab: X 220, Y 396, roots at X - 28)

uint32_t VCall(const CpuContext& c, uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> more = {}) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), more.begin(), more.end());
    const uint32_t fn = Memory::Read32(Memory::Read32(obj + 4) + slot);
    switch (a.size()) {
    case 1: return Call(c, fn, {a[0]});
    case 2: return Call(c, fn, {a[0], a[1]});
    case 3: return Call(c, fn, {a[0], a[1], a[2]});
    default: return Call(c, fn, {a[0], a[1], a[2], a[3]});
    }
}
uint32_t HandleOf(const CpuContext& c, uint32_t obj) {
    if (!GuestPtr(obj)) return 0;
    if (!Memory::Read32(obj + 0x20)) VCall(c, obj, 0x18);
    return Memory::Read32(obj + 0x20);
}
void Enable(const CpuContext& c, uint32_t node, bool on) { if (GuestPtr(node)) VCall(c, node, 0xE4, {on ? 1u : 0u}); }
void SetPos(const CpuContext& c, uint32_t handle, float x, float y) { if (handle) Call(c, kSetPosXYRef, {handle}, {x, y}); }
// A node property by name (node vtbl+0x48 FindProperty, prop vtbl+0x3C SetValue), as dev_menu sets "Position".
void SetProp3(const CpuContext& c, uint32_t node, const char* name, float x, float y, float z) {
    if (!GuestPtr(node)) return;
    const uint32_t scratch = OpucGuest::ScratchBelow(c, 0x40);
    uint32_t i = 0;
    for (; name[i]; ++i) Memory::Write8(scratch + i, static_cast<uint8_t>(name[i]));
    Memory::Write8(scratch + i, 0);
    const uint32_t cookie = scratch + 0x20, value = scratch + 0x28;
    Memory::Write32(cookie, 0);
    Memory::WriteFloat32(value, x);
    Memory::WriteFloat32(value + 4, y);
    Memory::WriteFloat32(value + 8, z);
    const uint32_t prop = VCall(c, node, 0x48, {scratch, cookie, 0xFFFFFFFFu});
    if (GuestPtr(prop)) Call(c, Memory::Read32(Memory::Read32(prop + 4) + 0x3C), {prop, node, value});
}
// ALNode::SetScaleX(f1, node) 80374120 (it swaps in a scalable posture when needed), then the posture's
// SetScaleY (posture vtbl+0x34), as the function after it does.
void ScaleNode(const CpuContext& c, uint32_t node, float sx, float sy) {
    if (!GuestPtr(node)) return;
    Call(c, kSetScaleX, {node}, {sx});
    const uint32_t posture = Memory::Read32(node + 0x54);
    if (GuestPtr(posture)) Call(c, Memory::Read32(Memory::Read32(posture + 4) + 0x34), {posture}, {sy});
}
void SetKey(const CpuContext& c, uint32_t node, uint32_t key) {
    const uint32_t ds = Memory::Read32(node + 0x78);
    if (!GuestPtr(ds)) return;
    const uint32_t k = OpucGuest::ScratchBelow(c, 8);
    Memory::Write32(k, key);
    VCall(c, ds, 0x14, {k});
}

struct Sprite { uint32_t h = 0, node = 0; };
struct Hud {
    uint32_t layout = 0;  // the WinFish AquaDesignerLayout the assist hangs under
    bool built = false;
    uint32_t gaugeRoot[3] = {}, gaugeNode[3] = {};
    Sprite marker[4], needle, digit[5];  // digits: tens, ones, point, tenth, metre
    float digitW[10] = {}, digitH = 0, pointW = 0, metreW = 0, ukiH = 0;
    int blinkT = -1;  // -1: the strip is hidden
    bool shown[3] = {};
} hud;

uint32_t Gauge(int i) { return kGaugeMem + 0x80u * static_cast<uint32_t>(i); }

Sprite MakeSprite(const CpuContext& c, uint32_t parent, uint32_t tex, uint32_t frame, uint32_t cx, uint32_t cy) {
    Sprite s;
    const uint32_t o = Call(c, kSpriteCreate);
    s.h = HandleOf(c, o);
    s.node = s.h ? Memory::Read32(s.h) : 0;
    if (!GuestPtr(s.node)) return {};
    SetKey(c, s.node, 0xCA0000);  // prio 0xCA0: over the rod gauge's own pieces (BindRodNodes uses 0xC30-0xC90)
    Enable(c, s.node, true);
    VCall(c, parent, 0xC0, {s.node});
    VCall(c, s.node, 0x10);
    VCall(c, s.node, 0xE0, {parent});
    Call(c, kSetTextureById, {s.h, (frame << 24) | tex, 0});
    Call(c, kSetCenterMode, {s.h, cx, cy});
    return s;
}

float FrameSize(const CpuContext& c, uint32_t tex, int frame, bool height = false) {
    const uint32_t t = Call(c, kGetById, {tex});
    if (!GuestPtr(t)) return 0;
    const float w = static_cast<float>(Call(c, height ? kGetFrameHeight : kGetFrameWidth, {t, static_cast<uint32_t>(frame)}));
    Call(c, Memory::Read32(Memory::Read32(t + 4) + 0x10), {t});  // release
    return w;
}

void HudRelease(const CpuContext& c) {
    if (!hud.built) return;
    for (int i = 0; i < 3; ++i) Call(c, kGaugeDestruct, {Gauge(i), 0});
    hud = Hud{};
}

void HudBuild(const CpuContext& c, uint32_t winfish) {
    const uint32_t layout = Memory::Read32(winfish + 8);
    if (!GuestPtr(layout)) return;
    const uint32_t rootHandle = Memory::Read32(layout + 8);
    const uint32_t parent = GuestPtr(rootHandle) ? Memory::Read32(rootHandle) : 0;
    const uint32_t screen = Memory::Read32(winfish);
    if (!GuestPtr(parent) || !GuestPtr(screen)) return;
    // The bracket of DataUpdateReady: the HUD's screen current, the menu scene on the r13-0x4BD0 stack.
    Call(c, kPushCurrent);
    Call(c, kSetCurrentScene, {Memory::Read32(screen)});
    const uint32_t depth = Memory::Read32(kR13 - 0x66C0) - 1;
    Memory::Write32(kR13 - 0x66C0, depth);
    Memory::Write32(0x8067EDB8 + depth * 4, Memory::Read32(kR13 - 0x4BD0));
    Memory::Write32(kR13 - 0x4BD0, Memory::Read32(Memory::Read32(Memory::Read32(kR13 - 0x5918) + 4)));

    const uint32_t ids[3] = {kGaugeSP, kGaugeSP, kGaugeHP};
    const float ys[3] = {kY, kY + 22, kY + 22};
    for (int i = 0; i < 3; ++i) {
        const uint32_t g = Gauge(i);
        Call(c, kGaugeConstruct, {g});
        const uint32_t h = HandleOf(c, Call(c, kLookupObjectByFileId, {0, ids[i]}));
        const uint32_t node = h ? Memory::Read32(h) : 0;
        if (!GuestPtr(node)) continue;
        VCall(c, parent, 0xC0, {node});
        VCall(c, node, 0x10);
        VCall(c, node, 0xE0, {parent});
        Call(c, kGaugeBind, {g, h, 1000, 1});  // refMax = max: the bar is exactly the length given to SetValue
        const uint32_t link = Memory::Read32(g);
        if (GuestPtr(link) && GuestPtr(Memory::Read32(link))) SetKey(c, Memory::Read32(link), 0xC8000);
        SetPos(c, h, kX, ys[i]);
        // No HP / SP plate: BASE parents the frame, so it keeps drawing and only its picture goes.
        if (const uint32_t base = VCall(c, node, 0xB4, {0x45534142u})) {  // FindNode 'BASE'
            const uint32_t bh = HandleOf(c, base);
            if (bh) Call(c, kSetTextureById, {bh, kBlankTex, 0});
        }
        hud.gaugeRoot[i] = h;
        hud.gaugeNode[i] = node;
        Enable(c, node, false);
    }
    // Markers and digits under the power gauge, the needle under the track.
    if (hud.gaugeNode[0]) {
        for (Sprite& m : hud.marker) {
            m = MakeSprite(c, hud.gaugeNode[0], kUki, 0, 1, 0);  // centre x, top y, as BODR (BindRodNodes)
            ScaleNode(c, m.node, 0.45f, 0.45f);
            Enable(c, m.node, false);
        }
        for (int i = 0; i < 5; ++i) {
            const uint32_t tex = i == 2 ? kPoint : i == 4 ? kMetre : kDigits;
            hud.digit[i] = MakeSprite(c, hud.gaugeNode[0], tex, 0, 0, 0);
            const float sc = i < 2 ? 0.8f : i == 2 ? 0.6f : i == 3 ? 0.65f : 1.f;
            ScaleNode(c, hud.digit[i].node, sc, sc);
        }
        for (int f = 0; f < 10; ++f) hud.digitW[f] = FrameSize(c, kDigits, f);
        hud.digitH = FrameSize(c, kDigits, 0, true);
        hud.pointW = FrameSize(c, kPoint, 0);
        hud.metreW = FrameSize(c, kMetre, 0);
        hud.ukiH = FrameSize(c, kUki, 0, true);
    }
    if (hud.gaugeNode[1]) {
        hud.needle = MakeSprite(c, hud.gaugeNode[1], kUki, 0, 1, 0);
        ScaleNode(c, hud.needle.node, 0.5f, 0.5f);
    }

    Memory::Write32(kR13 - 0x4BD0, Memory::Read32(0x8067EDB8 + Memory::Read32(kR13 - 0x66C0) * 4));
    Memory::Write32(kR13 - 0x66C0, Memory::Read32(kR13 - 0x66C0) + 1);
    Call(c, kPopCurrent);
    hud.layout = layout;
    hud.built = true;
    RT_LOG(RT_TAG_HLE) << "[opuc] fishing: assist HUD built (gauges " << (hud.gaugeNode[0] != 0) << (hud.gaugeNode[1] != 0)
                       << (hud.gaugeNode[2] != 0) << ", digit width " << hud.digitW[0] << ")" << std::endl;
}

// The BLK strip over the fill (WinDispInfo::PreviewSpCost 8018e924 sizes it: BLKC scale (fill - capW) / texW,
// BLKR at fill - capW + texW(BLKL)), its alpha pulsed here 0 -> 0.75 -> 0 twice in 50 frames like GaugeBlink
// 0xA00459. The clip itself is not bound: it also keys the HP / SP plate's picture, which the assist blanks.
void BlinkFill(const CpuContext& c, int gi, double frac, float barLen, float alpha) {
    const uint32_t g = Gauge(gi);
    const uint32_t hl = Memory::Read32(g + 0x2C), hc = Memory::Read32(g + 0x30), hr = Memory::Read32(g + 0x34);
    const uint32_t nl = GuestPtr(hl) ? Memory::Read32(hl) : 0, nc = GuestPtr(hc) ? Memory::Read32(hc) : 0, nr = GuestPtr(hr) ? Memory::Read32(hr) : 0;
    if (!GuestPtr(nl) || !GuestPtr(nc) || !GuestPtr(nr)) return;
    auto texW = [&](uint32_t node) {
        const uint32_t ds = Memory::Read32(node + 0x78), tex = GuestPtr(ds) ? Memory::Read32(ds + 0x14) : 0;
        const float w = GuestPtr(tex) ? static_cast<float>(Call(c, kTextureGetWidth, {tex, 0})) : 0.f;
        return w > 0 ? w : 8.f;
    };
    const float wl = texW(nl), wr = texW(nr), capW = wl + wr;
    const float fill = barLen * static_cast<float>(std::clamp(frac, 0.0, 1.0));
    const bool low = fill < capW, any = fill > 0;
    Enable(c, nl, any);
    Enable(c, nc, any && !low);
    Enable(c, nr, any);
    if (!any) return;
    Call(c, kSetScaleX, {nl}, {low ? fill / capW : 1.f});
    if (!low) Call(c, kSetScaleX, {nc}, {(fill - capW) / texW(nc)});
    const uint32_t pr = Memory::Read32(nr + 0x54);
    SetPos(c, hr, std::max(0.f, fill - capW) + wl, GuestPtr(pr) ? F32(pr + 0x14) : 0.f);
    for (uint32_t n : {nl, nc, nr}) Call(c, Memory::Read32(Memory::Read32(n + 4) + 0x138), {n}, {alpha});  // node SetAlpha
}

void GaugeValue(const CpuContext& c, int i, float len, double cur, double back) {
    const uint32_t g = Gauge(i);
    Memory::Write32(g + 0x58, 0xFFFFFFFFu);  // force the redraw (Refresh acts only when cur != shown)
    Call(c, kGaugeSetValueBackAndLayout,
         {g, static_cast<uint32_t>(std::lround(std::clamp(cur, 0.0, 1.0) * 1000)), 1000,
          static_cast<uint32_t>(std::lround(std::clamp(back, 0.0, 1.0) * 1000)), 0, 0},
         {len});
}

void Show(const CpuContext& c, int i, bool on) {
    if (!hud.gaugeNode[i] || hud.shown[i] == on) return;
    Enable(c, hud.gaugeNode[i], on);
    hud.shown[i] = on;
}

// Per frame after the game's update: values and visibility (Play tab assistTick / drawAssist).
void HudTick(const CpuContext& c, uint32_t work, int state) {
    const uint32_t winfish = Memory::Read32(work + 8);
    if (!GuestPtr(winfish)) return;
    const uint32_t layout = Memory::Read32(winfish + 8);
    if (hud.built && layout != hud.layout) hud = Hud{};  // rebuilt without us (the wrapper should prevent it)
    if (!hud.built) {
        if (!RuntimeConfigFile::FishingAssist(true) || !GuestPtr(layout)) return;
        HudBuild(c, winfish);
        if (!hud.built) return;
    }
    const bool on = RuntimeConfigFile::FishingAssist(true);
    const bool simple = !RuntimeConfigFile::FishingRemoteInput(false);
    const double bar = CurrentBar();
    const float line = F32(work + 0x50);
    const bool showPower = on && simple && state == 0 && bar >= 0;
    const bool fight = on && state == 4;
    Show(c, 0, showPower);
    Show(c, 1, fight);
    if (showPower) {
        const double lim = std::min(1.0, BarOfDist(line));
        GaugeValue(c, 0, kBarW, bar, lim < 1 ? lim : 0);
        // The bite bands this line reaches: a float above the bar where each starts.
        size_t k = 0;
        for (auto& b : g_bands) {
            if (k >= 4) break;
            if (b.first >= line) continue;
            Sprite& m = hud.marker[k++];
            Enable(c, m.node, true);
            SetPos(c, m.h, 32 + kBarW * static_cast<float>(std::min(1.0, BarOfDist(b.first))), -14 - hud.ukiH * 0.45f / 2);
        }
        for (; k < 4; ++k) Enable(c, hud.marker[k].node, false);
        // The predicted distance in the rod gauge's digits, right of the bar, centred on its height.
        const double d = std::min(99.9, DistOfBar(bar));
        const int tens = static_cast<int>(d / 10), ones = static_cast<int>(d) % 10, tenth = static_cast<int>(d * 10) % 10;
        float x = 32 + kBarW + 16;
        const float yc = 8;
        // Atlas frames (the digits) draw around their pivot, the frame centre; ALIG pictures (point, m) from their
        // top-left corner (vault menus.md §8c).
        auto place = [&](Sprite& s, int frame, float w, float sc, float dx, float y, bool show) {
            Enable(c, s.node, show);
            if (!show || !s.node) return 0.f;
            if (frame >= 0) {
                VCall(c, Memory::Read32(s.node + 0x78), 0x58, {static_cast<uint32_t>(frame)});
                SetPos(c, s.h, x + dx + w * sc / 2, y + hud.digitH * sc / 2);
            } else {
                SetPos(c, s.h, x + dx, y);
            }
            return w * sc;
        };
        x += place(hud.digit[0], tens, hud.digitW[tens], 0.8f, 0, yc - 9, tens != 0);
        x += place(hud.digit[1], ones, hud.digitW[ones], 0.8f, 0, yc - 9, true);
        x += place(hud.digit[2], -1, hud.pointW, 0.6f, -1, yc - 3, true);
        x += place(hud.digit[3], tenth, hud.digitW[tenth], 0.65f, 0, yc - 7, true);
        place(hud.digit[4], -1, hud.metreW, 1.f, 1, yc - 6, true);
        // The SP cost blink (GaugeBlink 0xA00459) over the bar while it is inside a bite band.
        bool inBand = false;
        for (auto& b : g_bands) inBand |= d >= b.first && d <= std::min(b.second, static_cast<double>(line));
        if (inBand) {
            hud.blinkT = (hud.blinkT + 1) % 50;
            const float t = (hud.blinkT % 25) / 12.5f;
            BlinkFill(c, 0, bar, kBarW, 0.75f * (1.f - std::fabs(t - 1.f)));
        } else if (hud.blinkT >= 0) {
            BlinkFill(c, 0, 0, kBarW, 0);
            hud.blinkT = -1;
        }
    }
    if (fight) {
        GaugeValue(c, 1, kLeanW, 0, 0);
        const uint32_t flt = Memory::Read32(work + 0x0C);
        const bool running = GuestPtr(flt) && Memory::Read32(flt + 0x48) == 1;
        Show(c, 2, running);
        if (running) {
            const int32_t dirI = static_cast<int32_t>(Memory::Read32(flt + 0x54));
            const float dir = (dirI == 1 || dirI == -1) ? static_cast<float>(dirI) : (F32(flt + 0x54) < 0 ? -1.f : 1.f);
            const float z0 = std::max(-1.f, dir - 0.4f), z1 = std::min(1.f, dir + 0.4f), w = (z1 - z0) / 2 * kLeanW;
            SetPos(c, hud.gaugeRoot[2], kX + (z0 + 1) / 2 * kLeanW, kY + 22);
            GaugeValue(c, 2, std::max(12.f, std::round(w)), 1, 0);
        }
        const float lean = std::clamp(F32(work + 0x60), -1.f, 1.f);
        // Above the track (the gauge pieces draw in their own pass over any child sprite), like the band markers
        // above the power bar; the Play tab centres it on the track.
        SetPos(c, hud.needle.h, 32 + (lean + 1) / 2 * kLeanW, -14 - hud.ukiH * 0.5f / 2);
        if (g.trace && g_frame % 60 == 0) {
            const uint32_t pst = hud.needle.node ? Memory::Read32(hud.needle.node + 0x54) : 0;
            RT_LOG(RT_TAG_HLE) << "[opuc] fishing needle lean " << F32(work + 0x60) << " pitch " << F32(work + 0x5C) << " node 0x" << std::hex << hud.needle.node
                               << " flags 0x" << (hud.needle.node ? Memory::Read32(hud.needle.node + 0x2C) : 0) << std::dec << " pos "
                               << (pst ? F32(pst + 0x10) : -1) << "," << (pst ? F32(pst + 0x14) : -1) << " digit flags 0x" << std::hex
                               << (hud.digit[1].node ? Memory::Read32(hud.digit[1].node + 0x2C) : 0) << std::dec << std::endl;
        }
    } else Show(c, 2, false);
}

struct Watch {
    uint32_t work = 0;
    int state = -1, step = -1, hookStep = 0, floatState = 0;
    bool reachedWater = false;
    bool told = false;
} w;

constexpr uint32_t kYellow = 0xFFE08A, kRed = 0xFF9F9F;

// After the game's update: the assist messages, from what the state machine did this frame.
void AfterUpdate(const CpuContext& c, uint32_t work) {
    const int state = static_cast<int>(Memory::Read32(work + 0x1C)), step = static_cast<int>(Memory::Read32(work + 0x24));
    const int hookStep = static_cast<int>(Memory::Read32(work + 0x2C));
    const uint32_t flt = Memory::Read32(work + 0x0C);
    const int floatState = GuestPtr(flt) ? static_cast<int>(Memory::Read32(flt + 8)) : 0;  // u32: 1 flying, 2 water, 4 ground
    if (work != w.work || g_frame - g_updateFramePrev > 30) {
        // A new fishing session: warn when no fish of this spot bites within the line.
        w = Watch{};
        w.work = work;
        const float line = F32(work + 0x50);
        const auto bands = BiteBands(c, work);
        bool reach = false;
        for (auto& b : bands) reach |= b.first < line;
        if (!bands.empty() && !reach) Inform(c, u"No fish here bite within this line (Fishing Line Upgrade at Development)", kRed);
        g_bands = bands;
    }
    const bool threw = RuntimeConfigFile::FishingRemoteInput(false) || g_frame - g.throwFrame < 60;
    if (w.state == 0 && w.step == 3 && state == 0 && step == 1 && threw) Inform(c, u"Too weak - no cast", kYellow);
    if (state == 2 && w.state != 2) Inform(c, u"The line snapped - cast shorter", kYellow);
    // A flight that ends without the lure reaching the water (float state 2) came down on the ground: the game
    // reels it back (state 1 -> 0) with the float state going 3 -> 0 (seen in Fst away from a point).
    if (w.state == 1 && state == 0 && !w.reachedWater) Inform(c, u"The float landed on the ground", kYellow);
    if (state == 1 && floatState == 2) w.reachedWater = true;
    if (state != 1) w.reachedWater = (state == 3 || state == 4);
    if (state == 1 && floatState == 2 && w.floatState != 2 && static_cast<int32_t>(Memory::Read32(work + 0xE0)) < 0)
        Inform(c, u"No fish here - cast somewhere else", kYellow);
    if (state == 3 && hookStep == 1 && w.hookStep != 1)
        Inform(c, Memory::Read32(work + 0x98) == 1 ? u"Too early - that was a nibble" : u"Nothing on the hook yet", kYellow);
    if (g.trace && state != w.state && GuestPtr(flt))
        RT_LOG(RT_TAG_HLE) << "[opuc] fishing float 0x" << std::hex << flt << ": " << Memory::Read32(flt) << " " << Memory::Read32(flt + 4) << " " << Memory::Read32(flt + 8) << " " << Memory::Read32(flt + 0xC) << std::dec << " spot " << static_cast<int32_t>(Memory::Read32(work + 0xE0)) << std::endl;
    {
        const int biting = Memory::Read8(work + 0x84), kind = static_cast<int>(Memory::Read32(work + 0x98));
        static int lastBiting = -1, lastKind = -1;
        if (g.trace && (biting != lastBiting || kind != lastKind))
            RT_LOG(RT_TAG_HLE) << "[opuc] fishing bite kind " << kind << " biting " << biting << " frame " << g_frame << std::endl;
        lastBiting = biting; lastKind = kind;
    }
    if (g.trace && (floatState != w.floatState || hookStep != w.hookStep))
        RT_LOG(RT_TAG_HLE) << "[opuc] fishing float " << floatState << " hook " << hookStep << " state " << state << std::endl;
    w.state = state; w.step = step; w.hookStep = hookStep; w.floatState = floatState;
    HudTick(c, work, state);
}

}  // namespace
}  // namespace OpucFishing

namespace OpucFishing {
namespace {

constexpr uint32_t kGetWorldTranslationPtr = 0x804CE7AC;  // CharaObj::GetWorldTranslationPtr(obj) -> f32[3]

// The 3D lure - player distance (UpdateRodControl's line check: PSVEC__Distance(lure, player)); -1 without a lure.
float LureDistance(const CpuContext& c, uint32_t work) {
    const uint32_t lure = Memory::Read32(work + 0xD4), chara = Memory::Read32(work + 0x1458);
    if (!GuestPtr(lure) || !GuestPtr(chara)) return -1;
    const uint32_t a = Call(c, kGetWorldTranslationPtr, {lure}), b = Call(c, kGetWorldTranslationPtr, {chara});
    if (!GuestPtr(a) || !GuestPtr(b)) return -1;
    const float dx = F32(a) - F32(b), dy = F32(a + 4) - F32(b + 4), dz = F32(a + 8) - F32(b + 8);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Line never breaks (Play tab fish_unbreakable): line HP (+0x40) kept at 5 % of its max (+0x44) or more, so the
// fight never reaches HP 0 (state 5); in the flight and the fight the line length (+0x50) is raised for the update
// when the lure is about to pass it, so "distance > line length" (state 2 in the flight, 5 in the fight) never
// holds. Returns the length to put back (0: untouched).
float BeforeUpdate(const CpuContext& c, uint32_t work) {
    if (!Dev().lineNeverBreaks) return 0;
    const int state = static_cast<int>(Memory::Read32(work + 0x1C));
    const float hpMax = F32(work + 0x44);
    if (state == 3 || state == 4) {
        if (hpMax > 0 && F32(work + 0x40) < 0.05f * hpMax) Memory::WriteFloat32(work + 0x40, 0.05f * hpMax);
    }
    if (state != 1 && state != 4) return 0;
    const float len = F32(work + 0x50), dist = LureDistance(c, work);
    if (len <= 0 || dist < 0.97f * len) return 0;
    Memory::WriteFloat32(work + 0x50, dist + 5.f);
    static int told = 0;
    if (g.trace && dist > len && (++told % 60) == 1)
        RT_LOG(RT_TAG_HLE) << "[opuc] fishing: line never breaks, lure " << dist << " m past the " << len << " m line (state " << state << ")" << std::endl;
    return len;
}

// The Remote graph: the game's WiiMotion (+0x130): the |acc| ring +0x1228 (60 floats, next index +0x1224), the
// gesture byte +0x1320; power and quiet as AnalyzeSwing 80025494 counts them (Play tab drawOverlay).
void SnapshotGraph(uint32_t work) {
    GraphSnapshot snap;
    snap.active = true;
    const uint32_t wm = work + 0x130;
    const uint32_t idx = Memory::Read32(wm + 0x1224) % 60;
    for (uint32_t i = 0; i < 60; ++i) snap.ring[i] = F32(wm + 0x1228 + 4 * ((idx + i) % 60));
    double ema = 0, decay = 1;
    int count = 0, quiet = 0;
    for (int k = 59; k >= 0; --k) {
        const float v = snap.ring[k];
        if (v <= 1.5f) { if (count) break; decay *= 0.9; quiet++; }
        else { count++; ema = 0.98 * ema + v; }
    }
    snap.quiet = quiet;
    snap.power = quiet < 12 ? ema * decay * 0.25 : 0;
    snap.gesture = Memory::Read8(wm + 0x1320);
    std::lock_guard<std::mutex> lock(g_graphMutex);
    g_graph = snap;
}

}  // namespace
}  // namespace OpucFishing

// CharaFishing::Update(self, chara): caches the work object for the controls, runs the game's own update, then the
// assist's messages and HUD.
extern "C" void OpucFishing_Update(CpuContext* ctx) {
    const CpuContext c = *ctx;
    const uint32_t work = ctx->gpr[3];
    if (work != OpucFishing::g_fishing)
        RT_LOG(RT_TAG_HLE) << "[opuc] fishing: CharaFishing 0x" << std::hex << work << std::dec << std::endl;
    OpucFishing::g_fishing = work;
    float restoreLen = 0;
    try {
        restoreLen = OpucFishing::BeforeUpdate(c, work);
    } catch (...) {
    }
    GuestFunctions::NextTranslation(OpucFishing::kCharaFishingUpdate)(ctx);
    if (restoreLen > 0) Memory::WriteFloat32(work + 0x50, restoreLen);
    try {
        OpucFishing::AfterUpdate(c, work);
        if (OpucFishing::Dev().remoteGraph) OpucFishing::SnapshotGraph(work);
    } catch (...) {
    }
    OpucFishing::g_updateFramePrev = OpucFishing::g_updateFrame = OpucFishing::g_frame;
}
REGISTER_NATIVE_WRAPPER_AS(0x80037A60, OpucFishing_Update, "OpucFishing_Update");

// AquaDesignerFishing::DataUpdateReady(self, data): with data flag 0x80 (+0x54) it deletes and rebuilds the HUD
// layout, which would free the assist hanging under it: release it first, HudTick builds it again.
extern "C" void OpucFishing_DataUpdateReady(CpuContext* ctx) {
    const CpuContext c = *ctx;
    const uint32_t data = ctx->gpr[4];
    try {
        if (OpucFishing::hud.built && (Memory::Read8(data + 0x54) & 0x80)) OpucFishing::HudRelease(c);
    } catch (...) {
    }
    GuestFunctions::NextTranslation(0x801A5720)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x801A5720, OpucFishing_DataUpdateReady, "OpucFishing_DataUpdateReady");

// CharaFishing::BeginOrEnd(self, chara, begin): the end deletes the HUD; the assist goes first.
extern "C" void OpucFishing_BeginOrEnd(CpuContext* ctx) {
    const CpuContext c = *ctx;
    try {
        if (ctx->gpr[5] == 0) OpucFishing::HudRelease(c);
    } catch (...) {
    }
    GuestFunctions::NextTranslation(0x80035C48)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80035C48, OpucFishing_BeginOrEnd, "OpucFishing_BeginOrEnd");
