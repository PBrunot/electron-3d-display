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
- ~~Auto-avvio orbitali dopo 5s~~ **Rimosso (2026-08-30)**: sostituito dalla
  navigazione a touch, vedi sezione "Touch (XPT2046)" sotto — `main.cpp`
  chiama `runChooser()` su CYD esattamente come sulla S3.

### Touch (`src/ux/touch.h`/`.cpp`, `src/ux/touch_gesture.h`/`.cpp`) -- 2026-08-30

Sostituisce l'IMU come input di navigazione su CYD (deciso con l'utente:
swipe direzionali che rispecchiano i tilt, non zone di tap né tap+hold; vedi
sopra "Navigazione a tilt non sostituita" -- ora risolto).

- `Xpt2046` (`src/ux/touch.h`/`.cpp`): driver bit-banged (nessun bus SPI
  hardware libero -- HSPI sul display, VSPI sulla SD, l'ESP32 classica non ne
  ha un terzo) sui pin documentati sotto (CLK 25, MOSI 32, CS 33, IRQ 36,
  MISO 39). `isTouched()` legge il pin IRQ (basso quando il pannello è
  premuto); `readRaw()` restituisce campioni ADC 12-bit grezzi (0..4095), non
  calibrati su pixel schermo -- non serve, vedi sotto. No-op sulla S3, stesso
  pattern di `Qmi8658`.
- `TouchGestureDetector` (`src/ux/touch_gesture.h`/`.cpp`) implementa la
  stessa interfaccia `GestureSource::poll() -> TiltEvent` di
  `TiltGestureDetector` (nuova classe base astratta, `src/ux/tilt_gesture.h`)
  -- ogni call site esistente (`chooser.cpp`, `orbital_view.cpp`,
  `atom_view.cpp`) prende `GestureSource&` invece di `TiltGestureDetector&`
  e funziona invariato. Stesso modello di interazione del tilt: tocco,
  trascinamento oltre una soglia (`kTouchSwipeThresholdRaw`, unità ADC
  grezze) in una direzione, poi HOLD di `kTouchHoldConfirmMs` (1000ms, come
  il default del tilt) per confermare -- stessa freccia di progresso
  (`drawTiltArrow[At]()`), stesso `TiltPhase::kHolding/kConfirmed`. Nessuna
  calibrazione delle direzioni come per il tilt: l'orientamento degli assi
  grezzi del pannello è fissato a compile-time da
  `kTouchSwapXY`/`kTouchInvertDx`/`kTouchInvertDy`
  (`config/hardware_constants.h`).
