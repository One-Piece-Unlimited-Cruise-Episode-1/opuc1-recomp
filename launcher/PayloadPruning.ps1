# What the Windows release payload leaves out of the portable toolchain and the pinned dependency
# trees. Dot-sourced by Build-Installer.ps1, which applies it to the staged payload only; the
# maintainer copies under launcher/artifacts stay complete.
#
# The local build (LocalBuild.ps1) compiles and links for x86_64-w64-mingw32 only: the x86_64 clang,
# clang++ and windres wrappers, clang-22, ld.lld, llvm-ar/ranlib, clang-scan-deps, and compiler-rt's
# x86_64 builtins. Everything else is dropped: the other targets' sysroots and wrappers, Linux and
# sanitizer/fuzzer/profile runtimes, editor and analysis tools, Python, debuggers, and CMake's GUI,
# test and packaging tools and documentation.

function Remove-OpucPathIfPresent([string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

function Remove-OpucUnusedToolkitFiles([string]$Toolkit) {
    $llvm = Join-Path $Toolkit 'llvm-mingw'
    $llvmBin = Join-Path $llvm 'bin'
    $resourceDirectories = @(Get-ChildItem -LiteralPath (Join-Path $llvm 'lib\clang') -Directory)
    if ($resourceDirectories.Count -ne 1) {
        throw "Expected one clang resource directory in llvm-mingw, found $($resourceDirectories.Count)."
    }
    $compilerRt = Join-Path $resourceDirectories[0].FullName 'lib'

    foreach ($relative in @(
        'python', 'busybox',
        'i686-w64-mingw32', 'armv7-w64-mingw32', 'aarch64-w64-mingw32', 'arm64ec-w64-mingw32',
        'lib\libscanbuild', 'lib\libear',
        'x86_64-w64-mingw32\bin\libclang_rt.asan_dynamic-x86_64.dll', 'x86_64-w64-mingw32\bin\libomp.dll'
    )) { Remove-OpucPathIfPresent (Join-Path $llvm $relative) }
    Remove-OpucPathIfPresent (Join-Path $compilerRt 'linux')
    # A MinGW link that is not sanitized or instrumented pulls in the builtins archive only.
    Get-ChildItem -LiteralPath (Join-Path $compilerRt 'windows') -File |
        Where-Object { $_.Name -ne 'libclang_rt.builtins-x86_64.a' } |
        Remove-Item -Force

    $unusedTools = @(
        'clangd.exe', 'clang-tidy.exe', 'run-clang-tidy', 'clang-format.exe', 'git-clang-format',
        'analyze-build', 'intercept-build', 'scan-build-py', 'widl.exe', 'libpython3.dll',
        'libclang_rt.asan_dynamic-x86_64.dll', 'libomp.dll', 'llvm-cov.exe', 'llvm-profdata.exe', 'llvm-pdbutil.exe'
    )
    Get-ChildItem -LiteralPath $llvmBin -File | Where-Object {
        $_.Name -match '^(i686|armv7|aarch64|arm64ec)-' -or $_.Name -like 'x86_64-w64-mingw32uwp-*' -or
        $_.Name -like 'lldb*' -or $_.Name -like 'liblldb*' -or $_.Name -like 'libpython3.*.dll' -or
        $unusedTools -contains $_.Name
    } | Remove-Item -Force

    foreach ($relative in @('bin\cmake-gui.exe', 'bin\ctest.exe', 'bin\cpack.exe', 'bin\cmcldeps.exe', 'doc', 'man')) {
        Remove-OpucPathIfPresent (Join-Path (Join-Path $Toolkit 'CMake') $relative)
    }
}

function Remove-OpucUnusedDependencyFiles([string]$Dependencies) {
    # SDL's test programs and examples (SDL_TESTS / SDL_EXAMPLES are off when aurora builds SDL as a
    # subproject), Tracy's profiler GUI and stand-alone tools (aurora builds only TracyClient from
    # public/), and FreeType's documentation. None of them is read by any configure or build.
    foreach ($relative in @(
        'SDL\test', 'SDL\examples',
        'tracy\profiler', 'tracy\server', 'tracy\update', 'tracy\capture', 'tracy\csvexport', 'tracy\import',
        'tracy\merge', 'tracy\monitor', 'tracy\python', 'tracy\manual', 'tracy\examples', 'tracy\tests',
        'freetype\docs'
    )) { Remove-OpucPathIfPresent (Join-Path $Dependencies $relative) }
}
