@echo off
rem ============================================================================
rem  build-host.bat  -  configure + build glsplay-host (native C++, Windows)
rem
rem  Usage (from a terminal, or just double-click for a Release build):
rem    build-host.bat                 Release build (default)
rem    build-host.bat Debug           Debug build
rem    build-host.bat clean           delete apps\host\build, then Release build
rem    build-host.bat clean Debug     delete build dir, then Debug build
rem    build-host.bat Release novigem  Release build with -DGLSPLAY_ENABLE_VIGEM=OFF
rem
rem  Prereqs (see docs\BUILD-HOST.md):
rem    - Visual Studio 2022 + "Desktop development with C++" (MSVC v143)
rem    - CMake 3.24+ on PATH (ships with the VS workload)
rem    - apps\host\third_party\webrtc\   (download - see third_party\README.md)
rem    - apps\host\third_party\nvenc\Interface\nvEncodeAPI.h  (committed)
rem ============================================================================
setlocal EnableDelayedExpansion

rem --- keep the window open if this was double-clicked in Explorer -----------
set "DBLCLICK="
echo(%cmdcmdline%| findstr /i /c:"%~nx0" >nul && set "DBLCLICK=1"

set "ERR=0"
set "REPO_ROOT=%~dp0"
set "HOST_DIR=%REPO_ROOT%apps\host"
set "BUILD_DIR=%HOST_DIR%\build"

set "CONFIG=Release"
set "DO_CLEAN=0"
set "CMAKE_EXTRA="

rem --- parse args (order-independent) --------------------------------------
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="clean"    ( set "DO_CLEAN=1" & shift & goto parse )
if /i "%~1"=="Release"  ( set "CONFIG=Release" & shift & goto parse )
if /i "%~1"=="Debug"    ( set "CONFIG=Debug"   & shift & goto parse )
if /i "%~1"=="novigem"  ( set "CMAKE_EXTRA=!CMAKE_EXTRA! -DGLSPLAY_ENABLE_VIGEM=OFF" & shift & goto parse )
if /i "%~1"=="nonvml"   ( set "CMAKE_EXTRA=!CMAKE_EXTRA! -DGLSPLAY_ENABLE_NVML=OFF"  & shift & goto parse )
echo [warn] unknown argument "%~1" - ignored
shift
goto parse
:parsed

echo(
echo === glsplay-host build =====================================================
echo   config    : %CONFIG%
echo   host dir  : %HOST_DIR%
echo   extra     :%CMAKE_EXTRA%
echo ===========================================================================

rem --- tool check --------------------------------------------------------------
where cmake >nul 2>nul
if errorlevel 1 (
  echo [error] cmake not found on PATH.
  echo         Open the "x64 Native Tools Command Prompt for VS 2022" and re-run,
  echo         or add CMake to PATH ^(installed with the VS C++ workload^).
  set "ERR=1"
  goto end
)

rem --- dependency check ------------------------------------------------------
if not exist "%HOST_DIR%\third_party\webrtc\include" (
  echo [error] libwebrtc not found: %HOST_DIR%\third_party\webrtc\include
  echo         Fetch it first:  powershell -ExecutionPolicy Bypass -File vm-scripts\fetch-deps.ps1
  echo         Details:         apps\host\third_party\README.md
  set "ERR=1"
  goto end
)
if not exist "%HOST_DIR%\third_party\nvenc\Interface\nvEncodeAPI.h" (
  echo [error] NVENC headers not found: %HOST_DIR%\third_party\nvenc\Interface\nvEncodeAPI.h
  set "ERR=1"
  goto end
)

rem --- clean ---------------------------------------------------------------------
if "%DO_CLEAN%"=="1" (
  echo [clean] removing %BUILD_DIR%
  if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

rem --- configure --------------------------------------------------------------
echo(
echo [1/2] cmake configure
cmake -S "%HOST_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 %CMAKE_EXTRA%
if errorlevel 1 (
  echo [error] configure failed
  set "ERR=1"
  goto end
)

rem --- build ----------------------------------------------------------------
echo(
echo [2/2] cmake build ^(%CONFIG%^)
cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel
if errorlevel 1 (
  echo [error] build failed
  set "ERR=1"
  goto end
)

set "EXE=%BUILD_DIR%\bin\%CONFIG%\glsplay-host.exe"
echo(
if exist "%EXE%" (
  echo === OK ====================================================================
  echo   %EXE%
  for %%F in ("%EXE%") do echo   built %%~tF   %%~zF bytes
  echo ===========================================================================
) else (
  echo [error] build reported success but %EXE% is missing
  set "ERR=1"
)

:end
echo(
if defined DBLCLICK (
  echo Press any key to close . . .
  pause >nul
)
exit /b %ERR%
