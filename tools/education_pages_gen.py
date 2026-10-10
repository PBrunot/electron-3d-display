"""Generate the Italian lesson, bilingual learning portals and search index.

The English lesson is handwritten in web/shell_filling.html. Its Italian
counterpart and the portal pages are generated here; do not edit those outputs.
Run from any directory with Python 3. Use --check to detect stale outputs.
"""
import argparse
from html import escape
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = 'https://pbrunot.github.io/electron-3d-display/'

TRANSLATIONS = {
    '<html lang="en">': '<html lang="it">',
    'Electron configuration animation: shell filling & orbital diagrams | Atom Cube': 'Configurazione elettronica: animazione di gusci e orbitali | Atom Cube',
    'Learn electron configurations with an interactive shell-filling animation. Build atoms from hydrogen to calcium and explore Aufbau, Hund’s rule and Pauli spin pairing.': 'Impara la configurazione elettronica con un’animazione interattiva: riempi gli atomi dall’idrogeno al calcio e scopri Aufbau, Hund e il principio di Pauli.',
    'Electron configuration animation | Atom Cube': 'Configurazione elettronica animata | Atom Cube',
    'Watch an atom fill while orbital boxes explain Aufbau, Hund and Pauli.': 'Osserva il riempimento dell’atomo mentre i diagrammi degli orbitali spiegano Aufbau, Hund e Pauli.',
    'content="en_GB"': 'content="it_IT"',
    '<nav aria-label="Main navigation"><a href="../index.html">← Explore the atom</a> · <a href="index.html">3D orbital viewer</a> · <a href="shell_filling_it.html" lang="it" hreflang="it">Italiano</a></nav>': '<nav aria-label="Navigazione principale"><a href="../it/index.html">← Esplora l’atomo</a> · <a href="index.html">Visualizzatore 3D (EN)</a> · <a href="shell_filling.html" lang="en" hreflang="en">English</a></nav>',
    'Electron configuration: watch an atom fill': 'Configurazione elettronica: guarda l’atomo riempirsi',
    'One electron at a time, from hydrogen to calcium. Watch subshells fill, then see how their electrons add up to shells.': 'Un elettrone alla volta, dall’idrogeno al calcio. Osserva il riempimento dei sottolivelli e il totale degli elettroni in ogni guscio.',
    'Target atom ': 'Atomo ',
    '>Play</button>': '>Avvia</button>',
    'aria-label="Remove one electron"': 'aria-label="Rimuovi un elettrone"',
    'aria-label="Add one electron"': 'aria-label="Aggiungi un elettrone"',
    '− Electron': '− Elettrone',
    '+ Electron': '+ Elettrone',
    '>Reset</button>': '>Ricomincia</button>',
    '<label>Pace ': '<label>Velocità ',
    '>Slow</option>': '>Lenta</option>',
    '>Normal</option>': '>Normale</option>',
    '>Fast</option>': '>Rapida</option>',
    'Electrons placed: 0': 'Elettroni inseriti: 0',
    'Watch the atom fill': 'Osserva l’atomo',
    'aria-label="Atom filling visualization"': 'aria-label="Animazione del riempimento dell’atomo"',
    'Shell-colored schematic electron probability clouds. The shell totals below give the electron counts.': 'Nuvole schematiche di probabilità elettronica colorate per guscio. I totali sotto indicano il numero di elettroni.',
    '> Rotate atom': '> Ruota l’atomo',
    '<label>Points ': '<label>Punti ',
    'Watch the atom build electron by electron, in sync with the orbital boxes.': 'Osserva l’atomo formarsi elettrone dopo elettrone, in sincronia con il diagramma degli orbitali.',
    'Schematic s spheres and p lobes at a fixed scale. Dots represent probability density, not individual electrons; radii and radial nodes are simplified.': 'Sfere s e lobi p schematici a scala fissa. I punti rappresentano la densità di probabilità; raggi e nodi radiali sono semplificati.',
    'aria-label="Orbital filling diagram"': 'aria-label="Diagramma di riempimento degli orbitali"',
    'Subshells & orbital boxes': 'Sottolivelli e orbitali',
    'Read top to bottom in filling order. One box = one orbital; each arrow = one electron’s spin.': 'Leggi dall’alto verso il basso nell’ordine di riempimento. Un riquadro è un orbitale; ogni freccia indica lo spin di un elettrone.',
    'aria-label="Electron configuration"': 'aria-label="Configurazione elettronica"',
    'Shell totals': 'Elettroni per guscio',
    'The number in 2p means shell n = 2; p identifies the subshell within it.': 'Il numero in 2p indica il guscio n = 2; p identifica il sottolivello al suo interno.',
    'Bars compare occupancy with the shell capacity 2n². Calcium has 2, 8, 8, 2: 4s fills before 3d, so shell 3 is still below its capacity of 18.': 'Le barre confrontano gli elettroni presenti con la capacità del guscio, 2n². Nel calcio sono 2, 8, 8, 2: il 4s si riempie prima del 3d, quindi il terzo guscio resta sotto la capacità di 18.',
    'Three rules to watch': 'Tre regole da osservare',
    'for these neutral ground states, follow 1s → 2s → 2p → 3s → 3p → 4s.': 'per questi stati fondamentali neutri, segui 1s → 2s → 2p → 3s → 3p → 4s.',
    'in a p subshell, occupy all three equal-energy orbitals with parallel spins before pairing.': 'in un sottolivello p, occupa i tre orbitali di pari energia con spin paralleli prima di formare coppie.',
    'an orbital holds at most two electrons, with opposite spins.': 'un orbitale contiene al massimo due elettroni, con spin opposti.',
    'This is a construction diagram for a selected neutral atom, not a simulation of adding electrons to an ion over time. Arrows show spin states, not electron trajectories. Box spacing does not represent measured energies. Beyond calcium, ground-state exceptions require additional treatment.': 'È un diagramma per costruire la configurazione dell’atomo neutro scelto. Le frecce indicano gli stati di spin; la distanza fra i riquadri non rappresenta energie misurate. Oltre il calcio occorre considerare anche le eccezioni di configurazione.',
    'Reference: OpenStax · Electronic structure of atoms': 'Fonte: OpenStax · Struttura elettronica degli atomi (EN)',
    'Try these electron configurations': 'Prova queste configurazioni elettroniche',
    'Nitrogen: 1s² 2s² 2p³': 'Azoto: 1s² 2s² 2p³',
    'shows three singly occupied p orbitals.': 'mostra tre orbitali p occupati singolarmente.',
    'Oxygen: 1s² 2s² 2p⁴': 'Ossigeno: 1s² 2s² 2p⁴',
    'adds the first pair.': 'aggiunge la prima coppia.',
    'Calcium: [Ar] 4s²': 'Calcio: [Ar] 4s²',
    'shows why the outer shell starts before 3d fills.': 'mostra perché il guscio esterno inizia prima del riempimento del 3d.',
}

