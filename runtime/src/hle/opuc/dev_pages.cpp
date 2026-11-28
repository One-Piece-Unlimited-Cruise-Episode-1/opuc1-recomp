// Dev menu pages (not in the game), ported from the Play tab's pauseext.js devPage / debugPage / flagsPage /
// fishingPage / spotTablePage. dev_menu.cpp draws them as the game's text lists; this file says what each row shows
// and what A does, in terms of the game's own state:
//   Debug    the game's DebugFlag names (DebugFlag::IsEnabled 80102514, retail always false; vault
//            entities/debug-flags.md) answered true while switched on, and the Play tab's own switches: Freeze
//            enemies (EnemyManager +0xE, what the script EnemyStop(true) writes), Infinite HP / Infinite SP (any
//            lowering of the leader's HP / SP, current or max, refused where the game writes it: PlayStatus::ApplyValue
//            804d8bb8 and the HP SetMax store), Wii button art ([game] pc_button_art off), State
//            text (a corner readout in the overlay).
//   Flags    the save's flag records, edited in place (vault entities/save-adventure-records.md).
//   Fishing  the fishing switches (fishing_pc.cpp), the spot table of this island, the fully upgraded rod.
#include "dev_pages.h"

#include "ai_view.h"
#include "fishing_pc.h"
#include "guest_call.h"
#include "hle_stubs.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace OpucDev {

void QueueRodGrant();  // dev_menu.cpp: the fully upgraded rod when the pause closes

