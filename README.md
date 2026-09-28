# EDF6 Weapon Compendium

Overlay inside Earth Defense Force 6 with all 1560 weapons in the game: which ones you have, which
ones you're missing, where to farm each one and which mission is worth running. F1 opens it, in the
lobby or in a mission.

C++ plugin for [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader), with imgui on DX11.

## Build

```bash
build.bat
```

Needs the VS2019 Build Tools (MSVC 14.29). Dependencies aren't in the repo; clone them into `deps/`:

```bash
git clone https://github.com/ocornut/imgui           deps/imgui
git clone https://github.com/TsudaKageyu/minhook     deps/minhook
git clone https://github.com/BlueAmulet/EDFModLoader deps/EDFModLoader
git clone https://github.com/Quarri6343/EDF6Plugins  deps/EDF6Plugins
```

## Package

```bash
python tools/package.py
```

Writes the zip to `../builds/`. It refuses to package if any source file is newer than the DLL, so
a stale build never ships (it happened once).

For a public release, build the catalog first with `python tools/gen_tsv.py --no-owned`: the owned
column is the fallback for when the save can't be read, and without that option it's filled with the
weapons of whoever builds the package.

## Generating the data

The overlay doesn't read the game live: it consumes TSVs generated from the assets extracted from
`Root.cpk` by the `EDF6-UI` toolchain, which has the SGO/DSGO parsers.

```bash
python tools/gen_tsv.py       # weapons.tsv, from EDF6-UI/build/catalog.json and WEAPONTEXT
python tools/gen_strats.py    # strats.tsv, from data/strats.txt
```

`data/missions.tsv` is already in the repo, so there's no third command. It comes from [Beardmo's
Equipment Farming Tool](https://docs.google.com/spreadsheets/d/17KuXJJOhsRqB0Fi82DLdd0p5hU79Z_OcLRGp8_xTF1A),
which isn't redistributed here. To rebuild the table, download it as `.xlsx` to `data/finder.xlsx`
and run:

```bash
python tools/gen_missions.py
```

Note that the table **is not a copy** of the spreadsheet: its Inferno column lists missions where the
weapon can't drop, and `gen_missions.py` fixes that by calibrating against the pool size. The why is
in the script's docstring.

## What took figuring out

**The save is encrypted with AES-256-CTR**, with key and IV derived from the file name:

    key = MD5(utf16le("edf6MAIN.GST.sav")) + "Edf5.*_Steam_Ver"
    iv  = MD5(utf16le("edf6MAIN.GST.stm"))

The plaintext starts with the magic `MDB`. Algorithm by Quarri6343, published in FevGrave's
EDFSaveEditor. Implemented here in `src/savedata.cpp` with Windows CNG, and in `tools/aes.py` for the
tools. The save folder is `EarthDefen`**c**`eForce6`, with a C: searching for "DEFENSE" misses it.

**Owned weapons table: offset `0x7CFC`, 2048 entries of 12 bytes**, indexed like WEAPONTABLE. The
first 4 bytes at zero mean "you don't have it"; the next 8 are each stat's upgrade level. An earlier
memory scan had failed because it tried strides of 1, 2, 4, 8, 16, 20, 24 and 32, skipping exactly 12.

**Crates don't roll over the whole catalog.** Base-game missions only drop base weapons, DLC1 ones
add MissionPack A and DLC2 ones B too. Each weapon and each mission carry a `tier`, and a weapon is in
the pool if `weapon_tier <= mission_tier`. Checked against the sheet's `1/chance`: 745 of 808 rows
match exactly, against 602 treating the catalog as a single pool.

**Stats scale with stars, and the listed value is star 5.** Every upgradable value in WEAPONTEXT is a
group of 7 numbers: base (the value at star 5), stat type, save byte, max level, two curve
coefficients and whether it's fractional. The game computes
`base * (1 ± a * ((star / 5)^b - 1))`, with minus for types that improve by going down (times,
spread, energy cost, fire intervals, which are stored in frames), rounding whole-number stats; a
stat capped below star 5 has its levels shifted to end at star 5. Fitted to in-game values of six
weapons, all matching to the shown decimal (`src/weapon_stats.cpp`).

**The Steam overlay broke the `Present` hook.** `GameOverlayRenderer64.dll` patches it *inline*, and
MinHook did too: one of the two ended up orphaned and `hkPresent` stopped being called after the
first frames. So **the swapchain's vtable slots** are hooked instead (8 Present, 13 ResizeBuffers,
22 Present1), writing the pointer with `VirtualProtect` instead of patching the body. That way both
hooks chain. MinHook is only left for the user32 functions of the input block.

**The key is looked for through three paths at once**, because any of them fails depending on the
machine: `GetAsyncKeyState`, the WndProc's `WM_KEYDOWN`, and reading the key from the array the game
itself asks `GetKeyboardState` for (those hooks already existed to mute the game's input).
`WH_KEYBOARD_LL` was ruled out because antivirus software reads it as a keylogger. `poll=0` in
`config.ini` turns off the first one, to check that the fallback ones work.

**The focus filter doesn't work with EDF6** and comes off: `GetForegroundWindow()` never returns the
game's window, checked on two different machines.

## Modules

Other DLLs can hang off the overlay without hooking Present or the input themselves, which is what
took stabilizing. The contract is in `src/edf6_overlay_api.h` (version 3, plain C):

- the module's DLL looks for `EDF6Compendium.dll` and calls its `Edf6Overlay_Register` export with an
  `Edf6OverlayModule` (name, key, `onToggle`, `wantsDraw`, `draw` and, since 3, `panel`);
- the Compendium detects the key through the same three paths as F1 and calls `onToggle`;
- every frame, if `wantsDraw` returns non-zero, it calls `draw` with an `Edf6OverlayHost`: rectangles,
  text with the panel fonts and letter spacing (since 2), screen size, scale and the log. What it
  draws goes to the background, under the panels.

**Panels (version 3).** A module with `panel` gets a window of its own, like the Compendium: its key
opens and closes it, only one panel is open at a time and, while it's open, the Compendium blocks the
game's input. `panel` runs inside the Compendium's imgui frame, and the module draws with imgui on the
host's context (`imguiContext`, `imguiAllocators`, `imguiFont`). Sharing imgui across DLLs requires
the same version and struct layout: the module declares `IMGUI_VERSION_NUM` and `EDF6_IMGUI_LAYOUT`,
and if they don't match the Compendium's it isn't registered and the log says so, instead of
crashing. In practice, build the module against the same `deps/imgui` commit.

Version 3 also shares the catalog: `weaponCount` and `weapon` give name, class, category, level and
whether you have it or have it maxed, from any thread.

The host struct only grows at the end, so the Compendium accepts modules of any version up to its
own; a module asking for version N needs a Compendium with N or newer. Up to 8 modules fit:

- [EDF6-EnemyHp](https://github.com/Divineridh/EDF6-EnemyHp): HP of the enemies you hit.
- [EDF6-Loadouts](https://github.com/Divineridh/EDF6-Loadouts): the loadouts panel (F2), which lived
  inside the Compendium up to 0.3.0.

## Diagnostics

The plugin writes `Compendium.log` next to `EDF6.exe`. The first line has the build date, and the key
startup is numbered in stages: `0.` polling (with how many frames were drawn), `0b.` the keyboard
responds, `1.` key detected and through which path, `2.` overlay open, `3.` first frame drawn. The
number of the last stage that shows up tells where it stops.
