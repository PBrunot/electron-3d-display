# CYD (Cheap Yellow Display, ESP32-2432S028R) — note del branch `CYD-test`

Documento dedicato al lavoro sul branch `CYD-test` (non su `master`). Per
l'hardware/architettura principale del progetto (Waveshare ESP32-S3-LCD-1.3),
vedi `CLAUDE.md`.

Secondo `platformio.ini` environment, `[env:CYD]`, per la scheda "Cheap
Yellow Display" (ESP32-2432S028R — vedi
https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display), pensata per
eseguire lo **stesso codice sorgente** in `src/` della S3 (non un fork/porting
separato): niente pyramid/prism su questa scheda (è un pannello LCD piatto
normale, non un setup Pepper's Ghost), ma la stessa pipeline point-cloud/
orbitali gira su un secondo hardware più economico e diffuso.

Differenze hardware rilevanti rispetto alla Waveshare S3 (CLAUDE.md §2): SoC
ESP32 classico (Xtensa LX6, non S3), display **ILI9341** 240×320 (non ST7789
240×240), **nessun IMU** (niente QMI8658), touch resistivo XPT2046, 4 MB
flash (non 16 MB), niente PSRAM (confermato via boot log su hardware reale:
`external RAM: 0/0 bytes free/total, 0 largest block` — non un'ipotesi da
scheda generica, misurato su questa unità specifica).

**Nota (2026-08-22): questo file è stato riscritto da zero.** Il branch è
stato ri-portato sopra `origin/master` dopo che quest'ultimo si era
riorganizzato in profondità (`src/` diviso in `physics/render/ux/views/debug/
util`, molte view/feature nuove) mentre `CYD-test` era rimasto indietro sulla
vecchia struttura piatta. La versione precedente di questo documento
descriveva anche una fase intermedia ormai superata (risoluzione logica
ridotta a 192×192 in letterbox); quel dettaglio è storico, non riflette più
il codice attuale — vedi §"Framebuffer" sotto per l'architettura finale
effettivamente in uso.

## Cosa è già fatto (branch a compile-time su `CONFIG_IDF_TARGET_ESP32`)

### Display (`src/render/display.h`/`.cpp`)

- Driver pannello: `esp_lcd_new_panel_ili9341` invece di
  `esp_lcd_new_panel_st7789`, dietro `#if CONFIG_IDF_TARGET_ESP32`. Pin,
  clock SPI (40 MHz — il repo CYD ufficiale verifica 55 MHz funzionante, qui
  si parte più conservativi in attesa di verifica su hardware reale; vedi
  `LCD_PIXEL_CLOCK_HZ` in display.cpp) dietro lo stesso `#if`.
- Dipendenza gestita via IDF Component Manager (`src/idf_component.yml`,
  pacchetto `espressif/esp_lcd_ili9341@^2.0.0` — la 1.x usa la vecchia API
  `rgb_endian`, incompatibile con questo IDF 6.0.1). La regola
  `rules: if target == esp32` esclude il fetch di rete per la build S3.
- **Risoluzione logica: piena risoluzione fisica del pannello, 240×320**
  (non più un letterbox ridotto). `Display::kDisplayWidth/kDisplayHeight` =
  240/320 su CYD, 240/240 su S3 — invariato altrove: tutto il resto di `src/`
  (camera.h, atom_view.cpp, orbital_view.cpp, ...) usa questi simboli, mai
  `240`/`320` hardcoded.
- **Framebuffer a blocchi multipli, non un singolo `heap_caps_malloc`**: la
  SRAM interna della ESP32 classica è frammentata in più regioni non
  contigue all'avvio, quindi un singolo buffer 240×320 RGB565 (150 KiB) non
  garantisce un'allocazione contigua. `Display::Display()` interroga il blocco
  DMA libero più grande, dimensiona i blocchi di conseguenza, e ne alloca
  quanti servono per coprire l'intera altezza logica — con backoff (dimezza
  la dimensione del blocco e riprova) se la frammentazione reale è peggiore
  di quanto stimato. Sulla S3 questo quasi sempre risolve a un blocco solo
  (comportamento equivalente al vecchio buffer singolo).
