# ATOMS.md — Estensione a atomi multi-elettronici (PC, web e firmware ESP32)

Stato di avanzamento e note tecniche per riprendere la sessione. Riguarda
la visualizzazione approssimata di atomi con Z>1, costruita sopra la
matematica idrogenoide già validata in `ORBITALI.md`.

**Aggiornamento (agosto 2026):** questo documento descrive **due** modelli
che coesistono nel codice:

1. Il modello "storico" §0-§4 sotto — idrogenoide con carica nucleare
   efficace (Clementi-Raimondi/Slater), usato per la modalità `hydrogenic`
   e come **fallback per Z>92** (dove la tabella §5 non copre).
2. Il modello di default attuale — funzioni radiali **tabulate** da un
   risolutore Kohn-Sham a tutti gli elettroni (SPARC-atomSFE), validate
   contro NIST, **portate su PC, web e firmware ESP32 (C++ e MicroPython)**.
   Vedi §5 (che sostituisce/aggiorna la vecchia sezione "Accuratezza") e i
   documenti di dettaglio in inglese `pc/screened_potential_model.md`
   (design/stato) e `pc/RUN_HFS.md` (runbook) — non duplicati qui.

Il porting firmware, descritto come lavoro futuro nella versione precedente
di questo file, **è fatto** (§5.4).

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

Modello storico (idrogenoide + Z_eff, invariato da questa sessione):

```
micropython/slater.py       Config. elettronica (Madelung + eccezioni reali,
                             _CONFIG_EXCEPTIONS), Z_eff (Clementi-Raimondi via
                             z_eff_cr()/z_eff()/z_eff_radial(), fallback
                             Slater con correzione n*, n_star()), regola di
                             Hund (hund_fill_m), tabella simboli Z=1..118.
micropython/slater_cr_zeff.py  Tabella Z_eff Clementi-Raimondi Z=1..54 per
                             sottoshell (dati, citati in testa al file).
micropython/pointcloud.py   init_radial_sampler()/sample_isotropic_point()
                             (sottoshell piene), z_eff opzionale in
                             init_orbital_sampler(), radial_mode_radius()
                             (moda di r²R(r)², usata da pc/validate_atoms.py);
                             + interp_u()/_build_inverse_cdf_from_grid() e
                             radial_fn opzionale, usati dal path a tabella §5.
micropython/orbitals.py     Motore d'onda idrogenoide, condiviso col catalogo
                             puro dell'idrogeno — vedi ORBITALI.md.
micropython/atom_cloud.py   Orchestrazione: electron_configuration ->
                             gruppi di disegno (_drawing_groups, isotropo
                             vs Hund) -> point cloud unica; accetta
                             radial_tables= opzionale (§5) per sostituire la
                             sorgente radiale idrogenoide con quella tabulata.
                             Anche: ANGSTROM_PER_BOHR, scale_for_atom(),
                             PIXELS_PER_BOHR (calibrazione scala fissa),
                             kAtomShellRgb (colori per shell K/L/M/N...).
pc/atom_view_pc.py           Viewer tkinter: Su/Giù cambia elemento (Z),
                             --model hydrogenic|hfs (hfs = tabelle §5).
pc/atom_main.py              Entry point: python3 pc/atom_main.py [Z] [--model hfs]
pc/atom_dissection_common.py Piano di dissezione a sottoshell (Fase 0-5,
                             scala/clip/timing) condiviso tra pc/atom_view_pc.py
                             e web/py/web_atom.py. Esplicitamente NON copre
                             micropython/atom_view.py: il dispositivo non ha
                             la dissezione.
pc/orbital_view_pc.py        draw_orbit_marker() e draw_scale_bar() riusabili.
```

Modello a tabelle radiali (§5, default attuale) e porting firmware:

