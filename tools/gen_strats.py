"""Converts data/strats.txt to the TSV the plugin reads.

The source file is in readable blocks so anyone can add strategies without
touching code; here they are flattened to one line per strategy, with line
breaks escaped as \\n.
"""

import os
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUENTE = os.path.join(RAIZ, "data", "strats.txt")
SALIDA = os.path.join(RAIZ, "build", "strats.tsv")

CAMPOS = ("mision", "dificultad", "clase", "fuente")

# The source file is in English because it ships with the mod; both spellings
# of each field are accepted so old blocks don't break.
ALIAS = {
    "mission": "mision", "mision": "mision",
    "difficulty": "dificultad", "dificultad": "dificultad",
    "class": "clase", "clase": "clase",
    "source": "fuente", "fuente": "fuente",
}


def cabecera(linea):
    datos = {c: "any" for c in CAMPOS}
    datos["fuente"] = ""
    for parte in linea.lstrip("=").split("|"):
        if ":" not in parte:
            continue
        clave, valor = parte.split(":", 1)
        clave = ALIAS.get(clave.strip().lower())
        if clave:
            datos[clave] = valor.strip()
    return datos


def parsear(texto):
    entradas = []
    actual = None
    for linea in texto.splitlines():
        if linea.startswith("#"):
            continue
        if linea.startswith("=="):
            actual = cabecera(linea)
            actual["titulo"] = None
            actual["cuerpo"] = []
            entradas.append(actual)
            continue
        if actual is None:
            continue
        if actual["titulo"] is None:
            if linea.strip():
                actual["titulo"] = linea.strip()
            continue
        actual["cuerpo"].append(linea.rstrip())

    for e in entradas:
        cuerpo = "\n".join(e["cuerpo"]).strip()
        e["cuerpo"] = cuerpo
    return [e for e in entradas if e["titulo"]]


def main():
    entradas = parsear(open(FUENTE, encoding="utf-8").read())
    filas = []
    for e in entradas:
        filas.append("\t".join([
            e["mision"], e["dificultad"], e["clase"], e["titulo"], e["fuente"],
            e["cuerpo"].replace("\t", " ").replace("\n", "\\n"),
        ]))

    os.makedirs(os.path.dirname(SALIDA), exist_ok=True)
    with open(SALIDA, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(filas) + "\n")
    print("%s: %d strategies" % (SALIDA, len(filas)))
    for e in entradas:
        print("  [mission %-4s] %s" % (e["mision"], e["titulo"][:56]))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
