"""Lets micropython/orbitals.py, micropython/pointcloud.py, and
micropython/nudge.py import and run UNMODIFIED under CPython -- the whole
point of this pc/ tool (see README.md) is to debug the *same* orbital math
and nudge-detection code the ESP32 runs, not a reimplementation of it that
could quietly drift out of sync.

orbitals.py/pointcloud.py decorate their hot functions with
`@micropython.native`. On real MicroPython that's recognized directly by
the compiler as a decorator *syntax* (a hint about which bytecode emitter
to use for that function), not a normal runtime attribute lookup -- see
orbitals.py's own docstring for why neither file bothers `import
micropython` first. CPython has no such special case: it evaluates
`@micropython.native` as an ordinary decorator expression and needs
`micropython` to be a resolvable name, which normally it isn't --
`import orbitals` under plain CPython fails with
`NameError: name 'micropython' is not defined`.

Fix: inject a `micropython` object into `builtins` (not just this module's
own globals) so it resolves from *any* module without editing
orbitals.py/pointcloud.py at all -- builtins is the only namespace
CPython's normal name-resolution fallback reaches that doesn't require
touching those files. `.native`/`.viper` are both identity decorators
here: on a PC there's no reason to want MicroPython's native/viper
bytecode emitters, plain CPython bytecode is already far faster than
either would be relative to interpreted MicroPython on an ESP32.

Also patches the standard `time` module with MicroPython's `ticks_ms()` /
`ticks_diff()` / `ticks_add()` -- a wrapping-safe millisecond counter API
that CPython's `time` module doesn't have at all (not a compiler-syntax
question like `micropython.native` above, just a genuinely missing
function). micropython/nudge.py's NudgeDetector calls all three for its
cooldown timer. CPython's `time.monotonic()` never wraps in any timeframe
this program runs for, so ticks_diff/ticks_add here are plain subtraction/
addition -- MicroPython's versions handle counter wraparound, which this
shim doesn't need to reproduce for that reason.

Import this before importing anything from micropython/ -- see
orbital_view_pc.py's import order, which does exactly that.
"""

import builtins
import time


class _MicropythonShim:
    @staticmethod
    def native(f):
        return f

    @staticmethod
    def viper(f):
        return f


builtins.micropython = _MicropythonShim()

time.ticks_ms = lambda: int(time.monotonic() * 1000)
time.ticks_diff = lambda a, b: a - b
time.ticks_add = lambda a, b: a + b
