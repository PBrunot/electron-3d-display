// Single-page phone UI served by net/web_remote.cpp at "/". Hand-written (not generated):
// plain HTML/CSS/JS, no external resources -- the phone is on the board's own access point
// with no internet, so a CDN link would never load. Element/orbital lists are NOT duplicated
// here: the page fetches them from /api/catalog, built on-device from the same tables the
// viewers use (slater.h, element_names_it.h, periodic_grid.h, orbital_library.h).
//
// API used by the script below (see web_remote.cpp):
//   GET  /api/catalog  {"maxZ":92,"elements":[[z,"Fe","Ferro",row,col],...],"orbitals":[["3dxy",n,l,m],...]}
//   GET  /api/state    {"mode":"menu"|"element"|"orbital","index":N,"pending":bool}
//   POST /api/cmd?c=element|orbital|next|prev|dissect|menu[&v=N]
#pragma once

inline constexpr char kWebRemotePage[] = R"HTML(<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0a0e1a">
<title>Ologramma Atomi</title>
<style>
:root{
  --bg:#0a0e1a;--panel:#131a2c;--panel2:#1b2440;--line:#26304f;--text:#e8ecf8;--dim:#8d97b5;
  --accent:#ff9a3c;--accent2:#4da3ff;--ok:#3ddc97;
  --s:#ff6b6b;--p:#ffc34d;--d:#4da3ff;--f:#3ddc97;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;background:var(--bg);color:var(--text);
  font:16px/1.35 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
body{padding-bottom:calc(92px + env(safe-area-inset-bottom))}
header{position:sticky;top:0;z-index:5;background:linear-gradient(var(--bg) 70%,transparent);
  padding:14px 16px 6px}
.brand{font-weight:700;letter-spacing:.02em;font-size:18px;display:flex;align-items:center;gap:8px}
.brand svg{width:26px;height:26px}
.now{margin-top:8px;display:flex;align-items:center;gap:10px;background:var(--panel);
  border:1px solid var(--line);border-radius:14px;padding:10px 12px;min-height:58px}
.now .big{font-size:28px;font-weight:800;min-width:48px;text-align:center;color:var(--accent)}
.now .lbl{font-size:12px;color:var(--dim);text-transform:uppercase;letter-spacing:.08em}
.now .val{font-size:16px;font-weight:600}
.dot{width:9px;height:9px;border-radius:50%;background:var(--dim);margin-left:auto;flex:none}
.dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.tabs{display:flex;gap:6px;margin:10px 16px 0;background:var(--panel);border-radius:12px;padding:4px;
  border:1px solid var(--line)}
.tabs button{flex:1;border:0;border-radius:9px;padding:10px;background:transparent;color:var(--dim);
  font:inherit;font-weight:600}
.tabs button.on{background:var(--panel2);color:var(--text)}
section{padding:12px 16px}
input[type=search]{width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);
  background:var(--panel);color:var(--text);font:inherit;outline:none}
input[type=search]:focus{border-color:var(--accent2)}
.hits{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}
.hits:empty{display:none}
.chip{border:1px solid var(--line);background:var(--panel2);color:var(--text);border-radius:999px;
  padding:8px 12px;font:inherit;font-size:14px}
.chip b{color:var(--accent);margin-right:6px}
.wrap{overflow-x:auto;margin:12px -16px 0;padding:0 16px 8px;scroll-behavior:smooth}
.pt{display:grid;grid-template-columns:repeat(18,40px);grid-auto-rows:46px;gap:3px;width:max-content}
.el{border:0;border-radius:7px;padding:3px 0 0;font:inherit;color:var(--text);display:flex;
  flex-direction:column;align-items:center;justify-content:flex-start;position:relative;
  background:color-mix(in srgb,var(--c) 22%,var(--panel));transition:transform .08s}
