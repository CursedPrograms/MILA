@echo off
REM Builds and runs the mock MILA robot (sim\MockMila.cs) for testing controllers
REM without hardware. Needs the .NET Framework compiler that ships with Windows.
REM Usage: run-sim.bat [port] [--any]   (--any binds all interfaces; Windows will ask to allow it)
setlocal
cd /d "%~dp0sim"

"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" -nologo -out:MockMila.exe MockMila.cs || exit /b 1
.\MockMila.exe %*
