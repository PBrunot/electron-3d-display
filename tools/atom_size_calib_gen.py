#!/usr/bin/env python3
"""Generate the Clementi-Raimondi size-calibration factor tables consumed by
each port's atom point cloud.

For each element the factor is

    f[z] = CR_literature_valence_radius / model_valence_mode_radius

so scaling a rendered atom's point cloud by f[z] makes its valence subshell
mode radius land on the Clementi-Raimondi literature value
(pc/clementi_radii.py), while the internal shell structure stays the
model's own. "model" differs per output, matching what each port actually
renders (see pc/RUN_HFS.md's device note):

- src/atom_size_calib.h (device, C++): TABLE-based --
  compute_table_factors(), CR / HFS-table valence mode
  (pc/hfs_tables_reduced.npz, same tables src/hfs_radial.h renders through
  since src/atom_cloud.cpp switched to them) -- mirrors
  pc/atom_view_pc.py's clementi_size_factor() when radial_tables is set.
- micropython/hfs_atom_size_calib.py: TABLE-based, same factors as the C++
  header above -- used ONLY by micropython/atom_view.py, which now renders
  through hfs_radial_tables.py's tables the same way the device does.
- micropython/atom_size_calib.py: HYDROGENIC -- compute_factors(), CR /
  hydrogenic valence mode (slater.z_eff_radial: CR Z_eff to Z=54,
  Slater-rule fallback past it) -- left UNCHANGED and still generated here,
  because it is a genuinely shared module with consumers that must stay
  hydrogenic: pc/atom_view_pc.py's default (`--model hydrogenic`) path and
  pc/atom_dissection_common.py's default_size_factor() fallback (itself
  shared with the web viewer, see that module's docstring). Renaming its
  meaning to table-based would silently miscalibrate all of those --
  hence the separate hfs_atom_size_calib.py file above instead of repurposing
  this one.

Elements without a CR literature value (Fr, Ra) get 1.0 in all three.

Usage:
    python3 tools/atom_size_calib_gen.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pc'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'micropython'))

import micropython_shim  # noqa: F401 -- CPython shim for @micropython.native
import slater  # noqa: E402
import pointcloud  # noqa: E402
import clementi_radii  # noqa: E402
import hfs_tables  # noqa: E402

A0_PM = 52.9177210903
MAX_Z = 92  # display range cap (slater.MAX_DISPLAY_Z)
MODE_RESOLUTION = 200000
DEFAULT_HFS_NPZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pc',
                                'hfs_tables_reduced.npz')


def _valence_subshell(config):
    n_max = max(n for n, _ell, _occ in config)
    return max(((n, ell) for n, ell, _occ in config if n == n_max), key=lambda t: t[1])


def compute_factors():
    """Hydrogenic-model factors -- see module docstring."""
    factors = []
    for z in range(1, MAX_Z + 1):
        lit = clementi_radii.CLEMENTI_RADIUS_PM.get(z)
        if not lit:
            factors.append(1.0)
            continue
        config = slater.electron_configuration(z)
        n, ell = _valence_subshell(config)
        z_eff = slater.z_eff_radial(z, config, n, ell)
        mode_pm = pointcloud.radial_mode_radius(n, ell, z_eff, resolution=MODE_RESOLUTION) * A0_PM
        factors.append(lit / mode_pm if mode_pm > 0.0 else 1.0)
    return factors


def compute_table_factors(tables):
    """HFS-table-based factors -- see module docstring. `tables` is an
    hfs_tables.HfsTables (same schema as the reduced npz src/hfs_radial.h's
    generated data comes from), so this stays exactly the calibration the
    embedded device tables need, not a separate model."""
    factors = []
    for z in range(1, MAX_Z + 1):
        lit = clementi_radii.CLEMENTI_RADIUS_PM.get(z)
        if not lit:
            factors.append(1.0)
            continue
        config = slater.electron_configuration(z)
        n, ell = _valence_subshell(config)
        if not tables.has(z, n, ell):
            factors.append(1.0)
            continue
        mode_pm = tables.source(z, n, ell).mode_radius() * A0_PM
        factors.append(lit / mode_pm if mode_pm > 0.0 else 1.0)
    return factors


def emit_header(factors):
    lines = [
        "// Clementi-Raimondi size-calibration factors for the atom point cloud, indexed by",
        "// Z-1 (Z=1..92, the display range cap -- see periodic_grid.h's kMaxDisplayZ).",
        "// Scaling a rendered cloud's coordinates by kAtomSizeCalibFactor[z-1] makes its",
        "// valence subshell mode radius land on the Clementi-Raimondi literature value",
        "// (pc/clementi_radii.py), keeping the model's own internal shell structure.",
        "// HFS-table factors (compute_table_factors()): f = CR_lit / HFS-table valence",
        "// mode (pc/hfs_tables_reduced.npz, the same tables src/hfs_radial.h renders",
        "// through) -- elements without a CR value (Fr, Ra) are 1.0. See pc/RUN_HFS.md",
        "// and pc/atom_view_pc.py's clementi_size_factor for the PC sibling.",
        "//",
        "// GENERATED by tools/atom_size_calib_gen.py -- do not edit by hand.",
        "#pragma once",
        "",
        '#include "orbitals.h" // orb_real_t',
        "",
        "constexpr int kAtomSizeCalibCount = %d;" % len(factors),
        "",
        "constexpr orb_real_t kAtomSizeCalibFactor[%d] = {" % len(factors),
    ]
    for i in range(0, len(factors), 4):
        chunk = ", ".join("orb_real_t(%.4f)" % f for f in factors[i:i + 4])
        lines.append("    %s," % chunk)
    lines.append("};")
    return "\n".join(lines) + "\n"


def emit_module(factors, model_desc, consumer_desc):
    rows = []
    for i in range(0, len(factors), 8):
        rows.append("    " + ", ".join("%.4f" % f for f in factors[i:i + 8]))
    return (
        '"""Clementi-Raimondi size-calibration factors for the atom cloud (%s model),' % model_desc
        + "\nindexed by Z-1 (Z=1..92, the display range cap -- slater.MAX_DISPLAY_Z)."
        "\nScaling the cloud coordinates by FACTOR[z-1] makes the valence subshell mode"
        "\nradius land on the Clementi-Raimondi literature value, keeping the model's own"
        "\ninternal shell structure. Elements without a CR value (Fr, Ra) are 1.0."
        "\n"
        "\n%s" % consumer_desc
        + "\n"
        "\nGENERATED by tools/atom_size_calib_gen.py -- do not edit by hand."
        '\n"""'
        "\n"
        "\nFACTOR = ("
        "\n" + ",\n".join(rows) + ",\n)"
        "\n"
    )


