"""Builds ../builds/EDF6Compendium.zip from the compiled DLL, the generated data and package/.

Refuses to package when a source file is newer than the DLL, so a stale build never ships;
--anyway skips that check.
"""

import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DATA = os.path.join(ROOT, "data")
SOURCE = os.path.join(ROOT, "package")
STAGING = os.path.join(BUILD, "package")
OUTPUT = os.path.join(os.path.dirname(ROOT), "builds")
ZIP = os.path.join(OUTPUT, "EDF6Compendium.zip")
DLL = os.path.join(BUILD, "EDF6Compendium.dll")

CONTENTS = [
    (DLL, "Mods/Plugins/EDF6Compendium.dll"),
    (os.path.join(BUILD, "weapons.tsv"), "Mods/Compendium/weapons.tsv"),
    (os.path.join(DATA, "missions.tsv"), "Mods/Compendium/missions.tsv"),
    (os.path.join(BUILD, "strats.tsv"), "Mods/Compendium/strats.tsv"),
    (os.path.join(SOURCE, "config.ini"), "Mods/Compendium/config.ini"),
    (os.path.join(SOURCE, "README.txt"), "README.txt"),
]


def newer_than_dll():
    if not os.path.exists(DLL):
        return ["(the DLL doesn't exist)"]
    cutoff = os.path.getmtime(DLL)
    stale = []
    for base, _, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            path = os.path.join(base, name)
            if os.path.getmtime(path) > cutoff:
                stale.append(os.path.relpath(path, ROOT))
    return sorted(stale)


def main():
    missing = [src for src, _ in CONTENTS if not os.path.exists(src)]
    if missing:
        raise SystemExit("missing files for the package:\n  " + "\n  ".join(missing))

    stale = newer_than_dll()
    if stale and "--anyway" not in sys.argv:
        raise SystemExit("the DLL is older than the code; run build.bat first:\n  " + "\n  ".join(stale))

    if os.path.exists(STAGING):
        shutil.rmtree(STAGING)
    for src, dest in CONTENTS:
        target = os.path.join(STAGING, dest.replace("/", os.sep))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(src, target)

    os.makedirs(OUTPUT, exist_ok=True)
    if os.path.exists(ZIP):
        os.remove(ZIP)
    with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for _, dest in CONTENTS:
            z.write(os.path.join(STAGING, dest.replace("/", os.sep)), dest)

    print(ZIP)
    for _, dest in CONTENTS:
        path = os.path.join(STAGING, dest.replace("/", os.sep))
        print("  %-44s %8d bytes" % (dest, os.path.getsize(path)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
