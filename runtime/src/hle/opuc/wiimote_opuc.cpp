// OPUC virtual Wii Remote + Nunchuk at the KPAD layer.
//
// OPUC reads input only through ALPadWii::Read (803e973c, vault entities/pad-input): KPADRead(chan, buf, 16)
// (8030454c) fills KPADStatus samples (0xB0 bytes each) and WPADProbe(chan, &devType) (802c30f0) decides
// bConnected; ALPadWii::ConvertKpadToRaw (803e9074) then reads hold +0, dev_type +0x5C, the Nunchuk stick +0x60/+0x64
// and the remote / Nunchuk acceleration. Upstream (MKWii) only passes real remotes through an emulated Bluetooth
// controller, so OPUC gets a virtual remote here: channel 0 is a remote with a Nunchuk attached, channels 1-3 are
// empty. The game's WPAD/Bluetooth stack still runs translated underneath and simply sees no hardware.
//
// Sources, OR-ed together every read:
//   keyboard + mouse  the Play app's control scheme (programs/apps/shared/web/input/controller.js, its default
//             bindings, hardcoded; see kBindings below). Every action ends in Wii buttons / stick / acceleration:
//               WASD stick (CapsLock toggles walk = half tilt), Space/K jump = B, LMB/J attack = A (hold charges),
//               RMB/L shake attack = a held Remote swing, Shift tap = dodge (A + B on one frame, on release within
//               12 frames), Shift held = dash (C, -, C, then C held), Q skill 1 = C then C + A, R skill 2 = C then
//               C + shake, MMB/T lock on = Z, wheel = switch target (Z double-tap), O = Nunchuk shake (break free),
//               E/Enter = A, V = 1 (first-person look), C = - (character select), X = -, 1/2/3/4 = D-pad
//               up/left/down/right (item slots / next page), arrows = D-pad, Esc = + (pause), Tab = +,
//               Backspace = B.
//             In menus and windows ("ui": any ProjProcess state other than AdventureMain / BattleMain, or an active
//             AdvMain pause process) Esc and RMB mean B, WASD also send the D-pad and Shift is C, as in the Play app.
//             Not ported: mouse look (the Play app turns its own camera; the pad has no such input), its fishing
//             simulation keys and its viewer-only keys. Input is ignored while the runtime overlay blocks input.
//   gamepad   first SDL gamepad: South=A East=B West=1 North=2 Start=+ Back=- Guide=HOME, D-pad, left stick,
//             left trigger=Z left shoulder=C, right shoulder=shake
//   script    OPUC_INPUT_SCRIPT="<sec>:<buttons>[:<dur>],..." with buttons joined by '+', e.g. "8:A:0.2,12:A+B"
//             (seconds since the first KPADRead; default duration 0.15 s). Stick: SX=-1..1 / SY=-1..1 tokens.
//             "S<hex state>@<sec>:<buttons>[:<dur>]" counts from the first read that sees ProjProcess in that
//             state, e.g. "S1@8.3:A" presses A 8.3 s after the title state (1) starts. It fires only while the
//             game is still in that state. Key tokens "KEY_<SDL scancode name>" / "MOUSE_LEFT|MIDDLE|RIGHT" /
//             "WHEEL" press keyboard / mouse inputs through the scheme above (for tests), e.g. "S3@40:KEY_LSHIFT:0.1".
//   OPUC_TRACE_INPUT=1 logs every change of the written hold word, the ui flag and shake pulses.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "input_bindings.h"
#include "memory.h"
#include "runtime_log.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

// KPAD hold bits (vault entities/pad-input, ConvertKpadToRaw)
constexpr uint32_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kPlus = 0x0010;
constexpr uint32_t k2 = 0x0100, k1 = 0x0200, kB = 0x0400, kA = 0x0800, kMinus = 0x1000, kZ = 0x2000, kC = 0x4000,
                   kHome = 0x8000;
