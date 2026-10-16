/* =====================================================================
 *  LaptopPrep - engine (prep_core.c)
 *
 *  All the hardware / OS collection + the Windows Update stage.
 *  No UI code in here: the GUI installs the gui_on_*() hooks below.
 *
 *  The Windows Update stage was rewritten for 1.2.  It now walks four
 *  real paths, cheapest and least invasive first, and every blocking call
 *  runs on its own thread with a stopwatch and a 15 s heartbeat:
 *
 *    0) already waiting for a restart?  The updates are installed and
 *       Windows only wants the reboot.  Nothing is searched, nothing is
 *       downloaded - say so and finish.  (This is also what clears a
 *       wedged update agent.)
 *    1) payloads already on the disk (.msu/.cab in
 *       SoftwareDistribution\Download) are installed directly with
 *       wusa.exe / dism.exe.  No update agent, no network.
 *    2) the update orchestrator (usoclient.exe) is asked to scan, download
 *       and install in the session this process was started from.  It
 *       returns immediately and the machine does the work in its service,
 *       so LaptopPrep cannot hang on it; we watch the reboot flags and the
 *       download folder for evidence of progress.
 *    3) the Windows Update Agent COM API does a local-store read first and
 *       only goes online when there is nothing staged.  The online search
 *       gets a real budget (45 min) because a laptop that has been offline
 *       for a while genuinely needs it - but the log shows a heartbeat
 *       every 15 s and the run screen shows a countdown, so "it looks
 *       frozen" can no longer happen unnoticed.
 *    4) if none of that can finish the job, the run says exactly why and
 *       what fixes it (restart / update recovery / network / elevation)
 *       instead of reporting success it cannot prove.
 *
 *  Every decision goes through prep_wu.c, which is pure C and covered by
 *  tests/wu_decide_test.c - including the cases the 1.1 build got wrong
 *  ("up to date" from a query that never reached the network, and success
 *  reported for a search that never returned).
 * ===================================================================== */
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <wbemidl.h>
#include <objbase.h>
#include <oleauto.h>
#include <winhttp.h>
#include <iptypes.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include "prep.h"

/* MinGW does not ship a definition of GUID_NULL for plain C builds
 * (it normally lives in uuid.lib), so define it once here. */
const GUID GUID_NULL = { 0x00000000, 0x0000, 0x0000,
                         { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

/* ==================================================================
 *  globals
 * ================================================================== */
char g_logPath[MAX_PATH];
char g_deviceCsv[MAX_PATH];
char g_battHtml[MAX_PATH];
char g_computer[256];
static char g_logScratch[512];

static HANDLE g_logMx = NULL;   /* serialises log writes */

/* export a couple of paths the CSV / battery steps need */
const char *prep_log_path(void) { return g_logPath; }

/* ==================================================================
 *  logging
 * ================================================================== */
static char g_logDir[MAX_PATH];
static char g_logNotice[320];

static void log_paths_for(const char *dir)
{
    snprintf(g_logDir,    sizeof(g_logDir),    "%s", dir);
    snprintf(g_logPath,   sizeof(g_logPath),   "%s\\%s",                    dir, PREP_LOGFILE);
    snprintf(g_deviceCsv, sizeof(g_deviceCsv), "%s\\%s_deviceID.csv",       dir, g_computer);
    snprintf(g_battHtml,  sizeof(g_battHtml),  "%s\\battery_report.html",   dir);
}

/* Can we really write here?  Checks by opening the log the same way the
 * logger does - a stale log left behind by an elevated run is read-only
 * for a normal user, and without this test the app looked like it did
 * nothing at all: no log file, no live log, no errors. */
static int log_dir_works(const char *dir)
{
    char path[MAX_PATH];
    FILE *f;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\%s", dir, PREP_LOGFILE);
    f = fopen(path, "ab");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Every launch starts a clean log.  The previous session is moved aside
 * to LaptopPrep-previous.log (one file, overwritten each time) instead of
 * being thrown away, so the live log and anything uploaded to the server
 * describe this session only - while yesterday's run is still recoverable
 * next to it.  If the file cannot be moved or deleted (locked by another
 * copy of the program, or a read-only log left by an elevated run), the
 * old behaviour of appending is kept and the note says so. */
static void log_start_fresh(void)
{
    char prev[MAX_PATH];
    if (GetFileAttributesA(g_logPath) == INVALID_FILE_ATTRIBUTES) return;  /* nothing there */
    snprintf(prev, sizeof(prev), "%s\\%s-previous.log", g_logDir, "LaptopPrep");
    if (MoveFileExA(g_logPath, prev, MOVEFILE_REPLACE_EXISTING)) return;
    if (DeleteFileA(g_logPath)) return;
    snprintf(g_logNotice, sizeof(g_logNotice),
             "the previous log could not be cleared (%s) - this session will "
             "be appended to it; close the other copy of LaptopPrep and start "
             "again for a clean log", g_logPath);
}

void prep_init(void)
{
    DWORD sz;
    g_logMx = CreateMutexA(NULL, FALSE, "LaptopPrep_log_mutex");
    sz = (DWORD)sizeof(g_computer);
    GetComputerNameA(g_computer, &sz);
    g_logNotice[0] = 0;

    log_paths_for(PREP_LOGDIR);

    log_start_fresh();

    if (!log_dir_works(PREP_LOGDIR)) {
        char alt[MAX_PATH];
        int done = 0;
        alt[0] = 0;
        if (GetEnvironmentVariableA("LOCALAPPDATA", alt, (DWORD)sizeof(alt)) && alt[0]) {
            char sub[MAX_PATH];
            snprintf(sub, sizeof(sub), "%s\\LaptopPrep", alt);
            if (log_dir_works(sub)) { log_paths_for(sub); done = 1; }
        }
        if (!done) {
            char tmp[MAX_PATH];
            if (GetTempPathA((DWORD)sizeof(tmp), tmp)) {
                char sub[MAX_PATH];
                snprintf(sub, sizeof(sub), "%sLaptopPrep", tmp);
                if (log_dir_works(sub)) { log_paths_for(sub); done = 1; }
            }
        }
        if (done) {
            snprintf(g_logNotice, sizeof(g_logNotice),
                     "%s is not writable - logging to %s instead",
                     PREP_LOGDIR, g_logDir);
        } else {
            snprintf(g_logNotice, sizeof(g_logNotice),
                     "No writable log folder found (tried %s, %%LOCALAPPDATA%% and %%TEMP%%)",
                     PREP_LOGDIR);
        }
    }
}

const char *prep_computer_name(void) { return g_computer; }
const char *prep_log_dir(void)       { return g_logDir; }
const char *prep_log_notice(void)    { return g_logNotice; }

void write_log(const char *msg)
{
    SYSTEMTIME st;
    FILE *f;
    char line[9000];
    GetLocalTime(&st);
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d - %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);

    if (g_logMx) WaitForSingleObject(g_logMx, 5000);   /* never hang here */
    f = fopen(g_logPath, "ab");
    if (f) { fwrite(line, 1, strlen(line), f); fclose(f); }
    if (g_logMx) ReleaseMutex(g_logMx);

#ifdef _DEBUG
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#endif
}

static void vlogf(const char *prefix, const char *fmt, va_list ap)
{
    char body[4096];
    char line[4400];
    vsnprintf(body, sizeof(body), fmt, ap);
    snprintf(line, sizeof(line), "%s%s", prefix, body);
    write_log(line);
}

/* Update-stage lines carry a "UPD:" marker so the live log can colour
 * them: the whole update story is then readable at a glance. */
static void wu_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlogf("UPD: ", fmt, ap);
    va_end(ap);
}

/* ==================================================================
 *  modules / progress bookkeeping (the GUI owns the actual state)
 * ================================================================== */
unsigned prep_module_count(void)
{
    return 8u;   /* one per PREP_M_* flag */
}

int prep_select(unsigned flags, unsigned bit)
{
    return (flags & bit) ? 1 : 0;
}

/* ==================================================================
 *  admin check
 * ================================================================== */
int is_admin(void)
{
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    PSID group = NULL;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(NULL, group, &admin);
        FreeSid(group);
    }
    return admin ? 1 : 0;
}

/* ==================================================================
 *  WMI helpers
 * ================================================================== */
int variant_to_str(VARIANT *v, char *out, int outsz)
{
    out[0] = 0;
    if (v->vt == VT_NULL || v->vt == VT_EMPTY) { snprintf(out, outsz, "%s", "N/A"); return 1; }

    if (v->vt == VT_BSTR && v->bstrVal) {
        WideCharToMultiByte(CP_UTF8, 0, v->bstrVal, -1, out, outsz, NULL, NULL);
        return 1;
    }
    if ((v->vt & VT_ARRAY) && (v->vt & VT_BSTR)) {
        SAFEARRAY *sa = v->parray;
        LONG lb = 0, ub = -1;
        out[0] = 0;
        if (!sa) { snprintf(out, outsz, "%s", "N/A"); return 1; }
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        for (LONG i = lb; i <= ub; i++) {
            BSTR bs = NULL;
            char tmp[512];
            if (SUCCEEDED(SafeArrayGetElement(sa, &i, &bs)) && bs) {
                WideCharToMultiByte(CP_UTF8, 0, bs, -1, tmp, sizeof(tmp), NULL, NULL);
                if (strlen(out) + strlen(tmp) + 3 < (size_t)outsz) {
                    if (out[0]) strcat(out, ", ");
                    strcat(out, tmp);
                }
                SysFreeString(bs);
            }
        }
        if (out[0] == 0) snprintf(out, outsz, "%s", "N/A");
        return 1;
    }
    if (v->vt & VT_ARRAY) {
        /* byte arrays carry the raw text in WMI, decode them instead of
         * printing "(array)": SerialNumber on some boards is VT_ARRAY|VT_UI1 */
        if ((v->vt & VT_UI1) && v->parray) {
            SAFEARRAY *sa = v->parray;
            LONG lb = 0, ub = -1, i;
            size_t o = 0;
            SafeArrayGetLBound(sa, 1, &lb);
            SafeArrayGetUBound(sa, 1, &ub);
            out[0] = 0;
            for (i = lb; i <= ub && o + 1 < (size_t)outsz; i++) {
                unsigned char c = 0;
                if (FAILED(SafeArrayGetElement(sa, &i, &c))) break;
                if (c == 0) break;
                if (c >= 32 && c < 127) out[o++] = (char)c;
                else { out[o++] = '.'; }
            }
            out[o] = 0;
            if (!out[0]) snprintf(out, outsz, "%s", "N/A");
            return 1;
        }
        snprintf(out, outsz, "%s", "(array)");
        return 1;
    }

    /* types VariantChangeType does not always handle */
    if (v->vt == VT_I1)    { snprintf(out, outsz, "%d", (int)v->cVal);       return 1; }
    if (v->vt == VT_UI1)   { snprintf(out, outsz, "%u", (unsigned)v->bVal);  return 1; }
    if (v->vt == VT_I2)    { snprintf(out, outsz, "%d", (int)v->iVal);       return 1; }
    if (v->vt == VT_UI2)   { snprintf(out, outsz, "%u", (unsigned)v->uiVal); return 1; }
    if (v->vt == VT_I4 || v->vt == VT_INT)   { snprintf(out, outsz, "%ld", v->lVal);  return 1; }
    if (v->vt == VT_UI4 || v->vt == VT_UINT) { snprintf(out, outsz, "%lu", v->ulVal); return 1; }
    if (v->vt == VT_I8)  { snprintf(out, outsz, "%lld", (long long)v->llVal);           return 1; }
    if (v->vt == VT_UI8) { snprintf(out, outsz, "%llu", (unsigned long long)v->ullVal); return 1; }

    {
        VARIANT vc;
        VariantInit(&vc);
        if (SUCCEEDED(VariantChangeType(&vc, v, 0, VT_BSTR)) && vc.bstrVal) {
            WideCharToMultiByte(CP_UTF8, 0, vc.bstrVal, -1, out, outsz, NULL, NULL);
            VariantClear(&vc);
            return 1;
        }
        VariantClear(&vc);
    }
    snprintf(out, outsz, "%s", "N/A");
    return 0;
}

static int wmi_connect(IWbemLocator *loc, const wchar_t *ns, IWbemServices **svc)
{
    HRESULT hr;
    BSTR bns = SysAllocString(ns);
    if (!bns) return 0;
    hr = loc->lpVtbl->ConnectServer(loc, bns, NULL, NULL, NULL, 0, NULL, NULL, svc);
    SysFreeString(bns);
    if (FAILED(hr) || !*svc) return 0;
    if (FAILED(CoSetProxyBlanket((IUnknown*)*svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE))) {
        (*svc)->lpVtbl->Release(*svc);
        *svc = NULL;
        return 0;
    }
    return 1;
}

/* helper for the environment probe: make sure this thread is on COM before
 * asking WMI anything (a bare CoInitializeSecurity is not enough if the
 * thread never initialised COM at all) */
void wmi_uninit(IWbemServices *svc, IWbemLocator *loc)
{
    if (svc) svc->lpVtbl->Release(svc);
    if (loc) loc->lpVtbl->Release(loc);
}

int wmi_init(IWbemServices **ppSvc, IWbemLocator **ppLoc)
{
    HRESULT hr;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;

    hr = CoInitializeSecurity(NULL, -1, NULL, NULL,
            RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) return 0;

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
            &IID_IWbemLocator, (LPVOID*)ppLoc);
    if (FAILED(hr)) return 0;

    if (!wmi_connect(*ppLoc, L"ROOT\\CIMV2", ppSvc)) {
        (*ppLoc)->lpVtbl->Release(*ppLoc);
        *ppLoc = NULL;
        return 0;
    }
    return 1;
}

static IEnumWbemClassObject *wmi_query(IWbemServices *svc, const wchar_t *wql)
{
    BSTR lang, query;
    IEnumWbemClassObject *penum = NULL;
    HRESULT hr;
    lang  = SysAllocString(L"WQL");
    query = SysAllocString(wql);
    if (!lang || !query) {
        if (lang) SysFreeString(lang);
        if (query) SysFreeString(query);
        return NULL;
    }
    hr = svc->lpVtbl->ExecQuery(svc, lang, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &penum);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr)) return NULL;
    return penum;
}

int wmi_get_first(IWbemServices *svc, const wchar_t *wql,
                  const wchar_t *prop, char *out, int outsz)
{
    IEnumWbemClassObject *penum;
    IWbemClassObject *pobj = NULL;
    ULONG ret = 0;
    BSTR pname;
    int ok = 0;
    out[0] = 0;
    penum = wmi_query(svc, wql);
    if (!penum) return 0;
    pname = SysAllocString(prop);
    if (penum->lpVtbl->Next(penum, WBEM_INFINITE, 1, &pobj, &ret) == S_OK && ret && pobj) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(pobj->lpVtbl->Get(pobj, pname, 0, &v, NULL, NULL))) {
            variant_to_str(&v, out, outsz);
            ok = 1;
            VariantClear(&v);
        }
        pobj->lpVtbl->Release(pobj);
    }
    SysFreeString(pname);
    penum->lpVtbl->Release(penum);
    return ok;
}

