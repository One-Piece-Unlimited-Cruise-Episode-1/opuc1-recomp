[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$PortableToolsDirectory = 'launcher/artifacts/portable-tools',
    [string]$DependencySourceDirectory = 'launcher/artifacts/dependencies',
    [string]$VcRuntimeDirectory,
    [string]$ToolkitReleaseTag = $env:GITHUB_REF_NAME
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$arguments = @{
    PortableToolsDirectory = $PortableToolsDirectory
    DependencySourceDirectory = $DependencySourceDirectory
    ToolkitReleaseTag = $ToolkitReleaseTag
}
if (-not [string]::IsNullOrWhiteSpace($VcRuntimeDirectory)) {
    $arguments.VcRuntimeDirectory = $VcRuntimeDirectory
}

& (Join-Path $PSScriptRoot 'Build-Installer.ps1') @arguments
if ($LASTEXITCODE -ne 0) { throw "Build-Installer.ps1 failed with exit code $LASTEXITCODE." }

Write-Host 'The release artifact is ready. It contains no translated game executable, game image, main.dol, or other game data.'
