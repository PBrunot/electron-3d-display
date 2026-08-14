# electron-3d-display

Ologramma tipo Pepper's Ghost su ESP32-S3, cubo prisma: nuvole di punti 3D
che ruotano nello spazio, con l'obiettivo finale di rappresentare orbitali
elettronici reali (|ψ|² campionati per densità di probabilità).

Documentazione completa (leggere prima di scrivere codice):

- **[`CLAUDE.md`](CLAUDE.md)** — hardware, pinout verificato, scelta del
  linguaggio/toolchain, architettura software, budget di performance,
  roadmap (M1-M4), domande di design aperte.
- **[`ORBITALI.md`](ORBITALI.md)** — riferimento fisico/matematico per
  calcolare le distribuzioni di probabilità degli orbitali dell'idrogeno
  (formule chiuse, rejection sampling, cosa girare offline vs on-device).

## Hardware

Waveshare `ESP32-S3-LCD-1.3` (ESP32-S3R8, display 240×240 ST7789V2, IMU
QMI8658, cubo prisma opzionale per l'effetto olografico). Dettagli e pinout
completo in `CLAUDE.md` §2.

⚠️ **Prima di collegare il pannello**: `CLAUDE.md` §2 documenta due
correzioni hardware verificate empiricamente su un'unità fisica reale (mirror
del pannello, ordine colore BGR non RGB) — non presenti nel `User_Setup.h`
"di riferimento" usato dalle altre demo per questa scheda. Vedi la sezione
"Correzione verificata" in `CLAUDE.md` §2 prima di dare per scontata la
configurazione display.

## Stato attuale

Scaffolding iniziale (PlatformIO + `TFT_eSPI`, board definition dedicata in
`boards/`), `src/main.cpp` ancora vuoto. Milestone corrente: **M1**, nuvola
di punti procedurale (sfera/toro) che ruota in tempo reale — vedi roadmap
completa in `CLAUDE.md` §7.

## Struttura del repo

```
src/main.cpp          setup()/loop(), punto di ingresso firmware (vuoto per ora)
boards/                board definition PlatformIO per questa scheda esatta
examples/               sketch Arduino standalone, di riferimento — non fanno
                        parte del firmware, non build/flashati da platformio.ini
  holly/                demo esterna (VolosR/esp32Prism) copiata come riferimento
                        per il pattern doppio-buffer/sprite
  cube/                 cubo 3D a facce colorate — pipeline di rendering completa
                        (rotazione, proiezione prospettica, backface culling,
                        ombreggiatura di profondità), sviluppato apposta per
                        preparare render3d.h/.cpp
  corner_calibration/   test diagnostico: mappa lo spazio-sprite sullo
                        spazio-visto attraverso il cubo prisma (nessuna
                        matematica 3D) — ha anche rivelato il bug di ordine
                        colore sopra
```

Ogni cartella in `examples/` ha il proprio `README.md` con i dettagli.

## Build

```
pio run                # compila
pio run -t upload      # compila e flasha
pio device monitor      # serial monitor (115200 baud)
```

Richiede [PlatformIO](https://platformio.org/) (CLI o estensione VSCode, già
raccomandata in `.vscode/extensions.json`).
