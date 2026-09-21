# Builds the driving-training dataset (sensors -> what the human did) from telemetry exports.
# Needs gfortran. With no arguments it uses every Documents\MILA-telemetry\telemetry_*.csv and writes
# Documents\MILA-telemetry\datasets\drive_policy.csv.
# Usage: .\run-dataset.ps1 [out.csv [telemetry.csv ...]]
$ErrorActionPreference = "Stop"

$dir = Join-Path $env:USERPROFILE "Documents\MILA-telemetry"
$full = { param($p) $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathForOutput($p) }
$out = $null
$inputs = @()
if ($args.Count -ge 1) {
    $out = & $full $args[0]
    $inputs = @($args | Select-Object -Skip 1 | ForEach-Object { & $full $_ })
}
Set-Location (Join-Path $PSScriptRoot "fortran")

gfortran -O2 -Wall -Wextra -std=f2008 -static telemetry_common.f90 make_dataset.f90 -o make_dataset.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $out) {
    New-Item -ItemType Directory -Force (Join-Path $dir "datasets") | Out-Null
    $out = Join-Path $dir "datasets\drive_policy.csv"
}
if ($inputs.Count -eq 0) {
    $inputs = @(Get-ChildItem "$dir\telemetry_*.csv" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}
if ($inputs.Count -eq 0) {
    Write-Error "No telemetry exports found. Press EXPORT CSV in a controller after driving, or pass files."
    exit 1
}
& .\make_dataset.exe $out @inputs
