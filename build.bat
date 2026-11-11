@echo off
rem Builds B2KShake.ofx.bundle with MSVC. Needs the "Desktop development with C++"
rem workload and the OpenFX headers in .\openfx\include
setlocal
cd /d "%~dp0"
rem fetch the OpenFX headers (BSD-3-Clause) on first build
if not exist openfx\include\ofxImageEffect.h (
    git -c core.longpaths=true clone --depth 1 --filter=blob:none --sparse https://github.com/AcademySoftwareFoundation/openfx.git openfx || exit /b 1
    git -C openfx -c core.longpaths=true sparse-checkout set include || exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo MSVC C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set OUT=B2KShake.ofx.bundle\Contents\Win64
if not exist "%OUT%" mkdir "%OUT%"
if not exist build mkdir build
cl /nologo /LD /O2 /fp:fast /std:c++17 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /I openfx\include src\B2KShake.cpp /Fobuild\ /Fe"%OUT%\B2KShake.ofx" /link /NOLOGO || exit /b 1
del /q "%OUT%\B2KShake.exp" "%OUT%\B2KShake.lib" 2>nul
echo Built %OUT%\B2KShake.ofx