def main():
    hydro_factors = compute_factors()
    assert len(hydro_factors) == MAX_Z
    tables = hfs_tables.load(DEFAULT_HFS_NPZ)
    table_factors = compute_table_factors(tables)
    assert len(table_factors) == MAX_Z
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    h_path = os.path.join(root, 'src', 'atom_size_calib.h')
    hydro_py_path = os.path.join(root, 'micropython', 'atom_size_calib.py')
    table_py_path = os.path.join(root, 'micropython', 'hfs_atom_size_calib.py')
    with open(h_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(emit_header(table_factors))
    with open(hydro_py_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(emit_module(
            hydro_factors, "hydrogenic",
            "Consumers that render the hydrogenic model and so need HYDROGENIC factors:\n"
            "pc/atom_view_pc.py's default (`--model hydrogenic`) path,\n"
            "pc/atom_dissection_common.py's default_size_factor() fallback (shared with the\n"
            "web viewer), and the web viewer itself. NOT micropython/atom_view.py, which\n"
            "renders through the HFS tables now -- see hfs_atom_size_calib.py."))
    with open(table_py_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(emit_module(
            table_factors, "HFS-table",
            "Same factors as src/atom_size_calib.h (device C++). Consumed ONLY by\n"
            "micropython/atom_view.py, which renders through hfs_radial_tables.py's tables\n"
            "the same way the device does -- see pc/RUN_HFS.md's device note."))
    print("wrote %s (%d table-based factors)" % (h_path, len(table_factors)))
    print("wrote %s (%d hydrogenic factors)" % (hydro_py_path, len(hydro_factors)))
    print("wrote %s (%d table-based factors)" % (table_py_path, len(table_factors)))
    sample = (0, 2, 25, 45, 54, 91)  # H, Li, Fe, Pd, Cs, U
    labels = ("H", "Li", "Fe", "Pd", "Cs", "U")
    print("table   sample: " + " ".join("%s=%.4f" % (l, table_factors[i]) for l, i in zip(labels, sample)))
    print("hydro   sample: " + " ".join("%s=%.4f" % (l, hydro_factors[i]) for l, i in zip(labels, sample)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
