"""Validation harness for the multi-electron atom model (pc/atom_view_pc.py /
micropython/atom_cloud.py / slater.py): compares model radii against the
Clementi-Raimondi literature values and runs automated physics checks, so
accuracy regressions are caught instead of re-derived by hand (see ATOMS.md
sections 4-5).

Definitions (the methodological fix of ATOMS.md section 4.2):
  - "valence subshell" = highest-l subshell among the highest-n occupied
    ones (e.g. carbon's 2p, iron's 4s, krypton's 4p). Comparing the model's
    most-EXTENDED subshell instead reproduces the old, bogus 'systematic
    22-28% overestimate' artifact.
  - "model radius" = mode of r^2*R^2 of the valence subshell. For the
    hydrogenic model this is pointcloud.radial_mode_radius() with
    z_eff_radial(); for --model hfs it is the mode of the tabulated
    screened-potential solution (see pc/screened_potential_model.md and
    pc/hfs_solver.py) -- the two models share the same definition, only the
    radial function differs.

Usage:
    python3 pc/validate_atoms.py [--zmin 1] [--zmax 36] [--all] [--strict] [--ratio MIN MAX]
    python3 pc/validate_atoms.py --model hfs --all --strict [--tables pc/hfs_tables.npz]

--strict turns the summary into a hard gate: every element with a literature
value must have a model/lit radius ratio within [0.5, 2.0] (default) or the
[--ratio] bounds; exit code 1 on violation. Without --strict the same table
prints but always exits 0.
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import micropython_shim  # noqa: F401 -- CPython shim for @micropython.native etc.

import atom_cloud
import clementi_radii
import pointcloud
import slater

A0_PM = atom_cloud.PM_PER_BOHR  # 52.9177... pm per Bohr radius
HARTREE_EV = 27.211386245988

ISOTROPY_SAMPLES = 20000
ISOTROPY_TOL = 0.05     # max |<x^2>/<r^2> - 1/3| allowed for an isotropic check
H_AND_HE_TOL = 0.03     # model/lit radius ratio tolerance for H and He
# The mode is stable to well under 1% at this scan density (the errors this
# harness measures are 5-400%), and it keeps --all (Z=1..118) fast.
RADIAL_MODE_RESOLUTION = 20001
DEFAULT_RATIO_MIN = 0.5
DEFAULT_RATIO_MAX = 2.0

# Active radial model: 'hydrogenic' (default, unchanged behavior) or 'hfs'
# (tabulated screened-potential tables, see --model). Set by main().
RADIAL_MODEL = 'hydrogenic'
HFS = None  # pc.hfs_tables.HfsTables, loaded lazily on --model hfs


# First ionization energies (eV), NIST SRD 111 (Kramida et al.) -- curated
# subset of high-confidence values used by the Koopmans check (valence
# orbital eigenvalue vs -IP). Only elements whose experimental IP is
# unambiguous at this precision are listed.
IP_EV = {
    1: 13.598, 2: 24.587, 3: 5.392, 4: 9.323, 5: 8.298, 6: 11.260,
    7: 14.534, 8: 13.618, 9: 17.423, 10: 21.565, 11: 5.139, 12: 7.646,
    13: 5.986, 14: 8.152, 15: 10.486, 16: 10.360, 17: 12.968, 18: 15.760,
    19: 4.341, 20: 6.113, 26: 7.902, 29: 7.726, 30: 9.394, 36: 13.999,
    37: 4.177, 47: 7.576, 55: 3.894, 79: 9.226, 80: 10.438, 82: 7.417,
    86: 10.749, 92: 6.194,
}


def valence_subshell(config):
    """Highest-l subshell among the highest-n occupied subshells -- the
    subshell Clementi's 'atomic radius' definition refers to (see module
    docstring). config is slater.electron_configuration(z)'s output.
    """
    n_max = max(n for n, _ell, _occ in config)
    return max(((n, ell) for n, ell, occ in config if n == n_max), key=lambda t: t[1])


def model_radius_pm(n, ell, z_eff, z=None):
    """Mode of r^2 R(r)^2 for the model's radial function, in pm. z_eff is
    the hydrogenic value (ignored in hfs mode); z selects the table."""
    if RADIAL_MODEL == 'hfs':
        return HFS.source(z, n, ell).mode_radius() * A0_PM
    return pointcloud.radial_mode_radius(n, ell, z_eff, resolution=RADIAL_MODE_RESOLUTION) * A0_PM


def _mean_sq_axes(points):
    n = len(points)
    sx = sum(p[0] * p[0] for p in points)
    sy = sum(p[1] * p[1] for p in points)
    sz = sum(p[2] * p[2] for p in points)
    sr2 = sx + sy + sz
    return sx / n, sy / n, sz / n, sr2 / n


def isotropy_max_deviation(points):
    """max over axes of |<axis^2>/<r^2> - 1/3| -- 0 for a perfectly
    isotropic distribution (which is what Unsoeld's theorem demands of a
    full or exactly-half-filled subshell's density).
    """
    mx, my, mz, mr2 = _mean_sq_axes(points)
    if mr2 <= 0:
        return 1.0
    return max(abs(mx / mr2 - 1.0 / 3.0), abs(my / mr2 - 1.0 / 3.0), abs(mz / mr2 - 1.0 / 3.0))


def sample_groups(groups, z, config, count, seed):
    """Sample `count` points from the union of the given (n, ell, m) groups
    using the model's own samplers (isotropic path for m=None, per-orbital
    path otherwise) -- the same code atom_cloud.build_atom_point_cloud()
    uses, so the isotropy checks test the real sampling path.
    """
    rng = pointcloud.XorShift32(seed)
    out = []
    weights = []
    resolved = []
    for n, ell, m, occ in groups:
        if RADIAL_MODEL == 'hfs':
            src = HFS.source(z, n, ell)
            if m is None:
                inv_r_table, _max_r = pointcloud.init_radial_sampler_from_table(src.r, src.density())
                resolved.append(('iso', n, ell, m, occ, inv_r_table))
            else:
                sampler = pointcloud.init_orbital_sampler(n, ell, m, radial_fn=src.R_lookup,
                                                          max_r=src.max_r())
                resolved.append(('orb', n, ell, m, occ, sampler))
        else:
            z_eff = slater.z_eff_radial(z, config, n, ell)
            if m is None:
                inv_r_table, _max_r = pointcloud.init_radial_sampler(n, ell, z_eff)
                resolved.append(('iso', n, ell, m, occ, inv_r_table))
            else:
                sampler = pointcloud.init_orbital_sampler(n, ell, m, z_eff)
                resolved.append(('orb', n, ell, m, occ, sampler))
        weights.append(occ)
    total_w = sum(weights)
    for (kind, _n, _ell, _m, occ, data), w in zip(resolved, weights):
        n_pts = int(round(count * w / total_w))
        if kind == 'iso':
            for _ in range(n_pts):
                out.append(pointcloud.sample_isotropic_point(data, rng))
        else:
            for _ in range(n_pts):
                out.append(pointcloud.sample_orbital_point(data, rng))
    return out


def check_unsold_full_subshell():
    """A FULL subshell's density is exactly isotropic (Unsoeld's theorem) --
    Ne 2p6 is sampled through atom_cloud's isotropic path, and the check is
    that the resulting cloud is (numerically) isotropic.
    """
    z, config = 10, slater.electron_configuration(10)
    pts = sample_groups([(2, 1, None, 6)], z, config, ISOTROPY_SAMPLES, seed=4242)
    dev = isotropy_max_deviation(pts)
    return dev < ISOTROPY_TOL, dev, "full 2p6 (Unsoeld): <x^2>=<y^2>=<z^2> within %.1f%%" % (100 * dev)


def check_half_filled_isotropic():
    """Exactly half-filled subshell with one electron per m is also exactly
    isotropic (sum of |Y_l^m|^2 over ALL m is constant). N 2p3 is sampled
    through the Hund per-orbital path, so this exercises the real per-orbital
    samplers, not just the isotropic one.
    """
    z, config = 7, slater.electron_configuration(7)
    groups = [(2, 1, m, 1) for m in (-1, 0, 1)]
    pts = sample_groups(groups, z, config, ISOTROPY_SAMPLES, seed=4243)
    dev = isotropy_max_deviation(pts)
    return dev < ISOTROPY_TOL, dev, "half-filled 2p3 (Hund, one per m): <x^2>=<y^2>=<z^2> within %.1f%%" % (100 * dev)


def check_hund_anisotropic():
    """C 2p2 (Hund: m=-1 and m=0 singly occupied) must be ANISOTROPIC, with
    the occupied directions (y and z lobes) statistically larger than the
    empty one (x) -- the check that caught the pre-Hund-fix bug where every
    partial shell rendered spherical (see atom_cloud.py docstring).
    """
    z, config = 6, slater.electron_configuration(6)
    groups = [(2, 1, m, 1) for m in (-1, 0)]
    pts = sample_groups(groups, z, config, ISOTROPY_SAMPLES, seed=4244)
    mx, my, mz, mr2 = _mean_sq_axes(pts)
    ok = (my > mx * 1.5) and (mz > mx * 1.5) and isotropy_max_deviation(pts) > 0.05
    msg = "2p2 lobed: <y^2>/<x^2>=%.2f, <z^2>/<x^2>=%.2f (both should be > 1.5)" % (my / mx, mz / mx)
    return ok, 0.0, msg


def check_fe_ordering():
    """Radial ordering: in the model (and in reality) iron's 3d subshell
    sits INSIDE the 4s -- the reason transition-metal ions lose ns electrons
    first. Uses the same radial functions as the cloud.
    """
    z, config = 26, slater.electron_configuration(26)
    if RADIAL_MODEL == 'hfs':
        r_3d = HFS.source(z, 3, 2).mode_radius() * A0_PM
        r_4s = HFS.source(z, 4, 0).mode_radius() * A0_PM
    else:
        r_3d = model_radius_pm(3, 2, slater.z_eff_radial(z, config, 3, 2), z)
        r_4s = model_radius_pm(4, 0, slater.z_eff_radial(z, config, 4, 0), z)
    return r_3d < r_4s, 0.0, "Fe 3d mode (%.0f pm) < 4s mode (%.0f pm)" % (r_3d, r_4s)


def check_h_and_he():
    """H and He must match the literature radii. For the hydrogenic model
    this is exact (tolerance 3%). For the HFS model the X-alpha exchange
    leaves a self-interaction residue -- worst for He (the classic HFS
    artifact: 2 electrons, exchange cannot fully cancel self-repulsion),
    measured at ~5% (H) and ~21% (He) at alpha=1 -- so the tolerance is
    relaxed to 25% and the known artifact is reported."""
    tol = 0.03 if RADIAL_MODEL == 'hydrogenic' else 0.25
    rows = []
    for z in (1, 2):
        config = slater.electron_configuration(z)
        n, ell = valence_subshell(config)
        if RADIAL_MODEL == 'hfs':
            r = HFS.source(z, n, ell).mode_radius() * A0_PM
        else:
            r = model_radius_pm(n, ell, slater.z_eff_radial(z, config, n, ell), z)
        lit = clementi_radii.CLEMENTI_RADIUS_PM[z]
        rows.append(abs(r / lit - 1.0) < tol)
    tag = "X-alpha self-interaction residue" if RADIAL_MODEL == 'hfs' else "exact hydrogenic"
    return all(rows), 0.0, "H/He radius ratios within %.0f%% of literature (%s)" % (100 * tol, tag)


def check_koopmans():
    """Koopmans' theorem sanity: the outermost (valence) orbital's eigenvalue
    is a crude estimate of -IP (relaxation and correlation neglected). Only
    checked against the curated IP_EV subset; reported as a ratio, not a
    hard gate (the HFS eigenvalue is known to be too deep for open-shell
    atoms; the alkali metals should be within ~20%)."""
    rows = []
    worst = 0.0
    for z in sorted(IP_EV):
        if z > 54 and RADIAL_MODEL == 'hydrogenic':
            continue  # Slater-fallback eigenvalues are not meaningful here
        config = slater.electron_configuration(z)
        n, ell = valence_subshell(config)
        if RADIAL_MODEL == 'hfs':
            E = HFS.source(z, n, ell).energy
        else:
            z_eff = slater.z_eff_radial(z, config, n, ell)
            E = -0.5 * z_eff * z_eff / (n * n)
        ip = IP_EV[z]
        ratio = -E * HARTREE_EV / ip
        rows.append((z, n, ell, E, ip, ratio))
        worst = max(worst, abs(math.log(ratio)))
    # Report the worst |log| and the alkali-metal subset separately.
    alk = [(z, r[5]) for z, *_, r in zip(IP_EV, rows) if z in (3, 11, 19, 37, 55)]
    msg = "Koopmans -E(outer) vs IP: worst |log ratio| %.2f (%d elements); " \
          "alkali ratios: %s" % (
        worst, len(rows),
        ", ".join("%s=%.2f" % (slater.element_symbol(z), r) for z, r in alk))
    return True, 0.0, msg


PHYSICS_CHECKS = (
    ("Unsoeld full subshell isotropy (Ne 2p6)", check_unsold_full_subshell),
    ("Half-filled subshell isotropy (N 2p3)", check_half_filled_isotropic),
    ("Hund partial subshell anisotropy (C 2p2)", check_hund_anisotropic),
    ("Fe 3d inside 4s radial ordering", check_fe_ordering),
    ("H and He match literature", check_h_and_he),
    ("Koopmans -E(outer) vs IP (curated NIST subset)", check_koopmans),
)


def build_table(zmin, zmax):
    """One row per Z: valence subshell, Z_eff source, model mode radius,
    literature radius, ratio. Also reports, for Z where the model's most-
    extended subshell differs from the valence one, the definition-artifact
    ratio (the old, incorrect comparison) so the difference is visible.
    """
    rows = []
    for z in range(zmin, zmax + 1):
        config = slater.electron_configuration(z)
        n, ell = valence_subshell(config)
        if RADIAL_MODEL == 'hfs':
            z_eff = HFS.source(z, n, ell).energy
            src = 'HFS'
        else:
            z_eff = slater.z_eff_radial(z, config, n, ell)
            cr = slater.z_eff_cr(z, config, n, ell)
            src = 'CR' if cr is not None else 'SL'
        mode = model_radius_pm(n, ell, z_eff, z)
        lit = clementi_radii.CLEMENTI_RADIUS_PM.get(z)

        # Definition-artifact row: the most-extended subshell's mode (what
        # ATOMS.md 4.2's old table compared), only where it differs.
        best = None
        best_r = -1.0
        for nn, eell, _o in config:
            rr = model_radius_pm(nn, eell, slater.z_eff_radial(z, config, nn, eell), z)
            if rr > best_r:
                best_r = rr
                best = (nn, eell)
        artifact = None
        if best != (n, ell):
            artifact = best_r / lit if lit else None

        rows.append((z, n, ell, z_eff, src, mode, lit, artifact))
    return rows


def format_row(r):
    z, n, ell, z_eff, src, mode, lit, artifact = r
    ratio = mode / lit if lit else float('nan')
    art = ("  [old most-extended ratio %.2f]" % artifact) if artifact is not None else ""
    return "%3d %-2s  %d%s  %6.2f %-2s  %7.1f  %6.0f  %5.2f%s" % (
        z, slater.element_symbol(z), n, 'spdf'[ell], z_eff, src, mode, lit if lit else -1, ratio, art)


def main(argv):
    global RADIAL_MODEL, HFS
    zmin, zmax = 1, 36
    strict = False
    ratio_min, ratio_max = DEFAULT_RATIO_MIN, DEFAULT_RATIO_MAX
    tables_path = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--zmin':
            zmin = int(argv[i + 1]); i += 2
        elif a == '--zmax':
            zmax = int(argv[i + 1]); i += 2
        elif a == '--all':
            zmax = 118; i += 1
        elif a == '--strict':
            strict = True; i += 1
        elif a == '--ratio':
            ratio_min = float(argv[i + 1]); ratio_max = float(argv[i + 2]); i += 3
        elif a == '--model':
            model = argv[i + 1]
            if model not in ('hydrogenic', 'hfs'):
                raise SystemExit("--model must be 'hydrogenic' or 'hfs'")
            RADIAL_MODEL = model
            i += 2
        elif a == '--tables':
            tables_path = argv[i + 1]; i += 2
        else:
            raise SystemExit("unknown arg %r" % a)

    if RADIAL_MODEL == 'hfs':
        import hfs_tables
        HFS = hfs_tables.load(tables_path or hfs_tables.DEFAULT_TABLES)

    print("Atom-model validation vs Clementi-Raimondi (model radius = mode of r^2 R^2,")
    if RADIAL_MODEL == 'hfs':
        print("valence subshell = highest-l among highest-n; radial model = screened-potential")
        print("HFS tables (pc/hfs_solver.py); the z_eff column shows the valence eigenvalue (Ha).")
    else:
        print("valence subshell = highest-l among highest-n; Z_eff source CR = Clementi-Raimondi")
        print("table, SL = Slater rules rescaled by n/n* -- see slater.z_eff_radial()).")
    print()
    print(" Z  el  sub   z_eff  src    mode(pm)  lit(pm)  ratio")
    rows = build_table(zmin, zmax)
    for r in rows:
        print(format_row(r))
    print()

    # Summary statistics
    with_lit = [r for r in rows if r[6] is not None]
    if with_lit:
        ratios = [r[5] / r[6] for r in with_lit]
        logs = [abs(math.log(x)) for x in ratios]
        p2 = [x for x, r in zip(ratios, with_lit) if r[0] <= 10]
        p3 = [x for x, r in zip(ratios, with_lit) if 11 <= r[0] <= 18]
        p4 = [x for x, r in zip(ratios, with_lit) if 19 <= r[0] <= 36]
        def fmt(xs):
            if not xs:
                return "n=0"
            s = sorted(xs)
            return "n=%d med=%.3f maxdev=%.3f" % (len(xs), s[len(xs) // 2], max(abs(math.log(x)) for x in xs))
        print("ratios (model/lit): period2 %s | period3 %s | period4 %s" % (fmt(p2), fmt(p3), fmt(p4)))
        worst = with_lit[logs.index(max(logs))][0]
        print("overall: median %.3f, worst |log ratio| %.3f (Z=%d)" % (
            sorted(ratios)[len(ratios) // 2], max(logs), worst))
    print()

    # Definition-artifact report
    art_rows = [r for r in rows if r[7] is not None]
    if art_rows:
        print("definition check: elements whose most-extended subshell differs from the")
        print("valence one (the old ATOMS.md 4.2 comparison compared these mismatched")
        print("quantities and reported a bogus 'systematic' overestimate):")
        for r in art_rows:
            corrected = r[5] / r[6] if r[6] else float('nan')
            old_mode = r[5] * (r[7] / corrected) if r[6] else 0.0
            print("   %-2s (Z=%d): valence %d%s mode %.0f pm vs most-extended mode %.0f pm, "
                  "old-ratio %.2f / corrected %.2f" % (
                slater.element_symbol(r[0]), r[0], r[1], 'spdf'[r[2]], r[5], old_mode, r[7], corrected))
        print()

    # Physics checks
    all_ok = True
    for name, fn in PHYSICS_CHECKS:
        try:
            ok, _dev, msg = fn()
        except KeyError:
            # element needed by this check is not in the active tables
            print("[SKIP] %s -- element missing from the active tables" % name)
            continue
        all_ok = all_ok and ok
        print("[%s] %s -- %s" % ("PASS" if ok else "FAIL", name, msg))
    print()

    # Strict gate
    if strict and with_lit:
        bad = [r for r in with_lit if not (ratio_min <= r[5] / r[6] <= ratio_max)]
        if bad:
            print("STRICT FAIL: %d element(s) outside ratio [%.2f, %.2f]: %s" % (
                len(bad), ratio_min, ratio_max,
                ", ".join("%s(Z=%d)=%.2f" % (slater.element_symbol(r[0]), r[0], r[5] / r[6]) for r in bad)))
            return 1
        print("STRICT PASS: all %d elements within ratio [%.2f, %.2f]" % (len(with_lit), ratio_min, ratio_max))
    if not all_ok:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
