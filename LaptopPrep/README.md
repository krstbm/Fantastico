# LaptopPrep 1.2

Copyright (c) 2026 Wintr & AGPCS. All rights reserved.
Licensed to Wintr and AGPCS and their customers - see LICENSE.txt.
Wintr: https://wintr.lol - AGPCS: https://agpcs.nl

A refurbishment console for Windows laptops in plain C (Win32 + GDI/GDI+).
It collects the machine's facts, reports on them honestly, prepares Windows
Update, and writes one log you can keep.

    LaptopPrep.exe            the window (elevate it)
    LaptopPrep-console.exe    the same run, no window, exit codes 0/1/2/3

## Layout

    src/        prep_core.c   engine: collection, DISM/WUA, watchdogs, logging
                prep_wu.c     the Windows Update decision layer (8 steps, verdict)
                prep_gui.c    the whole window: custom-painted, anti-aliased
                console_main.c, prep.h, prep_wu.h, resource.h, prep.rc,
                laptopprep.manifest (per-monitor-v2 DPI), laptopprep.ico
    tools/      fix-windows-update.bat   the repair the verdict points at
                make_icon.py             regenerates the .ico
                fetch-fonts.py           optional OFL fonts when no Segoe UI
    tests/      wu_decide_test.c         decision-layer tests, no Windows needed
    docs/       QUALITY-REPORT.md        what was broken, what fixed it, evidence
                _shot_*.png              real screenshots of the running program
    build.sh / build.bat                 build both exes / console only / tests

## Build

Windows:   build.bat all      (or: console, test)
Linux:     bash build.sh both (or: console, test)   with mingw-w64
           bash build.sh test -> 20 checks in 22 scenarios

Both exes land in this folder next to assets/ and fonts/.

Compiling is optional: LaptopPrep.exe and LaptopPrep-console.exe are already
built and sit next to the sources.

To build it yourself you need mingw-w64 (MSYS2: pacman -S
mingw-w64-x86_64-toolchain, then use the "MSYS2 MINGW64" shell).  Both build
scripts check the compiler first and stop with an explanation if it is a
toolchain that cannot build this project - the old MinGW.org compiler
(gcc -dumpmachine prints "mingw32", 32-bit, last released in 2018) has no
WMI / WinHTTP / GDI+ headers, which is why it dies on wbemidl.h.

## First thing to read when it misbehaves

The log's telemetry line, every 60 s while it runs:

    UI: 0 GDI handles (baseline 0), 1242 paints, 1 off-screen buffers, 165 MB working set

0 leaked handles + flat working set = the window is healthy. See
docs/QUALITY-REPORT.md section 6 for the diagnostic switches.

## Log, storage and upload

* Every launch starts from a clean folder, the way the original esprit-ict tool
  did: `C:\RefurbLogs` holds `<COMPUTERNAME>_prep.log` and `battery_report.html`
  (plus, until the next launch deletes them, any `<COMPUTERNAME>_deviceID.csv`
  left by an older build - no new CSV is written; the device identity is in
  the log's DEVICE ID section), and the previous run's files are deleted
  before the new session writes anything - so the live log and the upload
  describe this run only, and one laptop always has one set of files.
* **The one uploaded file is the log itself**, exactly as esprit-ict.c sends
  it: the whole `<COMPUTERNAME>_prep.log` as a `text/plain` POST to the
  endpoint. That only works while the log keeps the esprit format the
  server validates - first line of a run
  `=== Starting laptop prep on <COMPUTERNAME> ===`, and
  `=== Prep phase complete ===` written *before* the upload, or the
  server answers `422 not_a_prep_log` / files the device as incomplete.
  Do not rebrand those two lines (see src/prep.h).
* The upload address is not fixed at compile time.  It is taken from, in order:
  `LP_LOG_URL` in the environment, `url=` in `LaptopPrep.cfg` next to the exe,
  `HKCU\Software\Esprit ICT\LaptopPrep : LogUrl`, and last the `PREP_LOG_URL`
  in src/prep.h.  Settings -> "Log upload server" shows the address in use and
  where it came from, and lets it be typed in place (Enter saves to the cfg
  file and the registry).  Every run writes it to the log as its first line.
* Six threads do the read-only work - the five hardware queries, the battery
  report and the update stage's opening survey - because those are independent.
  The Windows Update *steps* stay in order: Windows installs one update at a
  time.  `LP_POOL_THREADS` and `LP_NO_POOL=1` change it for testing.

## Still open

* Verified under Wine on Xvfb, not on real Windows hardware - see QUALITY-REPORT.md.
