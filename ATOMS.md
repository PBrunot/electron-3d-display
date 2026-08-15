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

1. **Riempimento shell**: regola di Madelung (n+l) — `slater.electron_configuration(z)`.
   Ignora le eccezioni reali note (Cr, Cu, Nb, Au, ... dove un d "ruba" un
   elettrone da ns) — vedi §4.3 per l'impatto misurato su Au.
2. **Carica nucleare efficace**: regole di Slater (1930) —
   `slater.slater_z_eff(z, config, n, ell)`. Un solo Z_eff per sottoshell
   (non per singolo elettrone).
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
micropython/slater.py       Config. elettronica (Madelung), Z_eff (Slater),
                             regola di Hund (hund_fill_m), tabella simboli
                             elemento Z=1..118. Nessuna dipendenza hardware.
micropython/pointcloud.py   + init_radial_sampler()/sample_isotropic_point()
                             (nuovo, sottoshell piene)
                             + z_eff opzionale in init_orbital_sampler()
                             (nuovo parametro, default 1.0 = comportamento
                             invariato per i preset idrogeno esistenti)
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
questo modello (raggio p90 da 1.37 a₀ per il neon a 5.47 a₀ per il litio,
~4×), coerente con la chimica reale (i metalli alcalini sono i più
diffusi/grandi nel loro periodo). Calibrando sul caso peggiore, nessun
elemento sfora il canvas 240×240 a riposo.

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
- Nessun effetto relativistico (rilevante per elementi pesanti, vedi §4.3).
- Nessuna delle eccezioni reali di riempimento (Cr, Cu, Nb, Mo, Ru, Rh, Pd,
  Ag, Pt, Au, ...) — regola di Madelung pura.

## 4. Validazione contro la letteratura (fatta in questa sessione)

### 4.1 Z_eff (regole di Slater) — corrispondenza esatta

Verificato contro esempi da manuale (Wikipedia "Slater's rules" e fonti
citate sotto): per il carbonio, elettrone 2p, Z_eff calcolato = **3.25**
(letteratura: 3.25); per il ferro, elettrone 3d, Z_eff = **6.25**
(letteratura: shielding = 0.35×5 + 1.00×18 = 19.75 → Z_eff = 26−19.75 =
6.25). Corrispondenza esatta — l'implementazione delle regole di shielding
(0.35/0.85/1.00, gruppo (n,l) sp/d/f, caso speciale 1s=0.30) è corretta.

### 4.2 Raggio calcolato vs tabella Clementi-Raimondi (1963) — H/He esatti, poi deviazione sistematica

