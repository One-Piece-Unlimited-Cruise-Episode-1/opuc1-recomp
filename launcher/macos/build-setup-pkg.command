#!/usr/bin/env bash
# Maintainer release builder. It packages setup/source/tooling only -- never a
# translated executable, extracted DATA tree, disc image, SYSCONF, or icon made from game art.
set -euo pipefail

fail() { printf 'build-setup-pkg.command: error: %s\n' "$*" >&2; exit 1; }
# Release payloads must not inherit Finder metadata, resource forks, or a
# downloaded-file quarantine bit from a maintainer's working volume.
copy_clean() { DITTONORSRC=1 ditto --norsrc --noqtn "$@"; }
usage() {
    cat <<'EOF'
Usage: build-setup-pkg.command --nodtool-arm64 PATH --nodtool-x86_64 PATH --translator-arm64 PATH --translator-x86_64 PATH --setup-arm64 PATH --setup-x86_64 PATH --cmake-root DIR --ninja-arm64 PATH --ninja-x86_64 PATH --output PKG [options]

Creates a game-code-free "One Piece Unlimited Recompilation Episode 1 Setup" .pkg. The supplied tools must be
maintainer-verified, redistributable macOS artifacts for both arm64 and
x86_64. The setup package selects native tools for its host while the game is
compiled locally for that host architecture. CMake must be universal2. The
resulting pkg is unsigned unless
--installer-identity is supplied; releases should sign and notarize it with a
Developer ID Installer certificate.

  --workspace DIR             Repository root (default: script's grandparent)
  --version VERSION           Bundle/package version (default: project version, or 0.1.0)
  --installer-identity NAME   Developer ID Installer identity for productbuild
  --run-output FILE           Also write the same bundle as a self-extracting .run for frontends
EOF
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/../.." && pwd); nodtool_arm64=""; nodtool_x86_64=""; translator_arm64=""; translator_x86_64=""; setup_arm64=""; setup_x86_64=""; cmake_root=""; ninja_arm64=""; ninja_x86_64=""; output=""; version=0.1.0; identity=""; run_output=""
version_supplied=false
while (($#)); do
    case "$1" in
        --workspace) workspace=${2:-}; shift 2 ;;
        --nodtool-arm64) nodtool_arm64=${2:-}; shift 2 ;;
        --nodtool-x86_64) nodtool_x86_64=${2:-}; shift 2 ;;
        --translator-arm64) translator_arm64=${2:-}; shift 2 ;;
        --translator-x86_64) translator_x86_64=${2:-}; shift 2 ;;
        --setup-arm64) setup_arm64=${2:-}; shift 2 ;;
        --setup-x86_64) setup_x86_64=${2:-}; shift 2 ;;
        --cmake-root) cmake_root=${2:-}; shift 2 ;;
        --ninja-arm64) ninja_arm64=${2:-}; shift 2 ;;
        --ninja-x86_64) ninja_x86_64=${2:-}; shift 2 ;;
        --output) output=${2:-}; shift 2 ;;
        --version) version=${2:-}; version_supplied=true; shift 2 ;;
        --installer-identity) identity=${2:-}; shift 2 ;;
        --run-output) run_output=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
