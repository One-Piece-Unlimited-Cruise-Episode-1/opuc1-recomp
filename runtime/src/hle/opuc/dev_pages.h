// Dev menu pages (not in the game), ported from the Play tab's pauseext.js: the Dev list and its Debug, Flags and
// Fishing lists. dev_menu.cpp shows each Page as one of the game's text lists (SlateList.aar, as Information > Stone
// Monuments) with a value column; dev_pages.cpp says what the rows are and what A does.
#pragma once

#include "abi_bridge.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace OpucDev {

struct Page;

struct Row {
    std::u16string label, value, desc;
    uint32_t rgb = 0;  // value colour (0xRRGGBB), 0 = the text style's own
    std::function<void(const CpuContext&)> toggle;          // A: change something; the rows refresh
    std::function<std::shared_ptr<Page>()> open;            // A: open a nested list
    bool items = false;                                     // A: the Dev Items page
};

struct Page {
    std::u16string title, help;
    float valueX = 400;  // the value column, from the row's NAME
    std::function<std::vector<Row>(const CpuContext&)> rows;
};

std::shared_ptr<Page> DevPage();

// The game's DebugFlag names switched on from the Debug page (DebugFlag::IsEnabled 80102514 answers them).
bool DebugFlagOn(std::string_view name);

// Per frame on the game thread (KPADRead): Freeze enemies, the State text readout.
void Tick();

// The Infinite HP / Infinite SP switches, for the PlayStatus wrappers in dev_pages.cpp.
bool InfiniteHp();
bool InfiniteSp();

// Test helper OPUC_TEST_STATUS_HIT (dev_pages.cpp), once per field camera frame (the Solo camera wrapper).
void TestFieldFrame(const CpuContext& c);

// The State text readout for the overlay (empty = off).
std::string StateText();

}  // namespace OpucDev
