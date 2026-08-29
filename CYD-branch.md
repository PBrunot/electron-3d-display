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

**Aggiornamento (2026-08-22, stesso giorno): risoluzione piena 240×320
verificata su hardware reale**, non solo a compile-time. Al primo re-port il
buffer 240×320 falliva davvero all'allocazione (misurato: `abort()` a boot,
vedi log sotto) — la nota sopra descriveva l'architettura a blocchi come
"finale" ma la board non aveva ancora abbastanza SRAM interna libera perché
quell'architettura raggiungesse la risoluzione piena. La causa e il fix sono
in §"Budget RAM interna" sotto: liberati ~76 KB statici (soprattutto
escludendo del tutto la console screenshot su questo target, un percorso che
era comunque già un no-op qui), poi verificato via log seriale reale che il
buffer 240×320 si alloca con margine e che la vista orbitale carica un
preset senza crash. Questo è il primo punto in cui qualcosa in questo
documento è stato confermato su hardware fisico invece che solo dedotto dal
build log — vedi anche §"Cosa NON è ancora fatto" per cosa resta comunque
non verificato (colori/mirror).

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
  sulla S3 (invariato), **3400/1000 sulla CYD** (era 2000/1000 al primo
  re-port; vedi §"Budget RAM interna" sotto per come si è arrivati a 3400 e
  perché `kAtomNumPoints` non è stato toccato). Questi array sono
  `EXT_RAM_BSS_ATTR` (pensati per vivere in PSRAM): sulla S3 finiscono in
  PSRAM gratis, sulla CYD (niente PSRAM) ricadono in SRAM interna — e dato
  che ESP-IDF/PlatformIO linkano il componente `main` whole-archive, anche
  il codice mai chiamato a runtime (es. `benchmark_test.cpp` quando
  `BENCHMARK_TEST` non è definita) pesa comunque sul budget statico, a meno
  che non resti anche staticamente irraggiungibile (nessun call site vivo
  da nessuna parte) e venga quindi scartato da `--gc-sections`.

### Budget RAM interna: commonalizzazione, esclusioni CYD-specifiche, verifica su hardware (2026-08-22)

Con la risoluzione piena 240×320 il framebuffer da solo richiede 153600
byte contigui-a-blocchi di SRAM DMA-capace. Al primo re-port (`kOrbitalNumPoints
= 2000`, console screenshot attiva) questo falliva su hardware reale:

```
I (483) display: frame buffer: 240x320 logical (153600 bytes needed), largest
free DMA block=110592 bytes, total free DMA=146900 bytes -> starting at 213
rows/block
[...retry a granularità decrescente...]
E (538) display: failed to allocate frame buffer (even at 1 row/block)
abort() was called at PC 0x400d3549 on core 0
```

Due fix hanno liberato abbastanza SRAM interna perché lo stesso identico
allocatore a blocchi (nessuna modifica alla logica di `Display::Display()`,
solo più byte liberi da cui attingere) riuscisse:

1. **Console screenshot esclusa del tutto sulla build CYD** — non solo il
   comando batch, l'intera `startScreenshotConsole(display)` (`main.cpp`,
   dietro `#if !CONFIG_IDF_TARGET_ESP32`). Motivazione: `debug/
   screenshot_batch.cpp`'s `captureOrbitals()`/`captureAllPresets()`
   dichiaravano ciascuna una propria copia statica `EXT_RAM_BSS_ATTR` di
   `OrbitalPresetState`/`AtomPresetState` (tens of KB ciascuna, duplicati
   di quella già usata dalla vista live) **puramente per supportare il
   comando `'a'`/`SS_CAP_ALL`** — un comando che quella stessa funzione
   documenta essere già un no-op su CYD (`heap_caps_malloc(...,
   MALLOC_CAP_SPIRAM)` fallisce sempre senza PSRAM, la funzione logga e
   ritorna). Il commento originale della funzione affermava "no static
   reservation left behind" su una board senza PSRAM: **falso** — un
   array `static EXT_RAM_BSS_ATTR` dentro una funzione mai raggiunta a
   runtime pesa comunque sul link se la funzione resta raggiungibile
   *staticamente* (ESP-IDF/PlatformIO linkano `main` whole-archive), e con
   `esp_lcd`/questo componente niente PSRAM significa fallback in SRAM
   interna, non "nessuna riserva". Verificato via `nm`/`readelf` sul
   `firmware.elf` collegato: i simboli `captureOrbitals()::preset`
   (36232 byte) e `captureAllPresets()::atomPreset` (12920 byte) erano
   presenti in `.dram0.bss`, non ottimizzati via. Escludere l'intera
   console (stesso meccanismo di esclusione di `orbital_slice.cpp`: nessun
   call site vivo → `--gc-sections` scarta l'intera unità di traduzione,
   incluso `screenshot.cpp`/`screenshot_batch.cpp`/`png_writer.cpp`) ha
   liberato **76952 byte** di RAM statica (167176→90224 byte, misurato,
   più della somma dei soli due simboli sopra: la console portava con sé
   anche i propri buffer di riga/protocollo).
