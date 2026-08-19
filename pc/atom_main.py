"""Entry point for the multi-electron atom PC debug simulator -- see
README.md's "Multi-electron atoms" section.

    python3 pc/atom_main.py [Z] [--model hydrogenic|hfs] [--tables PATH]

Z defaults to atom_view_pc.DEFAULT_Z (carbon) if not given. --model hfs
switches the radial model to the screened-potential tables from
pc/hfs_solver.py (see pc/screened_potential_model.md); --tables overrides
the npz path (default pc/hfs_tables.npz).
"""

import sys

import atom_view_pc

_model = 'hydrogenic'
_tables = None
_z = None
_argv = sys.argv[1:]
while _argv:
    a = _argv.pop(0)
    if a == '--model':
        _model = _argv.pop(0)
    elif a == '--tables':
        _tables = _argv.pop(0)
    else:
        _z = int(a)

_rt = None
if _model == 'hfs':
    import hfs_tables
    _rt = hfs_tables.load(_tables or hfs_tables.DEFAULT_TABLES)

atom_view_pc.run(_z if _z is not None else atom_view_pc.DEFAULT_Z, radial_tables=_rt)
