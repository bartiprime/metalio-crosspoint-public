@echo off
rem ---------------------------------------------------------------------------
rem CrossPoint Metalio - canonical ESP-IDF build entry point (Windows host)
rem
rem WHY THIS EXISTS
rem   `idf.py` REFUSES to run from an MSYS/MINGW shell (git-bash). It prints
rem   "MSys/Mingw is no longer supported..." and exits 0 WITHOUT BUILDING.
rem   That false-green has cost several sessions: the log is one line long,
rem   no binary is produced, and a caller checking only the exit code reports
rem   success. Never invoke idf.py directly from bash on this host - call this
rem   script through cmd.exe instead (see docs/engineering/BUILD.md).
rem
rem USAGE (from bash / any shell):
rem   cd <repo> && MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "tools\\build.bat -B build-r90c build"
rem   tools\\build.bat -B build-bench -DCP_BENCH=ON build
rem
rem -B IS RESOLVED AGAINST THE REPO ROOT, not against idf/.
rem   This script cds into <repo>/idf because that is where the ESP-IDF project
rem   lives. That used to mean a relative `-B build-r90c` silently created
rem   <repo>/idf/build-r90c while the historical <repo>/build-r90c stayed stale -
rem   two build trees, and the stale one is a false-green waiting to happen.
rem   Any relative -B is now rewritten to an absolute path under the repo root.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

rem --- kill the MSYS markers so the IDF guard cannot fire --------------------
set MSYSTEM=
set MINGW_PREFIX=
set MSYS2_ARG_CONV_EXCL=*

set "REPO=%~dp0.."
for %%I in ("%REPO%") do set "REPO=%%~fI"
pushd "%REPO%"

rem --- ESP-IDF install locations (override by setting these before calling) --
if not defined IDF_PATH set "IDF_PATH=M:\cpc\packages\framework-espidf"
if not defined IDF_TOOLS_PATH set "IDF_TOOLS_PATH=M:\cpc"
if not defined IDF_PYTHON_ENV_PATH set "IDF_PYTHON_ENV_PATH=M:\cpc\python_env\idf6.1_py3.12_env"

rem --- ENV1 (r108): FAIL LOUDLY BEFORE PYTHON WHEN ESP_IDF_VERSION IS MISSING -----
rem WHY: idf.py reads os.getenv('ESP_IDF_VERSION') at CLI init through
rem   Version.coerce(...) in idf_component_manager, so an unset value dies with
rem     TypeError: expected string or bytes-like object, got 'NoneType'
rem   after a partial configure, with ZERO compiler diagnostics -- it reads like a
rem   broken tree and has cost two build cycles. It is an env export gap, so it
rem   is reported as one, here, before any python runs. No default is invented:
rem   a build must name its IDF version, or it does not start.
if not defined ESP_IDF_VERSION (
    echo [build.bat] FATAL: ESP_IDF_VERSION is not set - refusing to start idf.py.
    echo [build.bat]   Without it idf.py dies with: TypeError: expected string or bytes-like
    echo [build.bat]   object, got 'NoneType' - at Version.coerce of the env var, no compile error.
    echo [build.bat]   Export ALL FOUR before calling this script:
    echo [build.bat]     ESP_IDF_VERSION=6.1.0
    echo [build.bat]     IDF_PATH=M:\cpc\packages\framework-espidf
    echo [build.bat]     IDF_TOOLS_PATH=M:\cpc
    echo [build.bat]     IDF_PYTHON_ENV_PATH=M:\cpc\python_env\idf6.1_py3.12_env
    popd
    exit /b 9
)

rem --- resolve the toolchain version directory DYNAMICALLY -------------------
rem The install nests as tools\riscv32-esp-elf\<version>\bin . A hardcoded
rem version, or one extra path segment (...\<version>\riscv32-esp-elf\bin),
rem silently breaks CMake with "Tell CMake where to find the compiler".
set "TCBIN="
for /d %%D in ("%IDF_TOOLS_PATH%\tools\riscv32-esp-elf\*") do (
    if exist "%%D\bin\riscv32-esp-elf-gcc.exe" set "TCBIN=%%D\bin"
)
if not defined TCBIN if exist "%IDF_TOOLS_PATH%\tools\toolchain-riscv32-esp\bin\riscv32-esp-elf-gcc.exe" (
    set "TCBIN=%IDF_TOOLS_PATH%\tools\toolchain-riscv32-esp\bin"
)
if not defined TCBIN (
    echo [build.bat] FATAL: no riscv32-esp-elf toolchain found under %IDF_TOOLS_PATH%\tools
    popd & exit /b 3
)

set "CMAKEBIN="
for /d %%D in ("%IDF_TOOLS_PATH%\tools\cmake\*") do if exist "%%D\bin\cmake.exe" set "CMAKEBIN=%%D\bin"
if not defined CMAKEBIN if exist "%IDF_TOOLS_PATH%\tools\tool-cmake\*\bin\cmake.exe" set "CMAKEBIN=%IDF_TOOLS_PATH%\tools\tool-cmake"
if not defined CMAKEBIN (
    echo [build.bat] FATAL: no cmake found under %IDF_TOOLS_PATH%\tools
    popd & exit /b 4
)

