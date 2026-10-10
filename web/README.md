# Browser viewers

The repository root is now an educational portal: `index.html` (English) and
`it/index.html` (Italian). Both link to the filling lesson, the 3D viewer and
the calculation guides. `shell_filling_it.html` translates the lesson and its
controls, explanations, canvas labels and element names. Links such as
`shell_filling_it.html?z=8` select an example and retain it when switching language.

Regenerate the Italian lesson, portals and sitemap with
`python3 tools/education_pages_gen.py`; use `--check` to check they are current.
Edit the English lesson and the generator’s translations rather than generated HTML.
Canonical URLs, reciprocal language alternatives, page descriptions, social
metadata and static learning content make both languages discoverable without JavaScript.

Assemble the published site with `python3 tools/assemble_web_site.py /tmp/atom-cube-site`
(choose an empty output directory). This also adds a visible, static link to the
[original FabLab Bergamo article](https://www.fablabbergamo.it/2026/09/20/orbitali-elettronici-esp32/)
to every HTML page, including all calculation guides. Links use
`utm_source=github_pages`, `utm_medium=referral`, `utm_campaign=electron_orbitals`,
and a page/language-specific `utm_content`. FabLab’s analytics can attribute
arrivals using these tags if it records campaign parameters. No analytics script
or tracking request is added to the GitHub Pages site.
The Pages workflow uses this assembler, so future HTML pages receive the link too.

Serve the repository root with `python3 -m http.server 8000`, then open
`http://localhost:8000/web/index.html` for the atom/orbital viewer or
`http://localhost:8000/web/shell_filling.html` for the educational filling animation.
The filling page also works directly from a local file, without downloads or dependencies.

The filling animation covers neutral ground-state configurations H–Ca.
Choose an element, play/pause, step forward/backward, or scrub the electron count.
Orbital boxes show Hund filling and opposite-spin pairing; shell bars sum
subshell occupancies. A synchronized, rotating atom cloud adds one contribution
per electron, with matching shell colors. Its
spheres and p lobes use schematic radial sampling, not the calibrated physics
of the main atom viewer; dots are probability samples, not electron particles.
The vibrant cloud rotates independently of filling playback; use Rotate atom
to stop rotation. Points adjusts the cloud density.
Reduced-motion mode disables rotation, point turnover, and arrivals.
Nitrogen/oxygen illustrate the onset of p pairing;
potassium/calcium illustrate 4s filling before 3d. Reduced-motion preferences
disable visual transitions. Changing element resets the construction.

This is an instructional configuration-building sequence, not an ionization
process or a physical electron trajectory. It uses schematic box spacing,
not numerical orbital energies. The H–Ca scope avoids transition-metal
ground-state exceptions; the existing atom viewer covers the wider range.
Physics reference: [OpenStax, electronic structure of atoms](https://openstax.org/books/chemistry-atoms-first-2e/pages/3-4-electronic-structure-of-atoms-electron-configurations).

Run the model checks with `node web/shell_filling_model.test.js`.
The existing Pages workflow copies these assets with the rest of `web/`.