CONTENT = {
    'en': {
        'title': 'Atomic structure, electron configurations & 3D orbitals | Atom Cube',
        'description': 'Explore inside the atom: interactive electron shell filling, 3D atomic orbitals, shell dissection and guides to the physics behind the visualizations.',
        'nav': 'Explore', 'language': 'Italiano', 'label': 'A learning portal from FabLab Bergamo',
        'heading': 'Look inside the atom.',
        'intro': 'Build its electron configuration. Rotate its orbitals. Explore how probability clouds turn into the structure of an element.',
        'start': 'Start with electron filling', 'view': 'Explore 3D atoms & orbitals',
        'caption': 'Carbon: a sampled electron probability cloud from the project’s atom viewer.',
        'journey': 'Three ways to explore atomic structure',
        'filltitle': '1. Build an electron configuration',
        'fillbody': 'Place electrons one at a time. Read orbital boxes, compare shell totals, and see Aufbau, Hund’s rule and Pauli pairing in action. The filling lesson currently covers hydrogen to calcium.',
        'fillcta': 'Open the filling animation',
        'orbtitle': '2. See the shape of an orbital',
        'orbbody': 'Open the viewer and choose hydrogen orbitals. Rotate the probability cloud to see lobes and nodes from different angles. Quantum numbers describe the state; the point cloud shows its spatial distribution.',
        'orbcta': 'Open the 3D viewer',
        'atomtitle': '3. Explore a complete atom',
        'atombody': 'In the viewer, choose Element Explorer. Compare elements and use shell dissection to inspect the occupied subshells. This wider atom model complements the introductory filling lesson.',
        'atomcta': 'Explore elements',
        'terms': 'Shell, subshell or orbital?',
        'tableheads': ['Term', 'Meaning', 'Electron capacity'],
        'rows': [['Shell', 'A principal level identified by n, such as n = 2.', 'At most 2n²'], ['Subshell', 'A family within a shell: s, p, d or f.', '2, 6, 10 or 14'], ['Orbital', 'One spatial state within a subshell; shown as one box.', 'At most 2, with opposite spins']],
        'order': 'Shells do not simply fill one after another. For example, 4s begins before 3d. Use calcium to see the difference between a shell’s capacity and its actual occupancy.',
        'examples': 'Try a question, then watch the answer',
        'exampleslist': [('Why does nitrogen have three unpaired p electrons?', 7), ('Where does oxygen’s fourth p electron go?', 8), ('Why does calcium have a 2, 8, 8, 2 shell pattern?', 20)],
        'math': 'Follow the physics into the implementation',
        'mathintro': 'For a deeper dive, follow the path from a wavefunction to a rotating probability cloud. These technical guides are in English.',
        'guides': [('Quantum numbers → sampling tables', 'How angular and radial functions become sampling distributions.', 'coefficients_to_tables.html'), ('Tables → device data', 'How orbital and atomic radial tables are prepared for the device.', 'tables_to_bin.html'), ('Samples → a 3D cloud', 'How sampled coordinates become the points drawn on screen.', 'tables_to_pointclouds.html')],
        'faq': 'Questions about atoms and electrons',
        'questions': [('Are the dots individual electrons?', 'No. Each dot is a sampled possible position. Together they show the spatial probability distribution, rather than a path around the nucleus.'), ('What do the arrows in an orbital diagram mean?', 'They label electron spin states. Two arrows in the same box must have opposite directions; equal-energy orbitals fill singly before electrons pair.'), ('Is the filling animation an exact atom simulation?', 'It is a teaching diagram with schematic cloud shapes. The 3D atom viewer uses the project’s radial models and sampling calculations; the guides explain their construction.')],
        'source': 'Explore the source code and hardware project on GitHub',
    },
    'it': {
        'title': 'Struttura atomica, configurazione elettronica e orbitali 3D | Atom Cube',
        'description': 'Esplora l’interno dell’atomo: configurazione elettronica animata, riempimento dei gusci, orbitali atomici 3D e guide alla fisica delle nuvole di probabilità.',
        'nav': 'Esplora', 'language': 'English', 'label': 'Un percorso didattico del FabLab Bergamo',
        'heading': 'Guarda dentro l’atomo.',
        'intro': 'Costruisci la configurazione elettronica. Ruota gli orbitali. Scopri come le nuvole di probabilità descrivono la struttura di un elemento.',
        'start': 'Inizia dal riempimento elettronico', 'view': 'Esplora atomi e orbitali 3D',
        'caption': 'Carbonio: una nuvola di probabilità elettronica campionata dal visualizzatore del progetto.',
        'journey': 'Tre percorsi per esplorare la struttura atomica',
        'filltitle': '1. Costruisci una configurazione elettronica',
        'fillbody': 'Inserisci gli elettroni uno alla volta. Leggi i diagrammi degli orbitali, confronta i totali per guscio e osserva Aufbau, Hund e Pauli. La lezione sul riempimento copre attualmente gli elementi dall’idrogeno al calcio.',
        'fillcta': 'Apri l’animazione del riempimento',
        'orbtitle': '2. Osserva la forma di un orbitale',
        'orbbody': 'Apri il visualizzatore e scegli gli orbitali dell’idrogeno. Ruota la nuvola di probabilità per osservare lobi e nodi da diversi angoli. I numeri quantici descrivono lo stato; i punti ne mostrano la distribuzione spaziale.',
        'orbcta': 'Apri il visualizzatore 3D (EN)',
        'atomtitle': '3. Esplora un atomo completo',
        'atombody': 'Nel visualizzatore, scegli Element Explorer. Confronta gli elementi e usa la dissezione per ispezionare i sottolivelli occupati. Questo modello atomico più ampio affianca la lezione introduttiva sul riempimento.',
        'atomcta': 'Esplora gli elementi (EN)',
        'terms': 'Guscio, sottolivello o orbitale?',
        'tableheads': ['Termine', 'Significato', 'Capacità elettronica'],
        'rows': [['Guscio', 'Un livello principale identificato da n, per esempio n = 2.', 'Al massimo 2n²'], ['Sottolivello', 'Una famiglia all’interno di un guscio: s, p, d o f.', '2, 6, 10 o 14'], ['Orbitale', 'Uno stato spaziale del sottolivello, rappresentato da un riquadro.', 'Al massimo 2, con spin opposti']],
        'order': 'I gusci non si riempiono semplicemente uno dopo l’altro: il 4s inizia prima del 3d. Osserva il calcio per distinguere la capacità di un guscio dagli elettroni effettivamente presenti.',
        'examples': 'Scegli una domanda e osserva la risposta',
        'exampleslist': [('Perché l’azoto ha tre elettroni p spaiati?', 7), ('Dove va il quarto elettrone p dell’ossigeno?', 8), ('Perché il calcio ha 2, 8, 8, 2 elettroni nei gusci?', 20)],
        'math': 'Dalla fisica all’implementazione',
        'mathintro': 'Per approfondire, segui il percorso dalla funzione d’onda alla nuvola di probabilità rotante. Le guide tecniche sono in inglese.',
        'guides': [('Numeri quantici → tabelle di campionamento', 'Come le funzioni angolari e radiali diventano distribuzioni campionabili.', 'coefficients_to_tables.html'), ('Tabelle → dati sul dispositivo', 'Come si preparano le tabelle radiali degli orbitali e degli atomi.', 'tables_to_bin.html'), ('Campioni → nuvola 3D', 'Come le coordinate campionate diventano i punti disegnati sullo schermo.', 'tables_to_pointclouds.html')],
        'faq': 'Domande su atomi ed elettroni',
        'questions': [('I punti sono singoli elettroni?', 'No. Ogni punto è una possibile posizione campionata. Insieme mostrano una distribuzione spaziale di probabilità, anziché una traiettoria intorno al nucleo.'), ('Che cosa indicano le frecce nel diagramma degli orbitali?', 'Indicano gli stati di spin degli elettroni. Le due frecce nello stesso riquadro devono essere opposte; gli orbitali di pari energia si occupano singolarmente prima di formare coppie.'), ('L’animazione del riempimento è una simulazione atomica esatta?', 'È un diagramma didattico con forme schematiche delle nuvole. Il visualizzatore 3D usa i modelli radiali e i calcoli di campionamento del progetto; le guide ne spiegano la costruzione.')],
        'source': 'Scopri il codice sorgente e il progetto hardware su GitHub',
    },
}


