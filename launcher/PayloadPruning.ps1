# What the Windows release payload leaves out of the portable toolchain and the pinned dependency
# trees. Dot-sourced by Build-Installer.ps1, which applies it to the staged payload only; the
# maintainer copies under launcher/artifacts stay complete.
#
# The local build (LocalBuild.ps1) compiles and links for x86_64-w64-mingw32 only: the x86_64 clang,
# clang++ and windres wrappers, clang-22, ld.lld, llvm-ar/ranlib, clang-scan-deps, and compiler-rt's
# x86_64 builtins. Everything else is dropped: the other targets' sysroots and wrappers, Linux and
# sanitizer/fuzzer/profile runtimes, editor and analysis tools, Python, debuggers, and CMake's GUI,
# test and packaging tools and documentation, clang's builtin headers for other targets, and the
# dependency trees' tests, docs and examples. Remove-OpucDebugSections strips DWARF from the MinGW
# runtime libraries, Remove-OpucDependencySources leaves only headers and licenses in the dependency
# trees, and Remove-OpucUntracedToolkitFiles cuts the MinGW headers and libraries to a traced build.

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

    # Clang's builtin headers for other targets and languages. The build compiles C and C++ for
    # x86_64-w64-mingw32 without sanitizers, XRay, profiling, offloading or modules, so the ARM, PowerPC,
    # s390, Hexagon, LoongArch, MIPS, RISC-V, VE, WebAssembly, GPU, CUDA/HIP and OpenCL headers are never
    # included; the x86 and target-neutral headers stay.
    $builtinHeaders = Join-Path $resourceDirectories[0].FullName 'include'
    foreach ($directory in @('sanitizer', 'xray', 'fuzzer', 'profile', 'orc', 'ppc_wrappers', 'cuda_wrappers',
        'openmp_wrappers', 'llvm_libc_wrappers', 'llvm_offload_wrappers', 'zos_wrappers')) {
        Remove-OpucPathIfPresent (Join-Path $builtinHeaders $directory)
    }
    Get-ChildItem -LiteralPath $builtinHeaders -File | Where-Object {
        $_.Name -match '^(arm|riscv|hexagon|hvx_hexagon|sifive|andes|velintrin|lasx|lsx|larch|wasm|nvptx|amdgpu|s390|htm|opencl-c)' -or
        $_.Name -match '^__clang_(cuda|hip)_' -or
        @('armintr.h', 'arm64intr.h', 'altivec.h', 'vecintrin.h', 'msa.h') -contains $_.Name
    } | Remove-Item -Force
}

function Remove-OpucDebugSections([string]$Toolkit) {
    <#
    Drops DWARF debug sections from the MinGW runtime libraries and DLLs the toolkit links into every
    game build (libmingwex.a, the CRT archives, the target libc++.dll and friends); nothing in a release build
    reads them. Only files that carry debug sections are rewritten. llvm-strip cannot rewrite import
    libraries made of short import members ("unsupported object file format"); those keep their bytes.
    #>
    $llvm = Join-Path $Toolkit 'llvm-mingw'
    $strip = Join-Path $llvm 'bin\llvm-strip.exe'
    Assert-File $strip 'llvm-strip'
    # Latin-1 maps every byte to one char, so a string search finds the section name in the raw file.
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $candidates = @(
        Get-ChildItem -LiteralPath (Join-Path $llvm 'x86_64-w64-mingw32\lib') -File -Filter '*.a'
        # The startup objects (crt2.o, dllcrt2.o, ...) are linked into every executable.
        Get-ChildItem -LiteralPath (Join-Path $llvm 'x86_64-w64-mingw32\lib') -File -Filter '*.o'
        # Not llvm-mingw\bin\*.dll: those are the compiler's own runtime (llvm-strip.exe itself has them
        # loaded, so they cannot be rewritten in place) and never end up in a game build.
        Get-ChildItem -LiteralPath (Join-Path $llvm 'x86_64-w64-mingw32\bin') -File -Filter '*.dll'
    )
    $stripped = 0
    $kept = 0
    foreach ($file in $candidates) {
        $text = $latin1.GetString([IO.File]::ReadAllBytes($file.FullName))
        if ($text.IndexOf('.debug_', [StringComparison]::Ordinal) -lt 0) { continue }
        # Windows PowerShell turns redirected native stderr into a terminating error under 'Stop'.
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { $output = & $strip --strip-debug $file.FullName 2>&1 | Out-String }
        finally { $ErrorActionPreference = $previousPreference }
        if ($LASTEXITCODE -eq 0) { $stripped++; continue }
        if ("$output" -match 'unsupported object file format') { $kept++; continue }
        throw "llvm-strip failed on $($file.FullName): $output"
    }
    Write-Host "Stripped debug sections from $stripped toolkit libraries ($kept import libraries keep theirs)."
}

