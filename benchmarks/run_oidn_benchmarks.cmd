@echo off
REM One-shot launcher for the OIDN sync-mode benchmark PowerShell script.
REM Bypasses execution policy so it runs on a fresh system by double-click or CLI.
REM Any arguments are forwarded to run_oidn_benchmarks.ps1, e.g.:
REM     run_oidn_benchmarks.cmd -Backends vulkan -InstallDeps
set "SCRIPT=%~dp0run_oidn_benchmarks.ps1"

if not exist "%SCRIPT%" (
	echo ERROR: Script not found: "%SCRIPT%"
	exit /b 1
)

where powershell >nul 2>nul
if %ERRORLEVEL% EQU 0 (
	powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
	exit /b %ERRORLEVEL%
)

where pwsh >nul 2>nul
if %ERRORLEVEL% EQU 0 (
	pwsh -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
	exit /b %ERRORLEVEL%
)

echo ERROR: Neither "powershell" nor "pwsh" was found on PATH.
echo Install Windows PowerShell or PowerShell 7, then retry.
exit /b 1
