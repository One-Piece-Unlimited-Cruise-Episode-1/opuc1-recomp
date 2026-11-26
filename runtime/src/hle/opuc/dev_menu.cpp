// Dev menu (not in the game): a "Dev" entry under the pause top page, ported from the Play tab
// (programs/apps/play/web/play/pauseext.js, devitems.js): Dev opens a list (Debug, Flags, Items, Fishing; the rows and
// what A does are in dev_pages.cpp). Debug, Flags (and its categories), Fishing (and the spot table) are shown in
// place in the same list; Items opens the Items page.
//
// Everything is built from the game's own classes the way the game builds them (nothing is copied from existing
// objects); the runtime wraps a few guest functions (REGISTER_NATIVE_WRAPPER_AS: the base translation stays and runs
// through GuestFunctions::NextTranslation) and edits their arguments or results. Vault: entities/menus.md sections 4c2-4c5.
//
// Pause top (AquaDesignerPauseMenu, PauseMenu.aar PauseMenuTop 0xA00200):
//   CreateMenuData 80159738   after it: a new UBDW button WBC7, built as the layout loader builds WBC0-6 (see
//                             CreateDevButton), two row pitches below WBC6, decision kind (+0x140) 8, label "Dev".
//   ALText::SetTableText 803c2838  the button's Normal clip (WBC6's sub-layout) carries TableText "Information":
//                             print "Dev" instead.
//   CreateContent 8015910c    kind 8 (unused by the game, IsSlotUnlocked returns true for it): the Dev list.
//   PushTableRow 80180178     the breadcrumb row 8 would be "Cooking": MenuBreadcrumb::Push "Dev".
//   OnReactivate 8015a03c     the game indexes buttons[kind] (7 entries); for kind 8 focus the Dev button.
// Dev list = AquaDesignerTextListMenu built by AquaDesignerSlateList::ConstructStele 801dd354 (SlateList.aar 0xA002D0,
//   as Information > Stone Monuments), with its own vtable (rows, detail, decision, reactivate are natives),
//   CreateListData 801da1f8 told the row count, and a value column (a text node per row, CreateValueNode). A on
//   "Items" builds the Items page in the list's scene and pushes it, as AquaDesignerPauseInfo::DoDecision 8015ad88
//   does for its lists; A on Debug / Flags / Fishing swaps the rows in place and MenuShell::Back 8017fc78 (B) goes
//   back a level (ShowLevel).
// Dev Items page = AquaDesignerMenuItem mode 3 (camp Take Out: ItemMenu.aar BaseMenuItem 0xA002E1, 9 x 5 cells):
//   CreateItemList 8016f6c0   its bag argument becomes a 45-slot fake ItemBag that shows a window of the valid
//                             item ids (1-249 with a translated name), 9 per row; rows scroll through the
//                             widget pad override (+0x108) of the top and bottom row cells.
//   SetupHelpPrompts 801704dc no C Arrange / - Discard prompts.
//   SelectGetItem 801722d8    A: the pause Items page's count window instead (TryBeginDrop 80170a88, with the
//                             game's all_item_drop debug flag through DebugFlag::IsEnabled 80102514), turned into
//                             "Add how many?" 1-99 by NumSelect::Construct 801e876c; the rods 4 / 5 / 6 skip it.
//   NumSelect::Update 801e94a8 A skips the "Are you sure?" step (NumSelect::OnConfirmChoice 801ea1c8).
//   count callback            OnDropCountChosen(0, list) (unlock), then the grant is queued.
//   OnPauseMenuClosed 800404f8 the queued grants run: CharaManager::GrantItemById(cm, id, n, 1, 1), the ground-
//                             pickup grant (INFORMATION line, item-get scripts, picture book); the rod = item 6 at
//                             rod row 32 / line row 30.
//   ItemBagSet::IsSeenInMenu 800f537c / MarkSeenInMenu 800f539c: while the Dev page is open, no New! badges and
//                             browsing does not mark items seen.
// Test switch: OPUC_DEV_MENU=0 / 1 overrides [game] dev_menu.
#include "dev_pages.h"
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <map>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using OpucGuest::Call;

// --- guest addresses (main.dol ROUPAF) ---------------------------------------------------------------------------
constexpr uint32_t kCreateMenuData = 0x80159738;
constexpr uint32_t kCreateContent = 0x8015910C;
constexpr uint32_t kPauseTopOnReactivate = 0x8015A03C;
constexpr uint32_t kPushTableRow = 0x80180178;
constexpr uint32_t kBreadcrumbPush = 0x80180288;   // (const u16* text, void** srcTextHandle, int drawSlot, parent)
constexpr uint32_t kBreadcrumbPop = 0x80180C04;
constexpr uint32_t kBindButtonEvents = 0x8017E304; // (content, buttonHandle)
constexpr uint32_t kGetStateMotionId = 0x803F2370; // (widget, state)
constexpr uint32_t kBindClipById = 0x803747D4;     // ALMotion::BindClipById(widget, clip, 0)
constexpr uint32_t kSetFocus = 0x803FEA90;         // ALWidget::SetFocus(widget, 1)
constexpr uint32_t kPrintfW = 0x803C199C;          // ALText::PrintfW(textHandle, fmt, ...)
constexpr uint32_t kSePlayId = 0x801AA1E8;
constexpr uint32_t kAlloc = 0x80356154;            // (size, file, func, line), global heap
constexpr uint32_t kAllocFile = 0x8050EEEE;        // "AquaDesignerPauseMenu.cpp"
constexpr uint32_t kAllocFunc = 0x8059D8B8;        // "CreateContent"
constexpr uint32_t kMenuItemConstruct = 0x8016DE20; // (obj 0x90, mode, cursorOwner, prio)
constexpr uint32_t kCreateItemList = 0x8016F6C0;
constexpr uint32_t kSetupHelpPrompts = 0x801704DC;
constexpr uint32_t kSelectGetItem = 0x801722D8;
constexpr uint32_t kSyncFocusedWidget = 0x80182AC4;
constexpr uint32_t kGetFocusedCell = 0x80182B80;
constexpr uint32_t kRefireFocusCallback = 0x80182BA0; // (list, cellWidgetHandle)
constexpr uint32_t kListRefresh = 0x80184E68;      // AquaDesignerItemList::Refresh(list): re-binds every cell
constexpr uint32_t kSetTableText = 0x803C2838;     // ALText::SetTableText(textNode, table, key, textCol, keyCol)
constexpr uint32_t kNumSelectConstruct = 0x801E876C;
constexpr uint32_t kNumSelectUpdate = 0x801E94A8;
constexpr uint32_t kNumSelectOnConfirmChoice = 0x801EA1C8;
constexpr uint32_t kSceneSuspend = 0x803B5080;     // called by NumSelect::Update before its confirm window
constexpr uint32_t kGrantItemById = 0x80040AC4;    // (cm, id, n, showUi, allowOverflow)
constexpr uint32_t kGrantItemFromSpec = 0x80040B4C; // (cm, ItemRef*, n, showUi, allowOverflow)
constexpr uint32_t kItemRefInit = 0x800F8934;      // (ref, id, extra0, extra1)
constexpr uint32_t kItemRefDestruct = 0x800F8958;  // (ref, -1)
constexpr uint32_t kFindMenuByName = 0x8007AAAC;
constexpr uint32_t kOnPauseMenuClosed = 0x800404F8; // CharaManager::OnPauseMenuClosed(cm)
constexpr uint32_t kIsSeenInMenu = 0x800F537C;
constexpr uint32_t kMarkSeenInMenu = 0x800F539C;

constexpr uint32_t kCharaManagerPtr = 0x8068A18C;  // r13-0x5D14
constexpr uint32_t kMenuShellPtr = 0x8068A594;     // r13-0x590C: shell, contents +0x14[4], count +0x24
constexpr uint32_t kItemSrcBasePtr = 0x805E91E8;   // ItemDataManage +8: item_src rows, 0x224 each, name at +0
constexpr uint32_t kItemSrcRowSize = 0x224;
constexpr uint32_t kPadNodeEnabled = 0x8067DD80;   // u32 count; the pad node is **0x8067DD7C (NumSelect::Update)
constexpr uint32_t kPadNodePtr = 0x8067DD7C;
constexpr uint32_t kNameItemBag = 0x80687F48;      // "ItemBag"
constexpr uint32_t kNameItemEquip = 0x8051CAA4;    // "ItemEquip"

// A guest address with no code: the runtime registers the row-scroll native here so the game's widget code can
// call it through widget +0x108 like any guest callback.
constexpr uint32_t kRowScrollCallback = 0x80003FF0;
constexpr uint32_t kCountChosenCallback = 0x80003FE0;  // the Dev number window's onChosen(count, list)
constexpr uint32_t kTryBeginDrop = 0x80170A88;     // AquaDesignerMenuItem::TryBeginDrop(self): the pause count window
constexpr uint32_t kOnDropCountChosen = 0x80170F7C; // (count, list): count 0 = unlock only
constexpr uint32_t kDebugFlagIsEnabled = 0x80102514;
constexpr uint32_t kFlagAllItemDrop = 0x8050F2DF;  // "all_item_drop"

