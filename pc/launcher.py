"""Unified PC entry point (see pc/main.py): a 2s boot splash (the atomic-cube
image, port of the device's kSplashHoldMs boot screen), then a chooser screen
("Orbitals" vs "Atoms") over the SAME static splash image as its background
-- port of the device's chooser ("the chooser screen shall have the same
fixed splash screen background - no animation needed"; no "ATOM CUBE" title
text over the image, and plain option names since the PC doesn't gesture
UP/DOWN) -- then hands off to whichever viewer the user picks. Escape inside
either viewer returns here.

One shared tk.Tk() root/Canvas/image item is created once here and reused
across all scenes -- the splash, the chooser and both viewer apps -- so
switching between them never opens or closes a window; each scene just takes
over the existing canvas (see OrbitalViewApp/AtomViewApp's `root=`/`canvas=`/
`image_id=` constructor params) and unbinds its own key/mouse bindings when
it hands control back (see those classes' stop()).

    python3 pc/main.py

For direct CLI-argument testing without the chooser (e.g. jumping straight
to a specific element), pc/atom_main.py's standalone entry point still works
unchanged.
"""

import os
import sys

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import tkinter as tk
from PIL import Image, ImageTk

from viewer_common import DISPLAY_SIZE

import orbital_view_pc
import atom_view_pc

CHOICE_ORBITALS = 'orbitals'
CHOICE_ATOM = 'atom'
CHOICE_ORDER = [CHOICE_ORBITALS, CHOICE_ATOM]
CHOICE_LABELS = {
    # Plain names -- no UP/DOWN prefixes: the PC navigates with arrow keys,
    # so the option labels just read as "Orbitals"/"Atoms". The device
    # keeps its own "UP: Orbitals"/"DOWN: Elements" wording, since it
    # navigates with real tilt gestures.
    CHOICE_ORBITALS: 'Orbitals',
    CHOICE_ATOM: 'Atoms',
}

# Canvas-item coordinates, NOT viewer_common.CENTER -- the chooser's buttons
# are plain tkinter Canvas items layered on top of the splash image, so they
# live in the CANVAS's own coordinate space (DISPLAY_SIZE, the on-screen
# window size) rather than the small WIDTH x HEIGHT math buffer the viewers
# render into.
#
# Sized as FRACTIONS of DISPLAY_SIZE, not fixed pixel counts -- PC's window
# happens to be fixed-size, but these same fractions are also the reference
# the web (web/py/web_chooser.py) and device (micropython/chooser.py) ports
# use on their own, much smaller canvases, so one set of numbers defines the
# chooser's proportions everywhere instead of three independently-tuned ones.
DISPLAY_CENTER = DISPLAY_SIZE[0] // 2
BUTTON_WIDTH_FRAC = 0.50
BUTTON_HEIGHT_FRAC = 0.10
BUTTON_FONT_FRAC = 0.042   # bigger options, port of the device's scaled-up menu font
HINT_FONT_FRAC = 0.0135
BUTTON1_Y_FRAC = 0.4375    # device kChooserOption1Y=105 on 240 -> fraction of the canvas height
BUTTON2_Y_FRAC = 0.6875    # device kChooserOption2Y=165 on 240

BUTTON_WIDTH = round(DISPLAY_SIZE[0] * BUTTON_WIDTH_FRAC)
BUTTON_HEIGHT = round(DISPLAY_SIZE[1] * BUTTON_HEIGHT_FRAC)
BUTTON_FONT = ('Helvetica', round(DISPLAY_SIZE[1] * BUTTON_FONT_FRAC), 'bold')
HINT_FONT = ('Helvetica', round(DISPLAY_SIZE[1] * HINT_FONT_FRAC))
BUTTON1_Y = round(DISPLAY_SIZE[1] * BUTTON1_Y_FRAC)
BUTTON2_Y = round(DISPLAY_SIZE[1] * BUTTON2_Y_FRAC)

COLOR_NORMAL_TEXT = '#c8c8c8'
COLOR_NORMAL_BG = '#141414'
COLOR_NORMAL_OUTLINE = '#3a3a3a'
COLOR_SELECTED_TEXT = '#101010'
COLOR_SELECTED_BG = '#ffdc28'
COLOR_SELECTED_OUTLINE = '#ffdc28'
COLOR_HINT = '#888888'

