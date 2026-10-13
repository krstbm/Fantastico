#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_DCOM
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wbemidl.h>
#include <objbase.h>
#include <oleauto.h>
#include <winhttp.h>
#include <iptypes.h>
#include <iphlpapi.h>
#include <shellapi.h>

#define PREP_LOG_URL "http://localhost/api/v1/prep-logs"
#define LOGDIR "C:\\RefurbLogs"

char g_logPath[MAX_PATH];
char g_deviceCsv[MAX_PATH];
char g_battHtml[MAX_PATH];
char g_computer[256];

void write_log(const char *msg) {
    SYSTEMTIME st;
    FILE *f;
    GetLocalTime(&st);
    f = fopen(g_logPath, "a");
    if (f) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d - %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
    printf("%04d-%02d-%02d %02d:%02d:%02d - %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, msg);
    fflush(stdout);
}

void write_logf(const char *fmt, const char *a, const char *b) {
    char buf[4096];
    if (a && b) snprintf(buf, sizeof(buf), fmt, a, b);
    else if (a) snprintf(buf, sizeof(buf), fmt, a);
    else snprintf(buf, sizeof(buf), "%s", fmt);
    write_log(buf);
}

int is_admin() {
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    PSID group = NULL;
    if (AllocateAndInitializeSid(&auth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(NULL, group, &admin);
        FreeSid(group);
    }
    return admin ? 1 : 0;
}

void relaunch_admin() {
    char path[MAX_PATH];
    wchar_t wpath[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    ShellExecuteW(NULL, L"runas", wpath, NULL, NULL, SW_SHOWNORMAL);
    exit(0);
}

void ensure_logdir() {
    DWORD sz = sizeof(g_computer);
    GetComputerNameA(g_computer, &sz);
    CreateDirectoryA(LOGDIR, NULL);
    snprintf(g_logPath, sizeof(g_logPath), "%s\\%s_prep.log", LOGDIR, g_computer);
    snprintf(g_deviceCsv, sizeof(g_deviceCsv), "%s\\%s_deviceID.csv", LOGDIR, g_computer);
    snprintf(g_battHtml, sizeof(g_battHtml), "%s\\battery_report.html", LOGDIR);
}

int delete_pattern(const char *pattern) {
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char full[MAX_PATH];
    int n = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(full, sizeof(full), "%s\\%s", LOGDIR, fd.cFileName);
        if (DeleteFileA(full)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

void clean_old_logs() {
    char pat[MAX_PATH];
    char line[512];
    int total = 0;
    snprintf(pat, sizeof(pat), "%s\\*_prep.log", LOGDIR);
    total += delete_pattern(pat);
    snprintf(pat, sizeof(pat), "%s\\*_deviceID.csv", LOGDIR);
    total += delete_pattern(pat);
    snprintf(pat, sizeof(pat), "%s\\battery_report.html", LOGDIR);
    total += delete_pattern(pat);
    if (total > 0) {
        snprintf(line, sizeof(line), "Deleted %d old refurb log file(s)", total);
        write_log(line);
    } else {
        write_log("No old refurb logs found");
    }
}

int variant_to_str(VARIANT *v, char *out, int outsz) {
    out[0] = 0;
    if (v->vt == VT_NULL || v->vt == VT_EMPTY) {
        snprintf(out, outsz, "%s", "N/A");
        return 1;
    }
    if (v->vt == VT_BSTR && v->bstrVal) {
        WideCharToMultiByte(CP_ACP, 0, v->bstrVal, -1, out, outsz, NULL, NULL);
        return 1;
    }
    if (v->vt == VT_I1) { snprintf(out, outsz, "%d", (int)v->cVal); return 1; }
    if (v->vt == VT_UI1) { snprintf(out, outsz, "%u", (unsigned int)v->bVal); return 1; }
    if (v->vt == VT_I2) { snprintf(out, outsz, "%d", (int)v->iVal); return 1; }
    if (v->vt == VT_UI2) { snprintf(out, outsz, "%u", (unsigned int)v->uiVal); return 1; }
    if (v->vt == VT_I4 || v->vt == VT_INT) { snprintf(out, outsz, "%ld", v->lVal); return 1; }
    if (v->vt == VT_UI4 || v->vt == VT_UINT) { snprintf(out, outsz, "%lu", v->ulVal); return 1; }
    if (v->vt == VT_I8) { snprintf(out, outsz, "%lld", (long long)v->llVal); return 1; }
    if (v->vt == VT_UI8) { snprintf(out, outsz, "%llu", (unsigned long long)v->ullVal); return 1; }
    if (v->vt == VT_R4) { snprintf(out, outsz, "%f", (double)v->fltVal); return 1; }
    if (v->vt == VT_R8) { snprintf(out, outsz, "%f", v->dblVal); return 1; }
    if (v->vt == VT_BOOL) { snprintf(out, outsz, "%s", (v->boolVal == VARIANT_TRUE) ? "True" : "False"); return 1; }
    if ((v->vt & VT_ARRAY) && (v->vt & VT_BSTR)) {
        SAFEARRAY *sa = v->parray;
        LONG lb = 0, ub = -1;
        char tmp[512];
        out[0] = 0;
        if (!sa) { snprintf(out, outsz, "%s", "N/A"); return 1; }
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        for (LONG i = lb; i <= ub; i++) {
            BSTR bs = NULL;
            SafeArrayGetElement(sa, &i, &bs);
            if (bs) {
                WideCharToMultiByte(CP_ACP, 0, bs, -1, tmp, sizeof(tmp), NULL, NULL);
                if (strlen(out) + strlen(tmp) + 3 < (size_t)outsz) {
                    if (strlen(out) > 0) strcat(out, ", ");
                    strcat(out, tmp);
                }
                SysFreeString(bs);
            }
        }
        if (strlen(out) == 0) snprintf(out, outsz, "%s", "N/A");
        return 1;
    }
    if ((v->vt & VT_ARRAY) && ((v->vt & ~VT_ARRAY) == VT_I4 || (v->vt & ~VT_ARRAY) == VT_UI4)) {
        snprintf(out, outsz, "%s", "(array)");
        return 1;
    }
    {
        VARIANT vc;
        VariantInit(&vc);
        if (SUCCEEDED(VariantChangeType(&vc, v, 0, VT_BSTR)) && vc.bstrVal) {
            WideCharToMultiByte(CP_ACP, 0, vc.bstrVal, -1, out, outsz, NULL, NULL);
            VariantClear(&vc);
            return 1;
        }
        VariantClear(&vc);
    }
    snprintf(out, outsz, "%s", "N/A");
    return 0;
}

int wmi_init(IWbemServices **ppSvc, IWbemLocator **ppLoc) {
    HRESULT hres;
    BSTR ns;
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres) && hres != RPC_E_CHANGED_MODE) return 0;
    hres = CoInitializeSecurity(NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL);
    if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
        CoUninitialize();
        return 0;
    }
    hres = CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        &IID_IWbemLocator, (LPVOID*)ppLoc);
    if (FAILED(hres)) {
        CoUninitialize();
        return 0;
    }
    ns = SysAllocString(L"ROOT\\CIMV2");
    hres = (*ppLoc)->lpVtbl->ConnectServer(*ppLoc, ns, NULL, NULL,
        NULL, 0, NULL, NULL, ppSvc);
    SysFreeString(ns);
    if (FAILED(hres)) {
        (*ppLoc)->lpVtbl->Release(*ppLoc);
        CoUninitialize();
        return 0;
    }
    hres = CoSetProxyBlanket((IUnknown*)*ppSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
        NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);
    if (FAILED(hres)) {
        (*ppSvc)->lpVtbl->Release(*ppSvc);
        (*ppLoc)->lpVtbl->Release(*ppLoc);
        CoUninitialize();
        return 0;
    }
    return 1;
}

int wmi_get_first(IWbemServices *svc, const wchar_t *wql,
    const wchar_t *prop, char *out, int outsz) {
    HRESULT hres;
    BSTR lang, query, pname;
    IEnumWbemClassObject *penum = NULL;
    IWbemClassObject *pobj = NULL;
    ULONG ret = 0;
    int ok = 0;
    out[0] = 0;
    lang = SysAllocString(L"WQL");
    query = SysAllocString(wql);
    pname = SysAllocString(prop);
    hres = svc->lpVtbl->ExecQuery(svc, lang, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &penum);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hres) || !penum) {
        SysFreeString(pname);
        return 0;
    }
    if (penum->lpVtbl->Next(penum, WBEM_INFINITE, 1, &pobj, &ret) == S_OK && ret) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(pobj->lpVtbl->Get(pobj, pname, 0, &v, 0, 0))) {
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
    wchar_t **props, char **labels, int nprops, const char *section) {
    HRESULT hres;
    BSTR lang, query;
    IEnumWbemClassObject *penum = NULL;
    IWbemClassObject *pobj = NULL;
    ULONG ret = 0;
    int idx = 0;
    char line[4096];
    char val[2048];
    BSTR *pnames;
    pnames = (BSTR*)malloc(sizeof(BSTR) * (size_t)nprops);
    if (!pnames) return;
    for (int i = 0; i < nprops; i++) pnames[i] = SysAllocString(props[i]);
    lang = SysAllocString(L"WQL");
    query = SysAllocString(wql);
    hres = svc->lpVtbl->ExecQuery(svc, lang, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &penum);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hres) || !penum) {
        for (int i = 0; i < nprops; i++) SysFreeString(pnames[i]);
        free(pnames);
        snprintf(line, sizeof(line), "%s: query failed", section);
        write_log(line);
        return;
    }
    while (penum->lpVtbl->Next(penum, WBEM_INFINITE, 1, &pobj, &ret) == S_OK && ret) {
        idx++;
        if (idx == 1) {
            snprintf(line, sizeof(line), "----- %s -----", section);
            write_log(line);
        } else {
            snprintf(line, sizeof(line), "--- %s [%d] ---", section, idx);
            write_log(line);
        }
        for (int i = 0; i < nprops; i++) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(pobj->lpVtbl->Get(pobj, pnames[i], 0, &v, 0, 0))) {
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
    }
    if (idx == 0) {
        snprintf(line, sizeof(line), "%s: no data", section);
        write_log(line);
    }
    for (int i = 0; i < nprops; i++) SysFreeString(pnames[i]);
    free(pnames);
    penum->lpVtbl->Release(penum);
}

