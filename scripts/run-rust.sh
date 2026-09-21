#!/usr/bin/env bash
# Builds and launches the Rust MILA controller (needs cargo).
# Usage: ./run-rust.sh [--host IP] [--port N]
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

cargo run --release --manifest-path rust/Cargo.toml -- "$@"
