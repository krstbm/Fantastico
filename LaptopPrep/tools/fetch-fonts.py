#!/usr/bin/env python3
"""
fetch-fonts.py - downloads the open fonts LaptopPrep prefers.

The interface works without this: with no fonts next to the exe it uses
Segoe UI Variable (Windows 11) or Segoe UI (Windows 10), which is what
Windows itself uses.  Installing these three makes it look the way it was
designed, and they are all OFL licensed, so shipping them is fine:

    Inter                body text            (Rasmus Andersson, OFL)
    Space Grotesk        headings, the numbers (Florian Karsten, OFL)
    JetBrains Mono       the live log          (JetBrains, OFL)

They are copied into assets/fonts/ (the source of truth) and the build
copies them next to the exe as fonts/.  Run once:

    python3 tools/fetch-fonts.py
"""

import io
import os
import sys
import urllib.request
import zipfile

INTER = ("https://github.com/rsms/inter/releases/download/v4.0/"
         "Inter-4.0.zip")
GROTESK = ("https://github.com/floriankarsten/space-grotesk/releases/download/"
           "2.0.0/SpaceGrotesk-2.0.0.zip")
MONO = ("https://github.com/JetBrains/JetBrainsMono/releases/download/"
        "v2.304/JetBrainsMono-2.304.zip")

WANT = [
    (INTER, ["extras/ttf/InterVariable.ttf",
             "extras/ttf/InterDisplay-Regular.ttf",
             "extras/ttf/Inter-Regular.ttf",
             "extras/ttf/Inter-Medium.ttf",
             "extras/ttf/Inter-SemiBold.ttf",
             "InterVariable.ttf", "Inter-Regular.ttf", "Inter-SemiBold.ttf",
             "Inter-Medium.ttf"]),
    (GROTESK, ["fonts/ttf/SpaceGrotesk-Regular.ttf",
               "fonts/ttf/SpaceGrotesk-Bold.ttf",
               "fonts/ttf/SpaceGrotesk-Medium.ttf",
               "SpaceGrotesk-Regular.ttf", "SpaceGrotesk-Bold.ttf",
               "SpaceGrotesk-Medium.ttf"]),
    (MONO, ["fonts/ttf/JetBrainsMono-Regular.ttf",
            "JetBrainsMono-Regular.ttf"]),
]


def fetch(url):
    print("  downloading %s" % url.split("/")[-1])
    req = urllib.request.Request(url, headers={"User-Agent": "LaptopPrep-build"})
    with urllib.request.urlopen(req, timeout=90) as r:
        return r.read()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "fonts")
    os.makedirs(out, exist_ok=True)
    got = 0

    for url, names in WANT:
        try:
            blob = fetch(url)
            zf = zipfile.ZipFile(io.BytesIO(blob))
        except Exception as e:                       # noqa: BLE001
            print("  ! %s" % e)
            print("    (no internet? the app falls back to the system font)")
            continue
        wanted = {n.split("/")[-1]: n for n in names}
        for entry in zf.namelist():
            base = entry.split("/")[-1]
            if base in wanted and base.lower().endswith(".ttf"):
                with open(os.path.join(out, base), "wb") as f:
                    f.write(zf.read(entry))
                print("    -> assets/fonts/%s" % base)
                got += 1

    if got:
        print("\n%d font files in assets/fonts/." % got)
        print("Now rebuild (build.bat all / ./build.sh all) - fonts/ next to the")
        print("exe is what the app loads, as a private font so nothing is installed.")
    else:
        print("\nNothing downloaded. LaptopPrep still works: it will use the")
        print("Windows system font.")
    return 0 if got else 1


if __name__ == "__main__":
    sys.exit(main())