function Remove-OpucUnusedDependencyFiles([string]$Dependencies) {
    # SDL's test programs and examples (SDL_TESTS / SDL_EXAMPLES are off when aurora builds SDL as a
    # subproject), Tracy's profiler GUI and stand-alone tools (aurora builds only TracyClient from
    # public/), and FreeType's documentation. None of them is read by any configure or build.
    # The rest are each library's own tests, docs and examples. A from-source fallback build still
    # works without them: aurora's extern/CMakeLists.txt turns off every option that reads them
    # (BUILD_TESTING for zlib-ng, PNG_TESTS/PNG_TOOLS for libpng's contrib/, ZSTD_BUILD_TESTS and
    # ZSTD_BUILD_CONTRIB for zstd, FMT_TEST/FMT_DOC default off for a subproject), and the others
    # (xxhash, imgui, libusb, tracy) have no build rule that reads them.
    foreach ($relative in @(
        'SDL\test', 'SDL\examples', 'SDL\VisualC\tests', 'SDL\VisualC-GDK\tests',
        'tracy\profiler', 'tracy\server', 'tracy\update', 'tracy\capture', 'tracy\csvexport', 'tracy\import',
        'tracy\merge', 'tracy\monitor', 'tracy\python', 'tracy\manual', 'tracy\examples', 'tracy\tests',
        'tracy\doc',
        'freetype\docs', 'freetype\tests',
        'xxhash\tests', 'xxhash\doc', 'xxhash\fuzz',
        'zlib\test', 'zlib\doc',
        'zstd\tests', 'zstd\doc', 'zstd\contrib', 'zstd\examples', 'zstd\zlibWrapper\examples',
        'zstd\build\single_file_libs\examples',
        'png\contrib', 'png\tests',
        'fmt\test', 'fmt\doc',
        'imgui\examples', 'imgui\docs',
        'libusb\examples', 'libusb\tests', 'libusb\doc', 'libusb\android\examples'
    )) { Remove-OpucPathIfPresent (Join-Path $Dependencies $relative) }
}

function Remove-OpucUntracedToolkitFiles([string]$Toolkit, [string[]]$TraceFiles) {
    <#
    Cuts the MinGW-w64 headers (llvm-mingw\include, apart from libc++'s include\c++) and the target
    libraries (llvm-mingw\x86_64-w64-mingw32\lib) down to the files a game build reads. The read set
    comes from Test-PayloadBuild.ps1 -TraceOutput, run by Build-Installer.ps1 on this very payload for
    both CPU profiles just before this, so it follows the current runtime sources every release; the
    same script then builds again on the result as the release gate. libc++'s and clang's own headers
    stay whole: they are small, and they are where a new include in translated or runtime code would
    most likely land.
    #>
    $read = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($trace in $TraceFiles) {
        Assert-File $trace 'Toolkit read trace'
        foreach ($line in [IO.File]::ReadAllLines($trace)) { if ($line.Trim()) { [void]$read.Add($line.Trim()) } }
    }
    if ($read.Count -eq 0) { throw 'The toolkit read traces are empty.' }
    $llvm = [IO.Path]::GetFullPath((Join-Path $Toolkit 'llvm-mingw'))
    $libcxx = (Join-Path $llvm 'include\c++') + '\'
    $removed = 0
    $removedBytes = 0L
    foreach ($root in @((Join-Path $llvm 'include'), (Join-Path $llvm 'x86_64-w64-mingw32\lib'))) {
        $kept = 0
        foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force)) {
            if ($file.FullName.StartsWith($libcxx, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if ($read.Contains($file.FullName)) { $kept++; continue }
            $removed++
            $removedBytes += $file.Length
            Remove-Item -LiteralPath $file.FullName -Force
        }
        # A trace that kept nothing here means it did not describe this toolkit at all.
        if ($kept -eq 0) { throw "No traced file lies under $root; refusing to empty it." }
        Get-ChildItem -LiteralPath $root -Recurse -Directory -Force |
            Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1) } |
            Remove-Item -Force
    }
    Write-Host ("Left out {0} MinGW headers and libraries no game build reads ({1:N1} MiB)." -f $removed, ($removedBytes / 1MB))
}

