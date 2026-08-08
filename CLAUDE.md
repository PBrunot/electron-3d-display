# CLAUDE.md — Ologramma a piramide su ESP32-S3 (orbitali atomici)

Istruzioni di progetto per Claude Code. Leggi questo file per intero prima di
scrivere codice: definisce hardware, linguaggio scelto (e perché), pinout
reale, architettura software e roadmap.

## 1. Obiettivo

Costruire un "ologramma" tipo Pepper's Ghost su un display 240×240 con cubo
prisma, capace di mostrare in prima battuta una **nuvola di punti 3D** che
ruota nello spazio, per poi arrivare a rappresentare **orbitali elettronici**
di vari elementi (nuvole di probabilità |ψ|² campionate come point cloud).

Milestone 1 (questo repo, per ora): far ruotare in tempo reale una nuvola di
punti 3D qualsiasi (sfera, toro, o un primo orbitale 1s) dentro la piramide,
con un frame rate percepito fluido (target 20–30 FPS).

## 2. Hardware

**Scheda**: Waveshare `ESP32-S3-LCD-1.3` (venduta anche come
`ESP32-S3-LCD-1.3-B` con case, `-C` con case+cubo prisma).

- SoC: ESP32-S3R8, Xtensa LX7 dual-core @ 240 MHz, Wi-Fi/BLE 5
- RAM/Flash: 512 KB SRAM interna + 8 MB PSRAM (OPI) + 16 MB Flash esterna
- Display: 1.3", 240×240, 262K colori, driver **ST7789V2**, bus **SPI**
- IMU: QMI8658 (accelerometro + giroscopio 3 assi) su I2C
- Slot TF card, connettore USB-C, header GPIO 13/16 pin
- Cubo prisma opzionale per l'effetto olografico (Pepper's Ghost a 4 facce)

Wiki ufficiale: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3
Datasheet ST7789VW: https://files.waveshare.com/wiki/ESP32-S3-LCD-1.3/ST7789VW_ESP32S3.pdf
Datasheet QMI8658: https://files.waveshare.com/wiki/common/QMI8658C_datasheet_rev_0.9.pdf
Schema elettrico: https://files.waveshare.com/wiki/ESP32-S3-LCD-1.3/ESP32S3_1.3inch.pdf

### Pinout reale (verificato in demo funzionanti per questa scheda esatta)

```
Display (SPI, ST7789V2):
  TFT_MOSI  41
  TFT_SCLK  40
  TFT_CS    39
  TFT_DC    38
  TFT_RST   42
  Backlight 20   (GPIO, HIGH = accesa)

IMU QMI8658 (I2C):
  SDA  47
  SCL  48

Config display: 240x240, TFT_INVERSION_ON, TFT_RGB_ORDER = RGB
SPI_FREQUENCY: 40 MHz (verificato stabile nelle demo ufficiali;
  provare 60-80MHz solo dopo aver validato 40MHz, con margine per glitch)
```

Questi valori sono presi da `User_Setup.h` di TFT_eSPI usato realmente nei
progetti per questa scheda (non dedotti da datasheet generico), quindi sono
il punto di partenza corretto: non serve ridedurli.

## 3. Linguaggio e toolchain — decisione e motivazione

**Scelto: C++ su framework Arduino (via PlatformIO), libreria `TFT_eSPI`.**

Motivazione (come richiesto: usare MicroPython solo se dimostrabile che
raggiunge performance comparabili al video-replay delle demo native):

- Le uniche demo pubbliche esistenti di ologramma a piramide su *questa
  identica scheda* (VolosR/esp32Prism, nishad2m8/WS-1.3,
  LINXX3/ESP32-S3-LCD-1.3-Weather-Station, tutte linkate dalla wiki
  Waveshare) sono scritte al 99%+ in C/C++ Arduino con `TFT_eSPI` e sprite a
  doppio buffer (`TFT_eSprite`). Nessuna usa MicroPython.
- Waveshare stessa offre solo due percorsi ufficiali per questa scheda:
  Arduino IDE ed ESP-IDF (entrambi C/C++), raccomandando esplicitamente
  ESP-IDF quando servono "requisiti di performance elevati".