void collect_specs(IWbemServices *svc) {
    char cpu[1024] = {0};
    char osCap[1024] = {0};
    char osVer[256] = {0};
    char osBuild[256] = {0};
    char osArch[256] = {0};
    char line[4096];
    MEMORYSTATUSEX ms;
    double ramGB;
    ms.dwLength = sizeof(ms);
    write_log("Collecting system specs...");
    wmi_get_first(svc, L"SELECT Name FROM Win32_Processor", L"Name", cpu, sizeof(cpu));
    wmi_get_first(svc, L"SELECT Caption FROM Win32_OperatingSystem", L"Caption", osCap, sizeof(osCap));
    wmi_get_first(svc, L"SELECT Version FROM Win32_OperatingSystem", L"Version", osVer, sizeof(osVer));
    wmi_get_first(svc, L"SELECT BuildNumber FROM Win32_OperatingSystem", L"BuildNumber", osBuild, sizeof(osBuild));
    wmi_get_first(svc, L"SELECT OSArchitecture FROM Win32_OperatingSystem", L"OSArchitecture", osArch, sizeof(osArch));
    GlobalMemoryStatusEx(&ms);
    ramGB = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    snprintf(line, sizeof(line), "CPU: %s", cpu);
    write_log(line);
    snprintf(line, sizeof(line), "RAM: %.2f GB", ramGB);
    write_log(line);
    snprintf(line, sizeof(line), "OS: %s %s Build %s %s", osCap, osVer, osBuild, osArch);
    write_log(line);
    {
        wchar_t *p1[] = {L"Model", L"Size", L"Status", L"InterfaceType", L"SerialNumber"};
        char *l1[] = {"Model", "SizeBytes", "Status", "Interface", "Serial"};
        wmi_log_query(svc, L"SELECT Model, Size, Status, InterfaceType, SerialNumber FROM Win32_DiskDrive",
            p1, l1, 5, "DISK DRIVES");
    }
    {
        wchar_t *p2[] = {L"DeviceID", L"Size", L"FreeSpace", L"FileSystem", L"VolumeName"};
        char *l2[] = {"Drive", "SizeBytes", "FreeBytes", "FS", "Label"};
        wmi_log_query(svc, L"SELECT DeviceID, Size, FreeSpace, FileSystem, VolumeName FROM Win32_LogicalDisk WHERE DriveType=3",
            p2, l2, 5, "LOGICAL DISKS");
    }
}