namespace {

using OpucGuest::Call;

constexpr uint32_t kOn = 0xFFE25A, kOff = 0x9FB4C4;  // Play tab ON / OFF colours

bool GuestPtr(uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }
float F32(uint32_t at) { return Memory::ReadFloat32(at); }

std::u16string U(std::string_view s) { return std::u16string(s.begin(), s.end()); }

Row OnOff(std::u16string label, std::u16string desc, bool on, std::function<void(const CpuContext&)> toggle) {
    Row r;
    r.label = std::move(label);
    r.desc = std::move(desc);
    r.value = on ? u"ON" : u"OFF";
    r.rgb = on ? kOn : kOff;
    r.toggle = std::move(toggle);
    return r;
}

// --- Debug ----------------------------------------------------------------------------------------------------
std::mutex g_mutex;          // the flags below are read by the overlay thread too
std::set<std::string, std::less<>> g_flags;  // DebugFlag names switched on
bool g_freeze = false, g_infiniteHp = false, g_infiniteSp = false, g_stateText = false;
std::string g_stateLine;

bool FlagOn(std::string_view name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_flags.find(name) != g_flags.end();
}

void SetFlag(std::string_view name, bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (on) g_flags.emplace(name);
    else g_flags.erase(std::string(name));
    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: debug flag " << name << (on ? " on" : " off") << std::endl;
}

struct GameFlag { const char* name; const char16_t* desc; };
constexpr GameFlag kGameFlags[] = {
    {"no_enemy", u"Game flag: enemy groups do not spawn (the group spawn loop is skipped); groups already out stay."},
    {"menu_content_all_open", u"Game flag: every pause page and entry is open (the top page from the next time the pause opens)."},
    {"event_test", u"Game flag: the event-test path of the scripts (EventTestWarp moves the player)."},
    {"showhit", u"Game flag: draw the collision around the player and the hit volumes."},
    {"debug_breakrush", u"Game flag: the Break Rush sequence has 2 commands (InitBreakRush 8002e314 / RollSequence 8002e984); from the next leader change or area load."},
    {"debug_breakrush_none", u"Game flag: no Break Rush (InitBreakRush 8002e314); from the next leader change or area load."},
};

std::vector<Row> DebugRows(const CpuContext&) {
    std::vector<Row> rows;
    for (const GameFlag& f : kGameFlags) {
        const std::string name = f.name;
        rows.push_back(OnOff(U(name), f.desc, FlagOn(name), [name](const CpuContext&) { SetFlag(name, !FlagOn(name)); }));
    }
    rows.push_back(OnOff(u"Freeze enemies", u"Not in the game: enemy AI stops (as the script EnemyStop(true)).", g_freeze,
                         [](const CpuContext&) { g_freeze = !g_freeze; if (!g_freeze) Tick(); }));
    rows.push_back(OnOff(u"Infinite HP", u"Not in the game: the leader’s HP never goes down.", g_infiniteHp,
                         [](const CpuContext&) { g_infiniteHp = !g_infiniteHp; }));
    rows.push_back(OnOff(u"Infinite SP", u"Not in the game: the leader’s SP and SP max never go down.", g_infiniteSp,
                         [](const CpuContext&) { g_infiniteSp = !g_infiniteSp; }));
    rows.push_back(OnOff(u"Wii button art",
                         u"Menus, tutorials, the check bubbles, the window prompt and the tool guides show the game\u2019s "
                         u"Wii Remote / Nunchuk buttons. Off (the default): the keys and mouse buttons. A picture already "
                         u"on screen changes when the game loads it again.",
                         !RuntimeConfigFile::PcButtonArt(true),
                         [](const CpuContext&) { RuntimeConfigFile::SetPcButtonArt(!RuntimeConfigFile::PcButtonArt(true)); }));
    rows.push_back(OnOff(u"State text", u"Not in the game: the move state / HP / position readout in the corner.", g_stateText,
                         [](const CpuContext&) { std::lock_guard<std::mutex> lock(g_mutex); g_stateText = !g_stateText; }));
    {
        Row r;
        r.label = u"Enemy AI view";
        r.desc = u"Not in the game: what each enemy’s AI looks at and picks (rings, lines, inspector, planner, log).";
        r.value = OpucAiView::Get().view ? u"ON" : u"OFF";
        r.rgb = OpucAiView::Get().view ? kOn : kOff;
        r.open = OpucAiView::AiPage;
        rows.push_back(r);
    }
    return rows;
}

// --- Fishing ----------------------------------------------------------------------------------------------------
// --- Spot table ---------------------------------------------------------------------------------------------------
// field/fishp.bin as ItemDataManage (0x805E91E0) holds it: +0x60 size, +0x64 rows (0x5C each, count = u32 at size - 8);
// row +0 field AllIdx, +4 spot AllIdx, +8 five {u32 fish id, u16 weight, u16}. Remap(row +0) is the FieldId
// ^[inferred: as the fishing code compares it]; the current field is FieldManager (*(0x8068A218)) +0x20, the index the field
// objects are filtered by (PlayStatus::GetCurrentIsland 800fe06c read 0 on Fst after the skipped opening).
// fish_prm (FindFishPrm 800fabfc): +0x10 / +0x12 size min / range, +0x14 / +0x16 distance min / range (cm).
constexpr uint32_t kItemDataManage = 0x805E91E0;
constexpr uint32_t kRemapMsgId = 0x80102794;
constexpr uint32_t kFindFishPrm = 0x800FABFC;
constexpr uint32_t kFieldManagerPtr = 0x8068A218;
constexpr uint32_t kItemSrcBasePtr = 0x805E91E8;  // item_src rows, 0x224 each, wide name at +0

std::u16string ItemName(uint32_t id) {
    const uint32_t base = Memory::Read32(kItemSrcBasePtr);
    if (!GuestPtr(base) || id >= 0x400) return u"item " + U(std::to_string(id));
    std::u16string s = OpucGuest::ReadWide(base + id * 0x224, 31);
    return s.empty() ? u"item " + U(std::to_string(id)) : s;
}

std::u16string Metres(uint32_t cm) {
    char b[16];
    std::snprintf(b, sizeof(b), cm % 100 ? "%.1f" : "%.0f", cm / 100.0);
    return U(b);
}

std::vector<Row> SpotRows(const CpuContext& c) {
    std::vector<Row> out;
    const uint32_t size = Memory::Read32(kItemDataManage + 0x60), rows = Memory::Read32(kItemDataManage + 0x64);
    const uint32_t fm = Memory::Read32(kFieldManagerPtr);
    if (!GuestPtr(rows) || size < 8 || !GuestPtr(fm)) return out;
    const uint32_t field = Memory::Read32(fm + 0x20), count = std::min<uint32_t>(Memory::Read32(rows + size - 8), 64);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t row = rows + i * 0x5C;
        if (Call(c, kRemapMsgId, {Memory::Read32(row)}) != field) continue;
        const uint32_t spot = Call(c, kRemapMsgId, {Memory::Read32(row + 4)});
        uint32_t sum = 0;
        for (int k = 0; k < 5; ++k) if (Memory::Read32(row + 8 + 8 * k)) sum += Memory::Read16(row + 8 + 8 * k + 4);
        for (int k = 0; k < 5; ++k) {
            const uint32_t raw = Memory::Read32(row + 8 + 8 * k);
            if (!raw) continue;
            const uint32_t w = Memory::Read16(row + 8 + 8 * k + 4), id = Call(c, kRemapMsgId, {raw});
            const uint32_t prm = Call(c, kFindFishPrm, {kItemDataManage, id});
            const std::u16string name = ItemName(id);
            std::u16string dist = u"?", size16;
            if (GuestPtr(prm)) {
                const uint32_t lo = Memory::Read16(prm + 0x14), range = Memory::Read16(prm + 0x16);
                dist = (!lo && !range) ? u"any" : Metres(lo) + u"-" + Metres(lo + range) + u" m";  // the menu font has no en dash
                const uint32_t smin = Memory::Read16(prm + 0x10), srange = Memory::Read16(prm + 0x12);
                size16 = U(std::to_string(smin)) + u"-" + U(std::to_string(smin + srange)) + u" cm";
            }
            Row r;
            r.label = U(std::to_string(spot)) + u" " + name;
            r.value = U(std::to_string(sum ? (100 * w + sum / 2) / sum : 0)) + u" %, " + dist;
            r.rgb = kOn;
            r.desc = u"Spot " + U(std::to_string(spot)) + u": " + name + u", weight " + U(std::to_string(w)) +
                     u", bites at " + dist + (size16.empty() ? u"" : u", size " + size16) + u".";
            out.push_back(r);
        }
    }
    if (out.empty()) {
        Row r;
        r.label = u"No fishing spot on this island";
        out.push_back(r);
    }
    return out;
}

