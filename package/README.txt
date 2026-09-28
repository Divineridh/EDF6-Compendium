EDF6 Weapon Compendium
======================

In-game overlay with all 1560 weapons: which ones you have, which ones you're
missing, where to farm each one and which mission is worth running.

REQUIREMENT
-----------
Needs EDFModLoader installed. If the game folder has no winmm.dll and no Mods
folder, the plugin doesn't load and nothing happens.

    https://github.com/BlueAmulet/EDFModLoader

INSTALL
-------
Copy the Mods folder from this package over the game's, the one next to
EDF6.exe. It ends up like this:

    EARTH DEFENSE FORCE 6\Mods\Plugins\EDF6Compendium.dll
    EARTH DEFENSE FORCE 6\Mods\Compendium\weapons.tsv
    EARTH DEFENSE FORCE 6\Mods\Compendium\missions.tsv
    EARTH DEFENSE FORCE 6\Mods\Compendium\strats.tsv
    EARTH DEFENSE FORCE 6\Mods\Compendium\config.ini

The wishlist (wishlist.txt) is created in that same folder the first time you
use it.

USING IT
--------
F1 opens and closes the compendium, in the lobby or in a mission. While it's
open the game gets no keyboard or mouse input.

At the top are the tabs for each class, with the percentage you've collected,
and the Farming, Missions and Strats tabs. Q and E move between them.

In a class tab:
  - All / Missing / Not maxed / Wishlist filter the list;
  - "/" focuses the search box and Esc clears it;
  - the arrows move through the list, and the left column filters by category;
  - on the right is the selected weapon's detail: upgrades collected and "How
    to get", with the best mission to farm it and every mission that drops it.

Leaving the mouse on a weapon for a moment shows its stats:
  - weapons you don't have: star 0 / max, from no upgrades to fully upgraded;
  - weapons you have: now / max, with each stat's stars;
  - hold Ctrl on a weapon you have: star 0 / now / max.
Max values are shown in yellow.

Owned weapons are read from the save by themselves and update when you come
back from each mission. "Reload" at the top right forces a reread.

WISHLIST
--------
W, or the heart on each row, marks a weapon as wanted. The list is saved by
itself in Mods\Compendium\wishlist.txt.

It's used by:
  - each class's Wishlist filter;
  - "Wishlist only" in Farming, which turns the calculation around and tells
    you which mission to run to get exactly those, with the route with the
    fewest missions that covers them all;
  - in Missions, clicking a weapon you're missing adds it or removes it.

DROP NOTICE
-----------
When you come back from a mission the mod rereads the save by itself, even
with the overlay closed. If anything that dropped was on the wishlist, a notice
pops up at the top right. "+N new" also shows up in the compendium's header:
hovering it lists the new weapons, and a click dismisses it.

WHAT CAN BE FARMED AND WHAT CAN'T
---------------------------------
Some weapons are never dropped by any crate: they are mission or DLC rewards.
The detail says so ("Mission reward or DLC"), and hovering "Missing" shows how
many of the ones you're missing can be farmed.

Crates don't roll over the whole catalog either: base-game missions only drop
base weapons, DLC1 missions add MissionPack A's and DLC2 missions add B's too.
Farming and Missions already account for it.

HOW MANY CRATES
---------------
Besides the chance per crate, Farming says how many crates it takes on average
for one new weapon and for all the ones you're missing from that mission.
Missions shows, for each mission, how many weapons of its pool you're missing
and a histogram of your missing weapons by level with that mission's range
marked.

LOADOUTS
--------
Since 0.4.0 the loadouts panel is a separate mod, EDF6 Loadouts, that hangs
off the Compendium:

    https://github.com/Divineridh/EDF6-Loadouts/releases

With it installed, F2 opens the same panel as before, and your saved loadouts
are copied to Mods\Loadouts by themselves.

IF F1 DOESN'T OPEN IT
---------------------
The game writes Compendium.log next to EDF6.exe. The first line says which
build it is, and then the key startup is numbered in stages:

    0.  polling: ...                   the loop runs and waits for the key
    0b. the keyboard DOES respond: ... the keyboard reaches the game
    1.  key detected by ...            the key was read
    2.  overlay -> open                it opened
    3.  first overlay frame drawn      it was drawn

The number of the LAST stage that shows up tells where it stops:

  - The file doesn't exist
        The plugin didn't load. EDFModLoader is missing, or the DLL isn't in
        Mods\Plugins. Check ModLoader.log too.

  - It ends at "hooks installed in the vtable, waiting for the first frame"
        Drawing is hooked but no frame ever arrived. It's usually another
        overlay fighting over the same thing: try closing Steam's, Discord's,
        GeForce Experience or MSI Afterburner.

  - It reaches "0." but never "1."
        It runs but the key doesn't reach it. If "0b." never shows up either,
        something is taking the keyboard before the game. If the "0." line says
        "the game does NOT read the keyboard", try another key in config.ini:
        for example key=0x2D for Insert.

  - It reaches "1." but never "2."
        It reads the key and something drops the toggle. The "2." line itself
        explains why if it was the debounce.

  - It reaches "2." but never "3."
        The overlay opens and isn't drawn. The "3." line says whether imgui or
        the render target is missing. Try windowed or borderless instead of
        exclusive fullscreen.

THE KEY IS LOOKED FOR THROUGH THREE PATHS
-----------------------------------------
Any of the three can fail depending on the machine, so all of them are tried:
direct polling, the window message, and the keyboard read the game itself
does. The "1." line says which one worked. To check that the fallback ones
work, set poll=0 in config.ini and see that the overlay still opens.

CREDITS
-------
Drop table per mission: Beardmo's EDF6 Equipment Farming Tool and Inventory
Tracker (public spreadsheet).
Save decryption: Quarri6343's algorithm, published in FevGrave's
EDFSaveEditor.
