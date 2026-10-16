/* =====================================================================
 *  LaptopPrep - refurbishment / preparation tool for Windows laptops
 *  prep.h - shared API between the engine (prep_core.c + prep_wu.c),
 *           the console build (console_main.c) and the GUI (prep_gui.c)
 *  ---------------------------------------------------------------------
 *  Build (64-bit, MinGW-w64):
 *      windres prep.rc -O coff -o prep.res
 *      gcc prep_core.c prep_wu.c prep_gui.c prep.res -o LaptopPrep.exe \
 *          -municode -mwindows -lcomctl32 -lole32 -loleaut32 \
 *          -lwbemuuid -lwinhttp -lws2_32 -liphlpapi -lshell32 -lgdiplus
 *  Or simply: build.sh all        (Windows: build.bat all)
 * ===================================================================== */
#ifndef PREP_H
#define PREP_H

#define PREP_VERSION_STR  "1.2"
#define PREP_VERSION_NUM  1,2,0,0

/* ---- who owns this ---------------------------------------------- *
 *  LaptopPrep is made for Wintr and AGPCS.  The strings here are the
 *  single source for the About page, the log header and the console
 *  banner; the same text lives in the version resource (prep.rc). */
#define PREP_OWNER        "Wintr & AGPCS"
#define PREP_COPYRIGHT    "Copyright (c) 2026 Wintr & AGPCS. All rights reserved."
#define PREP_LICENCE_LINE "Licensed to Wintr and AGPCS and their customers. Copying, " \
                          "redistributing or using this tool without written " \
                          "permission is not allowed."
#define PREP_URL_WINTR    "https://wintr.lol"
#define PREP_URL_AGPCS    "https://agpcs.nl"
#define PREP_URL_WINTR_SH "wintr.lol"
#define PREP_URL_AGPCS_SH "agpcs.nl"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- *
 *  TARGET BUILD MACHINE  (must match build.bat / build.sh)
 * ---------------------------------------------------------------- */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   /* Windows 10 / 11 */
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif

#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif
#include <windows.h>
#include <wbemidl.h>

/* the update decision layer: step list, phases, verdicts (pure C99) */
#include "prep_wu.h"

/* ---------------------------------------------------------------- *
 *  Server upload endpoint (single source of truth - change it HERE
 *  and rebuild; nothing else in the code hard-codes a URL).
 *
 *  log_upload() does nothing while this string still contains the
 *  placeholder "<SERVER>", so the app is safe to run before a real
 *  endpoint exists.  A 404 in the log means the route is not on the
 *  server yet; the log then stays on disk and the app says so.
 * ---------------------------------------------------------------- */
#define PREP_LOG_URL "https://project.catgirlz.xyz/api/v1/prep-logs"

/* ---------------------------------------------------------------- *
 *  THE UPLOADED FILE - read this before touching the log format.
 *
 *  Exactly one file is uploaded, the same one esprit-ict.c uploads:
 *  C:\RefurbLogs\<COMPUTERNAME>_prep.log, POSTed whole as
 *  Content-Type: text/plain (no multipart, no CSV, no battery HTML -
 *  the server parses the battery/device sections out of the text).
 *
 *  The server validates that text against the esprit-ict.c format
 *  and rejects it (422, code "not_a_prep_log") unless it contains,
 *  verbatim:
 *      === Starting laptop prep on <COMPUTERNAME> ===
 *  and it only marks the device "complete" when the uploaded text
 *  also contains:
 *      === Prep phase complete ===
 *  prep_run_all() therefore writes the first line at the start of
 *  every run and the second one BEFORE log_upload() is called.
 *  Rebranding or moving either line silently breaks every upload -
 *  that is the bug that made LaptopPrep uploads disappear while
 *  esprit-ict.c (http://localhost in that file is its placeholder
 *  for this same route) went through.
 * ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- *
 *  Windows Update watchdogs
 *
 *  The WUA COM API is synchronous and a wedged Windows Update service
 *  cannot be interrupted, only given up on.  So every blocking call runs
 *  on its own thread and the run keeps a stopwatch on it.  In all cases
 *  the log gets a heartbeat every PREP_WU_HEARTBEAT_MS so a long wait is
 *  visible instead of looking frozen.
 * ---------------------------------------------------------------- */
#define PREP_WU_LOCAL_TIMEOUT_MS     (90u  * 1000u)       /* local store read   */
#define PREP_WU_SEARCH_TIMEOUT_MS    (45u  * 60u * 1000u) /* online search      */
#define PREP_WU_DOWNLOAD_TIMEOUT_MS  (60u  * 60u * 1000u) /* downloading        */
#define PREP_WU_INSTALL_TIMEOUT_MS   (90u  * 60u * 1000u) /* installing         */
#define PREP_WU_HEARTBEAT_MS         (15u  * 1000u)       /* log progress       */
#define PREP_WU_SERVICE_WAIT_MS      (15u  * 1000u)       /* start wuauserv     */
#define PREP_WU_USO_SCAN_WAIT_MS     (10u  * 60u * 1000u) /* orchestrator: did  */
#define PREP_WU_USO_DOWNLOAD_WAIT_MS (25u  * 60u * 1000u) /* anything happen?   */
#define PREP_WU_USO_INSTALL_WAIT_MS  (25u  * 60u * 1000u)

