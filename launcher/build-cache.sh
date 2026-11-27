# Shared incremental-build and release-selection logic for the Unix setups (Linux AppImage and
# macOS), sourced by local-build.sh, local-build-macos.command, macos/extract-disc.command, the
# AppImage's AppRun and macos/sync-workspace.command.
# Mirrors the Windows installer's translation/toolchain identities and timestamp preservation.
# Must stay compatible with macOS's bash 3.2.

if command -v sha256sum >/dev/null 2>&1; then
    opuc_sha256_cmd=(sha256sum)
else
    opuc_sha256_cmd=(shasum -a 256)
fi

opuc_sha256() {
    "${opuc_sha256_cmd[@]}" "$@" | awk '{print $1}'
}

# Package provenance uses sorted relative paths followed by their file digests.
opuc_fingerprint_tree() (
    set -o pipefail
    cd -- "$1" || exit 1
    shift
    local args=(. -name .git -prune -o) dir record digest
    for dir in "$@"; do args+=(-path "./$dir" -prune -o); done
    find "${args[@]}" -type f -print0 | LC_ALL=C sort -z |
        if [[ ${opuc_sha256_cmd[0]} == sha256sum ]]; then
            xargs -0 -r sha256sum -z -- | while IFS= read -r -d '' record; do
                # sha256sum -z emits the digest, two spaces, then the ./ path.
                printf '%s %s\n' "${record:68}" "${record:0:64}"
            done
        else
            while IFS= read -r -d '' record; do
                digest=$(opuc_sha256 < "$record") || exit 1
                printf '%s %s\n' "${record#./}" "$digest"
            done
        fi | opuc_sha256
)

# Prints "<sha>  <relative path>" for every file under each directory, relative to $1.
opuc_digest_tree() {
    local root=$1; shift
    (
        cd "$root" || exit 1
        local dir
        for dir in "$@"; do
            if [[ -d "$dir" ]]; then
                printf 'tree %s\n' "$dir"
                find "$dir" -type f -print0 | LC_ALL=C sort -z | xargs -0 "${opuc_sha256_cmd[@]}" /dev/null
            else
                printf 'absent %s\n' "$dir"
            fi
        done
    )
}

# Content-only digest of a file or directory, independent of where it lives (the AppImage mount
# path changes every launch).
opuc_digest_path() {
    if [[ -d "$1" ]]; then
        opuc_digest_tree "$1" . | opuc_sha256
    elif [[ -f "$1" ]]; then
        opuc_sha256 < "$1"
    else
        printf 'absent\n'
    fi
}

# Same digest as the translator's RuntimeNativeIndexFile.ComputeSourceDigest. $1 = runtime/src.
opuc_native_source_digest() {
    (
        cd "$1" || exit 1
        find . -type f \( -iname '*.cpp' -o -iname '*.h' -o -iname '*.hpp' \) -print0 | LC_ALL=C sort -z |
            xargs -0 "${opuc_sha256_cmd[@]}" /dev/null
    ) | grep -v '  /dev/null$' | sed 's|  \./|  |' | opuc_sha256
}

# The native registrations the translator reads from runtime/src. A current native-index.json
# stands in for the sources (minus its source digest), so runtime edits that leave the
# registrations alone keep the translation; a missing or stale index falls back to the sources.
# $1 = workspace.
opuc_native_index_identity() {
    local index="$1/runtime/native-index.json" recorded
    recorded=$(opuc_read_identity "$index" sourceDigest)
    if [[ -n "$recorded" && -d "$1/runtime/src" && "$recorded" == "$(opuc_native_source_digest "$1/runtime/src")" ]]; then
        printf 'native-index %s\n' "$(grep -v '^[[:space:]]*"sourceDigest"[[:space:]]*:' "$index" | opuc_sha256)"
    else
        opuc_digest_tree "$1" runtime/src
    fi
}

