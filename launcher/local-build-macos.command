#!/usr/bin/env bash
# Native macOS build automation: optionally extract -> translate -> compile -> publish .app.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
default_workspace=$(cd "$script_dir/.." && pwd)
# shellcheck source=build-cache.sh
source "$script_dir/build-cache.sh"

fail() { printf 'local-build-macos.command: error: %s\n' "$*" >&2; exit 1; }
step() { printf 'OPUCBUILD:STEP:%s %s\n' "$1" "$2"; }
assert_file() { [[ -f "$1" ]] || fail "$2 is missing: $1"; }

usage() {
    cat <<'EOF'
Usage: local-build-macos.command --output-dir DIR [options]

  --workspace DIR                 Repository root (default: this script's parent directory)
  --output-dir DIR                Directory that receives the game's .app bundle (required)
  --game IMAGE --nodtool PATH     Extract and verify your own disc image first
  --icon-dir DIR                  Product icon directory (opuc1_recomp.png) for the app icon
  --force-clean-build             Delete local generated and current-architecture native build caches
  --parallel N                    Pin translation and build parallelism
  --cmake PATH --ninja PATH       Override build tools
  --dotnet PATH                   Override dotnet
  --translator-bin PATH           Use a self-contained Translator.Cli executable
  --setup-version VERSION         Version recorded in the runtime log (default: <Version> in
                                  launcher/Directory.Build.props when present)
EOF
}

workspace="$default_workspace"; output_dir=""; icon_dir=""
game=""; nodtool=""; force_clean=0
parallel=0; cmake_bin=cmake; ninja_bin=ninja; dotnet_bin=dotnet; translator_bin=""
setup_version=""
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

while (($#)); do
    case "$1" in
        --workspace) require_value "$@"; workspace=$2; shift 2 ;;
        --output-dir) require_value "$@"; output_dir=$2; shift 2 ;;
        --icon-dir) require_value "$@"; icon_dir=$2; shift 2 ;;
        --game) require_value "$@"; game=$2; shift 2 ;;
        --nodtool) require_value "$@"; nodtool=$2; shift 2 ;;
        --force-clean-build) force_clean=1; shift ;;
        --parallel) require_value "$@"; parallel=$2; shift 2 ;;
        --cmake) require_value "$@"; cmake_bin=$2; shift 2 ;;
        --ninja) require_value "$@"; ninja_bin=$2; shift 2 ;;
        --dotnet) require_value "$@"; dotnet_bin=$2; shift 2 ;;
        --translator-bin) require_value "$@"; translator_bin=$2; shift 2 ;;
        --setup-version) require_value "$@"; setup_version=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ $(uname -s) == Darwin ]] || fail 'this build script is for macOS only'
macos_arch=$(uname -m)
case "$macos_arch" in arm64|x86_64) ;; *) fail "unsupported macOS architecture: $macos_arch" ;; esac
macos_deployment_target=12.0
macos_sysroot=$(xcrun --show-sdk-path 2>/dev/null) && [[ -d "$macos_sysroot" ]] || fail 'could not resolve the macOS SDK; install the Xcode Command Line Tools (xcode-select --install)'
workspace=$(cd "$workspace" && pwd)
if [[ -z "$setup_version" && -f "$workspace/launcher/Directory.Build.props" ]]; then
    setup_version=$(sed -nE 's/.*<Version>([^<]+)<\/Version>.*/\1/p' "$workspace/launcher/Directory.Build.props" | head -n 1)
