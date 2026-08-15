# orbitals_host — cross-check tra i porting C++/MicroPython e il riferimento JS

Confronta due implementazioni candidate della matematica degli orbitali
dell'idrogeno contro `js_reference.js` (l'estrazione verbatim delle funzioni
equivalenti da `examples/js-calculations/quantum-physics.js`, di Manuel
Joffre):

- `src/orbitals.h/.cpp` — porting C++, pensato per compilare sia qui sul PC
  sia dentro PlatformIO/ESP-IDF per l'ESP32.
- `micropython/orbitals.py` — porting MicroPython, pensato per girare sia qui
  sotto l'unix port sia come firmware MicroPython sull'ESP32.

Le due implementazioni sono alternative allo stesso problema — questa cartella
esiste per rispondere "il codice è corretto?" per **entrambe**, così la scelta
tra ESP-IDF/C++ e MicroPython per il firmware finale si possa fare su altri
criteri (prestazioni, esperienza di sviluppo, manutenibilità) e non sul
sospetto che una delle due abbia un bug nel porting. Vedi
`examples/js-calculations/README.md` per il contesto completo e CLAUDE.md
§5/§7 (M2) per come questo si inserisce nella pipeline della nuvola di punti.

Nessuna dipendenza esterna oltre a strumenti di sistema: `node`, `g++`
(C++17), `python3`, e — solo per il porting MicroPython — un eseguibile
`micropython` sul `PATH` (qui installato dal pacchetto `micropython` di
Ubuntu/apt, unix port). Se `micropython` non è disponibile, `run_crosscheck.sh`
salta quella passata invece di fallire.

## Come si esegue

```sh
./run_crosscheck.sh
```

Genera gli output di riferimento (`out/js/`), compila ed esegue il porting
C++ in due precisioni (`out/c_f64/` con `-DORBITAL_USE_DOUBLE`, `out/c_f32/`
con il default `float`, la stessa precisione che userà l'ESP32 in C++), esegue
il porting MicroPython (`out/mpy/`) se disponibile, poi confronta tutto con
`compare.py`.

## Tre passate di confronto, tolleranze diverse apposta

1. **`out/js` vs `out/c_f64`, tolleranza stretta (rtol=1e-9)** — test di
   correttezza del porting C++: JS gira in double, `-DORBITAL_USE_DOUBLE` fa
   girare lo stesso identico `orbitals.cpp` in double. Qualunque scostamento
   oltre l'errore di arrotondamento macchina segnala un bug nel porting, non
   un limite di precisione.
2. **`out/js` vs `out/c_f32`, tolleranza larga (rtol=2e-3)** — informativa,
   non un gate di correttezza: quantifica quanto costa passare a `float` (la
   precisione reale della FPU dell'ESP32 in C++) **prima** di scoprirlo su
   hardware. Un fallimento qui non blocca lo script, ma va guardato — tipico
   vicino a uno zero della funzione d'onda per (n,ℓ) alti, dove la
   cancellazione numerica in float32 amplifica l'errore assoluto pur restando
   trascurabile in termini di forma complessiva della nuvola di punti.
3. **`out/js` vs `out/mpy`, tolleranza stretta (rtol=1e-9)** — test di
   correttezza del porting MicroPython, stesso principio del punto 1: l'unix
   port usato qui (vedi nota sulla precisione più sotto) gira in double, quindi
   ci si aspetta lo stesso accordo a livello di epsilon macchina. Fa parte del
   codice di uscita dello script tanto quanto la passata 1.

Lo script esce con codice non zero se la passata 1 **o** la passata 3
falliscono (correttezza); la passata 2 è solo informativa e non influenza il
codice di uscita.

## Nota sulla precisione di MicroPython

L'unix port installato qui (pacchetto Ubuntu `micropython`, v1.17) usa float a
**doppia precisione** — verificato empiricamente (`1/3` stampa 16 cifre
significative, non le ~7 di un float32). Questo rende la passata 3 un vero
gate di correttezza (double vs double), **non** una misura della precisione
che si avrà realmente sul firmware ESP32: build diverse di MicroPython per
ESP32 possono usare float singola o doppia precisione a seconda della
configurazione della scheda. Prima di usare questa cartella per decidere se
MicroPython è "abbastanza preciso" sull'hardware finale, verificare la
precisione float del firmware ESP32 target (es. `1.0 + 1e-10 == 1.0` nella
REPL: `True` → singola precisione, `False` → doppia) — e tenere presente che
la doppia precisione su un microcontrollore senza FPU per double è anche
molto più lenta, un fattore rilevante quanto la precisione per la scelta
ESP-IDF/C++ vs MicroPython.

## File

- `test_cases.csv` — lista `n,ℓ,m` condivisa da tutti i generatori (unica
  fonte di verità, così JS, C++ e MicroPython testano esattamente le stesse
  combinazioni).
- `gen_js_reference.js` / `gen_c_reference.cpp` / `gen_mpy_reference.py` —
  per ogni caso scrivono 4 CSV con lo stesso schema
  (`<n>_<l>_<m>_coeffs.csv`, `..._legendre_table.csv`,
  `..._radial_table.csv`, `..._psi_samples.csv`), così `compare.py` può fare
  un diff riga per riga. La griglia di campionamento di `psi_samples.csv`
  (frazioni di r, valori di θ/φ) è definita come costante identica nei tre
  file — se la cambi, cambiala in tutti e tre.
- `compare.py` — confronto con la stessa convenzione di `numpy.allclose`
  (`abs_err <= atol + rtol*|riferimento|`), stampa una tabella riassuntiva,
  exit code non zero se qualcosa fallisce.
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
