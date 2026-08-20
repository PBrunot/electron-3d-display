"""numpy-vectorized point-cloud render core, SHARED by the PC simulator
(pc/viewer_common.py) and the browser port (web/py/web_common.py -- fetched
into Pyodide as render_core.py, see web/index.html's PY_FILES) so the two
can't drift apart. No PIL/tkinter: everything operates on a caller-supplied
(height, width, 3) uint8 numpy view plus plain arrays, so the same code runs
under CPython and Pyodide (both have numpy). The MicroPython device port
never imports this -- it has its own Q8 fixed-point renderer.

The per-point Python loop was the whole reason the PC ran at ~7 fps; this
core is what brings a frame to a few ms (see blend_points()'s docstring for
the exact sequential-blend semantics it preserves).

All functions take the display geometry (width/height/center) and render
style (electron size/alpha, persistence, proton size/color) as explicit
parameters -- each platform binds its own constants at the call site. The
platforms call the numpy path only for electron_size 1 or 2 (the general
case is handled by their pure-Python blend).
"""

try:
    import numpy as np
    _HAS_NUMPY = True
except ImportError:  # numpy is optional; the pure-Python paths in the platform modules still work
    np = None
    _HAS_NUMPY = False


def preset_np(preset):
    """Cached numpy arrays of a preset's point data -- built once and kept on
    the preset as `_np_cache` (point turnover invalidates it by setting it to
    None, see orbital_view_pc.Preset.resample()). Also collects the atom
    preset's shells/ells/signs and per-point shell colors for the dissection
    view. Returns None when numpy isn't installed.
    """
    if not _HAS_NUMPY:
        return None
    cache = getattr(preset, '_np_cache', None)
    if cache is not None:
        return cache
    cache = {
        'xs': np.asarray(preset.xs, dtype=np.float64),
        'ys': np.asarray(preset.ys, dtype=np.float64),
        'zs': np.asarray(preset.zs, dtype=np.float64),
        'colors': np.asarray(preset.colors, dtype=np.uint8),
    }
    if getattr(preset, 'shells', None) is not None:
        # Multi-electron atom preset: shells/ells/signs and the per-point
        # shell color needed by the dissection view's color rules. atom_cloud
        # is shared (micropython/) and importable in both PC and Pyodide.
        import atom_cloud
        cache['shells'] = np.asarray(preset.shells, dtype=np.int32)
        cache['ells'] = np.asarray(preset.ells, dtype=np.int32)
        cache['signs'] = np.asarray(preset.signs, dtype=np.int32)
        shell_rgb = atom_cloud.SHELL_RGB
        cache['shell_colors'] = np.asarray(
            [shell_rgb[s] if s < len(shell_rgb) else shell_rgb[-1] for s in preset.shells],
            dtype=np.uint8)
    preset._np_cache = cache
    return cache


