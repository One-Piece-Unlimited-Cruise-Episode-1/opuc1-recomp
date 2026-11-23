#!/usr/bin/env bash
# Extract a user-owned One Piece Unlimited Cruise Episode 1 disc image (PAL, ROUPAF) for a local build.
# This script deliberately contains no game data and is intended for the macOS setup application and
# for maintainers testing that setup path.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../build-cache.sh
source "$script_dir/../build-cache.sh"

fail() { printf 'extract-disc.command: error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | awk '{ print $1 }'; }

usage() {
    cat <<'EOF'
Usage: extract-disc.command --game IMAGE --assets-dir DIR --nodtool PATH [--workspace DIR]

Extracts a user-owned One Piece Unlimited Cruise Episode 1 image into DIR. The image must be
the release a projects/*/recomp.yml in the workspace (default: this script's repository) pins
(ROUPAF). The final layout is DIR/main.dol and DIR/DATA. Existing data is left untouched
unless the complete new extraction passes its checks.
EOF
}

game=""
assets_dir=""
nodtool=""
workspace=$(cd "$script_dir/../.." && pwd)
while (($#)); do
    case "$1" in
        --workspace) workspace=${2:-}; shift 2 ;;
        --game) game=${2:-}; shift 2 ;;
        --assets-dir) assets_dir=${2:-}; shift 2 ;;
        --nodtool) nodtool=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ -f "$game" ]] || fail "disc image does not exist: $game"
[[ -n "$assets_dir" ]] || fail "--assets-dir is required"
[[ -x "$nodtool" ]] || fail "nodtool is not executable: $nodtool"

assets_dir=$(mkdir -p "$assets_dir" && cd "$assets_dir" && pwd)

# The disc's game id selects the release, so an existing extraction is only reused for the same one.
printf 'OPUCBUILD:STEP:validate-disc Checking the selected disc image\n'
disc_info=$("$nodtool" info "$game") || fail 'nodtool could not read the disc image'
game_id=$(awk '/^Game ID: / { print $3; exit }' <<<"$disc_info")
[[ -n "$game_id" ]] || fail 'nodtool did not report the disc game ID'
project=$(opuc_project_for_game "$workspace" "$game_id") ||
    fail "this disc is $game_id; only the PAL release of One Piece Unlimited Cruise Episode 1 (ROUPAF) is supported"
expected_dol_sha256=$(opuc_project_pin "$project" dol)
[[ -n "$expected_dol_sha256" ]] || fail "$project does not pin the clean $game_id main.dol digest"

# Files every later step needs, relative to the DATA root.
required_files=(sys/main.dol sys/boot.bin sys/fst.bin files/memcard/banner_ep1.en.tpl)

# Sorted "<size> <path>" of every extracted file; recorded at extraction, compared on reuse.
data_manifest() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 stat -f '%z %N'); }

if [[ -d "$assets_dir/DATA/files" && -f "$assets_dir/.data-manifest" && -f "$assets_dir/main.dol" ]] && \
   [[ $(sha256 "$assets_dir/main.dol") == "$expected_dol_sha256" ]] && \
   [[ "$(data_manifest "$assets_dir/DATA")" == "$(cat "$assets_dir/.data-manifest")" ]]; then
    printf 'OPUCBUILD:STEP:disc-ready Reusing the verified extracted game files\n'
    exit 0
fi
scratch=$(mktemp -d "${TMPDIR:-/tmp}/opuc1-recomp-disc.XXXXXX")
cleanup() { rm -rf "$scratch"; }
trap cleanup EXIT

printf 'OPUCBUILD:STEP:extract-disc Extracting the user-owned disc image\n'
"$nodtool" extract "$game" "$scratch/extracted"

dol=$(find "$scratch/extracted" -type f -path '*/sys/main.dol' -print -quit)
[[ -n "$dol" ]] || fail 'nodtool extraction did not contain sys/main.dol'
data_root=$(dirname "$(dirname "$dol")")
for relative in "${required_files[@]}"; do
    [[ -f "$data_root/$relative" ]] || fail "the extracted disc is missing $relative"
done
dol_sha=$(sha256 "$dol")
[[ $dol_sha == "$expected_dol_sha256" ]] || fail "main.dol does not match the clean $game_id image (expected $expected_dol_sha256, got $dol_sha)"
boot_id=$(head -c 6 "$data_root/sys/boot.bin")
[[ "$boot_id" == "$game_id" ]] || fail "sys/boot.bin names $boot_id, not $game_id"

# Stage beside the destination so the final replacement stays on one volume.
stage="$assets_dir/.extract-stage-$$"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$data_root" "$stage/DATA"
ditto "$dol" "$stage/main.dol"
data_manifest "$stage/DATA" > "$stage/.data-manifest"

backup="$assets_dir/.previous-extraction-$(date +%Y%m%d-%H%M%S)"
if [[ -e "$assets_dir/DATA" || -e "$assets_dir/.data-manifest" || -e "$assets_dir/main.dol" ]]; then
    mkdir -p "$backup"
    for item in DATA .data-manifest main.dol; do
        if [[ -e "$assets_dir/$item" ]]; then mv "$assets_dir/$item" "$backup/$item"; fi
    done
fi
mv "$stage/DATA" "$assets_dir/DATA"
mv "$stage/.data-manifest" "$assets_dir/.data-manifest"
mv "$stage/main.dol" "$assets_dir/main.dol"
rmdir "$stage"
rm -rf "$backup"

printf 'OPUCBUILD:STEP:disc-ready Verified and extracted the clean %s game files\n' "$game_id"
