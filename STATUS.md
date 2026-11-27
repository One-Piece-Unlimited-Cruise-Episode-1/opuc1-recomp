# One Piece: Unlimited Recompilation Episode 1: status

The hand-off between sessions. Read this first; update it last. One iteration of the loop is described in
[LOOP.md](LOOP.md). Measured numbers live in the metrics block below (rewritten by `node programs/Recomp/tools/status.mjs`).

<!-- metrics:begin (tools/status.mjs) -->
```json
{
  "measured": "2026-11-27T23:29:45.696Z",
  "mapStarts": 18670,
  "mapNamed": 6337,
  "mapSdkNamed": 1097,
  "rebindMappedDistinct": 401,
  "rebindUnbound": 62,
  "rebindTodoDistinct": 1,
  "translatedFunctions": 18850,
  "translateOk": true,
  "compileErrors": 0,
  "compileLastLine": "[2/3] Linking CXX executable opuc1-recomp.exe",
  "executables": [
    "opuc1-recomp.exe"
  ],
  "runMilestones": [
    "M2 guest code ran OSInit",
    "M2 reached main (ALSystem::Main 803F03D8)",
    "M3 first GX draw",
    "M3 first GXCopyDisp (not yet a presented frame)",
    "M4 disc file read"
  ],
  "runFatal": null
}
```
<!-- metrics:end -->

## Milestones

| # | Milestone | Proof | State |
|---|---|---|---|
| M0 | Whole main.dol translates to C++ with unsupported instructions disallowed | `build/logs/translate.log`: 18,849 functions (Ghidra list + data-pointer and code-constant seeds), exit 0 | done 2026-11-22 |
| M1 | Native build links `build/native/opuc1-recomp.exe` | `build.ps1 -Stage compile` exit 0 | done 2026-11-22 (verified) |
| M2 | Boots to `main` (`ALSystem::Main` 803F03D8) | run log: `OSExceptionInit: skipped` (OSInit ran) and `[opuc] milestone main` (first GXInit) | done 2026-11-22 (from `data/extract/DATA`): __start → OSInit (IOS53 check, SYSCONF, STM, IPC) → DVDInit → ctors → main → GXInit; `[opuc] milestone frame` also seen |
| M3 | First frame presented (VI/GX init, strap screen draw) | `[opuc] milestone first-draw` + `frame`, `run.ps1 -Shot` screenshot | done 2026-11-22: the Wii Remote strap screen renders at 60 FPS (`build/logs/run.png`) |
| M4 | Disc reads from `data/extract/DATA`, AAR packs load | `[opuc] dvd read` lines | done 2026-11-22: HomeButton2, resource/En, Menu, event/chara packs, sound archives, startlogo (order on vault boot-flow) |
| M4b | Disc reads straight from `data/opuc1.iso` (user requirement) | run with `paths.disc_image` and no extracted tree | open |
| M5 | Strap and logo screens look like Dolphin's | screenshot vs `programs/DolphinHarness` capture | in progress: strap screen looks right by eye; no side-by-side with Dolphin yet |
| M6 | Input: virtual Wii Remote + Nunchuk (KPAD) from keyboard/gamepad | scripted A passes the strap screen | done 2026-11-22: keyboard + mouse use the Play app's control scheme (dodge/dash/skill macros, held-shake swing, menu "ui" switch), gamepad, `OPUC_INPUT_SCRIPT`; no IR pointer, no mouse look: `hle/opuc/wiimote_opuc.cpp` |
| M7 | Title menu, NAND save create/load | save written under the NAND root, reloads | in progress: on an empty NAND the boot save step creates banner.bin + share/system.bin + user00-02.bin (2026-11-22); the title (logo, PRESS A) and the Record book menu render like Dolphin; Record 1 → Start Game reaches state 0xe → 3 (2026-11-22); loading an existing save not tested yet |
| M8 | Audio: AX/AXFX mixing, nw4r snd, BGM streams | strap BGM audible, no underruns in log | open |
| M9 | Adventure field (Fst) loads, player moves, camera follows | harness scenario parity | in progress: AdventureMain runs `st_newgame`, loads Fst and plays the opening movie (THP + captions, skippable with +) and the opening event; Luffy gets control in Fst and runs / dashes with the HUD up (keyboard scheme, 2026-11-22); camera, combat and parity not checked |
| M10 | Battles, events, menus; full playthrough parity with Dolphin | sweep reports | open |

## Current blocker

Field play in Fst (M9): the player moves now. A new game played with the keyboard scheme (title: Enter, LMB, E, J;
`S3@6:KEY_Tab` skips the movie; E every 2.5 s through the opening) gives Luffy control in Fst about 118 s into state 3
(t≈156 s, `build/logs/run_218.png`: dashing with the HUD up, 60 FPS). Faster for pause tests: launch-time A every
2 s to 56 s plus `S3@2..20:KEY_Tab` every 2 s skips the movie and the opening scenes; Luffy is playable about 30 s into
state 3 (launch ≈ 55 s), and `S3@40:PLUS` opens the pause. Nothing beyond that has been checked: camera follow,
combat, enemies, field objects, menus past the + banner, parity with Dolphin.

## Next steps (in order)

1. Field parity: walk / dash / attack in Fst and compare with a Dolphin harness capture at the same spot (camera, HUD,
   animation). Also check the movie's aspect: the 640×480 THP (bars baked in) fills the 16:9 window; compare with Dolphin.
2. Audit the remaining TEV/GX split bindings the way GXSetTevAlphaOp failed: a translated SDK function that
   read-modify-writes a gxData shadow (tevc +0x180, teva +0x1C0, tevKsel +0x200, cmode0 +0x220, genMode +0x254)
   while its siblings are HLE'd sends stale fields to aurora. List every OPUC GX function that is still translated and
   touches one of those shadows (`OPUC_UNBOUND` rows in rebind_report.md whose OPUC twin exists).
