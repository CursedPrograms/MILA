# Builds the C++ MILA controller (MinGW g++) and launches it.
# Usage: .\run-cpp.ps1 [--host IP|auto] [--port N]
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

g++ -std=c++17 -O2 -mwindows -static mila_controller.cpp -o mila_controller.exe -lwinhttp -lgdi32 -luser32 -lshell32 -lws2_32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\mila_controller.exe @args
