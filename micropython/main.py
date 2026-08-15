"""Boot entry point: runs the CLAUDE.md M1/M2 point-cloud animation (see
orbital_view.py for the hydrogen n=2 orbital math/rendering pipeline).

For the display bring-up smoke test (corner colors + color cycle), run
corner_test.py instead -- it was main.py's content before this animation
existed; kept as a standalone re-runnable diagnostic.

On this board, `mpremote run <file>` hung after a watchdog reset (looked
like a raw-paste-mode / USB-passthrough interaction, not a MicroPython or
driver bug). What worked reliably instead:
    mpremote connect <port> fs cp -r micropython/. :
    mpremote connect <port> exec "exec(open('main.py').read())"
"""

import orbital_view

orbital_view.run()
