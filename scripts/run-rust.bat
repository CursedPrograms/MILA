@echo off
REM Builds and launches the Rust MILA controller (needs cargo).
REM Usage: run-rust.bat [--host IP] [--port N]
setlocal
cd /d "%~dp0"

cargo run --release --manifest-path rust\Cargo.toml -- %*
