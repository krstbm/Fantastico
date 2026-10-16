#!/usr/bin/env bash
# =====================================================================
#  LaptopPrep - build script (MinGW-w64, Linux or MSYS2)
#
#      ./build.sh all        the windowed console (LaptopPrep.exe)
#      ./build.sh console    the headless build (LaptopPrep-console.exe)
#      ./build.sh both       both of the above
#      ./build.sh test       build + run the update decision tests (host gcc)
#      ./build.sh clean      remove the build directory and the exes
#
#  Cross compiling from Linux:
#      sudo apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64
#      GCC=x86_64-w64-mingw32-gcc WINDRES=x86_64-w64-mingw32-windres ./build.sh both
#
#  Both exes land in the project root, next to assets/ and fonts/, which
#  is where the app looks for them.
# =====================================================================
set -u

GCC=${GCC:-gcc}
WINDRES=${WINDRES:-windres}
STRIP=${STRIP:-${GCC%-gcc}-strip}

SRC=src
BUILD=build
OUT_GUI=LaptopPrep.exe
OUT_CON=LaptopPrep-console.exe

# --- what the linker needs -------------------------------------------
#  ole32/oleaut32  the Windows Update Agent COM API
#  wbemuuid       WMI (hardware collection)
#  winhttp        the reachability probe and the log upload
#  ws2_32/iphlpapi  network adapters
#  wtsapi32       the logged-on session question
#  gdiplus        every anti-aliased shape in the interface
LIBS="-lole32 -loleaut32 -lwbemuuid -lwinhttp -lws2_32 -liphlpapi \
      -lwtsapi32 -ladvapi32 -lshell32 -lcomctl32 -lgdiplus -lgdi32 -luser32 -lpsapi"

CFLAGS_COMMON="-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter \
 -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -I $SRC"

# --- does this gcc know -municode? -----------------------------------
#  The GUI uses the Unicode entry point (wWinMain), which mingw-w64 turns
#  on with -municode.  The old MinGW.org compiler and Cygwin's gcc reject
#  that flag, so probe once and fall back to the WinMain entry point
#  (-DLP_ANSI_ENTRY) when it is missing - same program either way.
MUNICODE="-municode"
ENTRYDEF=""
if ! echo 'int main(void){return 0;}' > /tmp/lp_probe.c 2>/dev/null ||
   ! "$GCC" -municode -c /tmp/lp_probe.c -o /tmp/lp_probe.o >/dev/null 2>&1; then
    MUNICODE=""
    ENTRYDEF="-DLP_ANSI_ENTRY"
    echo "  note: this gcc has no -municode - using the WinMain entry point"
fi
rm -f /tmp/lp_probe.c /tmp/lp_probe.o

# --- can this gcc build the project at all? --------------------------
#  tools/build_probe.c includes every SDK header the project uses and
#  takes one symbol from every library it links.  A compiler without the
#  WMI / WinHTTP / GDI+ headers (MinGW.org, Cygwin) stops here with one
#  clear message instead of dying halfway through $SRC/prep_core.c.
if [ -f tools/build_probe.c ]; then
    if ! $GCC tools/build_probe.c -o /tmp/lp_sdk_probe.exe $CFLAGS_COMMON $LIBS \
         > /tmp/lp_sdk_probe.log 2>&1; then
        echo
        echo "  This gcc cannot build LaptopPrep:"
        echo "    $($GCC -dumpmachine 2>/dev/null)"
        sed 's/^/      /' /tmp/lp_sdk_probe.log | head -6
        echo "  It needs the WMI, WinHTTP, GDI+ and psapi headers that ship"
        echo "  with mingw-w64.  Use a MinGW-w64 toolchain, or run the built"
        echo "  LaptopPrep.exe / LaptopPrep-console.exe that shipped with this"
        echo "  folder - nothing has to be compiled to use those."
        rm -f /tmp/lp_sdk_probe.log
        exit 1
    fi
    rm -f /tmp/lp_sdk_probe.exe /tmp/lp_sdk_probe.log
fi

# prep_wu.c MUST be on both link lines: it holds the update decision layer
CORE="$SRC/prep_core.c $SRC/prep_wu.c"

