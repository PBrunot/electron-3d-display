"""R1 validation harness for the multi-electron atom model
(pc/atom_view_pc.py / micropython/atom_cloud.py / slater.py).

Compares the model's radii against the Clementi-Raimondi literature values
and runs automated physics checks, so accuracy regressions are caught
instead of being re-derived by hand each session (see ATOMS.md section 4).

Definitions used here (the methodological fix of ATOMS.md section 4.2):
  - "valence subshell" = highest-l subshell among the highest-n occupied
    ones.  This is what Clementi's 'radius of maximum charge density of the
    outermost shell' tables refer to (e.g. carbon's 2p, iron's 4s, krypton's
    4p).  Comparing the model's most-EXTENDED subshell instead (e.g.
    carbon's 2s, which reaches farther than 2p at the same n) reproduces the
    old, bogus 'systematic 22-28% overestimate' of ATOMS.md 4.2 -- that
    artifact is exactly what this harness exists to prevent.
  - "model radius" = mode of r^2*R(z_eff_radial*r)^2 (pointcloud.
    radial_mode_radius()), with the SAME z_eff_radial() the point cloud is
    built with (Clementi-Raimondi Z_eff where the table covers the
    subshell, else Slater's rules rescaled by n/n*).

Usage:
    python3 pc/validate_atoms.py [--zmin 1] [--zmax 36] [--all] [--strict]

--strict turns the summary into a hard gate: every element with a
literature value must have a model/lit radius ratio within [0.5, 2.0]
(default) or [RATIO_MIN, RATIO_MAX] given as extra args; exit code 1 on
violation.  Without --strict the same table prints but always exits 0.
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

ISOTROPY_SAMPLES = 20000
ISOTROPY_TOL = 0.05     # max |<x^2>/<r^2> - 1/3| allowed for an isotropic check
H_AND_HE_TOL = 0.03     # model/lit radius ratio tolerance for H and He


def valence_subshell(config):
    """Highest-l subshell among the highest-n occupied subshells -- the
    subshell Clementi's 'atomic radius' definition refers to (see module
    docstring). config is slater.electron_configuration(z)'s output.
    """
    n_max = max(n for n, _ell, _occ in config)
    return max(((n, ell) for n, ell, occ in config if n == n_max), key=lambda t: t[1])


def model_radius_pm(n, ell, z_eff):
    """Mode of r^2 R(r)^2 for the model's radial substitution, in pm.
    Resolution 20001: the mode is stable to well under 1% at this scan
    density (the physics errors this harness measures are 5-400%), and it
    keeps --all (Z=1..118) usable in a couple of seconds.
    """
    return pointcloud.radial_mode_radius(n, ell, z_eff, resolution=20001) * A0_PM


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
    uses, so the isotropy checks below test the real sampling path.
    Returns a list of (x, y, z) tuples.
    """
    rng = pointcloud.XorShift32(seed)
    out = []
    weights = []
    resolved = []
    for n, ell, m, occ in groups:
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
    """A FULL subshell's density is exactly isotropic (Unsoeld's theorem).
    Ne 2p6 is sampled through atom_cloud's isotropic path; the check is that
    the sampled cloud is (numerically) isotropic.
    """
    z, config = 10, slater.electron_configuration(10)
    pts = sample_groups([(2, 1, None, 6)], z, config, ISOTROPY_SAMPLES, seed=4242)
    dev = isotropy_max_deviation(pts)
    return dev < ISOTROPY_TOL, dev, "full 2p6 (Unsoeld): <x^2>=<y^2>=<z^2> within %.1f%%" % (100 * dev)


def check_half_filled_isotropic():
    """Exactly half-filled subshell with one electron per m is also exactly
    isotropic (sum of |Y_l^m|^2 over ALL m is constant -- same theorem).
    N 2p3 is sampled through the Hund per-orbital path, so this exercises
    the real per-orbital samplers, not just the isotropic one.
    """
    z, config = 7, slater.electron_configuration(7)
    groups = [(2, 1, m, 1) for m in (-1, 0, 1)]
    pts = sample_groups(groups, z, config, ISOTROPY_SAMPLES, seed=4243)
    dev = isotropy_max_deviation(pts)
    return dev < ISOTROPY_TOL, dev, "half-filled 2p3 (Hund, one per m): <x^2>=<y^2>=<z^2> within %.1f%%" % (100 * dev)