.el:active{transform:scale(.92)}
.el .z{font-size:9px;color:var(--dim);line-height:1}
.el .sy{font-size:16px;font-weight:700;line-height:1.25}
.el.cur{outline:2px solid var(--accent);background:color-mix(in srgb,var(--accent) 40%,var(--panel))}
.gap{grid-column:1/-1;height:6px}
.legend{display:flex;gap:12px;flex-wrap:wrap;font-size:12px;color:var(--dim);margin-top:6px}
.legend i{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:5px;vertical-align:-1px}
.hint{font-size:12px;color:var(--dim);margin:4px 0 0}
.shell{margin:4px 0 16px}
.shell h3{margin:0 0 8px;font-size:13px;color:var(--dim);font-weight:600;text-transform:uppercase;
  letter-spacing:.08em}
.orbs{display:grid;grid-template-columns:repeat(auto-fill,minmax(92px,1fr));gap:8px}
.orb{border:1px solid var(--line);background:var(--panel);color:var(--text);border-radius:12px;
  padding:10px 6px;font:inherit;text-align:center;transition:transform .08s}
.orb:active{transform:scale(.95)}
.orb .nm{font-size:22px;font-weight:700}
.orb .nm sub{font-size:13px;font-weight:600;color:var(--accent2)}
.orb .qn{font-size:11px;color:var(--dim);margin-top:2px}
.orb.cur{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 22%,var(--panel))}
.dock{position:fixed;left:0;right:0;bottom:0;z-index:6;display:grid;grid-template-columns:1fr 1.4fr 1.4fr 1fr;
  gap:8px;padding:10px 12px calc(10px + env(safe-area-inset-bottom));background:rgba(10,14,26,.92);
  border-top:1px solid var(--line);backdrop-filter:blur(8px)}
.dock button{border:1px solid var(--line);background:var(--panel2);color:var(--text);border-radius:14px;
  padding:14px 6px;font:inherit;font-weight:700;font-size:15px}
.dock button:disabled{opacity:.35}
.dock .pri{background:var(--accent);border-color:var(--accent);color:#1a1000}
.toast{position:fixed;left:50%;bottom:calc(96px + env(safe-area-inset-bottom));transform:translate(-50%,20px);
  background:var(--panel2);border:1px solid var(--line);padding:10px 16px;border-radius:999px;font-size:14px;
  opacity:0;transition:.2s;pointer-events:none;white-space:nowrap}
.toast.show{opacity:1;transform:translate(-50%,0)}
.err{color:var(--s)}
</style>
</head>
<body>
<header>
  <div class="brand">
    <svg viewBox="0 0 32 32" fill="none" stroke-width="1.6"><circle cx="16" cy="16" r="2.6" fill="#ff9a3c"/>
      <ellipse cx="16" cy="16" rx="13" ry="5" stroke="#4da3ff"/>
      <ellipse cx="16" cy="16" rx="13" ry="5" stroke="#4da3ff" transform="rotate(60 16 16)"/>
      <ellipse cx="16" cy="16" rx="13" ry="5" stroke="#4da3ff" transform="rotate(120 16 16)"/></svg>
    Ologramma Atomi
  </div>
  <div class="now">
    <div class="big" id="nowBig">&middot;</div>
    <div><div class="lbl" id="nowLbl">Connessione&hellip;</div><div class="val" id="nowVal"></div></div>
    <div class="dot" id="dot"></div>
  </div>
</header>

<div class="tabs">
  <button data-tab="el" class="on">Elementi</button>
  <button data-tab="orb">Orbitali</button>
</div>

<section id="el">
  <input id="q" type="search" placeholder="Cerca: ferro, Fe, 26&hellip;" autocomplete="off">
  <div class="hits" id="hits"></div>
  <div class="wrap" id="wrap"><div class="pt" id="pt"></div></div>
  <div class="legend">
    <span><i style="background:var(--s)"></i>blocco s</span><span><i style="background:var(--p)"></i>blocco p</span>
    <span><i style="background:var(--d)"></i>blocco d</span><span><i style="background:var(--f)"></i>blocco f</span>
  </div>
  <p class="hint">Tocca un elemento per vederlo nell'ologramma. Scorri la tavola di lato &harr;</p>
</section>

<section id="orb" hidden>
  <div id="orbs"></div>
  <p class="hint">Arancio e blu indicano il segno della funzione d'onda (&psi; &gt; 0 / &psi; &lt; 0).</p>
</section>

<nav class="dock">
  <button id="prev" aria-label="Precedente">&#9664;</button>
  <button id="dissect" class="pri">Seziona</button>
  <button id="menu">Menu</button>
  <button id="next" aria-label="Successivo">&#9654;</button>
</nav>
<div class="toast" id="toast"></div>

<script>
"use strict";
const $ = s => document.querySelector(s);
let cat = null, state = {mode: "menu", index: 0}, holdUntil = 0, toastTimer = 0;
const ELL = "spdf";
const F_SUB = {"xx2-3y2": "x(x²−3y²)", "y3x2-y2": "y(3x²−y²)", "zx2-y2": "z(x²−y²)"};

function toast(msg, isErr) {
  const t = $("#toast");
  t.textContent = msg;
  t.className = "toast show" + (isErr ? " err" : "");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.className = "toast", 1800);
}