msg()  { printf '  %s\n' "$*"; }
fail() { printf '\nBUILD FAILED: %s\n' "$*" >&2; exit 1; }

need_compiler() {
    command -v "$GCC" >/dev/null 2>&1 || fail "$GCC not found.
  On Linux:  sudo apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64
  On MSYS2:  pacman -S mingw-w64-x86_64-toolchain"
    mkdir -p "$BUILD"
}

# --- resources: icon + version info + the DPI manifest ---------------
build_res() {
    [ -f "$SRC/prep.rc" ] || fail "src/prep.rc is missing"
    if [ ! -f "$SRC/laptopprep.ico" ]; then
        msg "src/laptopprep.ico is missing - generating it"
        python3 tools/make_icon.py || fail "could not generate the icon"
    fi
    msg "resources -> $BUILD/prep.res"
    $WINDRES -I "$SRC" "$SRC/prep.rc" -O coff -o "$BUILD/prep.res" \
        || fail "windres could not compile src/prep.rc"
}

# --- copy the optional art and fonts next to the exe -----------------
stage_assets() {
    if [ -f assets/logo_side.bmp ]; then
        cp -f assets/logo_side.bmp . 2>/dev/null
        msg "assets/logo_side.bmp copied to the project root"
    fi
    if [ -f assets/logo_about.bmp ]; then
        cp -f assets/logo_about.bmp . 2>/dev/null
        msg "assets/logo_about.bmp copied to the project root"
    fi
    if compgen -G "assets/fonts/*.ttf" >/dev/null 2>&1; then
        mkdir -p fonts
        cp -f assets/fonts/*.ttf fonts/ 2>/dev/null
        msg "fonts copied to ./fonts ($(ls fonts/*.ttf 2>/dev/null | wc -l) files)"
    else
        msg "no fonts/*.ttf found - the interface uses the Windows system"
        msg "  font (Segoe UI Variable on Windows 11). To use the bundled"
        msg "  typography instead: python3 tools/fetch-fonts.py"
    fi
}

build_gui() {
    msg "compiling $OUT_GUI (windowed, GDI+)"
    $GCC $CFLAGS_COMMON $CORE "$SRC/prep_gui.c" "$BUILD/prep.res" \
         $ENTRYDEF -o "$OUT_GUI" $MUNICODE -mwindows -Wl,--subsystem,windows -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,0 $LIBS \
        || fail "$OUT_GUI did not link"
    $STRIP "$OUT_GUI" 2>/dev/null
    msg "$OUT_GUI  ->  $(du -h "$OUT_GUI" | cut -f1)"
}

build_console() {
    msg "compiling $OUT_CON (headless, usable from a script)"
    $GCC $CFLAGS_COMMON $CORE "$SRC/console_main.c" "$BUILD/prep.res" \
         -o "$OUT_CON" $LIBS \
        || fail "$OUT_CON did not link"
    $STRIP "$OUT_CON" 2>/dev/null
    msg "$OUT_CON  ->  $(du -h "$OUT_CON" | cut -f1)"
}

run_tests() {
    local tcc=${HOST_GCC:-gcc}
    msg "building the update decision tests with $tcc (no Windows needed)"
    $tcc -std=c99 -Wall -Wextra -O1 -I "$SRC" tests/wu_decide_test.c \
         "$SRC/prep_wu.c" -o "$BUILD/wu_decide_test" \
        || fail "the test build failed"
    "$BUILD/wu_decide_test" || fail "the update decision tests failed"
}

case "${1:-all}" in
all)
    need_compiler; build_res; build_gui; stage_assets
    msg ""
    msg "done: ./$OUT_GUI"
    msg "log goes to C:\\RefurbLogs\\<COMPUTERNAME>_prep.log"
    ;;
console)
    need_compiler; build_res; build_console
    ;;
both)
    need_compiler; build_res; build_gui; build_console; stage_assets
    msg ""
    msg "done: ./$OUT_GUI and ./$OUT_CON"
    ;;
test)
    mkdir -p "$BUILD"; run_tests
    ;;
clean)
    rm -rf "$BUILD" "$OUT_GUI" "$OUT_CON"
    msg "removed $BUILD/, $OUT_GUI, $OUT_CON"
    ;;
*)
    sed -n '2,20p' "$0"
    exit 1
    ;;
esac