def check_hund_anisotropic():
    """C 2p2 (Hund: m=-1 and m=0 singly occupied) must be ANISOTROPIC, with
    the occupied directions (y and z lobes) statistically larger than the
    empty one (x). This is the check that caught the pre-Hund-fix bug where
    every partial shell rendered spherical (see atom_cloud.py docstring).
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
    first. Uses the same z_eff_radial() as the cloud.
    """
    z, config = 26, slater.electron_configuration(26)
    r_3d = model_radius_pm(3, 2, slater.z_eff_radial(z, config, 3, 2))
    r_4s = model_radius_pm(4, 0, slater.z_eff_radial(z, config, 4, 0))
    return r_3d < r_4s, 0.0, "Fe 3d mode (%.0f pm) < 4s mode (%.0f pm)" % (r_3d, r_4s)


def check_h_and_he():
    """H and He must match the literature radii almost exactly -- the model
    is exact hydrogenic for Z<=2, and ATOMS.md 4.2 already verified these.
    """
    rows = []
    for z in (1, 2):
        config = slater.electron_configuration(z)
        n, ell = valence_subshell(config)
        r = model_radius_pm(n, ell, slater.z_eff_radial(z, config, n, ell))
        lit = clementi_radii.CLEMENTI_RADIUS_PM[z]
        rows.append(abs(r / lit - 1.0) < H_AND_HE_TOL)
    return all(rows), 0.0, "H/He radius ratios within %.0f%% of literature" % (100 * H_AND_HE_TOL)


PHYSICS_CHECKS = (
    ("Unsoeld full subshell isotropy (Ne 2p6)", check_unsold_full_subshell),
    ("Half-filled subshell isotropy (N 2p3)", check_half_filled_isotropic),
    ("Hund partial subshell anisotropy (C 2p2)", check_hund_anisotropic),
    ("Fe 3d inside 4s radial ordering", check_fe_ordering),
    ("H and He match literature", check_h_and_he),
)


def build_table(zmin, zmax):
    """One row per Z: valence subshell, Z_eff source, model mode radius,
    literature radius, ratio. Also reports, for Z where the model's most-
    extended subshell differs from the valence one, the definition artifact
    ratio (the OLD, incorrect comparison) so the difference is visible.
    """
    rows = []
    for z in range(zmin, zmax + 1):
        config = slater.electron_configuration(z)
        n, ell = valence_subshell(config)
        z_eff = slater.z_eff_radial(z, config, n, ell)
        cr = slater.z_eff_cr(z, config, n, ell)
        src = 'CR' if cr is not None else 'SL'
        mode = model_radius_pm(n, ell, z_eff)
        lit = clementi_radii.CLEMENTI_RADIUS_PM.get(z)

        # definition-artifact row: the most-extended subshell's mode (what
        # ATOMS.md 4.2's OLD table compared), only where it differs.
        best = None
        best_r = -1.0
        for nn, eell, _o in config:
            rr = model_radius_pm(nn, eell, slater.z_eff_radial(z, config, nn, eell))
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
    zmin, zmax = 1, 36
    strict = False
    ratio_min, ratio_max = 0.5, 2.0
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
        else:
            raise SystemExit("unknown arg %r" % a)

    print("Atom-model validation vs Clementi-Raimondi (model radius = mode of r^2 R^2,")
    print("valence subshell = highest-l among highest-n; Z_eff source CR = Clementi-Raimondi")
    print("table, SL = Slater rules rescaled by n/n* -- see slater.z_eff_radial()).")
    print()
    print(" Z  el  sub   z_eff  src    mode(pm)  lit(pm)  ratio")
    rows = build_table(zmin, zmax)
    for r in rows:
        print(format_row(r))
    print()

    # summary statistics
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

    # definition-artifact report
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

    # physics checks
    all_ok = True
    for name, fn in PHYSICS_CHECKS:
        ok, _dev, msg = fn()
        all_ok = all_ok and ok
        print("[%s] %s -- %s" % ("PASS" if ok else "FAIL", name, msg))
    print()

    # strict gate
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
