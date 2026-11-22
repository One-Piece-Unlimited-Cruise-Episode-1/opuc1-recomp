# One Piece: Unlimited Recompilation Episode 1: status

The hand-off between sessions. Read this first; update it last. One iteration of the loop is described in
[LOOP.md](LOOP.md). Measured numbers live in the metrics block below (rewritten by `node programs/Recomp/tools/status.mjs`).

<!-- metrics:begin (tools/status.mjs) -->
```json
{
  "measured": "2026-11-22T18:27:43.179Z",
  "mapStarts": 18669,
  "mapNamed": 6337,
  "mapSdkNamed": 1097,
  "rebindMappedDistinct": 404,
  "rebindUnbound": 85,
  "rebindTodoDistinct": 64,
  "translatedFunctions": 18849,
  "translateOk": true,
  "compileErrors": 0,
  "compileLastLine": "[32/33] Linking CXX executable opuc1-recomp.exe",
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
(t≈156 s, `build/logs/run_218.png`: dashing with the HUD up, 60 FPS). Nothing beyond that has been checked: camera follow,
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
7. Strip the remaining MKWii-only modules as they come up (`boot_race`, `strap_scene`, `egg_decomp`, `gx_egg`,
   `task_thread`, `friend_code_keyboard`, `motion_trick*`, `recomp_mod_loader`, `product/retro_rewind_product`).
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
- Boot needs: IOS53 in low memory 0x80003140/0x80003188 (`system_bridge.cpp`), maker `AF`, and a NAND SYSCONF
  (`run.ps1` seeds it from the local Dolphin NAND or `OPUC_SYSCONF`; generating one in the runtime is open work).
- Function starts: Ghidra's list + 3,273 data-pointer seeds (vtables, callback tables; fixpoint over owners) +
  305 code-constant seeds (`lis`/`addi` pairs). `bctr`, `blr`, tail-call `b` and prologues mark entries.
- Engine coroutines: the context switch `803dfc94` is native (`hle/opuc/coroutine.cpp`); each coroutine is a host
  fiber with its own CpuContext, keyed by a marker frame {token, 'OPCO' at +8} that travels with the guest stack
  image the game copies in and out. `fiber_manager` learned `ExchangeCurrentThreadHostFiber`, and
  `SwitchToScheduler` now targets the main thread's current fiber.
- Frame pacing: OPUC waits for a GX draw-done callback (803ee194) that swaps the XFB and releases
  ALEngine::RunFrame. `hle/opuc/gx_opuc.cpp` HLEs GXSetDrawDone (802930e4) to drain GX and run the finish
  interrupt's work synchronously. The runtime's startup cover lifts at the first GXCopyDisp.
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
  that), the Play app's fishing simulation keys, its viewer keys. Remote/Nunchuk acceleration uses the Play app's values
  in the KPAD frame (Play (x, y, z) → KPAD (x, −z, −y); rest (0, −1, 0)). Keys arrive through SDL_AddEventWatch (taps
  shorter than a frame still count) and are dropped while the runtime overlay blocks input. The overlay no longer opens
  its exit prompt on Esc (F10 top bar / window close instead). The first SDL gamepad still works as before.
  `OPUC_INPUT_SCRIPT` also takes `KEY_<SDL scancode name>` (e.g. `KEY_Left Shift`), `MOUSE_LEFT|MIDDLE|RIGHT` and
  `WHEEL` tokens that go through the scheme; `OPUC_TRACE_INPUT=1` logs hold-word / ui changes and shake pulses.
- `runtime/src/hle/opuc/` holds OPUC-native HLE (`vi_opuc.cpp`: VIGetScanMode; `milestones.cpp`); the rebind tool skips it.

- Basis: WiiCompiled (translator and runtime, GPL-3.0, with its aurora fork, MIT). See `docs/reference-study.md`.
  That name appears only where the basis is credited: README.md (Credits), docs/reference-study.md, the UPSTREAM.txt
  files, translator/README.md, projects/mkwii-reference/README.md and THIRD-PARTY-NOTICES.md. Nothing else names it. The official name is **One Piece: Unlimited Recompilation Episode 1** (2026-11-22): window
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
- OPUC is single-region: `GuestRegion::RowOf` maps unknown addresses to themselves; `kFacts[Pal]` is `ROUPAF`
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
