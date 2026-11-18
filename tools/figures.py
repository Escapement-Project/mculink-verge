# /// script
# requires-python = ">=3.11"
# dependencies = ["fonttools", "brotli"]
# ///
"""Draw ARCHITECTURE.md's figures and hold docs/figures/ to them. `--write` replaces the files
instead: edit a figure here, then write.

    uv run tools/figures.py [--write] [--fonts DIR]

A figure reads on GitHub, where it is an <img> and no page style reaches it, and on the project's
site, which copies it into the page so the site's colours, fonts and day and night faces reach it.
Its one <style> sets each colour as var(--token, fallback), with night fallbacks under
prefers-color-scheme. Firefox's reader view removes <style>, so every element also carries
presentation attributes that draw it in one colour: currentColor strokes and text, no fills. Nothing
refers to an id, since reader view's <base> breaks url(#id): arrowheads are paths, not markers.

Every line of text is placed by hand, so the drawing does not depend on which fonts are installed.
Each is then measured in the site's fonts (DIR, by default the site's checkout beside this one) and
in Liberation Sans and Mono, the metrics of the Arial and Courier that GitHub's readers often see,
and one that does not fit its room fails the check. Fonts that are not there are skipped, and the
check says so.
"""
import argparse
import functools
import html
import math
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "figures"
W = 720

STYLE = """\
.vf{color:var(--ink,#11171d);font-family:var(--font-text,"Hanken Grotesk","Helvetica Neue",Arial,sans-serif)}
.vf .vf-2{fill:var(--ink-2,#525f6a)}
.vf .vf-rule{stroke:var(--rule,#738691)}
.vf .vf-hair{stroke:var(--hair,#bccbd3)}
.vf .vf-wire{stroke:var(--series-verge,#0f8a6c)}
.vf .vf-wiref{fill:var(--series-verge,#0f8a6c)}
.vf .vf-rulef{fill:var(--rule,#738691)}
.vf .vf-surf{fill:var(--surface,#eef4f6)}
.vf .vf-code{font-family:var(--font-code,"Martian Mono",ui-monospace,monospace)}
.vf .vf-head{font-family:var(--font-display,"Saira","Hanken Grotesk","Helvetica Neue",Arial,sans-serif);font-stretch:var(--display-stretch,normal)}
@media (prefers-color-scheme:dark){
.vf{color:var(--ink,#e6ebee)}
.vf .vf-2{fill:var(--ink-2,#aab5be)}
.vf .vf-rule{stroke:var(--rule,#636e79)}
.vf .vf-hair{stroke:var(--hair,#29333c)}
.vf .vf-wire{stroke:var(--series-verge,#1aa384)}
.vf .vf-wiref{fill:var(--series-verge,#1aa384)}
.vf .vf-rulef{fill:var(--rule,#636e79)}
.vf .vf-surf{fill:var(--surface,#151b21)}
}"""

# ---------------------------------------------------------------- measuring

FONTS: dict[str, list] = {"text": [], "strong": [], "head": [], "code": []}
SKIPPED: list[str] = []


def find_fonts(site: pathlib.Path):
    """The fonts each kind of label is measured in: the site's own, then the system's fallbacks."""
    wanted = {
        "text": [(site / "hanken-grotesk.woff2", (("wght", 400),)), ("Liberation Sans", ())],
        "strong": [(site / "hanken-grotesk.woff2", (("wght", 600),)), ("Liberation Sans:bold", ())],
        "head": [(site / "saira-expanded.woff2", (("wght", 500),)), ("Liberation Sans", ())],
        "code": [(site / "martian-mono.woff2", (("wdth", 100),)), ("Liberation Mono", ())],
    }
    for kind, specs in wanted.items():
        for where, axes in specs:
            if isinstance(where, pathlib.Path):
                path = where if where.is_file() else None
            else:
                path = system_font(where)
            if path:
                FONTS[kind].append((str(path), axes))
            elif str(where) not in SKIPPED:
                SKIPPED.append(str(where))


def system_font(pattern: str):
    if not shutil.which("fc-match"):
        return None
    family, _, style = pattern.partition(":")
    found = subprocess.run(["fc-match", "-f", "%{family}\n%{file}", pattern], capture_output=True, text=True).stdout
    names, _, path = found.partition("\n")
    return path if family in names.split(",") else None


@functools.cache
def font(path: str, axes: tuple = ()):
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    f = TTFont(path)
    if axes:
        f = instancer.instantiateVariableFont(f, dict(axes))
    return f.getBestCmap(), f["hmtx"].metrics, f["head"].unitsPerEm


def advance(spec, s: str, size: float) -> float:
    cmap, hmtx, upem = font(*spec)
    total = 0
    for ch in s:
        g = cmap.get(ord(ch))
        if g is None:
            sys.exit(f"no glyph for {ch!r} in {spec[0]}")
        total += hmtx[g][0]
    return total * size / upem


CODE_SCALE = 0.9  # a code span beside text, set a little smaller, as the site sets inline code


def segments(s: str, kind: str):
    """A label split into (kind, text) runs: `code spans` in the code face."""
    return [("code" if i % 2 else kind, part) for i, part in enumerate(re.split(r"`([^`]*)`", s)) if part]


def width(kind: str, s: str, size: float) -> float:
    """The widest the label draws in any of the fonts measured. Each run is taken at its widest
    separately, which can only overstate the whole."""
    total = 0
    for k, part in segments(s, kind):
        z = size * CODE_SCALE if k == "code" and kind != "code" else size
        total += max((advance(spec, part, z) for spec in FONTS[k]), default=0)
    return total


# ---------------------------------------------------------------- drawing

def num(v: float) -> str:
    return f"{v:.2f}".rstrip("0").rstrip(".")


