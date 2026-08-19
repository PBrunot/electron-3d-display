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

Two additional, deliberately cruder reference series are plotted alongside
this project's model to show what each approximation buys you:

  - "Naive hydrogen (Z_eff=Z)": the valence subshell's hydrogenic radial
    function with NO screening at all -- every other electron ignored, so
    the nucleus's full charge Z pulls the valence electron straight in.
    This is what you get before accounting for electron-electron repulsion
    at all; it collapses far inside the literature radius for anything past
    a one-electron ion; slater.z_eff_radial() is what corrects for it.
  - "Slater/CR Z_eff (hydrogenic)": the same hydrogenic radial function, but
    with the effective charge from slater.z_eff_radial() -- Clementi-Raimondi
    Hartree-Fock Z_eff where the table covers it (Z<=54), else Slater's 1930
    empirical shielding rules (rescaled by n/n*, see slater.py's module
    docstring). This is validate_atoms.py's `--model hydrogenic` (the
    original model, before the tabulated screened-potential `hfs` solver
    existed) -- a purely analytic middle ground between the naive model and
    the numerically-solved screened potential.

A second panel below plots this project's own first-ionization-energy
estimate against the NIST SRD 111 literature values (pc/ionization_energy.py)
-- the radius sawtooth's inverse counterpart (ionization energy peaks where
radius troughs, at each noble gas). The model series applies Koopmans'
theorem (IP ~= -E_valence, relaxation and correlation neglected) to the same
per-element HFS valence-subshell eigenvalue build_table() already returns
for the radius panel -- see validate_atoms.check_koopmans(), whose curated
IP_EV subset pc/ionization_energy.py extends to full Z=1..92 coverage. The
sawtooth shape reproduces well, but the model runs systematically low
(~0.6x literature, printed below the plot) -- the same LDA valence-orbital
diffuseness behind the radius panel's overshoot means a shallower, less
tightly bound valence eigenvalue here.

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
import pointcloud  # noqa: E402
import slater  # noqa: E402
import validate_atoms  # noqa: E402
from ionization_energy import IONIZATION_EV  # noqa: E402

import matplotlib  # noqa: E402
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402

# Fixed categorical colors (not cycled) for the series -- a standard
# colorblind-distinguishable palette (Wong 2011), not a rainbow.
MODEL_COLOR = '#0072B2'   # blue -- this project's computed radius
LIT_COLOR = '#D55E00'     # vermillion -- Clementi-Raimondi literature
ION_COLOR = '#009E73'     # bluish green -- NIST first ionization energy
NAIVE_COLOR = '#CC79A7'   # reddish purple -- naive hydrogen, Z_eff=Z (no screening)
SLATER_COLOR = '#E69F00'  # orange -- Slater/CR Z_eff, hydrogenic radial function
GRID_COLOR = '#DDDDDD'
PERIOD_BAND_COLOR = '#F2F2F2'

# Period boundaries (last Z of each period, up to the tables' Z=92 cap).
PERIOD_ENDS = [2, 10, 18, 36, 54, 86, 92]


def _draw_period_bands(ax):
    """Recessive period-boundary bands, alternating, so the sawtooth's
    period structure reads at a glance without needing to count elements.
    """
    band_start = 1
    for i, end in enumerate(PERIOD_ENDS):
        if i % 2 == 1:
            ax.axvspan(band_start - 0.5, end + 0.5, color=PERIOD_BAND_COLOR, zorder=0)
        band_start = end + 1


def _label_every_5th(ax, z_vals, symbols, y_vals):
    """Direct labels every 5th element (dense enough to orient against a
    reference chart, sparse enough to stay legible across 92 elements).
    """
    for z, sym, y in zip(z_vals, symbols, y_vals):
        if z % 5 == 0 or z == 1:
            ax.annotate(sym, (z, y), textcoords='offset points', xytext=(0, 6),
                        ha='center', fontsize=7, color='#444444')


