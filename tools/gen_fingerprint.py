"""Known ground truth, to locate the owned-weapons table in memory.

It comes from categories that look COMPLETE on the equipment screen (they end
with blank space), so what's listed is exactly what's owned and everything else
in that category is a guaranteed zero.

Source: capture from 30-Aug-2026, Air Raider.
"""

import json
import os

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
CATALOGO = os.path.join(os.path.dirname(RAIZ), "EDF6-UI", "build", "catalog.json")
SALIDA = os.path.join(RAIZ, "build", "fingerprint.txt")

# category -> owned names (the rest of the category counts as not owned)
VERDAD = {
    302: [
        "Life Vendor", "Electromagnetic Bunker", "Guard Post", "Life Vendor M2",
        "Decoy [Green]", "Power Post", "Anti-Aircraft Bunker", "Life Vendor M3",
        "Guard Post M2", "Heavy Bunker", "Power Post M2", "Decoy [Blue]",
        "Zone Protector", "Electromagnetic Bunker M2",
    ],
    305: [
        "Laser Guide Kit", "Guide Beacon Gun", "Suppress Gun",
        "High-Speed Laser Guide Kit", "Guide Beacon Gun M2", "Suppress Gun B2",
        "Suppress Gun GA", "Laser Guide Kit T2", "High Output Beacon Gun",
        "Guide Beacon Gun M3", "Laser Guide Kit T3", "Suppress Gun MH",
    ],
}


def main():
    catalogo = json.load(open(CATALOGO, encoding="utf-8"))

    # Classes are interleaved in WEAPONTABLE, so besides the global index we also
    # emit the ordinal within the class: if the game keeps one array per class,
    # that's the index space it uses.
    ordinal = {}
    for clase in {w["class"] for w in catalogo}:
        for n, w in enumerate(sorted([x for x in catalogo if x["class"] == clase],
                                     key=lambda x: x["index"])):
            ordinal[w["index"]] = n

    filas = []
    for categoria, obtenidas in VERDAD.items():
        armas = [w for w in catalogo if w["category"] == categoria]
        faltan = [w["name"] for w in armas if w["name"] not in obtenidas]
        if len(armas) != len(obtenidas) + len(faltan):
            raise SystemExit("category %d doesn't add up" % categoria)
        for w in armas:
            tiene = 1 if w["name"] in obtenidas else 0
            filas.append(("global", w["index"], tiene, w["name"]))
            filas.append(("clase", ordinal[w["index"]], tiene, w["name"]))

    filas.sort()
    os.makedirs(os.path.dirname(SALIDA), exist_ok=True)
    with open(SALIDA, "w", encoding="utf-8", newline="\n") as fh:
        for espacio, index, tiene, nombre in filas:
            fh.write("%s\t%d\t%d\t%s\n" % (espacio, index, tiene, nombre))

    for espacio in ("global", "clase"):
        sub = [f for f in filas if f[0] == espacio]
        tengo = sum(1 for f in sub if f[2])
        print("%-7s %d constraints (%d owned, %d missing), indices %d..%d" % (
            espacio, len(sub), tengo, len(sub) - tengo,
            min(f[1] for f in sub), max(f[1] for f in sub)))


if __name__ == "__main__":
    main()
