# One Piece: Unlimited Recompilation Episode 1: one loop iteration

The recompilation is driven by repeated iterations of this protocol, by the main session (`/loop` with this
file as the prompt, or a Ralph loop) and by `opuc-recomp` worker agents for scoped sub-tasks. Each iteration
moves one milestone in [STATUS.md](STATUS.md) or removes one blocker. It never ends without updating STATUS.md.

## Starting the loop

In a Claude Code session in this repo (Ghidra running with project `opuc1`):

```text
/loop Run one iteration of programs/Recomp/LOOP.md for One Piece: Unlimited Recompilation Episode 1.
```

`/loop` without an interval lets each iteration pace the next one. A Ralph loop (`/ralph-loop`) with the same
prompt works too. Cloud routines cannot reach Ghidra, Dolphin or the disc dump, so the loop runs locally.

## Tools

| Command | What it proves |
|---|---|
| `node programs/Recomp/tools/status.mjs` | metrics block in STATUS.md (map, rebind, translate, compile, last run) |
| `powershell -File programs/Recomp/tools/build.ps1 [-Stage map|rebind|translate|configure|compile]` | pipeline; logs in `build/logs/` |
| `powershell -File programs/Recomp/tools/run.ps1 -Seconds 20 [-Headless]` | bounded run; `build/logs/run.log` |
| `node programs/Recomp/tools/check-bindings.mjs [--list]` | every HLE binding sits on a function start whose OPUC name matches the MKWii symbol, or on a cited override; no address bound twice |

A crash naming a guest address `0xF0xxxxxx` is an `OPUC_TODO` poison: the runtime used an MKWii address
`0x80xxxxxx` that has no OPUC counterpart yet (`projects/opuc1/rebind_report.md`).

## Iteration

1. **Orient.** `node programs/Recomp/tools/status.mjs` and read `programs/Recomp/STATUS.md`
   (current blocker, next steps, temporary debug edits).
2. **Pick one target**: the current blocker, else the first open next step. Register it in the open-points ledger
   (`node .claude/hooks/opuc-loop.mjs add "<target>" --test "<check that fails today>"`).
3. **Reproduce.** Run the cheapest check that shows the failure today: a build stage
   (`powershell -File programs/Recomp/tools/build.ps1 -Stage <stage>`), a run (`tools/run.ps1` once it exists),
   or a translator test. Keep the failing output in `build/logs/`.
4. **Read before guessing.** For runtime behaviour, read the game's code in Ghidra (project `opuc1`, through the
   `ghidra-mcp:ghidra-agent-mcp` agent) and the upstream (MKWii) HLE it replaces. When the recomp and the game
   disagree, run the same moment in Dolphin (`programs/DolphinHarness`) and compare RAM.
5. **Fix the cause in the right layer**, never in `generated/`:
   - wrong/missing function start → `projects/opuc1/MAP.txt` inputs (Ghidra export, `sdk_names.tsv`)
   - MKWii address with no OPUC match → `projects/opuc1/rebind_overrides.tsv` + `build.ps1 -Stage rebind`
   - OPUC-specific SDK/engine behaviour → OPUC HLE in `runtime/src/hle/opuc/`
   - wrong lifted instruction → translator + a translator test
6. **Prove it.** Rerun the failing check; it passes now. After any rebind change, `check-bindings.mjs` must exit 0. Mark the ledger point done with the command and result,
   then spawn `opuc-verifier` with the id.
7. **Record.**
   - `node programs/Recomp/tools/status.mjs` (metrics block), then edit STATUS.md: milestone state, new current
     blocker, next steps, temporary debug edits.
   - New game facts (SDK globals, addresses, struct offsets) go into the vault page `entities/main-dol-layout`
     (or the page that owns the subsystem) and `obsidian-wiki memory sync`, as CLAUDE.md requires.

## Parallel work for workers

Hand `opuc-recomp` agents work that does not touch the same files:

- **rebind batch**: resolve up to 20 TODO addresses from `rebind_report.md` for one library (OS, VI, DVD, GX,
  AX, NAND, SC) by reading OPUC's SDK functions in Ghidra; output rows for `rebind_overrides.tsv`.
- **SDK naming batch**: name ambiguous Dolphin matches in one SDK library range; output rows for `sdk_names.tsv`.
- **HLE module**: write or port one OPUC HLE module (virtual Wii Remote, NAND paths, ISO reader) behind a switch.

Only the main session edits STATUS.md, runs the full build and merges worker output.