constexpr uint32_t kKpadStatusSize = 0xB0;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------------------------
// Keyboard + mouse: the Play app's VirtualController (controller.js), default bindings only.
// ---------------------------------------------------------------------------------------------------------------
enum Act : uint8_t {
    MoveUp, MoveDown, MoveLeft, MoveRight, WalkToggle, Jump, Dodge, Attack, Shake, Skill1, Skill2, LockOn, TargetNext,
    Struggle, Interact, Look, CharSelect, Item1, Item2, Item3, ItemPage, DpadUp, DpadDown, DpadLeft, DpadRight, Pause,
    Menu, Back, MinusBtn, kActCount
};

// Input codes: SDL scancodes, then the mouse buttons and the wheel after SDL_SCANCODE_COUNT.
constexpr int kMouseLeft = SDL_SCANCODE_COUNT, kMouseMiddle = SDL_SCANCODE_COUNT + 1,
              kMouseRight = SDL_SCANCODE_COUNT + 2, kWheel = SDL_SCANCODE_COUNT + 3, kCodeCount = SDL_SCANCODE_COUNT + 4;

struct Binding { Act act; int code; };
// controller.js ACTIONS defaults (KeyboardEvent.code -> SDL scancode; Mouse0/1/2 = left/middle/right).
constexpr Binding kBindings[] = {
    {MoveUp, SDL_SCANCODE_W}, {MoveDown, SDL_SCANCODE_S}, {MoveLeft, SDL_SCANCODE_A}, {MoveRight, SDL_SCANCODE_D},
    {WalkToggle, SDL_SCANCODE_CAPSLOCK},
    {Jump, SDL_SCANCODE_SPACE}, {Jump, SDL_SCANCODE_K},
    {Dodge, SDL_SCANCODE_LSHIFT}, {Dodge, SDL_SCANCODE_RSHIFT},
    {Attack, kMouseLeft}, {Attack, SDL_SCANCODE_J},
    {Shake, kMouseRight}, {Shake, SDL_SCANCODE_L},
    {Skill1, SDL_SCANCODE_Q}, {Skill2, SDL_SCANCODE_R},
    {LockOn, kMouseMiddle}, {LockOn, SDL_SCANCODE_T},
    {TargetNext, kWheel},
    {Struggle, SDL_SCANCODE_O},
    {Interact, SDL_SCANCODE_E}, {Interact, SDL_SCANCODE_RETURN},
    {Look, SDL_SCANCODE_V},
    {CharSelect, SDL_SCANCODE_C},
    {Item1, SDL_SCANCODE_1}, {Item2, SDL_SCANCODE_2}, {Item3, SDL_SCANCODE_3}, {ItemPage, SDL_SCANCODE_4},
    {DpadUp, SDL_SCANCODE_UP}, {DpadDown, SDL_SCANCODE_DOWN}, {DpadLeft, SDL_SCANCODE_LEFT},
    {DpadRight, SDL_SCANCODE_RIGHT},
    {Pause, SDL_SCANCODE_ESCAPE}, {Menu, SDL_SCANCODE_TAB}, {Back, SDL_SCANCODE_BACKSPACE},
    {MinusBtn, SDL_SCANCODE_X},
};
constexpr int kDodgeTapFrames = 12;   // <= 200 ms counts as a tap (dodge on release); longer starts the dash

struct Macro { bool A = false, B = false, C = false, Z = false, PLUS = false, shake = false; };
// Macro from a button list, e.g. Mac("CA") = C + A; letters A B C Z P (= +) S (= Remote shake).
Macro Mac(const char* bits) {
    Macro m;
    for (const char* p = bits; *p; ++p) {
        switch (*p) {
        case 'A': m.A = true; break; case 'B': m.B = true; break; case 'C': m.C = true; break;
        case 'Z': m.Z = true; break; case 'P': m.PLUS = true; break; case 'S': m.shake = true; break;
        default: break;
        }
    }
    return m;
}

