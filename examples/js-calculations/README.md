# js-calculations — riferimento matematico per gli orbitali atomici (M2)

Questa cartella conteneva una copia locale di materiale di terze parti
(`quantum-physics.js`, `hydrogenOrbitals.js`, e la pagina salvata `Quantum
physics online.html` + asset) di Manuel Joffre (c) 2020-2022,
[www.quantum-physics.polytechnique.fr](https://www.quantum-physics.polytechnique.fr/),
fornito da un professore universitario come riferimento verificato per la
matematica degli orbitali dell'idrogeno — la base per la Milestone 2 di
CLAUDE.md §7 (campionamento di |ψ|² per generare la nuvola di punti di un
orbitale reale, dopo la sfera/toro procedurale di M1).

**Rimossa**: non abbiamo diritti di ridistribuzione su quel codice, quindi
non è più committata in questo repo. Per recuperarla:

- Pagina originale (JS incluso via browser dev tools / "salva pagina come"):
  <https://www.quantum-physics.polytechnique.fr/hydrogenOrbitals.php>
- Home del sito, con l'elenco completo delle altre simulazioni della stessa
  libreria (`quantum-physics.js` è condiviso da più pagine):
  <https://www.quantum-physics.polytechnique.fr/>

## Cosa conteneva (per contesto)

- `quantum-physics.js` (~3000 righe): libreria generica del sito, di cui solo
  le righe ~19-205 riguardano gli orbitali dell'idrogeno (polinomi di
  Legendre associati, funzione radiale via polinomi di Laguerre associati,
  lookup table). Il resto (FFT, simulazione della doppia fenditura, grafici
  2D via `Graphix`/`Ticker`, algebra lineare per la diagonalizzazione) non è
  legato agli orbitali.
- `hydrogenOrbitals.js`: usa le funzioni di cui sopra per costruire e colorare
  una superficie parametrica THREE.js (isosuperficie a soglia di probabilità,
  meshing, GUI). **Non portato** — questo progetto non fa rendering 3D via
  THREE.js, e l'ologramma non usa isosuperfici ma nuvole di punti
  rejection-sampled (CLAUDE.md §5).

## Cosa resta nel repo

`tools/orbitals_host/js_reference.js` contiene una estrazione verbatim di
sole 7 funzioni matematiche pure (nessuna dipendenza da DOM/THREE.js):
`initLegendreCoeffs`, `computePLM`, `initLookupTable`, `initLaguerreCoeffs`,
`hydrogenRadialFunction`, `initLookupTableRadial`, `getValueFromLookupTable`.
Resta nel repo (decisione esplicita, non un'svista) perché è il riferimento
di verità (ground truth) usato per cross-validare il porting C++ in
`src/orbitals.h/.cpp` e quello MicroPython in `micropython/orbitals.py` — vedi
`tools/orbitals_host/README.md` per come si esegue il confronto.

## Cosa NON era direttamente riusabile

- Tutta la parte THREE.js (`ParametricGeometry`, `sphereMesh`, colori HSL per
  fase, GUI/select box) — visualizzazione, non calcolo.
- La ricerca degli zeri (`findLegendreZeros`, `findRadialZeros`) e l'istogramma
  probabilità-vs-soglia (`initEtaTable`) servono a costruire l'isosuperficie a
  soglia fissa di `updateSurface()`; per il rejection sampling di M2 non sono
  necessari (bastano R(r) e P_l^m(θ) per calcolare |ψ|² punto per punto).