3. Compare the strap screen with Dolphin (programs/DolphinHarness) for M5. The status line now shows ProjProcess
   state 9 (StartLogo1, strap screen) while it waits.
4. Sound init faults (seen 2026-11-22): `SoundSe::OpenArchivesThread` writes 0xFFFFF7FF and 0x881F19C4 (unmapped,
   committed as zero pages by the runtime) and `nw4r::snd::SoundArchivePlayer::GetRequiredMemSize+0x28`
   (LR 0x8031ACE8) reads 0x01800010 during SoundSe::Init: likely a bad NAND/brsar read result; check the
   `/tmp/OPA2.brsar` NAND copy path (vault boot-flow Sound) and the NAND HLE results.
5. Keep walking the boot path past the title (new game, adventure): each stop names the MKWii address or the missing
   function start. Resolve in Ghidra, add a cited override row, rerun.
6. Audit every name-matched binding for Dolphin-signature collisions (the audio batch found `AIRegisterDMACallback`
   on four addresses, one of them `VISetPreRetraceCallback`). `build-function-map.mjs` now drops names matched at
   more than one address; single-address false matches are still possible, so each library batch confirms its
   bindings in Ghidra.
7. Strip what is left of the inherited Mario Kart extras: real Wii Remote support (Bluetooth host, pairing,
   L2CAP/HID/SDP link).
8. M4b: serve the disc from `data/opuc1.iso` with nod (aurora already fetches it for `aurora_dvd`): open the DATA
   partition, publish its FST at 0x80000038, read files through `nod_partition_open_file`.

## Known facts and decisions

- All four library batches (OS/VI/SC, AI/DSP/AX, DVD/NAND/IPC, GX) are merged: 402 MKWii addresses map to OPUC,
  64 TODO addresses remain (`rebind_report.md`), `check-bindings.mjs` passes on 292 bindings. The batches found
  Dolphin-signature collisions (AIRegisterDMACallback on four addresses, NANDRead/NANDWrite swapped); manual names
  in `sdk_names.tsv` override those. Workers renamed ~370 SDK functions in the Ghidra project.
- GXSetTevAlphaOp is OPUC `80296080` (no Dolphin-signature name; twin of GXSetTevColorOp `80296028` on teva
  gd+0x1C0). While it ran translated next to HLE'd SetTevAlphaIn/SetTevSwapMode it re-sent stale alpha inputs from
  the guest shadow, and the title logo / PRESS A drew as white quads. Now bound (override row; check-bindings 297/297).
- `OPUC_INPUT_SCRIPT` items may be state-gated: `S<hex state>@<sec>:<buttons>[:<dur>]` fires `<sec>` after ProjProcess first
  enters that state, and only while it is still in it (`wiimote_opuc.cpp`). Use these instead of launch-relative
  seconds; load times vary between runs. `tools/run.ps1` refuses to start while another opuc1-recomp is running (exit 3).
  Don't pipe it through `Select-Object -First N`: that cuts the child's output and the game outlives the timeout.
- PPCMfhid2 is OPUC `8026a6b4` (`mfspr r3,HID2; blr`). The upstream HLE was registered with REGISTER_TRANSLATED_FUNCTION at
  the raw MKWii address 0x8012e630, which `rebind-runtime.mjs` does not rewrite (it lands mid-function in OPUC). It is now a
  PPC_NATIVE_OVERRIDE plus an override row. Without it THPVideoDecode (804b42e0) failed with 0x1C (HID2.LCE clear: hid2 is 0 on
  the main and coroutine contexts, and LCEnable is a stub), and every movie ended on its first frame (vault menus "Opening movie").
  No other REGISTER_TRANSLATED_FUNCTION site is left in runtime/src.
- `OPUC_TRACE_TEX=1` logs each distinct GXLoadTexObj (tex map, size, format, data address, caller LR) as `[gx] tex-load`.
- Upstream HLE bugs fixed for OPUC: __GXPEInit / GXFinishInterruptHandler wrote GXData+0xA (cpEnable) instead of
  the PE MMIO register; VI IsInitialized / dimming flags are 32-bit in OPUC (upstream wrote 8-bit); SC game-region
  table has 5 entries (upstream assumed 4); NAND title id high is 0x00010000 (upstream 0x00010004).
- Boot needs: IOS53 in low memory 0x80003140/0x80003188 (`system_bridge.cpp`) and maker `AF`. A NAND SYSCONF is optional
  (checked 2026-11-23): without it `NANDOpen /shared2/sys/SYSCONF` fails and boot still reaches the save check, because
  `SCCheckStatus` is HLE'd to return success (`runtime/src/hle/sc.cpp`). `run.ps1` and the setup (`launcher/`) seed one
  from the local Dolphin NAND or `OPUC_SYSCONF` / `--sysconf` when available.
- Function starts: Ghidra's list + 3,273 data-pointer seeds (vtables, callback tables; fixpoint over owners) +
  305 code-constant seeds (`lis`/`addi` pairs). `bctr`, `blr`, tail-call `b` and prologues mark entries.
- Engine coroutines: the context switch `803dfc94` is native (`hle/opuc/coroutine.cpp`); each coroutine is a host
  fiber with its own CpuContext, keyed by a marker frame {token, 'OPCO' at +8} that travels with the guest stack
  image the game copies in and out. `fiber_manager` learned `ExchangeCurrentThreadHostFiber`, and
  `SwitchToScheduler` now targets the main thread's current fiber.
- Frame pacing: OPUC waits for a GX draw-done callback (803ee194) that swaps the XFB and releases
  ALEngine::RunFrame. `hle/opuc/gx_opuc.cpp` HLEs GXSetDrawDone (802930e4) to drain GX and run the finish
  interrupt's work synchronously. The runtime's startup cover lifts at the first GXCopyDisp.