struct Keyboard {
    std::mutex m;
    std::array<bool, kCodeCount> held{};
    std::array<bool, kCodeCount> latched{};   // pressed since the last sample: a tap shorter than a frame counts once
    std::array<std::vector<Act>, kCodeCount> acts{};
    std::deque<Macro> queue;
    int wheel = 0;
    bool walk = false;
    struct { bool on = false; int frames = 0; bool dash = false; int hold = 0; } dodge;
    uint64_t frame = 0;
    std::atomic<bool> ui{true};
    bool watching = false;
} kb;

bool ActHeldLocked(Act a) {
    for (const Binding& b : kBindings)
        if (b.act == a && (kb.held[b.code] || kb.latched[b.code])) return true;
    return false;
}

void PressLocked(int code) {
    if (code < 0 || code >= kCodeCount || kb.held[code]) return;
    kb.held[code] = true;
    kb.latched[code] = true;
    const bool ui = kb.ui.load(std::memory_order_relaxed);
    for (Act a : kb.acts[code]) {
        switch (a) {
        case WalkToggle: kb.walk = !kb.walk; break;
        case Skill1: kb.queue.insert(kb.queue.end(), {Mac("C"), Mac("CA"), Mac("CA"), Mac("C")}); break;
        case Skill2: kb.queue.insert(kb.queue.end(), {Mac("C"), Mac("CS"), Mac("C"), Mac("C")}); break;
        case Pause: kb.queue.insert(kb.queue.end(), 2, ui ? Mac("B") : Mac("P")); break;
        case Menu: kb.queue.insert(kb.queue.end(), 2, Mac("P")); break;
        case Dodge: kb.dodge.on = true; kb.dodge.frames = 0; kb.dodge.dash = false; break;
        default: break;
        }
    }
}

void ReleaseLocked(int code) {
    if (code < 0 || code >= kCodeCount || !kb.held[code]) return;
    kb.held[code] = false;
    bool dodgeKey = false;
    for (Act a : kb.acts[code]) dodgeKey |= a == Dodge;
    if (dodgeKey && kb.dodge.on) {
        bool stillHeld = false;
        for (const Binding& b : kBindings) stillHeld |= b.act == Dodge && kb.held[b.code];
        if (!stillHeld) {
            // Tap: A + B on the same frame (the evade trigger, EvadeCanEnter 80052510). Hold: the dash ends with C.
            if (!kb.dodge.dash && !kb.ui.load(std::memory_order_relaxed)) {
                kb.queue.push_front(Mac("AB"));
                kb.queue.push_front(Mac("AB"));
            }
            kb.dodge.on = false;
        }
    }
}

void ClearLocked() {
    kb.held.fill(false);
    kb.latched.fill(false);
    kb.dodge.on = false;
}

int MouseCode(uint8_t button) {
    return button == SDL_BUTTON_LEFT ? kMouseLeft : button == SDL_BUTTON_MIDDLE ? kMouseMiddle
         : button == SDL_BUTTON_RIGHT ? kMouseRight : -1;
}

// Runs on the thread that pumps SDL events (aurora's SDL_PollEvent). Key repeats are ignored, as in the Play app.
bool SDLCALL EventWatch(void*, SDL_Event* e) {
    if (InputBindings::InputBlocked()) {
        // The runtime overlay owns input; drop what is held so nothing sticks when it closes.
        std::lock_guard<std::mutex> l(kb.m);
        ClearLocked();
        return true;
    }
    std::lock_guard<std::mutex> l(kb.m);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: if (!e->key.repeat) PressLocked(e->key.scancode); break;
    case SDL_EVENT_KEY_UP: ReleaseLocked(e->key.scancode); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: PressLocked(MouseCode(e->button.button)); break;
    case SDL_EVENT_MOUSE_BUTTON_UP: ReleaseLocked(MouseCode(e->button.button)); break;
    case SDL_EVENT_MOUSE_WHEEL: if (e->wheel.y != 0) kb.wheel++; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: ClearLocked(); break;
    default: break;
    }
    return true;
}

