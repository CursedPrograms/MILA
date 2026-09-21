@echo off
REM Builds the C++ MILA controller (MinGW g++) and launches it.
REM Usage: run-cpp.bat [--host IP|auto] [--port N]
setlocal
cd /d "%~dp0"

g++ -std=c++17 -O2 -mwindows -static mila_controller.cpp -o mila_controller.exe -lwinhttp -lgdi32 -luser32 -lshell32 -lws2_32 || exit /b 1
.\mila_controller.exe %*