// --- per frame: Freeze enemies, the State text ---------------------------------------------------------------------
// Leader: *(CharaManager (0x8068A18C) + 0x88). PlayObj PO = **(chara + 0x20); status slot PS = **(*(PO + 0x3C) + 0x34);
// PS +0x40 / +0x44 point at the HP / SP PlayStatus (+0 int* current, +4 int* max, +8 int* base max).
constexpr uint32_t kCharaManagerPtr = 0x8068A18C;
constexpr uint32_t kEnemyManagerPtr = 0x8068A1E8;  // +0xE: EnemyStop (CmdEnemyStop 800bb924 writes only that byte)

uint32_t Leader() {
    const uint32_t cm = Memory::Read32(kCharaManagerPtr);
    return GuestPtr(cm) ? Memory::Read32(cm + 0x88) : 0;
}

uint32_t Deref(uint32_t p) { return GuestPtr(p) ? Memory::Read32(p) : 0; }

// The leader's HP (0) or SP (1) PlayStatus, or 0.
uint32_t LeaderStatus(int which) {
    const uint32_t chara = Leader();
    if (!GuestPtr(chara)) return 0;
    const uint32_t po = Deref(Deref(chara + 0x20));
    if (!GuestPtr(po)) return 0;
    const uint32_t ps = Deref(Deref(Deref(po + 0x3C) + 0x34));
    if (!GuestPtr(ps)) return 0;
    const uint32_t st = Deref(ps + 0x40 + 4 * which);
    return GuestPtr(st) && GuestPtr(Deref(st)) && GuestPtr(Deref(st + 4)) ? st : 0;
}

int32_t StatusValue(uint32_t st, int field) { return static_cast<int32_t>(Memory::Read32(Deref(st + 4 * field))); }

bool g_wasFreeze = false;

std::vector<Row> FishingRows(const CpuContext&) {
    OpucFishing::DevSwitches& d = OpucFishing::Dev();
    std::vector<Row> rows;
    rows.push_back(OnOff(u"Assist overlay",
                         u"Phase hints, the cast power bar (predicted distance, where fish bite, the line length), the "
                         u"bite cue and the fight panel.",
                         RuntimeConfigFile::FishingAssist(true),
                         [](const CpuContext&) { RuntimeConfigFile::SetFishingAssist(!RuntimeConfigFile::FishingAssist(true)); }));
    {
        Row r;
        const bool remote = RuntimeConfigFile::FishingRemoteInput(false);
        r.label = u"Input";
        r.value = remote ? u"Remote (mouse)" : u"Simple";
        r.rgb = kOn;
        r.desc = u"Simple: hold the cast key for power, release to throw; A / D lean, W raises. Remote: the mouse moves "
                 u"the Wii Remote (flick to cast, tug to hook, sideways to lean).";
        r.toggle = [](const CpuContext&) { RuntimeConfigFile::SetFishingRemoteInput(!RuntimeConfigFile::FishingRemoteInput(false)); };
        rows.push_back(r);
    }
    rows.push_back(OnOff(u"Auto-hook", u"A real bite is hooked by itself (the 40 ms tip jerk is played for you).",
                         d.autoHook, [](const CpuContext&) { auto& v = OpucFishing::Dev().autoHook; v = !v; }));
    rows.push_back(OnOff(u"Line never breaks", u"Line strength never runs out and a long cast does not snap the line.",
                         d.lineNeverBreaks, [](const CpuContext&) { auto& v = OpucFishing::Dev().lineNeverBreaks; v = !v; }));
    rows.push_back(OnOff(u"Fish always bite",
                         u"The bite rolls always succeed and any of the spot\u2019s fish may bite at any distance.",
                         d.alwaysBite, [](const CpuContext&) { auto& v = OpucFishing::Dev().alwaysBite; v = !v; }));
    rows.push_back(OnOff(u"PC guide",
                         u"The control pictures at the right edge show the bound keys and mouse buttons instead of the "
                         u"Wii Remote (Simple input only). Off: the game\u2019s Wii pictures.",
                         RuntimeConfigFile::FishingPcGuide(true),
                         [](const CpuContext&) { RuntimeConfigFile::SetFishingPcGuide(!RuntimeConfigFile::FishingPcGuide(true)); }));
    rows.push_back(OnOff(u"Remote graph",
                         u"The live Wii Remote swing ring (|acc| per sample, the 1.5 g line), the cast power, quiet "
                         u"samples and the hook gesture flag.",
                         d.remoteGraph, [](const CpuContext&) { auto& v = OpucFishing::Dev().remoteGraph; v = !v; }));
    {
        Row r;
        r.label = u"Spot table";
        r.desc = u"The fishing spots of this island: each fish with its weight, the distance it bites at and its size.";
        r.open = [] {
            auto p = std::make_shared<Page>();
            p->title = u"Spot table";
            p->help = u"Weight, bite distance, size";
            p->valueX = 300;
            p->rows = SpotRows;
            return p;
        };
        rows.push_back(r);
    }
    {
        Row r;
        r.label = u"Give fishing rod";
        r.desc = u"The fishing rod at its maximum upgrade (Sea King Fishing Rod, Rod Lv MAX, Line Lv MAX: 100 m) into "
                 u"the bag, when the pause closes.";
        r.toggle = [](const CpuContext&) { QueueRodGrant(); };
        rows.push_back(r);
    }
    return rows;
}