void InitKeyboard() {
    for (const Binding& b : kBindings) kb.acts[b.code].push_back(b.act);
    kb.watching = SDL_AddEventWatch(EventWatch, nullptr);
    if (!kb.watching)
        RT_LOG(RT_TAG_HLE) << "[opuc] keyboard/mouse scheme: SDL_AddEventWatch failed: " << SDL_GetError() << std::endl;
}

// ui (controller.js context().ui): menus and windows. Everything but AdventureMain (3) / BattleMain (2) is a menu
// or a screen with no field; inside them an active AdvMain pause process (menus, talk windows, scenes) is one too.
// AdvMain = *(0x8068A120) (r13 - 0x5D80); pause records at AdvMain+0x20, next at +0, active at +0x34
// (AdvMain::ActivateNextPauseProcess 800084cc; vault entities/events "Pause process").
bool GameUi(uint32_t projState) {
    if (projState != 2 && projState != 3) return true;
    auto ok = [](uint32_t p) { return p >= 0x80000000u && p < 0x81800000u; };
    try {
        const uint32_t adv = Memory::Read32(0x8068A120u);
        if (!ok(adv)) return true;   // AdvMain not built yet: NOW LOADING
        uint32_t rec = Memory::Read32(adv + 0x20u);
        for (int n = 0; ok(rec) && n < 64; ++n, rec = Memory::Read32(rec))
            if (Memory::Read32(rec + 0x34u) != 0) return true;
        return false;
    } catch (...) {
        return true;
    }
}

struct KbSample {
    uint32_t hold = 0;
    float sx = 0, sy = 0;
    bool shake = false, shakePulse = false, nunchukShake = false;
};

// controller.js read(): one sample per 60 Hz game frame.
KbSample ReadKeyboard(bool ui, const std::array<bool, kCodeCount>& scriptKeys, int scriptWheel) {
    std::lock_guard<std::mutex> l(kb.m);
    kb.ui.store(ui, std::memory_order_relaxed);
    // Script key presses behave like real key events (edges included).
    static std::array<bool, kCodeCount> lastScript{};
    for (int c = 0; c < kCodeCount; ++c) {
        if (scriptKeys[c] && !lastScript[c]) PressLocked(c);
        if (!scriptKeys[c] && lastScript[c]) ReleaseLocked(c);
    }
    lastScript = scriptKeys;
    kb.wheel += scriptWheel;
    auto h = [](Act a) { return ActHeldLocked(a); };

    // Dash: held past the tap window -> C, release, C held (a double-tap within 12 frames, DashCanEnter 800511fc).
    bool dashC = false;
    if (kb.dodge.on) {
        kb.dodge.frames++;
        if (!kb.dodge.dash && kb.dodge.frames > kDodgeTapFrames && !ui) {
            kb.dodge.dash = true;
            kb.queue.insert(kb.queue.end(), {Mac("C"), Macro{}, Mac("C")});
            kb.dodge.hold = 3;
        }
        if (kb.dodge.dash) { if (kb.dodge.hold > 0) kb.dodge.hold--; else dashC = true; }
    }
    // Switch target: a Z double-tap (LockOn 12-frame window). The Play app sends it only while locked on; the native
    // build has no lock-on flag here, so it is sent whenever the field has control.
    if (kb.wheel > 0) {
        if (!ui) kb.queue.insert(kb.queue.end(), {Mac("Z"), Macro{}, Mac("Z"), Macro{}});
        kb.wheel = 0;
    }
    Macro m;
    if (!kb.queue.empty()) { m = kb.queue.front(); kb.queue.pop_front(); }

    const bool rmb = kb.held[kMouseRight] || kb.latched[kMouseRight];
    const bool mouseShake = h(Shake) && !(ui && rmb);
    KbSample s;
    if (h(Attack) || h(Interact) || m.A) s.hold |= kA;
    if (h(Jump) || h(Back) || (ui && rmb) || m.B) s.hold |= kB;
    if (dashC || m.C || (ui && h(Dodge))) s.hold |= kC;   // in menus the dodge key is C (Arrange, Zoom Map)
    if (h(LockOn) || m.Z) s.hold |= kZ;
    if (h(Look)) s.hold |= k1;
    if (m.PLUS) s.hold |= kPlus;
    if (h(MinusBtn) || h(CharSelect)) s.hold |= kMinus;
    // In menus and windows the movement keys also send the D-pad.
    if (h(DpadUp) || h(Item1) || (ui && h(MoveUp))) s.hold |= kUp;
    if (h(DpadLeft) || h(Item2) || (ui && h(MoveLeft))) s.hold |= kLeft;
    if (h(DpadDown) || h(Item3) || (ui && h(MoveDown))) s.hold |= kDown;
    if (h(DpadRight) || h(ItemPage) || (ui && h(MoveRight))) s.hold |= kRight;

    float x = (h(MoveRight) ? 1.f : 0.f) - (h(MoveLeft) ? 1.f : 0.f);
    float y = (h(MoveUp) ? 1.f : 0.f) - (h(MoveDown) ? 1.f : 0.f);
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.f) { x /= len; y /= len; }
    if (kb.walk) { x *= 0.5f; y *= 0.5f; }   // inside the 0.2-0.82 band (walk, lock-on strafe)
    s.sx = x;
    s.sy = y;
    s.shake = mouseShake && !ui;
    s.shakePulse = m.shake;
    s.nunchukShake = h(Struggle);
    kb.frame++;
    kb.latched.fill(false);
    return s;
}

