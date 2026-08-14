# cube.ino — riferimento per il rendering 3D

Sviluppato in [VolosR/esp32Prism](https://github.com/VolosR/esp32Prism) (repo
di esperimenti sulla stessa identica scheda, cartella `cube/`) come banco di
prova per la pipeline di rendering 3D **prima** di applicarla a una nuvola di
punti. Copiato qui perché la matematica/pipeline mappa direttamente sul
modulo `render3d.h/.cpp` previsto in CLAUDE.md §5.

Sketch Arduino IDE autocontenuto (non ancora convertito a PlatformIO/`.h+.cpp`
separati) — un cubo con facce colorate piene, illuminazione di profondità,
che ruota di continuo su tutti e tre gli assi.

## Cosa dimostra, riusabile per `render3d.cpp`

- **Rotazione 3D incrementale**: matrice di rotazione X→Y→Z applicata una
  volta per frame agli angoli globali, poi la stessa trasformazione a ogni
  vertice — esattamente il pattern raccomandato in CLAUDE.md §5.1.
- **Proiezione prospettica**: `factor = scale / (z + camDistance)`, la stessa
  idea di "profondità → dimensione" richiesta per la nuvola di punti
  (CLAUDE.md §5.3), qui applicata a vertici invece che a singoli punti.
- **Backface culling via area con segno 2D (shoelace) sulle coordinate GIÀ
  proiettate a schermo**, non tramite prodotto scalare normale·vista in 3D.
  Punto importante: con una telecamera vicina rispetto all'oggetto (qui
  `camDistance=2.8` contro mezzo lato cubo=1 — proporzione paragonabile a
  quella prevista per la nuvola di punti nella piramide), il test 3D
  normale·vista è solo un'approssimazione parassiale e produce artefatti
  visibili vicino al profilo dell'oggetto. Il test 2D sull'area con segno è
  invece esatto indipendentemente dalla prospettiva. Vedi commento
  `signedArea()` nel codice per la derivazione completa — bug reale trovato
  e corretto durante lo sviluppo di questo esempio.
- **Un solo passaggio, nessun ordinamento per profondità**: per un solido
  **convesso**, il backface culling da solo basta — le facce visibili non si
  sovrappongono mai a schermo, quindi non serve painter's algorithm. Per una
  nuvola di punti sparsa (non convessa, punti non facce) questo non si
  applica direttamente, ma il principio "non ottimizzare con un ordinamento
  se il culling/l'occlusione economica bastano già" resta valido (vedi
  CLAUDE.md §5.3, che infatti raccomanda ordinamento pittore *approssimato o
  nessuno* per i punti).
- **Ombreggiatura di profondità**: colore di ogni faccia scalato in luminosità
  in base alla Z media dei suoi vertici (`minBrightness` come pavimento per
  non sparire nel nero) — stesso principio di "z → luminosità" indicato per i
  punti in CLAUDE.md §5.3, qui per faccia invece che per punto.

## Cosa NON è direttamente riusabile

- È scritto per **vertici di poligoni** (8 vertici, 6 facce), non per una
  **nuvola sparsa di migliaia di punti indipendenti** — la nuvola non ha
  facce da riempire né normali da calcolare, solo `drawPixel`/`fillCircle`
  per punto. La rotazione+proiezione (la parte pesante matematicamente) è
  invece la stessa identica logica, solo applicata punto per punto anziché
  vertice per vertice.
- `fillTriangle`/riempimento facce non serve per una nuvola di punti rada.

## Correzioni hardware verificate su questa scheda fisica

Applicate in questo sketch (`tft.writecommand(TFT_MADCTL)` +
`tft.writedata(...)` dopo `tft.setRotation(4)`), **importanti anche per
`display.cpp`** — vedi la nota in CLAUDE.md §2 e il file
`examples/corner_calibration/README.md` per i dettagli completi:

1. **Mirror hardware del pannello**: il pannello di questa scheda scandisce
   le colonne in direzione opposta a quella che il bit `MX` di `MADCTL`
   assume di default. Nessuna delle 4 combinazioni standard di
   `tft.setRotation()` (0-3) può correggerlo da sola — sono tutte rotazioni
   proprie, preservano lo specchiamento invece di rimuoverlo. Serve una
   scrittura raw di `MADCTL` con **solo** il bit `MX` impostato (non `MX+MY`,
   che risulterebbe ancora specchiato, solo ruotato).
2. **Ordine colore BGR, non RGB**: contrariamente a quanto documentato in
   CLAUDE.md §2 (`TFT_RGB_ORDER = RGB`, preso da `User_Setup.h` di altre demo
   per questa scheda), il pannello testato in `esp32Prism` mostra i colori
   scambiati R/B se configurato RGB (giallo → ciano, rosso ↔ blu, verde
   invariato — firma classica di uno scambio canali R/B). Verificato con un
   test dedicato a colori piatti nei 4 angoli (vedi
   `examples/corner_calibration/`). Possibile variazione di lotto/pannello
   tra unità diverse della stessa scheda — **riverificare su questa unità
   fisica specifica** con lo stesso test prima di fidarsi ciecamente.
