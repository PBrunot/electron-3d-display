"""Unified PC entry point (see pc/main.py): an initial chooser screen
("Hydrogen Orbitals" vs "Element Explorer") with a randomly picked tumbling
preset playing behind it as a backdrop, then hands off to whichever viewer
the user picks. Escape inside either viewer returns here (see
orbital_view_pc.OrbitalViewApp._request_exit()/atom_view_pc.AtomViewApp's
matching method).

One shared tk.Tk() root/Canvas/image item is created once here and reused
across all three scenes -- the chooser and both viewer apps -- so switching
between them never opens or closes a window; each scene just takes over the
existing canvas (see OrbitalViewApp/AtomViewApp's `root=`/`canvas=`/
`image_id=` constructor params) and unbinds its own key/mouse bindings when
it hands control back (see those classes' stop()).

    python3 pc/main.py

For direct CLI-argument testing without the chooser (e.g. jumping straight
to a specific element), pc/atom_main.py's standalone entry point still works
unchanged.
"""

import math
import os
import random
import sys

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import slater

import tkinter as tk

from viewer_common import (
    WIDTH, HEIGHT, DISPLAY_SIZE, FRAME_DELAY_MS,
    ANGLE_STEP, _TILT_ANGLE_START, _ROLL_ANGLE_START,
    render_frame, blit_to_canvas,
)

import cloud_common
import orbital_view_pc
import atom_view_pc

CHOICE_ORBITALS = 'orbitals'
CHOICE_ATOM = 'atom'
CHOICE_ORDER = [CHOICE_ORBITALS, CHOICE_ATOM]
CHOICE_LABELS = {
    CHOICE_ORBITALS: 'Hydrogen Orbitals',
    CHOICE_ATOM: 'Element Explorer',
}

# Canvas-item coordinates, NOT viewer_common.CENTER -- the chooser's buttons
# are plain tkinter Canvas items layered on top of the rendered image, so
# they live in the CANVAS's own coordinate space (DISPLAY_SIZE, the
# on-screen window size) rather than the small WIDTH x HEIGHT math buffer
# render_frame() draws into.
#
# Sized as FRACTIONS of DISPLAY_SIZE, not fixed pixel counts -- PC's window
# happens to be fixed-size, but these same fractions are also the reference
# the web (web/py/web_chooser.py) and device (micropython/chooser.py) ports
# use on their own, much smaller canvases, so one set of numbers defines the
# chooser's proportions everywhere instead of three independently-tuned ones.
DISPLAY_CENTER = DISPLAY_SIZE[0] // 2
BUTTON_WIDTH_FRAC = 0.44
BUTTON_HEIGHT_FRAC = 0.0875
BUTTON_FONT_FRAC = 0.027   # of DISPLAY_SIZE[1], rounded to an int point size
TITLE_FONT_FRAC = 0.017
HINT_FONT_FRAC = 0.0135
TITLE_GAP_FRAC = 0.052     # title baseline above the top button, as a gap

BUTTON_WIDTH = round(DISPLAY_SIZE[0] * BUTTON_WIDTH_FRAC)
BUTTON_HEIGHT = round(DISPLAY_SIZE[1] * BUTTON_HEIGHT_FRAC)
BUTTON_FONT = ('Helvetica', round(DISPLAY_SIZE[1] * BUTTON_FONT_FRAC), 'bold')
TITLE_FONT = ('Helvetica', round(DISPLAY_SIZE[1] * TITLE_FONT_FRAC))
HINT_FONT = ('Helvetica', round(DISPLAY_SIZE[1] * HINT_FONT_FRAC))
TITLE_GAP = round(DISPLAY_SIZE[1] * TITLE_GAP_FRAC)

COLOR_NORMAL_TEXT = '#c8c8c8'
COLOR_NORMAL_BG = '#141414'
COLOR_NORMAL_OUTLINE = '#3a3a3a'
COLOR_SELECTED_TEXT = '#101010'
COLOR_SELECTED_BG = '#ffdc28'
COLOR_SELECTED_OUTLINE = '#ffdc28'
COLOR_HINT = '#888888'

TITLE_TEXT = 'Choose a viewer'
HINT_TEXT = 'Up/Down or click to choose, Enter to confirm'


