param(
    [string]$Chip = "bl616",
    [string]$Board = "bl616dk",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "env.ps1")

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppDir = Join-Path $RepoRoot "ports\bl616"

if (-not (Test-Path $AppDir)) {
    throw "BL616 application directory not found: $AppDir"
}

if ($Clean) {
    & make -C $AppDir clean

    if ($LASTEXITCODE -ne 0) {
        throw "Clean failed with exit code $LASTEXITCODE"
    }
}

& make -C $AppDir "CHIP=$Chip" "BOARD=$Board"

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "BL616 build completed successfully."
Write-Host "Output: $AppDir\build\build_out"
