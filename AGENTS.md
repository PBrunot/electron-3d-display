# AGENTS.md — Ologramma a piramide su ESP32-S3 (orbitali atomici)

Istruzioni di progetto per Claude Code. Il progetto è oltre le milestone
iniziali: questo file descrive lo stato attuale, non solo il piano.

## 1. Obiettivo e stato

Ologramma tipo Pepper's Ghost su display 240×240 + cubo prisma. Implementato
e funzionante su hardware reale:

- **Orbital viewer**: nuvole di punti |ψ|² per orbitali idrogenoidi (1s..3d),
  colorate per segno di ψ (blu/arancio).
- **Atom viewer**: nuvole multi-elettrone (Clementi-Raimondi, Z=1..54),
  colorate per shell, con dissezione automatica subshell-per-subshell.
- **Menu/chooser**: tilt Up → orbitali, tilt Down → elementi; navigazione
  tabella periodica, auto-advance idle, calibrazione IMU guidata al boot.

Vedi §4 per la mappa dei moduli.

## 2. Hardware

**Scheda**: Waveshare `ESP32-S3-LCD-1.3` (`-B` con case, `-C` con case+prisma).

- SoC: ESP32-S3R8, Xtensa LX7 dual-core @ 240 MHz
- 512 KB SRAM + 8 MB PSRAM (OPI) + 16 MB Flash
- Display 1.3" 240×240 262K colori, driver ST7789V2, bus SPI
- IMU QMI8658 (accel+gyro 3 assi) su I2C

Wiki: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3

## 3. Toolchain

**C++26 su ESP-IDF puro** (non Arduino/TFT_eSPI — quella era la scelta
iniziale, superata; framework Arduino non più usato in `src/`), via
PlatformIO. `platformio.ini` reale in root:

```ini
[env:WS_ESP32_S3_LCD_1_3]
platform = espressif32@7.0.1        ; ESP-IDF 6.0.1
board = WS-ESP32-S3-LCD-1-3         ; boards/WS-ESP32-S3-LCD-1-3.json
framework = espidf
build_flags = -std=gnu++26 -I include -I src -Wall -Wextra
build_src_flags = -fconstexpr-ops-limit=100000000 -Wstack-usage=2048
board_build.partitions = partitions_16M.csv
extra_scripts = pre:tools/extra_script_ccache.py   ; ccache via SCons hook, vedi quel file
```

**Setup dev**: interamente da VSCode dentro WSL — `~/.platformio`, `pio` in
PATH nella home Linux, flash/monitor via `/dev/ttyACM0`. Non buildare/flashare
in autonomia: chiedere all'utente di eseguire `pio run`/`pio device monitor` e
riportare eventuali errori (vedi istruzioni globali).

Riferimenti demo/community che hanno originato pinout e scelte iniziali
(VolosR/esp32Prism, nishad2m8/WS-1.3) restano solo come cronaca — il codice
attuale in `src/` non dipende più da TFT_eSPI/Arduino.

## 4. Architettura software