def blend_points(buf_np, xs, ys, zs, colors, cy, sy, cx, sx, cz, sz, scale,
                 width, height, center, electron_size=2, alpha=0.92,
                 buzz_fraction=0.0, clip_rz_max=None, skip_mask=None):
    """Rotate (yaw/tilt/roll -- same transform as pc/viewer_common.py's
    rotate_yaw_tilt_roll()), project, and alpha-blend every point's
    electron_size block into `buf_np` (a (height, width, 3) uint8 array).

    The blend preserves the pure-Python path's exact sequential semantics
    (per hit: v = v*(1-alpha) + c*alpha, in point order) via a per-pixel
    "rounds" loop: pixels are grouped by index, then each round applies one
    hit to every pixel that has at least that many hits -- fully vectorized
    per round, and the number of rounds is just the max hits-per-pixel in
    this frame (small for a sparse point cloud).

    The 2x2 block expansion is POINT-MAJOR (entries [4i..4i+3] are point i's
    corners) so the stable argsort keeps hits at a shared pixel in point
    order, matching the Python sequential blend exactly -- a corner-major
    layout would reverse the blend order at overlapping pixels.

    `clip_rz_max` (dissection) drops points whose post-yaw-and-tilt depth
    exceeds it; `skip_mask` (dissection pass structure) skips points;
    `buzz_fraction` randomly drops a fraction of points (orbital buzz).
    Only electron_size 1 or 2 is supported (the platforms gate on that
    before calling the numpy path).
    """
    n = len(xs)
    if n == 0:
        return
    rx1 = xs * cy + zs * sy
    rz1 = zs * cy - xs * sy
    ry2 = ys * cx - rz1 * sx
    rz = ys * sx + rz1 * cx
    rx3 = rx1 * cz - ry2 * sz
    ry3 = rx1 * sz + ry2 * cz
    px = np.rint(center + rx3 * scale).astype(np.int32)
    py = np.rint(center - ry3 * scale).astype(np.int32)

    ok = (px >= 0) & (px < width) & (py >= 0) & (py < height)
    if clip_rz_max is not None:
        ok &= rz <= clip_rz_max
    if skip_mask is not None:
        ok &= ~skip_mask
    if buzz_fraction:
        ok &= np.random.random(n) >= buzz_fraction
    if not ok.any():
        return
    px = px[ok]
    py = py[ok]
    col = colors[ok]

    size = electron_size
    if size == 2:
        # 2x2 block, point-major (see docstring).
        n_ok = len(px)
        px4 = np.empty(4 * n_ok, dtype=px.dtype)
        py4 = np.empty(4 * n_ok, dtype=py.dtype)
        px4[0::4] = px - 1
        py4[0::4] = py - 1
        px4[1::4] = px - 1
        py4[1::4] = py
        px4[2::4] = px
        py4[2::4] = py - 1
        px4[3::4] = px
        py4[3::4] = py
        px, py = px4, py4
        col = np.repeat(col, 4, axis=0)
    elif size == 1:
        pass
    else:
        # Unsupported size in the numpy path -- the platforms gate sizes 1/2
        # before calling this, so this is defensive only.
        return

    ok2 = (px >= 0) & (px < width) & (py >= 0) & (py < height)
    px = px[ok2]
    py = py[ok2]
    col = col[ok2]
    if px.size == 0:
        return

    pix = py * width + px
    flat = buf_np.reshape(-1, 3)
    order = np.argsort(pix, kind='stable')  # stable: keeps point order within a pixel
    p = pix[order]
    c = col[order]
    is_start = np.empty(p.size, dtype=bool)
    is_start[0] = True
    is_start[1:] = p[1:] != p[:-1]
    uniq = p[is_start]
    starts = np.flatnonzero(is_start)
    hits = np.diff(np.concatenate((starts, [p.size])))
    keep_frac = 1.0 - alpha
    for k in range(1, int(hits.max()) + 1):
        sel = hits >= k
        if not sel.any():
            break
        u = uniq[sel]
        ck = c[starts[sel] + (k - 1)]
        old = flat[u].astype(np.float32)
        flat[u] = np.rint(old * keep_frac + ck * alpha).astype(np.uint8)


def draw_nucleus(buf_np, width, height, center, proton_size, proton_color):
    """The fully-opaque proton_size circle at screen center -- drawn ON TOP
    of the cloud (callers invoke it after blend_points()), so a point landing
    on the same pixel can't dim/hide it.
    """
    radius = proton_size // 2
    x0 = center - radius
    x1 = x0 + proton_size
    y0 = center - radius
    y1 = y0 + proton_size
    yy, xx = np.ogrid[y0:y1, x0:x1]
    mask = (xx - center) ** 2 + (yy - center) ** 2 <= radius * radius
    buf_np[y0:y1, x0:x1][mask] = proton_color


