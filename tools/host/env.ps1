$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ToolchainBin = Join-Path $RepoRoot "third_party\toolchain_gcc_t-head_windows\bin"
$MakeBin      = Join-Path $RepoRoot "third_party\bouffalo_sdk\tools\make"
$NinjaBin     = Join-Path $RepoRoot "third_party\bouffalo_sdk\tools\ninja"
$CMakeBin     = Join-Path $RepoRoot "third_party\bouffalo_sdk\tools\cmake\bin"
$HostTools    = Join-Path $RepoRoot "tools\host"

$RequiredPaths = @(
    $ToolchainBin,
    $MakeBin,
    $NinjaBin,
    $CMakeBin,
    $HostTools
)

foreach ($PathEntry in $RequiredPaths) {
    if (-not (Test-Path $PathEntry)) {
        throw "Required path not found: $PathEntry"
    }
}

$env:Path = ($RequiredPaths -join ";") + ";" + $env:Path

Write-Host "DS5Dongle BL616 environment loaded."
Write-Host "Repository: $RepoRoot"

riscv64-unknown-elf-gcc --version | Select-Object -First 1
make --version | Select-Object -First 1
ninja --version
cmake --version | Select-Object -First 1
python3 --version
