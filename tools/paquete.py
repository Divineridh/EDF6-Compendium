import os
import shutil
import sys
import zipfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(RAIZ, "build")
DATOS = os.path.join(RAIZ, "data")
FUENTE = os.path.join(RAIZ, "paquete")
DESTINO = os.path.join(BUILD, "paquete")
SALIDA = os.path.join(os.path.dirname(RAIZ), "builds")
ZIP = os.path.join(SALIDA, "EDF6Compendium.zip")
DLL = os.path.join(BUILD, "EDF6Compendium.dll")

CONTENIDO = [
    (DLL, "Mods/Plugins/EDF6Compendium.dll"),
    (os.path.join(BUILD, "weapons.tsv"), "Mods/Compendium/weapons.tsv"),
    (os.path.join(DATOS, "missions.tsv"), "Mods/Compendium/missions.tsv"),
    (os.path.join(BUILD, "strats.tsv"), "Mods/Compendium/strats.tsv"),
    (os.path.join(FUENTE, "config.ini"), "Mods/Compendium/config.ini"),
    (os.path.join(FUENTE, "LEEME.txt"), "LEEME.txt"),
]


def mas_nuevos_que_el_dll():
    if not os.path.exists(DLL):
        return ["(el DLL no existe)"]
    corte = os.path.getmtime(DLL)
    pendientes = []
    for base, _, archivos in os.walk(os.path.join(RAIZ, "src")):
        for a in archivos:
            ruta = os.path.join(base, a)
            if os.path.getmtime(ruta) > corte:
                pendientes.append(os.path.relpath(ruta, RAIZ))
    return sorted(pendientes)


def main():
    faltan = [o for o, _ in CONTENIDO if not os.path.exists(o)]
    if faltan:
        raise SystemExit("faltan archivos para armar el paquete:\n  " + "\n  ".join(faltan))

    pendientes = mas_nuevos_que_el_dll()
    if pendientes and "--igualmente" not in sys.argv:
        raise SystemExit(
            "el DLL es mas viejo que el codigo; corre build.bat primero:\n  "
            + "\n  ".join(pendientes))

    if os.path.exists(DESTINO):
        shutil.rmtree(DESTINO)
    for origen, relativo in CONTENIDO:
        destino = os.path.join(DESTINO, relativo.replace("/", os.sep))
        os.makedirs(os.path.dirname(destino), exist_ok=True)
        shutil.copy2(origen, destino)

    os.makedirs(SALIDA, exist_ok=True)
    if os.path.exists(ZIP):
        os.remove(ZIP)
    with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for _, relativo in CONTENIDO:
            z.write(os.path.join(DESTINO, relativo.replace("/", os.sep)), relativo)

    print("%s" % ZIP)
    for _, relativo in CONTENIDO:
        ruta = os.path.join(DESTINO, relativo.replace("/", os.sep))
        print("  %-44s %8d bytes" % (relativo, os.path.getsize(ruta)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
