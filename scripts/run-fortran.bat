@echo off
REM Builds the Fortran telemetry analyzer and runs it on a telemetry CSV
REM (default: the newest export in %USERPROFILE%\Documents\MILA-telemetry). Needs gfortran.
REM Usage: run-fortran.bat [file.csv]
setlocal
set "FILE=%~1"
if defined FILE for %%I in ("%FILE%") do set "FILE=%%~fI"
cd /d "%~dp0fortran"

gfortran -O2 -Wall -Wextra -std=f2008 -static telemetry_common.f90 telemetry_stats.f90 -o telemetry_stats.exe || exit /b 1
if not defined FILE for /f "delims=" %%F in ('dir /b /o-d "%USERPROFILE%\Documents\MILA-telemetry\*.csv" 2^>nul') do if not defined FILE set "FILE=%USERPROFILE%\Documents\MILA-telemetry\%%F"
if not defined FILE (
  echo No telemetry CSV found. Press EXPORT CSV in a controller first, or pass a file.
  exit /b 1
)
.\telemetry_stats.exe "%FILE%"
