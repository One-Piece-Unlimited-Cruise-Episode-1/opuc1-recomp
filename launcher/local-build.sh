#!/usr/bin/env bash
# Linux build automation: translate -> emit build shards -> configure -> compile -> publish.
#
# The native-Linux counterpart to launcher/LocalBuild.ps1 and tools/build.ps1 (translate,
# configure, compile stages). Without --native-prebuilt-dir, aurora is built from source, letting
# its own CMake auto-detect Vulkan and vendor SDL3/Dawn via FetchContent. With it, aurora is not
# compiled at all - see launcher/Prepare-NativePrebuilt.sh, which harvests exactly that package.
set -euo pipefail

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log_step() {
    # $1 = machine-readable step id (BuildStepIds in Opuc1Recomp.Setup.Common/InstallProgress.cs),
    # $2 = human sentence for the terminal.
    printf 'OPUCBUILD:STEP:%s %s\n' "$1" "$2"
}

fail() {
    echo "local-build.sh: error: $*" >&2
    exit 1
}

assert_file() {
    [[ -f "$1" ]] || fail "$2 is missing: $1"
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found on PATH (override with --$2)"
}

sha256_of() {
    opuc_sha256 "$1"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=build-cache.sh
source "$script_dir/build-cache.sh"
workspace=$(cd "$script_dir/.." && pwd)
output_dir=""
icon_dir=""
force_clean_build=0
parallel_override=0
cc_override=""
cxx_override=""
cmake_override=""
ninja_override=""
dotnet_override=""
translator_dll_override=""
translator_bin_override=""
fuse_ld_override=""
native_prebuilt_dir=""
sysroot=""
setup_version=""
x86_cpu_profile=auto

usage() {
    cat <<'EOF'
Usage: local-build.sh --output-dir DIR [options]

  --workspace DIR                 Repository root (default: this script's parent directory)
  --output-dir DIR                Where the built game is published (required)
  --icon-dir DIR                  Product icon directory written by the setup (opuc1_recomp.png/.ico/.rc);
                                  passed to CMake as OPUC_ICON_DIR and copied beside the game
  --force-clean-build             Discard every translation/build cache first
  --parallel N                    Pin translator threads, translated-shard job pool, and Ninja parallelism to N
  --cc PATH / --cxx PATH          C/C++ compiler (default: clang/clang++ on PATH)
  --fuse-ld NAME_OR_PATH          Linker passed to clang as -fuse-ld=NAME_OR_PATH (default: clang's own default linker)
  --cmake PATH / --ninja PATH     Build tools (default: on PATH)
  --dotnet PATH                   dotnet executable (default: on PATH)
  --translator-dll PATH           Pre-built Translator.Cli.dll (skips building the translator; still needs --dotnet to run it)
  --translator-bin PATH           Self-contained Translator.Cli executable (skips building AND needs no dotnet at all)
  --native-prebuilt-dir DIR       Precompiled aurora/third-party package (see Prepare-NativePrebuilt.sh);
                                   skips compiling third_party/aurora from source entirely
  --sysroot PATH                   Passed to CMake as -DCMAKE_SYSROOT: where the compiler resolves
                                   standard headers/startup files
  --setup-version VERSION          Version recorded in the runtime log (default: <Version> in
                                   launcher/Directory.Build.props when present)
  --x86-cpu-profile {auto|v2|v3}   x86-64 instruction set to build for (default: auto, v3 when
                                   this machine supports it)
EOF
}

# Same guard as Prepare-NativePrebuilt.sh: a value-taking option must not swallow a missing,
# empty, or option-like value.
require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        fail "$1 requires a value"
    fi
    if [[ "$2" == --* || "$2" == -h ]]; then
        fail "$1 requires a value; \"$2\" looks like another option"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workspace) require_value "$@"; workspace=$(cd "$2" && pwd); shift 2 ;;
        --output-dir) require_value "$@"; output_dir=$2; shift 2 ;;
        --icon-dir) require_value "$@"; icon_dir=$2; shift 2 ;;
        --force-clean-build) force_clean_build=1; shift ;;
        --parallel) require_value "$@"; parallel_override=$2; shift 2 ;;
        --cc) require_value "$@"; cc_override=$2; shift 2 ;;
        --cxx) require_value "$@"; cxx_override=$2; shift 2 ;;
        --fuse-ld) require_value "$@"; fuse_ld_override=$2; shift 2 ;;
        --cmake) require_value "$@"; cmake_override=$2; shift 2 ;;
        --ninja) require_value "$@"; ninja_override=$2; shift 2 ;;
        --dotnet) require_value "$@"; dotnet_override=$2; shift 2 ;;
        --translator-dll) require_value "$@"; translator_dll_override=$2; shift 2 ;;
        --translator-bin) require_value "$@"; translator_bin_override=$2; shift 2 ;;
        --native-prebuilt-dir) require_value "$@"; native_prebuilt_dir=$2; shift 2 ;;
        --sysroot) require_value "$@"; sysroot=$2; shift 2 ;;
        --setup-version) require_value "$@"; setup_version=$2; shift 2 ;;
        --x86-cpu-profile) require_value "$@"; x86_cpu_profile=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

