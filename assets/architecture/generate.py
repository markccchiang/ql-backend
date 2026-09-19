"""Regenerate the architecture figure in the README.

    python3 -m venv .venv && .venv/bin/pip install fonttools
    .venv/bin/python assets/architecture/generate.py

Writes architecture.svg and architecture-dark.svg beside this file. Like the
logo (assets/logo/generate.py, whose font cache and colours this shares), the
type is outlined from Inter at generation time, so the figure renders the same
in a GitHub README as anywhere else and carries no font dependency.

What it shows, left to right: the clients, which hold the document; one
ql-backend process, whose event loop runs the gateway and the supervisor; and
the worker threads, one live QuantLib graph each. The session in front shows
why the service is interactive: a dragged quote reprices along its own path
and nothing else is rebuilt.
"""

import pathlib
import re
import sys
from dataclasses import dataclass

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "logo"))
from fontTools.pens.svgPathPen import SVGPathPen  # noqa: E402
from generate import HANDLE, TEAL_DARK, TEAL_LIGHT, inter, mark  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
WIDTH, HEIGHT = 1000, 462
TITLE = "ql-backend architecture"
DESCRIPTION = ("Clients exchange Protobuf frames with ql-backend over a WebSocket. Inside one "
               "process, an event loop runs the gateway and the supervisor, which route each "
               "session to a worker thread holding its own live QuantLib graph; moving one quote "
               "reprices only what depends on it.")


@dataclass(frozen=True)
class Theme:
    """Mantine's palette, as the logo and the frontend use it."""
    teal: str
    teal_soft: str
    handle: str
    handle_soft: str
    ink: str
    muted: str
    line: str
    panel: str
    card: str
    edge: str


LIGHT = Theme(teal=TEAL_LIGHT, teal_soft="#e6fcf5", handle=HANDLE, handle_soft="#fff4e6",
              ink="#1a1b1e", muted="#5c5f66", line="#dee2e6", panel="#f8f9fa",
              card="#ffffff", edge="#adb5bd")
DARK = Theme(teal=TEAL_DARK, teal_soft="#0b3b30", handle="#ff922b", handle_soft="#3d2410",
             ink="#f1f3f5", muted="#a6a7ab", line="#373a40", panel="#1f2023",
             card="#25262b", edge="#5c5f66")


def num(value):
    return f"{value:.2f}".rstrip("0").rstrip(".")


