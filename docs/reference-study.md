# Reference recompilation projects: study and reuse decision

Studied 2026-11-22 from shallow clones (read only, nothing of theirs was run except the translator we vendored).
This project is based on [WiiCompiled](https://github.com/patchzyy/wiicompiled) (GPL-3.0); below it is called "the base".

| Project | Target | Lifter | Bridge layer | Graphics | Audio |
|---|---|---|---|---|---|
| the base (GPL-3.0) | Mario Kart Wii (Wii, PowerPC Broadway) | C# .NET translator: DOL → IR/SSA → one C++ function per guest function | HLE of RVL SDK functions **by guest address** (`PPC_NATIVE_OVERRIDE`, ~580 sites), plus emulated IOS devices (`/dev/fs`, `/dev/es`, BT HCI, net) | aurora fork; GX SDK calls replaced, and raw write-gather-pipe stores (0xCC008000) parsed by a FIFO decoder | DSP mailbox HLE + C++ AX mixer (AXWii-style), SDL3 output |
| [aurora](https://github.com/encounter/aurora) (MIT) | GameCube/Wii **decompilations** | — | Source-level SDK: GX (full FIFO + command processor), GD, MTX, VI, PAD, SI, DVD (nod: ISO/RVZ), CARD, WPAD/KPAD on SDL | WebGPU via Dawn (D3D12/Vulkan/Metal), SDL3 window | none (no AX); no OSThread/Mutex/Alarm/Message, no NAND/SC/IPC |
| [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) | Zelda TotK (Switch, ARM64) | Python lifter → C per function; jump tables by a symbolic pass checked against Unicorn | `nn::*` functions replaced by symbol (~163), the rest of the SDK runs lifted over emulated SVCs/IPC; one host thread per guest thread, 3 virtual cores | NVN recorded, replayed to Vulkan 1.3; Maxwell shaders → SPIR-V with Ryujinx's translator | SDL3 |
| [metroid-prime-4-recomp](https://github.com/jaigner-hub/metroid-prime-4-recomp) | Metroid Prime 4 (Switch) | mechanical copy of the TotK/fe3h core ("about a hundred lines of game-bound tables to reach a build") | same | same | same |

## What carries over to OPUC

1. **The base translator is game-agnostic.** It is driven by a YAML manifest (DOL path + hash, memory, SDA bases, entry, function map). Its only MKWii dependency in the base path was a mod payload check in `Program.cs`, removed here. It lifts every Broadway instruction OPUC uses, including paired singles with GQR propagation, `lwarx/stwcx.`, FPSCR ops and string ops. Indirect calls go through a hashed address registry, so **every indirect target must be a known function start**: the function map is the discovery oracle (no vtable scan in the base path).
2. **The base runtime is reusable in design but bound to MKWii addresses.** Every HLE replacement and every SDK global it touches is a hard-coded MKWii PAL address. Two RVL SDK builds share function names but not addresses, so the port is a **rebinding** problem: MKWii symbol → RVL SDK name → OPUC address. `tools/rebind-runtime.mjs` does this from `projects/mkwii-reference/MAP.txt` and our `projects/opuc1/MAP.txt`.
3. **aurora (the base's fork, vendored in `third_party/aurora`) is the graphics/input backend.** It does not provide threads, AX or NAND; the runtime supplies those (fibers for OSThread, AX mixer, NAND on a host directory).
4. **Wii Remote input is the biggest gap.** The base passes real Wii Remotes through an emulated BT HCI controller; it has no virtual remote/Nunchuk/IR. OPUC needs Wiimote + Nunchuk (KPAD), so a virtual remote has to be written. aurora's own WPAD/KPAD emulation on SDL is a candidate if KPAD is HLE'd instead of running the game's BTE/WPAD stack.
5. **Disc access.** The runtime reads an extracted `DATA` tree (rebuilds the FST); aurora's nod reads ISO/RVZ. OPUC starts from `data/extract/DATA` and moves to direct ISO reading through nod later.

## Agent-loop practice taken from the Switch recomps

- One status ledger (`STATUS.md`) is the hand-off between sessions: what works, the current blocker, the next step, temporary debug edits.
- Debugging gate (TotK `docs/agents/debugging.md`): evidence → where it reproduces → known already? → read the code (Ghidra) → cheapest falsifying check → red-then-green repro → fix the cause. "A fix without a reproduced failure is a guess."
- Generated code is never edited by hand; change the translator, the map or the runtime.
- Differential testing against an oracle for every lifter change (they use Unicorn; we use Dolphin).
- Probe modes in the exe (stop at first GX call, at main, etc.) and game-time `--seconds` for unattended runs.

## Decision

Take the base translator and runtime (GPL-3.0, so this project is GPL-3.0 too) and its aurora fork (MIT) as the
starting point, keep their layout so fixes can be compared, and do all OPUC work through: the function map, the rebind
tool and its overrides, OPUC-specific HLE in `runtime/src`, and translator fixes when a lifted instruction is wrong.