if [[ -z "$setup_version" && -f "$workspace/launcher/Directory.Build.props" ]]; then
    setup_version=$(sed -nE 's/.*<Version>([^<]+)<\/Version>.*/\1/p' "$workspace/launcher/Directory.Build.props" | head -n 1)
fi
[[ "$setup_version" =~ ^[0-9A-Za-z.+-]*$ ]] || fail "--setup-version contains unsupported characters"

[[ -n "$output_dir" ]] || { usage; fail "--output-dir is required"; }
case "$x86_cpu_profile" in
    auto|v2|v3) ;;
    *) fail "--x86-cpu-profile must be auto, v2, or v3" ;;
esac
if [[ -n "$icon_dir" ]]; then
    assert_file "$icon_dir/opuc1_recomp.png" "Product icon"
    icon_dir=$(cd "$icon_dir" && pwd)
fi

# ---------------------------------------------------------------------------
# Tool resolution and prerequisite checks
# ---------------------------------------------------------------------------

dotnet_bin=${dotnet_override:-dotnet}
cmake_bin=${cmake_override:-cmake}
ninja_bin=${ninja_override:-ninja}
cc_bin=${cc_override:-clang}
cxx_bin=${cxx_override:-clang++}
aurora_dir=$workspace/third_party/aurora

# A self-contained --translator-bin needs no dotnet at all (it bundles its own runtime); dotnet is
# only required when the translator has to be built from source or run as a plain .dll.
if [[ -z "$translator_bin_override" ]]; then
    require_command "$dotnet_bin" dotnet
fi
require_command "$cmake_bin" cmake
require_command "$ninja_bin" ninja
require_command "$cc_bin" cc
require_command "$cxx_bin" cxx

if [[ -n "$native_prebuilt_dir" ]]; then
    assert_file "$native_prebuilt_dir/native_prebuilt.cmake" "Native prebuilt package"
    assert_file "$native_prebuilt_dir/provenance.json" "Native prebuilt provenance"
    native_prebuilt_dir=$(cd "$native_prebuilt_dir" && pwd)
    assert_file "$aurora_dir/CMakeLists.txt" "Aurora source tree"
    aurora_fingerprint=$(opuc_fingerprint_tree "$aurora_dir" extern build)
    "$cmake_bin" "-DAURORA_FINGERPRINT=$aurora_fingerprint" "-DPROVENANCE_FILE=$native_prebuilt_dir/provenance.json" \
        -P "$script_dir/../runtime/cmake/AuroraFingerprint.cmake"
fi

host_supports_x86_64_v3() {
    local macros macro
    macros=$("$cc_bin" -march=native -dM -E -x c /dev/null 2>/dev/null) || return 1
    for macro in __AVX__ __AVX2__ __BMI__ __BMI2__ __F16C__ __FMA__ __LZCNT__ __MOVBE__; do
        grep -q "^#define $macro " <<<"$macros" || return 1
    done
}

if [[ "$x86_cpu_profile" == "auto" ]]; then
    x86_cpu_profile=v3
    if [[ "$(uname -m)" == "x86_64" ]] && ! host_supports_x86_64_v3; then
        x86_cpu_profile=v2
        echo "OPUCBUILD: This CPU does not support x86-64-v3; building for x86-64-v2"
    fi
fi

assets=$workspace/Assets
generated=$workspace/generated
functions=$generated/functions
metadata=$generated/base_translation_output.json
sources_bundle=$generated/base_translation_sources.bin
shards=$generated/build_shards
build=$workspace/native-build
translation_provenance=$generated/translation-provenance.json
toolchain_provenance=$build/toolchain-provenance.json
product=opuc1-recomp

