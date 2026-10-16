/* =====================================================================
 *  LaptopPrep - console build (console_main.c)
 *  ---------------------------------------------------------------------
 *  The same engine, no window: for a rack of laptops driven by one script,
 *  for a scheduled task, or to see what a machine reports without a GUI.
 *
 *      LaptopPrep-console.exe --all
 *      LaptopPrep-console.exe --updates --upload
 *      LaptopPrep-console.exe --specs --devid --battery
 *      LaptopPrep-console.exe --status
 *
 *  Exit code: 0 everything clean, 1 something to look at, 2 needs
 *  attention (the update stage could not finish), 3 no run happened.
 *  That makes it usable as a check in a batch file.
 * ===================================================================== */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prep.h"          /* pulls winsock2.h before windows.h - order matters */
#include "resource.h"

/* ------------------------------------------------------------------ *
 *  the engine calls these four hooks; in this build they are the
 *  console's progress display (in the GUI build they drive the window)
 * ------------------------------------------------------------------ */
static const char *bit_name(unsigned bit)
{
    switch (bit) {
        case PREP_M_SPECS:     return "System specs and disks";
        case PREP_M_DEVICE_ID: return "Device identity (serials, MAC)";
        case PREP_M_BOARD:     return "Motherboard and BIOS";
        case PREP_M_EXTRA:     return "Full hardware sweep";
        case PREP_M_TPM:       return "TPM status";
        case PREP_M_BATTERY:   return "Battery report";
        case PREP_M_UPDATES:   return "Windows Update";
        case PREP_M_UPLOAD:    return "Upload the log";
        default:               return "?";
    }
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void gui_on_module_begin(unsigned bit)
{
    char line[160];
    snprintf(line, sizeof(line), "  .. %-32s", bit_name(bit));
    fputs(line, stdout);
    fflush(stdout);
}

void gui_on_module_result(unsigned bit, int ok)
{
    char line[128];
    snprintf(line, sizeof(line), "\r  %s %s\n", ok ? "OK" : "!!", bit_name(bit));
    fputs(line, stdout);
    fflush(stdout);
}

void gui_on_run_finished(int rc)
{
    (void)rc;
}

void gui_tick(void)
{
    /* the console has nothing to repaint; the engine uses it as a yield */
    Sleep(0);
}

/* ------------------------------------------------------------------ *
 *  the eight update steps, as a text table
 * ------------------------------------------------------------------ */
static void print_update_steps(void)
{
    int i, n = prep_update_step_count();
    const char *verdict = prep_update_verdict_text();
    const char *advice  = prep_update_advice_text();

    say("");
    say("  Windows Update - %d steps (%s)", n,
        prep_update_severity() == 0 ? "clean" :
        prep_update_severity() == 1 ? "notice" :
        prep_update_severity() == 2 ? "warning" : "needs attention");
    for (i = 0; i < n; i++) {
        int st = prep_update_step_state(i);
        long ms = prep_update_step_elapsed_ms(i);
        say("   %d. %-42s %-9s %s", i + 1, prep_update_step_title(i),
            st == WU_SS_DONE    ? "done" :
            st == WU_SS_WARN    ? "check" :
            st == WU_SS_FAILED  ? "FAILED" :
            st == WU_SS_SKIPPED ? "not needed" :
            st == WU_SS_RUNNING ? "running" : "queued",
            ms ? "" : "");
    }
    if (verdict && verdict[0]) {
        say("");
        say("  Verdict: %s", verdict);
    }
    if (advice && advice[0]) say("  Next   : %s", advice);
}

/* what did the last run leave behind?  (reads the tail of the log) */
static int show_status(void)
{
    const char *path = prep_log_path();
    FILE *f = fopen(path, "rb");
    long size;
    char buf[2048];
    char *p, *q;
    int found = 0;

    if (!f) {
        say("No log at %s yet - nothing has run on this machine.", path);
        return 3;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size > (long)sizeof(buf) - 1) size = (long)sizeof(buf) - 1;
    fseek(f, -size, SEEK_END);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 3; }
    buf[size] = 0;
    fclose(f);

    say("Log: %s", path);
    /* the last verdict, the last phase and the last few lines - walking
     * backwards keeps this honest even for a huge log */
    for (p = buf + size; p > buf; p--) {
        if ((p == buf || p[-1] == '\n') && !strncmp(p, "UPD: Verdict", 12)) {
            q = strchr(p, '\n');
            if (q) *q = 0;
            say("%s", p);
            found = 1;
            break;
        }
    }
    if (!found)
        say("The log has no finished update stage in it yet.");
    else {
        /* repeat of the advice line, so a script log is self-explanatory */
        for (p = buf + size; p > buf; p--) {
            if ((p == buf || p[-1] == '\n') && !strncmp(p, "UPD: Next", 9)) {
                q = strchr(p, '\n');
                if (q) *q = 0;
                say("%s", p);
                break;
            }
        }
    }
    return found ? 0 : 3;
}