void collect_device_id(IWbemServices *svc) {
    char mfr[512] = {0};
    char model[512] = {0};
    char biosSN[512] = {0};
    char boardSN[512] = {0};
    char uuid[512] = {0};
    char asset[512] = {0};
    char encSN[512] = {0};
    char macs[2048] = {0};
    char line[4096];
    FILE *csv;
    ULONG sz = 0;
    IP_ADAPTER_ADDRESSES *addrs = NULL;
    IP_ADAPTER_ADDRESSES *cur = NULL;
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
    addrs = (IP_ADAPTER_ADDRESSES*)malloc(sz);
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
                    cur->PhysicalAddress[0], cur->PhysicalAddress[1],
                    cur->PhysicalAddress[2], cur->PhysicalAddress[3],
                    cur->PhysicalAddress[4], cur->PhysicalAddress[5]);
                if (strlen(macs) + strlen(tmp) + 3 < sizeof(macs)) {
                    if (strlen(macs) > 0) strcat(macs, ", ");
                    strcat(macs, tmp);
                }
            }
        }
        free(addrs);
    }
    if (strlen(macs) == 0) snprintf(macs, sizeof(macs), "%s", "N/A");
    write_log("----- DEVICE ID -----");
    snprintf(line, sizeof(line), "ComputerName: %s", g_computer);
    write_log(line);
    snprintf(line, sizeof(line), "Manufacturer: %s", mfr);
    write_log(line);
    snprintf(line, sizeof(line), "Model: %s", model);
    write_log(line);
    snprintf(line, sizeof(line), "BIOS Serial: %s", biosSN);
    write_log(line);
    snprintf(line, sizeof(line), "Motherboard SN: %s", boardSN);
    write_log(line);
    snprintf(line, sizeof(line), "System UUID: %s", uuid);
    write_log(line);
    snprintf(line, sizeof(line), "MAC Address(es): %s", macs);
    write_log(line);
    snprintf(line, sizeof(line), "Asset Tag: %s", asset);
    write_log(line);
    snprintf(line, sizeof(line), "Enclosure SN: %s", encSN);
    write_log(line);
    write_log("---------------------");
    csv = fopen(g_deviceCsv, "w");
    if (csv) {
        fprintf(csv, "ComputerName,Manufacturer,Model,BIOS_Serial,Motherboard_SN,System_UUID,MAC_Addresses,Asset_Tag,Enclosure_SN\n");
        fprintf(csv, "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
            g_computer, mfr, model, biosSN, boardSN, uuid, macs, asset, encSN);
        fclose(csv);
        snprintf(line, sizeof(line), "Device CSV written: %s", g_deviceCsv);
        write_log(line);
    } else {
        write_log("Failed to write device CSV");
    }
}