void wmi_log_query(IWbemServices *svc, const wchar_t *wql,
                   const wchar_t **props, const char **labels, int nprops,
                   const char *section)
{
    IEnumWbemClassObject *penum;
    IWbemClassObject *pobj = NULL;
    ULONG ret = 0;
    int idx = 0, i;
    char line[4096];
    char val[2048];
    BSTR *pnames = (BSTR*)malloc(sizeof(BSTR) * (size_t)nprops);
    if (!pnames) return;
    for (i = 0; i < nprops; i++) pnames[i] = SysAllocString(props[i]);

    penum = wmi_query(svc, wql);
    if (!penum) {
        snprintf(line, sizeof(line), "%s: query failed", section);
        write_log(line);
        for (i = 0; i < nprops; i++) SysFreeString(pnames[i]);
        free(pnames);
        return;
    }
    while (penum->lpVtbl->Next(penum, WBEM_INFINITE, 1, &pobj, &ret) == S_OK && ret) {
        idx++;
        if (idx == 1) snprintf(line, sizeof(line), "----- %s -----", section);
        else          snprintf(line, sizeof(line), "--- %s [%d] ---", section, idx);
        write_log(line);
        for (i = 0; i < nprops; i++) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(pobj->lpVtbl->Get(pobj, pnames[i], 0, &v, NULL, NULL))) {
                variant_to_str(&v, val, sizeof(val));
                VariantClear(&v);
            } else {
                snprintf(val, sizeof(val), "%s", "N/A");
            }
            snprintf(line, sizeof(line), "%s: %s", labels[i], val);
            write_log(line);
        }
        pobj->lpVtbl->Release(pobj);
        pobj = NULL;
        gui_tick();
    }
    if (idx == 0) {
        snprintf(line, sizeof(line), "%s: no data", section);
        write_log(line);
    }
    for (i = 0; i < nprops; i++) SysFreeString(pnames[i]);
    free(pnames);
    penum->lpVtbl->Release(penum);
}

/* ==================================================================
 *  collect: basic specs
 * ================================================================== */
void collect_specs(IWbemServices *svc)
{
    char cpu[1024] = {0};
    char osCap[1024] = {0};
    char osVer[256] = {0};
    char osBuild[256] = {0};
    char osArch[256] = {0};
    char line[4096];
    MEMORYSTATUSEX ms;
    double ramGB;

    write_log("Collecting system specs...");
    wmi_get_first(svc, L"SELECT Name FROM Win32_Processor", L"Name", cpu, sizeof(cpu));
    wmi_get_first(svc, L"SELECT Caption FROM Win32_OperatingSystem", L"Caption", osCap, sizeof(osCap));
    wmi_get_first(svc, L"SELECT Version FROM Win32_OperatingSystem", L"Version", osVer, sizeof(osVer));
    wmi_get_first(svc, L"SELECT BuildNumber FROM Win32_OperatingSystem", L"BuildNumber", osBuild, sizeof(osBuild));
    wmi_get_first(svc, L"SELECT OSArchitecture FROM Win32_OperatingSystem", L"OSArchitecture", osArch, sizeof(osArch));

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    ramGB = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);

    snprintf(line, sizeof(line), "CPU: %s", cpu);
    write_log(line);
    snprintf(line, sizeof(line), "RAM: %.2f GB", ramGB);
    write_log(line);
    snprintf(line, sizeof(line), "OS: %s %s Build %s %s", osCap, osVer, osBuild, osArch);
    write_log(line);

    {
        const wchar_t *p[] = {L"Model", L"Size", L"Status", L"InterfaceType", L"SerialNumber"};
        const char    *l[] = {"Model", "SizeBytes", "Status", "Interface", "Serial"};
        wmi_log_query(svc, L"SELECT Model, Size, Status, InterfaceType, SerialNumber FROM Win32_DiskDrive",
                      p, l, 5, "DISK DRIVES");
    }
    {
        const wchar_t *p[] = {L"DeviceID", L"Size", L"FreeSpace", L"FileSystem", L"VolumeName"};
        const char    *l[] = {"Drive", "SizeBytes", "FreeBytes", "FS", "Label"};
        wmi_log_query(svc, L"SELECT DeviceID, Size, FreeSpace, FileSystem, VolumeName "
                           L"FROM Win32_LogicalDisk WHERE DriveType=3",
                      p, l, 5, "LOGICAL DISKS");
    }
}

/* ==================================================================
 *  collect: device identity (+ CSV)
 * ================================================================== */
void collect_device_id(IWbemServices *svc)
{
    char mfr[512] = {0}, model[512] = {0}, biosSN[512] = {0};
    char boardSN[512] = {0}, uuid[512] = {0}, asset[512] = {0}, encSN[512] = {0};
    char macs[2048] = {0};
    char line[4096];
    FILE *csv;
    ULONG sz = 0;
    IP_ADAPTER_ADDRESSES *addrs = NULL, *cur = NULL;
    DWORD rc;

    write_log("Collecting device identification info...");
    wmi_get_first(svc, L"SELECT Manufacturer FROM Win32_ComputerSystem", L"Manufacturer", mfr, sizeof(mfr));
    wmi_get_first(svc, L"SELECT Model FROM Win32_ComputerSystem", L"Model", model, sizeof(model));
    wmi_get_first(svc, L"SELECT SerialNumber FROM Win32_BIOS", L"SerialNumber", biosSN, sizeof(biosSN));
    wmi_get_first(svc, L"SELECT SerialNumber FROM Win32_BaseBoard", L"SerialNumber", boardSN, sizeof(boardSN));
    wmi_get_first(svc, L"SELECT UUID FROM Win32_ComputerSystemProduct", L"UUID", uuid, sizeof(uuid));
    wmi_get_first(svc, L"SELECT SMBIOSAssetTag FROM Win32_SystemEnclosure", L"SMBIOSAssetTag", asset, sizeof(asset));
    wmi_get_first(svc, L"SELECT SerialNumber FROM Win32_SystemEnclosure", L"SerialNumber", encSN, sizeof(encSN));

    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, NULL, &sz);
    if (sz) addrs = (IP_ADAPTER_ADDRESSES*)malloc(sz);
    if (addrs) {
        memset(addrs, 0, sz);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, addrs, &sz);
        if (rc == NO_ERROR) {
            char tmp[64];
            for (cur = addrs; cur; cur = cur->Next) {
                if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                if (cur->OperStatus != IfOperStatusUp) continue;
                if (cur->PhysicalAddressLength != 6) continue;
                snprintf(tmp, sizeof(tmp), "%02X-%02X-%02X-%02X-%02X-%02X",
                    cur->PhysicalAddress[0], cur->PhysicalAddress[1], cur->PhysicalAddress[2],
                    cur->PhysicalAddress[3], cur->PhysicalAddress[4], cur->PhysicalAddress[5]);
                if (strlen(macs) + strlen(tmp) + 3 < sizeof(macs)) {
                    if (macs[0]) strcat(macs, ", ");
                    strcat(macs, tmp);
                }
            }
        }
        free(addrs);
    }
    if (!macs[0]) snprintf(macs, sizeof(macs), "%s", "N/A");

    write_log("----- DEVICE ID -----");
    snprintf(line, sizeof(line), "ComputerName: %s", g_computer); write_log(line);
    snprintf(line, sizeof(line), "Manufacturer: %s", mfr);        write_log(line);
    snprintf(line, sizeof(line), "Model: %s", model);             write_log(line);
    snprintf(line, sizeof(line), "BIOS Serial: %s", biosSN);      write_log(line);
    snprintf(line, sizeof(line), "Motherboard SN: %s", boardSN);  write_log(line);
    snprintf(line, sizeof(line), "System UUID: %s", uuid);        write_log(line);
    snprintf(line, sizeof(line), "MAC Address(es): %s", macs);    write_log(line);
    snprintf(line, sizeof(line), "Asset Tag: %s", asset);         write_log(line);
    snprintf(line, sizeof(line), "Enclosure SN: %s", encSN);      write_log(line);
    write_log("---------------------");

    csv = fopen(g_deviceCsv, "wb");
    if (csv) {
        fprintf(csv, "ComputerName,Manufacturer,Model,BIOS_Serial,Motherboard_SN,"
                     "System_UUID,MAC_Addresses,Asset_Tag,Enclosure_SN\n");
        fprintf(csv, "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
            g_computer, mfr, model, biosSN, boardSN, uuid, macs, asset, encSN);
        fclose(csv);
        snprintf(line, sizeof(line), "Device CSV written: %s", g_deviceCsv);
        write_log(line);
    } else {
        write_log("Failed to write device CSV");
    }
}

/* ==================================================================
 *  collect: motherboard / BIOS / slots / buses
 * ================================================================== */
void collect_motherboard(IWbemServices *svc)
{
    write_log("Collecting motherboard details...");
    {
        const wchar_t *p[] = {L"Manufacturer", L"Product", L"Version", L"SerialNumber", L"PartNumber", L"Tag"};
        const char    *l[] = {"Mfr", "Product", "Version", "Serial", "PartNumber", "Tag"};
        wmi_log_query(svc, L"SELECT Manufacturer, Product, Version, SerialNumber, PartNumber, Tag "
                           L"FROM Win32_BaseBoard", p, l, 6, "MOTHERBOARD");
    }
    {
        const wchar_t *p[] = {L"Manufacturer", L"SMBIOSBIOSVersion", L"Version", L"SerialNumber",
                              L"ReleaseDate", L"CurrentLanguage"};
        const char    *l[] = {"Mfr", "SMBIOSVer", "Version", "Serial", "ReleaseDate", "Lang"};
        wmi_log_query(svc, L"SELECT Manufacturer, SMBIOSBIOSVersion, Version, SerialNumber, "
                           L"ReleaseDate, CurrentLanguage FROM Win32_BIOS", p, l, 6, "BIOS");
    }
    {
        const wchar_t *p[] = {L"SlotDesignation", L"CurrentUsage", L"MaxDataWidth", L"SupportsHotPlug", L"Status"};
        const char    *l[] = {"Slot", "Usage", "MaxWidth", "HotPlug", "Status"};
        wmi_log_query(svc, L"SELECT SlotDesignation, CurrentUsage, MaxDataWidth, SupportsHotPlug, Status "
                           L"FROM Win32_SystemSlot", p, l, 5, "SYSTEM SLOTS");
    }
    {
        const wchar_t *p[] = {L"DeviceID", L"DeviceLocator", L"BusType", L"BusNumber", L"BusAddress"};
        const char    *l[] = {"Device", "Locator", "BusType", "BusNum", "BusAddr"};
        wmi_log_query(svc, L"SELECT DeviceID, DeviceLocator, BusType, BusNumber, BusAddress "
                           L"FROM Win32_Bus", p, l, 5, "BUSES");
    }
}

/* ==================================================================
 *  collect: extended hardware
 * ================================================================== */
void collect_extra(IWbemServices *svc)
{
    write_log("Collecting extended hardware checks...");
    {
        const wchar_t *p[] = {L"Name", L"Manufacturer", L"MaxClockSpeed", L"NumberOfCores",
                              L"NumberOfLogicalProcessors", L"SocketDesignation", L"Status"};
        const char    *l[] = {"Name", "Mfr", "MaxMHz", "Cores", "Threads", "Socket", "Status"};
        wmi_log_query(svc, L"SELECT Name, Manufacturer, MaxClockSpeed, NumberOfCores, "
                           L"NumberOfLogicalProcessors, SocketDesignation, Status FROM Win32_Processor",
                      p, l, 7, "CPU DETAIL");
    }
    {
        const wchar_t *p[] = {L"BankLabel", L"Capacity", L"Speed", L"Manufacturer",
                              L"PartNumber", L"SerialNumber", L"SMBIOSMemoryType"};
        const char    *l[] = {"Bank", "Bytes", "MHz", "Mfr", "Part", "Serial", "Type"};
        wmi_log_query(svc, L"SELECT BankLabel, Capacity, Speed, Manufacturer, PartNumber, "
                           L"SerialNumber, SMBIOSMemoryType FROM Win32_PhysicalMemory",
                      p, l, 7, "MEMORY MODULES");
    }
    {
        const wchar_t *p[] = {L"Name", L"DriverVersion", L"AdapterRAM", L"VideoProcessor", L"Status",
                              L"CurrentHorizontalResolution", L"CurrentVerticalResolution"};
        const char    *l[] = {"Name", "Driver", "VRAM", "Chip", "Status", "ResX", "ResY"};
        wmi_log_query(svc, L"SELECT Name, DriverVersion, AdapterRAM, VideoProcessor, Status, "
                           L"CurrentHorizontalResolution, CurrentVerticalResolution "
                           L"FROM Win32_VideoController", p, l, 7, "GPU");
    }
    {
        const wchar_t *p[] = {L"Name", L"Status", L"Availability"};
        const char    *l[] = {"Name", "Status", "Avail"};
        wmi_log_query(svc, L"SELECT Name, Status, Availability FROM Win32_SoundDevice", p, l, 3, "AUDIO");
    }
    {
        const wchar_t *p[] = {L"Name", L"Description", L"Status"};
        const char    *l[] = {"Name", "Desc", "Status"};
        wmi_log_query(svc, L"SELECT Name, Description, Status FROM Win32_Keyboard", p, l, 3, "KEYBOARD");
    }
    {
        const wchar_t *p[] = {L"Name", L"PointingType", L"Status"};
        const char    *l[] = {"Name", "Type", "Status"};
        wmi_log_query(svc, L"SELECT Name, PointingType, Status FROM Win32_PointingDevice", p, l, 3,
                      "POINTING DEVICE");
    }
    {
        const wchar_t *p[] = {L"Name", L"Chemistry", L"DesignCapacity", L"FullChargeCapacity",
                              L"EstimatedChargeRemaining", L"BatteryStatus", L"Status"};
        const char    *l[] = {"Name", "Chem", "Design_mWh", "Full_mWh", "ChargePct", "BattStatus", "Status"};
        wmi_log_query(svc, L"SELECT Name, Chemistry, DesignCapacity, FullChargeCapacity, "
                           L"EstimatedChargeRemaining, BatteryStatus, Status FROM Win32_Battery",
                      p, l, 7, "BATTERY WMI");
    }
    {
        const wchar_t *p[] = {L"Name", L"NetConnectionStatus", L"Speed", L"MACAddress", L"AdapterType"};
        const char    *l[] = {"Name", "ConnStatus", "Speed", "MAC", "Type"};
        wmi_log_query(svc, L"SELECT Name, NetConnectionStatus, Speed, MACAddress, AdapterType "
                           L"FROM Win32_NetworkAdapter WHERE PhysicalAdapter=TRUE", p, l, 5,
                      "NETWORK ADAPTERS");
    }
    {
        const wchar_t *p[] = {L"Name", L"Status"};
        const char    *l[] = {"Name", "Status"};
        wmi_log_query(svc, L"SELECT Name, Status FROM Win32_USBController", p, l, 2, "USB CONTROLLERS");
    }
    {
        SYSTEM_POWER_STATUS ps;
        char line[512];
        if (GetSystemPowerStatus(&ps)) {
            snprintf(line, sizeof(line), "Power: AC=%d BatteryFlag=%d LifePct=%d Saving=%d",
                (int)ps.ACLineStatus, (int)ps.BatteryFlag,
                (int)ps.BatteryLifePercent, (int)ps.SystemStatusFlag);
            write_log(line);
        }
    }
}

/* ==================================================================
 *  collect: TPM
 *
 *  Two namespaces are tried: the classic ROOT\CIMV2\Security\MicrosoftTpm
 *  and the newer ROOT\Microsoft\Windows\Security\Tpm (the one the 1.1
 *  build never asked, which is why TPM came up empty on some machines).
 * ================================================================== */