# Everything the translation depends on: the game's main.dol, the project, native registrations,
# the translator itself and the build script that drives it.
# $1 = workspace, $2 = translator file or directory, $3 = build script.
opuc_translation_identity() {
    local workspace=$1 translator=$2 script=$3
    {
        printf 'opuc-translation-v1\n'
        printf 'dol %s\n' "$(opuc_digest_path "$workspace/Assets/main.dol")"
        printf 'translator %s\n' "$(opuc_digest_path "$translator")"
        printf 'script %s\n' "$(opuc_digest_path "$script")"
        opuc_digest_tree "$workspace" projects
        opuc_native_index_identity "$workspace"
    } | opuc_sha256
}

# Inputs that make compiled objects unsafe to reuse: compiler/CMake/Ninja versions and the
# from-source dependency pins. Source changes are excluded; Ninja rebuilds exactly what they touch.
# $1 = workspace, remaining = tools (C compiler, C++ compiler, cmake, ninja).
opuc_toolchain_identity() {
    local workspace=$1; shift
    local tool
    {
        printf 'opuc-toolchain-v1\n'
        for tool in "$@"; do
            # First line only: clang appends its InstalledDir, which moves with the AppImage mount.
            printf '%s\n' "$("$tool" --version 2>/dev/null | head -n 1)"
        done
        opuc_digest_tree "$workspace" third_party/aurora/extern third_party/aurora/cmake
        printf 'aurora %s\n' "$(opuc_digest_path "$workspace/third_party/aurora/CMakeLists.txt")"
    } | opuc_sha256
}

