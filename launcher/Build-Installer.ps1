[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$OutputDirectory = 'launcher/dist',
    [string]$PortableToolsDirectory = 'launcher/artifacts/portable-tools',
    [string]$DependencySourceDirectory = 'launcher/artifacts/dependencies',
    [string]$VcRuntimeDirectory,
    [string]$ToolkitReleaseTag = $env:GITHUB_REF_NAME,
    # xz (XZ Utils 5.2 or newer) for the payload compression. Defaults to xz on PATH, then the copy Git for
    # Windows ships (present on GitHub's windows runners). The setup decodes it itself (XzDecoderStream).
    [string]$XzPath
)

# Produces launcher/dist/opuc1-recomp-setup.exe: the setup host with the game-code-free payload
# (bundled translator, LLVM-MinGW, CMake, Ninja, nodtool, the runtime and aurora sources, the pinned
# dependency sources and the precompiled aurora/runtime package) appended behind an OPUCPAY2 trailer.
# The payload is xz-compressed tar parts with repeated files stored once (PayloadArchive.cs).
# The user's own disc is translated and compiled on their machine; nothing game-derived ships.
# The host and the translator are Native AOT executables (their csproj files set PublishAot). The
# payload carries no second copy of the host: an installation writes the release file's leading
# host bytes (PayloadArchive.ExtractHost).

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

# The canonical configure flags, and the Assert-File/Assert-Directory/Get-OpucFileSha256
# helpers shared with LocalBuild.ps1 and Prepare-NativePrebuilt.ps1.
. (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1')
# Remove-OpucUnusedToolkitFiles / Remove-OpucUnusedDependencyFiles: what the staged payload leaves out.
. (Join-Path $PSScriptRoot 'PayloadPruning.ps1')

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDirectory))
$portableTools = [IO.Path]::GetFullPath((Join-Path $repoRoot $PortableToolsDirectory))
$dependencySources = [IO.Path]::GetFullPath((Join-Path $repoRoot $DependencySourceDirectory))
$auroraSource = Join-Path (Join-Path $repoRoot 'third_party') 'aurora'
$workRoot = Join-Path $PSScriptRoot 'artifacts\installer-build'
$publish = Join-Path $workRoot 'publish'
$payloadRoot = Join-Path $workRoot 'payload'
$setupProject = Join-Path $PSScriptRoot 'Opuc1Recomp.Setup.Windows\Opuc1Recomp.Setup.Windows.csproj'
$translatorProject = Join-Path $repoRoot 'translator\src\Translator.Cli\Translator.Cli.csproj'
# The setup host's assembly name; the release asset and the copy inside an installation share it.
$setupName = 'opuc1-recomp-setup.exe'

# Every supported release is one projects/<name>/recomp.yml (today only projects/opuc1, PAL ROUPAF).
# Directories without one (the SDK symbol reference map used only by the rebind
# tool) are not releases and are never staged.
$releaseProjects = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'projects') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'recomp.yml') -PathType Leaf } |
    Sort-Object Name)
if ($releaseProjects.Count -eq 0) { throw 'No translation project defines a supported release.' }
$supportedReleases = @(foreach ($releaseProject in $releaseProjects) {
    $pins = Get-OpucProjectPins (Join-Path $releaseProject.FullName 'recomp.yml')
    [ordered]@{ GameId = $pins.GameId; DolSha256 = $pins.DolSha256 }
})
foreach ($key in @('GameId', 'DolSha256')) {
    $duplicates = @($supportedReleases | Group-Object { $_[$key] } | Where-Object Count -gt 1)
    if ($duplicates.Count -gt 0) { throw "Two translation projects pin the same $($key): $($duplicates[0].Name)." }
}
# The audit reports by throwing, which aborts this run; nothing native has been invoked yet, so
# there is no exit code to inspect.
& (Join-Path $PSScriptRoot 'Test-PinnedFacts.ps1') -RepositoryRoot $repoRoot

