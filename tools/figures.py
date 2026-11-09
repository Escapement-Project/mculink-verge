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

    def rect(self, x, y, w, h, r=4, tone=None, width=1.5, fill=None):
        d = (f"M{num(x + r)},{num(y)}H{num(x + w - r)}A{num(r)},{num(r)} 0 0 1 {num(x + w)},{num(y + r)}"
             f"V{num(y + h - r)}A{num(r)},{num(r)} 0 0 1 {num(x + w - r)},{num(y + h)}H{num(x + r)}"
             f"A{num(r)},{num(r)} 0 0 1 {num(x)},{num(y + h - r)}V{num(y + r)}A{num(r)},{num(r)} 0 0 1 {num(x + r)},{num(y)}Z")
        self.path(d, tone=tone, width=width, fill=fill)

    def head(self, x, y, angle, tone=None, size=7):
        """A filled arrowhead with its tip at (x, y), pointing along `angle` (degrees, 0 = right)."""
        a = math.radians(angle)
        bx, by = x - size * math.cos(a), y - size * math.sin(a)
        nx, ny = -math.sin(a) * size * 0.45, math.cos(a) * size * 0.45
        d = f"M{num(x)},{num(y)}L{num(bx + nx)},{num(by + ny)}L{num(bx - nx)},{num(by - ny)}Z"
        cls = {"wire": ' class="vf-wiref"', "rule": ' class="vf-rulef"'}.get(tone, "")
        self.parts.append(f'<path d="{d}" fill="currentColor"{cls}/>')

    def arrow(self, pts, tone=None, dash=None, start=False, end=True):
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
        self.path("M" + "L".join(f"{num(x)},{num(y)}" for x, y in line), tone=tone, dash=dash)
        for p, q, on in ((pts[-2], pts[-1], end), (pts[1], pts[0], start)):
            if on:
                self.head(*q, math.degrees(math.atan2(q[1] - p[1], q[0] - p[0])), tone=tone)

    def node(self, x, y, w, lines, size=13, pad=7, lead=16, h=None):
        """A box of centred lines, each a string or a (text, kind, tone), centred on y. It is as
        tall as its lines unless `h` says more. Returns its top and bottom."""
        rows = [(s, "text", None) if isinstance(s, str) else s for s in lines]
        h = max(h or 0, len(rows) * lead + 2 * pad - (lead - size) + 2)
        top = y - h / 2
        self.rect(x, top, w, h, fill="surface", tone="hair", width=1)
        first = y - (len(rows) - 1) * lead / 2 + size * 0.35
        for i, (s, kind, tone) in enumerate(rows):
            self.text(x + w / 2, first + i * lead, s, size=size, kind=kind, tone=tone, anchor="middle",
                      room=w - 2 * pad)
        return top, top + h

    def heading(self, x, y, title, job, room, anchor="start"):
        self.text(x, y, title, size=17, kind="head", anchor=anchor, room=room)
        if job:
            self.text(x, y + 20, job, size=14, tone="2", anchor=anchor, room=room)

    def mailbox(self, x0, x1, y):
        """Each core raises the other's mailbox: a dashed arrow both ways, as the Verge page dashes it."""
        self.arrow([(x0, y), (x1, y)], tone="rule", dash="4 3", start=True)
        self.text((x0 + x1) / 2, y - 8, "mailbox", size=12, kind="code", tone="2", anchor="middle", room=x1 - x0)

    def svg(self, height) -> str:
        body = "\n".join(self.parts)
        return (f'<svg xmlns="http://www.w3.org/2000/svg" class="vf" viewBox="0 0 {W} {num(height)}" '
                f'width="{W}" height="{num(height)}">\n<style>\n{STYLE}\n</style>\n{body}\n</svg>\n')


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
    tw = 136
    tx = W - 1 - tw
    chip_w = tx - 20 - chip_x
    pad, col, mid_w = 18, 154, 120
    c0x = chip_x + pad
    c1x = chip_x + chip_w - pad - col
    gap = (c1x - c0x - col - mid_w) / 2
    mid_x = c0x + col + gap
    y0 = top + 46
    c0 = core_rows(f, c0x, y0, col, "CPU 0", "Answers the host",
                   ["USB, every endpoint", "The energy stream", "LEDs, reboots", "The serial consoles"], right=True)
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
    # The target: a second package, its pins level with the lines that reach them.
    pins = [(c1[1], "SWD, JTAG, cJTAG", "wire"), (c1[2], "SWO", None), (c1[3], "DIO, SPI, I2C", None),
            (c1[4], "Supply", None)]
    f.rect(tx, top, tw, chip_h, tone="rule")
    f.heading(tx + 12, y0, "Target", "Under debug", tw - 16)
    for yy, name, tone in pins:
        f.text(tx + 12, yy + 5, name, size=13, room=tw - 16)
        f.arrow([(c1x + col + 4, yy), (tx - 2, yy)], tone=tone)
    f.text(tx + 12, serial_y + 5, "Serial", size=13, room=tw - 16)
    # The consoles' line leaves their row from below, then runs under the shared memory and CPU 1's
    # list to the target's serial lines.
    drop = c0x + 60
    f.arrow([(drop, c0[3] + 12), (drop, serial_y), (tx - 2, serial_y)], start=True)
    return f, top + chip_h + 2


