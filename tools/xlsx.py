"""Minimal xlsx reader: sharedStrings + cells, no dependencies."""

import re
import sys
import zipfile
import xml.etree.ElementTree as ET

NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"


def col_index(ref):
    letras = re.match(r"([A-Z]+)", ref).group(1)
    n = 0
    for c in letras:
        n = n * 26 + (ord(c) - 64)
    return n - 1


def load(path):
    z = zipfile.ZipFile(path)
    wbroot = ET.fromstring(z.read("xl/workbook.xml"))
    nombres = [s.get("name") for s in wbroot.iter(NS + "sheet")]

    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        root = ET.fromstring(z.read("xl/sharedStrings.xml"))
        for si in root:
            shared.append("".join(t.text or "" for t in si.iter(NS + "t")))

    hojas = {}
    for i, nombre in enumerate(nombres, start=1):
        ruta = "xl/worksheets/sheet%d.xml" % i
        if ruta not in z.namelist():
            continue
        root = ET.fromstring(z.read(ruta))
        filas = []
        for row in root.iter(NS + "row"):
            celdas = {}
            for c in row.iter(NS + "c"):
                ref = c.get("r", "")
                if not ref:
                    continue
                tipo = c.get("t")
                v = c.find(NS + "v")
                texto = ""
                if tipo == "s" and v is not None:
                    texto = shared[int(v.text)]
                elif tipo == "inlineStr":
                    is_ = c.find(NS + "is")
                    texto = "".join(t.text or "" for t in is_.iter(NS + "t")) if is_ is not None else ""
                elif v is not None:
                    texto = v.text or ""
                celdas[col_index(ref)] = texto
            if celdas:
                ancho = max(celdas) + 1
                filas.append([celdas.get(j, "") for j in range(ancho)])
        hojas[nombre] = filas
    return hojas


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    hojas = load(sys.argv[1])
    objetivo = sys.argv[2] if len(sys.argv) > 2 else None
    for nombre, filas in hojas.items():
        if objetivo and objetivo.lower() not in nombre.lower():
            continue
        print("=== %s: %d rows ===" % (nombre, len(filas)))
        for f in filas[:int(sys.argv[3]) if len(sys.argv) > 3 else 6]:
            print("  ", f[:14])
