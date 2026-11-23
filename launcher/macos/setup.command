#!/usr/bin/env bash
# Entry point bundled in "One Piece Unlimited Recompilation Episode 1 Setup.app". The package contains
# source and tools only; a user's own verified disc is extracted into Application Support.
set -euo pipefail

resources=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace_source="$resources/workspace"
cmake_bin="$resources/tools/cmake/bin/cmake"
app_name="One Piece Unlimited Recompilation Episode 1"
# The runtime's own per-user directory (kApplicationDirectoryName): Config.toml, NAND and Logs live here.
support_root="$HOME/Library/Application Support/$app_name"
workspace="$support_root/BuildWorkspace"
products="$support_root/Products"

fail() { printf 'One Piece: Unlimited Recompilation Episode 1 Setup: %s\n' "$*" >&2; exit 1; }
notice() {
    /usr/bin/osascript - "$1" >/dev/null <<'APPLESCRIPT'
on run argv
    display dialog (item 1 of argv) buttons {"OK"} default button "OK" with icon caution
end run
APPLESCRIPT
}
usage() {
    cat <<'EOF'
Usage: setup.command --game IMAGE [--sysconf FILE] [--install-location {user|applications}]

Without arguments this script opens file pickers. It is normally launched by
"One Piece Unlimited Recompilation Episode 1 Setup.app", not run directly.
EOF
}

game=""; sysconf=""; install_location=applications
while (($#)); do
    case "$1" in
        --game) game=${2:-}; shift 2 ;;
        --sysconf) sysconf=${2:-}; shift 2 ;;
        --install-location) install_location=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[[ "$install_location" == user || "$install_location" == applications ]] || fail '--install-location must be user or applications'

host_arch=$(uname -m)
case "$host_arch" in arm64|x86_64) ;; *) fail "unsupported macOS architecture: $host_arch" ;; esac
# Same x86-64-v2 check as runtime/src/host_cpu_baseline.cpp, before anything is built;
# local-build-macos.command builds for v2 when the CPU lacks v3.
if [[ "$host_arch" == x86_64 ]]; then
    cpu_features=$(/usr/sbin/sysctl -n machdep.cpu.features machdep.cpu.extfeatures 2>/dev/null | tr '\n' ' ' || true)
    if [[ -n "${cpu_features// /}" ]]; then
        missing_features=""
        for feature in SSE3=SSE3 SSSE3=SSSE3 CX16=CMPXCHG16B SSE4.1=SSE4.1 SSE4.2=SSE4.2 POPCNT=POPCNT \
                LAHF=LAHF-SAHF; do
            [[ " $cpu_features " == *" ${feature%%=*} "* ]] || missing_features+="${missing_features:+, }${feature#*=}"
        done
        if [[ -n "$missing_features" ]]; then
            message="This build needs a processor that supports the x86-64-v2 instruction set.

Missing on this machine: $missing_features

x86-64-v2 covers Intel Core processors from Nehalem (2008) onward and AMD processors from Jaguar (2013) onward."
            printf 'One Piece: Unlimited Recompilation Episode 1 Setup: %s\n' "$message" >&2
            notice "$message"
            exit 1
        fi
    fi
fi
host_tools="$resources/tools/$host_arch"
nodtool="$host_tools/nodtool"
translator="$host_tools/Translator.Cli"
setup_cli="$host_tools/opuc1-recomp-setup"
ninja_bin="$host_tools/ninja"

if [[ -z "$game" ]]; then
    game=$(/usr/bin/osascript <<'APPLESCRIPT'
set selectedFile to choose file with prompt "Choose your own One Piece Unlimited Cruise Episode 1 disc image (PAL, ROUPAF)"
POSIX path of selectedFile
APPLESCRIPT
) || exit 0
fi

[[ -x "$nodtool" ]] || fail 'the packaged nodtool is missing or not executable'
[[ -x "$translator" ]] || fail 'the packaged Translator.Cli is missing or not executable'
[[ -x "$setup_cli" ]] || fail 'the packaged opuc1-recomp-setup is missing or not executable'
[[ -x "$cmake_bin" && -x "$ninja_bin" ]] || fail 'the packaged CMake or Ninja tool is missing'
if ! /usr/bin/xcode-select -p >/dev/null 2>&1; then
    notice 'Xcode Command Line Tools are required once to compile the game. Click OK, complete the Apple installer, then run the setup again.'
    /usr/bin/xcode-select --install || true
    exit 1
