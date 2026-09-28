"""Fills in the difficulty medals of missions already won on Inferno.

Each .MST file of the save holds an array of one byte per mission, whose bits
are the completed difficulties:

    bit 0  Easy      bit 1  Normal    bit 2  Hard
    bit 3  Hardest   bit 4  Inferno

If you won a mission on Inferno, the other four are a formality; this marks
them. The array lives at 0x041C in the three .MST files, which share size and
layout; only bytes that ALREADY have the Inferno bit are touched, so there's no
need to know how many missions each campaign has.

CAREFUL: the .MST files carry a checksum at 0x0C that we couldn't reproduce. If
the game checks it on load, an edited save is rejected. That's why the default
mode is a dry run: it shows what would change and touches nothing. --apply
writes, and always leaves a .bak next to each file.
"""

import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from aes import ctr_xor
from savedec import claves

ARCHIVOS = ("DEFP_M00.MST", "DEFP_DLC1.MST", "DEFP_DLC2.MST")

INFERNO = 0x10
TODAS = 0x1F
NOMBRES = ("Easy", "Normal", "Hard", "Hardest", "Inferno")


def carpeta_save():
    base = os.path.join(os.environ["LOCALAPPDATA"], "EarthDefenceForce6", "SAVE_DATA")
    slots = []
    for cuenta in os.listdir(base):
        d = os.path.join(base, cuenta)
        if not os.path.isdir(d):
            continue
        for slot in os.listdir(d):
            s = os.path.join(d, slot)
            if slot.lower().startswith("saveslot") and os.path.isdir(s):
                slots.append((os.path.getmtime(os.path.join(s, "MAIN.GST")), s))
    if not slots:
        raise SystemExit("couldn't find any saveslot")
    return max(slots)[1]


ARRAY = 0x041C
VENTANA = 256


def nombres_de_mision():
    ruta = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "data", "missions.tsv")
    if not os.path.exists(ruta):
        return {}
    out = {}
    with open(ruta, encoding="utf-8") as fh:
        for linea in fh:
            campos = linea.rstrip("\n").split("\t")
            if len(campos) > 2:
                out[campos[1]] = campos[2]
    return out


def completar(datos):
    cambios = []
    salida = bytearray(datos)
    for i in range(ARRAY, min(ARRAY + VENTANA, len(datos))):
        v = datos[i]
        if v & INFERNO and v != TODAS:
            faltaban = [NOMBRES[b] for b in range(5) if not v & (1 << b)]
            cambios.append((i - ARRAY, v, faltaban))
            salida[i] = TODAS
    ganadas = sum(1 for i in range(ARRAY, min(ARRAY + VENTANA, len(datos))) if datos[i])
    return bytes(salida), cambios, ganadas


def main():
    aplicar = "--apply" in sys.argv
    slot = carpeta_save()
    nombres = nombres_de_mision()
    print("save: %s" % slot)
    print()
    total = 0
    for nombre in ARCHIVOS:
        ruta = os.path.join(slot, nombre)
        if not os.path.exists(ruta):
            continue
        key, iv = claves(nombre)
        claro = ctr_xor(open(ruta, "rb").read(), key, iv)
        if claro[:3] != b"MDB":
            print("%-16s didn't decrypt, skipping it" % nombre)
            continue
        nuevo, cambios, ganadas = completar(claro)
        print("%-16s %d missions with some difficulty done" % (nombre, ganadas))
        for indice, antes, faltaban in cambios:
            titulo = nombres.get(str(indice + 1), "")
            print("   mission %-4d %-34s 0x%02X -> 0x1F   adds %s" % (
                indice + 1, titulo[:34], antes, ", ".join(faltaban)))
        if not cambios:
            print("   nothing to fill in")
            continue
        total += len(cambios)
        if aplicar:
            shutil.copyfile(ruta, ruta + ".bak")
            open(ruta, "wb").write(ctr_xor(nuevo, key, iv))
            print("   written (copy in %s.bak)" % nombre)
    print()
    if not aplicar:
        print("dry run: nothing was touched. --apply writes.")
    else:
        print("%d missions filled in. If the game rejects the save, restore the .bak files" % total)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
