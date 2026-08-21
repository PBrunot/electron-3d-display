# Firmware MicroPython — guida al flashing

**Read in / Leggi in:** [English](README.md) · Italiano

Questa cartella è la cosa vera che gira sul pannello: menu di avvio,
viewer degli orbitali, esploratore degli elementi, gesti di nudge guidati
dall'IMU, e i moduli matematici condivisi usati anche da `pc/` e dalla demo
web. Il flashing è in due passi separati — l'**interprete MicroPython di
base** (una volta sola, o ogni volta che si vuole una release più recente
di MicroPython), poi **i file di questo progetto** (ogni volta che si
modifica qualcosa qui dentro).

## 1. Requisiti scheda e firmware

Scheda target: **Waveshare ESP32-S3-LCD-1.3** — ESP32-S3R8, dual-core
Xtensa LX7 @ 240 MHz, 512 KB SRAM, **8 MB di PSRAM cablata in modalità
octal/OPI**, 16 MB di flash.
([wiki](https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3))

Poiché la PSRAM è octal, serve la build **octal-SPIRAM** di MicroPython
per ESP32-S3 — la build ESP32-S3 normale/quad-SPIRAM non riesce a
inizializzare correttamente la PSRAM di questa scheda.

Scarica l'ultima release stabile da
[micropython.org/download/ESP32_GENERIC_S3](https://micropython.org/download/ESP32_GENERIC_S3/)
e scegli la variante **`SPIRAM_OCT`**, es.
`ESP32_GENERIC_S3-SPIRAM_OCT-<versione>.bin` (non l'immagine
`ESP32_GENERIC_S3` semplice, e non `SPIRAM_N16R8` — quelle sono per un
cablaggio PSRAM diverso).

## 2. Strumenti

```sh
pip install esptool mpremote
```

## 3. Flasha il firmware MicroPython di base

Collega la scheda via USB-C e identifica la sua porta seriale:

```sh
ls /dev/tty*      # Linux/WSL — di solito /dev/ttyACM0 (USB-CDC nativo sull'S3)
                   # su WSL potrebbe servire prima `usbipd attach` per passare
                   # il dispositivo alla VM Linux
# oppure controlla la porta COM in Gestione dispositivi su Windows
```

Se l'auto-reset di esptool in modalità bootloader non funziona, forzalo a
mano: tieni premuto **BOOT**, premi **RESET**, rilascia **BOOT**.

```sh
esptool.py --chip esp32s3 --port <PORTA> erase_flash
esptool.py --chip esp32s3 --port <PORTA> --baud 460800 write_flash -z 0 ESP32_GENERIC_S3-SPIRAM_OCT-<versione>.bin
```

`erase_flash` è importante: cancella qualsiasi filesystem/tabella delle
partizioni residuo di un firmware precedente (es. la build C++/ESP-IDF in
`src/`, che usa una tabella delle partizioni completamente diversa) prima
di scrivere quella di MicroPython.

Riavvia la scheda, poi verifica che sia viva:

```sh
mpremote connect <porta> repl
```

Dovresti atterrare su un prompt `>>>`. `Ctrl-]` esce dalla REPL e torna
alla shell.

## 4. Copia i file di questo progetto sulla scheda

Dalla root del repository:

```sh
mpremote connect <porta> fs cp -r micropython/. :
```

Questo copia ogni modulo presente qui (`boot.py`, `main.py`, `chooser.py`,
`orbital_view.py`, `atom_view.py`, i driver IMU/display, e le tabelle dati
come `hfs_tables.bin`) piatto sul filesystem root del dispositivo,
coerentemente con gli import in `main.py`.

Cancella `micropython/__pycache__` in locale se esiste (cache `.pyc` da
esecuzioni sul PC) — è innocua sul dispositivo ma spreca flash per
qualcosa che lì non viene mai importato.

## 5. Eseguilo

Un power-cycle della scheda basta: MicroPython esegue automaticamente
`boot.py` e poi `main.py` a ogni avvio, che lancia il menu di
`chooser.py` — inclina la scheda verso l'alto per il viewer degli
orbitali dell'idrogeno, verso il basso per l'esploratore degli elementi.

Per eseguirlo subito senza riavviare:

```sh
mpremote connect <porta> exec "exec(open('main.py').read())"
```

Per saltare direttamente in un viewer invece del menu (utile in fase di
debug):

```sh
mpremote connect <porta> exec "import atom_view; atom_view.run()"
mpremote connect <porta> exec "import orbital_view; orbital_view.run()"
```

## Risoluzione problemi

- **`mpremote run <file>` si blocca dopo un watchdog reset.** Visto su
  questa scheda — sembra un'interazione raw-paste-mode / USB-passthrough,
  non un bug di MicroPython o dei driver. Usa il flusso `fs cp` + `exec`
  sopra invece di `mpremote run`.
- **Porta seriale non trovata.** Ricontrolla `ls /dev/tty*` (Linux/WSL) o
  Gestione dispositivi (Windows) prima/dopo aver collegato la scheda; su
  WSL il dispositivo va prima collegato con `usbipd` prima di comparire
  come `/dev/ttyACM0`.
- **La PSRAM sembra assente** (es. `import gc; gc.mem_free()` riporta solo
  qualche centinaio di KB invece di megabyte). Probabilmente hai flashato
  la build non-octal — riflasha con l'immagine `SPIRAM_OCT` del punto 1.
- **File copiati ma non parte nulla / errori di import.** Assicurati che
  `cp -r micropython/. :` abbia copiato nella *root* del filesystem (i
  due punti `:` finali senza percorso dopo), non in una sottocartella
  `micropython/` — `main.py` fa `import chooser` ecc. aspettandosi tutto
  piatto nella root.
