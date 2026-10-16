@echo off
rem =====================================================================
rem  LaptopPrep - build script for Windows (MinGW-w64 / MSYS2)
rem
rem      build.bat all        the windowed console (LaptopPrep.exe)
rem      build.bat console    the headless build (LaptopPrep-console.exe)
rem      build.bat both       both
rem      build.bat test       build + run the update decision tests
rem      build.bat clean      remove build/ and the exes
rem
rem  Needs gcc, windres and strip on PATH - the MSYS2 "MINGW64 Shell" or a
rem  MinGW-w64 installation both provide them:
rem      pacman -S mingw-w64-x86_64-toolchain
rem
rem  Both exes land in this folder, next to assets\ and fonts\.
rem =====================================================================
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "SRC=src"
set "BUILD=build"
set "OUT_GUI=LaptopPrep.exe"
set "OUT_CON=LaptopPrep-console.exe"
set "LIBS=-lole32 -loleaut32 -lwbemuuid -lwinhttp -lws2_32 -liphlpapi -lwtsapi32 -ladvapi32 -lshell32 -lcomctl32 -lgdiplus -lgdi32 -luser32 -lpsapi"
set "CFLAGS=-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -I %SRC%"

where gcc >nul 2>&1 || (
    echo.
    echo   gcc was not found on PATH.
    echo   Open the "MSYS2 MINGW64" shell, or add your MinGW-w64 bin folder
    echo   to PATH, then run this script again.
    echo.
    pause & exit /b 1
)
where windres >nul 2>&1 || (
    echo   windres was not found on PATH - it comes with binutils-mingw-w64.
    pause & exit /b 1
)
if not exist "%BUILD%" mkdir "%BUILD%"

if /i "%~1"=="clean" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    del /q "%OUT_GUI%" "%OUT_CON%" 2>nul
    echo   removed %BUILD%\, %OUT_GUI%, %OUT_CON%
    exit /b 0
)

if /i "%~1"=="test" (
    echo   building the update decision tests...
    gcc -std=c99 -Wall -Wextra -O1 -I %SRC% tests\wu_decide_test.c %SRC%\prep_wu.c -o %BUILD%\wu_decide_test.exe || (
        echo   test build failed & pause & exit /b 1
    )
    "%BUILD%\wu_decide_test.exe" || (
        echo   update decision tests FAILED & pause & exit /b 1
    )
    exit /b 0
)

rem ---- resources ------------------------------------------------------
if not exist "%SRC%\prep.rc" (
    echo   src\prep.rc is missing & pause & exit /b 1
)
if not exist "%SRC%\laptopprep.ico" (
    echo   src\laptopprep.ico is missing - generating it with python
    python3 tools\make_icon.py || python tools\make_icon.py || (
        echo   could not generate the icon; run: python tools\make_icon.py
        pause & exit /b 1
    )
)
echo   resources -^> %BUILD%\prep.res
windres -I %SRC% %SRC%\prep.rc -O coff -o %BUILD%\prep.res || (
    echo   windres failed on src\prep.rc & pause & exit /b 1
)

rem  prep_wu.c must be on both link lines: it holds the update decision layer
set "CORE=%SRC%\prep_core.c %SRC%\prep_wu.c"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

if /i "%TARGET%"=="all" (
    echo   compiling %OUT_GUI% ...
    gcc %CFLAGS% %CORE% %SRC%\prep_gui.c %BUILD%\prep.res -o %OUT_GUI% -municode -mwindows -Wl,--subsystem,windows %LIBS% || (
        echo   %OUT_GUI% did not link & pause & exit /b 1
    )
    if not exist fonts mkdir fonts
    if exist assets\logo_side.bmp  copy /y assets\logo_side.bmp  . >nul
    if exist assets\logo_about.bmp copy /y assets\logo_about.bmp . >nul
    if exist assets\fonts\*.ttf (
        copy /y assets\fonts\*.ttf fonts\ >nul
        echo   fonts copied to .\fonts
    ) else (
        echo   no assets\fonts\*.ttf - using the Windows system font
        echo     (run: python tools\fetch-fonts.py to add the bundled ones)
    )
    echo.
    echo   done: %OUT_GUI%
    echo   log goes to C:\RefurbLogs\LaptopPrep.log
    pause
    exit /b 0
)

if /i "%TARGET%"=="console" (
    echo   compiling %OUT_CON% ...
    gcc %CFLAGS% %CORE% %SRC%\console_main.c %BUILD%\prep.res -o %OUT_CON% %LIBS% || (
        echo   %OUT_CON% did not link & pause & exit /b 1
    )
    echo   done: %OUT_CON%
    pause
    exit /b 0
)

if /i "%TARGET%"=="both" (
    echo   compiling %OUT_GUI% ...
    gcc %CFLAGS% %CORE% %SRC%\prep_gui.c %BUILD%\prep.res -o %OUT_GUI% -municode -mwindows -Wl,--subsystem,windows %LIBS% || (
        echo   %OUT_GUI% did not link & pause & exit /b 1
    )
    echo   compiling %OUT_CON% ...
    gcc %CFLAGS% %CORE% %SRC%\console_main.c %BUILD%\prep.res -o %OUT_CON% %LIBS% || (
        echo   %OUT_CON% did not link & pause & exit /b 1
    )
    if not exist fonts mkdir fonts
    if exist assets\fonts\*.ttf copy /y assets\fonts\*.ttf fonts\ >nul
    echo   done: %OUT_GUI% and %OUT_CON%
    pause
    exit /b 0
)

echo   unknown target: %TARGET%
echo   use: all ^| console ^| both ^| test ^| clean
pause
exit /b 1
