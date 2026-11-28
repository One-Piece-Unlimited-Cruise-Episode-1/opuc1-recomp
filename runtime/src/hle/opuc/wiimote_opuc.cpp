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
//               RMB/L shake attack = a held Remote swing (with a pickaxe / net in the hand: hold and drag to swing,
//               see ToolFrame), Shift tap = dodge (A + B on one frame, on release within
//               12 frames), Shift held = dash (C, -, C, then C held), Q skill 1 = C then C + A, R skill 2 = C then
//               C + shake, MMB/T lock on = Z, wheel = switch target (Z double-tap), O = Nunchuk shake (break free),
//               E/Enter = A, V = 1 (first-person look), C = - (character select), X = -, 1/2/3/4 = D-pad
//               up/left/down/right (item slots / next page), arrows = D-pad, Esc = + (pause), Tab = +,
//               Backspace = B.
//             In menus and windows ("ui": any ProjProcess state other than AdventureMain / BattleMain, or an active
//             AdvMain pause process) Esc and RMB mean B, WASD also send the D-pad and Shift is C, as in the Play app.
//             Mouse look: on the field the mouse is captured (relative mode) and turns the follow / first-person
//             camera ([game] mouse_camera; see "Mouse camera" below); menus free it, Alt held frees it, and a
//             held pickaxe / net or the rod keeps the game's camera. Not ported: the Play app's fishing simulation
//             keys and its viewer-only keys. Input is ignored while the runtime overlay blocks input.
//   gamepad   first SDL gamepad: South=A East=B West=1 North=2 Start=+ Back=- Guide=HOME, D-pad, left stick,
//             left trigger=Z left shoulder=C, right shoulder=shake
//   script    OPUC_INPUT_SCRIPT="<sec>:<buttons>[:<dur>],..." with buttons joined by '+', e.g. "8:A:0.2,12:A+B"
//             (seconds since the first KPADRead; default duration 0.15 s). Stick: SX=-1..1 / SY=-1..1 tokens.
//             "S<hex state>@<sec>:<buttons>[:<dur>]" counts from the first read that sees ProjProcess in that
//             state, e.g. "S1@8.3:A" presses A 8.3 s after the title state (1) starts. It fires only while the
//             game is still in that state. Key tokens "KEY_<SDL scancode name>" / "MOUSE_LEFT|MIDDLE|RIGHT" /
//             "WHEEL" press keyboard / mouse inputs through the scheme above (for tests), e.g. "S3@40:KEY_LSHIFT:0.1";
//             "MDX=<px>" / "MDY=<px>" move the mouse that many pixels a frame while the event lasts.
//   OPUC_TRACE_INPUT=1 logs every change of the written hold word, the ui flag and shake pulses.
#include "hle_stubs.h"
#include "dev_pages.h"
#include "fishing_pc.h"
#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "input_bindings.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
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
    double mouseDx = 0, mouseDy = 0;   // mouse movement since the last sample, pixels (fishing Remote input / lean)
    bool walk = false;
    struct { bool on = false; int frames = 0; bool dash = false; int hold = 0; } dodge;
    uint64_t frame = 0;
    std::atomic<bool> ui{true};
    std::atomic<bool> captured{false};   // relative mouse mode is on (OpucInput_UpdateMouseCapture)
    std::atomic<bool> mouseTool{false};  // a pickaxe / net in the hand or the rod out: the mouse drives the remote
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
    case SDL_EVENT_MOUSE_MOTION: kb.mouseDx += e->motion.xrel; kb.mouseDy += e->motion.yrel; break;
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
// The Sunny's camp menu (the hub, every page under it, and the pages Sunny room objects open) is not seen by that
// walk (scripted run 2026-11-28: ui stayed 0 with the hub and its Save page open, so RMB was a field shake and did
// not go back; its record comes from AdvMain::PauseProcessStart in CharaManager::RequestCampMenu 800424a0, why the
// walk misses it is not traced). It is open while CharaManager +0x30C (the AquaDesignerCampMenu; CharaManager = *(0x8068A18C)) is set:
// RequestCampMenu / CampMenuStart 800425a0 build it, CampMenu::DispatchCloseAction 80042764 clears it.
bool GameUi(uint32_t projState) {
    if (projState != 2 && projState != 3) return true;
    auto ok = [](uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); };
    try {
        const uint32_t cm = Memory::Read32(0x8068A18Cu);
        if (ok(cm) && Memory::Read32(cm + 0x30Cu) != 0) return true;
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
    bool toolKey = false;           // the shake key (RMB / L) held in the field: a held tool's drag swing
    double mousePxX = 0, mousePxY = 0;  // mouse movement this frame, pixels
    OpucFishing::PadIn fish;   // the Play app's Fishing key group (read while the rod is out)
};
constexpr double kLookRadPerPx = 0.0025;   // controller.js LOOK_RAD_PER_PX

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
    // Fishing group defaults: fishCast Mouse0 / J (= Attack), fishHook Mouse2 / L (= Shake), lean A / D and raise W
    // (= the move keys), fishQuit Space / Backspace (= Jump / Back).
    static bool prevHook = false, prevQuit = false;
    const bool hook = h(Shake), quit = h(Jump) || h(Back);
    s.fish.cast = h(Attack);
    s.fish.hookEdge = hook && !prevHook;
    s.fish.quitEdge = quit && !prevQuit;
    prevHook = hook;
    prevQuit = quit;
    s.fish.sx = x;
    s.fish.sy = y;
    s.toolKey = h(Shake) && !ui;
    s.mousePxX = kb.mouseDx;
    s.mousePxY = kb.mouseDy;
    s.fish.mouseDx = kb.mouseDx * kLookRadPerPx;
    s.fish.mouseDy = kb.mouseDy * kLookRadPerPx;
    kb.mouseDx = kb.mouseDy = 0;
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
    float mdx = 0, mdy = 0;  // MDX= / MDY=: mouse movement in pixels per frame while the event is active
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
    if (t.rfind("MDX=", 0) == 0) { e.mdx = std::strtof(t.c_str() + 4, nullptr); return 0; }
    if (t.rfind("MDY=", 0) == 0) { e.mdy = std::strtof(t.c_str() + 4, nullptr); return 0; }
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
    bool toolKey = false, padShake = false;
    bool scriptMouse = false;   // an OPUC_INPUT_SCRIPT MDX / MDY event moved the mouse this frame
    double mousePxX = 0, mousePxY = 0;
    OpucFishing::PadIn fish;
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
            if (e.mdx != 0 || e.mdy != 0) {
                std::lock_guard<std::mutex> l(kb.m);
                kb.mouseDx += e.mdx;
                kb.mouseDy += e.mdy;
                s.scriptMouse = true;
            }
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
    s.fish = k.fish;
    s.toolKey = k.toolKey;
    s.mousePxX = k.mousePxX;
    s.mousePxY = k.mousePxY;

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
        if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) { s.shake = true; s.padShake = true; }
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

