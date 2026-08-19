"""Plot this project's computed atomic radii (valence-subshell mode of the
SPARC-atomSFE radial model, see pc/screened_potential_model.md) against the
Clementi-Raimondi literature values (pc/clementi_radii.py), across Z=1..92
(the radial tables' coverage) -- for eyeballing the periodic sawtooth trend
against a reference chart (e.g. a periodic-table atomic-radius plot).

Reuses pc/validate_atoms.py's own build_table() (the exact same per-element
valence-subshell + mode-radius computation validate_atoms.py's --model hfs
regression gate uses) rather than recomputing radii a second way.

The model series is the RAW radial-model radius, not the per-element
Clementi-Raimondi size CALIBRATION applied at render time (atom_view_pc.py's
clementi_size_factor()) -- that calibration forces the displayed valence
radius to land exactly on the literature value by construction, so plotting
it would just redraw the literature curve. The raw model is the genuine
"what the physics computes" series; see the run-time systematic offset note
printed below the plot (documented in pc/RUN_HFS.md section 5: LDA valence
orbitals are more diffuse than the Hartree-Fock Clementi-Raimondi reference
-- self-interaction error, roughly 2.2x for H, 1.7x for period 2, 1.1x for
Fe, 1.5x for U).

Usage:
    python3 pc/plot_atomic_radii.py [--out pc/atomic_radii.png] [--tables pc/hfs_tables.npz]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'micropython'))

import micropython_shim  # noqa: F401,E402 -- must precede micropython/ imports

import hfs_tables  # noqa: E402
import slater  # noqa: E402
import validate_atoms  # noqa: E402

import matplotlib  # noqa: E402
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402

# Fixed categorical colors (not cycled) for the two series -- a standard
# colorblind-distinguishable blue/orange pair (Wong 2011), not a rainbow.
MODEL_COLOR = '#0072B2'   # blue -- this project's computed radius
LIT_COLOR = '#D55E00'     # vermillion -- Clementi-Raimondi literature
GRID_COLOR = '#DDDDDD'
PERIOD_BAND_COLOR = '#F2F2F2'

# Period boundaries (last Z of each period, up to the tables' Z=92 cap).
PERIOD_ENDS = [2, 10, 18, 36, 54, 86, 92]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                        'atomic_radii.png'))
    parser.add_argument('--tables', default=None)
    args = parser.parse_args()

    validate_atoms.RADIAL_MODEL = 'hfs'
    validate_atoms.HFS = hfs_tables.load(args.tables or hfs_tables.DEFAULT_TABLES)
    zmax = max(validate_atoms.HFS.z_list)  # 92, the SPARC-atomSFE tables' coverage cap

    rows = validate_atoms.build_table(1, zmax)
    z_vals = [r[0] for r in rows]
    model_pm = [r[5] for r in rows]
    lit_pm = [r[6] for r in rows]
    symbols = [slater.element_symbol(z) for z in z_vals]

    z_lit = [z for z, l in zip(z_vals, lit_pm) if l is not None]
    lit_only = [l for l in lit_pm if l is not None]
    ratios = [m / l for m, l in zip(model_pm, lit_pm) if l is not None]
    mean_ratio = sum(ratios) / len(ratios)

    fig, ax = plt.subplots(figsize=(14, 6), dpi=150)

    # Recessive period-boundary bands, alternating, so the sawtooth's period
    # structure reads at a glance without needing to count elements.
    band_start = 1
    for i, end in enumerate(PERIOD_ENDS):
        if i % 2 == 1:
            ax.axvspan(band_start - 0.5, end + 0.5, color=PERIOD_BAND_COLOR, zorder=0)
        band_start = end + 1

    ax.plot(z_vals, model_pm, color=MODEL_COLOR, linewidth=2, marker='o', markersize=3,
            label='This project (SPARC-atomSFE radial model, raw)', zorder=3)
    ax.plot(z_lit, lit_only, color=LIT_COLOR, linewidth=2, marker='o', markersize=3,
            label='Clementi-Raimondi (1963/1967) literature', zorder=3)

    # Direct labels every 5th element (dense enough to orient against a
    # reference chart, sparse enough to stay legible across 92 elements).
    for z, sym, y in zip(z_vals, symbols, model_pm):
        if z % 5 == 0 or z == 1:
            ax.annotate(sym, (z, y), textcoords='offset points', xytext=(0, 6),
                        ha='center', fontsize=7, color='#444444')

    ax.set_xlabel('Atomic number (Z)')
    ax.set_ylabel('Valence-subshell radius (pm)')
    ax.set_title('Atomic radius vs atomic number: computed model vs literature (Z=1-%d)' % zmax)
    ax.xaxis.set_major_locator(mticker.MultipleLocator(5))
    ax.grid(True, color=GRID_COLOR, linewidth=0.6, zorder=1)
    ax.set_axisbelow(True)
    for spine in ('top', 'right'):
        ax.spines[spine].set_visible(False)
    ax.legend(frameon=False, loc='upper right')

    fig.text(0.01, 0.01,
              "Mean raw-model / literature ratio: %.2fx -- consistent with the documented LDA "
              "self-interaction offset\n(more diffuse valence orbitals than Hartree-Fock; see "
              "pc/RUN_HFS.md section 5), not a modeling error." % mean_ratio,
              fontsize=8, color='#666666')

    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(args.out)
    print('wrote %s' % args.out)
    print('mean model/literature ratio (raw, uncalibrated): %.3fx over %d elements with a literature value'
          % (mean_ratio, len(ratios)))


if __name__ == '__main__':
    main()
