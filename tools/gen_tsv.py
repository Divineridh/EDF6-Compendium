"""Genera el weapons.tsv que lee el plugin.

Fuente: el catalogo extraido del cpk por EDF6-UI/tools/weapons.py.
El estado de obtenidas sale de un obtenidas.txt (ver EDF6-UI/tools/obtenidas.py para
el orden de busqueda); mientras no exista, se marca por nivel para poder ver los dos
colores en pantalla. El plugin ya no lo escribe: lee el save solo, y esta columna es
el respaldo para cuando el save no se puede leer.
"""

import json
import os
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
UI = os.path.join(os.path.dirname(RAIZ), "EDF6-UI")
CATALOGO = os.path.join(UI, "build", "catalog.json")
CATEGORIAS = os.path.join(UI, "build", "categories.json")
RESPALDO_OBTENIDAS = os.path.join(RAIZ, "build", "obtenidas.txt")
SALIDA = os.path.join(RAIZ, "build", "weapons.tsv")

WEAPON_TEXT = os.path.join(UI, "extract", "WEAPON", "WEAPON", "WEAPONTEXT.EN.SGO")

sys.path.insert(0, os.path.join(UI, "tools"))

import obtenidas
from dsgo import Dsgo

WEAPON_LIST_RECORD = 1
STAT_SEPARATOR = ";"
FIELD_SEPARATOR = "|"


# Every upgradable value in WEAPONTEXT is a 7-number group: base (the value at star 5), stat type,
# save byte, max level, the two curve coefficients and whether it is fractional. The plugin needs
# the raw group to compute the value at any star, so it goes into its own column:
# label|template|group|group;label|template... with the group's numbers comma-separated.
def star_specs():
    text = Dsgo(open(WEAPON_TEXT, "rb").read())
    specs = []
    for i in text.children(WEAPON_LIST_RECORD):
        stats = []
        for stat in text.record(i)[2]:
            label, template = str(stat[0]), str(stat[1])
            for part in (label, template):
                if any(sep in part for sep in (STAT_SEPARATOR, FIELD_SEPARATOR, "\t", "\n")):
                    raise SystemExit("separator inside a stat text: %r" % part)
            groups = [",".join("%g" % float(v) for v in group) for group in stat[2:]]
            stats.append(FIELD_SEPARATOR.join([label, template] + groups))
        specs.append(STAT_SEPARATOR.join(stats))
    return specs

ORDEN = ["Ranger", "Wing Diver", "Fencer", "Air Raider"]

# Entradas de la WEAPONTABLE que no son armas: son el hueco "sin equipar" de cada
# clase. Contarlas inflaba los totales y el pool de toda mision de nivel 0.
NO_SON_ARMAS = {"Not Equipped", "No Equipment"}

# Los nombres de categoria salen de las claves internas de CONFIG.SGO, que traen
# los typos de Sandlot (Weapon_Heavy_Sheild, Weapon_MissleLauncher...).
TYPOS_CATEGORIA = {
    "Missle": "Missile",
    "Sheild": "Shield",
    "Horming": "Homing",
    "Actuater": "Actuator",
}


def corregir_categoria(nombre):
    return " ".join(TYPOS_CATEGORIA.get(p, p) for p in nombre.split(" "))


# Las cajas de una mision no sortean sobre todo el catalogo: las del juego base
# solo sueltan armas base, las de DLC1 suman el MissionPack A y las de DLC2 suman
# tambien el B. Un arma entra al pool si su tier <= el tier de la mision.
# Contrastado contra 1/probabilidad de la hoja: 745 de 808 filas dan exacto,
# contra 602 tratando el catalogo como un pool unico.
def tier(w):
    ident = str(w["id"]).upper()
    if ident.startswith("MPACK_B"):
        return 2
    if ident.startswith("MPACK_") or "DLC" in ident:
        return 1
    return 0


def main():
    catalogo = json.load(open(CATALOGO, encoding="utf-8"))
    sin_obtenidas = "--sin-obtenidas" in sys.argv
    if sin_obtenidas:
        ruta_obtenidas, tengo, placeholder = None, set(), False
    else:
        ruta_obtenidas, tengo = obtenidas.cargar(RESPALDO_OBTENIDAS)
        placeholder = tengo is None
        if placeholder:
            tengo = {w["name"] for w in catalogo if w["level"] <= 25}

    cats = json.load(open(CATEGORIAS, encoding="utf-8"))
    specs = star_specs()
    if len(specs) != len(catalogo):
        raise SystemExit("WEAPONTEXT has %d weapons and the catalog %d" % (len(specs), len(catalogo)))

    filas = []
    for clase in ORDEN:
        armas = [w for w in catalogo
                 if w["class"] == clase and w["name"] not in NO_SON_ARMAS]
        for w in sorted(armas, key=lambda x: (x["category"], x["level"])):
            cat = corregir_categoria(cats.get(str(w["category"]), {}).get("name", "cat %d" % w["category"]))
            stats = " | ".join("%s: %s" % (s["label"], s["text"]) for s in w["stats"])
            filas.append("\t".join([
                str(w["index"]), clase, str(w["category"]), cat,
                str(round(w["level"])), w["name"],
                "1" if w["name"] in tengo else "0",
                stats.replace("\t", " ").replace("\n", " "),
                ",".join(str(u) for u in w["upgrades"]),
                str(tier(w)),
                specs[w["index"]],
            ]))

    os.makedirs(os.path.dirname(SALIDA), exist_ok=True)
    with open(SALIDA, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(filas) + "\n")

    print("escrito %s: %d armas" % (SALIDA, len(filas)))
    if sin_obtenidas:
        print("  obtenidas: todas en 0 (--sin-obtenidas)")
    else:
        print(obtenidas.informe(ruta_obtenidas, None if placeholder else tengo, RESPALDO_OBTENIDAS))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
