@echo off
cd /D "%~dp0"
echo Starting OpenCarnivores with dgVoodoo2...
echo Make sure DDraw.dll, D3DImm.dll, and dgVoodoo.conf are in build\Debug\
build\Debug\OpenCarnivores.exe prj=HUNTDAT\AREAS\AREA1 din=1 wep=1 -nosnd
echo.
echo Exit code: %ERRORLEVEL%
if exist crash.log (
    echo === CRASH LOG ===
    type crash.log
)
pause