class Fig:
    def __init__(self, name: str):
        self.name, self.parts, self.misfits = name, [], []

    def text(self, x, y, s, size=14, kind="text", tone=None, anchor="start", room=None):
        """A line of text with its baseline at y. `room` is the width it must fit in."""
        if room is not None:
            w = width(kind, s, size)
            if w > room + 0.01:
                self.misfits.append(f"{self.name}: {s!r} is {w:.1f} wide, room {room:.1f}")
        cls = {"code": "vf-code", "head": "vf-head"}.get(kind)
        classes = " ".join(c for c in (cls, "vf-2" if tone == "2" else None) if c)
        attrs = [f'x="{num(x)}"', f'y="{num(y)}"', f'font-size="{num(size)}"', 'fill="currentColor"']
        if kind == "strong":
            attrs.append('font-weight="600"')
        if kind == "head":
            attrs.append('font-weight="500"')
        if anchor != "start":
            attrs.append(f'text-anchor="{anchor}"')
        if classes:
            attrs.append(f'class="{classes}"')
        body = "".join(
            f'<tspan class="vf-code" font-size="{num(size * CODE_SCALE)}">{html.escape(part)}</tspan>'
            if k == "code" and kind != "code" else html.escape(part)
            for k, part in segments(s, kind))
        self.parts.append(f"<text {' '.join(attrs)}>{body}</text>")

    def path(self, d, tone=None, width=1.5, dash=None, fill=None):
        attrs = [f'd="{d}"', 'fill="none"', 'stroke="currentColor"', f'stroke-width="{num(width)}"']
        if dash:
            attrs.append(f'stroke-dasharray="{dash}"')
        cls = [{"rule": "vf-rule", "hair": "vf-hair", "wire": "vf-wire"}.get(tone)]
        if fill == "surface":
            cls.append("vf-surf")
        cls = " ".join(c for c in cls if c)
        if cls:
            attrs.append(f'class="{cls}"')
        self.parts.append(f"<path {' '.join(attrs)}/>")

    def rect(self, x, y, w, h, r=4, tone=None, width=1.5, fill=None, dash=None):
        if r:
            d = (f"M{num(x + r)},{num(y)}H{num(x + w - r)}A{num(r)},{num(r)} 0 0 1 {num(x + w)},{num(y + r)}"
                 f"V{num(y + h - r)}A{num(r)},{num(r)} 0 0 1 {num(x + w - r)},{num(y + h)}H{num(x + r)}"
                 f"A{num(r)},{num(r)} 0 0 1 {num(x)},{num(y + h - r)}V{num(y + r)}A{num(r)},{num(r)} 0 0 1 {num(x + r)},{num(y)}Z")
        else:
            d = f"M{num(x)},{num(y)}H{num(x + w)}V{num(y + h)}H{num(x)}Z"
        self.path(d, tone=tone, width=width, fill=fill, dash=dash)

    def head(self, x, y, angle, tone=None, size=7):
        """A filled arrowhead with its tip at (x, y), pointing along `angle` (degrees, 0 = right)."""
        a = math.radians(angle)
        bx, by = x - size * math.cos(a), y - size * math.sin(a)
        nx, ny = -math.sin(a) * size * 0.45, math.cos(a) * size * 0.45
        d = f"M{num(x)},{num(y)}L{num(bx + nx)},{num(by + ny)}L{num(bx - nx)},{num(by - ny)}Z"
        cls = {"wire": ' class="vf-wiref"', "rule": ' class="vf-rulef"'}.get(tone, "")
        self.parts.append(f'<path d="{d}" fill="currentColor"{cls}/>')

    def arrow(self, pts, tone=None, dash=None, start=False, end=True, width=1.5):
        """A line through `pts`, with arrowheads where asked. The line stops short of a head."""
        def trim(p, q, by):
            dx, dy = q[0] - p[0], q[1] - p[1]
            n = math.hypot(dx, dy)
            return (q[0] - dx / n * by, q[1] - dy / n * by)
        line = list(pts)
        if end:
            line[-1] = trim(pts[-2], pts[-1], 5)
        if start:
            line[0] = trim(pts[1], pts[0], 5)
        self.path("M" + "L".join(f"{num(x)},{num(y)}" for x, y in line), tone=tone, dash=dash, width=width)
        for p, q, on in ((pts[-2], pts[-1], end), (pts[1], pts[0], start)):
            if on:
                self.head(*q, math.degrees(math.atan2(q[1] - p[1], q[0] - p[0])), tone=tone)

    def trace(self, x0, x1, y):
        """The wire as the Verge page draws it: a stretch of square wave, in Verge's colour."""
        steps = [0, 14, 34, 44, 74, 84, 96, 120]
        d, low = f"M{num(x0)},{num(y + 4)}", True
        for a, b in zip(steps, steps[1:]):
            d += f"H{num(x0 + (x1 - x0) * b / 120)}"
            if b != 120:
                low = not low
                d += f"V{num(y + 4 if low else y - 4)}"
        self.path(d, tone="wire")

    def bracket(self, x, y0, y1, toward):
        """A thin bracket from y0 to y1 at x, its ends turned toward the bar (+1 right, -1 left)."""
        self.path(f"M{num(x + 5 * toward)},{num(y0 + 1)}H{num(x)}V{num(y1 - 1)}H{num(x + 5 * toward)}",
                  tone="rule", width=1)

    def lines(self, x, y, rows, size=14, lead=18, anchor="start", room=None):
        """Lines of text from the baseline y down; each a string or a (text, kind, tone)."""
        for i, row in enumerate(rows):
            s, kind, tone = (row, "text", None) if isinstance(row, str) else row
            self.text(x, y + i * lead, s, size=size if tone != "2" else size - 1, kind=kind, tone=tone,
                      anchor=anchor, room=room)

    def heading(self, x, y, title, job, room, anchor="start"):
        self.text(x, y, title, size=17, kind="head", anchor=anchor, room=room)
        if job:
            self.text(x, y + 20, job, size=14, tone="2", anchor=anchor, room=room)

    def mailbox(self, x0, x1, y):
        """Each core raises the other's mailbox: a dashed arrow both ways, as the Verge page dashes it."""
        self.arrow([(x0, y), (x1, y)], tone="rule", dash="4 3", start=True)
        self.text((x0 + x1) / 2, y - 8, "mailbox", size=13, tone="2", anchor="middle", room=x1 - x0)

    def svg(self, height) -> str:
        body = "\n".join(self.parts)
        return (f'<svg xmlns="http://www.w3.org/2000/svg" class="vf" viewBox="0 0 {W} {num(height)}" '
                f'width="{W}" height="{num(height)}">\n<style>\n{STYLE}\n</style>\n{body}\n</svg>\n')