- `main.cpp`: su CYD costruisce `Xpt2046`+`TouchGestureDetector` e chiama
  `runChooser()` esattamente come la S3 -- il loop di boot diretto
  `runOrbitalView()`/`runAtomView()` e `kViewCrossSwitchProbability` (il
  workaround che aggirava l'assenza di un modo per tornare al menu) sono
  stati rimossi, non più necessari ora che lo swipe SINISTRA-e-hold torna al
  chooser su entrambe le board.
- **Verificato su hardware reale (2026-08-30)**: il protocollo bit-banged
  XPT2046 (byte di comando 0xD0/0x90 per X/Y, 16 clock di risposta) funziona
  su questa unità, swipe rilevati correttamente via log `touch_gesture`.
  Calibrazione risultante: asse sinistra/destra invertito rispetto al
  grezzo (`kTouchInvertDx = true`), asse su/giù già corretto
  (`kTouchInvertDy = false`, `kTouchSwapXY = false`). `kTouchHoldConfirmMs`
  ridotto da 1000ms a 500ms (il default mutuato dal tilt risultava lento al
  tocco). Il pin IRQ (GPIO36) e MISO (GPIO39) sono pad solo-input
  dell'ESP32 classico senza pull-up interno: `gpio_config()` non può
  abilitarne uno software (loggava due `gpio_pullup_en(...): GPIO number
  error` benigni a ogni boot, ora eliminati impostando
  `GPIO_PULLUP_DISABLE` in `Xpt2046::Xpt2046()`) -- la board CYD ha
  evidentemente un pull-up esterno sulla linea IRQ, perché la rilevazione
  del tocco funziona comunque idle-high/low-quando-premuto.

### Punti nuvola (`src/config/visual_constants.h`)

- `kOrbitalNumPoints`/`kAtomNumPoints` branchati per target: 12000/12000
  sulla S3 (invariato), **3000/5000 sulla CYD** (storia: 2000/1000 al primo
  re-port, poi 3400/1000 — vedi §"Budget RAM interna" sotto per come si è
  arrivati lì — poi tagliati a 1800/500 quando `runAtomView()` è diventato
  raggiungibile dal loop di boot CYD, vedi §"Aggiornamento 2026-08-30" sotto
  per come si è arrivati a 3000/5000). Questi array sono `EXT_RAM_BSS_ATTR`
  (pensati per vivere in PSRAM): sulla S3 finiscono in PSRAM gratis, sulla
  CYD (niente PSRAM) ricadono in SRAM interna — e dato che ESP-IDF/
  PlatformIO linkano il componente `main` whole-archive, anche il codice
  mai chiamato a runtime (es. `benchmark_test.cpp` quando `BENCHMARK_TEST`
  non è definita) pesa comunque sul budget statico, a meno che non resti
  anche staticamente irraggiungibile (nessun call site vivo da nessuna
  parte) e venga quindi scartato da `--gc-sections`.

#### Aggiornamento (2026-08-30): 1800/500 → 3000/5000, quattro passi di deduplicazione

Quando `runAtomView()` è diventato raggiungibile dal loop di boot CYD (per
permettere il cross-switch idle tra le due viste, `kViewCrossSwitchProbability`),
`kOrbitalNumPoints`/`kAtomNumPoints` erano stati tagliati insieme a 1800/500
per far posto ai suoi ~35KB di scratch fisso sempre residente. Analisi
successiva ha mostrato che un punto atomo costa in realtà MENO di un punto
orbitale (12 byte: solo x/y/z, nessun colore per-punto — la colorazione è
per-subshell, non per-punto come il `colors[]` trainato dal turnover
orbitale, 18 byte/punto) — il tetto basso di `kAtomNumPoints` non era mai un
problema di costo-per-punto, ma di overhead fisso non legato al conteggio
punti. Quattro interventi di deduplicazione (i primi tre a comportamento
visibile invariato, il quarto con un trade-off esplicito — vedi punto 4)
hanno liberato margine reale, ciascuno verificato via boot seriale reale
confrontando la riga `total free DMA` di `Display::Display()` coi 153600
byte richiesti dal framebuffer 240×320:

1. **`hfs_radial.cpp`**: tre statiche `RadialTable` indipendenti da ~4KB
   l'una (`buildHfsRadialSamplerIsotropic()`/`buildHfsRadialSamplerOriented()`/
   `buildRadialSamplerRuntime()`, quest'ultima spostata qui da `pointcloud.h`),
   usate in modo mutuamente esclusivo nello stesso loop sequenziale per-gruppo
   di `atom_cloud.cpp`'s `buildAtomPointCloud()`, unificate in un'unica coppia
   `RadialTable`/`weight` condivisa — **~12.5KB liberati su entrambe le
   board**, incondizionatamente (non erano mai state `EXT_RAM_BSS_ATTR`/
   PSRAM-eligible).
2. **`orbital_presets.cpp`**: `scaleFromRadii()` aveva un proprio scratch
   `radii[kOrbitalNumPoints]` separato invece di riusare l'union
   `OrderRadiiScratch` già condivisa con `computeOrbitalLevels()` (nonostante
   il commento originale sopra affermasse il contrario) — corretto, **~7.2KB
   liberati** a 1800 punti.
3. **`src/physics/view_scratch_arena.h`** (nuovo header): lo scratch di
   caricamento di `orbital_view.cpp` (`psi2`/`signs`/`levels`) e di
   `orbital_presets.cpp` (`order`/`radii`) e lo scratch per-subshell di
   `atom_cloud.cpp` (`sSubshellRadii`) condividono ora un'unica arena statica
   invece di tre riserve separate — sicuro perché `runOrbitalView()` e
   `runAtomView()` non girano mai in contemporanea (stesso loop sequenziale
   di `main.cpp`), quindi il loro scratch "solo di caricamento" non deve mai
   coesistere. Finché il fabbisogno scratch lato atomo (4 byte/punto) resta
   sotto l'impronta di caricamento lato orbitale (10 byte/punto ×
   `kOrbitalNumPoints`), alzare `kAtomNumPoints` non costa nulla in più da
   questo scratch. Implementazione identica su entrambe le board (nessun
   ramo `#if CONFIG_IDF_TARGET_ESP32`): sulla S3 (PSRAM abbondante)
   condividere l'arena non costa nulla di significativo, e un solo percorso
   di codice testato su entrambe le board è più sicuro di un secondo percorso
   S3-only mai verificato su hardware reale.