assert_file "$assets/main.dol" "Extracted main.dol (run the setup with --game <your disc image> first)"
assert_file "$aurora_dir/CMakeLists.txt" "Aurora source tree"
# The translation project is the one pinned to the extracted main.dol.
source_project=$(opuc_resolve_project "$workspace" "$assets") || fail "the extracted game cannot be translated"
entry_point=$(opuc_project_entry_point "$source_project")
[[ -n "$entry_point" ]] || fail "Could not find a translation entry point in $source_project"

# The AppImage bundles Clang, but Linux startup objects and the C/C++ link runtimes
# still come from the host. Check them before the expensive translation so a missing
# development package produces a useful error instead of CMake's generic exit 1.
link_probe_dir=$(mktemp -d)
link_probe_flags=()
[[ -z "$sysroot" ]] || link_probe_flags+=(--sysroot="$sysroot")
[[ -z "$fuse_ld_override" ]] || link_probe_flags+=(-fuse-ld="$fuse_ld_override")
printf 'int main(void) { return 0; }\n' > "$link_probe_dir/probe.c"
cat > "$link_probe_dir/probe.cpp" <<'EOF'
#include <vector>
int main() { std::vector<int> values{1}; return values.front() - 1; }
EOF
if ! "$cc_bin" ${link_probe_flags[@]+"${link_probe_flags[@]}"} "$link_probe_dir/probe.c" -o "$link_probe_dir/probe-c" > "$link_probe_dir/error" 2>&1; then
    cat "$link_probe_dir/error" >&2
    rm -rf "$link_probe_dir"
    fail "The C compiler cannot link a test program. Linux needs C development files (glibc startup objects and a compiler runtime) in addition to the bundled Clang. Install your distribution's C development packages."
fi
if ! "$cxx_bin" ${link_probe_flags[@]+"${link_probe_flags[@]}"} "$link_probe_dir/probe.cpp" -o "$link_probe_dir/probe-cxx" > "$link_probe_dir/error" 2>&1; then
    cat "$link_probe_dir/error" >&2
    rm -rf "$link_probe_dir"
    fail "The C++ compiler cannot link a test program. Install your distribution's C++ development packages."
fi
rm -rf "$link_probe_dir"

translator_bin=$translator_bin_override
translator_dll=$translator_dll_override
if [[ -n "$translator_bin" ]]; then
    assert_file "$translator_bin" "Translator.Cli executable"
    translator() { "$translator_bin" "$@"; }
else
    if [[ -z "$translator_dll" ]]; then
        translator_dll=$workspace/translator/src/Translator.Cli/bin/Release/net10.0/Translator.Cli.dll
        log_step build-translator "Building the translator"
        "$dotnet_bin" build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    fi
    assert_file "$translator_dll" "Translator.Cli.dll"
    translator() { "$dotnet_bin" "$translator_dll" "$@"; }
fi

# ---------------------------------------------------------------------------
# Parallelism: three independent knobs, same reasoning as LocalBuild.ps1 -
# translator_threads (translation's own worker threads), translated_jobs (the real RAM guard,
# capping concurrent compiles of memory-hungry translated TUs via Ninja's MKW_TRANSLATED_COMPILE_JOBS
# pool), global_jobs (Ninja's overall parallelism). --parallel pins all three.
# ---------------------------------------------------------------------------

cpu_count=$(nproc)
mem_gib=$(( $(awk '/^MemTotal:/{print $2}' /proc/meminfo) / 1024 / 1024 ))
(( mem_gib < 1 )) && mem_gib=1

if (( parallel_override > 0 )); then
    translator_threads=$parallel_override
    translated_jobs=$parallel_override
    global_jobs=$parallel_override
else
    translator_threads=$(( cpu_count < 16 ? cpu_count : 16 ))
    (( translator_threads < 1 )) && translator_threads=1
    mem_based_cap=$(( mem_gib / 2 ))
    (( mem_based_cap < 1 )) && mem_based_cap=1
    translated_jobs=$(( cpu_count < mem_based_cap ? cpu_count : mem_based_cap ))
    (( translated_jobs < 1 )) && translated_jobs=1
    global_jobs=$(( translated_jobs > cpu_count ? translated_jobs : cpu_count ))
fi

# ---------------------------------------------------------------------------
# Translation cache: this script is the only owner of the reuse decision. The identity (see
# build-cache.sh) covers main.dol, the project, native registrations, translator and this script.
# ---------------------------------------------------------------------------

