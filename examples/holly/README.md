# holly.ino — riferimento esterno

Copiato da [VolosR/esp32Prism](https://github.com/VolosR/esp32Prism), cartella
`holly/`, per la stessa identica scheda (Waveshare ESP32-S3-LCD-1.3).

`images.h` (i 79 frame RGB565 precompilati, ~9.6 MB) **non** è incluso qui:
è specifico a quella demo (playback di un'animazione fissa) e non serve come
riferimento architetturale. Se serve per compilare `holly.ino` così com'è,
scaricalo da:
https://raw.githubusercontent.com/VolosR/esp32Prism/main/holly/images.h

Pattern da cui prendere spunto (vedi CLAUDE.md §4):
- doppio buffer con `TFT_eSprite` 240×240, `setSwapBytes(true)`, `pushSprite(0,0)`
- `tft.setRotation(4)`, `tft.invertDisplay(1)`
- backlight accesa via `pinMode(20, OUTPUT); digitalWrite(20, HIGH);`
- `delay(28)` fisso nel loop → ~36 FPS nominali, riferimento di "fluido" su
  questo stesso hardware
