[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Workspace,
    [Parameter(Mandatory)] [string]$Toolkit,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    # The product icon the setup host generated from the user's disc (ProductIcon in
    # Opuc1Recomp.Setup.Common). Optional: without it the executable simply has no icon.
    [string]$IconDirectory = '',
    # The caller's recomputed cache-reuse identities (see ToolkitFingerprint.ComputeComponents).
    # They are compared against the provenance recorded beside the caches themselves; a missing or
    # mismatched identity degrades that cache to a clean rebuild, never the other way around.
    [string]$TranslationFingerprint = '',
    [string]$NativeToolchainFingerprint = '',
    # Discards every cache first. Used when the product was reported broken/blocked, so a possibly
    # corrupted translation or build directory can never contribute to the repaired product.
    [switch]$ForceCleanBuild,
    [int]$Parallel = 0,
    [ValidateSet('v2', 'v3')] [string]$X86CpuProfile = 'v3'
)

# Translates the user's own main.dol and compiles One Piece: Unlimited Recompilation Episode 1 with
# the bundled toolchain. Run by the installed setup host (LocalBuildService.cs) against a
# BuildWorkspace / Toolkit pair:
#   BuildWorkspace\runtime, third_party\aurora, projects\opuc1\recomp.yml (inputs.dol.path =
#   Assets/main.dol), Assets\main.dol, Dependencies\, LocalBuild.ps1, NativeBuildFlags.ps1
#   Toolkit\Translator\Translator.Cli.exe, CMake\, Ninja\, llvm-mingw\, Redist\

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

