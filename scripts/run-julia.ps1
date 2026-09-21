# Installs missing Julia packages (first run precompiles GLMakie, slow) and
# launches the Julia MILA controller.
# Usage: .\run-julia.ps1 [--host IP] [--port N]
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$pkgs = @'
using Pkg; for p in ["GLMakie", "HTTP", "JSON"]; Base.find_package(p) === nothing && Pkg.add(p); end
'@
julia -e $pkgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
julia mila_controller.jl @args
