# orbitals_host — cross-check tra i porting C++/MicroPython e il riferimento JS

Confronta due implementazioni candidate della matematica degli orbitali
dell'idrogeno **e** del campionamento a nuvola di punti (M2, CLAUDE.md §5/§7)
contro `js_reference.js` (l'estrazione/estensione di
`examples/js-calculations/quantum-physics.js`, di Manuel Joffre):

- `src/orbitals.h/.cpp` (funzione d'onda) + `src/pointcloud.h/.cpp`
  (campionamento della nuvola di punti via CDF inversa) — porting C++,
  pensato per compilare sia qui sul PC sia dentro PlatformIO/ESP-IDF per
  l'ESP32.
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

`sampleOrbitalPoint()`/`sample_orbital_point()` campiona (r,θ,φ) dalla densità
di probabilità fisica |ψ|²·r²·sin(θ) (il fattore r²sin(θ) è l'elemento di
volume in coordinate sferiche — senza non si campionerebbe la probabilità
*fisica*, solo dove |ψ| è grande). Invece di confrontare le tre
implementazioni solo statisticamente (istogrammi, ecc.), tutte e tre usano lo
**stesso generatore pseudocasuale portabile** (`XorShift32`, il triplo
(13,17,5) di Marsaglia, mai gli shift/xor di libreria del linguaggio) con lo
stesso seme e lo stesso ordine di estrazione. Risultato: dato lo stesso seme,
le tre implementazioni producono la **stessa identica sequenza di punti**
(a meno dell'arrotondamento macchina in double, o di uno scostamento maggiore
atteso in float32 — vedi passate 3/4/6 sotto). Questo è un confronto molto
più severo di un confronto statistico: cattura anche bug sottili (es. un
fattore mancante nella densità) che uno scostamento nella sola forma
complessiva della nuvola potrebbe non rivelare.

**r, θ, φ sono campionati da tre tabelle di CDF inversa (quantile)
precalcolate**, non per rigetto. Due versioni precedenti — rigetto 3D
congiunto, poi rigetto separabile per asse — sono conservate nella cronologia
git; questa è la terza iterazione, motivata dal bisogno di generare molti più
punti di quanto il rigetto (anche separabile) permettesse in tempi
ragionevoli su MicroPython/ESP32. È ancora esatta, non un'approssimazione: la
densità bersaglio si fattorizza come

```
|ψ|²·r²·sin(θ)  =  [r·R(r)]²  ×  [P_l^m(θ)²·sin(θ)]  ×  azimuthal(φ)²
                      solo r         solo θ              solo φ
```

cioè un prodotto di tre funzioni di una sola variabile ciascuna — campionare
ogni fattore marginale indipendentemente e combinare i risultati riproduce
esattamente la densità congiunta (stessa fattorizzazione già sfruttata dalla
versione a rigetto separabile). La differenza rispetto al rigetto: invece di
tabulare solo il *bound* di ciascun fattore e poi accettare/scartare
candidati uno per uno, `initOrbitalSampler()`/`init_orbital_sampler()`
precalcola, per ciascun asse, l'intera **CDF inversa** — la funzione
"quantile → valore" — con una singola scansione in avanti (la CDF è
monotona, e così il quantile bersaglio, quindi non serve alcuna ricerca né a
tempo di costruzione né campione per campione). Campionare un punto costa
allora esattamente **tre letture di tabella interpolate** (`getValueFromLookupTable`/
`get_value_from_lookup_table`, già usata altrove nel codebase) più la
trigonometria per la conversione cartesiana — **zero rigetti, zero varianza
sul costo per punto, tempo per punto costante indipendentemente da (n,ℓ,m)**.

Questa tecnica — precalcolare la funzione inversa stessa anziché solo la CDF
— è la stessa usata dal sampler GPU di
[stef1949/Electron-Orbital-Simulator](https://github.com/stef1949/Electron-Orbital-Simulator/tree/main/src/sampling)
(`sampling/lut.js`), generalizzata qui anche a φ (loro costruiscono una
tabella 2D congiunta θ×φ, che sulla GPU con VRAM abbondante è preferibile per
generalità; qui, con memoria limitata su ESP32 e la fattorizzazione già
sfruttata, tre tabelle 1D separate sono più economiche ed equivalenti).
Per m=0 il fattore azimutale è costante (1): la CDF di φ degenera
automaticamente in una rampa lineare (φ uniforme), senza bisogno di un caso
speciale nel codice. Vedi "Prestazioni" più sotto per i numeri misurati.

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
   stesso seme + stesse tabelle di CDF inversa (costruite in double) ⇒
   stessi tre valori interpolati per ogni punto. Fa parte del codice di uscita.
4. **vs `out/points_c_f32`, tolleranza larga (rtol=2e-3)** — informativa: le
   tabelle di CDF inversa sono costruite sommando ~1001 termini in float32
   (somma cumulativa), che accumula più errore di arrotondamento di una
   singola valutazione puntuale — da qui uno scostamento maggiore (ma ancora
   ampiamente entro tolleranza) rispetto alle versioni precedenti a rigetto.
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
- **Prestazioni**: costruire una tabella da 1001 punti richiede ~70-80ms. Il
  campionamento dei punti è passato per quattro iterazioni — rigetto 3D
  congiunto, rigetto separabile per asse, CDF inversa precalcolata, poi CDF
  inversa + ottimizzazioni a livello Python (vedi sezione dedicata sotto) —
  misurate **sulla stessa scheda fisica** a ogni passaggio:

  | caso (n,ℓ,m) | congiunto | separabile | CDF inversa | CDF + ottimizzato | congiunto→finale |
  |---|---|---|---|---|---|
  | 9,7,3 (il più difficile) | 6.28 s / 100 pt (62.8 ms/pt) | 0.40 s / 100 pt (4.0 ms/pt) | 0.05-0.07 s / 100 pt (0.5-0.7 ms/pt) | 0.038 s / 100 pt (0.38 ms/pt) | **~165×** |
  | 11 casi, solo campionamento punti (100 pt ciascuno) | ~27 s (stimato) | 3.05 s (misurato) | 0.78 s (misurato) | 0.42 s (misurato) | **~64×** |

  La CDF inversa aggiunge un costo di inizializzazione per orbitale che le
  versioni a rigetto non avevano in questa forma (costruire tre tabelle
  invertite anziché solo scansionare un massimo): **~255-300ms per
  orbitale** dopo le ottimizzazioni (era ~300-430ms prima) — ma è un costo
  fisso pagato una sola volta per (n,ℓ,m), non per punto. Generare 3000 punti
  sul caso più difficile costa ora init (~0.3s) + campionamento (~1.1s) ≈
  **~1.4s totali**, contro ~2.3s (CDF non ottimizzata), ~12s (separabile) o
  ~3 min (congiunto, proiettati) di prima — abbastanza veloce da poter essere
  rigenerato **a ogni cambio di orbitale scelto dall'utente**, non solo una
  tantum all'avvio (rilevante per l'interattività di CLAUDE.md M3), pur
  restando comunque un costo per-orbitale e non per-frame — coerente con
  l'architettura di CLAUDE.md §5 (i punti si generano una volta per
  orbitale, poi si ruotano).

  Nota: lo sweep completo di `device_gen.py` (tabelle + 280 campioni psi +
  punti, 11 casi) è rimasto quasi invariato (~28-30s) nonostante questi
  guadagni — perché quel numero è dominato dal codice dell'**harness di
  test stesso** (formattazione `%.17g` di ~26k valori, scrittura file),
  mai ottimizzato perché non fa parte del firmware reale. Il numero che
  conta per il prodotto finale è quello di campionamento punti sopra, che
  è migliorato ~1.9× in più rispetto alla CDF non ottimizzata.

