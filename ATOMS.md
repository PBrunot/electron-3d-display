# ATOMS.md — Estensione a atomi multi-elettronici (PC, non ancora firmware)

Stato di avanzamento e note tecniche per riprendere la sessione. Riguarda
SOLO la visualizzazione approssimata di atomi con Z>1 (`pc/atom_main.py`),
costruita sopra la matematica idrogenoide già validata in `ORBITALI.md`.
Non ancora portata su ESP32/firmware — vedi §6.

## 0. Obiettivo e approccio scelto

Estendere la nuvola di punti (finora orbitali dell'idrogeno puri) a
qualsiasi atomo Z=1..118, riusando il più possibile la matematica
idrogenoide esistente (`orbitals.py`/`pointcloud.py`), con approssimazioni
esplicite e documentate invece di risolvere il vero problema
multi-elettronico (intrattabile analiticamente).

Modello a tre livelli, ciascuno un'approssimazione standard da manuale, non
qualcosa di specifico a questo progetto:

1. **Riempimento shell**: regola di Madelung (n+l) con le eccezioni reali
   note applicate — `slater.electron_configuration(z)` usa
   `slater._CONFIG_EXCEPTIONS` (Cr, Cu, Nb, Mo, Ru, Rh, Pd, Ag, Pt, Au e le
   anomalie La/Ce/Gd/Ac/Th/Pa/U/Np/Cm/Lr).
2. **Carica nucleare efficace**: per la sostituzione radiale `r → Z_eff·r` il
   modello usa `slater.z_eff_radial(z, config, n, ell)`: il valore
   **Clementi-Raimondi** (Hartree-Fock SCF, 1963/1967, tabella in
   `micropython/slater_cr_zeff.py`, copertura Z≤54) dove disponibile, altrimenti
   le regole di Slater (1930) riscalate di n/n* — la consistenza con il numero
   quantico efficace n* di Slater (`slater.n_star()`), che corregge la
   sovrastima sistematica dei raggi per n≥4 dell'uso naive di Z_eff con la
   funzione idrogenoide. Un solo Z_eff per sottoshell (non per singolo
   elettrone).
3. **Forma della densità**:
   - sottoshell **piena** → media sferica esatta (teorema di Unsöld) —
     `pointcloud.sample_isotropic_point()`.
   - sottoshell **parziale** → regola di Hund (`slater.hund_fill_m()`)
     assegna gli elettroni ai singoli orbitali reali (m = -l..l, un
     elettrone ciascuno prima di accoppiare), poi ciascuno campionato con
     lo stesso sampler angolare già usato per i preset dell'idrogeno
     (`pointcloud.sample_orbital_point()`, con Z_eff iniettato via
     sostituzione di variabile `r → Z_eff·r`, **non modificando**
     `orbitals.py`, che resta il modulo cross-validato con C++/JS).

Osservazione empirica interessante (chiesta esplicitamente dall'utente
durante la sessione): senza il passaggio Hund, TUTTE le shell esterne
apparivano sferiche anche quando non dovrebbero esserlo (es. carbonio
2p²) — la media sferica è esatta solo per shell piene. Dopo il fix, il
carbonio (2p² → occupa m=-1,0) mostra estensione |y|,|z| maggiore di |x|;
l'azoto (2p³, mezza shell, ogni orbitale singolarmente occupato) resta
quasi perfettamente sferico come previsto dalla fisica (Unsöld vale anche
per shell esattamente semi-piene con un elettrone per orbitale); il neon
(2p⁶, piena) è sferico esatto.

## 1. Mappa dei file

```
micropython/slater.py       Config. elettronica (Madelung + eccezioni reali,
                             _CONFIG_EXCEPTIONS), Z_eff (Clementi-Raimondi via
                             z_eff_cr()/z_eff()/z_eff_radial(), fallback
                             Slater con correzione n*, n_star()), regola di
                             Hund (hund_fill_m), tabella simboli Z=1..118.
micropython/slater_cr_zeff.py  Tabella Z_eff Clementi-Raimondi Z=1..54 per
                             sottoshell (dati, citati in testa al file).
micropython/pointcloud.py   + init_radial_sampler()/sample_isotropic_point()
                             (nuovo, sottoshell piene)
                             + z_eff opzionale in init_orbital_sampler()
                             + radial_mode_radius() (moda di r²R(r)², usata
                             dalla validazione — vedi pc/validate_atoms.py)
micropython/orbitals.py     NON MODIFICATO — lo Z_eff entra come
                             sostituzione di variabile al momento della
                             chiamata (r → Z_eff·r), non nella libreria
                             stessa.
micropython/atom_cloud.py   Orchestrazione: electron_configuration ->
                             gruppi di disegno (_drawing_groups, isotropo
                             vs Hund) -> point cloud unica. Anche:
                             ANGSTROM_PER_BOHR, scale_for_atom(),
                             PIXELS_PER_BOHR (calibrazione scala fissa).
pc/atom_view_pc.py           Viewer tkinter: Su/Giù cambia elemento (Z).
pc/atom_main.py              Entry point: python3 pc/atom_main.py [Z]
pc/orbital_view_pc.py        + draw_orbit_marker() e draw_scale_bar()
                             estratti come funzioni riusabili (refactor
                             minimo, comportamento demo idrogeno invariato)
```

## 2. Perché la scala della camera doveva essere fissa, non per-atomo

`cloud_common.scale_from_radii()` (usato dai preset idrogeno) rinormalizza
OGNI nuvola al **suo** raggio p90 → 100px fisso: bene per confrontare
orbitali diversi a schermo, ma per gli atomi **cancella** la differenza di
dimensione reale tra elementi (litio e uranio finirebbero sempre alla
stessa dimensione apparente).

Soluzione: `atom_cloud.scale_for_atom()` usa **`PIXELS_PER_BOHR`**, una
costante di conversione px/raggio-di-Bohr UGUALE per tutti gli elementi,
calibrata una sola volta all'import su **litio (Z=3)** — verificato
empiricamente essere l'atomo più diffuso in tutto l'intervallo Z=1..118 di
questo modello (raggio p90 ≈ 6.5 a₀ per il litio a count=2000/seed=SEED;
la contrazione n* dei pesanti in fallback Slater li tiene sotto il litio,
vedi `pc/validate_atoms.py`), coerente con la chimica reale (i metalli
alcalini sono i più diffusi/grandi nel loro periodo). Calibrando sul caso
peggiore, nessun elemento sfora il canvas 240×240 a riposo.

Aggiunta correlata: **barra di scala fisica** in basso a sinistra
(`orbital_view_pc.draw_scale_bar()`), in Ångström
(`atom_cloud.ANGSTROM_PER_BOHR = 0.529177210903`, CODATA), ricalcolata
OGNI frame dalla scala corrente (non quella a riposo) così segue
correttamente il respiro dello zoom e le escursioni — sceglie sempre una
lunghezza "tonda" (1/2/5 × potenza di dieci) che sta nel canvas.

## 3. Cosa NON fa (limiti espliciti, non bug)

- Nessun point-turnover/resample per la modalità atomo — la nuvola è
  statica dopo il caricamento (richiederebbe estendere `ResampleState` per
  gestire una miscela di più sottoshell/Z_eff diversi contemporaneamente).
- Nessuna colorazione di fase/segno — solo colore per shell (K/L/M/N...).
  I gruppi Hund (orbitali reali con m definito) avrebbero un segno reale
  disponibile (`orbitals.psi_real`), i gruppi isotropi no (la media
  angolare lo cancella) — non implementato per evitare uno schema di
  colore incoerente tra i due casi nello stesso atomo.
- Un solo Z_eff per sottoshell, non per singolo elettrone (approssimazione
  standard di Slater stesso).
- La tabella Clementi-Raimondi copre Z≤54 (fino a Xe): per Z>54 si torna
  alle regole di Slater + correzione n* (approssimazione peggiore, vedi §4.2).
- Nessun effetto relativistico (rilevante per elementi pesanti, vedi §4.2).

## 4. Validazione contro la letteratura (fatta in questa sessione)

### 4.1 Z_eff (regole di Slater) — corrispondenza esatta

Verificato contro esempi da manuale (Wikipedia "Slater's rules" e fonti
citate sotto): per il carbonio, elettrone 2p, Z_eff calcolato = **3.25**
(letteratura: 3.25); per il ferro, elettrone 3d, Z_eff = **6.25**
(letteratura: shielding = 0.35×5 + 1.00×18 = 19.75 → Z_eff = 26−19.75 =
6.25). Corrispondenza esatta — l'implementazione delle regole di shielding
(0.35/0.85/1.00, gruppo (n,l) sp/d/f, caso speciale 1s=0.30) è corretta.

### 4.1b Z_eff (tabella Clementi-Raimondi, ora il valore primario)

La tabella trascritta in `micropython/slater_cr_zeff.py` (Clementi &
Raimondi 1963 per Z≤36; Clementi, Raimondi & Reinhardt 1967 per Z=37..54,
come riportata nell'articolo Wikipedia "Effective nuclear charge" archiviato)
è verificata su punti noti: Li 2s = 1.279, C 2p = 3.136, Na 3s = 2.507,
Fe 3d = 11.180, Kr 4p = 9.338, e il fatto che Pd non abbia voce 5s (il suo
stato fondamentale è 4d¹⁰, coerente con l'eccezione di configurazione).
Nota: la voce Kr 2p della pagina archiviata riporta 26.047, che rompe la
sequenza monotona (Br 2p 31.056, Rb 2p 33.039); è un refuso di trascrizione
del 1963 originale (32.047), corretto e documentato nel file dati.

### 4.2 Raggio calcolato vs tabella Clementi-Raimondi — correzione metodologica e numeri nuovi (post-R5)

**Correzione metodologica (questa sessione):** la tabella precedente
confrontava la moda di r²R(r)² della sottoshell **più estesa** del modello
con i valori Clementi-Raimondi, che sono definiti sulla **sottoshell di
valenza** ("raggio di massima densità di carica nella shell più esterna" =
la sottoshell con l più alto tra quelle con n più alto). Le due quantità
NON coincidono: a parità di n una s (l=0) si estende più di una p
(per carbonio la 2s arriva a 85 pm mentre la 2p a 65 pm), quindi per
B..Ne la vecchia tabella confrontava la 2s del modello con il valore di
letteratura della 2p — da qui la "sovrastima sistematica del 22-28%" sul
periodo 2, che era in gran parte un artefatto di definizione, non un errore
delle costanti (verificato con `pc/validate_atoms.py`, che riporta per ogni
elemento anche il confronto "vecchio" più-esteso vs "corretto" di valenza).

Numeri attuali (modello con Z_eff Clementi-Raimondi + correzione n* sul
fallback Slater + eccezioni di configurazione; generati da
`python3 pc/validate_atoms.py`):

| Z  | Elemento | Valenza | Z_eff (fonte) | Modello (pm) | Letteratura (pm) | Rapporto |
|----|----------|---------|--------------:|-------------:|------------------:|---------:|
| 1  | H        | 1s      | 1.00 (CR)     |         52.9 |                 53 |     1.00 |
| 2  | He       | 1s      | 1.69 (CR)     |         31.3 |                 31 |     1.01 |
| 3  | Li       | 2s      | 1.28 (CR)     |        216.6 |                167 |     1.30 |
| 4  | Be       | 2s      | 1.91 (CR)     |        144.9 |                112 |     1.29 |
| 5  | B        | 2p      | 2.42 (CR)     |         87.4 |                 87 |     1.00 |
| 6  | C        | 2p      | 3.14 (CR)     |         67.5 |                 67 |     1.01 |
| 7  | N        | 2p      | 3.83 (CR)     |         55.2 |                 56 |     0.99 |
| 8  | O        | 2p      | 4.45 (CR)     |         47.5 |                 48 |     0.99 |
| 9  | F        | 2p      | 5.10 (CR)     |         41.5 |                 42 |     0.99 |
| 10 | Ne       | 2p      | 5.76 (CR)     |         36.8 |                 38 |     0.97 |
| 11 | Na       | 3s      | 2.51 (CR)     |        276.0 |                190 |     1.45 |
| 26 | Fe       | 4s      | 5.43 (CR)     |        239.7 |                156 |     1.54 |
| 36 | Kr       | 4p      | 9.34 (CR)     |        133.6 |                 88 |     1.52 |
| 55 | Cs       | 6s      | 3.14 (SL+n*)  |        994.0 |                298 |     3.34 |
| 79 | Au       | 6s      | 5.29 (SL+n*)  |        591.0 |                174 |     3.40 |
| 92 | U        | 7s      | 5.00 (SL+n*)  |        867.0 |                175 |     4.96 |

**Interpretazione (dopo R5):**

- **H, He: corrispondenza esatta** (nessuno shielding complesso in gioco)
  → conferma che le unità di base (a₀, conversione Å) sono corrette.
- **Periodo 2 blocco p (B-Ne): corrispondenza quasi esatta (0.97-1.01).**
  Con la definizione corretta (valenza vs valenza) le costanti di Slater
  bastavano già (~0.94-1.0); con la tabella Clementi-Raimondi il massimo
  scarto è ~3%. La vecchia sovrastima del 22-28% era l'artefatto di
  definizione descritto sopra.
- **Li, Be (2s) e periodi 3-4 (Na..Kr): sovrastima residua del 29-54%.**
  La causa NON è più Z_eff (per Li le costanti Slater e Clementi-Raimondi
  coincidono entro il 2%, eppure il raggio è ancora ~1.3×): è la **forma
  funzionale idrogenoide**. La funzione radiale Hartree-Fock reale è più
  contratta di qualunque idrogenoide con lo stesso Z_eff, perché deve
  essere ortogonale al core pieno (oscillazioni interne che spingono il
  massimo di r²R² verso l'interno); l'effetto cresce con n (periodo 4 ≈
  1.5×, periodo 3 ≈ 1.3×, periodo 2 ≈ 1.0×).
- **Z > 54 (fallback Slater + n*): Cs/Au/U ancora 3.3-5.0×.** La
  correzione n* recupera il 30% su Cs (1420 → 994 pm, vedi tabella
  vecchia) ma la forma idrogenoide + le costanti Slater grezze per i
  pesanti lasciano un residuo grande. La soluzione vera è un potenziale a
  schermo risolto numericamente (Numerov) — vedi §5.

**Nota metodologica (vale ancora, ora codificata):** la definizione
Clementi-Raimondi è la moda di UNA sola sottoshell, quella di valenza — non
la più estesa (per Fe la 3d riempie DOPO la 4s ma è spazialmente PIÙ
INTERNA: moda 3d = 43 pm vs moda 4s = 240 pm — fatto noto in chimica, è il
motivo per cui gli ioni dei metalli di transizione perdono prima gli
elettroni ns). Il valore `r_ref`/`base_scale` del viewer è il percentile 90
dell'INTERA nuvola mista — utile per la camera, non confrontabile con le
tabelle. La moda di r²R(r)² è ora una funzione di libreria
(`pointcloud.radial_mode_radius()`) e l'intero confronto è automatizzato in
`pc/validate_atoms.py` (tabella + statistiche + gate `--strict` + check
fisici: isotropia Unsöld su shell piene e semi-piene, anisotropia Hund su
2p², ordinamento radiale Fe 3d<4s, H/He esatti).

### Fonti

- [Slater's rules — Wikipedia](https://en.wikipedia.org/wiki/Slater%27s_rules)
  (formula, costanti di shielding, nota sul raffinamento Clementi, n*)
- Clementi, E.; Raimondi, D. L. (1963), "Atomic Screening Constants from
  SCF Functions", *J. Chem. Phys.* **38**, 2686 — pubblicazione originale
  dei raggi atomici calcolati e della tabella Z_eff per Z≤36
- Clementi, E.; Raimondi, D. L.; Reinhardt, W. P. (1967), "Atomic Screening
  Constants from SCF Functions. II", *J. Chem. Phys.* **47**, 1300 —
  tabella Z_eff per Z=37..54
- [Effective nuclear charge — Wikipedia (rev. archiviata)](https://en.wikipedia.org/w/index.php?title=Effective_nuclear_charge&oldid=712358437)
  (tabella Z_eff Clementi-Raimondi Z=1..54 trascritta in
  `micropython/slater_cr_zeff.py`; voce Kr 2p corretta da 26.047 a 32.047,
  refuso di trascrizione documentato nel file)
- [Atomic Radius (Calculated) — SchoolMyKids periodic table](https://www.schoolmykids.com/learn/periodic-table/atomic-radius-of-all-the-elements)
  (tabella numerica in pm trascritta in `pc/clementi_radii.py`)
- [WebElements — Atomic radii (Clementi)](https://winter.group.shef.ac.uk/webelements/periodicity/atomic_radius/)
  (conferma provenienza/definizione dei dati)

## 5. Accuratezza — fatto e da fare

Fatto in questa sessione (R1/R4/R5):

- **Eccezioni di configurazione** (R4): tabella `slater._CONFIG_EXCEPTIONS`
  per Cr, Cu, Nb, Mo, Ru, Rh, Pd, Ag, Pt, Au, La, Ce, Gd, Ac, Th, Pa, U,
  Np, Cm, Lr — corregge la forma qualitativa della shell esterna (es. Pd
  diventa 4d¹⁰ senza la 5s diffusa che Madelung sbagliato avrebbe creato).
- **Z_eff Clementi-Raimondi** (R5): tabella trascritta e verificata in
  `micropython/slater_cr_zeff.py` (Z≤54), usata da `slater.z_eff_radial()`;
  oltre Xe si torna a Slater. Impatto misurato: corregge il residuo del
  periodo 2 (già quasi esatto con la definizione giusta) e aiuta i periodi
  3-4 di qualche punto %, ma la sovrastima residua è dominata dalla forma
  idrogenoide (vedi §4.2), non dalle costanti.
- **Consistenza n*** (R5, resa come `n_star()`): `slater.n_star()` applicato SOLO al fallback
  Slater (il valore Clementi-Raimondi è per costruzione consistente con n,
  Z_eff = n·√(−2ε), quindi non va riscalato). Recupera il 30% su Cs 6s
  (1420 → 994 pm); i pesanti restano ~3-5×.
- **Harness di validazione** (R1): `pc/validate_atoms.py` + dati letteratura
  in `pc/clementi_radii.py` + `pointcloud.radial_mode_radius()`. La
  metodologia è ora quella corretta (valenza vs valenza) e il gate `--strict`
  protegge dalle regressioni.

Da fare (il vero salto di accuratezza):

- **Potenziale centrale a schermo + Numerov (R2, consigliato)**: sostituire
  l'idrogenoide a Z_eff costante con autofunzioni di V(r) = −Z_eff(r)/r
  (Z_eff(r) → Z per r→0, → valore asintotico per r→∞), risolte numericamente
  offline e tabulate come [rR]² — il sampler esistente non cambia. È l'unica
  strada che corregge davvero la sovrastima di Li/Be e dei periodi 3-4
  (1.3-1.5×), dei pesanti (3-5×) e che dà la coda asintotica giusta (carica
  +1), perché la forma radiale reale è più contratta dell'idrogenoide per
  ortogonalità al core.
- **Effetto relativistico (R3)** per Z≳55: contrazione di scala
  √(1−(Zα)²) (8% a Cs, 18% ad Au, 26% a U) sugli orbitali s/p — come
  correzione del potenziale se si fa R2, o come fattore empirico sul raggio
  se si resta idrogenoidi.

## 6. Prossimi passi

Accuratezza fisica (in ordine di impatto):

1. **Potenziale centrale a schermo + Numerov (R2)** — l'unico passo che
   rimuove davvero la sovrastima residua (1.3-1.5× periodi 3-4, 3-5×
   pesanti); il sampler non cambia, si tabula solo [rR]² diverso.
2. **Correzione relativistica (R3)** per Z≳55 (√(1−(Zα)²), 8-26%).
3. Estendere la tabella Z_eff Clementi-Raimondi oltre Z=54 se si trova la
   fonte (il paper 1967 copre fino a Z=86; la pagina archiviata usata si
   ferma a Xe).

Visivo/interattivo (invariato):

- Colorazione per fase/segno nei gruppi Hund (vedi §3).
- Point-turnover/shimmer per la modalità atomo (vedi §3).
- Eventuale porting firmware ESP32 (vedi CLAUDE.md §7 roadmap M4) — non
  iniziato, questa modalità è PC-only per ora (`pc/atom_view_pc.py`).

Ricordarsi di rieseguire `python3 pc/validate_atoms.py --strict` dopo
qualunque modifica alla matematica radiale o a `slater.py`.
