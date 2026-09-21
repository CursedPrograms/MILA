#!/usr/bin/env bash
# Installs missing Julia packages (first run precompiles GLMakie, slow) and
# launches the Julia MILA controller.
# Usage: ./run-julia.sh [--host IP] [--port N]
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

julia -e 'using Pkg; for p in ["GLMakie", "HTTP", "JSON"]; Base.find_package(p) === nothing && Pkg.add(p); end'
julia mila_controller.jl "$@"
