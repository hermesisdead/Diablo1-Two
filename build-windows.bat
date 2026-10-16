@echo off
rem ===========================================================================
rem  build-windows.bat
rem
rem  Builds Diablo1-Two (DevilutionX fork) into a runnable devilutionx.exe
rem  placed in the same folder as this .bat file.
rem
rem  Requirements (one-time):
rem    - Visual Studio 2022 or newer (Community is fine) with the workload
rem      "Desktop development with C++" (includes CMake and Ninja)
rem    - Git for Windows  (https://gitforwindows.org/)
rem
rem  Usage:
rem    Put this file in the root of the repository (next to CMakeLists.txt)
rem    and double-click it. If it sits in an empty folder instead, it clones
rem    the repository into a "source" subfolder first.
rem
rem  Options:
rem    build-windows.bat           Release build (default)
rem    build-windows.bat debug     Debug build (enables the dev/debug hooks)
rem    build-windows.bat clean     Delete the build folder first, then build
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "OUT_DIR=%~dp0"
if "%OUT_DIR:~-1%"=="\" set "OUT_DIR=%OUT_DIR:~0,-1%"
set "REPO_URL=https://github.com/hermesisdead/Diablo1-Two.git"
set "REPO_BRANCH=feature/procedural-town"
set "BUILD_TYPE=Release"
set "CLEAN=0"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="debug" set "BUILD_TYPE=Debug"
if /I "%~1"=="release" set "BUILD_TYPE=Release"
if /I "%~1"=="clean" set "CLEAN=1"
shift
goto parse_args
:args_done

echo.
echo === Diablo1-Two Windows build (%BUILD_TYPE%) ===
echo Output folder: %OUT_DIR%
echo.

rem --- Locate the source tree ------------------------------------------------
if exist "%OUT_DIR%\CMakeLists.txt" (
    set "SRC_DIR=%OUT_DIR%"
    rem Running from a checkout of the repository: pull the latest changes first
    rem (skipped quietly if there are local edits or no network).
    where git >nul 2>nul && if exist "%OUT_DIR%\.git" (
        echo Updating this checkout ...
        git -C "%OUT_DIR%" pull --ff-only || echo [WARN] Could not update, building what is here.
    )
) else (
    set "SRC_DIR=%OUT_DIR%\source"
    where git >nul 2>nul || (
        echo [ERROR] Git was not found. Install Git for Windows and try again.
        goto fail
    )
    if not exist "!SRC_DIR!\CMakeLists.txt" (
        echo Cloning %REPO_URL% [%REPO_BRANCH%] ...
        git clone --branch %REPO_BRANCH% --recurse-submodules "%REPO_URL%" "!SRC_DIR!" || goto fail
    ) else (
        echo Updating existing clone in !SRC_DIR! ...
        git -C "!SRC_DIR!" pull --ff-only
    )
)
set "BUILD_DIR=%SRC_DIR%\build-windows-%BUILD_TYPE%"

rem --- Visual Studio developer environment -----------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio was not found. Install Visual Studio 2022 with
    echo         the "Desktop development with C++" workload.
    goto fail
)
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo [ERROR] Visual Studio is installed but the C++ tools are missing.
    echo         Add the "Desktop development with C++" workload in the VS Installer.
    goto fail
)
echo Using Visual Studio at: %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto fail

where cmake >nul 2>nul || (
    echo [ERROR] CMake not found. Add "C++ CMake tools for Windows" in the VS Installer.
    goto fail
)
where ninja >nul 2>nul || (
    echo [ERROR] Ninja not found. Add "C++ CMake tools for Windows" in the VS Installer.
    goto fail
)

rem --- vcpkg (dependencies: SDL2, zlib, bzip2, lua, ...) ----------------------
rem Use a private vcpkg next to the bat so the pinned baseline is always available.
set "VCPKG_DIR=%OUT_DIR%\.vcpkg"
if not exist "%VCPKG_DIR%\vcpkg.exe" (
    where git >nul 2>nul || (
        echo [ERROR] Git was not found. Install Git for Windows and try again.
        goto fail
    )
    if not exist "%VCPKG_DIR%\.git" (
        echo Cloning vcpkg ...
        git clone https://github.com/microsoft/vcpkg "%VCPKG_DIR%" || goto fail
    )
    echo Bootstrapping vcpkg ...
    call "%VCPKG_DIR%\bootstrap-vcpkg.bat" -disableMetrics || goto fail
) else (
    rem Make sure the pinned baseline commit exists locally.
    git -C "%VCPKG_DIR%" fetch --quiet origin >nul 2>nul
)
set "VCPKG_ROOT=%VCPKG_DIR%"

