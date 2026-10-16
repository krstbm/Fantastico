/* =====================================================================
 *  LaptopPrep - Windows Update decision layer  (prep_wu.c)
 *
 *  Plain C99, no Windows headers: see prep_wu.h for why.  Everything in
 *  here is a pure function of the evidence the engine collected, which is
 *  what makes it unit testable (tests/wu_decide_test.c) and what stops
 *  the UI from ever showing a verdict the evidence does not support.
 * ===================================================================== */
#include <string.h>
#include "prep_wu.h"

/* ------------------------------------------------------------------ *
 *  the steps, in the words an operator reads
 * ------------------------------------------------------------------ */
static const char *const TITLE[WU_STEP_COUNT] = {
    "Read what this machine already has",
    "Install packages that are already here",
    "Verify the update payloads",
    "Hand the work to the update orchestrator",
    "Check the current user session",
    "Ask Windows Update what is missing",
    "Download and install",
    "Restart check and record"
};

static const char *const NOTE[WU_STEP_COUNT] = {
    "the local update store, downloaded packages and restart flags",
    "wusa / dism on the .msu and .cab files already on this disk",
    "DISM payload check - the repair only runs when it is needed",
    "usoclient asks, then watches while Windows installs - it stops early "
    "when nothing is queued",
    "the per-user half of the update engine",
    "local cache first, then the online service",
    "one update at a time, every one named in the log",
    "verdict, next action and the record in the log"
};

const char *wu_step_title(int step)
{
    if (step < 0 || step >= WU_STEP_COUNT) return "?";
    return TITLE[step];
}

const char *wu_step_note(int step)
{
    if (step < 0 || step >= WU_STEP_COUNT) return "";
    return NOTE[step];
}

const char *wu_step_state_id(int state)
{
    switch (state) {
        case WU_SS_QUEUED:  return "queued";
        case WU_SS_RUNNING: return "running";
        case WU_SS_DONE:    return "done";
        case WU_SS_WARN:    return "look";
        case WU_SS_FAILED:  return "failed";
        case WU_SS_SKIPPED: return "skipped";
        default:            return "?";
    }
}

/* Defaults are deliberately generous on the slow steps: an estimate that
 * is too short makes the UI look stuck, one that is too long is harmless
 * because the step ends as soon as the real answer arrives. */
unsigned wu_step_default_est_ms(int step)
{
    switch (step) {
        case WU_S_SCAN:    return 10u * 1000u;
        case WU_S_STAGED:  return 90u * 1000u;
        case WU_S_HEALTH:  return 60u * 1000u;
        case WU_S_ORCH:    return 120u * 1000u;
        case WU_S_SESSION: return 20u * 1000u;
        case WU_S_AGENT:   return 90u * 1000u;
        case WU_S_INSTALL: return 120u * 1000u;
        case WU_S_FINAL:   return 2u * 1000u;
        default:           return 5u * 1000u;
    }
}

int wu_known_pending(const WuState *s)
{
    int n = s->store_count;
    if (s->online_pending > n) n = s->online_pending;
    return n;
}

/* ------------------------------------------------------------------ *
 *  the verdict
 * ------------------------------------------------------------------ */
int wu_severity(int verdict)
{
    switch (verdict) {
        case WU_J_NONE:
        case WU_J_SUCCESS:          return 0;
        case WU_J_SUCCESS_RESTART:  return 1;
        case WU_J_UNFINISHED:
        case WU_J_OFFLINE:
        case WU_J_UNCONFIRMED:
        case WU_J_NEEDS_ELEVATION:  return 2;
        case WU_J_BLOCKED:
        case WU_J_WEDGED:
        case WU_J_ERROR:            return 3;
        default:                    return 3;
    }
}

const char *wu_verdict_id(int verdict)
{
    switch (verdict) {
        case WU_J_NONE:             return "not-run";
        case WU_J_SUCCESS:          return "up-to-date";
        case WU_J_SUCCESS_RESTART:  return "installed-restart-needed";
        case WU_J_UNFINISHED:       return "updates-known-not-installed";
        case WU_J_BLOCKED:          return "update-components-missing";
        case WU_J_OFFLINE:          return "no-internet-nothing-cached";
        case WU_J_UNCONFIRMED:      return "not-confirmed-current";
        case WU_J_WEDGED:           return "update-service-wedged";
        case WU_J_NEEDS_ELEVATION:  return "needs-administrator";
        case WU_J_ERROR:            return "could-not-tell";
        default:                    return "unknown";
    }
}

