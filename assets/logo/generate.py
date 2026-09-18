"""Regenerate the Interactive QuantLib Service logo set.

    python3 -m venv .venv && .venv/bin/pip install fonttools
    .venv/bin/python assets/logo/generate.py

Writes mark.svg, mark-dark.svg, logo.svg and logo-dark.svg beside this file.
The wordmark is outlined from Inter (SIL Open Font License 1.1) at generation
time, so the SVGs carry no font dependency and render the same wherever they
are shown -- a GitHub README, a browser tab, a Sphinx sidebar. The font is
downloaded to a cache on first run rather than checked in.

The mark is a Q. Its bowl is QuantLib; its tail is a short curve, as a price
curve is; and the tail ends in a handle, which is the quote a user drags and
the reason the service is interactive at all.
"""

import pathlib
import urllib.request

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

HERE = pathlib.Path(__file__).resolve().parent
CACHE = pathlib.Path.home() / ".cache" / "ql-logo"
INTER_URL = "https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf"

NAME = "Interactive QuantLib Service"

# Mantine's teal, which the frontend already uses as its primary colour: 7 on
# light grounds, 5 on dark ones, where 7 would sit too close to the ground.
TEAL_LIGHT, TEAL_DARK = "#0ca678", "#20c997"
HANDLE = "#fd7e14"                       # Mantine orange 6
INK, PAPER = "#1a1b1e", "#f1f3f5"        # Mantine dark 7, gray 1


def inter(weight):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / "Inter.ttf"
    if not path.exists():
        urllib.request.urlretrieve(INTER_URL, path)
    return instantiateVariableFont(TTFont(path), {"wght": weight, "opsz": 32})


def outline(font, text, size, x0, baseline):
    """One SVG path for `text`, and its advance, in user units."""
    scale = size / font["head"].unitsPerEm
    cmap, glyphs, hmtx = font.getBestCmap(), font.getGlyphSet(), font["hmtx"]
    pen, x = SVGPathPen(glyphs), 0.0
    for ch in text:
        glyph = cmap[ord(ch)]
        glyphs[glyph].draw(TransformPen(pen, (scale, 0, 0, -scale, x0 + x * scale, baseline)))
        x += hmtx[glyph][0]
    return pen.getCommands(), x * scale


def mark(teal):
    """The Q, drawn in a 64-unit box around a bowl centred at (29, 28)."""
    return (f'<circle cx="29" cy="28" r="18" fill="none" stroke="{teal}" stroke-width="6.5"/>'
            f'<path d="M34.5 33.5 Q41 42 51.5 43.5" fill="none" stroke="{teal}" '
            f'stroke-width="6.5" stroke-linecap="round"/>'
            f'<circle cx="52.5" cy="43.5" r="6.5" fill="{HANDLE}"/>')


def svg(width, height, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:g} {height:g}" '
            f'width="{width:g}" height="{height:g}" role="img" aria-label="{NAME}">'
            f'<title>{NAME}</title>{body}</svg>\n')


def square_mark(teal):
    # The drawing spans x 7.75..59 and y 6.75..50; centre it in its square so
    # a favicon or an avatar crop takes equal margins on every side.
    return svg(64, 64, f'<g transform="translate(-1.4 3.6)">{mark(teal)}</g>')


def lockup(teal, ink, regular, semibold):
    """Mark, then the name on one line, "QuantLib" set heavier than the rest.

    30-unit type on a 39-unit baseline centres Inter's cap height on the
    bowl, so the name sits level with the Q rather than with its tail.
    """
    x, words, width = 82, [], 0.0
    for text, font in (("Interactive ", regular), ("QuantLib", semibold), (" Service", regular)):
        d, advance = outline(font, text, 30, x + width, 39)
        words.append(f'<path d="{d}" fill="{ink}"/>')
        width += advance
    return svg(round(x + width + 4), 64, mark(teal) + "".join(words))


def main():
    regular, semibold = inter(430), inter(650)
    files = {
        "mark.svg": square_mark(TEAL_LIGHT),
        "mark-dark.svg": square_mark(TEAL_DARK),
        "logo.svg": lockup(TEAL_LIGHT, INK, regular, semibold),
        "logo-dark.svg": lockup(TEAL_DARK, PAPER, regular, semibold),
    }
    for name, text in files.items():
        (HERE / name).write_text(text, encoding="utf-8")
        print("wrote", HERE / name)


if __name__ == "__main__":
    main()