class Figure:
    """Type is set from glyphs defined once, in font units, and placed with
    <use>: outlining every character separately made each file 850 KB."""

    def __init__(self, theme, regular, semibold):
        self.t, self.fonts = theme, {False: regular, True: semibold}
        self.parts, self.glyphs = [], {}

    # -- type -------------------------------------------------------------
    def glyph(self, bold, ch):
        font = self.fonts[bold]
        name = font.getBestCmap()[ord(ch)]
        key = ("b" if bold else "r") + str(len(self.glyphs))
        if (bold, name) not in self.glyphs:
            pen = SVGPathPen(font.getGlyphSet())
            font.getGlyphSet()[name].draw(pen)
            d = re.sub(r"-?\d+\.\d+", lambda m: num(float(m.group())), pen.getCommands())
            self.glyphs[(bold, name)] = (key, d)
        return self.glyphs[(bold, name)][0], font["hmtx"][name][0]

    def width_of(self, text, size, bold=False, tracking=0.0):
        scale = size / self.fonts[bold]["head"].unitsPerEm
        return sum(self.glyph(bold, ch)[1] for ch in text) * scale + tracking * max(len(text) - 1, 0)

    def text(self, text, x, y, size, colour, bold=False, anchor="start", tracking=0.0):
        scale = size / self.fonts[bold]["head"].unitsPerEm
        width = self.width_of(text, size, bold, tracking)
        x = {"start": x, "middle": x - width / 2, "end": x - width}[anchor]
        uses = []
        for ch in text:
            key, advance = self.glyph(bold, ch)
            if ch != " ":
                uses.append(f'<use href="#{key}" x="{num(x / scale)}"/>')
            x += advance * scale + tracking
        self.parts.append(f'<g fill="{colour}" transform="translate(0 {num(y)}) '
                          f'scale({scale:.6g} {-scale:.6g})">' + "".join(uses) + "</g>")

    def label(self, text, x, y):
        """A small-caps section label."""
        self.text(text.upper(), x, y, 10.5, self.t.muted, bold=True, tracking=1.1)

    # -- shapes -----------------------------------------------------------
    def box(self, x, y, w, h, fill, stroke, radius=10, width=1.0, dash=None):
        extra = f' stroke-dasharray="{dash}"' if dash else ""
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" '
                          f'fill="{fill}" stroke="{stroke}" stroke-width="{width}"{extra}/>')

    def card(self, x, y, w, h, accent=None):
        self.box(x, y, w, h, self.t.card, self.t.line)
        if accent:
            # A bar along the top edge, clipped to the card's rounded corners.
            self.parts.append(f'<path d="M{x} {y + 10} a10 10 0 0 1 10 -10 h{w - 20} '
                              f'a10 10 0 0 1 10 10 v-6 h-{w} z" fill="{accent}"/>')

    def pill(self, text, x, y, fg, bg):
        w = self.width_of(text, 10.5, bold=True) + 16
        self.box(x - w, y - 13, w, 19, bg, bg, radius=9.5)
        self.text(text, x - w / 2, y + 0.5, 10.5, fg, bold=True, anchor="middle")

    def line(self, d, colour, width=1.4, dash=None, start=False, end=False):
        extra = f' stroke-dasharray="{dash}"' if dash else ""
        ends = (f' marker-start="url(#{self.marker(colour)})"' if start else "") + \
               (f' marker-end="url(#{self.marker(colour)})"' if end else "")
        self.parts.append(f'<path d="{d}" fill="none" stroke="{colour}" stroke-width="{width}" '
                          f'stroke-linecap="round" stroke-linejoin="round"{extra}{ends}/>')

    def marker(self, colour):
        return "arrow-" + colour.lstrip("#")

    def bullets(self, x, y, lines):
        for i, text in enumerate(lines):
            cy = y + i * 23
            self.parts.append(f'<circle cx="{x + 3}" cy="{cy - 4.5}" r="2.6" fill="{self.t.teal}"/>')
            self.text(text, x + 14, cy, 13, self.t.ink)

    # -- the figure -------------------------------------------------------
    def clients(self):
        t = self.t
        self.label("Clients", 24, 104)
        for y, title, sub in ((116, "Workbench", "ql-frontend, in a browser"),
                              (210, "Any client", "Python, a CLI, a notebook")):
            self.card(24, y, 196, 70)
            self.text(title, 40, y + 29, 15, t.ink, bold=True)
            self.text(sub, 40, y + 50, 12, t.muted)
        self.box(24, 304, 196, 58, "none", t.edge, dash="4 4")
        self.text("Orchestrator", 40, 329, 14, t.ink, bold=True)
        self.text("liveness probe", 40, 349, 12, t.muted)
        self.text("The client holds the document:", 24, 404, 12, t.muted)
        self.text("the market and the trade.", 24, 421, 12, t.muted)

    def backend(self):
        t = self.t
        self.box(290, 36, 690, 404, t.panel, t.line, radius=14)
        self.parts.append(f'<g transform="translate(308 50) scale(0.42)">{mark(t.teal)}</g>')
        self.text("ql-backend", 342, 70, 16, t.ink, bold=True)
        self.text("one process  ·  C++17  ·  QuantLib 1.43", 436, 70, 12, t.muted)

        # The event loop: the gateway and the supervisor share its one thread.
        self.label("Event loop", 312, 104)
        self.card(312, 116, 240, 138, accent=t.teal)
        self.text("Gateway", 330, 146, 15, t.ink, bold=True)
        self.text("uWebSockets", 536, 146, 12, t.muted, anchor="end")
        self.bullets(330, 176, ["Token and origin checks", "Routes frames by session",
                                "Backpressure, resume window"])
        self.card(312, 282, 240, 138, accent=t.teal)
        self.text("Supervisor", 330, 312, 15, t.ink, bold=True)
        self.text("no locks", 536, 312, 12, t.muted, anchor="end")
        self.bullets(330, 342, ["Session log of every write", "Placement on worker seats",
                                "Cancel, and replay on a crash"])
        self.line("M432 256 V280", t.edge, start=True, end=True)

    def wires(self):
        t = self.t
        # WebSocket, both ways, from each client to the gateway.
        self.line("M222 151 C262 151 270 158 310 158", t.teal, width=1.8, start=True, end=True)
        self.text("WebSocket", 266, 140, 11, t.teal, bold=True, anchor="middle")
        self.line("M222 245 C262 245 272 204 310 204", t.teal, width=1.8, start=True, end=True)
        self.text("Protobuf", 246, 270, 11, t.teal, bold=True, anchor="middle")
        # Plain HTTP for the probe, into the gateway's lower corner.
        self.line("M222 333 C276 333 280 244 310 244", t.edge, dash="4 4", end=True)
        self.text("HTTP", 244, 356, 11, t.muted, anchor="middle")
        # Gateway and supervisor to the workers.
        self.line("M554 170 C576 170 578 190 596 190", t.edge, start=True, end=True)
        self.line("M554 350 C576 350 580 330 596 330", t.edge, end=True)

    def workers(self):
        t = self.t
        self.label("Worker threads, one session each", 598, 104)
        for dx, dy in ((24, -4), (12, 2)):
            self.box(598 + dx, 120 + dy, 360 - dx, 248, t.card, t.line)
        x0, y0, w = 598, 128, 360
        self.card(x0, y0, w, 248)
        self.text("Session s-1", x0 + 18, y0 + 30, 15, t.ink, bold=True)
        self.pill("live QuantLib graph", x0 + w - 16, y0 + 25, t.teal, t.teal_soft)

        # The graph: three quotes feed the term structures and the engine.
        qx, cy = x0 + 36, y0 + 108
        quotes = (("S", cy - 38, True), ("r", cy, False), ("σ", cy + 38, False))
        curve = (x0 + 88, cy - 4, 92, 50)
        engine = (x0 + 206, cy - 36, 62, 36)
        npv = (x0 + 290, cy - 36, 52, 36)

        cx_, cy_, cw, ch = curve
        ex, ey, ew, eh = engine
        nx, ny, nw, nh = npv
        # r and σ into the curves and the surface, which the engine observes.
        self.line(f"M{qx + 11} {cy} H{cx_ - 2}", t.edge, end=True)
        self.line(f"M{qx + 11} {cy + 38} C{qx + 40} {cy + 38} {cx_ - 24} {cy + 14} {cx_ - 2} {cy + 14}",
                  t.edge, end=True)
        self.line(f"M{cx_ + cw + 2} {cy + 10} C{ex - 14} {cy + 10} {ex - 26} {ey + eh - 8} {ex - 2} {ey + eh - 8}",
                  t.edge, end=True)
        # The dragged quote, and the only path it recomputes.
        self.line(f"M{qx + 11} {cy - 38} C{qx + 70} {cy - 38} {ex - 60} {ey + 12} {ex - 2} {ey + 12}",
                  t.handle, width=2.2, end=True)
        self.line(f"M{ex + ew + 2} {ey + eh / 2} H{nx - 2}", t.handle, width=2.2, end=True)

        self.box(cx_, cy_, cw, ch, t.panel, t.line, radius=8)
        self.text("curves", cx_ + cw / 2, cy_ + 21, 12, t.ink, bold=True, anchor="middle")
        self.text("vol surface", cx_ + cw / 2, cy_ + 38, 11, t.muted, anchor="middle")
        self.box(ex, ey, ew, eh, t.handle_soft, t.handle, radius=8, width=1.4)
        self.text("engine", ex + ew / 2, ey + 22.5, 12, t.ink, bold=True, anchor="middle")
        self.box(nx, ny, nw, nh, t.handle_soft, t.handle, radius=8, width=1.4)
        self.text("NPV", nx + nw / 2, ny + 22.5, 12, t.ink, bold=True, anchor="middle")

        for name, y, dragged in quotes:
            fill, stroke = (t.handle, t.handle) if dragged else (t.card, t.edge)
            self.parts.append(f'<circle cx="{qx}" cy="{y}" r="11" fill="{fill}" stroke="{stroke}" '
                              f'stroke-width="1.4"/>')
            self.text(name, qx, y + 4.5, 12, t.card if dragged else t.ink, bold=True,
                      anchor="middle")
        self.text("quotes", qx, cy + 72, 11, t.muted, anchor="middle")
        self.text("curves untouched", cx_ + cw / 2, cy + 72, 11, t.muted, anchor="middle")

        self.parts.append(f'<line x1="{x0 + 18}" y1="{y0 + 196}" x2="{x0 + w - 18}" '
                          f'y2="{y0 + 196}" stroke="{t.line}"/>')
        self.text("Drag the spot: its engine and its price recompute,", x0 + 18, y0 + 219, 12, t.muted)
        self.text("and the graph stays warm between requests.", x0 + 18, y0 + 236, 12, t.muted)
        self.text("One request in, exactly one terminal frame out.", 598, 412, 12, t.ink, bold=True)

    def render(self):
        self.clients()
        self.backend()
        self.wires()
        self.workers()
        glyphs = "".join(f'<path id="{key}" d="{d}"/>' for key, d in self.glyphs.values())
        colours = {self.t.teal, self.t.edge, self.t.handle}
        markers = "".join(
            f'<marker id="{self.marker(c)}" viewBox="0 0 10 10" refX="8.5" refY="5" '
            f'markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
            f'<path d="M1 1.5 L8.5 5 L1 8.5" fill="none" stroke="{c}" stroke-width="1.6" '
            f'stroke-linecap="round" stroke-linejoin="round"/></marker>'
            for c in sorted(colours))
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" '
                f'width="{WIDTH}" height="{HEIGHT}" role="img" aria-labelledby="t d">'
                f'<title id="t">{TITLE}</title><desc id="d">{DESCRIPTION}</desc>'
                f'<defs>{markers}{glyphs}</defs>{"".join(self.parts)}</svg>\n')


def main():
    regular, semibold = inter(430), inter(650)
    for name, theme in (("architecture.svg", LIGHT), ("architecture-dark.svg", DARK)):
        (HERE / name).write_text(Figure(theme, regular, semibold).render(), encoding="utf-8")
        print("wrote", HERE / name)


if __name__ == "__main__":
    main()
