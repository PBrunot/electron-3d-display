# js-calculations — riferimento matematico per gli orbitali atomici (M2)

Materiale di terze parti, **non nostro**: `quantum-physics.js` e
`hydrogenOrbitals.js` sono di Manuel Joffre (c) 2020-2022,
[www.quantum-physics.polytechnique.fr](https://www.quantum-physics.polytechnique.fr/)
(salvato qui insieme alla pagina HTML/asset del sito, `Quantum physics
online.html` + `Quantum physics online_files/`, per riferimento offline).
Fornito da un professore universitario come riferimento verificato per la
matematica degli orbitali dell'idrogeno — la base per la Milestone 2 di
CLAUDE.md §7 (campionamento di |ψ|² per generare la nuvola di punti di un
orbitale reale, dopo la sfera/toro procedurale di M1).

## Cosa contiene

- `quantum-physics.js` (~3000 righe): libreria generica del sito, di cui solo
  le righe ~19-205 riguardano gli orbitali dell'idrogeno (polinomi di
  Legendre associati, funzione radiale via polinomi di Laguerre associati,
  lookup table). Il resto (FFT, simulazione della doppia fenditura, grafici
  2D via `Graphix`/`Ticker`, algebra lineare per la diagonalizzazione) non è
  legato agli orbitali e non è stato toccato.
- `hydrogenOrbitals.js`: usa le funzioni di cui sopra per costruire e colorare
  una superficie parametrica THREE.js (isosuperficie a soglia di probabilità,
  meshing, GUI). **Non portato** — questo progetto non fa rendering 3D via
  THREE.js, e l'ologramma non usa isosuperfici ma nuvole di punti
  rejection-sampled (CLAUDE.md §5).

## Cosa è stato estratto per il porting in C++

Solo la matematica pura (nessuna dipendenza da DOM/THREE.js in quelle righe)
è stata copiata **verbatim** in `tools/orbitals_host/js_reference.js`:
`initLegendreCoeffs`, `computePLM`, `initLookupTable`, `initLaguerreCoeffs`,
`hydrogenRadialFunction`, `initLookupTableRadial`, `getValueFromLookupTable`.
Questo file JS eseguibile via Node è il riferimento di verità (ground truth)
usato per cross-validare il porting C++ in `src/orbitals.h/.cpp` — vedi
`tools/orbitals_host/README.md` per come si esegue il confronto e cosa
significano le tolleranze usate.

## Cosa NON è direttamente riusabile

- Tutta la parte THREE.js (`ParametricGeometry`, `sphereMesh`, colori HSL per
  fase, GUI/select box) — visualizzazione, non calcolo.
- La ricerca degli zeri (`findLegendreZeros`, `findRadialZeros`) e l'istogramma
  probabilità-vs-soglia (`initEtaTable`) servono a costruire l'isosuperficie a
  soglia fissa di `updateSurface()`; per il rejection sampling di M2 non sono
  necessari (bastano R(r) e P_l^m(θ) per calcolare |ψ|² punto per punto).