void collect_motherboard(IWbemServices *svc) {
    write_log("Collecting motherboard details...");
    {
        wchar_t *p[] = {L"Manufacturer", L"Product", L"Version", L"SerialNumber", L"PartNumber", L"Tag"};
        char *l[] = {"Mfr", "Product", "Version", "Serial", "PartNumber", "Tag"};
        wmi_log_query(svc, L"SELECT Manufacturer, Product, Version, SerialNumber, PartNumber, Tag FROM Win32_BaseBoard",
            p, l, 6, "MOTHERBOARD");
    }
    {
        wchar_t *p[] = {L"PrimaryBusType", L"RevisionNumber", L"SystemName", L"Availability", L"Status"};
        char *l[] = {"BusType", "Revision", "System", "Availability", "Status"};
        wmi_log_query(svc, L"SELECT PrimaryBusType, RevisionNumber, SystemName, Availability, Status FROM Win32_MotherboardDevice",
            p, l, 5, "MOTHERBOARD DEVICE");
    }
    {
        wchar_t *p[] = {L"Manufacturer", L"SMBIOSBIOSVersion", L"Version", L"SerialNumber", L"ReleaseDate", L"CurrentLanguage"};
        char *l[] = {"Mfr", "SMBIOSVer", "Version", "Serial", "ReleaseDate", "Lang"};
        wmi_log_query(svc, L"SELECT Manufacturer, SMBIOSBIOSVersion, Version, SerialNumber, ReleaseDate, CurrentLanguage FROM Win32_BIOS",
            p, l, 6, "BIOS");
    }
    {
        wchar_t *p[] = {L"SlotDesignation", L"CurrentUsage", L"MaxDataWidth", L"SupportsHotPlug", L"Status"};
        char *l[] = {"Slot", "Usage", "MaxWidth", "HotPlug", "Status"};
        wmi_log_query(svc, L"SELECT SlotDesignation, CurrentUsage, MaxDataWidth, SupportsHotPlug, Status FROM Win32_SystemSlot",
            p, l, 5, "SYSTEM SLOTS");
    }
    {
        wchar_t *p[] = {L"DeviceID", L"DeviceLocator", L"BusType", L"BusNumber", L"BusAddress"};
        char *l[] = {"Device", "Locator", "BusType", "BusNum", "BusAddr"};
        wmi_log_query(svc, L"SELECT DeviceID, DeviceLocator, BusType, BusNumber, BusAddress FROM Win32_Bus",
            p, l, 5, "BUSES");
    }
}

