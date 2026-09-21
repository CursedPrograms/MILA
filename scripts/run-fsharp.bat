@echo off
REM Builds and launches the F# MILA controller (needs the .NET 8 SDK).
REM Usage: run-fsharp.bat [--host IP|auto] [--port N]
setlocal
cd /d "%~dp0"

dotnet run --project fsharp\MilaController.fsproj -c Release -- %*
