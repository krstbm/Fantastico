# LaptopPrep 1.2 — what was broken, what replaced it, how it was checked

This is the honest write-up you asked for: which parts of the old code were broken,
what the new Windows Update stage does instead, what changed in the interface, how to
build it, and — importantly — what was *not* verified on real hardware.

Everything below was measured on the shipped binaries in this project. Screenshots in
`docs/` are real captures of the running program.

---

## 1. What was wrong with the old Windows Update code

The old stage had four defects that between them made it useless exactly when it
mattered — on a laptop whose update components are already broken:

1. **A wedged agent counted as success.** The old code called the update agent, waited,
   and if nothing came back it treated the silence as "up to date". A laptop with a
   dead `wuauserv`/TrustedInstaller therefore got a green result and was shipped out
   with no updates. This was the single worst bug: the tool lied.
2. **No per-step reporting.** The stage was one opaque call. When it failed you got a
   severity code and no way to tell whether the local store was empty, the payload was
   corrupt, the service refused, or the network was dead.
3. **A broken agent was never repaired.** Nothing checked the agent's health, so a
   machine needing `CheckHealth`/reregistration looked identical to a healthy one.
4. **Silent DISM.** A DISM run that printed nothing was treated as a DISM run that
   succeeded. Several smaller things hit the same pattern: relative `dism.exe`
   invocation, a false "this machine is current" verdict, a duplicated motherboard
   query, a `%SystemRoot%\...utlcli.exe` path that never existed.

`do_windows_update()` is gone. It is replaced by the decision-layer-driven stage below;
the WU decision logic lives in `src/prep_wu.c` and is covered by `tests/wu_decide_test.c`.

## 2. The replacement: "Windows Update — the eight steps"

The new stage is ordered **cheapest possible first, online last**, so a laptop with no
internet still gets every update that is already on its disk:

| # | Step | What it does | Budget |
|---|------|--------------|--------|
| 1 | Read what this machine already has | local update store, downloaded packages, restart flags | ≈10 s |
| 2 | Install packages that are already here | `wusa`/`dism` on the `.msu`/`.cab` files on disk | 45 min |
| 3 | Verify the update payloads | DISM `CheckHealth`, and `RestoreHealth` **only if needed** | 25 min |
| 4 | Hand the work to the update orchestrator | `usoclient` scan/install, per machine | 10 / 25 min |
| 5 | Check the current user session | the per-user half of the update engine | ≈20 s |
| 6 | Ask Windows Update what is missing | online agent search; **no answer = failure, not success** | 45 min |
| 7 | Download and install | one update at a time, each one named in the log | 60 / 90 min |
| 8 | Restart check and record | verdict, next action, and the record in the log | ≈2 s |

Each step reports one of **done / not needed / look / failed** in the interface *and* in
the log, with one sentence saying what actually happened. The verdict line never claims
success it cannot prove: if the agent never answered, the run says so and points at
*Fix Windows Update*, instead of printing "up to date".

`wmic.exe` is deliberately not used anywhere — Windows 11 24H2 removed it.

**Every network operation is bounded by a watchdog** (probe 10 s, upload 90 s, plus a
15 s heartbeat in the log while a step is busy). A black-holed DNS or a captive portal
delays the run by seconds and is then reported honestly ("treated as offline",
"upload given up on"), rather than holding the run open. This became a hard requirement
after the measurement in §4 below.

The console build reports the same thing without a window and uses exit codes:
`0` clean, `1` look, `2` needs attention, `3` nothing ran.

## 2b. The restart prompt (added after the first hand-over)

When a run ends with Windows waiting for a restart to finish the updates, the
window now asks for that restart itself - **after** the log upload has already
been tried, and no matter how that upload went:

* the run finishes, the upload runs (it is even forced on when a restart is
  pending), and only then does the offer appear;
* the offer is a banner at the top of the run screen: *Restart needed to finish
  the updates*, plus one honest line about the record - "The run record is on
  the server", or "Upload failed - the log is saved at C:\RefurbLogs\... and can
  be sent afterwards with Upload", or "No upload endpoint is configured";
