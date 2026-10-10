"""Assemble GitHub Pages and add a static, attributable article link to every page.

Usage: python3 tools/assemble_web_site.py /tmp/atom-cube-site
Output must be empty. No source documents or generated physics data are changed.
"""
import argparse
from html import escape
from pathlib import Path
import re
import shutil
from urllib.parse import quote, urlencode, urlsplit

ROOT = Path(__file__).resolve().parents[1]
ARTICLE = 'https://www.fablabbergamo.it/2026/09/20/orbitali-elettronici-esp32/'
SOURCE = 'https://github.com/PBrunot/electron-3d-display/blob/master/'


def resolve_source_links(page, file, destination):
    # Calculation guides also link to source scripts that are not site assets.
    def replace(match):
        url = match.group(2)
        parsed = urlsplit(url)
        if parsed.scheme or parsed.netloc or not parsed.path:
            return match.group(0)
        target = (file.parent / parsed.path).resolve()
        if not target.is_relative_to(destination.resolve()):
            raise ValueError('Local link escapes the site: ' + url)
        if target.exists():
            return match.group(0)
        relative = target.relative_to(destination.resolve())
        if not (ROOT / relative).is_file():
            raise ValueError('Broken local link: ' + str(file) + ' -> ' + url)
        resolved = SOURCE + quote(relative.as_posix())
        if parsed.fragment:
            resolved += '#' + parsed.fragment
        return 'href=' + match.group(1) + resolved + match.group(1)
    return re.sub(r'href=([\"\'])([^\"\']+)\1', replace, page)


def article_footer(page, relative):
    italian = bool(re.search(r'<html\b[^>]*\blang=[\"\']it[\"\']', page, re.I))
    language = 'it' if italian else 'en'
    content = relative.as_posix().removesuffix('.html').replace('/', '-')
    url = ARTICLE + '?' + urlencode({
        'utm_source': 'github_pages', 'utm_medium': 'referral',
        'utm_campaign': 'electron_orbitals', 'utm_content': content + '-' + language,
    })
    label = 'Leggi l’articolo originale del FabLab Bergamo' if italian else 'Read the original FabLab Bergamo article (Italian)'
    return f'''<aside id="fablab-article" aria-label="FabLab Bergamo" style="clear:both;max-width:1080px;margin:32px auto;padding:20px;border-top:1px solid #465367;font:16px/1.6 system-ui,sans-serif;text-align:center">
<a href="{escape(url, quote=True)}" style="color:#6dcaff;text-decoration:underline;text-underline-offset:4px">{label} →</a>
</aside>'''


def assemble(destination):
    if destination.exists() and any(destination.iterdir()):
        raise ValueError('The output directory must be empty: ' + str(destination))
    destination.mkdir(parents=True, exist_ok=True)
    for folder in ['web', 'micropython', 'pc', 'docs', 'it']:
        shutil.copytree(ROOT / folder, destination / folder,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.test.js'))
    for filename in ['index.html', 'sitemap.xml', 'robots.txt']:
        shutil.copy2(ROOT / filename, destination / filename)
    (destination / 'img').mkdir()
    shutil.copy2(ROOT / 'img/atom_C.png', destination / 'img/atom_C.png')
    (destination / '.nojekyll').touch()
    pages = list(destination.rglob('*.html'))
    for path in pages:
        page = path.read_text()
        page = resolve_source_links(page, path, destination)
        footer = article_footer(page, path.relative_to(destination))
        if '</body>' not in page:
            raise ValueError('Missing body end in ' + str(path))
        path.write_text(page.replace('</body>', footer + '\n</body>', 1))
    print(f'Assembled {len(pages)} HTML pages with page-specific FabLab article attribution.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    assemble(args.destination)