# No title text over the splash image -- it stands clean, with only the two
# options drawn on top.
HINT_TEXT = 'Up/Down or click to choose, Enter to confirm'

# Boot-splash hold -- port of the device's kSplashHoldMs=2000 in main.cpp.
SPLASH_HOLD_MS = 2000

# The splash image the device embeds as a packed RGB565 array
# (img/atomic_cube.jpg -> src/render/splash_bitmap.h/.cpp); the PC just loads the
# original JPEG directly.
_SPLASH_PATH = os.path.join(os.path.dirname(__file__), '..', 'img', 'atomic_cube.jpg')


def _load_splash_photo():
    """Load img/atomic_cube.jpg, resized to the canvas size, as a PhotoImage
    -- the PC counterpart of the device's kSplashBitmapData. Shared by the
    splash and chooser scenes, which use the same static background.
    """
    image = Image.open(_SPLASH_PATH).convert('RGB')
    image = image.resize(DISPLAY_SIZE, Image.LANCZOS)
    return ImageTk.PhotoImage(image)


class SplashScene:
    """Boot splash: the atomic cube image alone for SPLASH_HOLD_MS, then the
    chooser -- the PC counterpart of main.cpp's drawSplashScreen() +
    vTaskDelay(kSplashHoldMs). No interaction; any key/click skips the wait.
    """

    def __init__(self, root, canvas, image_id, on_done):
        self.root = root
        self.canvas = canvas
        self.image_id = image_id
        self.on_done = on_done
        self.photo = _load_splash_photo()
        self.canvas.itemconfig(self.image_id, image=self.photo)

        self._after_id = None
        self.active = True
        # Any key/click skips the splash (root.bind so focus doesn't matter).
        self._skip_id = self.canvas.bind_all('<Key>', self._skip)
        self._click_id = self.canvas.bind_all('<Button-1>', self._skip)
        self._after_id = self.root.after(SPLASH_HOLD_MS, self._done)

    def _skip(self, event=None):
        if self.active:
            self._done()

    def _done(self):
        if not self.active:
            return
        self.active = False
        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
            self._after_id = None
        self.canvas.unbind_all('<Key>')
        self.canvas.unbind_all('<Button-1>')
        self.on_done()

    def stop(self):
        """Not normally called (the splash hands off itself); defensive."""
        self._done()


class ChooserScene:
    """Static splash-image background + two-button chooser -- the PC
    counterpart of the device's drawChooserScreen() ("same fixed splash
    screen background - no animation needed"): the atomic cube image at full
    brightness behind an electric-blue "ATOM CUBE" title and the two option
    buttons at a bigger font. No per-frame rendering (the background is
    static), unlike the old tumbling-preset backdrop.
    """

    def __init__(self, root, canvas, image_id, on_choice):
        self.root = root
        self.canvas = canvas
        self.image_id = image_id
        self.on_choice = on_choice

        self.photo = _load_splash_photo()
        self.canvas.itemconfig(self.image_id, image=self.photo)

        self.selection = CHOICE_ORBITALS
        self._bound_sequences = []
        self.active = True

        self._bind('<Up>', lambda e: self._move(-1))
        self._bind('<Down>', lambda e: self._move(1))
        self._bind('<Left>', lambda e: self._move(-1))
        self._bind('<Right>', lambda e: self._move(1))
        self._bind('<Return>', lambda e: self._confirm())
        self._bind('<KP_Enter>', lambda e: self._confirm())
        self.canvas.focus_set()

        self._create_widgets()

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
        # No title text -- the splash image stands clean (see the constants
        # comment); just the two option buttons.
        self.rect_ids = {}
        self.text_ids = {}
        for choice, y0 in ((CHOICE_ORBITALS, BUTTON1_Y), (CHOICE_ATOM, BUTTON2_Y)):
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
        for sequence in self._bound_sequences:
            self.canvas.unbind(sequence)
        for item in (*self.rect_ids.values(), *self.text_ids.values()):
            self.canvas.delete(item)


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
        self._show_splash()

    def _show_splash(self):
        print("launcher: -> splash")
        self.scene = SplashScene(self.root, self.canvas, self.image_id, self._show_chooser)

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