- Esistono driver MicroPython veloci per ST7789 su ESP32-S3 (es.
  `russhughes/s3lcd`, che usa DMA via ESP_LCD sotto al cofano — ma è comunque
  codice C compilato, esposto a MicroPython), quindi il *push* dei pixel via
  SPI può essere rapido anche da Python. Il vero collo di bottiglia per una
  nuvola di punti 3D però non è il push SPI: è il loop che ruota/proietta
  ogni punto ad ogni frame, ed è lì che l'overhead dell'interprete
  MicroPython pesa (tipicamente uno o due ordini di grandezza più lento di
  codice C compilato per loop numerici stretti). Non esiste alcun benchmark
  pubblico che dimostri un frame-rate comparabile alle demo native per
  contenuto animato pieno-schermo su questo hardware.
- Conclusione: si parte con C++/Arduino, allineato alle demo esistenti.
  Se in futuro si vuole comunque tentare MicroPython, la strada più
  realistica è MicroPython + `ulab` (numpy-like, vettorializzato in C) per la
  sola matematica di rotazione, mantenendo il rendering via un driver
  framebuffer nativo — ma è un esperimento separato, non lo starting point.

### Perché PlatformIO e non Arduino IDE

Le demo Waveshare/VolosR usano Arduino IDE (GUI), ma esiste già un progetto
PlatformIO community-maintained per questa scheda esatta
(nishad2m8/WS-1.3, cartella `PIO-1.3/`) con board-definition JSON dedicata.
PlatformIO è preferibile per lavorare con Claude Code perché:
- build/flash/monitor da CLI, niente dipendenza da IDE grafico
- gestione dipendenze dichiarativa e riproducibile (`platformio.ini`)
- stesso framework Arduino sottostante, stesse librerie (TFT_eSPI ecc.)

`platformio.ini` di partenza (adattato da quello verificato funzionante):

```ini
[env:WS-ESP32-S3-LCD-1-3]
platform = espressif32@6.5.0
board = WS-ESP32-S3-LCD-1-3
framework = arduino
monitor_speed = 115200

lib_deps =
    bodmer/TFT_eSPI

build_flags =
    -I include
    -D USER_SETUP_LOADED=1
    -D ST7789_DRIVER=1
    -D TFT_WIDTH=240
    -D TFT_HEIGHT=240
    -D TFT_INVERSION_ON=1
    -D TFT_RGB_ORDER=1
    -D TFT_MOSI=41
    -D TFT_SCLK=40
    -D TFT_CS=39
    -D TFT_DC=38
    -D TFT_RST=42
    -D SPI_FREQUENCY=40000000
    -D LOAD_GLCD=1
```

Board definition minima da creare in `boards/WS-ESP32-S3-LCD-1-3.json`
(basata su quella verificata nel progetto community, adattare se necessario):

```json
{
  "build": {
    "arduino": { "partitions": "default_16MB.csv", "memory_type": "qio_opi" },
    "core": "esp32",
    "extra_flags": ["-DARDUINO_ESP32S3_DEV", "-DARDUINO_USB_MODE=1", "-DARDUINO_USB_CDC_ON_BOOT=1"],
    "f_cpu": "240000000L",
    "f_flash": "80000000L",
    "flash_mode": "qio",
    "psram_type": "opi",
    "mcu": "esp32s3",
    "variant": "esp32s3"
  },
  "connectivity": ["wifi", "bluetooth"],
  "frameworks": ["arduino", "espidf"],
  "name": "Waveshare ESP32-S3-LCD-1.3",
  "upload": { "flash_size": "16MB", "maximum_ram_size": 327680, "maximum_size": 16777216, "speed": 921600 },
  "url": "https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3",
  "vendor": "Waveshare"
}
```

Nota: se PlatformIO/build_flags danno problemi con TFT_eSPI (a volte preferisce
un `User_Setup.h` fisico piuttosto che flag di compilazione), come fallback
creare `include/User_Setup.h` con lo stesso contenuto e impostare
`-D USER_SETUP_LOADED=1` puntando a quel file — è il pattern usato nelle demo
reali (vedi `TFT_eSPI/User_Setup.h` in nishad2m8/WS-1.3).

