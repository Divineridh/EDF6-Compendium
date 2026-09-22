"""Genera un campo de tiro reemplazando la mision 1 (el prologo).

Por que el prologo:
  - Es la unica mision jugable que no esta pisada por el mod de misiones instalado.
  - En la tabla de dropeo es la unica con rangos vacios: por diseno no suelta
    items, asi que probar cosas ahi no ensucia la coleccion.
  - Su mapa es el interior de la base de la EDF y ya tiene ocho puntos de spawn
    llamados 射撃的1..8, que significa literalmente "blanco de tiro". El prologo
    de EDF6 es el tutorial de puntería: ya era un campo de tiro.

El andamiaje de AngelScript se toma tal cual de M104 —la mision mas simple del
juego, dos eventos— y solo se reemplazan el mapa, los preloads y los spawns. Asi
la estructura que espera el motor queda intacta.

El evento de cierre original se conserva: matando todo la mision termina, lo que
sirve para reiniciar rapido.
"""

import os
import re
import shutil
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLANTILLA = os.path.join(os.path.dirname(RAIZ), "EDF6-UI", "extract", "MISSION",
                         "EDF6_DLC", "M104", "MISSION.AC")
SALIDA = os.path.join(RAIZ, "build", "MISSION", "EDF6", "M000B", "MISSION.AC")

# Los puntos de spawn no vienen del mapa sino del MISSION.RMPA de cada mision, y
# ese archivo tambien pasa por el redirector. Asi que para cambiar de escenario
# alcanza con copiar el RMPA de otra mision: nos quedamos con sus puntos, ya
# ubicados donde corresponde.
ESCENARIOS = {
    # El prologo: interior de la base, ocho puntos llamados "blanco de tiro".
    # Corto pero con paredes y cobertura.
    "base": {
        "mapa": "app:/map/ig_EDFBasement01.mac",
        "clima": "fine",
        "jugador": "プレイヤー",
        "puntos": ["射撃的%d" % i for i in range(1, 9)],
        "puertas": True,
        "rmpa": None,
    },
    # M514: llanura abierta con 19 puntos individuales de enemigo terrestre.
    #
    # Importante que sean PUNTOS y no areas: CreateEnemy solo acepta puntos y
    # devuelve el objeto, que es lo que permite frenarle la IA a cada blanco. El
    # primer intento uso un area y no spawneo nada — la mision se autocompletaba
    # porque el evento de cierre es "todos los enemigos destruidos" y no habia
    # ninguno.
    "campo": {
        "mapa": "app:/map/ig_Heigen601.mac",
        "clima": "cloudy2",   # cada mapa acepta sus climas; este no tiene "fine"
        "jugador": "プレイヤー",
        "puntos": (["敵06_%d" % i for i in range(1, 11)] +
                   ["敵08_%d" % i for i in range(1, 6)] +
                   ["敵11_%d" % i for i in range(1, 5)]),
        "puertas": False,
        "rmpa": os.path.join(os.path.dirname(RAIZ), "EDF6-UI", "extract", "MISSION",
                             "EDF6_DLC", "M514", "MISSION.RMPA"),
    },
}

ENEMIGOS = {
    "hormiga": "app:/object/e514_dango.sgo",
    "hormiga-grande": "app:/object/e660_heavy_dango_M.sgo",
    "hormiga-gigante": "app:/object/e665_giantant_medium.sgo",
    "arana": "app:/object/e667_spider_medium.sgo",
    "abeja": "app:/object/e668_giantbee_medium.sgo",
    "marciano": "app:/object/e601_martian_gs.sgo",
    "rana-asalto": "app:/object/e503_frog_af.sgo",
    "rana-escopeta": "app:/object/e503_frog_sg.sgo",
    "rana-blindada": "app:/object/e503_armorfrog_af.sgo",
}


# Las puertas del prologo las abre un NPC guionado que nosotros no creamos, asi
# que sin esto quedas encerrado en la sala inicial. Se crean y se abren con
# Action(0), que es exactamente lo que hace el script original.
PUERTAS = ([("ドア%d" % i, "app:/object/Basement_Door.sgo") for i in (1, 2, 3)] +
           [("人用ドア%d" % i, "app:/object/Basement_humanDoor.sgo") for i in range(1, 7)])


def puertas_abiertas():
    lineas = ["\t\t// puertas abiertas: sin esto no se sale de la sala inicial"]
    for n, (punto, sgo) in enumerate(PUERTAS):
        lineas.append('\t\tObject puerta%d = CreateNeutral("%s", "%s", 1.0f);' % (n, punto, sgo))
        lineas.append('\t\tpuerta%d.Action(0);' % n)
    return "\n".join(lineas) + "\n"