// --- Flags ------------------------------------------------------------------------------------------------------
// Bank 1 records (SaveDataBank::GetBySlot(1) 8007a298 = *(0x805E8B00 + 4)): a list from bank +0 of nodes {+0 next,
// +8 name, +0xC size, +0x10 data}, the walk SaveDataBank::FindByName 80079dd8 does. The data is edited in place.
constexpr uint32_t kBankSlots = 0x805E8B00;
constexpr uint32_t kSteleList = 0x8068A160;   // StelePointList*: +0 rows, +4 stele.bin rows (0x14), +0xC u8 flags[20]
constexpr uint32_t kBossTable = 0x8050CB70;   // 15 x {CharaId, FieldId, bit}: GameData +0xB344 + FieldId * 4

std::string CString(uint32_t at, size_t max = 32) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const char ch = static_cast<char>(Memory::Read8(at + static_cast<uint32_t>(i)));
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

struct Record { uint32_t data = 0, size = 0; };
Record FindRecord(std::string_view name) {
    const uint32_t bank = Memory::Read32(kBankSlots + 4);
    if (!GuestPtr(bank)) return {};
    uint32_t node = Memory::Read32(bank);
    for (int guard = 0; GuestPtr(node) && guard < 256; ++guard, node = Memory::Read32(node)) {
        const uint32_t nm = Memory::Read32(node + 8);
        if (GuestPtr(nm) && CString(nm) == name) return {Memory::Read32(node + 0x10), Memory::Read32(node + 0xC)};
    }
    return {};
}

// Event flag bit n: big-endian u32 at (n / 32) * 4, bit n % 32 from the LSB.
bool BitGet(uint32_t base, uint32_t n) { return (Memory::Read32(base + (n / 32) * 4) >> (n % 32)) & 1; }
void BitSet(uint32_t base, uint32_t n, bool on) {
    const uint32_t at = base + (n / 32) * 4, v = Memory::Read32(at), m = 1u << (n % 32);
    Memory::Write32(at, on ? (v | m) : (v & ~m));
}

std::u16string FieldLabel(uint32_t f) {
    static const char16_t* kNames[] = {u"Sny", u"Fst", u"Vly", u"Gcr", u"Cvn", u"Cn1"};
    if (f < 6) return kNames[f];
    return u"Field " + U(std::to_string(f));
}

struct Entry { uint32_t key; std::u16string label; };
struct Category {
    std::u16string name, desc;
    std::function<std::vector<Entry>()> list;  // what is set now (bit records) or every entry (fixed tables)
    std::function<bool(uint32_t)> get;
    std::function<void(uint32_t, bool)> set;
    bool keepSeen = true;                       // ids switched off this session stay listed
};

std::map<std::u16string, std::map<uint32_t, std::u16string>> g_seen;

std::vector<Entry> SetBits(std::string_view record) {
    std::vector<Entry> out;
    const Record r = FindRecord(record);
    if (!GuestPtr(r.data)) return out;
    for (uint32_t w = 0; w < r.size / 4; ++w) {
        const uint32_t v = Memory::Read32(r.data + 4 * w);
        for (uint32_t b = 0; v && b < 32; ++b)
            if ((v >> b) & 1) out.push_back({w * 32 + b, U(std::to_string(w * 32 + b))});
    }
    return out;
}

Category BitCategory(std::u16string name, std::u16string desc, std::string record) {
    Category c;
    c.name = std::move(name);
    c.desc = std::move(desc);
    c.list = [record] { return SetBits(record); };
    c.get = [record](uint32_t id) { const Record r = FindRecord(record); return GuestPtr(r.data) && id / 8 < r.size && BitGet(r.data, id); };
    c.set = [record](uint32_t id, bool on) { const Record r = FindRecord(record); if (GuestPtr(r.data) && id / 8 < r.size) BitSet(r.data, id, on); };
    return c;
}

uint32_t GameDataAt(uint32_t off) {
    const Record r = FindRecord("GameData");
    return GuestPtr(r.data) && off < r.size ? r.data + off : 0u;
}