## Ottimizzazioni MicroPython (docs.micropython.org/.../speed_python.html)

Applicate a `micropython/orbitals.py` e `micropython/pointcloud.py` dopo aver
letto la guida ufficiale alle prestazioni, nell'ordine che raccomanda lei
stessa (prima l'efficienza Python, poi l'emettitore nativo, poi — non fatto
qui, vedi sotto — Viper):

- **Alias di modulo per `math.sin`/`cos`/`sqrt`/`exp`/`pow`/`pi`** e per le
  funzioni stabili di `orbitals` usate da `pointcloud.py` — evita la doppia
  ricerca (nome del modulo + attributo) a ogni chiamata, significativo nei
  loop da 1001 iterazioni.
- **Eliminazione di ricalcoli invarianti nel loop**: es. `legendre_coeffs()`
  ricalcolava `math.pi/2` e `math.pi/100` a ogni iterazione di un loop
  invece di calcolarli una volta prima; `build_legendre_table()` ricalcolava
  `math.pi/(n-1)` implicitamente a ogni passo invece di un singolo
  `delta_theta` precalcolato (stesso pattern già usato correttamente da
  `build_radial_table()`, ora uniformato).
- **`array.array('d', ...)` al posto di `list`** per le tabelle interne di
  `pointcloud.py` (le tre CDF inverse e gli array scratch di
  `_build_inverse_cdf`) — una `list` di MicroPython alloca ogni elemento
  float come oggetto separato sull'heap; `array.array` impacchetta i valori
  in un unico buffer contiguo, evitando ~9000 allocazioni piccole (e la
  pressione sul garbage collector che comportano) per ogni orbitale
  inizializzato. Typecode `'d'` (non `'f'`) deliberatamente: su `'f'` l'unix
  port tronca ogni valore a float32 in scrittura, il che avrebbe
  silenziosamente degradato la precisione del gate di correttezza stretto
  (rtol=1e-9); su `'d'` la precisione doppia dell'unix port resta intatta,
  mentre sulla scheda reale (build a precisione singola) `'d'` non costa
  nulla in più — verificato empiricamente che `'d'` e `'f'` restituiscono
  gli stessi valori troncati a float32 su quella build.
