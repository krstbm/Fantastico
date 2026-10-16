@echo off
rem =====================================================================
rem  LaptopPrep - build script for Windows (MinGW-w64)
rem
rem      build.bat all        the windowed app (LaptopPrep.exe)
rem      build.bat console    the headless build (LaptopPrep-console.exe)
rem      build.bat both       both of the above
rem      build.bat test       build + run the update decision tests
rem      build.bat clean      remove build/ and the exes
rem
rem  Needs gcc, windres and strip on PATH: the MSYS2 "MINGW64 Shell" or a
rem  MinGW-w64 installation both provide them.
rem      pacman -S mingw-w64-x86_64-toolchain
rem
rem  Both exes land in this folder, next to assets\ and fonts\.
rem
rem  Written in label style on purpose: no nested "( )" blocks, so every
rem  cmd.exe parses it the same way - the nested style mis-parses under
rem  some implementations even when it looks fine.
rem =====================================================================

setlocal EnableExtensions
cd /d "%~dp0"

set "SRC=src"
set "BUILD=build"
set "OUT_GUI=LaptopPrep.exe"
set "OUT_CON=LaptopPrep-console.exe"
set "LIBS=-lole32 -loleaut32 -lwbemuuid -lwinhttp -lws2_32 -liphlpapi -lwtsapi32 -ladvapi32 -lshell32 -lcomctl32 -lgdiplus -lgdi32 -luser32 -lpsapi"
set "CFLAGS=-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -I src"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

where gcc >nul 2>&1
if errorlevel 1 goto :no_gcc
where windres >nul 2>&1
if errorlevel 1 goto :no_windres

rem ---- which gcc is this, and does it know -municode? ----------------
rem  The windowed build uses the Unicode entry point (wWinMain), which
rem  mingw-w64 enables with -municode.  The old MinGW.org compiler (the
rem  32-bit one, gcc 6.x/8.x) and Cygwin's gcc do not have it and stop
rem  with "unrecognized command line option '-municode'", so the flag is
rem  probed and the same program is built through the WinMain entry point
rem  instead (LP_ANSI_ENTRY) when it is missing.
set "MUNICODE=-municode"
set "ENTRYDEF="
set "GCC_TARGET="
for /f "delims=" %%i in ('gcc -dumpmachine 2^>nul') do set "GCC_TARGET=%%i"
if "%GCC_TARGET%"=="" set "GCC_TARGET=unknown"
echo   gcc target: %GCC_TARGET%

rem  pure string tests on purpose: a piped "find" is not portable here
set "GT=%GCC_TARGET%"
if not "%GT:msys=%"=="%GT%" goto :cygwin_msys
if not "%GT:cygwin=%"=="%GT%" goto :cygwin_msys

rem  Success is judged by the file the compiler was asked to write, not by
rem  errorlevel: a leftover object file from an earlier run must not be able
rem  to make this decision.
echo int main(void^){return 0;} > "%TEMP%\lp_flag_probe.c"
del "%TEMP%\lp_flag_probe.o" >nul 2>&1
gcc -municode -c "%TEMP%\lp_flag_probe.c" -o "%TEMP%\lp_flag_probe.o" >nul 2>&1
if exist "%TEMP%\lp_flag_probe.o" goto :flag_ok
set "MUNICODE="
set "ENTRYDEF=-DLP_ANSI_ENTRY"
:flag_ok
del "%TEMP%\lp_flag_probe.c" "%TEMP%\lp_flag_probe.o" >nul 2>&1
if not "%MUNICODE%"=="" goto :toolchain_probe
echo   this gcc has no -municode: building through the WinMain entry point
echo   ^(works the same; install mingw-w64 from MSYS2 for the Unicode one^)