const char *wu_action_id(int action)
{
    switch (action) {
        case WU_A_NONE:               return "none";
        case WU_A_UPLOAD_AND_RESTART: return "upload-and-restart";
        case WU_A_RESTART_LAPTOP:     return "restart-laptop";
        case WU_A_OPEN_UPDATE:        return "open-windows-update";
        case WU_A_FIX_UPDATE_ENGINE:  return "fix-update-engine";
        case WU_A_CHECK_NETWORK:      return "check-network";
        case WU_A_RUN_ELEVATED:       return "run-as-administrator";
        case WU_A_READ_LOG:           return "read-the-log";
        case WU_A_RUN_AGAIN:          return "run-again";
        default:                      return "unknown";
    }
}

const char *wu_phase_id(int phase)
{
    switch (phase) {
        case WU_P_IDLE:               return "idle";
        case WU_P_CHECKING:           return "checking-local-state";
        case WU_P_INSTALLING_CACHED:  return "installing-cached";
        case WU_P_VERIFYING:          return "verifying-payloads";
        case WU_P_ORCHESTRATOR:       return "orchestrator";
        case WU_P_SESSION:            return "user-session";
        case WU_P_SEARCHING:          return "searching";
        case WU_P_INSTALLING:         return "installing";
        case WU_P_RESTART:            return "restart-required";
        case WU_P_CURRENT:            return "up-to-date";
        case WU_P_BLOCKED:            return "could-not-complete";
        default:                      return "unknown";
    }
}

const char *wu_verdict_text(int verdict)
{
    switch (verdict) {
        case WU_J_SUCCESS:
            return "This laptop is up to date: Windows Update itself says there is "
                   "nothing for it.";
        case WU_J_SUCCESS_RESTART:
            return "The updates are installed. A restart finishes them.";
        case WU_J_UNFINISHED:
            return "Updates are known for this machine but they are not installed "
                   "yet.";
        case WU_J_BLOCKED:
            return "The Windows Update components on this machine are not working, "
                   "so no update path could run.";
        case WU_J_OFFLINE:
            return "There is no internet here and nothing is cached on this disk, "
                   "so the updates could not be checked.";
        case WU_J_UNCONFIRMED:
            return "Not confirmed: the local update store says nothing is pending "
                   "for this machine, but Windows Update never gave an answer - so "
                   "this is NOT a clean bill of health.";
        case WU_J_WEDGED:
            return "Windows accepted the update request and never answered: the "
                   "update service is wedged. A restart clears it.";
        case WU_J_NEEDS_ELEVATION:
            return "Windows refused the update work because LaptopPrep is not "
                   "running as administrator.";
        case WU_J_ERROR:
            return "The update stage finished without a clean answer - Windows "
                   "Update itself has to be looked at.";
        default:
            return "The update stage has not run yet.";
    }
}

const char *wu_advice_text(int verdict)
{
    switch (verdict) {
        case WU_J_SUCCESS:
            return "Nothing to do. The next laptop can be started.";
        case WU_J_SUCCESS_RESTART:
            return "Upload the log and restart this laptop - the reboot is what "
                   "completes the updates. It can be done at the end of the day "
                   "for a whole rack.";
        case WU_J_UNFINISHED:
            return "Run the prep again after a restart. If it fails in the same "
                   "place, use 'Fix Windows Update' (or fix-windows-update.bat as "
                   "administrator) and run again.";
        case WU_J_BLOCKED:
            return "Repair the update components first: 'Fix Windows Update' "
                   "re-registers the agent and restarts the update services. "
                   "Settings > Windows Update shows the same repair.";
        case WU_J_UNCONFIRMED:
            return "Run the prep again. If it stays unconfirmed, open Settings > "
                   "Windows Update once and let it scan on its own - that scan is "
                   "what usually unblocks the service.";
        case WU_J_OFFLINE:
            return "Give the laptop a network - a network cable is the quickest - "
                   "and run the prep again. Everything else has already been "
                   "collected.";
        case WU_J_WEDGED:
            return "Restart the laptop, then run the prep again. A wedged update "
                   "session never recovers without it.";
        case WU_J_NEEDS_ELEVATION:
            return "Close LaptopPrep and start it again as administrator (right "
                   "click > Run as administrator), then run the prep.";
        case WU_J_ERROR:
            return "Open Settings > Windows Update and use the built-in repair, "
                   "then run the prep again. The log has the whole story.";
        default:
            return "Press Run prep to check this machine.";
    }
}