* **Restart now** runs `shutdown /r /t 45`, shows a live countdown, brings the
  window to the front and flashes it in the taskbar; **Cancel restart** runs
  `shutdown /a` and says plainly that the updates stay installed and finish at
  the next restart. The context button on the summary card becomes the same
  choice (its old "Upload + restart" label had become redundant);
* without administrator rights Windows refuses a scheduled restart, and the
  program says so in a dialog and points at Start > Power > Restart instead of
  pretending it worked;
* **Later** keeps the banner on screen but never restarts anything by itself.

Tested by making Windows actually wait for a restart (writing the real
`RebootRequired` key) and pointing the upload at a port that refuses
connections, so the failure path is what the screenshot shows.

## 3. The interface: why it is no longer pixelated

- **Every string is drawn through GDI+ (`GdipDrawString`), not GDI.** GDI snaps
  stems to the pixel grid, which is exactly the stair-stepping that is visible on a
  dark theme at these sizes. Measured on a full run screen (~200 strings a frame at
  30 fps): 973 vs 943 CPU ticks per ten seconds, i.e. 3 % - so the smoother
  rasteriser is the default. `LP_GDI_TEXT=1` falls back to the old GDI path.
- Every card, pill, badge, ring, checkbox and glyph is drawn anti-aliased through GDI+
  (`SmoothingModeAntiAlias`, half-pixel offset), not with GDI's jagged primitives.
- The in-window title bar, the sidebar, the plan card, the live "At a glance" card and
  the step list are all custom-painted — there is no stock grey control anywhere except
  the two where Windows does it better (`CF 6.0` toolbar/combo metrics).
- The brand icon is decoded once and resampled with an 8× supersample box filter, so it
  is smooth at 100 %, 125 %, 150 % and 200 % scaling.
- `per-monitor-v2` DPI manifest, so Windows does not bitmap-stretch the window on a
  scaled display (that stretch is the classic "I can count the pixels" cause).
- Type scale comes from the real Segoe UI family with three weights (`g_fStep`,
  `g_fBody`, `g_fSmall`/`g_fTiny`); optional OFL fonts are fetched by
  `tools/fetch-fonts.py` when Segoe UI is missing.
- The home screen now shows the plan itself: the eight steps with their estimates, a
  "will run" chip, and two note cards (Before you start / At a glance — modules, last
  run, log folder, elevation).

Two real bugs were found while checking the rendering, both fixed:

**3a. The window stopped updating after its first frame.** The back-buffer cache and the
buffer-teardown test disagreed about who owned the buffer: on the first paint the
program created the off-screen buffer, cached it — and then deleted it, because the
teardown test could not distinguish "just created and cached" from "creation failed".
The first frame looked perfect (it was blitted before the delete), and every later
`BitBlt` failed with `ERROR_INVALID_HANDLE` while the program happily kept painting into
nothing. On screen: the app opened, the first frame appeared, and then nothing ever
changed again — clicks, progress and results were invisible. Fixed with an explicit
`keepBuf` flag and a comment explaining the trap.

**3b. One run screen ate ~1.5 GB and was killed by Windows.** A GDI+ fill or pen with
alpha below 255 makes the renderer build a scratch 32-bit surface for that operation
(on this window ≈5 MB); a handful of those per frame at 30 fps grew the working set
until the process was OOM-killed ~40 s into a run. Measured, then removed: no animated
path draws with alpha any more — translucency is pre-blended against the surface with
`gfx_mix()` and painted opaque. Panels at rest keep their true alpha (they are drawn
once), everything that moves does not.

| What was measured | Before | After |
|---|---|---|
| Peak working set during a run | ~1500 MB → OOM kill | **165–175 MB, flat** |
| Frames an OOM-doomed run survived | ~40 s, window frozen from frame 1 | full run, ~1200+ paints, no leak |
| GDI handles after a run | — | 0 leaked (reported in the log) |
| A 8-step run, no network at all | hung/OOM | **10.8 s (console), ~25 s (GUI)** |

## 4. What is *not* proven: the Wine finding (please read)

Everything above was verified by running the actual `LaptopPrep.exe` and
`LaptopPrep-console.exe` under Wine 10.0 on Xvfb — **not on real Windows hardware**.
That distinction matters, and one finding in particular:

> **Wine's WinHTTP allocates ~1.5 GB and dies in ~7 s when the host has no network.**
> Reproduced on its own with a 20-line test program: RSS 13 MB → OOM. `curl` to the same
> host times out. This is a Wine defect, not behaviour of this program.

It is why the run stage now has the network watchdogs of §2: on a real laptop with a
dead network or a captive portal the same principle applies (never let an unreachable
network hold the run open), and the log then says so instead of hanging.

What the Wine run therefore does **not** exercise: installing a real `.msu`, a real
`usoclient` orchestrator, a running `wuauserv`/TrustedInstaller, `powercfg`, TPM, and a
real online update search. The restart prompt was exercised end to end (offer,
countdown, `shutdown /r` accepted, `shutdown /a` cancel), but Wine's `shutdown.exe`
did not actually reboot the prefix - on real hardware the machine will restart, so
check that once on a spare laptop before rollout. In the test prefix those are absent, so the run takes the
"not needed / failed" honest-reporting paths — which is exactly what they are for, and
what the screenshots show. **Worth doing on one real laptop before rollout:** run
`LaptopPrep-console.exe --updates` elevated on a machine with (a) updates available and
(b) with the network cable pulled, and read the two logs.

The log upload endpoint is also still unconfirmed:
`https://project.catgirlz.xyz/api/v1/prep-logs` answered `422` when a server was
reachable and `530` when the host was down — it exists but has not accepted a round of
logs. Give me the real endpoint and it is a one-line change; until then the upload
step reports the exact reason and the local log always survives.

## 4b. Motion (added after the first hand-over)

Every animation is built from opaque colours (see §3b for why nothing may use
alpha while it moves):

* the eight step rows **cascade in** when the run screen opens - each a little
  behind the one above it, sliding up as it fades in;
* a soft **sheen travels along the working row**, so a step that takes ten
  minutes still looks alive;
* a completed step's **tick draws itself** instead of popping in, and the row
  flashes briefly when its state changes;
* the progress **ring breathes** while work is running, and the summary card's
  primary button has a slow breath of its own;
* the restart banner **slides in** and its countdown ticks once a second.

## 4c. Ownership, copyright and the fresh log (added after the second hand-over)

* **LaptopPrep is made for Wintr and AGPCS** and says so in every place a user
  can look:
  * the **About page**: "Copyright (c) 2026 Wintr & AGPCS. All rights reserved.",
    the licence line, and two clickable pills - **wintr.lol** and **agpcs.nl** -
    that open the sites in the default browser (they light up on hover);
  * the **version resource** of the .exe: CompanyName / LegalCopyright /
    Comments, which is what Windows shows under Properties > Details, so the
    claim travels with the file;
  * the **header of every log file**, so a log sent to the server carries it;
  * the **console banner** (`LaptopPrep 1.2 - console - Wintr & AGPCS`);
  * `LICENSE.txt` and the README.
  The exact wording: *Licensed to Wintr and AGPCS and their customers. Copying,
  redistributing or using this tool without written permission is not allowed.*
* **Every launch starts a clean log.** Later changed to the esprit-ict rule
  (section 4f): the old `<COMPUTERNAME>_prep.log`, `<COMPUTERNAME>_deviceID.csv`
  and `battery_report.html` are deleted before the new session writes anything,
  so the live log and the upload describe the current session only and the log
  folder holds one set of files per laptop. If a file cannot be cleared
  (another copy of LaptopPrep is open, or a read-only log left by an elevated
  run), the program keeps appending and says why in the log and in the UI.
* **A stray line on "Run prep" was removed.** The button had a separate two
  pixel "highlight" rectangle drawn above its corner radius, so it stuck out
  past both rounded ends - the light is now the first colour of the button's
  own gradient, which follows the corner curvature and cannot overhang.

## 4d. First real-hardware run (after the third hand-over)

LaptopPrep was run on a real laptop and the whole run finished in **28 minutes**,
with step 4 ("hand the work to the update orchestrator") taking the bulk of it.
That is the watchdog working as designed - the code asks the orchestrator and
then *watches* Windows install, up to 10 minutes for the scan phase and up to
25 minutes for the install phase - but the plan card had been labelling step 4
as "~2 min" (the time the *ask* takes, not the wait), which made a healthy
long install look stuck. The label is now **"2-35 min"** (see "4g" - the scan
watch is 10 minutes and the install watch 25, and a machine whose work is already
visible can finish the step in about two). While the step is
running the row says what it is waiting for:

    waiting for Windows to install - 14:32 so far, up to 25 min before it moves on