function block(z, row, col) {
  if (row >= 8) return "f";
  if (z === 2 || col <= 2) return "s";
  return col >= 13 ? "p" : "d";
}

function orbHtml(label) {
  const head = label.slice(0, 2), sub = label.slice(2);
  const pretty = F_SUB[sub] || sub.replace(/([a-z])2/g, "$1²").replace(/-/g, "−");
  return head + (pretty ? "<sub>" + pretty + "</sub>" : "");
}

function buildTable() {
  const pt = $("#pt");
  let html = "";
  for (const [z, sym, name, row, col] of cat.elements) {
    const gridRow = row >= 8 ? row + 1 : row; // one spacer row before lanthanides/actinides
    html += `<button class="el" data-z="${z}" style="grid-row:${gridRow};grid-column:${col};` +
            `--c:var(--${block(z, row, col)})" title="${name}"><span class="z">${z}</span>` +
            `<span class="sy">${sym}</span></button>`;
  }
  html += '<div class="gap" style="grid-row:8"></div>';
  pt.innerHTML = html;
  pt.addEventListener("click", e => {
    const b = e.target.closest(".el");
    if (b) showElement(+b.dataset.z);
  });
}

function buildOrbitals() {
  const byN = {};
  cat.orbitals.forEach(([label, n, l, m], i) => (byN[n] = byN[n] || []).push({label, n, l, m, i}));
  let html = "";
  for (const n of Object.keys(byN).sort()) {
    html += `<div class="shell"><h3>Livello n = ${n}</h3><div class="orbs">`;
    for (const o of byN[n])
      html += `<button class="orb" data-i="${o.i}"><div class="nm">${orbHtml(o.label)}</div>` +
              `<div class="qn">ℓ=${o.l} · m=${String(o.m).replace("-", "−")}</div></button>`;
    html += "</div></div>";
  }
  const box = $("#orbs");
  box.innerHTML = html;
  box.addEventListener("click", e => {
    const b = e.target.closest(".orb");
    if (b) showOrbital(+b.dataset.i);
  });
}

function elementByZ(z) { return cat.elements[z - 1]; }

// Keeps an optimistic highlight from being overwritten by a /api/state poll that races the
// hologram actually picking the request up.
function holdHighlight() { holdUntil = Date.now() + 4000; }

async function send(c, v) {
  try {
    const r = await fetch(`/api/cmd?c=${c}` + (v !== undefined ? `&v=${v}` : ""), {method: "POST"});
    if (!r.ok) throw new Error(r.status);
    return true;
  } catch (e) {
    toast("Ologramma non raggiungibile", true);
    return false;
  }
}

async function showElement(z) {
  const [, sym, name] = elementByZ(z);
  render({mode: "element", index: z});
  holdHighlight();
  if (await send("element", z)) toast(`Mostro ${name} (${sym})`);
}

async function showOrbital(i) {
  render({mode: "orbital", index: i});
  holdHighlight();
  if (await send("orbital", i)) toast("Mostro l'orbitale " + cat.orbitals[i][0]);
}