4. **`src/physics/view_steady_arena.h`** (nuovo header): non solo lo
   scratch, ma anche i dati steady-state veri e propri —
   `OrbitalPresetState`'s `points`/`colors`/`resample.psi2Sorted` e
   `AtomPresetState`'s `points` — condividono ora un'unica arena,
   dimensionata sulla vista più grande, invece di due riserve sempre
   residenti (stesso ragionamento di sicurezza: mai in contemporanea). SOLO
   le due istanze delle viste live (`runOrbitalView()`/`runAtomView()`'s
   proprio `preset`) si legano a quest'arena — `debug/screenshot_batch.cpp`
   e `debug/gif_capture_test.cpp` mantengono deliberatamente proprio storage
   privato separato, perché le loro catture possono girare sul task della
   console screenshot in contemporanea alla vista live (vedi
   `debug/screenshot_pause.h`): condividere l'arena anche lì corromperebbe
   silenziosamente quel che la vista live sta mostrando. **Trade-off
   esplicito**: dato che ora le due viste live condividono la memoria
   fisica dei punti, rientrare in una vista dopo che l'altra ha girato deve
   sempre richiamare `load()` da capo (ricostruzione fresca, ~100-200ms)
   invece di riprendere istantaneamente — i vecchi guard
   `presetIndex < 0`/`preset.z == 0` (solo primo avvio) sono stati sostituiti
   da un ricaricamento incondizionato ad ogni rientro. Il preset ricostruito
   è visivamente IDENTICO (stesso indice/Z ricordato, stesso seed RNG
   fisso) — l'unico costo è la pausa di ricaricamento, mai un risultato
   diverso. Implementazione identica su entrambe le board, stesso
   ragionamento del punto 3.

Margine reale misurato dopo i primi tre interventi: **~46KB di DMA libera**
a 1800/500 (contro i ~12.5KB storicamente accettati alla coppia 3400/1000
pre-cross-switch). Portando `kAtomNumPoints` a 3000 (solo punti 1-3,
`kOrbitalNumPoints` lasciato invariato a 1800): **~16.2KB di margine reale**
(`total free DMA=169836` contro 153600 richiesti), allocatore del
framebuffer che fa backoff a 13 blocchi da 26 righe — comportamento di
backoff normale, non un segnale di problemi (vedi sopra). Aggiungendo il
punto 4 e portando `kAtomNumPoints` a **5000**: **~22.6KB di margine reale**
(`total free DMA=176220` contro 153600 richiesti), stessa granularità di
backoff (13 blocchi). Entrambi i valori verificati su hardware reale
(`/dev/ttyUSB0`): boot pulito, `runOrbitalView()` e `runAtomView()` (incluso
il percorso di caricamento elemento che esercita lo scratch condiviso
`sSubshellRadii` via `refreshDissectPlan()`, e ripetuti cross-switch che
esercitano il ricaricamento forzato introdotto dal punto 4) avviati e
alternati ripetutamente senza crash né corruzione — confermato dal log
seriale: dopo il punto 4, ogni rientro in una vista mostra esplicitamente
`loading preset N...`/`loading Z=N...`, laddove prima (punti 1-3 soltanto)
i rientri successivi al primo non mostravano quella riga (ripresa
istantanea, nessun ricaricamento).

**`kOrbitalNumPoints` portato da 1800 a 3000** (stesso giorno, con
`kAtomNumPoints` fermo a 5000): provato direttamente su hardware reale
anziché solo calcolato, seguendo la stessa prassi di misura di questo
documento:

- **5000** (pari a `kAtomNumPoints`): **fallisce al link** — `.dram0.bss`
  sfora `dram0_0_seg` di 2048 byte.
- **4000**: linka, ma va in **abort() al boot** — `total free DMA=144288`
  contro i 153600 richiesti, già negativo prima ancora del backoff a grana
  fine.
- **3500**: boota, ma l'allocatore del framebuffer arriva a fare backoff
  fino a 320 blocchi da 1 riga per soli ~4.7KB di margine reale
  (`total free DMA=158288`) — stesso pattern fragile già scartato per
  `kAtomNumPoints` (vedi sopra), non accettato solo perché boota una volta.
- **3000** (valore tenuto): boot pulito, `total free DMA=166288` contro
  153600 richiesti — **~12.7KB di margine**, allocatore a 13 blocchi da 26
  righe (stessa granularità ragionevole dei traguardi precedenti).
  Verificato con `runOrbitalView()`/`runAtomView()` avviati e alternati
  ripetutamente a questa esatta coppia, nessun crash né corruzione.

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
   console (nessun call site vivo → `--gc-sections` scarta l'intera unità
   di traduzione, incluso `screenshot.cpp`/`screenshot_batch.cpp`/
   `png_writer.cpp`) ha
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

### Vista plane-slice heatmap: **rimossa dal progetto**

La vista plane-slice heatmap (`views/orbital_slice.h/.cpp`, gesto Right
tilt-hold nel visore orbitali) è stata rimossa interamente dal progetto, non
solo esclusa dalla build CYD -- non serve più alcuna nota di esclusione
per-board qui.

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
- ~~Navigazione a tilt non sostituita~~ **Risolto (2026-08-30, non ancora
  verificato su hardware)**: touch resistivo XPT2046, swipe direzionali —
  vedi §"Touch (XPT2046)" sopra. Il bit-banging e l'orientamento
  X/Y restano da confermare su un device reale.
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
