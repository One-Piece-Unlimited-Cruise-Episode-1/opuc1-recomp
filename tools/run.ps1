# Runs the recompiled OPUC1 executable for a bounded time and keeps its log.
#   powershell -File tools/run.ps1 [-Seconds 20] [-Headless] [-Shot] [-ShotAt '10,14'] [-Exe <path>] [-ConfigExtra <toml>]
# Uses the portable layout of the runtime: build/native/portable.txt marks the root, so Config.toml, NAND,
# cache and logs live in build/native/UserData. The disc is served from the extracted tree data/extract/DATA at the repo root
# (paths.dvd_root); NAND saves go to build/native/UserData/nand. Output: build/logs/run.log.
param(
    [int] $Seconds = 20,
    [switch] $Headless,
    [switch] $Shot,          # capture the game window to build/logs/run.png right before the timeout
    [string] $ShotAt = '',   # extra captures at these seconds, e.g. '10,14.5,20' -> build/logs/run_<sec>.png
    [string] $Exe = '',
    [string] $ConfigExtra = ''   # TOML appended to the generated Config.toml, e.g. "[game]`nfishing_pc_guide = false"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$data = Join-Path $root 'data'   # folder or directory link, never committed
$native = Join-Path $root 'build/native'
if (-not $Exe) {
    $Exe = Get-ChildItem $native -Filter *.exe | Where-Object { $_.Name -notmatch 'test|probe' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Exe -or -not (Test-Path $Exe)) { Write-Error "no game executable under $native (run build.ps1 -Stage compile)"; exit 2 }

# One run at a time: two processes share UserData/nand and build/logs, and the second one gets the game's
# "error occurred during the process of reading from or writing to the Wii System Memory" screen.
$exeName = [IO.Path]::GetFileNameWithoutExtension($Exe)
$running = Get-Process $exeName -ErrorAction SilentlyContinue
if ($running) { Write-Error "$exeName is already running (pid $($running.Id -join ',')); another run owns UserData/nand and build/logs"; exit 3 }

$userData = Join-Path $native 'UserData'
New-Item -ItemType Directory -Force $userData, (Join-Path $userData 'nand') | Out-Null
Set-Content -Encoding ascii (Join-Path $native 'portable.txt') 'One Piece: Unlimited Recompilation Episode 1 portable root (tools/run.ps1)'
# OPUC's SC library runs translated and reads /shared2/sys/SYSCONF during OSInit (SCInit 802bca5c). Boot also works
# without it (SCCheckStatus is HLE'd; checked 2026-11-23). Seed it from OPUC_SYSCONF or the local
# Dolphin NAND until the runtime can
# generate one (STATUS.md).
$sysconf = Join-Path $userData 'nand/shared2/sys/SYSCONF'
if (-not (Test-Path $sysconf)) {
    $seed = if ($env:OPUC_SYSCONF) { $env:OPUC_SYSCONF } else { Join-Path $data 'dolphin-user/Wii/shared2/sys/SYSCONF' }
    if (Test-Path $seed) {
        New-Item -ItemType Directory -Force (Split-Path $sysconf) | Out-Null
        Copy-Item $seed $sysconf
        Write-Host "[opuc-run] seeded NAND SYSCONF from $seed"
    } else { Write-Warning "no SYSCONF to seed (set OPUC_SYSCONF); boot continues without one" }
}
$dvd = (Join-Path $data 'extract/DATA') -replace '\\', '/'
$nand = (Join-Path $userData 'nand') -replace '\\', '/'
@"
# Written by tools/run.ps1
[paths]
dvd_root = "$dvd"
nand_root = "$nand"
"@ + $(if ($ConfigExtra) { "`n" + $ConfigExtra + "`n" } else { '' }) | Set-Content -Encoding utf8 (Join-Path $userData 'Config.toml')

$logs = Join-Path $root 'build/logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$out = Join-Path $logs 'run.stdout.log'
$err = Join-Path $logs 'run.stderr.log'
$runArgs = @()
if ($Headless) { $runArgs += '--headless' }
$llvm = (Get-ChildItem "$env:LOCALAPPDATA/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW.UCRT*/llvm-mingw-*/bin" -Directory |
    Sort-Object Name -Descending | Select-Object -First 1).FullName
if ($llvm) { $env:Path = "$llvm;" + $env:Path }   # libc++/libunwind DLLs when the exe is not statically linked

Write-Host "[opuc-run] $Exe $runArgs (timeout ${Seconds}s)"
$sw = [Diagnostics.Stopwatch]::StartNew()
$startArgs = @{ FilePath = $Exe; WorkingDirectory = $native; PassThru = $true; NoNewWindow = $true
    RedirectStandardOutput = $out; RedirectStandardError = $err }
if ($runArgs.Count) { $startArgs.ArgumentList = $runArgs }   # Windows PowerShell rejects an empty -ArgumentList
$p = Start-Process @startArgs
$shotPath = Join-Path $logs 'run.png'
function Save-Shot([string] $path) {
    Add-Type -AssemblyName System.Drawing
    if (-not ('OpucShot' -as [type])) {
        Add-Type @'
using System; using System.Runtime.InteropServices;
public static class OpucShot {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
    }
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Warning "no main window to capture"; return }
    $r = New-Object OpucShot+RECT
    [void][OpucShot]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [void][OpucShot]::PrintWindow($h, $dc, 2)   # PW_RENDERFULLCONTENT: works for D3D windows
    $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "[opuc-run] screenshot $path"
}
if ($ShotAt) {
    Get-ChildItem $logs -Filter 'run_*.png' | Remove-Item -ErrorAction SilentlyContinue
    $inv = [Globalization.CultureInfo]::InvariantCulture
    foreach ($t in ($ShotAt -split ',' | ForEach-Object { [double]::Parse($_.Trim(), $inv) } | Sort-Object)) {
        $wait = [int](($t - $sw.Elapsed.TotalSeconds) * 1000)
        if ($wait -gt 0 -and $p.WaitForExit($wait)) { break }
        # invariant culture: run_23.5.png, not run_23,5.png on a German locale
        Save-Shot (Join-Path $logs ([string]::Format($inv, 'run_{0}.png', $t)))
    }
}
if ($Shot) {
    Remove-Item $shotPath -ErrorAction SilentlyContinue
    $exited = $p.WaitForExit([int][Math]::Max(1000, ($Seconds - 2 - $sw.Elapsed.TotalSeconds) * 1000))
    if (-not $exited) {
        Save-Shot $shotPath
        $exited = $p.WaitForExit(2000)
    }
} else {
    $exited = $p.WaitForExit([int][Math]::Max(0, ($Seconds - $sw.Elapsed.TotalSeconds) * 1000))
}
if (-not $exited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit() }
$code = if ($exited) { $p.ExitCode } else { 'timeout' }
$log = Join-Path $logs 'run.log'
@("# run.ps1 $(Get-Date -Format s) exe=$Exe seconds=$Seconds exit=$code elapsed=$([int]$sw.Elapsed.TotalSeconds)s",
  '## stdout') + (Get-Content $out -ErrorAction SilentlyContinue) + @('## stderr') + (Get-Content $err -ErrorAction SilentlyContinue) |
    Set-Content -Encoding utf8 $log
Write-Host "[opuc-run] exit=$code log=$log"
Get-Content $log -Tail 40