// ---------------------------------------------------------------------------------------------------------------
// Held tools: the pickaxe (CharaDigup) and the bug nets (CharaCapture) swing on a WiiMotion gesture, not on a button
// (vault entities/gathering.md §1c, entities/pad-input "Gestures"). The CanEnters (pickaxe 80031f5c, nets 8002f620)
// read the gesture byte WiiMotion +0x1320: 2 -> record 1 (the overhead swing), 4 -> record 2, 8 -> record 3 (the
// sideways ones). The held 6 Hz shake of the scheme trips all three detectors at random and the publish quirk eats
// most of them, so while a tool is in the hand the shake key sends one clean gesture per stroke instead:
//   hold RMB (or L) and drag the mouse top to bottom: the 40 ms tip pull (0, +2.6, -1) g -> detector 0 -> 2;
//   hold RMB and drag sideways: 15 ms at (+-0.8, -1, 1.5) g -> detector 1 (x > 0, bit 8) or 2 (x < 0, bit 4);
//   a click (released within 20 frames without a stroke) or the gamepad's right shoulder: the overhead swing.
// One swing per stroke: a long drag swings once; an upward drag rearms the overhead swing (chopping: down, up, down),
// a sideways swing rearms the other side (sweeping back and forth), a new press rearms all.
// Checked by script (OPUC_TRACE_INPUT): each stroke publishes its gesture and the CanEnter takes it, pickaxe and net.
// ^[inferred: drag right -> x > 0 -> record 3; which of records 2 / 3 sweeps left-to-right is not verified]
constexpr int kToolStrokePx = 40, kToolClickFrames = 20, kToolCooldown = 10;
constexpr uint32_t kRecordsPickaxe = 0x805012E8, kRecordsNet = 0x80501158;

struct Tool {
    OpucFishing::MotionSource motion;
    std::atomic<uint32_t> chara{0};    // the owner CharaObj of the last CanEnter / Update (work +0x13E8)
    std::atomic<uint64_t> seen{0};     // the KPAD frame of that call
    bool keyPrev = false, padPrev = false, stroked = false;
    bool armed[4] = {true, true, true, true};  // per record: a stroke fires once until it is rearmed
    double ax = 0, ay = 0;
    int pressFrames = 0, cooldown = 0;
    uint32_t lastHeld = 0;
    std::vector<OpucFishing::V3> acc;
} tool;

bool ToolPtr(uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }

// The held item id of the CharaObj that owns the last tool WiiMotion / CanEnter / Update call (CharaObj::GetHeldItemId
// 8006b614: *(*(chara + 0x16C4) + 0x9C)): 1 pickaxe, 2 net, 3 broken net. A tool polls its CanEnter every field frame
// while it is in the hand (and runs Update while it swings), so a stale owner means no tool.
uint32_t HeldTool() {
    if (kb.frame - tool.seen.load() > 120) return 0;
    try {
        const uint32_t ch = tool.chara.load();
        if (!ToolPtr(ch)) return 0;
        const uint32_t use = Memory::Read32(ch + 0x16C4);
        if (!ToolPtr(use)) return 0;
        const uint32_t id = Memory::Read32(use + 0x9C);
        return id >= 1 && id <= 3 ? id : 0;
    } catch (...) {
        return 0;
    }
}