void collect_extra(IWbemServices *svc) {
    write_log("Collecting extended hardware checks...");
    {
        wchar_t *p[] = {L"Name", L"Manufacturer", L"MaxClockSpeed", L"NumberOfCores", L"NumberOfLogicalProcessors", L"SocketDesignation", L"Status"};
        char *l[] = {"Name", "Mfr", "MaxMHz", "Cores", "Threads", "Socket", "Status"};
        wmi_log_query(svc, L"SELECT Name, Manufacturer, MaxClockSpeed, NumberOfCores, NumberOfLogicalProcessors, SocketDesignation, Status FROM Win32_Processor",
            p, l, 7, "CPU DETAIL");
    }
    {
        wchar_t *p[] = {L"BankLabel", L"Capacity", L"Speed", L"Manufacturer", L"PartNumber", L"SerialNumber", L"SMBIOSMemoryType"};
        char *l[] = {"Bank", "Bytes", "MHz", "Mfr", "Part", "Serial", "Type"};
        wmi_log_query(svc, L"SELECT BankLabel, Capacity, Speed, Manufacturer, PartNumber, SerialNumber, SMBIOSMemoryType FROM Win32_PhysicalMemory",
            p, l, 7, "MEMORY MODULES");
    }
    {
        wchar_t *p[] = {L"Name", L"DriverVersion", L"AdapterRAM", L"VideoProcessor", L"Status", L"CurrentHorizontalResolution", L"CurrentVerticalResolution"};
        char *l[] = {"Name", "Driver", "VRAM", "Chip", "Status", "ResX", "ResY"};
        wmi_log_query(svc, L"SELECT Name, DriverVersion, AdapterRAM, VideoProcessor, Status, CurrentHorizontalResolution, CurrentVerticalResolution FROM Win32_VideoController",
            p, l, 7, "GPU");
    }
    {
        wchar_t *p[] = {L"Name", L"Status", L"Availability"};
        char *l[] = {"Name", "Status", "Avail"};
        wmi_log_query(svc, L"SELECT Name, Status, Availability FROM Win32_SoundDevice",
            p, l, 3, "AUDIO");
    }
    {
        wchar_t *p[] = {L"Name", L"Description", L"Status"};
        char *l[] = {"Name", "Desc", "Status"};
        wmi_log_query(svc, L"SELECT Name, Description, Status FROM Win32_Keyboard",
            p, l, 3, "KEYBOARD");
    }
    {
        wchar_t *p[] = {L"Name", L"PointingType", L"Status"};
        char *l[] = {"Name", "Type", "Status"};
        wmi_log_query(svc, L"SELECT Name, PointingType, Status FROM Win32_PointingDevice",
            p, l, 3, "POINTING DEVICE");
    }
    {
        wchar_t *p[] = {L"Name", L"Chemistry", L"DesignCapacity", L"FullChargeCapacity", L"EstimatedChargeRemaining", L"BatteryStatus", L"Status"};
        char *l[] = {"Name", "Chem", "Design_mWh", "Full_mWh", "ChargePct", "BattStatus", "Status"};
        wmi_log_query(svc, L"SELECT Name, Chemistry, DesignCapacity, FullChargeCapacity, EstimatedChargeRemaining, BatteryStatus, Status FROM Win32_Battery",
            p, l, 7, "BATTERY WMI");
    }
    {
        wchar_t *p[] = {L"Name", L"NetConnectionStatus", L"Speed", L"MACAddress", L"AdapterType"};
        char *l[] = {"Name", "ConnStatus", "Speed", "MAC", "Type"};
        wmi_log_query(svc, L"SELECT Name, NetConnectionStatus, Speed, MACAddress, AdapterType FROM Win32_NetworkAdapter WHERE PhysicalAdapter=TRUE",
            p, l, 5, "NETWORK ADAPTERS");
    }
    {
        wchar_t *p[] = {L"Name", L"Status"};
        char *l[] = {"Name", "Status"};
        wmi_log_query(svc, L"SELECT Name, Status FROM Win32_USBController",
            p, l, 2, "USB CONTROLLERS");
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

void collect_tpm(IWbemLocator *loc) {
    HRESULT hres;
    BSTR ns;
    IWbemServices *svc2 = NULL;
    char line[1024];
    char v1[512] = {0};
    char v2[512] = {0};
    char v3[512] = {0};
    ns = SysAllocString(L"ROOT\\CIMV2\\Security\\MicrosoftTpm");
    hres = loc->lpVtbl->ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc2);
    SysFreeString(ns);
    if (FAILED(hres) || !svc2) {
        write_log("TPM: not present or no access");
        return;
    }
    CoSetProxyBlanket((IUnknown*)svc2, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    wmi_get_first(svc2, L"SELECT IsActivated_InitialValue FROM Win32_Tpm", L"IsActivated_InitialValue", v1, sizeof(v1));
    wmi_get_first(svc2, L"SELECT IsEnabled_InitialValue FROM Win32_Tpm", L"IsEnabled_InitialValue", v2, sizeof(v2));
    wmi_get_first(svc2, L"SELECT SpecVersion FROM Win32_Tpm", L"SpecVersion", v3, sizeof(v3));
    snprintf(line, sizeof(line), "TPM Activated=%s Enabled=%s Spec=%s", v1, v2, v3);
    write_log(line);
    svc2->lpVtbl->Release(svc2);
}

void run_battery_report() {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[1024];
    char line[1024];
    DWORD code = 0;
    write_log("Generating battery report...");
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    snprintf(cmd, sizeof(cmd), "powercfg /batteryreport /output \"%s\"", g_battHtml);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        snprintf(line, sizeof(line), "powercfg exit code: %lu", (unsigned long)code);
        write_log(line);
        if (GetFileAttributesA(g_battHtml) != INVALID_FILE_ATTRIBUTES) {
            snprintf(line, sizeof(line), "Battery report written: %s", g_battHtml);
            write_log(line);
        } else {
            write_log("Battery report file not found after powercfg");
        }
    } else {
        snprintf(line, sizeof(line), "Failed to launch powercfg, error %lu", (unsigned long)GetLastError());
        write_log(line);
    }
}

void run_process_wait(const char *cmdline, int timeoutMs) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[1024];
    char line[1024];
    DWORD code = 0;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    snprintf(cmd, sizeof(cmd), "%s", cmdline);
    snprintf(line, sizeof(line), "Run: %s", cmd);
    write_log(line);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)timeoutMs);
        if (w == WAIT_TIMEOUT) {
            write_log("Process timed out, terminating");
            TerminateProcess(pi.hProcess, 1);
        }
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        snprintf(line, sizeof(line), "Exit code: %lu", (unsigned long)code);
        write_log(line);
    } else {
        snprintf(line, sizeof(line), "CreateProcess failed, error %lu", (unsigned long)GetLastError());
        write_log(line);
    }
}