function render(s) {
  state = s;
  document.querySelectorAll(".cur").forEach(e => e.classList.remove("cur"));
  let big = "·", lbl = "Menu", val = "Scegli un elemento o un orbitale";
  if (s.mode === "element" && cat) {
    const [z, sym, name] = elementByZ(s.index);
    big = sym; lbl = "Elemento · Z " + z; val = name;
    const b = document.querySelector(`.el[data-z="${z}"]`);
    if (b) b.classList.add("cur");
  } else if (s.mode === "orbital" && cat) {
    const [label, n, l, m] = cat.orbitals[s.index];
    big = ""; lbl = "Orbitale"; val = `n=${n} ℓ=${l} m=${m}`;
    $("#nowBig").innerHTML = orbHtml(label);
    const b = document.querySelector(`.orb[data-i="${s.index}"]`);
    if (b) b.classList.add("cur");
  }
  if (big) $("#nowBig").textContent = big;
  $("#nowLbl").textContent = lbl;
  $("#nowVal").textContent = val;
  const inView = s.mode !== "menu";
  $("#prev").disabled = $("#next").disabled = $("#menu").disabled = !inView;
  $("#dissect").disabled = s.mode !== "element";
}

function scrollToCurrent() {
  const b = document.querySelector(".el.cur");
  if (b) $("#wrap").scrollLeft = b.offsetLeft - $("#wrap").clientWidth / 2 + 20;
}

async function poll() {
  try {
    const r = await fetch("/api/state", {cache: "no-store"});
    const s = await r.json();
    $("#dot").classList.add("on");
    if (Date.now() > holdUntil && (s.mode !== state.mode || s.index !== state.index)) render(s);
  } catch (e) {
    $("#dot").classList.remove("on");
  }
  setTimeout(poll, 1500);
}

function selectTab(name) {
  document.querySelectorAll(".tabs button").forEach(b => b.classList.toggle("on", b.dataset.tab === name));
  $("#el").hidden = name !== "el";
  $("#orb").hidden = name !== "orb";
  if (name === "el") scrollToCurrent();
}

$(".tabs").addEventListener("click", e => { if (e.target.dataset.tab) selectTab(e.target.dataset.tab); });
$("#prev").onclick = () => send("prev").then(ok => ok && toast("Precedente"));
$("#next").onclick = () => send("next").then(ok => ok && toast("Successivo"));
$("#menu").onclick = () => { render({mode: "menu", index: 0}); holdHighlight(); send("menu").then(ok => ok && toast("Torno al menu")); };
$("#dissect").onclick = () => send("dissect").then(ok => ok && toast("Sezione dei gusci elettronici"));

$("#q").addEventListener("input", e => {
  const q = e.target.value.trim().toLowerCase();
  const hits = $("#hits");
  if (!q || !cat) { hits.innerHTML = ""; return; }
  const found = cat.elements.filter(([z, sym, name]) =>
    String(z) === q || sym.toLowerCase() === q || name.toLowerCase().startsWith(q)).slice(0, 8);
  hits.innerHTML = found.length
    ? found.map(([z, sym, name]) => `<button class="chip" data-z="${z}"><b>${sym}</b>${name}</button>`).join("")
    : '<span class="hint">Nessun elemento trovato</span>';
});
$("#hits").addEventListener("click", e => {
  const b = e.target.closest(".chip");
  if (!b) return;
  showElement(+b.dataset.z);
  $("#q").value = ""; $("#hits").innerHTML = "";
  scrollToCurrent();
});

(async function init() {
  for (;;) {
    try { cat = await (await fetch("/api/catalog")).json(); break; }
    catch (e) { await new Promise(r => setTimeout(r, 1500)); }
  }
  buildTable();
  buildOrbitals();
  try { render(await (await fetch("/api/state")).json()); } catch (e) { render(state); }
  if (state.mode === "orbital") selectTab("orb"); else scrollToCurrent();
  poll();
})();
</script>
</body>
</html>
)HTML";
