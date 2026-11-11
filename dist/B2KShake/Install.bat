@echo off
rem B2K Shake installer: copies the OFX plugin into the shared OFX folder.
setlocal
title B2K Shake - Install

rem --- needs admin rights (Program Files); re-launch elevated if necessary
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0B2KShake.ofx.bundle"
set "DST=%CommonProgramW6432%\OFX\Plugins"
if "%CommonProgramW6432%"=="" set "DST=%CommonProgramFiles%\OFX\Plugins"

if not exist "%SRC%\Contents\Win64\B2KShake.ofx" (
    echo ERROR: B2KShake.ofx.bundle not found next to this file.
    echo Please extract the whole ZIP first, then run Install.bat from the extracted folder.
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq vegas*" 2>nul | find /i "vegas" >nul
if not errorlevel 1 (
    echo VEGAS is still running. Please close VEGAS and run Install.bat again.
    pause
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"
rem remove an old version (also the earlier name ShakeFX)
if exist "%DST%\ShakeFX.ofx.bundle" rmdir /s /q "%DST%\ShakeFX.ofx.bundle"
if exist "%DST%\B2KShake.ofx.bundle" rmdir /s /q "%DST%\B2KShake.ofx.bundle"

xcopy "%SRC%" "%DST%\B2KShake.ofx.bundle\" /E /I /Y /Q >nul
if errorlevel 1 (
    echo ERROR: copying failed.
    pause
    exit /b 1
)

echo.
echo Installed to: %DST%\B2KShake.ofx.bundle
echo Start VEGAS. You will find:
echo   Video FX     ^> OFX ^> B2K ^> B2K Shake / B2K Dissolve Shake
echo   Transitions  ^> OFX ^> B2K ^> B2K Dissolve Shake
echo.
pause