NOTE = "2"


# ---------------------------------------------------------------- the blocks

def core_rows(f: Fig, x, y, w, title, job, rows, pitch=28, right=False):
    """A core as the Verge page lists it: its name, its job, then its work, a hairline above each
    row. `right` sets the name and job against the column's right edge, beside the mailbox. Returns
    each row's middle, where a line to a pin starts."""
    f.heading(x + w if right else x, y, title, job, w, anchor="end" if right else "start")
    top = y + 34
    for i, r in enumerate(rows):
        f.path(f"M{num(x)},{num(top + i * pitch)}H{num(x + w)}", tone="hair", width=1)
        f.text(x, top + i * pitch + 19, r, size=14, room=w)
    return [top + i * pitch + 14 for i in range(len(rows))]


def blocks():
    f = Fig("architecture-blocks")
    top, chip_x = 8, 64
    tw = 150
    tx = W - 1 - tw
    chip_w = tx - 20 - chip_x
    pad, col, mid_w = 18, 154, 120
    c0x = chip_x + pad
    c1x = chip_x + chip_w - pad - col
    gap = (c1x - c0x - col - mid_w) / 2
    mid_x = c0x + col + gap
    y0 = top + 46
    c0 = core_rows(f, c0x, y0, col, "CPU 0", "Answers the host",
                   ["USB, every endpoint", "The energy stream", "LEDs, restarts", "The serial consoles"], right=True)
    c1 = core_rows(f, c1x, y0, col, "CPU 1", "Drives the target",
                   ["CMSIS-DAP commands", "Generated wire code", "SWO capture", "DIO, SPI and I2C",
                    "Supply, energy capture"])
    # On the cores' names' baseline: the line's lower edge sits on it.
    f.mailbox(mid_x + 4, mid_x + mid_w - 4, y0 - 0.75)
    # The memory both cores use, a third column between them in the cores' style: its name where
    # theirs give their job, its rows on their rhythm.
    mid = mid_x + mid_w / 2
    f.text(mid, y0 + 20, "Shared memory", size=14, tone="2", anchor="middle", room=mid_w)
    for i, s in enumerate(["Commands", "Replies", "Staged copies", "Stream messages"]):
        f.path(f"M{num(mid_x)},{num(c0[i] - 14)}H{num(mid_x + mid_w)}", tone="hair", width=1)
        f.text(mid, c0[i] + 5, s, size=14, anchor="middle", room=mid_w)
    serial_y = c1[-1] + 34
    chip_h = serial_y + 22 - top
    f.rect(chip_x, top, chip_w, chip_h, tone="rule")
    f.text(chip_x + 18, top + 18, "LPC55S69", size=12, kind="code", tone="2")
    f.text(0, c0[0] - 2, "Host", size=15, kind="strong", room=chip_x - 12)
    f.text(0, c0[0] + 16, "USB", size=14, tone="2", room=chip_x - 12)
    f.arrow([(40, c0[0]), (c0x - 4, c0[0])], start=True)
    # The target: a second package, its pins level with the lines that reach them. The wire is drawn
    # as the Verge page's trace, from just past its row's words; SWO comes in from the target.
    f.rect(tx, top, tw, chip_h, tone="rule")
    f.heading(tx + 12, y0, "Target", "Under debug", tw - 16)
    ends = (c1x + col + 4, tx - 2)
    wire_from = c1x + col - 14
    if width("text", "Generated wire code", 14) > wire_from - 6 - c1x:
        f.misfits.append(f"{f.name}: 'Generated wire code' runs into the wire's trace")
    f.trace(wire_from, ends[1], c1[1])
    f.arrow([(ends[1], c1[2]), (ends[0], c1[2])])
    f.arrow([(ends[0], c1[3]), (ends[1], c1[3])], start=True)
    f.arrow([(ends[0], c1[4]), (ends[1], c1[4])])
    for yy, name in ((c1[1], "SWD, JTAG, cJTAG"), (c1[2], "SWO"), (c1[3], "DIO, SPI, I2C"), (c1[4], "Supply"),
                     (serial_y, "Serial")):
        f.text(tx + 12, yy + 5, name, size=14, room=tw - 16)
    # The consoles' line leaves their row from below, then runs under the shared memory and CPU 1's
    # list to the target's serial lines.
    drop = c0x + 60
    f.arrow([(drop, c0[3] + 12), (drop, serial_y), (tx - 2, serial_y)], start=True)
    return f, top + chip_h + 2


# ---------------------------------------------------------------- a command's path

