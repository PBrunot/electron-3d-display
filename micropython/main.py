"""Boot entry point: shows chooser.py's menu (Hydrogen Orbitals vs Element
Explorer, a randomly picked tumbling preset playing behind it) -- nudge Up
launches the orbital viewer (orbital_view.py, CLAUDE.md's M1/M2 point-cloud
animation), nudge Down launches the multi-electron atom viewer
(atom_view.py, pc/atom_view_pc.py's device counterpart). Nudging Up again
from inside either one returns to this menu -- see chooser.py's module
docstring for the full nudge-gesture layout.

For the display bring-up smoke test (corner colors + color cycle), run
corner_test.py instead -- it was main.py's content before this animation
existed; kept as a standalone re-runnable diagnostic.

To skip the menu and jump straight into one viewer (e.g. for debugging),
run it directly the same way corner_test.py is run:
    mpremote connect <port> exec "import atom_view; atom_view.run()"
    mpremote connect <port> exec "import orbital_view; orbital_view.run()"

On this board, `mpremote run <file>` hung after a watchdog reset (looked
like a raw-paste-mode / USB-passthrough interaction, not a MicroPython or
driver bug). What worked reliably instead:
    mpremote connect <port> fs cp -r micropython/. :
    mpremote connect <port> exec "exec(open('main.py').read())"
"""

import chooser

chooser.run()