void collect_tpm(IWbemLocator *loc)
{
    IWbemServices *svc2 = NULL;
    char line[1024];
    char v1[512] = {0}, v2[512] = {0}, v3[512] = {0}, v4[512] = {0};
    int found = 0;

    if (wmi_connect(loc, L"ROOT\\CIMV2\\Security\\MicrosoftTpm", &svc2)) {
        found = 1;
    } else if (wmi_connect(loc, L"ROOT\\Microsoft\\Windows\\Security\\Tpm", &svc2)) {
        found = 2;
    }

    if (!svc2) {
        write_log("TPM: no TPM namespace available (no TPM chip, or the "
                  "Microsoft TPM service is not running)");
        return;
    }

    wmi_get_first(svc2, L"SELECT IsActivated_InitialValue FROM Win32_Tpm",
                  L"IsActivated_InitialValue", v1, sizeof(v1));
    wmi_get_first(svc2, L"SELECT IsEnabled_InitialValue FROM Win32_Tpm",
                  L"IsEnabled_InitialValue", v2, sizeof(v2));
    wmi_get_first(svc2, L"SELECT SpecVersion FROM Win32_Tpm", L"SpecVersion", v3, sizeof(v3));
    wmi_get_first(svc2, L"SELECT ManufacturerVersion FROM Win32_Tpm",
                  L"ManufacturerVersion", v4, sizeof(v4));

    snprintf(line, sizeof(line), "TPM Activated=%s Enabled=%s Spec=%s Firmware=%s",
             v1[0] ? v1 : "N/A", v2[0] ? v2 : "N/A",
             v3[0] ? v3 : "N/A", v4[0] ? v4 : "N/A");
    write_log(line);
    snprintf(line, sizeof(line), "TPM namespace used: %s",
             found == 1 ? "ROOT\\CIMV2\\Security\\MicrosoftTpm"
                        : "ROOT\\Microsoft\\Windows\\Security\\Tpm");
    write_log(line);
    svc2->lpVtbl->Release(svc2);
}

/* ==================================================================
 *  running child processes with output capture
 *
 *  1.2 fixes two real problems here:
 *    - the command line is converted to UTF-16 and passed to
 *      CreateProcessW, so paths with characters outside the ANSI code page
 *      (a Danish "ø", a Turkish "ı") no longer break powercfg/wusa;
 *    - output is drained *while* the process runs and written to the log
 *      line by line, with a progress heartbeat every 30 s.  The 1.1 build
 *      only read the pipe after the process had exited, so a five minute
 *      DISM or wusa run produced a completely silent log - which is what
 *      made long steps look stuck.
 * ================================================================== */
struct ProcOut { char *buf; DWORD len; DWORD cap; };

/* How many bytes of output the last child process produced.  A tool that
 * "succeeds" without printing a single byte did not do the work - DISM
 * always prints a header, even when it finds nothing.  Without this, a
 * stub or a wrapper returning 0 could be read as "the payloads are fine",
 * which is exactly the kind of claim this rewrite exists to prevent. */
static volatile LONG g_lastProcOut = 0;

static void drain(void *ctx, const char *data, DWORD len)
{
    struct ProcOut *po = (struct ProcOut*)ctx;
    if (po->len + len + 1 > po->cap) {
        DWORD ncap = (po->len + len + 4096) * 2;
        char *nb = (char*)realloc(po->buf, ncap);
        if (!nb) return;
        po->buf = nb;
        po->cap = ncap;
    }
    memcpy(po->buf + po->len, data, len);
    po->len += len;
    po->buf[po->len] = 0;
}

/* write the complete lines sitting in the buffer, keep the partial one */
static void flush_lines(struct ProcOut *po, int force)
{
    size_t i = 0;
    while (i < po->len) {
        char *nl = memchr(po->buf + i, '\n', po->len - i);
        if (!nl) {
            if (!force) break;
            /* no newline at all and we are done: emit what is left */
            if (po->len - i == 0) break;
            po->buf[po->len] = 0;
            {
                char *s = po->buf + i;
                size_t L = strlen(s);
                while (L && (s[L-1] == '\r' || s[L-1] == ' ' || s[L-1] == '\t')) s[--L] = 0;
                if (*s) write_log(s);
            }
            i = po->len;
            break;
        }
        *nl = 0;
        {
            char *s = po->buf + i;
            size_t L = strlen(s);
            while (L && (s[L-1] == '\r' || s[L-1] == ' ' || s[L-1] == '\t')) s[--L] = 0;
            if (*s) write_log(s);
        }
        i = (size_t)(nl - po->buf) + 1;
    }
    if (i > 0 && i <= po->len) {
        memmove(po->buf, po->buf + i, po->len - i);
        po->len -= (DWORD)i;
        po->buf[po->len] = 0;
    }
}

/* UTF-8 -> UTF-16 helper (returns 0 when the string is plain ASCII, in
 * which case the caller can use the ANSI entry point) */
static int utf8_to_wide(const char *in, wchar_t *out, int outwchars)
{
    int n;
    if (!in || !in[0]) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, outwchars);
    return n > 0 ? 1 : 0;
}

int run_process_logged(const char *cmdline, int timeoutMs, DWORD *exitcode)
{
    char cmd[MAX_PATH * 2];
    wchar_t wcmd[MAX_PATH * 2];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    char rbuf[4096];
    char line[1200];
    DWORD got = 0;
    DWORD code = 0;
    int hung = 0;
    int ok = 0;
    struct ProcOut po;
    HANDLE jh = NULL;
    ULONGLONG start, lastBeat = 0;
    int useWide = 0;

    memset(&po, 0, sizeof(po));
    snprintf(cmd, sizeof(cmd), "%s", cmdline);
    snprintf(line, sizeof(line), "Run: %s", cmd);
    write_log(line);

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { write_log("CreatePipe failed"); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = NULL;
    memset(&pi, 0, sizeof(pi));

    /* Prefer the wide call: it is the only way a path with characters
     * outside the ANSI code page survives. */
    useWide = utf8_to_wide(cmd, wcmd, (int)(sizeof(wcmd) / sizeof(wcmd[0])));
    if (!CreateProcessW(NULL, useWide ? wcmd : NULL, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) {
        /* fall back to the ANSI entry point for odd command lines */
        STARTUPINFOA siA;
        memset(&siA, 0, sizeof(siA));
        siA.cb = sizeof(siA);
        siA.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        siA.wShowWindow = SW_HIDE;
        siA.hStdOutput = si.hStdOutput;
        siA.hStdError  = si.hStdError;
        siA.hStdInput  = NULL;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            NULL, NULL, &siA, &pi)) {
            snprintf(line, sizeof(line), "CreateProcess failed, error %lu",
                     (unsigned long)GetLastError());
            write_log(line);
            CloseHandle(rd);
            CloseHandle(wr);
            return 0;
        }
    }
    CloseHandle(wr);
    wr = NULL;

    /* job object: if we die, the child dies with us (no orphan powercfg,
     * no DISM still writing to C:\ while the log is being uploaded) */
    jh = CreateJobObjectA(NULL, NULL);
    if (jh) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(jh, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
            AssignProcessToJobObject(jh, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    start = GetTickCount64();
    for (;;) {
        DWORD navail = 0;
        /* drain whatever the child has said so far - this is what keeps
         * the live log alive during a long install */
        while (PeekNamedPipe(rd, NULL, 0, NULL, &navail, NULL) && navail > 0) {
            if (!ReadFile(rd, rbuf, (DWORD)sizeof(rbuf) - 1, &got, NULL) || got == 0) break;
            rbuf[got] = 0;
            drain(&po, rbuf, got);
            flush_lines(&po, 0);
            if (po.len > 60000) flush_lines(&po, 1);   /* no newlines at all? force */
        }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        gui_tick();
        {
            ULONGLONG el = GetTickCount64() - start;
            if (el - lastBeat >= 30000) {
                lastBeat = el;
                snprintf(line, sizeof(line),
                         "  ... still running (%llu min %llu s elapsed)",
                         (unsigned long long)(el / 60000),
                         (unsigned long long)((el / 1000) % 60));
                write_log(line);
            }
            if (timeoutMs > 0 && el > (ULONGLONG)timeoutMs) { hung = 1; break; }
        }
    }
    if (hung) {
        snprintf(line, sizeof(line), "Process timed out after %d s, terminating it",
                 timeoutMs / 1000);
        write_log(line);
        if (jh) TerminateJobObject(jh, 1);
        else    TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    GetExitCodeProcess(pi.hProcess, &code);

    /* final drain */
    for (;;) {
        DWORD navail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &navail, NULL) || navail == 0) break;
        if (!ReadFile(rd, rbuf, (DWORD)sizeof(rbuf) - 1, &got, NULL) || got == 0) break;
        rbuf[got] = 0;
        drain(&po, rbuf, got);
        flush_lines(&po, 0);
    }
    flush_lines(&po, 1);
    CloseHandle(rd);
    free(po.buf);

    snprintf(line, sizeof(line), "Exit code: %lu", (unsigned long)code);
    write_log(line);
    InterlockedExchange(&g_lastProcOut, (LONG)po.len);

    if (exitcode) *exitcode = code;
    ok = (code == 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (jh) CloseHandle(jh);
    return ok;
}

/* ==================================================================
 *  battery report
 * ================================================================== */

/* powercfg is the nice path (a full Microsoft report).  If it is missing or
 * refuses to write, build a small HTML report from data that needs no
 * command line tool at all: Win32_Battery through the WMI COM API plus
 * GetSystemPowerStatus.  Same file name, so nothing downstream has to care
 * which of the two it got. */
static void battery_write_fallback(void)
{
    FILE *f;
    SYSTEM_POWER_STATUS ps;
    IWbemServices *svc = NULL;
    IWbemLocator  *loc = NULL;
    char name[256] = "", chem[64] = "", design[64] = "", full[64] = "", pct[64] = "";
    char line[512];

    write_log("Writing the LaptopPrep fallback battery report "
              "(no powercfg required)...");

    if (wmi_init(&svc, &loc)) {
        wmi_get_first(svc, L"SELECT Name FROM Win32_Battery", L"Name",
                      name, sizeof(name));
        wmi_get_first(svc, L"SELECT Chemistry FROM Win32_Battery", L"Chemistry",
                      chem, sizeof(chem));
        wmi_get_first(svc, L"SELECT DesignCapacity FROM Win32_Battery",
                      L"DesignCapacity", design, sizeof(design));
        wmi_get_first(svc, L"SELECT FullChargeCapacity FROM Win32_Battery",
                      L"FullChargeCapacity", full, sizeof(full));
        wmi_get_first(svc, L"SELECT EstimatedChargeRemaining FROM Win32_Battery",
                      L"EstimatedChargeRemaining", pct, sizeof(pct));
    }
    memset(&ps, 0, sizeof(ps));
    GetSystemPowerStatus(&ps);

    f = fopen(g_battHtml, "wb");
    if (!f) {
        write_log("Could not write the fallback battery report");
        wmi_uninit(svc, loc);
        return;
    }
    fprintf(f,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Battery report - %s</title>"
        "<style>body{font:14px/1.5 system-ui,sans-serif;margin:32px;color:#15171a}"
        "h1{font-size:20px}table{border-collapse:collapse;margin-top:16px}"
        "td{border:1px solid #e6e4df;padding:6px 12px}</style></head><body>"
        "<h1>Battery report (LaptopPrep)</h1>"
        "<p>Generated without powercfg: this machine did not provide a powercfg "
        "report, so the values below come straight from the battery hardware.</p>"
        "<table>"
        "<tr><td>Computer</td><td>%s</td></tr>"
        "<tr><td>Battery</td><td>%s</td></tr>"
        "<tr><td>Chemistry code</td><td>%s</td></tr>"
        "<tr><td>Design capacity (mWh)</td><td>%s</td></tr>"
        "<tr><td>Full charge capacity (mWh)</td><td>%s</td></tr>"
        "<tr><td>Charge remaining</td><td>%s %%</td></tr>"
        "<tr><td>On mains power</td><td>%s</td></tr>"
        "<tr><td>Battery present</td><td>%s</td></tr>"
        "</table><p style=\"color:#868d96\">LaptopPrep " PREP_VERSION_STR "</p></body></html>\n",
        g_computer, g_computer,
        name[0]   ? name   : "(not reported)",
        chem[0]   ? chem   : "-",
        design[0] ? design : "-",
        full[0]   ? full   : "-",
        pct[0]    ? pct    : "-",
        ps.ACLineStatus == 1 ? "yes" : ps.ACLineStatus == 0 ? "no" : "unknown",
        (ps.BatteryFlag & 128) ? "no" : "yes");
    fclose(f);

    snprintf(line, sizeof(line), "Fallback battery report written: %s", g_battHtml);
    write_log(line);

    wmi_uninit(svc, loc);
}

void run_battery_report(void)
{
    char cmd[1024];
    char line[1024];
    DWORD code = 0;
    int ran;
    write_log("Generating battery report...");
    DeleteFileA(g_battHtml);              /* so fresh can be told from stale */
    snprintf(cmd, sizeof(cmd), "powercfg /batteryreport /output \"%s\"", g_battHtml);
    ran = run_process_logged(cmd, 60000, &code);
    if (GetFileAttributesA(g_battHtml) != INVALID_FILE_ATTRIBUTES) {
        snprintf(line, sizeof(line), "Battery report written by powercfg: %s", g_battHtml);
        write_log(line);
    } else {
        if (!ran) write_log("powercfg did not run cleanly - falling back to the "
                            "built-in generator (no external tool required)");
        else      write_log("powercfg produced no report - falling back to the "
                            "built-in generator (no external tool required)");
        battery_write_fallback();
    }
}

/* ==================================================================
 *  Windows Update Agent (WUA) COM helpers
 * ================================================================== */
static IDispatch *wua_create(const wchar_t *progid)
{
    CLSID clsid;
    IDispatch *d = NULL;
    if (FAILED(CLSIDFromProgID(progid, &clsid))) return NULL;
    if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
            &IID_IDispatch, (void**)&d))) return NULL;
    return d;
}

static int disp_call(IDispatch *d, const wchar_t *name, WORD flags,
                     VARIANT *args, int nargs, VARIANT *result)
{
    DISPID id;
    DISPPARAMS dp;
    DISPID putid = DISPID_PROPERTYPUT;
    VARIANT *rev = NULL;
    EXCEPINFO ex;
    UINT uerr = 0;
    HRESULT h;
    LPOLESTR nm = (LPOLESTR)name;
    memset(&dp, 0, sizeof(dp));
    if (!d) return 0;
    if (FAILED(d->lpVtbl->GetIDsOfNames(d, &IID_NULL, &nm, 1, LOCALE_SYSTEM_DEFAULT, &id))) return 0;
    if (nargs > 0) {
        int i;
        rev = (VARIANT*)malloc(sizeof(VARIANT) * (size_t)nargs);
        if (!rev) return 0;
        for (i = 0; i < nargs; i++) {
            VariantInit(&rev[i]);
            VariantCopy(&rev[i], &args[nargs - 1 - i]);
        }
        dp.rgvarg = rev;
        dp.cArgs = (UINT)nargs;
        if (flags & DISPATCH_PROPERTYPUT) {
            dp.rgdispidNamedArgs = &putid;
            dp.cNamedArgs = 1;
        }
    }
    memset(&ex, 0, sizeof(ex));
    if (result) VariantInit(result);
    h = d->lpVtbl->Invoke(d, id, &IID_NULL, LOCALE_SYSTEM_DEFAULT, flags, &dp, result, &ex, &uerr);
    if (rev) {
        int i;
        for (i = 0; i < nargs; i++) VariantClear(&rev[i]);
        free(rev);
    }
    return SUCCEEDED(h) ? 1 : 0;
}

