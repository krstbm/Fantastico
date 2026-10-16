/* =====================================================================
 *  LaptopPrep - GUI (prep_gui.c)  -  version 1.2
 *
 *  A Win32 dark-themed console for the refurbishment engine:
 *    - side menu: Home / Live log / Settings / About
 *    - Home holds the run options (one module checkbox per engine module)
 *    - the run screen shows the progress ring, the live step, the eight
 *      Windows Update steps with a countdown, and the verdict with the
 *      action that belongs to it
 *    - the whole run happens on a worker thread, all child processes are
 *      hidden, nothing waits for a keypress
 *    - the log tail is streamed into the Live log page while it runs
 *
 *  DRAWING RULES (this is what stops the "I can count the pixels" look):
 *    1. every rounded shape, arc, circle and border goes through GDI+
 *       (gfx_* below) - never RoundRect/Ellipse in plain GDI, which has no
 *       anti-aliasing for geometry;
 *    2. every 1 px line is snapped to a whole pixel and stroked inset, so
 *       it is exactly one pixel wide instead of two grey ones;
 *    3. every string is drawn through dtext(), which clips to its box, so
 *       nothing can ever paint outside the card it was measured for;
 *    4. spacing comes from one scale (S()) and from named gaps, not from
 *       numbers picked per painter.
 * ===================================================================== */
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdiplus.h>
#include <psapi.h>
#include "prep.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "wtsapi32.lib")

/* ------------------------------------------------------------------ *
 *  theme
 *  The values are the design tokens of the website: dark theme =
 *  html[data-theme='dark'], light theme = :root, brand ramp #ff8a2a on
 *  dark / #e4570a on light, and the semantic colours for step states.
 * ------------------------------------------------------------------ */
typedef struct {
    COLORREF bg, side, sideSel, surface, surfaceHi, line, lineHi;
    COLORREF text, text2, muted, track;
    COLORREF brand, brandHi, brandPress, brandSoft, brandLine, brandBright, onBrand;
    COLORREF good, goodSoft, warn, warnSoft, warnLine, bad, badSoft, info, hold;
    COLORREF logbg, logText, logDim, shadow;
    int      dark;
} Theme;

static Theme P;

static const Theme THEME_DARK = {
    RGB(0x0b,0x0c,0x0e), RGB(0x0d,0x0f,0x12), RGB(0x2a,0x1b,0x0e),
    RGB(0x14,0x16,0x19), RGB(0x1c,0x1f,0x24), RGB(0x24,0x28,0x2d), RGB(0x34,0x3a,0x41),
    RGB(0xec,0xee,0xf0), RGB(0xaa,0xb1,0xb9), RGB(0x6f,0x76,0x7e), RGB(0x1a,0x1e,0x22),
    RGB(0xff,0x8a,0x2a), RGB(0xff,0xa2,0x54), RGB(0xd9,0x6c,0x12),
    RGB(0x2a,0x1b,0x0e), RGB(0x55,0x37,0x1a), RGB(0xff,0xb8,0x77), RGB(0x1a,0x11,0x06),
    RGB(0x4c,0xc3,0x8a), RGB(0x12,0x2b,0x1f), RGB(0xe0,0xa4,0x3c),
    RGB(0x2e,0x23,0x12), RGB(0x4e,0x3b,0x18), RGB(0xf0,0x65,0x5a), RGB(0x33,0x17,0x15),
    RGB(0x6a,0xae,0xf5), RGB(0x94,0x8b,0xf0),
    RGB(0x0d,0x0f,0x12), RGB(0xd6,0xda,0xde), RGB(0x7d,0x85,0x8d), RGB(0x00,0x00,0x00),
    1
};

static const Theme THEME_LIGHT = {
    RGB(0xf3,0xf3,0xf1), RGB(0xfa,0xfa,0xf8), RGB(0xff,0xf1,0xe6),
    RGB(0xff,0xff,0xff), RGB(0xf0,0xef,0xeb), RGB(0xe6,0xe4,0xdf), RGB(0xd3,0xd0,0xc8),
    RGB(0x15,0x17,0x1a), RGB(0x4e,0x54,0x5c), RGB(0x86,0x8d,0x96), RGB(0xe9,0xe7,0xe1),
    RGB(0xe4,0x57,0x0a), RGB(0xff,0x7a,0x29), RGB(0xb3,0x43,0x06),
    RGB(0xff,0xf1,0xe6), RGB(0xff,0xd3,0xae), RGB(0xff,0x9a,0x4d), RGB(0xff,0xff,0xff),
    RGB(0x15,0x80,0x3d), RGB(0xea,0xf6,0xee), RGB(0x9a,0x62,0x10),
    RGB(0xfb,0xf3,0xe2), RGB(0xeb,0xd4,0xa4), RGB(0xc0,0x36,0x2c), RGB(0xfc,0xec,0xea),
    RGB(0x16,0x63,0xc7), RGB(0x5b,0x4b,0xc4),
    RGB(0xff,0xff,0xff), RGB(0x2b,0x2f,0x34), RGB(0x8a,0x90,0x97), RGB(0x3a,0x3a,0x38),
    0
};

static int g_themeDark = 1;

static void theme_apply(void) { P = g_themeDark ? THEME_DARK : THEME_LIGHT; }

#define THEME_KEY  "Software\\Esprit ICT\\LaptopPrep"
#define THEME_VAL  "ThemeDark"

static void theme_store(int dark)
{
    HKEY k;
    DWORD v = (DWORD)(dark ? 1 : 0);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, THEME_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, THEME_VAL, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static void theme_load(void)
{
    HKEY k;
    DWORD v = 0, n = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, THEME_KEY, 0, KEY_QUERY_VALUE, &k)
            == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, THEME_VAL, 0, &type, (BYTE*)&v, &n) == ERROR_SUCCESS
            && type == REG_DWORD) {
            g_themeDark = (v != 0);
            RegCloseKey(k);
            return;
        }
        RegCloseKey(k);
    }
    g_themeDark = 1;                     /* fall back to the brand's dark */
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        v = 1; n = sizeof(v); type = 0;
        if (RegQueryValueExA(k, "AppsUseLightTheme", 0, &type, (BYTE*)&v, &n)
                == ERROR_SUCCESS && type == REG_DWORD)
            g_themeDark = (v == 0);
        RegCloseKey(k);
    }
}

/* ------------------------------------------------------------------ *
 *  fonts: Inter for text, Space Grotesk for headings, JetBrains Mono for
 *  the log.  Loaded privately from fonts\ next to the exe.
 * ------------------------------------------------------------------ */
static const char *F_HEAD  = "Space Grotesk LP";
static const char *F_BODY  = "Inter LP";
static const char *F_MONO  = "JetBrains Mono LP";

#define NOBORDER      0xFFFFFFFFu
#define NOFILL        0xFFFFFFFFu

/* ------------------------------------------------------------------ *
 *  views / timers / step states
 * ------------------------------------------------------------------ */
enum { VIEW_HOME = 0, VIEW_LOG, VIEW_SETTINGS, VIEW_ABOUT, VIEW_COUNT };
enum { TIMER_ANIM = 1 };

/* the engine reports a step as ok / warn / failed; the GUI adds "running"
 * and "queued" for itself */
enum { STEP_QUEUED = 0, STEP_RUNNING, STEP_OK, STEP_WARN, STEP_ERR, STEP_SKIP };

/* hit-test ids: 0..7 modules, 100+ nav, 200+ settings rows, 300+ log
 * toolbar, 400+ title bar buttons, 1000+ footer/action buttons */
#define WHAT_BTN_PRIMARY   1000
#define WHAT_BTN_SECONDARY 1001
#define WHAT_BTN_GHOST     1002
#define WHAT_BTN_WU_ACTION 1003
#define WHAT_BTN_RESTART      1004
#define WHAT_BTN_RESTART_LATE 1005
#define WHAT_LINK_WINTR       1006
#define WHAT_LINK_AGPCS       1007
#define WHAT_TITLE_MIN     400
#define WHAT_TITLE_MAX     401
#define WHAT_TITLE_CLOSE   402
#define WHAT_TITLE_THEME   403
#define WHAT_NOTICE_CLOSE  310
#define TB_BTNS            4

/* ------------------------------------------------------------------ *
 *  module table (ORDER MATTERS: index = step slot)
 * ------------------------------------------------------------------ */
typedef struct {
    unsigned    bit;
    const char *title;
    const char *desc;
    const char *note;         /* what it produces, shown on hover */
} ModuleRow;

static const ModuleRow g_mods[] = {
    { PREP_M_SPECS,     "System specs & disks", "CPU, RAM, Windows edition, disk drives, volumes",
      "Writes CPU, RAM, OS build and every fixed disk to the log" },
    { PREP_M_DEVICE_ID, "Device identity",      "Serials, UUID, MAC addresses + CSV export",
      "One CSV row per laptop, ready for the asset database" },
    { PREP_M_BOARD,     "Motherboard & BIOS",   "Board, BIOS revision, slots, buses",
      "Board and BIOS identity, plus the slot and bus inventory" },
    { PREP_M_EXTRA,     "Full hardware sweep",  "GPU, memory modules, audio, input, network, USB",
      "Memory banks, GPU, audio, keyboard, network, USB" },
    { PREP_M_TPM,       "TPM status",           "Activated / enabled / spec version",
      "Needed before BitLocker or a Windows 11 re-image" },
    { PREP_M_BATTERY,   "Battery report",       "powercfg HTML report into the log folder",
      "Falls back to a built-in report if powercfg is missing" },
    { PREP_M_UPDATES,   "Windows Update",       "Local cache first, then online - 8 visible steps",
      "Installs what is already downloaded before it searches" },
    { PREP_M_UPLOAD,    "Upload log to server", "POST the finished log to the log endpoint",
      "Optional: needs the endpoint in prep.h to be configured" },
};
#define NMOD ((int)(sizeof(g_mods) / sizeof(g_mods[0])))

static const char *const g_nav[] = { "Home", "Live log", "Settings", "About" };

/* ------------------------------------------------------------------ *
 *  state
 * ------------------------------------------------------------------ */
static HWND      g_hMain = NULL;
static int       g_view = VIEW_HOME;
static int       g_dpi = 96;

static int       g_runScreen = 0;
static unsigned  g_selected = PREP_DEFAULT_FLAGS | PREP_M_UPLOAD;
static volatile LONG g_step[NMOD];
static volatile LONG g_active = -1;
static int       g_finished = 0;
static int       g_anyErr = 0;
static int       g_anyWarn = 0;
static ULONGLONG g_tStart = 0, g_tEnd = 0;
static ULONGLONG g_lastLogChange = 0;
static int       g_saidWaiting = 0;
static unsigned  g_runFlags = 0;
static int       g_autoScroll = 1;
static int       g_minimizeOnRun = 0;

static volatile LONG g_runFinishedFlag = 0;
static int       g_runRc = 0;
static HANDLE    g_hWorker = NULL;
static char      g_status[220] = "Ready. Nothing has run yet in this session.";

int g_runningNow(void);

/* ------------------------------------------------------------------ *
 *  animation state - every value eases towards its target once per frame
 * ------------------------------------------------------------------ */
static float g_navHov[VIEW_COUNT];
static float g_modHov[NMOD];
/* the two links on the About page */
static float g_linkHov[2];
static float g_btnHov[6];
static float g_btnPress[6];
static float g_titleHov[TB_BTNS];
/* press flashes: the title-bar buttons and the Settings rows get the same
 * "the click landed" beat the big buttons have, which matters most for the
 * theme switch and for the rows that open the log folder or send the log -
 * on those the window does not change, so the flash is the only answer
 * until the work finishes */
static float g_titlePress[TB_BTNS];
static float g_rowPress[8];          /* the Settings rows, by their table index */
static float g_ringPct;
static float g_ringSpin;
static float g_ringPulse;
static float g_stepEase[NMOD];
static float g_stepFlash[NMOD];
/* ---- the restart offer ------------------------------------------- *
 *  When a run ends with Windows waiting for a restart to finish the
 *  updates, the interface offers that restart itself.  The offer comes
 *  after the log upload has already been tried, and it says plainly what
 *  happened to that upload - a failed upload never blocks the restart,
 *  it is reported.  g_restartDeadline is a GetTickCount64 deadline, so
 *  the countdown cannot drift and survives every repaint. */
static int        g_restartPending  = 0;   /* Windows wants a restart  */
static int        g_restartOffered  = 0;   /* the offer is on screen   */
static int        g_restartLater    = 0;   /* the operator chose Later */
static float      g_restartAnim     = 0.0f;
static ULONGLONG  g_restartDeadline = 0;   /* != 0 while counting down */
static int        g_restartByUs     = 0;   /* we called shutdown /r    */

static float g_runIn     = 1.0f;   /* 0 -> 1: the run screen rows arrive */
static float g_wuCheck[WU_STEP_COUNT];   /* the tick drawing itself      */
static float g_wuStepEase[WU_STEP_COUNT];
static float g_wuRowFlash[WU_STEP_COUNT];
static float g_viewAnim = 1.0f;
static float g_scrollAnim;
static float g_logFade = 1.0f;
static float g_frame;
static float g_modPop[NMOD];
static float g_navGlow;
static int   g_wuLastState[WU_STEP_COUNT];
static float g_logBtnHov[3];
static DWORD *g_lineBorn = NULL;
static int    g_lineBornCap = 0;

/* forward declarations */
static void paint_mark(HDC dc, int x, int y, int w, int h);
static void draw_logo(HDC dc, HBITMAP bmp, int srcW, int srcH, int x, int y, int h);
static void paint_brand(HDC dc, int x, int y, int h);
static int  mod_from_bit(unsigned bit);
static const char *step_state_text(int st);
static COLORREF step_state_color(int st);
static void start_run(void);
static void finish_run_ui(void);
static void log_copy_all(void);
static void log_pump_file(void);
static void open_logdir(void);
static void buf_release(void);
static void open_windows_update(void);
static int  run_update_recovery(void);
static void restart_after_upload(void);
static void restart_offer(void);
static void begin_restart(void);
static void cancel_restart(void);
static int  restart_seconds_left(void);
static void power_glyph(HDC dc, int cx, int cy, int rad, COLORREF col);
static void relaunch_elevated(void);
static int  active_bit_is_updates(void);
static DWORD WINAPI WorkerProc(LPVOID param);

/* ------------------------------------------------------------------ *
 *  small numeric helpers
 * ------------------------------------------------------------------ */
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static float flerp(float cur, float target, float speed)
{
    float d = target - cur;
    if (d > -0.0005f && d < 0.0005f) return target;
    return cur + d * speed;
}

