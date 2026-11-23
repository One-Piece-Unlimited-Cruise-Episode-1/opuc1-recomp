#!/usr/bin/env bash
# Exercise a real dyld dependency chain before and after app packaging.
set -euo pipefail

[[ $(uname -s) == Darwin ]] || { printf 'This test requires macOS.\n' >&2; exit 1; }
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
temp_root=$(cd "${TMPDIR:-/tmp}" && pwd)
test_root=$(mktemp -d "$temp_root/opuc1-recomp-publish-test.XXXXXX")
[[ "$test_root" == "$temp_root"/opuc1-recomp-publish-test.* ]] || exit 1
trap 'rm -rf "$test_root"' EXIT

app_name="One Piece Unlimited Recompilation Episode 1"
build_dir="$test_root/build with spaces"
output_dir="$test_root/output with spaces"
mkdir -p "$build_dir/A/b" "$build_dir/A/c" "$output_dir"
cat > "$test_root/c.c" <<'EOF'
int value_c(void) { return 7; }
EOF
cat > "$test_root/b.c" <<'EOF'
extern int value_c(void);
int value_b(void) { return 2 * value_c(); }
EOF
cat > "$test_root/a.c" <<'EOF'
extern int value_b(void);
int value_a(void) { return 1 + value_b(); }
EOF
cat > "$test_root/main.c" <<'EOF'
#include <stdio.h>
extern int value_a(void);
int main(void) { printf("%d\n", value_a()); return 0; }
EOF

clang -dynamiclib "$test_root/c.c" -o "$build_dir/A/c/libC.dylib" \
    -Wl,-headerpad_max_install_names -Wl,-install_name,@rpath/libC.dylib
clang -dynamiclib "$test_root/b.c" -o "$build_dir/A/b/libB.dylib" \
    -L "$build_dir/A/c" -lC \
    -Wl,-headerpad_max_install_names -Wl,-install_name,@rpath/libB.dylib
clang -dynamiclib "$test_root/a.c" -o "$build_dir/A/libA.dylib" \
    -L "$build_dir/A/b" -lB \
    -Wl,-headerpad_max_install_names -Wl,-install_name,@rpath/libA.dylib \
    -Wl,-rpath,@loader_path/b -Wl,-rpath,@loader_path/c
clang "$test_root/main.c" -o "$build_dir/opuc1-recomp" \
    -L "$build_dir/A" -lA \
    -Wl,-headerpad_max_install_names -Wl,-rpath,@executable_path/A

# B has no rpaths: its C dependency must inherit A's loader-relative path.
[[ $("$build_dir/opuc1-recomp") == 15 ]]
for asset in dsp_coef.bin; do
    : > "$build_dir/$asset"
done
bash "$script_dir/publish-app.command" --build-dir "$build_dir" \
    --output-dir "$output_dir" --architecture "$(uname -m)"
for name in libA.dylib libB.dylib libC.dylib; do
    [[ -f "$output_dir/$app_name.app/Contents/Frameworks/$name" ]]
done

# Remove access to the original paths and relocate the app before executing.
mv "$build_dir" "$test_root/hidden build"
mkdir "$test_root/relocated app"
mv "$output_dir/$app_name.app" "$test_root/relocated app/$app_name.app"
[[ $("$test_root/relocated app/$app_name.app/Contents/MacOS/opuc1-recomp") == 15 ]]
printf 'publish-app dependency-chain test passed\n'