# ---------------------------------------------------------------- a command's path

def command():
    f = Fig("architecture-command")
    lanes = [("Host", "USB"), ("CPU 0", None), ("Shared memory", "USB SRAM, SRAM3"), ("CPU 1", None),
             ("Target", "The wire")]
    xs = [34, 190, 360, 530, 682]
    for (name, sub), x in zip(lanes, xs):
        f.text(x, 22, name, size=14, kind="strong", anchor="middle", room=150)
        if sub:
            f.text(x, 40, sub, size=13, tone="2", anchor="middle", room=150)
    HOST, C0, MEM, C1, TGT = range(5)
    # The list's step number, if the arrow starts one; from, to; the label; tone; both ways.
    steps = [
        (1, C0, MEM, "arm a free request slot", None, False),
        (2, HOST, C0, "command, `EP1 OUT`", None, False),
        (None, C0, C1, "an abort only sets CPU 1's flag", "rule", False),
        (3, C0, MEM, "copy it into `dap_stage`", None, False),
        (None, C0, C1, "mailbox", "rule", False),
        (4, MEM, C1, "take the oldest command", None, False),
        (5, C1, TGT, "run it", "wire", True),
        (6, C1, MEM, "publish the reply", None, False),
        (None, C1, C0, "mailbox", "rule", False),
        (7, MEM, C0, "copy the reply out", None, False),
        (None, C0, HOST, "reply, `EP1 IN`", None, False),
    ]
    actor = {1: C0, 2: C0, 3: C0, 4: C1, 5: C1, 6: C1, 7: C0}
    y0, pitch = 80, 34
    breaks = {i: [] for i in range(5)}
    for n, (badge, a, b, label, tone, both) in enumerate(steps):
        y = y0 + n * pitch
        on = actor.get(badge)
        if badge:
            breaks[on].append((y - 12, y + 12))
            f.parts.append(f'<circle cx="{xs[on]}" cy="{num(y)}" r="10" fill="none" stroke="currentColor" stroke-width="1.5"/>')
            f.text(xs[on], y + 4.5, str(badge), size=13, kind="strong", anchor="middle")
        d = 1 if xs[b] > xs[a] else -1
        x0 = xs[a] + d * (12 if badge and a == on else 3)
        x1 = xs[b] - d * (12 if badge and b == on else 3)
        f.arrow([(x0, y), (x1, y)], tone=tone, dash="4 3" if tone == "rule" else None, start=both)
        mailbox = label == "mailbox"
        size = 12 if mailbox else 13
        f.text((x0 + x1) / 2, y - 7, label, size=size, kind="code" if mailbox else "text",
               tone="2" if tone == "rule" else None, anchor="middle", room=abs(x1 - x0) - 4)
        # A label sits centred over its arrow: break each lifeline the arrow crosses around it.
        for i, x in enumerate(xs):
            if min(x0, x1) < x < max(x0, x1):
                breaks[i].append((y - 7 - size, y - 3))
    bottom = y0 + (len(steps) - 1) * pitch + 20
    for i, x in enumerate(xs):
        t = 50
        for a, b in sorted(breaks[i]):
            if a > t:
                f.path(f"M{x},{num(t)}V{num(a)}", tone="hair", width=1)
            t = max(t, b)
        f.path(f"M{x},{num(t)}V{num(bottom)}", tone="hair", width=1)
    return f, bottom + 4


