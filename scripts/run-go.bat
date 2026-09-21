@echo off
REM Fetches dependencies, builds and launches the Go MILA controller (needs Go).
REM Usage: run-go.bat [-host IP] [-port N]
setlocal
cd /d "%~dp0go"

go mod tidy || exit /b 1
go run . %*