std::vector<Category> Categories() {
    std::vector<Category> cats;
    cats.push_back(BitCategory(u"Story flags", u"FlagOn / FlagOff ids (save record EventFlag, ids 0-511).", "EventFlag"));
    cats.push_back(BitCategory(u"Scenes ended", u"EventScriptEndFlag: scene ids that have played to the end.", "EventScriptEndFlag"));
    cats.push_back(BitCategory(u"Script functions", u"EventFuncEnableFlag: FuncSetEnable(id) of the registered chk_ functions.", "EventFuncEnableFlag"));
    cats.push_back(BitCategory(u"Functions ended", u"EventFuncEndFlag: handlers that have ended with 98 / 99.", "EventFuncEndFlag"));
    {
        // GameData +4: u32 per FieldId, bit = shortcut (GameData::SetShortcutFound 800fe114); key = field * 32 + bit.
        Category c;
        c.name = u"Shortcuts found";
        c.desc = u"FieldShortCutSetFind: the found bits per island (GameData +4).";
        c.list = [] {
            std::vector<Entry> out;
            if (const uint32_t b = GameDataAt(4))
                for (uint32_t f = 0; f < 11; ++f)
                    for (uint32_t bit = 0; bit < 32; ++bit)
                        if (BitGet(b + f * 4, bit)) out.push_back({f * 32 + bit, FieldLabel(f) + u" " + U(std::to_string(bit))});
            return out;
        };
        c.get = [](uint32_t k) { const uint32_t b = GameDataAt(4); return b && BitGet(b, k); };
        c.set = [](uint32_t k, bool on) { if (const uint32_t b = GameDataAt(4)) BitSet(b, k, on); };
        cats.push_back(c);
    }
    {
        // StelePointList: flags[row] & 3 = found and read (every writer, StelePointList::MarkRead 80024d44, sets both).
        Category c;
        c.name = u"Monuments found";
        c.desc = u"SleteSetFind / SteleOpenPoint: the stone monuments found (flags & 3 per stele.bin row).";
        c.keepSeen = false;
        c.list = [] {
            std::vector<Entry> out;
            const uint32_t sl = Memory::Read32(kSteleList);
            if (!GuestPtr(sl)) return out;
            const uint32_t n = std::min<uint32_t>(Memory::Read32(sl), 20), rows = Memory::Read32(sl + 4);
            for (uint32_t i = 0; i < n; ++i)
                out.push_back({i, U("Stele " + std::to_string(GuestPtr(rows) ? Memory::Read32(rows + i * 0x14) : i))});
            return out;
        };
        c.get = [](uint32_t i) {
            const uint32_t sl = Memory::Read32(kSteleList);
            return GuestPtr(sl) && (Memory::Read8(sl + 0xC + i) & 3) != 0;
        };
        c.set = [](uint32_t i, bool on) {
            const uint32_t sl = Memory::Read32(kSteleList);
            if (!GuestPtr(sl)) return;
            const uint32_t at = sl + 0xC + i;
            Memory::Write8(at, static_cast<uint8_t>(on ? (Memory::Read8(at) | 3) : (Memory::Read8(at) & ~3)));
        };
        cats.push_back(c);
    }
    {
        // GameData +0xB344: defeated-boss bits per FieldId, from the table at 8050cb70.
        Category c;
        c.name = u"Bosses defeated";
        c.desc = u"The defeated-boss bits per island (GameData +0xB344, table 8050cb70); two bosses share a bit on Vly and Gcr.";
        c.keepSeen = false;
        c.list = [] {
            static const std::map<uint32_t, const char16_t*> kBoss = {
                {0x0C, u"Kuma"}, {0x0D, u"Eneru"}, {0x18, u"Buggy"}, {0x0E, u"Kuro"}, {0x0F, u"Krieg"}, {0x10, u"Moria"},
                {0x1B, u"Nightmare Luffy"}, {0x12, u"Wapol"}, {0x13, u"Spandam"}, {0x14, u"Aokiji"},
                {0x1D, u"Rampaging Chopper"}, {0x15, u"Ace"}, {0x16, u"Blackbeard"}, {0x1A, u"Evil Guardian"},
                {0x17, u"Doom Guardian"}};
            std::vector<Entry> out;
            for (uint32_t i = 0; i < 15; ++i) {
                const uint32_t id = Memory::Read32(kBossTable + 12 * i), f = Memory::Read32(kBossTable + 12 * i + 4);
                const uint32_t bit = Memory::Read32(kBossTable + 12 * i + 8);
                const auto it = kBoss.find(id);
                out.push_back({f * 32 + bit, (it != kBoss.end() ? std::u16string(it->second) : U("Chara " + std::to_string(id))) +
                                                 u" (" + FieldLabel(f) + u")"});
            }
            return out;
        };
        c.get = [](uint32_t k) { const uint32_t b = GameDataAt(0xB344); return b && BitGet(b, k); };
        c.set = [](uint32_t k, bool on) { if (const uint32_t b = GameDataAt(0xB344)) BitSet(b, k, on); };
        cats.push_back(c);
    }
    return cats;
}

// A category's entries: the set ones now, plus those seen set or switched this session (Play tab flagsPage).
std::vector<Entry> Listed(const Category& c) {
    std::vector<Entry> now = c.list();
    if (!c.keepSeen) return now;
    auto& seen = g_seen[c.name];
    for (const Entry& e : now) seen[e.key] = e.label;
    std::vector<Entry> out;
    for (const auto& [k, label] : seen) out.push_back({k, label});
    return out;
}

std::vector<Row> CategoryRows(const std::u16string& name) {
    std::vector<Row> out;
    for (const Category& c : Categories()) {
        if (name != c.name) continue;
        for (const Entry& e : Listed(c)) {
            const uint32_t k = e.key;
            const auto get = c.get;
            const auto set = c.set;
            out.push_back(OnOff(e.label, c.desc, get(k), [get, set, k, name](const CpuContext&) {
                const bool on = !get(k);
                set(k, on);
                RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: flag " << std::string(name.begin(), name.end()) << " " << k
                                   << (on ? " on" : " off") << " (reads " << get(k) << ")" << std::endl;
            }));
        }
    }
    return out;
}

