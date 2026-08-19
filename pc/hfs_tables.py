#!/usr/bin/env python3
"""Loader for the HFS screened-potential radial tables produced by
pc/hfs_solver.py (see pc/screened_potential_model.md for the design).

Exposes per-(Z, n, ell) radial sources to the point-cloud samplers and the
validation harness. Platform-agnostic by construction (plain lists/math --
the tables themselves are the device-ready artifact; this module is the PC
reader; the device port will embed the same numbers as PROGMEM arrays).

The npz format (see hfs_solver.save_tables()):
    r            float64[2001]  shared log-uniform grid (Bohr)
    z_list       int32[Z]       atomic numbers present
    z<N>_config  int32[(k,3)]   (n, ell, occ) configuration
    z<N>_<n>_<ell>_u   float32[2001]  u(r) = r*R(r), normalized
    z<N>_<n>_<ell>_E   float64         eigenvalue (Hartree)
    z<N>_<n>_<ell>_occ int32          occupancy

A RadialSource evaluates R(r) = u(r)/r by log-linear interpolation of u on
the shared grid (u is smooth there) and hands out the density u^2 and the
eigenvalue -- everything the samplers and validation need.
"""

import math
import os

import numpy as np

DEFAULT_TABLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hfs_tables.npz')


class RadialSource:
    """Solved radial function for one subshell (n, ell) of one element.

    Duck-typed interface consumed by micropython/atom_cloud.py's
    radial_tables path (see build_atom_point_cloud()): `.r` (grid in Bohr),
    `.u` (u = r*R on that grid), `.max_r()`, `.R_lookup` (callable R(r)).
    """

    __slots__ = ('z', 'n', 'ell', 'occ', 'energy', 'r', 'u')

    def __init__(self, z, n, ell, occ, energy, r_grid, u):
        self.z = z
        self.n = n
        self.ell = ell
        self.occ = occ
        self.energy = energy
        self.r = r_grid
        self.u = u

    def u_at(self, r):
        """u(r) = r*R(r) by linear interpolation on the log grid."""
        return float(np.interp(r, self.r, self.u))

    def R_lookup(self, r):
        """R(r) = u(r)/r -- callable for init_orbital_sampler(radial_fn=...)."""
        if r <= 1e-9:
            return 0.0
        return float(np.interp(r, self.r, self.u)) / r

    def density(self):
        """u^2 = r^2 R^2 on the grid -- the radial probability density shape."""
        return self.u * self.u

    def mode_radius(self):
        """Mode of r^2 R^2 (Bohr), parabolic refinement around the argmax."""
        w = self.u * self.u
        i = int(np.argmax(w))
        r = self.r
        if 0 < i < len(w) - 1:
            x0, x1, x2 = r[i - 1], r[i], r[i + 1]
            y0, y1, y2 = w[i - 1], w[i], w[i + 1]
            denom = (x0 - x1) * (x0 - x2) * (x1 - x2)
            a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / denom
            b = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / denom
            if abs(a) > 1e-30:
                peak = -b / (2.0 * a)
                if r[i - 1] < peak < r[i + 1]:
                    return peak
        return float(r[i])

    def max_r(self):
        return float(self.r[-1])


class HfsTables:
    """All solved subshells of all elements in one npz."""

    def __init__(self, path=DEFAULT_TABLES):
        self.path = path
        data = np.load(path)
        self.r = data['r']
        self.z_list = [int(z) for z in data['z_list']]
        self._data = data
        # (z, n, ell) -> RadialSource
        self._sources = {}
        for z in self.z_list:
            config = [tuple(int(v) for v in row) for row in data['z%d_config' % z]]
            for n, ell, occ in config:
                key = (z, n, ell)
                E = float(data['z%d_%d_%d_E' % (z, n, ell)])
                u = data['z%d_%d_%d_u' % (z, n, ell)]
                self._sources[key] = RadialSource(z, n, ell, occ, E, self.r, u)
            self._configs = getattr(self, '_configs', {})
            self._configs[z] = config

    def has(self, z, n, ell):
        return (z, n, ell) in self._sources

    def source(self, z, n, ell):
        return self._sources[(z, n, ell)]

    def config(self, z):
        return self._configs[z]

    def close(self):
        self._data.close()


def load(path=DEFAULT_TABLES):
    return HfsTables(path)


if __name__ == '__main__':
    import sys
    t = load(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TABLES)
    print("loaded %s: %d elements, r in [%.2e, %.0f] Bohr, %d points" %
          (t.path, len(t.z_list), t.r[0], t.r[-1], len(t.r)))
    for z in (1, 3, 26, 92):
        for n, ell, occ in t.config(z):
            s = t.source(z, n, ell)
            print("  Z=%3d %d%s  occ=%d  E=%10.5f Ha  mode=%7.3f a0 (%6.1f pm)"
                  % (z, n, 'spdf'[ell], occ, s.energy, s.mode_radius(),
                     s.mode_radius() * 52.9177210903))