// Building the button (as ALAssembleHandler::Assemble 8032d420 / WindowSelect::Build 801e6224 do).
constexpr uint32_t kWidgetButtonConstruct = 0x803D5A48; // ALWidgetButton::Construct(obj, 0, skinId, drawStateFn)
constexpr uint32_t kCreateDrawState = 0x801E7088;    // ALWidget::CreateDrawState
constexpr uint32_t kAllocWidgetFile = 0x805DB044;    // "ALWidgetImp.h" (ALWidgetButton::CreateImp's allocation)
constexpr uint32_t kAllocWidgetFunc = 0x805D7D08;    // "CreateImpFunc"
constexpr uint32_t kLookupObjectByFileId = 0x8032D0D8; // ALResource::LookupObjectByFileId(parent, fileId): Assemble
constexpr uint32_t kButtonSubLayout = 0xA00210;      // WBC6's Assemble / WidgetNormalMotion (TopMenuAdvInfo)
constexpr uint32_t kSetOwner = 0x803FE494;           // ALWidget::SetOwner(widget, owner)
constexpr uint32_t kSetStateMotion = 0x803F2004;     // ALWidget::SetStateMotion(widget, state, clip)
constexpr uint32_t kApplyDefaultTextStyle = 0x8017B36C; // AquaDesigner::ApplyDefaultTextStyle(textHandle, font)
constexpr uint32_t kGetFontOrDefault = 0x8017F1F0;   // AquaDesignerMenuContent::GetFontOrDefault(shell)
// The Dev list (as AquaDesignerPauseInfo::DoDecision 8015ad88 builds Stone Monuments).
constexpr uint32_t kConstructStele = 0x801DD354;     // AquaDesignerSlateList::ConstructStele(obj 0x114)
constexpr uint32_t kSteleVtable = 0x8059F55C;
constexpr uint32_t kTextListVtableSize = 0x5C;
constexpr uint32_t kDevPageVtable = 0x80002500;      // low memory, after the fake bag block (0x80002000-0x800024FF)
constexpr uint32_t kCreateListData = 0x801DA1F8;
constexpr uint32_t kPushContent = 0x8017FA20;        // MenuShell::PushContent(shell, content)
constexpr uint32_t kScreenPushCurrent = 0x803B4F64;
constexpr uint32_t kScreenSetCurrentScene = 0x803B5024;
constexpr uint32_t kScreenPopCurrent = 0x803B4F94;
constexpr uint32_t kSceneStackIndex = 0x806897E0;    // r13-0x66C0
constexpr uint32_t kSceneStack = 0x8067EDB8;
constexpr uint32_t kCurrentScene = 0x8068B2D0;       // r13-0x4BD0
// Natives at code-free guest addresses, called by the game through the Dev list's vtable.
constexpr uint32_t kDevPageDecision = 0x80003FA0;
constexpr uint32_t kDevPageOnReactivate = 0x80003FB0;
constexpr uint32_t kDevPageRefreshRows = 0x80003FC0;
constexpr uint32_t kDevPageShowDetail = 0x80003FD0;
constexpr uint32_t kSeOpen = 0x20000E5C;
constexpr uint32_t kPanelBaseLayout = 0xA002D7;      // SlateList.aar PanelBase: the list scroll bar and EXPL
constexpr uint32_t kCreateRootContainer = 0x8036FA08; // ALNode::CreateRootContainer() -> handle
constexpr uint32_t kPagerConstruct = 0x8018100C;     // MenuListPager::Construct(pager 0x30)
constexpr uint32_t kPagerBindBarNodes = 0x80181090;  // (pager, SBAR, BARU, BARD, BARC handles)
constexpr uint32_t kPagerReset = 0x80181150;         // (pager, top, visible, total)
constexpr uint32_t kPagerScrollUp = 0x801811F4;      // (pager, step): thumb, arrow nudge, SE
constexpr uint32_t kPagerScrollDown = 0x80181420;
constexpr uint32_t kDevPager = 0x80002560;           // low memory, after the Dev list vtable copy (0x80002500..55B)
constexpr uint32_t kNameSBAR = 0x52414253, kNameBARU = 0x55524142, kNameBARD = 0x44524142, kNameBARC = 0x43524142;
constexpr uint32_t kNameEXPL = 0x4C505845;
constexpr uint32_t kSlateRowOpenClip = 0xA002D4;     // SlateList.aar "CotnentOpen" (sic)
constexpr uint32_t kSlatePaneOpenClip = 0xA002D8;    // SlateList.aar PanelBaseOpen

constexpr uint32_t kDevKind = 8;
constexpr uint32_t kWbc7 = 0x37434257;  // node names are packed low byte first: "WBC7"
constexpr uint32_t kTxtm = 0x4D545854;  // "TXTM"
constexpr float kRowPitch = 26.0f;

constexpr uint32_t kCols = 9, kRows = 5, kCells = kCols * kRows;
constexpr uint32_t kRodItem = 6, kRodMaxRow = 32, kLineMaxRow = 30;  // Play tab state.js ROD
constexpr uint32_t kSeMove = 0x2000156B, kSeAdd = 0x20001566, kSeBack = 0x20000E5F, kSeBuzz = 0x20001560;

// --- state ------------------------------------------------------------------------------------------------------
// The fake bag, its slots and the number-window text live in low memory the game and the runtime leave unused
// (0x80001800-0x80002FFF, between the exception vectors and the OS globals at 0x80003000). A block from the game's
// allocator, kept across pause sessions, ended up inside memory the next session reused (heap corruption on the
// third pause open, 2026-11-24).
constexpr uint32_t g_mem = 0x80002000;  // 0x500 bytes
uint32_t g_topContent = 0;     // the pause top content the Dev button was added to
uint32_t g_devNode = 0;        // WBC7
uint32_t g_devTxtmHandle = 0;
uint32_t g_devItem = 0;        // the Dev Items AquaDesignerMenuItem
uint32_t g_devList = 0;
bool g_titlePending = false;
bool g_inSelect = false;
uint32_t g_selectId = 0;
uint32_t g_top = 0;            // first visible row
std::vector<uint32_t> g_valid; // valid item ids
// Grants chosen on the Dev page, run when the pause closes (OnPauseMenuClosed). A grant inside the pause menu builds
// HUD / script objects under the pause scene, which the close frees (heap corruption seen 2026-11-24); the game never
// grants items while its pause menu is open. id kRodGrant = the fully upgraded rod.
struct PendingGrant { uint32_t id; uint32_t count; };
constexpr uint32_t kRodGrant = 0xFFFFu;
std::vector<PendingGrant> g_pending;
uint32_t g_rowHandles[2][kCols] = {};  // cell widget handles of the top (0) and bottom (1) rows
uint32_t g_prevOverride[2][kCols] = {};

uint32_t Bag() { return g_mem; }
uint32_t Slots() { return g_mem + 0x20; }
uint32_t TextBuf() { return g_mem + 0x300; }  // 0x200 bytes

bool Enabled() {
    if (const char* env = std::getenv("OPUC_DEV_MENU")) return env[0] != '0';  // overrides [game] dev_menu
    return RuntimeConfigFile::DevMenuEnabled(true);
}

uint32_t VCall(const CpuContext& c, uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> more = {}) {
    std::vector<uint32_t> args{obj};
    args.insert(args.end(), more.begin(), more.end());
    const uint32_t fn = Memory::Read32(Memory::Read32(obj + 4) + slot);
    switch (args.size()) {
    case 1: return Call(c, fn, {args[0]});
    case 2: return Call(c, fn, {args[0], args[1]});
    case 3: return Call(c, fn, {args[0], args[1], args[2]});
    default: return Call(c, fn, {args[0], args[1], args[2], args[3]});
    }
}

// node -> its handle (node +0x20, created through vtbl+0x18 on first use); *handle == node.
uint32_t HandleOf(const CpuContext& c, uint32_t node) {
    if (!node) return 0;
    if (!Memory::Read32(node + 0x20)) VCall(c, node, 0x18);
    return Memory::Read32(node + 0x20);
}

uint32_t Alloc(const CpuContext& c, uint32_t size) { return Call(c, kAlloc, {size, kAllocFile, kAllocFunc, 0x80}); }

uint32_t WriteScratchWide(const CpuContext& c, std::u16string_view s) {
    const uint32_t at = OpucGuest::ScratchBelow(c, static_cast<uint32_t>(s.size() * 2 + 2));
    OpucGuest::WriteWide(at, s);
    return at;
}

void PlaySe(const CpuContext& c, uint32_t id) { Call(c, kSePlayId, {id}); }

// ALText::PrintfW appends; the game clears a text first (draw state node+0x78 vtbl+0x6C, then node vtbl+0xD0), as
// ALText::SetTableText and NumSelect::Update do.
void SetLabel(const CpuContext& c, uint32_t textNode, uint32_t textHandle, std::u16string_view text) {
    VCall(c, Memory::Read32(textNode + 0x78), 0x6C);
    VCall(c, textNode, 0xD0);
    Call(c, kPrintfW, {textHandle, WriteScratchWide(c, text)});
}

// The transform at node +0x54: position x +0x10 / y +0x14; dirty marking as AquaDesignerItemList::BuildGrid does.
void MoveNodeY(const CpuContext& c, uint32_t node, float y) {
    const uint32_t t = Memory::Read32(node + 0x54);
    Memory::WriteFloat32(t + 0x14, y);
    if ((Memory::Read8(t + 0x30) & 0x80) == 0) VCall(c, t, 0x1C);
    Memory::Write8(t + 0x30, Memory::Read8(t + 0x30) | 0x88);
    const uint32_t parent = Memory::Read32(t + 0x24);
    Memory::Write8(parent + 0x30, Memory::Read8(parent + 0x30) | 4);
}