static float ease_out(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static COLORREF blend(COLORREF a, COLORREF b, float t)
{
    int r, g, bl;
    if (t <= 0) return a;
    if (t >= 1) return b;
    r  = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    g  = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* click state - used to ignore the second half of a double click */
static int   g_lastClickWhat = -1;
static DWORD g_lastClickTick = 0;

static int  g_hoverNav = -1;
static int  g_hoverModule = -1;
static int  g_hoverBtn = -1;
static int  g_hoverTitle = -1;

/* the closable warning bar (see paint_notice) */
static int   g_noticeOff  = 0;
static float g_noticeFade = 1.0f;

/* the in-place editor for the upload address (Settings) */
static int  g_editRow   = -1;
static char g_editBuf[512];
static int  g_editCaret = 0;

#define TITLE_H     36
#define TB_BTN_W    46

static HBITMAP g_bmpLogo = NULL;
static HBITMAP g_bmpLogoBig = NULL;
static int     g_logoW = 0, g_logoH = 0;
static int     g_logoBigW = 0, g_logoBigH = 0;
static int     g_logoLoaded = 0;

/* fonts */
static HFONT g_fLogo, g_fH1, g_fTitle, g_fBody, g_fSmall, g_fTiny, g_fBig, g_fMono;
static HFONT g_fBrand, g_fBrandI, g_fStep, g_fNum;

static HBRUSH g_brCard = NULL, g_brCardHi = NULL, g_brSideSel = NULL, g_brLog = NULL;

/* live log: held in memory with a line index and painted by us */
static char             *g_logBuf = NULL;
static size_t            g_logLen = 0, g_logCap = 0, g_logTail = 0;
static size_t           *g_lines = NULL;
static int               g_nLines = 0, g_linesCap = 0;
static LONGLONG          g_logPos = 0;
static int               g_logFollow = 1;
static int               g_logScroll = 0;
static int               g_logMaxScroll = 0;

/* ------------------------------------------------------------------ *
 *  font loading
 * ------------------------------------------------------------------ */
#define MAX_LOADED_FONTS 12
static char  g_fontFiles[MAX_LOADED_FONTS][MAX_PATH];
static int   g_fontCount = 0;

static void exe_dir(char *out, size_t n);
static void theme_brushes(void);

static void fonts_load(void)
{
    char dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int tries;
    const char *subs[2];
    subs[0] = "fonts";
    subs[1] = "assets\\fonts";

    for (tries = 0; tries < 2; tries++) {
        exe_dir(dir, sizeof(dir));
        snprintf(pat, sizeof(pat), "%s\\%s\\*.ttf", dir, subs[tries]);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char full[MAX_PATH];
            if (g_fontCount >= MAX_LOADED_FONTS) break;
            snprintf(full, sizeof(full), "%s\\%s\\%s", dir, subs[tries], fd.cFileName);
            if (AddFontResourceExA(full, FR_PRIVATE | FR_NOT_ENUM, 0)) {
                snprintf(g_fontFiles[g_fontCount], MAX_PATH, "%s", full);
                g_fontCount++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void fonts_unload(void)
{
    int i;
    for (i = 0; i < g_fontCount; i++)
        RemoveFontResourceExA(g_fontFiles[i], FR_PRIVATE | FR_NOT_ENUM, 0);
    g_fontCount = 0;
}

static int CALLBACK enum_font_cb(const LOGFONTA *lf, const TEXTMETRICA *tm,
                                 DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(int*)lp = 1;
    return 0;
}

static int font_available(const char *face)
{
    LOGFONTA lf;
    HDC dc;
    int found = 0;
    memset(&lf, 0, sizeof(lf));
    snprintf(lf.lfFaceName, LF_FACESIZE, "%s", face);
    lf.lfCharSet = DEFAULT_CHARSET;
    dc = GetDC(NULL);
    EnumFontFamiliesExA(dc, &lf, enum_font_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static const char *font_pick(const char *want, const char *fallback)
{
    if (font_available(want)) return want;
    return fallback;
}

static HFONT mkfont(int px, int weight, const char *face, int fixed)
{
    return CreateFontA(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       fixed ? FIXED_PITCH : DEFAULT_PITCH, face);
}

static void make_fonts(void)
{
    HFONT *f[12];
    int i, n = 0;
    const char *head, *body, *mono;

    f[n++] = &g_fLogo;  f[n++] = &g_fH1;   f[n++] = &g_fTitle; f[n++] = &g_fBody;
    f[n++] = &g_fSmall; f[n++] = &g_fTiny; f[n++] = &g_fBig;   f[n++] = &g_fMono;
    f[n++] = &g_fBrand; f[n++] = &g_fBrandI;
    f[n++] = &g_fStep;  f[n++] = &g_fNum;
    for (i = 0; i < n; i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }

    fonts_load();                                  /* private, before picking */
    head = font_pick(F_HEAD, font_pick("Segoe UI Variable Display", "Segoe UI"));
    body = font_pick(F_BODY, font_pick("Segoe UI Variable Text", "Segoe UI"));
    mono = font_pick(F_MONO, font_pick("Cascadia Mono", "Consolas"));

    /* the site's type scale, in device pixels ----------------------- */
    g_fH1     = mkfont(27, FW_BOLD,     head, 0);   /* page heading      */
    g_fTitle  = mkfont(14, FW_SEMIBOLD, head, 0);   /* card heading      */
    g_fLogo   = mkfont(20, FW_BOLD,     head, 0);   /* wordmark          */
    g_fStep   = mkfont(14, FW_SEMIBOLD, body, 0);   /* step / row label  */
    g_fBody   = mkfont(13, FW_NORMAL,   body, 0);   /* body              */
    g_fSmall  = mkfont(12, FW_NORMAL,   body, 0);   /* secondary         */
    g_fTiny   = mkfont(11, FW_NORMAL,   body, 0);   /* meta / chips      */
    g_fBig    = mkfont(34, FW_BOLD,     head, 0);   /* the big number    */
    g_fNum    = mkfont(19, FW_BOLD,     head, 0);   /* ring percentage   */
    g_fMono   = mkfont(12, FW_NORMAL,   mono, 1);   /* the log           */
    g_fBrand  = mkfont(25, FW_BOLD,     body, 0);
    g_fBrandI = mkfont(25, FW_NORMAL,   body, 0);
}

static void free_fonts(void)
{
    HFONT *f[12];
    int i, n = 0;
    f[n++] = &g_fLogo;  f[n++] = &g_fH1;   f[n++] = &g_fTitle; f[n++] = &g_fBody;
    f[n++] = &g_fSmall; f[n++] = &g_fTiny; f[n++] = &g_fBig;   f[n++] = &g_fMono;
    f[n++] = &g_fBrand; f[n++] = &g_fBrandI;
    f[n++] = &g_fStep;  f[n++] = &g_fNum;
    for (i = 0; i < n; i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
    fonts_unload();
}

/* ------------------------------------------------------------------ *
 *  plain GDI primitives (text and fills only - no geometry)
 * ------------------------------------------------------------------ */
static void rect_fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void dtext(HDC dc, const char *s, int x, int y, int w, int h,
                  COLORREF col, HFONT f, UINT fmt);
static void stroke_line(HDC dc, int x0, int y0, int x1, int y1,
                        COLORREF col, int thick);
static void dtext_center(HDC dc, const char *s, int x, int y, int w, int h,
                         COLORREF col, HFONT f);
static void dtext_right(HDC dc, const char *s, int x, int y, int w, int h,
                        COLORREF col, HFONT f);

/* ------------------------------------------------------------------ *
 *  GDI+ shape layer - every anti-aliased shape in the program
 *
 *  One rule that is not obvious, measured rather than assumed: nothing on
 *  an animated path may be drawn with an alpha below 255.  A GDI+ fill or
 *  pen with alpha makes the renderer build a scratch 32-bit surface for
 *  the operation - on this window roughly 5 MB - and a few of those per
 *  frame at 30 fps made one run screen eat gigabytes of working set.  It
 *  is documented GDI+ behaviour (translucent drawing composites against a
 *  backing surface); wine's implementation allocates one per operation.
 *
 *  So translucency is done the cheap way instead: gfx_mix() blends the
 *  colour with whatever it is drawn on and paints the result opaque.  To
 *  the eye it is the same picture; to the allocator it is nothing at all.
 * ------------------------------------------------------------------ */
static ULONG_PTR g_gpToken = 0;
static int       g_gfxOk = 0;

static void gfx_start(void)
{
    GdiplusStartupInput in;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    g_gfxOk = (GdiplusStartup(&g_gpToken, &in, NULL) == Ok);
}

static void gfx_stop(void)
{
    if (g_gfxOk) { GdiplusShutdown(g_gpToken); g_gpToken = 0; g_gfxOk = 0; }
}

/* an opaque colour that looks like `col` at `a`/255 drawn over `under` */
static COLORREF gfx_mix(COLORREF under, COLORREF col, int a)
{
    if (a >= 255) return col;
    if (a <= 0)   return under;
    return blend(under, col, (float)a / 255.0f);
}

static ARGB gfx_argb(COLORREF c, int a)
{
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return ((ARGB)a << 24) | ((ARGB)GetRValue(c) << 16) |
           ((ARGB)GetGValue(c) << 8) | (ARGB)GetBValue(c);
}

static GpGraphics *gfx_begin(HDC dc)
{
    GpGraphics *g = NULL;
    if (!g_gfxOk) return NULL;
    if (GdipCreateFromHDC(dc, &g) != Ok) return NULL;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    /* deliberately no GdipSetTextRenderingHint here: all text is drawn with
     * GDI (which does its own ClearType), and asking wine's GDI+ for a
     * ClearType glyph cache on every short-lived context cost hundreds of
     * megabytes per frame - measured, not guessed */
    return g;
}

static void gfx_end(GpGraphics *g)
{
    if (g) GdipDeleteGraphics(g);
}

static GpPath *gfx_round_path(int x, int y, int w, int h, int rad)
{
    GpPath *p = NULL;
    int d;
    if (w <= 0 || h <= 0) return NULL;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    if (rad < 1) rad = 0;
    d = rad * 2;
    if (GdipCreatePath(FillModeAlternate, &p) != Ok) return NULL;
    if (rad == 0) {
        GdipAddPathRectangleI(p, x, y, w, h);
        return p;
    }
    GdipAddPathArcI(p, x, y, d, d, 180.0f, 90.0f);
    GdipAddPathArcI(p, x + w - d, y, d, d, 270.0f, 90.0f);
    GdipAddPathArcI(p, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    GdipAddPathArcI(p, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(p);
    return p;
}

static void gfx_fill_path(GpGraphics *g, GpPath *p, COLORREF col, int alpha)
{
    GpSolidFill *b = NULL;
    if (GdipCreateSolidFill(gfx_argb(col, alpha), &b) == Ok) {
        GdipFillPath(g, (GpBrush *)b, p);
        GdipDeleteBrush((GpBrush *)b);
    }
}

static void gfx_stroke_path(GpGraphics *g, GpPath *p, COLORREF col, int alpha, REAL width)
{
    GpPen *pen = NULL;
    if (GdipCreatePen1(gfx_argb(col, alpha), width, UnitPixel, &pen) == Ok) {
        GdipSetPenMode(pen, PenAlignmentInset);   /* 1 px means 1 px */
        GdipDrawPath(g, pen, p);
        GdipDeletePen(pen);
    }
}

static void gfx_arc_stroke(GpGraphics *g, int cx, int cy, int rad,
                           float a0, float a1, COLORREF col, int alpha,
                           REAL width, int round_caps)
{
    GpPath *p = NULL;
    GpPen *pen = NULL;
    float sweep = a1 - a0;
    if (sweep < 0.1f) sweep = 0.1f;
    if (GdipCreatePath(FillModeAlternate, &p) != Ok) return;
    if (sweep >= 359.5f) GdipAddPathEllipseI(p, cx - rad, cy - rad, rad * 2, rad * 2);
    else                 GdipAddPathArcI(p, cx - rad, cy - rad, rad * 2, rad * 2, a0, sweep);
    if (GdipCreatePen1(gfx_argb(col, alpha), width, UnitPixel, &pen) == Ok) {
        if (round_caps) {
            GdipSetPenStartCap(pen, LineCapRound);
            GdipSetPenEndCap(pen, LineCapRound);
        }
        GdipDrawPath(g, pen, p);
        GdipDeletePen(pen);
    }
    GdipDeletePath(p);
}

static void gfx_poly_fill(HDC dc, const POINT *pts, int n, COLORREF col)
{
    GpGraphics *g = gfx_begin(dc);
    GpPath *p = NULL;
    if (g && n >= 2 && GdipCreatePath(FillModeAlternate, &p) == Ok) {
        int i;
        GdipAddPathLineI(p, pts[0].x, pts[0].y, pts[1].x, pts[1].y);
        for (i = 1; i < n; i++) {
            int j = (i + 1) % n;
            GdipAddPathLineI(p, pts[i].x, pts[i].y, pts[j].x, pts[j].y);
        }
        GdipClosePathFigure(p);
        gfx_fill_path(g, p, col, 255);
        GdipDeletePath(p);
    } else if (!g) {
        HBRUSH br = CreateSolidBrush(col);
        HPEN pn = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, pn);
        Polygon(dc, pts, n);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pn);
    }
    gfx_end(g);
}

static void gfx_round_stroke(HDC dc, int x, int y, int w, int h, int rad,
                             COLORREF col, int alpha, REAL width)
{
    GpGraphics *g = gfx_begin(dc);
    GpPath *p;
    if (!g) return;
    p = gfx_round_path(x, y, w, h, rad);
    if (p) { gfx_stroke_path(g, p, col, alpha, width); GdipDeletePath(p); }
    gfx_end(g);
}

/* .card : 1 px border plus the site's two-layer shadow ----------------- */
static void shadow_card(HDC dc, int x, int y, int w, int h, int rad)
{
    GpGraphics *g = gfx_begin(dc);
    int i;
    if (!g) return;
    for (i = 5; i >= 1; i--) {
        GpPath *p = gfx_round_path(x - i + 1, y + i, w + i * 2, h + i, rad + i);
        int alpha = (int)(5.0f * (float)(6 - i) * 3.2f);
        if (p) { gfx_fill_path(g, p, gfx_mix(P.bg, P.shadow, alpha), 255);
                 GdipDeletePath(p); }
    }
    {
        GpPath *p = gfx_round_path(x, y + 2, w, h, rad);
        if (p) { gfx_fill_path(g, p, gfx_mix(P.bg, P.shadow, 40), 255);
                 GdipDeletePath(p); }
    }
    gfx_end(g);
}

static void card(HDC dc, int x, int y, int w, int h, COLORREF fill,
                 COLORREF border, int rad)
{
    GpGraphics *g = gfx_begin(dc);
    GpPath *p;
    if (!g) {
        /* fallback: plain GDI rounded rectangle (no anti-aliasing, but the
         * program still runs if GDI+ cannot start) */
        HBRUSH b = (fill == NOFILL) ? (HBRUSH)GetStockObject(NULL_BRUSH)
                                    : CreateSolidBrush(fill);
        HPEN   pn = (border == NOBORDER) ? (HPEN)GetStockObject(NULL_PEN)
                                         : CreatePen(PS_SOLID, 1, border);
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, pn);
        RoundRect(dc, x, y, x + w, y + h, rad, rad);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        if (fill   != NOFILL)   DeleteObject(b);
        if (border != NOBORDER) DeleteObject(pn);
        return;
    }
    p = gfx_round_path(x, y, w, h, rad);
    if (p) {
        if (fill != NOFILL)     gfx_fill_path(g, p, fill, 255);
        if (border != NOBORDER) gfx_stroke_path(g, p, border, 255, 1.0f);
        GdipDeletePath(p);
    }
    gfx_end(g);
}

/* vertical gradient clipped to a rounded rectangle - .btn-primary */
static void round_vgrad(HDC dc, int x, int y, int w, int h, int rad,
                        COLORREF top, COLORREF bot, COLORREF border)
{
    GpGraphics *g = gfx_begin(dc);
    GpPath *p;
    if (!g) { card(dc, x, y, w, h, top, border, rad); return; }
    p = gfx_round_path(x, y, w, h, rad);
    if (p) {
        GpPoint a, b;
        GpLineGradient *lg = NULL;
        a.X = x; a.Y = y;
        b.X = x; b.Y = y + h;
        if (GdipCreateLineBrushI(&a, &b, gfx_argb(top, 255), gfx_argb(bot, 255),
                                 WrapModeTile, &lg) == Ok) {
            GdipFillPath(g, (GpBrush *)lg, p);
            GdipDeleteBrush((GpBrush *)lg);
        } else {
            gfx_fill_path(g, p, top, 255);
        }
        if (border != NOBORDER) gfx_stroke_path(g, p, border, 255, 1.0f);
        GdipDeletePath(p);
    }
    gfx_end(g);
}


static void rrect(HDC dc, int x, int y, int w, int h, int rad,
                  COLORREF fill, COLORREF border)
{
    card(dc, x, y, w, h, fill, border, rad);
}

static void circle_fill(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border)
{
    GpGraphics *g = gfx_begin(dc);
    GpPath *p = NULL;
    if (g) {
        if (GdipCreatePath(FillModeAlternate, &p) == Ok) {
            GdipAddPathEllipseI(p, cx - r, cy - r, r * 2, r * 2);
            if (fill != NOFILL)     gfx_fill_path(g, p, fill, 255);
            if (border != NOBORDER && r > 0) gfx_stroke_path(g, p, border, 255, 1.0f);
            GdipDeletePath(p);
        }
        gfx_end(g);
        return;
    }
    {
        HBRUSH b = (fill == NOFILL) ? (HBRUSH)GetStockObject(NULL_BRUSH)
                                    : CreateSolidBrush(fill);
        HPEN   pn = (border == NOBORDER) ? (HPEN)GetStockObject(NULL_PEN)
                                         : CreatePen(PS_SOLID, 1, border);
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, pn);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        if (fill   != NOFILL)   DeleteObject(b);
        if (border != NOBORDER) DeleteObject(pn);
    }
}

/* a check mark drawn as a stroked path, so it is anti-aliased too */
static void check_mark(HDC dc, int cx, int cy, int size, COLORREF col, int thick)
{
    GpGraphics *g = gfx_begin(dc);
    GpPath *p = NULL;
    GpPen *pen = NULL;
    if (!g) {
        /* fallback: the original polyline */
        HGDIOBJ op, ob;
        HPEN pn = CreatePen(PS_SOLID, thick, col);
        op = SelectObject(dc, pn);
        ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        MoveToEx(dc, cx - size, cy + size / 6, NULL);
        LineTo(dc,   cx - size / 3, cy + size);
        LineTo(dc,   cx + size, cy - size);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pn);
        return;
    }
    if (GdipCreatePath(FillModeAlternate, &p) == Ok) {
        GdipAddPathLineI(p, cx - size, cy + size / 6, cx - size / 3, cy + size);
        GdipAddPathLineI(p, cx - size / 3, cy + size, cx + size, cy - size);
        if (GdipCreatePen1(gfx_argb(col, 255), (REAL)thick, UnitPixel, &pen) == Ok) {
            GdipSetPenStartCap(pen, LineCapRound);
            GdipSetPenEndCap(pen, LineCapRound);
            GdipSetPenLineJoin(pen, LineJoinRound);
            GdipDrawPath(g, pen, p);
            GdipDeletePen(pen);
        }
        GdipDeletePath(p);
    }
    gfx_end(g);
}

/* the same tick, but drawn only as far as `t` (0..1) - used when a step
 * completes, so the tick visibly draws itself instead of popping in */
static void check_mark_progress(HDC dc, int cx, int cy, int size, COLORREF col,
                                int thick, float t)
{
    float x0 = (float)(cx - size), y0 = (float)(cy + size / 6);
    float x1 = (float)(cx - size / 3), y1 = (float)(cy + size);
    float x2 = (float)(cx + size), y2 = (float)(cy - size);
    float l1, l2, d;
    if (t <= 0.0f) return;
    if (t >= 0.999f) { check_mark(dc, cx, cy, size, col, thick); return; }
    l1 = (float)sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    l2 = (float)sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    d = t * (l1 + l2);
    if (d <= l1) {
        float f = (l1 > 0.0f) ? d / l1 : 1.0f;
        stroke_line(dc, (int)x0, (int)y0, (int)(x0 + (x1 - x0) * f),
                    (int)(y0 + (y1 - y0) * f), col, thick);
    } else {
        float f = (l2 > 0.0f) ? (d - l1) / l2 : 1.0f;
        stroke_line(dc, (int)x0, (int)y0, (int)x1, (int)y1, col, thick);
        stroke_line(dc, (int)x1, (int)y1, (int)(x1 + (x2 - x1) * f),
                    (int)(y1 + (y2 - y1) * f), col, thick);
    }
}

/* the restart symbol: a ring with a gap at the top and a stem through it */
static void power_glyph(HDC dc, int cx, int cy, int rad, COLORREF col)
{
    GpGraphics *g = gfx_begin(dc);
    if (!g) {
        circle_fill(dc, cx, cy, rad, NOFILL, col);
        stroke_line(dc, cx, cy - rad - S(2), cx, cy, col, S(2) > 0 ? S(2) : 1);
        return;
    }
    gfx_arc_stroke(g, cx, cy, rad, -60.0f, 240.0f, col, 255,
                   (REAL)(S(2) > 0 ? S(2) : 1), 1);
    {
        GpPen *pen = NULL;
        if (GdipCreatePen1(gfx_argb(col, 255), (REAL)(S(2) > 0 ? S(2) : 1),
                           UnitPixel, &pen) == Ok) {
            GdipSetPenStartCap(pen, LineCapRound);
            GdipSetPenEndCap(pen, LineCapRound);
            GdipDrawLine(g, pen, (REAL)cx, (REAL)(cy - rad - S(3)),
                         (REAL)cx, (REAL)(cy - S(2)));
            GdipDeletePen(pen);
        }
    }
    gfx_end(g);
}

/* a stroked single line / cross / chevron, anti-aliased */
static void stroke_line(HDC dc, int x0, int y0, int x1, int y1, COLORREF col, int thick)
{
    GpGraphics *g = gfx_begin(dc);
    GpPen *pen = NULL;
    if (!g) {
        HGDIOBJ op = SelectObject(dc, CreatePen(PS_SOLID, thick, col));
        MoveToEx(dc, x0, y0, NULL);
        LineTo(dc, x1, y1);
        DeleteObject(SelectObject(dc, op));
        gfx_end(g);
        return;
    }
    if (GdipCreatePen1(gfx_argb(col, 255), (REAL)thick, UnitPixel, &pen) == Ok) {
        GdipSetPenStartCap(pen, LineCapRound);
        GdipSetPenEndCap(pen, LineCapRound);
        GdipDrawLine(g, pen, x0, y0, x1, y1);
        GdipDeletePen(pen);
    }
    gfx_end(g);
}

/* ------------------------------------------------------------------ *
 *  text - always clipped to the box it was given
 * ------------------------------------------------------------------ */
/* ---- text, drawn by GDI+ -----------------------------------------
 *  Every string goes through GdipDrawString rather than DrawText: GDI
 *  snaps stems to the pixel grid and the stair-stepping is visible on a
 *  dark theme at these sizes, while GDI+ anti-aliases against the whole
 *  surface.  Measured on a full run screen (about two hundred strings a
 *  frame at 30 fps): 973 vs 943 CPU ticks per ten seconds - 3 % - so the
 *  smoother rasteriser is the default.  LP_GDI_TEXT=1 falls back to the
 *  old GDI path (useful on a machine with an odd font situation). */
static GpFont *gpfont_for(HFONT f)
{
    static struct { HFONT hf; GpFont *gf; } cache[24];
    static int n = 0;
    LOGFONTA lf;
    WCHAR fam[LF_FACESIZE];
    GpFontFamily *ff = NULL;
    GpFont *gf = NULL;
    int i, style = 0;

    for (i = 0; i < n; i++)
        if (cache[i].hf == f) return cache[i].gf;

    memset(&lf, 0, sizeof(lf));
    if (!f || GetObjectA(f, sizeof(lf), &lf) != sizeof(lf)) return NULL;
    MultiByteToWideChar(CP_ACP, 0, lf.lfFaceName, -1, fam, LF_FACESIZE);
    if (lf.lfWeight >= FW_BOLD) style |= FontStyleBold;
    if (lf.lfItalic)            style |= FontStyleItalic;
    if (GdipCreateFontFamilyFromName(fam, NULL, &ff) != Ok || !ff) return NULL;
    if (GdipCreateFont(ff, (REAL)(lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight),
                       style, UnitPixel, &gf) != Ok) gf = NULL;
    GdipDeleteFontFamily(ff);
    if (gf && n < 24) { cache[n].hf = f; cache[n].gf = gf; n++; }
    return gf;
}

static int dtext_gp(HDC dc, const char *s, int x, int y, int w, int h,
                    COLORREF col, HFONT f, UINT fmt)
{
    GpGraphics *g;
    GpFont *gf;
    GpStringFormat *sf = NULL;
    GpSolidFill *br = NULL;
    WCHAR *ws;
    int len;
    RectF rf;

    gf = gpfont_for(f);
    if (!gf) return 0;

    len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return 0;
    ws = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)len);
    if (!ws) return 0;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, len);

    g = gfx_begin(dc);
    if (!g) { free(ws); return 0; }

    if (GdipCreateStringFormat(0, LANG_NEUTRAL, &sf) != Ok) sf = NULL;
    if (sf) {
        INT flags = StringFormatFlagsNoClip;
        if (fmt & DT_SINGLELINE) flags |= StringFormatFlagsNoWrap;
        GdipSetStringFormatFlags(sf, flags);
        GdipSetStringFormatTrimming(sf, StringTrimmingEllipsisCharacter);
        if (fmt & DT_CENTER)     GdipSetStringFormatAlign(sf, StringAlignmentCenter);
        else if (fmt & DT_RIGHT) GdipSetStringFormatAlign(sf, StringAlignmentFar);
        if (fmt & DT_VCENTER)    GdipSetStringFormatLineAlign(sf, StringAlignmentCenter);
    }
    if (GdipCreateSolidFill(gfx_argb(col, 255), &br) != Ok) br = NULL;

    rf.X = (REAL)x; rf.Y = (REAL)y;
    rf.Width = (REAL)w; rf.Height = (REAL)h;
    if (br) GdipDrawString(g, ws, -1, gf, &rf, sf, (GpBrush *)br);

    if (br) GdipDeleteBrush((GpBrush *)br);
    if (sf) GdipDeleteStringFormat(sf);
    gfx_end(g);
    free(ws);
    return 1;
}


static void dtext(HDC dc, const char *s, int x, int y, int w, int h,
                  COLORREF col, HFONT f, UINT fmt)
{
    RECT r;
    int saved;
    if (!s || !f || w <= 0 || h <= 0) return;
    if (!getenv("LP_GDI_TEXT") && dtext_gp(dc, s, x, y, w, h, col, f, fmt)) return;
    if ((fmt & DT_SINGLELINE) && !(fmt & DT_WORDBREAK))
        fmt |= DT_END_ELLIPSIS;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, f);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, s, -1, &r, fmt);
    RestoreDC(dc, saved);
}

