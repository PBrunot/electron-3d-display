# orbitals_host — cross-check tra i porting C++/MicroPython e il riferimento JS

Confronta due implementazioni candidate della matematica degli orbitali
dell'idrogeno **e** del campionamento a nuvola di punti (M2, CLAUDE.md §5/§7)
contro `js_reference.js` (l'estrazione/estensione di
`examples/js-calculations/quantum-physics.js`, di Manuel Joffre):

- `src/orbitals.h/.cpp` (funzione d'onda) + `src/pointcloud.h/.cpp`
  (campionamento per rigetto) — porting C++, pensato per compilare sia qui sul
  PC sia dentro PlatformIO/ESP-IDF per l'ESP32.
- `micropython/orbitals.py` + `micropython/pointcloud.py` — porting
  MicroPython, pensato per girare sia qui sotto l'unix port sia come firmware
  MicroPython sull'ESP32.

Le due implementazioni sono alternative allo stesso problema — questa cartella
esiste per rispondere "il codice è corretto?" per **entrambe**, così la scelta
tra ESP-IDF/C++ e MicroPython per il firmware finale si possa fare su altri
criteri (prestazioni, esperienza di sviluppo, manutenibilità) e non sul
sospetto che una delle due abbia un bug nel porting. Vedi
`examples/js-calculations/README.md` per il contesto completo.

Nessuna dipendenza esterna oltre a strumenti di sistema: `node`, `g++`
(C++17), `python3`, e — solo per il porting MicroPython — un eseguibile
`micropython` sul `PATH` (qui installato dal pacchetto `micropython` di
Ubuntu/apt, unix port). Se `micropython` non è disponibile, `run_crosscheck.sh`
salta quella passata invece di fallire.

## Come si esegue

```sh
./run_crosscheck.sh
```

Genera gli output di riferimento per la funzione d'onda (`out/js/`,
`out/c_f64/`, `out/c_f32/`, `out/mpy/`) **e** per la nuvola di punti
(`out/points_js/`, `out/points_c_f64/`, `out/points_c_f32/`,
`out/points_mpy/`), poi confronta tutto con `compare.py`.

## Nuvola di punti: stesso seme, stessi punti — non solo stessa statistica

