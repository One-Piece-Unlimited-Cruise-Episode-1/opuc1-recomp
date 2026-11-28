# Builds the game product from a staged installer payload exactly the way an installation does
# (the payload's own translator, LLVM-MinGW, CMake and Ninja, its headers-only Dependencies and the
# precompiled aurora/runtime package), but from a synthetic DOL instead of the player's disc, so it
# runs on a release machine that has no game data.
#
# Build-Installer.ps1 runs it twice per CPU profile: once with -TraceOutput on the full toolkit,
# which records every toolkit file the configure checks, compiles and links read (ninja depfiles,
# kept try_compile trees and the linker's verbose output), so the toolkit can be cut down to that
# set; and once more on the cut-down toolkit as the release gate.
#
# The synthetic program (li r3,40; addi r3,r3,2; nop; blr) is the one Test-Recompilation.ps1 uses;
# that test builds aurora from source with the maintainer toolchain, this one never does.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$PayloadRoot,
    [Parameter(Mandatory)] [string]$StageDirectory,
    [ValidateSet('v2', 'v3')] [string]$X86CpuProfile = 'v3',
    # When given: the file receives one absolute path per line, every file under Toolkit\llvm-mingw
    # this build read.
    [string]$TraceOutput = '',
    [ValidateRange(1, 64)] [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$payload = [IO.Path]::GetFullPath($PayloadRoot)
$toolkit = Join-Path $payload 'Toolkit'
$workspace = Join-Path $payload 'BuildWorkspace'
. (Join-Path $workspace 'NativeBuildFlags.ps1')

$stage = [IO.Path]::GetFullPath($StageDirectory)
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
[IO.Directory]::CreateDirectory($stage) | Out-Null

$bin = Join-Path $toolkit 'llvm-mingw\bin'
$cmake = Join-Path $toolkit 'CMake\bin\cmake.exe'
$ninja = Join-Path $toolkit 'Ninja\ninja.exe'
$translator = Join-Path $toolkit 'Translator\Translator.Cli.exe'
$dependencies = Join-Path $workspace 'Dependencies'
foreach ($tool in @($cmake, $ninja, $translator, (Join-Path $bin 'x86_64-w64-mingw32-clang++.exe'))) {
    Assert-File $tool 'Payload tool'
}

# The runtime build writes beside its sources (../generated), so the sources are copied; the
# Dependencies tree and the toolkit are read in place, as an installation reads them.
Copy-Item -LiteralPath (Join-Path $workspace 'runtime') -Destination (Join-Path $stage 'runtime') -Recurse
[IO.Directory]::CreateDirectory((Join-Path $stage 'third_party')) | Out-Null
Copy-Item -LiteralPath (Join-Path $workspace 'third_party\aurora') -Destination (Join-Path $stage 'third_party\aurora') -Recurse

[uint32]$entry = 0x80001000L
$textSize = 0x10
$dataOffset = 0x100 + $textSize
$dol = [byte[]]::new($dataOffset + 4)
function Write-BigEndian32([int]$Offset, [uint32]$Value) {
    $dol[$Offset] = [byte](($Value -shr 24) -band 255)
    $dol[$Offset + 1] = [byte](($Value -shr 16) -band 255)
    $dol[$Offset + 2] = [byte](($Value -shr 8) -band 255)
    $dol[$Offset + 3] = [byte]($Value -band 255)
}
Write-BigEndian32 0x00 0x100       # text[0] file offset
Write-BigEndian32 0x48 $entry      # text[0] guest address
Write-BigEndian32 0x90 $textSize   # text[0] length
Write-BigEndian32 0x1C $dataOffset # data[0] file offset
Write-BigEndian32 0x64 0x80600000L # data[0] guest address
Write-BigEndian32 0xAC 4           # data[0] length
Write-BigEndian32 0xD8 0x80601000L # BSS address
Write-BigEndian32 0xDC 32          # BSS length
Write-BigEndian32 0xE0 $entry      # entry point
Write-BigEndian32 0x100 0x38600028 # li r3,40
Write-BigEndian32 0x104 0x38630002 # addi r3,r3,2
Write-BigEndian32 0x108 0x60000000 # nop
Write-BigEndian32 0x10C 0x4E800020 # blr
Write-BigEndian32 $dataOffset 0x12345678
[IO.File]::WriteAllBytes((Join-Path $stage 'synthetic.dol'), $dol)
[IO.File]::WriteAllText((Join-Path $stage 'synthetic-functions.txt'), ('{0:X8} func_{0:X8}' -f $entry))
$manifest = Join-Path $stage 'recomp.yml'
[IO.File]::WriteAllText($manifest, @"
schema_version: 1
workspace_root: .
project:
  id: payload-synthetic-dol
  display_name: Payload Synthetic DOL
memory:
  base: 0x80000000
  size: 0x01800000
  sda_base: 0x80600000
  sda2_base: 0x80600000
inputs:
  dol:
    path: synthetic.dol
translation:
  entry_points: [$('0x{0:X8}' -f $entry)]
  function_map:
    path: synthetic-functions.txt
  allow_unsupported_instructions: false
runtime:
  native_abi_directories: []
  native_registration_root: runtime/src
output:
  root: generated
"@)

# An icon resource like the one the setup generates from the save banner (ProductIcon.cs writes
# '1 ICON "opuc1_recomp.ico"'), so the resource compiler runs too. A 1x1 32-bit icon.
$icons = Join-Path $stage 'icon'
[IO.Directory]::CreateDirectory($icons) | Out-Null
$ico = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($ico)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]1)                 # ICONDIR
$writer.Write([byte]1); $writer.Write([byte]1); $writer.Write([byte]0); $writer.Write([byte]0)
$writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]48); $writer.Write([uint32]22)
$writer.Write([uint32]40); $writer.Write([int32]1); $writer.Write([int32]2)                   # BITMAPINFOHEADER
$writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]0); $writer.Write([uint32]8)
$writer.Write([int32]0); $writer.Write([int32]0); $writer.Write([uint32]0); $writer.Write([uint32]0)
$writer.Write([uint32]0xFF2060C0L); $writer.Write([uint32]0)                                    # pixel, AND mask
$writer.Flush()
[IO.File]::WriteAllBytes((Join-Path $icons 'opuc1_recomp.ico'), $ico.ToArray())
[IO.File]::WriteAllText((Join-Path $icons 'opuc1_recomp.rc'), "1 ICON `"opuc1_recomp.ico`"`n")

function ConvertTo-CommandLineArgument([string]$Value) {
    # The MSVCRT rules CreateProcess callers follow: quote when needed, double the backslashes
    # that precede a quote.
    if ($Value -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
function Invoke-Logged([string]$File, [string[]]$Arguments, [string]$Log) {
    <#
    Runs a native tool with stdout and stderr written to $Log as they are. Windows PowerShell's own
    redirection would turn stderr lines into error records (terminating under 'Stop') and wrap
    long lines, which would break the paths read back from the log.
    #>
    $start = [Diagnostics.ProcessStartInfo]::new($File)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = ($Arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' '
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [IO.File]::WriteAllText($Log, $stdout.Result + $stderr.Result)
    return $process.ExitCode
}

$oldPath = $env:PATH
try {
    $env:PATH = Get-OpucToolchainPath $toolkit
    $metadata = Join-Path $stage 'generated\base_translation_output.json'
    Invoke-Checked $translator @('translate-recursive', ('0x{0:X8}' -f $entry), '--project', $manifest,
        '--output-metadata', $metadata, '--threads', "$Parallel") 'Translating the synthetic DOL'
    Invoke-Checked $translator @('generate-data-init', '--project', $manifest) 'Generating synthetic data initialization'
    Invoke-Checked $translator @('emit-build-shards', '--project', $manifest) 'Emitting the build graph'

    $package = Resolve-OpucNativePrebuiltDirectory $dependencies $bin (Join-Path $workspace 'third_party\aurora') `
        (Join-Path $workspace 'runtime\third_party')
    if (-not $package) { throw 'The staged payload rejects its own precompiled aurora package.' }

    $build = Join-Path $stage 'native-build'
    $additional = @("-DMKW_X86_CPU_PROFILE=$X86CpuProfile", "-DOPUC_ICON_DIR=$(ConvertTo-OpucCMakePath $icons)")
    # Tracing only: keep the try_compile trees and make lld name every file it reads. Neither flag
    # is part of the canonical flag set, so the precompiled package is still accepted.
    if ($TraceOutput) { $additional += @('--debug-trycompile', '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--verbose') }
    $configure = Get-OpucNativeConfigureArguments -SourceDirectory (Join-Path $stage 'runtime') -BuildDirectory $build `
        -Ninja $ninja -CCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang.exe') `
        -CxxCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang++.exe') `
        -ResourceCompiler (Join-Path $bin 'x86_64-w64-mingw32-windres.exe') `
        -AuroraDirectory (Join-Path $stage 'third_party\aurora') `
        -DependenciesDirectory $dependencies -NativePrebuiltDirectory $package -AdditionalArguments $additional
    $buildLog = Join-Path $stage 'build-output.txt'
    $code = Invoke-Logged $cmake $configure (Join-Path $stage 'configure-output.txt')
    if ($code -ne 0) { throw "Configuring the payload build failed ($code); see $stage\configure-output.txt" }
    $code = Invoke-Logged $cmake @('--build', $build, '--target', 'opuc1_recomp', '--parallel', "$Parallel") $buildLog
    if ($code -ne 0) { throw "Building the product from the payload failed ($code); see $buildLog" }
    Assert-File (Join-Path $build 'opuc1-recomp.exe') 'Product linked from the payload'

    if ($TraceOutput) {
        $read = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $toolchainRoot = [IO.Path]::GetFullPath((Join-Path $toolkit 'llvm-mingw')).TrimEnd('\') + '\'
        function Add-Read([string]$Path) {
            $trimmed = $Path.Trim().Trim('"')
            if (-not $trimmed) { return }
            $full = [IO.Path]::GetFullPath($trimmed.Replace('/', '\'))
            if ($full.StartsWith($toolchainRoot, [StringComparison]::OrdinalIgnoreCase)) { [void]$read.Add($full) }
        }
        # Headers: every ninja-built object records its includes, the main build and each kept
        # try_compile tree alike.
        $ninjaTrees = @($build) + @(Get-ChildItem -LiteralPath (Join-Path $build 'CMakeFiles\CMakeScratch') -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
        foreach ($tree in $ninjaTrees) {
            if (-not (Test-Path -LiteralPath (Join-Path $tree '.ninja_deps'))) { continue }
            Push-Location $tree
            try { $deps = & $ninja -t deps } finally { Pop-Location }
            if ($LASTEXITCODE -ne 0) { throw "ninja -t deps failed in $tree" }
            foreach ($line in $deps) { if ($line -match '^\s{4}(\S.*)$') { Add-Read $Matches[1] } }
        }
        # Libraries and objects: lld's "Reading <file>" lines, from the product link and from every
        # configure-time test link.
        foreach ($log in @($buildLog, (Join-Path $build 'CMakeFiles\CMakeConfigureLog.yaml'))) {
            if (-not (Test-Path -LiteralPath $log)) { continue }
            foreach ($match in [regex]::Matches([IO.File]::ReadAllText($log), 'ld\.lld: Reading (?<path>[^\r\n]+)')) {
                Add-Read $match.Groups['path'].Value
            }
        }
        $sorted = [string[]]@($read)
        [Array]::Sort($sorted, [StringComparer]::OrdinalIgnoreCase)
        [IO.File]::WriteAllLines([IO.Path]::GetFullPath($TraceOutput), $sorted)
        Write-Host "Payload build ($X86CpuProfile) read $($sorted.Count) toolkit files."
    } else {
        Write-Host "Payload build ($X86CpuProfile) linked the product."
    }
} finally {
    $env:PATH = $oldPath
}
