@echo off
setlocal
rem TelemetryMon packaging launcher: runs package.ps1 via PowerShell 7.
rem Args are passed through, e.g.  package.bat -Preset ninja -SkipBuild

where pwsh >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] PowerShell 7 ^(pwsh^) is required but was not found in PATH.
    echo         Install it with:  winget install Microsoft.PowerShell
    echo         or download from: https://aka.ms/powershell
    exit /b 1
)

pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" %*
