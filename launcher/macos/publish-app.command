#!/usr/bin/env bash
# Turn the locally compiled macOS game into a self-contained .app bundle.
set -euo pipefail

fail() { printf 'publish-app.command: error: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: publish-app.command --build-dir DIR --output-dir DIR [options]

Copies the locally built game (opuc1-recomp) and its runtime assets into
OUTPUT-DIR/One Piece Unlimited Recompilation Episode 1.app. It bundles non-system
dylibs, rewrites their install names, and ad-hoc signs the result. This is
suitable for local use; a release must replace ad-hoc signing with Developer ID
signing and notarization.

  --architecture {arm64|x86_64}       Required architecture of the compiled game (default: host)
  --minimum-system-version VERSION    App bundle minimum macOS version (default: 12.0)
  --version VERSION                   CFBundleShortVersionString (default: 0.1.0)
  --icon-png FILE                     256 px PNG turned into the app icon (opuc1_recomp.png from the setup);
                                      without it the bundle has no icon
EOF
}

exe_name=opuc1-recomp
app_name="One Piece Unlimited Recompilation Episode 1"
display_name="One Piece: Unlimited Recompilation Episode 1"
build_dir=""; output_dir=""; architecture=$(uname -m); minimum_system_version=12.0; version=0.1.0; icon_png=""
while (($#)); do
    case "$1" in
        --build-dir) build_dir=${2:-}; shift 2 ;;
        --output-dir) output_dir=${2:-}; shift 2 ;;
        --architecture) architecture=${2:-}; shift 2 ;;
        --minimum-system-version) minimum_system_version=${2:-}; shift 2 ;;
        --version) version=${2:-}; shift 2 ;;
        --icon-png) icon_png=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[[ -n "$output_dir" ]] || fail '--output-dir is required'