# ---------------------------------------------------------------- memory

NOTE = "2"


def memory():
    f = Fig("architecture-memory")
    cols = [("CPU 0", 108, 196), ("Both cores", 316, 196), ("CPU 1", 524, 195)]
    for name, x, w in cols:
        f.text(x, 24, name, size=17, kind="head", room=w)
    f.path(f"M0,36H{W}", tone="rule", width=1)
    rows = [
        ("Flash", "0x0000_0000", {
            0: ["CPU 0's code, from `0x200`"],
            2: ["CPU 1's code,", "from `0x4_0000`"]}),
        ("SRAMX", "0x0400_0000", {
            2: ["The generated wire code", ("written at run time", "text", NOTE)]}),
        ("SRAM0", "0x2000_0000", {
            0: ["Data, and the stack in the top", "4 KiB; the loop, and the code", "a command passes through",
                ("The consoles' rings and the", "text", NOTE), ("energy ring, which DMA fills", "text", NOTE)],
            1: ["Energy capture's state, held", "by one core at a time"]}),
        ("SRAM1", "0x2001_0000", {
            1: ["CPU 1's abort flag, which", "CPU 0 sets"],
            2: ["16 KiB of stack, then data;", "the command path, the SWD", "generator, the JTAG scans"]}),
        ("SRAM2", "0x2002_0000", {
            2: ["The rest of CPU 1's data"]}),
        ("SRAM3", "0x2003_0000", {
            1: ["`dap_stage`, the top 8 KiB", "from `0x2003_E000`: staged", "commands and replies, HID",
                "replies, Verge Aux's slot"]}),
        ("SRAM4", "0x2004_0000", {
            1: ["Stream messages: 16 slots", "of 512 bytes", ("written by the core that", "text", NOTE),
                ("holds the capture state", "text", NOTE)]}),
        ("USB SRAM", "0x4010_0000", {
            0: ["TinyUSB's buffers, Verge", "Aux's included, in the first", "4 KiB",
                ("written by CPU 0 and the", "text", NOTE), ("USB controller", "text", NOTE)],
            1: ["`dap_shared`, from", "`0x4010_1000`: request and", "reply slots, queues, DMA's",
                "channel table, the SWO ring", ("written by both cores, the", "text", NOTE),
                ("USB controller and DMA", "text", NOTE)]}),
    ]
    y = 36
    for name, address, cells in rows:
        h = max(len(lines) for lines in cells.values()) * 16 + 2 * 7 - 3 + 2 + 14
        mid = y + h / 2
        f.text(0, mid - 2, name, size=14, kind="strong", room=100)
        f.text(0, mid + 15, address, size=12, kind="code", tone="2", room=100)
        for c, lines in cells.items():
            _, x, w = cols[c]
            f.node(x, mid, w, lines)
        y += h
        f.path(f"M0,{num(y)}H{W}", tone="hair", width=1)
    return f, y + 2


# ---------------------------------------------------------------- energy capture

