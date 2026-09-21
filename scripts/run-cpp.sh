#!/usr/bin/env bash
# Builds the C++ MILA controller (Windows/MinGW) and launches it.
# Usage: ./run-cpp.sh [--host IP] [--port N]
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

g++ -std=c++17 -O2 -mwindows -static mila_controller.cpp -o mila_controller.exe -lwinhttp -lgdi32 -luser32 -lshell32
./mila_controller.exe "$@"
