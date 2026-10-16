/* =====================================================================
 *  LaptopPrep - Windows Update decision layer  (prep_wu.h)
 *  ---------------------------------------------------------------------
 *  This header and its .c are deliberately plain C99 with no Windows
 *  headers at all: the whole "what is this machine's update state and
 *  what does the operator do next" logic is pure data, so it can be
 *  compiled and unit tested on any machine (see tests/wu_decide_test.c)
 *  and so the GUI simply reads a snapshot instead of guessing.
 *
 *  Why this file exists at all
 *  ---------------------------
 *  The first version of this tool drove Windows Update through a single
 *  blocking COM call and treated "the call came back" as "the machine is
 *  up to date".  In the field that produced the two failures this rewrite
 *  is about:
 *
 *    a) the local update store (an offline query, answered by the client
 *       side cache) answered "0 pending", and the tool reported
 *       "up to date" although the online service had never been reached;
 *    b) a wedged update service swallowed the search: the call never
 *       returned and the run had no verdict at all - just silence.
 *
 *  The rules that replace them:
 *    - "up to date" needs the *online* service to have answered with zero
 *      pending updates, or the local store to have answered zero while
 *      the machine can reach the service;
 *    - never answering is its own outcome (WU_J_WEDGED / WU_J_OFFLINE),
 *      with the restart that actually clears it as the next action;
 *    - a step that was not needed is *skipped* and says so, a step that
 *      was reached and found nothing is *done*: the two are never
 *      conflated;
 *    - every step carries an evidence rule, and the verdict is derived
 *      from that evidence only - never from a call having returned.
 * ===================================================================== */
#ifndef PREP_WU_H
#define PREP_WU_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  the eight steps of the update stage, in the order they run
 * ------------------------------------------------------------------ */
#define WU_STEP_COUNT 8

enum {
    WU_S_SCAN = 0,   /* local update store, downloaded payloads, restart flags */
    WU_S_STAGED,     /* install .msu / .cab that are already on this disk     */
    WU_S_HEALTH,     /* verify the update payloads (DISM, only when needed)   */
    WU_S_ORCH,       /* the update orchestrator: per machine, fire and watch  */
    WU_S_SESSION,    /* the current user's half of the update engine          */
    WU_S_AGENT,      /* search for updates (local cache first, then online)   */
    WU_S_INSTALL,    /* download and install, one update at a time            */
    WU_S_FINAL       /* restart requirement, verdict, record                  */
};

/* state of one step, as shown in the UI */
enum {
    WU_SS_QUEUED = 0,   /* not reached yet                                 */
    WU_SS_RUNNING,      /* being worked on right now                       */
    WU_SS_DONE,         /* reached, ran, and produced a good result        */
    WU_SS_WARN,         /* reached, but it could not do what it wanted     */
    WU_SS_FAILED,       /* reached and it failed                           */
    WU_SS_SKIPPED       /* not needed on this machine / in this run        */
};

/* ------------------------------------------------------------------ *
 *  what the stage is doing right now (drives the caption and the colour)
 * ------------------------------------------------------------------ */
enum {
    WU_P_IDLE = 0,
    WU_P_CHECKING,          /* reading what the machine already has     */
    WU_P_INSTALLING_CACHED, /* installing local packages                */
    WU_P_VERIFYING,         /* DISM payload check / repair              */
    WU_P_ORCHESTRATOR,      /* usoclient                                */
    WU_P_SESSION,           /* per-user part of the engine              */
    WU_P_SEARCHING,         /* asking Windows Update                   */
    WU_P_INSTALLING,        /* download + install                       */
    WU_P_RESTART,           /* updates need a restart                   */
    WU_P_CURRENT,           /* nothing to do, machine is current        */
    WU_P_BLOCKED            /* it could not be done - see the verdict   */
};

/* ------------------------------------------------------------------ *
 *  how the stage ended.  NONE means "not finished yet", and that is the
 *  only value that may be shown while the stage is still running.
 * ------------------------------------------------------------------ */
enum {
    WU_J_NONE = 0,
    WU_J_SUCCESS,           /* verified current                          */
    WU_J_SUCCESS_RESTART,   /* updates are installed, a restart finishes */
    WU_J_UNFINISHED,        /* updates are known but not installed       */
    WU_J_BLOCKED,           /* the update components themselves are gone */
    WU_J_OFFLINE,           /* machine cannot be checked from here       */
    WU_J_UNCONFIRMED,       /* the local store said 0, the service never
                             * confirmed - deliberately NOT "up to date"    */
    WU_J_WEDGED,            /* the service accepted and never answered   */
    WU_J_NEEDS_ELEVATION,   /* Windows refused: not administrator        */
    WU_J_ERROR              /* reached the end without a clean answer    */
};

/* what the operator should do about it */
enum {
    WU_A_NONE = 0,
    WU_A_UPLOAD_AND_RESTART,
    WU_A_RESTART_LAPTOP,
    WU_A_OPEN_UPDATE,
    WU_A_FIX_UPDATE_ENGINE,
    WU_A_CHECK_NETWORK,
    WU_A_RUN_ELEVATED,
    WU_A_READ_LOG,
    WU_A_RUN_AGAIN
};

