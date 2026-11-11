@echo off
rem Release build of bench.exe with MSVC (x64).
rem   build.bat          build bench.exe
rem   build.bat version  print the compiler banner and the flags (used by run_all.py)
setlocal
set "FLAGS=/nologo /std:c++20 /O2 /DNDEBUG /EHsc /MD /W4"

if defined VSCMD_VER goto have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo ERROR: vswhere.exe not found - is Visual Studio installed? 1>&2 & exit /b 1)
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (echo ERROR: no Visual Studio with the C++ x64 toolset found 1>&2 & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_env

if /i "%~1"=="version" (
    cl 2>&1 | findstr /n "^" | findstr /b "1:"
    echo flags: %FLAGS%
    exit /b 0
)

cd /d "%~dp0"
cl %FLAGS% bench.cpp /Fe:bench.exe /link /nologo || exit /b 1
del /q bench.obj 2>nul
