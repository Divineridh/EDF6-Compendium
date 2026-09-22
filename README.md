# EDF6 Weapon Compendium

Overlay dentro de Earth Defense Force 6 con las 1560 armas del juego: cuáles tenés, cuáles te
faltan, dónde farmear cada una y a qué misión conviene ir. Se abre con F1, en el lobby o en misión.

Plugin C++ para [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader), con imgui sobre DX11.

## Compilar

```bash
build.bat
```

Necesita las Build Tools de VS2019 (MSVC 14.29). Las dependencias no están en el repo; se clonan a
mano dentro de `deps/`:

```bash
git clone https://github.com/ocornut/imgui           deps/imgui
git clone https://github.com/TsudaKageyu/minhook     deps/minhook
git clone https://github.com/BlueAmulet/EDFModLoader deps/EDFModLoader
git clone https://github.com/Quarri6343/EDF6Plugins  deps/EDF6Plugins
```

## Empaquetar

```bash
python tools/paquete.py
```

Deja el zip en `../builds/`. Se niega a empaquetar si algún `.cpp` es más nuevo que el DLL, para no
distribuir un build viejo — que ya pasó una vez.

## Generar los datos

El overlay no lee el juego en vivo: consume TSVs generados desde los assets extraídos del `Root.cpk`
por el toolchain de `EDF6-UI`, que es el que tiene los parsers de SGO/DSGO.

```bash
python tools/gen_tsv.py       # weapons.tsv, desde EDF6-UI/build/catalog.json
python tools/gen_missions.py  # missions.tsv, desde data/finder.xlsx
python tools/gen_strats.py    # strats.tsv, desde data/strats.txt
```

`data/finder.xlsx` es una copia del Equipment Farming Tool de Beardmo (spreadsheet público); la
atribución está en `tools/gen_missions.py`.

## Lo que costó averiguar

**El save está cifrado con AES-256-CTR**, clave e IV derivados del nombre del archivo:

    clave = MD5(utf16le("edf6MAIN.GST.sav")) + "Edf5.*_Steam_Ver"
    iv    = MD5(utf16le("edf6MAIN.GST.stm"))

El texto plano arranca con el magic `MDB`. Algoritmo de Quarri6343, publicado en el EDFSaveEditor de
FevGrave. Implementado acá en `src/savedata.cpp` con Windows CNG, y en `tools/aes.py` para las
herramientas. La carpeta del save es `EarthDefen`**c**`eForce6`, con C: buscar "DEFENSE" no la
encuentra.

**Tabla de armas obtenidas: offset `0x7CFC`, 2048 entradas de 12 bytes**, indexadas igual que
WEAPONTABLE. Los primeros 4 bytes en cero significan "no la tenés"; los 8 siguientes son el nivel de
mejora de cada stat. El escaneo de memoria previo había fallado por probar pasos de 1, 2, 4, 8, 16,
20, 24 y 32 salteando justo el 12.

**Las cajas no sortean sobre todo el catálogo.** Las misiones del juego base solo sueltan armas base,
las de DLC1 suman el MissionPack A y las de DLC2 también el B. Cada arma y cada misión llevan un
`tier` y entra al pool si `tier_arma <= tier_misión`. Contrastado contra `1/probabilidad` de la
planilla: 745 de 808 filas dan exacto, contra 602 tratando el catálogo como un pool único.

**El overlay de Steam rompía el hook de `Present`.** `GameOverlayRenderer64.dll` lo parchea *inline*,
y MinHook también: uno de los dos quedaba huérfano y `hkPresent` dejaba de llamarse tras los primeros
frames. Por eso se enganchan **los slots de la vtable del swapchain** (8 Present, 13 ResizeBuffers,
22 Present1) escribiendo el puntero con `VirtualProtect`, en vez de parchear el cuerpo. Así los dos
hooks se encadenan. MinHook quedó solo para las funciones de user32 del bloqueo de input.

**La tecla se busca por tres caminos a la vez**, porque según la máquina falla cualquiera:
`GetAsyncKeyState`, el `WM_KEYDOWN` del WndProc, y leer la tecla del array que el propio juego le
pide a `GetKeyboardState` — esos hooks ya existían para mutearle el input al juego. Se descartó
`WH_KEYBOARD_LL` porque los antivirus lo leen como keylogger. `sondeo=0` en `config.ini` apaga el
primero y sirve para comprobar que los de respaldo andan.

**El filtro de foco no funciona con EDF6** y viene apagado: `GetForegroundWindow()` nunca devuelve la
ventana del juego, comprobado en dos máquinas distintas.

## Diagnóstico

El plugin escribe `Compendium.log` al lado del `EDF6.exe`. La primera línea trae la fecha del build,
y el arranque de la tecla va numerado por etapas — `0.` sondeando (con cuántos frames se dibujaron),
`0b.` el teclado responde, `1.` tecla detectada y por cuál camino, `2.` overlay abierto, `3.` primer
frame dibujado. El número de la última etapa que aparezca dice dónde se corta.