rem --- Configure & build ------------------------------------------------------
if "%CLEAN%"=="1" if exist "%BUILD_DIR%" (
    echo Removing old build folder ...
    rmdir /s /q "%BUILD_DIR%"
)

echo.
echo Configuring (first run downloads and builds dependencies, this can take a while) ...
cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows ^
    -DBUILD_TESTING=OFF ^
    -DDISABLE_LTO=ON ^
    -DBUILD_ASSETS_MPQ=OFF || goto fail

echo.
echo Building ...
cmake --build "%BUILD_DIR%" --parallel || goto fail

rem --- Copy the runnable game next to this .bat -------------------------------
echo.
echo Copying game files to %OUT_DIR% ...
if not exist "%BUILD_DIR%\devilutionx.exe" (
    echo [ERROR] devilutionx.exe was not produced.
    goto fail
)
copy /y "%BUILD_DIR%\devilutionx.exe" "%OUT_DIR%\" >nul || goto fail
if exist "%BUILD_DIR%\devilutionx.pdb" copy /y "%BUILD_DIR%\devilutionx.pdb" "%OUT_DIR%\" >nul
for %%f in ("%BUILD_DIR%\*.dll") do copy /y "%%f" "%OUT_DIR%\" >nul
rem When the .bat sits in the repository root, the repo's own assets\ and mods\
rem folders are already next to the exe (the build copies are a subset), so
rem they are only copied when building from a separate "source" clone.
if /I not "%SRC_DIR%"=="%OUT_DIR%" (
    if exist "%BUILD_DIR%\assets" robocopy "%BUILD_DIR%\assets" "%OUT_DIR%\assets" /E /NFL /NDL /NJH /NJS /NP >nul
    if exist "%BUILD_DIR%\mods" robocopy "%BUILD_DIR%\mods" "%OUT_DIR%\mods" /E /NFL /NDL /NJH /NJS /NP >nul
)
if exist "%BUILD_DIR%\devilutionx.mpq" copy /y "%BUILD_DIR%\devilutionx.mpq" "%OUT_DIR%\" >nul

echo.
echo === Build finished ===
echo   %OUT_DIR%\devilutionx.exe
echo.
if not exist "%OUT_DIR%\DIABDAT.MPQ" if not exist "%OUT_DIR%\diabdat.mpq" if not exist "%OUT_DIR%\spawn.mpq" (
    echo NOTE: The game needs the original Diablo data to run.
    echo       Copy DIABDAT.MPQ from your Diablo CD or GOG install into:
    echo         %OUT_DIR%
    echo       ^(or spawn.mpq for the shareware version; hellfire*.mpq for Hellfire^).
    echo.
)
echo What's new to try:
echo   - Tristram: walk to the western edge of the woods for "Into the Darkwood"
echo     ^(endless, level-scaled monster waves; single player^).
echo   - Tremain the Gambler stands by the fountain; shops now use the item grid.
echo   - Rare ^(yellow^) items and level-scaled loot.
echo   - Diablo II style skill trees: press T to spend skill points.
echo   - Sand Tristram: talk to Jamal by the cows ^(next to the cow with bags^).
echo   - Weather everywhere ^(rain, storms, snow, wind, fireflies, ash...^) with
echo     ambient sound; adjust it with the Ambience slider in the in-game Options.
echo   - Free movement: the hero and monsters move at any angle, not along the
echo     tile grid. Press F to switch between free movement and the grid.
echo   - Smooth effects: fog, rain, snow and glows blend in full colour instead
echo     of a dither pattern, and torches, fire and spells cast coloured light.
echo     Settings ^> Graphics: Smooth Effects / Coloured Lighting.
echo   - Spell effects: spells trail embers, sparks or motes, glow, light up the
echo     floor around them and burst where they hit ^(Settings ^> Graphics^).
echo   - F8 opens the weather panel: current weather, time until it changes,
echo     an intensity slider and on/off switches. Tab is only the map now ^(the
echo     weather stays on while it is open^). Spell hotkeys are F5-F7.
echo   - F4 toggles action mode ^(close camera, WASD, dodge, lock-on^).
echo     In action mode the mana potion moved from F to G.
echo   Options for all of these are under Settings ^> Gameplay / Graphics.
echo.
pause
exit /b 0

:fail
echo.
echo *** Build failed. Scroll up for the first error. ***
echo.
pause
exit /b 1
