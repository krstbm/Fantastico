/* =====================================================================
 *  build_probe.c - "can this compiler build LaptopPrep at all?"
 *
 *  build.bat compiles this tiny file before it touches the real sources.
 *  It includes every Windows SDK header the project uses and takes the
 *  address of one symbol from each import library it links against, so a
 *  toolchain that cannot build the project fails here - with one clear
 *  message - instead of dying halfway through src\prep_core.c.
 *
 *  It exists because there is a whole class of compiler that looks fine
 *  and is not: the old MinGW.org toolchain (gcc target "mingw32", last
 *  release 2018) and Cygwin's gcc have neither the WMI / WinHTTP / GDI+
 *  headers nor their import libraries.  Both would otherwise fail with
 *  "fatal error: wbemidl.h: No such file or directory", which reads like
 *  a mistake in this project rather than a toolchain problem.
 *
 *  Build it by hand to check a compiler:
 *      gcc tools\build_probe.c -o probe.exe -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00
 *          -lole32 -loleaut32 -lwbemuuid -lwinhttp -lws2_32 -liphlpapi
 *          -lwtsapi32 -ladvapi32 -lshell32 -lcomctl32 -lgdiplus -lgdi32
 *          -luser32 -lpsapi
 *  Exit code 0 means this gcc can build LaptopPrep.
 * ===================================================================== */

#include <winsock2.h>
#include <windows.h>
#include <wbemidl.h>
#include <winhttp.h>
#include <iptypes.h>
#include <iphlpapi.h>
#include <wtsapi32.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <psapi.h>

int main(void)
{
    /* one symbol from each library the real link line needs */
    void *p[9];
    p[0] = (void *)&CLSID_WbemLocator;         /* wbemuuid - the WMI locator   */
    p[1] = (void *)&WinHttpOpen;               /* winhttp  - reachability/upload*/
    p[2] = (void *)&GdiplusStartup;            /* gdiplus  - every shape drawn  */
    p[3] = (void *)&GetProcessMemoryInfo;      /* psapi    - the UI telemetry   */
    p[4] = (void *)&GetAdaptersAddresses;      /* iphlpapi - the MAC addresses  */
    p[5] = (void *)&WTSQuerySessionInformationA; /* wtsapi32 - session check    */
    p[6] = (void *)&RegOpenKeyExA;             /* advapi32 - registry reads    */
    p[7] = (void *)&InitCommonControlsEx;      /* comctl32 - the two controls   */
    p[8] = (void *)&ShellExecuteA;             /* shell32  - opening pages      */

    return (p[0] && p[1] && p[2] && p[3] && p[4] && p[5] && p[6] && p[7] && p[8])
           ? 0 : 1;
}