function Assert-OpucSameBuildConfiguration([string]$TraceStage, [string]$GateStage) {
    <#
    A linked product does not prove the cut-down toolkit configures the same way: a header or library
    that a configure check only probes for (check_include_file, find_library, __has_include) could be
    gone without breaking anything, yet flip a result. So the gate build's CMake cache and compiler
    detection files must equal those of the trace build, which ran on the full toolkit. Stage paths
    are normalized; CMAKE_EXE_LINKER_FLAGS is the trace's own -Wl,--verbose.
    #>
    function Read-Normalized([string]$Path, [string]$Stage) {
        Assert-File $Path 'Payload build configuration file'
        $stageForms = @([IO.Path]::GetFullPath($Stage).TrimEnd('\'))
        $stageForms += $stageForms[0].Replace('\', '/')
        $lines = foreach ($line in [IO.File]::ReadAllLines($Path)) {
            if ($line.StartsWith('#') -or $line.StartsWith('//') -or $line -match '^CMAKE_EXE_LINKER_FLAGS:') { continue }
            foreach ($form in $stageForms) {
                $line = [regex]::Replace($line, [regex]::Escape($form), '<STAGE>', 'IgnoreCase')
            }
            $line
        }
        return ,@($lines)
    }
    $traceBuild = Join-Path $TraceStage 'native-build'
    $gateBuild = Join-Path $GateStage 'native-build'
    $files = @('CMakeCache.txt') + @(Get-ChildItem -LiteralPath (Join-Path $traceBuild 'CMakeFiles') -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+' } | ForEach-Object {
            Get-ChildItem -LiteralPath $_.FullName -File -Filter 'CMake*.cmake' | ForEach-Object {
                "CMakeFiles\$($_.Directory.Name)\$($_.Name)" } })
    foreach ($relative in $files) {
        $expected = Read-Normalized (Join-Path $traceBuild $relative) $TraceStage
        $actual = Read-Normalized (Join-Path $gateBuild $relative) $GateStage
        $difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
        if ($difference.Count -gt 0) {
            $shown = ($difference | Select-Object -First 6 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join "`n"
            throw "The cut-down toolkit configures differently from the full one ($relative):`n$shown"
        }
    }
    Write-Host "The cut-down toolkit configures exactly like the full one ($($files.Count) files compared)."
}

# The marker LocalBuild.ps1 looks for: with it, a rejected precompiled package is a damaged
# installation to repair, not a reason to try a from-source aurora build these trees cannot feed.
$OpucSourcesOmittedMarker = 'SOURCES-OMITTED.txt'

function Remove-OpucDependencySources([string]$Dependencies) {
    <#
    Every installation links the precompiled aurora/third-party package (native_prebuilt; the release
    gate in Build-Installer.ps1 refuses a package an installation would reject), so the user's build
    never compiles a dependency. What it still reads from Dependencies\ is fixed by the package itself:
    the include directories native_prebuilt.cmake lists (MKW_NP_INCLUDE_DIRECTORIES) and the Dawn
    package (dawn_prebuilt, kept whole: headers, CMake config, import libraries, runtime DLLs). From
    every other tree only the license files stay, plus a note that says why the sources are gone.
    #>
    $packageFile = Join-Path $Dependencies 'native_prebuilt\native_prebuilt.cmake'
    Assert-File $packageFile 'Precompiled package description'
    $description = [IO.File]::ReadAllText($packageFile)
    $block = [regex]::Match($description, '(?s)set\(MKW_NP_INCLUDE_DIRECTORIES(?<body>[^)]*)\)')
    if (-not $block.Success) { throw "native_prebuilt.cmake has no MKW_NP_INCLUDE_DIRECTORIES: $packageFile" }
    $includeRoots = @([regex]::Matches($block.Groups['body'].Value, '"@DEPS@/(?<path>[^"]+)"') |
        ForEach-Object { (Join-Path $Dependencies $_.Groups['path'].Value.Replace('/', '\')).TrimEnd('\') + '\' })
    if ($includeRoots.Count -eq 0) { throw "native_prebuilt.cmake lists no dependency include directory: $packageFile" }

    $headerExtensions = @('.h', '.hh', '.hpp', '.hxx', '.h++', '.inl', '.inc', '.ipp', '.tcc')
    $licenseName = '^(licen[cs]e|copying|notice|copyright|unlicense)([._-].*)?$'
    $removedBytes = 0L
    foreach ($tree in @(Get-ChildItem -LiteralPath $Dependencies -Directory)) {
        if (@('native_prebuilt', 'dawn_prebuilt') -contains $tree.Name) { continue }
        foreach ($file in @(Get-ChildItem -LiteralPath $tree.FullName -Recurse -File -Force)) {
            if ($file.Name -match $licenseName) { continue }
            $underInclude = @($includeRoots | Where-Object {
                $file.FullName.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
            if ($underInclude -and ($headerExtensions -contains $file.Extension.ToLowerInvariant())) { continue }
            $removedBytes += $file.Length
            Remove-Item -LiteralPath $file.FullName -Force
        }
        # Deepest first, so emptied parents go too.
        Get-ChildItem -LiteralPath $tree.FullName -Recurse -Directory -Force |
            Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1) } |
            Remove-Item -Force
        @"
The sources of this library are not shipped. The game build links the precompiled package in
..\native_prebuilt (built from these pinned sources by the release); only the headers the build
compiles against and the library's license files remain here.
"@ | Set-Content -LiteralPath (Join-Path $tree.FullName $OpucSourcesOmittedMarker) -Encoding UTF8
    }
    Write-Host ("Dependency sources left out: {0:N1} MiB (headers kept under {1})." -f ($removedBytes / 1MB),
        (($includeRoots | ForEach-Object { $_.Substring($Dependencies.Length).Trim('\') }) -join ', '))
}
