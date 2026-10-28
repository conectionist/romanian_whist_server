#!/usr/bin/env python3
"""Rebuild docs/web-client-mockup/web_client_mockup.html from its sources.

A mock-up of the browser client planned in docs/web-client-plan.md. Like the desktop
client's table redesign mock-up (its docs/ui-redesign/mockup/), the built page is one
self-contained file that opens in any browser, so it embeds its pictures as data
URIs. The readable parts live here:

    page.html   markup and CSS
    mockup.js   the screens: geometry, sample states, the little interaction there is

and this script stitches them together with the art, taken straight from the desktop
client's resources so the mock-up and the desktop client use the same pictures:

    resources/images/cards/png/<rank>_of_<suit>.png   only the cards the screens show
    resources/images/avatars/*                         the portrait catalogue
    resources/images/table/table_wood.png, table_felt.png

    python3 docs/web-client-mockup/source/build_mockup.py [--desktop PATH] [--body-only OUT]

--desktop names the desktop client's checkout. Without it, the script looks for a
romanian_whist_desktop checkout beside this one.

--body-only writes the page without its <!doctype>/<head> lines, for a host that
wraps a page in a document of its own.

Needs Pillow (for the WebP re-encode that keeps the page small). The fonts come
from Google Fonts, as the redesign mock-up's do, so they need a connection.
"""

import argparse
import base64
import io
import json
import pathlib
import sys

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = HERE.parent / 'web_client_mockup.html'

HEAD = ('<!doctype html>\n<html lang="en">\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n')

RANK_FILES = {'3': '3', '4': '4', '5': '5', '6': '6', '7': '7', '8': '8', '9': '9', 'T': '10',
              'J': 'jack', 'Q': 'queen', 'K': 'king', 'A': 'ace'}
SUIT_FILES = {'C': 'clubs', 'D': 'diamonds', 'H': 'hearts', 'S': 'spades'}

# The cards the screens put on the table, in a hand or as the trump. Keeping the
# list short keeps the page small; add a card here when a screen needs it.
CARDS = ['AH', 'TH', '7H', 'JC', 'QD', '8D', '9D', 'KD', '6D', '7D', 'TS', 'KS']


def find_desktop(given):
    candidates = [pathlib.Path(given)] if given else [ROOT.parent / 'romanian_whist_desktop']
    for candidate in candidates:
        if (candidate / 'resources' / 'images' / 'cards' / 'png').is_dir():
            return candidate.resolve()
    sys.exit('No desktop client checkout found (looked in: '
             + ', '.join(str(c) for c in candidates) + '). Pass --desktop PATH.')


def webp_uri(image, quality):
    buffer = io.BytesIO()
    image.save(buffer, 'WEBP', quality=quality, method=6)
    return 'data:image/webp;base64,' + base64.b64encode(buffer.getvalue()).decode()


def card_images(desktop):
    folder = desktop / 'resources' / 'images' / 'cards' / 'png'
    cards = {}
    for code in CARDS:
        image = Image.open(folder / f'{RANK_FILES[code[0]]}_of_{SUIT_FILES[code[1]]}.png').convert('RGBA')
        image = image.resize((320, round(320 * image.height / image.width)), Image.LANCZOS)
        cards[code] = webp_uri(image, 86)
    return cards


def avatar_images(desktop):
    folder = desktop / 'resources' / 'images' / 'avatars'
    catalogue = json.loads((folder / 'avatars.json').read_text())
    avatars = {}
    for entry in catalogue:
        path = folder / entry['file']
        if path.suffix == '.svg':
            uri = 'data:image/svg+xml;base64,' + base64.b64encode(path.read_bytes()).decode()
        else:
            # Twice the 88 x 100 portrait box, for sharp portraits on a high-density screen.
            image = Image.open(path).convert('RGB')
            image = image.resize((176, round(176 * image.height / image.width)), Image.LANCZOS)
            uri = webp_uri(image, 82)
        avatars[entry['id']] = {'gender': entry['gender'], 'strategy': entry.get('strategy'), 'src': uri}
    return avatars


def main():
    parser = argparse.ArgumentParser(description='Rebuild the web client mock-up.')
    parser.add_argument('--desktop', help="the desktop client's checkout, for its art")
    parser.add_argument('--body-only', metavar='OUT', help='write the page without its head, to OUT')
    args = parser.parse_args()

    desktop = find_desktop(args.desktop)
    textures = desktop / 'resources' / 'images' / 'table'
    wood = webp_uri(Image.open(textures / 'table_wood.png'), 86)
    felt = webp_uri(Image.open(textures / 'table_felt.png'), 92)

    page = (HERE / 'page.html').read_text()
    script = (HERE / 'mockup.js').read_text()

    art = ('const CARD_IMG=' + json.dumps(card_images(desktop)) + ';\n'
           'const AVATARS=' + json.dumps(avatar_images(desktop)) + ';\n'
           f'const WOOD_TEX="{wood}";\nconst FELT_TEX="{felt}";\n')

    body = page + '<script>\n' + art + '</script>\n<script>\n' + script + '</script>\n'

    if args.body_only:
        out = pathlib.Path(args.body_only)
        out.write_text(body)
    else:
        out = OUT
        out.write_text(HEAD + body)
    print(f'wrote {out} ({out.stat().st_size // 1024} KB), art from {desktop}')


if __name__ == '__main__':
    main()
