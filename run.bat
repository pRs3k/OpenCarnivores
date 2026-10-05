@echo off
setlocal
rem Dev launcher: runs the CMake build from the repo root (where HUNTDAT\,
rem shaders\ and shaderpacks\ live). With no arguments it drops straight into
rem Area 1; any arguments you pass replace the defaults, e.g.
rem   run.bat                         (quick test hunt)
rem   run.bat -windowed               (normal start, main menu)
rem   run.bat prj=HUNTDAT\AREAS\AREA2 din=3 wep=2
cd /D "%~dp0"

set "EXE="
for %%P in ("build\OpenCarnivores.exe" "build\Release\OpenCarnivores.exe" "build\Debug\OpenCarnivores.exe") do (
    if not defined EXE if exist "%%~P" set "EXE=%%~P"
)
if not defined EXE (
    echo OpenCarnivores.exe not found in build\, build\Release\ or build\Debug\.
    echo Build it first from this folder:
    echo   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DRENDERER=opengl -B build
    echo   cmake --build build
    echo See BUILD_REQUIREMENTS.md.
    pause
    exit /b 1
)

set "ARGS=%*"
if "%~1"=="" set "ARGS=prj=HUNTDAT\AREAS\AREA1 din=1 wep=1 -nosnd"

if exist crash.log del crash.log
echo Starting %EXE% %ARGS%
"%EXE%" %ARGS%
echo.
echo Exit code: %ERRORLEVEL%
if exist crash.log (
    echo === CRASH LOG ===
    type crash.log
)
pause
