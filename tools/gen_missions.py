"""Regenerates data/missions.tsv from Beardmo's Equipment Farming Tool.

The derived table is already in the repo; this is only needed to rebuild it.
The spreadsheet isn't redistributed: download it as .xlsx to data/finder.xlsx.

https://docs.google.com/spreadsheets/d/17KuXJJOhsRqB0Fi82DLdd0p5hU79Z_OcLRGp8_xTF1A

The "Mission Data" tab gives, per mission and difficulty, the window of weapon
levels it can drop and the chance per crate.

Eligibility rule: `min <= level <= max`, on all four difficulties.

CAREFUL: in its Inferno column the sheet lists missions where the weapon can't
drop; it behaves as if Inferno had no ceiling. That's a mistake in the sheet.
The proof is arithmetic and doesn't depend on trusting anyone: since each crate
picks uniformly from the pool, `1 / chance` has to equal the number of weapons
in the range. With the real ceiling it matches exactly in 187 of the 202 Inferno
rows (376 and 376, 362 and 362, 351 and 351...); ignoring it gives 977 against 357.

Calibrating against the sheet's output would have spread the mistake;
calibrating against the pool size catches it.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import xlsx

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBRO = os.path.join(RAIZ, "data", "finder.xlsx")
SALIDA = os.path.join(RAIZ, "data", "missions.tsv")

# label -> (min column, max column, chance column)
DIFICULTADES = [
    ("Normal", 3, 4, 11),
    ("Hard", 5, 6, 12),
    ("Hardest", 7, 8, 13),
    ("Inferno", 9, 10, 14),
]


def tier(ident):
    if ident.startswith("DLC2"):
        return 2
    if ident.startswith("DLC1"):
        return 1
    return 0


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main():
    hojas = xlsx.load(LIBRO)
    filas_hoja = [f for f in hojas["Mission Data"]
                  if len(f) > 14 and f[1] not in ("", "Mission number")]

    filas = []
    for f in filas_hoja:
        ident = f[1].replace(".0", "") if f[1].endswith(".0") else f[1]
        nombre = f[2]
        for etiqueta, cmin, cmax, cprob in DIFICULTADES:
            lo, hi, prob = num(f[cmin]), num(f[cmax]), num(f[cprob])
            if lo is None or hi is None or prob is None or lo > 900 or prob <= 0:
                continue
            filas.append("\t".join([
                etiqueta, ident, nombre,
                "%g" % lo, "%g" % hi,
                "%.6f" % prob, str(tier(ident)),
            ]))

    os.makedirs(os.path.dirname(SALIDA), exist_ok=True)
    with open(SALIDA, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(filas) + "\n")
    print("%s: %d mission/difficulty combinations, from %d missions" % (
        SALIDA, len(filas), len(filas_hoja)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