2. **`order[]`/`radii[]` (physics/orbital_presets.cpp) unificati in un solo
   scratch condiviso** (`OrderRadiiScratch`, union `int[kOrbitalNumPoints]`/
   `orb_real_t[kOrbitalNumPoints]`): `computeOrbitalLevels()` e
   `scaleFromRadii()` li usavano ciascuna come proprio array statico
   privato a funzione, ma sono sempre chiamate in sequenza dentro un unico
   `OrbitalPresetState::load()` (mai concorrenti — `screenshot_pause.h`
   serializza comunque ogni `load()` contro qualunque altro lettore/
   scrittore di questo scratch), e ciascun uso è auto-contenuto (scrive,
   legge, scarta) entro la propria chiamata. Un solo buffer, reinterpretato
   come serve, sostituisce due array separati da `kOrbitalNumPoints`
   elementi — 4 byte/punto risparmiati (a 3400 punti: 13600 byte). Questa è
   l'unica vera "commonalizzazione tra moduli" trovata che valesse lo
   sforzo: gli altri array a grandezza `kOrbitalNumPoints`/`kAtomNumPoints`
   sparsi nel codice erano già singleton condivisi (static locali a
   funzione, un'unica istanza indipendentemente da quante volte/da dove la
   funzione viene chiamata) — non c'era altra duplicazione reale da
   rimuovere.

Con entrambi i fix, a `kOrbitalNumPoints = 3400`: **RAM 41.2% (135024/327680
byte)**, Flash invariata (12.7%, 392339/3080192 byte). Verificato via log
seriale reale (`/dev/ttyUSB0`, non solo build):

```
I (483) display: frame buffer: 240x320 logical (153600 bytes needed), largest
free DMA block=110592 bytes, total free DMA=166100 bytes -> starting at 213
rows/block
[...retry, come sempre su questa unità: la stima iniziale basata sul blocco
singolo più grande è ottimistica, il backoff a grana più fine è il
comportamento atteso, non un problema...]
I (509) display: frame buffer: allocated 13 block(s), 26 rows each (12480
bytes) + last block 8 rows (3840 bytes), 153600 bytes total
[...]
I (7593) orbital_view: display ready, 36 presets available
I (7593) orbital_view: loading preset 4 (2pz, n=2 l=1 m=0)...
I (7667) orbital: orbital sampler table ready: 36 presets, 1001 pts/table
I (7765) orbital_view: 2pz loaded in 172ms, scale=12.5
```

166100 byte liberi contro 153600 richiesti: **12500 byte di margine**
verificato, non solo stimato — il numero `total free DMA` (nuova riga di
log in `Display::Display()`, `heap_caps_get_free_size(MALLOC_CAP_DMA)`,
lasciata nel codice apposta) è lo strumento più veloce per ri-controllare
questo margine dopo qualunque futuro cambio a `kOrbitalNumPoints`/
`kAtomNumPoints`/`Display::kDisplayWidth/Height`. `kAtomNumPoints` non è
stato toccato/non pesa oggi su questo budget: `AtomPresetState`/
`atom_cloud.cpp`'s scratch sono raggiungibili solo dalla catena chooser→
`runAtomView()`, e su CYD `main.cpp` salta `runChooser()` del tutto (vedi
sotto) — quel codice è staticamente irraggiungibile e `--gc-sections` lo
scarta. Se in futuro la CYD guadagna un input reale e la vista atomo
diventa raggiungibile, questo budget va ricalcolato (il costo non è più
zero).

Perché non si è arrivati esattamente a 5000 punti (obiettivo iniziale):
al primo tentativo (`kOrbitalNumPoints = 4000`, stessi due fix) il
framebuffer falliva ancora — `total free DMA=146900` contro 153600
richiesti, insufficiente nonostante ~76 KB liberati dai due fix sopra.
Causa: la memoria libera DMA-capace è un **sottoinsieme** della RAM libera
totale (`internal_free` nel log `benchmark: BENCH,MEM` include anche SRAM
non DMA-capace, es. la regione IRAM-only da 76 KiB elencata da
`heap_init`) — il modello lineare "byte statico liberato = byte libero per
il framebuffer" è vero in prima approssimazione ma va ri-verificato via
`total free DMA` reale, non assunto dal solo delta di RAM statica. 3400 è
il punto scelto dopo aver ri-misurato su hardware reale con questo numero,
non da un calcolo puramente teorico.

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

- `partitions_cyd.csv` + `sdkconfig.defaults.esp32`: tabella partizioni e
  sdkconfig dedicati per i 4 MB flash / niente PSRAM della CYD.
  `sdkconfig.defaults` (root) resta condiviso da entrambe le board;
  `sdkconfig.defaults.esp32` lo sovrascrive per il target `esp32` (CYD),
  `sdkconfig.defaults.esp32s3` (16 MB flash + PSRAM) per il target `esp32s3`
  (S3) — questo file era prima chiamato `sdkconfig.defaults.CYD`, un nome
  MAI riconosciuto da ESP-IDF (che cerca solo
  `sdkconfig.defaults.$IDF_TARGET`, es. `sdkconfig.defaults.esp32`), quindi
  non veniva mai letto: la build CYD girava per intero sulle impostazioni
  della root pensate per la S3 (16 MB flash — causa reale del warning
  PlatformIO "Expected 4MB, found 16MB!" — vedi sotto), e una build S3 da
  clone pulito (senza un `sdkconfig.WS_ESP32_S3_LCD_1_3` locale già
  persistito da una vecchia sessione di menuconfig) compilava con PSRAM
  disabilitata e sforava il DRAM interno di oltre 1 MB in fase di link.
  Corretto rinominando il file al target IDF corretto e spostando le
  impostazioni PSRAM della S3 in `sdkconfig.defaults.esp32s3` — vedi il
  commento in testa a `sdkconfig.defaults` per i dettagli completi.