// Play app accelerations are in g in its remote frame (+y = pointing direction, +z = top face, rest (0, 0, 1)).
// KPAD's frame reads (0, -1, 0) for a remote held level and pointing at the screen, and a forward swing on z
// (vault entities/pad-input, WiiMotion), so KPAD = (x, -z, -y).
struct Vec3 { float x, y, z; };
Vec3 PlayToKpad(float x, float y, float z) { return {x, -z, -y}; }

// Held shake: a 6 Hz swing; the game counts a shake when |acc| first reaches 3.0 (GWPad::Update), so a held key
// repeats it. A macro shake is one frame above the threshold.
Vec3 RemoteAccel(bool shaking, bool pulse, uint64_t frame) {
    const double t = static_cast<double>(frame) / 60.0;
    if (pulse) return PlayToKpad(0.f, 3.6f, 1.f);
    if (!shaking) return PlayToKpad(0.f, 0.f, 1.f);
    const float sw = static_cast<float>(std::sin(t * 2 * kPi * 6));
    return PlayToKpad(0.6f * sw, 3.2f * sw, 1.f + 1.8f * static_cast<float>(std::cos(t * 2 * kPi * 6)));
}
Vec3 NunchukAccel(bool shaking, uint64_t frame) {
    if (!shaking) return PlayToKpad(0.f, 0.f, 1.f);
    const double t = static_cast<double>(frame) / 60.0;
    return PlayToKpad(0.f, 3.2f * static_cast<float>(std::sin(t * 2 * kPi * 6)),
                      1.f + 1.8f * static_cast<float>(std::cos(t * 2 * kPi * 6)));
}

// ---------------------------------------------------------------------------------------------------------------
// Script and gamepad
// ---------------------------------------------------------------------------------------------------------------
struct ScriptEvent {
    int64_t gate = -1;   // ProjProcess state id the time is relative to; -1 = first KPADRead
    double at = 0, dur = 0.15;
    uint32_t buttons = 0;
    float sx = 0, sy = 0;
    bool stick = false, shake = false, wheel = false;
    std::vector<int> keys;   // scheme input codes (KEY_* / MOUSE_*)
};