std::vector<Row> FlagsRows(const CpuContext&) {
    std::vector<Row> rows;
    for (const Category& cat : Categories()) {
        Row r;
        r.label = cat.name;
        r.desc = cat.desc;
        size_t on = 0;
        for (const Entry& e : Listed(cat)) on += cat.get(e.key) ? 1 : 0;
        r.value = U(std::to_string(on));
        r.rgb = kOn;
        const std::u16string name = cat.name, desc = cat.desc;
        r.open = [name, desc] {
            auto p = std::make_shared<Page>();
            p->title = name;
            p->help = desc;
            p->valueX = 260;
            p->rows = [name](const CpuContext&) { return CategoryRows(name); };
            return p;
        };
        rows.push_back(r);
    }
    return rows;
}

}  // namespace

std::shared_ptr<Page> DevPage() {
    auto p = std::make_shared<Page>();
    p->title = u"Dev";
    p->help = u"A: open";
    p->rows = [](const CpuContext&) {
        std::vector<Row> rows(4);
        rows[0].label = u"Debug";
        rows[0].desc = u"Game DebugFlag switches, and switches added by the recompilation.";
        rows[0].open = [] {
            auto q = std::make_shared<Page>();
            q->title = u"Debug";
            q->help = u"A: switch";
            q->rows = DebugRows;
            return q;
        };
        rows[1].label = u"Flags";
        rows[1].desc = u"The save\u2019s story, scene and function flags by category.";
        rows[1].open = [] {
            auto q = std::make_shared<Page>();
            q->title = u"Flags";
            q->help = u"A: open the category";
            q->rows = FlagsRows;
            return q;
        };
        rows[2].label = u"Items";
        rows[2].desc = u"Every valid item: give or take any amount.";
        rows[2].items = true;
        rows[3].label = u"Fishing";
        rows[3].desc = u"Fishing aids and switches: the assist overlay, input mode, auto-hook, the line, the spot table, the rod.";
        rows[3].open = [] {
            auto q = std::make_shared<Page>();
            q->title = u"Fishing";
            q->help = u"A: switch / open";
            q->valueX = 360;
            q->rows = FishingRows;
            return q;
        };
        return rows;
    };
    return p;
}

bool DebugFlagOn(std::string_view name) { return FlagOn(name); }
bool InfiniteHp() { return g_infiniteHp; }
bool InfiniteSp() { return g_infiniteSp; }

// Test helpers. OPUC_TEST_INFINITE="hp,sp" switches Infinite HP / SP on at start. OPUC_TEST_STATUS_HIT=<frames>: that
// many field camera frames in, the leader's gauges get the game's own calls once, AddCurrent(HP, -30),
// SetMax(HP, max / 2), AddCurrent(SP, -40), AddMax(SP, -50), with the values logged before and after.
void TestInit() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = std::getenv("OPUC_TEST_INFINITE")) {
        g_infiniteHp = std::strstr(e, "hp") != nullptr;
        g_infiniteSp = std::strstr(e, "sp") != nullptr;
        RT_LOG(RT_TAG_HLE) << "[opuc] test: Infinite HP " << g_infiniteHp << ", Infinite SP " << g_infiniteSp << std::endl;
    }
}

void TestFieldFrame(const CpuContext& c) {
    static int frames = -1;
    static const char* e = std::getenv("OPUC_TEST_STATUS_HIT");
    if (!e) return;
    if (frames < 0) frames = std::atoi(e);
    if (frames == 0 || --frames != 0) return;
    const uint32_t hp = LeaderStatus(0), sp = LeaderStatus(1);
    if (!hp || !sp) { frames = 1; return; }
    auto line = [&](const char* when) {
        RT_LOG(RT_TAG_HLE) << "[opuc] test: status hit " << when << ": HP " << StatusValue(hp, 0) << "/" << StatusValue(hp, 1)
                           << " SP " << StatusValue(sp, 0) << "/" << StatusValue(sp, 1) << std::endl;
    };
    line("before");
    Call(c, 0x804D8DEC, {hp, static_cast<uint32_t>(-30), 0});               // PlayStatus::AddCurrent
    Call(c, 0x804D8A84, {hp, static_cast<uint32_t>(StatusValue(hp, 1) / 2)}); // PlayStatus::SetMax
    Call(c, 0x804D8DEC, {sp, static_cast<uint32_t>(-40), 0});
    Call(c, 0x804D8B0C, {sp, static_cast<uint32_t>(-50), 0});               // PlayStatus::AddMax
    line("after");
}

std::string StateText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stateText ? g_stateLine : std::string();
}