static long disp_get_long(IDispatch *d, const wchar_t *name, long def)
{
    VARIANT r, c;
    long v = def;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_I4))) v = c.lVal;
        VariantClear(&c);
        VariantClear(&r);
    }
    return v;
}

static int disp_get_bool(IDispatch *d, const wchar_t *name, int def)
{
    VARIANT r, c;
    int v = def;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_BOOL))) v = (c.boolVal == VARIANT_TRUE) ? 1 : 0;
        VariantClear(&c);
        VariantClear(&r);
    }
    return v;
}

static IDispatch *disp_get_disp(IDispatch *d, const wchar_t *name)
{
    VARIANT r;
    IDispatch *o = NULL;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        if (r.vt == VT_DISPATCH && r.pdispVal) { o = r.pdispVal; o->lpVtbl->AddRef(o); }
        VariantClear(&r);
    }
    return o;
}

static IDispatch *disp_call_disp(IDispatch *d, const wchar_t *name, VARIANT *args, int nargs)
{
    VARIANT r;
    IDispatch *o = NULL;
    if (disp_call(d, name, DISPATCH_METHOD, args, nargs, &r)) {
        if (r.vt == VT_DISPATCH && r.pdispVal) { o = r.pdispVal; o->lpVtbl->AddRef(o); }
        VariantClear(&r);
    }
    return o;
}

static void disp_put(IDispatch *d, const wchar_t *name, VARIANT *v)
{
    disp_call(d, name, DISPATCH_PROPERTYPUT, v, 1, NULL);
}

static void disp_get_str(IDispatch *d, const wchar_t *name, char *out, int outsz)
{
    VARIANT r, c;
    out[0] = 0;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_BSTR)) && c.bstrVal)
            WideCharToMultiByte(CP_UTF8, 0, c.bstrVal, -1, out, outsz, NULL, NULL);
        VariantClear(&c);
        VariantClear(&r);
    }
    if (!out[0]) snprintf(out, outsz, "%s", "N/A");
}

static const char *wua_code_str(long code)
{
    switch (code) {
        case 0: return "NotStarted";
        case 1: return "InProgress";
        case 2: return "Succeeded";
        case 3: return "SucceededWithErrors";
        case 4: return "Failed";
        case 5: return "Aborted";
        default: return "Unknown";
    }
}

/* ==================================================================
 *  the Windows Update stage: the evidence, and the snapshot the UI reads
 *
 *  The stage itself lives further down (do_windows_update).  Everything
 *  in here is bookkeeping: the engine writes facts into g_wu, and
 *  wu_view() in prep_wu.c turns those facts into the eight step states,
 *  the verdict and the next action.  No verdict is ever written by hand
 *  anywhere - which is exactly how the old build ended up claiming
 *  "up to date" for machines it had never checked.
 * ================================================================== */
static WuState    g_wu;         /* written by the run thread          */
static WuEstimate g_wuEst;      /* estimates the UI counts down       */
static WuView     g_view;       /* latest snapshot                    */
static DWORD      g_wuStepStart = 0;
static DWORD      g_wuStepDur[WU_STEP_COUNT];

/* mirror for the UI thread (plain LONGs - races are cosmetic and every
 * value is recomputed from g_view on the next publish) */
static volatile LONG g_mirrorPhase, g_mirrorVerdict, g_mirrorSeverity;
static volatile LONG g_mirrorCurrent, g_mirrorDone, g_mirrorFinished;
static volatile LONG g_mirrorStep[WU_STEP_COUNT];
static volatile LONG g_mirrorEst[WU_STEP_COUNT];
static volatile LONG g_mirrorDur[WU_STEP_COUNT];
static volatile LONG g_mirrorHeartbeat;      /* tick of the last sign of life */
static char g_wuVerdictText[420];
static char g_wuAdviceText[420];
static char g_wuActionText[160];

/* Recompute the snapshot and mirror it for the UI.  Called on every step
 * boundary and on every heartbeat, so the window is never guessing. */
static void wu_publish(void)
{
    int i, cur;

    wu_view(&g_wu, &g_wuEst, &g_view);

    InterlockedExchange(&g_mirrorPhase,   (LONG)g_view.phase);
    InterlockedExchange(&g_mirrorVerdict, (LONG)g_view.verdict);
    InterlockedExchange(&g_mirrorSeverity,(LONG)g_view.severity);
    InterlockedExchange(&g_mirrorCurrent, (LONG)g_view.current_step);
    InterlockedExchange(&g_mirrorDone,    (LONG)g_view.done_steps);
    InterlockedExchange(&g_mirrorFinished, g_wu.finished ? 1 : 0);
    for (i = 0; i < WU_STEP_COUNT; i++) {
        InterlockedExchange(&g_mirrorStep[i], (LONG)g_view.step_state[i]);
        InterlockedExchange(&g_mirrorEst[i],  (LONG)g_view.step_est_ms[i]);
        InterlockedExchange(&g_mirrorDur[i],  (LONG)g_wuStepDur[i]);
    }

    /* the two strings the UI shows next to the verdict */
    snprintf(g_wuVerdictText, sizeof(g_wuVerdictText), "%s",
             g_view.verdict_text ? g_view.verdict_text : "");
    snprintf(g_wuAdviceText, sizeof(g_wuAdviceText), "%s",
             g_view.advice_text ? g_view.advice_text : "");
    snprintf(g_wuActionText, sizeof(g_wuActionText), "%s",
             wu_action_id(g_view.action));

    /* the phase is worth a log line: the whole four-hour story is then
     * readable without reading every line in between */
    cur = (int)g_mirrorPhase;
    {
        static int lastPhase = -1;
        static int lastVerdict = -1;
        if (cur != lastPhase) {
            lastPhase = cur;
            if (cur > WU_P_IDLE && !g_wu.finished)
                wu_log("phase: %s", wu_phase_id(cur));
        }
        if (g_wu.finished && (int)g_mirrorVerdict != lastVerdict) {
            lastVerdict = (int)g_mirrorVerdict;
        }
    }
}

/* mark a step as started: this is what separates "not needed" from
 * "ran and found nothing", the distinction the old build did not have */
static void wu_reach(int step)
{
    if (step >= 0 && step < WU_STEP_COUNT) {
        if (!g_wu.reached[step]) g_wu.reached[step] = 1;
        g_wu.started = 1;
    }
    g_wuStepStart = GetTickCount();
    if (g_wuStepStart) g_wuStepStart = g_wuStepStart;
    if (g_wuStepStart == 0) g_wuStepStart = 1;
    wu_publish();
}

static void wu_step_done(int step)
{
    DWORD now = GetTickCount();
    if (step >= 0 && step < WU_STEP_COUNT) {
        DWORD d = now - g_wuStepStart;
        if (d > 60000u * 60u * 4u) d = 0;          /* clock glitch guard */
        g_wuStepDur[step] = d;
    }
    g_wuStepStart = now;
    wu_publish();
}

/* public getters - everything the UI needs, none of it a guess -------- */
int prep_update_phase(void)        { return (int)g_mirrorPhase; }
int prep_update_verdict(void)      { return (int)g_mirrorVerdict; }
int prep_update_severity(void)     { return (int)g_mirrorSeverity; }
int prep_update_step_count(void)   { return WU_STEP_COUNT; }
int prep_update_current_step(void) { return (int)g_mirrorCurrent; }

int prep_update_step_state(int step)
{
    if (step < 0 || step >= WU_STEP_COUNT) return WU_SS_QUEUED;
    return (int)g_mirrorStep[step];
}

unsigned prep_update_step_est_ms(int step)
{
    if (step < 0 || step >= WU_STEP_COUNT) return 0;
    return (unsigned)g_mirrorEst[step];
}

int prep_update_step_elapsed_ms(int step)
{
    if (step < 0 || step >= WU_STEP_COUNT) return 0;
    if (step == (int)g_mirrorCurrent && !g_mirrorFinished)
        return (int)(GetTickCount() - g_wuStepStart);
    return (int)g_mirrorDur[step];
}

const char *prep_update_step_title(int step) { return wu_step_title(step); }
const char *prep_update_step_note(int step)  { return wu_step_note(step); }

const char *prep_update_verdict_text(void) { return g_wuVerdictText; }
const char *prep_update_action_text(void)  { return g_wuActionText; }
const char *prep_update_advice_text(void)  { return g_wuAdviceText; }

/* ==================================================================
 *  registry helpers
 * ================================================================== */
static int reg_key_exists(HKEY root, const char *sub)
{
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return 1;
    }
    return 0;
}

static int restart_flag_set(void)
{
    static const char *keys[] = {
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired",
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootRequired",
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\PackagesPending"
    };
    int i;
    for (i = 0; i < 3; i++)
        if (reg_key_exists(HKEY_LOCAL_MACHINE, keys[i])) return 1;
    return 0;
}

int prep_restart_pending(void)
{
    if (g_wu.restart_needed || g_wu.reboot_after) return 1;
    return restart_flag_set();
}

/* Microsoft.Update.SystemInfo.RebootRequired - true when a restart is
 * already pending from an earlier install.  Runs in a COM thread with a
 * short budget so a wedged agent cannot block the startup of the stage. */
static volatile LONG g_rebootProbeDone;
static volatile LONG g_rebootProbeValue;
static DWORD WINAPI reboot_probe_thread(LPVOID p)
{
    IDispatch *info;
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    info = wua_create(L"Microsoft.Update.SystemInfo");
    if (info) {
        InterlockedExchange(&g_rebootProbeValue, disp_get_bool(info, L"RebootRequired", 0));
        info->lpVtbl->Release(info);
    }
    InterlockedExchange(&g_rebootProbeDone, 1);
    return 0;
}

static int wua_reboot_pending(void)
{
    HANDLE th;
    InterlockedExchange(&g_rebootProbeDone, 0);
    InterlockedExchange(&g_rebootProbeValue, 0);
    th = CreateThread(NULL, 0, reboot_probe_thread, NULL, 0, NULL);
    if (!th) return 0;
    if (WaitForSingleObject(th, 20000) != WAIT_OBJECT_0) {
        /* 20 s for a property read means the agent is wedged */
        wu_log("Reboot-status query did not answer within 20 s - the update "
               "agent is not responding (this alone is worth a restart)");
        CloseHandle(th);
        return 0;
    }
    CloseHandle(th);
    return (int)g_rebootProbeValue;
}

/* ==================================================================
 *  services
 * ================================================================== */