fi
[[ "$setup_version" =~ ^[0-9A-Za-z.+-]*$ ]] || fail '--setup-version contains unsupported characters'
[[ -n "$output_dir" ]] || fail '--output-dir is required'
if [[ -n "$game" || -n "$nodtool" ]]; then [[ -n "$game" && -n "$nodtool" ]] || fail '--game and --nodtool must be supplied together'; fi
if [[ -n "$icon_dir" ]]; then assert_file "$icon_dir/opuc1_recomp.png" 'product icon'; icon_dir=$(cd "$icon_dir" && pwd); fi
# Apple clang must match the SDK headers; another clang earlier on PATH may not.
cc_bin=$(xcrun -f clang 2>/dev/null) || fail 'Apple clang not found; install the Xcode Command Line Tools (xcode-select --install)'
cxx_bin=$(xcrun -f clang++ 2>/dev/null) || fail 'Apple clang++ not found; install the Xcode Command Line Tools (xcode-select --install)'
for tool in "$cmake_bin" "$ninja_bin" "$cc_bin" "$cxx_bin" shasum; do command -v "$tool" >/dev/null || fail "required tool not found: $tool"; done

assets="$workspace/Assets"; generated="$workspace/generated"
functions="$generated/functions"; metadata="$generated/base_translation_output.json"
sources_bundle="$generated/base_translation_sources.bin"; shards="$generated/build_shards"
native_build="$workspace/native-build-macos-$macos_arch"
aurora_dir="$workspace/third_party/aurora"
assert_file "$aurora_dir/CMakeLists.txt" 'Aurora source tree'
x86_cpu_profile=v3
if [[ "$macos_arch" == x86_64 ]]; then
    native_macros=$("$cc_bin" -march=native -dM -E -x c /dev/null 2>/dev/null || true)
    for macro in __AVX__ __AVX2__ __BMI__ __BMI2__ __F16C__ __FMA__ __LZCNT__ __MOVBE__; do
        if ! grep -q "^#define $macro " <<<"$native_macros"; then
            x86_cpu_profile=v2
            printf 'OPUCBUILD: This CPU does not support x86-64-v3; building for x86-64-v2\n'
            break
        fi
    done
fi
if [[ -n "$game" ]]; then "$script_dir/macos/extract-disc.command" --workspace "$workspace" --game "$game" --assets-dir "$assets" --nodtool "$nodtool"; fi
assert_file "$assets/main.dol" 'extracted main.dol'
# The translation project is the one pinned to the extracted main.dol.
source_project=$(opuc_resolve_project "$workspace" "$assets") || fail 'the extracted game cannot be translated'
entry_point=$(opuc_project_entry_point "$source_project"); [[ -n "$entry_point" ]] || fail 'could not find the translation entry point'

if (( force_clean )); then
    printf 'OPUCBUILD: Discarding translation and native build caches\n'
    rm -rf "$generated" "$native_build"
fi

if [[ -n "$translator_bin" ]]; then
    assert_file "$translator_bin" 'Translator.Cli executable'
    translator() { "$translator_bin" "$@"; }
else
    command -v "$dotnet_bin" >/dev/null || fail "required tool not found: $dotnet_bin"
    translator_dll="$workspace/translator/src/Translator.Cli/bin/Release/net10.0/Translator.Cli.dll"
    if [[ ! -f "$translator_dll" ]]; then
        step build-translator 'Building the translator'
        "$dotnet_bin" build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    fi
    translator() { "$dotnet_bin" "$translator_dll" "$@"; }
fi

cpu=$(sysctl -n hw.ncpu); mem_gib=$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )); (( mem_gib < 1 )) && mem_gib=1
if (( parallel > 0 )); then translator_threads=$parallel; translated_jobs=$parallel; global_jobs=$parallel
else translator_threads=$(( cpu < 16 ? cpu : 16 )); translated_jobs=$(( cpu < mem_gib / 2 ? cpu : mem_gib / 2 )); (( translated_jobs < 1 )) && translated_jobs=1; global_jobs=$cpu; fi

mkdir -p "$generated"
# The checkout's recomp.yml points at data/extract (the developer layout); the build translates a
# copy that names Assets/main.dol.
project=$(opuc_write_build_project "$workspace" "$source_project") || fail 'could not prepare the translation project'