void Tick() {
    TestInit();
    // Freeze enemies: EnemyManager +0xE as EnemyStop(true / false) writes it; held each frame while on, so a script's
    // EnemyStop(false) does not undo it.
    const uint32_t em = Memory::Read32(kEnemyManagerPtr);
    if (GuestPtr(em)) {
        if (g_freeze) Memory::Write8(em + 0xE, 1);
        else if (g_wasFreeze) Memory::Write8(em + 0xE, 0);
    }
    if (g_freeze != g_wasFreeze)
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: EnemyStop byte " << (GuestPtr(em) ? int(Memory::Read8(em + 0xE)) : -1) << std::endl;
    g_wasFreeze = g_freeze;
    bool want;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        want = g_stateText;
    }
    if (!want) return;
    // The State text: move state *(*(**(chara + 0x3C) + 0x48) + 0x10) (-1 without a state node), HP / SP, position
    // *(**(chara + 0xC) + 0x54) + 0x10.
    std::string line;
    if (const uint32_t chara = Leader(); GuestPtr(chara)) {
        const uint32_t node = Deref(Deref(Deref(chara + 0x3C)) + 0x48);
        const int state = GuestPtr(node) ? static_cast<int32_t>(Memory::Read32(node + 0x10)) : -1;
        char b[160];
        const uint32_t hp = LeaderStatus(0), sp = LeaderStatus(1);
        const uint32_t post = Deref(Deref(Deref(chara + 0xC)) + 0x54);
        std::snprintf(b, sizeof(b), "state %d  HP %d/%d  SP %d/%d", state, hp ? StatusValue(hp, 0) : -1,
                      hp ? StatusValue(hp, 1) : -1, sp ? StatusValue(sp, 0) : -1, sp ? StatusValue(sp, 1) : -1);
        line = b;
        if (GuestPtr(post)) {
            std::snprintf(b, sizeof(b), "\npos %.2f, %.2f, %.2f", F32(post + 0x10), F32(post + 0x14), F32(post + 0x18));
            line += b;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stateLine = line;
}

}  // namespace OpucDev

// Infinite HP / Infinite SP. PlayStatus::ApplyValue(self r3, int* slot r4, int* limit r5, flags* r6, notify r7,
// new absolute value r8, options r9) 804d8bb8 writes every status value: SetCurrent 804d8da0 (and through it
// AddCurrent / AddCurrentB / SetCurrentThunk / IncCurrent: hits, status ticks, ring-out falls, evade and skill costs,
// script natives), AddMax 804d8b0c (the SP max a C+A / C+shake spends, then current), RestoreFull and SetMax call it
// (vault entities/player-locomotion). slot == *(self + 4) is the max, *(self + 0) the current value. For the leader's
// HP / SP a lower request becomes the old value (no decrease flag, no OnDecrease callback), and a value the clamp to
// the limit still lowered is put back after the call. The HP gauge's SetMax 804d8a84 stores *max directly (flag 0x20
// clear) and is refused there. Before 2026-11-28 Infinite SP refilled SP current once a frame, so AddMax still took
// the SP max down and each special move left less, and Invincible refused only SetCurrent.
namespace {
int LeaderGauge(uint32_t self) {
    if (self == OpucDev::LeaderStatus(0)) return 0;
    if (self == OpucDev::LeaderStatus(1)) return 1;
    return -1;
}
bool KeepGauge(int which) { return which == 0 ? OpucDev::InfiniteHp() : which == 1 && OpucDev::InfiniteSp(); }
}  // namespace

extern "C" void OpucDev_PlayStatusApplyValue(CpuContext* ctx) {
    static const bool trace = std::getenv("OPUC_TRACE_HP") != nullptr;
    const uint32_t self = ctx->gpr[3], slot = ctx->gpr[4];
    const int which = (OpucDev::InfiniteHp() || OpucDev::InfiniteSp() || trace) ? LeaderGauge(self) : -1;
    if (which < 0 || !OpucDev::GuestPtr(slot)) {
        GuestFunctions::NextTranslation(0x804D8BB8)(ctx);
        return;
    }
    const bool keep = KeepGauge(which);
    const bool isMax = slot == Memory::Read32(self + 4);
    const int32_t old = static_cast<int32_t>(Memory::Read32(slot));
    const int32_t want = static_cast<int32_t>(ctx->gpr[8]);
    if (keep && want < old) ctx->gpr[8] = static_cast<uint32_t>(old);
    GuestFunctions::NextTranslation(0x804D8BB8)(ctx);
    int32_t now = static_cast<int32_t>(Memory::Read32(slot));
    if (keep && now < old) {
        Memory::Write32(slot, static_cast<uint32_t>(old));
        now = old;
    }
    if (trace && want < old)
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: leader " << (which ? "SP" : "HP") << (isMax ? " max " : " ") << old
                           << " -> " << want
                           << (keep && want < old ? (which ? " refused (Infinite SP)" : " refused (Infinite HP)") : "")
                           << ", now " << now << std::endl;
}
REGISTER_NATIVE_WRAPPER_AS(0x804D8BB8, OpucDev_PlayStatusApplyValue, "OpucDev_PlayStatusApplyValue");