static void dtext_center(HDC dc, const char *s, int x, int y, int w, int h,
                         COLORREF col, HFONT f)
{
    dtext(dc, s, x, y, w, h, col, f,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void dtext_right(HDC dc, const char *s, int x, int y, int w, int h,
                        COLORREF col, HFONT f)
{
    dtext(dc, s, x, y, w, h, col, f,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void hline(HDC dc, int x, int y, int w, COLORREF c)
{
    rect_fill(dc, x, y, w, 1, c);
}

static int in_rect(int x, int y, RECT r)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static int point_in(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ------------------------------------------------------------------ *
 *  .chip : pill
 * ------------------------------------------------------------------ */
static int chip_w(HDC dc, const char *txt, HFONT f)
{
    SIZE sz;
    HFONT old = (HFONT)SelectObject(dc, f ? f : GetStockObject(DEFAULT_GUI_FONT));
    sz.cx = 0; sz.cy = 0;
    if (txt) GetTextExtentPoint32A(dc, txt, (int)strlen(txt), &sz);
    SelectObject(dc, old);
    return sz.cx + S(20);
}

static int chip(HDC dc, int x, int y, const char *txt, COLORREF fg,
                COLORREF bg, COLORREF border, HFONT f)
{
    int w = chip_w(dc, txt, f), h = S(20);
    card(dc, x, y, w, h, bg, border, h / 2);
    dtext_center(dc, txt, x, y, w, h, fg, f);
    return w;
}

/* ------------------------------------------------------------------ *
 *  click feedback: ripples, press state, soft scroll
 * ------------------------------------------------------------------ */
#define FX_RIPPLES 6
typedef struct {
    int      active;
    int      id;
    float    x, y;
    float    t;
    COLORREF col;
} Ripple;

static Ripple g_ripples[FX_RIPPLES];
static float  g_scrollKick = 0.0f;
static int    g_pressRelease = 0;
static float  g_modPress[NMOD];
static float  g_navPress[VIEW_COUNT];

static void fx_ripple_spawn(int id, int x, int y, COLORREF col)
{
    int i, slot = -1;
    for (i = 0; i < FX_RIPPLES; i++) {
        if (!g_ripples[i].active) { slot = i; break; }
        if (slot >= 0 && g_ripples[i].t > g_ripples[slot].t) slot = i;
    }
    if (slot < 0) return;
    g_ripples[slot].active = 1;
    g_ripples[slot].id = id;
    g_ripples[slot].x = (float)x;
    g_ripples[slot].y = (float)y;
    g_ripples[slot].t = 0.0f;
    g_ripples[slot].col = col;
}

static void fx_press_release(void) { g_pressRelease = 1; }

static void fx_tick(void)
{
    int i, anyPress = 0;
    for (i = 0; i < FX_RIPPLES; i++) {
        if (!g_ripples[i].active) continue;
        g_ripples[i].t += 0.045f;
        if (g_ripples[i].t >= 1.0f) g_ripples[i].active = 0;
    }
    for (i = 0; i < NMOD; i++) {
        if (g_pressRelease) g_modPress[i] = flerp(g_modPress[i], 0.0f, 0.34f);
        if (g_modPress[i] > 0.01f) anyPress = 1; else g_modPress[i] = 0.0f;
    }
    for (i = 0; i < VIEW_COUNT; i++) {
        if (g_pressRelease) g_navPress[i] = flerp(g_navPress[i], 0.0f, 0.34f);
        if (g_navPress[i] > 0.01f) anyPress = 1; else g_navPress[i] = 0.0f;
    }
    if (!anyPress) g_pressRelease = 0;
    g_scrollKick = flerp(g_scrollKick, 0.0f, 0.14f);
    if (g_scrollKick > -0.5f && g_scrollKick < 0.5f) g_scrollKick = 0.0f;
}

/* A click ripple: one ring that grows out of the click point and fades.
 *
 *  Drawn as a *stroke* on purpose.  An earlier version filled an alpha
 *  ellipse inside a GDI+ clip path, which made the renderer build a
 *  temporary surface for every ring: on a 1400x950 window that is 5 MB per
 *  ring, hundreds of megabytes per second while the window animates.  A
 *  stroked outline needs no surface of its own, and the ring is clamped to
 *  the element it belongs to, so nothing has to be clipped at all. */
static void fx_ripple_paint(HDC dc, int id, int x, int y, int w, int h)
{
    GpGraphics *g;
    int i, any = 0;

    for (i = 0; i < FX_RIPPLES; i++)
        if (g_ripples[i].active && g_ripples[i].id == id) any = 1;
    if (!any || w <= 0 || h <= 0) return;

    g = gfx_begin(dc);
    if (!g) return;
    for (i = 0; i < FX_RIPPLES; i++) {
        Ripple *r = &g_ripples[i];
        float ease, rad;
        int alpha, cx, cy, half;
        if (!r->active || r->id != id) continue;

        half = (w < h ? w : h) / 2;
        cx = clampi((int)r->x, x + half / 2, x + w - half / 2);
        cy = clampi((int)r->y, y + half / 2, y + h - half / 2);

        ease = 1.0f - (1.0f - r->t) * (1.0f - r->t);
        rad = ease * (float)half;
        alpha = (int)((1.0f - r->t) * (1.0f - r->t) * 150.0f);
        if (rad > 2.0f && alpha > 2)
            gfx_arc_stroke(g, cx, cy, (int)rad, 0.0f, 360.0f,
                           gfx_mix(P.surface, r->col, alpha), 255,
                           2.0f + (1.0f - r->t) * 2.0f, 0);
    }
    gfx_end(g);
}

/* ------------------------------------------------------------------ *
 *  title bar metrics
 * ------------------------------------------------------------------ */
static void titlebar_metrics(HWND hwnd, RECT *bar, RECT btn[TB_BTNS])
{
    RECT rc;
    int i;
    GetClientRect(hwnd, &rc);
    bar->left = 0;
    bar->top = 0;
    bar->right = rc.right;
    bar->bottom = S(TITLE_H);
    /* right to left: close, maximise, minimise, then the theme switch.
     * That is the order every Windows user expects under the mouse. */
    for (i = 0; i < TB_BTNS; i++) {
        btn[i].right = rc.right - S(4) - i * S(TB_BTN_W);
        if (i == 3) btn[i].right -= S(6);
        btn[i].left = btn[i].right - S(TB_BTN_W);
        btn[i].top = 0;
        btn[i].bottom = S(TITLE_H);
    }
}

/* titlebar_metrics() lays the buttons out right to left - index 0 is the
 * rightmost one - and draw_title_glyph() draws the close cross there, then
 * maximise, then minimise, then the theme switch.  So the id for an index
 * is *not* WHAT_TITLE_MIN + index: mapping them the naive way made the X
 * minimise the window and the dash close it. */
static int title_what_for_index(int i)
{
    static const int map[TB_BTNS] = { WHAT_TITLE_CLOSE, WHAT_TITLE_MAX,
                                      WHAT_TITLE_MIN,   WHAT_TITLE_THEME };
    return (i >= 0 && i < TB_BTNS) ? map[i] : -1;
}

static int title_index_for_what(int what)
{
    int i;
    for (i = 0; i < TB_BTNS; i++)
        if (title_what_for_index(i) == what) return i;
    return -1;
}

static void draw_theme_glyph(HDC dc, RECT r, int dark, int hovered)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, i;
    COLORREF col = hovered ? P.brand : P.muted;
    int t = S(1) > 0 ? S(1) : 1;

    if (dark) {                                   /* sun */
        int rad = S(4);
        circle_fill(dc, cx, cy, rad, NOFILL, col);
        for (i = 0; i < 8; i++) {
            double a = i * 3.14159265358979 / 4.0;
            stroke_line(dc,
                        cx + (int)(cos(a) * (rad + S(2))),
                        cy + (int)(sin(a) * (rad + S(2))),
                        cx + (int)(cos(a) * (rad + S(4))),
                        cy + (int)(sin(a) * (rad + S(4))), col, t);
        }
    } else {                                      /* crescent */
        int rad = S(6);
        circle_fill(dc, cx, cy, rad, col, NOBORDER);
        circle_fill(dc, cx + S(4), cy - S(1), rad, P.bg, NOBORDER);
    }
}

static void draw_title_glyph(HDC dc, int which, int hovered, int maximized)
{
    HWND hwnd = g_hMain;
    RECT bar, btn[TB_BTNS];
    RECT r;
    int cx, cy, col;

    if (!hwnd) return;
    titlebar_metrics(hwnd, &bar, btn);
    r = btn[which];
    cx = (r.left + r.right) / 2;
    cy = (r.top + r.bottom) / 2;
    col = hovered ? P.text : P.muted;

    {
        int arm = S(5);
        int t = S(1) > 0 ? S(1) : 1;
        if (which == 0) {                       /* close */
            stroke_line(dc, cx - arm, cy - arm, cx + arm, cy + arm, col, t);
            stroke_line(dc, cx + arm, cy - arm, cx - arm, cy + arm, col, t);
        } else if (which == 2) {                /* minimise */
            stroke_line(dc, cx - arm, cy, cx + arm, cy, col, t);
        } else if (maximized) {                 /* restore */
            stroke_line(dc, cx - arm + 2, cy - arm, cx + arm, cy - arm, col, t);
            stroke_line(dc, cx + arm, cy - arm, cx + arm, cy + arm - 2, col, t);
            stroke_line(dc, cx - arm, cy - arm + 2, cx - arm, cy + arm, col, t);
            stroke_line(dc, cx - arm, cy + arm, cx + arm - 2, cy + arm, col, t);
        } else {                                /* maximise */
            stroke_line(dc, cx - arm, cy - arm, cx + arm, cy - arm, col, t);
            stroke_line(dc, cx + arm, cy - arm, cx + arm, cy + arm, col, t);
            stroke_line(dc, cx + arm, cy + arm, cx - arm, cy + arm, col, t);
            stroke_line(dc, cx - arm, cy + arm, cx - arm, cy - arm, col, t);
        }
    }
}

static void paint_titlebar(HDC dc, HWND hwnd)
{
    RECT bar, btn[TB_BTNS];
    int i;
    int maximized = IsZoomed(hwnd) ? 1 : 0;

    titlebar_metrics(hwnd, &bar, btn);
    rect_fill(dc, bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top, P.bg);

    if (g_bmpLogo)
        draw_logo(dc, g_bmpLogo, g_logoW, g_logoH, S(14),
                  bar.top + (bar.bottom - S(22)) / 2, S(22));
    else
        rrect(dc, S(15), bar.top + (bar.bottom - S(18)) / 2, S(4), S(18), S(2),
              P.brand, NOBORDER);

    {
        int tx = S(14) + (g_bmpLogo ? (int)((double)S(22) * g_logoW / (g_logoH ? g_logoH : 1))
                                    : S(26));
        dtext(dc, "LaptopPrep", tx, bar.top, S(120), bar.bottom - bar.top, P.text,
              g_fTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, PREP_VERSION_STR, tx + S(84), bar.top + 1, S(60),
              bar.bottom - bar.top, P.muted, g_fTiny,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    /* an elevation chip: the operator has to know whether the tool is
     * allowed to touch the update layer before wondering why a step failed */
    {
        int admin = is_admin();
        const char *lbl = admin ? "administrator" : "not elevated";
        COLORREF fg = admin ? P.good : P.warn;
        COLORREF bg = admin ? P.goodSoft : P.warnSoft;
        COLORREF bd = admin ? blend(P.goodSoft, P.good, 0.45f) : P.warnLine;
        int cw = chip_w(dc, lbl, g_fTiny);
        int dot = S(18);
        int cx = btn[3].left - cw - dot - S(14);
        int cy = (bar.bottom - S(20)) / 2;
        if (cx > S(230)) {
            circle_fill(dc, cx + S(6), cy + S(10), S(3), fg, NOBORDER);
            chip(dc, cx + dot - S(6), cy, lbl, fg, bg, bd, g_fTiny);
        }
    }

    for (i = 0; i < TB_BTNS; i++) {
        /* the wash colour follows the map, not the draw order: index 0 is
         * the rightmost button and that one is the close cross, so it is
         * the one that turns red under the mouse */
        int what = title_what_for_index(i);
        float h = g_titleHov[i];
        float pr = g_titlePress[i];
        int dy = (int)(pr * S(2));            /* a 2 px shove while pressed */
        COLORREF bg = P.bg;
        if (h > 0.01f) {
            COLORREF hi = (what == WHAT_TITLE_CLOSE) ? P.bad
                       : (what == WHAT_TITLE_THEME) ? P.brandSoft : P.surfaceHi;
            bg = blend(bg, hi, h);
        }
        if (pr > 0.01f) bg = blend(bg, P.brandSoft, pr * 0.5f);
        rrect(dc, btn[i].left + S(2), btn[i].top + S(5) + dy,
              (btn[i].right - btn[i].left) - S(4),
              (btn[i].bottom - btn[i].top) - S(10), S(8), bg,
              (what == WHAT_TITLE_THEME && h > 0.01f) ? blend(P.bg, P.brandLine, h)
                                                      : NOBORDER);
        fx_ripple_paint(dc, what, btn[i].left, btn[i].top,
                        btn[i].right - btn[i].left, btn[i].bottom - btn[i].top);
        if (i < 3) draw_title_glyph(dc, i, h > 0.35f, maximized);
    }
    draw_theme_glyph(dc, btn[3], g_themeDark ? 1 : 0, g_titleHov[3] > 0.35f);

    /* a 2 px progress line across the window while a run is going: the
     * whole width, so the state is readable from across the room */
    if (g_tStart && !g_finished) {
        float pct = g_ringPct / 100.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        rrect(dc, 0, bar.bottom - 2, bar.right, 2, 0, P.track, NOBORDER);
        rrect(dc, 0, bar.bottom - 2, (int)(bar.right * pct), 2, 0,
              g_anyErr ? P.warn : P.brand, NOBORDER);
    } else {
        hline(dc, 0, bar.bottom - 1, bar.right, P.line);
    }
}

/* ------------------------------------------------------------------ *
 *  step vocabulary
 * ------------------------------------------------------------------ */
static const char *step_state_text(int st)
{
    switch (st) {
        case STEP_RUNNING: return "running";
        case STEP_OK:      return "done";
        case STEP_WARN:    return "check";
        case STEP_ERR:     return "failed";
        case STEP_SKIP:    return "skipped";
        default:           return "queued";
    }
}

static COLORREF step_state_color(int st)
{
    switch (st) {
        case STEP_RUNNING: return P.brand;
        case STEP_OK:      return P.good;
        case STEP_WARN:    return P.warn;
        case STEP_ERR:     return P.bad;
        case STEP_SKIP:    return P.muted;
        default:           return P.track;
    }
}

static int mod_from_bit(unsigned bit)
{
    int i;
    for (i = 0; i < NMOD; i++) if (g_mods[i].bit == bit) return i;
    return -1;
}

static int progress_pct(void)
{
    int i, sel = 0, done = 0;
    for (i = 0; i < NMOD; i++) {
        if (!(g_selected & g_mods[i].bit)) continue;
        sel++;
        if (g_step[i] == STEP_OK || g_step[i] == STEP_ERR ||
            g_step[i] == STEP_WARN || g_step[i] == STEP_SKIP) done++;
    }
    if (sel == 0) return 0;
    return done * 100 / sel;
}

static void fmt_elapsed(char *out, size_t n)
{
    ULONGLONG now, sec;
    if (!g_tStart) { snprintf(out, n, "--"); return; }
    now = g_finished ? g_tEnd : GetTickCount64();
    sec = (now - g_tStart) / 1000;
    if (sec < 60) snprintf(out, n, "%llus", (unsigned long long)sec);
    else if (sec < 3600)
        snprintf(out, n, "%llum %02llus", (unsigned long long)(sec / 60),
                 (unsigned long long)(sec % 60));
    else
        snprintf(out, n, "%lluh %02llum", (unsigned long long)(sec / 3600),
                 (unsigned long long)((sec % 3600) / 60));
}

/* mm:ss from milliseconds - used for step countdowns */
static void fmt_mmss(char *out, size_t n, long ms)
{
    long sec;
    if (ms < 0) ms = 0;
    sec = ms / 1000;
    if (sec < 60)      snprintf(out, n, "%lds", sec);
    else if (sec < 3600) snprintf(out, n, "%ldm %02lds", sec / 60, sec % 60);
    else               snprintf(out, n, "%ldh %02ldm", sec / 3600, (sec % 3600) / 60);
}

/* ------------------------------------------------------------------ *
 *  live log
 * ------------------------------------------------------------------ */
#define LOG_MAX_LINES   20000
#define LOG_KEEP_LINES   8000

static void log_index_add(size_t off)
{
    if (g_nLines >= g_linesCap) {
        int ncap = g_linesCap ? g_linesCap * 2 : 2048;
        size_t *nb = (size_t*)realloc(g_lines, sizeof(size_t) * (size_t)ncap);
        if (!nb) return;
        g_lines = nb;
        g_linesCap = ncap;
    }
    if (g_nLines >= g_lineBornCap) {
        int ncap = g_lineBornCap ? g_lineBornCap * 2 : 2048;
        DWORD *nb = (DWORD*)realloc(g_lineBorn, sizeof(DWORD) * (size_t)ncap);
        if (nb) { g_lineBorn = nb; g_lineBornCap = ncap; }
    }
    g_lines[g_nLines] = off;
    if (g_lineBorn && g_nLines < g_lineBornCap) g_lineBorn[g_nLines] = GetTickCount();
    g_nLines++;
}

static void log_reset(void)
{
    g_logLen = 0;
    g_logTail = 0;
    g_nLines = 0;
    g_logScroll = 0;
    if (g_logBuf) g_logBuf[0] = 0;
}

static void log_trim(void)
{
    int drop, i;
    size_t base;
    if (g_nLines <= LOG_MAX_LINES) return;
    drop = g_nLines - LOG_KEEP_LINES;
    base = g_lines[drop];
    memmove(g_logBuf, g_logBuf + base, g_logLen - base);
    g_logLen -= base;
    g_logBuf[g_logLen] = 0;
    for (i = drop; i < g_nLines; i++) {
        g_lines[i - drop] = g_lines[i] - base;
        if (g_lineBorn && i < g_lineBornCap) g_lineBorn[i - drop] = g_lineBorn[i];
    }
    g_nLines -= drop;
    g_logTail = (g_logTail > base) ? g_logTail - base : 0;
}

static void log_buf_append(const char *data, size_t len)
{
    size_t need = g_logLen + len + 1, i;
    if (need > g_logCap) {
        size_t ncap = g_logCap ? g_logCap : 65536;
        char *nb;
        while (ncap < need) ncap *= 2;
        nb = (char*)realloc(g_logBuf, ncap);
        if (!nb) return;
        g_logBuf = nb;
        g_logCap = ncap;
    }
    memcpy(g_logBuf + g_logLen, data, len);
    g_logLen += len;
    g_logBuf[g_logLen] = 0;

    for (i = g_logLen - len; i < g_logLen; i++) {
        if (g_logBuf[i] == '\n') {
            log_index_add(g_logTail);
            g_logTail = i + 1;
        }
    }
    log_trim();
}

static void log_pump_file(void)
{
    HANDLE h = CreateFileA(prep_log_path(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz, off;
    char tmp[8192];
    DWORD got;

    if (h == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return; }
    if (sz.QuadPart < g_logPos) log_reset();          /* the log was recreated */
    if (sz.QuadPart > g_logPos) {
        off.QuadPart = g_logPos;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
        while (ReadFile(h, tmp, (DWORD)sizeof(tmp) - 1, &got, NULL) && got) {
            log_buf_append(tmp, got);
            g_logPos += got;
        }
    }
    CloseHandle(h);
}

static int log_line_count(void)
{
    return g_nLines + ((g_logTail < g_logLen) ? 1 : 0);
}

static size_t log_line_off(int idx)
{
    return (idx < g_nLines) ? g_lines[idx] : g_logTail;
}

static size_t log_line_len(int idx)
{
    size_t a = log_line_off(idx);
    size_t b = (idx + 1 <= g_nLines) ? log_line_off(idx + 1) : g_logLen;
    size_t n = (b > a) ? (b - a) : 0;
    while (n && (g_logBuf[a + n - 1] == '\n' || g_logBuf[a + n - 1] == '\r')) n--;
    return n;
}

static int ci_contains(const char *s, size_t n, const char *needle)
{
    size_t k = strlen(needle), i, j;
    if (k == 0 || n < k) return 0;
    for (i = 0; i + k <= n; i++) {
        for (j = 0; j < k; j++) {
            char a = s[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == k) return 1;
    }
    return 0;
}

static int starts_with(const char *s, size_t n, const char *pre)
{
    size_t k = strlen(pre);
    return n >= k && strncmp(s, pre, k) == 0;
}

/* the colouring rules: the update stage has its own colour so its story is
 * readable in one look, and every failure is red without having to read */
static COLORREF log_msg_color(const char *s, size_t n)
{
    if (n >= 5 && (!strncmp(s, "-----", 5) || !strncmp(s, "=====", 5))) return P.brand;
    if (starts_with(s, n, "UPD:")) {
        if (ci_contains(s, n, "gave up") || ci_contains(s, n, "refused") ||
            ci_contains(s, n, "not report success") || ci_contains(s, n, "timed out"))
            return P.warn;
        if (starts_with(s, n, "UPD: Verdict")) return P.brand;
        return P.info;
    }
    if (ci_contains(s, n, "failed") || ci_contains(s, n, "error") ||
        ci_contains(s, n, "NOT available") || ci_contains(s, n, "could not"))
        return P.bad;
    if (ci_contains(s, n, "skipped") || ci_contains(s, n, "timed out") ||
        ci_contains(s, n, "not elevated") || ci_contains(s, n, "does not work"))
        return P.warn;
    if (starts_with(s, n, "Exit code:")) {
        size_t k = 10;
        while (k < n && s[k] == ' ') k++;
        return (k < n && s[k] == '0') ? P.good : P.warn;
    }
    if (starts_with(s, n, "Run:")) return P.muted;
    return P.logText;
}

/* "YYYY-MM-DD HH:MM:SS - " */
static int log_line_has_stamp(const char *s, size_t n)
{
    return (n >= 22 && s[4] == '-' && s[7] == '-' && s[10] == ' ' &&
            s[13] == ':' && s[16] == ':' && s[19] == ' ' && s[20] == '-' && s[21] == ' ');
}

static void log_copy_all(void)
{
    HGLOBAL mem;
    char *dst;
    size_t i, o = 0;
    if (!g_logLen) return;
    mem = GlobalAlloc(GMEM_MOVEABLE, g_logLen + (g_logLen / 32) + 16);
    if (!mem) return;
    dst = (char*)GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return; }
    for (i = 0; i < g_logLen; i++) {
        if (g_logBuf[i] == '\n') dst[o++] = '\r';
        dst[o++] = g_logBuf[i];
    }
    dst[o] = 0;
    GlobalUnlock(mem);
    if (OpenClipboard(g_hMain)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_TEXT, mem)) GlobalFree(mem);
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

/* ------------------------------------------------------------------ *
 *  animation
 * ------------------------------------------------------------------ */
static void anim_view_changed(void) { g_viewAnim = 0.0f; }

static DWORD g_gdiBase = 0;
static DWORD g_gdiLastReport = 0;
static int   g_gdiWarned = 0, g_gdiPanic = 0;

static unsigned g_paintCount = 0;
static unsigned g_bufCount = 0;

static void gdi_audit(void)
{
    DWORD n;
    char b[240];
    DWORD now = GetTickCount();
    PROCESS_MEMORY_COUNTERS pmc;
    n = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (!g_gdiBase) g_gdiBase = n;
    if (now - g_gdiLastReport < (getenv("LP_GDI_FAST") ? 5000u : 60000u)) return;
    g_gdiLastReport = now;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    snprintf(b, sizeof(b),
             "UI: %lu GDI handles (baseline %lu), %u paints, %u off-screen "
             "buffers, %lu MB working set",
             (unsigned long)n, (unsigned long)g_gdiBase, g_paintCount, g_bufCount,
             (unsigned long)(pmc.WorkingSetSize / (1024ul * 1024ul)));
    write_log(b);
    if (n > 2000 && !g_gdiWarned) {
        g_gdiWarned = 1;
        write_log("UI WARNING: the GDI handle count is climbing - a painter is "
                  "leaking GDI objects, please report this log");
    }
    if (n > 7000 && !g_gdiPanic) {
        g_gdiPanic = 1;
        write_log("UI: handle count is close to the Windows limit - the window "
                  "could stop drawing. Everything is already on disk and on the "
                  "server, so it is safe to close and reopen LaptopPrep");
        MessageBoxA(g_hMain,
            "LaptopPrep is using an unusual number of Windows drawing handles.\n\n"
            "Your log is already saved on disk (and uploaded when configured),\n"
            "so nothing is lost: close LaptopPrep, reopen it and press Run prep\n"
            "again - it picks up where Windows Update left off.\n\n"
            "Please send the log so this can be fixed.",
            "LaptopPrep - please restart the app", MB_ICONWARNING | MB_OK);
    }
}

static int ui_needs_frame(void)
{
    static DWORD lastForced = 0;
    int i;
    DWORD now = GetTickCount();

    if (g_runningNow()) return 1;
    if (g_viewAnim < 0.999f) return 1;
    if (g_restartOffered && g_restartAnim < 0.999f) return 1;
    if (g_runScreen && g_runIn < 0.999f) return 1;
    for (i = 0; i < WU_STEP_COUNT; i++)
        if (g_wuCheck[i] > 0.001f && g_wuCheck[i] < 0.999f) return 1;
    if (g_restartDeadline) return 1;            /* the countdown ticks */
    if (g_scrollKick > 0.5f || g_scrollKick < -0.5f) return 1;
    if (g_noticeOff && g_noticeFade > 0.02f) return 1;
    if (g_editRow >= 0) return 1;               /* the caret blinks */
    for (i = 0; i < FX_RIPPLES; i++)
        if (g_ripples[i].active) return 1;
    for (i = 0; i < NMOD; i++)
        if (g_modPress[i] > 0.01f) return 1;
    for (i = 0; i < VIEW_COUNT; i++)
        if (g_navPress[i] > 0.01f) return 1;
    if (g_hoverNav >= 0 || g_hoverModule >= 0 || g_hoverBtn >= 0 ||
        g_hoverTitle >= 0) return 1;
    for (i = 0; i < VIEW_COUNT; i++)
        if (g_navHov[i] > 0.01f && g_navHov[i] < 0.99f) return 1;
    for (i = 0; i < NMOD; i++)
        if ((g_modHov[i] > 0.01f && g_modHov[i] < 0.99f) ||
            (g_modPop[i] > 0.01f) ||
            (g_stepEase[i] > 0.01f && g_stepEase[i] < 0.99f) ||
            (g_stepFlash[i] > 0.01f)) return 1;
    for (i = 0; i < WU_STEP_COUNT; i++)
        if ((g_wuStepEase[i] > 0.01f && g_wuStepEase[i] < 0.99f) ||
            g_wuRowFlash[i] > 0.01f) return 1;
    for (i = 0; i < 6; i++)
        if ((g_btnHov[i] > 0.01f && g_btnHov[i] < 0.99f) || g_btnPress[i] > 0.01f) return 1;
    for (i = 0; i < 2; i++)
        if (g_linkHov[i] > 0.01f && g_linkHov[i] < 0.99f) return 1;
    for (i = 0; i < TB_BTNS; i++)
        if ((g_titleHov[i] > 0.01f && g_titleHov[i] < 0.99f) ||
            g_titlePress[i] > 0.01f) return 1;
    for (i = 0; i < 8; i++)
        if (g_rowPress[i] > 0.01f) return 1;
    if (g_ringPct > 0.0f && g_ringPct < 99.9f) return 1;
    if (now - lastForced > 500) { lastForced = now; return 1; }
    return 0;
}

static void anim_tick(void)
{
    int i;
    g_frame += 1.0f;

    for (i = 0; i < VIEW_COUNT; i++)
        g_navHov[i] = flerp(g_navHov[i], (g_hoverNav == i) ? 1.0f : 0.0f, 0.22f);
    for (i = 0; i < NMOD; i++) {
        float tgt = (g_hoverModule == i) ? 1.0f : 0.0f;
        g_modHov[i] = flerp(g_modHov[i], tgt, 0.28f);
        g_modPop[i] = flerp(g_modPop[i], 0.0f, 0.10f);
    }
    g_btnHov[0] = flerp(g_btnHov[0], (g_hoverBtn == WHAT_BTN_PRIMARY)   ? 1.0f : 0.0f, 0.28f);
    g_btnHov[1] = flerp(g_btnHov[1], (g_hoverBtn == WHAT_BTN_SECONDARY) ? 1.0f : 0.0f, 0.28f);
    g_btnHov[2] = flerp(g_btnHov[2], (g_hoverBtn == WHAT_BTN_GHOST)     ? 1.0f : 0.0f, 0.28f);
    g_btnHov[3] = flerp(g_btnHov[3], (g_hoverBtn == WHAT_BTN_WU_ACTION) ? 1.0f : 0.0f, 0.28f);
    g_btnHov[4] = flerp(g_btnHov[4], (g_hoverBtn == WHAT_BTN_RESTART)      ? 1.0f : 0.0f, 0.28f);
    g_btnHov[5] = flerp(g_btnHov[5], (g_hoverBtn == WHAT_BTN_RESTART_LATE) ? 1.0f : 0.0f, 0.28f);
    g_linkHov[0] = flerp(g_linkHov[0], (g_hoverBtn == WHAT_LINK_WINTR) ? 1.0f : 0.0f, 0.30f);
    g_linkHov[1] = flerp(g_linkHov[1], (g_hoverBtn == WHAT_LINK_AGPCS) ? 1.0f : 0.0f, 0.30f);
    for (i = 0; i < 4; i++)
        g_btnPress[i] = flerp(g_btnPress[i], 0.0f, 0.12f);
    for (i = 0; i < TB_BTNS; i++)
        g_titleHov[i] = flerp(g_titleHov[i], (g_hoverTitle == i) ? 1.0f : 0.0f, 0.32f);
    for (i = 0; i < TB_BTNS; i++)
        g_titlePress[i] = flerp(g_titlePress[i], 0.0f, 0.14f);
    for (i = 0; i < 8; i++)
        g_rowPress[i] = flerp(g_rowPress[i], 0.0f, 0.16f);

    g_ringPct = flerp(g_ringPct, (float)progress_pct(), g_finished ? 0.16f : 0.10f);
    if (g_runningNow()) g_ringSpin += 0.05f;
    g_ringPulse = g_runningNow() ? (0.5f + 0.5f * (float)sin(g_frame * 0.07)) : 0.0f;
    g_navGlow += 0.045f;

    for (i = 0; i < NMOD; i++) {
        float tgt = (g_step[i] == -1) ? 0.0f : 1.0f;
        g_stepEase[i] = flerp(g_stepEase[i], tgt, g_finished ? 0.35f : 0.14f);
        g_stepFlash[i] = flerp(g_stepFlash[i], 0.0f, 0.055f);
    }

    /* the update step list: rows fade in as they become relevant, and a row
     * flashes when its state changes so the eye is drawn to it */
    for (i = 0; i < WU_STEP_COUNT; i++) {
        int st = g_runScreen && (g_selected & PREP_M_UPDATES) && g_tStart
                 ? prep_update_step_state(i) : STEP_QUEUED;
        int relevant = (st != STEP_QUEUED);
        g_wuStepEase[i] = flerp(g_wuStepEase[i], relevant ? 1.0f : 0.0f, 0.12f);
        if (st != g_wuLastState[i]) {
            g_wuLastState[i] = st;
            if (st == STEP_OK || st == STEP_WARN || st == STEP_ERR) g_wuRowFlash[i] = 1.0f;
            if (st == STEP_OK) g_wuCheck[i] = 0.0f;      /* draw the tick */
        }
        g_wuRowFlash[i] = flerp(g_wuRowFlash[i], 0.0f, 0.05f);
        if (g_wuCheck[i] < 1.0f) g_wuCheck[i] = flerp(g_wuCheck[i], 1.0f, 0.075f);
    }

    g_restartAnim = flerp(g_restartAnim, g_restartOffered ? 1.0f : 0.0f, 0.14f);
    g_runIn = flerp(g_runIn, 1.0f, 0.10f);

    g_viewAnim = flerp(g_viewAnim, 1.0f, 0.16f);
    fx_tick();
    g_scrollAnim = flerp(g_scrollAnim, (float)g_logScroll + g_scrollKick, 0.30f);
    if (g_noticeOff && g_noticeFade > 0.0f) {
        g_noticeFade = flerp(g_noticeFade, 0.0f, 0.30f);
        if (g_noticeFade < 0.02f) g_noticeFade = 0.0f;
    }
    if (g_view == VIEW_LOG) g_logFade = flerp(g_logFade, 1.0f, 0.2f);
    else g_logFade = 0.0f;
}

/* ------------------------------------------------------------------ *
 *  engine hooks (called from the worker thread)
 * ------------------------------------------------------------------ */
void gui_on_module_begin(unsigned bit)
{
    int i = mod_from_bit(bit);
    if (i < 0) return;
    InterlockedExchange(&g_active, i);
    InterlockedExchange(&g_step[i], STEP_RUNNING);
    snprintf(g_status, sizeof(g_status), "Working on: %s", g_mods[i].title);
    if (g_hMain) PostMessageA(g_hMain, WM_APP + 10, 0, 0);
}

void gui_on_module_result(unsigned bit, int ok)
{
    int i = mod_from_bit(bit);
    int st;
    if (i < 0) return;
    /* the update stage reports its own severity: amber is not red, and
     * "needs attention" has to be visible without reading the log */
    if (bit == PREP_M_UPDATES) {
        int sev = prep_update_severity();
        if (!ok)                    st = STEP_ERR;
        else if (sev >= 3)          st = STEP_ERR;
        else if (sev >= 2)          st = STEP_WARN;
        else                        st = STEP_OK;
        if (st == STEP_WARN) g_anyWarn = 1;
    } else {
        st = ok ? STEP_OK : STEP_ERR;
    }
    g_stepFlash[i] = 1.0f;
    InterlockedExchange(&g_step[i], st);
    InterlockedExchange(&g_active, -1);
    if (st == STEP_ERR) g_anyErr = 1;
    if (g_hMain) PostMessageA(g_hMain, WM_APP + 10, 0, 0);
}

void gui_on_run_finished(int rc)
{
    g_runRc = rc;
    g_tEnd = GetTickCount64();
    InterlockedExchange(&g_runFinishedFlag, 1);
    if (g_hMain) PostMessageA(g_hMain, WM_APP + 11, 0, 0);
}

void gui_tick(void)
{
    /* The engine runs on the worker thread; the UI repaints itself from its
     * own timer, so there is nothing to do here.  It exists so the engine
     * never has to know about window handles. */
    Sleep(0);
}

/* ------------------------------------------------------------------ *
 *  branding
 * ------------------------------------------------------------------ */
static void exe_dir(char *out, size_t n)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    DWORD i;
    if (!len || len >= MAX_PATH) { snprintf(out, n, "."); return; }
    for (i = len; i > 0; i--)
        if (path[i - 1] == '\\' || path[i - 1] == '/') break;
    path[i ? i - 1 : 0] = 0;
    snprintf(out, n, "%s", path);
}

static HBITMAP load_bitmap_file(const char *name, int *outW, int *outH)
{
    char dir[MAX_PATH], path[MAX_PATH * 2];
    HBITMAP bmp;
    exe_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0,
                              LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!bmp) {
        snprintf(path, sizeof(path), "%s\\assets\\%s", dir, name);
        bmp = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0,
                                  LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    }
    if (bmp && outW && outH) {
        BITMAP bm;
        if (GetObject(bmp, sizeof(bm), &bm)) { *outW = bm.bmWidth; *outH = bm.bmHeight; }
    }
    return bmp;
}

static void load_branding(void)
{
    g_bmpLogo    = load_bitmap_file("logo_side.bmp", &g_logoW, &g_logoH);
    g_bmpLogoBig = load_bitmap_file("logo_about.bmp", &g_logoBigW, &g_logoBigH);
    g_logoLoaded = 1;
}

static void draw_logo(HDC dc, HBITMAP bmp, int srcW, int srcH, int x, int y, int h)
{
    HDC mem;
    HGDIOBJ old;
    int w;
    if (!bmp || srcW <= 0 || srcH <= 0) return;
    w = (int)((double)h * (double)srcW / (double)srcH + 0.5);
    mem = CreateCompatibleDC(dc);
    if (!mem) return;
    old = SelectObject(mem, bmp);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x, y, w, h, mem, 0, 0, srcW, srcH, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

/* the esprit ICT mark: polygons so it stays crisp at every DPI */
static void paint_mark(HDC dc, int x, int y, int w, int h)
{
    POINT bar[4], up[4], lo[4];
#define MX(f) (x + (int)((double)(w) * (f) + 0.5))
#define MY(f) (y + (int)((double)(h) * (f) + 0.5))

    bar[0].x = MX(0.765); bar[0].y = MY(0.000);
    bar[1].x = MX(0.975); bar[1].y = MY(0.000);
    bar[2].x = MX(0.975); bar[2].y = MY(1.000);
    bar[3].x = MX(0.765); bar[3].y = MY(1.000);
    Polygon(dc, bar, 4);

    up[0].x = MX(0.100); up[0].y = MY(0.195);
    up[1].x = MX(0.765); up[1].y = MY(0.355);
    up[2].x = MX(0.765); up[2].y = MY(0.510);
    up[3].x = MX(0.005); up[3].y = MY(0.335);
    Polygon(dc, up, 4);

    lo[0].x = MX(0.100); lo[0].y = MY(0.805);
    lo[1].x = MX(0.765); lo[1].y = MY(0.645);
    lo[2].x = MX(0.765); lo[2].y = MY(0.490);
    lo[3].x = MX(0.005); lo[3].y = MY(0.665);
    Polygon(dc, lo, 4);
    gfx_poly_fill(dc, bar, 4, P.brand);
    gfx_poly_fill(dc, up, 4, P.brand);
    gfx_poly_fill(dc, lo, 4, P.brand);
#undef MX
#undef MY
}

static void paint_brand(HDC dc, int x, int y, int h)
{
    int markW = (int)((double)h * (81.0 / 157.0) + 0.5);
    int tx = x + markW + S(10);
    SIZE a;
    int tw;

    paint_mark(dc, x, y, markW, h);

    SelectObject(dc, g_fBrand);
    GetTextExtentPoint32A(dc, "esprit", 6, &a);
    tw = a.cx;

    dtext(dc, "esprit", tx, y, S(170), h, P.text, g_fBrand,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    dtext(dc, "ICT", tx + tw + S(3), y, S(80), h, P.text2, g_fBrandI,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

/* ------------------------------------------------------------------ *
 *  layout: one place that decides every rectangle on a page
 * ------------------------------------------------------------------ */
#define SIDE_W     268
#define CONTENT_PAD 34

typedef struct {
    RECT  client;
    RECT  side;
    RECT  content;
    RECT  cards[NMOD];
    RECT  planCard;              /* home: what the update stage will do */
    RECT  infoA, infoB;          /* home: the two note cards           */
    RECT  btnPrimary, btnSecondary, btnGhost, btnAction;
    RECT  nav[VIEW_COUNT];
    RECT  logArea;
    RECT  btnLog[3];
    RECT  summary;              /* run screen: the progress/verdict card */
    RECT  restartBand;          /* run screen: the restart offer          */
    RECT  btnRestart, btnRestartLate;
    RECT  creditTitle;          /* about: the copyright line              */
    RECT  creditPerm;           /* about: the licence line                */
    RECT  linkWintr, linkAgpcs; /* about: the two clickable home pages    */
    RECT  stepsCard;            /* run screen: the 8 update steps         */
    RECT  stepRows[WU_STEP_COUNT];
    int   rowH;
    int   stepRowH;
    int   twoCol;
    int   cw, ch;
} Layout;

static void paint_restart_band(HDC dc, Layout *L);

static void compute_layout(HWND hwnd, Layout *L)
{
    int w, h, i, x0, y0;
    int sbw = S(SIDE_W);
    RECT wr;
    int footerH = S(60);

    GetClientRect(hwnd, &L->client);
    w = L->client.right;
    h = L->client.bottom;

    L->side.left = 0; L->side.top = S(TITLE_H); L->side.right = sbw; L->side.bottom = h;

    x0 = sbw + S(CONTENT_PAD);
    y0 = S(TITLE_H) + S(26);
    L->content.left = x0;
    L->content.top = y0;
    L->content.right = w - S(CONTENT_PAD);
    L->content.bottom = h - S(26);
    L->cw = L->content.right - L->content.left;
    L->ch = L->content.bottom - L->content.top;

    memset(L->nav, 0, sizeof(L->nav));
    for (i = 0; i < VIEW_COUNT; i++) {
        L->nav[i].left   = S(16);
        L->nav[i].top    = S(TITLE_H) + S(58) + i * S(46);
        L->nav[i].right  = sbw - S(16);
        L->nav[i].bottom = L->nav[i].top + S(40);
    }

    /* ---- home: module options, the update plan, the notes ----------- *
     *  Three stacked blocks.  Each one has a minimum and a comfortable
     *  height; the space that is left is shared out, and when the window
     *  is short the blocks at the bottom are dropped rather than squashed. */
    L->twoCol = (L->cw >= S(880)) ? 1 : 0;
    memset(&L->planCard, 0, sizeof(L->planCard));
    memset(&L->infoA, 0, sizeof(L->infoA));
    memset(&L->infoB, 0, sizeof(L->infoB));
    {
        int top = L->content.top + S(96);          /* below the header  */
        int bottom = L->content.bottom - footerH;
        int rows = L->twoCol ? (NMOD + 1) / 2 : NMOD;
        int gap = S(8);
        int avail = bottom - top;
        int used, planH, infoH;
        int colw = L->twoCol ? (L->cw - S(14)) / 2 : L->cw;

        L->rowH = (rows ? (avail - (rows - 1) * gap) * 52 / 100 / rows : S(48));
        L->rowH = clampi(L->rowH, S(44), S(72));
        used = rows * L->rowH + (rows - 1) * gap;

        planH = (avail - used - gap) * 58 / 100;
        planH = clampi(planH, 0, S(200));
        if (planH < S(104)) planH = 0;

        infoH = avail - used - gap - ((planH > 0) ? planH + gap : 0);
        if (infoH > S(148)) infoH = S(148);
        if (infoH < S(84)) {
            /* no room for the notes: give it to the plan card instead */
            if (planH > 0) {
                int add = avail - used - gap - planH;
                if (add > 0) planH += add;
                infoH = 0;
            }
        }

        for (i = 0; i < NMOD; i++) {
            int col = L->twoCol ? (i / rows) : 0;
            int row = L->twoCol ? (i % rows) : i;
            int cx = x0 + col * (colw + S(14));
            int cy = top + row * (L->rowH + gap);
            L->cards[i].left = cx;
            L->cards[i].top = cy;
            L->cards[i].right = cx + colw;
            L->cards[i].bottom = cy + L->rowH;
        }

        {
            int y = top + used + gap;
            if (planH >= S(104)) {
                L->planCard.left = x0;
                L->planCard.top = y;
                L->planCard.right = L->content.right;
                L->planCard.bottom = y + planH;
                y = L->planCard.bottom + gap;
            }
            if (infoH >= S(84)) {
                int c1w = (L->cw - S(14)) * 3 / 5;
                L->infoA.left = x0;
                L->infoA.top = y;
                L->infoA.right = x0 + c1w;
                L->infoA.bottom = y + infoH;
                L->infoB.left = L->infoA.right + S(14);
                L->infoB.top = y;
                L->infoB.right = L->content.right;
                L->infoB.bottom = y + infoH;
            }
        }
    }

    /* ---- run screen: the restart offer, summary, the step list ------ */
    {
        int top = L->content.top + S(96);
        int bottom = L->content.bottom - footerH;
        int sh = S(132);
        int avail;

        memset(&L->restartBand, 0, sizeof(L->restartBand));
        memset(&L->btnRestart, 0, sizeof(L->btnRestart));
        memset(&L->btnRestartLate, 0, sizeof(L->btnRestartLate));
        if (g_restartOffered && g_view == VIEW_HOME && g_runScreen) {
            int bh = S(76);
            L->restartBand.left  = x0;
            L->restartBand.top   = top;
            L->restartBand.right = L->content.right;
            L->restartBand.bottom = top + bh;
            L->btnRestart.right = L->content.right - S(16);
            L->btnRestart.left  = L->btnRestart.right - S(150);
            L->btnRestart.top   = L->restartBand.top + (bh - S(34)) / 2;
            L->btnRestart.bottom = L->btnRestart.top + S(34);
            L->btnRestartLate.right = L->btnRestart.left - S(8);
            L->btnRestartLate.left  = L->btnRestartLate.right - S(90);
            L->btnRestartLate.top   = L->btnRestart.top;
            L->btnRestartLate.bottom = L->btnRestart.bottom;
            top += bh + S(12);
        }

        if (bottom - top < S(280)) {                /* very short window */
            sh = S(104);
        }
        L->summary.left = x0;
        L->summary.top = top;
        L->summary.right = L->content.right;
        L->summary.bottom = top + sh;

        L->stepsCard.left = x0;
        L->stepsCard.top = L->summary.bottom + S(12);
        L->stepsCard.right = L->content.right;
        L->stepsCard.bottom = bottom;
        avail = L->stepsCard.bottom - L->stepsCard.top - S(40);   /* header row */
        L->stepRowH = avail / WU_STEP_COUNT;
        L->stepRowH = clampi(L->stepRowH, S(30), S(48));
        for (i = 0; i < WU_STEP_COUNT; i++) {
            L->stepRows[i].left = L->stepsCard.left + S(10);
            L->stepRows[i].right = L->stepsCard.right - S(10);
            L->stepRows[i].top = L->stepsCard.top + S(34) + i * L->stepRowH;
            L->stepRows[i].bottom = L->stepRows[i].top + L->stepRowH;
        }
    }

    /* ---- footer buttons -------------------------------------------- */
    L->btnPrimary.right  = L->content.right;
    L->btnPrimary.left   = L->btnPrimary.right - S(196);
    L->btnPrimary.bottom = L->content.bottom;
    L->btnPrimary.top    = L->btnPrimary.bottom - S(44);

    L->btnSecondary.right  = L->btnPrimary.left - S(10);
    L->btnSecondary.left   = L->btnSecondary.right - S(150);
    L->btnSecondary.top    = L->btnPrimary.top;
    L->btnSecondary.bottom = L->btnPrimary.bottom;

    L->btnGhost.right  = L->btnSecondary.left - S(10);
    L->btnGhost.left   = L->btnGhost.right - S(150);
    L->btnGhost.top    = L->btnPrimary.top;
    L->btnGhost.bottom = L->btnPrimary.bottom;

    /* the context action button sits above the footer, right aligned */
    L->btnAction.right  = L->content.right;
    L->btnAction.left   = L->btnAction.right - S(210);
    L->btnAction.bottom = L->summary.bottom - S(18);
    L->btnAction.top    = L->btnAction.bottom - S(38);
    if (L->btnAction.top < L->summary.top + S(20)) {
        memset(&L->btnAction, 0, sizeof(L->btnAction));
    }

    /* ---- about: the credits block ---------------------------------- *
     *  The About painter draws at exactly these rectangles, so the links
     *  are clickable where they appear. */
    {
        int ax = L->content.left;
        int ay = L->content.top + S(96) + S(144);
        L->creditTitle.left  = ax;
        L->creditTitle.top   = ay;
        L->creditTitle.right = ax + L->cw;
        L->creditTitle.bottom = ay + S(20);
        L->linkWintr.left  = ax;
        L->linkWintr.top   = ay + S(26);
        L->linkWintr.right = ax + S(150);
        L->linkWintr.bottom = L->linkWintr.top + S(28);
        L->linkAgpcs.left  = L->linkWintr.right + S(8);
        L->linkAgpcs.top   = L->linkWintr.top;
        L->linkAgpcs.right = L->linkAgpcs.left + S(150);
        L->linkAgpcs.bottom = L->linkWintr.bottom;
        L->creditPerm.left   = ax;
        L->creditPerm.top    = L->linkWintr.bottom + S(8);
        L->creditPerm.right  = ax + L->cw;
        L->creditPerm.bottom = L->creditPerm.top + S(34);
    }

    /* ---- log page -------------------------------------------------- */
    L->logArea.left   = x0;
    L->logArea.top    = L->content.top + S(92);
    L->logArea.right  = L->content.right;
    L->logArea.bottom = L->content.bottom - S(30);
    {
        int bw = S(112), bh = S(30);
        int bx = L->content.right - bw;
        int by = L->content.top + S(16);
        for (i = 2; i >= 0; i--) {
            L->btnLog[i].left = bx; L->btnLog[i].top = by;
            L->btnLog[i].right = bx + bw; L->btnLog[i].bottom = by + bh;
            bx -= (bw + S(8));
        }
    }
    (void)wr;
}

/* ------------------------------------------------------------------ *
 *  shared painters
 * ------------------------------------------------------------------ */
/* The warning bar lives on every view - which is right until somebody has
 * read it, so it now has a close cross.  Dismissing it collapses the bar
 * instead of fading it: nothing in this interface is drawn with a partial
 * alpha, and a height animation does not need one. */
static int notice_rect(Layout *L, RECT *r)
{
    int y = L->content.bottom - S(70);
    if (y < L->content.top + S(120)) return 0;
    r->left   = L->content.left;
    r->top    = y;
    r->right  = L->content.left + L->cw;
    r->bottom = y + S(26);
    return 1;
}

static int notice_close_rect(Layout *L, RECT *r)
{
    RECT n;
    if (!notice_rect(L, &n)) return 0;
    r->right  = n.right - S(6);
    r->left   = r->right - S(22);
    r->top    = n.top + S(2);
    r->bottom = n.bottom - S(2);
    return 1;
}

static void paint_notice(HDC dc, Layout *L)
{
    const char *n = prep_log_notice();
    RECT r, c;
    int h;
    if (!n || !n[0]) return;
    if (g_noticeOff && g_noticeFade < 0.02f) return;
    if (!notice_rect(L, &r)) return;
    h = (int)((float)S(26) * g_noticeFade);
    if (h < 2) return;

    rrect(dc, r.left, r.top, r.right - r.left, h, S(8), P.warnSoft, P.warnLine);
    circle_fill(dc, r.left + S(16), r.top + h / 2, S(3), P.warn, NOBORDER);
    dtext(dc, n, r.left + S(26), r.top + 4, (r.right - r.left) - S(72), S(18), P.warn,
          g_fTiny, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (h > S(14) && notice_close_rect(L, &c)) {
        int cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, arm = S(4);
        int t  = S(1) > 0 ? S(1) : 1;
        int hot = (g_hoverBtn == WHAT_NOTICE_CLOSE);
        COLORREF col = hot ? P.text : P.warn;
        rrect(dc, c.left, c.top, c.right - c.left, c.bottom - c.top, S(6),
              hot ? P.warnLine : P.warnSoft, NOBORDER);
        stroke_line(dc, cx - arm, cy - arm, cx + arm, cy + arm, col, t);
        stroke_line(dc, cx + arm, cy - arm, cx - arm, cy + arm, col, t);
    }
}

static void paint_header(HDC dc, Layout *L, const char *title, const char *sub)
{
    int x = L->content.left;
    int y = L->content.top;
    int w = L->cw;

    dtext(dc, "LAPTOPPREP", x, y, w, S(14), P.brand, g_fTiny,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    dtext(dc, title, x, y + S(16), w, S(34), P.text, g_fH1,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    dtext(dc, sub, x, y + S(54), w, S(20), P.text2, g_fSmall,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

/* one stroked glyph per nav row, so the rail needs no image assets */
static void draw_nav_icon(HDC dc, int which, int cx, int cy, COLORREF col)
{
    int a = S(7), i;
    switch (which) {
        case 0:                                   /* home */
            stroke_line(dc, cx - a, cy + S(1), cx, cy - a + S(1), col, S(1) > 0 ? S(1) : 1);
            stroke_line(dc, cx, cy - a + S(1), cx + a, cy + S(1), col, S(1) > 0 ? S(1) : 1);
            stroke_line(dc, cx - a + S(3), cy + S(1), cx - a + S(3), cy + a - S(1), col, S(1) > 0 ? S(1) : 1);
            stroke_line(dc, cx - a + S(3), cy + a - S(1), cx + a - S(3), cy + a - S(1), col, S(1) > 0 ? S(1) : 1);
            stroke_line(dc, cx + a - S(3), cy + a - S(1), cx + a - S(3), cy + S(1), col, S(1) > 0 ? S(1) : 1);
            break;
        case 1:                                   /* log: three text lines */
            for (i = -1; i <= 1; i++)
                stroke_line(dc, cx - a, cy + i * S(5),
                            cx + (i == 0 ? a : a - S(4)), cy + i * S(5),
                            col, S(1) > 0 ? S(1) : 1);
            break;
        case 2:                                   /* settings: sliders */
            for (i = 0; i < 2; i++) {
                int y = cy + i * S(7) - S(3);
                stroke_line(dc, cx - a, y, cx + a, y, col, S(1) > 0 ? S(1) : 1);
                circle_fill(dc, cx + (i ? -S(3) : S(3)), y, S(3),
                            col, NOBORDER);
            }
            break;
        default:                                  /* about: info */
            circle_fill(dc, cx, cy, S(7), NOFILL, col);
            rect_fill(dc, cx, cy - S(1), S(2), S(5), col);
            rect_fill(dc, cx, cy - S(4), S(2), S(2), col);
            break;
    }
}

static void paint_sidebar(HDC dc, Layout *L)
{
    int i;
    int sw = L->side.right - L->side.left;
    int x = S(16), w = sw - S(32);
    int y = L->side.top + S(18);

    rect_fill(dc, L->side.left, L->side.top, sw, L->side.bottom - L->side.top, P.side);

    dtext(dc, "PREPARE", x + S(6), y, w - S(6), S(14), P.muted, g_fTiny,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    for (i = 0; i < VIEW_COUNT; i++) {
        int sel = (g_view == i);
        float hv = g_navHov[i];
        RECT r = L->nav[i];
        COLORREF bg = P.side, halo, fg;
        int icx = r.left + S(24), icy = (r.top + r.bottom) / 2;
        int tx = r.left + S(44);
        int tw = r.right - tx - S(12);

        if (sel)         bg = blend(bg, P.sideSel, 1.0f);
        if (hv > 0.01f)  bg = blend(bg, P.surfaceHi, hv * (sel ? 0.45f : 1.0f));
        if (g_navPress[i] > 0.01f) bg = blend(bg, P.brandSoft, g_navPress[i] * 0.55f);
        if (sel || hv > 0.01f || g_navPress[i] > 0.01f)
            rrect(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, S(10), bg,
                  sel ? P.brandLine
                      : (g_navPress[i] > 0.01f ? blend(P.lineHi, P.brand, g_navPress[i])
                                               : blend(P.line, P.lineHi, hv)));
        if (g_navPress[i] > 0.02f)
            gfx_round_stroke(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             S(10), P.brand, (int)(g_navPress[i] * 200.0f), 2.0f);
        fx_ripple_paint(dc, 100 + i, r.left, r.top, r.right - r.left, r.bottom - r.top);

        if (sel) {
            float gl = 0.5f + 0.5f * (float)sin(g_navGlow);
            int rh = (r.bottom - r.top) - S(18);
            rrect(dc, r.left + S(3), r.top + S(9), S(3), rh, S(2), P.brand, NOBORDER);
            halo = blend(P.sideSel, P.brand, 0.06f + 0.06f * gl);
            rrect(dc, r.left + S(7), r.top + S(6), r.right - r.left - S(14),
                  r.bottom - r.top - S(12), S(8), halo, NOBORDER);
        }

        fg = sel ? P.text : blend(P.text2, P.text, hv);
        draw_nav_icon(dc, i, icx, icy, sel ? P.brand : fg);

        if (i == VIEW_LOG && g_tStart && !g_finished) {
            int dotR = S(4) + (int)(S(1) * g_ringPulse + 0.5f);
            circle_fill(dc, r.right - S(18), icy, dotR,
                        blend(P.brandPress, P.brand, g_ringPulse), NOBORDER);
            tw -= S(16);
        }
        dtext(dc, g_nav[i], tx, r.top, tw, r.bottom - r.top, fg,
              sel ? g_fStep : g_fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    /* ---- this machine ---------------------------------------------- */
    {
        int cy = L->nav[VIEW_COUNT - 1].bottom + S(18);
        int chh = S(70);
        char line[192];
        const char *sname = prep_computer_name();
        int admin = is_admin();

        dtext(dc, "THIS MACHINE", x + S(6), cy, w - S(6), S(14), P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        cy += S(18);
        rrect(dc, x, cy, w, chh, S(12), P.surface, P.line);
        circle_fill(dc, x + S(16), cy + S(18), S(3),
                    g_tStart && !g_finished ? P.brand : P.good, NOBORDER);
        snprintf(line, sizeof(line), "%s", sname ? sname : "unknown");
        dtext(dc, line, x + S(26), cy + S(10), w - S(38), S(16), P.text, g_fStep,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        dtext(dc, "Windows Update prep", x + S(16), cy + S(32), w - S(32), S(16),
              P.text2, g_fTiny, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, admin ? "running as administrator" : "not elevated",
              x + S(16), cy + S(48), w - S(32), S(16),
              admin ? P.good : P.warn, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }

    /* ---- footer status -------------------------------------------- */
    {
        int fh = S(86);
        int fy = L->side.bottom - fh - S(14);
        char buf[220];
        if (fy < L->nav[VIEW_COUNT - 1].bottom + S(120)) return;
        hline(dc, S(16), fy - S(14), w, P.line);
        rrect(dc, x, fy, w, fh, S(12), P.surface, P.line);
        if (g_tStart) {
            char el[32];
            fmt_elapsed(el, sizeof(el));
            snprintf(buf, sizeof(buf), g_finished ? "Last run %s" : "Running %s", el);
        } else {
            snprintf(buf, sizeof(buf), "Ready");
        }
        circle_fill(dc, x + S(16), fy + S(18), S(3),
                    g_tStart && !g_finished ? P.brand : (g_finished && g_anyErr ? P.bad : P.good),
                    NOBORDER);
        dtext(dc, buf, x + S(26), fy + S(10), w - S(38), S(16),
              g_tStart && !g_finished ? P.brand : P.text, g_fStep,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        snprintf(buf, sizeof(buf), "%s", g_status);
        dtext(dc, buf, x + S(16), fy + S(32), w - S(32), fh - S(40),
              P.muted, g_fTiny, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX |
              DT_END_ELLIPSIS);
    }
}

/* ------------------------------------------------------------------ *
 *  the progress ring
 * ------------------------------------------------------------------ */
static void paint_ring(HDC dc, int cx, int cy, int rad, int thick, float pct,
                       int running)
{
    GpGraphics *g = gfx_begin(dc);
    COLORREF fill;
    int i;

    if (!g) {
        /* plain GDI fallback: a ring of dots, still readable */
        int n = 48, k;
        for (k = 0; k < n; k++) {
            double a = (double)k / n * 6.28318530717959 - 1.5707963267949;
            int on = ((double)k / n) <= (pct / 100.0);
            circle_fill(dc, cx + (int)(cos(a) * rad), cy + (int)(sin(a) * rad),
                        thick / 2, on ? P.brand : P.track, NOBORDER);
        }
        return;
    }

    fill = g_anyErr ? P.warn : (pct >= 99.5f ? P.good : P.brand);

    if (running) {
        int breathe = (int)(g_ringPulse * (float)S(2) + 0.5f);
        for (i = 3; i >= 1; i--) {
            COLORREF c = blend(P.surface, fill,
                               0.13f * (float)i * (0.5f + 0.5f * g_ringPulse));
            gfx_arc_stroke(g, cx, cy, rad + breathe / 2, 0.0f, 360.0f, c, 255,
                           (REAL)(thick + i * S(3)), 0);
        }
    }
    gfx_arc_stroke(g, cx, cy, rad, 0.0f, 360.0f,
                   blend(P.track, fill, running ? 0.10f * g_ringPulse : 0.0f),
                   255, (REAL)thick, 0);
    if (pct > 0.2f) {
        float end = (pct >= 99.5f) ? 359.9f : pct * 3.6f;
        gfx_arc_stroke(g, cx, cy, rad, -90.0f, -90.0f + end, fill, 255,
                       (REAL)thick, 1);
    }
    if (running) {
        float a0 = (float)((int)(g_ringSpin * 57.2957795) % 360);
        gfx_arc_stroke(g, cx, cy, rad, a0, a0 + 46.0f,
                       blend(fill, RGB(255, 255, 255), 0.35f), 255,
                       (REAL)(thick / 3 + S(2)), 1);
    }
    gfx_end(g);
}

/* a button with the same treatment everywhere: press offset, hover, ripple */
typedef struct {
    int    id;                 /* hit-test id, for hover/ripple lookup */
    RECT   r;
    const char *label;
    int    kind;               /* 0 primary, 1 secondary, 2 ghost */
    int    enabled;
    HFONT  font;
} BtnSpec;

static void paint_button(HDC dc, BtnSpec *b)
{
    int off = 0, bw, bh, idx;
    float hv, pr;
    COLORREF bo;
    int i;

    /* map the hit-test id to the animation slot */
    switch (b->id) {
        case WHAT_BTN_PRIMARY:   idx = 0; break;
        case WHAT_BTN_SECONDARY: idx = 1; break;
        case WHAT_BTN_GHOST:     idx = 2; break;
        default:                 idx = 3; break;
    }
    hv = b->enabled ? g_btnHov[idx] : 0.0f;
    if (b->id == WHAT_LINK_WINTR) hv = g_linkHov[0];      /* the About links */
    if (b->id == WHAT_LINK_AGPCS) hv = g_linkHov[1];
    pr = b->enabled ? g_btnPress[idx] : 0.0f;
    off = (int)(S(2) * pr + 0.5f);
    bw = b->r.right - b->r.left;
    bh = b->r.bottom - b->r.top - off;

    if (b->kind == 0) {
        COLORREF top = blend(P.brandHi, P.brandBright, hv * 0.6f);
        COLORREF bot = blend(P.brand, P.brandPress, pr);
        if (b->enabled && g_tStart && !g_finished) {
            /* a slow breath while the engine works: the button is the way to
             * the live log, so it should look awake without shouting */
            float br = 0.5f + 0.5f * (float)sin(g_frame * 0.055);
            top = blend(top, P.brandBright, br * 0.30f);
            bot = blend(bot, P.brand, br * 0.18f);
        }
        if (!b->enabled) { top = P.surfaceHi; bot = P.surfaceHi; }
        for (i = 5; i >= 1; i--) {
            COLORREF sc = blend(P.bg, P.shadow, (b->enabled ? 0.10f : 0.05f) * (float)(6 - i));
            rrect(dc, b->r.left - i + 1, b->r.top + off + i / 2, bw + i * 2, bh + i,
                  S(10) + i, sc, NOBORDER);
        }
        /* A brighter first colour in the button's own gradient replaces the
         * separate highlight line: that line was a two pixel rectangle drawn
         * above the corner radius, so it stuck out past both rounded ends -
         * exactly the stray line that was visible on "Run prep".  Baking the
         * light into the gradient follows the corner curvature and cannot
         * overhang anything. */
        top = blend(top, P.brandBright, b->enabled ? 0.34f : 0.0f);
        round_vgrad(dc, b->r.left, b->r.top + off, bw, bh, S(10), top, bot, NOBORDER);
        fx_ripple_paint(dc, b->id, b->r.left, b->r.top, bw, bh + off);
        dtext_center(dc, b->label, b->r.left, b->r.top + off, bw, bh,
                     b->enabled ? P.onBrand : P.muted, b->font);
        return;
    }

    bo = blend(P.line, P.lineHi, hv);
    if (b->kind == 2) bo = blend(P.line, P.brandLine, hv);
    card(dc, b->r.left, b->r.top + off, bw, bh,
         b->kind == 2 ? blend(P.surface, P.brandSoft, hv * 0.5f)
                      : blend(P.surface, P.surfaceHi, hv),
         bo, S(10));
    fx_ripple_paint(dc, b->id, b->r.left, b->r.top, bw, bh + off);
    if (b->kind == 2) {
        /* ghost buttons get a small tinted dot so they read as secondary */
        COLORREF dc2 = b->enabled ? P.brand : P.muted;
        dtext_center(dc, b->label, b->r.left + S(20), b->r.top + off, bw - S(30), bh,
                     b->enabled ? P.text : P.muted, b->font);
        circle_fill(dc, b->r.left + S(14), b->r.top + off + bh / 2, S(3), dc2, NOBORDER);
    } else {
        dtext_center(dc, b->label, b->r.left, b->r.top + off, bw, bh,
                     b->enabled ? P.text : P.muted, b->font);
    }
}

/* ------------------------------------------------------------------ *
 *  painting: home
 * ------------------------------------------------------------------ */
static void paint_home(HDC dc, Layout *L)
{
    int i, nsel = 0;
    char buf[220];

    paint_header(dc, L, "Refurbishment console",
                 "Prepare this laptop for the next user. Everything runs in the "
                 "background - you can keep this window open or come back later.");

    for (i = 0; i < NMOD; i++) if (g_selected & g_mods[i].bit) nsel++;

    for (i = 0; i < NMOD; i++) {
        RECT r = L->cards[i];
        int on = (g_selected & g_mods[i].bit) ? 1 : 0;
        int hov = (g_hoverModule == i);
        float hv = g_modHov[i];
        float pr = g_modPress[i];
        int lift = (int)(1.5f * hv + 0.5f);
        int w = r.right - r.left, h = r.bottom - r.top;
        int bx = r.left + S(18);
        int by = r.top + (h - S(22)) / 2 - lift;
        COLORREF bg, bord;

        bg   = blend(P.surface, P.surfaceHi, hv);
        bord = on ? blend(P.brandLine, P.brandPress, hv)
                  : blend(P.line, P.lineHi, hv);
        if (pr > 0.01f) {
            bg   = blend(bg, P.bg, pr * 0.22f);
            bord = blend(bord, P.brand, pr * 0.75f);
            bg   = blend(bg, P.brandSoft, pr * 0.18f);
        }

        if (hv > 0.02f || on) shadow_card(dc, r.left, r.top - lift, w, h, S(14));
        card(dc, r.left, r.top - lift, w, h, bg, bord, S(14));
        if (pr > 0.02f)
            gfx_round_stroke(dc, r.left, r.top - lift, w, h, S(14), P.brand,
                             (int)(pr * 210.0f), 2.0f);
        fx_ripple_paint(dc, i, r.left, r.top - lift, w, h);

        /* the rail that grows out of the left edge while hovered */
        if (hv > 0.02f) {
            int rh = (int)((h - S(20)) * (0.55f + 0.45f * hv) + 0.5f);
            int rw = (int)(S(3) * hv + 0.5f);
            rrect(dc, r.left + 1, r.top - lift + (h - rh) / 2, rw, rh, rw / 2 + 1,
                  blend(P.surface, P.brand, hv), NOBORDER);
        }
        if (on) {
            HRGN g2 = CreateRoundRectRgn(r.left + 1, r.top - lift + 1, r.right,
                                         r.top - lift + h - 6, S(12), S(12));
            int sy2 = SaveDC(dc);
            if (g2) {
                SelectClipRgn(dc, g2);
                rect_fill(dc, r.left + 1, r.top - lift + 1, w - 2, 1,
                          blend(bg, P.brandHi, 0.30f));
                RestoreDC(dc, sy2);
                DeleteObject(g2);
            }
        }

        /* checkbox */
        {
            float pop = g_modPop[i];
            int extra = (int)(S(3) * pop + 0.5f);
            int ox = bx - extra, oy = by - extra;
            int sz = S(22) + extra * 2;
            int rad = S(7) + extra;
            rrect(dc, ox, oy, sz, sz, rad, on ? P.brand : P.surfaceHi,
                  on ? P.brand : blend(P.line, P.lineHi, hv));
            if (on)
                check_mark(dc, ox + sz / 2, oy + sz / 2, S(5) + extra / 2,
                           P.onBrand, S(2) > 1 ? S(2) : 1);
        }

        /* title + description */
        {
            int tx = bx + S(34);
            int tw = w - S(160);
            if (L->twoCol) tw = w - S(150);
            dtext(dc, g_mods[i].title, tx, r.top - lift + S(11), tw, S(20),
                  P.text, g_fStep,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (h >= S(48))
                dtext(dc, g_mods[i].desc, tx, r.top - lift + h - S(24), tw, S(18),
                      on ? P.text2 : P.muted, g_fSmall,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        (void)hov;

        /* right side: state chip */
        {
            int st = (int)g_step[i];
            const char *s;
            COLORREF c, cbg, cbd;
            int cw, cxp, cyp;

            if (!g_tStart)            s = on ? "selected" : "off";
            else if (st < 0)          s = "skipped";
            else                      s = step_state_text(st);
            c = g_tStart ? step_state_color(st) : (on ? P.good : P.muted);
            if (!g_tStart) {
                cbg = on ? P.goodSoft : P.surfaceHi;
                cbd = on ? blend(P.goodSoft, P.good, 0.45f) : P.line;
            } else {
                switch (st) {
                    case STEP_OK:      cbg = P.goodSoft; cbd = blend(P.goodSoft, P.good, 0.45f); break;
                    case STEP_WARN:    cbg = P.warnSoft; cbd = P.warnLine; break;
                    case STEP_ERR:     cbg = P.badSoft;  cbd = blend(P.badSoft, P.bad, 0.45f); break;
                    case STEP_RUNNING: cbg = P.brandSoft; cbd = P.brandLine; break;
                    case STEP_SKIP:    cbg = P.surfaceHi; cbd = P.line; break;
                    default:           cbg = P.surfaceHi; cbd = P.line; break;
                }
            }
            cw = chip_w(dc, s, g_fTiny);
            cxp = r.right - cw - S(16);
            cyp = r.top - lift + (h - S(20)) / 2;
            chip(dc, cxp, cyp, s, c, cbg, cbd, g_fTiny);
            if (g_tStart && st == STEP_RUNNING)
                circle_fill(dc, cxp + S(9), cyp + S(10), S(3), P.brand, NOBORDER);
        }
    }

    /* ---- what the update stage is going to do ---------------------- *
     *  The eight steps are the replacement for the old single blocking
     *  COM call: an operator can see what will happen, in which order, and
     *  roughly how long each part takes before anything starts.          */
    if (L->planCard.right > L->planCard.left) {
        RECT c = L->planCard;
        int cw = c.right - c.left, chh = c.bottom - c.top;
        int pad = S(18);
        int colw = (cw - pad * 3) / 2;
        int on = (g_selected & PREP_M_UPDATES) ? 1 : 0;
        int i;

        card(dc, c.left, c.top, cw, chh, P.surface, P.line, S(16));
        dtext(dc, "WINDOWS UPDATE - THE EIGHT STEPS", c.left + pad, c.top + S(11),
              S(300), S(16), P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, on ? "in this order, cheapest path first"
                     : "switched off for this run",
              c.left + pad + S(250), c.top + S(11), cw - S(420), S(16),
              on ? P.text2 : P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        {
            const char *lbl = on ? "will run" : "off";
            int chipw = chip_w(dc, lbl, g_fTiny);
            chip(dc, c.right - chipw - pad, c.top + S(10), lbl,
                 on ? P.brand : P.muted, on ? P.brandSoft : P.surfaceHi,
                 on ? P.brandLine : P.line, g_fTiny);
        }

        if (on) {
            int rowsN = (WU_STEP_COUNT + 1) / 2;
            int avail = chh - S(34) - S(26);
            int rh = clampi(avail / (rowsN ? rowsN : 1), S(17), S(28));
            for (i = 0; i < WU_STEP_COUNT; i++) {
                int col = i / rowsN, row = i % rowsN;
                int rx = c.left + pad + col * (colw + pad);
                int ry = c.top + S(32) + row * rh;
                char tb[64];
                int bx;

                if (ry + rh > c.bottom - S(24)) break;
                bx = rx + S(9);
                rrect(dc, rx, ry + (rh - S(18)) / 2, S(18), S(18), S(6),
                      P.brandSoft, P.brandLine);
                snprintf(tb, sizeof(tb), "%d", i + 1);
                dtext_center(dc, tb, rx, ry + (rh - S(18)) / 2, S(18), S(18),
                             P.brand, g_fTiny);

                {
                    unsigned est = wu_step_default_est_ms(i);
                    char es[40];
                    if (i == WU_S_AGENT)      snprintf(es, sizeof(es), "5-45 min");
                    else if (i == WU_S_INSTALL) snprintf(es, sizeof(es), "5-30 min");
                    /* the orchestrator is the one step whose estimate is a
                     * watch, not a call: two seconds to ask, up to 25 minutes
                     * waiting for Windows to raise the restart flag.  Saying
                     * "~2 min" there made a healthy long install look stuck -
                     * measured on a real laptop: 28 minutes for the whole run. */
                    else if (i == WU_S_ORCH)  snprintf(es, sizeof(es), "2-35 min");
                    else if (est < 60000u)    snprintf(es, sizeof(es), "~%us", est / 1000u);
                    else                      snprintf(es, sizeof(es), "~%u min", est / 60000u);
                    dtext_right(dc, es, rx + colw - S(70), ry, S(70), rh, P.muted,
                                g_fTiny);
                }
                dtext(dc, wu_step_title(i), bx + S(18), ry, colw - S(110), rh,
                      P.text2, g_fSmall,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
            }
            if (chh >= S(140)) {
                dtext(dc, "Packages already on this disk are installed first, so a "
                          "laptop with no internet still gets its updates - and if "
                          "Windows never answers, the run says so instead of "
                          "pretending it is up to date.",
                      c.left + pad, c.bottom - S(22), cw - pad * 2, S(18), P.muted,
                      g_fTiny,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
            }
        } else {
            dtext(dc, "The Windows Update stage is not selected, so this laptop will "
                      "not be checked for updates in this run.",
                  c.left + pad, c.top + chh / 2 - S(10), cw - pad * 2, S(20),
                  P.muted, g_fBody,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    /* ---- the two note cards ---------------------------------------- */
    if (L->infoA.right > L->infoA.left) {
        RECT a = L->infoA;
        int gh = a.bottom - a.top;
        int py = a.top + S(12);
        const char *bl[4];
        int b;

        card(dc, a.left, a.top, a.right - a.left, gh, P.surface, P.line, S(14));
        dtext(dc, "Before you start", a.left + S(18), py, a.right - a.left - S(36),
              S(20), P.text, g_fTitle,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        bl[0] = "Administrator rights: TPM, DISM and Windows Update need elevation.";
        bl[1] = "The update stage installs what is already downloaded before it searches.";
        bl[2] = "Every wait has a time limit and the log shows a heartbeat every 15 seconds.";
        bl[3] = "The update step explains its verdict - it never reports success it cannot prove.";
        for (b = 0; b < 4; b++) {
            int by = py + S(26) + b * S(16);
            if (by + S(15) > a.bottom - S(6)) break;
            circle_fill(dc, a.left + S(23), by + S(7), S(2), P.brand, NOBORDER);
            dtext(dc, bl[b], a.left + S(32), by, a.right - a.left - S(48), S(15),
                  P.text2, g_fSmall,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        if (a.right - a.left > S(420) && gh > S(96)) {
            hline(dc, a.left + S(18), a.bottom - S(30), a.right - a.left - S(36), P.line);
            dtext(dc, "On a machine that has never been updated, the whole stage can "
                      "take 30-60 minutes. It runs in the background.",
                  a.left + S(18), a.bottom - S(26), a.right - a.left - S(36), S(18),
                  P.muted, g_fTiny,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    if (L->infoB.right > L->infoB.left) {
        RECT b = L->infoB;
        int gh = b.bottom - b.top;
        int py = b.top + S(12);
        int i;
        char v1[64], v2[64];
        const char *lb[3];
        const char *vv[3];

        card(dc, b.left, b.top, b.right - b.left, gh, P.surface, P.line, S(14));
        dtext(dc, "AT A GLANCE", b.left + S(18), py, b.right - b.left - S(36), S(14),
              P.muted, g_fTiny, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        snprintf(v1, sizeof(v1), "%d of %d", nsel, NMOD);
        fmt_elapsed(v2, sizeof(v2));
        lb[0] = "modules";     vv[0] = v1;
        lb[1] = "last run";    vv[1] = g_tStart ? v2 : "--";
        lb[2] = "log folder";  vv[2] = prep_log_dir();
        for (i = 0; i < 3; i++) {
            int ry = py + S(22) + i * S(18);
            if (ry + S(15) > b.bottom - S(6)) break;
            dtext(dc, lb[i], b.left + S(18), ry, S(84), S(15), P.muted, g_fTiny,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            dtext(dc, vv[i], b.left + S(104), ry, b.right - b.left - S(122), S(15),
                  i == 2 ? P.brand : P.text, i == 2 ? g_fTiny : g_fMono,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        }
        if (gh > S(120)) {
            hline(dc, b.left + S(18), b.bottom - S(30), b.right - b.left - S(36), P.line);
            dtext(dc, is_admin() ? "Elevated: the update layer can act."
                                 : "Not elevated: the update layer will refuse.",
                  b.left + S(18), b.bottom - S(26), b.right - b.left - S(36), S(18),
                  is_admin() ? P.good : P.warn, g_fTiny,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    /* ---- footer ---------------------------------------------------- */
    {
        int t1 = L->btnPrimary.top + S(2);
        int t2 = t1 + S(17);
        int tw = L->btnGhost.left - L->content.left - S(20);
        snprintf(buf, sizeof(buf), "%d of %d modules selected", nsel, NMOD);
        dtext(dc, buf, L->content.left, t1, tw, S(18), P.text2, g_fStep,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, g_tStart ? g_status : "Nothing happens until you press Run prep.",
              L->content.left, t2, tw, S(18), P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    {
        BtnSpec b;
        memset(&b, 0, sizeof(b));
        b.id = WHAT_BTN_GHOST; b.r = L->btnGhost; b.kind = 2; b.enabled = 1;
        b.label = "Open log folder"; b.font = g_fSmall;
        paint_button(dc, &b);

        b.id = WHAT_BTN_SECONDARY; b.r = L->btnSecondary; b.kind = 1; b.enabled = 1;
        b.label = (nsel == NMOD) ? "Clear all" : "Select all"; b.font = g_fBody;
        paint_button(dc, &b);

        b.id = WHAT_BTN_PRIMARY; b.r = L->btnPrimary; b.kind = 0;
        b.enabled = !g_runningNow();
        b.label = g_runningNow() ? "Running..." : "Run prep";
        b.font = g_fStep;
        paint_button(dc, &b);
    }
}

/* ------------------------------------------------------------------ *
 *  painting: run screen
 * ------------------------------------------------------------------ */
static COLORREF wu_step_color(int st)
{
    switch (st) {
        case STEP_RUNNING:  return P.brand;
        case STEP_OK:       return P.good;
        case STEP_WARN:     return P.warn;
        case STEP_ERR:      return P.bad;
        case STEP_SKIP:     return P.muted;
        default:            return P.track;
    }
}

static const char *wu_step_state_text(int st)
{
    switch (st) {
        case STEP_RUNNING:  return "working";
        case STEP_OK:       return "done";
        case STEP_WARN:     return "look";
        case STEP_ERR:      return "failed";
        case STEP_SKIP:     return "not needed";
        default:            return "queued";
    }
}

static void paint_run(HDC dc, Layout *L)
{
    int running = g_runningNow();
    int admin = is_admin();
    char buf[512], el[32];
    int showWu = (g_selected & PREP_M_UPDATES) ? 1 : 0;

    if (running) {
        paint_header(dc, L, "Prep in progress",
                     "The engine is working in the background - child processes "
                     "are hidden, the log is live on the Live log page.");
    } else if (g_finished) {
        paint_header(dc, L, g_anyErr ? "Run finished with something to fix"
                                     : "Run report",
                     "What happened on this laptop, and what to do next.");
    } else {
        paint_header(dc, L, "Run report",
                     "The last run on this machine. Open the log for the full detail.");
    }

    /* ---- summary card --------------------------------------------- */
    {
        RECT c = L->summary;
        int cw = c.right - c.left, chh = c.bottom - c.top;
        int rrad = clampi(chh / 3, S(24), S(34));
        int cx = c.left + S(22) + rrad;
        int cy = c.top + chh / 2;
        int pct = (int)(g_ringPct + 0.5f);
        int tx = cx + rrad + S(22);
        /* the restart banner carries its own buttons, so the card's action
         * button disappears with it and the headline gets that width back */
        int actW = (!g_restartOffered && L->btnAction.right > L->btnAction.left)
                   ? S(220) : S(24);
        int tw = cw - (tx - c.left) - actW;

        card(dc, c.left, c.top, cw, chh, P.surface, P.line, S(16));
        paint_ring(dc, cx, cy, rrad, S(9), g_ringPct, running);

        if (g_finished && g_ringPct > 99.0f) {
            check_mark(dc, cx, cy, S(10), g_anyErr ? P.warn : P.good, S(3) > 1 ? S(3) : 2);
        } else {
            snprintf(buf, sizeof(buf), "%d%%", pct);
            dtext_center(dc, buf, cx - rrad, cy - S(14), rrad * 2, S(28),
                         P.text, g_fNum);
        }

        /* headline: the verdict when the stage is over, the live step while
         * it runs - never a vague "working..." */
        {
            const char *head;
            int a = (int)g_active;
            if (running) {
                if (a >= 0 && g_mods[a].bit == PREP_M_UPDATES) {
                    int cur = prep_update_current_step();
                    head = (cur >= 0) ? prep_update_step_title(cur) : g_mods[a].title;
                } else if (a >= 0) {
                    head = g_mods[a].title;
                } else {
                    head = "Starting up";
                }
            } else if (g_finished) {
                const char *vt = prep_update_verdict_text();
                head = (vt && vt[0] && showWu) ? vt
                                               : (g_anyErr ? "Finished with issues"
                                                           : "All selected modules finished");
            } else {
                head = "Nothing has run yet";
            }
        dtext(dc, head, tx, c.top + S(14), tw, S(22), P.text, g_fStep,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        /* second line: the live detail + elapsed / countdown */
        {
            int a = (int)g_active;
            fmt_elapsed(el, sizeof(el));
            if (running) {
                int cur = prep_update_current_step();
                if (a >= 0 && g_mods[a].bit == PREP_M_UPDATES && cur >= 0) {
                    long elms = prep_update_step_elapsed_ms(cur);
                    unsigned est = prep_update_step_est_ms(cur);
                    long left = (long)est - elms;
                    char eta[32], done[32];
                    fmt_mmss(done, sizeof(done), elms);
                    fmt_mmss(eta, sizeof(eta), left);
                    snprintf(buf, sizeof(buf),
                             "step %d of %d - %s in this step%s%s",
                             cur + 1, prep_update_step_count(), done,
                             left > 0 ? ", about " : "",
                             left > 0 ? eta : "");
                } else {
                    snprintf(buf, sizeof(buf), "working for %s", el);
                }
            } else if (g_finished) {
                snprintf(buf, sizeof(buf), "elapsed %s", el);
            } else {
                snprintf(buf, sizeof(buf), "Press Run prep to start");
            }
            if (g_finished) {
                int i, done = 0, sel = 0;
                for (i = 0; i < NMOD; i++) {
                    if (!(g_selected & g_mods[i].bit)) continue;
                    sel++;
                    if (g_step[i] != STEP_RUNNING && g_step[i] != -1) done++;
                }
                snprintf(buf, sizeof(buf), "elapsed %s - %d of %d modules ran", el, done, sel);
            }
            dtext(dc, buf, tx, c.top + S(40), tw, S(20), P.text2, g_fSmall,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        /* third line: advice, or the heartbeat while it works */
        {
            const char *adv = NULL;
            if (running) {
                int hb = prep_update_heartbeat_age_ms();
                if (active_bit_is_updates() && hb > 8000)
                    snprintf(buf, sizeof(buf),
                             "last sign of life from the update layer: %d s ago "
                             "(a heartbeat is written every 15 s)", hb / 1000);
                else
                    snprintf(buf, sizeof(buf),
                             "The log records every step - open Live log to watch it");
                adv = buf;
            } else if (g_finished && showWu) {
                adv = prep_update_advice_text();
            } else if (!g_finished) {
                adv = "The update stage installs what is already downloaded first.";
            }
            if (adv && adv[0])
                dtext(dc, adv, tx, c.top + S(64), tw + S(60), S(34), P.muted, g_fTiny,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }

        /* a slim step progress bar for the update stage */
        if (showWu && (running || g_finished)) {
            int done = prep_update_done_steps();
            int tot = prep_update_step_count();
            int bx = tx, bw = tw + S(60), by = c.bottom - S(24);
            int filled = (int)((float)bw * (tot ? (float)done / tot : 0.0f) + 0.5f);
            if (bw > S(80)) {
                rrect(dc, bx, by, bw, S(5), S(3), P.track, NOBORDER);
                if (filled > 0)
                    rrect(dc, bx, by, filled, S(5), S(3),
                          g_anyErr ? P.warn : P.brand, NOBORDER);
            }
        }

        /* the context action button (the restart banner replaces it) */
        if (!running && !g_restartOffered && L->btnAction.right > L->btnAction.left) {
            const char *lbl = NULL;
            int sev = prep_update_severity();
            int verdict = prep_update_verdict();
            BtnSpec b;
            if (g_restartPending && restart_seconds_left() > 0) lbl = "Cancel restart";
            else if (g_restartPending)                         lbl = "Restart now";
            else if (g_finished && showWu && verdict != WU_J_NONE) {
                if (verdict == WU_J_SUCCESS_RESTART) lbl = "Upload + restart";
                else if (verdict == WU_J_NEEDS_ELEVATION) lbl = "Restart as admin";
                else if (verdict == WU_J_WEDGED) lbl = "Restart laptop";
                else if (verdict == WU_J_UNFINISHED || verdict == WU_J_BLOCKED) lbl = "Fix update engine";
                else if (verdict == WU_J_OFFLINE) lbl = "Open network settings";
                else if (verdict == WU_J_ERROR) lbl = "Open Windows Update";
            }
            if (lbl) {
                memset(&b, 0, sizeof(b));
                b.id = WHAT_BTN_WU_ACTION;
                b.r = L->btnAction;
                b.kind = 2;
                b.enabled = 1;
                b.label = lbl;
                b.font = g_fTiny;
                paint_button(dc, &b);
                if (sev >= 3) {
                    int cw2 = chip_w(dc, "needs attention", g_fTiny);
                    chip(dc, L->btnAction.left - cw2 - S(10),
                         L->btnAction.top + (L->btnAction.bottom - L->btnAction.top - S(20)) / 2,
                         "needs attention", P.bad, P.badSoft,
                         blend(P.badSoft, P.bad, 0.45f), g_fTiny);
                }
            }
        }
    }

    /* ---- the update step list -------------------------------------- */
    {
        RECT c = L->stepsCard;
        int cw = c.right - c.left, chh = c.bottom - c.top;
        int i;

        if (chh < S(80)) return;
        card(dc, c.left, c.top, cw, chh, P.surface, P.line, S(16));

        dtext(dc, showWu ? "WINDOWS UPDATE - THE EIGHT STEPS"
                         : "SELECTED MODULES",
              c.left + S(18), c.top + S(10), cw - S(140), S(16), P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        if (showWu) {
            char right[128];
            int done = prep_update_done_steps();
            snprintf(right, sizeof(right), "%d of %d resolved", done,
                     prep_update_step_count());
            dtext_right(dc, right, c.left + cw - S(200), c.top + S(10), S(182), S(16),
                        P.muted, g_fTiny);
        }

        for (i = 0; i < WU_STEP_COUNT; i++) {
            RECT r = L->stepRows[i];
            int st, e, roff;
            float ease = g_wuStepEase[i], fl = g_wuRowFlash[i], ri, rf;
            int ew, icx, icy, chw;
            COLORREF sc, sbg, sbd;

            if (!showWu) break;
            if (r.bottom > c.bottom - S(6)) break;
            st = g_tStart ? prep_update_step_state(i) : STEP_QUEUED;

            /* the rows cascade in when the run screen opens: each one is a
             * little behind the one above it, and slides up as it fades in */
            ri = (g_runIn - (float)i * 0.045f) / 0.45f;
            if (ri < 0.0f) ri = 0.0f;
            if (ri > 1.0f) ri = 1.0f;
            rf = ease_out(ri);
            roff = (int)((1.0f - rf) * S(12));
            r.top += roff; r.bottom += roff;
            if (rf < 0.02f) continue;

            e = (int)((1.0f - ease_out(ease)) * S(14));
            ew = r.right - r.left - S(10) - e;

            /* the active row gets a tinted strip; others stay flat */
            if (st == STEP_RUNNING) {
                rrect(dc, r.left - S(6), r.top + S(2), r.right - r.left + S(12),
                      r.bottom - r.top - S(4), S(8), P.brandSoft, NOBORDER);
                rrect(dc, r.left - S(6), r.top + S(2), S(3), r.bottom - r.top - S(4),
                      S(2), P.brand, NOBORDER);
                /* a soft sheen travels along the working row, so a step that
                 * takes ten minutes still looks alive - drawn as one opaque
                 * pill, never a translucent band (see the GDI+ note above) */
                {
                    int rh = r.bottom - r.top - S(4);
                    int pw = S(84), span = r.right - r.left + S(12) + pw;
                    int px = r.left - S(6) + (int)fmod((double)(g_frame * 3.4),
                                                       (double)span);
                    int pill = rh;
                    int l = px - pill, w2 = pill * 2;
                    int lo = r.left - S(6), hi = r.right + S(6);
                    if (l < lo) l = lo;
                    if (l + w2 > hi) w2 = hi - l;
                    if (w2 > S(10))
                        rrect(dc, l, r.top + S(4), w2, rh, rh / 2,
                              blend(P.brandSoft, P.brand, 0.22f), NOBORDER);
                }
            } else if (fl > 0.01f) {
                rrect(dc, r.left - S(6), r.top + S(2), r.right - r.left + S(12),
                      r.bottom - r.top - S(4), S(8),
                      blend(P.surface, wu_step_color(st), fl * 0.16f), NOBORDER);
            }

            /* state badge */
            icx = r.left + S(9) + e;
            icy = r.top + (r.bottom - r.top) / 2;
            sc  = blend(P.surface, wu_step_color(st), rf);
            sbg = (st == STEP_OK)   ? P.goodSoft   : (st == STEP_WARN) ? P.warnSoft
                : (st == STEP_ERR)  ? P.badSoft    : (st == STEP_RUNNING) ? P.brandSoft
                : P.surfaceHi;
            sbd = (st == STEP_QUEUED || st == STEP_SKIP) ? P.line
                : blend(sbg, sc, 0.45f);
            sbg = blend(P.surface, sbg, rf);
            sbd = blend(P.surface, sbd, rf);
            rrect(dc, icx - S(9), icy - S(9), S(18), S(18), S(6), sbg, sbd);
            if (st == STEP_OK) {
                check_mark_progress(dc, icx, icy, S(5), sc, S(2) > 1 ? S(2) : 1,
                                    g_wuCheck[i]);
            } else if (st == STEP_ERR) {
                stroke_line(dc, icx - S(4), icy - S(4), icx + S(4), icy + S(4), sc, S(2) > 1 ? S(2) : 1);
                stroke_line(dc, icx + S(4), icy - S(4), icx - S(4), icy + S(4), sc, S(2) > 1 ? S(2) : 1);
            } else if (st == STEP_WARN) {
                rect_fill(dc, icx, icy - S(5), S(2) > 0 ? S(2) : 1, S(6), sc);
                rect_fill(dc, icx, icy + S(3), S(2) > 0 ? S(2) : 1, S(2), sc);
            } else if (st == STEP_RUNNING) {
                int rr = S(3) + (int)(S(2) * g_ringPulse);
                circle_fill(dc, icx, icy, rr, sc, NOBORDER);
            } else if (st == STEP_SKIP) {
                rect_fill(dc, icx - S(4), icy, S(9), S(2) > 0 ? S(2) : 1, P.muted);
            } else {
                snprintf(buf, sizeof(buf), "%d", i + 1);
                dtext_center(dc, buf, icx - S(9), icy - S(8), S(18), S(16),
                             P.muted, g_fTiny);
            }

            /* title + note */
            chw = chip_w(dc, wu_step_state_text(st), g_fTiny);
            {
                int textX = icx + S(16);
                int textW = r.left + ew - chw - S(14) - textX;
                int hasNote = (r.bottom - r.top) >= S(36);
                COLORREF tc = (st == STEP_SKIP) ? P.muted : P.text;
                if (st == STEP_RUNNING) tc = P.text;
                tc = blend(P.surface, tc, rf);
                dtext(dc, prep_update_step_title(i), textX,
                      hasNote ? r.top + S(3) : r.top, textW, S(20), tc, g_fStep,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
                if (hasNote) {
                    const char *note = prep_update_step_note(i);
                    char nb[220];
                    long elms = prep_update_step_elapsed_ms(i);
                    if (st == STEP_ERR && i == WU_S_INSTALL && note[0]) {
                        /* name the code and the fix right on the row: this is
                         * the row a technician actually has to act on */
                        snprintf(nb, sizeof(nb), "%s", note);
                        note = nb;
                    } else if (st == STEP_RUNNING && i == WU_S_ORCH) {
                        char t1[32];
                        fmt_mmss(t1, sizeof(t1), elms);
                        snprintf(nb, sizeof(nb),
                                 "waiting for Windows to install - %s so far; it "
                                 "stops early when nothing is queued", t1);
                        note = nb;
                    } else if (st == STEP_RUNNING && elms > 2000) {
                        char t1[32];
                        fmt_mmss(t1, sizeof(t1), elms);
                        snprintf(nb, sizeof(nb), "%s - %s so far", note, t1);
                        note = nb;
                    } else if (st == STEP_SKIP) {
                        snprintf(nb, sizeof(nb), "%s - not needed on this machine", note);
                        note = nb;
                    }
                    dtext(dc, note, textX, r.top + S(19), textW, S(16),
                          blend(P.surface, P.muted, rf), g_fTiny,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX |
                          DT_END_ELLIPSIS);
                }
            }

            /* state chip + countdown/estimate on the right */
            {
                int cyp = r.top + (r.bottom - r.top - S(20)) / 2;
                int cxp = r.left + ew - chw;
                if (cxp > r.left + S(60)) {
                    chip(dc, cxp, cyp, wu_step_state_text(st), sc, sbg, sbd, g_fTiny);
                    if (st == STEP_QUEUED || st == STEP_RUNNING) {
                        char tb[48];
                        unsigned est = prep_update_step_est_ms(i);
                        if (st == STEP_RUNNING) {
                            long left = (long)est - prep_update_step_elapsed_ms(i);
                            char t2[32];
                            fmt_mmss(t2, sizeof(t2), left > 0 ? left : 0);
                            snprintf(tb, sizeof(tb), "about %s left", t2);
                        } else {
                            char t2[32];
                            fmt_mmss(t2, sizeof(t2), (long)est);
                            snprintf(tb, sizeof(tb), "about %s", t2);
                        }
                        dtext_right(dc, tb, cxp - S(150), cyp, S(140), S(20),
                                    st == STEP_RUNNING ? P.brand : P.muted, g_fTiny);
                    }
                }
            }
        }

        /* if there is room, a line under the steps explains the countdown */
        if (chh > S(80) + WU_STEP_COUNT * S(30)) {
            const char *foot = "The times on the right are estimates: a local read "
                               "takes seconds, an online search can take 45 minutes.";
            dtext(dc, foot, c.left + S(18), c.bottom - S(22), cw - S(36), S(16),
                  P.muted, g_fTiny,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }

    /* ---- footer buttons -------------------------------------------- */
    {
        BtnSpec b;
        memset(&b, 0, sizeof(b));

        b.id = WHAT_BTN_GHOST; b.r = L->btnGhost; b.kind = 2; b.enabled = 1;
        b.label = "Open log folder"; b.font = g_fSmall;
        paint_button(dc, &b);

        b.id = WHAT_BTN_SECONDARY; b.r = L->btnSecondary; b.kind = 1;
        b.enabled = !running;
        b.label = "Run again"; b.font = g_fBody;
        paint_button(dc, &b);

        b.id = WHAT_BTN_PRIMARY; b.r = L->btnPrimary; b.kind = 0; b.enabled = 1;
        b.label = running ? "Watch live log" : "Open log page";
        b.font = g_fStep;
        paint_button(dc, &b);
    }

    (void)admin;
}

/* ------------------------------------------------------------------ *
 *  painting: log
 * ------------------------------------------------------------------ */
static void paint_log(HDC dc, Layout *L)
{
    RECT a = L->logArea;
    int pad = S(14), lh = S(17);
    int total, vis, first, i, y, w, tsW;
    char buf[MAX_PATH + 128];
    SIZE tsz;

    paint_header(dc, L, "Live log", "Streamed from the log file while the prep runs.");

    for (i = 0; i < 3; i++) {
        const char *cap[3];
        RECT r;
        float hv;
        cap[0] = "Copy all";
        cap[1] = "Open folder";
        cap[2] = g_logFollow ? "Follow: on" : "Follow: off";
        r = L->btnLog[i];
        hv = flerp(g_logBtnHov[i], (g_hoverBtn == 300 + i) ? 1.0f : 0.0f, 0.28f);
        g_logBtnHov[i] = hv;
        rrect(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, S(8),
              blend(P.surface, P.surfaceHi, hv),
              blend(P.line, P.brandPress, hv * 0.5f));
        dtext_center(dc, cap[i], r.left, r.top, r.right - r.left, r.bottom - r.top,
                     i == 2 && g_logFollow ? P.good : P.text, g_fTiny);
    }

    card(dc, a.left, a.top, a.right - a.left, a.bottom - a.top, P.logbg, P.line, S(16));

    {
        int run = (g_tStart && !g_finished);
        const char *lbl = run ? "live" : "idle";
        int cw2 = chip_w(dc, lbl, g_fTiny);
        int cxp = L->btnLog[0].left - cw2 - S(14);
        int cyp = L->btnLog[0].top + ((L->btnLog[0].bottom - L->btnLog[0].top) - S(20)) / 2;
        chip(dc, cxp, cyp, lbl, run ? P.brand : P.muted,
             run ? P.brandSoft : P.surface, run ? P.brandLine : P.line, g_fTiny);
        if (run && g_ringPulse > 0.5f)
            circle_fill(dc, cxp - S(9), cyp + S(10), S(3), P.brand, NOBORDER);
    }

    total = log_line_count();
    vis = (a.bottom - a.top - pad * 2) / lh;
    if (vis < 1) vis = 1;
    g_logMaxScroll = (total > vis) ? (total - vis) : 0;
    if (!g_autoScroll) g_logFollow = 0;
    if (g_logFollow) {
        /* follow = stay on the newest line, which is scroll 0 here (the
         * old code pinned the view to the *top* of the log and then kept
         * it there while new lines arrived below) */
        g_logScroll = 0;
        g_scrollAnim = 0.0f;
    }
    if (g_logScroll > g_logMaxScroll) g_logScroll = g_logMaxScroll;
    if (g_logScroll < 0) g_logScroll = 0;
    {
        float overshoot = S(16);
        float sa = g_scrollAnim;
        if (sa > (float)g_logMaxScroll + overshoot) sa = (float)g_logMaxScroll + overshoot;
        if (sa < -overshoot) sa = -overshoot;
        g_scrollAnim = sa;
    }
    {
        int firstF = (int)((float)g_logMaxScroll - g_scrollAnim + 0.5f);
        if (firstF < 0) firstF = 0;
        first = firstF;
    }

    if (total == 0) {
        dtext_center(dc, "Waiting for the first log line...",
                     a.left, a.top, a.right - a.left, a.bottom - a.top, P.muted, g_fBody);
    }

    SelectObject(dc, g_fMono);
    GetTextExtentPoint32A(dc, "0000-00-00 00:00:00", 19, &tsz);
    tsW = tsz.cx + S(14);

    for (i = 0; i < vis; i++) {
        int idx = first + i;
        size_t off, len, msgOff;
        COLORREF col;
        RECT r;
        float born = 1.0f;
        if (idx < 0 || idx >= total) continue;
        off = log_line_off(idx);
        len = log_line_len(idx);
        y = a.top + pad + i * lh;

        if (g_lineBorn && idx < g_lineBornCap) {
            DWORD age = GetTickCount() - g_lineBorn[idx];
            born = (age >= 320) ? 1.0f : (float)age / 320.0f;
            if (born < 0) born = 0;
        }

        if (log_line_has_stamp(g_logBuf + off, len)) {
            r.left = a.left + pad; r.top = y; r.right = r.left + tsW; r.bottom = y + lh;
            SetTextColor(dc, blend(P.logbg, P.logDim, born));
            SetBkMode(dc, TRANSPARENT);
            DrawTextA(dc, g_logBuf + off, 19, &r,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            msgOff = 22;
        } else {
            msgOff = 0;
        }

        w = a.right - S(24) - (a.left + pad + (msgOff ? tsW : 0));
        if (w < S(40)) w = S(40);
        r.left = a.left + pad + (msgOff ? tsW : 0);
        r.top = y; r.right = r.left + w; r.bottom = y + lh;
        col = log_msg_color(g_logBuf + off + msgOff, len - msgOff);
        col = blend(P.logbg, col, born);
        SetTextColor(dc, col);
        DrawTextA(dc, g_logBuf + off + msgOff, (int)(len - msgOff), &r,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    /* slim scroll indicator */
    if (g_logMaxScroll > 0) {
        int trackX = a.right - S(9);
        int trackT = a.top + S(10), trackB = a.bottom - S(10);
        int trackH = trackB - trackT;
        int thumbH = trackH * vis / total;
        int thumbY;
        if (thumbH < S(24)) thumbH = S(24);
        thumbY = trackT + (int)((float)(trackH - thumbH) *
                                ((float)g_logMaxScroll - g_scrollAnim) / (float)g_logMaxScroll);
        rrect(dc, trackX, trackT, S(4), trackH, S(2), P.surfaceHi, NOBORDER);
        rrect(dc, trackX, thumbY, S(4), thumbH, S(2), P.muted, NOBORDER);
    }

    /* the legend: which colour means what, so the log can be skimmed */
    {
        int y2 = L->content.bottom - S(26);
        int x2 = L->content.left;
        dtext(dc, "UPD:", x2, y2, S(30), S(18), P.info, g_fTiny,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, "update stage", x2 + S(30), y2, S(80), S(18), P.muted, g_fTiny,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, "red", x2 + S(120), y2, S(28), S(18), P.bad, g_fTiny,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, "failed", x2 + S(148), y2, S(50), S(18), P.muted, g_fTiny,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, "amber", x2 + S(206), y2, S(44), S(18), P.warn, g_fTiny,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        dtext(dc, "skipped / timed out", x2 + S(250), y2, S(150), S(18), P.muted,
              g_fTiny, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    snprintf(buf, sizeof(buf), "Log file: %s   -   %d lines", prep_log_path(), total);
    dtext(dc, buf, L->content.left, L->content.bottom - S(8) - S(16),
          L->cw - S(8), S(16), P.muted, g_fTiny,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    dtext_right(dc, "wheel scrolls  -  Ctrl+C copies everything",
                L->content.right - S(420), L->content.bottom - S(8) - S(16), S(420), S(16),
                P.muted, g_fTiny);
}

/* ------------------------------------------------------------------ *
 *  painting: settings
 * ------------------------------------------------------------------ */
typedef struct {
    const char *title;
    const char *desc;
    int         kind;   /* 0 toggle, 1 action, 2 editable text */
    int         id;
} SettingRowDef;

/* One table, used by the painter *and* the hit test, so a click can never
 * land on a different row than the one that was drawn. */
static const SettingRowDef g_settingRows[] = {
    { "Auto-follow the live log while a run is in progress",
      "The log keeps scrolling to the newest line unless you scroll up", 0, 200 },
    { "Minimise the window when a run starts",
      "Useful when the laptop is left alone to work through the updates", 0, 201 },
    { "Open the log folder",
      "C:\\RefurbLogs holds the log, the device CSV and the battery report", 1, 202 },
    { "Theme",
      "Stored in HKCU; follows Windows until you choose here", 0, 203 },
    { "Upload the log that is on disk right now",
      "For after a reboot, when the run is finished but the upload is unsure", 1, 204 },
    { "Log upload server",
      "Click to type the address, Enter to save - it is written to LaptopPrep.cfg "
      "next to the console so every laptop can share it", 2, 207 },
    { "Fix a stuck Windows Update (runs fix-windows-update.bat as admin)",
      "Restarts the update services and clears a wedged session - keeps downloads", 1, 205 },
    { "Open Settings > Windows Update",
      "Hand the machine to the built-in updater when a person should look", 1, 206 },
};
#define N_SETTING_ROWS ((int)(sizeof(g_settingRows) / sizeof(g_settingRows[0])))

static int setting_row_index_for_id(int id)
{
    int i;
    for (i = 0; i < N_SETTING_ROWS; i++)
        if (g_settingRows[i].id == id) return i;
    return -1;
}

static int setting_row_on(int id)
{
    if (id == 200) return g_autoScroll;
    if (id == 201) return g_minimizeOnRun;
    if (id == 203) return g_themeDark;
    return 0;
}

static int setting_row_rect(Layout *L, int i, RECT *r)
{
    int y = L->content.top + S(96);
    r->left = L->content.left;
    r->top = y + i * (S(48) + S(8));
    r->right = L->content.left + L->cw;
    r->bottom = r->top + S(48);
    return (r->bottom <= L->content.bottom - S(150));
}

static void paint_settings(HDC dc, Layout *L)
{
    int w = L->cw;
    int i, nrows = N_SETTING_ROWS;
    char buildline[128];

    paint_header(dc, L, "Settings", "Small quality-of-life options for this console.");

    for (i = 0; i < nrows; i++) {
        RECT r;
        int hov, rowH = S(48);
        const SettingRowDef *row = &g_settingRows[i];
        if (!setting_row_rect(L, i, &r)) { nrows = i; break; }
        hov = (g_hoverModule == row->id);
        card(dc, r.left, r.top + (int)(g_rowPress[i] * S(1)), r.right - r.left, r.bottom - r.top,
             g_rowPress[i] > 0.01f ? blend(hov ? P.surfaceHi : P.surface, P.brandSoft,
                                           g_rowPress[i] * 0.55f)
                                   : (hov ? P.surfaceHi : P.surface),
             blend(P.line, hov ? P.brandLine : P.lineHi, hov ? 1.0f : 0.0f), S(12));
        if (hov) shadow_card(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, S(12));
        fx_ripple_paint(dc, row->id, r.left, r.top, r.right - r.left, rowH);

        if (row->kind == 0) {
            int on = setting_row_on(row->id);
            rrect(dc, r.left + S(18), r.top + (rowH - S(22)) / 2, S(22), S(22), S(7),
                  on ? P.brand : P.surfaceHi, on ? P.brand : P.line);
            if (on) check_mark(dc, r.left + S(29), r.top + rowH / 2, S(5), P.onBrand,
                               S(2) > 1 ? S(2) : 1);
        } else if (row->kind == 2) {
            /* a pen glyph: this row takes typing, not a click-and-run */
            int cx = r.left + S(29), cy = r.top + rowH / 2;
            circle_fill(dc, cx, cy, S(11), P.brandSoft, P.brandLine);
            stroke_line(dc, cx - S(4), cy + S(4), cx + S(2), cy - S(2), P.brand, S(2) > 1 ? S(2) : 1);
            stroke_line(dc, cx + S(2), cy - S(2), cx + S(4), cy - S(4), P.brand, S(2) > 1 ? S(2) : 1);
            stroke_line(dc, cx - S(5), cy + S(5), cx - S(2), cy + S(4), P.brand, S(2) > 1 ? S(2) : 1);
        } else {
            /* action rows get an arrow glyph */
            int cx = r.left + S(29), cy = r.top + rowH / 2;
            circle_fill(dc, cx, cy, S(11), P.brandSoft, P.brandLine);
            stroke_line(dc, cx - S(3), cy, cx + S(4), cy, P.brand, S(2) > 1 ? S(2) : 1);
            stroke_line(dc, cx + S(1), cy - S(4), cx + S(4), cy, P.brand, S(2) > 1 ? S(2) : 1);
            stroke_line(dc, cx + S(1), cy + S(4), cx + S(4), cy, P.brand, S(2) > 1 ? S(2) : 1);
        }

        dtext(dc, row->title, r.left + S(52), r.top + S(8), w - S(220), S(20),
              P.text, g_fBody,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (row->kind == 2) {
            if (g_editRow == row->id) {
                /* the value being typed, with a caret that blinks */
                int show = ((GetTickCount() / 500) % 2) == 0;
                char shown[560];
                snprintf(shown, sizeof(shown), "%s%s", g_editBuf, show ? "|" : " ");
                dtext(dc, shown, r.left + S(52), r.top + S(27), w - S(220), S(16),
                      P.brand, g_fTiny,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            } else {
                char shown[560];
                snprintf(shown, sizeof(shown), "%s   (from %s)",
                         prep_log_url(), prep_log_url_source());
                dtext(dc, shown, r.left + S(52), r.top + S(27), w - S(220), S(16),
                      prep_url_is_placeholder_ex() ? P.warn : P.muted, g_fTiny,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        } else {
            dtext(dc, row->desc, r.left + S(52), r.top + S(27), w - S(220), S(16),
                  P.muted, g_fTiny,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        {
            const char *st;
            int okchip;
            int cw;
            if (row->kind == 0) {
                st = setting_row_on(row->id) ? "on" : "off";
                okchip = setting_row_on(row->id);
            } else if (row->kind == 2) {
                if (g_editRow == row->id)      { st = "enter saves"; okchip = 1; }
                else if (prep_url_is_placeholder_ex()) { st = "set address"; okchip = 0; }
                else                            { st = "change"; okchip = 1; }
            } else {
                st = (row->id == 204 && !prep_url_is_placeholder_ex()) ? "send" : "run";
                okchip = (row->id != 204) || !prep_url_is_placeholder_ex();
            }
            cw = chip_w(dc, st, g_fTiny);
            chip(dc, r.right - cw - S(16), r.top + (rowH - S(20)) / 2, st,
                 okchip ? P.brand : P.muted,
                 okchip ? P.brandSoft : P.surfaceHi,
                 okchip ? P.brandLine : P.line, g_fTiny);
        }
    }
    (void)nrows;

    /* ---- paths ------------------------------------------------------ */
    {
        const char *lab[7];
        char val[7][MAX_PATH + 64];
        int k, lw = S(130), cardH;
        int x = L->content.left;
        int y = L->content.top + S(96) + nrows * (S(48) + S(8)) + S(6);

        lab[0] = "Log directory";   snprintf(val[0], sizeof(val[0]), "%s", prep_log_dir());
        lab[1] = "Log file";        snprintf(val[1], sizeof(val[1]), "%s", prep_log_path());
        lab[2] = "Device CSV";      snprintf(val[2], sizeof(val[2]), "%s\\<COMPUTERNAME>_deviceID.csv", prep_log_dir());
        lab[3] = "Battery report";  snprintf(val[3], sizeof(val[3]), "%s\\battery_report.html", prep_log_dir());
        lab[4] = "Upload endpoint"; snprintf(val[4], sizeof(val[4]), "%s",
                     prep_url_is_placeholder_ex() ? "(not configured - uploads are skipped)"
                                                  : prep_log_url_text());
        lab[5] = "Engine modules";  snprintf(val[5], sizeof(val[5]), "%u", prep_module_count());
        snprintf(buildline, sizeof(buildline), "%s  -  %s theme  -  engine %s",
                 PREP_VERSION_STR, g_themeDark ? "dark" : "light",
                 is_admin() ? "elevated" : "not elevated");
        lab[6] = "Build";           snprintf(val[6], sizeof(val[6]), "%s", buildline);

        cardH = S(30) + 7 * S(20) + S(12);
        if (y + cardH > L->content.bottom) {
            cardH = L->content.bottom - y;
            if (cardH < S(60)) return;
        }
        card(dc, x, y, w, cardH, P.surface, P.line, S(14));
        dtext(dc, "PATHS AND BUILD", x + S(18), y + S(9), S(200), S(16), P.muted,
              g_fTiny, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        for (k = 0; k < 7; k++) {
            int ry = y + S(30) + k * S(20);
            if (ry + S(18) > y + cardH - S(4)) break;
            dtext(dc, lab[k], x + S(18), ry, lw, S(18), P.muted, g_fSmall,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            dtext(dc, val[k], x + S(18) + lw, ry, w - lw - S(36), S(18), P.text2, g_fMono,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  painting: about
 * ------------------------------------------------------------------ */
static void paint_about(HDC dc, Layout *L)
{
    int x = L->content.left, y = L->content.top + S(96);
    int w = L->cw;
    char buf[1400];

    paint_header(dc, L, "About", "LaptopPrep - refurbishment helper for Windows laptops.");

    if (g_bmpLogoBig)
        draw_logo(dc, g_bmpLogoBig, g_logoBigW, g_logoBigH, x, y + S(4), S(52));
    else
        paint_brand(dc, x, y, S(58));

    dtext(dc, "LaptopPrep", x, y + S(76), w, S(30), P.text, g_fH1,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    snprintf(buf, sizeof(buf), "Version %s  -  Win32 / Win64  -  last run exit code %d  -  %u modules",
             PREP_VERSION_STR, g_runRc, prep_module_count());
    dtext(dc, buf, x, y + S(108), w, S(20), P.text2, g_fSmall,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    /* ---- who owns this, with the two home pages as real links ------ */
    {
        BtnSpec b;
        dtext(dc, PREP_COPYRIGHT, L->creditTitle.left, L->creditTitle.top,
              L->creditTitle.right - L->creditTitle.left, S(20), P.text, g_fStep,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        memset(&b, 0, sizeof(b));
        b.kind = 1; b.enabled = 1; b.font = g_fSmall;

        b.id = WHAT_LINK_WINTR; b.r = L->linkWintr;
        b.label = PREP_URL_WINTR_SH;
        paint_button(dc, &b);

        b.id = WHAT_LINK_AGPCS; b.r = L->linkAgpcs;
        b.label = PREP_URL_AGPCS_SH;
        paint_button(dc, &b);

        dtext(dc, PREP_LICENCE_LINE, L->creditPerm.left, L->creditPerm.top,
              L->creditPerm.right - L->creditPerm.left, S(34), P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    y = L->creditPerm.bottom + S(18);
    snprintf(buf, sizeof(buf),
        "Collects the hardware and software identity of a laptop that is being prepared for\r\n"
        "its next user, generates a battery report, finishes the Windows updates that are\r\n"
        "already on the machine and writes everything into a single log file plus a CSV row.\r\n");
    dtext(dc, buf, x, y, w, S(70), P.text2, g_fBody,
          DT_LEFT | DT_TOP | DT_NOPREFIX);

    y += S(84);
    dtext(dc, "Windows Update in this build", x, y, w, S(22), P.text, g_fTitle,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    y += S(28);
    {
        int cardH = S(210);
        if (y + cardH > L->content.bottom) cardH = L->content.bottom - y;
        if (cardH > S(70)) {
            card(dc, x - S(16), y - S(12), w + S(32), cardH, P.surface, P.line, S(16));
            snprintf(buf, sizeof(buf),
                "The update stage walks eight visible steps and always does the cheapest thing first:\r\n"
                "  1. read the local update store, the downloaded packages and the restart flags\r\n"
                "  2. install .msu / .cab packages that are already on the disk (no network)\r\n"
                "  3. verify the payloads with DISM, and repair only when something is broken\r\n"
                "  4. ask the update orchestrator (per machine, cannot hang on it)\r\n"
                "  5. check the current user session - the per-user half of the engine\r\n"
                "  6. search Windows Update, with a 45 minute budget and a 15 s heartbeat\r\n"
                "  7. download and install one update at a time, naming each one in the log\r\n"
                "  8. report the restart requirement and write the verdict with a next action\r\n"
                "\r\n"
                "It never claims \"up to date\" from a query that did not answer, and a wedged\r\n"
                "update service is reported as wedged - with the restart that actually fixes it.");
            dtext(dc, buf, x, y, w, cardH - S(20), P.text2, g_fSmall,
                  DT_LEFT | DT_TOP | DT_NOPREFIX);
        }
    }

    y += S(224);
    if (y + S(20) < L->content.bottom) {
        snprintf(buf, sizeof(buf),
                 "F5 / Ctrl+R start a run   -   Ctrl+L opens the live log   -   "
                 "Ctrl+O opens the log folder   -   Ctrl+C copies the log");
        dtext(dc, buf, x, y, w, S(20), P.muted, g_fTiny,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
}

/* ------------------------------------------------------------------ *
 *  off-screen buffer
 * ------------------------------------------------------------------ */
static HDC     g_bufDC = NULL;
static unsigned char *g_bufBits = NULL;   /* diagnostic: composed pixels */
static HBITMAP g_bufBmp = NULL;
static HBITMAP g_bufOld = NULL;
static int     g_bufW = 0, g_bufH = 0;

static void buf_release(void)
{
    if (g_bufDC) {
        if (g_bufOld) SelectObject(g_bufDC, g_bufOld);
        DeleteDC(g_bufDC);
        g_bufDC = NULL;
        g_bufOld = NULL;
    }
    if (g_bufBmp) { DeleteObject(g_bufBmp); g_bufBmp = NULL; }
    g_bufBits = NULL;
    g_bufW = g_bufH = 0;
}

/* ------------------------------------------------------------------ *
 *  diagnostic: write the frame we just composed to a .bmp file
 *
 *  LP_DUMP_FRAME=<folder> writes one file per view change (and one every
 *  300th frame) into that folder, so the picture the program drew can be
 *  looked at without a screen grabber.  Test aid only: with the switch
 *  unset this function returns immediately.
 * ------------------------------------------------------------------ */
static void dump_frame(const char *dir, int w, int h)
{
    static int lastView = -2, lastRun = -1;
    char path[MAX_PATH];
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE fhFile;
    DWORD wrote = 0, total;
    unsigned char *image;
    int y, ok = 1;
    DWORD stride;

    if (!dir || !*dir) return;
    {
        char d[110];
        if (!g_bufBits || w <= 0 || h <= 0) {
            snprintf(d, sizeof(d), "DIAG: dump skipped (bits=%d w=%d h=%d)",
                     g_bufBits != NULL, w, h);
            write_log(d);
            return;
        }
        if (g_view == lastView && g_runScreen == lastRun && (g_paintCount % 300) != 0)
            return;
        snprintf(d, sizeof(d), "DIAG: dump v=%d r=%d paint=%d",
                 g_view, g_runScreen, (int)g_paintCount);
        write_log(d);
    }
    lastView = g_view; lastRun = g_runScreen;

    stride = (DWORD)w * 4;
    total = stride * (DWORD)h;
    image = (unsigned char *)malloc(total);
    if (!image) { write_log("DIAG: frame malloc failed"); return; }
    for (y = 0; y < h; y++)                     /* BMPs are bottom-up */
        memcpy(image + (size_t)y * stride,
               g_bufBits + (size_t)(h - 1 - y) * stride, stride);

    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = total;
    fh.bfType = 0x4D42;
    fh.bfOffBits = (DWORD)(sizeof(fh) + sizeof(ih));
    fh.bfSize = fh.bfOffBits + total;

    snprintf(path, sizeof(path), "%s\\frame_v%d_r%d_p%04d.bmp", dir,
             g_view, g_runScreen, (int)(g_paintCount % 10000));
    fhFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (fhFile == INVALID_HANDLE_VALUE) {
        char d[160];
        snprintf(d, sizeof(d), "DIAG: CreateFile failed (%lu) '%s'",
                 (unsigned long)GetLastError(), path);
        write_log(d);
        free(image);
        return;
    }
    if (!WriteFile(fhFile, &fh, sizeof(fh), &wrote, NULL) || wrote != sizeof(fh)) ok = 0;
    if (ok && (!WriteFile(fhFile, &ih, sizeof(ih), &wrote, NULL) || wrote != sizeof(ih))) ok = 0;
    if (ok && (!WriteFile(fhFile, image, total, &wrote, NULL) || wrote != total)) ok = 0;
    CloseHandle(fhFile);
    free(image);
}

static void paint_window(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    HDC mem;
    HBITMAP bmp = NULL, obm = NULL;
    void *bits = NULL;
    Layout L;
    int cached = 0;          /* this paint reused the buffer we already had */
    int keepBuf = 0;         /* this paint built a buffer and kept it */

    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) { EndPaint(hwnd, &ps); return; }

    if (g_bufDC && g_bufW == rc.right && g_bufH == rc.bottom) {
        mem = g_bufDC;
        cached = 1;
    } else {
        buf_release();
        mem = CreateCompatibleDC(dc);
        /* A DIB section, not a compatible bitmap: GDI+ and GDI both draw into
         * it directly, while a device-dependent bitmap has to be converted on
         * every GDI+ operation (which is exactly the kind of hidden cost that
         * makes a custom-painted window crawl). */
        {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth       = rc.right;
            bi.bmiHeader.biHeight      = -rc.bottom;   /* top-down */
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        }
        if (!mem || !bmp) {
            if (mem) DeleteDC(mem);
            if (bmp) DeleteObject(bmp);
            mem = dc;
        } else {
            obm = (HBITMAP)SelectObject(mem, bmp);
            g_bufDC = mem; g_bufBmp = bmp; g_bufOld = obm; g_bufBits = bits;
            g_bufW = rc.right; g_bufH = rc.bottom;
            g_bufCount++;
            keepBuf = 1;
        }
    }

    compute_layout(hwnd, &L);
    g_paintCount++;
    gdi_audit();

    rect_fill(mem, 0, 0, rc.right, rc.bottom, P.bg);
    paint_sidebar(mem, &L);
    rect_fill(mem, L.side.right, 0, 1, rc.bottom, P.line);

    /* page transition: the content slides up into place after a nav click */
    {
        float t = ease_out(g_viewAnim);
        int dy = (int)((1.0f - t) * S(16));
        L.content.top += dy;
        L.content.bottom += dy;
    }

    switch (g_view) {
        case VIEW_HOME:
            if (g_runScreen) { paint_run(mem, &L); paint_restart_band(mem, &L); }
            else             paint_home(mem, &L);
            break;
        case VIEW_LOG:      paint_log(mem, &L); break;
        case VIEW_SETTINGS: paint_settings(mem, &L); break;
        case VIEW_ABOUT:    paint_about(mem, &L); break;
    }
    paint_notice(mem, &L);
    paint_titlebar(mem, hwnd);

    if (mem != dc) dump_frame(getenv("LP_DUMP_FRAME"), rc.right, rc.bottom);
    if (mem != dc) BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    /* Tear the buffer down again only when nobody kept it: a buffer that is
     * created and cached in the same paint must survive to the next frame.
     * (Getting this wrong is subtle - the first frame still looks perfect,
     * because it was blitted before the DC was deleted, and after that every
     * later BitBlt silently fails with error 6 and the window never updates
     * again while the program keeps painting into nothing.) */
    if (!cached && !keepBuf && mem != dc) {
        if (obm) SelectObject(mem, obm);
        DeleteDC(mem);
        if (bmp) DeleteObject(bmp);
    }
    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ *
 *  actions
 * ------------------------------------------------------------------ */
int g_runningNow(void)
{
    return (g_tStart != 0) && !g_finished;
}

/* is the module that is running right now the update stage? */
static int active_bit_is_updates(void)
{
    int a = (int)g_active;
    return (a >= 0 && g_mods[a].bit == PREP_M_UPDATES) ? 1 : 0;
}

static void open_windows_update(void)
{
    ShellExecuteA(g_hMain, "open", "ms-settings:windowsupdate", NULL, NULL, SW_SHOWNORMAL);
}

/* runs the shipped recovery script elevated; the script itself explains
 * what it does and pauses at the end so the operator can read it */
static int run_update_recovery(void)
{
    char dir[MAX_PATH], bat[MAX_PATH];
    char args[MAX_PATH + 32];
    HINSTANCE h;
    static const char *names[3];
    int i;

    names[0] = "fix-windows-update.bat";
    names[1] = "tools\\fix-windows-update.bat";
    names[2] = "..\\tools\\fix-windows-update.bat";
    exe_dir(dir, sizeof(dir));
    bat[0] = 0;
    for (i = 0; i < 3; i++) {
        snprintf(bat, sizeof(bat), "%s\\%s", dir, names[i]);
        if (GetFileAttributesA(bat) != INVALID_FILE_ATTRIBUTES) break;
        bat[0] = 0;
    }
    if (!bat[0]) {
        MessageBoxA(g_hMain,
            "fix-windows-update.bat was not found next to LaptopPrep.exe.\n\n"
            "It is shipped with the tool. Copy it next to the .exe and try again -\n"
            "or open Settings > Windows Update and use the built-in repair options.",
            "Recovery script missing", MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    if (MessageBoxA(g_hMain,
            "Run the Windows Update recovery now?\n\n"
            "It restarts the update services, re-registers the update agent and\n"
            "clears a stuck update session. It does NOT delete updates that are\n"
            "already downloaded, and it will ask for administrator rights.\n\n"
            "A command window opens and stays open so you can read what it did.",
            "Fix Windows Update", MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
        return 0;

    snprintf(args, sizeof(args), "/c \"\"%s\"\"", bat);
    h = ShellExecuteA(g_hMain, "runas", "cmd.exe", args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) {
        /* runas refuses when the user says no to the UAC prompt */
        MessageBoxA(g_hMain,
            "Windows did not start the recovery script.\n\n"
            "If you declined the administrator prompt, run LaptopPrep as\n"
            "administrator and try again.",
            "Recovery not started", MB_ICONWARNING | MB_OK);
        return 0;
    }
    write_log("Recovery script started (fix-windows-update.bat, elevated)");
    snprintf(g_status, sizeof(g_status),
             "Recovery script started - restart the laptop afterwards.");
    log_pump_file();
    return 1;
}

static void relaunch_elevated(void)
{
    wchar_t path[MAX_PATH];
    HINSTANCE h;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    h = ShellExecuteW(NULL, L"runas", path, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)h > 32) {
        DestroyWindow(g_hMain);
    } else {
        MessageBoxA(g_hMain,
            "Windows did not start the elevated copy.\n\n"
            "Continuing without administrator rights: the hardware collection still\n"
            "works, but TPM, DISM and Windows Update will report that they need\n"
            "elevation.",
            "Not elevated", MB_ICONINFORMATION | MB_OK);
    }
}

static void start_run(void)
{
    int i;
    if (g_runningNow()) return;

    if (!is_admin()) {
        int r = MessageBoxA(g_hMain,
            "LaptopPrep works best as administrator: reading the TPM, DISM and the\n"
            "Windows Update stage need elevation.\n\n"
            "Restart the app elevated now?\n"
            "(\"No\" runs anyway - the update stage will then report honestly that\n"
            "Windows refused it.)",
            "Administrator rights recommended", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1);
        if (r == IDYES) {
            relaunch_elevated();
            return;
        }
        write_log("WARNING: continuing without administrator rights - TPM, DISM and "
                  "Windows Update may be refused by Windows");
        snprintf(g_status, sizeof(g_status),
                 "Running without admin rights - some steps will be refused.");
    }

    if (!g_runScreen) anim_view_changed();
    g_view = VIEW_HOME;
    g_runScreen = 1;
    for (i = 0; i < NMOD; i++) {
        InterlockedExchange(&g_step[i], (g_selected & g_mods[i].bit) ? STEP_QUEUED : -1);
        g_stepEase[i] = 0.0f;
        g_stepFlash[i] = 0.0f;
    }
    for (i = 0; i < WU_STEP_COUNT; i++) {
        g_wuStepEase[i] = 0.0f;
        g_wuRowFlash[i] = 0.0f;
        g_wuLastState[i] = -1;
    }
    g_ringPct = 0.0f;
    g_anyErr = 0;
    g_anyWarn = 0;
    g_finished = 0;
    g_runRc = 0;
    g_lastLogChange = GetTickCount64();
    g_saidWaiting = 0;
    InterlockedExchange(&g_runFinishedFlag, 0);
    g_restartPending = 0;
    g_restartOffered = 0;
    g_restartLater = 0;
    g_restartAnim = 0.0f;
    g_restartDeadline = 0;
    g_runIn = 0.0f;
    for (i = 0; i < WU_STEP_COUNT; i++) g_wuCheck[i] = 1.0f;
    g_tStart = GetTickCount64();
    g_tEnd = 0;
    g_runFlags = g_selected;
    snprintf(g_status, sizeof(g_status), "Starting up...");
    log_reset();
    g_logPos = 0;

    if (g_hWorker) { CloseHandle(g_hWorker); g_hWorker = NULL; }
    g_hWorker = CreateThread(NULL, 0, WorkerProc, (LPVOID)(UINT_PTR)g_runFlags, 0, NULL);
    if (!g_hWorker) {
        g_finished = 1;
        g_tEnd = GetTickCount64();
        snprintf(g_status, sizeof(g_status), "Could not start the worker thread.");
        return;
    }
    if (g_minimizeOnRun) ShowWindow(g_hMain, SW_MINIMIZE);
}

static void finish_run_ui(void)
{
    if (g_finished) return;
    g_finished = 1;
    if (!g_tEnd) g_tEnd = GetTickCount64();
    {
        int v = prep_update_verdict();
        if (g_anyErr)
            snprintf(g_status, sizeof(g_status), "Last run finished with something to fix - see the log.");
        else if ((g_selected & PREP_M_UPDATES) && v == WU_J_SUCCESS_RESTART)
            snprintf(g_status, sizeof(g_status), "Updates installed - a restart finishes them.");
        else if (v != WU_J_NONE && v != WU_J_SUCCESS && (g_selected & PREP_M_UPDATES))
            snprintf(g_status, sizeof(g_status), "Update stage: %s", wu_verdict_id(v));
        else
            snprintf(g_status, sizeof(g_status), "Last run finished successfully.");
    }
    if (g_hWorker) { CloseHandle(g_hWorker); g_hWorker = NULL; }
    if (g_minimizeOnRun && IsIconic(g_hMain)) ShowWindow(g_hMain, SW_RESTORE);
    SetForegroundWindow(g_hMain);
    restart_offer();          /* after the upload step, always the same way */
}

static void open_logdir(void)
{
    ShellExecuteA(g_hMain, "open", prep_log_dir(), NULL, NULL, SW_SHOWNORMAL);
}

/* ---- the restart offer ------------------------------------------- */

/* what happened to the log, in one honest line */
static void restart_upload_note(char *out, size_t n)
{
    int up = prep_last_upload_result();
    if (prep_url_is_placeholder_ex()) {
        snprintf(out, n, "No upload endpoint is configured, so the log stays at %s.",
                 prep_log_path());
    } else if (up == 1) {
        snprintf(out, n, "The run record is on the server. The restart is what "
                         "finishes the installed updates.");
    } else if (up == -1) {
        snprintf(out, n, "Upload failed - the log is saved at %s and can be sent "
                         "afterwards with Upload.", prep_log_path());
    } else {
        snprintf(out, n, "Not uploaded yet. The log is at %s.", prep_log_path());
    }
}

/* called once per run from finish_run_ui(), after the upload has been tried */
static void restart_offer(void)
{
    char note[400];

    if (g_restartOffered) return;
    if (!(g_selected & PREP_M_UPDATES)) return;
    if (!prep_restart_pending()) return;

    g_restartPending = 1;
    g_restartOffered = 1;
    g_restartAnim = 0.0f;

    restart_upload_note(note, sizeof(note));
    write_log("--- Restart offered to the operator (the upload step has already run) ---");
    write_log(note);
    log_pump_file();

    /* make sure the operator actually sees the offer */
    if (g_minimizeOnRun && IsIconic(g_hMain)) ShowWindow(g_hMain, SW_RESTORE);
    SetForegroundWindow(g_hMain);
    FlashWindow(g_hMain, TRUE);
    snprintf(g_status, sizeof(g_status),
             "Updates are installed. A restart finishes them - press Restart now.");
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* the operator pressed Restart now */
static void begin_restart(void)
{
    DWORD rc = 0;
    char line[512];

    if (g_restartDeadline) return;              /* already counting down */

    if (!is_admin()) {
        write_log("Restart refused: this program is not running as administrator");
        log_pump_file();
        MessageBoxA(g_hMain,
            "Windows refuses a program-scheduled restart without administrator\n"
            "rights.\n\n"
            "The updates are already installed - restart the laptop from the Start\n"
            "menu and they will finish. The log is saved on disk and can be uploaded\n"
            "afterwards with the Upload button in Settings.",
            "Restart manually", MB_ICONINFORMATION | MB_OK);
        snprintf(g_status, sizeof(g_status),
                 "Restart needs administrator rights - use Start > Power > Restart.");
        InvalidateRect(g_hMain, NULL, FALSE);
        return;
    }

    write_log("--- Restart requested by the operator ---");
    restart_upload_note(line, sizeof(line));
    write_log(line);

    /* 45 seconds: long enough to save work, short enough not to be forgotten.
     * shutdown /a cancels it, and so does the Cancel button in the banner. */
    if (run_process_logged("shutdown /r /t 45 /c \"LaptopPrep: finishing Windows updates\"",
                           15000, &rc) && rc == 0) {
        g_restartDeadline = GetTickCount64() + 45000ull;
        g_restartByUs = 1;
        write_log("Restart scheduled in 45 seconds - 'shutdown /a' or the Cancel "
                  "button in the window stops it");
        snprintf(g_status, sizeof(g_status),
                 "Restarting in 45 seconds - Cancel keeps Windows running.");
    } else {
        write_log("shutdown /r did not succeed - the operator has to restart from "
                  "the Start menu");
        snprintf(g_status, sizeof(g_status),
                 "Windows refused the restart (code %lu) - use Start > Power > Restart.",
                 (unsigned long)rc);
    }
    log_pump_file();
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* the operator wants to stay in Windows after all */
static void cancel_restart(void)
{
    DWORD rc = 0;
    if (!g_restartDeadline) return;
    g_restartDeadline = 0;
    if (g_restartByUs)
        run_process_logged("shutdown /a", 10000, &rc);
    g_restartByUs = 0;
    write_log(rc == 0 ? "Restart cancelled by the operator - the updates stay "
                        "installed and finish at the next restart"
                      : "Restart cancel: 'shutdown /a' reported something odd; if the "
                        "machine restarts anyway that is harmless, the updates are "
                        "installed either way");
    snprintf(g_status, sizeof(g_status),
             "Restart cancelled. Windows finishes the updates at the next restart.");
    log_pump_file();
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* seconds left on the countdown, 0 when there is none */
static int restart_seconds_left(void)
{
    ULONGLONG now;
    if (!g_restartDeadline) return 0;
    now = GetTickCount64();
    if (now >= g_restartDeadline) return 0;
    return (int)((g_restartDeadline - now + 999ull) / 1000ull);
}

/* the banner, in the card language of the rest of the window */
static void paint_restart_band(HDC dc, Layout *L)
{
    RECT c;
    int cw, ch, cy, badgeR, tx, tw, left;
    char head[160], note[400];

    if (!g_restartOffered) return;
    c = L->restartBand;
    cw = c.right - c.left;
    ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0 || g_restartAnim < 0.02f) return;

    c.top += (int)((1.0f - ease_out(g_restartAnim)) * S(22));      /* slide in */
    c.bottom += (int)((1.0f - ease_out(g_restartAnim)) * S(22));

    shadow_card(dc, c.left, c.top, cw, ch, S(14));
    card(dc, c.left, c.top, cw, ch, P.surface, P.brandLine, S(14));

    cy = (c.top + c.bottom) / 2;
    badgeR = S(19);
    {
        int bcx = c.left + S(20) + badgeR;
        COLORREF bg = blend(P.surface, P.brand, 0.16f);
        circle_fill(dc, bcx, cy, badgeR, bg, blend(P.brandLine, P.brand, 0.35f));
        power_glyph(dc, bcx, cy, badgeR - S(8), P.brand);   /* restart symbol */
        tx = bcx + badgeR + S(16);
    }
    tw = c.right - tx - S(300);
    if (tw < S(150)) tw = c.right - tx - S(10);

    left = restart_seconds_left();
    if (left > 0)
        snprintf(head, sizeof(head), "Restarting in %d second%s - Cancel stops it",
                 left, left == 1 ? "" : "s");
    else if (g_restartLater)
        snprintf(head, sizeof(head), "Restart still needed to finish the updates");
    else
        snprintf(head, sizeof(head), "Restart needed to finish the updates");

    dtext(dc, head, tx, cy - S(19), tw, S(22), P.text, g_fTitle,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    restart_upload_note(note, sizeof(note));
    dtext(dc, note, tx, cy + S(3), tw, S(16), P.muted, g_fSmall,
          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    {
        BtnSpec b;
        memset(&b, 0, sizeof(b));
        if (left > 0) {
            b.id = WHAT_BTN_RESTART; b.r = L->btnRestart; b.kind = 1;
            b.enabled = 1; b.label = "Cancel restart"; b.font = g_fBody;
            paint_button(dc, &b);
        } else {
            b.id = WHAT_BTN_RESTART; b.r = L->btnRestart; b.kind = 0;
            b.enabled = 1; b.label = "Restart now"; b.font = g_fBody;
            paint_button(dc, &b);

            b.id = WHAT_BTN_RESTART_LATE; b.r = L->btnRestartLate; b.kind = 2;
            b.enabled = 1; b.label = "Later"; b.font = g_fBody;
            paint_button(dc, &b);
        }
    }
}

/* the restart: upload first, then hand the reboot to Windows ------------- */
static void restart_after_upload(void)
{
    DWORD rc = 0;
    int up = 0;
    char line[512];

    if (MessageBoxA(g_hMain,
        "Upload the log and then restart this laptop?\n\n"
        "The restart is what finishes the installed updates.\n"
        "Windows gives you 20 seconds to change your mind: run  shutdown /a  to cancel.",
        "Upload + restart", MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
        return;

    write_log("--- Restart requested by the operator ---");
    write_log("Uploading the log before the reboot...");
    log_pump_file();
    up = log_upload(prep_log_path());
    log_pump_file();
    if (up) write_log("Log uploaded - the run record is safe on the server");
    else    write_log("Upload did not succeed. The log is still on disk - copy it off "
                      "the machine before the disk is wiped");
    snprintf(line, sizeof(line), "Log file: %s", prep_log_path());
    write_log(line);

    if (!is_admin()) {
        snprintf(g_status, sizeof(g_status),
                 "Restart needs administrator rights - reboot manually to finish.");
        write_log("Restart refused: not running as administrator");
        log_pump_file();
        MessageBoxA(g_hMain,
            "Windows refused the restart because this program is not running as\n"
            "administrator.\n\n"
            "Restart the laptop from the Start menu to finish the updates - they are\n"
            "already installed and only need the reboot.",
            "Restart manually", MB_ICONINFORMATION | MB_OK);
        return;
    }

    write_log("Rebooting in 20 seconds - run 'shutdown /a' to cancel");
    log_pump_file();
    if (run_process_logged("shutdown /r /t 20 /c \"LaptopPrep: finishing Windows updates\"",
                           15000, &rc) && rc == 0) {
        snprintf(g_status, sizeof(g_status),
                 "Restart scheduled in 20 seconds - shutdown /a cancels it.");
    } else {
        snprintf(g_status, sizeof(g_status),
                 "Windows refused the restart (exit code %lu) - reboot from the Start menu.",
                 (unsigned long)rc);
        write_log("shutdown /r did not succeed - reboot manually from the Start menu");
    }
    InvalidateRect(g_hMain, NULL, FALSE);
}

/* ------------------------------------------------------------------ *
 *  worker
 * ------------------------------------------------------------------ */
static DWORD WINAPI WorkerProc(LPVOID param)
{
    unsigned flags = (unsigned)(UINT_PTR)param;
    int rc;
    rc = prep_run_all(flags);
    gui_on_run_finished(rc);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  hit testing
 * ------------------------------------------------------------------ */
static int hit_test(POINT p, Layout *L, int *what)
{
    int i;
    *what = -1;
    {
        RECT bar, tbtn[TB_BTNS];
        titlebar_metrics(g_hMain, &bar, tbtn);
        for (i = 0; i < TB_BTNS; i++)
            if (point_in(p.x, p.y, tbtn[i].left, tbtn[i].top,
                         tbtn[i].right - tbtn[i].left, tbtn[i].bottom - tbtn[i].top)) {
                *what = title_what_for_index(i);
                return 1;
            }
    }
    if (g_view == VIEW_ABOUT && L->linkWintr.right > L->linkWintr.left) {
        if (in_rect(p.x, p.y, L->linkWintr)) { *what = WHAT_LINK_WINTR; return 1; }
        if (in_rect(p.x, p.y, L->linkAgpcs)) { *what = WHAT_LINK_AGPCS; return 1; }
    }
    {
        RECT nc;
        if (!g_noticeOff && notice_close_rect(L, &nc) &&
            in_rect(p.x, p.y, nc)) { *what = WHAT_NOTICE_CLOSE; return 1; }
    }
    if (g_restartOffered && L->btnRestart.right > L->btnRestart.left) {
        if (in_rect(p.x, p.y, L->btnRestart))     { *what = WHAT_BTN_RESTART;      return 1; }
        if (!restart_seconds_left() && in_rect(p.x, p.y, L->btnRestartLate))
                                                  { *what = WHAT_BTN_RESTART_LATE; return 1; }
    }
    for (i = 0; i < VIEW_COUNT; i++)
        if (in_rect(p.x, p.y, L->nav[i])) { *what = 100 + i; return 1; }

    if (g_view == VIEW_HOME && !g_runScreen) {
        for (i = 0; i < NMOD; i++)
            if (in_rect(p.x, p.y, L->cards[i])) { *what = i; return 1; }
        if (in_rect(p.x, p.y, L->btnGhost))     { *what = WHAT_BTN_GHOST;     return 1; }
        if (in_rect(p.x, p.y, L->btnSecondary)) { *what = WHAT_BTN_SECONDARY; return 1; }
        if (in_rect(p.x, p.y, L->btnPrimary))   { *what = WHAT_BTN_PRIMARY;   return 1; }
    } else if (g_view == VIEW_HOME) {
        if (!g_runningNow() && L->btnAction.right > L->btnAction.left &&
            in_rect(p.x, p.y, L->btnAction))    { *what = WHAT_BTN_WU_ACTION; return 1; }
        if (in_rect(p.x, p.y, L->btnGhost))     { *what = WHAT_BTN_GHOST;     return 1; }
        if (in_rect(p.x, p.y, L->btnSecondary)) { *what = WHAT_BTN_SECONDARY; return 1; }
        if (in_rect(p.x, p.y, L->btnPrimary))   { *what = WHAT_BTN_PRIMARY;   return 1; }
    } else if (g_view == VIEW_LOG) {
        for (i = 0; i < 3; i++)
            if (in_rect(p.x, p.y, L->btnLog[i])) { *what = 300 + i; return 1; }
    } else if (g_view == VIEW_SETTINGS) {
        RECT r;
        for (i = 0; i < N_SETTING_ROWS; i++) {
            if (!setting_row_rect(L, i, &r)) break;
            if (in_rect(p.x, p.y, r)) { *what = g_settingRows[i].id; return 1; }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  window procedure
 * ------------------------------------------------------------------ */
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE:
            g_hMain = hwnd;
            return 0;

        case WM_NCCALCSIZE:
            if (wp) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS*)lp;
                if (IsZoomed(hwnd)) {
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi;
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoA(mon, &mi)) p->rgrc[0] = mi.rcWork;
                }
            }
            return 0;

        case WM_NCPAINT:
            return 0;

        case WM_NCACTIVATE:
            InvalidateRect(hwnd, NULL, FALSE);
            return TRUE;

        case WM_NCHITTEST:
        {
            /* the window has no system frame of its own, so the edges and the
             * title bar are hit-tested here and nowhere else */
            POINT p;
            RECT rc, bar, tbtn[TB_BTNS];
            int b = S(6), i, maximized = IsZoomed(hwnd) ? 1 : 0;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            GetWindowRect(hwnd, &rc);
            ScreenToClient(hwnd, &p);
            titlebar_metrics(hwnd, &bar, tbtn);

            for (i = 0; i < TB_BTNS; i++)
                if (point_in(p.x, p.y, tbtn[i].left, tbtn[i].top,
                             tbtn[i].right - tbtn[i].left,
                             tbtn[i].bottom - tbtn[i].top))
                    return HTCLIENT;

            if (!maximized) {
                int L = p.x < b, R = p.x >= rc.right - b;
                int T = p.y < b, B = p.y >= rc.bottom - b;
                if (T && L) return HTTOPLEFT;
                if (T && R) return HTTOPRIGHT;
                if (B && L) return HTBOTTOMLEFT;
                if (B && R) return HTBOTTOMRIGHT;
                if (T) return HTTOP;
                if (B) return HTBOTTOM;
                if (L) return HTLEFT;
                if (R) return HTRIGHT;
            }
            if (p.y < S(TITLE_H)) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_GETMINMAXINFO:
        {
            MINMAXINFO *mmi = (MINMAXINFO*)lp;
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            mmi->ptMinTrackSize.x = S(1060);
            mmi->ptMinTrackSize.y = S(700);
            if (GetMonitorInfoA(mon, &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left;
                mmi->ptMaxPosition.y = mi.rcWork.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
            return 0;
        }

        case WM_APP + 10:
        case WM_APP + 11:
            if (msg == WM_APP + 11) finish_run_ui();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_SETTINGCHANGE:
            {
                HKEY k;
                DWORD v = 1, n = sizeof(v);
                if (RegOpenKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
                    if (RegQueryValueExA(k, "AppsUseLightTheme", 0, NULL,
                                         (BYTE*)&v, &n) == ERROR_SUCCESS) {
                        int dark = (v == 0);
                        if (dark != g_themeDark) {
                            g_themeDark = dark;
                            theme_apply();
                            theme_brushes();
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                    RegCloseKey(k);
                }
            }
            return 0;

        case WM_PAINT:
            paint_window(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_MOUSEMOVE:
        {
            Layout L;
            int what = -1, redraw = 0;
            POINT p;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            compute_layout(hwnd, &L);
            if (hit_test(p, &L, &what)) {
                if (what >= 100 && what < 100 + VIEW_COUNT) {
                    if (g_hoverNav != what - 100) { g_hoverNav = what - 100; redraw = 1; }
                } else if (what >= 200 && what < 300) {
                    if (g_hoverModule != what) { g_hoverModule = what; redraw = 1; }
                } else if (what >= WHAT_TITLE_MIN && what <= WHAT_TITLE_THEME) {
                    int ti = title_index_for_what(what);
                    if (g_hoverTitle != ti) { g_hoverTitle = ti; redraw = 1; }
                } else if (what >= 0 && what < NMOD) {
                    if (g_hoverModule != what) { g_hoverModule = what; redraw = 1; }
                } else {
                    if (g_hoverBtn != what) { g_hoverBtn = what; redraw = 1; }
                }
            } else {
                if (g_hoverNav != -1 || g_hoverModule != -1 || g_hoverBtn != -1 ||
                    g_hoverTitle != -1) {
                    g_hoverNav = -1; g_hoverModule = -1; g_hoverBtn = -1; g_hoverTitle = -1;
                    redraw = 1;
                }
            }
            if (redraw) InvalidateRect(hwnd, NULL, FALSE);
            {
                TRACKMOUSEEVENT tme;
                memset(&tme, 0, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            fx_press_release();
            return 0;

        case WM_MOUSELEAVE:
            g_hoverNav = g_hoverModule = g_hoverBtn = g_hoverTitle = -1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_SETCURSOR:
        {
            Layout L;
            int what = -1;
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            compute_layout(hwnd, &L);
            if (hit_test(p, &L, &what)) {
                if (what < 1000) SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN:
        {
            Layout L;
            int what = -1;
            POINT p;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            compute_layout(hwnd, &L);
            SetFocus(hwnd);

            if (hit_test(p, &L, &what)) {
                DWORD now = GetTickCount();
                if (what == g_lastClickWhat && (now - g_lastClickTick) < 350)
                    return 0;
                g_lastClickWhat = what;
                g_lastClickTick = now;

                fx_ripple_spawn(what, p.x, p.y,
                                (what < NMOD) ? P.brand
                                              : blend(P.brand, RGB(255, 255, 255), 0.3f));
                if (what >= 0 && what < NMOD) g_modPress[what] = 1.0f;
                if (what >= 100 && what < 100 + VIEW_COUNT) g_navPress[what - 100] = 1.0f;

                if (what >= WHAT_TITLE_MIN && what <= WHAT_TITLE_THEME) {
                    int ti = title_index_for_what(what);
                    if (ti >= 0) g_titlePress[ti] = 1.0f;
                    if (what == WHAT_TITLE_MIN)  ShowWindow(hwnd, SW_MINIMIZE);
                    if (what == WHAT_TITLE_MAX)  ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    if (what == WHAT_TITLE_CLOSE) PostMessageA(hwnd, WM_CLOSE, 0, 0);
                    if (what == WHAT_TITLE_THEME) {
                        g_themeDark = !g_themeDark;
                        theme_apply();
                        theme_store(g_themeDark);
                        theme_brushes();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    return 0;
                } else if (what == WHAT_BTN_PRIMARY) {
                    if (g_view == VIEW_HOME && !g_runScreen) start_run();
                    else                                     g_view = VIEW_LOG;
                    if (g_view == VIEW_LOG) log_pump_file();
                } else if (what == WHAT_BTN_WU_ACTION) {
                    int v = prep_update_verdict();
                    if (g_restartPending) {
                        /* the offer has already been made and the upload has
                         * already run: this button is just the same choice */
                        if (restart_seconds_left() > 0) cancel_restart();
                        else                            begin_restart();
                    } else if (v == WU_J_SUCCESS_RESTART || v == WU_J_WEDGED) {
                        restart_after_upload();
                    } else if (v == WU_J_UNFINISHED || v == WU_J_BLOCKED) {
                        run_update_recovery();
                    } else if (v == WU_J_NEEDS_ELEVATION) {
                        relaunch_elevated();
                    } else if (v == WU_J_OFFLINE) {
                        ShellExecuteA(hwnd, "open", "ms-settings:network", NULL, NULL,
                                      SW_SHOWNORMAL);
                    } else if (v == WU_J_ERROR) {
                        open_windows_update();
                    } else {
                        g_view = VIEW_LOG;
                        log_pump_file();
                    }
                } else if (what == WHAT_BTN_GHOST) {
                    open_logdir();
                } else if (what == WHAT_BTN_SECONDARY) {
                    if (g_view == VIEW_HOME && !g_runScreen) {
                        g_selected = (g_selected == PREP_ALL_FLAGS) ? 0u
                                                                   : (unsigned)PREP_ALL_FLAGS;
                    } else {
                        start_run();
                    }
                } else if (what == WHAT_NOTICE_CLOSE) {
                    RECT nc;
                    g_noticeOff = 1;
                    if (notice_close_rect(&L, &nc))
                        fx_ripple_spawn(WHAT_NOTICE_CLOSE,
                                        (nc.left + nc.right) / 2,
                                        (nc.top + nc.bottom) / 2, P.warn);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                } else if (what >= 300 && what < 400) {
                    if (what == 300) log_copy_all();
                    if (what == 301) open_logdir();
                    if (what == 302) {
                        g_logFollow = !g_logFollow;
                        if (g_logFollow) g_logScroll = 0;
                    }
                } else if (what >= 200 && what < 300) {
                    if (what == 200) {
                        g_autoScroll = !g_autoScroll;
                        if (g_autoScroll) { g_logScroll = 0; g_logFollow = 1; }
                    }
                    if (what == 201) g_minimizeOnRun = !g_minimizeOnRun;
                    if (what == 202) open_logdir();
                    if (what == 203) {
                        g_themeDark = !g_themeDark;
                        theme_apply();
                        theme_store(g_themeDark);
                        theme_brushes();
                    }
                    if (what == 204) {
                        int configured, up, http;
                        configured = !prep_url_is_placeholder_ex();
                        snprintf(g_status, sizeof(g_status),
                                 configured ? "Uploading the log..." :
                                              "No upload endpoint configured.");
                        InvalidateRect(hwnd, NULL, TRUE);
                        UpdateWindow(hwnd);
                        up = configured ? log_upload(prep_log_path()) : 0;
                        http = prep_last_upload_http();
                        if (!configured)
                            snprintf(g_status, sizeof(g_status),
                                     "No upload endpoint configured - set the address in "
                                     "the row below.");
                        else if (up)
                            snprintf(g_status, sizeof(g_status),
                                     "Log uploaded - the server answered HTTP %d.", http);
                        else if (http > 0)
                            snprintf(g_status, sizeof(g_status),
                                     "The server refused the upload (HTTP %d) - the log is "
                                     "still on disk and still in the log folder.", http);
                        else
                            snprintf(g_status, sizeof(g_status),
                                     "No answer from %s - check the address and the "
                                     "network; the log is still on disk.", prep_log_url());
                        write_log(configured && up ? "Manual upload: OK"
                                                   : "Manual upload: not sent");
                        log_pump_file();
                    }
                    {
                        int ri = setting_row_index_for_id(what);
                        if (ri >= 0) g_rowPress[ri] = 1.0f;
                    }
                    if (what == 207) {
                        /* start editing the server address in place */
                        strncpy(g_editBuf, prep_log_url(), sizeof(g_editBuf) - 1);
                        g_editBuf[sizeof(g_editBuf) - 1] = 0;
                        g_editCaret = (int)strlen(g_editBuf);
                        g_editRow = 207;
                        SetFocus(hwnd);
                        snprintf(g_status, sizeof(g_status),
                                 "Type the address your server listens on, then press Enter "
                                 "(Esc cancels).");
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    if (what == 205) run_update_recovery();
                    if (what == 206) open_windows_update();
                } else if (what >= 100 && what < 100 + VIEW_COUNT) {
                    if (g_view != what - 100) anim_view_changed();
                    g_view = what - 100;
                    if (g_view == VIEW_HOME && !g_runningNow()) {
                        if (g_runScreen) anim_view_changed();
                        g_runScreen = 0;
                    }
                    if (g_view == VIEW_LOG) log_pump_file();
                } else if (what >= 0 && what < NMOD) {
                    g_selected ^= g_mods[what].bit;
                    g_modPop[what] = 1.0f;
                }
                if (what == WHAT_LINK_WINTR)
                    ShellExecuteA(hwnd, "open", PREP_URL_WINTR, NULL, NULL, SW_SHOWNORMAL);
                if (what == WHAT_LINK_AGPCS)
                    ShellExecuteA(hwnd, "open", PREP_URL_AGPCS, NULL, NULL, SW_SHOWNORMAL);
                if (what == WHAT_BTN_RESTART) {
                    if (restart_seconds_left()) cancel_restart();
                    else                        begin_restart();
                }
                if (what == WHAT_BTN_RESTART_LATE) {
                    g_restartLater = 1;
                    write_log("Restart postponed by the operator - the offer stays on "
                              "screen and nothing restarts by itself");
                    log_pump_file();
                    snprintf(g_status, sizeof(g_status),
                             "Restart postponed. The laptop finishes the updates at "
                             "the next restart.");
                }
                if (what == WHAT_BTN_PRIMARY)   g_btnPress[0] = 1.0f;
                if (what == WHAT_BTN_SECONDARY) g_btnPress[1] = 1.0f;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_CHAR:
            if (g_editRow >= 0) {
                unsigned ch = (unsigned)wp;
                if (ch == 27) {                                  /* Esc: cancel */
                    g_editRow = -1;
                    snprintf(g_status, sizeof(g_status), "Left the address as it was.");
                } else if (ch == 13 || ch == 10) {               /* Enter: save */
                    char saved[640];
                    if (prep_log_url_set(g_editBuf)) {
                        snprintf(saved, sizeof(saved), "Upload server set to %s",
                                 prep_log_url());
                        write_log(saved);
                        snprintf(g_status, sizeof(g_status),
                                 "Saved. Every upload from now on goes to %s.",
                                 prep_log_url());
                    } else {
                        snprintf(g_status, sizeof(g_status),
                                 "That is not an http:// or https:// address - nothing saved.");
                    }
                    g_editRow = -1;
                    log_pump_file();
                } else if (ch == 8) {                            /* backspace */
                    if (g_editCaret > 0) g_editBuf[--g_editCaret] = 0;
                } else if (ch >= 32 && ch < 127 &&
                           g_editCaret < (int)sizeof(g_editBuf) - 1) {
                    g_editBuf[g_editCaret++] = (char)ch;
                    g_editBuf[g_editCaret] = 0;
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            return 0;

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINT p;
            Layout L;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            ScreenToClient(hwnd, &p);
            compute_layout(hwnd, &L);
            if (g_view == VIEW_LOG && in_rect(p.x, p.y, L.logArea)) {
                /* g_logScroll counts how far *back* from the live tail the
                 * view sits: 0 shows the newest line.  Wheel up = further
                 * back through history, wheel down = back toward the tail.
                 * The kick overshoots in the direction of travel - it used
                 * to overshoot the other way, which made a downward scroll
                 * jump upwards first and then drop back, and follow used to
                 * switch on at the wrong end of the log. */
                if (delta > 0) { g_logScroll += 3; g_scrollKick += S(14); }
                else           { g_logScroll -= 3; g_scrollKick -= S(14); }
                if (g_logScroll < 0) g_logScroll = 0;
                if (g_logScroll > g_logMaxScroll) g_logScroll = g_logMaxScroll;
                g_logFollow = (g_logScroll <= 0);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == TIMER_ANIM) {
                static int pump = 0;
                if (++pump >= 4) {
                    size_t before = g_logLen;
                    pump = 0;
                    log_pump_file();
                    if (g_runningNow()) {
                        ULONGLONG now = GetTickCount64();
                        if (g_logLen != before) {
                            g_lastLogChange = now;
                            g_saidWaiting = 0;
                        } else if (!g_saidWaiting && (now - g_tStart) > 8000 &&
                                   (g_lastLogChange == 0 || (now - g_lastLogChange) > 8000)) {
                            g_saidWaiting = 1;
                            write_log("Still working - this step is busy; the log shows "
                                      "what it is doing and a heartbeat every 15 seconds");
                            log_pump_file();
                        }
                    }
                }
                anim_tick();
                if (ui_needs_frame()) InvalidateRect(hwnd, NULL, FALSE);
                if (InterlockedCompareExchange(&g_runFinishedFlag, 0, 0) && g_runningNow())
                    finish_run_ui();
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_F5 || (wp == 'R' && (GetKeyState(VK_CONTROL) & 0x8000))) {
                g_view = VIEW_HOME;
                g_runScreen = 0;
                start_run();
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wp == 'L' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                g_view = VIEW_LOG;
                log_pump_file();
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                open_logdir();
            } else if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) && g_view == VIEW_LOG) {
                log_copy_all();
            } else if (wp == VK_ESCAPE && g_view == VIEW_HOME && g_runScreen && !g_runningNow()) {
                g_runScreen = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (g_view == VIEW_LOG && wp == VK_PRIOR) {
                g_logFollow = 0;
                g_logScroll += 10;
                if (g_logScroll > g_logMaxScroll) g_logScroll = g_logMaxScroll;
            } else if (g_view == VIEW_LOG && wp == VK_NEXT) {
                g_logScroll -= 10;
                if (g_logScroll <= 0) { g_logScroll = 0; g_logFollow = 1; }
            } else if (g_view == VIEW_LOG && wp == VK_HOME) {
                g_logFollow = 0;
                g_logScroll = g_logMaxScroll;
            } else if (g_view == VIEW_LOG && wp == VK_END) {
                g_logScroll = 0;
                g_logFollow = 1;
            }
            return 0;

        case WM_CLOSE:
            if (g_runningNow()) {
                int r = MessageBoxA(hwnd,
                    "A prep run is still busy. Close the console anyway?\n\n"
                    "(child processes are killed - already installed updates stay on\n"
                    "the machine, and the log keeps what happened so far)",
                    "LaptopPrep", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                if (r != IDYES) return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            free_fonts();
            gfx_stop();
            buf_release();
            free(g_logBuf);
            free(g_lines);
            free(g_lineBorn);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void theme_brushes(void)
{
    if (g_brCard)    DeleteObject(g_brCard);
    if (g_brCardHi)  DeleteObject(g_brCardHi);
    if (g_brSideSel) DeleteObject(g_brSideSel);
    if (g_brLog)     DeleteObject(g_brLog);
    g_brCard    = CreateSolidBrush(P.surface);
    g_brCardHi  = CreateSolidBrush(P.surfaceHi);
    g_brSideSel = CreateSolidBrush(P.sideSel);
    g_brLog     = CreateSolidBrush(P.logbg);
}

/* ------------------------------------------------------------------ *
 *  entry point
 * ------------------------------------------------------------------ */
static void enable_dpi_awareness(void)
{
    HMODULE u32 = LoadLibraryA("user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *SetCtxFn)(void*);
        FARPROC raw = GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        SetCtxFn fn = NULL;
        if (raw) memcpy(&fn, &raw, sizeof(fn));
        if (fn && fn((void*)-4 /* PER_MONITOR_AWARE_V2 */)) return;
        FreeLibrary(u32);
    }
    SetProcessDPIAware();
}

/* ------------------------------------------------------------------ *
 *  entry point
 *
 *  mingw-w64 provides the Unicode entry point (wWinMain + -municode).
 *  The older MinGW.org toolchain and Cygwin's gcc do not: they only know
 *  WinMain, and they reject -municode with "unrecognized command line
 *  option".  So the real entry is lp_main() and the two wrappers below
 *  pick the one the toolchain has.  build.bat probes for -municode and
 *  defines LP_ANSI_ENTRY when it is missing; nothing else changes,
 *  because the wide command line is available either way.
 * ------------------------------------------------------------------ */
static int lp_main(HINSTANCE hInst, int show);

#ifdef LP_ANSI_ENTRY
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd;                 /* GetCommandLineW() still works */
    return lp_main(hInst, show);
}
#else
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    (void)hPrev; (void)cmd;
    return lp_main(hInst, show);
}
#endif

static int lp_main(HINSTANCE hInst, int show)
{
    WNDCLASSA wc;
    MSG msg;
    HICON icon;
    int w = 1320, h = 880;
    RECT wa, wr;

    enable_dpi_awareness();
    {
        HDC dc = GetDC(NULL);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
        if (g_dpi < 96) g_dpi = 96;
    }

    prep_init();
    theme_load();
    theme_apply();
    load_branding();
    make_fonts();
    InitCommonControls();
    theme_brushes();
    gfx_start();

    icon = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON,
                             S(32), S(32), LR_DEFAULTCOLOR);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "LaptopPrepMain";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.hIcon = icon;
    if (!RegisterClassA(&wc)) return 1;

    /* open at a size that always fits the screen: 92% of the work area,
     * clamped to the design range, never bigger than the screen */
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        int sw = wa.right - wa.left, sh = wa.bottom - wa.top;
        int wantW = (int)(sw * 0.92), wantH = (int)(sh * 0.92);
        w = clampi(wantW, S(1060), S(1400));
        h = clampi(wantH, S(700), S(940));
        if (w > sw) w = sw;
        if (h > sh) h = sh;
    }
    wr.left = 0; wr.top = 0; wr.right = w; wr.bottom = h;
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_hMain = CreateWindowExA(0, "LaptopPrepMain",
        "LaptopPrep " PREP_VERSION_STR "  -  refurbishment console",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInst, NULL);
    if (!g_hMain) return 1;

    SendMessageA(g_hMain, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessageA(g_hMain, WM_SETICON, ICON_SMALL, (LPARAM)icon);

    SetTimer(g_hMain, TIMER_ANIM, 33, NULL);

    ShowWindow(g_hMain, show ? show : SW_SHOWNORMAL);
    UpdateWindow(g_hMain);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hWorker) { CloseHandle(g_hWorker); g_hWorker = NULL; }
    return (int)msg.wParam;
}
