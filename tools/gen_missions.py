"""Tabla de dropeo por mision, desde el Equipment Farming Tool de Beardmo.

https://docs.google.com/spreadsheets/d/17KuXJJOhsRqB0Fi82DLdd0p5hU79Z_OcLRGp8_xTF1A

La pestana "Mission Data" trae, por mision y dificultad, la ventana de niveles de
arma que puede dropear y la probabilidad por caja.

Regla de elegibilidad: `min <= nivel <= max`, en las cuatro dificultades.

OJO: la hoja lista, en su columna de Inferno, misiones donde el arma no puede
caer — se comporta como si en Inferno no hubiera techo. Es un error de la hoja.
La prueba es aritmetica y no depende de creerle a nadie: como cada caja elige
uniforme del pool, `1 / probabilidad` tiene que dar la cantidad de armas del
rango. Con el techo real coincide exacto en 187 de las 202 filas de Inferno
(376 y 376, 362 y 362, 351 y 351...); ignorando el techo da 977 contra 357.

Calibrar contra la salida de la hoja habria propagado el error; calibrar contra
el tamano del pool lo detecta.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import xlsx

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBRO = os.path.join(RAIZ, "data", "finder.xlsx")
SALIDA = os.path.join(RAIZ, "build", "missions.tsv")

# etiqueta -> (col min, col max, col probabilidad)
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
    print("%s: %d combinaciones mision/dificultad, de %d misiones" % (
        SALIDA, len(filas), len(filas_hoja)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
