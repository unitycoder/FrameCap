@echo off
setlocal EnableExtensions DisableDelayedExpansion
title FrameCap - Windows x64 build
cd /d "%~dp0"
if errorlevel 1 goto bad_directory
if not exist "CMakeLists.txt" goto wrong_folder
if not exist "vcpkg.json" goto wrong_folder

rem Locate VS 2026 with x64 C++ tools, including preview and Build Tools.
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing_vs
rem Run directly: command substitution can strip quotes around Program Files.
set "VS_LOCATION_FILE=%TEMP%\FrameCap-vswhere-%RANDOM%-%RANDOM%.txt"
"%VSWHERE%" -latest -prerelease -products * -version "[18.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VS_LOCATION_FILE%"
if errorlevel 1 goto vs_lookup_failed
for /f "usebackq delims=" %%I in ("%VS_LOCATION_FILE%") do set "VSROOT=%%I"
del /q "%VS_LOCATION_FILE%" >nul 2>&1
if not defined VSROOT goto missing_vs
if not exist "%VSROOT%\Common7\Tools\VsDevCmd.bat" goto missing_vs

echo Loading Visual Studio 2026 x64 native tools...
echo "%VSROOT%"
echo.
rem Clear the inherited VS 2022 targets override from the reported error.
set "VCTargetsPath="
set "VSCMD_START_DIR=%~dp0"
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 goto failed
set "VCTargetsPath="
cd /d "%~dp0"
if errorlevel 1 goto bad_directory

rem Prefer tools bundled with the selected Visual Studio installation.
if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if exist "%VSROOT%\VC\vcpkg\vcpkg.exe" set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"
if not defined VCPKG_ROOT goto missing_vcpkg
if not exist "%VCPKG_ROOT%\vcpkg.exe" goto missing_vcpkg
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" goto missing_vcpkg
where cl.exe >nul 2>&1
if errorlevel 1 goto missing_compiler
where cmake.exe >nul 2>&1
if errorlevel 1 goto missing_cmake
where ninja.exe >nul 2>&1
if errorlevel 1 goto missing_ninja

rem Add a baseline only when missing; preserve an existing pinned baseline.
findstr /c:"builtin-baseline" "vcpkg.json" >nul 2>&1
if not errorlevel 1 goto configure
echo.
echo Adding the initial vcpkg baseline...
"%VCPKG_ROOT%\vcpkg.exe" x-update-baseline --add-initial-baseline
if errorlevel 1 goto failed

:configure
echo.
echo Configuring FrameCap for Release x64...
rem Fresh CMake configuration, separate from the old MSBuild directory.
cmake --fresh -S . -B build-vs2026-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
if errorlevel 1 goto failed

echo.
echo Building FrameCap...
cmake --build build-vs2026-x64 --parallel
if errorlevel 1 goto failed
if not exist "dist" mkdir "dist"
if errorlevel 1 goto failed
copy /y "build-vs2026-x64\FrameCap.exe" "dist\FrameCap.exe" >nul
if errorlevel 1 goto copy_failed
if exist "LICENSE" copy /y "LICENSE" "dist\LICENSE" >nul
if exist "THIRD-PARTY-NOTICES.txt" copy /y "THIRD-PARTY-NOTICES.txt" "dist\THIRD-PARTY-NOTICES.txt" >nul
if exist "build-vs2026-x64\vcpkg_installed\x64-windows-static\share\libvpx\copyright" copy /y "build-vs2026-x64\vcpkg_installed\x64-windows-static\share\libvpx\copyright" "dist\libvpx-LICENSE.txt" >nul
rem Remove the stale checksum for the original supplied binary.
if exist "dist\SHA256SUMS.txt" del "dist\SHA256SUMS.txt"
echo.
echo Build succeeded.
echo EXE: "%CD%\dist\FrameCap.exe"
echo.
pause
exit /b 0

:wrong_folder
echo ERROR: Put build.bat beside CMakeLists.txt and vcpkg.json in the FrameCap folder.
goto failed
:vs_lookup_failed
if exist "%VS_LOCATION_FILE%" del /q "%VS_LOCATION_FILE%" >nul 2>&1
echo ERROR: Visual Studio lookup failed. See the error above.
goto failed
:missing_vs
echo ERROR: Visual Studio 2026 with x64 C++ tools could not be found.
goto prerequisites
:missing_compiler
echo ERROR: The x64 C++ compiler was not initialized.
goto prerequisites
:missing_cmake
echo ERROR: CMake could not be found.
goto prerequisites
:missing_ninja
echo ERROR: Ninja could not be found.
goto prerequisites
:missing_vcpkg
echo ERROR: vcpkg could not be found in Visual Studio or VCPKG_ROOT.
goto prerequisites
:copy_failed
echo ERROR: Could not copy the EXE. Close FrameCap if it is running, then try again.
goto failed
:bad_directory
echo ERROR: Could not open the project directory. Extract the project to a local folder.
goto failed
:prerequisites
echo Open Visual Studio Installer and modify your VS 2026 installation.
echo Enable Desktop development with C++, Windows SDK, C++ CMake tools,
echo and the vcpkg package manager component.
echo.
:failed
echo.
echo Build failed. See the error above.
echo The first build requires an Internet connection for dependencies.
echo.
pause
exit /b 1