// --- item list ----------------------------------------------------------------------------------------------------
uint32_t ItemRow(uint32_t id) { return Memory::Read32(kItemSrcBasePtr) + id * kItemSrcRowSize; }

std::u16string ItemName(uint32_t id) { return OpucGuest::ReadWide(ItemRow(id), 31); }

// Play tab devitems.js validItems: ids 1-249 whose name has no CJK / full-width characters (the 35 rows that keep a
// Japanese name in the English table are cut content).
void BuildValidList() {
    g_valid.clear();
    if (!Memory::Read32(kItemSrcBasePtr)) return;
    for (uint32_t id = 1; id < 250; ++id) {
        const std::u16string name = ItemName(id);
        if (name.empty()) continue;
        const bool cjk = std::any_of(name.begin(), name.end(), [](char16_t ch) {
            return (ch >= 0x3000 && ch <= 0x9FFF) || (ch >= 0xFF00 && ch <= 0xFFEF);
        });
        if (!cjk) g_valid.push_back(id);
    }
}

uint32_t RowCount() { return static_cast<uint32_t>((g_valid.size() + kCols - 1) / kCols); }
uint32_t MaxTop() { return RowCount() > kRows ? RowCount() - kRows : 0; }

// How many of `id` the bag and the pallet hold, plus the Dev grants still waiting for the pause to close.
uint32_t HeldCount(const CpuContext& c, uint32_t id) {
    uint32_t n = 0;
    for (const PendingGrant& g : g_pending) {
        if (g.id == id) n += g.count;
    }
    for (uint32_t name : {kNameItemBag, kNameItemEquip}) {
        const uint32_t bag = Call(c, kFindMenuByName, {name});
        if (!bag) continue;
        const uint32_t used = Memory::Read32(bag), slots = Memory::Read32(bag + 8);
        for (uint32_t i = 0; i < used && i < 64; ++i) {
            if (Memory::Read32(slots + i * 0x10) == id) n += Memory::Read8(slots + i * 0x10 + 4);
        }
    }
    return n;
}

// The fake ItemBag (0x18): +0 used, +4 allocated, +8 slots, +0xC change callback, +0x10 owner set, +0x14 flag;
// slot 0x10: +0 id, +4 count (u8), +8 extra0, +0xC extra1. A count of 0 blanks the icon, so a cell shows the
// held count, at least 1 (the game prints it only for items that stack, maxHave >= 2).
void FillBag(const CpuContext& c) {
    const uint32_t bag = Bag();
    Memory::Write32(bag + 0x00, kCells);
    Memory::Write32(bag + 0x04, kCells);
    Memory::Write32(bag + 0x08, Slots());
    Memory::Write32(bag + 0x0C, 0);
    Memory::Write32(bag + 0x10, 0);
    Memory::Write32(bag + 0x14, 0);
    for (uint32_t k = 0; k < kCells; ++k) {
        const uint32_t i = g_top * kCols + k;
        const uint32_t id = i < g_valid.size() ? g_valid[i] : 0;
        const uint32_t s = Slots() + k * 0x10;
        const uint32_t count = id ? std::clamp<uint32_t>(HeldCount(c, id), 1, 99) : 0;
        Memory::Write32(s + 0x0, id);
        Memory::Write32(s + 0x4, count << 24);  // count byte at +4
        Memory::Write32(s + 0x8, 0);
        Memory::Write32(s + 0xC, 0);
    }
}

bool DevPageOpen() {
    if (!g_devItem) return false;
    const uint32_t shell = Memory::Read32(kMenuShellPtr);
    if (!shell) return false;
    const uint32_t n = std::min<uint32_t>(Memory::Read32(shell + 0x24), 4);
    for (uint32_t i = 0; i < n; ++i) {
        if (Memory::Read32(shell + 0x14 + 4 * i) == g_devItem) return true;
    }
    return false;
}

// --- pause top --------------------------------------------------------------------------------------------------
// The Dev button, built the way the layout loader builds WBC0-6 (ALAssembleHandler::Assemble 8032d420) and the way
// WindowSelect::Build 801e6224 builds a button in code; nothing is copied from another button. PauseMenuTop's data
// for a button WBCn: UBDW widget, ParentNodeID = WidgetOwner = WPTP, Assemble = WidgetNormalMotion = 0xA0020A + n
// (a TopMenuAdvInfo sub-layout: one TXTM), Focus 0xA00206, Decision 0xA00205, Invalid 0xA00209, Open 0xA00207,
// Close 0xA00208. WBC7 uses WBC6's sub-layout 0xA00210: there is no sub-layout or PauseMenuText row for "Dev", so
// the label is printed in code and the TableText that clip carries is answered in OpucDev_SetTableText.
void CreateDevButton(const CpuContext& c, uint32_t content) {
    const uint32_t h0 = Memory::Read32(content + 0x48), h6 = Memory::Read32(content + 0x60);
    const uint32_t layout = Memory::Read32(content + 0x20);
    if (!h0 || !h6 || !layout) return;
    const uint32_t first = Memory::Read32(h0), w6 = Memory::Read32(h6);
    const uint32_t root = Memory::Read32(Memory::Read32(layout + 8));     // layout root node
    const uint32_t pane = Memory::Read32(Memory::Read32(layout + 0x10));  // WPTP

    // ALWidgetButton::CreateImp 803f395c allocates 0x150 and constructs with the current skin id; the pane-layout
    // load builds WBC0-6 with a temporary skin, so this button gets no skin (-1) and WBC6's skin id (+0xCC).
    const uint32_t w = Call(c, kAlloc, {0x150, kAllocWidgetFile, kAllocWidgetFunc, 0x37});
    if (!w) return;
    Call(c, kWidgetButtonConstruct, {w, 0, 0xFFFFFFFFu, kCreateDrawState});
    Memory::Write32(w + 0xCC, Memory::Read32(w6 + 0xCC));
    Memory::Write32(w + 0x58, kWbc7);
    VCall(c, root, 0xC0, {w});                 // container AddChild (FindNode from the root finds it)
    VCall(c, w, 0x10);                         // drop the creation reference, as the loader does
    Call(c, kLookupObjectByFileId, {w, kButtonSubLayout});  // Assemble: TXTM under the button
    VCall(c, w, 0xE0, {pane});                 // ParentNodeID = WPTP
    Call(c, kSetOwner, {w, pane});             // WidgetOwner = WPTP: open / close / focus fan-out
    VCall(c, w, 0xE4, {1});                    // SetEnable
    // Position: two row pitches below WBC6, one empty row apart, like the camp hub's "Pause Menu" entry
    // (Play tab pauseext.js). Set through the node's "Position" property, as ALAssembleHandler::Assemble and the
    // layout's constant motion set it (node vtbl+0x48 FindProperty, prop vtbl+0x3C SetValue): a direct posture write
    // (+0x10/+0x14, as CreateCellParts does for a plain container) left the drawn position at the pane origin, because
    // a widget's posture keeps its own copies (+0x58 / +0x70) that only the property path fills.
    const uint32_t t6 = Memory::Read32(w6 + 0x54), t = Memory::Read32(w + 0x54);
    if (!t6 || !t) {
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: no posture on the new button" << std::endl;
        return;
    }
    const float y = Memory::ReadFloat32(t6 + 0x14) + 2 * kRowPitch;
    {
        const uint32_t scratch = OpucGuest::ScratchBelow(c, 0x30);  // "Position\0" + cookie + value
        const char kName[] = "Position";
        for (uint32_t i = 0; i < sizeof(kName); ++i) Memory::Write8(scratch + i, static_cast<uint8_t>(kName[i]));
        const uint32_t cookie = scratch + 0x10, value = scratch + 0x18;
        Memory::Write32(cookie, 0);
        Memory::WriteFloat32(value + 0, Memory::ReadFloat32(t6 + 0x10));
        Memory::WriteFloat32(value + 4, y);
        Memory::WriteFloat32(value + 8, Memory::ReadFloat32(t6 + 0x18));
        const uint32_t prop = VCall(c, w, 0x48, {scratch, cookie, 0xFFFFFFFFu});
        if (prop) {
            Call(c, Memory::Read32(Memory::Read32(prop + 4) + 0x3C), {prop, w, value});
        } else {
            RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: no Position property on the new button" << std::endl;
            MoveNodeY(c, w, y);
        }
    }
    // Widget*Motion: the clip ids PauseMenuTop gives every button, on the motion states each property sets
    // (ALWidget::SetStateMotion 803f2004).
    struct StateClip { uint32_t state, clip; };
    static constexpr StateClip kMotions[] = {
        {0, kButtonSubLayout}, {2, kButtonSubLayout}, {1, kButtonSubLayout},  // Normal
        {3, 0xA00206},                                                          // Focus
        {7, 0xA00205}, {5, 0xA00205}, {6, 0xA00205}, {4, 0xA00205},             // Decision
        {8, 0xA00209}, {10, 0xA00209},                                          // Invalid
        {0x10, 0xA00207}, {0x11, 0xA00207}, {0x12, 0xA00207}, {0x13, 0xA00207}, {0x18, 0xA00207}, {0x1A, 0xA00207},
        {0x20, 0xA00208}, {0x21, 0xA00208}, {0x22, 0xA00208}, {0x23, 0xA00208}, {0x28, 0xA00208}, {0x2A, 0xA00208},
    };
    for (const StateClip& m : kMotions) Call(c, kSetStateMotion, {w, m.state, m.clip});
    // What CreateMenuData does per button.
    const uint32_t txtm = VCall(c, w, 0xB4, {kTxtm});  // FindNode
    const uint32_t txtmHandle = HandleOf(c, txtm);
    if (txtmHandle) {
        Call(c, kApplyDefaultTextStyle, {txtmHandle, Call(c, kGetFontOrDefault, {Memory::Read32(kMenuShellPtr)})});
    }
    Memory::Write32(w + 0x140, kDevKind);
    const uint32_t handle = HandleOf(c, w);
    Call(c, kBindButtonEvents, {content, handle});
    Call(c, kBindClipById, {w, Call(c, kGetStateMotionId, {w, 0}), 0});
    VCall(c, w6, 0x1D8, {2, w});       // ALWidget::SetLink: WBC6 Down
    VCall(c, w, 0x1D8, {1, w6});       // Up
    VCall(c, w, 0x1D8, {2, first});    // Down
    VCall(c, first, 0x1D8, {1, w});    // WBC0 Up
    g_devTxtmHandle = txtmHandle;
    if (txtmHandle) SetLabel(c, txtm, txtmHandle, u"Dev");
    g_topContent = content;
    g_devNode = w;
    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: Dev button 0x" << std::hex << w << " label 0x" << txtm << " y "
                       << std::dec << y << std::endl;
}

