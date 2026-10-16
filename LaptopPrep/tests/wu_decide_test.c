/* =====================================================================
 *  LaptopPrep - Windows Update decision layer test
 *
 *  These are the field cases the stage has to get right, written as data.
 *  They are the reason the update code can be trusted at all: every one
 *  of them is a machine state that really happens on a refurb bench, and
 *  the expected verdict is the honest one, not the convenient one.
 *
 *  Build and run anywhere (no Windows needed):
 *      gcc -std=c99 -Wall -Wextra -I src tests/wu_decide_test.c src/prep_wu.c \
 *          -o build/wu_decide_test && ./build/wu_decide_test
 * ===================================================================== */
#include <stdio.h>
#include <string.h>
#include "prep_wu.h"

static int failures = 0;
static int checks = 0;

/* zeroed state with the two environment flags a bench machine has */
static WuState mk(const char *name)
{
    WuState s;
    (void)name;
    memset(&s, 0, sizeof(s));
    s.admin = 1;
    s.internet_likely = 1;
    return s;
}

/* every step the engine walked through */
static void walk(WuState *s, int first, int last)
{
    int i;
    for (i = first; i <= last && i < WU_STEP_COUNT; i++) s->reached[i] = 1;
}

static void walk_all(WuState *s) { walk(s, 0, WU_STEP_COUNT - 1); }

static const char *step_letters(const WuView *v)
{
    static char out[WU_STEP_COUNT * 2 + 1];
    static const char *ch = "qrdwfs";    /* queued running done warn failed skipped */
    int i;
    for (i = 0; i < WU_STEP_COUNT; i++) {
        int st = v->step_state[i];
        out[i * 2] = (st >= 0 && st <= 5) ? ch[st] : '?';
        out[i * 2 + 1] = (i + 1 < WU_STEP_COUNT) ? ' ' : 0;
    }
    out[WU_STEP_COUNT * 2 - 1] = 0;
    return out;
}

typedef struct {
    const char *name;
    const char *want_steps;      /* letters, as step_letters() prints */
    int         want_verdict;
    int         want_severity;
    int         want_phase;
    int         want_current;    /* -1 when finished */
    int         want_done;
} Expect;

static void check(const WuState *s, const Expect *e)
{
    WuView v;
    char got[64];
    WuEstimate est;
    int bad = 0;

    memset(&est, 0, sizeof(est));
    wu_view(s, &est, &v);
    snprintf(got, sizeof(got), "%s", step_letters(&v));

    checks++;
    if (strcmp(got, e->want_steps) != 0) {
        printf("   FAIL steps   : want [%s] got [%s]\n", e->want_steps, got);
        bad = 1;
    }
    if (v.verdict != e->want_verdict) {
        printf("   FAIL verdict : want %s got %s\n", wu_verdict_id(e->want_verdict),
               wu_verdict_id(v.verdict));
        bad = 1;
    }
    if (v.severity != e->want_severity) {
        printf("   FAIL severity: want %d got %d\n", e->want_severity, v.severity);
        bad = 1;
    }
    if (v.phase != e->want_phase) {
        printf("   FAIL phase   : want %s got %s\n", wu_phase_id(e->want_phase),
               wu_phase_id(v.phase));
        bad = 1;
    }
    if (v.current_step != e->want_current) {
        printf("   FAIL current : want %d got %d\n", e->want_current, v.current_step);
        bad = 1;
    }
    if (v.done_steps != e->want_done) {
        printf("   FAIL done    : want %d got %d\n", e->want_done, v.done_steps);
        bad = 1;
    }
    /* invariants that must hold in every possible state ---------------- */
    if (v.verdict != WU_J_NONE && v.verdict_text == NULL) {
        printf("   FAIL: a finished stage must always carry a verdict text\n");
        bad = 1;
    }
    if (v.advice_text == NULL || v.advice_text[0] == 0) {
        printf("   FAIL: a finished stage must always carry advice\n");
        bad = 1;
    }
    if (s->finished) {
        int i;
        if (v.current_step != -1) { printf("   FAIL: finished but a step is current\n"); bad = 1; }
        if (v.done_steps != WU_STEP_COUNT) {
            printf("   FAIL: finished but %d/%d steps resolved\n", v.done_steps,
                   WU_STEP_COUNT);
            bad = 1;
        }
        for (i = 0; i < WU_STEP_COUNT; i++)
            if (v.step_state[i] == WU_SS_QUEUED || v.step_state[i] == WU_SS_RUNNING) {
                printf("   FAIL: finished but step %d is still %s\n", i,
                       wu_step_state_id(v.step_state[i]));
                bad = 1;
            }
    }
    if (!s->started && v.verdict != WU_J_NONE) {
        printf("   FAIL: a stage that never started must not have a verdict\n");
        bad = 1;
    }
    if (bad) failures++;

    printf("   %-42s %-26s sev %d  %-18s current %2d  %d/%d  steps: %s\n",
           e->name, wu_verdict_id(v.verdict), v.severity, wu_phase_id(v.phase),
           v.current_step, v.done_steps, WU_STEP_COUNT, got);
}

