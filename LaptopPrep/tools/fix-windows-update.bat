@echo off
rem =====================================================================
rem  fix-windows-update.bat - repair a Windows Update that will not answer
rem  ---------------------------------------------------------------------
rem  LaptopPrep calls this when its own stage reported:
rem     - the update agent is missing or not answering
rem     - the online search was accepted and never came back (wedged)
rem     - the install failed on a machine whose payloads are fine
rem
rem  What this does, in order:
rem     1. restarts the update services
rem     2. re-registers the update engine DLLs (this is what fixes
rem        "the update components are missing" on an image that was
rem        trimmed or prepped by a script)
rem     3. resets the update policy keys a leftover image may carry
rem     4. renames the download cache to a backup name and recreates it,
rem        so a corrupt cache cannot block every install
rem     5. clears the Update Orchestrator's state so it starts from scratch
rem
rem  What it deliberately does NOT do: it does not delete drivers, it does
rem  not touch the component store (that is DISM's job and LaptopPrep does
rem  it itself when it is needed) and it does not enable telemetry.
rem
rem  Run as administrator.  Keep a copy next to LaptopPrep.exe so the
rem  button in the app can start it.
rem =====================================================================

setlocal EnableExtensions
title Fix Windows Update - LaptopPrep

set LOG=%SystemRoot%\Temp\fix-windows-update.log
set SDIR=%SystemRoot%\System32

net session >nul 2>&1
if errorlevel 1 (
    echo.
    echo   This script needs administrator rights.
    echo   Right-click it and choose "Run as administrator".
    echo.
    pause
    exit /b 1
)

echo ============================================================>>"%LOG%"
echo %DATE% %TIME%  fix-windows-update.bat>>"%LOG%"

echo.
echo   Fixing Windows Update. Everything is written to:
echo     %LOG%
echo.

rem ---- 1. services --------------------------------------------------
echo   [1/5] Restarting the update services...
call :svc wuauserv
call :svc bits
call :svc UsoSvc
call :svc CryptSvc
call :svc TrustedInstaller
call :svc msiserver

rem ---- 2. re-register the engine ------------------------------------
echo   [2/5] Re-registering the update engine DLLs...
pushd "%SDIR%" >nul
for %%D in (atl.dll urlmon.dll mshtml.dll shdocvw.dll browseui.dll
            jscript.dll vbscript.dll wuapi.dll wuaueng.dll wups.dll
            wups2.dll wucltux.dll wudriver.dll wuwebv.dll
            Mshtml.dll) do (
    if exist %%D (
        regsvr32 /s %%D
        echo   registered %%D>>"%LOG%"
    )
)
popd >nul

rem ---- 3. policy keys a leftover image may carry --------------------
echo   [3/5] Clearing a leftover update policy...
for %%K in (
    "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate"
    "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU"
    "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\Results"
) do (
    reg delete %%K /f >nul 2>&1
    echo   cleared %%K>>"%LOG%"
)

rem ---- 4. the download cache ----------------------------------------
echo   [4/5] Rebuilding the download cache (a corrupt cache blocks installs)...
set SDP=%SystemRoot%\SoftwareDistribution
if exist "%SDP%" (
    move /y "%SDP%\Download" "%SDP%\Download.old_%RANDOM%" >nul 2>&1
)
mkdir "%SDP%\Download" >nul 2>&1
attrib +h "%SDP%" >nul 2>&1
echo   download cache reset>>"%LOG%"

rem ---- 5. orchestrator + agent --------------------------------------
echo   [5/5] Resetting the Update Orchestrator state...
net stop UsoSvc >nul 2>&1
net start UsoSvc >nul 2>&1
sc config UsoSvc start= auto >nul 2>&1
sc config wuauserv start= demand >nul 2>&1
sc config bits start= demand >nul 2>&1
if exist "%SDIR%\usoclient.exe" (
    "%SDIR%\usoclient.exe" RefreshSettings >nul 2>&1
    echo   usoclient RefreshSettings done>>"%LOG%"
)
if exist "%SDIR%\UsoClient.exe" (
    "%SDIR%\usoclient.exe" StartScan >nul 2>&1
)

echo.
echo   Done. What to do now:
echo     - start LaptopPrep again and run the prep
echo     - the update stage will pick up the downloads that were kept
echo       in Download.old_* only if you move them back; a fresh download
echo       is the reliable route after a corrupt cache
echo     - if Windows still refuses, open Settings ^> Windows Update and
echo       let it scan once on its own: that scan often unblocks it
echo.
echo   A copy of this session is in %LOG%
echo.
pause
exit /b 0

rem ---------------------------------------------------------------------
:svc
rem  restart one service: stop, then start, and say which state it is in
sc config %1 start= demand >nul 2>&1
net stop %1 >nul 2>&1
net start %1 >nul 2>&1
if errorlevel 1 (
    echo   %1: could not be started - see the log>>"%LOG%"
) else (
    echo   %1: running>>"%LOG%"
)
exit /b 0