- **Partizione `storage` (SPIFFS, 1.5 MB)** aggiunta a `partitions_cyd.csv`
  (`factory` 0x10000–0x270000, `storage` 0x280000–0x400000):
  `src/physics/hfs_radial.cpp`/`src/physics/orbital_library.cpp` caricano
  `hfs_tables.bin`/`orbital_samplers.bin` da questa partizione a runtime (lo
  stesso meccanismo della S3, `partitions_16M.csv`'s `storage`, 7 MB). Senza
  questa partizione il mount fallisce silenziosamente e le viste degradano a
  un modello approssimato (atomi: fallback idrogenoide; orbitali: singolo
  punto nell'origine) — non un crash, ma un default visibile rotto proprio
  sulla vista in cui la CYD atterra automaticamente dopo il boot (vedi sopra,
  auto-avvio orbitali). 1.5 MB copre comodamente il payload attuale di `data/`
  (~900 KiB): `mkspiffs` non è mai pieno-al-100% utilizzabile, l'overhead di
  pagina/blocco (page=256, block=4096) mangia circa il 15% della dimensione
  nominale, quindi la partizione va dimensionata oltre il payload grezzo, non
  a pari misura (una versione precedente a 1 MB/1.125 MB falliva la build con
  `SPIFFS_write error(-10001): File system is full` pur avendo spazio
  nominale sufficiente sulla carta). Deploy con `pio run -e CYD -t
  uploadfs_cyd` (non `uploadfs` come sulla S3 — quel target shella fuori a
  `mkspiffs`, il cui binario precompilato per questa piattaforma è armhf-only
  e non gira su un host di build aarch64; `uploadfs_cyd` costruisce la stessa
  `data/` via lo `spiffsgen.py` puro-Python di ESP-IDF e la scrive via
  `esptool`, vedi il commento in `platformio.ini`). Manuale, non incatenato a
  ogni `upload` — **verificato funzionante su hardware reale (2026-08-22)**,
  vedi §"Cosa NON è ancora fatto" sotto.

  Per produrre un **singolo file `.bin`** pronto da flashare in un colpo solo
  (bootloader + tabella partizioni + app + `storage`), ad es. per un tool di
  flashing esterno o per distribuire un'immagine senza PlatformIO installato:
  `python3 tools/build_merged_bin.py CYD` (o `WS_ESP32_S3_LCD_1_3` per la
  S3) — compila l'app e l'immagine SPIFFS via `pio run`/`buildfs`, poi le
  unisce con `esptool merge_bin`. Legge gli offset direttamente dal CSV delle
  partizioni dell'ambiente (stesso approccio di `uploadfs_cyd` sopra) e
  calcola `--flash_size` dall'estensione reale della tabella partizioni
  invece di fidarsi del flash_size generato da PlatformIO in
  `flasher_args.json` (robusto per design anche indipendentemente dal bug di
  sdkconfig di cui sopra, ora comunque corretto). Output in
  `.pio/build/<env>/merged-flash.bin`; flash con
  `esptool.py --chip <esp32|esp32s3> write_flash 0x0 <file>`.

**Build CYD verificata con `pio run -e CYD`: compila, linka, E flasha/boota
su hardware reale** (2026-08-22, vedi §"Budget RAM interna" sopra per il
log seriale) — RAM 41.2% (135024/327680 byte), Flash 12.7% (392339/3080192
byte), a piena risoluzione 240×320 e `kOrbitalNumPoints = 3400`. Build S3
riverificata in parallelo: nessuna regressione (RAM 12.3%/40364 byte, Flash
15.3%/640479 byte — identica a prima di questo giro di modifiche; le uniche
righe condivise toccate, l'unione `order`/`radii` in
`orbital_presets.cpp`, sono compilate su entrambi i target ma non
cambiano il comportamento sulla S3).

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
- ~~`data/hfs_tables.bin`/`orbital_samplers.bin` non ancora deployati su
  hardware CYD reale~~ **Risolto/verificato (2026-08-22)**: la partizione
  `storage` è flashata (con `pio run -e CYD -t uploadfs_cyd` — non
  `uploadfs`, vedi il commento in `platformio.ini` sul perché) e le tabelle
  si caricano correttamente a runtime, confermato dal log seriale reale
  (`orbital: orbital sampler table ready: 36 presets, 1001 pts/table`,
  nessun fallback al modello degradato) — vedi §"Budget RAM interna" sopra.

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