if (( force_clean_build )); then
    echo "OPUCBUILD: A clean build was requested; discarding every translation and build cache"
    rm -rf "$generated" "$build"
fi

translator_identity_input=${translator_bin:-$(dirname "$translator_dll")}
translation_fingerprint=$(opuc_translation_identity "$workspace" "$translator_identity_input" "$script_dir/local-build.sh")
reuse_translation=0
if [[ "$(opuc_read_identity "$translation_provenance" TranslationFingerprint)" == "$translation_fingerprint" \
      && -f "$metadata" && -f "$sources_bundle" ]]; then
    reuse_translation=1
fi

mkdir -p "$generated"
# The checkout's recomp.yml points at data/extract (the developer layout); the build translates
# a copy that names Assets/main.dol.
project=$(opuc_write_build_project "$workspace" "$source_project") || fail "could not prepare the translation project"

if (( reuse_translation )); then
    log_step reuse-translation "Reusing the completed translation"
else
    rm -f "$translation_provenance"

    log_step translate "Translating the user-owned game"
    translator translate-recursive "$entry_point" --project "$project" \
        --outdir "$functions" --output-metadata "$metadata" \
        --production-source-bundle "$sources_bundle" \
        --no-function-files --prune-stale --threads "$translator_threads"

    opuc_write_identity "$translation_provenance" TranslationFingerprint "$translation_fingerprint"
fi

log_step generate-data-init "Generating local game data initialization"
translator generate-data-init --project "$project"

log_step emit-build-shards "Preparing local native build shards"
translator emit-build-shards --project "$project" --base-metadata "$metadata" \
    --base-functions-dir "$functions" --native-source-dir "$workspace/runtime/src" --out "$shards"

# ---------------------------------------------------------------------------
# Native configure + build.
# ---------------------------------------------------------------------------

keep_native_build=0
stale_reason=""
if [[ -f "$build/CMakeCache.txt" ]]; then
    expected_home=$workspace/runtime
    cache_home=$(grep '^CMAKE_HOME_DIRECTORY:INTERNAL=' "$build/CMakeCache.txt" | cut -d= -f2- || true)
    if [[ -n "$cache_home" && "$(cd "$cache_home" 2>/dev/null && pwd)" == "$expected_home" ]]; then
        keep_native_build=1
        # CMake caches auxiliary tool paths (CMAKE_AR/RANLIB/LINKER/ASM_COMPILER and their
        # per-language variants) only once, when a language's compiler is first checked. Any
        # :FILEPATH= cache entry whose recorded path no longer exists means this cache belongs to a
        # toolchain location that is gone (a moved system toolchain, a relocated portable-tools
        # directory), so the cache is discarded.
        while IFS= read -r tool_path; do
            [[ -n "$tool_path" ]] || continue
            # CMake's sentinel for an optional tool that was legitimately never found.
            [[ "$tool_path" != *-NOTFOUND ]] || continue
            if [[ ! -e "$tool_path" ]]; then
                keep_native_build=0
                stale_reason=" (cached tool path no longer exists: $tool_path)"
                break
            fi
        done < <(grep -o ':FILEPATH=.*' "$build/CMakeCache.txt" | sed 's/^:FILEPATH=//')
    fi
fi
# The AppImage keeps tool paths stable across updates, so a changed compiler is only visible here.
toolchain_fingerprint=$(opuc_toolchain_identity "$workspace" "$cc_bin" "$cxx_bin" "$cmake_bin" "$ninja_bin")
if [[ "$keep_native_build" -eq 1 && \
      "$(opuc_read_identity "$toolchain_provenance" ToolchainFingerprint)" != "$toolchain_fingerprint" ]]; then
    keep_native_build=0
    stale_reason=" (the toolchain or a pinned dependency changed)"
fi
if [[ -d "$build" && "$keep_native_build" -eq 0 ]]; then
    echo "OPUCBUILD: The native build cache does not belong to this workspace path or toolchain; rebuilding from scratch$stale_reason"
    rm -rf "$build"
elif [[ "$keep_native_build" -eq 1 ]]; then
    echo "OPUCBUILD: Reusing the incremental native build directory"
fi

configure_args=(-S "$workspace/runtime" -B "$build" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER="$cc_bin" -DCMAKE_CXX_COMPILER="$cxx_bin"
    -DCMAKE_MAKE_PROGRAM="$ninja_bin"
    -DMKW_AURORA_DIR="$aurora_dir"
    -DMKW_TRANSLATED_COMPILE_JOBS="$translated_jobs"
    -DMKW_SETUP_VERSION="$setup_version"
    -DMKW_X86_CPU_PROFILE="$x86_cpu_profile")