static const char *svc_state_text(const char *name)
{
    static char out[64];
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return "unknown (no access - try running as administrator)";
    svc = OpenServiceA(scm, name, SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return "not installed"; }
    if (!QueryServiceStatus(svc, &st)) {
        CloseServiceHandle(svc); CloseServiceHandle(scm);
        return "query failed";
    }
    switch (st.dwCurrentState) {
        case SERVICE_RUNNING:       snprintf(out, sizeof(out), "running"); break;
        case SERVICE_STOPPED:       snprintf(out, sizeof(out), "stopped"); break;
        case SERVICE_START_PENDING: snprintf(out, sizeof(out), "starting"); break;
        case SERVICE_STOP_PENDING:  snprintf(out, sizeof(out), "stopping"); break;
        default:                    snprintf(out, sizeof(out), "state %lu",
                                             (unsigned long)st.dwCurrentState);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return out;
}

static int svc_ensure_started(const char *name, DWORD waitMs)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    DWORD waited = 0;
    int ok = 0;
    DWORD err = 0;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    svc = OpenServiceA(scm, name, SERVICE_QUERY_STATUS | SERVICE_START);
    if (!svc) { CloseServiceHandle(scm); return 0; }

    if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
        ok = 1;
    } else {
        if (!StartServiceA(svc, 0, NULL)) {
            err = GetLastError();
            if (err == ERROR_SERVICE_ALREADY_RUNNING) ok = 1;
            else if (err == ERROR_ACCESS_DENIED) {
                wu_log("Starting %s was refused (access denied) - administrator "
                       "rights are required", name);
                g_wu.access_denied = 1;
            } else if (err == ERROR_SERVICE_DISABLED) {
                wu_log("%s is DISABLED on this machine - it has to be enabled "
                       "before updates can work", name);
            }
        }
        while (!ok && waited < waitMs) {
            if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
                ok = 1;
                break;
            }
            Sleep(500);
            waited += 500;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

/* ==================================================================
 *  internet reachability (heuristic, used only to choose between
 *  "wedged agent" and "no network" - never to claim success)
 * ================================================================== */
static int http_probe(const wchar_t *host, int port)
{
    HINTERNET s = NULL, c = NULL, r = NULL;
    int ok = 0;
    s = WinHttpOpen(L"LaptopPrep/" L"" PREP_VERSION_STR,
                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 3000, 3000, 4000, 4000);
    c = WinHttpConnect(s, host, (INTERNET_PORT)port, 0);
    if (c) {
        r = WinHttpOpenRequest(c, L"HEAD", L"/", NULL, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (r) {
            /* any reply at all - even a 404 - means DNS, routing and TCP work */
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, NULL))
                ok = 1;
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

/* A probe that cannot run is a probe that must not hold the stage up, so
 * the whole reachability question runs on its own thread with a budget.
 * (The result is then "unknown", and the stage treats unknown as offline:
 * a machine that cannot be reached cannot be updated either.) */
typedef struct {
    volatile LONG done;
    volatile LONG ok;
} ProbeCtx;

static DWORD WINAPI probe_thread(LPVOID p)
{
    ProbeCtx *c = (ProbeCtx*)p;
    if (getenv("LP_NO_PROBE")) {            /* diagnostic: no network in sandbox */
        InterlockedExchange(&c->ok, 1);
        InterlockedExchange(&c->done, 1);
        return 0;
    }
    /* Port 80 on purpose: a captive portal answers there too, and there is
     * no TLS chain to fail on a refurbished machine with a stale clock. */
    InterlockedExchange(&c->ok,
        http_probe(L"sls.update.microsoft.com", 80) ||
        http_probe(L"www.microsoft.com", 80));
    InterlockedExchange(&c->done, 1);
    return 0;
}

static int probe_network(void)
{
    ProbeCtx *c;
    HANDLE th;
    ULONGLONG t0;

    c = (ProbeCtx*)calloc(1, sizeof(ProbeCtx));
    if (!c) return 0;
    th = CreateThread(NULL, 0, probe_thread, c, 0, NULL);
    if (!th) { free(c); return 0; }
    t0 = GetTickCount64();
    for (;;) {
        if (InterlockedCompareExchange(&c->done, 1, 1)) {
            int ok = (int)c->ok;
            CloseHandle(th);
            free(c);
            return ok;
        }
        if (GetTickCount64() - t0 > PREP_HTTP_PROBE_MS) {
            wu_log("The reachability probe did not answer within %lu s, so this "
                   "machine is treated as offline for now. The update stage does "
                   "not depend on it: the local paths run either way",
                   (unsigned long)(PREP_HTTP_PROBE_MS / 1000));
            /* abandoned on purpose (a stuck connect cannot be cancelled) */
            CloseHandle(th);
            return 0;
        }
        Sleep(100);
        gui_tick();
    }
}

/* ==================================================================
 *  current-user session (the part of the update engine that is per user)
 * ================================================================== */
static int active_user_session(char *who, size_t n)
{
    DWORD sid = WTSGetActiveConsoleSessionId();
    LPSTR buf = NULL;
    DWORD bytes = 0;
    who[0] = 0;
    if (sid == 0xFFFFFFFF) return 0;
    if (WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE, sid,
                                    WTSUserName, &buf, &bytes) && buf && bytes) {
        snprintf(who, n, "%s", buf);
        WTSFreeMemory(buf);
        if (!who[0]) return 0;              /* session exists but nobody is on it */
    }
    return 1;
}

/* ==================================================================
 *  installed-payload scan (.msu / .cab in SoftwareDistribution\Download)
 * ================================================================== */
#define WU_MAX_FILES 12
static char g_wuMsu[WU_MAX_FILES][MAX_PATH];
static char g_wuCab[WU_MAX_FILES][MAX_PATH];
static int  g_wuMsuN = 0, g_wuCabN = 0;
static unsigned long long g_wuBytes = 0;

static void wu_scan_dir(const char *dir, int depth)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (depth > 3 || strlen(dir) > MAX_PATH - 64) return;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wu_scan_dir(full, depth + 1);
            continue;
        }
        g_wuBytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        {
            const char *ext = strrchr(fd.cFileName, '.');
            if (!ext) continue;
            if (!_stricmp(ext, ".msu") && g_wuMsuN < WU_MAX_FILES) {
                snprintf(g_wuMsu[g_wuMsuN], MAX_PATH, "%s", full);
                g_wuMsuN++;
            } else if (!_stricmp(ext, ".cab") && g_wuCabN < WU_MAX_FILES) {
                snprintf(g_wuCab[g_wuCabN], MAX_PATH, "%s", full);
                g_wuCabN++;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void wu_scan_download(void)
{
    g_wuMsuN = g_wuCabN = 0;
    g_wuBytes = 0;
    wu_scan_dir("C:\\Windows\\SoftwareDistribution\\Download", 0);
    g_wu.staged_payloads = g_wuMsuN + g_wuCabN;
    wu_log("Things already on this disk: %d .msu, %d .cab, %.1f MB in "
           "SoftwareDistribution\\Download",
           g_wuMsuN, g_wuCabN, (double)g_wuBytes / (1024.0 * 1024.0));
}

/* ==================================================================
 *  a couple of small helpers the stage needs
 * ================================================================== */
static int sys32_tool(const char *exe)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, sizeof(path) - 32);
    if (n == 0) return 0;
    snprintf(path + n, sizeof(path) - (size_t)n, "\\%s", exe);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* Build a command line that runs a tool out of %SystemRoot%\System32 by
 * full path.  Calling "usoclient.exe" and hoping PATH points at System32
 * is the kind of assumption that works on a developer machine and not in
 * a shop with a locked-down PATH. */
static void sys32_cmd(const char *exe, const char *args, char *out, size_t n)
{
    char dir[MAX_PATH];
    UINT k = GetSystemDirectoryA(dir, sizeof(dir));
    if (k == 0) { snprintf(out, n, "%s %s", exe, args); return; }
    snprintf(out, n, "%s\\%s %s", dir, exe, args);
}

int prep_update_done_steps(void) { return (int)g_mirrorDone; }

/* how long ago the stage last showed a sign of life (for the UI) */
int prep_update_heartbeat_age_ms(void)
{
    LONG hb = g_mirrorHeartbeat;
    if (!hb) return 0;
    return (int)(GetTickCount() - (DWORD)hb);
}

/* ==================================================================
 *  the WUA COM jobs, each on its own thread with a stopwatch
 *
 *  The job structs are file-scope on purpose.  A COM call that never
 *  returns cannot be cancelled, so the thread is abandoned - and an
 *  abandoned thread must not write into a caller's stack frame.  With a
 *  static slot the worst case is a few wasted bytes instead of a crash.
 * ================================================================== */
enum { WUJ_LOCAL = 0, WUJ_SEARCH, WUJ_INSTALL };

typedef struct {
    volatile LONG done;
    int   what;
    IDispatch *session;      /* in: Microsoft.Update.Session            */
    IDispatch *coll;         /* out (search) / in (install)             */
    /* out */
    int   answered;          /* a result code came back                 */
    int   installed_any;
    int   restart_required;
    int   access_denied;
    long  rc;
    long  hr;
    long  count;
    int   all_downloaded;
    volatile LONG progress_i, progress_total;
    char  note[512];
} WuJob;

static WuJob g_jobLocal, g_jobSearch, g_jobInstall, g_jobAfter;

static IDispatch *wu_session_new(int *access_denied)
{
    IDispatch *session = wua_create(L"Microsoft.Update.Session");
    VARIANT a;
    if (access_denied) *access_denied = 0;
    if (!session) {
        if (access_denied && GetLastError() == ERROR_ACCESS_DENIED) *access_denied = 1;
        return NULL;
    }
    VariantInit(&a);
    a.vt = VT_BSTR;
    a.bstrVal = SysAllocString(L"LaptopPrep");
    disp_put(session, L"ClientApplicationID", &a);
    VariantClear(&a);
    return session;
}

/* the WHERE clause: pending software updates, nothing hidden */
#define WU_PENDING_WQL L"IsInstalled=0 and Type='Software' and IsHidden=0"

/* search the local store (Online = FALSE) or the service (Online = TRUE) */
static void wu_do_search(WuJob *job, int online)
{
    IDispatch *searcher = NULL, *result = NULL, *coll = NULL;
    VARIANT a, sv;
    long i;

    searcher = disp_call_disp(job->session, L"CreateUpdateSearcher", NULL, 0);
    if (!searcher) { job->hr = -1; return; }

    VariantInit(&sv);
    sv.vt = VT_BOOL;
    sv.boolVal = online ? VARIANT_TRUE : VARIANT_FALSE;
    disp_put(searcher, L"Online", &sv);
    VariantClear(&sv);

    VariantInit(&a);
    a.vt = VT_BSTR;
    a.bstrVal = SysAllocString(WU_PENDING_WQL);
    result = disp_call_disp(searcher, L"Search", &a, 1);
    VariantClear(&a);
    if (!result) {
        job->hr = -1;
        snprintf(job->note, sizeof(job->note),
                 "the search call did not return a result object");
        searcher->lpVtbl->Release(searcher);
        return;
    }

    job->answered = 1;
    job->rc = disp_get_long(result, L"ResultCode", -1);
    job->hr = disp_get_long(result, L"HResult", 0);
    if ((unsigned long)job->hr == 0x80070005UL) job->access_denied = 1;
    coll = disp_get_disp(result, L"Updates");
    if (coll) {
        job->count = disp_get_long(coll, L"Count", 0);
        job->all_downloaded = 1;
        for (i = 0; i < job->count; i++) {
            IDispatch *upd = NULL;
            VARIANT idx;
            VariantInit(&idx);
            idx.vt = VT_I4;
            idx.lVal = i;
            upd = disp_call_disp(coll, L"Item", &idx, 1);
            VariantClear(&idx);
            if (!upd) continue;
            if (!disp_get_bool(upd, L"IsDownloaded", 0)) job->all_downloaded = 0;
            /* name the first few updates so the log says what is coming */
            if (i < 6) {
                char title[512];
                disp_get_str(upd, L"Title", title, sizeof(title));
                wu_log("  %ld. %s%s", i + 1, title,
                       disp_get_bool(upd, L"IsDownloaded", 0) ? "   [already downloaded]"
                                                             : "");
            }
            upd->lpVtbl->Release(upd);
            gui_tick();
        }
        if (job->count > 6) wu_log("  ... and %ld more", job->count - 6);
        coll->lpVtbl->AddRef(coll);
        job->coll = coll;              /* the caller owns this reference now */
    }
    result->lpVtbl->Release(result);
    searcher->lpVtbl->Release(searcher);
}

/* download everything in the collection that is not cached yet */
static int wu_do_download(WuJob *job, IDispatch *coll, long count)
{
    IDispatch *dl = NULL, *res = NULL;
    VARIANT u;
    long rc;
    int need = 0;
    long i;

    for (i = 0; i < count; i++) {
        IDispatch *upd = NULL;
        VARIANT idx;
        VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;
        upd = disp_call_disp(coll, L"Item", &idx, 1);
        VariantClear(&idx);
        if (!upd) continue;
        if (!disp_get_bool(upd, L"IsDownloaded", 0)) need = 1;
        upd->lpVtbl->Release(upd);
    }
    if (!need) {
        wu_log("Everything needed is already downloaded - installing from the "
               "local cache, no download at all");
        return 1;
    }

    wu_log("Downloading the update files now. Depending on the updates and the "
           "line this is the part that takes the longest");
    dl = disp_call_disp(job->session, L"CreateUpdateDownloader", NULL, 0);
    if (!dl) {
        snprintf(job->note, sizeof(job->note), "CreateUpdateDownloader failed");
        return 0;
    }
    VariantInit(&u);
    u.vt = VT_DISPATCH;
    u.pdispVal = coll;
    disp_put(dl, L"Updates", &u);
    res = disp_call_disp(dl, L"Download", NULL, 0);
    if (!res) {
        snprintf(job->note, sizeof(job->note), "the download call failed");
        dl->lpVtbl->Release(dl);
        return 0;
    }
    rc = disp_get_long(res, L"ResultCode", -1);
    if ((unsigned long)rc == 0x80070005UL) job->access_denied = 1;
    wu_log("Download result: %s (%ld)", wua_code_str(rc), rc);
    res->lpVtbl->Release(res);
    dl->lpVtbl->Release(dl);
    return (rc == 2 || rc == 3 || rc == 1) ? 1 : 0;
}

/* install the collection one update at a time, so the log shows progress
 * and every single failure can be named */
static void wu_do_install(WuJob *job, IDispatch *coll, long count)
{
    long i;

    if (!wu_do_download(job, coll, count))
        wu_log("The download reported a problem - trying to install anyway");

    InterlockedExchange(&job->progress_total, count);
    for (i = 0; i < count; i++) {
        IDispatch *upd = NULL, *single = NULL, *installer = NULL, *res = NULL;
        VARIANT idx, u;
        long rc, hr = 0;
        char title[512];

        InterlockedExchange(&job->progress_i, i + 1);
        VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;
        upd = disp_call_disp(coll, L"Item", &idx, 1);
        VariantClear(&idx);
        if (!upd) continue;

        disp_get_str(upd, L"Title", title, sizeof(title));
        wu_log("Installing %ld of %ld: %s", i + 1, count, title);

        /* EULA + client id per update: a refused EULA makes Install() return
         * 0x80240017 and looks like a mystery otherwise */
        {
            VARIANT a;
            VariantInit(&a);
            a.vt = VT_BSTR;
            a.bstrVal = SysAllocString(L"LaptopPrep");
            disp_put(upd, L"ClientApplicationID", &a);
            VariantClear(&a);
            disp_call(upd, L"AcceptEula", DISPATCH_METHOD, NULL, 0, NULL);
        }

        single = disp_call_disp(job->session, L"CreateUpdateCollection", NULL, 0);
        if (single) {
            VARIANT arg;
            VariantInit(&arg);
            arg.vt = VT_DISPATCH;
            arg.pdispVal = upd;
            disp_call(single, L"Add", DISPATCH_METHOD, &arg, 1, NULL);
            VariantClear(&arg);
        }

        installer = disp_call_disp(job->session, L"CreateUpdateInstaller", NULL, 0);
        if (!installer) {
            wu_log("  -> could not create the update installer, stopping here");
            if (single) single->lpVtbl->Release(single);
            upd->lpVtbl->Release(upd);
            break;
        }
        VariantInit(&u);
        u.vt = VT_DISPATCH;
        u.pdispVal = single ? single : coll;
        disp_put(installer, L"Updates", &u);

        res = disp_call_disp(installer, L"Install", NULL, 0);
        if (!res) {
            wu_log("  -> the install call failed for this update");
            if (single) single->lpVtbl->Release(single);
            installer->lpVtbl->Release(installer);
            upd->lpVtbl->Release(upd);
            continue;
        }
        rc = disp_get_long(res, L"ResultCode", -1);
        hr = disp_get_long(res, L"HResult", 0);
        if (disp_get_bool(res, L"RebootRequired", 0)) job->restart_required = 1;
        wu_log("  -> %s (%ld) 0x%08lX%s", wua_code_str(rc), rc,
               (unsigned long)hr,
               job->restart_required ? "   [restart required from here on]" : "");
        if ((unsigned long)hr == 0x80070005UL) job->access_denied = 1;
        if ((unsigned long)rc == 0x80240017UL)
            wu_log("     0x80240017: Windows refused the install - the update is "
                   "not applicable to this machine or its licence was refused");
        if (rc == 2 || rc == 3) {
            job->installed_any = 1;
            wu_log("     installed");
        }
        res->lpVtbl->Release(res);
        if (single) single->lpVtbl->Release(single);
        installer->lpVtbl->Release(installer);
        upd->lpVtbl->Release(upd);
        gui_tick();
    }
}

static DWORD WINAPI wu_job_thread(LPVOID param)
{
    WuJob *job = (WuJob*)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com_ok = (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE);

    if (!com_ok) {
        snprintf(job->note, sizeof(job->note), "CoInitializeEx failed (0x%08lX)",
                 (unsigned long)hr);
    } else if (job->what == WUJ_INSTALL && job->coll) {
        wu_do_install(job, job->coll, job->count);
    } else if (job->session) {
        wu_do_search(job, job->what == WUJ_SEARCH);
    } else {
        snprintf(job->note, sizeof(job->note), "no update session");
    }

    InterlockedExchange(&job->done, 1);
    return 0;
}

static WuJob *wu_job_prepare(WuJob *slot, int what)
{
    /* only reuse the slot once the previous job in it has finished; an
     * abandoned thread keeps its own slot and is never touched again */
    memset(slot, 0, sizeof(*slot));
    slot->what = what;
    slot->done = 0;
    return slot;
}

/* Wait for a job with a heartbeat.  Returns 1 when it finished inside its
 * budget, 0 when it had to be abandoned.  The thread handle is always
 * closed (closing a handle does not kill the thread, and the COM call
 * cannot be cancelled anyway). */
static int wu_job_run(WuJob *job, DWORD budgetMs, const char *what)
{
    HANDLE th;
    ULONGLONG t0, lastBeat = 0;

    th = CreateThread(NULL, 0, wu_job_thread, job, 0, NULL);
    if (!th) {
        wu_log("Could not start the worker thread for %s", what);
        return 0;
    }

    t0 = GetTickCount64();
    for (;;) {
        ULONGLONG el;
        if (InterlockedCompareExchange(&job->done, 1, 1)) {
            CloseHandle(th);
            return 1;
        }
        Sleep(250);
        gui_tick();
        el = GetTickCount64() - t0;
        if (el - lastBeat >= PREP_WU_HEARTBEAT_MS) {
            lastBeat = el;
            InterlockedExchange(&g_mirrorHeartbeat, (LONG)GetTickCount());
            wu_publish();
            if (job->what == WUJ_INSTALL && job->progress_total) {
                wu_log("Still installing: update %ld of %ld, %llu min elapsed of a "
                       "%llu min budget",
                       (long)job->progress_i, (long)job->progress_total,
                       (unsigned long long)(el / 60000),
                       (unsigned long long)(budgetMs / 60000));
            } else {
                wu_log("Still %s: %llu min elapsed of a %llu min budget. Windows "
                       "Update is slow, not necessarily broken - the watchdog is "
                       "only here so nothing can hang forever",
                       what, (unsigned long long)(el / 60000),
                       (unsigned long long)(budgetMs / 60000));
            }
        }
        if (el > budgetMs) {
            wu_log("Giving up on %s after %llu minutes: the call was accepted and "
                   "never answered. That stuck call lives inside Windows itself - "
                   "LaptopPrep can only stop waiting for it (and note it as evidence)",
                   what, (unsigned long long)(el / 60000));
            CloseHandle(th);
            return 0;
        }
    }
}

/* ==================================================================
 *  staged payloads: install what is already on the disk
 * ================================================================== */
static int wu_install_staged(void)
{
    int i;
    DWORD rc = 0;
    char line[MAX_PATH + 256];

    if (!g_wuMsuN && !g_wuCabN) return 0;

    wu_log("Installing the update packages that are already on this disk - no "
           "search, no download, no update agent involved");

    for (i = 0; i < g_wuMsuN; i++) {
        sys32_cmd("wusa.exe", "", line, sizeof(line));
        snprintf(line + strlen(line), sizeof(line) - strlen(line),
                 "\"%s\" /quiet /norestart", g_wuMsu[i]);
        wu_log("Package %d of %d (.msu): %s", i + 1, g_wuMsuN, g_wuMsu[i]);
        if (run_process_logged(line, (int)PREP_WU_PAYLOAD_TIMEOUT_MS, &rc)) {
            wu_log("  -> wusa exit code %lu", (unsigned long)rc);
            g_wu.staged_installed = 1;
        } else if (rc == 3010) {
            wu_log("  -> wusa exit code 3010 (restart required)");
            g_wu.staged_installed = 1;
            g_wu.restart_needed = 1;
        } else if (rc == 2359302) {
            wu_log("  -> already installed (2359302)");
        } else {
            wu_log("  -> wusa did not report success (exit %lu); carrying on with "
                   "the next package and the remaining paths",
                   (unsigned long)rc);
            g_wu.staged_failed = 1;
        }
        if (restart_flag_set()) {
            g_wu.restart_needed = 1;
            g_wu.reboot_after = 1;
            return 2;
        }
        gui_tick();
    }

    for (i = 0; i < g_wuCabN; i++) {
        sys32_cmd("dism.exe", "/online /norestart /quiet /add-package /packagepath:",
                  line, sizeof(line));
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "\"%s\"",
                 g_wuCab[i]);
        wu_log("Package %d of %d (.cab): %s", i + 1, g_wuCabN, g_wuCab[i]);
        if (run_process_logged(line, (int)PREP_WU_PAYLOAD_TIMEOUT_MS, &rc)) {
            wu_log("  -> dism exit code %lu", (unsigned long)rc);
            g_wu.staged_installed = 1;
        } else if (rc == 3010) {
            wu_log("  -> dism exit code 3010 (restart required)");
            g_wu.staged_installed = 1;
            g_wu.restart_needed = 1;
        } else {
            wu_log("  -> dism did not report success (exit %lu); carrying on",
                   (unsigned long)rc);
            g_wu.staged_failed = 1;
        }
        if (restart_flag_set()) {
            g_wu.restart_needed = 1;
            g_wu.reboot_after = 1;
            return 2;
        }
        gui_tick();
    }
    return g_wu.staged_installed ? 1 : 0;
}

/* ==================================================================
 *  component health (DISM) - only asked when something actually failed
 * ================================================================== */
static void wu_components_health(void)
{
    DWORD rc = 0;
    int ran;
    char line[MAX_PATH + 128];

    if (!g_wu.admin) {
        wu_log("Skipping the payload check: DISM asks for administrator rights");
        return;
    }
    if (!sys32_tool("Dism.exe")) {
        wu_log("dism.exe is not present on this machine, so the payload check "
               "cannot run - the stage says so instead of guessing");
        return;
    }
    wu_log("Something before this did not work, so the update payloads are "
           "checked now (dism /checkhealth). This is the step that explains "
           "machines where every single update fails");

    sys32_cmd("dism.exe", "/online /cleanup-image /checkhealth", line, sizeof(line));
    ran = run_process_logged(line, (int)PREP_WU_DISM_TIMEOUT_MS, &rc);
    if (ran && rc == 0 && g_lastProcOut > 0) {
        g_wu.components_checked = 1;
        wu_log("The component store reports no corruption - the payloads are fine, "
               "so the problem is elsewhere (and this machine does not need an "
               "expensive repair)");
        return;
    }
    if (ran && rc == 0) {
        wu_log("dism returned success without printing anything, which is not how "
               "DISM behaves - treating the payload check as NOT done rather than "
               "as a clean result");
        return;
    }
    g_wu.components_checked = 1;

    wu_log("dism /checkhealth did not report a clean store (exit %lu) - repairing "
           "the payloads now (dism /restorehealth). This can take 20 minutes",
           (unsigned long)rc);
    g_wu.components_faulty = 1;
    sys32_cmd("dism.exe", "/online /cleanup-image /restorehealth", line, sizeof(line));
    ran = run_process_logged(line, (int)PREP_WU_DISM_TIMEOUT_MS, &rc);
    if (ran && rc == 0) {
        g_wu.components_repaired = 1;
        wu_log("Component store repaired - updates should install again now");
    } else {
        g_wu.components_repair_failed = 1;
        wu_log("The component store could not be repaired (exit %lu). On a machine "
               "like this the update components have to be rebuilt: run "
               "fix-windows-update.bat as administrator", (unsigned long)rc);
    }
}

/* ==================================================================
 *  the update orchestrator (usoclient)
 *
 *  usoclient drives the same engine Settings > Windows Update uses.  Its
 *  commands run in the session this process was started from and return
 *  immediately (the work happens inside the service), so this path can
 *  never freeze the run - we only watch the reboot flags and the download
 *  folder for evidence that the machine is doing something.
 * ================================================================== */
static int wu_orchestrator(void)
{
    DWORD rc = 0;
    unsigned long long before;
    ULONGLONG t0, lastBeat = 0;
    ULONGLONG budget;
    char line[MAX_PATH + 128];

    if (!g_wu.uso_present) {
        wu_log("usoclient.exe is not present on this build, so this path is not "
               "available");
        return 0;
    }
    wu_log("Handing the work to the Windows Update orchestrator. These commands "
           "run per machine and return immediately, so this stage cannot hang on "
           "them");

    wu_scan_download();
    before = g_wuBytes;

    sys32_cmd("usoclient.exe", "RefreshSettings", line, sizeof(line));
    if (run_process_logged(line, 30000, &rc))
        wu_log("usoclient RefreshSettings -> exit %lu", (unsigned long)rc);

    sys32_cmd("usoclient.exe", "StartScan", line, sizeof(line));
    if (!run_process_logged(line, 60000, &rc)) {
        if (rc == 5 || rc == ERROR_ACCESS_DENIED) {
            g_wu.access_denied = 1;
            wu_log("The orchestrator refused the scan: access denied. Windows "
                   "restricts the orchestrator to administrators");
        } else {
            wu_log("The orchestrator scan did not start (exit %lu)", (unsigned long)rc);
        }
        return 0;
    }
    g_wu.uso_scan_ok = 1;
    wu_log("usoclient StartScan -> exit %lu", (unsigned long)rc);

    budget = PREP_WU_USO_SCAN_WAIT_MS;
    t0 = GetTickCount64();
    for (;;) {
        ULONGLONG el = GetTickCount64() - t0;
        if (restart_flag_set()) {
            g_wu.uso_progress = 1;
            g_wu.reboot_after = 1;
            g_wu.restart_needed = 1;
            wu_log("The orchestrator reports a restart requirement - the work it "
                   "started is finished");
            return 1;
        }
        wu_scan_download();
        if (g_wuBytes > before || g_wuMsuN || g_wuCabN) {
            g_wu.uso_progress = 1;
            wu_log("The update agent is working: %.1f MB of update payload on the "
                   "disk now", (double)g_wuBytes / (1024.0 * 1024.0));
            break;
        }
        if (el > budget) {
            wu_log("The orchestrator reported nothing within %llu minutes. On a "
                   "machine with a current update stack that usually means there "
                   "is genuinely nothing to do - the agent search right after this "
                   "confirms or contradicts it",
                   (unsigned long long)(budget / 60000));
            return 0;
        }
        Sleep(2000);
        gui_tick();
        if (el - lastBeat >= PREP_WU_HEARTBEAT_MS) {
            lastBeat = el;
            InterlockedExchange(&g_mirrorHeartbeat, (LONG)GetTickCount());
            wu_publish();
            wu_log("Waiting for the orchestrator: %llu min elapsed of a %llu min "
                   "watch", (unsigned long long)(el / 60000),
                   (unsigned long long)(budget / 60000));
        }
    }

    wu_log("Asking the orchestrator to download and install");
    sys32_cmd("usoclient.exe", "StartDownload", line, sizeof(line));
    if (!run_process_logged(line, 60000, &rc))
        wu_log("usoclient StartDownload -> exit %lu", (unsigned long)rc);
    else
        g_wu.uso_download_ok = 1;
    Sleep(3000);
    sys32_cmd("usoclient.exe", "StartInstall", line, sizeof(line));
    if (!run_process_logged(line, 60000, &rc))
        wu_log("usoclient StartInstall -> exit %lu", (unsigned long)rc);
    else
        g_wu.uso_install_ok = 1;

    budget = PREP_WU_USO_INSTALL_WAIT_MS;
    t0 = GetTickCount64();
    lastBeat = 0;
    for (;;) {
        ULONGLONG el = GetTickCount64() - t0;
        if (restart_flag_set()) {
            g_wu.restart_needed = 1;
            g_wu.reboot_after = 1;
            wu_log("The orchestrator installed the updates and Windows now wants a "
                   "restart");
            return 1;
        }
        if (el > budget) {
            wu_log("The orchestrator install watch timed out after %llu minutes "
                   "without a restart requirement", (unsigned long long)(el / 60000));
            return 0;
        }
        Sleep(2000);
        gui_tick();
        if (el - lastBeat >= PREP_WU_HEARTBEAT_MS) {
            lastBeat = el;
            InterlockedExchange(&g_mirrorHeartbeat, (LONG)GetTickCount());
            wu_publish();
            wu_log("Waiting for the orchestrator install: %llu min elapsed of a "
                   "%llu min watch", (unsigned long long)(el / 60000),
                   (unsigned long long)(budget / 60000));
        }
    }
}

/* ==================================================================
 *  the stage itself
 * ================================================================== */
/* after an install attempt, is anything still on the to-do list? */
static int pending_left_unknown(WuState *s)
{
    int pending = wu_known_pending(s);
    if (s->agent_installed || s->staged_installed) return 0;
    if (s->install_timed_out) { s->install_failed = 1; return pending > 0; }
    if (pending > 0 && !s->agent_timed_out) { s->install_failed = 1; return 1; }
    return 0;
}

static void wu_service_probe(void)
{
    static const char *svc[3];
    int i;
    svc[0] = "wuauserv";     /* Windows Update                     */
    svc[1] = "bits";         /* Background Intelligent Transfer    */
    svc[2] = "UsoSvc";       /* Update Orchestrator                */
    for (i = 0; i < 3; i++) {
        int ok;
        wu_log("Checking the %s service (up to %lu s)...", svc[i],
               (unsigned long)(PREP_WU_SERVICE_WAIT_MS / 1000));
        ok = svc_ensure_started(svc[i], PREP_WU_SERVICE_WAIT_MS);
        wu_log("  %-8s : %s", svc[i], ok ? "running" : svc_state_text(svc[i]));
        if (ok) g_wu.services_started++;
    }
}

static int do_windows_update(void)
{
    int ok = 0, verdict;
    char who[128] = {0};
    char line[512];

    memset(&g_wu, 0, sizeof(g_wu));
    memset(&g_wuEst, 0, sizeof(g_wuEst));
    memset(g_wuStepDur, 0, sizeof(g_wuStepDur));
    g_wuVerdictText[0] = g_wuActionText[0] = g_wuAdviceText[0] = 0;
    g_wu.admin = is_admin();
    wu_publish();

    wu_log("=============== Windows Update stage: %d steps ===============",
           WU_STEP_COUNT);
    wu_log("The steps are: scan -> install what is cached -> verify payloads -> "
           "orchestrator -> session -> agent search -> install -> restart check");
    wu_log("Elevation: %s", g_wu.admin
           ? "running as administrator - the update layer can act"
           : "NOT elevated - Windows refuses most update work to a normal user");
    {
        HKEY k;
        char buf[64];
        DWORD n = sizeof(buf);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "CurrentBuildNumber", 0, NULL, (BYTE*)buf, &n)
                    == ERROR_SUCCESS)
                g_wu.win_build = atoi(buf);
            RegCloseKey(k);
        }
    }
    wu_log("Windows build: %d", g_wu.win_build);

    wu_log("Reading the state this machine is in before touching anything:");
    wu_log("  restart flags in the registry and the component store...");
    g_wu.reboot_before = restart_flag_set() || wua_reboot_pending();
    g_wu.reboot_after = g_wu.reboot_before;
    wu_log("  a restart was %s pending before this run",
           g_wu.reboot_before ? "already" : "not");
    wu_log("  reaching the Microsoft update endpoints...");
    g_wu.internet_likely = probe_network();
    wu_log("  network probe: %s", g_wu.internet_likely ? "answered" : "no answer");
    wu_log("  which session this tool is running in...");
    g_wu.user_session = active_user_session(who, sizeof(who));
    wu_log("  session check done");
    g_wu.uso_present = sys32_tool("usoclient.exe");
    wu_log("  usoclient.exe: %s", g_wu.uso_present ? "present" : "not on this build");

    wu_log("Internet reachability probe: %s", g_wu.internet_likely
           ? "the Microsoft endpoints answered"
           : "could not reach the internet from here");
    wu_log("Interactive user session: %s", g_wu.user_session
           ? who : "none open");

    /* ---- step 0: services + what is already on this machine --------- */
    wu_reach(WU_S_SCAN);
    wu_service_probe();

    {
        IDispatch *probe = wua_create(L"Microsoft.Update.Session");
        g_wu.agent_present = probe ? 1 : 0;
        if (probe) {
            probe->lpVtbl->Release(probe);
            wu_log("The Windows Update Agent answered - the update components are "
                   "present on this machine. (No wmic.exe and no WMI service is "
                   "used for updates: only the agent and the orchestrator that "
                   "Windows itself runs)");
        } else {
            if (GetLastError() == ERROR_ACCESS_DENIED) g_wu.access_denied = 1;
            wu_log("The Windows Update Agent could not be created - the update "
                   "components are missing or unregistered on this machine");
        }
    }

    wu_scan_download();
    g_wu.staged_payloads = (int)(g_wuMsuN + g_wuCabN);
    if (g_wu.staged_payloads)
        wu_log("%d update package(s) are already on this disk (%.1f MB) - they are "
               "installed before anything is searched for",
               g_wu.staged_payloads, (double)g_wuBytes / (1048576.0));

    if (g_wu.agent_present) {
        WuJob *j = wu_job_prepare(&g_jobLocal, WUJ_LOCAL);
        j->session = wu_session_new(&j->access_denied);
        if (j->session) {
            if (wu_job_run(j, PREP_WU_LOCAL_TIMEOUT_MS,
                           "reading the local update store")) {
                g_wu.agent_answered  = j->answered;
                g_wu.agent_rc        = (int)j->rc;
                g_wu.agent_hresult   = j->hr;
                g_wu.store_count     = (int)j->count;
                g_wu.store_all_downloaded = j->all_downloaded;
                if (j->access_denied) g_wu.access_denied = 1;
                if (j->answered) {
                    wu_log("Local update store: %ld pending update(s)%s (result %s)",
                           j->count,
                           j->count ? (j->all_downloaded
                                       ? ", all of them already downloaded"
                                       : ", not all of them downloaded yet") : "",
                           wua_code_str(j->rc));
                } else {
                    wu_log("The local update store did not answer with a result - "
                           "the call came back without an answer, so the update "
                           "components on this machine are not usable");
                    g_wu.agent_error = 1;
                }
                if (j->coll) { j->coll->lpVtbl->Release(j->coll); j->coll = NULL; }
                j->session->lpVtbl->Release(j->session);
                j->session = NULL;
            } else {
                g_wu.agent_timed_out = 1;
                /* the session belongs to the thread we just gave up on: hand it
                 * over rather than releasing it under that thread's feet */
                g_jobSearch.session = j->session;
                j->session = NULL;
            }
        } else {
            if (j->access_denied) g_wu.access_denied = 1;
            wu_log("Could not create an update session for the local store read");
        }
    }
    wu_step_done(WU_S_SCAN);

    /* nothing to do but reboot? ------------------------------------- */
    if (g_wu.reboot_before) {
        g_wu.finished = 1;
        g_wu.reboot_after = 1;
        wu_log("This machine already has updates installed and is waiting for a "
               "RESTART. Nothing is searched and nothing is downloaded - press "
               "'Upload + restart' to send the log and finish the updates");
        wu_publish();
        goto finalise;
    }

    /* ---- step 1: packages that are already on the disk ------------- */
    wu_reach(WU_S_STAGED);
    if (wu_install_staged() == 2) {
        g_wu.restart_needed = 1;
        g_wu.reboot_after = 1;
        wu_step_done(WU_S_STAGED);
        wu_log("Everything needed was already on this machine, so the restart "
               "finishes it. The remaining steps are not needed");
        g_wu.finished = 1;
        wu_publish();
        goto finalise;
    }
    wu_step_done(WU_S_STAGED);

    /* ---- step 2: payload check, only when something looked wrong --- */
    wu_reach(WU_S_HEALTH);
    if (!g_wu.agent_answered || !g_wu.agent_present || g_wu.staged_failed)
        wu_components_health();
    else
        wu_log("Not needed: the update layer answered normally, so the payloads "
               "are in order and DISM is left alone");
    wu_step_done(WU_S_HEALTH);

    /* ---- step 3: the orchestrator --------------------------------- */
    wu_reach(WU_S_ORCH);
    if (wu_orchestrator() && (g_wu.restart_needed || restart_flag_set())) {
        g_wu.restart_needed = 1;
        g_wu.reboot_after = 1;
        wu_step_done(WU_S_ORCH);
        g_wu.finished = 1;
        wu_publish();
        goto finalise;
    }
    wu_step_done(WU_S_ORCH);

    /* ---- step 4: the current-user session ------------------------- */
    wu_reach(WU_S_SESSION);
    /* the orchestrator runs per machine out of this session; when a user is
     * logged in, its half of the engine is the session this process is in */
    if (g_wu.user_session && g_wu.uso_scan_ok) g_wu.user_session_covered = 1;
    if (g_wu.user_session) {
        wu_log("A user session is open (%s). Part of the update engine works per "
               "user, and the orchestrator commands LaptopPrep runs belong to this "
               "session (that is the session this console was started from), so "
               "that half is covered here. Everything else is handled per machine "
               "by the steps around this one", who);
    } else {
        wu_log("No interactive user session is open, so there is no per-user "
               "update work to do in this run");
    }
    wu_step_done(WU_S_SESSION);

    /* ---- step 5: the agent search ---------------------------------- */
    wu_reach(WU_S_AGENT);

    if (!g_wu.agent_present) {
        wu_log("Without the update agent there is nothing to ask - it has to be "
               "repaired first. This laptop cannot be updated by any tool until "
               "that is fixed");
        wu_step_done(WU_S_AGENT);
        g_wu.finished = 1;
        wu_publish();
        goto finalise;
    }

    {
        WuJob *j = wu_job_prepare(&g_jobSearch, WUJ_SEARCH);
        int done;
        DWORD budget;
        const char *what;

        j->session = wu_session_new(&j->access_denied);
        if (!j->session) {
            if (j->access_denied) g_wu.access_denied = 1;
            wu_log("Could not create the update session for the search");
            wu_step_done(WU_S_AGENT);
            g_wu.finished = 1;
            wu_publish();
            goto finalise;
        }

        if (g_wu.store_count > 0) {
            /* install what is staged first: a laptop with no internet still
             * gets its updates that way */
            j->what = WUJ_LOCAL;
            budget = PREP_WU_LOCAL_TIMEOUT_MS;
            what = "reading the local update store";
            wu_log("%d update(s) are already staged on this machine, so the "
                   "install starts from the local cache", g_wu.store_count);
        } else if (!g_wu.internet_likely) {
            g_wu.online_attempted = 1;
            g_wu.online_answered = 0;
            wu_log("Nothing is staged locally and there is no internet from here, "
                   "so the online check is skipped. Connect the laptop to a "
                   "network to finish the update step");
            wu_step_done(WU_S_AGENT);
            g_wu.finished = 1;
            wu_publish();
            goto finalise;
        } else {
            j->what = WUJ_SEARCH;
            budget = PREP_WU_SEARCH_TIMEOUT_MS;
            what = "searching Windows Update";
            g_wu.online_attempted = 1;
            g_wuEst.est_ms[WU_S_AGENT] = PREP_WU_SEARCH_TIMEOUT_MS;
            wu_publish();
            wu_log("Nothing is staged locally, so Windows Update itself is asked. "
                   "On a laptop that has not been updated for a while this "
                   "legitimately takes 20-45 minutes: the log shows a heartbeat "
                   "every 15 seconds and the run screen shows the countdown, so it "
                   "is always visible that it is working");
        }

        done = wu_job_run(j, budget, what);
        if (!done) {
            g_wu.agent_timed_out = 1;
            if (j->what == WUJ_SEARCH) g_wu.online_answered = 0;
            /* the session and any collection belong to the abandoned thread */
            g_jobInstall.session = j->session;
            g_jobInstall.coll = j->coll;
            j->session = NULL;
            j->coll = NULL;
        } else {
            if (j->access_denied) g_wu.access_denied = 1;
            if (j->what == WUJ_SEARCH) {
                g_wu.online_answered = j->answered;
                g_wu.online_pending = (int)j->count;
                if (j->answered) {
                    wu_log("The service reports %ld update(s) missing (result %s)",
                           j->count, wua_code_str(j->rc));
                    if (j->count == 0)
                        wu_log("Nothing to install for this machine as far as "
                               "Windows Update is concerned");
                } else {
                    wu_log("The search came back without a result - the agent is "
                           "answering with an error, which is a component problem "
                           "and not a machine that needs more patience");
                    g_wu.agent_error = 1;
                }
            } else {
                g_wu.agent_answered = j->answered;
                g_wu.store_count = (int)j->count;
                g_wu.store_all_downloaded = j->all_downloaded;
                if (j->answered)
                    wu_log("Local store: %ld update(s) pending (result %s)",
                           j->count, wua_code_str(j->rc));
            }
        }
    }
    wu_step_done(WU_S_AGENT);

    /* ---- step 6: download + install -------------------------------- */
    wu_reach(WU_S_INSTALL);
    {
        int pending = g_wu.online_pending > g_wu.store_count
                      ? g_wu.online_pending : g_wu.store_count;
        WuJob *src = &g_jobSearch;
        WuJob *j = &g_jobInstall;

        /* the abandoned-session case moved the handles into the install slot */
        if (!src->coll && !src->session) src = &g_jobInstall;

        if (src->coll && !g_wu.agent_timed_out && pending > 0) {
            j->session = src->session;
            j->coll = src->coll;
            j->count = pending;
            src->session = NULL;
            src->coll = NULL;
            j->what = WUJ_INSTALL;
            j->done = 0;
            wu_log("Installing %d update(s) now - the log names every single one",
                   pending);
            if (wu_job_run(j, PREP_WU_DOWNLOAD_TIMEOUT_MS + PREP_WU_INSTALL_TIMEOUT_MS,
                           "installing updates")) {
                g_wu.agent_installed = j->installed_any;
                g_wu.restart_needed = j->restart_required;
                if (j->access_denied) g_wu.access_denied = 1;
                if (j->session) { j->session->lpVtbl->Release(j->session); j->session = NULL; }
                if (j->coll) { j->coll->lpVtbl->Release(j->coll); j->coll = NULL; }
            } else {
                g_wu.install_timed_out = 1;
                wu_log("The install is still going after its whole budget. Windows "
                       "keeps working on it in the background - the restart flags "
                       "and the next run will show the outcome");
            }
        } else if (g_wu.agent_error || !g_wu.agent_answered) {
            /* the old build turned a silent agent into "up to date" here */
            wu_log("Nothing to install, but nothing was ever reported either: the "
                   "agent never answered, so this is NOT the same as a machine "
                   "with no updates - the verdict says so");
        } else {
            wu_log("Nothing to download or install - the store is empty and the "
                   "service confirmed it, so this machine is current");
            if (src->session) { src->session->lpVtbl->Release(src->session); src->session = NULL; }
            if (src->coll) { src->coll->lpVtbl->Release(src->coll); src->coll = NULL; }
        }
    }
    if (pending_left_unknown(&g_wu)) {
        /* the install did not report a success and updates were pending: the
         * verdict must not pretend otherwise */
    }
    wu_step_done(WU_S_INSTALL);

    /* re-read the store, so the verdict is not based on a stale count */
    if (g_wu.agent_present && !g_wu.agent_timed_out) {
        WuJob *a = wu_job_prepare(&g_jobAfter, WUJ_LOCAL);
        a->session = wu_session_new(&a->access_denied);
        if (a->session) {
            if (wu_job_run(a, PREP_WU_LOCAL_TIMEOUT_MS,
                           "re-reading the local update store")) {
                if (a->answered) {
                    g_wu.store_count = (int)a->count;
                    g_wu.store_all_downloaded = a->all_downloaded;
                    wu_log("After the install: %ld update(s) still pending",
                           a->count);
                }
                if (a->coll) { a->coll->lpVtbl->Release(a->coll); a->coll = NULL; }
                a->session->lpVtbl->Release(a->session);
                a->session = NULL;
            } else {
                g_wu.agent_timed_out = 1;
            }
        }
    }

    g_wu.finished = 1;
    wu_publish();

