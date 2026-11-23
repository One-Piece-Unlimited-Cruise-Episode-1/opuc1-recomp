#!/bin/bash
# Header of opuc1-recomp-setup-macos.run, the self-extracting command-line setup (the macOS counterpart
# of the Linux AppImage). build-setup-pkg.command fills in the version and appends the Setup.app
# Resources as a gzipped tar after the marker line. The bundle is unpacked once per version into
# Application Support, then its opuc1-recomp-setup entry point runs from there.
set -euo pipefail
version='@VERSION@'
if [[ $# -eq 1 && $1 == --version ]]; then printf '%s\n' "$version"; exit 0; fi

setup_root="$HOME/Library/Application Support/One Piece Unlimited Recompilation Episode 1/Setup"
bundle="$setup_root/$version"
if [[ ! -x "$bundle/opuc1-recomp-setup" ]]; then
    mkdir -p "$setup_root"
    staging=$(mktemp -d "$setup_root/.unpack.XXXXXX")
    trap 'rm -rf "$staging"' EXIT
    payload_line=$(awk '/^__OPUC1_RECOMP_PAYLOAD__$/ { print NR + 1; exit }' "$0")
    tail -n +"$payload_line" "$0" | /usr/bin/tar -xzf - -C "$staging" \
        || { printf 'opuc1-recomp-setup: could not unpack %s; the download may be incomplete\n' "$0" >&2; exit 1; }
    # Not safe against two first runs racing; run one setup operation at a time.
    rm -rf "$bundle"
    mv "$staging" "$bundle"
    trap - EXIT
    # Older versions are never run again once this one is unpacked.
    find "$setup_root" -mindepth 1 -maxdepth 1 ! -name "$version" ! -name '.unpack.*' -exec rm -rf {} +
fi
exec "$bundle/opuc1-recomp-setup" "$@"
__OPUC1_RECOMP_PAYLOAD__
