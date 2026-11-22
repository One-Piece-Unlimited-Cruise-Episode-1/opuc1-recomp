# Third-Party Notices

One Piece: Unlimited Recompilation Episode 1 (based on WiiCompiled) is licensed under the GNU General Public License v3.0
(see [`LICENSE`](LICENSE)). It incorporates, links against, or redistributes the third-party
components listed below. Each remains under its own license and copyright.

Nothing listed here is Nintendo intellectual property. This project ships no game code, assets,
or data of any kind - see the [README](README.md).

---

## Bundled in this repository

### aurora - MIT

Copyright (c) 2022 Luke Street.
Source: <https://github.com/encounter/aurora> - vendored in `third_party/aurora/`, license text at
`third_party/aurora/LICENSE`.

Aurora itself vendors:

- **magic_enum** 0.7.2 - MIT, Copyright (c) 2019-2021 Daniil Goncharov.
  `third_party/aurora/include/magic_enum.hpp`.
  Source: <https://github.com/Neargye/magic_enum>
- **libogc-derived SRAM structures** - zlib-style license, Copyright (c) Michael Wiedenbauer
  (shagkur) and Dave Murphy (WinterMute). `third_party/aurora/lib/card/SRAM.hpp`.
  Source: <https://github.com/devkitPro/libogc>

### Dolphin Emulator data files - GPL-2.0-or-later

Copyright (c) 2003+ Dolphin Emulator Project.
Source: <https://github.com/dolphin-emu/dolphin> - license at
<https://github.com/dolphin-emu/dolphin/blob/master/COPYING>

One data file from Dolphin's `Data/Sys` tree is redistributed here under GPL-2.0-or-later:

| File | Upstream path | Notes |
| --- | --- | --- |
| `runtime/assets/dsp/dsp_coef.bin` | `Data/Sys/GC/dsp_coef.bin` | Free DSP polyphase-resampling coefficient ROM written by the Dolphin team. 4096 bytes, SHA-256 `D7741279C2E8EC5C5FB318F8FBDD6DE6BF583520D288E836A5383233A4238179`. The runtime verifies this hash at build time. |

It contains no Nintendo executable code or game assets.

Dolphin was also used extensively as a behavioural reference during development of this project's
hardware and IOS high-level implementations; the offline WiiConnect24 KD time device
(`runtime/src/hle/wc24/kd_offline.cpp`) follows Dolphin's `NetKDTimeDevice`.

### Dolphin Emulator Riivolution code - GPL-2.0-or-later

Copyright (c) 2021 Dolphin Emulator Project.
The runtime's Riivolution patch handling is a port of Dolphin's
`Source/Core/DiscIO/RiivolutionParser.{h,cpp}` and the external-path resolution rules of
`Source/Core/DiscIO/RiivolutionPatcher.cpp`, adapted in
`runtime/include/hle/riivolution_contract.h` and `runtime/src/hle/storage/riivolution.cpp`
(both marked `SPDX-License-Identifier: GPL-2.0-or-later`).
Source: <https://github.com/dolphin-emu/dolphin>

### pugixml - MIT

Copyright (c) 2006-2025 Arseny Kapoulkine.
The Riivolution XML reader uses pugixml 1.15, vendored in
`runtime/third_party/pugixml` from commit `ee86beb30e4973f5feffe3ce63bfa4fbadf72f38`.
Source and license: <https://github.com/zeux/pugixml>

### Crypto++ 8.9.0 - Boost Software License 1.0 / public domain

Copyright (c) 1995-2019 Wei Dai and contributors.
The runtime uses Crypto++ for SHA-1 and sect233r1 ECDSA key derivation and signing. Its portable
sources are vendored in `runtime/third_party/cryptopp`; assembly implementations are disabled.
Source: <https://github.com/weidai11/cryptopp/tree/CRYPTOPP_8_9_0>. Full license text:
`runtime/third_party/cryptopp/License.txt`.

### toml11 4.4.0 - MIT

Copyright (c) 2017 Toru Niina.
The runtime configuration reader and scalar string writer use the single-header distribution,
vendored in `runtime/third_party/toml11`.
Source: <https://github.com/ToruNiina/toml11/tree/v4.4.0>. Full license text:
`runtime/third_party/toml11/LICENSE`.

### YamlDotNet - MIT

Copyright (c) Antoine Aubry and contributors.
Referenced by `translator/src/Translator.Core`. Source: <https://github.com/aaubry/YamlDotNet>

