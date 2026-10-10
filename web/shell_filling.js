/* Presentation is separate from the configuration model for headless validation. */
const {elements, configuration} = ShellFilling;
const text = ShellFillingLocale;
const byId = id => document.getElementById(id);
const colors = ['#31baff', '#ff9638', '#a477ff', '#32efb4'];
const atomView = ShellFillingAtom.createAtomView(byId('atom'), colors);
const requestedElement = Number(new URLSearchParams(location.search).get('z'));
let target = Number.isInteger(requestedElement) && requestedElement >= 1 && requestedElement <= elements.length ? requestedElement : 8;
let count = 0;
let timer = null;

elements.forEach(([symbol, name], index) => {
    const option = document.createElement('option');
    option.value = index + 1;
    option.textContent = `${symbol} · ${text.name(index)} (${index + 1})`;
    byId('element').append(option);
});
byId('element').value = target;

function pause() {
    clearInterval(timer);
    timer = null;
    byId('play').textContent = count === target ? text.replay : text.play;
}

function render(animate = false) {
    atomView.update(count, target, animate);
    const state = configuration(count);
    const previous = configuration(Math.max(0, count - 1));
    byId('orbitals').replaceChildren();
    let active = null;
    state.forEach((subshell, rowIndex) => {
        const row = document.createElement('div');
        row.className = 'row';
        row.style.setProperty('--shell', colors[subshell.n - 1]);
        const label = document.createElement('span');
        label.className = 'label';
        label.textContent = subshell.label;
        row.append(label);
        subshell.boxes.forEach((spins, boxIndex) => {
            const changed = spins !== previous[rowIndex].boxes[boxIndex];
            const box = document.createElement('span');
            box.className = 'box' + (changed ? ' active' : '');
            box.setAttribute('aria-label', `${subshell.label} ${text.orbital} ${boxIndex + 1}: ${spins.length} ${text.electrons}${spins.length === 2 ? text.opposite : ''}`);
            Array.from(spins).forEach((spin, spinIndex) => {
                const arrow = document.createElement('span');
                arrow.textContent = spin;
                if (animate && changed && spinIndex === spins.length - 1) arrow.className = 'new';
                box.append(arrow);
            });
            row.append(box);
            if (changed) active = {subshell, paired: spins.length === 2};
        });
        byId('orbitals').append(row);
    });
    byId('configuration').replaceChildren();
    state.filter(s => s.occupancy).forEach(s => {
        const segment = document.createElement('span');
        segment.style.color = colors[s.n - 1];
        segment.append(s.label);
        const superscript = document.createElement('sup');
        superscript.textContent = s.occupancy;
        segment.append(superscript, ' ');
        byId('configuration').append(segment);
    });
    for (let n = 1; n <= 4; ++n) {
        const total = state.filter(s => s.n === n).reduce((sum, s) => sum + s.occupancy, 0);
        let shell = byId('shells').children[n - 1];
        if (!shell) {
            shell = document.createElement('div');
            shell.className = 'shell';
            shell.style.setProperty('--shell', colors[n - 1]);
            const label = document.createElement('div');
            const track = document.createElement('div');
            track.className = 'track';
            const fill = document.createElement('div');
            fill.className = 'fill';
            track.append(fill);
            shell.append(label, track);
            byId('shells').append(shell);
        }
        shell.children[0].textContent = `${'KLMN'[n - 1]} · n = ${n}: ${total} / ${2 * n * n}`;
        const fill = shell.children[1].children[0];
        fill.style.width = `${100 * total / (2 * n * n)}%`;
    }
    const [symbol, name] = elements[target - 1];
    const languageLink = document.querySelector('nav a[hreflang]');
    if (languageLink) {
        const languageUrl = new URL(languageLink.href);
        languageUrl.searchParams.set('z', target);
        languageLink.href = languageUrl.href;
    }
    byId('progress').max = target;
    byId('progress').value = count;
    byId('progress-label').textContent = `${text.name(target - 1)} (${symbol}) · ${text.placed}: ${count} / ${target}`;
    let explanation = text.begin;
    if (active) {
        const rule = active.paired ? text.pauli : active.subshell.orbitals > 1 ? text.hund : text.aufbau;
        explanation = `${text.entry(count, active.subshell.label)} ${rule}`;
        if (count === target) explanation += ` ${text.complete(symbol)}`;
    }
    byId('explanation').textContent = explanation;
    byId('back').disabled = count === 0;
    byId('next').disabled = count === target;
    byId('play').textContent = timer ? text.pause : count === target ? text.replay : text.play;
}

function play() {
    if (timer) { pause(); return; }
    if (count === target) { count = 0; render(); }
    timer = setInterval(() => {
        count += 1;
        if (count === target) pause();
        render(true);
    }, Number(byId('speed').value));
    byId('play').textContent = text.pause;
}

byId('play').onclick = play;
byId('next').onclick = () => { pause(); count = Math.min(target, count + 1); render(true); };
byId('back').onclick = () => { pause(); count = Math.max(0, count - 1); render(); };
byId('reset').onclick = () => { pause(); count = 0; render(); };
byId('progress').oninput = event => { pause(); count = Number(event.target.value); render(); };
byId('element').onchange = event => { pause(); target = Number(event.target.value); count = 0; render(); };
byId('speed').onchange = () => { if (timer) { pause(); play(); } };
byId('rotation').onchange = event => atomView.setRotation(event.target.checked);
byId('density').oninput = event => {
    const percent = Number(event.target.value);
    atomView.setDensity(percent);
    byId('density-value').textContent = `${percent}%`;
};
document.addEventListener('visibilitychange', () => { if (document.hidden) pause(); });
render();
