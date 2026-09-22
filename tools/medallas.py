"""Completa las medallas de dificultad de las misiones ya ganadas en Inferno.

Cada archivo .MST del save trae un array de un byte por mision, donde los bits
son las dificultades completadas:

    bit 0  Easy      bit 1  Normal    bit 2  Hard
    bit 3  Hardest   bit 4  Inferno

Si ganaste una mision en Inferno, las otras cuatro son un tramite; esto las
marca. El array vive en 0x041C en los tres .MST, que tienen el mismo tamaño y
layout; solo se tocan los bytes que YA tienen el bit de Inferno, asi que no hace
falta saber cuantas misiones trae cada campaña.

OJO: los .MST llevan un checksum en 0x0C que no supimos reproducir. Si el juego
lo verifica al cargar, un save editado se rechaza. Por eso el modo por defecto
es simulacion: mostra que cambiaria y no toca nada. Con --aplicar escribe, y
siempre deja un .bak al lado.
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
        raise SystemExit("no encontre ningun saveslot")
    return max(slots)[1]


ARRAY = 0x041C
VENTANA = 256


def nombres_de_mision():
    ruta = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "build", "missions.tsv")
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
    aplicar = "--aplicar" in sys.argv
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
            print("%-16s no descifro bien, lo salteo" % nombre)
            continue
        nuevo, cambios, ganadas = completar(claro)
        print("%-16s %d misiones con alguna dificultad hecha" % (nombre, ganadas))
        for indice, antes, faltaban in cambios:
            titulo = nombres.get(str(indice + 1), "")
            print("   mision %-4d %-34s 0x%02X -> 0x1F   suma %s" % (
                indice + 1, titulo[:34], antes, ", ".join(faltaban)))
        if not cambios:
            print("   nada que completar")
            continue
        total += len(cambios)
        if aplicar:
            shutil.copyfile(ruta, ruta + ".bak")
            open(ruta, "wb").write(ctr_xor(nuevo, key, iv))
            print("   escrito (copia en %s.bak)" % nombre)
    print()
    if not aplicar:
        print("simulacion: no se toco nada. Con --aplicar se escribe.")
    else:
        print("%d misiones completadas. Si el juego rechaza el save, restaura los .bak" % total)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