# Canonical configure flags (plus Assert-File / Invoke-Checked / Get-OpucFileSha256),
# shared with launcher/Prepare-NativePrebuilt.ps1 so the precompiled aurora/third-party
# archives and the objects compiled here can never be produced with different settings.
# See NativeBuildFlags.ps1; Build-Installer.ps1 stages it beside this script.
. (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1')

function Write-OpucBuildStep([string]$StepId, [string]$Message) {
    <#
    Announces a build step. The identifier is the contract (BuildStepIds in
    Opuc1Recomp.Setup.Common/InstallProgress.cs); the trailing sentence is only for the log.
    #>
    Write-Host "OPUCBUILD:STEP:${StepId} $Message"
}

function Reset-LocalDirectory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($Workspace).TrimEnd('\') + '\'
    $installRoot = [IO.Path]::GetFullPath((Split-Path -Parent $realWorkspace)).TrimEnd('\') + '\'
    # The caller-supplied output destination is a legitimate reset target by definition,
    # wherever the caller placed it: a fresh install's operation scratch lives beside the
    # installation directory rather than inside it.
    $isAllowedDestination = $full.TrimEnd('\').Equals($OutputDirectory.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)
    if (-not $isAllowedDestination -and
        -not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -and
        -not $full.StartsWith($installRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset a directory outside the local build workspace: $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    [IO.Directory]::CreateDirectory($full) | Out-Null
}

$Workspace = [IO.Path]::GetFullPath($Workspace)
$Toolkit = [IO.Path]::GetFullPath($Toolkit)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$realWorkspace = $Workspace.TrimEnd('\')
$Workspace = Get-OpucBuildSafePath $realWorkspace 'workspace' 'runtime\CMakeLists.txt'
if (-not [string]::IsNullOrWhiteSpace($IconDirectory)) {
    $IconDirectory = [IO.Path]::GetFullPath($IconDirectory)
    # The icon directory normally sits inside the workspace; give it the workspace's safe spelling.
    if ($IconDirectory.StartsWith($realWorkspace + '\', [StringComparison]::OrdinalIgnoreCase)) {
        $IconDirectory = $Workspace + $IconDirectory.Substring($realWorkspace.Length)
    }
}
$translator = Join-Path $Toolkit 'Translator\Translator.Cli.exe'
# A translator step takes seconds on 16 threads; past this it has hung, and failing lets the clean retry run.
$translatorTimeoutSeconds = 1800
$toolchain = Get-OpucBuildSafePath $Toolkit 'toolchain' 'CMake\bin\cmake.exe'
$cmake = Join-Path $toolchain 'CMake\bin\cmake.exe'
$ninja = Join-Path $toolchain 'Ninja\ninja.exe'
$toolchainBin = Join-Path $toolchain 'llvm-mingw\bin'
$cc = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang.exe'
$cxx = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang++.exe'
$windres = Join-Path $toolchainBin 'x86_64-w64-mingw32-windres.exe'
$assets = Join-Path $Workspace 'Assets'
$aurora = Join-Path $Workspace 'third_party\aurora'
$generated = Join-Path $Workspace 'generated'
$functions = Join-Path $generated 'functions'
$metadata = Join-Path $generated 'base_translation_output.json'
$sourceBundle = Join-Path $generated 'base_translation_sources.bin'
$shards = Join-Path $generated 'build_shards'
$build = Join-Path $Workspace 'native-build'
# CMake target opuc1_recomp, OUTPUT_NAME opuc1-recomp (runtime/cmake/PublicProducts.cmake).
$productTarget = 'opuc1_recomp'
$productExecutable = 'opuc1-recomp.exe'

foreach ($required in @(
    @($translator, 'Bundled translator'), @($cmake, 'Bundled CMake'), @($ninja, 'Bundled Ninja'),
    @($cc, 'Bundled C compiler'), @($cxx, 'Bundled C++ compiler'), @($windres, 'Bundled resource compiler'),
    @((Join-Path $assets 'main.dol'), 'Extracted main.dol'),
    @((Join-Path $aurora 'CMakeLists.txt'), 'Aurora sources')
)) { Assert-File $required[0] $required[1] }

# Everything game-specific this script needs is read from the project file rather than restated
# here: the translation entry point below and the pinned main.dol digest. The project is the one
# pinned to the extracted main.dol, so any other executable is refused before translation.
$selectedProject = Resolve-OpucProjectForAssets $Workspace $assets
$project = $selectedProject.Path
$pins = $selectedProject.Pins

# Three independent knobs: $translatorThreads (the translator's own worker threads), $translatedJobs
# (the real RAM guard, capping concurrent clang compiles of memory-hungry translated TUs via the
# Ninja MKW_TRANSLATED_COMPILE_JOBS pool), and $globalJobs (Ninja's overall parallelism for
# everything else). An explicit -Parallel pins all three.
$memoryGiB = [math]::Max(1, [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB))
if ($Parallel -gt 0) {
    $translatorThreads = $Parallel
    $translatedJobs = $Parallel
    $globalJobs = $Parallel
} else {
    $translatorThreads = [math]::Max(1, [math]::Min([Environment]::ProcessorCount, 16))
    # A translated TU peaks around 0.26 GB in clang; 0.5 GiB each after a 2 GiB reserve.
    $translatedJobs = [math]::Max(1, [math]::Min([Environment]::ProcessorCount,
        [math]::Floor(($memoryGiB - 2) / 0.5)))
    $globalJobs = [math]::Max($translatedJobs, [Environment]::ProcessorCount)
}

$oldPath = $env:PATH
$oldDotnet = $env:DOTNET_ROOT
try {
    $env:PATH = Get-OpucToolchainPath $toolchain
    Remove-Item Env:DOTNET_ROOT -ErrorAction SilentlyContinue
    Push-Location $Workspace
    try {
        # This script is the single owner of every cache-reuse decision: it is what would misbuild.
        # The caller only supplies the identities to compare recorded provenance against.
        $translationProvenance = Join-Path $generated 'translation-provenance.json'
        $toolchainProvenance = Join-Path $build 'toolchain-provenance.json'

        function Get-RecordedFingerprint([string]$Path, [string]$Property) {
            if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
            try {
                $record = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
                $value = $record.PSObject.Properties[$Property]
                if ($null -eq $value -or [string]::IsNullOrWhiteSpace($value.Value)) { return '' }
                return [string]$value.Value
            } catch { return '' }
        }

        if ($ForceCleanBuild) {
            Write-Host 'OPUCBUILD: A clean build was requested; discarding every translation and build cache'
            Reset-LocalDirectory $generated
            if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
        }

        # Reused only when its exact inputs match and its artifacts survived. Otherwise it
        # retranslates incrementally: content-addressed outputs plus --prune-stale mean Ninja only
        # recompiles shards whose bytes actually moved.
        $reuseTranslation = $false
        if (-not $ForceCleanBuild -and -not [string]::IsNullOrWhiteSpace($TranslationFingerprint) -and
            (Get-RecordedFingerprint $translationProvenance 'TranslationFingerprint') -eq $TranslationFingerprint -and
            (Get-RecordedFingerprint $translationProvenance 'DolSha256') -eq $pins.DolSha256) {
            $artifacts = @($metadata, $sourceBundle)
            $reuseTranslation = -not ($artifacts | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
        }

        if ($reuseTranslation) {
            Write-OpucBuildStep 'reuse-translation' 'Reusing the completed translation'
        } else {
            # A failed translation must never leave provenance claiming the old outputs are current.
            if (Test-Path -LiteralPath $translationProvenance) { Remove-Item -LiteralPath $translationProvenance -Force }
            [IO.Directory]::CreateDirectory($generated) | Out-Null

            # No --clean-outdir: shard names are content-addressed and unchanged files keep their
            # bytes and mtimes, which is exactly what lets Ninja skip them. --prune-stale (active
            # only without --clean-outdir) removes everything the new translation no longer emits.
            Invoke-Checked $translator @(
                'translate-recursive', $pins.EntryPoint, '--project', $project,
                '--outdir', $functions, '--output-metadata', $metadata,
                '--production-source-bundle', $sourceBundle,
                '--no-function-files', '--prune-stale', '--threads', $translatorThreads
            ) 'Translating the user-owned main.dol' -StepId 'translate' `
                -TimeoutSeconds $translatorTimeoutSeconds

            if (-not [string]::IsNullOrWhiteSpace($TranslationFingerprint)) {
                [ordered]@{ SchemaVersion = 1; TranslationFingerprint = $TranslationFingerprint; DolSha256 = $pins.DolSha256 } |
                    ConvertTo-Json | Set-Content -LiteralPath $translationProvenance -Encoding UTF8
            }
        }

        Invoke-Checked $translator @('generate-data-init', '--project', $project) `
            'Generating local game data initialization' -StepId 'generate-data-init' `
            -TimeoutSeconds $translatorTimeoutSeconds

        Invoke-Checked $translator @(
            'emit-build-shards', '--project', $project, '--base-metadata', $metadata,
            '--base-functions-dir', $functions,
            '--native-source-dir', (Join-Path $Workspace 'runtime\src'), '--out', $shards
        ) 'Preparing local native build shards' -StepId 'emit-build-shards' `
            -TimeoutSeconds $translatorTimeoutSeconds

        # The build directory is kept whenever the exact toolchain that filled it is the one
        # installed now and its CMake cache still belongs to this workspace path. Object files are
        # only ever unsafe to reuse when the compiler/flags changed - source changes are what
        # CMake/Ninja exist to track, so a kept directory turns a small runtime or dependency
        # change into a recompile of just the affected objects instead of every shard.
        if (Test-Path -LiteralPath $build) {
            $keepNativeBuild = -not [string]::IsNullOrWhiteSpace($NativeToolchainFingerprint) -and
                (Get-RecordedFingerprint $toolchainProvenance 'NativeToolchainFingerprint') -eq $NativeToolchainFingerprint -and
                (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt') -PathType Leaf)
            if ($keepNativeBuild) {
                # A moved workspace invalidates every absolute path in the cache; CMake would hard
                # error rather than reconfigure. Move healing already deletes the directory for
                # portable installs, so this only fires for hand-moved trees.
                $expectedHome = (Join-Path $Workspace 'runtime')
                $cacheHome = Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') `
                    -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
                if ($null -eq $cacheHome -or
                    -not [string]::Equals([IO.Path]::GetFullPath($cacheHome.Matches[0].Groups[1].Value),
                        [IO.Path]::GetFullPath($expectedHome), [StringComparison]::OrdinalIgnoreCase)) {
                    $keepNativeBuild = $false
                }
            }
            if ($keepNativeBuild) {
                Write-Host 'OPUCBUILD: Reusing the incremental native build directory'
            } else {
                Write-Host 'OPUCBUILD: The native build cache does not belong to this toolchain; rebuilding from scratch'
                Remove-Item -LiteralPath $build -Recurse -Force
            }
        }
        $dependencies = Join-Path $Workspace 'Dependencies'
        $nativePrebuilt = Resolve-OpucNativePrebuiltDirectory $dependencies $toolchainBin `
            $aurora (Join-Path $Workspace 'runtime\third_party')
        if ($X86CpuProfile -eq 'v2') {
            Write-Host 'OPUCBUILD: This CPU does not support x86-64-v3; building for x86-64-v2'
        }
        # Always set, so a reused CMake cache can never keep an icon directory from an earlier run.
        $iconArgument = '-DOPUC_ICON_DIR='
        if (-not [string]::IsNullOrWhiteSpace($IconDirectory) -and
            (Test-Path -LiteralPath (Join-Path $IconDirectory "$productTarget.rc") -PathType Leaf)) {
            $iconArgument = "-DOPUC_ICON_DIR=$(ConvertTo-OpucCMakePath $IconDirectory)"
        } elseif (-not [string]::IsNullOrWhiteSpace($IconDirectory)) {
            Write-Host "OPUCBUILD: No product icon in $IconDirectory; the executable gets no icon"
        }
        $configure = Get-OpucNativeConfigureArguments `
            -SourceDirectory (Join-Path $Workspace 'runtime') -BuildDirectory $build `
            -Ninja $ninja -CCompiler $cc -CxxCompiler $cxx -ResourceCompiler $windres `
            -AuroraDirectory $aurora `
            -DependenciesDirectory $dependencies -NativePrebuiltDirectory $nativePrebuilt `
            -AdditionalArguments @("-DMKW_TRANSLATED_COMPILE_JOBS=$translatedJobs",
                "-DMKW_X86_CPU_PROFILE=$X86CpuProfile", $iconArgument)
        Invoke-Checked $cmake $configure 'Configuring the bundled native toolchain' -StepId 'configure-native'
        if (-not [string]::IsNullOrWhiteSpace($NativeToolchainFingerprint)) {
            [ordered]@{ SchemaVersion = 1; NativeToolchainFingerprint = $NativeToolchainFingerprint } |
                ConvertTo-Json | Set-Content -LiteralPath $toolchainProvenance -Encoding UTF8
        }
        Invoke-Checked $cmake @('--build', $build, '--target', $productTarget, '--parallel', $globalJobs) `
            'Compiling One Piece: Unlimited Recompilation Episode 1 locally' -StepId 'compile'

        # Hashed once here; the published product records the same digests.
        $dolSha = Get-OpucFileSha256 (Join-Path $assets 'main.dol')
        $compilerSha = Get-OpucFileSha256 (Join-Path $toolchainBin 'clang-22.exe')

        Reset-LocalDirectory $OutputDirectory
        $exe = Join-Path $build $productExecutable
        Assert-File $exe 'Locally compiled game executable'
        Copy-Item -LiteralPath $exe -Destination (Join-Path $OutputDirectory $productExecutable)
        # Every runtime DLL the product target copies beside the executable (Dawn, DXC, libc++,
        # libunwind, and SDL/zlib/libpng/sqlite3 when they are shared) plus the DSP coefficient ROM.
        Get-ChildItem -LiteralPath $build -Filter '*.dll' -File |
            Copy-Item -Destination $OutputDirectory
        $dspRom = Join-Path $build 'dsp_coef.bin'
        Assert-File $dspRom 'Wii DSP coefficient ROM beside the executable'
        Copy-Item -LiteralPath $dspRom -Destination (Join-Path $OutputDirectory 'dsp_coef.bin')
        $redist = Join-Path $Toolkit 'Redist'
        if (Test-Path -LiteralPath $redist -PathType Container) {
            Get-ChildItem -LiteralPath $redist -Filter '*.dll' -File |
                Copy-Item -Destination $OutputDirectory -Force
        }
        $provenance = [ordered]@{
            SchemaVersion = 1
            Profile = 'base'
            BuiltUtc = [DateTime]::UtcNow.ToString('O')
            DolSha256 = $dolSha
            Compiler = "llvm-mingw clang-22 sha256:$compilerSha"
        }
        $provenance | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'local-build.json') -Encoding UTF8
        Write-Host "OPUCBUILD:OUTPUT=$OutputDirectory"
    } finally { Pop-Location }
} finally {
    $env:PATH = $oldPath
    if ($null -ne $oldDotnet) { $env:DOTNET_ROOT = $oldDotnet }
}