/* ------------------------------------------------------------------ *
 *  the evidence collected by the engine during the run
 *
 *  Every field is a fact observed on this machine.  The decision layer
 *  never invents one: a value that was not observed stays 0, and the
 *  step that would have produced it stays un-reached, which the UI
 *  reports as "queued"/"not needed" instead of "fine".
 * ------------------------------------------------------------------ */
typedef struct {
    /* environment */
    int admin;                  /* running elevated                     */
    int win_build;              /* 19045 = Win10 22H2, 26100 = Win11 24H2 */
    int internet_likely;        /* a probe reached the update endpoints  */
    int started;                /* the stage was entered                 */
    int finished;               /* the stage ran to its final step       */

    /* which steps the engine actually entered */
    int reached[WU_STEP_COUNT];

    /* step 0/5: the Windows Update Agent itself */
    int agent_present;          /* Microsoft.Update.Session could be made */
    int agent_answered;         /* the local store search returned a code */
    long agent_rc;              /* WUResultCode of that search            */
    long agent_hresult;         /* its HResult (0x80070005 = refused)     */
    int store_count;            /* pending updates in the local store     */
    int store_all_downloaded;   /* all of those are already cached        */
    int agent_timed_out;        /* it accepted the call and never answered */
    int agent_error;            /* its calls come back with an error         */
    int access_denied;          /* Windows refused the update work        */

    /* step 1: payloads that were already on the disk */
    int staged_payloads;        /* .msu + .cab found on this machine      */
    int staged_installed;       /* at least one of them installed         */
    int staged_failed;          /* one of them failed hard                */

    /* step 2: component / payload health */
    int components_checked;     /* the check ran                          */
    int components_faulty;      /* it reported corruption                 */
    int components_repaired;    /* and the repair succeeded               */
    int components_repair_failed;

    /* step 3: the orchestrator */
    int uso_present;            /* usoclient.exe exists                    */
    int uso_scan_ok;            /* it accepted a scan                      */
    int uso_download_ok;
    int uso_install_ok;
    int uso_progress;           /* download folder / flags moved          */

    /* step 4: the user session */
    int user_session;           /* someone is logged in                    */
    int user_session_covered;   /* its half of the engine was driven       */

    /* step 6: search + install through the agent */
    int online_attempted;       /* the online search was actually started  */
    int online_answered;        /* and it returned a result code           */
    int online_pending;         /* updates the service reported missing    */
    int agent_installed;        /* the agent installed something           */
    int install_failed;         /* the install attempt failed              */
    long install_hresult;       /* why: the HResult of the first hard failure */
    int install_timed_out;

    /* services */
    int services_started;       /* wuauserv / bits / UsoSvc came up         */

    /* restart */
    int reboot_before;          /* a restart was already pending when we started */
    int reboot_after;           /* a restart is pending now                */
    int restart_needed;         /* an install requires it                  */
} WuState;

/* ------------------------------------------------------------------ *
 *  per-step time estimates.  They drive the countdown in the UI; the
 *  engine overwrites the ones it knows better (an online search is not
 *  a local read), and 0 means "use the default".
 * ------------------------------------------------------------------ */
typedef struct {
    unsigned est_ms[WU_STEP_COUNT];
} WuEstimate;

/* ------------------------------------------------------------------ *
 *  the snapshot the UI reads (one struct, filled by wu_view())
 * ------------------------------------------------------------------ */
typedef struct {
    int  step_state[WU_STEP_COUNT];  /* WU_SS_*                          */
    int  step_reached[WU_STEP_COUNT];
    unsigned step_est_ms[WU_STEP_COUNT];
    int  current_step;               /* -1 when nothing is pending       */
    int  done_steps;                 /* resolved (done/warn/failed/skip) */
    int  phase;                      /* WU_P_*                           */
    int  verdict;                    /* WU_J_*                           */
    int  action;                     /* WU_A_*                           */
    int  severity;                   /* 0 clean, 1 notice, 2 warning, 3 attention */
    const char *verdict_text;        /* one sentence, operator facing    */
    const char *advice_text;         /* what to do next                  */
} WuView;

/* ------------------------------------------------------------------ *
 *  the decision layer
 * ------------------------------------------------------------------ */
void        wu_view(const WuState *st, const WuEstimate *ovr, WuView *out);
int         wu_known_pending(const WuState *st);   /* best known count  */
unsigned    wu_step_default_est_ms(int step);
const char *wu_step_title(int step);
const char *wu_step_note(int step);
const char *wu_verdict_id(int verdict);
const char *wu_action_id(int action);
const char *wu_phase_id(int phase);
const char *wu_step_state_id(int state);
int         wu_severity(int verdict);
const char *wu_verdict_text(int verdict);
const char *wu_advice_text(int verdict);

#ifdef __cplusplus
}
#endif

#endif /* PREP_WU_H */