function Reset-Directory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $allowed = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts')).TrimEnd('\') + '\'
    if (-not $full.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset a directory outside launcher/artifacts: $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    [IO.Directory]::CreateDirectory($full) | Out-Null
}
function Reset-OutputDirectory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $allowed = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
    if (-not $full.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset an output directory outside launcher: $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    [IO.Directory]::CreateDirectory($full) | Out-Null
}
function Resolve-VcRuntimeDirectory([string]$RequestedPath) {
    if ($RequestedPath) {
        $resolved = [IO.Path]::GetFullPath($RequestedPath)
        Assert-File (Join-Path $resolved 'vcruntime140.dll') 'Visual C++ runtime'
        Assert-File (Join-Path $resolved 'msvcp140.dll') 'Visual C++ standard library runtime'
        return $resolved
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    Assert-File $vswhere 'Visual Studio locator (or pass -VcRuntimeDirectory)'
    $installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $installation) { throw 'Visual C++ redistributable files were not found; pass -VcRuntimeDirectory.' }
    $redistRoot = Join-Path $installation 'VC\Redist\MSVC'
    $candidate = Get-ChildItem -LiteralPath $redistRoot -Directory -Recurse | Where-Object {
        $_.Parent.Name -eq 'x64' -and $_.Name -match '^Microsoft\.VC\d+\.CRT$' -and
        (Test-Path (Join-Path $_.FullName 'vcruntime140.dll')) -and (Test-Path (Join-Path $_.FullName 'msvcp140.dll'))
    } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $candidate) { throw "No redistributable x64 Visual C++ runtime was found below $redistRoot." }
    return $candidate.FullName
}
function Copy-Directory([string]$Source, [string]$Destination) {
    Assert-Directory $Source "Source directory"
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    # Source payloads are materialized trees, independent of Git checkout metadata.
    foreach ($entry in Get-ChildItem -LiteralPath $Source -Force) {
        if ($entry.Name -eq '.git') { continue }
        $target = Join-Path $Destination $entry.Name
        if ($entry.PSIsContainer) {
            Copy-Directory $entry.FullName $target
        } else {
            Copy-Item -LiteralPath $entry.FullName -Destination $target -Force
        }
    }
}
function Resolve-Xz([string]$RequestedPath) {
    if ($RequestedPath) {
        $resolved = [IO.Path]::GetFullPath($RequestedPath)
        Assert-File $resolved 'xz'
        return $resolved
    }
    $onPath = Get-Command 'xz.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) { return $onPath.Source }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if (-not $root) { continue }
        foreach ($relative in @('Git\usr\bin\xz.exe', 'Git\mingw64\bin\xz.exe')) {
            $candidate = Join-Path $root $relative
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    throw 'xz was not found; install Git for Windows or XZ Utils, or pass -XzPath.'
}

Assert-File $setupProject '.NET setup project'
Assert-File $translatorProject 'Translator CLI project'
Assert-File (Join-Path $auroraSource 'CMakeLists.txt') 'third_party/aurora sources'

if (-not (Test-Path -LiteralPath (Join-Path $portableTools 'llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe'))) {
    # A PowerShell script, not a native executable - it never touches $LASTEXITCODE, and its own
    # $ErrorActionPreference = 'Stop' + throw already aborts this run on failure.
    & (Join-Path $PSScriptRoot 'Prepare-PortableTools.ps1') -Destination $portableTools
}
Assert-File (Join-Path $portableTools 'CMake\bin\cmake.exe') 'Portable CMake'
Assert-File (Join-Path $portableTools 'Ninja\ninja.exe') 'Portable Ninja'
# native_prebuilt carries the aurora/third-party/runtime archives the user no longer has
# to compile (launcher/Prepare-NativePrebuilt.ps1). The others are the FetchContent sources
# third_party/aurora declares (Prepare-Dependencies.ps1).
# Kept in step with InstalledLayout.DependencyNames by Test-PinnedFacts.ps1: the installed host
# refuses to call a toolkit complete unless every one of these directories is present.
$requiredDependencies = @('abseil-cpp','dawn_prebuilt','fmt','freetype','imgui','libusb','native_prebuilt','png','SDL','sqlite3','tracy','xxhash','zlib','zstd')
$missingSources = @($requiredDependencies | Where-Object { $_ -ne 'native_prebuilt' } |
    Where-Object { -not (Test-Path -LiteralPath (Join-Path $dependencySources $_) -PathType Container) })
if ($missingSources.Count -gt 0) {
    & (Join-Path $PSScriptRoot 'Prepare-Dependencies.ps1') -Destination $dependencySources
}
Assert-Directory $dependencySources 'Pinned offline dependency sources'

# The precompiled archives are only interchangeable with what the user's machine
# compiles if both came from this toolchain and this flag set, so a stale package
# is rebuilt here rather than shipped. Without it every installation would fall
# back to compiling aurora from source, which is what this package exists to
# avoid - and a mismatched one would be an ABI hazard.
$portableClangHash = Get-OpucFileSha256 (Join-Path $portableTools 'llvm-mingw\bin\clang-22.exe')
# The identity the runtime build compares against the package's runtime archive.
function Get-RuntimeSourceFingerprint([string]$RuntimeDirectory) {
    $arguments = @("-DMKW_RUNTIME_DIR=$(ConvertTo-OpucCMakePath $RuntimeDirectory)",
        '-P', (ConvertTo-OpucCMakePath (Join-Path $RuntimeDirectory 'cmake\RuntimeFingerprint.cmake')))
    $output = @(& (Join-Path $portableTools 'CMake\bin\cmake.exe') @arguments)
    if ($LASTEXITCODE -ne 0) { throw "Runtime fingerprinting failed with exit code $LASTEXITCODE." }
    $line = $output | Where-Object { "$_" -match '^runtime_fingerprint=[0-9a-f]{64}$' } | Select-Object -First 1
    if (-not $line) { throw "RuntimeFingerprint.cmake printed no fingerprint: $($output -join ' ')" }
    return "$line".Substring('runtime_fingerprint='.Length)
}
function Test-NativePrebuiltCurrent([string]$PackageDirectory) {
    $provenancePath = Join-Path $PackageDirectory 'provenance.json'
    if (-not (Test-Path -LiteralPath $provenancePath -PathType Leaf)) { return $false }
    $provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
    # Refreshing the pinned Dawn without re-harvesting would ship archives compiled
    # against headers that no longer describe the shipped DLL. A package predating
    # this field cannot prove it matches, so it is treated as stale.
    $recordedDawn = $provenance.PSObject.Properties['DawnRuntimeSha256']
    # Same reasoning for aurora itself: this release ships third_party/aurora sources
    # whose headers the user's runtime compiles against, so archives harvested
    # before an aurora change would be linked into call sites that no longer
    # match them.
    $recordedAurora = $provenance.PSObject.Properties['AuroraSourceFingerprint']
    # And for runtime/third_party (libco, pugixml, toml11), which the precompiled
    # runtime archive was compiled against.
    $recordedThirdParty = $provenance.PSObject.Properties['ThirdPartySourceFingerprint']
    # The precompiled runtime archive must be from these runtime sources; a package without one
    # would make every installation compile the runtime itself.
    $recordedRuntime = $provenance.PSObject.Properties['RuntimeSourceFingerprint']
    # Packages harvested before the runtime DLLs were stripped would ship their debug sections.
    $recordedStripped = $provenance.PSObject.Properties['RuntimeDllsDebugStripped']
    return ($provenance.CompilerSha256 -eq $portableClangHash) -and
           ($null -ne $recordedStripped) -and ($recordedStripped.Value -eq $true) -and
           ($provenance.FlagFingerprint -eq (Get-OpucNativeFlagFingerprint)) -and
           ($null -ne $recordedDawn) -and
           ($recordedDawn.Value -eq (Get-OpucDawnRuntimeSha256 $dependencySources)) -and
           ($null -ne $recordedAurora) -and
           ($recordedAurora.Value -eq (Get-OpucAuroraSourceFingerprint $auroraSource)) -and
           ($null -ne $recordedThirdParty) -and
           ($recordedThirdParty.Value -eq (Get-OpucThirdPartySourceFingerprint (Join-Path $repoRoot 'runtime\third_party'))) -and
           ($null -ne $recordedRuntime) -and
           ($recordedRuntime.Value -eq (Get-RuntimeSourceFingerprint (Join-Path $repoRoot 'runtime')))
}
$nativePrebuiltPackage = Join-Path $dependencySources 'native_prebuilt'
if (-not (Test-NativePrebuiltCurrent $nativePrebuiltPackage)) {
    Write-Host '[0/6] Building the precompiled aurora, third-party and runtime package...'
    & (Join-Path $PSScriptRoot 'Prepare-NativePrebuilt.ps1') `
        -PortableToolsDirectory $portableTools -DependencySourceDirectory $dependencySources
    if ($LASTEXITCODE -ne 0) { throw 'Precompiled aurora package preparation failed.' }
    if (-not (Test-NativePrebuiltCurrent $nativePrebuiltPackage)) {
        throw 'The precompiled aurora package still does not match this toolchain and flag set.'
    }
}
foreach ($name in $requiredDependencies) { Assert-Directory (Join-Path $dependencySources $name) "Pinned dependency $name" }
$vcRuntime = Resolve-VcRuntimeDirectory $VcRuntimeDirectory
$xz = Resolve-Xz $XzPath
Write-Host "Payload compressor: $xz ($((& $xz --version | Select-Object -First 1)))"

Reset-Directory $workRoot
Reset-OutputDirectory $outputRoot
foreach ($path in @($publish,$payloadRoot)) { [IO.Directory]::CreateDirectory($path) | Out-Null }

Write-Host '[1/6] Publishing the Native AOT setup host and translator...'
# PublishAot and the size options come from each csproj; Native AOT output is self-contained and one
# native file by itself. DebugType=None keeps the .pdb out of the publish directory.
& dotnet publish $setupProject -c Release -r win-x64 -p:DebugType=None -o (Join-Path $publish 'setup')
if ($LASTEXITCODE -ne 0) { throw "Setup publish failed with exit code $LASTEXITCODE." }
& dotnet publish $translatorProject -c Release -r win-x64 -p:DebugType=None -o (Join-Path $publish 'translator')
if ($LASTEXITCODE -ne 0) { throw "Translator publish failed with exit code $LASTEXITCODE." }
$setupHost = Join-Path $publish "setup\$setupName"
$translator = Join-Path $publish 'translator\Translator.Cli.exe'
Assert-File $setupHost 'Published setup host'
Assert-File $translator 'Self-contained translator'

# Resolved via the shared Opuc1Recomp.Setup.Common.Cli helper (also used by build-appimage.sh on
# Linux) rather than a separate download/version-pin copy here: it downloads and caches the same way
# NodToolProvider.cs always does (launcher/artifacts/nodtool.exe)
$nodToolCliProject = Join-Path $PSScriptRoot 'Opuc1Recomp.Setup.Common.Cli'
$nodTool = (& dotnet run --project $nodToolCliProject -c Release -- --workspace $repoRoot | Select-Object -Last 1)
if ($LASTEXITCODE -ne 0) { throw "nodtool resolution failed with exit code $LASTEXITCODE." }
Assert-File $nodTool 'Resolved nodtool'

Write-Host '[2/6] Staging the explicit, game-code-free payload allowlist...'
# The staged layout mirrors the installed layout exactly (Toolkit, BuildWorkspace): payload
# identities hash relative paths, so the names here are part of the fingerprint contract.
$toolkit = Join-Path $payloadRoot 'Toolkit'
$workspace = Join-Path $payloadRoot 'BuildWorkspace'
[IO.Directory]::CreateDirectory($toolkit) | Out-Null
Copy-Directory (Join-Path $portableTools 'llvm-mingw') (Join-Path $toolkit 'llvm-mingw')
Copy-Directory (Join-Path $portableTools 'CMake') (Join-Path $toolkit 'CMake')
Copy-Directory (Join-Path $portableTools 'Ninja') (Join-Path $toolkit 'Ninja')
Remove-OpucUnusedToolkitFiles $toolkit
Remove-OpucDebugSections $toolkit
[IO.Directory]::CreateDirectory((Join-Path $toolkit 'Translator')) | Out-Null
Copy-Item -LiteralPath $translator -Destination (Join-Path $toolkit 'Translator\Translator.Cli.exe')
Copy-Item -LiteralPath $nodTool -Destination (Join-Path $toolkit 'nodtool.exe')
[IO.Directory]::CreateDirectory((Join-Path $toolkit 'Redist')) | Out-Null
Copy-Item -Path (Join-Path $vcRuntime '*.dll') -Destination (Join-Path $toolkit 'Redist')
Copy-Item -Path (Join-Path $vcRuntime '*.dll') -Destination (Join-Path $toolkit 'CMake\bin')
Copy-Item -Path (Join-Path $vcRuntime '*.dll') -Destination (Join-Path $toolkit 'Ninja')
Copy-Item -Path (Join-Path $vcRuntime '*.dll') -Destination $toolkit

Copy-Directory (Join-Path $repoRoot 'runtime') (Join-Path $workspace 'runtime')
$stagedAurora = Join-Path $workspace 'third_party\aurora'
Copy-Directory $auroraSource $stagedAurora
# Source trees can contain ignored developer build output. It is never a release input.
foreach ($developerOutput in @(
    (Join-Path $workspace 'runtime\build'),
    (Join-Path $workspace 'runtime\native-index.json'),
    (Join-Path $stagedAurora 'build')
)) {
    if (Test-Path -LiteralPath $developerOutput) { Remove-Item -LiteralPath $developerOutput -Recurse -Force }
}
# extern/ keeps only its CMakeLists.txt; every FetchContent tree ships under Dependencies.
Get-ChildItem -LiteralPath (Join-Path $stagedAurora 'extern') -Directory -Force | Remove-Item -Recurse -Force
foreach ($releaseProject in $releaseProjects) {
    $stagedProject = Join-Path $workspace "projects\$($releaseProject.Name)"
    Copy-Directory $releaseProject.FullName $stagedProject
    # The repository project reads the developer's extracted disc; an installed workspace keeps
    # the user's main.dol at BuildWorkspace\Assets\main.dol (workspace_root stays ../..).
    Set-OpucProjectDolPath (Join-Path $stagedProject 'recomp.yml') 'Assets/main.dol'
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LocalBuild.ps1') -Destination (Join-Path $workspace 'LocalBuild.ps1')
# LocalBuild.ps1 dot-sources the canonical configure flags from this sibling.
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1') -Destination (Join-Path $workspace 'NativeBuildFlags.ps1')
[IO.Directory]::CreateDirectory((Join-Path $workspace 'Dependencies')) | Out-Null
foreach ($name in $requiredDependencies) { Copy-Directory (Join-Path $dependencySources $name) (Join-Path $workspace "Dependencies\$name") }
Remove-OpucUnusedDependencyFiles (Join-Path $workspace 'Dependencies')
Remove-OpucDependencySources (Join-Path $workspace 'Dependencies')

# The translation identity covers the runtime's native registrations through this index (see
# ToolkitFingerprint), so it is extracted from exactly the staged runtime sources.
$nativeIndex = Join-Path $workspace 'runtime\native-index.json'
# The payload has no game inputs, so the project file (which validates them) cannot be loaded here.
& $translator emit-native-index --native-source-dir (Join-Path $workspace 'runtime\src') --out $nativeIndex
if ($LASTEXITCODE -ne 0) { throw "Native index extraction failed with exit code $LASTEXITCODE." }
Assert-File $nativeIndex 'Runtime native index'
# A staged runtime that fingerprints differently from the shipped archive would make every
# installation silently compile the runtime from source.
$stagedRuntimeFingerprint = Get-RuntimeSourceFingerprint (Join-Path $workspace 'runtime')
$packagedRuntimeFingerprint = (Get-Content -LiteralPath (Join-Path $workspace 'Dependencies\native_prebuilt\provenance.json') -Raw |
    ConvertFrom-Json).RuntimeSourceFingerprint
if ($stagedRuntimeFingerprint -ne $packagedRuntimeFingerprint) {
    throw "The staged runtime ($stagedRuntimeFingerprint) does not match the precompiled runtime archive ($packagedRuntimeFingerprint)."
}
# Users' builds run LocalBuild.ps1 under Windows PowerShell 5.1, whatever shell built this
# installer, so the staged package is accepted (or not) exactly the way theirs will be.
$prebuiltGate = Join-Path $workRoot 'native-prebuilt-gate.ps1'
@'
param([string]$Workspace, [string]$Toolkit)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $Workspace 'NativeBuildFlags.ps1')
$package = Resolve-OpucNativePrebuiltDirectory (Join-Path $Workspace 'Dependencies') `
    (Join-Path $Toolkit 'llvm-mingw\bin') (Join-Path $Workspace 'third_party\aurora') (Join-Path $Workspace 'runtime\third_party')
if (-not $package) { exit 3 }
'@ | Set-Content -LiteralPath $prebuiltGate -Encoding UTF8
& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoLogo -NoProfile -NonInteractive `
    -ExecutionPolicy Bypass -File $prebuiltGate -Workspace $workspace -Toolkit $toolkit
if ($LASTEXITCODE -ne 0) {
    throw "Windows PowerShell rejects the staged precompiled aurora package (exit $LASTEXITCODE); every installation would build aurora from source."
}
Remove-Item -LiteralPath $prebuiltGate -Force

# What a game build reads from the MinGW headers and libraries is traced on this payload (its own
# translator, toolkit, headers-only Dependencies and precompiled package, a synthetic DOL), the
# toolkit is cut down to that, and the same build must then still link the product on the result.
# Both CPU profiles: v2 compiles the runtime itself, v3 links the precompiled runtime archive.
Write-Host 'Tracing which toolkit files a game build reads, then building again on the cut-down toolkit...'
$payloadBuilds = Join-Path $workRoot 'payload-build'
$traces = [Collections.Generic.List[string]]::new()
foreach ($cpuProfile in @('v3', 'v2')) {
    $trace = Join-Path $payloadBuilds "trace-$cpuProfile.txt"
    & (Join-Path $PSScriptRoot 'Test-PayloadBuild.ps1') -PayloadRoot $payloadRoot `
        -StageDirectory (Join-Path $payloadBuilds "trace-$cpuProfile") -X86CpuProfile $cpuProfile -TraceOutput $trace |
        Out-Host
    $traces.Add($trace)
}
Remove-OpucUntracedToolkitFiles $toolkit $traces.ToArray()
foreach ($cpuProfile in @('v3', 'v2')) {
    & (Join-Path $PSScriptRoot 'Test-PayloadBuild.ps1') -PayloadRoot $payloadRoot `
        -StageDirectory (Join-Path $payloadBuilds "gate-$cpuProfile") -X86CpuProfile $cpuProfile | Out-Host
    Assert-OpucSameBuildConfiguration (Join-Path $payloadBuilds "trace-$cpuProfile") (Join-Path $payloadBuilds "gate-$cpuProfile")
}
Remove-Item -LiteralPath $payloadBuilds -Recurse -Force

Write-Host '[3/6] Writing manifests and third-party license inventory...'
[IO.Directory]::CreateDirectory((Join-Path $payloadRoot 'licenses')) | Out-Null
Copy-Item (Join-Path $repoRoot 'LICENSE') (Join-Path $payloadRoot 'licenses\LICENSE-GPL-3.0.txt')
Copy-Item (Join-Path $repoRoot 'THIRD-PARTY-NOTICES.md') (Join-Path $payloadRoot 'licenses\THIRD-PARTY-NOTICES.md')
Copy-Item (Join-Path $portableTools 'README-LICENSES.txt') (Join-Path $payloadRoot 'licenses\Portable-build-tools.txt')
Copy-Item (Join-Path $auroraSource 'LICENSE') (Join-Path $payloadRoot 'licenses\Aurora-LICENSE.txt')
# The precompiled aurora/third-party archives are built from the very sources
# already shipped under BuildWorkspace\Dependencies and third_party\aurora, so they add
# no third-party component and therefore no new license obligation.
@"
nodtool (disc image extraction)

Project: https://github.com/encounter/nod
Dual-licensed under MIT OR Apache-2.0.

MIT License

Copyright 2021 Luke Street.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"@ | Set-Content (Join-Path $payloadRoot 'licenses\nodtool-LICENSE-MIT.txt') -Encoding UTF8
@"
Microsoft Visual C++ Runtime

Redistributable x64 runtime DLLs are included app-locally for nodtool and third-party renderer DLLs.
Microsoft license terms: https://visualstudio.microsoft.com/license-terms/
"@ | Set-Content (Join-Path $payloadRoot 'licenses\Microsoft-VC-Runtime.txt') -Encoding UTF8
# Compute the payload's content identities once, here, with the same code every installed host
# uses. Shipping them in the manifest is what keeps user machines from re-hashing the toolkit
# during installation; only --repair-products re-derives content identity locally.
Write-Host 'Computing the payload content identities (hashes the staged toolkit once)...'
$identityJson = & $setupHost --emit-payload-identities --payload-root $payloadRoot
if ($LASTEXITCODE -ne 0) { throw "Payload identity computation failed with exit code $LASTEXITCODE." }
$identities = ($identityJson -join '') | ConvertFrom-Json
foreach ($required in @('ToolkitFingerprint','TranslationFingerprint','NativeToolchainFingerprint',
    'ToolkitPackageFingerprint','RuntimeAssetsFingerprint')) {
    if ([string]::IsNullOrWhiteSpace($identities.$required)) { throw "Payload identity output is missing $required." }
}

$productVersion = ((& $setupHost --version) -join '').Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($productVersion)) { throw 'The setup host did not report its version.' }

$manifest = [ordered]@{
    SchemaVersion = 1
    ProductVersion = $productVersion
    SupportedReleases = $supportedReleases
    ToolkitReleaseTag = if ($ToolkitReleaseTag) { $ToolkitReleaseTag } else { '' }
    BuildModel = 'local-translation-and-compilation'
    ToolkitFingerprint = $identities.ToolkitFingerprint
    ToolkitPackageFingerprint = $identities.ToolkitPackageFingerprint
    RuntimeAssetsFingerprint = $identities.RuntimeAssetsFingerprint
    TranslationFingerprint = $identities.TranslationFingerprint
    NativeToolchainFingerprint = $identities.NativeToolchainFingerprint
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $payloadRoot 'payload-manifest.json') -Encoding UTF8

Write-Host '[4/6] Enforcing the copyright and generated-code boundary...'
# Both audits are PowerShell scripts that throw directly on failure (Test-PayloadBoundary.ps1
# never invokes a native command at all, so it never touches $LASTEXITCODE); there is no exit
# code to check here, and doing so risks reading a stale value from an unrelated earlier command.
& (Join-Path $PSScriptRoot 'Test-PayloadBoundary.ps1') -PayloadRoot $payloadRoot
& (Join-Path $PSScriptRoot 'Test-NativeDependencies.ps1') -PayloadRoot $payloadRoot -SetupHost $setupHost `
    -LlvmReadobjPath (Join-Path $portableTools 'llvm-mingw\bin\llvm-readobj.exe')

Write-Host '[5/6] Packing the installer payload (xz, repeated files stored once)...'
# The setup host writes the format it reads (PayloadPacker / PayloadArchive), so packer and reader
# cannot drift apart.
$payloadFile = Join-Path $workRoot 'payload.bin'
& $setupHost --pack-payload --payload-root $payloadRoot --xz $xz --output $payloadFile
if ($LASTEXITCODE -ne 0) { throw "Payload packing failed with exit code $LASTEXITCODE." }
Assert-File $payloadFile 'Packed payload'

Write-Host '[6/6] Producing the single-file setup executable...'
$outputSetup = Join-Path $outputRoot $setupName
Copy-Item $setupHost $outputSetup -Force
$outputStream = [IO.File]::Open($outputSetup, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $offset = $outputStream.Position
    $input = [IO.File]::OpenRead($payloadFile)
    try { $input.CopyTo($outputStream) } finally { $input.Dispose() }
    $length = $outputStream.Position - $offset
    $writer = [IO.BinaryWriter]::new($outputStream, [Text.Encoding]::ASCII, $true)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('OPUCPAY2'))
        $writer.Write([Int64]$offset)
        $writer.Write([Int64]$length)
    } finally { $writer.Dispose() }
} finally { $outputStream.Dispose() }

# The release file unpacks itself the way an installation does and must give back the staged tree
# byte for byte; a packer, decoder or link bug fails the release here instead of on a user's machine.
& $outputSetup --verify-payload --payload-root $payloadRoot
if ($LASTEXITCODE -ne 0) { throw "The finished setup does not reproduce its payload (exit $LASTEXITCODE)." }

$setupItem = Get-Item -LiteralPath $outputSetup
Write-Host ("{0}: {1} MiB; SHA-256 {2}" -f $setupItem.Name,
    [math]::Round($setupItem.Length / 1MB, 1), (Get-OpucFileSha256 $outputSetup))