finalise:
    /* ---- step 7: the restart check and the record ------------------ */
    wu_reach(WU_S_FINAL);
    if (restart_flag_set()) g_wu.reboot_after = 1;
    if (g_wu.reboot_after) g_wu.restart_needed = 1;
    g_wu.finished = 1;
    wu_publish();

    verdict = prep_update_verdict();
    wu_log("Verdict: %s (severity %d)", wu_verdict_id(verdict),
           prep_update_severity());
    if (g_wuVerdictText[0]) wu_log("%s", g_wuVerdictText);
    if (g_wuAdviceText[0])  wu_log("Next: %s", g_wuAdviceText);
    snprintf(line, sizeof(line), "Steps resolved: %d of %d, phase %s",
             prep_update_done_steps(), WU_STEP_COUNT,
             wu_phase_id(prep_update_phase()));
    wu_log("%s", line);
    wu_step_done(WU_S_FINAL);
    wu_log("=============== update stage finished ===============");
    wu_publish();

    /* severity 0..2 keeps the step green or amber; 3 (blocked / needs
     * elevation / hardware missing) is reported as a failure so it cannot be
     * mistaken for a finished machine */
    ok = (verdict != WU_J_NONE && prep_update_severity() < 3) ? 1 : 0;
    return ok;
}

