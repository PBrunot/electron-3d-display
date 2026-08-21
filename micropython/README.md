# MicroPython firmware — flashing guide

**Read in / Leggi in:** English · [Italiano](README.it.md)

This folder is the real thing running on the Waveshare panel: boot menu,
orbital viewer, element explorer, IMU-driven nudge gestures, and the shared
math modules also used by `pc/` and the web demo. Flashing it is two
separate steps — the **base MicroPython interpreter** (once, or whenever
you want a newer MicroPython release), then **this project's files**
(every time you change something in here).

## 1. Board & firmware requirements

Target board: **Waveshare ESP32-S3-LCD-1.3** — ESP32-S3R8, dual-core
Xtensa LX7 @ 240 MHz, 512 KB SRAM, **8 MB PSRAM wired in octal/OPI mode**,
16 MB flash. ([wiki](https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3))

Because the PSRAM is octal, you need the **octal-SPIRAM** MicroPython
build for ESP32-S3 — the plain/quad-SPIRAM ESP32-S3 build won't be able to
initialize this board's PSRAM correctly.

Download the latest stable release from
[micropython.org/download/ESP32_GENERIC_S3](https://micropython.org/download/ESP32_GENERIC_S3/)
and pick the **`SPIRAM_OCT`** variant, e.g.
`ESP32_GENERIC_S3-SPIRAM_OCT-<version>.bin` (not the plain `ESP32_GENERIC_S3`
image, and not `SPIRAM_N16R8` — those target different PSRAM wiring).

## 2. Tools

```sh
pip install esptool mpremote
```

## 3. Flash the base MicroPython firmware

Plug the board in via USB-C and identify its serial port:

```sh
ls /dev/tty*      # Linux/WSL — usually /dev/ttyACM0 (native USB-CDC on the S3)
                   # under WSL you may need `usbipd attach` first to hand the
                   # device to the Linux VM
# or check Device Manager on Windows for a COM port
```

If esptool's auto-reset into the bootloader doesn't work, force it
manually: hold **BOOT**, tap **RESET**, release **BOOT**.

```sh
esptool.py --chip esp32s3 --port <PORT> erase_flash
esptool.py --chip esp32s3 --port <PORT> --baud 460800 write_flash -z 0 ESP32_GENERIC_S3-SPIRAM_OCT-<version>.bin
```

`erase_flash` matters here: it clears any old filesystem/partition layout
left over from a previous firmware (e.g. the C++/ESP-IDF build in
`src/`, which uses a completely different partition table) before laying
down MicroPython's own.

Reboot the board, then confirm it's alive:

```sh
mpremote connect <port> repl
```

You should land on a `>>>` prompt. `Ctrl-]` exits the REPL back to your
shell.

## 4. Copy this project's files onto the board

From the repository root:

```sh
mpremote connect <port> fs cp -r micropython/. :
```

This copies every module here (`boot.py`, `main.py`, `chooser.py`,
`orbital_view.py`, `atom_view.py`, the IMU/display drivers, and the data
tables such as `hfs_tables.bin`) flat onto the device's root filesystem,
matching the imports in `main.py`.

Delete `micropython/__pycache__` first if it exists locally (`.pyc`
caches from running things on your PC) — it's harmless on the device but
wastes flash for nothing that's ever imported there.

## 5. Run it

Power-cycling the board is enough: MicroPython auto-runs `boot.py` then
`main.py` on every boot, which launches `chooser.py`'s menu — tilt the
board up for the hydrogen orbital viewer, down for the element explorer.

To run it immediately without a reboot:

```sh
mpremote connect <port> exec "exec(open('main.py').read())"
```

To jump straight into one viewer instead of the menu (useful while
debugging):

```sh
mpremote connect <port> exec "import atom_view; atom_view.run()"
mpremote connect <port> exec "import orbital_view; orbital_view.run()"
```

## Troubleshooting

- **`mpremote run <file>` hangs after a watchdog reset.** Seen on this
  board — looks like a raw-paste-mode / USB-passthrough interaction, not
  a MicroPython or driver bug. Use the `fs cp` + `exec` workflow above
  instead of `mpremote run`.
- **Serial port not found.** Re-check `ls /dev/tty*` (Linux/WSL) or
  Device Manager (Windows) before/after plugging in; under WSL the device
  needs `usbipd` attachment before it shows up as `/dev/ttyACM0`.
- **PSRAM looks missing** (e.g. `import gc; gc.mem_free()` reports only a
  few hundred KB instead of megabytes). You likely flashed the non-octal
  build — reflash with the `SPIRAM_OCT` image from step 1.
- **Files copied but nothing runs / import errors.** Make sure the `cp
  -r micropython/. :` copied into the filesystem *root* (the trailing
  `:` with no path after it), not into a `micropython/` subdirectory —
  `main.py`'s `import chooser` etc. expect everything flat at the root.