IDispatch* wua_create(const wchar_t *progid) {
    CLSID clsid;
    IDispatch *d = NULL;
    if (FAILED(CLSIDFromProgID(progid, &clsid))) return NULL;
    if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
        &IID_IDispatch, (void**)&d))) return NULL;
    return d;
}

int disp_call(IDispatch *d, const wchar_t *name, WORD flags, VARIANT *args, int nargs, VARIANT *result) {
    DISPID id;
    DISPPARAMS dp;
    DISPID putid = DISPID_PROPERTYPUT;
    VARIANT *rev = NULL;
    EXCEPINFO ex;
    UINT uerr = 0;
    HRESULT h;
    LPOLESTR nm = (LPOLESTR)name;
    memset(&dp, 0, sizeof(dp));
    if (FAILED(d->lpVtbl->GetIDsOfNames(d, &IID_NULL, &nm, 1, LOCALE_SYSTEM_DEFAULT, &id))) return 0;
    if (nargs > 0) {
        rev = (VARIANT*)malloc(sizeof(VARIANT) * (size_t)nargs);
        if (!rev) return 0;
        for (int i = 0; i < nargs; i++) {
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
        for (int i = 0; i < nargs; i++) VariantClear(&rev[i]);
        free(rev);
    }
    return SUCCEEDED(h) ? 1 : 0;
}

long disp_get_long(IDispatch *d, const wchar_t *name, long def) {
    VARIANT r;
    VARIANT c;
    long v = def;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_I4))) v = c.lVal;
        VariantClear(&c);
        VariantClear(&r);
    }
    return v;
}

int disp_get_bool(IDispatch *d, const wchar_t *name, int def) {
    VARIANT r;
    VARIANT c;
    int v = def;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_BOOL))) v = (c.boolVal == VARIANT_TRUE) ? 1 : 0;
        VariantClear(&c);
        VariantClear(&r);
    }
    return v;
}

IDispatch* disp_get_disp(IDispatch *d, const wchar_t *name) {
    VARIANT r;
    IDispatch *o = NULL;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        if (r.vt == VT_DISPATCH && r.pdispVal) {
            o = r.pdispVal;
            o->lpVtbl->AddRef(o);
        }
        VariantClear(&r);
    }
    return o;
}

IDispatch* disp_call_disp(IDispatch *d, const wchar_t *name, VARIANT *args, int nargs) {
    VARIANT r;
    IDispatch *o = NULL;
    if (disp_call(d, name, DISPATCH_METHOD, args, nargs, &r)) {
        if (r.vt == VT_DISPATCH && r.pdispVal) {
            o = r.pdispVal;
            o->lpVtbl->AddRef(o);
        }
        VariantClear(&r);
    }
    return o;
}

void disp_put(IDispatch *d, const wchar_t *name, VARIANT *v) {
    disp_call(d, name, DISPATCH_PROPERTYPUT, v, 1, NULL);
}

void disp_get_str(IDispatch *d, const wchar_t *name, char *out, int outsz) {
    VARIANT r;
    VARIANT c;
    out[0] = 0;
    if (disp_call(d, name, DISPATCH_PROPERTYGET, NULL, 0, &r)) {
        VariantInit(&c);
        if (SUCCEEDED(VariantChangeType(&c, &r, 0, VT_BSTR)) && c.bstrVal) {
            WideCharToMultiByte(CP_ACP, 0, c.bstrVal, -1, out, outsz, NULL, NULL);
        }
        VariantClear(&c);
        VariantClear(&r);
    }
    if (!out[0]) snprintf(out, outsz, "%s", "N/A");
}

const char* wua_code_str(long code) {
    if (code == 0) return "NotStarted";
    if (code == 1) return "InProgress";
    if (code == 2) return "Succeeded";
    if (code == 3) return "SucceededWithErrors";
    if (code == 4) return "Failed";
    if (code == 5) return "Aborted";
    return "Unknown";
}

