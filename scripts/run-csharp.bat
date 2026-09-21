@echo off
REM Builds and launches the C# MILA controller (needs the .NET SDK).
REM Usage: run-csharp.bat [--host IP] [--port N]
setlocal
cd /d "%~dp0"

dotnet run --project csharp\MilaController.csproj -c Release -- %*
