"""Boot entry point: runs the CLAUDE.md M1/M2 point-cloud animation (see
orbital_view.py for the hydrogen n=2 orbital math/rendering pipeline).

For the display bring-up smoke test (corner colors + color cycle), run
corner_test.py instead -- it was main.py's content before this animation
existed; kept as a standalone re-runnable diagnostic.

For the multi-electron atom viewer (pc/atom_view_pc.py's device
counterpart -- cycles atomic number Z instead of a fixed orbital list, see
atom_view.py's module docstring), run atom_view.py instead: not the default
boot animation (CLAUDE.md's roadmap is single-electron hydrogen orbitals),
but available the same way corner_test.py is --
    mpremote connect <port> exec "import atom_view; atom_view.run()"

On this board, `mpremote run <file>` hung after a watchdog reset (looked
like a raw-paste-mode / USB-passthrough interaction, not a MicroPython or
driver bug). What worked reliably instead:
    mpremote connect <port> fs cp -r micropython/. :
    mpremote connect <port> exec "exec(open('main.py').read())"
"""

import orbital_view

orbital_view.run()