- Gorge (vly) at a high internal resolution (2026-11-27): 60 → ~20 FPS at `resolution_multiplier = 8`, 60 FPS at 1;
  Fst and the Sunny held 60 at 8. Cause: every frame on vly the game copies the full 640×480 EFB into the
  screen-copy buffer twice, RGBA8 (fmt 6) and then I8 (fmt 1, the ShaderPRSepia post-effect, vault
  entities/geometry); the Sunny does only the RGBA8 one. Aurora keys
  copy textures by (dest, scaled size, format), so the two copies swap the pinned entry of one destination. Its
  copy cache budget was a fixed 64 MB (`RenderTextureCacheLimits`) and an unpinned entry above the budget is freed
  at once. At 8× the RGBA8 copy is 5120×3840×4 ≈ 79 MB, so it was re-created (Dawn texture allocation on the
  FIFO thread) every frame: `aurora_end_frame_gx` waited ~34 ms in `fifo::drain` while neither the GPU (~35 %)
  nor any CPU thread was saturated. Fix in `third_party/aurora` (`texture::fit_copy_cache_to_render_target`,
  called from `copy_tex`): the copy budget is now max(64 MB, 4 full render-target RGBA8 copies). Measured with the
  same save: 59–60 FPS on vly at 8×, `aurora_end_frame_gx` 0.02 ms. The shipped build links aurora from the
  native prebuilt package, so the package (`launcher/Prepare-NativePrebuilt.ps1`) must be rebuilt for the fix to
  reach an install. `g_offscreenCache` (recording.cpp) still uses the fixed 64 MB limit.
- Diagnostics: `[opuc] status` once per second (copies/s, ProjProcess), `[opuc] dvd read` per file,
  `OPUC_TRACE_NATIVE=1` prints the hottest native call sites (finds polling loops), `run.ps1 -Shot` saves
  `build/logs/run.png`.
- NAND working dir: the runtime now follows the guest's NAND_currentDir (0x805B7760) for native NAND calls and
  NANDGetCurrentDir; NANDInit sets it to the home dir (`hle/storage/nand_fs.cpp`, `nand_api.cpp`). `OPUC_TRACE_NAND=1`
  logs every /dev/fs IOS call with path, command and result. `run.ps1 -ShotAt '10,14'` saves timed screenshots.
- Input: `hle/opuc/wiimote_opuc.cpp` replaces KPADRead (8030454c) and WPADProbe (802c30f0); channel 0 is a remote
  + Nunchuk. Since 2026-11-22 the keyboard and mouse use the Play app's control scheme
  (`programs/apps/shared/web/input/controller.js`, default bindings hardcoded, no remapping): WASD stick (CapsLock walk),
  Space/K jump (B), LMB/J attack (A), RMB/L shake (held 6 Hz swing, |acc| ≥ 3 pulses), Shift tap dodge (A+B) / hold dash
  (C, -, C, C held), Q = C then C+A, R = C then C+shake, MMB/T = Z, wheel = Z double-tap, O = Nunchuk shake, E/Enter = A,
  V = 1, C/X = −, 1-4 = D-pad (items), arrows = D-pad, Esc = + (B in menus), Tab = +, Backspace = B. "ui" (menus and
  windows: ProjProcess state other than 2/3, or an active AdvMain pause record at *(0x8068A120)+0x20, +0x34) switches
  Esc/RMB to B, WASD to D-pad + stick and Shift to C, as in the Play app. Not ported: mouse look (no pad input does
  that), its viewer keys. KPADRead returns 3 or 4 samples a call (10 every 3 frames, the remote's 200 Hz; sample 0 the
  newest, acc_speed filled), because WiiMotion counts samples. Remote/Nunchuk acceleration uses the Play app's values
  in the KPAD frame (Play (x, y, z) → KPAD (x, −z, −y); rest (0, −1, 0)). Keys arrive through SDL_AddEventWatch (taps
  shorter than a frame still count) and are dropped while the runtime overlay blocks input. The overlay no longer opens
  its exit prompt on Esc (F10 top bar / window close instead). The first SDL gamepad still works as before.
  `OPUC_INPUT_SCRIPT` also takes `KEY_<SDL scancode name>` (e.g. `KEY_Left Shift`), `MOUSE_LEFT|MIDDLE|RIGHT` and
  `WHEEL` tokens that go through the scheme, and `MDX=<px>` / `MDY=<px>` mouse movement per frame; `OPUC_TRACE_INPUT=1`
  logs hold-word / ui changes, shake pulses and the held tools' swings.
  - Pickaxe and bug nets (2026-11-27, not in the game; `ToolFrame`): they swing on a WiiMotion gesture, and the held
    6 Hz shake tripped the three detectors at random. With one in the hand, hold RMB (or L) and drag the mouse 40 px:
    down → the 40 ms tip pull (0, +2.6, −1) g → gesture 2 (record 1, overhead); right / left → 15 ms at (±0.8, −1, 1.5) g
    → gesture 8 / 4 (records 3 / 2). One swing per stroke (an upstroke or the other side rearms); a click or the pad's
    right shoulder is the overhead swing. Tool in hand = the owner CharaObj (work +0x13E8) of the last
    `WiiMotion::Update` 8002626c / CanEnter (pickaxe 80031f5c, nets 8002f620) / `CharaDigup::Update` 80030988 call,
    held id 1–3. The vacuum keeps the shake. PlayGuide 0xA00498 reads "[RMB] Hold Button / While Swinging". Checked
    (scripted, Fst): pickaxe and net, down / right / left / click / long downstroke, each published and accepted by the
    CanEnter, the long stroke once. Not checked: which sideways record sweeps which way (frames do not tell).
    Adding a native wrapper on a function the translator already dispatches needs `build.ps1 -Stage translate` (the
    runtime otherwise stops with "Stale generated indirect dispatch winner").
  - Test helpers (dev_menu.cpp, at the first pause close with `OPUC_TEST_WARP`): `OPUC_TEST_ROD=1` the max rod,
    `OPUC_TEST_GIVE="1,3:1"` items silently (`id:extra0` through an ItemRef; 3:1 = the Bug-Catcher Net).
