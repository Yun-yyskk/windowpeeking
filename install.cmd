@echo off
rem Install WindowPeeking where it can actually own a tray icon.
rem
rem Windows gives a program the integrity level of the folder it lives in, and
rem this repository folder makes everything launched out of it low integrity.
rem A low integrity process may not register a tray icon (and may not even write
rem into your profile), so the cure is to copy the exe somewhere ordinary and
rem start it from there. Double-click this file -- Explorer runs it at normal
rem integrity, which is what makes the copy (and the icon) possible.
setlocal
cd /d "%~dp0"

set "SRC=build\windowpeeking.exe"
if not exist "%SRC%" set "SRC=windowpeeking.exe"
if not exist "%SRC%" (
    echo [install] windowpeeking.exe not found here - run build.cmd first.
    pause
    exit /b 1
)

set "DEST=%LOCALAPPDATA%\Programs\WindowPeeking"
if not exist "%DEST%" mkdir "%DEST%" 2>nul
copy /y "%SRC%" "%DEST%\windowpeeking.exe" >nul
if errorlevel 1 (
    echo [install] could not copy to "%DEST%".
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$d='%DEST%'; $lnk=Join-Path ([Environment]::GetFolderPath('Desktop')) 'WindowPeeking.lnk';" ^
  "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($lnk);" ^
  "$s.TargetPath=Join-Path $d 'windowpeeking.exe'; $s.WorkingDirectory=$d; $s.IconLocation=$s.TargetPath;" ^
  "$s.Description='WindowPeeking'; $s.Save()" >nul 2>&1

echo [install] installed: %DEST%\windowpeeking.exe
echo [install] desktop shortcut: WindowPeeking.lnk
echo [install] starting it from there, so the tray icon works...
taskkill /IM windowpeeking.exe /F >nul 2>&1
start "" "%DEST%\windowpeeking.exe"
endlocal