def energy():
    f = Fig("architecture-energy")
    adc, ring, cpu0, slots = (72, 104), (204, 96), (326, 190), (542, 84)
    boxes = [
        (adc, [("ADC0", "strong", None), "the target's", "voltage and", "current: a pair"]),
        (ring, [("Ring", "strong", None), "in SRAM0, in", "segments of", "about 1 ms", "or longer"]),
        (cpu0, [("CPU 0", "strong", None), "adds each segment to the", "energy total; writes the",
                "stream's messages", ("its loop switches the range", "text", NOTE), ("back to low", "text", NOTE)]),
        (slots, [("16 slots", "strong", None), "in SRAM4"]),
    ]
    h = max(len(lines) for _, lines in boxes) * 16 + 2 * 7 - 3 + 2
    y = 52 + h / 2
    for (x, w), lines in boxes:
        f.node(x, y, w, lines, h=h)
    f.text(0, y - 4, "Target", size=14, kind="strong", room=56)
    f.text(0, y + 13, "supply", size=14, kind="strong", room=56)
    f.arrow([(56, y), (adc[0] - 2, y)])
    f.arrow([(adc[0] + adc[1] + 2, y), (ring[0] - 2, y)])
    f.text((adc[0] + adc[1] + ring[0]) / 2, y - 7, "DMA", size=11, kind="code", tone="2", anchor="middle",
           room=ring[0] - adc[0] - adc[1] - 4)
    f.arrow([(ring[0] + ring[1] + 2, y), (cpu0[0] - 2, y)])
    f.arrow([(cpu0[0] + cpu0[1] + 2, y), (slots[0] - 2, y)])
    out = slots[0] + slots[1]
    f.arrow([(out + 2, y), (W - 2, y)])
    f.text(W - 1, y - 9, "Verge Aux", size=13, anchor="end", room=W - out - 6)
    f.text(W - 1, y + 20, "`EP5 IN`", size=13, anchor="end", room=W - out - 6)
    # The timer that paces the ADC.
    ax = adc[0] + adc[1] / 2
    f.node(adc[0], 16, adc[1], ["CTimer 2"], h=26)
    f.arrow([(ax, 29), (ax, y - h / 2 - 2)])
    f.text(ax + 8, (29 + y - h / 2) / 2 + 4, "triggers each pair", size=12, tone="2", room=cpu0[0] - ax - 8)
    # Control below the ADC, the stream's clock below the slots.
    cy = y + h / 2 + 52
    ct, cb = f.node(adc[0], cy, ring[0] + ring[1] - adc[0],
                    [("CPU 1", "strong", None), "starts, stops and reads a capture", "(`0x90`); calibrates"], h=56)
    f.arrow([(ax, ct - 2), (ax, y + h / 2 + 2)])
    sx = cpu0[0] + 60
    f.node(sx, cy, W - 1 - sx, [("CTimer 4: the stream's clock", "strong", None),
                                "pairs, DIO edges and markers are stamped on it"], h=56)
    f.arrow([(slots[0] + slots[1] / 2, ct - 2), (slots[0] + slots[1] / 2, y + h / 2 + 2)])
    # Which core holds the capture state.
    sy = cb + 40
    f.text(0, sy + 5, "The capture state", size=14, kind="strong", room=140)
    spans = [("CPU 1", "while no capture runs"), ("CPU 0", "while one runs"), ("CPU 1", "once it stops")]
    x0, gap = 150, 40
    seg = (W - 1 - x0 - 2 * gap) / 3
    for i, (who, when) in enumerate(spans):
        x = x0 + i * (seg + gap)
        f.rect(x, sy - 20, seg, 46, fill="surface", tone="hair", width=1)
        f.text(x + 10, sy - 1, who, size=14, kind="strong", room=seg - 20)
        f.text(x + 10, sy + 17, when, size=13, tone=NOTE, room=seg - 20)
        if i < 2:
            f.arrow([(x + seg + 4, sy + 3), (x + seg + gap - 4, sy + 3)])
            f.text(x + seg + gap / 2, sy - 4, ("start", "stop")[i], size=12, tone=NOTE, anchor="middle",
                   room=gap)
    return f, sy + 28


FIGURES = [blocks, command, memory, energy]


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