```
pc/hfs_atomsfe.py           Genera le tabelle radiali via SPARC-atomSFE
                             (solver Kohn-Sham a tutti gli elettroni, LDA_SVWN,
                             vendorizzato in pc/_atomsfe_vendor/). Sorgente di
                             default di pc/hfs_tables.npz (Z=1..92).
pc/hfs_solver.py             Vecchio risolutore HFS scritto in casa (potenziale
pc/dirac_solver.py           a schermo + Numerov/ARPACK, con passata
                             relativistica di Dirac per Z≥55) — SUPERATO dal
                             modello atomSFE, mantenuto per confronto
                             (pc/compare_old_vs_atomsfe.py) e per rigenerare
                             tabelle Z=1..118 (pc/RUN_HFS.md §2), scope non
                             più usato di default da nessun consumer.
pc/hfs_tables.py             Lettore npz (DEFAULT_TABLES = hfs_tables.npz).
pc/hfs_tables.npz            Tabella di default in uso (= hfs_tables_atomsfe.npz,
                             Z=1..92). hfs_tables_reduced.npz: compattazione a
                             128 punti/sottoshell (formato per il device).
                             "hfs_tables - Copia.npz": backup vecchio Z=1..118.
pc/nist_compare_atomsfe.py   Validazione primaria: autovalori vs NIST
                             dftdata (examples/nis data/dftdata) — vedi §5.2.
pc/nist_compare.py           Vecchia validazione (risolutore in-repo vs NIST,
                             α=2/3) — superata, mantenuta per confronto.
pc/validate_atoms.py         Invariato nel ruolo: --model hfs --strict --all.
pc/ionization_energy.py      Tabella energie di prima ionizzazione NIST SRD 111
                             (Z=1..92), usata da pc/plot_atomic_radii.py per il
                             pannello di confronto Koopmans vs sperimentale.
pc/screened_potential_model.md  Documento di design/stato (inglese) — dettaglio
pc/RUN_HFS.md                completo del modello a tabelle e runbook per
                             rigenerarle; non duplicato qui, vedi §5.

tools/hfs_table_gen.py      Impacchetta la npz ridotta in un blob binario
                             (griglia r condivisa + indice per Z + indice per
                             sottoshell (n,ell) + righe u(r)).
tools/atom_size_calib_gen.py  Genera 3 file di calibrazione scala: C++
                             (src/physics/atom_size_calib.h, a tabella),
                             MicroPython a tabella (micropython/hfs_atom_size_calib.py,
                             usato solo da atom_view.py) e MicroPython idrogenoide
                             (micropython/atom_size_calib.py, invariato — usato
                             da pc/atom_view_pc.py --model hydrogenic e dal
                             fallback di dissezione web).

data/hfs_tables.bin          Blob dati flashato sulla partizione SPIFFS
                             "storage" (partitions_16M.csv) via
                             `pio run -t uploadfs` — passo SEPARATO da
                             `pio run -t upload`. Il firmware NON lo include
                             come .rodata: viene letto on-demand da flash.
src/physics/hfs_radial.h/.cpp  Caricatore/campionatore delle tabelle da
                             flash: hfsInit() monta /storage in modo
                             idempotente; hfsFindU() ritorna la riga o
                             nullptr (Z>92 o partizione non montata) — nullptr
                             fa scattare il fallback idrogenoide, mai un crash.
src/physics/hfs_tables.h     Generato: solo 3 costanti di dimensione
                             (kHfsGridSize/kHfsElementCount/kHfsSubshellCount).
src/physics/pointcloud.h     + buildInverseCdfFromGrid()/interpOnGrid() per
                             costruire la CDF inversa da una griglia arbitraria
                             (le tabelle §5), oltre al path idrogenoide invariato.
src/physics/atom_cloud.h/.cpp  Seleziona la sorgente radiale per (Z,n,l):
                             tabulata se Z≤92, altrimenti idrogenoide.
src/views/atom_view.cpp/.h   Viewer atomo sul dispositivo (chooser/galleria,
                             Su/Giù cambia Z) — nessuna dissezione (vedi sopra).

micropython/hfs_radial_tables.py  Lettore da flash on-demand (header/indice
                             residenti, righe via open()/seek()).
micropython/hfs_tables.bin   File dati device (MicroPython), deployato con
                             `mpremote fs cp`.
micropython/atom_view.py     Entry point viewer device (MicroPython), passa
                             radial_tables= a build_atom_point_cloud().
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

## 5. Modello radiale attuale: da HFS in-repo a SPARC-atomSFE (default)

Il risolutore HFS "fatto in casa" descritto nella cronologia sopra (R1-R5,
§4) **non è più la sorgente di default** delle tabelle radiali. È stato
sostituito da **SPARC-atomSFE** (github.com/SPARC-X/SPARC-atomSFE,
vendorizzato in `pc/_atomsfe_vendor/`), un risolutore Kohn-Sham a tutti gli
elettroni con base a elementi finiti spettrali, funzionale LDA_SVWN — un
codice esterno validato dalla sua comunità, non più matematica ricostruita
a mano in questo repo. Dettaglio completo (design, derivazione, storia del
bug SCF) in `pc/screened_potential_model.md`; runbook di rigenerazione in
`pc/RUN_HFS.md`. Qui solo lo stato riassunto.

### 5.1 Copertura e ruolo dei due modelli

- **Modello a tabelle (atomSFE), default**: `pc/hfs_atomsfe.py` genera
  `pc/hfs_tables.npz` (= `pc/hfs_tables_atomsfe.npz`) per **Z=1..92**, non
  relativistico. È la sorgente usata di default da `pc/atom_view_pc.py`,
  dal viewer web e dal firmware ESP32 (§5.4).
- **Vecchio risolutore in-repo** (`pc/hfs_solver.py` + `pc/dirac_solver.py`,
  potenziale a schermo + Numerov/ARPACK, passata relativistica di Dirac per
  Z≥55, descritto in dettaglio in §4 sopra — R1-R5): **superato**,
  mantenuto solo per confronto (`pc/compare_old_vs_atomsfe.py`) e perché
  può ancora generare tabelle Z=1..118 (`pc/RUN_HFS.md` §2, →
  `pc/hfs_tables - Copia.npz`) — oggi non consumato da nessun viewer di
  default. I numeri R1-R5 sopra restano corretti come storia di quel
  modello, ma descrivono un percorso ormai secondario.
- **Fallback idrogenoide** (§0-§4.2): usato per Z>92 (oltre lo scope di
  atomSFE) e nella modalità esplicita `--model hydrogenic`.

### 5.2 Validazione NIST — fatta, con numeri concreti

Da `README.md` ("How we know the math is right"): il modello atomSFE
riproduce gli autovalori LDA del *NIST Atomic Reference Data for Electronic
Structure Calculations* (Kotochigova et al., 1997) entro **≤7×10⁻⁶ Ha su
tutte le 915 sottoshell di Z=1..92**, e le configurazioni elettroniche di
stato fondamentale coincidono con NIST **92/92**
(`pc/nist_compare_atomsfe.py`, confronto contro l'archivio NIST in
`examples/nis data/dftdata`, tolleranza `--tol 2e-5` Ha/sottoshell). Questo
sostituisce/supera la vecchia validazione (`pc/nist_compare.py`, risolutore
in-repo ad α=2/3, accordo ~1 eV, numeri riportati sopra) che resta nel repo
solo per confronto storico.

Nota fisica: gli orbitali di valenza LDA sono più diffusi del riferimento
Hartree-Fock (Clementi-Raimondi) per errore di self-interazione — la
struttura interna delle shell è quindi NIST-esatta, ma la **dimensione
resa a schermo** è ricalibrata per elemento sul raggio di letteratura
Clementi-Raimondi (`pc/validate_atoms.py --model hfs --strict --all`,
`tools/atom_size_calib_gen.py`), non presa grezza dal modello LDA.

Il bug SCF di collasso ns→d dei metalli di transizione (warm-start sigma
di ARPACK, cronologia sopra) è ora raccontato in
`pc/screened_potential_model.md` §5, citato dal README come "a real bug
caught by cross-checking against NIST eigenvalues" — riguarda solo il
vecchio risolutore in-repo, non atomSFE.

### 5.3 Pannello energie di ionizzazione (non un'interfaccia device)

`pc/ionization_energy.py` aggiunge la tabella NIST SRD 111 delle energie di
prima ionizzazione (Z=1..92, fonte Kramida/Ralchenko/Reader/NIST ASD, citata
in testa al file); `pc/plot_atomic_radii.py` la usa per un secondo pannello
matplotlib che confronta gli autovalori di valenza HFS/atomSFE (teorema di
Koopmans) con le IP sperimentali. È uno strumento di analisi/plotting
offline, non una schermata sul dispositivo, nonostante il nome del commit
che lo ha introdotto ("Add ionization energy panel").

### 5.4 Porting ESP32 — fatto (agosto 2026), C++ e MicroPython

Contrariamente a quanto diceva la versione precedente di questo documento
(porting ESP32 descritto come "obiettivo finale"), il porting firmware **è
completo**, su entrambe le vie:

- **Formato dati**: `tools/hfs_table_gen.py` compatta la npz ridotta
  (128 punti/sottoshell, `pc/hfs_tables_reduced.npz`) in un blob binario
  little-endian (griglia `r` condivisa + indice per Z + indice per
  sottoshell (n,ℓ) + righe `u(r)`).
- **C++/ESP-IDF**: il blob (`data/hfs_tables.bin`) va sulla partizione
  SPIFFS `storage` (`partitions_16M.csv`) via `pio run -t uploadfs` — **un
  passo separato** da `pio run -t upload` (facile da dimenticare). Letto
  on-demand da `src/physics/hfs_radial.h/.cpp`: `hfsInit()` monta
  `/storage` in modo idempotente, `hfsFindU()` ritorna la riga o `nullptr`
  (Z>92, o partizione non montata) — `nullptr` fa scattare il fallback
  idrogenoide, **mai un crash**: una scheda su cui non è stato ancora
  lanciato `uploadfs` continua a funzionare con i raggi idrogenoidi vecchi.
  `src/physics/pointcloud.h` guadagna `buildInverseCdfFromGrid()` +
  `interpOnGrid()` per costruire la CDF inversa da una griglia arbitraria
  (non equispaziata); `src/physics/atom_cloud.cpp` sceglie la sorgente
  radiale per (Z,n,ℓ). Il firmware **non include i dati come .rodata
  compilato**: un'iterazione precedente lo faceva (~470KB), ora sono letti
  da flash SPIFFS (partizione da 7MB, poco altro la usa).
- **MicroPython**: stesso blob (`micropython/hfs_tables.bin`, deployato via
  `mpremote fs cp`), letto da `micropython/hfs_radial_tables.py` (header/
  indice residenti in RAM, righe via `open()`/`seek()`);
  `micropython/atom_view.py` lo passa come `radial_tables=` a
  `build_atom_point_cloud()`.
- **Calibrazione scala**: `tools/atom_size_calib_gen.py` genera ora *tre*
  file — `src/physics/atom_size_calib.h` (C++, a tabella),
  `micropython/hfs_atom_size_calib.py` (MicroPython, a tabella, solo per
  `atom_view.py`) e l'originale `micropython/atom_size_calib.py` resta
  idrogenoide (condiviso da `pc/atom_view_pc.py --model hydrogenic` e dal
  fallback di dissezione web).
- **Validazione incrociata**: C++ (float64) vs NumPy (CDF trapezoidale)
  concordano a ~1e-8 relativo (casi Fe 4s, U 1s); pipeline MicroPython
  verificata byte-esatta sotto interprete MicroPython 1.17 unix-port reale.
- **Viewer device**: `src/views/atom_view.cpp/.h` — galleria/chooser con
  Z + configurazione elettronica, Su/Giù cambia elemento. **Nessuna
  dissezione a sottoshell sul device** (quella resta PC/web,
  `pc/atom_dissection_common.py`, la cui docstring dice esplicitamente che
  non copre `micropython/atom_view.py`).
- **Non ancora fatto**: benchmark/screenshot A/B su hardware reale vs PC
  (nessuna misura FPS sul device per questo path specifico — nessuna
  regressione attesa: il costo per punto resta un'interpolazione + una
  lettura di CDF inversa, uguale al path idrogenoide, più una singola
  lettura file per sottoshell al cambio elemento, non per frame/per punto).

## 6. Prossimi passi

1. **Estendere Z_eff Clementi-Raimondi oltre Z=54**, se si trova la fonte
   (il paper 1967 copre fino a Z=86) — non critico: il modello a tabelle
   (§5) non dipende da Z_eff per Z>54, riguarda solo il fallback idrogenoide.
2. **Benchmark su hardware reale** del path a tabelle (§5.4, ultimo punto):
   misura FPS/tempo di cambio-elemento su ESP32-S3 fisico, non solo stimato.
3. Valutare se estendere la copertura atomSFE oltre Z=92 (oggi limite duro
   della libreria/scelta di scope) o se lasciare il fallback idrogenoide per
   quel range, dato l'errore comunque grande atteso lì (§4.2).

Visivo/interattivo (invariato dalla versione precedente):

- Colorazione per fase/segno nei gruppi Hund (vedi §3) — non implementata.
- Point-turnover/shimmer per la modalità atomo (vedi §3) — non implementato.

Ricordarsi di rieseguire `python3 pc/validate_atoms.py --strict` dopo
qualunque modifica alla matematica radiale o a `slater.py` (e
`python3 pc/validate_atoms.py --model hfs --strict --all` per il modello a
tabelle §5). Per il dettaglio fisico/numerico completo del modello a
tabelle, vedi `pc/screened_potential_model.md` e `pc/RUN_HFS.md` — questo
file resta la nota di orientamento in italiano, non la fonte primaria per
quella parte.
