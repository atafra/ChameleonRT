@echo off
REM One-shot launcher for the OIDN sync-mode benchmark PowerShell script.
REM Bypasses execution policy so it runs on a fresh system by double-click or CLI.
REM Any arguments are forwarded to run_oidn_benchmarks.ps1, e.g.:
REM     run_oidn_benchmarks.cmd -Backends vulkan -InstallDeps
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_oidn_benchmarks.ps1" %*
