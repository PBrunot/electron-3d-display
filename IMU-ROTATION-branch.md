# Rotazione dell'atomo pilotata dall'IMU (accelerometro + giroscopio) — note del branch `imu-orientation-rotation`

Documento dedicato al lavoro sul branch `imu-orientation-rotation` (non su `master`), copiato qui
dal piano locale usato per progettare la feature così da poterlo riprendere anche da una macchina
diversa da quella su cui è stato scritto.

## Stato (aggiornato 2026-09-26)

**Implementazione completata e committata** su questo branch (commit `572f9fb`, "Drive atom/orbital
rotation from IMU orientation, not fixed auto-spin"), non ancora testata su hardware reale (la
scheda non era disponibile durante l'implementazione). Entrambe le build compilano pulite senza
warning (`pio run -e WS_ESP32_S3_LCD_1_3` e `pio run -e CYD`).

**Da fare appena la scheda è disponibile** — nell'ordine, seguendo la sezione "Verifica" più sotto:
1. Flashare e osservare il log seriale del dump periodico in `OrientationTracker::update()`
   (tag `orientation`) per tarare `pitchAxisSign`/`rollAxisSign`/`yawAxisSign` in
   `OrientationTrackerConfig` (default tutti a `+1`, quasi certamente uno o più andranno invertiti,
   ed è possibile — non solo probabile — che serva anche una permutazione fra i tre assi se il
   montaggio fisico dell'IMU sul PCB non è quello assunto).
2. Verificare il margine fra il clamp della rotazione continua (`tiltClampRad`/`rollClampRad`,
   partenza ~0.61 rad) e le nuove soglie del gesto di navigazione (`thresholdG=0.45`/`releaseG=0.32`
   in `tilt_gesture.h`) — regolare entrambi finché non c'è uno stacco percepibile fra "sto ancora
   ruotando la vista" e "sto navigando il menu".
3. Verificare la deriva dello yaw da fermo per qualche minuto, e valutare se `yawRecenterPerSecond`
   (default 0.05, ~20s di costante di tempo) va alzato, abbassato o azzerato in base a quanto
   "risucchia" una rotazione yaw mantenuta deliberatamente.
4. Solo dopo 1-3: eventuali ritocchi fini a `complementaryAlpha` (default 0.98) se il movimento
   risulta scattoso o troppo filtrato.

Il resto di questo documento è il piano originale (pre-implementazione) — l'architettura descritta
sotto rispecchia fedelmente cosa è stato effettivamente scritto nel codice; i riferimenti a
file/riga puntavano al codice PRIMA di queste modifiche e possono essere leggermente sfasati ora
(il codice reale è la fonte di verità, non questi numeri di riga).

## Contesto

Oggi la vista in stato stazionario (sia atomo che orbitali) ruota a velocità sintetica fissa: `stepCamera()` (`src/render/camera.cpp`) incrementa `CameraState{yaw,tilt,roll}` di costanti fisse ogni iterazione del loop, indipendentemente da qualunque input reale. Vogliamo che l'inclinazione fisica della scheda in mano determini l'orientamento renderizzato dell'atomo, come se fosse un oggetto solido — mapping assoluto, non "rate control" (la ricerca su controller a giroscopio mostra che il rate-control con zona morta soffre di deriva della zona morta stessa; il mapping assoluto orientamento→oggetto, come nelle app AR "magic window", non ha questo problema).

Decisioni confermate dall'utente durante la progettazione:
1. L'effetto "respiro" dello zoom (`zoomAngle`/`kZoomAngleStep`) resta invariato e disaccoppiato.
2. La rotazione sintetica automatica nel loop principale va sostituita dalla rotazione reale pilotata dall'IMU.
3. Il giroscopio va integrato fin dall'inizio (fusione accelerometro+giroscopio), non in una fase successiva.

Vincolo hardware: il QMI8658 non ha magnetometro. Pitch/roll si leggono in modo assoluto e senza deriva dall'accelerometro; lo yaw può venire solo dall'integrazione del giroscopio e **deriva nel tempo** — non è un limite risolvibile via firmware, va gestito (vedi sotto), non nascosto.

Il gesto di navigazione a "tilt estremo" (`TiltGestureDetector`) resta invariato nella logica, solo le soglie sono state alzate per lasciare spazio alla nuova zona di rotazione continua.

## Approccio (come implementato)

### 1. Nuovo modulo `src/ux/orientation_tracker.h`/`.cpp`

Stessa impostazione stilistica di `tilt_gesture.h`/`.cpp` (stesso tipo di config-struct con default nel costruttore, stesso schema di calibrazione).

```cpp
struct OrientationTrackerConfig {
    orb_real_t complementaryAlpha = 0.98;    // peso della componente giroscopica nel filtro
    orb_real_t tiltClampRad = 0.61;          // ~35°, va tarato su hardware
    orb_real_t rollClampRad = 0.61;
    orb_real_t yawRecenterPerSecond = 0.05;  // richiamo lento verso zero, vedi nota sotto
    int gyroCalibrationSamples = 100;        // stesso schema di TiltGestureConfig::calibrationSamples
    uint32_t gyroCalibrationSampleDelayMs = 10;
    int pitchAxisSign = 1, rollAxisSign = 1, yawAxisSign = 1; // da tarare in campo, vedi punto 6
};

class OrientationTracker {
public:
    explicit OrientationTracker(Qmi8658 &imu, const OrientationTrackerConfig &cfg = {});
    void calibrate();   // ~1s da fermo: bias giroscopio + pitch/roll di riposo
    void resync();      // azzera il clock del dt senza toccare gli angoli fusi
    void update();      // legge IMU, fonde, integra — una volta per frame in stato stazionario
    orb_real_t yawRad() const;
    orb_real_t tiltRad() const;  // clampato, non wrappato
    orb_real_t rollRad() const;  // clampato, non wrappato
private:
    Qmi8658 &imu_;
    OrientationTrackerConfig cfg_;
    orb_real_t gyroBiasX_ = 0, gyroBiasY_ = 0, gyroBiasZ_ = 0;
    orb_real_t pitchOffset_ = 0, rollOffset_ = 0;
    orb_real_t fusedPitch_ = 0, fusedRoll_ = 0, fusedYaw_ = 0;
    int64_t lastUpdateUs_ = 0;
};
```

**Fusione in `update()`**: lettura combinata accel+giroscopio (punto 2), applicazione dei segni di calibrazione assi (`pitchAxisSign`/`rollAxisSign`/`yawAxisSign`) ai valori grezzi, poi:
- `pitchAccel = atan2(-ax, sqrt(ay*ay+az*az)) - pitchOffset_`, `rollAccel = atan2(ay, az) - rollOffset_` (assolute, senza deriva).
- Filtro complementare: `fusedPitch_ = alpha*(fusedPitch_ + gyroRatePitch*dt) + (1-alpha)*pitchAccel` (idem per roll).
- Yaw: solo integrazione giroscopica (`fusedYaw_ += gyroRateYaw*dt`), poi richiamo esponenziale lento verso zero (`fusedYaw_ *= (1 - yawRecenterPerSecond*dt)`).
- `tiltRad()`/`rollRad()` applicano un clamp simmetrico (non un wrap come oggi) — l'inclinazione fisica ha un range naturale limitato.

**Nota sul richiamo dello yaw** (da confermare in fase di test): un decadimento costante nel tempo corregge la deriva quando il dispositivo è fermo, ma "tira indietro" anche uno yaw che l'utente sta deliberatamente mantenendo ruotando la scheda e tenendola ferma in quella posizione (con costante di tempo ~20s al valore di default). È un compromesso accettabile per questo tipo di dispositivo (non serve tracking di livello "navigazione"), ma è il primo parametro da regolare — anche a zero — se in prova sembra "risucchiare" la rotazione voluta.

### 2. Lettura combinata accel+giroscopio — `src/ux/imu.h`/`.cpp`

Nuovo metodo (senza toccare `readAccelG()`, che resta per `TiltGestureDetector`):
```cpp
bool readAccelGyro(orb_real_t *ax, orb_real_t *ay, orb_real_t *az,
                    orb_real_t *gx, orb_real_t *gy, orb_real_t *gz);
```
Un'unica burst-read a 12 byte da `kRegAccelOut` (accel e giroscopio sono contigui in memoria: 0x35–0x3A poi 0x3B–0x40, auto-increment già attivo via CTRL1=0x60) — nessuna transazione I2C aggiuntiva rispetto a leggerli separatamente. Decodifica accel con `kRange4gScale` esistente, giroscopio con la nuova `kGyroScale512dps`.

Costruttore: aggiunta la scrittura `writeReg(kRegCtrl3, (kGyroRange512dpsBits<<4)|kOdr250HzBits)` e cambiato CTRL7 da `0x01` a `0x03` (aEN|gEN). Range ±512dps scelto per non saturare su un flick veloce della mano (una saturazione del giroscopio corrompe l'integrale per il resto del movimento, peggio della risoluzione leggermente più grossa).

Sulla build CYD (nessun IMU) tutto resta invariato — `readAccelGyro()` segue lo stesso pattern no-op di `readAccelG()`.

### 3. Nuove costanti — `src/config/hardware_constants.h`
```cpp
inline constexpr uint8_t kRegCtrl3 = 0x04;
inline constexpr uint8_t kGyroRange512dpsBits = 5;   // 101 -> ±512dps, 64 LSB/dps
inline constexpr orb_real_t kGyroScale512dps = orb_real_t(64.0);
inline constexpr uint8_t kCtrl7AccelGyroEnabled = 0x03;
```

### 4. Calibrazione bias del giroscopio (`OrientationTracker::calibrate()`)

Stesso schema di `TiltGestureDetector::calibrate()` (`tilt_gesture.cpp`): media di `gyroCalibrationSamples` letture combinate (~1s, dispositivo fermo) → bias giroscopio (per rimuoverlo a runtime) e offset pitch/roll di riposo (per azzerare l'angolo di montaggio). A differenza della baseline "planare" dell'accelerometro (che può saltare la calibrazione live usando le costanti hardcoded `kDefaultBaseline*`), il bias del giroscopio dipende da temperatura/uscita dal reset e **va ricalcolato a ogni boot** — aggiunge ~1s all'avvio. Eseguita subito dopo aver costruito `OrientationTracker` in `main.cpp`, prima del ramo `checkPlanarAtBoot()` esistente (stessa assunzione: dispositivo ancora fermo dopo lo splash).

### 5. Collegamento allo `Qmi8658` condiviso

`TiltGestureDetector` non espone l'`Qmi8658&` che detiene privatamente, e resta così com'è. `OrientationTracker` è costruito in `main.cpp` accanto a `tilt` e passato in parallelo lungo la stessa catena di chiamate, come puntatore nullable (nullptr sulla build CYD, che non ha IMU):

- `runChooser(Display&, GestureSource& tilt, OrientationTracker* orientation)`
- `runOrbitalView(Display&, GestureSource&, OrientationTracker*)`
- `runAtomView(Display&, GestureSource&, OrientationTracker*)`

Ogni header dichiara `class OrientationTracker;` in forward declaration (stesso pattern già usato per `class Display;`).

### 6. Sostituzione nel loop principale (stato stazionario)

Nel loop stazionario di `atom_view.cpp` e `orbital_view.cpp`, al posto di `stepCamera(&camera);`:
```cpp
if (orientation != nullptr) {
    orientation->update();
    camera.yaw  = orientation->yawRad();
    camera.tilt = kCameraTiltStart + orientation->tiltRad();
    camera.roll = kCameraRollStart + orientation->rollRad();
} else {
    stepCamera(&camera);   // fallback CYD: nessun IMU, resta l'auto-rotazione sintetica
}
```
I punti di chiamata nelle animazioni transitorie (intro, dissezione, fly-over) **restano invariati** — continuano a usare `stepCamera()` sintetico, dato che sono animazioni scriptate e non lo stato "sto guardando l'atomo in mano".

**Tuning assi (passo obbligatorio in campo)**: non esiste alcuna mappatura nota fra gli assi fisici dell'IMU e yaw/tilt/roll. Si parte con tutti i segni a +1; `OrientationTracker::update()` stampa periodicamente (stesso ritmo di `FrameStats`, tag di log `orientation`) i valori grezzi e gli angoli fusi — inclinando la scheda un asse alla volta si verifica quale angolo si muove e in che verso, correggendo i segni in `OrientationTrackerConfig`. Se un asse risultasse scambiato (non solo invertito) rispetto all'atteso — possibile a seconda del montaggio fisico dell'IMU sul PCB — servirà anche una permutazione, non solo un segno: da verificare appena si hanno letture reali.

**dt e resync**: `OrientationTracker` calcola il proprio dt internamente da `esp_timer_get_time()` (come già fa `tilt_gesture.cpp`), senza toccare `FrameStats`. Ogni punto del loop che già chiamava `stats.reset()` dopo aver "bruciato" tempo reale senza un normale ciclo di `update()` (fly-over, dissezione, salti di idle, escursioni di zoom) chiama anche `orientation->resync()`, altrimenti il successivo `update()` vedrebbe un dt di più secondi e l'integratore del giroscopio farebbe un salto.

### 7. Soglie del gesto di navigazione — `src/ux/tilt_gesture.h`

Il clamp di rotazione continua (radianti, spazio angoli fusi) e `thresholdG`/`releaseG` (g, spazio deviazione accelerometro grezza) sono in unità diverse senza relazione diretta — la taratura è empirica:
- Default alzati da `thresholdG=0.28`/`releaseG=0.18` a `thresholdG=0.45`/`releaseG=0.32` (stesso margine di isteresi, spostato più in alto).
- Da verificare su hardware che il clamp della rotazione continua (`tiltClampRad`/`rollClampRad`, punto di partenza ~0.61 rad) saturi prima che il gesto di navigazione scatti, lasciando un margine percepibile fra "sto ancora ruotando la vista" e "sto navigando il menu".

### 8. Estensione anche alla vista orbitali

`orbital_view.cpp` condivide architettura e struttura del loop quasi identiche a `atom_view.cpp` (stessa `CameraState`/`stepCamera()`, loop stazionario praticamente sovrapponibile riga per riga). Lo stesso trattamento è stato applicato a entrambe le viste per coerenza.

## File coinvolti
- `src/ux/orientation_tracker.h`, `src/ux/orientation_tracker.cpp` (nuovi)
- `src/ux/imu.h`, `src/ux/imu.cpp`
- `src/config/hardware_constants.h`
- `src/ux/tilt_gesture.h` (solo default soglie)
- `src/views/atom_view.h`/`.cpp`, `src/views/orbital_view.h`/`.cpp`
- `src/ux/chooser.h`/`.cpp`
- `src/main.cpp`

## Verifica (nessun harness di test automatico — verifica manuale su hardware)

1. **Solo accelerometro come prima passata**: `complementaryAlpha=0` temporaneo (yaw fermo a 0), flashare e verificare che inclinare la scheda ruoti l'atomo nel verso atteso — usare questa fase per tarare i segni degli assi (punto 6) prima di fidarsi del contributo del giroscopio.
2. **Attivare il giroscopio** (`complementaryAlpha=0.98`): verificare movimento fluido durante l'inclinazione continua (nessuno scatto visibile) e che un flick rapido non causi glitch (saturazione a ±512dps, controllabile nel log di debug).
3. **Verifica deriva**: lasciare il dispositivo fermo per alcuni minuti in vista stazionaria — lo yaw non deve "scappare via" lentamente (valida la mitigazione di richiamo), pitch/roll restano ancorati.
4. **Regressione gesto di navigazione**: con le soglie alzate, verificare che i quattro gesti discreti (cambio elemento, dissezione, ritorno al menu) scattino ancora in modo affidabile oltre il punto di saturazione del clamp continuo.
5. **Isolamento zoom**: confermare che il respiro dello zoom non sia cambiato in periodo/ampiezza rispetto a prima.
6. **Sanità della calibrazione al boot**: da fermo, il bias di giroscopio loggato deve essere piccolo e con bassa deviazione standard; muovendo il dispositivo durante il boot deve scattare lo stesso tipo di warning già presente in `TiltGestureDetector::calibrate()`.
7. **Regressione CYD**: verificare che la build CYD non costruisca mai `OrientationTracker`, passi `nullptr` ovunque, e che entrambe le viste tornino al vecchio `stepCamera()` sintetico con la navigazione touch invariata.
8. **Performance**: controllare che il log periodico FPS/render/prepare di `FrameStats` non mostri regressioni percepibili con la lettura I2C a 12 byte + fusione aggiunte per frame.
