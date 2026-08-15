"""Entry point for the multi-electron atom PC debug simulator -- see
README.md's "Multi-electron atoms" section.

    python3 pc/atom_main.py [Z]

Z defaults to atom_view_pc.DEFAULT_Z (carbon) if not given.
"""

import sys

import atom_view_pc

atom_view_pc.run(int(sys.argv[1]) if len(sys.argv) > 1 else atom_view_pc.DEFAULT_Z)
