#!/usr/bin/env bash
# Builds the Fortran telemetry analyzer and runs it on a telemetry CSV
# (default: the newest export in ~/Documents/MILA-telemetry). Needs gfortran.
# Usage: ./run-fortran.sh [file.csv]
set -e

FILE=""
if [ -n "$1" ]; then FILE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; fi
cd "$(dirname "${BASH_SOURCE[0]}")/fortran"

gfortran -O2 -Wall -Wextra -std=f2008 -static telemetry_stats.f90 -o telemetry_stats.exe
[ -n "$FILE" ] || FILE="$(ls -t "$HOME/Documents/MILA-telemetry"/*.csv 2>/dev/null | head -1)"
[ -n "$FILE" ] || { echo "No telemetry CSV found. Press EXPORT CSV in a controller first, or pass a file." >&2; exit 1; }
./telemetry_stats.exe "$FILE"