- **Nessun puntatore diretto al framebuffer esposto**: l'API pubblica è
  `writePx`/`readPx`/`clearScreen`/`fade`/`blit`/`readAllPixels`, non più
  `getFrameBuf()`. Necessario perché con lo storage a blocchi non esiste un
  array contiguo unico da restituire; tutti i ~20 file che disegnavano
  direttamente su un `uint16_t *frameBuf` (camera.h, font.cpp, overlay.cpp,
  chooser.cpp, splash_bitmap.cpp, tilt_gesture.cpp, atom_view.cpp,
  orbital_view.cpp, tutti i moduli `debug/*_test.cpp`, screenshot_console/
  screenshot_batch) sono stati convertiti a questa API — comune a entrambi i
  target, non solo alla CYD.
- **`physicalRow()`**: il flip verticale software (necessario sulla S3 perché
  combinare i due mirror hardware produceva un'immagine rotta su
  quell'unità) è ora piegato dentro l'indirizzamento pixel invece che un
  passaggio di riordino a runtime — su CYD `physicalRow(y) = y` (nessun flip
  verificato necessario, da confermare su hardware).
- **`storageColor()`**: byte-swap per pixel su CYD (identità su S3).
  Necessario perché `esp_lcd_ili9341` ignora `data_endian` (verificato
  leggendo il sorgente del componente — a differenza del driver ST7789
  integrato nella S3, che usa `data_endian` per un bit reale nel registro
  RAMCTRL del pannello): l'ILI9341 si aspetta sempre big-endian via SPI,
  mentre Xtensa tiene i `uint16_t` little-endian in RAM.
- **Colore: `rgb_ele_order = BGR`** su CYD (non RGB) — confermato da
  [espressif/esp-idf#10242](https://github.com/espressif/esp-idf/issues/10242),
  stesso problema sulla stessa famiglia di componente.
- `presentFrame()` fire-and-forget con semaforo counting (`xSemaphoreCreateCounting`):
  accoda il DMA di ogni blocco senza attendere tra un blocco e l'altro;
  `waitForFlushDone()` drena un completamento per blocco. Comune a entrambi i
  target (non solo un fix CYD): elimina la copia flip-in-place che la S3
  faceva prima ad ogni frame.
- `syncForExternalRead()` e `readAllPixels()` (nuova) restano disponibili per
  gli screenshot: `readAllPixels()` copia l'intero framebuffer logico in un
  buffer piatto row-major fornito dal chiamante, unico modo per ottenere una
  vista "flat" ora che lo storage è a blocchi.

### IMU (`src/ux/imu.h`/`.cpp`)

- `Qmi8658` è un no-op su CYD (`#if CONFIG_IDF_TARGET_ESP32`): niente
  bring-up I2C (i pin S3 47/48 non sono nemmeno GPIO validi su ESP32
  classica — max GPIO 39), `readAccelG()` ritorna sempre `false`.
- `src/main.cpp` salta esplicitamente il ramo `checkPlanarAtBoot()`/
  `calibrateDirections()` su questo target invece di fare affidamento sui
  soli fallimenti di lettura: `calibrateDirections()` (ux/chooser.cpp) ha un
  loop **senza timeout** in attesa di un gesto di tilt confermato — su un
  device senza IMU sarebbe un hang di boot permanente.
- **Auto-avvio orbitali dopo 5s** (`main.cpp`, `#if CONFIG_IDF_TARGET_ESP32`):
  convenienza **temporanea** per testare senza input funzionante — salta
  `runChooser()` (inerte, nessun gesto di tilt può mai essere confermato) e
  chiama `runOrbitalView(display, tilt)` direttamente dopo un `vTaskDelay`
  di 5s. Da rimuovere quando un input reale (touch/pulsante BOOT) sostituirà
  la navigazione a tilt.

### Punti nuvola (`src/config/visual_constants.h`)

- `kOrbitalNumPoints`/`kAtomNumPoints` branchati per target: 12000/12000
  sulla S3 (invariato), **2000/1000 sulla CYD**. Questi array sono
  `EXT_RAM_BSS_ATTR` (pensati per vivere in PSRAM): sulla S3 finiscono in
  PSRAM gratis, sulla CYD (niente PSRAM) ricadono in SRAM interna — e dato
  che ESP-IDF/PlatformIO linkano il componente `main` whole-archive, anche
  il codice mai chiamato a runtime (es. `benchmark_test.cpp` quando
  `BENCHMARK_TEST` non è definita) pesa comunque sul budget statico.

### Vista plane-slice heatmap: **esclusa dalla build CYD**

- `src/views/orbital_slice.cpp` dichiara due array di scratch a livello di
  file (`sliceMag`/`sliceOrder`, dimensionati `kSliceGridSize² = kDisplayWidth²
  = 240×240 = 57600` elementi da 4 byte ciascuno) usati una volta per ogni
  build della tabella — **~450 KiB da soli**, indipendentemente da qualunque
  tuning dei contatori punti sopra. Su CYD (niente PSRAM) questo overflowava
  da solo il budget di link della SRAM interna (misurato: overflow di
  ~493 KiB al primo tentativo di build CYD).
- **Decisione presa**: escludere l'intera feature dalla build CYD invece di
  riscrivere questi array come scratch heap-allocata temporanea. Motivazione:
  il gesto che raggiunge questa sequenza (Right tilt-hold, sia manuale che
  auto-idle) richiede comunque l'IMU, che su CYD non esiste — il costo reale
  di *non* riscriverla è zero, dato che la feature sarebbe comunque
  irraggiungibile a runtime su questo target.
- Meccanismo: `platformio.ini`'s `[env:CYD]` ha un `build_src_filter` che
  esclude `views/orbital_slice.cpp` e `debug/orbital_slice_test.cpp` dalla
  compilazione. I call site in `src/views/orbital_view.cpp` (branch Right
  tilt-hold, branch idle-slice, `#include "views/orbital_slice.h"`) e il
  toggle `SLICE_TEST` in `src/main.cpp` sono dietro
  `#if !CONFIG_IDF_TARGET_ESP32` corrispondenti, così il codice compila
  pulito su entrambi i target senza simboli mancanti.
- `kSliceGridSize`/`kSliceCellPx` (`config/visual_constants.h`) restano
  invariate e non gated: sono `constexpr int` puri, senza storage, innocue
  di per sé — il problema era solo negli array che le usavano come
  dimensione.

### Partizioni e storage dati

- `partitions_cyd.csv` + `sdkconfig.defaults.CYD`: tabella partizioni e
  sdkconfig dedicati per i 4 MB flash della CYD (`sdkconfig.defaults`
  esistente resta specifico per i 16 MB + PSRAM della S3, i due file NON si
  sommano).
- **Partizione `storage` (SPIFFS, 1 MB)** aggiunta a `partitions_cyd.csv`:
  `src/physics/hfs_radial.cpp`/`src/physics/orbital_library.cpp` caricano
  `hfs_tables.bin`/`orbital_samplers.bin` da questa partizione a runtime (lo
  stesso meccanismo della S3, `partitions_16M.csv`'s `storage`, 7 MB). Senza
  questa partizione il mount fallisce silenziosamente e le viste degradano a
  un modello approssimato (atomi: fallback idrogenoide; orbitali: singolo
  punto nell'origine) — non un crash, ma un default visibile rotto proprio
  sulla vista in cui la CYD atterra automaticamente dopo il boot (vedi sopra,
  auto-avvio orbitali). 1 MB copre comodamente il payload attuale di `data/`
  (~900 KiB). Deploy con `pio run -e CYD -t uploadfs` (manuale, stesso
  workflow della S3 — non incatenato a ogni `upload`, vedi il commento in
  `platformio.ini`).

**Build CYD verificata con `pio run -e CYD`: compila E linka** — RAM 51.0%
(167176/327680 byte), Flash 18.4% (567987/3080192 byte). Build S3
riverificata in parallelo: nessuna regressione (RAM 12.3%/40364 byte, Flash
15.3%/640463 byte — identica a prima del re-port).

## Cosa NON è ancora fatto (bloccanti reali, non solo "todo")

- **Nessuno dei fix pin/colore/mirror sopra è verificato su hardware reale**
  — il fix BGR + byte-swap è basato su un caso documentato (issue esp-idf)
  sulla stessa libreria, non su un test diretto con
  `examples/corner_calibration` su QUESTA unità. Se i colori sono ancora
  sbagliati dopo questo fix, il prossimo sospetto è `invert_color` (oggi
  `false`) — provare `true`.
- **Landscape (320×240, come si tiene normalmente in mano la CYD) non
  cablato** — resta ritratto (240×320 nativo). Richiederebbe
  `esp_lcd_panel_swap_xy()`, non testato.
- **Navigazione a tilt non sostituita** (deciso: rimandato) — su CYD il menu
  chooser non risponde a nulla; per ora si aggira con l'auto-avvio dopo 5s
  sopra. Serve una vera decisione di design su come navigare (touch
  resistivo, pin già documentati sotto; pulsante BOOT IO0; o nessuna
  navigazione/vista fissa). **Da decidere con l'utente, non assumere.**
- **Plane-slice heatmap non disponibile su questo target** (vedi sopra) —
  se in futuro servisse anche su CYD, la strada è riscrivere
  `buildSliceTable()`'s scratch (`sliceMag`/`sliceOrder`) come allocazione
  heap temporanea invece di array statici, e verificare che ~460 KiB di
  picco heap siano davvero disponibili a runtime sulla CYD (non ovvio: è
  quasi metà della RAM interna totale del chip).
- **`data/hfs_tables.bin`/`orbital_samplers.bin` non ancora deployati su
  hardware CYD reale** — la partizione `storage` esiste nella tabella, ma va
  effettivamente flashata con `pio run -e CYD -t uploadfs` prima che gli
  atomi/orbitali smettano di usare il modello degradato.

## Pin verificati (fonte: PINS.md del repo CYD ufficiale)

```
Display (ILI9341, HSPI):
  TFT_MISO  12   TFT_DC    2
  TFT_MOSI  13   TFT_RST   -1 (= RESET scheda)
  TFT_SCLK  14   TFT_BL    21
  TFT_CS    15

Touch resistivo (XPT2046):
  CLK 25   MOSI 32   CS 33   IRQ 36   MISO 39

SD card (VSPI): CS 5, SCK 18, MISO 19, MOSI 23
RGB LED (attivo basso): R 4, G 16, B 17
LDR: IO34   Pulsante BOOT: IO0
```

## Tabelle fisiche senza PSRAM

Non serve un equivalente di `PROGMEM`: su ESP32 (anche senza PSRAM) i dati
`static const` finiscono in `.rodata`, mappato in esecuzione diretta da
flash (XIP) dalla cache della CPU — stesso meccanismo con cui gira il codice
stesso. Le tabelle vere e proprie (`hfs_tables.bin`/`orbital_samplers.bin`)
non sono comunque compilate nel binario su nessuno dei due target ormai
(vedi "Partizioni e storage dati" sopra): vengono caricate da flash on
demand via la partizione `storage`, quindi il vincolo reale è che l'intero
binario (codice, non le tabelle) deve entrare nella partizione `factory` di
`partitions_cyd.csv` (3 MB su 4 MB di flash totali).
