# Builds the Fortran telemetry analyzer and runs it on a telemetry CSV
# (default: the newest export in Documents\MILA-telemetry). Needs gfortran.
# Usage: .\run-fortran.ps1 [file.csv]
$ErrorActionPreference = "Stop"

$file = $null
if ($args.Count -gt 0) { $file = (Resolve-Path $args[0]).Path }
Set-Location (Join-Path $PSScriptRoot "fortran")

gfortran -O2 -Wall -Wextra -std=f2008 -static telemetry_common.f90 telemetry_stats.f90 -o telemetry_stats.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $file) {
    $latest = Get-ChildItem "$env:USERPROFILE\Documents\MILA-telemetry\*.csv" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($latest) { $file = $latest.FullName }
}
if (-not $file) {
    Write-Error "No telemetry CSV found. Press EXPORT CSV in a controller first, or pass a file."
    exit 1
}
& .\telemetry_stats.exe $file
