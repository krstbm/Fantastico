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
* **Every launch starts a clean log.** The previous session is moved to
  `LaptopPrep-previous.log` (one file, replaced each time) instead of being
  deleted, so the live log and the upload describe the current session only,
  while the last run is still there to read. If the file cannot be cleared
  (another copy of LaptopPrep is open, or a read-only log left by an elevated
  run), the program keeps appending and says why in the log and in the UI.
* **A stray line on "Run prep" was removed.** The button had a separate two
  pixel "highlight" rectangle drawn above its corner radius, so it stuck out
  past both rounded ends - the light is now the first colour of the button's
  own gradient, which follows the corner curvature and cannot overhang.

## 5. Build and run

Windows (from the project root, `cmd`):

```
build.bat all        :: both exes
build.bat console    :: headless build only
build.bat test       :: decision-layer tests
```

Cross-compile from Linux with mingw-w64:

```
bash build.sh both   :: LaptopPrep.exe + LaptopPrep-console.exe in the project root
bash build.sh test   :: 20 checks in 22 scenarios
```

Run: `LaptopPrep.exe` (elevate it — TPM, DISM and the update stage need it). The shipped
`tools/fix-windows-update.bat` is the repair the verdict points at when the update
components themselves are broken; it restarts the services, re-registers the agent and
clears a stuck session without deleting already-downloaded updates.

## 6. Diagnostics (for the next person, including me)

| Switch | Effect |
|---|---|
| `LP_DUMP_FRAME=<folder>` | writes the composed frame to `.bmp` per view change — the way the frozen-window bug was proven |
| `LP_GDI_FAST=1` | the `UI: … GDI handles … paints … off-screen buffers … MB working set` telemetry line every 5 s instead of every 60 s |
| `LP_NO_PROBE=1`, `LP_NO_UPLOAD=1` | skip the reachability probe / the log upload — for testing on a machine with no network |

The always-on telemetry line in the log (`UI: 0 GDI handles (baseline 0), 1242 paints,
1 off-screen buffers, 165 MB working set`) is the first thing to read when a machine
reports a sluggish or blank window: 0 leaked handles and a flat working set means the
interface is healthy.

Screenshots: `docs/_shot_home.png`, `_shot_run_mid.png`, `_shot_run_done.png`,
`_shot_restart.png` (the restart prompt with a failed upload), `_shot_log.png`,
`_shot_settings.png`, `_shot_about.png`.