// --- the Dev list page ----------------------------------------------------------------------------------------------
// The game's text-list page (AquaDesignerTextListMenu, 0x114) as Information > Stone Monuments builds it
// (AquaDesignerSlateList::ConstructStele 801dd354: SlateList.aar, layout 0xA002D0, 6 rows, scroll bar, EXPL), with
// its own vtable: a copy of Stele's (8059f55c) whose decision, reactivate, row and detail slots are natives at
// code-free guest addresses. The Dev list and its nested lists (Debug, Flags and its categories, Fishing, the spot
// table) are all such pages; their rows come from dev_pages.cpp (OpucDev::Page).

void PushScene(uint32_t screenHandle, uint32_t sceneHandle, const CpuContext& c) {
    Call(c, kScreenPushCurrent);
    Call(c, kScreenSetCurrentScene, {Memory::Read32(screenHandle)});
    const uint32_t i = Memory::Read32(kSceneStackIndex) - 1;
    Memory::Write32(kSceneStackIndex, i);
    Memory::Write32(kSceneStack + 4 * i, Memory::Read32(kCurrentScene));
    Memory::Write32(kCurrentScene, Memory::Read32(sceneHandle));
}

void PopScene(const CpuContext& c) {
    const uint32_t i = Memory::Read32(kSceneStackIndex);
    Memory::Write32(kCurrentScene, Memory::Read32(kSceneStack + 4 * i));
    Memory::Write32(kSceneStackIndex, i + 1);
    Call(c, kScreenPopCurrent);
}

// One Dev list page (an AquaDesignerTextListMenu) and what it shows.
struct ListState {
    std::shared_ptr<OpucDev::Page> page;
    std::vector<OpucDev::Row> rows;
    uint32_t count = 1;          // rows given to CreateListData (the pager's total)
    uint32_t valu[10] = {};      // the value text node handle of each visible row
    std::vector<std::shared_ptr<OpucDev::Page>> stack;  // the levels above the one shown (B goes back to them)
};
std::map<uint32_t, ListState> g_lists;

ListState* ListOf(uint32_t self) {
    const auto it = g_lists.find(self);
    return it == g_lists.end() ? nullptr : &it->second;
}

uint32_t CreateListPage(const CpuContext& c, std::shared_ptr<OpucDev::Page> spec) {
    const uint32_t obj = Call(c, kAlloc, {0x114, kAllocFile, kAllocFunc, 0x80});
    if (!obj) return 0;
    ListState st;
    st.page = std::move(spec);
    st.rows = st.page->rows(c);
    st.count = std::max<uint32_t>(1, static_cast<uint32_t>(st.rows.size()));
    g_lists[obj] = st;  // before the build coroutine reaches CreateListData
    const uint32_t page = Call(c, kConstructStele, {obj});
    for (uint32_t o = 0; o < kTextListVtableSize; o += 4) {
        Memory::Write32(kDevPageVtable + o, Memory::Read32(kSteleVtable + o));
    }
    Memory::Write32(kDevPageVtable + 0x24, kDevPageDecision);
    Memory::Write32(kDevPageVtable + 0x30, kDevPageOnReactivate);
    Memory::Write32(kDevPageVtable + 0x50, kDevPageRefreshRows);
    Memory::Write32(kDevPageVtable + 0x54, kDevPageShowDetail);
    Memory::Write32(page, kDevPageVtable);
    if (page != obj) {
        g_lists[page] = std::move(g_lists[obj]);
        g_lists.erase(obj);
    }
    return page;
}

// The Dev Items page (AquaDesignerMenuItem mode 3), built in the caller's current scene.
uint32_t CreateItemsPage(const CpuContext& c) {
    if (g_valid.empty()) BuildValidList();
    g_top = 0;
    FillBag(c);
    const uint32_t obj = Alloc(c, 0x90);
    g_devItem = obj;  // before Construct: its build may call CreateItemList right away
    g_devList = 0;
    const uint32_t content = obj ? Call(c, kMenuItemConstruct, {obj, 3, 0, 0x5C}) : 0;
    g_devItem = content;
    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: Items page 0x" << std::hex << content << std::dec << ", "
                       << g_valid.size() << " items" << std::endl;
    return content;
}

// Clears a text node and prints `text` (handle-based, as the list rows hold handles).
void SetTextHandle(const CpuContext& c, uint32_t textHandle, std::u16string_view text) {
    if (!textHandle) return;
    const uint32_t node = Memory::Read32(textHandle);
    if (!node) return;
    SetLabel(c, node, textHandle, text);
}

void InstallRowScroll(const CpuContext& c, uint32_t list) {
    const uint32_t rows = Memory::Read8(list + 0x5C), cols = Memory::Read8(list + 0x5D);
    const uint32_t rowTable = Memory::Read32(list + 0x64);
    if (rows != kRows || cols != kCols || !rowTable) {
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: unexpected grid " << rows << "x" << cols << std::endl;
        return;
    }
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t row = Memory::Read32(rowTable + 4 * (side ? kRows - 1 : 0));
        for (uint32_t col = 0; col < kCols; ++col) {
            const uint32_t handle = Memory::Read32(row + col * 0x28 + 8);
            const uint32_t widget = handle ? Memory::Read32(handle) : 0;
            g_rowHandles[side][col] = handle;
            g_prevOverride[side][col] = widget ? Memory::Read32(widget + 0x108) : 0;
            if (widget) Memory::Write32(widget + 0x108, kRowScrollCallback);
        }
    }
    (void)c;
}

// The scroll bar of the game's text lists, built for the Items grid: SlateList.aar's PanelBase layout (0xA002D7:
// SBAR bar004, BARC bar002 thumb, BARU / BARD bar_cursor arrows, EXPL) instantiated under the grid's layout root the
// way AquaDesignerListBase::CreateCellParts builds its New! badge (CreateRootContainer, LookupObjectByFileId, clip,
// AddChild / SetParent), and driven by a MenuListPager as AquaDesignerTextListMenu drives its own
// (CreateListData: BindBarNodes + Reset; Update: ScrollUp / ScrollDown). SlateList.aar is loaded while the Dev list
// (ConstructStele) exists, which is always below the Items page. The Play tab draws the same layout
// (devitems.js, SBAR at (568, 144) right of the grid).
bool g_pagerLive = false;

uint32_t FindChild(const CpuContext& c, uint32_t node, uint32_t name) {
    return node ? VCall(c, node, 0xB4, {name}) : 0;  // FindNode
}

void CreateScrollBar(const CpuContext& c, uint32_t list) {
    g_pagerLive = false;
    const uint32_t rootHandle = Memory::Read32(list + 8);  // the grid layout root (AquaDesignerItemList::BuildGrid)
    const uint32_t root = rootHandle ? Memory::Read32(rootHandle) : 0;
    const uint32_t containerHandle = Call(c, kCreateRootContainer);
    const uint32_t container = containerHandle ? Memory::Read32(containerHandle) : 0;
    if (!root || !container) return;
    Call(c, kLookupObjectByFileId, {container, kPanelBaseLayout});
    Call(c, kBindClipById, {container, kSlatePaneOpenClip, 0});  // PanelBaseOpen: fades in with the page
    VCall(c, root, 0xC0, {container});  // AddChild
    VCall(c, container, 0x10);          // drop the creation reference
    VCall(c, container, 0xE0, {root});  // SetParent
    if (const uint32_t expl = FindChild(c, container, kNameEXPL)) VCall(c, expl, 0xE4, {0});  // the grid has its own
    const uint32_t bar = HandleOf(c, FindChild(c, container, kNameSBAR));
    const uint32_t up = HandleOf(c, FindChild(c, container, kNameBARU));
    const uint32_t down = HandleOf(c, FindChild(c, container, kNameBARD));
    const uint32_t thumb = HandleOf(c, FindChild(c, container, kNameBARC));
    if (!bar || !up || !down || !thumb) {
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: scroll bar nodes missing" << std::endl;
        return;
    }
    Call(c, kPagerConstruct, {kDevPager});
    Call(c, kPagerBindBarNodes, {kDevPager, bar, up, down, thumb});
    Call(c, kPagerReset, {kDevPager, g_top, kRows, RowCount()});
    g_pagerLive = true;
    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: scroll bar, " << RowCount() << " rows" << std::endl;
}

