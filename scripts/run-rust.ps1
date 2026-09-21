# Builds and launches the Rust MILA controller (needs cargo).
# Usage: .\run-rust.ps1 [--host IP] [--port N]
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

cargo run --release --manifest-path rust\Cargo.toml -- @args