// 0 nothing, 1 overhead, 2 sideways with x < 0, 3 sideways with x > 0 (the gesture records they pick).
void ToolSwing(int rec) {
    using OpucFishing::Timeline;
    if (rec == 1) tool.motion.Play(OpucFishing::RodJerk());
    else tool.motion.Play(Timeline{{15, {rec == 3 ? 0.8f : -0.8f, -1.0f, 1.5f}}});
    tool.cooldown = kToolCooldown;
    if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] tool: swing record " << rec << " (held " << tool.lastHeld << ")" << std::endl;
}

// Per KPADRead: true while a tool is in the hand and the field has the controls; fills n samples (oldest first).
bool ToolFrame(const Sample& s, int n) {
    const uint32_t held = s.ui ? 0 : HeldTool();
    if (held != tool.lastHeld) {
        if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] tool: held " << held << std::endl;
        tool.lastHeld = held;
        tool.keyPrev = tool.padPrev = tool.stroked = false;
        tool.ax = tool.ay = 0;
    }
    if (!held) return false;
    if (tool.cooldown > 0 && !tool.motion.Playing()) tool.cooldown--;
    const bool ready = !tool.motion.Playing() && tool.cooldown == 0;
    if (s.toolKey && !tool.keyPrev) {
        tool.ax = tool.ay = 0; tool.pressFrames = 0; tool.stroked = false;
        for (bool& a : tool.armed) a = true;
    }
    auto fire = [&](int rec) {
        ToolSwing(rec);
        tool.stroked = true;
        tool.armed[rec] = false;
        if (rec == 2) tool.armed[3] = true;
        if (rec == 3) tool.armed[2] = true;
        tool.ax = tool.ay = 0;
    };
    if (s.toolKey) {
        tool.pressFrames++;
        tool.ax += s.mousePxX;
        tool.ay += s.mousePxY;
        const double adx = std::fabs(tool.ax), ady = std::fabs(tool.ay);
        if (ady >= kToolStrokePx && ady > adx) {
            if (tool.ay > 0) { if (ready && tool.armed[1]) fire(1); else if (!tool.armed[1]) tool.ax = tool.ay = 0; }
            else { tool.armed[1] = true; tool.ax = tool.ay = 0; }   // the upstroke rearms
        } else if (adx >= kToolStrokePx && adx > ady) {
            const int rec = tool.ax > 0 ? 3 : 2;
            if (ready && tool.armed[rec]) fire(rec);
            else if (!tool.armed[rec]) tool.ax = tool.ay = 0;
        }
    } else if (tool.keyPrev && !tool.stroked && tool.pressFrames <= kToolClickFrames && ready) {
        ToolSwing(1);
    }
    if (s.padShake && !tool.padPrev && ready) ToolSwing(1);
    tool.keyPrev = s.toolKey;
    tool.padPrev = s.padShake;
    tool.motion.Frame(n, tool.acc);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Mouse camera (the Play tab's mouse look, play/camera.js; not in the game, whose camera turns only with Z held +
// D-pad). KPADRead turns this frame's mouse movement into a yaw / pitch delta; the wrappers of the follow camera's
// GWCameraSolo::UpdateRotation (804bcfc0) and the first-person GWCameraSubject::Update (804be8c4) add it to the
// GWCameraManager angles before the game's own rotation step. Vault entities/adventure-camera ("Mouse camera").
//   GWCameraManager = *(*(0x8068A470) + 0x14); +0x230 pitch (stored negated: camera above = 0x10000 - n), +0x234 yaw,
//   both s32 BAM (0x10000 = 360°) masked to 0..0xFFFF; +0xb8 flags (0x2000 = Z held: no movement swing, no pitch
//   return; cleared by GWCameraManager::Update every frame at 804c4ea8), +0x150 non-zero = a mode blend runs.
//   UpdateRotation(r3 solo, r4 &vel, r5 pitch0, f1 dist) is only called with no blend, reset or recenter running;
//   it masks and clamps the angles only while its rotation rates are non-zero, so the wrapper does both itself
//   (pitch limit = SoloData row (solo + 0x68) + 0x28, 85°; Subject: SubjectData row (subject + 0x64) + 0x28, 30°).
// Off in menus (ui), while fishing (the mouse moves the remote) and while a pickaxe / net is in the hand (the mouse
// drags the swing): there the game's own camera runs untouched. As in the Play tab, a mouse movement holds the
// automatic swing (flag 0x2000) and the slope pitch push (UpdateRotation gets a zero velocity) for 1.5 s.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kBattleCameraPtr = 0x8068A470;
constexpr int kMouseHoldFrames = 90;
constexpr double kBamPerRad = 65536.0 / (2 * kPi);

struct MouseCam {
    std::atomic<int32_t> dyaw{0}, dpitch{0};   // pending BAM deltas for the next camera update
    std::atomic<int> hold{0};                  // frames left of the held automatic swing
    double fracYaw = 0, fracPitch = 0;         // sub-BAM remainders
    std::atomic<bool> zoomOk{false};           // ground zoom allowed (field, mouse camera on, mouse not on a tool)
    bool zoomed = false;                       // the ground zoom owns the distance (set in, follows the pitch out)
    bool scriptSeen = false;                   // a scripted MDX / MDY ran (tests have no capture)
    uint64_t lastLog = 0;
} mcam;

// One frame's delta at a time: a delta the camera did not take (blend, recenter, lock-on, a script camera) is
// dropped, not saved up for later. The first frames after the capture starts are skipped (the switch to relative
// mode can report the cursor's jump as motion).
void MouseCameraFrame(const Sample& s, bool remoteOwnsMouse) {
    static bool wasCaptured = false;
    static int settle = 0;
    const bool captured = kb.captured.load(std::memory_order_relaxed);
    if (captured && !wasCaptured) settle = 2;
    wasCaptured = captured;
    const bool on = !s.ui && !remoteOwnsMouse && RuntimeConfigFile::MouseCamera(true) && (captured || s.scriptMouse);
    kb.mouseTool.store(remoteOwnsMouse, std::memory_order_relaxed);
    if (s.scriptMouse) mcam.scriptSeen = true;
    mcam.zoomOk = !s.ui && !remoteOwnsMouse && RuntimeConfigFile::MouseCamera(true) && (captured || mcam.scriptSeen);
    mcam.dyaw = 0;
    mcam.dpitch = 0;
    if (!on) {
        mcam.hold = 0;
        mcam.fracYaw = mcam.fracPitch = 0;
        return;
    }
    if (settle > 0 && !s.scriptMouse) { settle--; return; }
    if (s.mousePxX == 0 && s.mousePxY == 0) {
        if (mcam.hold > 0) mcam.hold--;
        return;
    }
    const double k = kLookRadPerPx * RuntimeConfigFile::MouseCameraSensitivity(1.0f) * kBamPerRad;
    const double inv = RuntimeConfigFile::MouseCameraInvertY(false) ? -1.0 : 1.0;
    // play/camera.js: yaw -= dx, pitch (elevation) += dy. The game's yaw is the bearing look-at -> eye like the
    // port's; its pitch is the negated elevation.
    mcam.fracYaw += -s.mousePxX * k;
    mcam.fracPitch += -s.mousePxY * k * inv;
    const int32_t iy = static_cast<int32_t>(mcam.fracYaw), ip = static_cast<int32_t>(mcam.fracPitch);
    mcam.fracYaw -= iy;
    mcam.fracPitch -= ip;
    mcam.dyaw = iy;
    mcam.dpitch = ip;
    mcam.hold = kMouseHoldFrames;
}

// Pitch for a mouse step, as an orbit camera does it: the delta is added in signed space (stored pitch s > 0 = eye
// below the look-at), then held at the limits instead of wrapping. The game's own limit is a symmetric +-row+0x28
// (85 deg for Solo). Under the horizon, GWCameraSolo::Update's floor clamp / occlusion pull the eye back up and
// re-polarise the pitch every frame. Pushed on past about 35 deg (0x1945 on flat Sunny deck, 2026-11-28 trace), the
// re-polarised eye flips over the player: pitch 0x1945 -> 0xE2ED, the "snap to bird's-eye". A pitch the game itself
// put past a limit (slope push, collision) is left alone; the mouse only cannot push it further out.
//
// Ground zoom (as in Breath of the Wild / Mario Odyssey): under the horizon the follow camera comes closer instead of
// sinking into the floor, so it keeps tilting up toward the sky. After the game's rotation step the Solo wrapper caps
// the distance mgr+0x238 so the eye stays kFloorGap above the player's floor (look-at = row offset y above it; the
// game's own floor clamp is +0.5), but never nearer than kMinHoriz horizontally (IsEyeInsideTarget: target radius
// 0.8 + 0.4). Both together give the mouse's downward limit atan2(offY - kFloorGap, kMinHoriz), at most
// kMouseLowLimit (32.7 deg for offset 1.5 -> 30 deg). Once the zoom has engaged it owns the distance in both
// directions: dist = min(target, floor-safe distance at this pitch), target = UpdateRotation's f1. So raising the
// camera zooms straight back out (the game's own dist += 0.02 (target - dist) took ~5 s and felt stuck) and hands
// back to the game at the angle where the eye clears the floor at the target distance. On uneven ground the game's
// floor clamp and occlusion still have the last word.
constexpr int32_t kMouseLowLimit = 0x1555;   // 30 deg under the horizon at most (Solo); Subject keeps its row limit
constexpr float kFloorGap = 0.6f;
constexpr float kMinHoriz = 1.4f;

float ReadF32(uint32_t at) {
    const uint32_t bits = Memory::Read32(at);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// Look-at height above the followed node (SoloData row +0x04, 1.5 for row 0).
float SoloOffsetY(uint32_t row) {
    const float y = ToolPtr(row) ? ReadF32(row + 0x04) : 1.5f;
    return std::isfinite(y) && y > kFloorGap + 0.1f && y < 10.0f ? y : 1.5f;
}

int32_t SoloLowLimit(uint32_t row) {
    const double lim = std::atan2(SoloOffsetY(row) - kFloorGap, kMinHoriz) * kBamPerRad;
    return std::min(kMouseLowLimit, static_cast<int32_t>(lim));
}

// Sets the follow camera's distance while it looks up from below the horizon; returns the distance written (0 =
// untouched). target = the distance the game is easing toward (UpdateRotation's f1).
float GroundZoom(uint32_t mgr, uint32_t row, float target) {
    if (!mgr || !mcam.zoomOk.load()) { mcam.zoomed = false; return 0; }
    const float cur = ReadF32(mgr + 0x238);
    if (!std::isfinite(cur) || cur <= 0.0f) { mcam.zoomed = false; return 0; }
    if (!std::isfinite(target) || target <= 0.0f || target > 50.0f) target = std::max(cur, 4.5f);
    const int32_t v = static_cast<int32_t>(Memory::Read32(mgr + 0x230) & 0xFFFFu);
    const int32_t s = v <= 0x8000 ? v : v - 0x10000;
    double safe = 1e9;   // floor-safe distance at this pitch (unbounded at or above the horizon)
    if (s > 0) {
        const double a = s / kBamPerRad;
        safe = std::max(kMinHoriz / std::cos(a), (SoloOffsetY(row) - kFloorGap) / std::sin(a));
    }
    if (mcam.zoomed) {
        // Follow the pitch back out; release once the floor no longer limits the target distance.
        const float d = static_cast<float>(std::min<double>(target, safe));
        if (safe >= target) mcam.zoomed = false;
        if (d != cur) WriteF32(mgr + 0x238, d);
        return d;
    }
    if (safe >= cur) return 0;
    mcam.zoomed = true;
    const float d = static_cast<float>(safe);
    WriteF32(mgr + 0x238, d);
    return d;
}

uint32_t StepPitch(uint32_t pitch0, int32_t dp, int32_t up, int32_t down) {
    const int32_t v0 = static_cast<int32_t>(pitch0 & 0xFFFFu);
    const int64_t s0 = v0 <= 0x8000 ? v0 : v0 - 0x10000;
    int64_t s = s0 + dp;
    if (dp > 0 && s > down) s = std::max<int64_t>(down, s0);
    if (dp < 0 && s < -up) s = std::min<int64_t>(-up, s0);
    s = std::clamp<int64_t>(s, -0x4000, 0x4000);
    return static_cast<uint32_t>(s) & 0xFFFFu;
}

// Adds the pending delta to the manager's angles; returns the manager (0 when there is none or it is blending).
uint32_t ApplyMouseCamera(uint32_t rowPtrField, const char* what, bool solo) {
    const int32_t dy = mcam.dyaw.exchange(0), dp = mcam.dpitch.exchange(0);
    const uint32_t bc = Memory::Read32(kBattleCameraPtr);
    if (!ToolPtr(bc)) return 0;
    const uint32_t mgr = Memory::Read32(bc + 0x14);
    if (!ToolPtr(mgr) || Memory::Read32(mgr + 0x150) != 0 || (Memory::Read32(mgr + 0xB8) & 1)) return 0;
    if (dy == 0 && dp == 0) return mgr;
    const uint32_t row = Memory::Read32(rowPtrField);
    const int32_t limit = ToolPtr(row) ? static_cast<int32_t>(Memory::Read32(row + 0x28)) : 0x3C71;
    const uint32_t yaw0 = Memory::Read32(mgr + 0x234), pitch0 = Memory::Read32(mgr + 0x230);
    const uint32_t yaw = (yaw0 + static_cast<uint32_t>(dy)) & 0xFFFFu;
    const int32_t up = limit > 0 && limit < 0x4000 ? limit : 0x3C71;
    const uint32_t pitch = StepPitch(pitch0, dp, up, solo ? std::min(up, SoloLowLimit(row)) : up);
    Memory::Write32(mgr + 0x234, yaw);
    Memory::Write32(mgr + 0x230, pitch);
    if (g.trace && kb.frame - mcam.lastLog >= 30) {
        mcam.lastLog = kb.frame;
        RT_LOG(RT_TAG_HLE) << "[opuc] mouse camera (" << what << "): yaw 0x" << std::hex << yaw0 << " -> 0x" << yaw
                           << " pitch 0x" << pitch0 << " -> 0x" << pitch << std::dec << " dist "
                           << ReadF32(mgr + 0x238) << std::endl;
    }
    return mgr;
}

} // namespace

extern "C" void OpucMouseCam_SoloUpdateRotation(CpuContext* ctx) {
    uint32_t mgr = 0;
    const uint32_t vel = ctx->gpr[4];
    uint32_t vx = 0, vz = 0;
    bool held = false;
    const uint32_t soloRowField = ctx->gpr[3] + 0x68;
    const float targetDist = static_cast<float>(ctx->fpr[1].d);   // f1 = the distance the game eases toward
    try {
        mgr = ApplyMouseCamera(soloRowField, "solo", true);
        held = mgr && mcam.hold.load() > 0;
        if (held) {
            Memory::Write32(mgr + 0xB8, Memory::Read32(mgr + 0xB8) | 0x2000u);
            vx = Memory::Read32(vel);
            vz = Memory::Read32(vel + 8);
            Memory::Write32(vel, 0);
            Memory::Write32(vel + 8, 0);
        }
    } catch (...) { held = false; }
    GuestFunctions::NextTranslation(0x804BCFC0)(ctx);
    if (held) {
        // Update reads |vel| after the call (the rail branch): give it the real velocity back.
        try { Memory::Write32(vel, vx); Memory::Write32(vel + 8, vz); } catch (...) {}
    }
    // After the game's rotation and distance step, before Update places the eye (PolarToPos).
    try {
        const float d = GroundZoom(mgr, Memory::Read32(soloRowField), targetDist);
        if (d > 0 && g.trace && kb.frame % 15 == 0)
            RT_LOG(RT_TAG_HLE) << "[opuc] mouse camera ground zoom: pitch 0x" << std::hex
                               << (Memory::Read32(mgr + 0x230) & 0xFFFFu) << std::dec << " dist " << d << " target "
                               << targetDist << (mcam.zoomed ? "" : " (released)") << std::endl;
    } catch (...) {}
}
REGISTER_NATIVE_WRAPPER_AS(0x804BCFC0, OpucMouseCam_SoloUpdateRotation, "OpucMouseCam_SoloUpdateRotation");

extern "C" void OpucMouseCam_SubjectUpdate(CpuContext* ctx) {
    try { ApplyMouseCamera(ctx->gpr[3] + 0x64, "subject", false); } catch (...) {}
    GuestFunctions::NextTranslation(0x804BE8C4)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x804BE8C4, OpucMouseCam_SubjectUpdate, "OpucMouseCam_SubjectUpdate");

// The tools' CanEnter (r3 = pMoveNode; its sub-user work = *(*(r3) + 0x18), the CharaDigup / CharaCapture work) and
// CharaDigup::Update (r3 = work): remember the owner CharaObj, so the virtual remote knows a tool is in the hand.
namespace {
void ToolSeen(uint32_t work) {
    if (!ToolPtr(work)) return;
    const uint32_t ch = Memory::Read32(work + 0x13E8);
    if (ToolPtr(ch)) { tool.chara.store(ch); tool.seen.store(kb.frame); }
}
void ToolTraceEnter(const char* what, uint32_t work, uint32_t ret) {
    if (!g.trace || !ToolPtr(work)) return;
    const uint32_t flags = Memory::Read8(work + 0x6C + 0x1320), rec = Memory::Read32(work + 0x68);
    if (flags || ret)
        RT_LOG(RT_TAG_HLE) << "[opuc] tool: " << what << " gesture " << flags << " -> " << ret << " ("
                           << (rec == kRecordsPickaxe ? "pickaxe" : rec == kRecordsNet ? "net" : "?") << ")" << std::endl;
}
uint32_t MoveNodeWork(uint32_t node) {
    if (!ToolPtr(node)) return 0;
    const uint32_t n0 = Memory::Read32(node);
    return ToolPtr(n0) ? Memory::Read32(n0 + 0x18) : 0;
}
}  // namespace

extern "C" void OpucTool_DigupCanEnter(CpuContext* ctx) {
    const uint32_t work = MoveNodeWork(ctx->gpr[3]);
    try { ToolSeen(work); } catch (...) {}
    GuestFunctions::NextTranslation(0x80031F5C)(ctx);
    try { ToolTraceEnter("pickaxe CanEnter", work, ctx->gpr[3]); } catch (...) {}
}
REGISTER_NATIVE_WRAPPER_AS(0x80031F5C, OpucTool_DigupCanEnter, "OpucTool_DigupCanEnter");

extern "C" void OpucTool_CaptureCanEnter(CpuContext* ctx) {
    const uint32_t work = MoveNodeWork(ctx->gpr[3]);
    try { ToolSeen(work); } catch (...) {}
    GuestFunctions::NextTranslation(0x8002F620)(ctx);
    try { ToolTraceEnter("net CanEnter", work, ctx->gpr[3]); } catch (...) {}
}
REGISTER_NATIVE_WRAPPER_AS(0x8002F620, OpucTool_CaptureCanEnter, "OpucTool_CaptureCanEnter");

extern "C" void OpucTool_DigupUpdate(CpuContext* ctx) {
    try { ToolSeen(ctx->gpr[3]); } catch (...) {}
    GuestFunctions::NextTranslation(0x80030988)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80030988, OpucTool_DigupUpdate, "OpucTool_DigupUpdate");

// WiiMotion::Update(wm) 8002626c: its callers are the nets' CanEnter body (800305e0), the pickaxe's CanEnter
// (80031f5c), CharaDigup::Update (80030988) and CharaFishing::Update. A tool's WiiMotion sits at work +0x6C with the
// motion records at work +0x68, so the records tell a pickaxe / net call from the fishing one. Marks the tool as held
// and traces every gesture the game publishes for it.
extern "C" void OpucTool_WiiMotionUpdate(CpuContext* ctx) {
    const uint32_t wm = ctx->gpr[3], work = wm - 0x6C;
    uint32_t rec = 0;
    try {
        rec = ToolPtr(work) ? Memory::Read32(work + 0x68) : 0;
        if (rec == kRecordsPickaxe || rec == kRecordsNet) { ToolSeen(work); }
    } catch (...) {}
    GuestFunctions::NextTranslation(0x8002626C)(ctx);
    try {
        if (g.trace && (rec == kRecordsPickaxe || rec == kRecordsNet)) {
            const uint32_t gesture = Memory::Read8(wm + 0x1320);
            if (gesture) RT_LOG(RT_TAG_HLE) << "[opuc] tool: gesture " << gesture << " published (" << (rec == kRecordsNet ? "net" : "pickaxe") << ")" << std::endl;
        }
    } catch (...) {}
}
REGISTER_NATIVE_WRAPPER_AS(0x8002626C, OpucTool_WiiMotionUpdate, "OpucTool_WiiMotionUpdate");


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

// KPADRead (8030454c): one call per game frame for channel 0, none for the others. It returns about 3.3 samples
// a call (10 every 3 frames), as the real remote's 200 Hz KPAD does: the fishing rod's WiiMotion counts samples
// (AnalyzeSwing's quiet < 12, the gesture timers), so the accelerometer has to arrive at the game's rate. Sample 0
// is the newest; every sample carries the same buttons and stick.
extern "C" int32_t KPADRead_OPUC(uint32_t chan, uint32_t buf, uint32_t length) {
    if (!g.init) Init();
    if (chan != 0 || buf == 0 || length == 0) return 0;
    static double sampleAcc = 0;
    sampleAcc += OpucFishing::kSamplesPerFrame;
    int n = static_cast<int>(sampleAcc);
    sampleAcc -= n;
    n = std::clamp(n, 1, static_cast<int>(std::min<uint32_t>(length, 16)));
    const Sample s = Poll();
    OpucDev::Tick();  // the Dev Debug switches held each frame (dev_pages.cpp)
    uint32_t hold = s.hold;

    // Fishing owns A, B, D-pad Right and the accelerometer while the player fishes (fishing_pc.cpp).
    static uint32_t lastPhysHold = 0;
    static bool lastShake = false;
    OpucFishing::PadIn fin = s.fish;
    fin.cast = (s.hold & kA) != 0;
    fin.quitEdge = (s.hold & kB) && !(lastPhysHold & kB);
    fin.hookEdge = (s.shake && !lastShake) || s.shakePulse;
    fin.sx = s.sx;
    fin.sy = s.sy;
    lastPhysHold = s.hold;
    lastShake = s.shake;
    static std::vector<OpucFishing::V3> fishAcc;
    OpucFishing::PadOut fo;
    const bool fishing = OpucFishing::Frame(fin, n, fishAcc, fo);
    if (fishing) {
        hold &= ~(kA | kB);
        if (fo.A) hold |= kA;
        if (fo.B) hold |= kB;
        if (fo.right) hold |= kRight;
    }

    // A held pickaxe / net owns the shake key and the accelerometer (ToolFrame); the vacuum keeps the shake.
    const bool toolOn = !fishing && ToolFrame(s, n);
    MouseCameraFrame(s, fishing || toolOn);
    const Vec3 ra = RemoteAccel(s.shake && !fishing && !toolOn, s.shakePulse && !fishing && !toolOn, kb.frame);
    const Vec3 na = NunchukAccel(s.nunchukShake, kb.frame);
    float raValue = 0;
    // acc_speed (+0x1C) is |acc - previous sample|: WiiMotion's still-sample test reads it (0 < speed < 0.02).
    static Vec3 prevAcc{0.f, -1.f, 0.f};
    std::vector<Vec3> acc(static_cast<size_t>(n), ra);
    if (fishing)
        for (int i = 0; i < n; ++i)
            if (static_cast<size_t>(n - 1 - i) < fishAcc.size()) { const OpucFishing::V3& f = fishAcc[n - 1 - i]; acc[i] = {f.x, f.y, f.z}; }
    if (toolOn)
        for (int i = 0; i < n; ++i)
            if (static_cast<size_t>(n - 1 - i) < tool.acc.size()) { const OpucFishing::V3& f = tool.acc[n - 1 - i]; acc[i] = {f.x, f.y, f.z}; }
    std::vector<float> speed(static_cast<size_t>(n), 0.f);
    for (int i = n - 1; i >= 0; --i) {   // oldest (n - 1) to newest (0)
        const Vec3& v = acc[i];
        speed[i] = std::sqrt((v.x - prevAcc.x) * (v.x - prevAcc.x) + (v.y - prevAcc.y) * (v.y - prevAcc.y) + (v.z - prevAcc.z) * (v.z - prevAcc.z));
        prevAcc = v;
    }
    for (int i = 0; i < n; ++i) {
        const uint32_t e = buf + static_cast<uint32_t>(i) * kKpadStatusSize;
        for (uint32_t o = 0; o < kKpadStatusSize; o += 4) Memory::Write32(e + o, 0);
        Memory::Write32(e + 0x00, hold);
        Memory::Write32(e + 0x04, i == 0 ? hold & ~g.lastHold : 0);   // trig (the newest sample)
        Memory::Write32(e + 0x08, i == 0 ? g.lastHold & ~hold : 0);   // release
        const Vec3& v = acc[i];
        const float value = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (i == 0) raValue = value;
        WriteF32(e + 0x0C, v.x);
        WriteF32(e + 0x10, v.y);
        WriteF32(e + 0x14, v.z);
        WriteF32(e + 0x18, value);                     // acc_value
        WriteF32(e + 0x1C, speed[i]);                  // acc_speed
        Memory::Write8(e + 0x5C, 1);                   // dev_type: Nunchuk attached
        Memory::Write8(e + 0x5D, 0);                   // wpad_err: none
        WriteF32(e + 0x60, s.sx);                      // Nunchuk stick (ConvertKpadToRaw negates y)
        WriteF32(e + 0x64, s.sy);
        WriteF32(e + 0x68, na.x);
        WriteF32(e + 0x6C, na.y);
        WriteF32(e + 0x70, na.z);
        WriteF32(e + 0x74, std::sqrt(na.x * na.x + na.y * na.y + na.z * na.z));
    }
    if (g.trace) {
        if (hold != g.lastHold || s.ui != g.lastUi)
            RT_LOG(RT_TAG_HLE) << "[opuc] input hold=0x" << std::hex << hold << std::dec << " ui=" << s.ui
                               << " stick=(" << s.sx << "," << s.sy << ")" << (fishing ? " fishing" : "")
                               << " cm=0x" << std::hex << Memory::Read32(0x8068A18Cu) << " camp=0x"
                               << (Memory::Read32(0x8068A18Cu) ? Memory::Read32(Memory::Read32(0x8068A18Cu) + 0x30Cu) : 0)
                               << std::dec << std::endl;
        if (raValue >= 3.0f && g.lastAccValue < 3.0f)
            RT_LOG(RT_TAG_HLE) << "[opuc] input shake pulse acc_value=" << raValue << std::endl;
    }
    g.lastHold = hold;
    g.lastUi = s.ui;
    g.lastAccValue = raValue;
    return n;
}
PPC_NATIVE_OVERRIDE(8030454C, KPADRead_OPUC, int32_t, (uint32_t chan, uint32_t buf, uint32_t length), (chan, buf, length));

// Mouse camera capture, once per presented frame from settings_overlay::Draw (the thread that pumps SDL events, so
// the SDL window calls are safe): relative mouse mode while the field has the controls (no menu, window or pause
// process: the scheme's "ui" flag is clear), the game window has the keyboard focus, the runtime overlay does not
// own input and Alt is not held. Menus get the cursor back.
void OpucInput_UpdateMouseCapture() {
    static SDL_Window* captured = nullptr;
    SDL_Window* focus = SDL_GetKeyboardFocus();
    const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
    const bool field = kb.watching && RuntimeConfigFile::MouseCamera(true) && !InputBindings::InputBlocked() &&
                       !kb.ui.load(std::memory_order_relaxed) && !alt;
    // Test switch OPUC_MOUSE_CAPTURE=0: never capture (scripted runs keep the desktop's mouse; MDX / MDY still turn
    // the camera).
    static const bool noCapture = [] { const char* e = std::getenv("OPUC_MOUSE_CAPTURE"); return e && e[0] == '0'; }();
    const bool want = field && focus && !noCapture;
    static bool noFocusLogged = false;
    if (g.trace && field && !focus && !noFocusLogged) {
        noFocusLogged = true;
        RT_LOG(RT_TAG_HLE) << "[opuc] mouse camera: field, but the window has no keyboard focus (not captured)" << std::endl;
    }
    if (want && captured != focus) {
        if (captured) SDL_SetWindowRelativeMouseMode(captured, false);
        captured = SDL_SetWindowRelativeMouseMode(focus, true) ? focus : nullptr;
        if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] mouse camera: captured" << (captured ? "" : " (failed)") << std::endl;
    } else if (!want && captured) {
        SDL_SetWindowRelativeMouseMode(captured, false);
        captured = nullptr;
        if (g.trace) RT_LOG(RT_TAG_HLE) << "[opuc] mouse camera: released" << std::endl;
    }
    kb.captured.store(captured != nullptr, std::memory_order_relaxed);
}
