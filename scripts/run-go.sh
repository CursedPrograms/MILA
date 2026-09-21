#!/usr/bin/env bash
# Fetches dependencies, builds and launches the Go MILA controller (needs Go).
# Usage: ./run-go.sh [--host IP] [--port N]
set -e
cd "$(dirname "${BASH_SOURCE[0]}")/go"

go mod tidy
go run . "$@"