struct State {
    bool init = false;
    std::chrono::steady_clock::time_point start;
    std::vector<ScriptEvent> script;
    std::vector<bool> wheelFired;
    std::map<uint32_t, double> stateEntered;   // ProjProcess state id -> first time seen (s since first KPADRead)
    uint32_t lastHold = 0;
    bool lastUi = false;
    bool trace = false;
    float lastAccValue = 1.f;
    SDL_Gamepad* pad = nullptr;
} g;

uint32_t ParseButton(const std::string& t, ScriptEvent& e) {
    if (t == "A") return kA; if (t == "B") return kB; if (t == "1") return k1; if (t == "2") return k2;
    if (t == "PLUS" || t == "+") return kPlus; if (t == "MINUS" || t == "-") return kMinus; if (t == "HOME") return kHome;
    if (t == "UP") return kUp; if (t == "DOWN") return kDown; if (t == "LEFT") return kLeft; if (t == "RIGHT") return kRight;
    if (t == "Z") return kZ; if (t == "C") return kC;
    if (t == "SHAKE") { e.shake = true; return 0; }
    if (t == "WHEEL") { e.wheel = true; return 0; }
    if (t == "MOUSE_LEFT") { e.keys.push_back(kMouseLeft); return 0; }
    if (t == "MOUSE_MIDDLE") { e.keys.push_back(kMouseMiddle); return 0; }
    if (t == "MOUSE_RIGHT") { e.keys.push_back(kMouseRight); return 0; }
    if (t.rfind("KEY_", 0) == 0) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(t.c_str() + 4);
        if (sc != SDL_SCANCODE_UNKNOWN) { e.keys.push_back(sc); return 0; }
    }
    if (t.rfind("SX=", 0) == 0) { e.stick = true; e.sx = std::strtof(t.c_str() + 3, nullptr); return 0; }
    if (t.rfind("SY=", 0) == 0) { e.stick = true; e.sy = std::strtof(t.c_str() + 3, nullptr); return 0; }
    RT_LOG(RT_TAG_HLE) << "[opuc] OPUC_INPUT_SCRIPT: unknown token '" << t << "'" << std::endl;
    return 0;
}

void Init() {
    g.init = true;
    g.start = std::chrono::steady_clock::now();
    g.trace = std::getenv("OPUC_TRACE_INPUT") != nullptr;
    InitKeyboard();
    if (const char* s = std::getenv("OPUC_INPUT_SCRIPT")) {
        std::string all(s);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(',', pos);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(pos, end - pos);
            pos = end + 1;
            ScriptEvent e;
            if (item.size() > 2 && (item[0] == 'S' || item[0] == 's') && item.find('@') != std::string::npos) {
                const size_t at = item.find('@');   // S<hex state>@<sec>:...
                e.gate = static_cast<int64_t>(std::strtoul(item.c_str() + 1, nullptr, 16));
                item = item.substr(at + 1);
            }
            size_t c1 = item.find(':');
            if (c1 == std::string::npos) continue;
            e.at = std::strtod(item.c_str(), nullptr);
            size_t c2 = item.find(':', c1 + 1);
            std::string buttons = item.substr(c1 + 1, (c2 == std::string::npos ? item.size() : c2) - c1 - 1);
            if (c2 != std::string::npos) e.dur = std::strtod(item.c_str() + c2 + 1, nullptr);
            size_t bp = 0;
            while (bp <= buttons.size()) {
                size_t be = buttons.find('+', bp);
                if (be == bp) be = buttons.find('+', bp + 1);   // a token that is itself '+'
                if (be == std::string::npos) be = buttons.size();
                const std::string tok = buttons.substr(bp, be - bp);
                if (!tok.empty()) e.buttons |= ParseButton(tok, e);
                bp = be + 1;
            }
            g.script.push_back(e);
        }
        g.wheelFired.assign(g.script.size(), false);
        RT_LOG(RT_TAG_HLE) << "[opuc] input script: " << g.script.size() << " event(s)" << std::endl;
    }
}