void RefreshList(const CpuContext& c, uint32_t focusHandle) {
    FillBag(c);
    Call(c, kListRefresh, {g_devList});  // list vtable +0x10 (content-style objects keep the vtable at +0)
    if (focusHandle) Call(c, kRefireFocusCallback, {g_devList, focusHandle});
}

// The one rod item 6 at its maximum upgrade (Play tab fishing.js giveRod): a held rod (bag or pallet) is upgraded
// in place, otherwise one is granted.
void GiveMaxRod(const CpuContext& c) {
    for (uint32_t name : {kNameItemBag, kNameItemEquip}) {
        const uint32_t bag = Call(c, kFindMenuByName, {name});
        if (!bag) continue;
        const uint32_t used = Memory::Read32(bag), slots = Memory::Read32(bag + 8);
        for (uint32_t i = 0; i < used; ++i) {
            const uint32_t s = slots + i * 0x10;
            if (Memory::Read32(s) == kRodItem && Memory::Read8(s + 4)) {
                Memory::Write32(s + 0x8, kRodMaxRow);
                Memory::Write32(s + 0xC, kLineMaxRow);
                return;
            }
        }
    }
    const uint32_t ref = OpucGuest::ScratchBelow(c, 0x10);
    Call(c, kItemRefInit, {ref, kRodItem, kRodMaxRow, kLineMaxRow});
    Call(c, kGrantItemFromSpec, {Memory::Read32(kCharaManagerPtr), ref, 1, 1, 1});
    Call(c, kItemRefDestruct, {ref, 0xFFFFFFFFu});
}

uint32_t FocusedId(const CpuContext& c, uint32_t menuItem) {
    const uint32_t list = Memory::Read32(menuItem + 0x4C);
    if (!list) return 0;
    Call(c, kSyncFocusedWidget, {list});
    const uint32_t cell = Call(c, kGetFocusedCell, {list});
    return cell ? Memory::Read32(cell) : 0;
}

bool APressed() {
    if (!Memory::Read32(kPadNodeEnabled)) return false;
    const uint32_t node = Memory::Read32(Memory::Read32(kPadNodePtr));
    const uint32_t handle = node ? Memory::Read32(node + 0x20) : 0;
    const uint32_t o = handle ? Memory::Read32(handle) : 0;
    if (!o) return false;
    const uint32_t cur = Memory::Read32(o + 0x30 + 4 * Memory::Read32(o + 0x98));
    const uint32_t prev = Memory::Read32(o + 0x30 + 4 * Memory::Read32(o + 0x9C));
    return (cur & ~prev & 0x10) != 0;
}

} // namespace

// --- wrappers -----------------------------------------------------------------------------------------------------
extern "C" void OpucDev_CreateMenuData(CpuContext* ctx) {
    const uint32_t content = ctx->gpr[3];
    g_devNode = 0;
    g_devTxtmHandle = 0;
    g_topContent = 0;
    GuestFunctions::NextTranslation(kCreateMenuData)(ctx);
    if (Enabled()) CreateDevButton(*ctx, content);
}
REGISTER_NATIVE_WRAPPER_AS(0x80159738, OpucDev_CreateMenuData, "OpucDev_CreateMenuData");

// TopDoDecision runs CreateContent inside the top page's scene, as the Info page builds Stone Monuments in its own.
extern "C" void OpucDev_CreateContent(CpuContext* ctx) {
    if (ctx->gpr[3] != kDevKind || !g_devNode) {
        GuestFunctions::NextTranslation(kCreateContent)(ctx);
        return;
    }
    const uint32_t page = CreateListPage(*ctx, OpucDev::DevPage());
    g_titlePending = page != 0;
    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: Dev list 0x" << std::hex << page << std::dec << std::endl;
    ctx->gpr[3] = page;
}
REGISTER_NATIVE_WRAPPER_AS(0x8015910C, OpucDev_CreateContent, "OpucDev_CreateContent");

extern "C" void OpucDev_PushTableRow(CpuContext* ctx) {
    if (g_titlePending && ctx->gpr[3] == 0xA00200 && ctx->gpr[4] == kDevKind) {
        g_titlePending = false;
        const CpuContext c = *ctx;
        Call(c, kBreadcrumbPush, {WriteScratchWide(c, u"Dev"), g_devTxtmHandle, c.gpr[6], c.gpr[7]});
        return;
    }
    GuestFunctions::NextTranslation(kPushTableRow)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80180178, OpucDev_PushTableRow, "OpucDev_PushTableRow");

// AquaDesignerTextListMenu::CreateListData(self, layoutId, totalRows) 801da1f8, from Stele's build coroutine with the
// stone monument count: the Dev list gives its row count (the pager hides the scroll bar for fewer than 6).
// SlateList.aar has open clips for its rows ("CotnentOpen" 0xA002D4, sic) and its pane (PanelBaseOpen 0xA002D8), but
// layout 0xA002D0 never assigns them as WidgetOpenMotion: rows and pane carry only Normal / Focus / Invalid / Close
// clips (dumped 2026-11-25). The game never re-opens a text list after a child page, so it never shows; the Dev list
// does, and without them the close fade (ContentClose / PanelBaseClose) left the list blank after Back. The motion
// states are those of the WidgetOpenMotion property (ALWidgetOpenMotionProp::SetValue 8040bd5c).
// The value column's text node, made as AquaDesignerListBase::CreateCellParts 80181c40 makes its count text, inside the
// list's screen (the caller pushes it: a draw state joins the draw list of the screen current when it is built,
// r13-0x4BD0, and built outside it drew in another pass, behind the pane). ALText::Create 803c275c(0) (the factory
// ALText::CreateImpFunc 803eebf4, 0x90 bytes, starts disabled), the menu's default text style, a draw key one step
// nearer than the row's NAME (SetDrawKey 80340f4c keeps key >> 4 as a u16 at draw state +2; ALDrawList::Insert
// 803b7720 draws high keys first), AddChild to the layout root and SetParent under NAME, the position written into
// the posture (+0x54: +0x10 x, dirty bits as AquaDesignerItemList::BuildGrid), its own alpha, SetEnable.
constexpr uint32_t kTextCreate = 0x803C275C;

static uint32_t CreateValueNode(const CpuContext& c, uint32_t layoutRoot, uint32_t nameNode, float x) {
    if (!nameNode || !layoutRoot) return 0;
    const uint32_t n = Call(c, kTextCreate, {0});
    if (!n) return 0;
    const uint32_t h = HandleOf(c, n);
    Call(c, kApplyDefaultTextStyle, {h, Call(c, kGetFontOrDefault, {Memory::Read32(kMenuShellPtr)})});
    const uint32_t nameDs = Memory::Read32(nameNode + 0x78);
    const uint32_t nameKey = nameDs ? static_cast<uint32_t>(Memory::Read16(nameDs + 2)) << 4 : 0x5A000;
    const uint32_t key = OpucGuest::ScratchBelow(c, 8);
    Memory::Write32(key, nameKey > 0x1000 ? nameKey - 0x1000 : nameKey);
    VCall(c, Memory::Read32(n + 0x78), 0x14, {key});
    VCall(c, Memory::Read32(n + 0x78), 0x6C);
    VCall(c, n, 0xD0);
    VCall(c, layoutRoot, 0xC0, {n});  // AddChild, then the creation reference dropped
    VCall(c, n, 0x10);
    VCall(c, n, 0xE0, {nameNode});    // SetParent
    const uint32_t t = Memory::Read32(n + 0x54);
    if (t) {
        Memory::WriteFloat32(t + 0x10, x);
        Memory::WriteFloat32(t + 0x14, 0);
        Memory::WriteFloat32(t + 0x18, 0);
        if ((Memory::Read8(t + 0x30) & 0x80) == 0) VCall(c, t, 0x1C);
        Memory::Write8(t + 0x30, Memory::Read8(t + 0x30) | 0x88);
        const uint32_t parent = Memory::Read32(t + 0x24);
        if (parent) Memory::Write8(parent + 0x30, Memory::Read8(parent + 0x30) | 4);
    }
    // Its own alpha: a node made in code draws with the one shared default material, whose alpha is whatever any such
    // node was given last (vault entities/events.md "Alpha source"). SetAlpha (node vtbl+0x138, ALNode::SetAlpha
    // 80374fd8) gives it a private colour block at 1.
    Call(c, Memory::Read32(Memory::Read32(n + 4) + 0x138), {n}, {1.0});
    VCall(c, n, 0xE4, {1});
    return h;
}

// The value's text and colour. A solid font colour is what ALText::PropSetFontColor 803c86d8 writes into the
// ALTextGC (node +0x78): upper +0x108 = lower +0x118 (RGBA floats), gradient +0x104 = 0, dirty +0x101 = 1.
static void SetValueText(const CpuContext& c, uint32_t h, std::u16string_view text, uint32_t rgb) {
    const uint32_t n = h ? Memory::Read32(h) : 0;
    if (!n) return;
    std::u16string escaped;
    for (char16_t ch : text) {
        if (ch == u'%') escaped.push_back(u'%');
        escaped.push_back(ch);
    }
    SetLabel(c, n, h, escaped);
    const uint32_t gs = Memory::Read32(n + 0x78);
    if (!gs) return;
    const float col[4] = {rgb ? ((rgb >> 16) & 255) / 255.f : 1.f, rgb ? ((rgb >> 8) & 255) / 255.f : 1.f,
                          rgb ? (rgb & 255) / 255.f : 1.f, 1.f};
    for (int i = 0; i < 4; ++i) {
        Memory::WriteFloat32(gs + 0x108 + 4 * i, col[i]);
        Memory::WriteFloat32(gs + 0x118 + 4 * i, col[i]);
    }
    Memory::Write8(gs + 0x104, 0);
    Memory::Write8(gs + 0x101, 1);
}

