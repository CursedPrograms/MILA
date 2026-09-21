# Builds and launches the F# MILA controller (needs the .NET 8 SDK).
# Usage: .\run-fsharp.ps1 [--host IP|auto] [--port N]
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

dotnet run --project fsharp\MilaController.fsproj -c Release -- @args