fi
for tool in "$nodtool" "$translator" "$setup_cli" "$cmake_bin" "$ninja_bin"; do
    /usr/bin/lipo "$tool" -verify_arch "$host_arch" >/dev/null 2>&1 || \
        fail "the packaged $(basename "$tool") does not support $host_arch"
done

mkdir -p "$support_root" "$products"

# The game boots without a Wii SYSCONF in the NAND, but uses the console settings in one when the user has it:
# an existing one is kept, otherwise one from a Dolphin user folder is copied. seed-sysconf fails only for an explicit
# file that is not a SYSCONF; the user may then pick another one or continue without.
seed_args=()
if [[ -n "$sysconf" ]]; then seed_args+=(--sysconf "$sysconf"); fi
if ! "$setup_cli" seed-sysconf ${seed_args[@]+"${seed_args[@]}"}; then
    sysconf=$(/usr/bin/osascript <<'APPLESCRIPT'
set selectedFile to choose file with prompt "That file is not a Wii SYSCONF. Choose the SYSCONF from your Wii (NAND /shared2/sys/SYSCONF) or from a Dolphin user folder (Wii/shared2/sys/SYSCONF), or cancel to continue without one."
POSIX path of selectedFile
APPLESCRIPT
) || sysconf=""
    if [[ -n "$sysconf" ]]; then
        "$setup_cli" seed-sysconf --sysconf "$sysconf" || fail 'the chosen file is not a Wii SYSCONF'
    fi
fi

"$resources/sync-workspace.command" "$workspace_source" "$workspace"

# shellcheck source=../build-cache.sh
source "$workspace/launcher/build-cache.sh"
"$workspace/launcher/macos/extract-disc.command" --workspace "$workspace" --game "$game" \
    --assets-dir "$workspace/Assets" --nodtool "$nodtool"
opuc_resolve_project "$workspace" "$workspace/Assets" >/dev/null || fail 'the extracted game cannot be built'

icon_dir="$workspace/build/icon"
"$setup_cli" make-icon --data-root "$workspace/Assets/DATA" --out "$icon_dir" >/dev/null ||
    fail 'the game icon could not be created from the disc'

"$workspace/launcher/local-build-macos.command" --workspace "$workspace" --output-dir "$products" \
    --icon-dir "$icon_dir" --cmake "$cmake_bin" --ninja "$ninja_bin" --translator-bin "$translator"

config="$support_root/Config.toml"
toml_string() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
set_path() {
    local key=$1 value=$2 encoded line temporary
    encoded=$(toml_string "$value"); line="$key = \"$encoded\""; temporary="$config.tmp"
    touch "$config"
    if grep -q '^[[:space:]]*\[paths\][[:space:]]*$' "$config"; then
        awk -v key="$key" -v line="$line" '
            /^[[:space:]]*\[paths\][[:space:]]*$/ { print; print line; inside = 1; next }
            inside && /^[[:space:]]*\[/ { inside = 0 }
            inside && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" { next }
            { print }
        ' "$config" > "$temporary"
    else
        { cat "$config"; printf '\n[paths]\n%s\n' "$line"; } > "$temporary"
    fi
    mv "$temporary" "$config"
}
set_path dvd_root "$workspace/Assets/DATA"

destination="$HOME/Applications"
if [[ "$install_location" == applications ]]; then destination=/Applications; fi
app="$app_name.app"
[[ -d "$products/$app" ]] || fail "the build did not produce $products/$app"
if [[ "$destination" == /Applications ]]; then
    /usr/bin/osascript - "$products/$app" "/Applications/$app" <<'APPLESCRIPT'
on run argv
    set sourcePath to quoted form of (item 1 of argv)
    set installedPath to quoted form of (item 2 of argv)
    do shell script "mkdir -p /Applications && rm -rf " & installedPath & " && /usr/bin/ditto " & sourcePath & " " & installedPath with administrator privileges
end run
APPLESCRIPT
else
    mkdir -p "$destination"; rm -rf "${destination:?}/$app"; /usr/bin/ditto "$products/$app" "$destination/$app"
fi
notice "Installation complete. \"$app_name\" is in $destination."
