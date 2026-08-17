"""Top-level browser entry point (see index.html): shows the chooser screen
first (Hydrogen Orbitals vs Element Explorer, a randomly picked tumbling
preset playing behind it), then hands off to whichever viewer the user
picks -- the browser counterpart of pc/launcher.py.

Escape works very differently here than on PC (see
pc/orbital_view_pc.OrbitalViewApp._request_exit()/atom_view_pc.AtomViewApp's
matching machinery, an abort-flag-plus-cleanup protocol needed because
pc/viewer_common.fly_over()/the dissection sequence BLOCK the whole
interpreter, calling root.update() in a loop, so an in-progress animation
has to be told to unwind itself). Nothing here ever blocks: every scene's
tick() does exactly one frame of work and returns control to
index.html's requestAnimationFrame loop. So going "back" only ever means
"stop calling this scene's tick() and start calling the chooser's instead"
-- WebApp.on_key('Escape') just reassigns self.active and constructs a
fresh ChooserScene, no interruption protocol needed at all.

One shared canvas (bound once, in WebApp.start()) is reused across all
three scenes, same as pc/launcher.py's shared tk.Canvas -- see
web_common.bind_canvas().
"""

import math
import random

import slater
import cloud_common

import web_common as wc
from web_common import (
    WIDTH, HEIGHT, CENTER, ANGLE_STEP,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    render_frame,
)

import web_orbital
import web_atom

CHOICE_ORBITALS = 'orbitals'
CHOICE_ATOM = 'atom'
CHOICE_ORDER = [CHOICE_ORBITALS, CHOICE_ATOM]
CHOICE_LABELS = {
    # Plain names, matching pc/launcher.py -- the browser also navigates with
    # arrow keys, not gestures, so no UP/DOWN prefixes ("on PC we dont use
    # UP/DOWN only Orbitals or Atoms", 2026-08-17).
    CHOICE_ORBITALS: 'Orbitals',
    CHOICE_ATOM: 'Atoms',
}

# Same fractions as pc/launcher.py -- see that module's comment for why the
# chooser's proportions are defined as fractions of the canvas rather than
# fixed pixel counts (this canvas is WIDTH x HEIGHT = 480 x 480, nowhere
# near PC's 960x960 window, so a literal port of PC's pixel constants would
# have been badly oversized here). No title text over the backdrop, matching
# PC ("remove atom cube text from the splash screen", 2026-08-17 -- the web
# keeps its tumbling backdrop instead of the static image: Pyodide has no
# JPEG decoder, so the splash image can't be loaded in-browser without a JS
# decode bridge).
BUTTON_WIDTH_FRAC = 0.50
BUTTON_HEIGHT_FRAC = 0.10
BUTTON_FONT_FRAC = 0.042
HINT_FONT_FRAC = 0.0135
BUTTON1_Y_FRAC = 0.4375   # device kChooserOption1Y=105 on 240 -> fraction of the canvas height
BUTTON2_Y_FRAC = 0.6875   # device kChooserOption2Y=165 on 240

COLOR_NORMAL_TEXT = (200, 200, 200)
COLOR_NORMAL_BG = (20, 20, 20)
COLOR_NORMAL_OUTLINE = (58, 58, 58)
COLOR_SELECTED_TEXT = (16, 16, 16)
COLOR_SELECTED_BG = (255, 220, 40)
COLOR_SELECTED_OUTLINE = (255, 220, 40)
COLOR_HINT = (136, 136, 136)

ZOOM_FACTOR_STEP = 1.1  # mirrors atom_view_pc.ZOOM_FACTOR_STEP -- see on_wheel()/on_key()


def _button_rects():
    """(x0, y0, x1, y1) per choice, at the device/PC chooser's option
    heights (BUTTON1_Y_FRAC/BUTTON2_Y_FRAC of the canvas). Recomputed (not
    cached) each call -- cheap, and keeps this the single source of truth
    for both drawing and click hit-testing.
    """
    button_width = round(WIDTH * BUTTON_WIDTH_FRAC)
    button_height = round(HEIGHT * BUTTON_HEIGHT_FRAC)
    x0 = CENTER - button_width // 2
    x1 = CENTER + button_width // 2
    y1 = round(HEIGHT * BUTTON1_Y_FRAC)
    y2 = round(HEIGHT * BUTTON2_Y_FRAC)
    return {
        CHOICE_ORBITALS: (x0, y1, x1, y1 + button_height),
        CHOICE_ATOM: (x0, y2, x1, y2 + button_height),
    }