```
src/
  main.cpp                       app_main(): splash → calibrazione IMU → chooser
                                  (toggle #define in cima per test isolati:
                                  ATOM_VIEW_TEST, COLOR_TEST, BENCHMARK_TEST, ...)

  config/                        costanti trasversali, adjustable-by-eye
    visual_constants.h              colori/dimensioni/pacing animazioni (viewer, chooser, overlay)
    hardware_constants.h            bus I2C IMU, register map, calibrazione tilt di default

  physics/                       modello dati/scienza, nessuna dipendenza da IMU/UI
    orbitals.h/.cpp                 |ψ|² campionamento, coefficienti armoniche
    angular_library.h               armoniche sferiche precalcolate
    pointcloud.h/.cpp               strutture nuvola punti, sampler, XorShift32
    orbital_presets.h/.cpp          libreria preset 1s..3d
    orbital_library.h/.cpp          tabelle sampler orbitali caricate da flash
    hfs_radial.h/.cpp + hfs_tables.h  campionamento radiale Hartree-Fock-Slater
    slater.h + slater_data.h        regole di Slater / raggi Clementi-Raimondi
    atom_size_calib.h               fattori di calibrazione dimensione atomica
    atom_cloud.h/.cpp                nuvola multi-subshell, colorazione per shell

  render/                        pipeline di presentazione, condivisa dai viewer
    display.h/.cpp                  init esp_lcd/panel ST7789, doppio framebuffer
                                     PSRAM (DMA), packColor565()
    camera.h/.cpp                   rotazione/proiezione/fly-over
    overlay.h/.cpp                  scale bar, tilt arrow, titoli overlay comuni
    font.h/.cpp + font_data.h       font bitmap proprietario (vedi §5.1)
    ticker.h/.cpp                   testo scorrevole (banner intro)
    splash_bitmap.h/.cpp            splash screen di boot
    equation_bitmap.h/.cpp          sfondo formula per orbital_view

  ux/                            input/interazione/navigazione
    imu.h/.cpp                      driver QMI8658 (I2C), check planarità al boot
    tilt_gesture.h/.cpp             gesture tilt-and-hold a 4 direzioni su dati IMU
    tilt_defaults.h                 soglie/default gesture
    chooser.h/.cpp                  menu: tilt Up/Down lancia orbital/atom viewer,
                                     idle auto-launch, calibrazione direzioni guidata
    periodic_grid.h                 navigazione tabella periodica (snake order)
    element_names_it.h              nomi elementi in italiano

  views/                         composizione app-level (physics + render + ux)
    orbital_view.h/.cpp              viewer orbitali idrogenoidi
    atom_view.h/.cpp                 viewer atomi multi-elettrone + dissezione

  debug/                         diagnostica e strumenti di cattura screenshot
    screenshot.h/.cpp                cattura frame → PNG on-device
    png_writer.h/.cpp                encoder PNG minimale (LZ77+Huffman fisso)
    screenshot_console.h/.cpp        protocollo seriale SS_LIST/SS_GET/SS_DEL
    screenshot_batch.h/.cpp          batch capture di tutti i preset (per gallery)
    screenshot_pause.h/.cpp          pausa rendering durante cattura
    *_test.h/.cpp                    harness diagnostici dietro i #define di main.cpp

  util/                          utility generiche, nessuna dipendenza di dominio
    crc32.h/.cpp
```

Le directory sono gruppi logici, non componenti ESP-IDF separati (un solo
`idf_component_register` in `src/CMakeLists.txt`, con `GLOB_RECURSE` che
raccoglie ricorsivamente). Gli include locali usano percorsi relativi a `src/`
(es. `#include "physics/orbitals.h"`), risolti via `-I src` in
`platformio.ini` — non nomi file nudi, anche fra file nella stessa cartella.

Strumenti offline (PC, non compilati per il device):
- `tools/font_gen/` — rasterizza Jersey10-Regular.ttf in `src/render/font_data.h`
- `tools/splash_gen/`, `tools/equation_gen/` — asset PROGMEM precalcolati
- `tools/orbitals_host/` — reference/validation Python per i calcoli orbitali
- `pc/` — simulatore PC (Tkinter+PIL) degli stessi viewer, per iterare senza
  flashare; `pc/pull_screenshots.py` scarica screenshot dal device via seriale

### 4.1 Sistema font (`font.h`/`font.cpp`/`font_data.h`)

Font bitmap rasterizzato offline da un vero typeface (Jersey10-Regular),
**dati e logica separati**: `font_data.h` (generato da
`tools/font_gen/generate_font.py`, non toccare a mano) contiene solo le
tabelle glifi; `font.cpp` (scritto a mano, mai rigenerato) contiene
drawText/drawTextScaled/ecc. Tre taglie, ciascuna col tipo riga più stretto
che le serve (niente sprechi di flash su bit che non useranno mai):

- `kFontSmall` (10px, righe `uint8_t`) — sfondo "e-" della intro dissezione
- `kFontLarge` (18px, righe `uint16_t`) — titoli standard, legenda scale bar
- `kFontHuge` (54px, righe `uint64_t`) — titolo grande simbolo elemento in
  atom_view ed etichetta grande di subshell durante la dissezione, entrambi
  disegnati alla loro vera dimensione (non kFontLarge upscalato via
  drawTextScaled, che risultava a blocchi)