def portal(language):
    c = CONTENT[language]
    prefix = '../' if language == 'it' else ''
    path = 'it/' if language == 'it' else ''
    other = '../index.html' if language == 'it' else 'it/index.html'
    other_language = 'en' if language == 'it' else 'it'
    filling = prefix + 'web/shell_filling' + ('_it' if language == 'it' else '') + '.html'
    viewer = prefix + 'web/index.html'
    cards = ''.join(f'<article><h3>{c[key + "title"]}</h3><p>{c[key + "body"]}</p><a class="text-link" href="{link}">{c[key + "cta"]} →</a></article>' for key, link in [('fill', filling), ('orb', viewer), ('atom', viewer)])
    rows = ''.join('<tr>' + ''.join(f'<td>{escape(cell)}</td>' for cell in row) + '</tr>' for row in c['rows'])
    examples = ''.join(f'<li><a href="{filling}?z={z}">{question} →</a></li>' for question, z in c['exampleslist'])
    guides = ''.join(f'<li><a href="{prefix}docs/{filename}">{title} →</a><p>{body}</p></li>' for title, body, filename in c['guides'])
    questions = ''.join(f'<details open><summary>{question}</summary><p>{answer}</p></details>' for question, answer in c['questions'])
    schema = {
        '@context': 'https://schema.org', '@type': 'CollectionPage',
        'name': c['title'], 'description': c['description'],
        'url': BASE + path, 'inLanguage': language,
        'about': ['Atomic structure', 'Electron configuration', 'Atomic orbitals'],
        'hasPart': [{'@type': 'LearningResource', 'name': c['filltitle'], 'url': BASE + 'web/shell_filling' + ('_it' if language == 'it' else '') + '.html', 'inLanguage': language, 'learningResourceType': 'Interactive visualization'}],
    }
    return f'''<!doctype html>
<!-- Generated by tools/education_pages_gen.py; edit that source. -->
<html lang="{language}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{escape(c['title'])}</title>
<meta name="description" content="{escape(c['description'], quote=True)}">
<link rel="canonical" href="{BASE + path}">
<link rel="alternate" hreflang="en" href="{BASE}">
<link rel="alternate" hreflang="it" href="{BASE}it/">
<link rel="alternate" hreflang="x-default" href="{BASE}">
<meta property="og:type" content="website">
<meta property="og:title" content="{escape(c['title'], quote=True)}">
<meta property="og:description" content="{escape(c['description'], quote=True)}">
<meta property="og:url" content="{BASE + path}">
<meta property="og:image" content="{BASE}img/atom_C.png">
<meta property="og:locale" content="{'it_IT' if language == 'it' else 'en_GB'}">
<meta name="twitter:card" content="summary_large_image">
<link rel="stylesheet" href="{prefix}web/portal.css">
<script type="application/ld+json">{json.dumps(schema, ensure_ascii=False)}</script>
</head>
<body>
<a class="skip" href="#explore">{'Vai ai percorsi' if language == 'it' else 'Skip to learning paths'}</a>
<header><a class="brand" href="{prefix}index.html">ATOM CUBE <span>FabLab Bergamo</span></a><nav aria-label="{'Navigazione principale' if language == 'it' else 'Main navigation'}"><a href="#explore">{c['nav']}</a><a href="{other}" lang="{other_language}" hreflang="{other_language}">{c['language']}</a></nav></header>
<main>
<section class="hero"><div><p class="eyebrow">{c['label']}</p><h1>{c['heading']}</h1><p class="intro">{c['intro']}</p><div class="actions"><a class="button" href="{filling}">{c['start']} →</a><a class="secondary" href="{viewer}">{c['view']}</a></div></div><figure><img src="{prefix}img/atom_C.png" alt="{c['caption']}" width="480" height="480" fetchpriority="high"><figcaption>{c['caption']}</figcaption></figure></section>
<section id="explore"><p class="eyebrow">{'OSSERVA · CONFRONTA · COMPRENDI' if language == 'it' else 'OBSERVE · COMPARE · UNDERSTAND'}</p><h2>{c['journey']}</h2><div class="paths">{cards}</div></section>
<section class="concepts"><h2>{c['terms']}</h2><div class="table-wrap"><table><thead><tr>{''.join(f'<th scope="col">{head}</th>' for head in c['tableheads'])}</tr></thead><tbody>{rows}</tbody></table></div><p>{c['order']}</p></section>
<section><h2>{c['examples']}</h2><ul class="examples">{examples}</ul></section>
<section class="guides"><h2>{c['math']}</h2><p>{c['mathintro']}</p><ol>{guides}</ol></section>
<section class="faq"><h2>{c['faq']}</h2>{questions}</section>
</main>
<footer><a href="https://github.com/PBrunot/electron-3d-display">{c['source']} →</a></footer>
</body>
</html>
'''