class ChooserScene:
    """Backdrop-tumble + two-button chooser -- browser counterpart of
    pc/launcher.py's ChooserScene. No canvas ITEMS to create/delete here
    (the 2D canvas API is immediate-mode): every tick() redraws the
    backdrop, then the buttons on top, from scratch. No title text (see the
    constants comment).
    """

    def __init__(self, on_choice):
        self.on_choice = on_choice
        self.buf = bytearray(WIDTH * HEIGHT * 3)

        if random.random() < 0.5:
            index = random.randrange(len(cloud_common.ORBITAL_PRESETS))
            self.preset = web_orbital.Preset(index)
        else:
            z = random.randint(1, slater.MAX_Z)
            self.preset = web_atom.AtomPreset(z)
        self.scale = self.preset.base_scale

        self.angle = random.uniform(0, 2 * math.pi)
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.two_pi = 2 * math.pi

        self.selection = CHOICE_ORBITALS
        self.rects = _button_rects()

    def move(self, direction):
        idx = CHOICE_ORDER.index(self.selection)
        self.selection = CHOICE_ORDER[(idx + direction) % len(CHOICE_ORDER)]

    def confirm(self):
        self.on_choice(self.selection)

    def click(self, x, y):
        for choice, (x0, y0, x1, y1) in self.rects.items():
            if x0 <= x <= x1 and y0 <= y <= y1:
                self.selection = choice
                self.confirm()
                return

    def tick(self):
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, self.scale)
        wc.blit_buf(self.buf)
        self._draw_widgets()
        self.angle = (self.angle + ANGLE_STEP) % self.two_pi

    def _draw_widgets(self):
        button_font = round(HEIGHT * BUTTON_FONT_FRAC)

        for choice in CHOICE_ORDER:
            x0, y0, x1, y1 = self.rects[choice]
            selected = choice == self.selection
            wc.draw_rect_canvas(x0, y0, x1, y1,
                                 fill_color=COLOR_SELECTED_BG if selected else COLOR_NORMAL_BG,
                                 outline_color=COLOR_SELECTED_OUTLINE if selected else COLOR_NORMAL_OUTLINE)
            wc.draw_text_canvas(CENTER, (y0 + y1) / 2, CHOICE_LABELS[choice],
                                 COLOR_SELECTED_TEXT if selected else COLOR_NORMAL_TEXT,
                                 font_px=button_font, align='center', baseline='middle')


class WebApp:
    """Owns the shared canvas and routes tick()/on_key()/on_click()/
    on_wheel() to whichever scene is active -- browser counterpart of
    pc/launcher.py's Launcher, minus the window/widget management PC needs
    (no separate hint Label here; index.html's own DOM hint text below the
    canvas is updated per scene the same way, see that file).
    """

    def __init__(self):
        self.active = None  # CHOICE_ORBITALS | CHOICE_ATOM | 'chooser'
        self.chooser = None
        self.orbital = None
        self.atom = None

    def start(self, canvas_id='view'):
        wc.bind_canvas(canvas_id)
        self._show_chooser()

    def _show_chooser(self):
        self.active = 'chooser'
        self.chooser = ChooserScene(self._on_choice)

    def _on_choice(self, choice):
        if choice == CHOICE_ORBITALS:
            self.active = CHOICE_ORBITALS
            self.orbital = web_orbital.WebOrbitalApp()
            self.orbital.start()
        else:
            self.active = CHOICE_ATOM
            self.atom = web_atom.WebAtomApp()
            self.atom.start()

    def tick(self):
        if self.active == 'chooser':
            self.chooser.tick()
        elif self.active == CHOICE_ORBITALS:
            self.orbital.tick()
        elif self.active == CHOICE_ATOM:
            self.atom.tick()

    def on_key(self, key):
        if key == 'Escape':
            if self.active != 'chooser':
                self._show_chooser()
            return

        if self.active == 'chooser':
            if key in ('ArrowUp', 'ArrowLeft'):
                self.chooser.move(-1)
            elif key in ('ArrowDown', 'ArrowRight'):
                self.chooser.move(1)
            elif key in ('Enter', 'Return'):
                self.chooser.confirm()
        elif self.active == CHOICE_ORBITALS:
            if key == 'ArrowUp':
                self.orbital.request_step(1)
            elif key == 'ArrowDown':
                self.orbital.request_step(-1)
        elif self.active == CHOICE_ATOM:
            if key == 'ArrowUp':
                self.atom.request_z(1)
            elif key == 'ArrowDown':
                self.atom.request_z(-1)
            elif key in ('d', 'D'):
                self.atom.request_dissect()
            elif key in ('+', '='):
                self.atom.zoom_by(ZOOM_FACTOR_STEP)
            elif key == '-':
                self.atom.zoom_by(1 / ZOOM_FACTOR_STEP)

    def on_click(self, x, y):
        if self.active == 'chooser':
            self.chooser.click(x, y)

    def on_wheel(self, delta_y):
        if self.active == CHOICE_ATOM:
            self.atom.zoom_by(ZOOM_FACTOR_STEP if delta_y < 0 else 1 / ZOOM_FACTOR_STEP)

    def hint_text(self):
        if self.active == 'chooser':
            return 'Up/Down or click to choose, Enter to confirm'
        if self.active == CHOICE_ORBITALS:
            return 'Up/Down = change orbital. Esc = back to menu.'
        return ('Up/Down = change element (Z). Mouse wheel or +/- = zoom. '
                'D = dissect orbitals. Esc = back to menu.')

    def dissect_enabled(self):
        """Dissection is an Element Explorer-only feature -- a single
        hydrogen orbital has no shells to peel apart (see
        pc/atom_view_pc.py's module docstring). index.html polls this every
        frame to gray out btn-dissect outside CHOICE_ATOM, and also while
        one is already running (self.atom.dissecting) since clicking it
        again mid-sequence would just be a no-op (see
        WebAtomApp.request_dissect()) -- doubling as the "dissection is in
        progress" visual cue this button needed anyway.
        """
        return self.active == CHOICE_ATOM and not self.atom.dissecting


app = WebApp()


def init(canvas_id='view'):
    app.start(canvas_id)


def tick():
    app.tick()


def on_key(key):
    app.on_key(key)


def on_click(x, y):
    app.on_click(x, y)


def on_wheel(delta_y):
    app.on_wheel(delta_y)


def hint_text():
    return app.hint_text()


def dissect_enabled():
    return app.dissect_enabled()
