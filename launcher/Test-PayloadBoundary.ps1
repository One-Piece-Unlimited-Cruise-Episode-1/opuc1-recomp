[CmdletBinding()]
param([Parameter(Mandatory)] [string]$PayloadRoot)

# Fails the release build when the installer payload carries anything derived from the game: a
# translated executable, the user's disc or its files, translator output, or the product icon made
# from the save banner. The payload ships tools and sources only; the user's machine produces the rest.

$ErrorActionPreference = 'Stop'
$PayloadRoot = [IO.Path]::GetFullPath($PayloadRoot)
if (-not (Test-Path -LiteralPath $PayloadRoot -PathType Container)) { throw "Payload does not exist: $PayloadRoot" }

$forbiddenNames = @(
    'opuc1-recomp.exe', 'main.dol', 'boot.bin', 'bi2.bin', 'fst.bin', 'apploader.img',
    'base_translation_output.json', 'base_translation_sources.bin',
    'data_sections_init.cpp', 'data_sections_init_blobs.S', 'RuntimeConfig.h',
    'generated_func_aliases.cpp', 'translated_function_decls.h', 'shards.cmake',
    'opuc1_recomp.ico', 'opuc1_recomp.png', 'opuc1_recomp_48.png', 'opuc1_recomp.rc',
    'SYSCONF'
)
# Disc images and game executables, forbidden everywhere (programs/Recomp/.gitignore keeps the
# same list out of git).
$forbiddenExtensions = @('.iso', '.gcm', '.gcz', '.ciso', '.wbfs', '.wia', '.rvz', '.nkit', '.wad', '.dol', '.rel')
# Game asset formats. Third-party tool and dependency trees may use the same extensions for
# unrelated files (a .tpl template, for instance), so these apply to everything else.
$forbiddenGameAssetExtensions = @('.aar', '.thp', '.tpl', '.brsar', '.brstm', '.dsy')
# Matched anywhere in the payload-relative path.
$forbiddenSegments = @(
    '\BuildWorkspace\generated\', '\BuildWorkspace\native-build\', '\BuildWorkspace\runtime\build\',
    '\BuildWorkspace\build\', '\BuildWorkspace\Assets\', '\GameAssets\'
)
# Matched only at the payload root: the installed product directory.
$forbiddenRootDirectories = @('\Game\')
$violations = [Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $PayloadRoot -Recurse -File -Force) {
    $rootPrefix = $PayloadRoot.TrimEnd('\') + '\'
    if (-not $file.FullName.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Payload enumeration escaped its root: $($file.FullName)"
    }
    $relative = '\' + $file.FullName.Substring($rootPrefix.Length).Replace('/', '\')
    $underCompiler = $relative.StartsWith('\Toolkit\llvm-mingw\', [StringComparison]::OrdinalIgnoreCase)
    $underPinnedDependency = $relative.StartsWith('\BuildWorkspace\Dependencies\', [StringComparison]::OrdinalIgnoreCase)
    $extension = $file.Extension.ToLowerInvariant()
    if ($forbiddenNames -contains $file.Name -or $forbiddenExtensions -contains $extension) {
        $violations.Add($relative)
        continue
    }
    if (-not $underCompiler -and -not $relative.StartsWith('\Toolkit\', [StringComparison]::OrdinalIgnoreCase) -and
        -not $underPinnedDependency -and $forbiddenGameAssetExtensions -contains $extension) {
        $violations.Add("$relative (game asset format)")
        continue
    }
    if ($forbiddenRootDirectories | Where-Object { $relative.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }) {
        $violations.Add($relative)
        continue
    }
    if ($forbiddenSegments | Where-Object { $relative.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 }) {
        $violations.Add($relative)
        continue
    }
    if (-not $underCompiler -and -not $underPinnedDependency -and $file.Extension -in @('.obj','.o','.lib','.a')) {
        $violations.Add("$relative (precompiled object/library outside the licensed compiler payload)")
    }
}
if ($violations.Count -gt 0) {
    throw "Release payload contains forbidden game-derived or precompiled artifacts:`n$($violations -join "`n")"
}
Write-Host 'Payload boundary audit passed: no translated game executable, game file, generated shard, or generated data was packaged.'