void OpenGamepad() {
    if (g.pad) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) {
        g.pad = SDL_OpenGamepad(ids[0]);
        if (g.pad) RT_LOG(RT_TAG_HLE) << "[opuc] virtual Wii Remote reads gamepad '" << SDL_GetGamepadName(g.pad) << "'" << std::endl;
    }
    SDL_free(ids);
}

// ProjProcess state id: *(0x8068A2B0) -> ALTask, state at +0x64 (same read as milestones.cpp; vault boot-flow).
uint32_t CurrentProjState() {
    auto ok = [](uint32_t p) { return p >= 0x80000000u && p < 0x81800000u; };
    try {
        const uint32_t proj = Memory::Read32(0x8068A2B0u);
        if (!ok(proj)) return 0xFFFFFFFFu;
        const uint32_t task = Memory::Read32(proj);
        return ok(task) ? Memory::Read32(task + 0x64u) : 0xFFFFFFFFu;
    } catch (...) {
        return 0xFFFFFFFFu;
    }
}

struct Sample {
    uint32_t hold = 0;
    float sx = 0, sy = 0;
    bool shake = false, shakePulse = false, nunchukShake = false;
    bool ui = false;
};

Sample Poll() {
    Sample s;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.start).count();
    const uint32_t state = CurrentProjState();
    if (state != 0xFFFFFFFFu) g.stateEntered.emplace(state, t);
    s.ui = GameUi(state);

    // Script events: Wii buttons go straight into the sample; KEY_/MOUSE_/WHEEL tokens go through the scheme.
    std::array<bool, kCodeCount> scriptKeys{};
    int scriptWheel = 0;
    for (size_t i = 0; i < g.script.size(); ++i) {
        const ScriptEvent& e = g.script[i];
        double base = 0;
        if (e.gate >= 0) {
            const auto it = g.stateEntered.find(static_cast<uint32_t>(e.gate));
            if (it == g.stateEntered.end() || state != static_cast<uint32_t>(e.gate)) continue;
            base = it->second;
        }
        if (t >= base + e.at && t < base + e.at + e.dur) {
            s.hold |= e.buttons;
            if (e.stick) { s.sx += e.sx; s.sy += e.sy; }
            if (e.shake) s.shakePulse = true;
            for (int c : e.keys) scriptKeys[c] = true;
            if (e.wheel && !g.wheelFired[i]) { scriptWheel++; g.wheelFired[i] = true; }
        }
    }

    const KbSample k = ReadKeyboard(s.ui, scriptKeys, scriptWheel);
    s.hold |= k.hold;
    s.sx += k.sx;
    s.sy += k.sy;
    s.shake |= k.shake;
    s.shakePulse |= k.shakePulse;
    s.nunchukShake |= k.nunchukShake;

    OpenGamepad();
    if (g.pad) {
        auto b = [](SDL_GamepadButton x) { return SDL_GetGamepadButton(g.pad, x); };
        if (b(SDL_GAMEPAD_BUTTON_SOUTH)) s.hold |= kA;
        if (b(SDL_GAMEPAD_BUTTON_EAST)) s.hold |= kB;
        if (b(SDL_GAMEPAD_BUTTON_WEST)) s.hold |= k1;
        if (b(SDL_GAMEPAD_BUTTON_NORTH)) s.hold |= k2;
        if (b(SDL_GAMEPAD_BUTTON_START)) s.hold |= kPlus;
        if (b(SDL_GAMEPAD_BUTTON_BACK)) s.hold |= kMinus;
        if (b(SDL_GAMEPAD_BUTTON_GUIDE)) s.hold |= kHome;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) s.hold |= kUp;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.hold |= kDown;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.hold |= kLeft;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.hold |= kRight;
        if (b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) s.hold |= kC;
        if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) s.shake = true;
        if (SDL_GetGamepadAxis(g.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) s.hold |= kZ;
        const float ax = SDL_GetGamepadAxis(g.pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        const float ay = -SDL_GetGamepadAxis(g.pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (std::fabs(ax) > 0.15f) s.sx += ax;
        if (std::fabs(ay) > 0.15f) s.sy += ay;
    }
    const float mag = std::sqrt(s.sx * s.sx + s.sy * s.sy);
    if (mag > 1.0f) { s.sx /= mag; s.sy /= mag; }
    return s;
}

void WriteF32(uint32_t at, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    Memory::Write32(at, bits);
}

} // namespace

