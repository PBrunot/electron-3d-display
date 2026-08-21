# ORBITALI.md — Note sul calcolo delle distribuzioni di probabilità

Riferimento tecnico per generare i punti della nuvola a partire dagli
orbitali dell'idrogeno. Riguarda tutti e tre i porting che condividono
questa matematica: lo script offline PC (`pc/orbital_view_pc.py`), il
firmware C++/ESP-IDF (`src/physics/orbitals.h/.cpp` +
`src/physics/pointcloud.h/.cpp`) e MicroPython
(`micropython/orbitals.py` + `micropython/pointcloud.py`).

**Aggiornamento (agosto 2026):** le §3 "trappola scipy" e §4 "rejection
sampling" sotto descrivono l'implementazione **originale**, ormai superata
su entrambi i fronti (motore angolare/radiale e algoritmo di
campionamento) — vedi §3bis e §4bis per lo stato attuale. §0/§1/§2/§6/§7
restano matematica di base, invariata.

## 0. Correzione terminologica

Gli orbitali dell'idrogeno si ricavano dall'equazione di **Schrödinger**
indipendente dal tempo, non da quella di Heisenberg. Heisenberg formulò la
meccanica quantistica in forma matriciale — matematicamente equivalente a
Schrödinger, ma la soluzione analitica standard per l'atomo di idrogeno
(quella con numeri quantici n, l, m e le forme "a manubrio" note dalla
chimica) viene dalla separazione di variabili dell'equazione di Schrödinger
in coordinate sferiche. Utile saperlo se si cercano riferimenti o formule.

## 1. Struttura della funzione d'onda

```
ψ_nlm(r, θ, φ) = R_nl(r) · Y_l^m(θ, φ)
```

Numeri quantici:
- `n` (principale): n = 1, 2, 3, ...
- `l` (angolare/azimutale): 0 ≤ l ≤ n-1  (l=0 → s, l=1 → p, l=2 → d, l=3 → f)
- `m` (magnetico): -l ≤ m ≤ l

La densità di probabilità da campionare per la nuvola di punti è:

```
P(r, θ, φ) = |ψ_nlm(r, θ, φ)|²
```

## 2. Parte radiale R_nl(r)

Formula generale (a₀ = raggio di Bohr, ρ = 2r/(n·a₀)):

```
R_nl(r) = -sqrt[ (2/(n·a₀))³ · (n-l-1)! / (2n·[(n+l)!]³) ] · e^(-ρ/2) · ρ^l · L_{n-l-1}^{2l+1}(ρ)
```

dove `L_k^α` sono i polinomi di Laguerre associati. Per un catalogo finito di
orbitali (n basso) conviene **non** implementare Laguerre generale, ma
hardcodare le forme chiuse. Le più utili, in unità di a₀ (r espresso come
r/a₀, formule già semplificate per la parte radiale, costante di
normalizzazione globale omessa — va moltiplicata per la parte angolare e poi
tutto normalizzato insieme, vedi §3):

```
R_10 (1s):  ∝ e^(-r)
R_20 (2s):  ∝ (2 - r) · e^(-r/2)
R_21 (2p):  ∝ r · e^(-r/2)
R_30 (3s):  ∝ (27 - 18r + 2r²) · e^(-r/3)
R_31 (3p):  ∝ (6r - r²) · e^(-r/3)
R_32 (3d):  ∝ r² · e^(-r/3)
```

Estensione radiale tipica: scala circa come n²·a₀. Per il campionamento,
`r_max ≈ 4-6 · n² · a₀` è un punto di partenza ragionevole (verificare a
occhio disegnando R_nl(r)²·r² prima di fidarsi ciecamente).

## 3. Parte angolare Y_l^m(θ, φ) e le forme "da manuale"

**Trappola importante**: un singolo autostato Y_l^m con m ≠ 0 dà una densità
`|Y_l^m|²` **simmetrica rispetto all'asse z** (dipende solo da θ, non da φ,
perché la fase e^(imφ) sparisce nel modulo quadro) — cioè una ciambella
attorno all'asse z, non il lobo a manubrio dei libri di chimica.

Le forme p_x, p_y, p_z, d_xy, d_z² ecc. sono **combinazioni lineari reali**
di autostati degeneri con lo stesso n,l (legittime perché la degenerazione
in m rende ogni combinazione lineare ancora soluzione stazionaria valida):