- **`@micropython.native`** su tutte le funzioni calde di entrambi i moduli
  (compilazione a opcode nativi invece che bytecode, ~2× secondo la guida).
  È una direttiva riconosciuta dal compilatore MicroPython per la sintassi
  letterale `micropython.native` (non un vero decorator importabile), quindi
  non esiste uno shim `try/except ImportError` che la preservi: i due moduli
  non girano più sotto CPython semplice (proprietà dichiarata nei docstring
  originali ma mai sfruttata da questa cartella, che esegue sempre e solo
  sotto `micropython` reale) — un compromesso deliberato.
- **Viper NON applicato**, deliberatamente. La guida lo raccomanda proprio
  per codice intero/bitwise come `XorShift32.next()` — il candidato ideale
  sulla carta — ma i tipi interi di Viper sono una rappresentazione diversa
  dai normali `int` Python, e l'intera strategia di validazione incrociata
  di questo progetto dipende dal fatto che `XorShift32` produca output
  bit-identico tra i tre porting per lo stesso seme. Un mismatch sottile nel
  wraparound/nella rappresentazione Viper potrebbe rompere silenziosamente
  questa proprietà senza un cambiamento di comportamento abbastanza
  evidente da notare casualmente — rischio non giustificato per una singola
  funzione già velocizzata da `@micropython.native` (che non cambia la
  semantica intera, solo come viene eseguita la stessa logica). Da
  rivalutare solo se il profiling su hardware reale mostrasse
  `XorShift32` stesso come collo di bottiglia residuo.

Tutte le ottimizzazioni sono state riverificate con `run_crosscheck.sh`
(44/44 + 11/11 a epsilon macchina, invariato) e `run_on_device.sh` (43/44 +
11/11 entro tolleranza float32, stesso identico caso noto di prima) prima di
fidarsi dei numeri di prestazione sopra.
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
