# Builds and launches the C# MILA controller (needs the .NET SDK).
# Usage: .\run-csharp.ps1 [--host IP|auto] [--port N]
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

dotnet run --project csharp\MilaController.csproj -c Release -- @args