## 4. Demo di riferimento (analizzate, non solo linkate)

| Repo | Cosa fa | Cosa prendere come riferimento |
|---|---|---|
| [VolosR/esp32Prism](https://github.com/VolosR/esp32Prism) | Ologramma a piramide, cartella `holly/`: playback di 79 frame precompilati (immagini RGB565 in PROGMEM) via sprite; cartella `speed/`: quadrante analogico animato disegnato ogni frame | Pattern doppio buffer (`TFT_eSprite` 240×240, `setSwapBytes(true)`, `pushSprite(0,0)`), `tft.setRotation(4)`, `tft.invertDisplay(1)`, backlight su GPIO 20 |
| [nishad2m8/WS-1.3](https://github.com/nishad2m8/WS-1.3) | Orologio con fasi lunari, versione LVGL + PlatformIO | Progetto PlatformIO funzionante per questa scheda esatta (board json, `User_Setup.h` reale), uso di `WS_QMI8658` per l'IMU |
| [LINXX3/ESP32-S3-LCD-1.3---Prism-Version---Weather-Station](https://github.com/LINXX3/ESP32-S3-LCD-1.3---Prism-Version---Weather-Station) | Stazione meteo versione prisma | Riferimento aggiuntivo per layout contenuti pensati per la visione attraverso il cubo |

Punto architetturale importante osservato in `holly.ino`: il contenuto è
centrato sullo schermo (non diviso in quadranti). Per una piramide a 4 facce
questo funziona bene se il visore guarda **da un lato alla volta** — ogni
faccia riflette la stessa immagine centrale, quindi qualsiasi lato mostra la
stessa proiezione. Per un vero effetto "vedo lati diversi dell'oggetto 3D da
lati diversi della piramide simultaneamente" servirebbe invece dividere lo
schermo in 4 quadranti, ciascuno con la vista dell'oggetto ruotata per quel
lato — nessuna delle demo trovate lo fa. **Questa è una decisione di design
aperta, vedi §7.**

## 5. Architettura software (nuvola di punti)

Struttura consigliata, separando dati statici da rendering real-time (stesso
principio delle demo: gli asset pesanti — es. i frame di `holly` — sono
precalcolati, il loop fa solo playback/trasformazioni leggere):

```
src/
  main.cpp              // setup(), loop(), orchestrazione
  display.h/.cpp         // init TFT_eSPI, sprite, backlight, rotazione
  pointcloud.h/.cpp      // generazione statica dei punti (sfera/toro/orbitale)
  render3d.h/.cpp        // rotazione, proiezione, rasterizzazione nel buffer
  imu.h/.cpp              // (M3) lettura QMI8658 per rotazione controllata a mano
```

### Generazione dei punti (fase "offline", eseguita una volta in `setup()`)

- Milestone 1: punti su una sfera o un toro, generati proceduralmente
  (nessun bisogno di dati esterni).
- Milestone 2 (orbitali reali): campionare |ψ(r,θ,φ)|² per gli orbitali
  idrogenoidi (1s, 2s, 2p, 3d, ...) con rejection sampling, per generare N
  punti distribuiti secondo la densità di probabilità. Questo calcolo è
  pesante (funzioni trascendenti, rigetto) → farlo **offline su PC in
  Python**, esportare le coordinate come array C (`PROGMEM`) o come file
  binario su TF card, esattamente come `images.h` fa per i frame di `holly`.
  Non ricalcolare l'orbitale sul microcontrollore ad ogni avvio.

### Rendering real-time (nel `loop()`)

1. Aggiornare una matrice di rotazione 3×3 (incrementale, piccoli angoli per
   frame — evita di ricalcolare seno/coseno per ogni punto ogni volta:
   aggiorna gli angoli globali una volta per frame, poi applica la stessa
   matrice a tutti i punti).
2. Per ogni punto: `p' = R · p`, poi proiezione (ortografica per iniziare,
   prospettica se serve profondità più marcata) su coordinate schermo
   `(x_screen, y_screen)`.
3. Usare la profondità `z` per: dimensione del punto (1 px lontano, 2 px
   vicino) e/o luminosità (cue di profondità economico, niente z-buffer
   vero — per una nuvola di punti sparsa un ordinamento pittore *approssimato*
   o nessun ordinamento è spesso sufficiente).
4. Disegnare tutti i punti in un `TFT_eSprite` 240×240 RGB565 (`fillSprite`
   nero, poi `drawPixel`/`fillCircle` per punto), infine `pushSprite(0,0)`.

### Perché il buffer intero e non update parziali

Il costo dominante è il trasferimento SPI del frame intero: 240×240×2 byte =
115200 byte, che a 40 MHz SPI richiedono ~23 ms in teoria pura (quindi
~40 FPS come tetto teorico a quella frequenza). La matematica di rotazione
per qualche migliaio di punti su un core a 240 MHz con FPU è invece
trascurabile al confronto. Conclusione: non ottimizzare prematuramente la
matematica dei punti; se serve più FPS, il primo posto dove guardare è la
frequenza SPI (provare ad alzarla oltre 40 MHz con cautela) o ridurre l'area
ridisegnata.

## 6. Budget di performance (target)

- Target: 20–30 FPS percepiti per la rotazione (la demo `holly` gira a
  ~1000/28 ≈ 36 FPS nominali con `delay(28)` fisso, come riferimento di cosa
  "si vede fluido" su questo stesso hardware/display).
- Limite teorico SPI a 40 MHz per frame intero 240×240×16bit: ~23 ms → tetto
  di ~43 FPS. Con overhead di rendering CPU realistico, 25–30 FPS è un target
  ragionevole per M1.
- Numero di punti: partire con 500–1500 punti per M1 (sfera/toro di test),
  poi valutare per gli orbitali reali (nuvole più dense, es. 2000–5000 punti)
  se il frame rate regge.

## 7. Roadmap

1. **M1 — Nuvola di punti generica che ruota** (obiettivo di questa
   iterazione): sfera o toro proceduralmente generato, rotazione automatica
   continua, rendering via sprite, nessuna interazione.
2. **M2 — Primo orbitale reale**: sostituire i punti procedurali con punti
   campionati da |ψ|² dell'orbitale 1s (il più semplice, simmetria sferica),
   generati offline e caricati come `PROGMEM`.
3. **M3 — Controllo/interattività**: usare l'IMU QMI8658 per orientare la
   vista a mano (inclinando la scheda) invece della rotazione automatica, o
   per passare da un orbitale all'altro.
4. **M4 — Libreria di orbitali**: 2s, 2p (x/y/z), 3d, selezionabili, con
   eventuale UI minimale (nome elemento/orbitale mostrato a schermo).

## 8. Domande aperte da decidere insieme (non assumere, chiedere)

- **Vista singola vs 4 quadranti**: si accetta il modello "singola proiezione
  centrata, guardata da un lato per volta" (come tutte le demo trovate,
  semplice e già efficace per l'effetto Pepper's Ghost) oppure si vuole
  investire nel rendering a 4 quadranti per una vista simultanea multi-lato?
  Impatta molto la complessità e il budget di FPS (4× il lavoro di
  rasterizzazione).
- **Colore**: mappare la fase/segno della funzione d'onda al colore (es. blu
  per fase negativa, rosso/arancio per positiva, come nelle visualizzazioni
  standard di chimica) o restare in monocromatico bianco-su-nero?
- **Con o senza IMU fin da subito**: rotazione automatica costante per M1 è
  la scelta più semplice; se preferisci controllare l'orientamento a mano fin
  dal primo test, si porta avanti l'integrazione IMU prima.

## 9. Convenzioni di codice

- C++17, stile Arduino (`setup()`/`loop()`), file `.h/.cpp` separati per
  modulo come da struttura in §5.
- Nessuna allocazione dinamica nel `loop()` — pre-allocare tutti i buffer
  (array punti, sprite) in `setup()` o come variabili globali statiche.
- Punti in coordinate float in `[-1, 1]` prima della proiezione; scalare a
  pixel solo nell'ultimo passaggio.
- Commenti e nomi di variabili in inglese per coerenza con le librerie
  (TFT_eSPI ecc.); i messaggi utente/log possono restare in italiano.
