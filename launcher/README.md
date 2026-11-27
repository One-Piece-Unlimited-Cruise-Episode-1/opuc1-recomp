# launcher: setup and launcher

The setup program for **One Piece: Unlimited Recompilation Episode 1**. A player runs one file; it checks their own
One Piece Unlimited Cruise Episode 1 disc image (PAL `ROUPAF`), extracts it, translates and compiles the game on
their machine with a bundled toolchain, installs it, and from then on launches it.

The setup carries no game code and no game data. The release payload is the toolchain and the source tree only;
`Test-PayloadBoundary.ps1` fails the release build if a DOL, disc file, translated output or game asset gets into it.

This is the repurposed setup of the upstream project (see [Credits](#credits)): everything specific to the upstream
game (its mod profile, online payload, REL module, other regions and graphical-frontend contract) is gone, it
installs one product, and it has a guided mode of its own.

## What a player does

**Windows.** Download `opuc1-recomp-setup.exe` and double-click it. It asks for the disc image (file dialog; ISO, WBFS,
RVZ, WIA, GCZ, CISO, GCM), copies the Wii `SYSCONF` from the Dolphin user folder if there is one (optional), confirms the install
folder (`%LOCALAPPDATA%\Programs\One Piece Unlimited Recompilation Episode 1`), installs, and offers to start the game.
It adds a desktop and Start menu shortcut and an Apps & features entry. Running the shortcut, or the installed
`opuc1-recomp-setup.exe`, starts the game (its console window stays open while the game runs; closing it ends the
game); if the installation needs a rebuild it says why and rebuilds first.
Running a newer downloaded setup updates the installation (the disc image is asked for again).

**Linux.** `opuc1-recomp-setup-x86_64.AppImage` / `-aarch64.AppImage`, same flow in the terminal.
**macOS.** `opuc1-recomp-setup.pkg` installs a setup app that runs the same flow in Terminal.

The game reads the Wii system settings (`SYSCONF`, NAND `/shared2/sys/SYSCONF`) during boot. It also starts without
one, so this is optional: setup copies yours once into the NAND the runtime uses (from `--sysconf`, or found in a
Dolphin user folder) and never overwrites it afterwards.

## Command line (Windows)

```
opuc1-recomp-setup.exe                         guided install, update or launch
  --silent --game <image> [--install-dir <dir>] [--sysconf <file>] [--portable] [--progress-json]
  --verify-inputs --game <image> [--progress-json]
  --check-products [--install-dir <dir>] [--progress-json]     exit 2 = rebuild needed
  --repair-products --install-dir <dir> [--sysconf <file>] [--progress-json]
  --launch [--install-dir <dir>]
  --uninstall --install-dir <dir>  |  --silent-uninstall --install-dir <dir>
  --self-test | --version | --help
```

`--progress-json` writes one JSON object per line to stdout (`progress` lines, then exactly one `result` line);
diagnostics go to stderr. `--check-products` prints a `products` line with `game.status` one of `absent`, `current`,
`toolkit-changed`, `blocked`, `broken`. `--portable` makes the parent of `--install-dir` a portable root
(`portable.txt` + `UserData\`), so Config.toml, NAND, cache and logs stay beside the installation.

## Installed layout

```
<install>\opuc1-recomp-setup.exe     the launcher (shortcut target) and uninstaller
<install>\Game\opuc1-recomp.exe      the compiled game, its DLLs, dsp_coef.bin, build-fingerprint.json
<install>\GameAssets\DATA\           the extracted disc (the runtime's dvd_root)
<install>\Toolkit\                   translator, LLVM-MinGW, CMake, Ninja, nodtool
<install>\BuildWorkspace\            runtime and aurora sources, pinned dependencies, caches of the last build
<install>\install-state.json, toolkit-state.json, payload-manifest.json, licenses\
```

Every product records what it was built from (`build-fingerprint.json`: toolkit identity, main.dol SHA-256,
executable SHA-256). A launch checks that chain; a newer setup or a damaged file leads to a rebuild instead of a
silently stale game.

## Building the installers

Windows (needs the .NET 11 SDK and Visual Studio with the "Desktop development with C++" workload, which Native AOT
links with and which also provides the VC++ runtime DLLs unless `-VcRuntimeDirectory` is given):

```powershell
powershell -ExecutionPolicy Bypass -File launcher/Build-Installer.ps1
```

The first run downloads the pinned portable toolchain (`Prepare-PortableTools.ps1`) and dependency sources
(`Prepare-Dependencies.ps1`) into `launcher/artifacts/`, builds the precompiled aurora/third-party/runtime package
(`Prepare-NativePrebuilt.ps1`), publishes the setup host and the translator, stages the payload, runs the boundary
and dependency audits and writes `launcher/dist/opuc1-recomp-setup.exe`. The setup host and the translator are Native AOT
executables; the payload leaves out the parts of the toolchain the build never uses (`PayloadPruning.ps1`) and carries
no second copy of the host. Linux: `bash launcher/build-appimage.sh` (also needs `clang` and `zlib1g-dev` for Native AOT).
macOS: `launcher/macos/build-setup-pkg.command` (see the script header for the inputs).

`opuc1-recomp-setup.exe --self-test` runs the host's self-tests (no game data needed).
`Test-PinnedFacts.ps1` checks the facts the scripts and the C# host both carry (game id, entry point, dependency
list, build step ids).

## Source layout

| Path | What |
|---|---|
| `Opuc1Recomp.Setup.Common/` | shared by all hosts: runtime Config.toml editing, portable root, progress protocol, nodtool, product icon from the save banner, SYSCONF seeding, names |
| `Opuc1Recomp.Setup.Windows/` | Windows host: guided setup, install/repair/uninstall transactions, launch, self-tests |
| `Opuc1Recomp.Setup.Linux/` | Linux and macOS host |
| `Opuc1Recomp.Setup.Common.Cli/` | packaging-time helper that fetches the pinned nodtool |
| `LocalBuild.ps1`, `NativeBuildFlags.ps1` | the translate-and-compile run inside an installation (Windows) |
| `local-build.sh`, `local-build-macos.command` | the same for Linux and macOS |
| `Build-Installer.ps1`, `PayloadPruning.ps1`, `build-appimage.sh`, `macos/` | release packaging (`PayloadPruning.ps1`: what the Windows payload leaves out) |
| `Prepare-*.ps1`, `Prepare-NativePrebuilt.sh`, `prepare-portable-tools.sh` | pinned toolchain, dependencies, precompiled package |
| `Test-*.ps1`, `Test-*.sh` | release audits |

## Credits

Based on the `Launcher/` setup of [WiiCompiled](https://github.com/patchzyy/wiicompiled) (GPL-3.0) at the commit the
rest of this repository is based on (`a170c80`, identical to release 0.9.5 apart from the version number).