translation_provenance="$generated/translation-provenance.json"
translation_fingerprint=$(opuc_translation_identity "$workspace" "${translator_bin:-$(dirname "${translator_dll:-}")}" "$script_dir/local-build-macos.command")
if [[ "$(opuc_read_identity "$translation_provenance" TranslationFingerprint)" == "$translation_fingerprint" && -f "$metadata" && -f "$sources_bundle" ]]; then
    step reuse-translation 'Reusing the completed translation'
else
    rm -f "$translation_provenance"; mkdir -p "$functions"
    step translate 'Translating the user-owned game'
    translator translate-recursive "$entry_point" --project "$project" --outdir "$functions" --output-metadata "$metadata" --production-source-bundle "$sources_bundle" --no-function-files --prune-stale --threads "$translator_threads"
    opuc_write_identity "$translation_provenance" TranslationFingerprint "$translation_fingerprint"
fi
step generate-data-init 'Generating local game data initialization'; translator generate-data-init --project "$project"
step emit-build-shards 'Preparing native build shards'
translator emit-build-shards --project "$project" --base-metadata "$metadata" --base-functions-dir "$functions" --native-source-dir "$workspace/runtime/src" --out "$shards"

toolchain_provenance="$native_build/toolchain-provenance.json"
toolchain_fingerprint=$(printf '%s\n' "$(opuc_toolchain_identity "$workspace" "$cc_bin" "$cxx_bin" "$cmake_bin" "$ninja_bin")" "$cc_bin" "$cxx_bin" | opuc_sha256)
if [[ -d "$native_build" && "$(opuc_read_identity "$toolchain_provenance" ToolchainFingerprint)" != "$toolchain_fingerprint" ]]; then
    printf 'OPUCBUILD: The toolchain or a pinned dependency changed; rebuilding the native build from scratch\n'
    rm -rf "$native_build"
fi
step configure-native 'Configuring the native toolchain'
# Use Aurora's pinned SDL3 source on macOS. A system SDL3 can be older than Aurora's required API
# even when find_package() succeeds.
configure_args=(-S "$workspace/runtime" -B "$native_build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER="$cc_bin" -DCMAKE_CXX_COMPILER="$cxx_bin" -DCMAKE_MAKE_PROGRAM="$ninja_bin"
    -DCMAKE_OSX_ARCHITECTURES="$macos_arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$macos_deployment_target"
    -DCMAKE_OSX_SYSROOT="$macos_sysroot" -DMKW_AURORA_DIR="$aurora_dir"
    -DMKW_TRANSLATED_COMPILE_JOBS="$translated_jobs" -DMKW_X86_CPU_PROFILE="$x86_cpu_profile"
    -DAURORA_SDL3_PROVIDER=vendor -DMKW_SETUP_VERSION="$setup_version")
if [[ -n "$icon_dir" ]]; then configure_args+=(-DOPUC_ICON_DIR="$icon_dir"); fi
"$cmake_bin" "${configure_args[@]}"
opuc_write_identity "$toolchain_provenance" ToolchainFingerprint "$toolchain_fingerprint"
step compile 'Compiling the game locally'; "$cmake_bin" --build "$native_build" --target opuc1_recomp --parallel "$global_jobs"

publish_args=(--build-dir "$native_build" --output-dir "$output_dir" --architecture "$macos_arch" --minimum-system-version "$macos_deployment_target")
if [[ -n "$setup_version" ]]; then publish_args+=(--version "$setup_version"); fi
if [[ -n "$icon_dir" ]]; then publish_args+=(--icon-png "$icon_dir/opuc1_recomp.png"); fi
"$script_dir/macos/publish-app.command" "${publish_args[@]}"
cat > "$output_dir/local-build.json" <<JSON
{
  "SchemaVersion": 1,
  "BuiltUtc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "DolSha256": "$(opuc_sha256 "$assets/main.dol")",
  "SetupVersion": "$setup_version",
  "Compiler": "$("$cxx_bin" --version | head -1)"
}
JSON
printf 'OPUCBUILD:OUTPUT=%s\n' "$output_dir"
