# INSTALL — compilare e flashare il firmware ESP-IDF (C++)

**Read in / Leggi in:** [English](INSTALL.md) · Italiano

Questa guida copre il firmware C++ in `src/` (il porting PlatformIO/ESP-IDF),
compilato e flashato sulla **Waveshare ESP32-S3-LCD-1.3**. Per il firmware
MicroPython vedi invece [`micropython/README.it.md`](micropython/README.it.md).

## Toolchain

| | |
|---|---|
| IDE | VS Code + [estensione PlatformIO IDE](https://platformio.org/install/ide?install=vscode) |
| Framework | ESP-IDF **6.x** (`platform = espressif32@7.0.1` fissa ESP-IDF 6.0.1), pilotato tramite PlatformIO — non Arduino puro |
| Linguaggio | **C++26** (`-std=gnu++26`) |
| Scheda | Waveshare ESP32-S3-LCD-1.3 — `boards/WS-ESP32-S3-LCD-1-3.json` |

## 1. Installa i prerequisiti

- [VS Code](https://code.visualstudio.com/)
- L'estensione **PlatformIO IDE**, dal marketplace di VS Code. Include un
  suo Python e gestisce da sola il toolchain ESP-IDF 6.x — non serve
  installare ESP-IDF separatamente.
- (Opzionale, solo CLI) `pip install platformio` se preferisci pilotare le
  build con il comando `pio` da terminale invece che dall'interfaccia di
  VS Code.
- **Su Windows**, esegui l'intero flusso da **WSL** (è così che il
  progetto viene effettivamente sviluppato): installa PlatformIO dentro
  la tua distro WSL così che `~/.platformio` e `pio` stiano sul PATH
  Linux, e raggiungi la scheda tramite `/dev/ttyACM0` invece di una porta
  COM Windows nativa (vedi le note sulla porta al punto 4).

## 2. Apri il progetto

```sh
git clone <questo repo>
cd electron-3d-display
code .
```

PlatformIO legge `platformio.ini` automaticamente e, al primo avvio,
propone di installare la piattaforma `espressif32@7.0.1` (toolchain
ESP-IDF 6.0.1) più la board definition di questo progetto,
`WS-ESP32-S3-LCD-1.3` (`boards/WS-ESP32-S3-LCD-1-3.json`) — accetta.
La prima build scarica circa 1-2 GB di toolchain e richiede qualche
minuto.

## 3. Compila

```sh
pio run
```

(oppure, in VS Code: barra laterale PlatformIO → **Project Tasks** →
`WS_ESP32_S3_LCD_1_3` → **Build**, o l'icona con il segno di spunta nella
status bar.)

Impostazioni rilevanti in `platformio.ini`:

- `framework = espidf`, `platform = espressif32@7.0.1` → ESP-IDF 6.0.1
- `build_flags = -std=gnu++26 -I include -I src -Wall -Wextra` — questo è
  un codebase C++26
- `board_build.partitions = partitions_16M.csv` — combacia con i 16 MB di
  flash della scheda; `sdkconfig.defaults` fissa la stessa dimensione
  flash più il tuning di PSRAM/CPU/tick FreeRTOS (il framework espidf
  ignora la chiave `partitions` del board manifest, che è solo per
  Arduino, quindi questo è necessario, non ridondante)
- Le compilazioni passano attraverso `ccache` via
  `tools/extra_script_ccache.py` — vedi il commento di intestazione di
  quel file per il perché il builder espidf di PlatformIO ha bisogno di
  questo workaround invece del solito `CMAKE_*_COMPILER_LAUNCHER`

## 4. Flasha

Collega la scheda via USB-C, poi:

```sh
pio run -t upload
```

(VS Code: barra laterale PlatformIO → **Upload**, l'icona →.)

Note sulla porta:

- **Linux/WSL**: di solito `/dev/ttyACM0` (USB-CDC nativo sull'S3 —
  verifica con `ls /dev/tty*` prima/dopo aver collegato la scheda). Su
  WSL il dispositivo va prima collegato alla VM Linux
  (`usbipd attach ...`) prima di comparire.
- **Windows (nativo, non WSL)**: una porta COM; PlatformIO di solito la
  rileva da solo, oppure imposta `upload_port` in `platformio.ini` per
  forzarne una.
- Se l'auto-reset in modalità bootloader non scatta, tieni premuto
  **BOOT**, premi **RESET**, rilascia **BOOT**, poi riprova l'upload.

## 5. Carica la partizione dati

`data/` (`hfs_tables.bin`, `orbital_samplers.bin`, `atomic_cube.jpg`) è
una partizione SPIFFS separata (`storage`, vedi `partitions_16M.csv`), non
fa parte dell'immagine firmware — richiede un passo di upload a sé:

```sh
pio run -t uploadfs
```

Questo **non** viene eseguito automaticamente con un `upload` semplice al
momento (`tools/extra_script_uploadfs.py` esiste per concatenarlo, ma la
sua riga `pre:` è commentata in `platformio.ini`) — eseguilo a mano ogni
volta che `data/` cambia (es. dopo aver rigenerato le tabelle con
`tools/hfs_table_gen.py` / `tools/orbital_table_gen.py`), e almeno una
volta su una scheda appena flashata. Decommenta quella riga
`extra_scripts` se preferisci che giri a ogni flash automaticamente —
vedi il commento di intestazione dello script per il compromesso
(riformatta l'intera partizione, cancellando eventuali screenshot on-
device catturati dall'ultimo `uploadfs`).

## 6. Monitor / debug

```sh
pio device monitor
```

`monitor_filters = esp32_exception_decoder` in `platformio.ini`
simbolizza automaticamente i backtrace di crash.

## Generazione dati (font, tabelle radiali, tabelle sampler)

Tre artefatti binari/generati sono committati nel repo, quindi una build/
flash normale (punti 3-5) non richiede di rigenerare nulla — ma se
modifichi una delle sorgenti sotto, rigenera prima di riflashare:

- **Font** — `src/render/font_data.h` (le tabelle bitmap dei glifi da cui
  `font.cpp` disegna) è generato da un `.ttf` tramite `tools/font_gen/`:

  ```sh
  cd tools/font_gen
  pip install pillow
  python3 generate_font.py > ../../src/render/font_data.h
  ```

  Rigenera dopo aver cambiato il typeface sorgente o le dimensioni/
  spaziatura in punti nella lista `SIZES` di `generate_font.py`. Vedi
  `tools/font_gen/README.md` per la motivazione completa (perché la
  rasterizzazione offline, perché la modalità PIL `"1"`, come cambiare
  font/dimensioni).

- **Tabelle radiali HFS** (dati screened-potential per elemento) —
  `data/hfs_tables.bin` + `micropython/hfs_tables.bin` da
  `pc/hfs_tables_reduced.npz`:

  ```sh
  python3 tools/hfs_table_gen.py
  ```

- **Tabelle sampler degli orbitali** (i 36 preset idrogenoidi
  precompilati) — `data/orbital_samplers.bin`:

  ```sh
  python3 tools/orbital_table_gen.py
  ```

Gli output in `data/*.bin` hanno effetto sul dispositivo solo dopo
`pio run -t uploadfs` (punto 5) e un riavvio; `micropython/hfs_tables.bin`
ha effetto solo dopo aver ricopiato `micropython/` sulla scheda (vedi
`micropython/README.it.md`). `font_data.h` è compilato direttamente
nell'immagine firmware, quindi un normale `pio run -t upload` lo include.

## Varianti di build

I toggle `#define` in cima a `src/main.cpp` passano dall'app chooser di
default a build di test standalone — `ATOM_VALIDATION_TEST`,
`ATOM_VIEW_TEST`, `ATOM_VIEW`, `COLOR_TEST`, `BENCHMARK_TEST`,
`SLICE_TEST`. Ne può essere attivo esattamente uno alla volta; con
nessuno definito, `app_main()` avvia il vero menu chooser. Modifica il
file, decommenta uno, ricompila, riflasha.

## Screenshot on-device

Il firmware fa girare una piccola console testuale sullo stesso link
seriale già usato da `pio device monitor` per i log
(`src/debug/screenshot_console.h`), quindi puoi catturare e scaricare PNG
dal dispositivo in esecuzione senza hardware aggiuntivo.

**Cattura**, da una sessione `pio device monitor` (o qualsiasi terminale
seriale):

| Digita | Effetto |
|---|---|
| `s` | Cattura il frame corrente → `SS_CAPTURED <nome> <size>` |
| `l` | Elenca gli screenshot salvati → una riga `SS_FILE <nome> <size>` ciascuno, poi `SS_LIST_END` |
| `a` | Cattura in batch ogni preset orbitale + un set curato di elementi + una sequenza di dissezione di Fe (rispecchia l'output di `pc/screenshot.py`) — blocca per decine di secondi, progresso tramite le normali righe di log, poi `SS_CAP_ALL_DONE` |

Le catture vengono scritte come PNG sulla partizione SPIFFS `storage` (la
stessa su cui viene caricato `data/` — vedi punto 5), quindi sopravvivono
a un semplice `pio run -t upload` ma vengono cancellate dal successivo
`pio run -t uploadfs`.

**Download**, dal tuo PC (chiudi prima `pio device monitor` — solo un
processo alla volta può tenere aperta la porta seriale):

```sh
pip install pyserial
python3 pc/pull_screenshots.py --list                       # cosa c'è sul dispositivo
python3 pc/pull_screenshots.py --all                         # scarica tutto in screenshots/
python3 pc/pull_screenshots.py shot_0001.png shot_0003.png   # scarica file specifici
python3 pc/pull_screenshots.py --all --delete                # ...poi cancellali sul dispositivo
```

I file finiscono in `screenshots/` nella root del repo. La porta di
default è `/dev/ttyACM0`; sovrascrivila con `--port`. Questo è un
percorso di cattura separato, on-device, rispetto a `pc/screenshot.py`,
che renderizza le immagini equivalenti direttamente dal simulatore PC
(nessuna scheda necessaria) per le immagini di questo README.

## Alternativa: `idf.py` puro (percorso non testato)

Un `CMakeLists.txt` ESP-IDF puro è presente nella root del repo, quindi in
linea di principio il progetto può anche essere compilato con
`idf.py build/flash/monitor` contro un toolchain ESP-IDF 6.x installato a
mano invece di quello gestito da PlatformIO. Non è il percorso usato
davvero nel giorno per giorno — lo è PlatformIO (sopra).