version=${version#v}
if [[ "$version_supplied" == false ]]; then
    local_csproj="$workspace/launcher/Directory.Build.props"
    if [[ -f "$local_csproj" ]]; then
        detected=$(grep -m1 '<Version>' "$local_csproj" | sed -E 's/.*<Version>([^<]+)<\/Version>.*/\1/' || true)
        if [[ -n "$detected" ]]; then
            version="$detected"
        fi
    fi
fi
[[ "$version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || fail '--version must contain one to three period-separated integers'
IFS=. read -r version_major version_minor version_patch <<< "$version"
short_version="$version_major.${version_minor:-0}.${version_patch:-0}"
for tool in pkgbuild productbuild ditto codesign lipo; do command -v "$tool" >/dev/null || fail "required macOS tool unavailable: $tool"; done
for tool_path in "$nodtool_arm64" "$nodtool_x86_64" "$translator_arm64" "$translator_x86_64" "$setup_arm64" "$setup_x86_64" "$ninja_arm64" "$ninja_x86_64"; do [[ -x "$tool_path" ]] || fail 'each architecture-specific tool must name an executable'; done
[[ -x "$cmake_root/bin/cmake" ]] || fail '--cmake-root must contain bin/cmake'
require_arch() {
    local artifact=$1 arch=$2 label=$3
    lipo "$artifact" -verify_arch "$arch" >/dev/null 2>&1 || fail "$label must contain a $arch slice: $artifact"
}
require_arch "$nodtool_arm64" arm64 '--nodtool-arm64'; require_arch "$nodtool_x86_64" x86_64 '--nodtool-x86_64'
require_arch "$translator_arm64" arm64 '--translator-arm64'; require_arch "$translator_x86_64" x86_64 '--translator-x86_64'
require_arch "$setup_arm64" arm64 '--setup-arm64'; require_arch "$setup_x86_64" x86_64 '--setup-x86_64'
require_arch "$ninja_arm64" arm64 '--ninja-arm64'; require_arch "$ninja_x86_64" x86_64 '--ninja-x86_64'
lipo "$cmake_root/bin/cmake" -verify_arch arm64 x86_64 >/dev/null 2>&1 || fail '--cmake-root/bin/cmake must be universal2'

# Slice checks above prevent accidental cross-architecture packaging. Exercise
# each supplied executable as well: an incorrectly bundled runtime can have a
# valid Mach-O header but still fail before the setup app can use it. Apple
# Silicon maintainers validate Intel tools through Rosetta when it is present.
host_arch=$(uname -m)
run_for_arch() {
    local arch=$1 label=$2
    shift 2
    if [[ "$arch" == "$host_arch" ]]; then
        "$@" >/dev/null || fail "$label did not run successfully"
    elif [[ "$host_arch" == arm64 && "$arch" == x86_64 ]] && /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        /usr/bin/arch -x86_64 "$@" >/dev/null || fail "$label did not run successfully under Rosetta"
    else
        # Intel hosts cannot execute arm64 binaries. The slice remains checked
        # above; CI or an Apple Silicon maintainer must execute that tool set.
        printf 'build-setup-pkg.command: warning: unable to execute %s on %s; architecture slice was verified, but run it in %s CI before release\n' \
            "$label" "$host_arch" "$arch" >&2
    fi
}
for arch in arm64 x86_64; do
    if [[ "$arch" == arm64 ]]; then
        nodtool=$nodtool_arm64; translator=$translator_arm64; setup=$setup_arm64; ninja=$ninja_arm64
    else
        nodtool=$nodtool_x86_64; translator=$translator_x86_64; setup=$setup_x86_64; ninja=$ninja_x86_64
    fi
    run_for_arch "$arch" "--nodtool-$arch" "$nodtool" --version
    run_for_arch "$arch" "--translator-$arch" "$translator" --help
    run_for_arch "$arch" "--setup-$arch" "$setup" --version
    run_for_arch "$arch" "--ninja-$arch" "$ninja" --version
done
run_for_arch "$host_arch" '--cmake-root/bin/cmake' "$cmake_root/bin/cmake" --version
workspace=$(cd "$workspace" && pwd); output=$(cd "$(dirname "$output")" && pwd)/$(basename "$output")
stage=$(mktemp -d "${TMPDIR:-/tmp}/opuc1-recomp-pkg.XXXXXX")
trap 'rm -rf "$stage"' EXIT
app="$stage/root/Applications/One Piece Unlimited Recompilation Episode 1 Setup.app"
resources="$app/Contents/Resources"
mkdir -p "$app/Contents/MacOS" "$resources/tools"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Opuc1RecompSetup</string>
<key>CFBundleIdentifier</key><string>io.github.opuc1-recomp.setup</string>
<key>CFBundleName</key><string>One Piece Unlimited Recompilation Episode 1 Setup</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$short_version</string>
<key>CFBundleVersion</key><string>$version</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
</dict></plist>
EOF
cat > "$app/Contents/MacOS/Opuc1RecompSetup" <<'EOF'
#!/usr/bin/env bash
resources="$(cd "$(dirname "$0")/../Resources" && pwd)"

# Finder launches an app with no terminal attached. The setup work deliberately
# writes human-readable build progress to stdout, so run its .command entry
# point in Terminal instead of discarding that output behind an inert app icon.
exec /usr/bin/osascript - "$resources/setup.command" "$@" <<'APPLESCRIPT'
on run argv
    set commandLine to quoted form of (item 1 of argv)
    if (count of argv) > 1 then
        repeat with argumentIndex from 2 to (count of argv)
            set commandLine to commandLine & " " & quoted form of (item argumentIndex of argv)
        end repeat
    end if
    tell application "Terminal"
        activate
        do script commandLine
    end tell
end run
APPLESCRIPT
EOF
chmod +x "$app/Contents/MacOS/Opuc1RecompSetup"
copy_clean "$script_dir/setup.command" "$resources/setup.command"; chmod +x "$resources/setup.command"
copy_clean "$script_dir/sync-workspace.command" "$resources/sync-workspace.command"; chmod +x "$resources/sync-workspace.command"
# Copy only the build inputs. This deliberately avoids a maintainer's ignored
# output directories, local disc extraction, and developer-only packaging.
# projects/ carries only the supported release (projects/opuc1); the reference maps are not inputs.
mkdir -p "$resources/workspace/projects"
[[ -f "$workspace/third_party/aurora/CMakeLists.txt" ]] ||
    fail "third_party/aurora is empty or missing: $workspace/third_party/aurora"
for source in third_party runtime translator projects/opuc1; do
    [[ -d "$workspace/$source" ]] || fail "required workspace directory is missing: $source"
    copy_clean "$workspace/$source" "$resources/workspace/$source"
done
# Setup bundles contain materialized sources and must work without Git, and no developer build
# directory or already-fetched dependency checkout under aurora's extern/ is a release input.
find "$resources/workspace" -name .git -prune -exec rm -rf {} +
find "$resources/workspace" \( -name bin -o -name obj \) -type d -path '*/translator/*' -prune -exec rm -rf {} +
rm -rf "$resources/workspace/runtime/build"
# The translation identity (build-cache.sh) covers the runtime's native registrations through this
# index, so it is extracted from exactly the packaged runtime sources.
if [[ "$host_arch" == arm64 ]]; then host_translator=$translator_arm64; else host_translator=$translator_x86_64; fi
"$host_translator" emit-native-index --native-source-dir "$resources/workspace/runtime/src" \
    --out "$resources/workspace/runtime/native-index.json" || fail 'the runtime native index could not be extracted'
[[ -f "$resources/workspace/runtime/native-index.json" ]] || fail 'the runtime native index was not written'
mkdir -p "$resources/workspace/launcher/macos"
copy_clean "$workspace/launcher/Directory.Build.props" "$resources/workspace/launcher/Directory.Build.props"
copy_clean "$workspace/launcher/local-build-macos.command" "$resources/workspace/launcher/local-build-macos.command"
copy_clean "$workspace/launcher/build-cache.sh" "$resources/workspace/launcher/build-cache.sh"
copy_clean "$workspace/launcher/macos/extract-disc.command" "$resources/workspace/launcher/macos/extract-disc.command"
copy_clean "$workspace/launcher/macos/publish-app.command" "$resources/workspace/launcher/macos/publish-app.command"
chmod +x "$resources/workspace/launcher/local-build-macos.command" "$resources/workspace/launcher/macos/"*.command
# setup.command uses this marker to update source inputs in an existing user
# workspace without replacing the extracted game files.
printf '%s\n' "$version" > "$resources/workspace/.bundle-version"
mkdir -p "$resources/tools/cmake" "$resources/tools/arm64" "$resources/tools/x86_64"
for arch in arm64 x86_64; do
    if [[ "$arch" == arm64 ]]; then
        nodtool=$nodtool_arm64; translator=$translator_arm64; setup=$setup_arm64; ninja=$ninja_arm64
    else
        nodtool=$nodtool_x86_64; translator=$translator_x86_64; setup=$setup_x86_64; ninja=$ninja_x86_64
    fi
    copy_clean "$nodtool" "$resources/tools/$arch/nodtool"
    copy_clean "$translator" "$resources/tools/$arch/Translator.Cli"
    # setup.command uses the CLI for the SYSCONF seed and the icon; the .run uses it for everything.
    copy_clean "$setup" "$resources/tools/$arch/opuc1-recomp-setup"
    copy_clean "$ninja" "$resources/tools/$arch/ninja"
    chmod +x "$resources/tools/$arch/"*
done
copy_clean "$cmake_root" "$resources/tools/cmake"
copy_clean "$workspace/LICENSE" "$resources/LICENSE"
copy_clean "$workspace/THIRD-PARTY-NOTICES.md" "$resources/THIRD-PARTY-NOTICES.md"
codesign --force --deep --sign - "$app"
if [[ -n "$run_output" ]]; then
    # The same signed Resources plus the opuc1-recomp-setup entry point, behind setup-run-header.sh,
    # so the command-line setup can be downloaded and run without installing the pkg.
    run_output=$(cd "$(dirname "$run_output")" && pwd)/$(basename "$run_output")
    run_extra="$stage/run-extra"
    mkdir -p "$run_extra"
    copy_clean "$script_dir/opuc1-recomp-setup.command" "$run_extra/opuc1-recomp-setup"
    chmod +x "$run_extra/opuc1-recomp-setup"
    sed "s/@VERSION@/$version/" "$script_dir/setup-run-header.sh" > "$stage/setup.run"
    COPYFILE_DISABLE=1 /usr/bin/tar -czf - -C "$resources" . -C "$run_extra" . >> "$stage/setup.run"
    chmod +x "$stage/setup.run"
    ditto "$stage/setup.run" "$run_output"
    printf 'Created self-extracting setup: %s\n' "$run_output"
fi
pkg="$stage/opuc1-recomp-setup-unsigned.pkg"
DITTONORSRC=1 COPYFILE_DISABLE=1 pkgbuild --root "$stage/root" --identifier io.github.opuc1-recomp.setup --version "$version" --install-location / "$pkg"
if [[ -n "$identity" ]]; then productbuild --sign "$identity" --package "$pkg" "$output"; else ditto "$pkg" "$output"; fi
printf 'Created game-code-free package: %s\n' "$output"
