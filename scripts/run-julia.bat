@echo off
REM Installs missing Julia packages (first run precompiles GLMakie, slow) and
REM launches the Julia MILA controller.
REM Usage: run-julia.bat [--host IP] [--port N]
setlocal
cd /d "%~dp0"

julia -e "using Pkg; for p in [\"GLMakie\", \"HTTP\", \"JSON\"]; Base.find_package(p) === nothing && Pkg.add(p); end" || exit /b 1
julia mila_controller.jl %*
