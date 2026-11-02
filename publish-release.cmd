@echo off
rem Double-click to publish the GitHub release for this build (see publish-release.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish-release.ps1" %*
if errorlevel 1 pause
pause