"""Descifra los archivos de save de EDF6.

AES-256-CTR. La clave y el IV se derivan del nombre del archivo:

    clave = MD5(utf16le("edf6" + nombre + ".sav")) + b"Edf5.*_Steam_Ver"
    iv    = MD5(utf16le("edf6" + nombre + ".stm"))

El texto plano arranca con el magic "MDB".

Algoritmo publicado en EDFDecrypt.cpp del EDFSaveEditor de FevGrave, con credito
a Quarri6343 por haberlo descubierto. Reimplementado aca en Python para no
depender de un binario de terceros.
"""

import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from aes import ctr_xor

COLA_CLAVE = b"Edf5.*_Steam_Ver"


def claves(nombre, juego="edf6"):
    key = hashlib.md5(("%s%s.sav" % (juego, nombre)).encode("utf-16-le")).digest() + COLA_CLAVE
    iv = hashlib.md5(("%s%s.stm" % (juego, nombre)).encode("utf-16-le")).digest()
    return key, iv


def descifrar(ruta, nombre=None):
    nombre = nombre or os.path.basename(ruta)
    key, iv = claves(nombre)
    return ctr_xor(open(ruta, "rb").read(), key, iv)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    ruta = sys.argv[1]
    nombre = sys.argv[2] if len(sys.argv) > 2 else None
    datos = descifrar(ruta, nombre)
    magic = datos[:3]
    print("%s -> %d bytes, magic=%r %s" % (
        os.path.basename(ruta), len(datos), magic,
        "OK" if magic == b"MDB" else "<-- no descifro bien"))
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(datos)
        print("escrito", sys.argv[3])