def command():
    f = Fig("architecture-command")
    lanes = [("Host", "USB"), ("CPU 0", None), ("Shared memory", "USB SRAM, SRAM3"), ("CPU 1", None),
             ("Target", "Under debug")]
    xs = [30, 182, 356, 538, 666]
    for (name, sub), x in zip(lanes, xs):
        f.text(x, 22, name, size=17, kind="head", anchor="middle", room=160)
        if sub:
            f.text(x, 42, sub, size=14, tone="2", anchor="middle", room=160)
    HOST, C0, MEM, C1, TGT = range(5)
    y0, pitch = 86, 36
    breaks = {i: [] for i in range(5)}

    def badge(n, lane, y):
        breaks[lane].append((y - 12, y + 12))
        f.parts.append(f'<circle cx="{xs[lane]}" cy="{num(y)}" r="10" fill="none" stroke="currentColor" stroke-width="1.5"/>')
        f.text(xs[lane], y + 5, str(n), size=14, kind="strong", anchor="middle")

    def leg(a, b, y, label, tone=None, dash=None, both=False, from_badge=False, to_badge=False):
        """One arrow between two lanes, its label centred above it; lifelines it crosses break
        around the label."""
        d = 1 if xs[b] > xs[a] else -1
        x0 = xs[a] + d * (12 if from_badge else 3)
        x1 = xs[b] - d * (12 if to_badge else 3)
        f.arrow([(x0, y), (x1, y)], tone=tone, dash=dash, start=both)
        f.text((x0 + x1) / 2, y - 7, label, size=14, tone="2" if tone == "rule" else None, anchor="middle",
               room=abs(x1 - x0) - 4)
        for i, x in enumerate(xs):
            if min(x0, x1) < x < max(x0, x1):
                breaks[i].append((y - 21, y - 3))

    y = [y0 + i * pitch for i in range(10)]
    badge(1, C0, y[0])
    leg(C0, MEM, y[0], "arm a free request slot", from_badge=True)
    # The command lands in the slot step 1 armed; CPU 0 handles it on the way.
    badge(2, C0, y[1])
    leg(HOST, C0, y[1], "command, EP1 OUT", to_badge=True)
    leg(C0, MEM, y[1], "into that slot", from_badge=True)
    # An abort branches off at step 2 and goes no further.
    bx = xs[C0] + 7
    f.arrow([(bx, y[1] + 8), (bx, y[2]), (xs[C1] - 3, y[2])], tone="rule", dash="4 3")
    f.text((bx + xs[C1]) / 2, y[2] - 7, "an abort sets CPU 1's flag, and stops there", size=14, tone="2",
           anchor="middle", room=xs[C1] - bx - 8)
    breaks[MEM].append((y[2] - 21, y[2] - 3))
    badge(3, C0, y[3])
    leg(C0, MEM, y[3], "copy it into `dap_stage`", from_badge=True)
    leg(C0, C1, y[4], "mailbox", tone="rule", dash="4 3")
    badge(4, C1, y[5])
    leg(MEM, C1, y[5], "take the oldest command", to_badge=True)
    badge(5, C1, y[6])
    leg(C1, TGT, y[6], "run it", tone="wire", both=True, from_badge=True)
    badge(6, C1, y[7])
    leg(C1, MEM, y[7], "publish the reply", from_badge=True)
    leg(C1, C0, y[8], "mailbox", tone="rule", dash="4 3")
    badge(7, C0, y[9])
    leg(MEM, C0, y[9], "copy the reply out", to_badge=True)
    leg(C0, HOST, y[9], "reply, EP1 IN", from_badge=True)
    bottom = y[-1] + 20
    for i, x in enumerate(xs):
        t = 54
        for a, b in sorted(breaks[i]):
            if a > t:
                f.path(f"M{x},{num(t)}V{num(a)}", tone="hair", width=1)
            t = max(t, b)
        f.path(f"M{x},{num(t)}V{num(bottom)}", tone="hair", width=1)
    return f, bottom + 4


# ---------------------------------------------------------------- one packet's route