class ChooserScene:
    """Backdrop-tumble + two-button chooser. Deliberately minimal compared
    to OrbitalViewApp/AtomViewApp: no zoom breathing, no fly-overs, no
    dissection -- just a steady rotation so the screen isn't static while
    the user decides, per "an animation should be randomly played behind
    the choices". Which content tumbles (a random hydrogen orbital or a
    random element) is re-rolled every time the chooser is (re)shown.
    """

    def __init__(self, root, canvas, image_id, on_choice):
        self.root = root
        self.canvas = canvas
        self.image_id = image_id
        self.on_choice = on_choice

        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.photo = None  # kept alive on self; see viewer_common.blit_to_canvas()

        if random.random() < 0.5:
            index = random.randrange(len(cloud_common.ORBITAL_PRESETS))
            self.preset = orbital_view_pc.Preset(index)
        else:
            z = random.randint(1, slater.MAX_Z)
            self.preset = atom_view_pc.AtomPreset(z)
        self.scale = self.preset.base_scale

        self.angle = random.uniform(0, 2 * math.pi)
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.two_pi = 2 * math.pi

        self.selection = CHOICE_ORBITALS
        self._bound_sequences = []
        self._after_id = None
        self.active = True

        self._bind('<Up>', lambda e: self._move(-1))
        self._bind('<Down>', lambda e: self._move(1))
        self._bind('<Left>', lambda e: self._move(-1))
        self._bind('<Right>', lambda e: self._move(1))
        self._bind('<Return>', lambda e: self._confirm())
        self._bind('<KP_Enter>', lambda e: self._confirm())
        self.canvas.focus_set()

        self._create_widgets()
        self._tick()

    def _bind(self, sequence, handler):
        self.canvas.bind(sequence, handler)
        self._bound_sequences.append(sequence)

    def _move(self, direction):
        idx = CHOICE_ORDER.index(self.selection)
        self.selection = CHOICE_ORDER[(idx + direction) % len(CHOICE_ORDER)]
        self._update_widgets()

    def _confirm(self):
        choice = self.selection
        self.stop()
        self.on_choice(choice)

    def _click(self, choice):
        self.selection = choice
        self._confirm()

    def _create_widgets(self):
        # Centered on the 1/3 and 2/3 height marks, not clustered around the
        # middle -- the tumbling backdrop is densest at vertical center, so
        # this keeps it clearly visible between the two buttons instead of
        # covered by them.
        y1 = DISPLAY_SIZE[1] // 3 - BUTTON_HEIGHT // 2
        y2 = DISPLAY_SIZE[1] * 2 // 3 - BUTTON_HEIGHT // 2

        self.title_id = self.canvas.create_text(
            DISPLAY_CENTER, y1 - TITLE_GAP, text=TITLE_TEXT, font=TITLE_FONT, fill=COLOR_HINT)

        self.rect_ids = {}
        self.text_ids = {}
        for choice, y0 in ((CHOICE_ORBITALS, y1), (CHOICE_ATOM, y2)):
            rect = self.canvas.create_rectangle(
                DISPLAY_CENTER - BUTTON_WIDTH // 2, y0,
                DISPLAY_CENTER + BUTTON_WIDTH // 2, y0 + BUTTON_HEIGHT,
                fill=COLOR_NORMAL_BG, outline=COLOR_NORMAL_OUTLINE, width=2)
            text = self.canvas.create_text(
                DISPLAY_CENTER, y0 + BUTTON_HEIGHT // 2, text=CHOICE_LABELS[choice],
                font=BUTTON_FONT, fill=COLOR_NORMAL_TEXT)
            self.rect_ids[choice] = rect
            self.text_ids[choice] = text
            for item in (rect, text):
                self.canvas.tag_bind(item, '<Button-1>', lambda e, c=choice: self._click(c))

        self._update_widgets()

    def _update_widgets(self):
        for choice in CHOICE_ORDER:
            selected = choice == self.selection
            self.canvas.itemconfig(self.rect_ids[choice],
                                    fill=COLOR_SELECTED_BG if selected else COLOR_NORMAL_BG,
                                    outline=COLOR_SELECTED_OUTLINE if selected else COLOR_NORMAL_OUTLINE)
            self.canvas.itemconfig(self.text_ids[choice],
                                    fill=COLOR_SELECTED_TEXT if selected else COLOR_NORMAL_TEXT)

    def stop(self):
        self.active = False
        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
            self._after_id = None
        for sequence in self._bound_sequences:
            self.canvas.unbind(sequence)
        for item in (self.title_id, *self.rect_ids.values(), *self.text_ids.values()):
            self.canvas.delete(item)

    def _tick(self):
        if not self.active:
            return
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, self.scale)
        blit_to_canvas(self, lambda draw: None)  # buttons are separate Canvas items, no PIL overlay
        self.angle = (self.angle + ANGLE_STEP) % self.two_pi
        self._after_id = self.root.after(FRAME_DELAY_MS, self._tick)


ORBITAL_HINT_TEXT = "Arrow keys = nudge (switch orbital). Esc = back to menu."
ATOM_HINT_TEXT = ("Up/Down = change element (Z). Mouse wheel or +/- = zoom. "
                   "D = dissect orbitals. Esc = back to menu.")


class Launcher:
    """Owns the single shared window/canvas/image item and swaps the active
    scene (chooser <-> orbital viewer <-> atom viewer) in place. Also owns
    the one instructional Label below the canvas (OrbitalViewApp/
    AtomViewApp only create their own when run standalone -- see their
    `owns_root` checks -- since here that widget already exists and is
    reused, its text just swapped per scene).
    """

    def __init__(self):
        self.root = tk.Tk()
        self.root.title('Atom / Orbital Viewer')

        self.canvas = tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                 bg='black', highlightthickness=0)
        self.canvas.pack()
        self.canvas.focus_set()
        self.image_id = self.canvas.create_image(0, 0, anchor='nw')

        self.hint_label = tk.Label(self.root, text=HINT_TEXT, fg='white', bg='black')
        self.hint_label.pack(fill='x')

        self.scene = None
        self._show_chooser()

    def _show_chooser(self):
        print("launcher: -> chooser")
        self.hint_label.config(text=HINT_TEXT)
        self.scene = ChooserScene(self.root, self.canvas, self.image_id, self._on_choice)
        print("launcher: chooser ready, scene=%r" % (self.scene,))

    def _on_choice(self, choice):
        print("launcher: choice=%r" % (choice,))
        if choice == CHOICE_ORBITALS:
            self.hint_label.config(text=ORBITAL_HINT_TEXT)
            self.scene = orbital_view_pc.OrbitalViewApp(
                root=self.root, canvas=self.canvas, image_id=self.image_id, on_exit=self._show_chooser)
        else:
            self.hint_label.config(text=ATOM_HINT_TEXT)
            self.scene = atom_view_pc.AtomViewApp(
                root=self.root, canvas=self.canvas, image_id=self.image_id, on_exit=self._show_chooser)
        print("launcher: %r ready, scene=%r" % (choice, self.scene))

    def run(self):
        self.root.mainloop()


def run():
    Launcher().run()


if __name__ == '__main__':
    run()
