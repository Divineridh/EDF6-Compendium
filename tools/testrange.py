"""Builds a shooting range by replacing mission 1 (the prologue).

Why the prologue:
  - It's the only playable mission not overridden by the installed mission mod.
  - In the drop table it's the only one with empty ranges: by design it drops no
    items, so testing there doesn't pollute the collection.
  - Its map is the inside of the EDF base and it already has eight spawn points
    named 射撃的1..8, literally "shooting target". EDF6's prologue is the aiming
    tutorial: it already was a shooting range.

The AngelScript scaffolding is taken as-is from M104 (the game's simplest
mission, two events) and only the map, the preloads and the spawns are replaced.
That way the structure the engine expects stays intact.

The original end event is kept: killing everything ends the mission, which is
handy for a quick restart.
"""

import os
import re
import shutil
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLANTILLA = os.path.join(os.path.dirname(RAIZ), "EDF6-UI", "extract", "MISSION",
                         "EDF6_DLC", "M104", "MISSION.AC")
SALIDA = os.path.join(RAIZ, "build", "MISSION", "EDF6", "M000B", "MISSION.AC")

# Spawn points don't come from the map but from each mission's MISSION.RMPA, and
# that file also goes through the redirector. So changing the scenery only takes
# copying another mission's RMPA: we keep its points, already placed where they
# belong.
ESCENARIOS = {
    # The prologue: inside the base, eight points named "shooting target".
    # Short, but with walls and cover.
    "base": {
        "mapa": "app:/map/ig_EDFBasement01.mac",
        "clima": "fine",
        "jugador": "プレイヤー",
        "puntos": ["射撃的%d" % i for i in range(1, 9)],
        "puertas": True,
        "rmpa": None,
    },
    # M514: open plain with 19 single ground-enemy points.
    #
    # They have to be POINTS, not areas: CreateEnemy only takes points and returns
    # the object, which is what lets us stop each target's AI. The first attempt
    # used an area and spawned nothing; the mission completed itself because the
    # end event is "all enemies destroyed" and there were none.
    "field": {
        "mapa": "app:/map/ig_Heigen601.mac",
        "clima": "cloudy2",   # each map accepts its own weathers; this one has no "fine"
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
    "ant": "app:/object/e514_dango.sgo",
    "big-ant": "app:/object/e660_heavy_dango_M.sgo",
    "giant-ant": "app:/object/e665_giantant_medium.sgo",
    "spider": "app:/object/e667_spider_medium.sgo",
    "bee": "app:/object/e668_giantbee_medium.sgo",
    "martian": "app:/object/e601_martian_gs.sgo",
    "assault-frog": "app:/object/e503_frog_af.sgo",
    "shotgun-frog": "app:/object/e503_frog_sg.sgo",
    "armored-frog": "app:/object/e503_armorfrog_af.sgo",
}


# The prologue's doors are opened by a scripted NPC we don't create, so without
# this you're locked in the starting room. They are created and opened with
# Action(0), exactly what the original script does.
PUERTAS = ([("ドア%d" % i, "app:/object/Basement_Door.sgo") for i in (1, 2, 3)] +
           [("人用ドア%d" % i, "app:/object/Basement_humanDoor.sgo") for i in range(1, 7)])


def puertas_abiertas():
    lineas = ["\t\t// open doors: without this you can't leave the starting room"]
    for n, (punto, sgo) in enumerate(PUERTAS):
        lineas.append('\t\tObject door%d = CreateNeutral("%s", "%s", 1.0f);' % (n, punto, sgo))
        lineas.append('\t\tdoor%d.Action(0);' % n)
    return "\n".join(lineas) + "\n"


def spawns(sgo, nivel, puntos, invencible):
    """One target per point, standing still. CreateEnemy is used instead of
    CreateEnemyGroup because it returns the object and lets us stop each AI."""
    lineas = ["\t\t// targets: standing still, to test weapons against something that doesn't move"]
    for n, punto in enumerate(puntos):
        lineas.append(
            '\t\tObject target%d = CreateEnemy(\n'
            '\t\t\t"%s",\t// point\n'
            '\t\t\t"%s",\t// sgo\n'
            '\t\t\t%.1ff,\t// level: inflates HP\n'
            '\t\t\tfalse\t// starts without having seen you\n'
            '\t\t\t);\n'
            '\t\ttarget%d.SetAiMoveSpeed(0.0f);' % (n, punto, sgo, nivel, n))
        if invencible:
            lineas.append('\t\ttarget%d.SetObjectInvincible(true);' % n)
    return "\n".join(lineas) + "\n"


def generar(esc, sgo, nivel, puntos, invencible):
    base = open(PLANTILLA, encoding="utf-8-sig").read()

    # preloads: the map and the enemy model we are going to use
    base = re.sub(r'PreloadMap\("[^"]+", "[^"]+", -1\);',
                  'PreloadMap("%s", "%s", -1);' % (esc["mapa"], esc["clima"]), base)
    base = re.sub(r'\tPreload\("app:/object/[^"]+", -1\);\n', "", base)
    extra = '\tPreload("%s", -1);\n' % sgo
    if esc["puertas"]:
        extra += ('\tPreload("app:/object/Basement_Door.sgo", -1);\n'
                  '\tPreload("app:/object/Basement_humanDoor.sgo", -1);\n')
    base = base.replace('\tPreloadPlayerResource();', extra + '\tPreloadPlayerResource();')

    # body: from Map() up to right before the end of the spawn block
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
    p.add_argument("enemy", nargs="?", default="ant", choices=sorted(ENEMIGOS))
    p.add_argument("--level", type=float, default=60.0,
                   help="inflates the target's HP; 1 is a normal enemy")
    p.add_argument("--invincible", action="store_true",
                   help="infinite HP instead of inflated")
    p.add_argument("--scenario", default="field", choices=sorted(ESCENARIOS),
                   help="field = flat map with targets at 330m; base = inside the prologue base")
    p.add_argument("--targets", type=int, default=8, help="how many targets to create")
    args = p.parse_args()

    esc = ESCENARIOS[args.scenario]
    sgo = ENEMIGOS[args.enemy]
    cuantos = max(1, min(20, args.targets))
    disponibles = esc["puntos"]
    # Asking for more targets than points reuses them, stacked in the same place.
    puntos = [disponibles[i % len(disponibles)] for i in range(cuantos)]
    texto = generar(esc, sgo, args.level, puntos, args.invincible)

    os.makedirs(os.path.dirname(SALIDA), exist_ok=True)
    with open(SALIDA, "w", encoding="utf-8-sig", newline="\n") as fh:
        fh.write(texto)

    # The RMPA defines the spawn points: outside the prologue scenario, the donor
    # mission's one has to come along too.
    destino_rmpa = os.path.join(os.path.dirname(SALIDA), "MISSION.RMPA")
    if esc["rmpa"]:
        shutil.copyfile(esc["rmpa"], destino_rmpa)
    elif os.path.exists(destino_rmpa):
        os.remove(destino_rmpa)

    print("%s\n  scenario %s, %d %s targets, %s%s" % (
        SALIDA, args.scenario, len(puntos), args.enemy,
        "invincible" if args.invincible else "level %g" % args.level,
        "\n  + MISSION.RMPA copied from the donor mission" if esc["rmpa"] else ""))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