static void GiveOpenMotion(const CpuContext& c, uint32_t widget, uint32_t clip) {
    if (!widget) return;
    for (uint32_t state : {0x10u, 0x11u, 0x12u, 0x13u, 0x18u, 0x1Au}) Call(c, kSetStateMotion, {widget, state, clip});
}

extern "C" void OpucDev_CreateListData(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    ListState* st = ListOf(self);
    if (!st) {
        GuestFunctions::NextTranslation(kCreateListData)(ctx);
        return;
    }
    const CpuContext c = *ctx;
    ctx->gpr[5] = st->count;
    GuestFunctions::NextTranslation(kCreateListData)(ctx);
    st = ListOf(self);
    if (!st) return;
    // Only rows that hold an entry: the empty rows stay Invalid, and with an open clip on them the pane's Open state
    // did not finish, so the later Close (Items pushed) never took and the list stayed over the Items page.
    const uint32_t visible = std::min<uint32_t>({Memory::Read32(self + 0x54), 10u, st->count});
    for (uint32_t i = 0; i < visible; ++i) {
        const uint32_t buttonHandle = Memory::Read32(self + 0x58 + 0xC * i);
        GiveOpenMotion(c, buttonHandle ? Memory::Read32(buttonHandle) : 0, kSlateRowOpenClip);
    }
    const uint32_t layout = Memory::Read32(self + 0x20);
    const uint32_t paneHandle = layout ? Memory::Read32(layout + 0x10) : 0;
    GiveOpenMotion(c, paneHandle ? Memory::Read32(paneHandle) : 0, kSlatePaneOpenClip);
    // The value column (Play tab LayoutListPage: a text node VALU under each row's NAME, so it follows the focus
    // slide).
    const uint32_t rows = std::min<uint32_t>(Memory::Read32(self + 0x54), 10u);
    const uint32_t rootHandle = layout ? Memory::Read32(layout + 8) : 0;
    const uint32_t root = rootHandle ? Memory::Read32(rootHandle) : 0;
    PushScene(Memory::Read32(self + 0x10), Memory::Read32(self + 0x14), c);
    for (uint32_t i = 0; i < rows; ++i) {
        const uint32_t nameHandle = Memory::Read32(self + 0x58 + 0xC * i + 4);
        st->valu[i] = CreateValueNode(c, root, nameHandle ? Memory::Read32(nameHandle) : 0, st->page->valueX);
    }
    PopScene(c);
}
REGISTER_NATIVE_WRAPPER_AS(0x801DA1F8, OpucDev_CreateListData, "OpucDev_CreateListData");

// Dev list vtbl+0x50 RefreshRows(self), after AquaDesignerSlateList::RefreshSteleRows 801dd58c: visible row i shows
// record i + pager top; rows past the end are Invalid (AddState 8), which the cursor skips. The rows are asked again
// each time, so values follow what A changed.
static void RefreshListRows(const CpuContext& c, uint32_t self) {
    ListState* st = ListOf(self);
    if (!st) return;
    st->rows = st->page->rows(c);
    const uint32_t visible = std::min<uint32_t>(Memory::Read32(self + 0x54), 10);
    const uint32_t top = Memory::Read32(self + 0xD0);
    for (uint32_t i = 0; i < visible; ++i) {
        const uint32_t row = self + 0x58 + 0xC * i;
        const uint32_t buttonHandle = Memory::Read32(row), nameHandle = Memory::Read32(row + 4);
        const uint32_t button = buttonHandle ? Memory::Read32(buttonHandle) : 0;
        if (!button) continue;
        const uint32_t record = i + top;
        const OpucDev::Row* r = record < st->rows.size() ? &st->rows[record] : nullptr;
        if (record < st->count) {
            VCall(c, button, 0x1C4, {8});  // RemoveState(Invalid)
            SetTextHandle(c, nameHandle, r ? std::u16string_view(r->label) : std::u16string_view(u"None"));
            if (nameHandle) VCall(c, Memory::Read32(nameHandle), 0xE4, {1});
            VCall(c, button, 0xE4, {1});   // enabled again after a decision disabled it
            SetValueText(c, st->valu[i], r ? r->value : std::u16string(), r ? r->rgb : 0);
        } else {
            SetTextHandle(c, nameHandle, u"");
            SetValueText(c, st->valu[i], std::u16string(), 0);
            VCall(c, button, 0x1C0, {8});  // AddState(Invalid)
        }
    }
}