This is the first verification off Wine, and it confirms the part that matters
most: the orchestrator path completes on real Windows and the run reaches its
verdict instead of hanging.

## 4e. When an update fails to download or install

Windows shows "something went wrong" and a code. LaptopPrep now does three
things about it:

* **it prints the code it was given** - every install attempt already logs the
  WUA result (`-> Failed (4) 0x80070643`), and the code is kept in the state;
* **it explains the code in plain words**, from a table of the failures that
  actually turn up on refurbished laptops: out of disk space (0x80070070),
  not elevated (0x80070005), damaged component store (0x80073712 / 0x80070241
  / 0x80070570 / 0x800f081f), proxy or metered connection (0x8024402C /
  0x8024001E / 0x80248007), installer failure including the Windows 10 WinRE
  partition problem (0x80070643), payload rejected by the cache (0x8024200B),
  feature update rolled back by a driver (0xC1900101). The matching fix is
  written next to the code in the log and used as the verdict's next action;
* **it checks free space on C: before it asks for anything** and says so if the
  disk is tight (under 8 GB) - a full disk is the most common reason a download
  or install fails, and it is better to know before waiting half an hour.

## 4f. Upload and storage matched to the esprit-ict tool, and the idle orchestrator wait

Two things were copied straight out of `esprit-ict.c` — the script this project
grew out of — so both programs behave the same way on the same server.

### Storage: one machine, one set of files

`esprit-ict.c` creates `C:\RefurbLogs`, names everything after the computer and
clears the previous run before it starts:

```c
snprintf(g_logPath, ..., "%s\\%s_prep.log", LOGDIR, g_computer);
snprintf(g_deviceCsv, ..., "%s\\%s_deviceID.csv", LOGDIR, g_computer);
snprintf(g_battHtml, ..., "%s\\battery_report.html", LOGDIR);
```

