# orbitals_host — cross-check tra il porting C++ e il riferimento JS

Confronta `src/orbitals.h/.cpp` (il porting C++ della matematica degli
orbitali dell'idrogeno, pensato per compilare sia qui sul PC sia dentro
PlatformIO per l'ESP32) contro `js_reference.js` (l'estrazione verbatim delle
funzioni equivalenti da `examples/js-calculations/quantum-physics.js`, di
Manuel Joffre). Vedi `examples/js-calculations/README.md` per il contesto
completo e CLAUDE.md §5/§7 (M2) per come questo si inserisce nella pipeline
della nuvola di punti.

Nessuna dipendenza esterna: solo `node`, `g++` (C++17) e `python3` di sistema.

## Come si esegue

```sh
./run_crosscheck.sh
```

Genera gli output di riferimento (`out/js/`), compila ed esegue il porting
C++ in due precisioni (`out/c_f64/` con `-DORBITAL_USE_DOUBLE`, `out/c_f32/`
con il default `float`, la stessa precisione che userà l'ESP32), poi confronta
con `compare.py`.

## Due passate di confronto, tolleranze diverse apposta

1. **`out/js` vs `out/c_f64`, tolleranza stretta (rtol=1e-9)** — è il vero
   test di correttezza del porting: JS gira in double, `-DORBITAL_USE_DOUBLE`
   fa girare lo stesso identico `orbitals.cpp` in double. Qualunque
   scostamento oltre l'errore di arrotondamento macchina segnala un bug nel
   porting, non un limite di precisione. Lo script esce con lo stesso codice
   di questa passata.
2. **`out/js` vs `out/c_f32`, tolleranza larga (rtol=2e-3)** — informativa,
   non un gate di correttezza: quantifica quanto costa passare a `float`
   (la precisione reale della FPU dell'ESP32) **prima** di scoprirlo su
   hardware. Un fallimento qui non blocca lo script, ma va guardato — tipico
   vicino a uno zero della funzione d'onda per (n,ℓ) alti, dove la
   cancellazione numerica in float32 amplifica l'errore assoluto pur restando
   trascurabile in termini di forma complessiva della nuvola di punti.

## File

- `test_cases.csv` — lista `n,ℓ,m` condivisa da entrambi i generatori (unica
  fonte di verità, così JS e C++ testano esattamente le stesse combinazioni).
- `gen_js_reference.js` / `gen_c_reference.cpp` — per ogni caso scrivono 4 CSV
  con lo stesso schema (`<n>_<l>_<m>_coeffs.csv`, `..._legendre_table.csv`,
  `..._radial_table.csv`, `..._psi_samples.csv`), così `compare.py` può fare
  un diff riga per riga. La griglia di campionamento di `psi_samples.csv`
  (frazioni di r, valori di θ/φ) è definita come costante identica in
  entrambi i file — se la cambi, cambiala in tutti e due.
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
l'array prima di ogni chiamata apposta, per rendere il confronto con la
versione C++ (che parte sempre da zero) leale — altrimenti si vedono FAIL
spuri sugli indici inutilizzati, non un vero bug del porting.