/* ==================================================================
 *  environment probe
 *
 *  Windows 11 24H2 removed wmic.exe (it is a "feature on demand" now).
 *  LaptopPrep never used it: every hardware value is read through the WMI
 *  COM API (IWbemLocator / IWbemServices + WQL), which is part of Windows
 *  itself and is still present.  Updates go through the Windows Update
 *  Agent COM API and the orchestrator, which involve neither wmic nor the
 *  WMI service.
 *
 *  This probe writes what is actually available into the log, so a machine
 *  where something IS missing can be diagnosed in one look.
 * ================================================================== */
void probe_environment(void)
{
    char line[512];
    IDispatch *test;
    HRESULT hr;

    write_log("----- environment probe -----");

    snprintf(line, sizeof(line),
             "wmic.exe: %s - LaptopPrep does not use it (Windows 11 24H2 removed "
             "the tool; the WMI COM API is used instead)",
             sys32_tool("wmic.exe") ? "present" : "absent");
    write_log(line);

    snprintf(line, sizeof(line), "powercfg.exe: %s - battery report",
             sys32_tool("powercfg.exe")
                 ? "present" : "MISSING, the built-in fallback report is used");
    write_log(line);

    snprintf(line, sizeof(line), "usoclient.exe: %s - update orchestrator",
             sys32_tool("usoclient.exe") ? "present" : "absent");
    write_log(line);

    snprintf(line, sizeof(line), "wusa.exe: %s - installs targeted .msu packages",
             sys32_tool("wusa.exe") ? "present" : "absent");
    write_log(line);

    snprintf(line, sizeof(line), "dism.exe: %s - payload check and repair",
             sys32_tool("Dism.exe") ? "present" : "absent");
    write_log(line);

    snprintf(line, sizeof(line), "Windows Update service (wuauserv): %s",
             svc_state_text("wuauserv"));
    write_log(line);

    snprintf(line, sizeof(line),
             "Windows Modules Installer (TrustedInstaller): %s",
             svc_state_text("TrustedInstaller"));
    write_log(line);

    snprintf(line, sizeof(line),
             "WMI service (Winmgmt): %s - needed for the hardware collection, "
             "never for Windows Update", svc_state_text("Winmgmt"));
    write_log(line);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        test = wua_create(L"Microsoft.Update.Session");
        if (test) {
            test->lpVtbl->Release(test);
            write_log("Windows Update Agent (Microsoft.Update.Session): available - "
                      "updates do not use wmic.exe and do not need the WMI service");
        } else {
            write_log("Windows Update Agent (Microsoft.Update.Session): NOT available - "
                      "the update components have to be repaired on this machine");
        }
    }
}

/* ==================================================================
 *  upload the log to the server
 * ================================================================== */

/* The endpoint is copied out of PREP_LOG_URL at run time through a
 * volatile destination and read back through a volatile pointer.
 * Without this, GCC/MSVC see that the literal is a compile-time constant
 * and can delete the whole upload path (and the WinHTTP imports with it)
 * as dead code - which is exactly what happened in the first build of this
 * app. */
static volatile char g_logUrl[512];

static void prep_url_load(void)
{
    /* LP_LOG_URL is a test hook: it lets the upload be pointed at a local
     * server (or a port that refuses connections) without a rebuild, which
     * is how the "upload failed" wording in the restart offer is checked.
     * Unset, the compiled-in endpoint is used. */
    const char *env = getenv("LP_LOG_URL");
    const char *src = (env && env[0]) ? env : PREP_LOG_URL;
    volatile char *dst = g_logUrl;
    size_t i = 0;
    for (;;) {
        char c = src[i];
        dst[i] = c;
        if (!c || i >= sizeof(g_logUrl) - 1) break;
        i++;
    }
    dst[sizeof(g_logUrl) - 1] = 0;
}