def outputs():
    english = (ROOT / 'web/shell_filling.html').read_text()
    italian = english
    for source, translated in TRANSLATIONS.items():
        if source not in italian:
            raise ValueError(f'Translation source missing: {source}')
        italian = italian.replace(source, translated)
    for attribute in ['rel="canonical" href=', 'property="og:url" content=']:
        italian = italian.replace(attribute + '"' + BASE + 'web/shell_filling.html"', attribute + '"' + BASE + 'web/shell_filling_it.html"')
    italian = italian.replace('<!doctype html>', '<!doctype html>\n<!-- Generated by tools/education_pages_gen.py from the English lesson; do not edit. -->', 1)
    paths = ['', 'it/', 'web/shell_filling.html', 'web/shell_filling_it.html', 'web/index.html', 'docs/coefficients_to_tables.html', 'docs/tables_to_bin.html', 'docs/tables_to_pointclouds.html']
    sitemap = '<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n' + ''.join(f'  <url><loc>{BASE + path}</loc></url>\n' for path in paths) + '</urlset>\n'
    return {'index.html': portal('en'), 'it/index.html': portal('it'), 'web/shell_filling_it.html': italian, 'sitemap.xml': sitemap, 'robots.txt': 'User-agent: *\nAllow: /\nSitemap: ' + BASE + 'sitemap.xml\n'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    stale = []
    for name, content in outputs().items():
        destination = ROOT / name
        if args.check:
            if not destination.exists() or destination.read_text() != content:
                stale.append(name)
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
    if stale:
        parser.exit(1, 'Regenerate educational pages: ' + ', '.join(stale) + '\n')
    print('Educational pages are current.' if args.check else 'Generated bilingual educational pages and sitemap.')


if __name__ == '__main__':
    main()
