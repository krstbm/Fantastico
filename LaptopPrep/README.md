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

## First thing to read when it misbehaves

The log's telemetry line, every 60 s while it runs:

    UI: 0 GDI handles (baseline 0), 1242 paints, 1 off-screen buffers, 165 MB working set

0 leaked handles + flat working set = the window is healthy. See
docs/QUALITY-REPORT.md section 6 for the diagnostic switches.

## Still open

* Every launch starts a clean log (the previous session is kept next to it as
  `LaptopPrep-previous.log`); both the live log and the upload contain only the
  current session.
* The log-upload endpoint in src/prep.h (`PREP_LOG_URL`) is a placeholder.
* Verified under Wine on Xvfb, not on real Windows hardware - see QUALITY-REPORT.md.