Ogni glifo è rasterizzato in modalità PIL `"1"` (rasterizer monocromatico
hinted di FreeType), non `"L"` (grayscale) sogliato a 128 — le due divergono
sul bordo di uscita di molti glifi, il che si vedeva on-device come un bordo
sfocato/aliasato. Il range di righe di ogni glifo è anche ritagliato alla
vera finestra verticale d'inchiostro dell'intero charset invece che al box
ascent+descent del font (questo charset non ha maiuscole accentate, quindi
nessun glifo raggiunge mai il vero ascent) — senza ritaglio, quel padding
faceva sembrare il testo disegnato a una data `(x, y)` più basso/staccato di
quanto `(x, y)` fosse realmente. Vedi il docstring di modulo di
`generate_font.py` per il razionale completo.

Per cambiare font/taglie: editare `SIZES` in `generate_font.py` e rigenerare
con `python3 generate_font.py > ../../src/render/font_data.h` (vedi
`tools/font_gen/README.md`).

## 5. Performance

Misurato su hardware reale dopo tuning (240 MHz CPU, `-O2`, SPI/PSRAM a
80 MHz): **62.5 FPS** (da 35.7 FPS iniziali) sul rendering nuvola di punti a
schermo intero. Target originale di 20–30 FPS ampiamente superato — non
serve ottimizzare oltre a meno di regressioni misurate (vedi
`benchmark_test.cpp`, `BENCHMARK.md`).

## 6. Convenzioni di codice

- C++26 stile ESP-IDF (`app_main()`, task FreeRTOS), non `setup()`/`loop()` di Arduino
- Nessuna allocazione dinamica nei loop di rendering — buffer pre-allocati
  (spesso `EXT_RAM_BSS_ATTR`/PSRAM per strutture grandi, vedi `atom_view.cpp`).
- Punti in coordinate float prima della proiezione; scala a pixel solo
  nell'ultimo passaggio (`camera.h`).
- Commenti/nomi variabili in inglese; messaggi log/UI possono restare in
  italiano (nomi elementi in `element_names_it.h`).
- Dati generati offline (font, splash, asset PROGMEM) vanno rigenerati con i
  loro script in `tools/`, mai editati a mano nel file generato.
- Leggi STYLE.MD per capire meglio.

## 7. Nota operativa — backtick nelle docstring Python (lavorare con run_code)

Quando si scrive/modifica codice Python tramite lo strumento `run_code`, il
contenuto passa per un template literal JavaScript: ogni backtick (`) nel
contenuto termina il literal e rompe la chiamata. Le docstring Python citano
spesso identificatori fra backtick, quindi il problema è ricorrente.

Regole:
1. **Preferito** — costruire il file come array di righe JS in apici singoli,
   `content: lines.join("\n")`: nessun escaping necessario.
2. Se si usa un template literal, escapare ogni backtick come `\`` (`String.raw`
   non aiuta, il backtick termina comunque il literal).
3. Non appiattire i newline per passare script a `python -c` — scrivere in un
   file temporaneo, eseguire, cancellare.
4. Nei test, `sum(1 for b in buf if b)` per contare byte non-zero (non
   `sum(1 for b in buf)`, che conta tutti i byte).

I backtick nelle docstring Python vere sono validi caratteri normali — il
vincolo è solo sul trasporto JS, non "correggere" le docstring togliendoli.

## 8. Secondo target — CYD (Cheap Yellow Display, ESP32-2432S028R)

Esiste un secondo `platformio.ini` environment, `[env:CYD]`, per la scheda
"Cheap Yellow Display" (ESP32-2432S028R), pensato per eseguire lo **stesso
codice sorgente** in `src/` della S3 (branch a compile-time su
`CONFIG_IDF_TARGET_ESP32`, non un fork). Lavoro in corso sul branch git
**`CYD-test`** (non su `master`).

Per pinout verificato, stato dettagliato (cosa funziona/cosa manca, con le
cause misurate su hardware reale) e le decisioni prese finora, vedi
**`CYD-branch.md`** in root — tenuto separato da questo file perché è
lavoro/scoperte specifiche del branch, non architettura stabile del
progetto principale.
