@echo off
rem Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
rem Double-click to compile Scenario for your Unreal version and install it into a project.
rem Needs Visual Studio with the "Game development with C++" workload. See README.md.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\Install-Scenario.ps1" %*
set "status=%ERRORLEVEL%"
rem Started by a double-click (no arguments): keep the window open to read the result.
if "%~1"=="" pause
exit /b %status%