def spawns(sgo, nivel, puntos, invencible):
    """Un blanco por punto, quieto. Se usa CreateEnemy y no CreateEnemyGroup
    porque devuelve el objeto y permite pararle la IA uno por uno."""
    lineas = ["\t\t// blancos: quietos, para probar armas contra algo que no se mueve"]
    for n, punto in enumerate(puntos):
        lineas.append(
            '\t\tObject blanco%d = CreateEnemy(\n'
            '\t\t\t"%s",\t// punto\n'
            '\t\t\t"%s",\t// sgo\n'
            '\t\t\t%.1ff,\t// nivel: infla la vida\n'
            '\t\t\tfalse\t// arranca sin haberte visto\n'
            '\t\t\t);\n'
            '\t\tblanco%d.SetAiMoveSpeed(0.0f);' % (n, punto, sgo, nivel, n))
        if invencible:
            lineas.append('\t\tblanco%d.SetObjectInvincible(true);' % n)
    return "\n".join(lineas) + "\n"


def generar(esc, sgo, nivel, puntos, invencible):
    base = open(PLANTILLA, encoding="utf-8-sig").read()

    # preloads: el mapa y el modelo de enemigo que vamos a usar
    base = re.sub(r'PreloadMap\("[^"]+", "[^"]+", -1\);',
                  'PreloadMap("%s", "%s", -1);' % (esc["mapa"], esc["clima"]), base)
    base = re.sub(r'\tPreload\("app:/object/[^"]+", -1\);\n', "", base)
    extra = '\tPreload("%s", -1);\n' % sgo
    if esc["puertas"]:
        extra += ('\tPreload("app:/object/Basement_Door.sgo", -1);\n'
                  '\tPreload("app:/object/Basement_humanDoor.sgo", -1);\n')
    base = base.replace('\tPreloadPlayerResource();', extra + '\tPreloadPlayerResource();')

    # cuerpo: del Map() hasta justo antes del cierre del bloque de spawns
    cuerpo = ('\t\tMap("%s","%s");\n\n'
              '\t\tCreatePlayer("%s");\n\n\n' % (esc["mapa"], esc["clima"], esc["jugador"]))
    if esc["puertas"]:
        cuerpo += puertas_abiertas() + "\n"
    cuerpo += spawns(sgo, nivel, puntos, invencible)

    ini = base.index('\t\tMap("app:/map/')
    fin = base.index('\t}\n\t// -----------------')
    base = base[:ini] + cuerpo + "\n" + base[fin:]
    return base


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("enemigo", nargs="?", default="hormiga", choices=sorted(ENEMIGOS))
    p.add_argument("--nivel", type=float, default=60.0,
                   help="infla la vida del blanco; 1 es un enemigo normal")
    p.add_argument("--invencible", action="store_true",
                   help="vida infinita en vez de inflada")
    p.add_argument("--escenario", default="campo", choices=sorted(ESCENARIOS),
                   help="campo = mapa plano con blancos a 330m; base = interior del prologo")
    p.add_argument("--blancos", type=int, default=8, help="cuantos blancos crear")
    args = p.parse_args()

    esc = ESCENARIOS[args.escenario]
    sgo = ENEMIGOS[args.enemigo]
    cuantos = max(1, min(20, args.blancos))
    disponibles = esc["puntos"]
    # Si pedis mas blancos que puntos disponibles, se reutilizan y quedan
    # apilados en el mismo lugar.
    puntos = [disponibles[i % len(disponibles)] for i in range(cuantos)]
    texto = generar(esc, sgo, args.nivel, puntos, args.invencible)

    os.makedirs(os.path.dirname(SALIDA), exist_ok=True)
    with open(SALIDA, "w", encoding="utf-8-sig", newline="\n") as fh:
        fh.write(texto)

    # El RMPA define los puntos de spawn: si el escenario no es el del prologo,
    # hay que llevarse tambien el de la mision donante.
    destino_rmpa = os.path.join(os.path.dirname(SALIDA), "MISSION.RMPA")
    if esc["rmpa"]:
        shutil.copyfile(esc["rmpa"], destino_rmpa)
    elif os.path.exists(destino_rmpa):
        os.remove(destino_rmpa)

    print("%s\n  escenario %s, %d blancos de %s, %s%s" % (
        SALIDA, args.escenario, len(puntos), args.enemigo,
        "invencibles" if args.invencible else "nivel %g" % args.nivel,
        "\n  + MISSION.RMPA copiado de la mision donante" if esc["rmpa"] else ""))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