/*  Idle budgets for the two orchestrator watches.  When the machine shows
 *  no sign of work at all - no payload moving, no restart requirement -
 *  the watch stops after these short budgets instead of the full ones
 *  above, and the full budget applies again the moment anything moves.
 *  A machine that already has its updates used to sit out the whole
 *  10-minute scan watch and then the whole 25-minute install watch with
 *  nothing happening at either end. */
#define PREP_WU_ORCH_IDLE_MS         (2u  * 60u * 1000u)  /* scan watch      */
#define PREP_WU_DL_IDLE_MS           (3u  * 60u * 1000u)  /* install watch   */
#define PREP_WU_PAYLOAD_TIMEOUT_MS   (45u  * 60u * 1000u) /* one .msu / .cab    */
#define PREP_WU_DISM_TIMEOUT_MS      (25u  * 60u * 1000u) /* DISM repair        */
#define PREP_WU_FIX_TIMEOUT_MS       (30u  * 60u * 1000u) /* fix-windows-update */

/* Network work gets a budget of its own: a dead DNS server or a captive
 * portal must never hold a finished run up, and an upload that did not
 * happen has to be reported instead of assumed. */
#define PREP_HTTP_PROBE_MS           (10u * 1000u)        /* reachability probe  */
#define PREP_UPLOAD_TIMEOUT_MS       (90u * 1000u)        /* POST the log        */

/* ---------------------------------------------------------------- *
 *  Module IDs - used for the progress circle AND for the checkboxes
 *  in the side menu.  They are bit flags, so order is free.
 * ---------------------------------------------------------------- */
#define PREP_M_SPECS      (1u << 0)   /* CPU / RAM / OS + disks        */
#define PREP_M_DEVICE_ID  (1u << 1)   /* serials, UUID, MAC (in log)   */
#define PREP_M_BOARD      (1u << 2)   /* motherboard / BIOS / slots    */
#define PREP_M_EXTRA      (1u << 3)   /* CPU, RAM, GPU, audio, ...     */
#define PREP_M_TPM        (1u << 4)   /* TPM status                    */
#define PREP_M_BATTERY    (1u << 5)   /* powercfg battery report       */
#define PREP_M_UPDATES    (1u << 6)   /* the 8-step update stage       */
#define PREP_M_UPLOAD     (1u << 7)   /* POST the log to the server    */

/* Convenience presets for the UI checkboxes.  Upload is part of a
 * normal run, the way esprit-ict.c does it: that tool POSTs the log
 * unconditionally at the end of main(), it is not an extra the
 * operator has to remember to tick.  A run without it leaves the only
 * copy of the record on a laptop that is about to be wiped.  It can
 * still be unticked per run in the GUI / left out on the console. */
#define PREP_DEFAULT_FLAGS (PREP_M_SPECS | PREP_M_DEVICE_ID | PREP_M_BOARD | \
                            PREP_M_EXTRA | PREP_M_TPM | PREP_M_BATTERY | \
                            PREP_M_UPDATES | PREP_M_UPLOAD)
#define PREP_ALL_FLAGS     (PREP_DEFAULT_FLAGS | PREP_M_UPLOAD)

/* ---- where everything is written ------------------------------- *
 *  The log is named after the machine, exactly like the esprit-ict
 *  tool: C:\RefurbLogs\<COMPUTERNAME>_prep.log, next to
 *  battery_report.html.  A folder that collects logs from fifty
 *  laptops can then tell them apart.  There is no device CSV any
 *  more: it only duplicated the log's DEVICE ID section, and the
 *  server parses that section out of the uploaded log text.  Old
 *  *_deviceID.csv files are still cleaned up at startup. */
#define PREP_LOGDIR          "C:\\RefurbLogs"
#define PREP_LOGFILE_SUFFIX  "_prep.log"

/* ---- engine entry points (implemented in prep_core.c) ---------- */

/* Sets up the log paths, reads the computer name, creates the log folder
 * (falling back to %LOCALAPPDATA% or %TEMP% when it is not writable). */
void prep_init(void);

/* Bitmask of every module the engine knows about. */
unsigned prep_module_count(void);

/* Non-zero while the user asked for this module in the menu. */
int  prep_select(unsigned flags, unsigned bit);

/* Runs the whole selected workload. `flags` is a PREP_M_* bitmask.
 * Returns 0 when the run completed (individual modules report their own
 * success through gui_on_module_result). */
int  prep_run_all(unsigned flags);

