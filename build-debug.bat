@echo off
setlocal
cd /d "%~dp0"

set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" (
    echo MSBuild was not found.
    exit /b 1
)

"%MSBUILD%" "%~dp0SYSIM.slnx" /nologo /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v143
set "RESULT=%ERRORLEVEL%"

if "%RESULT%"=="0" (
    echo.
    echo Build succeeded.
) else (
    echo.
    echo Build failed with exit code %RESULT%.
)

exit /b %RESULT%