static void usage(void)
{
    say("LaptopPrep " PREP_VERSION_STR " - console");
    say("");
    say("Usage: LaptopPrep-console.exe [options]");
    say("");
    say("  --all            everything the GUI runs by default");
    say("  --specs          CPU, RAM, Windows edition, disks");
    say("  --devid          serials, UUID, MAC addresses (into the log)");
    say("  --board          motherboard, BIOS");
    say("  --extra          GPU, memory modules, audio, network, USB");
    say("  --tpm            TPM status");
    say("  --battery        battery report");
    say("  --updates        the 8-step Windows Update stage");
    say("  --upload         upload the log to the configured endpoint");
    say("  --status         show what the last run on this machine said");
    say("  --help           this text");
    say("");
    say("Log directory: " PREP_LOGDIR "  (falls back to %%LOCALAPPDATA%% or %%TEMP%%)");
    say("Exit codes: 0 clean, 1 look at it, 2 needs attention, 3 nothing ran");
}

int main(int argc, char **argv)
{
    unsigned flags = 0;
    int i, sev;
    int asked = 0;

    prep_init();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "/?")) {
            usage();
            return 0;
        } else if (!strcmp(a, "--all")) {
            flags |= PREP_DEFAULT_FLAGS; asked = 1;
        } else if (!strcmp(a, "--specs"))     { flags |= PREP_M_SPECS;     asked = 1; }
        else if (!strcmp(a, "--devid"))       { flags |= PREP_M_DEVICE_ID; asked = 1; }
        else if (!strcmp(a, "--board"))       { flags |= PREP_M_BOARD;     asked = 1; }
        else if (!strcmp(a, "--extra"))       { flags |= PREP_M_EXTRA;     asked = 1; }
        else if (!strcmp(a, "--tpm"))         { flags |= PREP_M_TPM;       asked = 1; }
        else if (!strcmp(a, "--battery"))     { flags |= PREP_M_BATTERY;   asked = 1; }
        else if (!strcmp(a, "--updates"))     { flags |= PREP_M_UPDATES;   asked = 1; }
        else if (!strcmp(a, "--upload"))      { flags |= PREP_M_UPLOAD;    asked = 1; }
        else if (!strcmp(a, "--status"))      { return show_status(); }
        else {
            say("Unknown option: %s", a);
            usage();
            return 3;
        }
    }

    if (!asked) {
        usage();
        return 3;
    }

    say("LaptopPrep " PREP_VERSION_STR " - console - " PREP_OWNER);
    say(PREP_COPYRIGHT);
    say("Log: %s", prep_log_path());
    if (prep_log_notice()[0]) say("Note: %s", prep_log_notice());
    if (!is_admin())
        say("Note: not running as administrator - TPM, DISM and Windows Update "
            "will be refused by Windows.");
    say("");

    prep_run_all(flags);

    if (flags & PREP_M_UPDATES) {
        print_update_steps();
        sev = prep_update_severity();
    } else {
        sev = 0;
    }

    say("");
    say("Log written to %s", prep_log_path());
    if (!prep_update_verdict()) return 0;
    return sev >= 3 ? 2 : (sev > 0 ? 1 : 0);
}