if [[ -n "$icon_dir" ]]; then
    configure_args+=(-DOPUC_ICON_DIR="$icon_dir")
fi
# CMAKE_AR/RANLIB/LINKER/ASM_COMPILER do not inherit the stability of a symlinked --cc (AppRun
# points it at a link it re-targets every launch): clang's driver resolves its own real path when
# it locates sibling tools, so CMake's auto-detection would record the ephemeral mount path and
# every run's differing command line would make Ninja rebuild everything. Deriving them from
# $cc_bin and passing them explicitly keeps them pinned to the same stable value.
if [[ "$cc_bin" == */* ]]; then
    toolchain_bin=$(dirname "$cc_bin")
    [[ -x "$toolchain_bin/llvm-ar" ]] && configure_args+=(-DCMAKE_AR="$toolchain_bin/llvm-ar" -DCMAKE_ASM_COMPILER_AR="$toolchain_bin/llvm-ar" -DCMAKE_C_COMPILER_AR="$toolchain_bin/llvm-ar" -DCMAKE_CXX_COMPILER_AR="$toolchain_bin/llvm-ar")
    [[ -x "$toolchain_bin/llvm-ranlib" ]] && configure_args+=(-DCMAKE_RANLIB="$toolchain_bin/llvm-ranlib" -DCMAKE_ASM_COMPILER_RANLIB="$toolchain_bin/llvm-ranlib" -DCMAKE_C_COMPILER_RANLIB="$toolchain_bin/llvm-ranlib" -DCMAKE_CXX_COMPILER_RANLIB="$toolchain_bin/llvm-ranlib")
    [[ -x "$toolchain_bin/ld.lld" ]] && configure_args+=(-DCMAKE_LINKER="$toolchain_bin/ld.lld")
    configure_args+=(-DCMAKE_ASM_COMPILER="$cc_bin")
fi
if [[ -n "$fuse_ld_override" ]]; then
    # CMAKE_EXE_LINKER_FLAGS is one string that CMake pastes into the shell link command.
    printf -v fuse_ld_quoted '%q' "$fuse_ld_override"
    configure_args+=(-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$fuse_ld_quoted")
fi
if [[ -n "$native_prebuilt_dir" ]]; then
    configure_args+=(-DMKW_NATIVE_PREBUILT_DIR="$native_prebuilt_dir")
else
    configure_args+=(-UMKW_NATIVE_PREBUILT_DIR)
fi
if [[ -n "$sysroot" ]]; then
    configure_args+=(-DCMAKE_SYSROOT="$sysroot")
else
    # Clear a cached CMAKE_SYSROOT from a prior configure so an incremental build that moves from
    # one sysroot to none does not silently keep the stale path.
    configure_args+=(-UCMAKE_SYSROOT)
fi

log_step configure-native "Configuring the native toolchain"
"$cmake_bin" "${configure_args[@]}"
opuc_write_identity "$toolchain_provenance" ToolchainFingerprint "$toolchain_fingerprint"

log_step compile "Compiling the game locally"
"$cmake_bin" --build "$build" --target opuc1_recomp --parallel "$global_jobs"

# ---------------------------------------------------------------------------
# Publish: the Linux build links SDL3/Dawn/etc statically, so beside the binary there is only the
# runtime's DSP coefficient ROM (copied there by the build) and the product icon.
# ---------------------------------------------------------------------------

mkdir -p "$output_dir"
assert_file "$build/$product" "Locally compiled game executable"
cp -f "$build/$product" "$output_dir/$product"
assert_file "$build/dsp_coef.bin" "DSP coefficient ROM"
cp -f "$build/dsp_coef.bin" "$output_dir/"
if [[ -n "$icon_dir" ]]; then
    cp -f "$icon_dir/opuc1_recomp.png" "$output_dir/"
fi

dol_sha=$(sha256_of "$assets/main.dol")
compiler_version=$("$cxx_bin" --version | head -1)
built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat > "$output_dir/local-build.json" <<JSON
{
  "SchemaVersion": 1,
  "BuiltUtc": "$built_utc",
  "DolSha256": "$dol_sha",
  "SetupVersion": "$setup_version",
  "Compiler": "$compiler_version"
}
JSON

echo "OPUCBUILD:OUTPUT=$output_dir"
