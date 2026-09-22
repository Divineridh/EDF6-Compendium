"""Huella de verdad conocida, para localizar la tabla de armas obtenidas en memoria.

Sale de categorias que en la pantalla de equipamiento se ven COMPLETAS (terminan
con espacio en blanco), asi que lo listado es exactamente lo obtenido y todo lo
demas de esa categoria es un cero garantizado.

Fuente: captura del 30-ago-2026, Air Raider.
"""

import json
import os

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
CATALOGO = os.path.join(os.path.dirname(RAIZ), "EDF6-UI", "build", "catalog.json")
SALIDA = os.path.join(RAIZ, "build", "fingerprint.txt")

# categoria -> nombres obtenidos (el resto de la categoria cuenta como no obtenida)
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

    # Las clases estan entrelazadas en WEAPONTABLE, asi que ademas del indice
    # global emitimos el ordinal dentro de la clase: si el juego guarda un array
    # por clase, ese es el espacio de indices que usa.
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
            raise SystemExit("categoria %d no cierra" % categoria)
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
        print("%-7s %d restricciones (%d obtenidas, %d faltantes), indices %d..%d" % (
            espacio, len(sub), tengo, len(sub) - tengo,
            min(f[1] for f in sub), max(f[1] for f in sub)))


if __name__ == "__main__":
    main()
