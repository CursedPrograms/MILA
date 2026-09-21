@echo off
REM Builds the driving-training dataset (sensors -> what the human did) from telemetry exports.
REM Needs gfortran. With no arguments it uses every %USERPROFILE%\Documents\MILA-telemetry\telemetry_*.csv
REM and writes %USERPROFILE%\Documents\MILA-telemetry\datasets\drive_policy.csv.
REM Usage: run-dataset.bat [out.csv [telemetry.csv ...]]
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "DIR=%USERPROFILE%\Documents\MILA-telemetry"

REM resolve arguments to full paths before changing directory
set "ARGS="
:next
if "%~1"=="" goto run
set ARGS=!ARGS! "%~f1"
shift
goto next

:run
cd /d "%HERE%fortran"
gfortran -O2 -Wall -Wextra -std=f2008 -static telemetry_common.f90 make_dataset.f90 -o make_dataset.exe || exit /b 1
if defined ARGS (
  .\make_dataset.exe !ARGS!
  exit /b !ERRORLEVEL!
)
if not exist "%DIR%\datasets" mkdir "%DIR%\datasets"
set "FILES="
for %%F in ("%DIR%\telemetry_*.csv") do set FILES=!FILES! "%%F"
if not defined FILES (
  echo No telemetry exports found. Press EXPORT CSV in a controller after driving, or pass files.
  exit /b 1
)
.\make_dataset.exe "%DIR%\datasets\drive_policy.csv" !FILES!