static void announce(const char *what)
{
    printf("\n== %s ==\n", what);
}

int main(void)
{
    WuState s;
    Expect e;

    printf("LaptopPrep - Windows Update decision layer: field cases\n"
           "steps legend: q=queued r=running d=done w=warn(look) f=failed s=skipped\n");

    /* ---------------------------------------------------------------- *
     *  the good cases
     * ---------------------------------------------------------------- */
    announce("a machine that is genuinely up to date");
    s = mk("current");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1; s.store_count = 0;
    s.staged_payloads = 0;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 1; s.online_pending = 0;
    memset(&e, 0, sizeof(e));
    e.name = "current, verified online";
    e.want_steps = "d s s d d d s d";
    e.want_verdict = WU_J_SUCCESS; e.want_severity = 0; e.want_phase = WU_P_CURRENT;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the updates were installed and a restart finishes them");
    s = mk("restart");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1;
    s.uso_present = 1; s.uso_scan_ok = 1; s.uso_download_ok = 1; s.uso_install_ok = 1;
    s.agent_installed = 1; s.restart_needed = 1; s.reboot_after = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 1; s.online_pending = 6;
    memset(&e, 0, sizeof(e));
    e.name = "installed, restart required";
    e.want_steps = "d s s d d d d d";
    e.want_verdict = WU_J_SUCCESS_RESTART; e.want_severity = 1;
    e.want_phase = WU_P_RESTART; e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the machine arrived already waiting for a restart");
    s = mk("reboot-pending");
    s.started = 1; s.finished = 1;
    walk(&s, WU_S_SCAN, WU_S_SCAN);
    walk(&s, WU_S_FINAL, WU_S_FINAL);
    s.agent_present = 1; s.agent_answered = 1;
    s.reboot_before = 1; s.reboot_after = 1; s.restart_needed = 1;
    memset(&e, 0, sizeof(e));
    e.name = "restart was already pending";
    e.want_steps = "d s s s s s s d";
    e.want_verdict = WU_J_SUCCESS_RESTART; e.want_severity = 1;
    e.want_phase = WU_P_RESTART; e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("offline, but the packages were already on the disk (USB drop)");
    s = mk("offline-staged");
    s.internet_likely = 0;
    s.started = 1; s.finished = 1;
    walk(&s, WU_S_SCAN, WU_S_ORCH);
    walk(&s, WU_S_FINAL, WU_S_FINAL);
    s.agent_present = 1; s.agent_answered = 1;
    s.staged_payloads = 4; s.staged_installed = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.restart_needed = 1; s.reboot_after = 1;
    s.user_session = 0;
    memset(&e, 0, sizeof(e));
    e.name = "cached .msu installed without network";
    e.want_steps = "d d s d s s s d";
    e.want_verdict = WU_J_SUCCESS_RESTART; e.want_severity = 1;
    e.want_phase = WU_P_RESTART; e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("updates were downloaded from the local cache and installed");
    s = mk("cache-install");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.agent_installed = 1;                       /* installed via the agent */
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 1; s.online_pending = 0;
    memset(&e, 0, sizeof(e));
    e.name = "store said 4 pending, installed from cache";
    e.want_steps = "d s s d d d d d";
    e.want_verdict = WU_J_SUCCESS; e.want_severity = 0; e.want_phase = WU_P_CURRENT;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the payloads were faulty and DISM repaired them");
    s = mk("repaired");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1;
    s.components_checked = 1; s.components_faulty = 1; s.components_repaired = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 1; s.online_pending = 0;
    memset(&e, 0, sizeof(e));
    e.name = "component store repaired, now current";
    e.want_steps = "d s d d d d s d";
    e.want_verdict = WU_J_SUCCESS; e.want_severity = 0; e.want_phase = WU_P_CURRENT;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    /* ---------------------------------------------------------------- *
     *  the failures that used to be reported as success
     * ---------------------------------------------------------------- */
    announce("case (a) of the old bug: the online service never answered");
    s = mk("unanswered-online");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1; s.store_count = 0;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 0;   /* asked, no answer */
    memset(&e, 0, sizeof(e));
    e.name = "local store said 0, service silent";
    e.want_steps = "d s s d d d s d";
    e.want_verdict = WU_J_UNCONFIRMED; e.want_severity = 2; e.want_phase = WU_P_BLOCKED;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("case (b) of the old bug: the service accepted and never answered");
    s = mk("wedged");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_timed_out = 1; s.agent_answered = 0;
    s.components_checked = 1; s.components_faulty = 0;
    s.uso_present = 1; s.uso_scan_ok = 0;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1;
    s.install_timed_out = 1;
    memset(&e, 0, sizeof(e));
    e.name = "watchdog fired on the search";
    e.want_steps = "d s d w d f w d";
    e.want_verdict = WU_J_WEDGED; e.want_severity = 3; e.want_phase = WU_P_BLOCKED;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the update components are missing on this machine");
    s = mk("no-agent");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 0;
    s.components_checked = 1; s.components_faulty = 1; s.components_repair_failed = 1;
    s.uso_present = 0;
    s.user_session = 1; s.user_session_covered = 1;
    memset(&e, 0, sizeof(e));
    e.name = "agent missing, store damaged";
    e.want_steps = "f s f s d s s d";
    e.want_verdict = WU_J_BLOCKED; e.want_severity = 3; e.want_phase = WU_P_BLOCKED;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the agent answers every call with an error (broken components)");
    s = mk("agent-error");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_error = 1; s.agent_answered = 0;
    s.components_checked = 1; s.components_faulty = 0;     /* payloads were fine */
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    memset(&e, 0, sizeof(e));
    e.name = "agent errors out, payloads are fine";
    e.want_steps = "d s d d d f s d";
    e.want_verdict = WU_J_BLOCKED; e.want_severity = 3; e.want_phase = WU_P_BLOCKED;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("Windows refused the work because the tool is not elevated");
    s = mk("elevation");
    s.admin = 0;
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 0; s.access_denied = 1;
    s.uso_present = 1; s.uso_scan_ok = 0;
    s.user_session = 1; s.user_session_covered = 0;
    memset(&e, 0, sizeof(e));
    e.name = "not administrator";
    e.want_steps = "d s s w w w s d";
    e.want_verdict = WU_J_NEEDS_ELEVATION; e.want_severity = 2;
    e.want_phase = WU_P_BLOCKED; e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("there is no internet and nothing is cached on this machine");
    s = mk("offline-empty");
    s.internet_likely = 0;
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1; s.store_count = 0;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 0;
    memset(&e, 0, sizeof(e));
    e.name = "offline, no cached payloads";
    /* the local store still answered - it is the *online* check that could
     * not happen, and that is what the verdict says */
    e.want_steps = "d s s d d d s d";
    e.want_verdict = WU_J_OFFLINE; e.want_severity = 2; e.want_phase = WU_P_BLOCKED;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the updates are known but the install did not get them in");
    s = mk("unfinished");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 1; s.online_pending = 5;
    s.install_failed = 1;
    memset(&e, 0, sizeof(e));
    e.name = "5 updates found, install failed";
    e.want_steps = "d s s d d d f d";
    e.want_verdict = WU_J_UNFINISHED; e.want_severity = 2;
    e.want_phase = WU_P_BLOCKED; e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the install ran out of time with the cache still pending");
    s = mk("install-timeout");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1; s.store_count = 3;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.install_timed_out = 1;
    memset(&e, 0, sizeof(e));
    e.name = "install over its budget";
    e.want_steps = "d s s d d d w d";
    e.want_verdict = WU_J_UNFINISHED; e.want_severity = 2;
    e.want_phase = WU_P_BLOCKED; e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("the component repair could not fix the store");
    s = mk("repair-failed");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 0;
    s.components_checked = 1; s.components_faulty = 1; s.components_repair_failed = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 0;
    memset(&e, 0, sizeof(e));
    e.name = "DISM restorehealth failed";
    e.want_steps = "d s f d d w s d";
    e.want_verdict = WU_J_BLOCKED; e.want_severity = 3; e.want_phase = WU_P_BLOCKED;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    announce("a session was open but this run could not drive its half");
    s = mk("session-warn");
    s.started = 1; s.finished = 1;
    walk_all(&s);
    s.agent_present = 1; s.agent_answered = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 0;
    s.online_attempted = 1; s.online_answered = 1; s.online_pending = 0;
    memset(&e, 0, sizeof(e));
    e.name = "per-user half not covered, service says current";
    e.want_steps = "d s s d w d s d";
    e.want_verdict = WU_J_SUCCESS; e.want_severity = 0; e.want_phase = WU_P_CURRENT;
    e.want_current = -1; e.want_done = 8;
    check(&s, &e);

    /* ---------------------------------------------------------------- *
     *  the cases the UI lives in while it runs
     * ---------------------------------------------------------------- */
    announce("mid-run: still reading the local state");
    s = mk("mid-1");
    s.started = 1;
    walk(&s, WU_S_SCAN, WU_S_SCAN);
    s.agent_present = 1;
    memset(&e, 0, sizeof(e));
    e.name = "step 0 running";
    e.want_steps = "r q q q q q q q";
    e.want_verdict = WU_J_NONE; e.want_severity = 0; e.want_phase = WU_P_CHECKING;
    e.want_current = 0; e.want_done = 0;
    check(&s, &e);

    announce("mid-run: waiting on the online search (the long wait)");
    s = mk("mid-6");
    s.started = 1;
    walk(&s, WU_S_SCAN, WU_S_AGENT);
    s.agent_present = 1; s.agent_answered = 1;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    s.online_attempted = 1; s.online_answered = 0;
    memset(&e, 0, sizeof(e));
    e.name = "step 5 running";
    e.want_steps = "d s s d d r q q";
    e.want_verdict = WU_J_NONE; e.want_severity = 0; e.want_phase = WU_P_SEARCHING;
    e.want_current = 5; e.want_done = 5;
    check(&s, &e);

    announce("mid-run: downloading and installing (one update at a time)");
    s = mk("mid-7");
    s.started = 1;
    walk(&s, WU_S_SCAN, WU_S_INSTALL);
    s.agent_present = 1; s.agent_answered = 1; s.store_count = 3;
    s.uso_present = 1; s.uso_scan_ok = 1;
    s.user_session = 1; s.user_session_covered = 1;
    memset(&e, 0, sizeof(e));
    e.name = "step 6 running";
    e.want_steps = "d s s d d d r q";
    e.want_verdict = WU_J_NONE; e.want_severity = 0; e.want_phase = WU_P_INSTALLING;
    e.want_current = 6; e.want_done = 6;
    check(&s, &e);

    announce("a stage that was never started (fresh window)");
    s = mk("fresh");
    memset(&e, 0, sizeof(e));
    e.name = "idle";
    e.want_steps = "q q q q q q q q";
    e.want_verdict = WU_J_NONE; e.want_severity = 0; e.want_phase = WU_P_IDLE;
    e.want_current = -1; e.want_done = 0;
    check(&s, &e);

    printf("\n----------------------------------------------\n");
    printf("%d checks in 22 scenarios, %d failed\n", checks, failures);
    printf("(each scenario checks the 8 step states, the verdict, the severity,\n"
           " the phase, the current step and the step count)\n");
    return failures ? 1 : 0;
}
