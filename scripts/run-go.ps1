# Fetches dependencies, builds and launches the Go MILA controller (needs Go).
# Usage: .\run-go.ps1 [--host IP] [--port N]
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "go")

go mod tidy
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
go run . @args
