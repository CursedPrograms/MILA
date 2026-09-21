# Builds and runs the mock MILA robot (sim\MockMila.cs) for testing controllers
# without hardware. Needs the .NET Framework compiler that ships with Windows.
# Usage: .\run-sim.ps1 [port] [--any]   (--any binds all interfaces; Windows will ask to allow it)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "sim")

& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" -nologo -out:MockMila.exe MockMila.cs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\MockMila.exe @args