extern "C" void OpucDev_PageRefreshRows(CpuContext* ctx) {
    const CpuContext c = *ctx;
    RefreshListRows(c, c.gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80003FC0, OpucDev_PageRefreshRows, "OpucDev_PageRefreshRows");

// Break a help line at spaces to at most `max` characters per line (Play tab pauseext.js wrap).
static std::u16string WrapText(std::u16string_view t, size_t max) {
    std::u16string out, line;
    size_t i = 0;
    while (i <= t.size()) {
        const size_t j = std::min(t.find(u' ', i), t.size());
        const std::u16string_view w = t.substr(i, j - i);
        if (!line.empty() && line.size() + 1 + w.size() > max) {
            out += line;
            out += u'\n';
            line.assign(w);
        } else {
            if (!line.empty()) line += u' ';
            line += w;
        }
        i = j + 1;
    }
    out += line;
    return out;
}

// Dev list vtbl+0x54 ShowDetail(self, visibleRow): the EXPL line (+0x44), as RefreshSteleRows' detail 801dd6f0.
static void ShowListDetail(const CpuContext& c, uint32_t self, uint32_t visibleRow) {
    ListState* st = ListOf(self);
    const uint32_t expl = Memory::Read32(self + 0x44);
    if (!st || !expl) return;
    const uint32_t record = visibleRow + Memory::Read32(self + 0xD0);
    std::u16string text = record < st->rows.size() ? st->rows[record].desc : std::u16string();
    if (text.empty()) text = st->rows.empty() ? u"None set yet." : st->page->help;
    std::u16string escaped;
    for (char16_t ch : WrapText(text, 50)) {
        if (ch == u'%') escaped.push_back(u'%');
        escaped.push_back(ch);
    }
    SetTextHandle(c, expl, escaped);
    VCall(c, Memory::Read32(expl), 0xE4, {1});
}

extern "C" void OpucDev_PageShowDetail(CpuContext* ctx) {
    const CpuContext c = *ctx;
    ShowListDetail(c, c.gpr[3], c.gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80003FD0, OpucDev_PageShowDetail, "OpucDev_PageShowDetail");

// The Dev list's nested lists (Debug, Flags and its categories, Fishing, the spot table) are shown in place, in the same
// AquaDesignerTextListMenu: A swaps the rows and pushes the title, B (MenuShell::Back, below) goes back a level. A text
// list re-opened after a child page with its scroll bar shown never finished its Close on the next B (rows and pane
// in Close with their looping Normal motion still running, 2026-11-26), and the game itself never re-opens a text list;
// the Dev list (no scroll bar) is re-opened only after the Items page.
static void SetNodeX(const CpuContext& c, uint32_t node, float x) {
    const uint32_t t = node ? Memory::Read32(node + 0x54) : 0;
    if (!t) return;
    Memory::WriteFloat32(t + 0x10, x);
    if ((Memory::Read8(t + 0x30) & 0x80) == 0) VCall(c, t, 0x1C);
    Memory::Write8(t + 0x30, Memory::Read8(t + 0x30) | 0x88);
    const uint32_t parent = Memory::Read32(t + 0x24);
    if (parent) Memory::Write8(parent + 0x30, Memory::Read8(parent + 0x30) | 4);
}

static void ShowLevel(const CpuContext& c, uint32_t self) {
    ListState* st = ListOf(self);
    if (!st) return;
    st->rows = st->page->rows(c);
    st->count = std::max<uint32_t>(1, static_cast<uint32_t>(st->rows.size()));
    const uint32_t visible = std::min<uint32_t>(Memory::Read32(self + 0x54), 10);
    Call(c, kPagerReset, {self + 0xD0, 0, visible, st->count});  // MenuListPager at +0xD0: top 0, the new total
    for (uint32_t i = 0; i < visible; ++i) SetNodeX(c, st->valu[i] ? Memory::Read32(st->valu[i]) : 0, st->page->valueX);
    RefreshListRows(c, self);
    const uint32_t buttonHandle = Memory::Read32(self + 0x58);
    Memory::Write8(self + 4, Memory::Read8(self + 4) | 0x10);
    if (buttonHandle) Call(c, kSetFocus, {Memory::Read32(buttonHandle), 1});
    Memory::Write8(self + 4, Memory::Read8(self + 4) & 0xEF);
    ShowListDetail(c, self, 0);
}

// Dev list vtbl+0x24 decision(self, buttonHandle), after AquaDesignerPauseInfo::DoDecision 8015ad88. Items: disable the
// button, build the Items page inside the list's scene, push it, push its title from the row label, SE. A nested list:
// shown in place. A switch: change it, refresh the rows and the detail line.
extern "C" void OpucDev_PageDecision(CpuContext* ctx) {
    const CpuContext c = *ctx;
    const uint32_t self = c.gpr[3], buttonHandle = c.gpr[4];
    const uint32_t button = buttonHandle ? Memory::Read32(buttonHandle) : 0;
    ListState* st = ListOf(self);
    if (!button || !st) return;
    const uint32_t visibleRow = Memory::Read32(button + 0x140);
    const uint32_t record = visibleRow + Memory::Read32(self + 0xD0);
    // A row that does nothing (the "None" row of an empty list) is refreshed like a switch: the refresh enables the
    // button again (button vtbl+0xE4), which the decision may have left disabled.
    if (record >= st->rows.size()) {
        RefreshListRows(c, self);
        PlaySe(c, kSeBuzz);
        return;
    }
    const OpucDev::Row row = st->rows[record];
    if (row.toggle) {
        row.toggle(c);
        RefreshListRows(c, self);
        ShowListDetail(c, self, visibleRow);
        PlaySe(c, kSeOpen);
        return;
    }
    const uint32_t nameHandle = Memory::Read32(self + 0x58 + 0xC * visibleRow + 4);
    if (row.open) {
        st->stack.push_back(st->page);
        st->page = row.open();
        Call(c, kBreadcrumbPush, {WriteScratchWide(c, row.label), nameHandle, 0x66, 0});
        ShowLevel(c, self);
        PlaySe(c, kSeOpen);
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: opened \"" << std::string(row.label.begin(), row.label.end()) << "\" ("
                           << st->rows.size() << " rows)" << std::endl;
        return;
    }
    if (!row.items) {
        RefreshListRows(c, self);
        PlaySe(c, kSeBuzz);
        return;
    }
    VCall(c, button, 0xE4, {0});
    PushScene(Memory::Read32(self + 0x10), Memory::Read32(self + 0x14), c);
    const uint32_t sub = CreateItemsPage(c);
    PopScene(c);
    if (!sub) return;
    Call(c, kPushContent, {Memory::Read32(kMenuShellPtr), sub});
    Call(c, kBreadcrumbPush, {WriteScratchWide(c, row.label), nameHandle, 0x66, 0});
    PlaySe(c, kSeOpen);
}
REGISTER_NATIVE_FUNCTION_AS(0x80003FA0, OpucDev_PageDecision, "OpucDev_PageDecision");

// Dev list vtbl+0x30 OnReactivate(self, closingAll), after AquaDesignerPauseInfo::OnReactivate 8015af94: the Items page
// was popped (Back) or the whole pause closes (closingAll 1).
extern "C" void OpucDev_PageOnReactivate(CpuContext* ctx) {
    const CpuContext c = *ctx;
    const uint32_t self = c.gpr[3];
    g_devItem = 0;
    g_pagerLive = false;
    g_devList = 0;
    Call(c, kBreadcrumbPop);
    if (c.gpr[4] != 0) {
        g_lists.erase(self);
        return;
    }
    // The re-open that follows (MenuShell::Back: pane AddState Open) runs OnOpen 801dab10: RefreshRows, focus row 0,
    // detail.
    const uint32_t buttonHandle = Memory::Read32(self + 0x58);
    Memory::Write8(self + 4, Memory::Read8(self + 4) | 0x10);
    if (buttonHandle) Call(c, kSetFocus, {Memory::Read32(buttonHandle), 1});
    Memory::Write8(self + 4, Memory::Read8(self + 4) & 0xEF);
    PlaySe(c, kSeBack);
}
REGISTER_NATIVE_FUNCTION_AS(0x80003FB0, OpucDev_PageOnReactivate, "OpucDev_PageOnReactivate");

// MenuShell::Back(shell) 8017fc78 (B): with a Dev list on top that shows a nested list, go back one level in place.
extern "C" void OpucDev_ShellBack(CpuContext* ctx) {
    const uint32_t shell = ctx->gpr[3];
    const uint32_t n = std::min<uint32_t>(Memory::Read32(shell + 0x24), 4);
    const uint32_t top = n ? Memory::Read32(shell + 0x14 + 4 * (n - 1)) : 0;
    ListState* st = ListOf(top);
    if (!st || st->stack.empty()) {
        GuestFunctions::NextTranslation(0x8017FC78)(ctx);
        return;
    }
    const CpuContext c = *ctx;
    st->page = st->stack.back();
    st->stack.pop_back();
    Call(c, kBreadcrumbPop);
    ShowLevel(c, top);
    PlaySe(c, kSeBack);
}
REGISTER_NATIVE_WRAPPER_AS(0x8017FC78, OpucDev_ShellBack, "OpucDev_ShellBack");


extern "C" void OpucDev_PauseTopOnReactivate(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    if (Memory::Read32(self + 0x64) != kDevKind || !g_devNode || self != g_topContent) {
        GuestFunctions::NextTranslation(kPauseTopOnReactivate)(ctx);
        return;
    }
    // AquaDesignerPauseTop::OnReactivate with the Dev button in place of buttons[kind].
    const CpuContext c = *ctx;
    g_devItem = 0;
    g_pagerLive = false;
    g_devList = 0;
    g_lists.clear();
    Call(c, kBreadcrumbPop);
    if (c.gpr[4] == 0) {
        Memory::Write8(self + 4, Memory::Read8(self + 4) | 0x10);
        Call(c, kSetFocus, {g_devNode, 1});
        Memory::Write8(self + 4, Memory::Read8(self + 4) & 0xEF);
        PlaySe(c, kSeBack);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8015A03C, OpucDev_PauseTopOnReactivate, "OpucDev_PauseTopOnReactivate");

extern "C" void OpucDev_CreateItemList(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    if (!g_devItem || self != g_devItem) {
        GuestFunctions::NextTranslation(kCreateItemList)(ctx);
        return;
    }
    const CpuContext c = *ctx;
    ctx->gpr[5] = Bag();
    GuestFunctions::NextTranslation(kCreateItemList)(ctx);
    const uint32_t count = Memory::Read32(self + 0x78);
    g_devList = count ? Memory::Read32(self + 0x4C + 4 * (count - 1)) : 0;
    if (g_devList) {
        InstallRowScroll(c, g_devList);
        CreateScrollBar(c, g_devList);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8016F6C0, OpucDev_CreateItemList, "OpucDev_CreateItemList");

extern "C" void OpucDev_SetupHelpPrompts(CpuContext* ctx) {
    if (g_devItem && ctx->gpr[3] == g_devItem) {
        ctx->gpr[4] = 0;
        ctx->gpr[5] = 0;
    }
    GuestFunctions::NextTranslation(kSetupHelpPrompts)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x801704DC, OpucDev_SetupHelpPrompts, "OpucDev_SetupHelpPrompts");

extern "C" void OpucDev_SelectGetItem(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    if (!g_devItem || self != g_devItem) {
        GuestFunctions::NextTranslation(kSelectGetItem)(ctx);
        return;
    }
    const CpuContext c = *ctx;
    const uint32_t id = FocusedId(c, self);
    if (id >= 4 && id <= 6) {
        g_pending.push_back({kRodGrant, 1});
        PlaySe(c, kSeAdd);
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: fishing rod at max level (when the pause closes)" << std::endl;
        return;
    }
    // Not the camp's take-out (SelectGetItem builds its number window in the camp menu system's scene, which the
    // pause tears down under it: heap corruption on the next pause, 2026-11-24). The pause Items page's own count
    // window, TryBeginDrop, builds it in the page's scene; its discard checks pass through the game's
    // all_item_drop debug flag, and NumSelect::Construct below turns it into "Add how many?".
    g_selectId = id;
    g_inSelect = true;
    Call(c, kTryBeginDrop, {self});
    g_inSelect = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x801722D8, OpucDev_SelectGetItem, "OpucDev_SelectGetItem");

// DebugFlag::IsEnabled(name) (retail: always false; vault entities/debug-flags.md). During the Dev decision,
// "all_item_drop" lets TryBeginDrop accept any item (it would refuse key items with "Can't discard this item.").
// The names switched on from the Dev Debug page answer true; the game keeps several copies of some names, so the
// string is compared, not its address.
extern "C" void OpucDev_DebugFlagIsEnabled(CpuContext* ctx) {
    if (g_inSelect && ctx->gpr[3] == kFlagAllItemDrop) {
        ctx->gpr[3] = 1;
        return;
    }
    if (ctx->gpr[3] >= 0x80000000u && ctx->gpr[3] < 0x81800000u) {
        char name[48];
        size_t n = 0;
        for (; n + 1 < sizeof(name); ++n) {
            name[n] = static_cast<char>(Memory::Read8(ctx->gpr[3] + static_cast<uint32_t>(n)));
            if (!name[n]) break;
        }
        if (n + 1 < sizeof(name) && OpucDev::DebugFlagOn(std::string_view(name, n))) {
            ctx->gpr[3] = 1;
            return;
        }
    }
    GuestFunctions::NextTranslation(kDebugFlagIsEnabled)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80102514, OpucDev_DebugFlagIsEnabled, "OpucDev_DebugFlagIsEnabled");

extern "C" void OpucDev_NumSelectConstruct(CpuContext* ctx) {
    if (g_inSelect && g_selectId) {
        // r6 max, r7 min (the start value), r8 the count callback (OnDropCountChosen -> ours), r10 the wchar_t
        // text (printed with ALText::PrintfW).
        ctx->gpr[8] = kCountChosenCallback;
        std::u16string text = u"Add how many?\n";
        for (char16_t ch : ItemName(g_selectId)) {
            if (ch == u'%') text.push_back(u'%');
            text.push_back(ch);
        }
        OpucGuest::WriteWide(TextBuf(), text.substr(0, 0xFE));
        ctx->gpr[6] = 99;
        ctx->gpr[7] = 1;
        ctx->gpr[10] = TextBuf();
    }
    GuestFunctions::NextTranslation(kNumSelectConstruct)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x801E876C, OpucDev_NumSelectConstruct, "OpucDev_NumSelectConstruct");

extern "C" void OpucDev_NumSelectUpdate(CpuContext* ctx) {
    const uint32_t ns = ctx->gpr[3];
    // ns words: [6] value, [0x13] onChosen, [0x15] its user data (the Dev list).
    if (g_devList && Memory::Read32(ns + 0x13 * 4) == kCountChosenCallback &&
        Memory::Read32(ns + 0x15 * 4) == g_devList &&
        static_cast<int32_t>(Memory::Read32(ns + 6 * 4)) > 0 && APressed()) {
        // A with a count: what "Yes" on the game's confirm window does, without the window.
        const CpuContext c = *ctx;
        Call(c, kSceneSuspend, {Memory::Read32(Memory::Read32(ns + 4)), 0x7FFFFFFF});
        Call(c, kNumSelectOnConfirmChoice, {0, ns});
        return;
    }
    GuestFunctions::NextTranslation(kNumSelectUpdate)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x801E94A8, OpucDev_NumSelectUpdate, "OpucDev_NumSelectUpdate");

// The Dev count callback (NumSelect onChosen(count, list), registered at a code-free guest address). It runs the
// game's OnDropCountChosen with count 0 (its cancel path: unlock the page, re-enable closing, discard nothing),
// then queues the grant.
extern "C" void OpucDev_OnCountChosen(CpuContext* ctx) {
    const int32_t value = static_cast<int32_t>(ctx->gpr[3]);
    const uint32_t list = ctx->gpr[4];
    const CpuContext c = *ctx;
    const uint32_t id = g_selectId;
    Call(c, kOnDropCountChosen, {0, list});
    if (value > 0 && id) {
        g_pending.push_back({id, static_cast<uint32_t>(value)});
        PlaySe(c, kSeAdd);
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: give item " << id << " x" << value << " (when the pause closes)"
                           << std::endl;
        if (list == g_devList) RefreshList(c, 0);  // the cell counts include the queued grants
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80003FE0, OpucDev_OnCountChosen, "OpucDev_OnCountChosen");

// widget +0x108: fn(selfHandle, padBits, &targetHandle). Up on the top row / Down on the bottom row scroll the
// window one row and cancel the move (target 0); at the ends the window jumps to the other end and the game's
// own wrap link moves the cursor there.
extern "C" void OpucDev_RowScroll(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3], pad = ctx->gpr[4], targetPtr = ctx->gpr[5];
    int side = -1;
    uint32_t col = 0;
    for (uint32_t s = 0; s < 2 && side < 0; ++s) {
        for (uint32_t k = 0; k < kCols; ++k) {
            if (g_rowHandles[s][k] == self) { side = static_cast<int>(s); col = k; break; }
        }
    }
    if (side < 0 || !DevPageOpen() || !g_devList) return;
    if (const uint32_t prev = g_prevOverride[side][col]) {
        Call(*ctx, prev, {self, pad, targetPtr});
    }
    const CpuContext c = *ctx;
    const bool up = side == 0 && (pad & 1), down = side == 1 && (pad & 2);
    if (!up && !down) return;
    if ((up && g_top > 0) || (down && g_top < MaxTop())) {
        // One row, through the game's pager when the bar exists (MenuListPager::ScrollUp / ScrollDown: top, thumb,
        // arrow nudge and the scroll SE 0x20000E5D, as the text lists scroll).
        if (g_pagerLive) {
            Call(c, up ? kPagerScrollUp : kPagerScrollDown, {kDevPager, 1});
            g_top = Memory::Read32(kDevPager);  // MenuListPager +0 top
        } else {
            g_top = up ? g_top - 1 : g_top + 1;
            PlaySe(c, kSeMove);
        }
        Memory::Write32(targetPtr, 0);
        RefreshList(c, self);
        return;
    }
    // Wrap: the window jumps to the other end; the cursor follows the game's wrap link.
    g_top = up ? MaxTop() : 0;
    if (g_pagerLive) Call(c, kPagerReset, {kDevPager, g_top, kRows, RowCount()});
    RefreshList(c, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80003FF0, OpucDev_RowScroll, "OpucDev_RowScroll");

// The button's Normal clip (0xA00210, WBC6's TopMenuAdvInfo sub-layout) carries TableText PauseMenuText row 6
// ("Information"), which is applied again
// whenever the button's clips bind (seen on every open). For the Dev button's TXTM, print "Dev" instead.
extern "C" void OpucDev_SetTableText(CpuContext* ctx) {
    // Recognised from the node itself, never from a saved pointer: after the pause closes, its nodes are freed and
    // the addresses come back for other layouts' TXTM nodes. The label is a TXTM whose parent (+0x48) is a WBC7 with
    // decision kind 8 (the camp hub's own WBC7 has another kind).
    const uint32_t node = ctx->gpr[3];
    const uint32_t parent = Memory::Read32(node + 0x48);
    const bool dev = Memory::Read32(node + 0x58) == kTxtm && parent >= 0x80000000u && parent < 0x94000000u &&
                     Memory::Read32(parent + 0x58) == kWbc7 && Memory::Read32(parent + 0x140) == kDevKind;
    if (!dev) {
        GuestFunctions::NextTranslation(kSetTableText)(ctx);
        return;
    }
    const CpuContext c = *ctx;
    SetLabel(c, node, HandleOf(c, node), u"Dev");
}
REGISTER_NATIVE_WRAPPER_AS(0x803C2838, OpucDev_SetTableText, "OpucDev_SetTableText");

// The pause has closed: run the Dev grants as the ground-pickup grant does (showUi 1, overflow 1: the "Got %ls"
// line, "Too many items!", picture book, item-get scripts, pallet refresh), now in the field's context.
// Test helper, OPUC_TEST_WARP="x,y,z[,yaw]": the first time the pause closes, the player is put at x, y, z facing
// yaw degrees through the game's own debug script commands CharaSetPos 800a41d4 / CharaSetEuler 800a4308 (Squirrel
// arguments: {value, type}, type 3 = float), as the Dolphin harness's probes place him. E.g. the Fst fishing point
// 11995: "-62.8,1.55,-33.87,160".
void TestWarp(const CpuContext& c) {
    static bool done = false;
    const char* env = std::getenv("OPUC_TEST_WARP");
    if (done || !env) return;
    done = true;
    float v[4] = {0, 0, 0, 0};
    int n = 0;
    for (const char* p = env; *p && n < 4; ++n) {
        char* end = nullptr;
        v[n] = std::strtof(p, &end);
        if (end == p) break;
        p = *end == ',' ? end + 1 : end;
    }
    if (n < 3) return;
    const uint32_t args = OpucGuest::ScratchBelow(c, 0x30);
    auto sq = [&](int i, float f) { Memory::WriteFloat32(args + 8 * i, f); Memory::Write32(args + 8 * i + 4, 3); };
    sq(0, v[0]); sq(1, v[1]); sq(2, v[2]);
    Call(c, 0x800A41D4, {args, args + 8, args + 16});
    sq(0, 0); sq(1, v[3]); sq(2, 0);
    Call(c, 0x800A4308, {args, args + 8, args + 16});
    RT_LOG(RT_TAG_HLE) << "[opuc] test warp to " << v[0] << ", " << v[1] << ", " << v[2] << " yaw " << v[3] << std::endl;
}

extern "C" void OpucDev_OnPauseMenuClosed(CpuContext* ctx) {
    const CpuContext c = *ctx;
    GuestFunctions::NextTranslation(kOnPauseMenuClosed)(ctx);
    TestWarp(c);
    if (g_pending.empty()) return;
    const std::vector<PendingGrant> grants = std::move(g_pending);
    g_pending.clear();
    const uint32_t cm = Memory::Read32(kCharaManagerPtr);
    for (const PendingGrant& g : grants) {
        if (g.id == kRodGrant) {
            GiveMaxRod(c);
            RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: granted the fishing rod at max level" << std::endl;
            continue;
        }
        const uint32_t ok = cm ? Call(c, kGrantItemById, {cm, g.id, g.count, 1, 1}) : 0;
        RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: granted item " << g.id << " x" << g.count
                           << (ok ? "" : " (bag full: the rest is dropped)") << std::endl;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x800404F8, OpucDev_OnPauseMenuClosed, "OpucDev_OnPauseMenuClosed");

extern "C" void OpucDev_IsSeenInMenu(CpuContext* ctx) {
    if (DevPageOpen()) {
        ctx->gpr[3] = 1;
        return;
    }
    GuestFunctions::NextTranslation(kIsSeenInMenu)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x800F537C, OpucDev_IsSeenInMenu, "OpucDev_IsSeenInMenu");

extern "C" void OpucDev_MarkSeenInMenu(CpuContext* ctx) {
    if (DevPageOpen()) return;
    GuestFunctions::NextTranslation(kMarkSeenInMenu)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x800F539C, OpucDev_MarkSeenInMenu, "OpucDev_MarkSeenInMenu");

namespace OpucDev {
// The Fishing page's "Give fishing rod": the fully upgraded rod, granted when the pause closes like the Items page's.
void QueueRodGrant() {
    g_pending.push_back({kRodGrant, 1});
    RT_LOG(RT_TAG_HLE) << "[opuc] dev menu: fishing rod at max level (when the pause closes)" << std::endl;
}
}  // namespace OpucDev