LaptopPrep now does exactly that: `C:\RefurbLogs\<COMPUTERNAME>_prep.log`,
`<COMPUTERNAME>_deviceID.csv`, `battery_report.html`, and `clean_old_logs()`
deletes the old `*_prep.log`, `*_deviceID.csv` and `battery_report.html` before
the first line of the new session is written ("Deleted N old refurb log file(s)"
/ "No old refurb logs found", the reference's own wording). The writability
fallbacks (`%LOCALAPPDATA%`, `%TEMP%`) stay, and they clean the folder they
actually use.

A small self-inflicted bug surfaced while testing this and is fixed: the
writability check used to create an *empty* `<COMPUTER>_prep.log` as its test,
so the clean-up deleted that file and every single run reported "Deleted 1 old
refurb log file(s)". The check now writes `LaptopPrep-write-test.tmp` and
removes it again.

### The upload: one header, the log as the body

The reference sends a plain-text POST and nothing else:

```c
WinHttpSendRequest(hreq, L"Content-Type: text/plain", (DWORD)-1,
                   fbuf, (DWORD)fsize, (DWORD)fsize, 0);
```

LaptopPrep's request had grown `Content-Type: text/plain; charset=utf-8` plus
`X-LaptopPrep:` and `X-LaptopPrep-Computer:` headers. That is now reduced to the
reference request, byte for byte: `Content-Type: text/plain`, the log text as
the whole body, `parse_url()` for host/port/`WINHTTP_FLAG_SECURE`, and 2xx =
success. The status explanations (404 / 401 / 413 / 422 / 5xx) and the 90-second
upload timeout are kept — they are what makes a failed upload readable.

Verified against a local receiver: the server saw
`POST /api/v1/prep-logs  content-type='text/plain'  len=2243`, the log recorded
`Server upload status 200`, and the folder held exactly `E2B_prep.log` +
`E2B_deviceID.csv`. A second run said `Deleted 2 old refurb log file(s)`; an
empty folder said `No old refurb logs found`.

### The orchestrator no longer waits ten minutes for nothing

The complaint was exact: on a machine with nothing to do, step 4 sat out the
10-minute scan watch and then the 25-minute install watch — 35 minutes of a
technician's day spent watching an idle machine. Measured on the real 28-minute
run, step 4's "waiting for Windows to install" row was the longest span of the
whole run.

Both watches now start on a short **idle** budget and are extended to the full
one the moment the agent shows a single sign of life:

| Watch | Before | Idle (nothing queued) | With work visible |
|---|---|---|---|
| scan (`PREP_WU_ORCH_IDLE_MS`) | 10 min | **2 min**, then the download/install ask goes out | full 10 min |
| install (`PREP_WU_DL_IDLE_MS`) | 25 min | **3 min** | full 25 min, and it says so when it extends |

"Sign of life" means payload bytes moving in `SoftwareDistribution\Download`, a
`.msu`/`.cab` appearing, or the `RebootRequired` flag being raised — the same
evidence the stage already used. Nothing about the *worked* case changed: an
install that is really running still gets its 25 minutes.

The log states which budget is in play and why, e.g.

```
The orchestrator has shown no sign of work in the first 2 min - no payload on
the disk and no restart requirement. Sitting out the rest of the 10 min scan
watch would be waiting for nothing, so the download/install ask goes out now
and gets a short 3 min watch unless the agent starts working
Nothing came back from the orchestrator in 3 min and the scan had found nothing
queued either, so the watch stops here instead of running to 25 min
```

Verified with the two test switches below (shortened to seconds so the check
takes seconds, not minutes): idle scan watch exited at 6 s instead of 10 min,
idle install watch at 8 s instead of 25 min; and when a payload was dropped into
the download folder mid-watch, the log showed
`The update agent is working - the install watch is extended to the full 25 min`.
On a machine with no orchestrator at all (or under Wine) the stage still returns
immediately at `usoclient.exe is not present on this build`, unchanged.

## 4g. The fourth hand-over: the six things the field reported

These came in together, with a screenshot of the warning bar sitting in the
middle of a run. Five are fixed in the code below; the sixth (the upload) has
its cause narrowed down and the tool now says out loud which address it used
and what came back, which is what was missing to finish it from here.

### 1. The "old log file(s) could not be removed" bar followed the operator around

The bar was honest but had no way out: once an old log could not be deleted the
line stayed in the window, view after view. It is now a proper bar with a close
box at its right end (22x22 px, its own hit test): clicking it sets `g_noticeOff`,
spawns the same ripple the buttons use, and collapses the bar over a few frames
(`g_noticeFade`, kept animating by `ui_needs_frame()`). It does not come back
when the operator moves between Live log, Home, Settings and About.

Two things behind it were wrong as well:

* the file could not be deleted *because of LaptopPrep itself*: the reader in
  `log_pump_file()` opened the live log without `FILE_SHARE_DELETE`, so the file
  was locked against its own deletion for as long as the window was open. The
  handle now shares delete.
* `delete_old_files()` gave up on the first `DeleteFileA` that failed. It now
  retries four times, 120 ms apart, before it counts a failure - an anti-virus
  scanner holding a file for a moment is not worth a warning.

Verified under Wine with the log held open by another process: the bar appeared,
the cross dismissed it, and a walk through all four views brought it back not
once. Screenshot: `docs/_shot_notice.png`.

### 2. The title-bar buttons were the wrong way round

`x` minimised and `-` closed the window. `titlebar_metrics()` lays the buttons
out right to left, and the click ids were derived as `WHAT_TITLE_MIN + index`,
which is only true if the layout runs left to right. The mapping is now explicit
and in one place:

```c
static const int map[TB_BTNS] = { WHAT_TITLE_CLOSE, WHAT_TITLE_MAX,
                                  WHAT_TITLE_MIN,   WHAT_TITLE_THEME };
```

and the hit test, the hover and the hover *wash* all go through it. Two
appearances of the same fault went with it: the close cross is the button that
turns red under the mouse (the red wash had been on the minimise button), and
the theme switch is the one that gets the brand-coloured wash. Checked by
clicking: rightmost cross closes, second button maximises, dash minimises.

### 3. The upload: the endpoint is no longer a compile-time secret

The code already sent what the reference tool sends (one `Content-Type:
text/plain` header, the log text as the whole body, 2xx = success - see "4f"),
so the remaining variable is *where* it is sent. That was a `#define` in
`src/prep.h`, which means a wrong address there is a silent failure and a
rebuild to change.

The address is now resolved at run time, first match wins:

| Order | Source | Written by |
|---|---|---|
| 1 | `LP_LOG_URL` in the environment | the test harness / a shortcut |
| 2 | `url=` line in `LaptopPrep.cfg` next to the exe | the Settings row, or Notepad |
| 3 | `HKCU\Software\Esprit ICT\LaptopPrep : LogUrl` | the Settings row |
| 4 | `PREP_LOG_URL` compiled in | the build |

Settings has a new row, **Log upload server**, which shows the address in use
*and where it came from* ("from built in", "from LaptopPrep.cfg", ...). Clicking
it starts an in-place edit (caret, Enter saves, Esc cancels); saving writes the
cfg file next to the exe and the registry value, so one laptop can be pointed at
the right server by copying the folder.

The row that uploads by hand ("Upload the log that is on disk right now") now
also *reports what happened* instead of a bare "not sent":

    Log uploaded - the server answered HTTP 200.
    The server refused the upload (HTTP 422) - the log is still on disk and still in the log folder.
    No answer from http://10.0.0.5/api/v1/prep-logs - check the address and the
    network; the log is still on disk.

Every run also writes the endpoint to the log on its first line, so the log
answers the question by itself:

    2026-10-16 10:09:43 - Upload endpoint: https://project.catgirlz.xyz/api/v1/prep-logs  (from built in)

Verified in Wine against a local receiver: the server saw
`POST /api/v1/prep-logs`, `Content-Type: text/plain`, a 256-byte body; the log
recorded `Server upload status 200` and `Manual upload: OK`, and the window said
"Log uploaded - the server answered HTTP 200" (`docs/_shot_settings.png`). With
the address pointed at a closed port the log recorded
`WinHttpSendRequest failed 12029` and the window said "No answer from
http://127.0.0.1:8898/api/v1/prep-logs - check the address and the network; the
log is still on disk."

What is still needed from the shop: the address the esprit-ict build posts to.
Type it into that row, press the upload row, and the status line now names the
outcome - HTTP code, refusal, or no answer at all.

### 4. "Use about four to six threads" - where threads actually help

Windows installs one update at a time; a second thread installing another one is
how a component store gets corrupted. So the eight update *steps* stay strictly
in order, and the threads went where the work is genuinely independent:

* **the six read-only collection modules** - five WMI queries and the powercfg
  battery report. Each now runs on its own thread with its own WMI connection
  (`wmi_init()` per thread, its own `CoUninitialize()`); they do not write the
  same file, and the log has a mutex around it.
* **the update stage's opening survey** - free disk space, restart flags, the
  reachability probe, the interactive session, `usoclient.exe` present. Five
  independent questions, four of them with a network or registry round trip in
  them, previously asked one after another.

The pool (`prep_pool_run()`) runs this thread plus up to five workers; the width
is 6 by default, `LP_POOL_THREADS=1..6` picks another, `LP_NO_POOL=1` puts it
all back on one thread for comparison.

Measured under Wine (console build, the same six modules, same results):

| Run | Module phase |
|---|---|
| `LP_NO_POOL=1` | 1.563 s |
| default (6 threads) | **0.454 s** |

and the log says what it is doing:
`Running 6 modules at the same time on 6 threads (they only read the machine)`.
Wine has no WMI, so on real hardware the queries are the slow part and the gain
is larger; the update installs themselves are unchanged by design.

### 5. More animations on click

Added to the existing motion (nav items, the big buttons, the module cards and
the Settings rows already rippled on click):

* the title-bar buttons now ripple on click and flash while pressed; the theme
  switch shows it best because the window stays where it is.
* Settings rows flash as well as ripple - on "Open the log folder" and the
  upload row the flash is the only immediate answer until the work finishes.
* the notice's close cross spawns its ripple (item 1).

All of it is the same machinery: `fx_ripple_spawn()` on the click,
`fx_ripple_paint()` in the painter, a float per control that `anim_tick()`
decays, and every one of them registered in `ui_needs_frame()` so the animation
is allowed the frames it needs and the window is idle again afterwards.

### 6. The log wheel scrolled the wrong way

Three faults in one gesture:

* the wheel delta was applied with the wrong sign;
* the springy "kick" that makes the motion feel like a wheel was applied with
  the *opposite* sign, so the text first jumped up and then came back down -
  the exact "when I scroll down it scrolls up and then goes down";
* "follow the live end" pinned the view to the *top* of the buffer
  (`g_logScroll = g_logMaxScroll`) instead of to the newest line.

Now: `g_logScroll` counts how far *back* from the live tail the view is (0 = the
newest line, which is how the painter indexes it: `first = maxScroll - scroll`),
wheel up goes back, wheel down returns toward the newest line, the kick goes the
way the wheel was turned, and follow pins `g_logScroll = 0`. Follow re-arms as
soon as the operator scrolls back to the newest line, and the "Auto scroll"
switch and the log's own button snap to the tail when they are switched on.
PageUp/PageDown/Home/End follow the same convention.

## 5. Build and run

Windows (from the project root, `cmd`):

```
build.bat all        :: both exes
build.bat console    :: headless build only
build.bat test       :: decision-layer tests
```

Both scripts check the compiler before they compile anything, and say what is
wrong if it is the wrong kind of gcc (5a and 5b below are the two failures
seen on real machines).

### 5a. "gcc: error: unrecognized command line option '-municode'"

That message means the `gcc` on PATH is **not mingw-w64**: the flag comes with
mingw-w64, and the old MinGW.org compiler (the 32-bit one, gcc 6.x/8.x) and
Cygwin's gcc do not have it. `build.bat` now asks the compiler what it is and
adapts:

* **mingw-w64** (MSYS2 "MINGW64" shell, WinLibs, or the x86_64 build from
  mingw-w64.org) - builds with `-municode`, the Unicode entry point;
* **any other gcc** - the same program is built through the `WinMain` entry
  point instead (`-DLP_ANSI_ENTRY`); nothing behaves differently, the window
  simply enters through the ANSI-named entry that every Windows compiler has;
* **Cygwin / MSYS (not MINGW) gcc** - refused with an explanation, because
  those link against a POSIX runtime and the .exe would need that runtime to
  start, which is not what a tool you copy onto a laptop can do.

Both paths were built and run on this project: with mingw-w64 the normal way,
and with the entry-point fallback, which starts, paints and runs identically.
The recommended toolchain is still MSYS2's MINGW64 shell - install it, run
`build.bat all`, and the exes appear in the project root.

Cross-compile from Linux with mingw-w64:

```
bash build.sh both   :: LaptopPrep.exe + LaptopPrep-console.exe in the project root
bash build.sh test   :: 20 checks in 22 scenarios
```

Run: `LaptopPrep.exe` (elevate it — TPM, DISM and the update stage need it). The shipped
`tools/fix-windows-update.bat` is the repair the verdict points at when the update
components themselves are broken; it restarts the services, re-registers the agent and
clears a stuck session without deleting already-downloaded updates.

### 5b. The second build failure: `wbemidl.h: No such file or directory`

After the `-municode` fix the build got further and died inside the sources:

    gcc target: mingw32
    this gcc has no -municode: building through the WinMain entry point
    resources -> build\prep.res
    compiling LaptopPrep.exe ...
    src\prep_core.c:48:21: fatal error: wbemidl.h: No such file or directory
    src\prep.h:51 via prep_gui.c: same
    LaptopPrep.exe did not link

`gcc -dumpmachine` printing `mingw32` is the giveaway: that is the old
MinGW.org compiler - 32-bit, last released in 2018.  It has no `-municode`
and, more finally, no WMI, WinHTTP, GDI+ or psapi headers and no import
libraries to match.  No amount of adaptation reaches it: the project needs
COM/WMI (`wbemidl.h`), the update payload downloader (`winhttp.h`), the
anti-aliased drawing (`gdiplus.h`) and the memory telemetry (`psapi.h`).
This compiler can never build LaptopPrep, and it would produce 32-bit output
even if it could.

Both build scripts now settle the question before they compile anything:

* `tools/build_probe.c` includes every Windows SDK header the project uses
  and takes one symbol from every library the link line needs.
* `build.bat` compiles that probe first (quietly) and, if it fails, prints
  one readable block - "This gcc cannot build LaptopPrep", the compiler's
  own error text, and the two ways forward (use the shipped exes, or install
  MSYS2 and build from the "MSYS2 MINGW64" shell) - then exits 1.  The
  sources are never touched.
* `build.sh` does the same with the same probe file.

Both probes now judge by the file the compiler was asked to write, not by
`errorlevel`: the object/exe is deleted first and its existence afterwards is
the verdict.  (While testing this, `errorlevel` after a redirected compile
proved unreliable under Wine's cmd, and a trailing space in `set STUB_SDK=0 &&`
was enough to make a stub compiler lie about its SDK - both traps are gone from
the test path.)

Verified with stub compilers under Wine, all four cases:

| Stub compiler | Result |
|---|---|
| `x86_64-pc-msys` | refused as MSYS/Cygwin, nothing compiled |
| `mingw32`, no SDK headers | refused with the `wbemidl.h` message and the two ways forward, nothing compiled |
| `x86_64-w64-mingw32` without `-municode` | WinMain fallback message, both exes built |
| `x86_64-w64-mingw32` normal | both exes built |

While proving this, two batch-language traps were found and removed - they
were the reason an earlier revision mis-parsed under some cmd.exe builds:

* `if errorlevel 1 set A & set B` runs `set B` **unconditionally**: the `&`
  ends the `if`.  That silently forced the ANSI entry point on good
  compilers too.  All of those lines are now `goto` label style.
* An unescaped `(`/`)` inside an `echo` in an `if`/`else` block breaks the
  whole block parse (`Syntax error: unexpected (`) and loses the script's
  argument.  Parentheses in echoed text are escaped as `^(` `^)`.

`build.bat` was rewritten in label style (no nested parentheses at all) and
the Cygwin/MSYS test changed from a piped `find` - which fails silently on
some cmd implementations, including Wine's - to plain `%VAR:msys=%` string
substitution.

## 6. Diagnostics (for the next person, including me)

| Switch | Effect |
|---|---|
| `LP_DUMP_FRAME=<folder>` | writes the composed frame to `.bmp` per view change — the way the frozen-window bug was proven |
| `LP_GDI_FAST=1` | the `UI: … GDI handles … paints … off-screen buffers … MB working set` telemetry line every 5 s instead of every 60 s |
| `LP_NO_PROBE=1`, `LP_NO_UPLOAD=1` | skip the reachability probe / the log upload — for testing on a machine with no network |
| `LP_ORCH_FAKE=1` | treat every `usoclient` verb as accepted without running it — the only way to walk the orchestrator watch loops on a machine (or in Wine) that has no orchestrator |
| `LP_ORCH_IDLE_MS`, `LP_ORCH_DL_MS` | shorten the two idle budgets (milliseconds) — 6000/8000 turns a ten-minute idle wait into a ten-second test |
| `LP_POOL_THREADS=1..6`, `LP_NO_POOL=1` | thread count for the parallel module/survey phase (default 6), or run that phase on a single thread |
| `LP_LOG_URL=<url>` | force the upload endpoint for one run, ahead of `LaptopPrep.cfg` and the registry |

The always-on telemetry line in the log (`UI: 0 GDI handles (baseline 0), 1242 paints,
1 off-screen buffers, 165 MB working set`) is the first thing to read when a machine
reports a sluggish or blank window: 0 leaked handles and a flat working set means the
interface is healthy.

The upload endpoint in use is the first thing the log says after the "Deleted N
old refurb log file(s)" line — `Upload endpoint: <url>  (from <source>)` — and the
Settings view shows the same string with a "change" chip. If an upload fails, the
two lines to read are that one and the `Server upload status` / `WinHttpSendRequest
failed` line under it; the window's status line repeats them in words.

Screenshots: `docs/_shot_home.png`, `_shot_run_mid.png`, `_shot_run_done.png`,
`_shot_restart.png` (the restart prompt with a failed upload), `_shot_log.png`,
`_shot_settings.png`, `_shot_about.png`.