rem ---- can this gcc actually build the project? ----------------------
rem  tools\build_probe.c includes every SDK header the project uses and
rem  takes one symbol from every library it links.  A compiler that lacks
rem  the WMI / WinHTTP / GDI+ headers fails here, with one clear message,
rem  instead of dying halfway through src\prep_core.c - which reads like a
rem  mistake in the project rather than a toolchain problem.
:toolchain_probe
if not exist "tools\build_probe.c" goto :skip_probe
del "%TEMP%\laptopprep_probe.exe" "%TEMP%\laptopprep_probe.log" >nul 2>&1
gcc "tools\build_probe.c" -o "%TEMP%\laptopprep_probe.exe" -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 %LIBS% >"%TEMP%\laptopprep_probe.log" 2>&1
if not exist "%TEMP%\laptopprep_probe.exe" goto :bad_toolchain
del "%TEMP%\laptopprep_probe.exe" "%TEMP%\laptopprep_probe.log" >nul 2>&1
goto :skip_probe

:bad_toolchain
echo.
echo   This gcc cannot build LaptopPrep.
echo   Compiler target: %GCC_TARGET%
echo.
echo   The project needs the WMI, WinHTTP, GDI+ and psapi headers and
echo   import libraries that ship with mingw-w64.  The old MinGW.org
echo   compiler ^(target "mingw32", last released in 2018^) and Cygwin do
echo   not have them - so the build stops here, with this message, instead
echo   of failing halfway through the sources.
echo.
echo   The compiler's own last words were:
type "%TEMP%\laptopprep_probe.log" 2>nul
echo.
echo   Two ways forward:
echo     1. Nothing has to be compiled at all: the built LaptopPrep.exe and
echo        LaptopPrep-console.exe ship next to this script.  Copy them to
echo        the laptop and run them.
echo     2. To build here, install MSYS2 ^(msys2.org^), open the
echo        "MSYS2 MINGW64" shell ^(not the plain MSYS one^), then:
echo             pacman -S mingw-w64-x86_64-toolchain
echo             cd  to this folder
echo             build.bat all
echo.
del "%TEMP%\laptopprep_probe.log" >nul 2>&1
pause
exit /b 1

:skip_probe

if "%TARGET%"=="clean" goto :do_clean
if "%TARGET%"=="test"  goto :do_test
if "%TARGET%"=="all"   goto :do_all
if "%TARGET%"=="console" goto :do_console
if "%TARGET%"=="both"  goto :do_both
goto :usage

rem ---- resources -----------------------------------------------------
:resources
if not exist "%SRC%\prep.rc" goto :no_rc
if exist "%SRC%\laptopprep.ico" goto :res_run
echo   src\laptopprep.ico is missing - generating it with python
python3 tools\make_icon.py
if errorlevel 1 python tools\make_icon.py
if errorlevel 1 goto :no_icon
:res_run
echo   resources -^> %BUILD%\prep.res
windres -I %SRC% %SRC%\prep.rc -O coff -o %BUILD%\prep.res
if errorlevel 1 goto :no_res
exit /b 0

rem ---- fonts ---------------------------------------------------------
:fonts
if not exist fonts mkdir fonts
if not exist assets\fonts\*.ttf goto :fonts_none
copy /y assets\fonts\*.ttf fonts\ >nul
echo   fonts copied to .\fonts
exit /b 0
:fonts_none
echo   no assets\fonts\*.ttf - using the Windows system font
echo     run: python tools\fetch-fonts.py to add the bundled ones
exit /b 0

rem ---- the targets ---------------------------------------------------
:do_all
if not exist "%BUILD%" mkdir "%BUILD%"
call :resources
echo   compiling %OUT_GUI% ...
gcc %CFLAGS% %ENTRYDEF% "%SRC%\prep_core.c" "%SRC%\prep_wu.c" "%SRC%\prep_gui.c" "%BUILD%\prep.res" -o %OUT_GUI% %MUNICODE% -mwindows -Wl,--subsystem,windows %LIBS%
if errorlevel 1 goto :no_link_gui
strip %OUT_GUI% 2>nul
if exist assets\logo_about.bmp copy /y assets\logo_about.bmp . >nul
call :fonts
echo   done: %OUT_GUI%
echo   log goes to C:\RefurbLogs\<COMPUTERNAME>_prep.log
pause
exit /b 0

