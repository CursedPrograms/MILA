#!/usr/bin/env bash
# Builds the driving-training dataset (sensors -> what the human did) from telemetry exports.
# Needs gfortran. With no arguments it uses every ~/Documents/MILA-telemetry/telemetry_*.csv and writes
# ~/Documents/MILA-telemetry/datasets/drive_policy.csv.
# Usage: ./run-dataset.sh [out.csv [telemetry.csv ...]]
set -e

DIR="$HOME/Documents/MILA-telemetry"
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }

OUT=""
INPUTS=()
if [ $# -ge 1 ]; then OUT="$(abs "$1")"; shift; fi
for f in "$@"; do INPUTS+=("$(abs "$f")"); done
cd "$(dirname "${BASH_SOURCE[0]}")/fortran"

gfortran -O2 -Wall -Wextra -std=f2008 -static telemetry_common.f90 make_dataset.f90 -o make_dataset.exe
if [ -z "$OUT" ]; then mkdir -p "$DIR/datasets"; OUT="$DIR/datasets/drive_policy.csv"; fi
if [ ${#INPUTS[@]} -eq 0 ]; then
  shopt -s nullglob
  INPUTS=("$DIR"/telemetry_*.csv)
fi
[ ${#INPUTS[@]} -gt 0 ] || { echo "No telemetry exports found. Press EXPORT CSV in a controller after driving, or pass files." >&2; exit 1; }
./make_dataset.exe "$OUT" "${INPUTS[@]}"
