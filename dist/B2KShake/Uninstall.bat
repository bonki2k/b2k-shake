@echo off
rem B2K Shake uninstaller: removes the OFX plugin from the shared OFX folder.
setlocal
title B2K Shake - Uninstall

net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "DST=%CommonProgramW6432%\OFX\Plugins"
if "%CommonProgramW6432%"=="" set "DST=%CommonProgramFiles%\OFX\Plugins"

tasklist /FI "IMAGENAME eq vegas*" 2>nul | find /i "vegas" >nul
if not errorlevel 1 (
    echo VEGAS is still running. Please close VEGAS and run Uninstall.bat again.
    pause
    exit /b 1
)

if exist "%DST%\B2KShake.ofx.bundle" rmdir /s /q "%DST%\B2KShake.ofx.bundle"
if exist "%DST%\ShakeFX.ofx.bundle" rmdir /s /q "%DST%\ShakeFX.ofx.bundle"
echo B2K Shake removed.
pause
