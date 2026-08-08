# ORBITALI.md — Note sul calcolo delle distribuzioni di probabilità

Riferimento tecnico per generare i punti della nuvola a partire dagli
orbitali dell'idrogeno. Riguarda sia lo script offline in Python sia
l'eventuale generazione diretta on-device in C++.

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

## 4. Algoritmo: rejection sampling

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

## 5. Dove farlo girare: offline (Python) vs on-device (C++)

**Offline (Python, script separato, non fa parte del firmware)**: obbligato
per qualunque P(x,y,z) arbitraria senza formula chiusa comoda, o per
orbitali con n alto dove i polinomi di Laguerre generali (fattoriali,
normalizzazione) sono più comodi da scrivere con `scipy.special`. Output:
array esportato come C/PROGMEM o file su TF card.

**On-device (C++, in `setup()` o al cambio orbitale)**: fattibile e
conveniente per il catalogo di orbitali "da manuale" (1s...3d) con le
formule chiuse del §2/§3 hardcoded. Vantaggi:
- nessun passaggio di export/flash per ogni nuovo orbitale, selezione a
  runtime
- costo stimato (Xtensa LX7 @ 240MHz, FPU, ~1-2 µs per punto candidato
  valutato, tasso di accettazione ~5-10%): per 10.000 punti accettati,
  10.000 × ~15 candidati × ~1.5 µs ≈ **200-250 ms**, una tantum
  all'avvio o al cambio orbitale — non tocca il budget del loop di
  rendering (che è un budget di tempo separato, vedi CLAUDE.md §6)

Usare `esp_random()` (RNG hardware) per i candidati uniformi e la soglia di
accettazione, non `rand()`.

Raccomandazione pratica: hardcodare on-device solo gli orbitali con formula
chiusa semplice (§2/§3, fino a n=3 circa). Per n più alti o casi meno comuni,
tornare al percorso offline Python + Laguerre generale via scipy — evita di
dover implementare fattoriali/ricorrenze di Laguerre generali in C per un
caso limite.

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
