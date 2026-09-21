#!/usr/bin/env bash
# Builds and launches the C# MILA controller (needs the .NET SDK, Windows).
# Usage: ./run-csharp.sh [--host IP] [--port N]
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

dotnet run --project csharp/MilaController.csproj -c Release -- "$@"
