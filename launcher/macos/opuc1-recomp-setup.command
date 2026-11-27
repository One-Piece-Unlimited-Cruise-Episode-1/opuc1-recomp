#!/usr/bin/env bash
# Command-line entry point bundled in opuc1-recomp-setup-macos.run as opuc1-recomp-setup. The macOS
# counterpart of the AppImage's AppRun: it runs the setup CLI with the bundled tools, so nothing has
# to be on PATH. An app launched from Finder or the Dock only gets /usr/bin:/bin:/usr/sbin:/sbin.
set -euo pipefail

resources=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
arch=$(uname -m)
case "$arch" in arm64|x86_64) ;; *) printf 'opuc1-recomp-setup: unsupported macOS architecture: %s\n' "$arch" >&2; exit 1 ;; esac
tools="$resources/tools/$arch"
# Shared with setup.command, so both entry points reuse one extraction and translation cache.
workspace="$HOME/Library/Application Support/One Piece Unlimited Recompilation Episode 1/BuildWorkspace"

# Only install (and the guided no-argument start, which may install) builds, so only they need the
# compiler and a writable workspace; --version and launch stay fast. Output goes to stderr because
# --progress-json reserves stdout for its event stream.
command_word=""
for argument in "$@"; do
    case "$argument" in --*) ;; *) command_word=$argument; break ;; esac
done
if [[ "$command_word" == install || -z "$command_word" ]] && [[ " $* " != *" --version "* && " $* " != *" --help "* ]]; then
    if ! /usr/bin/xcode-select -p >/dev/null 2>&1; then
        /usr/bin/xcode-select --install >/dev/null 2>&1 || true
        printf "opuc1-recomp-setup: Xcode Command Line Tools are required to compile the game. Finish Apple's installer, then try again.\n" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$workspace")"
    "$resources/sync-workspace.command" "$resources/workspace" "$workspace" >&2
fi

exec "$tools/opuc1-recomp-setup" --workspace "$workspace" \
    --translator-bin "$tools/Translator.Cli" --disc-tool-bin "$tools/nodtool" \
    --cmake "$resources/tools/cmake/bin/cmake" --ninja "$tools/ninja" "$@"