# Prints one fact a projects/*/recomp.yml pins: game_id or dol (the main.dol SHA-256).
# Literal line matching against the fixed shape, like NativeBuildFlags.ps1's Get-OpucProjectPins.
# $1 = recomp.yml, $2 = fact.
opuc_project_pin() {
    awk -v want="$2" '
        { sub(/\r$/, ""); sub(/#.*/, "") }
        /^[A-Za-z0-9_]+:/ { section = $1; sub(/:.*/, "", section); input = ""; next }
        section == "inputs" && /^  [A-Za-z0-9_]+:[[:space:]]*$/ { input = $1; sub(/:$/, "", input); next }
        section == "project" && $1 == (want ":") { print $2; exit }
        section == "inputs" && input == want && $1 == "sha256:" { print tolower($2); exit }
    ' "$1"
}

# Prints the first translation entry point of a recomp.yml (translation.entry_points[0]).
opuc_project_entry_point() {
    awk '
        { sub(/\r$/, "") }
        /^translation:/ { inside = 1; next }
        /^[A-Za-z0-9_]+:/ { inside = 0 }
        inside && /^[[:space:]]*-[[:space:]]*0[xX][0-9a-fA-F]+/ {
            sub(/^[[:space:]]*-[[:space:]]*/, ""); sub(/[[:space:]#].*$/, ""); print; exit
        }
    ' "$1"
}

# Prints the recomp.yml of the supported release whose game id is $2. $1 = workspace.
opuc_project_for_game() {
    local project
    for project in "$1"/projects/*/recomp.yml; do
        if [[ -f "$project" && "$(opuc_project_pin "$project" game_id)" == "$2" ]]; then
            printf '%s\n' "$project"
            return 0
        fi
    done
    return 1
}

# Prints the recomp.yml pinned to $2/main.dol after checking any extracted disc header against
# it, like NativeBuildFlags.ps1's Resolve-OpucProjectForAssets.
# $1 = workspace, $2 = assets directory.
opuc_resolve_project() {
    local project dol_sha game_id disc_id
    dol_sha=$(opuc_sha256 "$2/main.dol")
    for project in "$1"/projects/*/recomp.yml; do
        [[ -f "$project" && "$(opuc_project_pin "$project" dol)" == "$dol_sha" ]] || continue
        game_id=$(opuc_project_pin "$project" game_id)
        if [[ -f "$2/DATA/sys/boot.bin" ]]; then
            disc_id=$(head -c 6 "$2/DATA/sys/boot.bin")
            if [[ "$disc_id" != "$game_id" ]]; then
                printf 'The extracted game data is %s but main.dol is %s; extract the game again.\n' "$disc_id" "$game_id" >&2
                return 1
            fi
        fi
        printf '%s\n' "$project"
        return 0
    done
    printf 'main.dol (sha256 %s) is not the clean main.dol of a supported release (ROUPAF).\n' "$dol_sha" >&2
    return 1
}

# Writes the project the build actually translates: a copy of $2 (projects/<name>/recomp.yml)
# whose inputs.dol.path names the workspace's Assets/main.dol instead of the developer checkout's
# data/extract tree. It lives two directories below the workspace, like the original, so
# workspace_root (../..) still resolves to the workspace. $1 = workspace, $2 = project.
# Prints the written path.
opuc_write_build_project() {
    local workspace=$1 project=$2 destination
    destination="$workspace/generated/launcher-project/recomp.yml"
    mkdir -p "$(dirname "$destination")"
    awk '
        { line = $0; sub(/\r$/, "", line) }
        line ~ /^[A-Za-z0-9_]+:/ { section = line; sub(/:.*/, "", section); input = "" }
        section == "inputs" && line ~ /^  [A-Za-z0-9_]+:[[:space:]]*$/ { input = line; gsub(/[[:space:]:]/, "", input) }
        section == "inputs" && input == "dol" && line ~ /^[[:space:]]+path:/ {
            print "    path: Assets/main.dol"; rewritten = 1; next
        }
        { print line }
        END { if (!rewritten) exit 3 }
    ' "$project" > "$destination.tmp" || { rm -f "$destination.tmp"; printf '%s has no inputs.dol.path to rewrite\n' "$project" >&2; return 1; }
    grep -q '^workspace_root:[[:space:]]*\.\./\.\.[[:space:]]*$' "$destination.tmp" ||
        { rm -f "$destination.tmp"; printf '%s must use workspace_root: ../..\n' "$project" >&2; return 1; }
    mv "$destination.tmp" "$destination"
    printf '%s\n' "$destination"
}

# $1 = JSON file, $2 = key. Prints the recorded string value, or nothing.
opuc_read_identity() {
    [[ -f "$1" ]] || return 0
    grep -o "\"$2\" *: *\"[^\"]*\"" "$1" 2>/dev/null | sed 's/.*"\([^"]*\)"$/\1/' || true
}

opuc_write_identity() {
    printf '{"SchemaVersion":1,"%s":"%s"}\n' "$2" "$3" > "$1"
}

# Mirrors each named directory from $1 into $2. Unchanged files are left untouched so their
# timestamps (which clang's PCH validation and Ninja compare) survive; changed and new files are
# copied fresh and files no longer shipped are removed.
opuc_sync_tree() {
    local source=$1 destination=$2; shift 2
    local dir relative changed=0 unchanged=0 removed=0
    for dir in "$@"; do
        if [[ ! -d "$source/$dir" ]]; then
            rm -rf "$destination/$dir"
            continue
        fi
        if [[ -e "$destination/$dir" && ! -d "$destination/$dir" ]]; then
            rm -f "$destination/$dir"
        fi
        mkdir -p "$destination/$dir"
        while IFS= read -r -d '' relative; do
            relative=${relative#./}
            if [[ -d "$source/$dir/$relative" ]]; then
                [[ -d "$destination/$dir/$relative" ]] || { rm -f "$destination/$dir/$relative"; mkdir -p "$destination/$dir/$relative"; }
            elif [[ -f "$destination/$dir/$relative" ]] && cmp -s "$source/$dir/$relative" "$destination/$dir/$relative"; then
                unchanged=$((unchanged + 1))
            else
                rm -rf "$destination/$dir/$relative"
                cp "$source/$dir/$relative" "$destination/$dir/$relative"
                changed=$((changed + 1))
            fi
        done < <(cd "$source/$dir" && find . -mindepth 1 -print0)
        while IFS= read -r -d '' relative; do
            relative=${relative#./}
            if [[ ! -e "$source/$dir/$relative" && ( -e "$destination/$dir/$relative" || -L "$destination/$dir/$relative" ) ]]; then
                [[ -d "$destination/$dir/$relative" ]] || removed=$((removed + 1))
                rm -rf "$destination/$dir/$relative"
            fi
        done < <(cd "$destination/$dir" && find . -mindepth 1 -depth -print0)
    done
    printf 'Workspace refreshed: %d changed, %d unchanged, %d removed file(s).\n' "$changed" "$unchanged" "$removed"
}
