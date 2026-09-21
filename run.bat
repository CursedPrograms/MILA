@echo off
REM Sets up (or reuses) a venv in .\venv, makes sure pygame/requests are
REM installed, then launches the MILA WiFi controller.
setlocal
cd /d "%~dp0"

if not exist "venv" (
  echo Creating venv...
  python -m venv venv || exit /b 1
)

call venv\Scripts\activate.bat || exit /b 1
pip install -q -r requirements.txt || exit /b 1

python scripts\mila_controller.py %*
