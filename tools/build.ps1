# One Piece: Unlimited Recompilation Episode 1: static recompilation pipeline.
#   powershell -File tools/build.ps1 [-Stage all|map|rebind|translate|configure|compile] [-Jobs N]
# Stages (each one reruns the ones after it with -Stage all):
#   map        projects/opuc1/MAP.txt from Ghidra names + Dolphin SDK signatures (tools/build-function-map.mjs)
#   rebind     re-point the vendored runtime's HLE bindings at OPUC addresses (tools/rebind-runtime.mjs)
#   translate  translator: translate-recursive, generate-data-init, emit-build-shards -> generated/
#   configure  product icon from the dump (tools/make-icon.mjs -> build/icon), then CMake + Ninja with LLVM-MinGW clang -> build/native
#   compile    ninja -> build/native/opuc1-recomp.exe (product target name comes from runtime/CMakeLists.txt)
# Inputs come from data/ at the repo root (a folder or a directory link, never committed): data/extract/DATA is the
# extracted disc, data/totaldb.dsy Dolphin's signature DB (or OPUC_DOLPHIN_DSY).
# Logs go to build/logs/<stage>.log. Exit code is non-zero on the first failing stage.
param(
    [ValidateSet('all', 'map', 'rebind', 'translate', 'configure', 'compile')] [string] $Stage = 'all',
    [int] $Jobs = 8,
    [string] $Target = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path -replace '\\', '/'
$data = "$root/data"
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
$llvm = $env:OPUC_LLVM_MINGW
if (-not $llvm) {
    $llvm = (Get-ChildItem "$env:LOCALAPPDATA/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW.UCRT*/llvm-mingw-*/bin" -Directory |
        Sort-Object Name -Descending | Select-Object -First 1).FullName
}
$llvm = $llvm -replace '\\', '/'
$env:Path = ($llvm -replace '/', '\') + ';' + $env:Path
$dolphinDb = if ($env:OPUC_DOLPHIN_DSY) { $env:OPUC_DOLPHIN_DSY } else { "$data/totaldb.dsy" }
$translator = "$root/translator/src/Translator.Cli/bin/Release/net11.0/Translator.Cli.dll"
$project = "$root/projects/opuc1/recomp.yml"
$logs = "$root/build/logs"
New-Item -ItemType Directory -Force $logs | Out-Null

function Invoke-Step([string] $name, [scriptblock] $body) {
    $log = "$logs/$name.log"
    Write-Host "[opuc-recomp] $name ..."
    $sw = [Diagnostics.Stopwatch]::StartNew()
    # Windows PowerShell turns native stderr lines into terminating errors under 'Stop'; keep going and judge by
    # the exit code. Logs are UTF-8 so grep can read them.
    $ErrorActionPreference = 'Continue'
    & $body 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Write-Host ("[opuc-recomp] {0} exit={1} ({2:n1}s) log={3}" -f $name, $code, $sw.Elapsed.TotalSeconds, $log)
    if ($code -ne 0) { Get-Content $log -Tail 30; exit $code }
}

$stages = @('map', 'rebind', 'translate', 'configure', 'compile')
$from = if ($Stage -eq 'all') { 0 } else { [array]::IndexOf($stages, $Stage) }
$run = if ($Stage -eq 'all') { $stages } else { @($Stage) }

foreach ($s in $run) {
    switch ($s) {
        'map' {
            Invoke-Step 'dsy-match' { node "$root/tools/dsy-match.mjs" "$data/extract/DATA/sys/main.dol" $dolphinDb "$root/projects/opuc1/ghidra_functions.txt" "$root/projects/opuc1/dsy_matches.txt" }
            Invoke-Step 'map' { node "$root/tools/build-function-map.mjs" "$data/extract/DATA/sys/main.dol" "$root/projects/opuc1/ghidra_functions.txt" "$root/projects/opuc1/dsy_matches.txt" "$root/projects/opuc1/MAP.txt" "$root/projects/opuc1/map_report.json" }
        }
        'rebind' { Invoke-Step 'rebind' { node "$root/tools/rebind-runtime.mjs" $root } }
        'translate' {
            if (-not (Test-Path $translator)) { Invoke-Step 'translator-build' { dotnet build "$root/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release } }
            Push-Location $root
            try {
                Invoke-Step 'translate' { dotnet $translator translate-recursive 0x8000403C --project $project --outdir generated/functions --output-metadata generated/base_translation_output.json --production-source-bundle generated/base_translation_sources.bin --no-function-files --prune-stale --threads 12 }
                Invoke-Step 'datainit' { dotnet $translator generate-data-init --project $project }
                Invoke-Step 'shards' { dotnet $translator emit-build-shards --project $project --base-metadata generated/base_translation_output.json --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards }
            } finally { Pop-Location }
        }
        'configure' {
            # Product icon from the user's dump (save icon in memcard/banner_ep1.en.tpl) -> build/icon
            Invoke-Step 'icon' { node "$root/tools/make-icon.mjs" --dvd "$data/extract/DATA" --out "$root/build/icon" }
            Invoke-Step 'configure' {
                cmake -S "$root/runtime" -B "$root/build/native" -G Ninja -DCMAKE_BUILD_TYPE=Release `
                    "-DCMAKE_C_COMPILER=$llvm/clang.exe" "-DCMAKE_CXX_COMPILER=$llvm/clang++.exe" "-DCMAKE_RC_COMPILER=$llvm/llvm-windres.exe" `
                    "-DMKW_AURORA_DIR=$root/third_party/aurora" "-DMKW_TRANSLATED_COMPILE_JOBS=$Jobs" "-DOPUC_ICON_DIR=$root/build/icon"
            }
        }
        'compile' {
            $targetArgs = if ($Target) { @('--target', $Target) } else { @() }
            Invoke-Step 'compile' { cmake --build "$root/build/native" --parallel $Jobs @targetArgs }
        }
    }
}