Confronto tra il **raggio di massima probabilità** (moda di r²R(r)², NON il
percentile 90 misto multi-shell `r_ref` usato per il rendering — sono
quantità diverse, vedi nota metodologica sotto) della sottoshell più
estesa di ogni atomo, e i valori pubblicati (Clementi, Raimondi, Reinhardt,
*J. Chem. Phys.* 38, 2686 (1963) — "raggio di massima densità di carica
nella shell più esterna", la stessa identica definizione fisica):

| Z  | Elemento | Modello (pm) | Letteratura (pm) | Rapporto |
|----|----------|-------------:|------------------:|---------:|
| 1  | H        |         53.0 |                 53 |     1.00 |
| 2  | He       |         31.2 |                 31 |     1.00 |
| 3  | Li       |        213.0 |                167 |     1.28 |
| 4  | Be       |        142.0 |                112 |     1.27 |
| 5  | B        |        106.5 |                 87 |     1.22 |
| 6  | C        |         85.2 |                 67 |     1.27 |
| 7  | N        |         71.0 |                 56 |     1.27 |
| 8  | O        |         60.9 |                 48 |     1.27 |
| 9  | F        |         53.3 |                 42 |     1.27 |
| 10 | Ne       |         47.3 |                 38 |     1.25 |
| 11 | Na       |        314.4 |                190 |     1.65 |
| 26 | Fe       |        347.2 |                156 |     2.23 |
| 36 | Kr       |        157.8 |                 88 |     1.79 |
| 55 | Cs       |       1420.0 |                298 |     4.77 |
| 79 | Au       |        743.8 |                174 |     4.27 |

**Interpretazione:**

- **H, He: corrispondenza esatta** (nessuno shielding complesso in gioco)
  → conferma che le unità di base (a₀, conversione Å) sono corrette.
- **Periodo 2 (Li-Ne): sovrastima sistematica e stabile del ~22-28%.**
  Non è rumore/bug: è la firma nota dell'uso delle costanti di shielding
  ORIGINALI di Slater (1930), che sono un'approssimazione grossolana —
  è esplicitamente per questo che Clementi & Raimondi hanno pubblicato nel
  1963 un set di Z_eff *raffinato* via calcoli Hartree-Fock SCF (fonte:
  pagina Wikipedia "Slater's rules", che cita esplicitamente questo
  raffinamento).
- **Elementi più pesanti/transizione (Na, Fe, Kr, Cs, Au): deviazione
  molto più grande** (65% - 377%). Tre cause concorrenti, non un errore
  singolo:
  1. le regole di Slater sono documentate come più accurate per elementi
     leggeri del blocco s/p; degradano su d/f e Z alti;
  2. **nessun effetto relativistico** nel modello — per Cs/Au questo è
     fisicamente rilevante (la contrazione relativistica degli orbitali s
     è il motivo da manuale per cui l'oro ha proprietà anomale);
  3. Slater stesso definisce un numero quantico principale "effettivo"
     n* (3.7 per n=4, 4.0 per n=5, 4.2 per n=6) da usare quando si
     costruisce un orbitale di tipo Slater (STO, forma esponenziale senza
     nodi) — **non applicato qui** perché usiamo la funzione radiale
     idrogenoide VERA (con nodi, via `orbitals.py`), per cui n* non è
     direttamente traducibile (è stato inventato per compensare la forma
     funzionale semplificata delle STO, non è una correzione universale).
  Per Au specificamente si aggiunge la config. reale nota come eccezione
  (5d¹⁰6s¹, non 5d⁹6s² come darebbe Madelung puro) — vedi §3.

**Nota metodologica importante per confronti futuri**: la definizione
Clementi-Raimondi è "raggio di massima densità di carica nella shell più
esterna" — cioè la moda di UNA sola sottoshell (quella con l'estensione
maggiore, non necessariamente l'ultima riempita in ordine di Madelung: per
Fe la 3d riempie DOPO la 4s ma è spazialmente PIÙ INTERNA — fatto noto in
chimica, è il motivo per cui gli ioni dei metalli di transizione perdono
prima gli elettroni ns). Il valore `r_ref`/`base_scale` usato dal viewer
(`atom_cloud.scale_for_atom`) è invece il percentile 90 dell'INTERA nuvola
mista multi-shell — utile per inquadrare la camera, ma **non** direttamente
confrontabile con le tabelle di letteratura. Per validare, ricalcolare
sempre la moda della singola sottoshell più estesa (script usato per la
tabella sopra, non ancora salvato come funzione di libreria — vedi §6).

### Fonti

- [Slater's rules — Wikipedia](https://en.wikipedia.org/wiki/Slater%27s_rules)
  (formula, costanti di shielding, nota sul raffinamento Clementi, n*)
- Clementi, E.; Raimondi, D. L.; Reinhardt, W. P. (1963), *J. Chem. Phys.*
  **38**, 2686 — pubblicazione originale dei raggi atomici calcolati
  (citata da WebElements, non letta in originale in questa sessione)
- [Atomic Radius (Calculated) — SchoolMyKids periodic table](https://www.schoolmykids.com/learn/periodic-table/atomic-radius-of-all-the-elements)
  (tabella numerica usata per il confronto in pm)
- [WebElements — Atomic radii (Clementi)](https://winter.group.shef.ac.uk/webelements/periodicity/atomic_radius/)
  (conferma provenienza/definizione dei dati, nessun valore numerico
  estratto direttamente da qui)

## 5. Idee per migliorare l'accuratezza (non fatto, da valutare)

- Sostituire le costanti di shielding di Slater (1930) con quelle
  raffinate di Clementi-Raimondi (1963) per Z_eff — dovrebbe correggere
  gran parte della sovrastima sistematica del ~25% sul periodo 2 già
  misurata. Richiede reperire/trascrivere la tabella dei coefficienti
  raffinati (non ancora cercata in questa sessione).
- Valutare se applicare una contrazione empirica per n≥4 (ispirata a n*,
  ma adattata alla funzione radiale idrogenoide vera invece che alle STO)
  per ridurre la sovrastima su elementi pesanti — euristica, da tarare
  contro più punti dati prima di fidarsene.
- Aggiungere una funzione di libreria dedicata per il "raggio di massima
  probabilità" (moda di r²R(r)²) in `atom_cloud.py` o `pointcloud.py`, così
  la validazione contro la letteratura non richiede più uno script usa e
  getta come quello di questa sessione.

## 6. Prossimi passi

- Colorazione per fase/segno nei gruppi Hund (vedi §3) — miglioria visiva,
  non richiesta esplicitamente finora.
- Point-turnover/shimmer per la modalità atomo (vedi §3).
- Eventuale porting firmware ESP32 (vedi CLAUDE.md §7 roadmap M4) — non
  iniziato, questa modalità è PC-only per ora (`pc/atom_view_pc.py`).
- Se si vuole più accuratezza fisica: vedi §5.