- PC button art (2026-11-26, ported from the Play tab's play/ui/pcart.js; not in the game; `[game] pc_button_art`,
  F10 > Game): the 48 pictures that show a Wii Remote / Nunchuk button (tutorial reminders 0xA00050–56 and the
  `[icon:k]` pictures, the window A prompt 0xA00000 / 01, the check bubbles 0xA00490 / 93 / 94 / 95, the PauseMenu,
  StatusDetail, MakeItemTree, BaseMenu, CostumeChange, DataBase, EndMakeItem, ItemMenu, NextIsland and PictBook help
  strips and small buttons) are drawn again with the bound key. `tools/pcart_gen` (`node tools/pcart_gen/serve.mjs`,
  open http://localhost:8791/) runs the Play tab's own PcArt on the disc pictures and writes `pc_art.inc`: per picture
  the canvas operations that use the game picture (geometry only) and a layer with only the keyglyph.js glyphs (no
  game pixels; the page checks that replaying the operations matches the Play tab's output, max 4/255).
  `hle/opuc/pc_pictures.cpp` replays them on the disc picture in the `ALTexture::LoadById` wrapper: the ALIGs get a
  new GC32 texture (allocated in whole tiles, then given the true size, so nothing shifts), the ALTX check bubbles
  keep the game's atlas texture and frame table and get the new image through SetImage (layer count set to 1 first:
  SetupTexObj only ever sets 2). A picture of another size (another language's pack) keeps the game's art. The
  tutorial window's row is reworded while `AquaDesignerTutorialWindow::LayoutText` 8015bbe0 copies it
  (`wstr::Copy` 802a6068): row 2 "[icon:6] [icon:6] (Hold)" → one held key, row 4 "[icon:7]:" → "(Tap):". Checked
  (scripted runs): the talk window's E prompt, "Tutorials / Shift ↓ / Dashing", the dash tutorial window with one
  Shift icon, "Shift Zoom Map", "X Discard" / "Shift Arrange", "E Sunny" at the Fst camp point (OPUC_TEST_WARP);
  with the switch off the game's pictures and text. Not seen on screen: the other tutorial reminders and icons, the
  other bubbles and strips (built the same way; the generator's replay check covers their geometry). 2026-11-27:
  `com_huri` 0xA00204 (23 px tall) got a bigger key than `com_a` 0xA00202 (20 px), so RMB drew larger than LMB in the
  Status skill cells and the Break Rush rows; pcart.js sizes it as a 20-px picture (`fh: 20`), both are 16 × 21 now
  (seen on the Status page).
- PC fishing (2026-11-26, ported from the Play tab's play/fishing.js, ui/pcguide.js, ui/keyglyph.js; not in the game):
  `hle/opuc/fishing_pc.cpp` (controls, assist HUD, messages), `fishing_motion.h` (throw / jerk / tilt timelines,
  AnalyzeSwing, the fitted flight), `pc_pictures.cpp` + `fishing_guide_image.h` (PC PlayGuide pictures), `gx_texel.h`
  (GX texel codec), `fishing_glyphs.inc` (the key / mouse glyph rows, generated by `tools/fishing_glyphs` from
  keyglyph.js in a browser: `node tools/fishing_glyphs/serve.mjs`, open http://localhost:8790/; no game pixels).
  CharaFishing is found by wrapping `CharaFishing::Update` 80037a60 (state +0x1C, step +0x24, line +0x50).
  - Simple input (default): hold LMB in the wind-up and the power bar swings 0 → 1 → 0 over 2 s; release plays a rod
    throw into the accelerometer (3.4 g for 100–300 ms, harder past that, the top set per rod so a full bar lands
    1.15 × the line) with A held and let go in the follow-through (the predicted distance uses the traced flight:
    the cast speed at 45° from 2.635 up, −10 / −49 u/s² horizontal / vertical; vault gathering §1l. The old Play-tab fit
    fell 6–19 m short on long casts; now 6 casts from 27 to 98 m land within 1.2 m of the bar's number); RMB the 40 ms hook jerk; hold LMB to reel; A / D and
    the mouse sideways lean, W raises; Space / Backspace reels a line in, and in state 0 sends D-pad Right (Unequip).
    Remote input (`[game] fishing_remote_input`): the mouse moves the remote, RMB with LMB held throws.
  - Assist (`[game] fishing_assist`): two PlayStatus GaugeSP layouts and a GaugeHP bound to StatusGauges in low memory
    0x80002600 (refMax 1000), the float 0xA004F8 and the rod gauge's digits as sprites, all under the fishing HUD's
    layout root, built in its screen with the menu scene pushed: the power bar with a red BAK to the line length, a
    float over each bite band's start, the BLK strip pulsed while the bar is in a band, the predicted distance; in the
    fight the lean track, the running fish's zone and the float as the needle (above the track: the gauge pieces draw
    over child sprites). The HP / SP plate gets a transparent texture id 0xA0FFF0 that the LoadById hook serves.
    Released before `DataUpdateReady` rebuilds the HUD layout (flag 0x80) and at `BeginOrEnd(end)`. INFORMATION lines
    through `AquaDesignerInformation::PushLine` 8019bb48: too weak, line snapped, float on the ground, no fish here,
    too early / nothing on the hook, no fish within this line.
  - PC pictures (`[game] fishing_pc_guide`): `ALTexture::LoadById` 803cae2c wrapped for 0xA00498–0xA0049E builds a
    'GC32' (RGBA8) image from the game's 'GACC' picture (title strip, the 0xA00499 panel stretched, captions set from
    letters cut out of the baked caption lines) plus the glyph rows; heights padded to whole 4 × 4 tiles (an unpadded
    90-row picture overran the GC32 buffer and corrupted the heap).
  - Checked (scripted runs, Fst; `OPUC_TEST_WARP` puts Luffy at the point 11995 when the pause closes,
    `OPUC_FISHING_AUTOHOOK` jerks once per real bite, `OPUC_TRACE_FISHING` logs states): rod granted through Dev >
    Items, casts by bar (0.15 → 155 ms, 0.98 → 6.1 g / 300 ms), the lure in the water, bites, hook, fight with LMB
    reeling and A / D lean, the catch scene and result window, a second round; Space put the rod away; the PC Hold,
    Cast, Hooking and Action pictures; the power bar, markers, band blink, digits, lean track, zone, needle; "Nothing on
    the hook yet", "The float landed on the ground" (a flight back to state 0 without the float state 2); Remote input cast; the Wii pictures with fishing_pc_guide off. Not checked: mouse movement in Remote
    input (scripts cannot move the mouse), the tool pictures 0xA00498 / 0xA0049D / 0xA0049E on screen, the other
    messages. The Play tab's Dev > Fishing page is the Dev menu's Fishing list (below).
- Dev menu (2026-11-25, ported from the Play tab, not in the game): `hle/opuc/dev_menu.cpp` adds a "Dev" button
  (WBC7, two row pitches below WBC6) to the pause top page. The button is built from scratch the way the layout loader
  builds WBC0–6 (ALWidgetButton::Construct, Assemble 0xA00210 through LookupObjectByFileId, SetOwner WPTP, the
  PauseMenuTop clip ids through SetStateMotion, position through the "Position" property; a direct posture write
  leaves a widget drawn at the pane origin). Dev opens the Dev list: the game's text-list page
  (AquaDesignerSlateList::ConstructStele, SlateList.aar 0xA002D0) with its own vtable (rows, detail, decision,
  reactivate are natives at 0x80003FA0–0x80003FD0) and the rows Debug, Flags, Items, Fishing (`hle/opuc/dev_pages.cpp`, see
  the next entry). Items opens the Items page: the game's
  AquaDesignerMenuItem mode 3 grid (BaseMenuItem 0xA002E1, 9 × 5) over a 45-slot fake ItemBag (low memory
  0x80002000) that windows the 215 valid item ids and scrolls by rows (widget +0x108 override, a native at the
  code-free guest address 0x80003FF0). Right of the grid sits the text lists' scroll bar: SlateList.aar's PanelBase
  layout 0xA002D7 instantiated under the grid's layout root (CreateRootContainer, LookupObjectByFileId, PanelBaseOpen
  clip, AddChild / SetParent, EXPL hidden) and driven by a game MenuListPager at 0x80002560 (BindBarNodes, Reset,
  ScrollUp / ScrollDown, which play the scroll SE). A opens the pause's own count window (TryBeginDrop with the all_item_drop
  debug flag answered, retitled "Add how many?", 1–99, no "Are you sure?"; callback native at 0x80003FE0). Grants
  are queued and run when the pause closes (`OnPauseMenuClosed` → `GrantItemById(cm, id, n, 1, 1)`, the ground
  pickup: INFORMATION line, item-get scripts, picture book); the Dev grid counts include the queued ones. The rods
  4/5/6 give the one rod item 6 at rod row 32 / line row 30. All hooks are `REGISTER_NATIVE_WRAPPER_AS` wrappers
  calling the base translation (`GuestFunctions::NextTranslation`); `hle/opuc/guest_call.h` calls guest functions
  with arguments from native code. `[game] dev_menu` (default true, F10 > Game) or `OPUC_DEV_MENU=0` turns it off.
  Pitfalls found on the way (vault `entities/menus` §4c2–§4c5): an earlier version cloned WBC6, and cloning the
  already bound button corrupted the heap a few pauses later; the button's Normal clip carries TableText
  "Information", so `ALText::SetTableText` is hooked for its label; `PrintfW` appends; SlateList's rows and pane
  carry no WidgetOpenMotion (its open clips 0xA002D4 / 0xA002D8 are unassigned), so a list closed under a child page
  came back blank until the port gave the filled rows and the pane those open clips. Checked: 4 pause cycles with grants, B from Items
  to the Dev list and from the list to the top, + from the Items page, the "An Idea!" flash after granting Flame
  Powder, the pause Items page showing the grant.
- Dev menu Debug / Flags / Fishing (2026-11-26, ported from the Play tab's pauseext.js, not in the game;
  `hle/opuc/dev_pages.cpp` says what each row shows and does, `dev_menu.cpp` draws it). The three lists and their
  sub-lists (Flags categories, the Fishing spot table) are shown in place in the Dev list: A swaps the rows (pager
  Reset, rows, focus row 0, detail) and pushes the title; `MenuShell::Back` 8017fc78 is wrapped so B goes back a
  level. Pushing them as their own text-list pages failed: a text list with its scroll bar shown, re-opened after a
  child page, never finished its Close on the next B (rows and pane in Close with the looping Normal motion still
  running; binding the Close clip or setting the loop break bit did not help), and the game never re-opens a text
  list. Each row has a value column: an ALText node made in code as CreateCellParts makes its count text (inside the
  list's screen, draw key one step nearer than NAME, AddChild to the layout root, SetParent under NAME, its own
  alpha), coloured through the ALTextGC font colour (ON yellow, OFF grey). Rows:
  - Debug: the game DebugFlag names no_enemy, menu_content_all_open, event_test, showhit, debug_breakrush,
    debug_breakrush_none, answered by the existing `DebugFlag::IsEnabled` wrapper (by string, the game keeps several
    copies of some names); Freeze enemies (EnemyManager +0xE held at 1, what EnemyStop(true) writes); Invincible
    (`PlayStatus::SetCurrent` 804d8da0 wrapped: a lower value for the leader's HP is refused); Infinite SP (leader SP
    current kept at its max each frame, from KPADRead); Wii button art (`[game] pc_button_art` off); State text (an
    ImGui corner readout: move state, HP, SP, position); Enemy AI view (a sub-list, see the Enemy AI view entry).
  - Flags: the bank-1 records found by walking the bank (`*(0x805E8B04)`, nodes {next, name, size, data}):
    EventFlag, EventScriptEndFlag, EventFuncEnableFlag, EventFuncEndFlag (set ids plus the ones seen this session),
    GameData +4 shortcut bits, the StelePointList flags (& 3), GameData +0xB344 boss bits (table 8050cb70); A flips
    the bit in place.
  - Fishing: Assist overlay, Input, PC guide (the `[game]` switches), Auto-hook (`OpucFishing::Dev()`, also
    `OPUC_FISHING_AUTOHOOK`), Line never breaks (line HP kept at 5 % or more and the line length raised for the update
    while the lure is near it), Fish always bite (`PickFishSlot` 80036254 wrapped: no fish at this distance becomes a
    random spot fish, and the next `RandMT::NextU32` 803b4a70, the bite roll, returns 0), Remote graph (ImGui: the
    game's WiiMotion |acc| ring, power, quiet, gesture), Spot table (the current field's fishp rows, field =
    FieldManager +0x20), Give fishing rod (the max rod, granted when the pause closes).
  Checked (2026-11-26, input scripts with screenshots): every list opens, and B steps back to the top and closes the
  pause; toggles show ON / OFF; menu_content_all_open unlocked every top-page entry on the next pause; Wii button art
  brought back "C Zoom Map"; Freeze wrote the EnemyStop byte; the State text read 170/170, 100/100 and the new-game
  position; Invincible refused a fall into the sea (HP 170 -> 153 refused); Infinite SP refilled SP during a dash;
  Scenes ended 11010 switched off and read back 0; the spot table listed Fst 11995 / 11996 with their fish; the rod
  grant played the game's first-rod scene; at Fst 11995 always bite gave a nibble then the bite on the next roll and
  auto-hook landed the fish; a 0.97 bar cast passed the 100 m line (100.4 m) without the snap; the Remote graph drew.
  Not checked: Freeze with enemies on screen (no enemy group near the test spots); showhit, event_test, no_enemy and
  the Break Rush flags in play (they only reach the game's own checks).
- Enemy AI view (2026-11-26, not in the game; `hle/opuc/ai_view.cpp`, names in `ai_view_names.h`, label font in
  `ai_view_font.h`). Dev > Debug > Enemy AI view opens a sub-list: View (master), Mob rings, Group rings, Boss rings,
  Target lines, Labels, Through walls, Inspector, Planner, Decision log, Timeline. `OPUC_AI_VIEW=1` switches it on at
  start (`=marks` without the panels), `OPUC_TRACE_AI=1` writes every decision to the log, `OPUC_AI_TOGGLE_TEST=N` flips
  the view every N frames, `OPUC_AI_DUMP_TRANSITIONS=1` dumps the mob transition lists once, `OPUC_AI_TEST_BOSS_HP=P`
  sets a planner owner's HP to P % once through `PlayStatus::SetCurrent` (to reach HP conds in a test).
  - Mobs (EnemyAction state machine): the 43 transition conds called by `RunTransitions` 801b6d90 are wrapped (the cond
    pointers the lists at 805ECD50 hold, mostly 4-byte thunks): each result, and a `RandMT::NextU32` roll made while a
    cond runs, against the chance from the working EnemyAIData row (EnemyFactory +0x124). The snapshot walks
    EnemyManager +0x30 / +0x38 groups and their members (both in MEM2).
  - Bosses and CPU crew (UtilAi planner): `UtilAi::Tick` 8000d41c, the 12 scorer functions (f1 = score(node, ctx)),
    `SetStage` 800117c4, `EvalAdvCond` 800106b8, attack Enter / Exit 80016384 / 80017e48 and `EmitAttackSlotInput`
    8000edd0 are wrapped. The behaviour list is scored about every third tick (UtilAi +0x68); each node keeps its last
    score.
  - In-game marks: one ALPrim3D (type 0, GX triangles, cap 10000 vertices) created from inside the field update (the
    group range test 801b3998 and the planner tick call the frame hook) while the current screen's camera is the field
    camera, rebuilt every frame (ClearVertices, BeginAdd, AddVertexColorF per vertex, EndAdd), Z write off, Z test on
    unless Through walls, released with KillDeferred when the view turns off and dropped when the field camera changes.
    Mob rings (near, sidestep, attack kinds 0-2, r = EnemyBaseData radius; filled while the perception bit is set), the
    45-degree facing cone, the target line in the state's colour, the home point and the leash line; group encount /
    setup / chase rings (lit by group +0x20); boss mov layers (orbit, approach, speed, evade) in the Character editor's
    colours and the scored slot's range band; labels as camera-facing stroke text (ALCamera +0x104 view rows).
  - Panels (ImGui): the nearest mob's state, perception bits, every transition of its state with this frame's result
    and roll; the planner's stage strip (stages 0..n, the current one filled), the owner's HP %, the scale profile
    (UtilAiStage +0x17C) and every cond row (UtilAiStage +0x560 / +0x564, 0x2C each) as a sentence, HP rows evaluated
    now, rows of another stage or profile dimmed; the behaviour scores with the winner and the ScoreExecute checks of
    each attack slot (target, row, recover, range, facing); a stage change is logged with the SetMoveStage row that
    leads there; the decision log (repeats counted); the timeline (10 s of state bands).
  Checked (2026-11-26, input scripts with screenshots, Fst group at 75.3 / 4.8 / 30.3 and the Kuma fight from the boss
  tree, event 11300): the Dev rows switch; the log reads e.g. "LockOn: NoticeRandom yes, roll 0.20 < 0.70% -> Alert"
  and the snapshot's state ids agree; rings, lines and labels draw at the mobs, hidden behind walls; at Kuma the
  planner's winner is the behaviour that then runs (approach -> slot 0 at 0.50 with every check passing -> "slot 0
  attack starts", "presses A" -> recover wait at 0.90), with the label and the slot's band; 40 on / off cycles reuse
  the prim's memory (the same node / draw state addresses come back); with Freeze enemies on, the group spawned and no
  decision was logged; with `OPUC_AI_TEST_BOSS_HP=40` Kuma's HP went 500 -> 200 and the log read "stage 0 -> 1 by cond
  #1: HP% < 50 (HP now 40%)", the strip moved to 1 and the queued phase motion (0x2000, 1.10) won the next tick; his
  stage-1 rows (profile mask 6) show "not this profile" at profile 0, and he stays in stage 1. Not checked: CPU crew
  planners (none ran on Fst), frame identity with the view off (the wrappers only pass through then).
- `runtime/src/hle/opuc/` holds OPUC-native HLE (`vi_opuc.cpp`: VIGetScanMode; `milestones.cpp`); the rebind tool skips it.
- Offline runtime (2026-11-22): OPUC1 has no online features, so the inherited network HLE (sockets, SSL, DNS workers,
  IP top, NCD, deferred network IOS waits), Mbed TLS, the CA bundle, the WiiConnect24 NAND seed tree, the `[network]`
  config section and the translator's Mario Kart online-mod (Retro WFC) support are gone. A traced run from boot to
  adventure state 3 showed the game opening only `/dev/net/kd/request` (ioctl 0x01, NWC24 scheduler suspend) and
  `/dev/net/kd/time` (ioctl 0x17, set RTC counter); `hle/wc24/kd_offline.cpp` answers those locally and every other
  `/dev/net` path fails like a missing device.
- Mario Kart-only HLE removed (2026-11-22): `boot_race` and the `--race` / `--race-cpu` options (only `--headless` is
  accepted now), `egg_decomp`, `gx_egg`, `task_thread`, `strap_scene`, the TRK stub, `friend_code_keyboard`, the EGG::Screen
  half of `dynamic_aspect.cpp` (the viewport policy / force_aspect stays), the EGG::Thread start deferral, the THP decoder
  GQR seeding for Mario Kart's decoder threads, the EGG `sSystem` guard on the VI post-retrace callback (the callback is
  now called directly, as the SDK does), the ScnRenderer bloom hook (`ApplyRuntimeCallOptions`, the overlay's "Disable
  bloom" option and `[video] disabled_post_processing_paths`), the Mii library pump (`RFLiIsWorking`) and the
  `RFL_Res.dat` FaceLib seeding with its U8 reader. OPUC links neither EGG nor RFL (vault `entities/main-dol-layout`).
  Every removed hook was `OPUC_UNBOUND` or keyed to an `OPUC_TODO` address, so translation output did not change.
  Unbound generic SDK accessors (SC Bluetooth/WPAD items, SC product/serial) stay until a Ghidra check rebinds or rules
  them out.
- GameCube pad stack removed (2026-11-23): the game reads only KPAD, so the PAD emulation behind PADRead, the
  per-player GameCube mapping, keyboard-to-pad bindings, Dolphin input expressions (GCPadNew.ini import) and motion
  tricks are gone, with their config keys and tests. A traced run showed the game calling PADReset(0xF0000000) once at
  boot and never PADRecalibrate (vault `entities/pad-input`); both stay bound to aurora's PAD layer. The F10
  Controllers page now shows the detected gamepad, the SDL mapping wizard, the Wii Remote section and vibration;
  `input_bindings` is only the overlay's input-blocked flag.
- Crypto++ removed (2026-11-23): an OPUC_TRACE_NAND run from boot to AdventureMain opened no /dev/es and no /dev/sha
  (vault `entities/boot-flow`), and the game's ES library (ESP_InitLib / CloseLib / GetDataDir / GetTitleId) is
  answered in esp.cpp. So the vendored Crypto++, the keys.bin identity, the /dev/sha device and ES device-ID /
  device-certificate / signing are gone; those three ES requests are refused with ES_EINVAL (-1017) and logged as
  "not emulated". /dev/es itself still opens and answers GETTITLEID.
- Steam Input override removed (2026-11-23): the runtime no longer asks a running Steam client to apply another
  app's controller layout (`steam://forceinputappid/480`) while the window has focus, and the `[controller]`
  `steam_input_override` / `steam_input_app_id` keys are gone (old keys in a `Config.toml` are ignored), as is
  the overlay's unused Valve-controller check.
- Layout fast path and development-kit setup removed (2026-11-23):
  - gx_dl.cpp's nw4r::lyt DrawQuad fast path was never registered (both hooks `OPUC_UNBOUND`), so it is gone. If OPUC
    links layout code under another name, the translated code still draws it.
  - The runtime is now a retail console: 64 MiB MEM2 (was 128 MiB, an NDEV size), Hollywood revision 0x11 and
    OSGetConsoleType 0x21 (was 0x10000012). The game's ReportOSInfo prints "Retail 33" and "Memory 88 MB"; the MEM2
    arena is 0x90000800-0x93dc0000.
  - What the dev-kit bit changed: ReportOSInfo's text, UART init and a dev-only OSLaunchTitle branch (vault
    `entities/main-dol-layout`).
  - The one retail-only check, `__DVDCheckDevice`, is HLE'd to return 1, as are __write_console and OSReport.
- Mario Kart test suites and checks removed (2026-11-23):
  - `mkw_fiber_manager_tests`: it drove the scheduler through Mario Kart addresses (`__OSUnlockAllMutex` 801A8088,
    `OSExitThread` 801AA0F0, `EGG::Thread::Run` 8024373C) in a 4 MiB fixture, which no longer holds OPUC's scheduler
    flags at 0x8068A8xx. That is why it failed.
  - `mkw_nand_save_tests` and the probe it tested. The probe treated a blank `rksys.dat` as missing.
  - The RMC disc-ID region check in `dvd.cpp`.
  - The NTSC-U/J region translation: `guest_versions.inc`, the RMCE01/RMCJ01 rows, `GuestRegion::Translate` /
    `FatalUnknownAddress`, `kMkwGuestRegion`, and the translator's `MKW_TRANSLATED_GUEST_REGION`.
    `MKW_GADDR(x)` is now the literal address.
  - `native_cpu_calls.inc`: 38 native-call shortcuts keyed by Mario Kart addresses. None is an OPUC function start.
  - The Mario Kart `initial_pipeline_cache.db` seed and the configure check that required it. Aurora starts without a
    seed and logs that none was found.
- Runtime ctest (`ctest --test-dir build/native`, LLVM-MinGW bin on PATH): all 19 pass. `opuc_system_bridge_tests` used
  to expect Mario Kart constructor tables (DOL + StaticR.rel); it now checks that the runtime runs no constructor itself
  (OPUC's `__init_cpp` does). CI does not run ctest.
- CMake targets renamed from the Mario Kart `mkw_` prefix to `opuc_` (2026-11-27): every library (`opuc_platform`,
  `opuc_runtime_common`, `opuc_base_shared`, `opuc::libco` / `opuc::pugixml` / `opuc::toml11`, the prebuilt
  `opuc_runtime` archive and its `opuc_np::` imports), every test executable and ctest name (`opuc_*_tests`), and the
  CMake project (`opuc1_recomp`). The unused `mkw_release` and `mkw_platform_paths_check` custom targets are gone.
  Unchanged: `MKW_*` cache options and C++ macros, the `mkw_*` CMake helper functions, and the
  `.mkw_recompiled_managed_nand` marker in existing user NAND folders. A native prebuilt package made before the rename
  still works (its own `native_prebuilt.cmake` defines its `mkw_np::` imports and names `libmkw_runtime.a`), but the
  runtime fingerprint covers `runtime/cmake/`, so its runtime archive no longer matches and the runtime compiles from
  source until the package is regenerated.

- Basis: WiiCompiled (translator and runtime, GPL-3.0, with its aurora fork, MIT). See `docs/reference-study.md`.
  That name appears only where the basis is credited: README.md (Credits), docs/reference-study.md, the UPSTREAM.txt
  files, translator/README.md, launcher/README.md (Credits), projects/mkwii-reference/README.md and THIRD-PARTY-NOTICES.md. Nothing else names it. The official name is **One Piece: Unlimited Recompilation Episode 1** (2026-11-22): window
  title, overlay startup / about screens, dialogs, log line, config header, CMake messages, READMEs. The user data folder
  (non-portable mode) is `One Piece Unlimited Recompilation Episode 1` (no colon: not allowed in Windows paths). Technical
  identifiers stay short: exe `opuc1-recomp.exe`, CMake target `opuc1_recomp`, internal id strings (`opuc1-recomp-*`,
  `OPUC1Recomp-DolphinDevice`, `opuc1-runtime-native-index`).
- Icon: the exe / window icon is the game's save icon (TPL image 1 of `memcard/banner_ep1.en.tpl`, 48×48 RGB5A3), built
  from the dump by `tools/make-icon.mjs` in the configure stage into `build/icon/` (ICO 16-256 px, PNG entries) and
  compiled from `build/icon/opuc1_recomp.rc` (`-DOPUC_ICON_DIR`, `mkw_add_product_icon`). The upstream icon files are gone
  from `runtime/resources`; without a generated icon CMake warns and links no icon. Windows Explorer may show the old
  icon until its icon cache refreshes.
  The two MKWii Retro Rewind translator tests that referenced the missing upstream Launcher project were removed.
- OPUC is single-region: `MKW_GADDR` is the main.dol address itself, and `GuestRegion::kFacts` is `ROUPAF`
  (maker `AF`, `__ArenaLo` 0x806A29C0).
- `OPUC_UNBOUND(...)` = upstream HLE with no OPUC counterpart: the translated SDK code runs instead.
  `OPUC_TODO(...)` = upstream address use with no counterpart: evaluates to poison `0xF0xxxxxx`; the first access
  faults with `[opuc] FATAL ... unported MKWii 0x80xxxxxx`. It cannot fault at evaluation because many sites are
  namespace-scope constants initialized at startup.
- Upstream `GX_FATAL_STUB`s are gone (OPUC calls a different GX subset); unbound GX reaches aurora through the
  write-gather-pipe FIFO decoder.
- The runtime does not run DOL constructors itself: OPUC's `__start` → `__init_user` → `__init_cpp` does.
- `dynamic_aspect.cpp` (MKWii EGG::Screen widescreen) is neutralized; `music_attenuation.cpp` is a stub
  (LLVM-MinGW has no C++/WinRT).
- `--headless` runs on its own (upstream required an MKWii boot race).
- Boot milestones are explicit `[opuc] milestone <name>` lines from `runtime/src/hle/opuc/milestones.cpp`.

## Temporary debug edits

None.