/* Where the log actually lives.  prep_init() verifies that the folder is
 * writable and falls back to %LOCALAPPDATA%\LaptopPrep (or %TEMP%) if it
 * is not, so a run can never produce no log at all. */
const char *prep_log_path(void);
const char *prep_log_dir(void);
const char *prep_computer_name(void);
const char *prep_log_notice(void);

/* Logs which of the tools/APIs this machine actually has (wmic.exe is not
 * used by LaptopPrep - Windows 11 24H2 removed it). */
void probe_environment(void);

/* ------------------------------------------------------------------ *
 *  the Windows Update stage, as seen by the UI
 *
 *  phase     - WU_P_* : what it is doing right now
 *  verdict   - WU_J_* : how it ended (none while it is still running)
 *  severity  - 0 clean, 1 notice, 2 warning, 3 needs attention
 *  step_*    - the 8 steps of the stage and their states
 *  All of these are safe to poll from the UI thread while the run goes on.
 * ------------------------------------------------------------------ */
int         prep_update_phase(void);
int         prep_update_verdict(void);
int         prep_update_severity(void);
int         prep_update_step_count(void);
int         prep_update_step_state(int step);
int         prep_update_current_step(void);
int         prep_update_done_steps(void);            /* resolved steps      */
int         prep_update_heartbeat_age_ms(void);       /* ms since last sign of life */
int         prep_update_step_elapsed_ms(int step);   /* time inside a step */
unsigned    prep_update_step_est_ms(int step);       /* expected duration  */
const char *prep_update_step_title(int step);
const char *prep_update_step_note(int step);
const char *prep_update_verdict_text(void);          /* one sentence       */
const char *prep_update_action_text(void);           /* button label       */
const char *prep_update_advice_text(void);           /* what to do next    */
int         prep_restart_pending(void);

/* Everything below is used by both the engine and the GUI. */

void write_log(const char *msg);
int  is_admin(void);

/* WMI plumbing */
int  wmi_init(IWbemServices **ppSvc, IWbemLocator **ppLoc);
void wmi_uninit(IWbemServices *svc, IWbemLocator *loc);
int  wmi_get_first(IWbemServices *svc, const wchar_t *wql,
                   const wchar_t *prop, char *out, int outsz);
void wmi_log_query(IWbemServices *svc, const wchar_t *wql,
                   const wchar_t **props, const char **labels, int nprops,
                   const char *section);

/* Collection steps (all export their own log section) */
void collect_specs(IWbemServices *svc);
void collect_device_id(IWbemServices *svc);
void collect_motherboard(IWbemServices *svc);
void collect_extra(IWbemServices *svc);
void collect_tpm(IWbemLocator *loc);
void run_battery_report(void);
int  log_upload(const char *filepath);

/* result of the last upload: 0 never tried, 1 uploaded, -1 failed.
 * The GUI asks this before it offers to restart the machine. */
int  prep_last_upload_result(void);

/* Runs a command hidden in the background with a timeout, streams its
 * output into the log while it runs, returns non-zero on exit code 0.
 * `cmdline` is UTF-8 and converted to UTF-16 internally (a Danish laptop
 * has an "ø" in its user path, and CreateProcessA would mangle it). */
int  run_process_logged(const char *cmdline, int timeoutMs, DWORD *exitcode);

/* Log endpoint helpers (read PREP_LOG_URL at run time - deliberately not
 * compile-time constants, so the upload code is never optimised away) */
int         prep_url_is_placeholder_ex(void);

/* Where the finished log is posted.  The address can be set without a
 * rebuild: LP_LOG_URL in the environment, a "url=" line in LaptopPrep.cfg
 * next to the exe, or HKCU\Software\LaptopPrep : LogUrl - in that order,
 * with the compiled-in PREP_LOG_URL as the fallback. */
const char *prep_log_url(void);
const char *prep_log_url_source(void);
int         prep_log_url_set(const char *url);

/* HTTP status of the most recent upload attempt:
 *   > 0  the server answered with that code
 *   -1   no answer at all (DNS, TLS, timeout, no network)
 *   -2   nothing was sent (no endpoint configured, or the log was empty)
 *    0   no attempt yet. */
int         prep_last_upload_http(void);
const char *prep_log_url_text(void);

/* ---------------------------------------------------------------- *
 *  Hooks the GUI installs (all optional - the engine works headless)
 * ---------------------------------------------------------------- */

/* Called from prep_run_all() right BEFORE `bit` starts. */
extern void gui_on_module_begin(unsigned bit);

/* Called from prep_run_all() right AFTER `bit` finished.
 * `ok` is 0 when the module failed or was skipped. */
extern void gui_on_module_result(unsigned bit, int ok);

/* Called from prep_run_all() when the whole run is over. */
extern void gui_on_run_finished(int rc);

/* Called regularly while the engine waits on something (hidden child
 * process, update download, ...).  Must never block for long. */
extern void gui_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* PREP_H */
