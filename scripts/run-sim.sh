#!/usr/bin/env bash
# Builds and runs the mock MILA robot (sim/MockMila.cs) for testing controllers
# without hardware. Needs the .NET Framework compiler that ships with Windows.
# Usage: ./run-sim.sh [port] [--any]     (--any binds all interfaces; Windows will ask to allow it)
set -e
cd "$(dirname "${BASH_SOURCE[0]}")/sim"

CSC="${WINDIR:-/c/Windows}/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
"$CSC" -nologo -out:MockMila.exe MockMila.cs
./MockMila.exe "$@"