:do_console
if not exist "%BUILD%" mkdir "%BUILD%"
call :resources
echo   compiling %OUT_CON% ...
gcc %CFLAGS% "%SRC%\prep_core.c" "%SRC%\prep_wu.c" "%SRC%\console_main.c" "%BUILD%\prep.res" -o %OUT_CON% %LIBS%
if errorlevel 1 goto :no_link_con
strip %OUT_CON% 2>nul
echo   done: %OUT_CON%
pause
exit /b 0

:do_both
if not exist "%BUILD%" mkdir "%BUILD%"
call :resources
echo   compiling %OUT_GUI% ...
gcc %CFLAGS% %ENTRYDEF% "%SRC%\prep_core.c" "%SRC%\prep_wu.c" "%SRC%\prep_gui.c" "%BUILD%\prep.res" -o %OUT_GUI% %MUNICODE% -mwindows -Wl,--subsystem,windows %LIBS%
if errorlevel 1 goto :no_link_gui
echo   compiling %OUT_CON% ...
gcc %CFLAGS% "%SRC%\prep_core.c" "%SRC%\prep_wu.c" "%SRC%\console_main.c" "%BUILD%\prep.res" -o %OUT_CON% %LIBS%
if errorlevel 1 goto :no_link_con
strip %OUT_GUI% 2>nul
strip %OUT_CON% 2>nul
if exist assets\logo_about.bmp copy /y assets\logo_about.bmp . >nul
call :fonts
echo   done: %OUT_GUI% and %OUT_CON%
pause
exit /b 0

:do_test
echo   building the update decision tests ...
if not exist "%BUILD%" mkdir "%BUILD%"
gcc -std=c99 -Wall -Wextra -O1 -I "%SRC%" "tests\wu_decide_test.c" "%SRC%\prep_wu.c" -o "%BUILD%\wu_decide_test.exe"
if errorlevel 1 goto :no_test_build
"%BUILD%\wu_decide_test.exe"
if errorlevel 1 goto :tests_failed
pause
exit /b 0

:no_rc
echo   src\prep.rc is missing
pause & exit /b 1

:no_icon
echo   could not generate the icon; run: python tools\make_icon.py
pause & exit /b 1

:no_res
echo   windres failed on src\prep.rc
pause & exit /b 1

:no_link_gui
echo   %OUT_GUI% did not link
pause & exit /b 1

:no_link_con
echo   %OUT_CON% did not link
pause & exit /b 1

:no_test_build
echo   test build failed
pause & exit /b 1

:tests_failed
echo   update decision tests FAILED
pause & exit /b 1

:do_clean
if exist "%BUILD%" rmdir /s /q "%BUILD%"
del /q "%OUT_GUI%" "%OUT_CON%" 2>nul
echo   removed %BUILD%\, %OUT_GUI%, %OUT_CON%
exit /b 0

rem ---- error paths --------------------------------------------------
:no_gcc
echo.
echo   gcc was not found on PATH.
echo   Open the "MSYS2 MINGW64" shell, or add your MinGW-w64 bin folder
echo   to PATH, then run this script again.
echo.
pause
exit /b 1

:no_windres
echo   windres was not found on PATH - it comes with binutils-mingw-w64.
pause
exit /b 1

:cygwin_msys
echo.
echo   This gcc builds programs that need the MSYS/Cygwin runtime to run.
echo   LaptopPrep has to be a normal Windows program, so use the
echo   "MSYS2 MINGW64" shell ^(or a MinGW-w64 toolchain on PATH^) instead.
echo.
pause
exit /b 1

:usage
echo   unknown target: %TARGET%
echo   use: all ^| console ^| both ^| test ^| clean
pause
exit /b 1
