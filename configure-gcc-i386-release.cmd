@echo off

REM Configure a GCC i386 build that is Release, not Debug, and raises the
REM import-library filter to 0x0A00 so the Vista-and-later ntoskrnl exports
REM link directly instead of having to be resolved by name at run time.
REM
REM Release also drops KDBG, which is worth knowing when comparing boots: the
REM in-kernel debugger cannot stop the machine in a build that does not have it.
REM
REM It configures into its own output directory, so it sits alongside the
REM ordinary Debug tree rather than replacing it.

setlocal enabledelayedexpansion

if /I "%1" == "help" goto help
if /I "%1" == "/?" (
:help
    echo Syntax: configure-gcc-i386-release.cmd [Cmake-options]
    echo Configures a Release GCC i386 build with DLL_EXPORT_VERSION=0x0A00
    echo into output-MinGW-i386-rel. Run it from a RosBE shell.
    goto quit
)

set REACTOS_SOURCE_DIR=%~dp0
set REACTOS_SOURCE_DIR=%REACTOS_SOURCE_DIR:\=/%

echo %REACTOS_SOURCE_DIR%| find " " > NUL
if %ERRORLEVEL% == 0 (
    echo. & echo   Your source path contains at least one space.
    echo   This will cause problems with building.
    goto quit
)

cmd /c cmake --version 2>&1 | find "cmake version" > NUL || goto cmake_notfound

REM This script is GCC-only: it wants the RosBE environment, which sets ROS_ARCH.
if not defined ROS_ARCH (
    echo. & echo   Error: ROS_ARCH is not set.
    echo   Run this from a RosBE shell so the GCC toolchain is on PATH.
    goto quit
)

if /I not "%ROS_ARCH%" == "i386" (
    echo. & echo   Error: ROS_ARCH is %ROS_ARCH%, but this script configures i386.
    goto quit
)

set REACTOS_OUTPUT_PATH=output-MinGW-i386-rel

REM Collect any extra -D switches the caller passed through.
set CMAKE_PARAMS=
:repeat
if not "%1" == "" (
    set "CMAKE_PARAMS=!CMAKE_PARAMS! %1"
    shift
    goto repeat
)

echo Configuring a Release GCC i386 build in %REACTOS_OUTPUT_PATH%

if not exist %REACTOS_OUTPUT_PATH% (
    mkdir %REACTOS_OUTPUT_PATH%
)
cd %REACTOS_OUTPUT_PATH%

if exist CMakeCache.txt (
    del /q CMakeCache.txt
)

cmake -G Ninja ^
      -DENABLE_CCACHE:BOOL=0 ^
      -DCMAKE_TOOLCHAIN_FILE:FILEPATH=toolchain-gcc.cmake ^
      -DARCH:STRING=i386 ^
      -DCMAKE_BUILD_TYPE:STRING=Release ^
      -DDLL_EXPORT_VERSION:STRING=0x0A00 ^
      %CMAKE_PARAMS% "%REACTOS_SOURCE_DIR%"

if %ERRORLEVEL% NEQ 0 (
    echo. & echo   Configure script failed.
    goto quit
)

echo. & echo Configured. Build with:  ninja -C %REACTOS_OUTPUT_PATH% bootcd
goto quit

:cmake_notfound
echo. & echo   Unable to find cmake. Run this from a RosBE shell.

:quit
endlocal