`sampleOrbitalPoint()`/`sample_orbital_point()` campiona (r,θ,φ) per rigetto
dalla densità di probabilità fisica |ψ|²·r²·sin(θ) (il fattore r²sin(θ) è
l'elemento di volume in coordinate sferiche — senza non si campionerebbe la
probabilità *fisica*, solo dove |ψ| è grande). Invece di confrontare le tre
implementazioni solo statisticamente (istogrammi, ecc.), tutte e tre usano lo
**stesso generatore pseudocasuale portabile** (`XorShift32`, il triplo
(13,17,5) di Marsaglia, mai gli shift/xor di libreria del linguaggio) con lo
stesso seme e lo stesso ordine di estrazione per tentativo (r, θ, φ, u, in
quest'ordine, a ogni tentativo, accettato o no). Risultato: dato lo stesso
seme, le tre implementazioni producono la **stessa identica sequenza di punti
accettati** (a meno dell'arrotondamento macchina in double, o di uno
scostamento maggiore atteso in float32 — vedi passate 3/4/6 sotto). Questo è
un confronto molto più severo di un confronto statistico: cattura anche bug
sottili (es. un fattore mancante nella densità, o un bound scorretto) che uno
scostamento nella sola forma complessiva della nuvola potrebbe non rivelare.

`initOrbitalSampler()`/`init_orbital_sampler()` precalcola un bound valido
sulla densità (necessario per il rigetto) dal massimo tabulato di `|r·R(r)|`
e `|P_l^m(θ)|`, con un margine di sicurezza (`DENSITY_BOUND_MARGIN = 1.15`,
identico nei tre porting) contro il rischio che il vero massimo continuo
cada tra due punti tabulati — non è una garanzia rigorosa, ma sufficiente per
una nuvola di punti visuale (stesso spirito pragmatico delle approssimazioni
già documentate in CLAUDE.md §5.3).

## Sei passate di confronto, tolleranze diverse apposta

**Funzione d'onda** (`out/js` come riferimento):

1. **vs `out/c_f64`, tolleranza stretta (rtol=1e-9)** — correttezza del
   porting C++: JS gira in double, `-DORBITAL_USE_DOUBLE` fa girare lo stesso
   identico `orbitals.cpp` in double. Fa parte del codice di uscita.
2. **vs `out/c_f32`, tolleranza larga (rtol=2e-3)** — informativa: quantifica
   il costo di `float` (precisione reale della FPU dell'ESP32 in C++) prima
   di scoprirlo su hardware. Non blocca lo script.
5. **vs `out/mpy`, tolleranza stretta (rtol=1e-9)** — correttezza del porting
   MicroPython, stesso principio del punto 1 (l'unix port gira in double,
   vedi nota sotto). Fa parte del codice di uscita.

**Nuvola di punti** (`out/points_js` come riferimento, stesso seme fisso
in tutti i generatori):

3. **vs `out/points_c_f64`, tolleranza stretta (rtol=1e-9)** — ci si aspetta
   punti **bit-identici** (a epsilon macchina), non solo una forma simile:
   stesso seme + stesso algoritmo + stessa precisione double ⇒ stessa
   sequenza di accept/reject. Fa parte del codice di uscita.
4. **vs `out/points_c_f32`, tolleranza larga (rtol=2e-3)** — informativa: in
   float32 un tentativo vicino al confine accetta/rigetta può capovolgersi
   rispetto al riferimento double, disallineando (potenzialmente) la
   sequenza di punti accettati da quel punto in poi per quel caso di test.
   Non blocca lo script; nella pratica osservata qui i 100 punti/caso
   concordano comunque entro tolleranza per tutti gli 11 casi.
6. **vs `out/points_mpy`, tolleranza stretta (rtol=1e-9)** — come il punto 3,
   correttezza del porting MicroPython. Fa parte del codice di uscita.

Lo script esce con codice non zero se una qualunque delle passate 1/3/5/6
(correttezza) fallisce; le passate 2/4 sono solo informative.

## Nota sulla precisione di MicroPython (unix port vs ESP32 reale)

L'unix port installato qui (pacchetto Ubuntu `micropython`, v1.17) usa float a
**doppia precisione** — verificato empiricamente (`1/3` stampa 16 cifre
significative, non le ~7 di un float32). Questo rende le passate 5/6 un vero
gate di correttezza (double vs double), **non** una misura della precisione
che si avrà realmente sul firmware ESP32.

## Eseguito anche su un ESP32-S3 reale: `run_on_device.sh`

```sh
./run_on_device.sh [porta-seriale]   # default /dev/ttyACM0
```

Copia `micropython/orbitals.py`, `micropython/pointcloud.py` e
`test_cases.csv` su un ESP32-S3 collegato via USB (già flashato con
MicroPython — firmware ufficiale `ESP32_GENERIC_S3-SPIRAM_OCT` da
[micropython.org/download/ESP32_GENERIC_S3](https://micropython.org/download/ESP32_GENERIC_S3/),
la variante Octal-SPIRAM perché questa scheda usa PSRAM OPI/ottale, vedi
CLAUDE.md §2), esegue `device_gen.py` **sul microcontrollore stesso**
(stessi 11 casi, stesse tabelle, stessi 100 punti/caso), recupera i CSV
risultanti e li confronta con `out/js`/`out/points_js` — richiede `mpremote`
(`pip3 install --user mpremote`).

**Risultati misurati su una Waveshare ESP32-S3-LCD-1.3 reale (MicroPython
1.28.0, build `ESP32_GENERIC_S3-SPIRAM_OCT`):**

- **Precisione: singola precisione (float32)**, a differenza dell'unix port.
  Verificato con la stessa sonda (`1.0 + 1e-10 == 1.0` → `True` sul REPL del
  dispositivo; `1/3` stampa `0.33333334`, 8 cifre). Questo era esattamente il
  dubbio aperto lasciato nella nota precedente — ora risolto con una misura
  diretta, non più una supposizione.
- **Correttezza**: 43/44 file funzione d'onda e 11/11 file nuvola di punti
  entro la tolleranza informativa (rtol=2e-3), lo stesso identico schema
  visto per il build C++ float32 sul PC — compreso lo stesso identico caso
  di fallimento (`9_7_3_psi_samples.csv`, riga vicina a uno zero della
  funzione d'onda, non un bug). L'hardware reale si comporta esattamente
  come previsto dallo studio di precisione float32 già fatto sul PC.
- **Prestazioni**: costruire una tabella da 1001 punti richiede ~70-80ms;
  campionare 100 punti per rigetto richiede da pochi ms (orbitali semplici)
  a **~6.3 secondi per il caso più difficile** (n=9,ℓ=7,m=3 — bassa frazione
  di accettazione per un orbitale angolarmente complesso, moltiplicata per
  l'overhead dell'interprete). L'intero sweep di 11 casi (tabelle + 280
  campioni psi + 100 punti ciascuno) impiega **~55 secondi sul dispositivo**.
  Accettabile per una generazione "una tantum all'avvio", non per
  rigenerare la nuvola ogni frame — coerente con l'architettura already
  prevista in CLAUDE.md §5 (i punti si generano una volta, poi si ruotano).
- **Trasferimento file**: `mpremote fs cp -r` (copia ricorsiva) si è
  rivelato inaffidabile su questo setup (solleva `IsADirectoryError` a metà
  copia, sembra un bug/edge-case di mpremote 1.28.0 con questa combinazione
  device/transport) — `run_on_device.sh` copia invece i 55 file noti uno per
  uno, incatenati con `+` in un'unica sessione mpremote. Anche
  `mpremote run device_gen.py` con lo script che stampava ~20k righe di CSV
  direttamente sulla console seriale ha prodotto output interlacciato/
  corrotto sotto carico sostenuto (bytes riordinati, non un bug nel calcolo
  — le stesse tabelle scritte su file e poi lette sono risultate corrette);
  per questo `device_gen.py` scrive su file (`/out_dev/`) invece di stampare.

## File

- `test_cases.csv` — lista `n,ℓ,m` condivisa da tutti i generatori (unica
  fonte di verità, così JS, C++ e MicroPython testano esattamente le stesse
  combinazioni), sia per la funzione d'onda che per la nuvola di punti.
- `gen_js_reference.js` / `gen_c_reference.cpp` / `gen_mpy_reference.py` —
  funzione d'onda: per ogni caso scrivono 4 CSV con lo stesso schema
  (`<n>_<l>_<m>_coeffs.csv`, `..._legendre_table.csv`,
  `..._radial_table.csv`, `..._psi_samples.csv`). La griglia di
  campionamento di `psi_samples.csv` (frazioni di r, valori di θ/φ) è una
  costante identica nei tre file — se la cambi, cambiala in tutti e tre.
- `gen_points_js.js` / `gen_points_c.cpp` / `gen_points_mpy.py` — nuvola di
  punti: per ogni caso scrivono `<n>_<l>_<m>_points.csv` (colonne
  `index,x,y,z`), stesso seme (`SEED = 12345`) e stesso numero di punti
  (`POINTS_PER_CASE = 100`) — costanti identiche nei tre file.
- `compare.py` — confronto con la stessa convenzione di `numpy.allclose`
  (`abs_err <= atol + rtol*|riferimento|`), stampa una tabella riassuntiva,
  exit code non zero se qualcosa fallisce. Sa confrontare sia i CSV della
  funzione d'onda che quelli della nuvola di punti (colonne `x,y,z`).
- `device_gen.py` / `run_on_device.sh` / `parse_device_output.py` — eseguono
  lo stesso confronto ma **sul microcontrollore ESP32-S3 reale** invece che
  sull'unix port; vedi la sezione dedicata sopra.
- `out/` — generato, non committato (vedi `.gitignore`).

## Nota sui file `_coeffs.csv`

`initLegendreCoeffs()` nel JS originale non azzera l'array dei coefficienti
tra una chiamata e l'altra (commento originale: "No problem in using same
array thanks to even/odd alternation in coefficients") — gli indici di
parità sbagliata per la coppia (ℓ,m) corrente non vengono mai letti da
`computePLM`, quindi restano "sporchi" con valori della chiamata precedente
senza che questo influenzi il risultato. `gen_js_reference.js` azzera
l'array prima di ogni chiamata apposta, per rendere il confronto con le
versioni C++/MicroPython (che partono sempre da zero) leale — altrimenti si
vedono FAIL spuri sugli indici inutilizzati, non un vero bug del porting.