def render_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale,
                    width, height, center, electron_size, alpha, persistence_decay,
                    proton_size, proton_color, buzz_fraction=0.0, enable_persistence=True):
    """numpy fast path of the platform render_frame(): fade `buf` toward
    black by persistence_decay (or clear), blend every point (2x2 blocks at
    `alpha`), then draw the nucleus on top. Mutates `buf` (a bytearray or
    numpy buffer) in place via a zero-copy view.
    """
    buf_np = np.frombuffer(buf, dtype=np.uint8).reshape(height, width, 3)
    if enable_persistence:
        # Same per-byte map as the platforms' pure-Python translate() fade.
        buf_np[:] = (buf_np.astype(np.uint16) * persistence_decay) // 256
    else:
        buf_np[:] = 0
    cy, sy = np.cos(angle), np.sin(angle)
    cx, sx = np.cos(tilt_angle), np.sin(tilt_angle)
    cz, sz = np.cos(roll_angle), np.sin(roll_angle)
    blend_points(buf_np, arr['xs'], arr['ys'], arr['zs'], arr['colors'],
                 cy, sy, cx, sx, cz, sz, scale, width, height, center,
                 electron_size, alpha, buzz_fraction=buzz_fraction)
    draw_nucleus(buf_np, width, height, center, proton_size, proton_color)


def render_dissection_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale,
                               clip_z, active_subshell, dim_color,
                               width, height, center, electron_size, alpha, active_alpha,
                               proton_size, proton_color):
    """numpy fast path of the platform render_dissection_frame() -- same
    two-pass structure and color rules as the Python versions: pass 1 draws
    every point (except the active subshell when one is singled out) in
    `dim_color` at `alpha`; pass 2 draws only the active subshell's points in
    full color (phase colors where a sign is defined, its true SHELL_RGB
    where signs[i]==0 -- not preset.colors, see the Python paths' comment)
    at `active_alpha` (opaque). No persistence (the dissection re-applies a
    clip to a rotating cloud, so a glow would smear the cut edge). Mutates
    `buf` via a zero-copy numpy view.
    """
    import cloud_common
    buf_np = np.frombuffer(buf, dtype=np.uint8).reshape(height, width, 3)
    buf_np[:] = 0

    cy, sy = np.cos(angle), np.sin(angle)
    cx, sx = np.cos(tilt_angle), np.sin(tilt_angle)
    cz, sz = np.cos(roll_angle), np.sin(roll_angle)
    xs, ys, zs = arr['xs'], arr['ys'], arr['zs']
    signs = arr['signs']
    n = len(xs)
    pos = np.asarray(cloud_common.PHASE_POSITIVE_RGB, dtype=np.uint8)
    neg = np.asarray(cloud_common.PHASE_NEGATIVE_RGB, dtype=np.uint8)

    if active_subshell is not None:
        active_sel = (arr['shells'] == active_subshell[0]) & (arr['ells'] == active_subshell[1])
        # Pass 1: everything except the active subshell, flat dim color.
        col_dim = np.empty((n, 3), dtype=np.uint8)
        col_dim[:] = dim_color
        blend_points(buf_np, xs, ys, zs, col_dim, cy, sy, cx, sx, cz, sz, scale,
                     width, height, center, electron_size, alpha,
                     clip_rz_max=clip_z, skip_mask=active_sel)
        # Pass 2: active subshell only, full color (phase or shell), opaque.
        full_colors = np.where(signs[:, None] > 0, pos[None, :],
                               np.where(signs[:, None] < 0, neg[None, :], arr['shell_colors']))
        blend_points(buf_np, xs, ys, zs, full_colors, cy, sy, cx, sx, cz, sz, scale,
                     width, height, center, electron_size, active_alpha,
                     clip_rz_max=clip_z, skip_mask=~active_sel)
    else:
        # Single pass, full colors: phase where a sign is defined, the
        # preset's merged colors where signs[i]==0.
        full_colors = np.where(signs[:, None] > 0, pos[None, :],
                               np.where(signs[:, None] < 0, neg[None, :], arr['colors']))
        blend_points(buf_np, xs, ys, zs, full_colors, cy, sy, cx, sx, cz, sz, scale,
                     width, height, center, electron_size, alpha,
                     clip_rz_max=clip_z)

    draw_nucleus(buf_np, width, height, center, proton_size, proton_color)
