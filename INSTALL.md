# INSTALL — building & flashing the ESP-IDF (C++) firmware

**Read in / Leggi in:** English · [Italiano](INSTALL.it.md)

This covers the `src/` C++ firmware (the PlatformIO/ESP-IDF port), built
and flashed onto the **Waveshare ESP32-S3-LCD-1.3**. For the MicroPython
firmware instead, see [`micropython/README.md`](micropython/README.md).

## Toolchain

| | |
|---|---|
| IDE | VS Code + [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode) |
| Framework | ESP-IDF **6.x** (`platform = espressif32@7.0.1` pins ESP-IDF 6.0.1), driven through PlatformIO — not raw Arduino |
| Language | **C++26** (`-std=gnu++26`) |
| Board | Waveshare ESP32-S3-LCD-1.3 — `boards/WS-ESP32-S3-LCD-1-3.json` |

## 1. Install prerequisites

- [VS Code](https://code.visualstudio.com/)
- The **PlatformIO IDE** extension, from the VS Code marketplace. It
  bundles its own Python and manages the ESP-IDF 6.x toolchain itself —
  no separate ESP-IDF install is needed.
- (Optional, CLI only) `pip install platformio` if you'd rather drive
  builds with the `pio` command from a terminal instead of the VS Code UI.
- **On Windows**, do the whole workflow from **WSL** (this is how the
  project is actually developed): install PlatformIO inside your WSL
  distro so `~/.platformio` and `pio` live on the Linux PATH, and reach
  the board through `/dev/ttyACM0` rather than a native Windows COM port
  (see port notes in step 4).

## 2. Open the project

```sh
git clone <this repo>
cd electron-3d-display
code .
```

PlatformIO reads `platformio.ini` automatically and, on first load, offers
to install the `espressif32@7.0.1` platform (ESP-IDF 6.0.1 toolchain) plus
this project's `WS-ESP32-S3-LCD-1.3` board definition
(`boards/WS-ESP32-S3-LCD-1-3.json`) — accept it. The first build downloads
roughly 1-2 GB of toolchain and takes several minutes.

## 3. Build

```sh
pio run
```

(or, in VS Code: PlatformIO sidebar → **Project Tasks** →
`WS_ESP32_S3_LCD_1_3` → **Build**, or the checkmark icon in the status bar.)

Relevant `platformio.ini` settings:

- `framework = espidf`, `platform = espressif32@7.0.1` → ESP-IDF 6.0.1
- `build_flags = -std=gnu++26 -I include -I src -Wall -Wextra` — this is a
  C++26 codebase
- `board_build.partitions = partitions_16M.csv` — matches the board's
  16 MB flash; `sdkconfig.defaults` pins the matching flash size plus
  PSRAM/CPU/FreeRTOS-tick tuning (the espidf framework ignores the board
  manifest's Arduino-only `partitions` key, so this is required, not
  redundant)
- Compiles are routed through `ccache` via
  `tools/extra_script_ccache.py` — see that file's header comment for why
  PlatformIO's espidf builder needs this workaround instead of the usual
  `CMAKE_*_COMPILER_LAUNCHER`

## 4. Flash

Connect the board over USB-C, then:

```sh
pio run -t upload
```

(VS Code: PlatformIO sidebar → **Upload**, the → icon.)

Port notes:

- **Linux/WSL**: usually `/dev/ttyACM0` (native USB-CDC on the S3 — check
  with `ls /dev/tty*` before/after plugging in). Under WSL the device
  needs to be attached to the Linux VM first (`usbipd attach ...`) before
  it shows up.
- **Windows (native, non-WSL)**: a COM port; PlatformIO usually
  auto-detects it, or set `upload_port` in `platformio.ini` to force one.
- If auto-reset into the bootloader doesn't trigger, hold **BOOT**, tap
  **RESET**, release **BOOT**, then retry the upload.

## 5. Upload the data partition

`data/` (`hfs_tables.bin`, `orbital_samplers.bin`, `atomic_cube.jpg`) is a
separate SPIFFS partition (`storage`, see `partitions_16M.csv`), not part
of the firmware image — it needs its own upload step:

```sh
pio run -t uploadfs
```

This does **not** run automatically on a plain `upload` right now
(`tools/extra_script_uploadfs.py` exists to chain it on, but its
`pre:` line is commented out in `platformio.ini`) — run it manually
whenever `data/` changes (e.g. after regenerating tables with
`tools/hfs_table_gen.py` / `tools/orbital_table_gen.py`), and at least
once on a freshly-flashed board. Uncomment that `extra_scripts` line if
you'd rather it run on every flash automatically — see the script's
header comment for the tradeoff (it reformats the whole partition,
wiping any on-device screenshots captured since the last `uploadfs`).

## 6. Monitor / debug

```sh
pio device monitor
```

`monitor_filters = esp32_exception_decoder` in `platformio.ini`
symbolizes crash backtraces automatically.

## Generating data (fonts, radial tables, sampler tables)

Three binary/generated artifacts are checked into the repo, so a plain
build/flash (steps 3-5) doesn't require regenerating anything — but if
you change one of the sources below, regenerate before reflashing:

- **Fonts** — `src/render/font_data.h` (the glyph bitmap tables `font.cpp`
  renders from) is generated from a `.ttf` by `tools/font_gen/`:

  ```sh
  cd tools/font_gen
  pip install pillow
  python3 generate_font.py > ../../src/render/font_data.h
  ```

  Regenerate after changing the source typeface or the point
  sizes/spacing in `generate_font.py`'s `SIZES` list. See
  `tools/font_gen/README.md` for the full rationale (why offline
  rasterization, why PIL mode `"1"`, how to swap fonts/sizes).

- **HFS radial tables** (per-element screened-potential data) —
  `data/hfs_tables.bin` + `micropython/hfs_tables.bin` from
  `pc/hfs_tables_reduced.npz`:

  ```sh
  python3 tools/hfs_table_gen.py
  ```

- **Orbital sampler tables** (the 36 baked-in hydrogen-orbital presets) —
  `data/orbital_samplers.bin`:

  ```sh
  python3 tools/orbital_table_gen.py
  ```

`data/*.bin` outputs only take effect on the device after
`pio run -t uploadfs` (step 5) and a reboot; `micropython/hfs_tables.bin`
only takes effect after re-copying `micropython/` to the board (see
`micropython/README.md`). `font_data.h` is compiled straight into the
firmware image, so a normal `pio run -t upload` picks it up.

## Build variants

`src/main.cpp`'s top-of-file `#define` toggles switch between the
default chooser app and standalone test builds — `ATOM_VALIDATION_TEST`,
`ATOM_VIEW_TEST`, `ATOM_VIEW`, `COLOR_TEST`, `BENCHMARK_TEST`,
`SLICE_TEST`. Exactly one may be active at a time; with none defined,
`app_main()` boots the real chooser menu. Edit the file, uncomment one,
rebuild, reflash.

## On-device screenshots

The firmware runs a small text console over the same serial link
`pio device monitor` already uses for logs (`src/debug/screenshot_console.h`),
so you can capture and pull PNGs from the running device with no extra
hardware.

**Capture**, from a `pio device monitor` session (or any serial terminal):

| Type | Effect |
|---|---|
| `s` | Capture the current frame → `SS_CAPTURED <name> <size>` |
| `l` | List saved screenshots → one `SS_FILE <name> <size>` line each, then `SS_LIST_END` |
| `a` | Batch-capture every orbital preset + a curated set of elements + an Fe shell-dissection sequence (mirrors `pc/screenshot.py`'s output) — blocks for tens of seconds, progress via the normal log lines, then `SS_CAP_ALL_DONE` |

Captures are written as PNGs to the `storage` SPIFFS partition (the same
one `data/` is uploaded to — see step 5), so they survive a plain
`pio run -t upload` but are wiped by the next `pio run -t uploadfs`.

**Download**, from your PC (close `pio device monitor` first — only one
process can hold the serial port at a time):

```sh
pip install pyserial
python3 pc/pull_screenshots.py --list                       # what's on the device
python3 pc/pull_screenshots.py --all                         # pull everything into screenshots/
python3 pc/pull_screenshots.py shot_0001.png shot_0003.png   # pull specific files
python3 pc/pull_screenshots.py --all --delete                # ...then delete them on-device
```

Files land in `screenshots/` at the repo root. Port defaults to
`/dev/ttyACM0`; override with `--port`. This is a separate, on-device
capture path from `pc/screenshot.py`, which renders the equivalent stills
straight from the PC simulator (no board needed) for this README's images.

## Alternative: raw `idf.py` (untested path)

A plain ESP-IDF `CMakeLists.txt` sits at the repo root, so in principle
the project can also be built with `idf.py build/flash/monitor` against a
manually-installed ESP-IDF 6.x toolchain instead of PlatformIO's managed
one. This isn't the path actually used day to day — PlatformIO (above) is.