// WPADProbe (802c30f0): channel 0 has a remote with a Nunchuk (WPAD_DEV_FREESTYLE = 1), the others nothing.
extern "C" int32_t WPADProbe_OPUC(uint32_t chan, uint32_t typePtr) {
    if (chan != 0) {
        if (typePtr) Memory::Write32(typePtr, 0xFD);  // WPAD_DEV_NOT_FOUND
        return -1;                                     // WPAD_ERR_NO_CONTROLLER
    }
    if (typePtr) Memory::Write32(typePtr, 1);
    return 0;
}
PPC_NATIVE_OVERRIDE(802C30F0, WPADProbe_OPUC, int32_t, (uint32_t chan, uint32_t typePtr), (chan, typePtr));

// KPADRead (8030454c): one fresh sample per call for channel 0, none for the others.
extern "C" int32_t KPADRead_OPUC(uint32_t chan, uint32_t buf, uint32_t length) {
    if (!g.init) Init();
    if (chan != 0 || buf == 0 || length == 0) return 0;
    const Sample s = Poll();
    const uint32_t hold = s.hold;
    for (uint32_t o = 0; o < kKpadStatusSize; o += 4) Memory::Write32(buf + o, 0);
    Memory::Write32(buf + 0x00, hold);
    Memory::Write32(buf + 0x04, hold & ~g.lastHold);   // trig
    Memory::Write32(buf + 0x08, g.lastHold & ~hold);   // release
    const Vec3 ra = RemoteAccel(s.shake, s.shakePulse, kb.frame);
    const float raValue = std::sqrt(ra.x * ra.x + ra.y * ra.y + ra.z * ra.z);
    WriteF32(buf + 0x0C, ra.x);
    WriteF32(buf + 0x10, ra.y);
    WriteF32(buf + 0x14, ra.z);
    WriteF32(buf + 0x18, raValue);                     // acc_value
    Memory::Write8(buf + 0x5C, 1);                     // dev_type: Nunchuk attached
    Memory::Write8(buf + 0x5D, 0);                     // wpad_err: none
    WriteF32(buf + 0x60, s.sx);                        // Nunchuk stick (ConvertKpadToRaw negates y)
    WriteF32(buf + 0x64, s.sy);
    const Vec3 na = NunchukAccel(s.nunchukShake, kb.frame);
    WriteF32(buf + 0x68, na.x);
    WriteF32(buf + 0x6C, na.y);
    WriteF32(buf + 0x70, na.z);
    WriteF32(buf + 0x74, std::sqrt(na.x * na.x + na.y * na.y + na.z * na.z));
    if (g.trace) {
        if (hold != g.lastHold || s.ui != g.lastUi)
            RT_LOG(RT_TAG_HLE) << "[opuc] input hold=0x" << std::hex << hold << std::dec << " ui=" << s.ui
                               << " stick=(" << s.sx << "," << s.sy << ")" << std::endl;
        if (raValue >= 3.0f && g.lastAccValue < 3.0f)
            RT_LOG(RT_TAG_HLE) << "[opuc] input shake pulse acc_value=" << raValue << std::endl;
    }
    g.lastHold = hold;
    g.lastUi = s.ui;
    g.lastAccValue = raValue;
    return 1;
}
PPC_NATIVE_OVERRIDE(8030454C, KPADRead_OPUC, int32_t, (uint32_t chan, uint32_t buf, uint32_t length), (chan, buf, length));