def packet():
    """One DAP_Transfer, a DP read of CTRL/STAT over SWD, through the modules that handle it: down
    the left column to the wire, along the wire in time, and back up the right column. At each stop
    that reads or writes the packet, the bytes it handles are underlined."""
    f = Fig("architecture-packet")
    LX, RX = 96, 704          # the two routes, on which the stops' numbers sit
    TL, TR, ROOM = 118, 682, 276
    PITCH = 24

    def badge(x, y, n):
        f.parts.append(f'<circle cx="{x}" cy="{num(y)}" r="10" fill="none" stroke="currentColor" stroke-width="1.5"/>')
        f.text(x, y + 4.5, str(n), size=13, kind="strong", anchor="middle", room=17)

    def dump(y, data, marked, right=False, note=None):
        n = len(data)
        for i, b in enumerate(data):
            cx = TR - 10 - PITCH * (n - 1 - i) if right else TL + 10 + PITCH * i
            f.text(cx, y, b, size=13, kind="code", tone=None if i in marked else NOTE, anchor="middle",
                   room=PITCH - 4)
            if i in marked:
                f.path(f"M{num(cx - 10)},{num(y + 5)}H{num(cx + 10)}", width=1.5)
        if note:
            f.text(TL + PITCH * n + 6, y, note, size=14, tone=NOTE, room=ROOM - PITCH * n - 6)

    def stop(right, y, n, rows, room=ROOM):
        """A stop: its number on the route, then its rows from the baseline y down, 18 apart. A
        stop with nothing beside it in the other column can take a wider `room`."""
        badge(RX if right else LX, y - 5, n)
        yy = y
        for row in rows:
            if isinstance(row, tuple):
                dump(yy, *row[:2], right=right, note=row[2] if len(row) > 2 else None)
                yy += 4  # room under the underlines
            else:
                s, tone = (row[1:], NOTE) if row.startswith("~") else (row, None)
                f.text(TR if right else TL, yy, s, size=14, tone=tone, anchor="end" if right else "start",
                       room=room)
            yy += 18
        return y - 5

    def band(top, name, y, kind="head"):
        f.path(f"M0,{num(top)}H{W}", tone="hair", width=1)
        if kind == "head":
            f.text(0, y, name, size=17, kind="head", room=LX - 16)
        else:
            f.text(0, y, name, size=13, tone=NOTE, room=LX - 16)

    f.text(TL, 18, "The command", size=14, tone=NOTE, room=ROOM)
    f.text(TR, 18, "The reply", size=14, tone=NOTE, anchor="end", room=ROOM)
    cmd = ["05", "00", "01", "06"]
    reply = ["05", "01", "01", "d0", "d1", "d2", "d3"]

    band(30, "Host", 52)
    left = [stop(False, 52, 1, ["The host sends a `DAP_Transfer`", (cmd, (), "on EP1 OUT")])]
    right = [stop(True, 52, 12, ["The host reads it on EP1 IN", (reply, ())])]
    band(84, "USB", 106)
    left.append(stop(False, 106, 2, ["The controller fills a request slot", "~in `dap_shared`, in USB SRAM"]))
    right.append(stop(True, 106, 11, ["The controller sends it when asked", "~`tud_vendor_tx_cb` frees the slot"]))
    band(138, "CPU 0", 160)
    left.append(stop(False, 160, 3, ["`tud_vendor_rx_cb` in `main.c`", (cmd, (0,), "not an abort"),
                                     "Copies it into `dap_stage` in SRAM3,", "numbers it, raises CPU 1's mailbox"]))
    right.append(stop(True, 160, 10, ["`send_bulk_head` in `main.c`", "Copies it into the slot's USB buffer,",
                                      "starts EP1 IN"]))
    band(228, "mailbox", 246, kind="note")
    band(254, "CPU 1", 276)
    left.append(stop(False, 276, 4, ["`dap_task` in `core1_main.c`", "Takes the oldest command, frees its slot,",
                                     "runs it from `dap_stage`"]))
    right.append(stop(True, 276, 9, ["`dap_task` in `core1_main.c`", "Publishes it in the reply ring,",
                                     "raises CPU 0's mailbox"]))
    left.append(stop(False, 340, 5, ["`DAP_ProcessCommand` in `dap_dispatch.c`",
                                     (cmd, (0,), "`DAP_Transfer`, on SWD")], room=ROOM + 28))
    left.append(stop(False, 386, 6, ["`SWD_TransferCommand` in `swd_dp.c`", (cmd, (1, 2, 3)),
                                     "ARM's transfer loop: a DP read of 0x4,", "which `swd_gen_packet` makes 8D"]))
    left.append(stop(False, 468, 7, ["`swd_gen_read`, generated into SRAMX", "Drives the pins to the cycle,",
                                     "then samples the target's answer"]))
    right.append(stop(True, 468, 8, ["`SWD_TransferCommand` in `swd_dp.c`", (reply, (1, 2, 3, 4, 5, 6)),
                                     "One done, ACK OK, the word,", "written into `dap_stage`"]))

    # The routes: down the left, up the right, each stop's number breaking the line. The hop
    # between the cores is the mailbox, dashed as the other figures dash it.
    def route(x, a, b, up):
        hop = (a < 228 < b) or (b < 228 < a)
        f.arrow([(x, a + (-10 if up else 10)), (x, b + (10 if up else -10))],
                tone="rule" if hop else None, dash="4 3" if hop else None)
    for a, b in zip(left, left[1:]):
        route(LX, a, b, up=False)
    order = right[::-1]  # 8, 9, 10, 11, 12, as the reply climbs
    for a, b in zip(order, order[1:]):
        route(RX, a, b, up=True)

    # The wire, in time from left to right: SWCLK falls at each period's start and rises halfway.
    # The probe changes SWDIO as the clock falls; the target as it rises, so its bits sit half a
    # period later. The probe lets go just after the park bit's rise and takes the line back after
    # the last rise of the turnaround after the data (swd_gen.c, release() and schedule_read()), so
    # each turnaround leaves TRN whole periods undriven. The read runs with a two-clock turnaround,
    # as ARCHITECTURE.md says. 48 periods: request 8, turnaround 2, ACK 3, data and parity 33,
    # turnaround 2. The data's middle is cut out.
    band(536, "Wire", 558)
    P, X0, TRN = 20, 112, 2
    CK, DI = 576, 610           # the rows' middles
    hi_c, lo_c, hi, lo = CK - 7, CK + 7, DI - 7, DI + 7
    cut0 = X0 + 17 * P          # on a period's start, as is the last turnaround's end
    cut1 = cut0 + 32
    end = cut1 + 8 * P
    f.text(0, CK + 4, "SWCLK", size=13, kind="code", room=LX - 16)
    f.text(0, CK + 18, "P0_0", size=12, kind="code", tone=NOTE, room=LX - 16)
    f.text(0, DI + 4, "SWDIO", size=13, kind="code", room=LX - 16)
    f.text(0, DI + 18, "P0_2", size=12, kind="code", tone=NOTE, room=LX - 16)

    def clock(x0, x1, lead=0, tail=0):
        d = f"M{num(x0 - lead)},{num(hi_c)}"
        for x in range(x0, x1, P):
            d += f"H{num(x)}V{num(lo_c)}H{num(x + P / 2)}V{num(hi_c)}"
        d += f"H{num(x1 + tail)}"
        f.path(d, tone="wire")
    clock(X0, cut0, lead=8)
    clock(cut1, end, tail=688 - end)

    def levels(x0, bits, step, end=None):
        """Bits from x0, each `step` wide, the last ending at `end` where the driver lets go."""
        d, y = f"M{num(x0)},{num(hi if bits[0] else lo)}", hi if bits[0] else lo
        for i, bit in enumerate(bits):
            ny = hi if bit else lo
            if ny != y:
                d += f"V{num(ny)}"
                y = ny
            d += f"H{num(end if end is not None and i == len(bits) - 1 else x0 + step * (i + 1))}"
        f.path(d, tone="wire")
    request = [1, 0, 1, 1, 0, 0, 0, 1]  # 0x8D, least significant bit first
    park = X0 + 7 * P + P // 2  # the park bit's rise, where the probe lets go
    levels(X0, request, P, end=park)
    ack0 = X0 + (7 + TRN) * P + P // 2  # the target's first bit, from the turnaround's last rise
    levels(ack0, [1, 0, 0], P)       # OK
    for i, bit in enumerate(request):
        f.text(X0 + P * i + P / 2, DI - 12, str(bit), size=12, kind="code", tone=NOTE, anchor="middle", room=P)
    for i, bit in enumerate([1, 0, 0]):
        f.text(ack0 + P * i + P / 2, DI - 12, str(bit), size=12, kind="code", tone=NOTE, anchor="middle", room=P)
    # Neither drives the line in a turnaround, so it shows dashed.
    f.path(f"M{num(park)},{num(hi)}H{num(ack0)}", tone="rule", dash="2 1")
    # The target lets go at the rise after its parity bit; the probe takes the line back after the
    # turnaround's last rise, half a period before the end.
    d0, d1, back = ack0 + 3 * P, end - TRN * P - P // 2, end - P // 2
    f.path(f"M{num(d0)},{num(DI)}L{num(d0 + 5)},{num(hi)}H{num(cut0)}M{num(d0)},{num(DI)}L{num(d0 + 5)},{num(lo)}"
           f"H{num(cut0)}", tone="wire")
    f.path(f"M{num(cut1)},{num(hi)}H{num(d1 - 5)}L{num(d1)},{num(DI)}M{num(cut1)},{num(lo)}H{num(d1 - 5)}"
           f"L{num(d1)},{num(DI)}", tone="wire")
    f.path(f"M{num(d1)},{num(hi)}H{num(back)}", tone="rule", dash="2 1")
    f.path(f"M{num(back)},{num(hi)}H688", tone="wire")
    for x in (cut0 + 6, cut0 + 18):
        f.path(f"M{num(x - 4)},{num(lo + 6)}L{num(x + 4)},{num(hi_c - 6)}", tone="rule", width=1)

    # Who drives which bits.
    def drive(x0, x1, label):
        y = DI + 24
        f.path(f"M{num(x0)},{num(y - 5)}V{num(y)}H{num(x1)}V{num(y - 5)}", tone="rule", width=1)
        f.text((x0 + x1) / 2, y + 16, label, size=14, anchor="middle", room=x1 - x0 + 40)
    drive(X0, park, "8D, from the probe")
    drive(ack0, d1, "ACK OK, CTRL/STAT and parity, from the target")

    # Down the left route into the wire, out of it and up the right route.
    f.arrow([(LX, left[-1] + 10), (LX, hi), (X0 - 2, hi)])
    f.arrow([(692, hi), (RX, hi), (RX, right[4] + 10)])
    return f, DI + 50