void do_windows_update() {
    IDispatch *session = NULL;
    IDispatch *searcher = NULL;
    IDispatch *searchResult = NULL;
    IDispatch *updates = NULL;
    IDispatch *downloader = NULL;
    IDispatch *dlResult = NULL;
    IDispatch *installer = NULL;
    IDispatch *instResult = NULL;
    VARIANT a;
    VARIANT u;
    char line[1024];
    char title[1024];
    long count = 0;
    long rc = 0;
    write_log("Starting Windows Update phase via WUA API...");
    session = wua_create(L"Microsoft.Update.Session");
    if (!session) {
        write_log("Failed to create Microsoft.Update.Session, updates skipped");
        return;
    }
    VariantInit(&a);
    a.vt = VT_BSTR;
    a.bstrVal = SysAllocString(L"LaptopPrep");
    disp_put(session, L"ClientApplicationID", &a);
    VariantClear(&a);
    searcher = disp_call_disp(session, L"CreateUpdateSearcher", NULL, 0);
    if (!searcher) {
        write_log("CreateUpdateSearcher failed");
        session->lpVtbl->Release(session);
        return;
    }
    write_log("Searching for updates...");
    VariantInit(&a);
    a.vt = VT_BSTR;
    a.bstrVal = SysAllocString(L"IsInstalled=0 and Type='Software' and IsHidden=0");
    searchResult = disp_call_disp(searcher, L"Search", &a, 1);
    VariantClear(&a);
    if (!searchResult) {
        write_log("Update search failed");
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    rc = disp_get_long(searchResult, L"ResultCode", -1);
    snprintf(line, sizeof(line), "Search result: %s (%ld)", wua_code_str(rc), rc);
    write_log(line);
    updates = disp_get_disp(searchResult, L"Updates");
    if (!updates) {
        write_log("No update collection returned");
        searchResult->lpVtbl->Release(searchResult);
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    count = disp_get_long(updates, L"Count", 0);
    snprintf(line, sizeof(line), "Updates found: %ld", count);
    write_log(line);
    if (count <= 0) {
        write_log("System is up to date");
        updates->lpVtbl->Release(updates);
        searchResult->lpVtbl->Release(searchResult);
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    for (long i = 0; i < count; i++) {
        IDispatch *upd = NULL;
        VariantInit(&a);
        a.vt = VT_I4;
        a.lVal = i;
        upd = disp_call_disp(updates, L"Item", &a, 1);
        VariantClear(&a);
        if (upd) {
            disp_call(upd, L"AcceptEula", DISPATCH_METHOD, NULL, 0, NULL);
            disp_get_str(upd, L"Title", title, sizeof(title));
            snprintf(line, sizeof(line), "Update[%ld]: %s", i, title);
            write_log(line);
            upd->lpVtbl->Release(upd);
        }
    }
    write_log("Downloading updates...");
    downloader = disp_call_disp(session, L"CreateUpdateDownloader", NULL, 0);
    if (!downloader) {
        write_log("CreateUpdateDownloader failed");
        updates->lpVtbl->Release(updates);
        searchResult->lpVtbl->Release(searchResult);
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    VariantInit(&u);
    u.vt = VT_DISPATCH;
    u.pdispVal = updates;
    disp_put(downloader, L"Updates", &u);
    dlResult = disp_call_disp(downloader, L"Download", NULL, 0);
    if (!dlResult) {
        write_log("Download call failed");
        downloader->lpVtbl->Release(downloader);
        updates->lpVtbl->Release(updates);
        searchResult->lpVtbl->Release(searchResult);
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    rc = disp_get_long(dlResult, L"ResultCode", -1);
    snprintf(line, sizeof(line), "Download result: %s (%ld)", wua_code_str(rc), rc);
    write_log(line);
    write_log("Installing updates...");
    installer = disp_call_disp(session, L"CreateUpdateInstaller", NULL, 0);
    if (!installer) {
        write_log("CreateUpdateInstaller failed");
        dlResult->lpVtbl->Release(dlResult);
        downloader->lpVtbl->Release(downloader);
        updates->lpVtbl->Release(updates);
        searchResult->lpVtbl->Release(searchResult);
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    disp_put(installer, L"Updates", &u);
    instResult = disp_call_disp(installer, L"Install", NULL, 0);
    if (!instResult) {
        write_log("Install call failed");
        installer->lpVtbl->Release(installer);
        dlResult->lpVtbl->Release(dlResult);
        downloader->lpVtbl->Release(downloader);
        updates->lpVtbl->Release(updates);
        searchResult->lpVtbl->Release(searchResult);
        searcher->lpVtbl->Release(searcher);
        session->lpVtbl->Release(session);
        return;
    }
    rc = disp_get_long(instResult, L"ResultCode", -1);
    snprintf(line, sizeof(line), "Install result: %s (%ld)", wua_code_str(rc), rc);
    write_log(line);
    if (disp_get_bool(instResult, L"RebootRequired", 0)) {
        write_log("Reboot is required to complete updates");
    } else {
        write_log("No reboot required");
    }
    instResult->lpVtbl->Release(instResult);
    installer->lpVtbl->Release(installer);
    dlResult->lpVtbl->Release(dlResult);
    downloader->lpVtbl->Release(downloader);
    updates->lpVtbl->Release(updates);
    searchResult->lpVtbl->Release(searchResult);
    searcher->lpVtbl->Release(searcher);
    session->lpVtbl->Release(session);
    write_log("Windows Update phase done");
}

int parse_url(const char *url, char *host, int hostsz, char *path, int pathsz, int *secure, int *port) {
    const char *p;
    const char *h;
    const char *c;
    size_t hl;
    *secure = 0;
    *port = 80;
    if (strncmp(url, "https://", 8) == 0) {
        h = url + 8;
        *secure = 1;
        *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        h = url + 7;
    } else {
        return 0;
    }
    p = strchr(h, '/');
    if (!p) return 0;
    c = strchr(h, ':');
    if (c && c < p) {
        hl = (size_t)(c - h);
        *port = atoi(c + 1);
    } else {
        hl = (size_t)(p - h);
    }
    if (hl + 1 > (size_t)hostsz) return 0;
    memcpy(host, h, hl);
    host[hl] = 0;
    snprintf(path, pathsz, "%s", p);
    return 1;
}

int send_log_to_server(const char *filepath) {
    char host[256];
    char path[1024];
    wchar_t whost[256];
    wchar_t wpath[1024];
    HINTERNET hsess = NULL;
    HINTERNET hconn = NULL;
    HINTERNET hreq = NULL;
    FILE *f = NULL;
    long fsize = 0;
    unsigned char *fbuf = NULL;
    DWORD status = 0;
    DWORD slen = sizeof(status);
    DWORD flags = 0;
    int secure = 0;
    int port = 80;
    char line[1024];
    int ok = 0;
    if (strstr(PREP_LOG_URL, "<SERVER>") != NULL) {
        write_log("Prep log server not configured, skipping upload");
        return 0;
    }
    if (!parse_url(PREP_LOG_URL, host, sizeof(host), path, sizeof(path), &secure, &port)) {
        write_log("Invalid server URL format");
        return 0;
    }
    f = fopen(filepath, "rb");
    if (!f) {
        snprintf(line, sizeof(line), "Upload skip, cannot open %s", filepath);
        write_log(line);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        write_log("Upload skip, file empty");
        return 0;
    }
    fbuf = (unsigned char*)malloc((size_t)fsize);
    if (!fbuf) { fclose(f); return 0; }
    if (fread(fbuf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(fbuf);
        fclose(f);
        return 0;
    }
    fclose(f);
    f = NULL;
    MultiByteToWideChar(CP_ACP, 0, host, -1, whost, 256);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 1024);
    if (secure) flags = WINHTTP_FLAG_SECURE;
    hsess = WinHttpOpen(L"LaptopPrep/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hsess) {
        free(fbuf);
        write_log("WinHttpOpen failed");
        return 0;
    }
    hconn = WinHttpConnect(hsess, whost, (INTERNET_PORT)port, 0);
    if (!hconn) {
        WinHttpCloseHandle(hsess);
        free(fbuf);
        write_log("WinHttpConnect failed");
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
    if (!WinHttpSendRequest(hreq, L"Content-Type: text/plain", (DWORD)-1,
        fbuf, (DWORD)fsize, (DWORD)fsize, 0)) {
        snprintf(line, sizeof(line), "WinHttpSendRequest failed %lu", (unsigned long)GetLastError());
        write_log(line);
    } else {
        if (WinHttpReceiveResponse(hreq, NULL)) {
            if (WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                NULL, &status, &slen, NULL)) {
                snprintf(line, sizeof(line), "Server upload status %lu", (unsigned long)status);
                write_log(line);
                if (status >= 200 && status < 300) ok = 1;
            }
        } else {
            snprintf(line, sizeof(line), "WinHttpReceiveResponse failed %lu", (unsigned long)GetLastError());
            write_log(line);
        }
    }
    if (hreq) WinHttpCloseHandle(hreq);
    if (hconn) WinHttpCloseHandle(hconn);
    if (hsess) WinHttpCloseHandle(hsess);
    free(fbuf);
    return ok;
}

int main() {
    IWbemServices *svc = NULL;
    IWbemLocator *loc = NULL;
    char line[1024];
    if (!is_admin()) {
        printf("Requesting administrator privileges...\n");
        relaunch_admin();
        return 0;
    }
    ensure_logdir();
    clean_old_logs();
    printf("============================================\n");
    printf("Starting laptop prep process...\n");
    printf("This may take a while depending on updates.\n");
    printf("============================================\n");
    snprintf(line, sizeof(line), "=== Starting laptop prep on %s ===", g_computer);
    write_log(line);
    if (!wmi_init(&svc, &loc)) {
        write_log("WMI init failed, aborting hardware checks");
        return 1;
    }
    collect_specs(svc);
    collect_device_id(svc);
    collect_motherboard(svc);
    collect_extra(svc);
    collect_tpm(loc);
    run_battery_report();
    do_windows_update();
    write_log("=== Prep phase complete ===");
    send_log_to_server(g_logPath);
    svc->lpVtbl->Release(svc);
    loc->lpVtbl->Release(loc);
    CoUninitialize();
    printf("============================================\n");
    printf("Script finished. Log posted to server.\n");
    printf("============================================\n");
    printf("Press Enter to exit...\n");
    getchar();
    return 0;
}