### libco - ISC (valgrind.h: BSD-style)

Copyright byuu and the higan team.
Non-Windows builds use libco's symmetric stackful coroutines in place of Win32 Fibers for guest
OSThread scheduling (`runtime/src/fiber_manager.cpp`). Vendored in full (all non-Windows
CPU-architecture backends - amd64, x86, arm, aarch64, ppc, ppc64v2, plus the portable sjlj
fallback - though this project's x86_64-only target only ever compiles amd64.c) in
`runtime/third_party/libco` from commit `e18e09d634d612a01781168ad4d76be10a7e3bad`.
Source: <https://github.com/higan-emu/libco>. Full license text:
`runtime/third_party/libco/LICENSE`.

---

## Fetched at build time and redistributed in release builds

These are pinned in `third_party/aurora/extern/CMakeLists.txt`, `third_party/aurora/CMakeLists.txt` and
`third_party/aurora/cmake/AuroraDawnProvider.cmake`. They are not stored in this repository; the build
downloads them - each fetch is pinned to an exact version with a checked SHA-256 - and links or
redistributes the resulting binaries.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| Dawn (WebGPU) | `v20260603.191052` prebuilt | BSD-3-Clause | <https://dawn.googlesource.com/dawn> |
| Tint (part of Dawn) | with Dawn | BSD-3-Clause | <https://dawn.googlesource.com/dawn> |
| DirectXShaderCompiler (`dxcompiler.dll`) | with Dawn | NCSA / University of Illinois Open Source | <https://github.com/microsoft/DirectXShaderCompiler> |
| SDL | 3.4.4 | zlib | <https://github.com/libsdl-org/SDL> |
| libusb (linked into SDL on Windows) | 1.0.30 | LGPL-2.1-or-later | <https://github.com/libusb/libusb> |
| Abseil | LTS 20240722.0 | Apache-2.0 | <https://github.com/abseil/abseil-cpp> |
| Dear ImGui | 1.91.9b-docking | MIT | <https://github.com/ocornut/imgui> |
| {fmt} | 11.1.4 | MIT | <https://github.com/fmtlib/fmt> |
| xxHash | 0.8.4 | BSD-2-Clause | <https://github.com/Cyan4973/xxHash> |
| zlib | 1.3.2 | zlib | <https://github.com/madler/zlib> |
| libpng | 1.6.58 | PNG Reference Library License v2 | <https://github.com/pnggroup/libpng> |
| FreeType | 2.14.3 | **FreeType License (FTL)** - see below | <https://freetype.org/> |
| Zstandard | 1.5.7 | **BSD-3-Clause** - see below | <https://github.com/facebook/zstd> |
| SQLite | 3.51.3 amalgamation | Public domain | <https://sqlite.org/> |
| Tracy Profiler | pinned commit | BSD-3-Clause | <https://github.com/wolfpld/tracy> |
| C++/WinRT | - | MIT (Microsoft) | <https://github.com/microsoft/cppwinrt> |
| nodtool (disc image extraction) | v2.0.0-alpha.10 | MIT OR Apache-2.0 | <https://github.com/encounter/nod> |

### Dual-licensed components - elections made by this project

- **FreeType** is offered under the FreeType License (FTL) or GPL-2.0. **This project elects the
  FreeType License.** The FTL requires the following credit, which is given here and reproduced in
  distributed builds:

  > Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org).
  > All rights reserved.

- **Zstandard** is offered under BSD-3-Clause or GPL-2.0. **This project elects BSD-3-Clause.**
  Copyright (c) Meta Platforms, Inc. and affiliates.

## Development-only dependencies

Not redistributed in any release artifact.

| Component | License |
| --- | --- |
| xUnit.net 2.4.2, xunit.runner.visualstudio 2.4.5 | Apache-2.0 |
| Microsoft.NET.Test.Sdk 17.6.0 | MIT |
| coverlet.collector 6.0.0 | MIT |
| .NET 10 SDK | MIT |

---

## Reference material

Not code, but the documentation this project depends on:

- [WiiBrew](https://wiibrew.org/wiki/) - Wii hardware and IOS documentation.
- [Dolphin Emulator](https://github.com/dolphin-emu/dolphin) - behavioural reference for Wii hardware, IOS and
  the RVL SDK signature database used to name SDK functions in `main.dol`.

---

If you believe a component is missing or misattributed here, please open an issue.
