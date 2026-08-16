"""Run micropython/ modules unmodified under CPython for PC-side debugging.

MicroPython-only constructs that would otherwise fail on CPython:

- `@micropython.native` / `@micropython.viper` decorators: on the device
  these are compiler hints, not runtime lookups, so orbitals.py/pointcloud.py
  never `import micropython` themselves. We inject a shim object into
  `builtins` so CPython resolves the name from any module; both decorators
  are identity functions (CPython bytecode is already fast enough).
- `time.ticks_ms()` / `ticks_diff()` / `ticks_add()`: MicroPython's
  wrapping-safe millisecond counter, used by nudge.py's cooldown timer.
  Implemented as plain monotonic-time arithmetic (CPython's monotonic()
  never wraps within a run).

Import this before importing anything from micropython/.
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
