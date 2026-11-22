# One Piece: Unlimited Recompilation Episode 1: translator

Based on the WiiCompiled translator (GPL-3.0); see `UPSTREAM.txt` and the repository [`LICENSE`](../LICENSE).

The translator parses GameCube/Wii DOL files, decodes PowerPC instructions, lifts them through
IR/SSA and type inference, and emits C++ that is compiled into a native executable together with
a runtime in `runtime/`. Project-specific paths and addresses are supplied through a versioned
YAML manifest, so the translator itself contains no game-specific data.

The recompilation is static and covers one `main.dol`. There is no REL module and no mod or patch
pipeline.

> Translating a DOL does not exempt you from owning the game it came from.

## How translating a DOL works

A project manifest (YAML) names the input DOL and pins its layout; everything else is derived.
The production manifest is `projects/opuc1/recomp.yml`, and `tools/build.ps1` runs the steps below.

1.  **`translate-recursive <entry-point> --project <manifest>`** - walks the call graph from the
entry point, decodes every reachable function, and emits C++ (as a source bundle, or as one file per
function) plus JSON metadata describing what was emitted.

2.  **`generate-data-init --project <manifest>`** - writes the embedded DOL section initializer,
the guest symbol table and `RuntimeConfig.h`.

3.  **`emit-build-shards --project <manifest>`** - packs the translated functions into
content-addressed shards and emits the CMake build graph (`shards.cmake`) that `runtime/` includes.
Native registrations in `runtime/src` decide which translations are excluded or overridden.

4.  **CMake + Ninja with Clang** compiles `runtime/` plus the generated output into one executable.

Discovery is recursive from the entry point; the manifest's optional `function_map` (one
`hexaddr name` per line) seeds every additional function start. Unsupported instructions fail
translation by default.

## Prerequisites

| Tool | Notes |
| --- | --- |
| .NET 10 SDK | Builds and runs the translator. |
| CMake ≥ 3.25 and Ninja | Configures and drives the native build. |
| Clang / LLVM | The shipped build uses LLVM-MinGW targeting `x86-64-v3`. MSVC is not the tested path. |

Build the CLI once and invoke the assembly directly:

```powershell
dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release
$translator = 'translator/src/Translator.Cli/bin/Release/net10.0/Translator.Cli.dll'
```

## Manifest essentials

-  `inputs.dol.path` - the DOL to translate; optional SHA-256 pinning rejects wrong revisions.
-  `memory.base` / `size` - guest address space.
-  `memory.sda_base` / `sda2_base` - the r13/r2 Small Data Area bases your DOL's boot code installs
(`lis`/`ori` pairs in `__init_registers`). Required by any command that writes `RuntimeConfig.h`;
the translator does not guess them.
-  `translation.function_map.path` - optional symbol map used as the discovery oracle.
-  `translation.allow_unsupported_instructions` - off by default; enabling it emits runtime traps
instead of failing, and such a build can never ship.
-  `runtime.native_registration_root` - the runtime sources scanned for native registrations.
-  `output.*` - where generated files go, relative to `output.root`.

Relative paths resolve from `workspace_root`, which itself resolves from the manifest directory.

## Commands

-  `info [--project path]`
-  `translate-recursive <address> --project path [--outdir path] [--output-metadata path] [--production-source-bundle path] [--no-function-files] [--prune-stale] [--threads N]`
-  `generate-data-init --project path`
-  `emit-build-shards --project path [--base-metadata path] [--base-functions-dir path] [--native-source-dir path] [--out path]`
-  `emit-native-index --project path --out path [--native-source-dir path]` - writes the serialized runtime native index

Any command prints its own option list with `--help`.

## Test

```powershell
dotnet test translator/Translator.sln -c Release
```