set "NINJABIN="
for /d %%D in ("%IDF_TOOLS_PATH%\tools\ninja\*") do if exist "%%D\ninja.exe" set "NINJABIN=%%D"
if not defined NINJABIN if exist "%IDF_TOOLS_PATH%\tools\ninja\ninja.exe" set "NINJABIN=%IDF_TOOLS_PATH%\tools\ninja"
if not defined NINJABIN (
    echo [build.bat] FATAL: no ninja found under %IDF_TOOLS_PATH%\tools
    popd & exit /b 5
)

set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%NINJABIN%;%CMAKEBIN%;%TCBIN%;%PATH%"

rem --- rom-elfs (needed by a FRESH build dir; idf.py raises
rem     "ESP_ROM_ELF_DIR environment variable is not defined" without it) --------
if not defined ESP_ROM_ELF_DIR (
    for /d %%D in ("%IDF_TOOLS_PATH%\tools\esp-rom-elfs\*") do set "ESP_ROM_ELF_DIR=%%D"
)
if not defined ESP_ROM_ELF_DIR (
    echo [build.bat] FATAL: no esp-rom-elfs found under %IDF_TOOLS_PATH%\tools
    popd & exit /b 7
)
echo [build.bat] romelfs=%ESP_ROM_ELF_DIR%

rem --- rewrite a RELATIVE -B to an absolute path under the repo root ----------
rem ---------------------------------------------------------------------------
rem ALSO: cmd.exe treats '=' as a TOKEN DELIMITER, so a quoted "-DNAME=VALUE"
rem reaches this loop as two arguments, "-DNAME" and "VALUE". Passing them
rem through separately made idf.py fail with
rem     ValueError: not enough values to unpack (expected 2, got 1)   [tools.py:734]
rem i.e. every -D<option>=<value> on this script's command line was broken --
rem which also means the CP_* A/B switches could never be selected through it.
rem Re-join a -D pair here.
rem ---------------------------------------------------------------------------
set "NEWARGS="
:argloop
if "%~1"=="" goto argdone
if /i "%~1"=="-B" (
    set "BV=%~2"
    rem a drive letter means it is already absolute (M:\... or M:/...)
    echo(!BV! | findstr /r /c:"^[A-Za-z]:" >nul
    if errorlevel 1 set "BV=%REPO%\!BV!"
    set "NEWARGS=!NEWARGS! -B "!BV!""
    shift
) else (
    set "A=%~1"
    if "!A:~0,2!"=="-D" (
        set "NEWARGS=!NEWARGS! "!A!=%~2""
        shift
    ) else (
        rem remember the first bare argument - that is the idf.py ACTION
        if not defined ACT if not "!A:~0,1!"=="-" set "ACT=%~1"
        set "NEWARGS=!NEWARGS! "%~1""
    )
)
shift
goto argloop
:argdone

rem --- build from the idf/ project directory ---------------------------------
if not exist "%REPO%\idf\CMakeLists.txt" (
    echo [build.bat] FATAL: %REPO%\idf\CMakeLists.txt not found - wrong repo layout?
    popd & exit /b 6
)
pushd "%REPO%\idf"

echo [build.bat] repo =%REPO%
echo [build.bat] idf  =%IDF_PATH%
echo [build.bat] tc   =%TCBIN%
echo [build.bat] cmake=%CMAKEBIN%
echo [build.bat] args =%NEWARGS%

rem --- version-stamp guard ----------------------------------------------------
rem WHY: CROSSPOINT_VERSION is the ONLY source of the string `ver` prints. An
rem image built into build-r1018/ while the stamp still said r101.7 was flashed
rem and reported r101.7 - 15 wasted minutes plus a wrong conclusion. Refuse to
rem compile when the -B dir name and the stamp disagree.
rem Non-build actions (clean/fullclean/menuconfig/...) are NOT guarded, and an
rem unrecognised build dir name (no rNNNN) only warns - never a false block.
rem Escape hatch: SST_ALLOW_STAMP_MISMATCH=1 downgrades the failure to a warning.
set "GUARD_ACTION=%ACT%"
if not defined GUARD_ACTION set "GUARD_ACTION=build"
if not defined BV set "BV=%REPO%\build"
if /i not "%GUARD_ACTION%"=="build" (
    echo [build.bat] stamp guard skipped: action=%GUARD_ACTION%
    goto stamp_done
)
python "%REPO%\tools\stamp_guard.py" --dir "%BV%" --action "%GUARD_ACTION%"
if errorlevel 3 if not errorlevel 4 goto stamp_mismatch
if errorlevel 1 goto stamp_warn
goto stamp_done
:stamp_mismatch
echo [build.bat] FATAL: stamp guard rejected this build - nothing was compiled.
popd & popd & endlocal & exit /b 8
:stamp_warn
echo [build.bat] WARN: stamp guard could not run - continuing UNGUARDED.
:stamp_done

python "%IDF_PATH%\tools\idf.py" %NEWARGS%
set "RC=%ERRORLEVEL%"

popd
popd
endlocal & exit /b %RC%