```
p_z = Y_1^0                              (già reale)
p_x = (Y_1^{-1} - Y_1^{1}) / √2
p_y = i · (Y_1^{-1} + Y_1^{1}) / √2

d_z²      = Y_2^0
d_xz      = (Y_2^{-1} - Y_2^{1}) / √2
d_yz      = i · (Y_2^{-1} + Y_2^{1}) / √2
d_x²-y²   = (Y_2^{-2} + Y_2^{2}) / √2
d_xy      = i · (Y_2^{-2} - Y_2^{2}) / √2
```

### Scorciatoia cartesiana (consigliata on-device: zero trigonometria)

Per le combinazioni reali sopra, la parte angolare si esprime direttamente
come polinomio in x,y,z/r — niente sin/cos/atan2, solo sqrt e divisioni:

```
s        ∝ 1
p_z      ∝ z/r
p_x      ∝ x/r
p_y      ∝ y/r
d_z²     ∝ (3z² - r²)/r²
d_xz     ∝ xz/r²
d_yz     ∝ yz/r²
d_x²-y²  ∝ (x² - y²)/r²
d_xy     ∝ xy/r²
```

con `r = sqrt(x²+y²+z²)`. Combinato con le forme radiali chiuse del §2
(anch'esse esprimibili in funzione di `r` diretto), l'intera densità
|ψ|² per un orbitale reale si calcola con: 1 `sqrt`, al più 1 `exp`,
e una manciata di moltiplicazioni/somme. Nessuna funzione trascendente
oltre a queste due.

### Trappola scipy (solo per lo script offline in Python)

`scipy.special.sph_harm(m, l, theta, phi)` usa `theta` = angolo azimutale
(longitudine — quello che in fisica si chiama solitamente φ) e `phi` =
angolo polare (colatitudine — di solito θ). **Convenzione invertita**
rispetto a quella fisica standard usata sopra. Se non si controlla la
docstring e si passano gli argomenti "a intuito", si ottengono orbitali
ruotati di 90° senza nessun errore o warning. Commentare esplicitamente
quale convenzione si sta usando nel codice.

**Nota (obsoleta ma tenuta come promemoria):** questa trappola riguardava
un uso diretto di `scipy.special.sph_harm` che non esiste più in nessuno
dei tre porting attuali — vedi §3bis. `scipy`/`numpy` restano in uso altrove
nel progetto (es. `pc/hfs_solver.py`, risolutori agli autovalori per il
modello multi-elettronico, `ATOMS.md`), ma non per la parte angolare degli
orbitali dell'idrogeno.

### 3bis. Motore attuale: coefficienti Legendre/Laguerre generali, non formule hardcoded

L'implementazione reale in `orbitals.h`/`orbitals.py` **non** hardcoda le
forme chiuse del §2/§3 per n basso come raccomandato più sotto (§5): è un
motore generale, porting funzione-per-funzione di
[quantum-physics.js](https://www.quantum-physics.polytechnique.fr/) di
Manuel Joffre (École Polytechnique) — codice non ridistribuito in questo
repo, vedi `examples/js-calculations/README.md` per dove recuperarlo e
`tools/orbitals_host/` per l'harness di validazione incrociata che lo usa
come riferimento.

- **Parte angolare**: `legendreCoeffs()`/`legendre_coeffs()` calcola i
  coefficienti del polinomio di Legendre associato P_l^m(cos θ) per
  QUALSIASI (l, m) supportato (non solo i casi p/d cablati a mano del §3);
  `computePLM()`/`compute_plm()` li valuta a runtime via `sin`/`cos`/`pow`
  standard — **non** la scorciatoia cartesiana x/y/z/r del §3, che nel
  codice attuale non è usata.
- **Parte radiale**: `laguerreCoeffs()`/`laguerre_coeffs()` calcola i
  coefficienti del polinomio di Laguerre associato generale (non le forme
  chiuse §2 per n=1..3), valutati da `computeRadialR()`/analoga.
- **Range supportato**: `kOrbitalNMax = 16` (`ell` fino a 15) — non solo il
  "catalogo da manuale" fino a n=3 che il §5 sotto raccomandava di
  hardcodare.
- **Header-only e `constexpr` in C++** (`orbitals.h`): con un toolchain che
  supporta `<cmath>` constexpr (C++23/26, questo progetto compila con
  `-std=gnu++26`), l'intero calcolo di un orbitale può essere fatto
  eseguire dal compilatore ed essere incorporato come `.rodata` a tempo di
  compilazione invece che ricalcolato ogni boot — vedi `orbital_library.h`
  per il catalogo di orbitali "cablati" in questo modo.
- **Validazione incrociata a tre vie** (`tools/orbitals_host/`): C++
  (double e float32), MicroPython (unix port) e la libreria JS di
  riferimento vengono confrontati su 11 casi di test condivisi
  (`test_cases.csv`), sia per la funzione d'onda (coefficienti, tabella di
  Legendre, tabella radiale, campioni di ψ) sia per la nuvola di punti
  (§4bis) — tolleranza stretta (rtol=1e-9) tra le coppie in doppia
  precisione, informativa (rtol=2e-3) contro il float32. Eseguibile anche
  **su un ESP32-S3 reale** (`run_on_device.sh`): confermato float32 single
  precision sul dispositivo, 43/44 file funzione d'onda e 11/11 nuvola di
  punti entro tolleranza informativa.

## 4. Algoritmo: rejection sampling (versione originale, superata — vedi §4bis)

Indipendente da dove gira (Python offline o C on-device):

1. Scegliere n, l, m (o la combinazione reale desiderata).
2. Stimare `r_max` (vedi §2) e il valore massimo di |ψ|² nel dominio
   (analiticamente se possibile, altrimenti campionando una griglia grezza
   una tantum).
3. Loop di campionamento:
   - generare un punto candidato (x,y,z) uniforme in un cubo/sfera di
     raggio `r_max`
   - calcolare `p = |ψ(x,y,z)|²`
   - accettare il punto con probabilità `p / p_max`
     (estrarre `u` uniforme in [0,1], accettare se `u < p/p_max`)
4. Ripetere finché non si raggiunge il numero di punti target (10.000 nel
   progetto attuale).

Tasso di accettazione tipico per orbitali con nodi: 5-10%, quindi contare
~10-20 candidati generati per punto accettato.

### 4bis. Algoritmo attuale: campionamento esatto per CDF inversa (quantile), non rigetto

Il rejection sampling sopra è stato **sostituito** (due iterazioni
intermedie — rigetto 3D congiunto, poi rigetto separabile per asse — sono
conservate nella cronologia git, non nel codice attuale) da un
campionamento **esatto**, non un'approssimazione più veloce:

```
|ψ|²·r²·sin(θ) = [r·R(r)]² × [P_l^m(θ)²·sin(θ)] × azimutale(φ)²
                    solo r          solo θ            solo φ
```

la densità bersaglio si fattorizza in tre funzioni di una sola variabile
ciascuna (stessa fattorizzazione sfruttata anche dalla vecchia versione a
rigetto separabile) — campionare ogni fattore marginale indipendentemente
e comporre i risultati riproduce esattamente la densità congiunta. La
differenza: invece di tabulare solo il *bound* di ciascun fattore e poi
accettare/scartare candidati uno per uno, `initOrbitalSampler()`/
`init_orbital_sampler()` precalcola, per ciascun asse, l'intera **CDF
inversa** (la funzione "quantile → valore") con un'unica scansione in
avanti (`buildInverseCdf()`/`_build_inverse_cdf()`: la CDF è monotona, e
così il quantile bersaglio, quindi nessuna ricerca né in costruzione né
per campione). Campionare un punto costa poi esattamente **tre letture di
tabella interpolate** (`getValueFromLookupTable()`) più la trigonometria
per la conversione cartesiana — **zero rigetti, costo per punto costante
indipendentemente da (n,ℓ,m) e da quanto l'orbitale ha nodi**. Tecnica
generalizzata dal sampler GPU di
[stef1949/Electron-Orbital-Simulator](https://github.com/stef1949/Electron-Orbital-Simulator)
(che usa una tabella 2D congiunta θ×φ; qui, con poca RAM su ESP32 e la
fattorizzazione già sfruttata, tre tabelle 1D separate sono più economiche
ed equivalenti).

**Costo misurato su ESP32-S3 reale** (Waveshare ESP32-S3-LCD-1.3,
MicroPython 1.28.0, `tools/orbitals_host/run_on_device.sh`, caso
(n,ℓ,m)=(9,7,3), il più difficile del catalogo di test): rigetto congiunto
62.8 ms/punto → rigetto separabile 4.0 ms/punto → CDF inversa 0.5-0.7
ms/punto → CDF inversa + ottimizzazioni MicroPython (alias di modulo,
`array.array('d', ...)` invece di `list`, `@micropython.native`) 0.38
ms/punto — **~165× più veloce del rigetto congiunto originale**. La CDF
inversa aggiunge però un costo di inizializzazione per orbitale che il
rigetto non aveva in questa forma (costruire tre tabelle invertite):
~255-300ms per orbitale dopo le ottimizzazioni — comunque un costo fisso
per (n,ℓ,m), non per punto/frame, quindi compatibile con un cambio
orbitale a runtime scelto dall'utente (non serve rigenerare tutto
all'avvio). Numeri completi, metodologia dei sei confronti e note sulla
precisione float32 vs double in `tools/orbitals_host/README.md`.

Stesso PRNG portabile (`XorShift32`, xorshift a 32 bit, tripla di Marsaglia
(13,17,5)) mirrorato bit-per-bit in C++/MicroPython/JS: con lo stesso seme
le tre implementazioni producono la **stessa identica sequenza di punti**
(non solo la stessa statistica), il che rende la validazione incrociata un
confronto molto più severo di un istogramma — cattura anche un fattore
mancante nella densità che una forma complessiva simile potrebbe non
rivelare.

## 5. Dove gira: offline (Python) vs on-device (C++/MicroPython) — superato da §3bis/§4bis

Questa sezione descriveva la scelta originale: hardcodare a mano solo un
catalogo "da manuale" (1s...3d) on-device e delegare tutto il resto a uno
script Python offline con `scipy.special`. **Non è più così**: §3bis
(motore Legendre/Laguerre generale, `kOrbitalNMax = 16`) e §4bis
(campionamento a CDF inversa) girano **entrambi on-device**, in C++ e in
MicroPython, per l'intero catalogo n=1..16 supportato — nessuna
generazione offline necessaria per gli orbitali dell'idrogeno, nessun
export PROGMEM per-orbitale. I numeri di costo per punto e di
inizializzazione per orbitale sono in §4bis (misurati su ESP32-S3 reale).
La divisione offline/on-device resta corretta solo per il modello
**multi-elettronico** (Z>1, tabelle radiali tabulate da un risolutore
esterno, generazione offline obbligata) — vedi `ATOMS.md` §5, un problema
diverso da quello di questo documento.

Nota RNG (ancora valida): la generazione dei candidati usa `XorShift32`
(§4bis), non `esp_random()`/`rand()` — scelto deliberatamente per essere
portabile bit-per-bit tra C++/MicroPython/JS e quindi verificabile in
`tools/orbitals_host/`, non per qualità crittografica.

## 6. Precisione numerica

In unità di raggio di Bohr, `r` per questi orbitali resta nell'ordine di
poche decine al massimo anche per n moderati — float a precisione singola
è più che sufficiente ovunque in questo calcolo, nessun bisogno di double.

## 7. Riferimenti

Formule standard, reperibili su qualsiasi testo di meccanica quantistica
(es. Griffiths, *Introduction to Quantum Mechanics*, capitolo sull'atomo di
idrogeno) o sintetizzate su risorse come Wikipedia
("Hydrogen atom", "Table of spherical harmonics",
"Atomic orbital" per le forme reali p/d).

**Riferimento diretto dell'implementazione attuale (§3bis/§4bis):**
[Quantum Physics Online](https://www.quantum-physics.polytechnique.fr/)
(Manuel Joffre, École Polytechnique) — `orbitals.h`/`orbitals.py` sono un
porting funzione-per-funzione del suo `quantum-physics.js`; vedi
`examples/js-calculations/README.md` per dove recuperare l'originale (non
ridistribuito qui) e `tools/orbitals_host/README.md` per l'harness di
validazione incrociata C++/MicroPython/JS che lo usa come riferimento di
verità. La tecnica di campionamento per CDF inversa (§4bis) generalizza
quella del sampler GPU di
[stef1949/Electron-Orbital-Simulator](https://github.com/stef1949/Electron-Orbital-Simulator).