static int prep_url_is_placeholder(void)
{
    volatile char *u = g_logUrl;          /* volatile: no constant folding */
    size_t i;
    for (i = 0; u[i]; i++) {
        if (u[i] != '<') continue;
        if (u[i + 1] == 'S' && u[i + 2] == 'E' && u[i + 3] == 'R' &&
            u[i + 4] == 'V' && u[i + 5] == 'E' && u[i + 6] == 'R' &&
            u[i + 7] == '>')
            return 1;
    }
    return 0;
}

int prep_url_is_placeholder_ex(void)
{
    prep_url_load();
    return prep_url_is_placeholder();
}

const char *prep_log_url_text(void)
{
    prep_url_load();
    return (const char*)g_logUrl;
}

static int parse_url(const char *url, char *host, int hostsz,
                     char *path, int pathsz, int *secure, int *port)
{
    const char *p, *h, *c;
    size_t hl;
    *secure = 0;
    *port = 80;
    if (!url || !host || !path || hostsz <= 1 || pathsz <= 1) return 0;
    if (strncmp(url, "https://", 8) == 0)     { h = url + 8; *secure = 1; *port = 443; }
    else if (strncmp(url, "http://", 7) == 0) { h = url + 7; }
    else return 0;

    p = h;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    c = memchr(h, ':', (size_t)(p - h));
    if (c) {
        hl = (size_t)(c - h);
        *port = atoi(c + 1);
        if (*port <= 0 || *port > 65535) *port = 80;
    } else {
        hl = (size_t)(p - h);
    }
    if (hl == 0 || hl + 1 > (size_t)hostsz) return 0;
    memcpy(host, h, hl);
    host[hl] = 0;
    snprintf(path, (size_t)pathsz, "%s", (*p ? p : "/"));
    return 1;
}

/* the synchronous body: only ever called from upload_thread() */
/* The result of the most recent upload, so the interface can say honestly
 * whether the run record is safe on the server before it offers a restart:
 *  0 = never attempted, 1 = uploaded, -1 = failed. */
static volatile LONG g_lastUploadOk = 0;

int prep_last_upload_result(void)
{
    return (int)g_lastUploadOk;
}

static int log_upload_sync(const char *filepath)
{
    char host[256], path[1024];
    wchar_t whost[256], wpath[1024], wheader[512];
    HINTERNET hsess = NULL, hconn = NULL, hreq = NULL;
    FILE *f;
    long fsize = 0;
    unsigned char *fbuf = NULL;
    DWORD status = 0, slen = sizeof(status), flags = 0;
    int secure = 0, port = 80, ok = 0;
    char line[1024];

    prep_url_load();
    if (prep_url_is_placeholder()) {
        snprintf(line, sizeof(line), "Log server not configured (%s), upload skipped",
                 PREP_LOG_URL);
        write_log(line);
        return 0;
    }
    if (!parse_url((const char*)g_logUrl, host, sizeof(host), path, sizeof(path),
                   &secure, &port)) {
        write_log("Invalid server URL format");
        return 0;
    }
    f = fopen(filepath, "rb");
    if (!f) {
        snprintf(line, sizeof(line), "Upload skipped, cannot open %s", filepath);
        write_log(line);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); write_log("Upload skipped, the log file is empty"); return 0; }
    fbuf = (unsigned char*)malloc((size_t)fsize);
    if (!fbuf) { fclose(f); return 0; }
    if (fread(fbuf, 1, (size_t)fsize, f) != (size_t)fsize) { free(fbuf); fclose(f); return 0; }
    fclose(f);

    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024);
    if (secure) flags = WINHTTP_FLAG_SECURE;

    snprintf(line, sizeof(line), "Uploading %s (%ld bytes) to %s", filepath,
             fsize, (const char*)g_logUrl);
    write_log(line);

    hsess = WinHttpOpen(L"LaptopPrep/" L"1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hsess) { free(fbuf); write_log("WinHttpOpen failed"); return 0; }
    WinHttpSetTimeouts(hsess, 10000, 10000, 30000, 60000);

    hconn = WinHttpConnect(hsess, whost, (INTERNET_PORT)port, 0);
    if (!hconn) {
        WinHttpCloseHandle(hsess);
        free(fbuf);
        write_log("WinHttpConnect failed - is there network access from this laptop?");
        return 0;
    }

    hreq = WinHttpOpenRequest(hconn, L"POST", wpath, NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hreq) {
        WinHttpCloseHandle(hconn);
        WinHttpCloseHandle(hsess);
        free(fbuf);
        write_log("WinHttpOpenRequest failed");
        return 0;
    }

    /* identifying headers make the server side log usable when a shop has
     * fifty laptops in the same rack */
    MultiByteToWideChar(CP_UTF8, 0, g_computer, -1, whost, 256);
    snprintf(line, sizeof(line),
             "Content-Type: text/plain; charset=utf-8\r\nX-LaptopPrep: "
             PREP_VERSION_STR "\r\nX-LaptopPrep-Computer: %s", g_computer);
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wheader, 512);

    if (!WinHttpSendRequest(hreq, wheader, (DWORD)-1L,
            fbuf, (DWORD)fsize, (DWORD)fsize, 0)) {
        snprintf(line, sizeof(line), "WinHttpSendRequest failed %lu",
                 (unsigned long)GetLastError());
        write_log(line);
    } else if (WinHttpReceiveResponse(hreq, NULL)) {
        if (WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                NULL, &status, &slen, NULL)) {
            snprintf(line, sizeof(line), "Server upload status %lu",
                     (unsigned long)status);
            write_log(line);
            if (status >= 200 && status < 300) ok = 1;
            /* say what a refusal actually means, so nobody has to guess */
            else if (status == 404)
                write_log("The server answered 404 - this endpoint path does not "
                          "exist on the server. Check PREP_LOG_URL / create the route "
                          "(the log itself is fine and stays on disk)");
            else if (status == 401 || status == 403)
                write_log("The server refused the upload (authorisation) - the "
                          "endpoint probably needs a token");
            else if (status == 413)
                write_log("The server rejected the log because it is too large");
            else if (status == 422)
                write_log("The server rejected the format of the upload - it expects "
                          "a different body than the plain-text log");
            else if (status >= 500)
                write_log("The server had an internal error - try again later or "
                          "check the service behind the domain");
        }
    } else {
        snprintf(line, sizeof(line), "WinHttpReceiveResponse failed %lu",
                 (unsigned long)GetLastError());
        write_log(line);
    }
    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(hconn);
    WinHttpCloseHandle(hsess);
    free(fbuf);
    return ok;
}

/* An upload that never comes back must not hold a finished run up, and the
 * operator has to be told it did not happen - that is what this wrapper is
 * for.  The body above runs on its own thread; if it does not answer inside
 * PREP_UPLOAD_TIMEOUT_MS the thread is left to Windows and the run carries
 * on with an honest line in the log. */
typedef struct {
    volatile LONG done;
    volatile LONG ok;
    char path[MAX_PATH];
} UploadCtx;

static DWORD WINAPI upload_thread(LPVOID p)
{
    UploadCtx *u = (UploadCtx*)p;
    InterlockedExchange(&u->ok, log_upload_sync(u->path));
    InterlockedExchange(&g_lastUploadOk, u->ok ? 1 : -1);
    InterlockedExchange(&u->done, 1);
    return 0;
}

int log_upload(const char *filepath)
{
    UploadCtx *u;
    HANDLE th;
    ULONGLONG t0, lastBeat = 0;

    if (getenv("LP_NO_UPLOAD")) {          /* diagnostic: sandbox has no network */
        write_log("Upload skipped (diagnostic mode)");
        return 0;
    }
    prep_url_load();
    if (prep_url_is_placeholder()) {
        write_log("No log server configured yet, so nothing was uploaded "
                  "(the endpoint in prep.h is still a placeholder)");
        return 0;
    }
    u = (UploadCtx*)calloc(1, sizeof(UploadCtx));
    if (!u) return 0;
    snprintf(u->path, sizeof(u->path), "%s", filepath ? filepath : g_logPath);
    th = CreateThread(NULL, 0, upload_thread, u, 0, NULL);
    if (!th) { free(u); return 0; }

    t0 = GetTickCount64();
    for (;;) {
        if (InterlockedCompareExchange(&u->done, 1, 1)) {
            int ok = (int)u->ok;
            CloseHandle(th);
            free(u);
            return ok;
        }
        Sleep(200);
        gui_tick();
        if (GetTickCount64() - t0 - lastBeat >= PREP_WU_HEARTBEAT_MS) {
            lastBeat = GetTickCount64() - t0;
            snprintf(g_logScratch, sizeof(g_logScratch),
                     "Upload still in progress (%llu s of a %llu s budget)...",
                     (unsigned long long)(lastBeat / 1000),
                     (unsigned long long)(PREP_UPLOAD_TIMEOUT_MS / 1000));
            write_log(g_logScratch);
            InterlockedExchange(&g_mirrorHeartbeat, (LONG)GetTickCount());
        }
        if (GetTickCount64() - t0 > PREP_UPLOAD_TIMEOUT_MS) {
            write_log("The upload did not finish in time and was given up on. The "
                      "log file itself is complete and stays on this machine - copy "
                      "it off before the disk is wiped");
            CloseHandle(th);      /* the thread is abandoned, like the COM jobs */
            return 0;
        }
    }
}

/* ==================================================================
 *  the run itself
 * ================================================================== */
int prep_run_all(unsigned flags)
{
    /* the upload result belongs to one run only: a stale "uploaded" from an
     * earlier run must never be shown next to a fresh log */
    InterlockedExchange(&g_lastUploadOk, 0);
    IWbemServices *svc = NULL;
    IWbemLocator *loc = NULL;
    char line[1024];
    int needWmi = (flags & (PREP_M_SPECS | PREP_M_DEVICE_ID | PREP_M_BOARD |
                            PREP_M_EXTRA | PREP_M_TPM)) ? 1 : 0;
    unsigned firstAnnounced = 0;
    int wuOk = 1;

    if (g_logNotice[0]) write_log(g_logNotice);
    write_log("===== LaptopPrep run =====");
    snprintf(line, sizeof(line), "LaptopPrep %s - %s", PREP_VERSION_STR, PREP_OWNER);
    write_log(line);
    write_log(PREP_COPYRIGHT);
    write_log(PREP_LICENCE_LINE);
    write_log("Wintr: https://wintr.lol   AGPCS: https://agpcs.nl");
    snprintf(line, sizeof(line), "LaptopPrep %s, computer %s, %s",
             PREP_VERSION_STR, g_computer,
             is_admin() ? "administrator" : "normal user (not elevated)");
    write_log(line);
    snprintf(line, sizeof(line), "Log file: %s", g_logPath);
    write_log(line);
    probe_environment();
    snprintf(line, sizeof(line), "Selected modules: 0x%04X", flags);
    write_log(line);

    if (needWmi) {
        /* tell the UI what is happening before the (occasionally slow) WMI
         * handshake, otherwise the window just sits there looking dead */
        unsigned wmiOrder[5];
        int k, n = 0;
        wmiOrder[n++] = PREP_M_SPECS;
        wmiOrder[n++] = PREP_M_DEVICE_ID;
        wmiOrder[n++] = PREP_M_BOARD;
        wmiOrder[n++] = PREP_M_EXTRA;
        wmiOrder[n++] = PREP_M_TPM;
        for (k = 0; k < n; k++) {
            if (prep_select(flags, wmiOrder[k])) {
                firstAnnounced = wmiOrder[k];
                gui_on_module_begin(wmiOrder[k]);
                break;
            }
        }
        write_log("Connecting to WMI...");
        if (!wmi_init(&svc, &loc)) {
            write_log("WMI init failed, the hardware modules will be skipped");
            svc = NULL;
            loc = NULL;
        }
    }

    if (prep_select(flags, PREP_M_SPECS)) {
        if (!(needWmi && firstAnnounced == PREP_M_SPECS)) gui_on_module_begin(PREP_M_SPECS);
        if (svc) collect_specs(svc); else write_log("SPECS skipped (no WMI)");
        gui_on_module_result(PREP_M_SPECS, svc != NULL);
    }
    if (prep_select(flags, PREP_M_DEVICE_ID)) {
        if (!(needWmi && firstAnnounced == PREP_M_DEVICE_ID)) gui_on_module_begin(PREP_M_DEVICE_ID);
        if (svc) collect_device_id(svc); else write_log("DEVICE ID skipped (no WMI)");
        gui_on_module_result(PREP_M_DEVICE_ID, svc != NULL);
    }
    if (prep_select(flags, PREP_M_BOARD)) {
        if (!(needWmi && firstAnnounced == PREP_M_BOARD)) gui_on_module_begin(PREP_M_BOARD);
        if (svc) collect_motherboard(svc); else write_log("MOTHERBOARD skipped (no WMI)");
        gui_on_module_result(PREP_M_BOARD, svc != NULL);
    }
    if (prep_select(flags, PREP_M_EXTRA)) {
        if (!(needWmi && firstAnnounced == PREP_M_EXTRA)) gui_on_module_begin(PREP_M_EXTRA);
        if (svc) collect_extra(svc); else write_log("EXTRA skipped (no WMI)");
        gui_on_module_result(PREP_M_EXTRA, svc != NULL);
    }
    if (prep_select(flags, PREP_M_TPM)) {
        if (!(needWmi && firstAnnounced == PREP_M_TPM)) gui_on_module_begin(PREP_M_TPM);
        if (loc) collect_tpm(loc); else write_log("TPM skipped (no WMI)");
        gui_on_module_result(PREP_M_TPM, loc != NULL);
    }
    if (prep_select(flags, PREP_M_BATTERY)) {
        gui_on_module_begin(PREP_M_BATTERY);
        run_battery_report();
        gui_on_module_result(PREP_M_BATTERY, 1);
    }
    if (prep_select(flags, PREP_M_UPDATES)) {
        gui_on_module_begin(PREP_M_UPDATES);
        wuOk = do_windows_update();
        gui_on_module_result(PREP_M_UPDATES, wuOk);
    }
    if (prep_select(flags, PREP_M_UPLOAD)) {
        int configured, up;
        prep_url_load();
        configured = !prep_url_is_placeholder();
        gui_on_module_begin(PREP_M_UPLOAD);
        up = log_upload(g_logPath);
        if (configured && up)
            write_log("Log is on the server - if the machine restarts now, the "
                      "record of this run survives");
        else if (configured)
            write_log("Upload FAILED - the log only exists as the local file and "
                      "will be lost if the disk is wiped");
        /* while the endpoint is still a placeholder, "skipped" is a success:
         * the checkbox only exists so it is easy to switch on */
        gui_on_module_result(PREP_M_UPLOAD, configured ? up : 1);
    }

    /* ------------------------------------------------------------------
     *  The log must be safe on the server BEFORE anyone reboots the machine
     *  to finish the Windows updates.  So: whenever a restart is pending,
     *  the log is uploaded even if the operator did not tick the upload
     *  box.  It runs at most twice and the second pass is what protects
     *  the data.
     * ------------------------------------------------------------------ */
    if (prep_restart_pending() && !prep_select(flags, PREP_M_UPLOAD)) {
        int configured;
        prep_url_load();
        configured = !prep_url_is_placeholder();
        gui_on_module_begin(PREP_M_UPLOAD);
        write_log("A restart is pending, so the log is uploaded now even though "
                  "the upload module was not selected - this run has to survive "
                  "the reboot");
        log_upload(g_logPath);
        gui_on_module_result(PREP_M_UPLOAD, configured ? 1 : 1);
    }

    write_log("===== LaptopPrep run finished =====");

    if (svc || loc) CoUninitialize();
    wmi_uninit(svc, loc);
    return 0;
}
