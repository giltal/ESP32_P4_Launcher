#!/usr/bin/env python3
"""
Controller pictures for the launcher: our own line drawings, one per layout family (no product photos or logos).

    python tools/make_pad_pictures.py [--out SDcard/config/pads] [--preview build/pads_preview.png]

For each family it writes <name>.png (400x220, transparent, the launcher's colours) and <name>.txt, the
position of each button function in the picture:

    # gamepad: Xbox-style
    A=318,128,13
    L1=102,47,36,11

(x,y = centre, r = radius, or rx,ry for a wide button: in the 400x220 picture). The functions are the launcher's (PAPP_BTN_*): A is the
south face button, B east, X west, Y north, whatever the pad prints on them; Map controller rings the one it
asks for. Names: UP DOWN LEFT RIGHT A B X Y L1 R1 L2 R2 SELECT START L3 R3 HOME.

gamepads.txt picks a pad's picture with pic=<name>; without one the launcher picks by maker and kind
(lx_input_pad_picture): Sony = playstation, Nintendo = nintendo (Wii Remote = wiimote / wiimote_nunchuk),
flight stick / throttle / wheel by kind, else gamepad. Needs Pillow.
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 400, 220
S = 3                                   # drawn 3x, scaled down = smooth edges

BODY = (22, 40, 74, 255)                # launcher panel blues
EDGE = (96, 132, 190, 255)
BTN = (36, 64, 110, 255)
BTN_EDGE = (130, 160, 210, 255)
LABEL = (205, 218, 240, 255)
DARK = (12, 22, 42, 255)


def font(px):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, px * S)
        except OSError:
            pass
    return ImageFont.load_default(px * S)


class Pic:
    def __init__(self, name, title):
        self.name, self.title = name, title
        self.img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)
        self.pos = {}

    # -- shapes, in picture coordinates --
    def _b(self, box):
        return [v * S for v in box]

    def rrect(self, x0, y0, x1, y1, r, fill=BODY, edge=EDGE, w=2):
        self.d.rounded_rectangle(self._b((x0, y0, x1, y1)), r * S, fill=fill, outline=edge, width=w * S)

    def ellipse(self, x0, y0, x1, y1, fill=BODY, edge=EDGE, w=2):
        self.d.ellipse(self._b((x0, y0, x1, y1)), fill=fill, outline=edge, width=w * S)

    def poly(self, pts, fill=BODY, edge=EDGE, w=2):
        p = [(x * S, y * S) for x, y in pts]
        self.d.polygon(p, fill=fill)
        self.d.line(p + [p[0]], fill=edge, width=w * S, joint="curve")

    def text(self, x, y, t, px=12, fill=LABEL):
        self.d.text((x * S, y * S), t, font=font(px), fill=fill, anchor="mm")

    # -- controls; each records where its function is --
    def button(self, fn, x, y, r=11, label="", px=11, shape=None):
        self.ellipse(x - r, y - r, x + r, y + r, fill=BTN, edge=BTN_EDGE)
        if shape:
            self.symbol(shape, x, y, r * 0.5)
        elif label:
            self.text(x, y, label, px)
        if fn:
            self.pos[fn] = (x, y, r + 3)

    def symbol(self, shape, x, y, k):
        w = int(1.6 * S)
        if shape == "cross":
            self.d.line(self._b((x - k, y - k, x + k, y + k)), fill=LABEL, width=w)
            self.d.line(self._b((x - k, y + k, x + k, y - k)), fill=LABEL, width=w)
        elif shape == "circle":
            self.d.ellipse(self._b((x - k, y - k, x + k, y + k)), outline=LABEL, width=w)
        elif shape == "square":
            self.d.rectangle(self._b((x - k, y - k, x + k, y + k)), outline=LABEL, width=w)
        elif shape == "triangle":
            p = [(x * S, (y - k) * S), ((x + k) * S, (y + k * 0.8) * S), ((x - k) * S, (y + k * 0.8) * S)]
            self.d.polygon(p, outline=LABEL, width=w)

    def pill(self, fn, x, y, w=22, h=9, label="", px=8):
        self.rrect(x - w / 2, y - h / 2, x + w / 2, y + h / 2, h / 2, fill=BTN, edge=BTN_EDGE, w=1)
        if label:
            self.text(x, y + h / 2 + 7, label, px, fill=EDGE)
        if fn:
            self.pos[fn] = (x, y, w / 2 + 4, h / 2 + 4)

    def dpad(self, x, y, a=11, names=("UP", "DOWN", "LEFT", "RIGHT")):
        """A plus; names = the functions of up, down, left, right as drawn."""
        t = a * 0.95
        pts = [(x - t / 2, y - a - t / 2), (x + t / 2, y - a - t / 2), (x + t / 2, y - t / 2),
               (x + a + t / 2, y - t / 2), (x + a + t / 2, y + t / 2), (x + t / 2, y + t / 2),
               (x + t / 2, y + a + t / 2), (x - t / 2, y + a + t / 2), (x - t / 2, y + t / 2),
               (x - a - t / 2, y + t / 2), (x - a - t / 2, y - t / 2), (x - t / 2, y - t / 2)]
        self.poly(pts, fill=BTN, edge=BTN_EDGE, w=1)
        for fn, (dx, dy) in zip(names, ((0, -1), (0, 1), (-1, 0), (1, 0))):
            if fn:
                self.pos[fn] = (x + dx * a, y + dy * a, t / 2 + 5)

    def stick(self, fn, x, y, r=15):
        self.ellipse(x - r - 4, y - r - 4, x + r + 4, y + r + 4, fill=DARK, edge=EDGE, w=1)
        self.ellipse(x - r, y - r, x + r, y + r, fill=BTN, edge=BTN_EDGE)
        self.ellipse(x - r * 0.55, y - r * 0.55, x + r * 0.55, y + r * 0.55, fill=None, edge=BTN_EDGE, w=1)
        if fn:
            self.pos[fn] = (x, y, r + 6)

    def shoulder(self, fn, x0, y0, x1, y1, label, r=5):
        self.rrect(x0, y0, x1, y1, r, fill=BTN, edge=BTN_EDGE, w=1)
        self.text((x0 + x1) / 2, (y0 + y1) / 2, label, 9)
        if fn:
            self.pos[fn] = ((x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 + 4, (y1 - y0) / 2 + 4)

    def save(self, out):
        img = self.img.resize((W, H), Image.LANCZOS)
        img.save(os.path.join(out, self.name + ".png"), optimize=True)
        with open(os.path.join(out, self.name + ".txt"), "w", newline="\n") as f:
            f.write(f"# {self.name}: {self.title} (tools/make_pad_pictures.py; x,y,r or x,y,rx,ry in the "
                    f"{W}x{H} picture)\n")
            for fn, v in self.pos.items():
                f.write(f"{fn}={','.join(str(round(n)) for n in v)}\n")
        return img


def pad_body(p, top=58):
    """A two-grip pad body."""
    p.d.rounded_rectangle(p._b((70, top, 330, top + 92)), 40 * S, fill=BODY)
    for cx in (98, 302):
        p.ellipse(cx - 48, top + 20, cx + 48, top + 150, fill=BODY, edge=BODY)
    # outline: draw the union's edge by stroking each part, then refill the inside
    p.d.rounded_rectangle(p._b((70, top, 330, top + 92)), 40 * S, outline=EDGE, width=2 * S)
    for cx in (98, 302):
        p.d.ellipse(p._b((cx - 48, top + 20, cx + 48, top + 150)), outline=EDGE, width=2 * S)
    p.d.rounded_rectangle(p._b((72, top + 2, 328, top + 90)), 38 * S, fill=BODY)
    for cx in (98, 302):
        p.d.ellipse(p._b((cx - 46, top + 22, cx + 46, top + 148)), fill=BODY)


def face(p, x, y, labels, shapes=None, gap=19):
    """Four face buttons: functions A south, B east, X west, Y north; labels/shapes in the same order."""
    for i, (fn, dx, dy) in enumerate((("A", 0, 1), ("B", 1, 0), ("X", -1, 0), ("Y", 0, -1))):
        p.button(fn, x + dx * gap, y + dy * gap, 10, labels[i] if labels else "", 10,
                 shapes[i] if shapes else None)


def triggers(p, l1="LB", r1="RB", l2="LT", r2="RT"):
    p.shoulder("L2", 78, 16, 122, 34, l2)
    p.shoulder("R2", 278, 16, 322, 34, r2)
    p.shoulder("L1", 70, 40, 134, 54, l1)
    p.shoulder("R1", 266, 40, 330, 54, r1)


def gamepad():
    p = Pic("gamepad", "Xbox-style pad: left stick above the d-pad")
    triggers(p)
    pad_body(p)
    p.stick("L3", 110, 100)
    p.dpad(150, 150)
    p.stick("R3", 250, 150)
    face(p, 296, 100, ("A", "B", "X", "Y"))
    p.pill("SELECT", 176, 100, label="View")
    p.pill("START", 224, 100, label="Menu")
    p.button("HOME", 200, 76, 9)
    return p


def playstation():
    p = Pic("playstation", "PlayStation-style pad: d-pad above the left stick, symbols")
    triggers(p, "L1", "R1", "L2", "R2")
    pad_body(p)
    p.rrect(160, 64, 240, 108, 8, fill=DARK, edge=BTN_EDGE, w=1)            # touch pad
    p.dpad(104, 100)
    face(p, 296, 100, None, ("cross", "circle", "square", "triangle"))
    p.stick("L3", 154, 150)
    p.stick("R3", 246, 150)
    p.pill("SELECT", 146, 72, 10, 16, "")
    p.pill("START", 254, 72, 10, 16, "")
    p.button("HOME", 200, 128, 8)
    return p


def nintendo():
    p = Pic("nintendo", "Switch-style pad: A east, B south (functions by position)")
    triggers(p, "L", "R", "ZL", "ZR")
    pad_body(p)
    p.stick("L3", 110, 100)
    p.dpad(150, 150)
    p.stick("R3", 250, 150)
    face(p, 296, 100, ("B", "A", "Y", "X"))
    p.button("SELECT", 176, 82, 7, "-", 12)
    p.button("START", 224, 82, 7, "+", 12)
    p.button("HOME", 214, 112, 8)
    return p


def snes():
    p = Pic("snes", "SNES-style pad: d-pad, four buttons, two shoulders")
    p.shoulder("L1", 60, 52, 140, 66, "L")
    p.shoulder("R1", 260, 52, 340, 66, "R")
    p.rrect(110, 70, 290, 170, 50)
    p.ellipse(40, 62, 160, 178)
    p.ellipse(240, 62, 360, 178)
    p.d.rounded_rectangle(p._b((112, 72, 288, 168)), 48 * S, fill=BODY)
    p.dpad(100, 120, 13)
    face(p, 300, 120, ("B", "A", "Y", "X"), gap=21)
    p.pill("SELECT", 180, 124, 22, 8, "SELECT", 7)
    p.pill("START", 220, 124, 22, 8, "START", 7)
    return p


def flightstick():
    p = Pic("flightstick", "Flight stick: trigger, thumb button, hat, base buttons")
    p.ellipse(90, 150, 310, 214)                                              # base
    p.rrect(186, 70, 214, 170, 10)                                            # shaft
    p.rrect(160, 20, 240, 90, 22)                                             # grip head
    p.dpad(200, 46, 9)                                                         # hat on top
    p.button("A", 166, 78, 9, "1", 9)                                         # trigger (front)
    p.button("B", 228, 30, 8, "2", 9)                                         # thumb button
    p.button("X", 128, 180, 9, "3", 9)
    p.button("Y", 156, 194, 9, "4", 9)
    p.button("L1", 244, 194, 9, "5", 9)
    p.button("R1", 272, 180, 9, "6", 9)
    p.pill("SELECT", 112, 160, 16, 8)
    p.pill("START", 288, 160, 16, 8)
    p.rrect(300, 120, 316, 170, 6, fill=BTN, edge=BTN_EDGE, w=1)              # throttle wheel
    p.text(308, 110, "thr", 8, EDGE)
    return p


def throttle():
    p = Pic("throttle", "Throttle: a lever with buttons on its handle")
    p.rrect(70, 150, 330, 206, 16)                                            # base
    p.poly([(150, 160), (190, 160), (250, 60), (210, 60)])                    # lever
    p.rrect(196, 22, 290, 74, 18)                                             # handle
    p.dpad(222, 48, 8)
    p.button("A", 262, 38, 8, "1", 9)
    p.button("B", 274, 60, 8, "2", 9)
    for i, fn in enumerate(("X", "Y", "L1", "R1")):
        p.button(fn, 110 + i * 34, 178, 9, str(i + 3), 9)
    p.pill("SELECT", 270, 178, 16, 8)
    p.pill("START", 300, 178, 16, 8)
    return p


def wheel():
    p = Pic("wheel", "Wheel: buttons on the spokes, paddles behind, pedals")
    p.ellipse(110, 14, 290, 194, fill=None, edge=EDGE, w=14)
    p.ellipse(110, 14, 290, 194, fill=None, edge=BTN_EDGE, w=1)
    p.rrect(150, 90, 250, 140, 16)                                            # hub + spokes
    p.rrect(118, 100, 160, 120, 6)
    p.rrect(240, 100, 282, 120, 6)
    p.dpad(172, 112, 8)
    face(p, 228, 112, ("A", "B", "X", "Y"), gap=12)
    for fn in ("A", "B", "X", "Y"):                                           # smaller there
        p.pos[fn] = p.pos[fn][:2] + (9,)
    p.pill("SELECT", 186, 132, 12, 7)
    p.pill("START", 214, 132, 12, 7)
    p.shoulder("L1", 92, 70, 108, 108, "-")                                   # paddles
    p.shoulder("R1", 292, 70, 308, 108, "+")
    p.shoulder("L2", 24, 150, 64, 206, "Brk")                                 # pedals
    p.shoulder("R2", 336, 150, 376, 206, "Gas")
    return p


def wiimote():
    p = Pic("wiimote", "Wii Remote held sideways: d-pad left, 1 and 2 right")
    p.rrect(30, 70, 370, 150, 30)
    p.dpad(78, 110, 13)             # held sideways the arms point the way they read (lx_pad_wii.c turns them)
    p.button("X", 134, 110, 13, "A", 11)
    p.button("SELECT", 196, 92, 7, "-", 12)
    p.button("HOME", 196, 110, 7)
    p.button("START", 196, 128, 7, "+", 12)
    p.button("B", 290, 110, 11, "1", 11)
    p.button("A", 330, 110, 11, "2", 11)
    p.text(134, 162, "B underneath", 8, EDGE)
    p.pos["Y"] = (134, 162, 26)
    return p


def wiimote_nunchuk():
    p = Pic("wiimote_nunchuk", "Wii Remote upright with a Nunchuk")
    p.rrect(230, 10, 300, 212, 24)                                            # remote
    p.dpad(265, 44, 12)
    p.button("A", 265, 84, 12, "A", 11)
    p.button("SELECT", 245, 118, 7, "-", 12)
    p.button("HOME", 265, 118, 7)
    p.button("START", 285, 118, 7, "+", 12)
    p.button("X", 265, 162, 9, "1", 10)
    p.button("Y", 265, 188, 9, "2", 10)
    p.text(318, 84, "B", 10, EDGE)
    p.pos["B"] = (310, 84, 12)
    p.d.line(p._b((230, 200, 190, 214, 150, 200)), fill=EDGE, width=2 * S)    # cable
    p.ellipse(90, 40, 170, 206)                                               # nunchuk
    p.stick(None, 130, 84, 16)
    p.pos["L3"] = (130, 84, 22)
    p.shoulder("L1", 92, 26, 130, 40, "C")
    p.shoulder("L2", 130, 22, 168, 40, "Z")
    return p


FAMILIES = (gamepad, playstation, nintendo, snes, flightstick, throttle, wheel, wiimote, wiimote_nunchuk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "SDcard", "config", "pads"))
    ap.add_argument("--preview", help="also write every picture with its positions ringed into one PNG")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    pics = []
    for make in FAMILIES:
        p = make()
        pics.append((p, p.save(a.out)))
        print(f"{p.name}: {len(p.pos)} positions")
    if a.preview:
        cols = 3
        sheet = Image.new("RGBA", (W * cols, H * ((len(pics) + cols - 1) // cols)), (11, 22, 48, 255))
        for i, (p, img) in enumerate(pics):
            ox, oy = (i % cols) * W, (i // cols) * H
            sheet.alpha_composite(img, (ox, oy))
            d = ImageDraw.Draw(sheet)
            for fn, v in p.pos.items():
                x, y, rx = v[:3]
                ry = v[3] if len(v) > 3 else rx
                d.ellipse((ox + x - rx, oy + y - ry, ox + x + rx, oy + y + ry), outline=(245, 166, 35, 255), width=2)
                d.text((ox + x + rx - 2, oy + y - ry - 6), fn, fill=(245, 166, 35, 255))
            d.text((ox + 6, oy + 4), p.name, fill=(255, 255, 255, 255))
        os.makedirs(os.path.dirname(os.path.abspath(a.preview)), exist_ok=True)
        sheet.save(a.preview)
        print("preview:", a.preview)


if __name__ == "__main__":
    main()