# ---------------------------------------------------------------- memory

def memory():
    """A memory map: one bar of the banks, high addresses at the top, each region named with its
    base address on its lower edge. CPU 0's contents hang off the bar's left, CPU 1's off its right.
    What both link scripts place at one address is a shaded band inside the bar; what a core reaches
    through a pointer in dap_shared sits between dashed rules."""
    f = Fig("architecture-memory")
    bar_x, bar_w = 236, 250
    left, right = bar_x - 16, bar_x + bar_w + 16
    f.heading(left - 6, 22, "CPU 0", None, left, anchor="end")
    f.heading(right + 6, 22, "CPU 1", None, W - right - 6)
    LEAD, PAD = 18, 8

    # Each region top to bottom: (id, inside lines, base line (name, address), band, min height).
    # A band is "shared" (placed by both link scripts) or "pointer" (reached through dap_shared).
    blocks_ = [
        [("dap_shared", ["Request and reply slots, queues,", "DMA's channel table, the SWO ring",
                         ("written by both cores, the USB", "text", NOTE), ("controller and DMA", "text", NOTE)],
          ("`dap_shared`", "0x4010_1000"), "shared", 0),
         ("usb", [], ("USB SRAM", "0x4010_0000"), None, 60)],
        [("sram4_free", [], None, None, 12),
         ("slots", ["Stream messages:", "16 slots of 512 bytes", ("written by the core that owns the", "text", NOTE),
                    ("capture state", "text", NOTE)], ("SRAM4", "0x2004_0000"), "shared", 0),
         ("stage", ["Staged commands and replies,", "HID replies, Verge Aux's slot", ("written by both cores", "text", NOTE)],
          ("`dap_stage`", "0x2003_E000"), "shared", 0),
         ("sram3", [], ("SRAM3", "0x2003_0000"), None, 34),
         ("abort", [], ("The abort flag, which CPU 0 sets", None), "pointer", 0),
         ("sram2", [], ("SRAM2", "0x2002_0000"), None, 48),
         ("sram1_code", [], (None, "0x2001_4000"), None, 48),
         ("sram1_stack", [], ("SRAM1", "0x2001_0000"), None, 34),
         ("sram0_stack", [], None, None, 26),
         ("energy", [], ("Energy capture's state", None), "pointer", 0),
         ("sram0", [], ("SRAM0", "0x2000_0000"), None, 74)],
        [("sramx", [], ("SRAMX", "0x0400_0000"), None, 44)],
        [("flash1", [], (None, "0x4_0000"), None, 34),
         ("flash0", [], ("Flash", "0x0000_0000"), None, 34)],
    ]
    y = 44
    at = {}
    for block in blocks_:
        block_top = y
        for i, (rid, inside, base, band, min_h) in enumerate(block):
            h = max(min_h, PAD + len(inside) * LEAD + (LEAD + 4 if base else 0))
            top, bottom = y, y + h
            at[rid] = (top, bottom)
            if band == "shared":
                f.rect(bar_x, top, bar_w, h, r=0, fill="surface", tone="rule", width=1)
            elif band == "pointer":
                for yy in (top, bottom):
                    f.path(f"M{bar_x},{num(yy)}H{bar_x + bar_w}", tone="rule", width=1, dash="4 3")
            elif i:
                above = block[i - 1][3]
                if not above:
                    f.path(f"M{bar_x},{num(top)}H{bar_x + bar_w}", tone="hair", width=1)
            f.lines(bar_x + 10, top + PAD + 13, inside, room=bar_w - 20)
            if base:
                name, address = base
                yb = bottom - 8
                if name:
                    kind = "strong" if not name.startswith("`") and address else "text"
                    f.text(bar_x + 10, yb, name, size=14, kind=kind, room=bar_w - 20 - (104 if address else 0))
                if address:
                    f.text(bar_x + bar_w - 10, yb, address, size=13, kind="code", tone=NOTE, anchor="end",
                           room=104)
            y = bottom
        f.rect(bar_x, block_top, bar_w, y - block_top, r=0, tone="rule")
        y += 16
    bar_bottom = y - 16

    def note(side, first, last, rows, dashed_to=None):
        """Words for a range of regions, off the bar's `side`, behind a bracket."""
        t, b = at[first][0], at[last][1]
        if side == "left":
            f.bracket(left + 6, t, b, +1)
            x, anchor, room = left - 6, "end", left - 6
        else:
            f.bracket(right - 6, t, b, -1)
            x, anchor, room = right + 6, "start", W - right - 6
        if dashed_to:
            dt = at[dashed_to][0]
            bx = left + 6 if side == "left" else right - 6
            f.path(f"M{num(bx)},{num(t)}V{num(dt + 1)}", tone="rule", width=1, dash="2 3")
        n = len(rows)
        first_base = (t + b) / 2 - (n - 1) * 17 / 2 + 5
        f.lines(x, first_base, rows, size=14, lead=17, anchor=anchor, room=room)

    note("left", "usb", "usb", ["TinyUSB's buffers,", "Verge Aux's included", ("the USB controller writes them too", "text", NOTE)])
    note("left", "sram0_stack", "sram0_stack", ["The stack, in the top 4 KiB"])
    note("left", "energy", "sram0", ["Data, the loop and the code a", "command passes through", ("the consoles' rings and the energy", "text", NOTE),
                                     ("ring, which DMA fills", "text", NOTE)])
    note("left", "flash0", "flash0", ["Code, from `0x200`"])
    note("right", "abort", "sram1_code", ["From `0x2001_4000`, the", "command path, the SWD", "generator and the JTAG",
                                          "generator's scans, then data,", "running on into SRAM2",
                                          ("how far depends on the compiler;", "text", NOTE),
                                          ("rarely used data may reach SRAM3", "text", NOTE)], dashed_to="sram3")
    note("right", "sram1_stack", "sram1_stack", ["16 KiB of stack"])
    note("right", "sramx", "sramx", ["The generated wire code,", ("written at runtime", "text", NOTE)])
    note("right", "flash1", "flash1", ["Code, from `0x4_0000`"])
    # What the shading and the dashes mean.
    ky = bar_bottom + 30
    f.rect(bar_x, ky - 11, 22, 14, r=0, fill="surface", tone="rule", width=1)
    f.text(bar_x + 30, ky, "placed at one address by both link scripts", size=13, tone=NOTE, room=W - bar_x - 30)
    for yy in (ky + 11, ky + 25):
        f.path(f"M{bar_x},{num(yy)}h22", tone="rule", width=1, dash="4 3")
    f.text(bar_x + 30, ky + 23, "reached through a pointer in `dap_shared`", size=13, tone=NOTE, room=W - bar_x - 30)
    return f, ky + 32