/* ------------------------------------------------------------------ *
 *  the whole picture, from the evidence
 * ------------------------------------------------------------------ */
void wu_view(const WuState *s, const WuEstimate *ovr, WuView *v)
{
    int i, cur = -1, pending;

    memset(v, 0, sizeof(*v));
    v->current_step = -1;
    v->verdict = WU_J_NONE;

    if (!s->started) {
        v->phase = WU_P_IDLE;
        v->verdict_text = wu_verdict_text(WU_J_NONE);
        v->advice_text  = wu_advice_text(WU_J_NONE);
        for (i = 0; i < WU_STEP_COUNT; i++) {
            v->step_est_ms[i] = (ovr && ovr->est_ms[i]) ? ovr->est_ms[i]
                                : wu_step_default_est_ms(i);
            v->step_reached[i] = 0;
            v->step_state[i] = WU_SS_QUEUED;
        }
        return;
    }

    pending = wu_known_pending(s);

    /* ---- one rule set per step ------------------------------------ */
    for (i = 0; i < WU_STEP_COUNT; i++) {
        int st = WU_SS_QUEUED;
        if (s->reached[i]) {
            switch (i) {
                case WU_S_SCAN:
                    /* the agent is the whole tool chain: without it nothing
                     * downstream can be attempted honestly */
                    st = s->agent_present ? WU_SS_DONE : WU_SS_FAILED;
                    break;

                case WU_S_STAGED:
                    if (s->staged_installed)          st = WU_SS_DONE;
                    else if (s->staged_payloads <= 0) st = WU_SS_SKIPPED;
                    else if (s->staged_failed)        st = WU_SS_WARN;
                    else                              st = WU_SS_DONE;
                    break;

                case WU_S_HEALTH:
                    if (!s->components_checked)                    st = WU_SS_SKIPPED;
                    else if (s->components_faulty && s->components_repaired)
                                                                   st = WU_SS_DONE;
                    else if (s->components_repair_failed)          st = WU_SS_FAILED;
                    else if (s->components_faulty)                 st = WU_SS_WARN;
                    else                                           st = WU_SS_DONE;
                    break;

                case WU_S_ORCH:
                    if (!s->uso_present)                        st = WU_SS_SKIPPED;
                    else if (s->uso_scan_ok)                    st = WU_SS_DONE;
                    else if (s->access_denied && !s->admin)     st = WU_SS_WARN;
                    else                                        st = WU_SS_WARN;
                    break;

                case WU_S_SESSION:
                    if (!s->user_session)          st = WU_SS_SKIPPED;
                    else if (s->user_session_covered) st = WU_SS_DONE;
                    else                           st = WU_SS_WARN;
                    break;

                case WU_S_AGENT:
                    if (!s->agent_present)                        st = WU_SS_SKIPPED;
                    else if (s->agent_timed_out)                  st = WU_SS_FAILED;
                    else if (s->agent_error && !s->agent_answered) st = WU_SS_FAILED;
                    else if (s->agent_answered)                    st = WU_SS_DONE;
                    else                                           st = WU_SS_WARN;
                    break;

                case WU_S_INSTALL:
                    if (s->agent_installed || s->staged_installed) st = WU_SS_DONE;
                    else if (s->install_timed_out)                st = WU_SS_WARN;
                    else if (s->install_failed)                   st = WU_SS_FAILED;
                    else if (pending == 0)                        st = WU_SS_SKIPPED;
                    else                                          st = WU_SS_FAILED;
                    break;

                case WU_S_FINAL:
                    st = WU_SS_DONE;
                    break;
            }
        }
        v->step_state[i] = st;
        v->step_reached[i] = s->reached[i] ? 1 : 0;
        v->step_est_ms[i] = (ovr && ovr->est_ms[i]) ? ovr->est_ms[i]
                            : wu_step_default_est_ms(i);
    }

    /* a step the run never needed is not "queued", it is done with */
    if (s->finished) {
        for (i = 0; i < WU_STEP_COUNT; i++)
            if (!s->reached[i]) v->step_state[i] = WU_SS_SKIPPED;
    } else {
        /* the step being worked on right now is the newest one entered */
        for (i = 0; i < WU_STEP_COUNT; i++)
            if (s->reached[i]) cur = i;
        v->current_step = cur;
        if (cur >= 0) v->step_state[cur] = WU_SS_RUNNING;
    }

    for (i = 0; i < WU_STEP_COUNT; i++)
        if (v->step_state[i] == WU_SS_DONE || v->step_state[i] == WU_SS_WARN ||
            v->step_state[i] == WU_SS_FAILED || v->step_state[i] == WU_SS_SKIPPED)
            v->done_steps++;

    /* ---- the verdict, in priority order --------------------------- */
    if (!s->finished) {
        v->verdict = WU_J_NONE;
        v->action = WU_A_NONE;
        v->severity = 0;
    } else if (!s->agent_present) {
        v->verdict = WU_J_BLOCKED;
    } else if (s->components_repair_failed && !s->agent_answered) {
        v->verdict = WU_J_BLOCKED;
    } else if (s->agent_timed_out) {
        v->verdict = WU_J_WEDGED;
    } else if (s->agent_error && !s->agent_answered && !s->online_answered &&
               !s->agent_installed && !s->staged_installed) {
        /* the calls come back with an error: that is a broken component, and
         * "repair the update engine" fixes it - not patience and not a
         * restart (which is what WU_J_WEDGED is for) */
        v->verdict = WU_J_BLOCKED;
    } else if (s->reboot_after || s->restart_needed) {
        v->verdict = WU_J_SUCCESS_RESTART;
    } else if (s->agent_installed || s->staged_installed) {
        v->verdict = WU_J_SUCCESS;
    } else if (s->online_attempted && s->online_answered && pending == 0) {
        v->verdict = WU_J_SUCCESS;
    } else if (s->access_denied && !s->admin) {
        v->verdict = WU_J_NEEDS_ELEVATION;
    } else if (!s->internet_likely && pending == 0) {
        v->verdict = WU_J_OFFLINE;
    } else if (pending > 0) {
        v->verdict = WU_J_UNFINISHED;
    } else if (s->agent_answered) {
        /* The rule that the old build got wrong: the local store answering
         * "nothing pending" is NOT an up-to-date machine.  The stage only
         * claims that when the online service itself answered. */
        v->verdict = WU_J_UNCONFIRMED;
    } else if (!s->admin) {
        v->verdict = WU_J_NEEDS_ELEVATION;
    } else {
        v->verdict = WU_J_ERROR;
    }

    v->severity = wu_severity(v->verdict);

    switch (v->verdict) {
        case WU_J_SUCCESS_RESTART:  v->action = WU_A_UPLOAD_AND_RESTART; break;
        case WU_J_WEDGED:           v->action = WU_A_RESTART_LAPTOP;     break;
        case WU_J_UNFINISHED:
        case WU_J_BLOCKED:          v->action = WU_A_FIX_UPDATE_ENGINE;  break;
        case WU_J_UNCONFIRMED:      v->action = WU_A_RUN_AGAIN;          break;
        case WU_J_OFFLINE:          v->action = WU_A_CHECK_NETWORK;      break;
        case WU_J_NEEDS_ELEVATION:  v->action = WU_A_RUN_ELEVATED;       break;
        case WU_J_ERROR:            v->action = WU_A_OPEN_UPDATE;        break;
        default:                    v->action = WU_A_NONE;               break;
    }

    /* ---- the phase ------------------------------------------------- */
    if (s->finished) {
        switch (v->verdict) {
            case WU_J_SUCCESS_RESTART: v->phase = WU_P_RESTART;  break;
            case WU_J_SUCCESS:         v->phase = WU_P_CURRENT;  break;
            case WU_J_NONE:            v->phase = WU_P_IDLE;     break;
            default:                   v->phase = WU_P_BLOCKED;  break;
        }
    } else {
        switch (v->current_step) {
            case WU_S_SCAN:    v->phase = WU_P_CHECKING;           break;
            case WU_S_STAGED:  v->phase = WU_P_INSTALLING_CACHED;  break;
            case WU_S_HEALTH:  v->phase = WU_P_VERIFYING;          break;
            case WU_S_ORCH:    v->phase = WU_P_ORCHESTRATOR;       break;
            case WU_S_SESSION: v->phase = WU_P_SESSION;            break;
            case WU_S_AGENT:   v->phase = WU_P_SEARCHING;          break;
            case WU_S_INSTALL: v->phase = WU_P_INSTALLING;         break;
            default:           v->phase = WU_P_IDLE;               break;
        }
    }

    v->verdict_text = wu_verdict_text(v->verdict);
    v->advice_text  = wu_advice_text(v->verdict);
}