[[ "$architecture" == arm64 || "$architecture" == x86_64 ]] || fail '--architecture must be arm64 or x86_64'
[[ "$minimum_system_version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || fail '--minimum-system-version must contain two or three period-separated integers'
short_version=$(printf '%s' "$version" | sed -nE 's/^([0-9]+(\.[0-9]+){0,2}).*/\1/p')
[[ -n "$short_version" ]] || short_version=0.1.0
for tool in codesign ditto install_name_tool lipo otool; do command -v "$tool" >/dev/null || fail "required macOS tool is unavailable: $tool"; done
[[ -x "$build_dir/$exe_name" ]] || fail "missing compiled game: $build_dir/$exe_name"
lipo "$build_dir/$exe_name" -verify_arch "$architecture" || fail "compiled game is not $architecture: $build_dir/$exe_name"
# The only runtime asset the game reads beside its executable.
runtime_assets=(dsp_coef.bin)
for asset in "${runtime_assets[@]}"; do [[ -e "$build_dir/$asset" ]] || fail "missing runtime asset: $build_dir/$asset"; done
if [[ -n "$icon_png" ]]; then
    [[ -f "$icon_png" ]] || fail "missing icon PNG: $icon_png"
    for tool in sips iconutil; do command -v "$tool" >/dev/null || fail "required macOS tool is unavailable: $tool"; done
fi

app="$output_dir/$app_name.app"
macos="$app/Contents/MacOS"
frameworks="$app/Contents/Frameworks"
resources="$app/Contents/Resources"
rm -rf "$app"
mkdir -p "$macos" "$frameworks" "$resources"
icon_entry=""
if [[ -n "$icon_png" ]]; then
    # The icon is game art generated from the user's own disc, so it is built here and never shipped.
    iconset=$(mktemp -d "${TMPDIR:-/tmp}/opuc1-recomp-icon.XXXXXX")/opuc1_recomp.iconset
    mkdir -p "$iconset"
    for size in 16 32 128 256; do
        sips -z "$size" "$size" "$icon_png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        sips -z "$double" "$double" "$icon_png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$resources/opuc1_recomp.icns"
    rm -rf "$(dirname "$iconset")"
    icon_entry="  <key>CFBundleIconFile</key><string>opuc1_recomp.icns</string>"
fi
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$exe_name</string>
  <key>CFBundleIdentifier</key><string>io.github.opuc1-recomp</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
$icon_entry
  <key>CFBundleName</key><string>$app_name</string>
  <key>CFBundleDisplayName</key><string>$display_name</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$short_version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>$minimum_system_version</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
ditto "$build_dir/$exe_name" "$macos/$exe_name"
for asset in "${runtime_assets[@]}"; do
    ditto "$build_dir/$asset" "$resources/$asset"
    ln -s "../Resources/$asset" "$macos/$asset"
done

# Expand each image's rpaths before inheriting them, so @loader_path stays
# relative to the image that declared it rather than a descendant library.
expanded_rpaths() {
    local target=$1 rpath
    while IFS= read -r rpath; do
        case "$rpath" in
            @loader_path/*) rpath="$(dirname "$target")/${rpath#@loader_path/}" ;;
            @loader_path) rpath="$(dirname "$target")" ;;
            @executable_path/*) rpath="$build_dir/${rpath#@executable_path/}" ;;
            @executable_path) rpath="$build_dir" ;;
        esac
        printf '%s\n' "$rpath"
    done < <(otool -l "$target" | awk '
/LC_RPATH/ { rpath = 1; next }
rpath && /^[[:space:]]*path / {
    sub(/^[[:space:]]*path[[:space:]]+/, "");
    sub(/[[:space:]]+\(offset[[:space:]]+[0-9]+\)$/, "");
    print;
    rpath = 0;
}')
}

# Resolve a non-system dependency using the current image's rpaths followed
# by the inherited loader stack, matching dyld's dependency-chain search.
dependency_path() {
    local current=$1 dependency=$2 search_rpaths=$3 rpath candidate
    case "$dependency" in
        /System/Library/*|/usr/lib/*)
            return 1
            ;;
        /*)
            [[ -f "$dependency" ]] && { printf '%s\n' "$dependency"; return 0; }
            ;;
        @loader_path/*)
            candidate="$(dirname "$current")/${dependency#@loader_path/}"
            [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
            ;;
        @executable_path/*)
            candidate="$build_dir/${dependency#@executable_path/}"
            [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
            ;;
        @rpath/*|*.dylib)
            local subpath
            if [[ "$dependency" == @rpath/* ]]; then
                subpath="${dependency#@rpath/}"
            else
                subpath="$dependency"
            fi
            while IFS= read -r rpath; do
                [[ -n "$rpath" ]] || continue
                candidate="$rpath/$subpath"
                [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
            done <<< "$search_rpaths"
            ;;
    esac
    return 1
}

# Build a closure of non-system dylibs. System libraries remain system
# references, while every resolved dependency is copied beside the executable.
queue=("$build_dir/$exe_name")
queue_rpaths=("")
while ((${#queue[@]})); do
    current=${queue[0]}
    inherited_rpaths=${queue_rpaths[0]}
    queue=("${queue[@]:1}")
    queue_rpaths=("${queue_rpaths[@]:1}")
    current_rpaths=$(expanded_rpaths "$current")
    if [[ -n "$inherited_rpaths" ]]; then
        current_rpaths="${current_rpaths}${current_rpaths:+$'\n'}$inherited_rpaths"
    fi
    self_id=$(otool -D "$current" 2>/dev/null | tail -n +2 | sed -nE 's/^[[:space:]]*(.*)[[:space:]]*$/\1/p' || true)
    while IFS= read -r dependency; do
        [[ -z "$dependency" ]] && continue
        [[ -n "$self_id" && "$dependency" == "$self_id" ]] && continue
        case "$dependency" in
            /System/Library/*|/usr/lib/*) continue ;;
        esac
        dep_path=$(dependency_path "$current" "$dependency" "$current_rpaths") || fail "unresolved non-system dependency: '$dependency' needed by '$current'"
        name=$(basename "$dep_path")
        if [[ ! -f "$frameworks/$name" ]]; then
            ditto "$dep_path" "$frameworks/$name"
            install_name_tool -id "@rpath/$name" "$frameworks/$name"
            queue+=("$dep_path")
            queue_rpaths+=("$current_rpaths")
        fi
    done < <(otool -L "$current" | tail -n +2 | sed -nE 's/^[[:space:]]*(.*)[[:space:]]+\(compatibility version .*/\1/p')
done
while IFS= read -r binary; do
    while IFS= read -r old; do
        name=$(basename "$old")
        [[ -f "$frameworks/$name" ]] || continue
        if [[ "$binary" == "$macos/$exe_name" ]]; then
            install_name_tool -change "$old" "@executable_path/../Frameworks/$name" "$binary"
        else
            install_name_tool -change "$old" "@loader_path/$name" "$binary"
        fi
    done < <(otool -L "$binary" | tail -n +2 | sed -nE 's/^[[:space:]]*(.*)[[:space:]]+\(compatibility version .*/\1/p')
done < <(find "$frameworks" -type f -print; printf '%s\n' "$macos/$exe_name")

find "$frameworks" -type f -exec codesign --force --sign - {} +
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
printf 'OPUCBUILD:APP=%s\n' "$app"