# ---------------------------------------------------------------- energy capture

def energy():
    """Energy capture as a signal chain: what moves the pairs labels the arrows, and the state's
    owner is a strip along time."""
    f = Fig("architecture-energy")
    y = 132                               # the chain's line
    adc_x, ring_c, slots_x, out_x = 124, 270, 548, W - 1
    # Control down the left: CPU 1 starts the timer that paces the ADC.
    f.text(adc_x, 20, "CPU 1", size=14, kind="strong", anchor="middle")
    f.text(adc_x + 30, 20, "starts, stops and reads a capture (`0x90`), and calibrates", size=13, tone=NOTE,
           room=W - adc_x - 30)
    f.arrow([(adc_x, 28), (adc_x, 50)])
    f.text(adc_x, 68, "CTimer 2", size=14, kind="strong", anchor="middle")
    f.arrow([(adc_x, 76), (adc_x, y - 22)])
    f.text(adc_x + 8, 100, "triggers samples", size=13, tone=NOTE, room=ring_c - adc_x - 40)
    # The chain.
    f.text(0, y - 3, "Target", size=14, kind="strong", room=60)
    f.text(0, y + 14, "supply", size=14, kind="strong", room=60)
    f.arrow([(60, y), (adc_x - 26, y)])
    f.text(adc_x, y + 5, "ADC0", size=14, kind="strong", anchor="middle")
    f.lines(adc_x, y + 28, [("voltage and", "text", NOTE), ("current, a sample", "text", NOTE)],
            anchor="middle", room=104)
    f.arrow([(adc_x + 26, y), (ring_c - 24, y)])
    f.text((adc_x + ring_c) / 2, y - 7, "DMA", size=14, anchor="middle", room=ring_c - adc_x - 60)
    # The ring: a circle with an arrowhead running round it.
    r = 16
    f.parts.append(f'<path d="M{num(ring_c + r)},{num(y)}A{r},{r} 0 1 1 {num(ring_c)},{num(y - r)}" fill="none" '
                   f'stroke="currentColor" stroke-width="1.5"/>')
    f.head(ring_c + 4.5, y - r, 0)
    f.text(ring_c, y + 34, "Ring, in SRAM0", size=14, kind="strong", anchor="middle", room=120)
    f.lines(ring_c, y + 52, [("segments of about", "text", NOTE), ("1 ms or longer", "text", NOTE)],
            anchor="middle", room=120)
    # CPU 0 moves each segment on, the heavier arrow.
    f.arrow([(ring_c + r + 6, y), (slots_x - 8, y)], width=2.5)
    f.text((ring_c + slots_x) / 2, y - 9, "CPU 0", size=14, kind="strong", anchor="middle")
    f.lines((ring_c + slots_x) / 2 + 14, y + 22, [("adds each segment to", "text", NOTE),
                                                  ("the energy total; while", "text", NOTE),
                                                  ("a stream runs, writes the", "text", NOTE),
                                                  ("samples into stream messages", "text", NOTE)],
            anchor="middle", room=180)
    # The slots: sixteen cells, two rows of eight.
    cell, gap_ = 9, 2
    for i in range(16):
        cx = slots_x + (i % 8) * (cell + gap_)
        cy = y - cell - 1 + (i // 8) * (cell + gap_)
        f.rect(cx, cy, cell, cell, r=0, tone="rule", width=1)
    slots_w = 8 * cell + 7 * gap_
    f.text(slots_x + slots_w / 2, y + 34, "16 slots", size=14, kind="strong", anchor="middle")
    f.text(slots_x + slots_w / 2, y + 52, "in SRAM4", size=13, tone=NOTE, anchor="middle")
    f.arrow([(slots_x + slots_w + 8, y), (out_x, y)])
    f.text(out_x, y - 9, "Verge Aux", size=14, anchor="end", room=out_x - slots_x - slots_w - 12)
    f.text(out_x, y + 18, "EP5 IN", size=14, anchor="end", room=out_x - slots_x - slots_w - 12)
    # The range: CPU 0's loop switches it back to low, a loop back to the input.
    fy = y + 124
    fx0 = (ring_c + slots_x) / 2 + 10
    fx1 = 68
    f.arrow([(fx0, y + 98), (fx0, fy), (fx1, fy), (fx1, y + 5)], tone="rule", dash="4 3")
    f.text((fx0 + fx1) / 2, fy - 7, "its loop switches the current range back to low", size=13, tone=NOTE,
           anchor="middle", room=fx0 - fx1 - 10)
    # Who owns the capture state, along time; what is stamped on CTimer 4 while a capture runs.
    ty = fy + 66
    ax0, ax1 = 0, W - 1
    start_x, stop_x = 190, 560
    f.arrow([(ax0, ty), (ax1, ty)])
    f.text(ax1, ty + 20, "time", size=13, tone=NOTE, anchor="end")
    for x, word in ((start_x, "start"), (stop_x, "stop")):
        f.path(f"M{x},{ty - 9}V{ty + 9}", width=1.5)
        f.text(x, ty + 24, word, size=13, tone=NOTE, anchor="middle")
    f.text(start_x / 2, ty - 26, "CPU 1 owns the state", size=14, anchor="middle", room=start_x - 12)
    f.text((start_x + stop_x) / 2, ty - 26, "CPU 0 owns it", size=14, anchor="middle")
    f.text((stop_x + ax1) / 2, ty - 26, "CPU 1 owns it", size=14, anchor="middle")
    # Pairs as ticks, a DIO edge as a step, a marker as a flag, all on the stream's clock.
    for x in range(start_x + 14, stop_x - 6, 12):
        if x in (start_x + 110, start_x + 230):
            continue
        f.path(f"M{x},{ty - 6}V{ty}", tone="rule", width=1)
    ex = start_x + 110
    f.path(f"M{ex - 6},{ty - 2}H{ex}V{ty - 12}H{ex + 6}", width=1.5)
    f.text(ex, ty + 24, "DIO edge", size=12, tone=NOTE, anchor="middle")
    mx = start_x + 230
    f.path(f"M{mx},{ty}V{ty - 16}", width=1.5)
    f.parts.append(f'<path d="M{mx},{ty - 16}l9,3.5l-9,3.5Z" fill="currentColor"/>')
    f.text(mx, ty + 24, "marker", size=12, tone=NOTE, anchor="middle")
    f.text(start_x + 50, ty + 24, "samples", size=12, tone=NOTE, anchor="middle")
    f.text((start_x + stop_x) / 2, ty + 46, "stamped on CTimer 4, the stream's clock", size=13, tone=NOTE,
           anchor="middle")
    return f, ty + 54


FIGURES = [blocks, command, packet, memory, energy]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--write", action="store_true", help="replace docs/figures/ rather than check it")
    ap.add_argument("--fonts", type=pathlib.Path,
                    default=ROOT.parent / "escapement-project.github.io" / "static" / "fonts",
                    help="the site's web fonts, to measure labels in")
    a = ap.parse_args()
    find_fonts(a.fonts)
    drawn, misfits = {}, []
    for make in FIGURES:
        f, h = make()
        drawn[f"{f.name}.svg"] = f.svg(h)
        misfits += f.misfits
    if a.write:
        OUT.mkdir(parents=True, exist_ok=True)
        for name, svg in drawn.items():
            (OUT / name).write_text(svg)
    stale = [name for name, svg in drawn.items()
             if not (OUT / name).is_file() or (OUT / name).read_text() != svg]
    extra = sorted(p.name for p in OUT.glob("*.svg") if p.name not in drawn)
    for line in misfits:
        print(line)
    for name in stale:
        print(f"docs/figures/{name} is not what tools/figures.py draws: run it with --write")
    for name in extra:
        print(f"docs/figures/{name} is drawn by nothing here")
    if SKIPPED:
        print(f"labels not measured in: {', '.join(SKIPPED)}")
    if misfits or stale or extra:
        sys.exit(1)
    print(f"{len(drawn)} figures as drawn{', every label fits' if not SKIPPED else ''}")


if __name__ == "__main__":
    main()