// PlayStatus::SetMax(self r3, value r4) 804d8a84: with flag 0x20 clear (the HP gauge) it stores *max = value without
// ApplyValue (CharaSetHp / CharaSetSp script modes, the form teardown halving).
extern "C" void OpucDev_PlayStatusSetMax(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const int which = (OpucDev::InfiniteHp() || OpucDev::InfiniteSp()) ? LeaderGauge(self) : -1;
    if (which >= 0 && KeepGauge(which)) {
        const uint32_t maxPtr = Memory::Read32(self + 4);
        if (OpucDev::GuestPtr(maxPtr)) {
            const int32_t old = static_cast<int32_t>(Memory::Read32(maxPtr));
            if (static_cast<int32_t>(ctx->gpr[4]) < old) {
                if (std::getenv("OPUC_TRACE_HP"))
                    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: leader " << (which ? "SP" : "HP") << " SetMax " << old
                                       << " -> " << static_cast<int32_t>(ctx->gpr[4]) << " refused" << std::endl;
                ctx->gpr[4] = static_cast<uint32_t>(old);
            }
        }
    }
    GuestFunctions::NextTranslation(0x804D8A84)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x804D8A84, OpucDev_PlayStatusSetMax, "OpucDev_PlayStatusSetMax");

// Fish always bite (Play tab fish_alwaysbite). CharaFishing::PickFishSlot(f1 lineDistM, work, fishpRow) 80036254
// returns a slot 0-4 or -1; in state 3 it runs every tick before the bite timer test, and when the timer (+0x94) is
// under 1 the next RandMT::NextU32 803b4a70 call is the bite roll r = rand % 400 (% 80 after a nibble): r 0 gives a
// nibble, and after a nibble the real bite. With the switch on, no fish at this distance becomes any of the spot's
// fish, and that roll returns 0.
namespace {
bool g_rollZero = false;
}

extern "C" void OpucDev_PickFishSlot(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], row = ctx->gpr[4];
    GuestFunctions::NextTranslation(0x80036254)(ctx);
    if (!OpucFishing::Dev().alwaysBite || !OpucDev::GuestPtr(row)) return;
    if (static_cast<int32_t>(ctx->gpr[3]) < 0) {
        int slots[5], n = 0;
        for (int k = 0; k < 5; ++k)
            if (Memory::Read32(row + 8 + 8 * k)) slots[n++] = k;
        if (n) ctx->gpr[3] = static_cast<uint32_t>(slots[std::rand() % n]);
        if (n && std::getenv("OPUC_TRACE_FISHING"))
            RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: always bite: no fish at this distance, slot " << ctx->gpr[3] << std::endl;
    }
    if (static_cast<int32_t>(ctx->gpr[3]) >= 0 && Memory::Read32(work + 0x1C) == 3 &&
        static_cast<int32_t>(Memory::Read32(work + 0x94)) < 1)
        g_rollZero = true;
}
REGISTER_NATIVE_WRAPPER_AS(0x80036254, OpucDev_PickFishSlot, "OpucDev_PickFishSlot");

extern "C" void OpucDev_RandNextU32(CpuContext* ctx) {
    if (g_rollZero) {
        g_rollZero = false;
        ctx->gpr[3] = 0;
        if (std::getenv("OPUC_TRACE_FISHING")) RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: always bite: bite roll 0" << std::endl;
        return;
    }
    GuestFunctions::NextTranslation(0x803B4A70)(ctx);
    OpucAiView::OnRand(ctx->gpr[3]);
}
REGISTER_NATIVE_WRAPPER_AS(0x803B4A70, OpucDev_RandNextU32, "OpucDev_RandNextU32");

// The overlay (settings_overlay.cpp, ImGui thread): the State text in the top left corner and the Remote graph (Play
// tab fishing.js drawOverlay).
void OpucDev_DrawOverlays() {
    const ImGuiIO& io = ImGui::GetIO();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    OpucAiView::DrawPanels();
    const std::string state = OpucDev::StateText();
    if (!state.empty()) {
        ImGui::SetNextWindowPos(ImVec2(10, 40), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("Dev State Text", nullptr, kFlags)) ImGui::TextUnformatted(state.c_str());
        ImGui::End();
    }
    if (!OpucFishing::Dev().remoteGraph) return;
    const OpucFishing::GraphSnapshot g = OpucFishing::Graph();
    if (!g.active) return;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("Dev Remote Graph", nullptr, kFlags)) {
        ImGui::TextUnformatted("remote |acc| (4 g top, 1.5 g line)");
        const float w = 360, h = 100;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 60; ++i) {
            const float v = g.ring[i], bh = std::min(h, v / 4.f * h);
            dl->AddRectFilled(ImVec2(o.x + i * w / 60, o.y + h - bh), ImVec2(o.x + (i + 1) * w / 60 - 1, o.y + h),
                              v > 1.5f ? IM_COL32(255, 210, 74, 255) : IM_COL32(119, 170, 170, 255));
        }
        dl->AddLine(ImVec2(o.x, o.y + h - 1.5f / 4 * h), ImVec2(o.x + w, o.y + h - 1.5f / 4 * h), IM_COL32(255, 74, 74, 255));
        ImGui::Dummy(ImVec2(w, h));
        if (g.gesture)
            ImGui::TextColored(ImVec4(1, 0.82f, 0.29f, 1), "power %.1f (cast >= 12)  quiet %d  gesture %d", g.power, g.quiet, g.gesture);
        else
            ImGui::Text("power %.1f (cast >= 12)  quiet %d", g.power, g.quiet);
    }
    ImGui::End();
}
