# corner_calibration.ino — metodologia di calibrazione sprite→vista fisica

Sviluppato in [VolosR/esp32Prism](https://github.com/VolosR/esp32Prism) per
rispondere a una domanda concreta emersa durante lo sviluppo di
`examples/cube/`: **come si mappano le coordinate dello sprite 240×240 su
ciò che si vede fisicamente guardando attraverso il cubo prisma?** Questa è
esattamente l'informazione mancante per decidere la domanda aperta in
CLAUDE.md §8 ("vista singola vs 4 quadranti") con dati reali invece che
supposizioni.

## Cosa fa

Disegna un quadrato di colore pieno in ognuno dei 4 angoli dello sprite (rosso
in alto-sx, verde in alto-dx, blu in basso-sx, giallo in basso-dx — posizioni
*nello sprite*, non ancora corrette per nessuna trasformazione), poi chiede
di riportare quale colore compare in quale angolo **fisico** guardando il
cubo prisma. Nessuna rotazione, nessuna proiezione, nessuna matematica 3D:
isola la sola domanda "come si mappa lo spazio-sprite sullo spazio-visto",
scorporata da qualunque bug/approssimazione nella pipeline di rendering.

## Risultato ottenuto (su questa scheda fisica, con cubo prisma)

Ogni angolo dello sprite corrisponde all'angolo **diagonalmente opposto**
nella vista fisica (sprite alto-sx → visto in basso-dx, ecc.) — cioè una
**rotazione di 180°** tra spazio-sprite e spazio-visto attraverso lo specchio
a 45° del cubo prisma. Punto importante: è una rotazione propria, non uno
specchiamento — la chiralità si preserva (un angolo resta un angolo, non
diventa il suo riflesso), coerente con una riflessione planare pura (uno
specchio piano è un'isometria: riposiziona l'immagine ma non la scala né la
deforma). Questo ha corretto un bug reale nell'esempio `cube.ino`: un
tentativo precedente di compensare con uno stretch verticale del `1/cos(45°)`
(pensato per un pannello inclinato guardato di sbieco, non per una
riflessione speculare) causava un cubo visibilmente "allungato" — rimosso una
volta chiarito che una riflessione planare non deforma, sposta soltanto.

**Attenzione**: questo risultato (rotazione di 180°) dipende dal montaggio
fisico esatto scheda+cubo prisma della unità testata in `esp32Prism` — con un
case/cubo diverso (es. `-B` senza cubo, o un montaggio ad angolazione
diversa) la trasformazione potrebbe essere diversa. **Rieseguire questo test
sulla configurazione fisica reale del progetto** prima di hardcodare
`180°` nel codice di produzione — non assumere che valga automaticamente.

## Bonus: bug colore trovato con lo stesso test

Lo stesso sketch (nella sua prima versione, prima della correzione BGR — vedi
`examples/cube/README.md` per i dettagli) ha anche rivelato lo scambio canali
R/B (giallo mostrato come ciano): utile tenere questo tipo di test a
disposizione come diagnostica generale "sprite→schermo", non solo per la
geometria ma anche per il colore, ogni volta che si tocca `display.cpp` o si
cambia pannello/lotto.
