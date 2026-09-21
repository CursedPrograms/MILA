#!/usr/bin/env bash
# Builds and launches the F# MILA controller (needs the .NET 8 SDK, Windows).
# Usage: ./run-fsharp.sh [--host IP|auto] [--port N]
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

dotnet run --project fsharp/MilaController.fsproj -c Release -- "$@"