def _style_axes(ax, log_y=False):
    ax.xaxis.set_major_locator(mticker.MultipleLocator(5))
    ax.grid(True, color=GRID_COLOR, linewidth=0.6, zorder=1)
    ax.set_axisbelow(True)
    for spine in ('top', 'right'):
        ax.spines[spine].set_visible(False)
    if log_y:
        # The naive (Z_eff=Z) and Slater/CR series span 1-2 orders of
        # magnitude more than the HFS model/literature pair (e.g. naive IP
        # ~ Z^2 reaches ~2700 eV at U vs ~6 eV literature) -- linear axes
        # flatten the model/literature comparison this plot exists for.
        ax.set_yscale('log')
        ax.yaxis.set_major_formatter(mticker.ScalarFormatter())
        ax.yaxis.set_minor_formatter(mticker.NullFormatter())


def _naive_and_slater_series(rows):
    """The two cruder reference models (see module docstring), evaluated at
    each row's own valence subshell (n, ell) -- same subshell the HFS/
    literature series use, so all four series are directly comparable.

    Both reuse pointcloud.radial_mode_radius() directly (not
    validate_atoms.model_radius_pm(), which dispatches on the global
    RADIAL_MODEL -- these two series are always the hydrogenic radial
    function, regardless of which model build_table() was run under).
    """
    naive_pm, slater_pm, naive_ip_ev, slater_ip_ev = [], [], [], []
    for z, n, ell, *_rest in rows:
        config = slater.electron_configuration(z)
        z_naive = float(z)
        z_slater = slater.z_eff_radial(z, config, n, ell)
        naive_pm.append(pointcloud.radial_mode_radius(
            n, ell, z_naive, resolution=validate_atoms.RADIAL_MODE_RESOLUTION) * validate_atoms.A0_PM)
        slater_pm.append(pointcloud.radial_mode_radius(
            n, ell, z_slater, resolution=validate_atoms.RADIAL_MODE_RESOLUTION) * validate_atoms.A0_PM)
        # Same hydrogenic-energy Koopmans estimate as check_koopmans()'s
        # 'hydrogenic' branch: E = -0.5*z_eff^2/n^2 (Hartree).
        naive_ip_ev.append(0.5 * z_naive * z_naive / (n * n) * validate_atoms.HARTREE_EV)
        slater_ip_ev.append(0.5 * z_slater * z_slater / (n * n) * validate_atoms.HARTREE_EV)
    return naive_pm, slater_pm, naive_ip_ev, slater_ip_ev


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

    # Koopmans' theorem: IP ~= -E_valence. r[3] is the HFS valence-subshell
    # eigenvalue (Hartree) build_table() already computed for the radius
    # panel above -- same source validate_atoms.check_koopmans() uses.
    model_ip_ev = [-r[3] * validate_atoms.HARTREE_EV for r in rows]

    z_ion = [z for z in z_vals if z in IONIZATION_EV]
    ion_ev = [IONIZATION_EV[z] for z in z_ion]
    ion_symbols = [slater.element_symbol(z) for z in z_ion]

    ion_ratios = [m / IONIZATION_EV[z] for z, m in zip(z_vals, model_ip_ev) if z in IONIZATION_EV]
    mean_ion_ratio = sum(ion_ratios) / len(ion_ratios)

    naive_pm, slater_pm, naive_ip_ev, slater_ip_ev = _naive_and_slater_series(rows)
    naive_ratios = [m / l for m, l in zip(naive_pm, lit_pm) if l is not None]
    slater_ratios = [m / l for m, l in zip(slater_pm, lit_pm) if l is not None]
    mean_naive_ratio = sum(naive_ratios) / len(naive_ratios)
    mean_slater_ratio = sum(slater_ratios) / len(slater_ratios)

    fig, (ax_r, ax_i) = plt.subplots(2, 1, figsize=(14, 11), dpi=150, sharex=True)

    _draw_period_bands(ax_r)
    ax_r.plot(z_vals, naive_pm, color=NAIVE_COLOR, linewidth=1.2, linestyle='--', marker='o', markersize=2,
              alpha=0.8, label='Naive hydrogen (Z_eff=Z, no screening)', zorder=2)
    ax_r.plot(z_vals, slater_pm, color=SLATER_COLOR, linewidth=1.2, linestyle='--', marker='o', markersize=2,
              alpha=0.85, label='Slater/CR Z_eff (hydrogenic radial fn)', zorder=2)
    ax_r.plot(z_vals, model_pm, color=MODEL_COLOR, linewidth=2, marker='o', markersize=3,
              label='This project (SPARC-atomSFE radial model, raw)', zorder=3)
    ax_r.plot(z_lit, lit_only, color=LIT_COLOR, linewidth=2, marker='o', markersize=3,
              label='Clementi-Raimondi (1963/1967) literature', zorder=3)
    _label_every_5th(ax_r, z_vals, symbols, model_pm)
    ax_r.set_ylabel('Valence-subshell radius (pm)')
    ax_r.set_title('Atomic radius vs atomic number: computed model vs literature (Z=1-%d)' % zmax)
    _style_axes(ax_r)
    ax_r.legend(frameon=False, loc='upper right')

    _draw_period_bands(ax_i)
    ax_i.plot(z_vals, naive_ip_ev, color=NAIVE_COLOR, linewidth=1.2, linestyle='--', marker='o', markersize=2,
              alpha=0.8, label='Naive hydrogen (Z_eff=Z, no screening)', zorder=2)
    ax_i.plot(z_vals, slater_ip_ev, color=SLATER_COLOR, linewidth=1.2, linestyle='--', marker='o', markersize=2,
              alpha=0.85, label='Slater/CR Z_eff (hydrogenic radial fn)', zorder=2)
    ax_i.plot(z_vals, model_ip_ev, color=MODEL_COLOR, linewidth=2, marker='o', markersize=3,
              label='This project (Koopmans, HFS valence eigenvalue, raw)', zorder=3)
    ax_i.plot(z_ion, ion_ev, color=ION_COLOR, linewidth=2, marker='o', markersize=3,
              label='NIST SRD 111 literature', zorder=3)
    _label_every_5th(ax_i, z_vals, symbols, model_ip_ev)
    ax_i.set_xlabel('Atomic number (Z)')
    ax_i.set_ylabel('First ionization energy (eV)')
    ax_i.set_title('First ionization energy vs atomic number: Koopmans estimate vs literature (Z=1-%d)' % zmax)
    _style_axes(ax_i)
    ax_i.legend(frameon=False, loc='upper right')

    fig.text(0.01, 0.01,
              "Mean raw-model / literature ratio: radius %.2fx (LDA self-interaction offset, more diffuse "
              "valence orbitals than Hartree-Fock;\nsee pc/RUN_HFS.md section 5) -- ionization energy %.2fx "
              "(Koopmans' theorem neglects orbital relaxation and correlation;\nsee "
              "validate_atoms.check_koopmans()). Neither is a modeling error.\n"
              "Reference series: naive hydrogen (no screening) radius %.2fx literature -- Slater/CR Z_eff "
              "(hydrogenic) radius %.2fx literature. Both show what each layer of approximation buys over "
              "the plain hydrogen atom." % (mean_ratio, mean_ion_ratio, mean_naive_ratio, mean_slater_ratio),
              fontsize=8, color='#666666')

    fig.tight_layout(rect=(0, 0.05, 1, 1))
    fig.savefig(args.out)
    print('wrote %s' % args.out)
    print('mean radius model/literature ratio (raw, uncalibrated): %.3fx over %d elements with a literature value'
          % (mean_ratio, len(ratios)))
    print('mean ionization-energy model/literature ratio (Koopmans, raw): %.3fx over %d elements with a '
          'literature value' % (mean_ion_ratio, len(ion_ratios)))
    print('mean radius naive-hydrogen/literature ratio: %.3fx over %d elements' % (mean_naive_ratio, len(naive_ratios)))
    print('mean radius Slater-CR/literature ratio: %.3fx over %d elements' % (mean_slater_ratio, len(slater_ratios)))


if __name__ == '__main__':
    main()
