#!/usr/bin/env python3
"""make_icon.py [out.png]: the page's icon (icon.png), drawn from scratch: heavy rain over an invented night city, a
red haze on the horizon and a red streak across the sky. Original shapes only (no game art, no real building, no
lettering). Needs Pillow. Drawn at 2048 px and scaled to 512 for smooth edges; the random parts have fixed seeds, so it
is the same each run.
"""
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 2048
OUT = 512
HORIZON = 1500  # where the street-level haze is brightest


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def sky():
    img = Image.new("RGB", (S, S))
    d = ImageDraw.Draw(img)
    for y in range(S):
        t = y / HORIZON
        c = lerp((7, 9, 16), (52, 58, 76), t ** 1.6) if y < HORIZON else (52, 58, 76)
        d.line([(0, y), (S, y)], fill=c)
    img = img.convert("RGBA")
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))  # the city's red haze, low in the sky
    ImageDraw.Draw(glow).ellipse([-300, HORIZON - 520, S + 300, HORIZON + 380], fill=(150, 24, 30, 120))
    return Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(140)))


def city(rng, base, low, high, width, color, window_chance, window_color):
    """a row of invented buildings standing on base: flat or stepped tops, a few lit windows"""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x = -60
    while x < S + 60:
        w = int(rng.uniform(*width))
        top = int(base - rng.uniform(low, high))
        d.rectangle([x, top, x + w, S], fill=color)
        if rng.random() < 0.45:  # a stepped top: a narrower block on the roof
            inset = int(w * rng.uniform(0.18, 0.32))
            step = int(rng.uniform(40, 110))
            d.rectangle([x + inset, top - step, x + w - inset, top], fill=color)
            if rng.random() < 0.4:  # and sometimes a mast
                mx = x + w // 2
                d.rectangle([mx - 5, top - step - int(rng.uniform(60, 140)), mx + 5, top - step], fill=color)
        for wy in range(top + 30, S, 46):  # the windows, in a grid, a few lit
            for wx in range(x + 18, x + w - 24, 38):
                if rng.random() < window_chance:
                    d.rectangle([wx, wy, wx + 16, wy + 22], fill=window_color + (int(rng.uniform(120, 230)),))
        x += w + int(rng.uniform(4, 26))
    return layer


def rain(rng, count, length, width, alpha):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for _ in range(count):
        n = rng.uniform(*length)
        x, y = rng.uniform(-300, S), rng.uniform(-400, S)
        d.line([(x, y), (x + n * 0.22, y + n)], fill=(200, 210, 228, int(rng.uniform(*alpha))),
               width=int(rng.uniform(*width)))
    return layer.filter(ImageFilter.GaussianBlur(1.0))


def streak():
    """a red tracer across the sky, brightest at its head, with a glow"""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = 160, 760, 1780, 420
    steps = 400
    for i in range(steps):
        t = i / steps
        x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
        r = 3 + 9 * t
        d.ellipse([x - r, y - r, x + r, y + r], fill=(232, 40, 40, int(30 + 225 * t ** 1.5)))
    glow = layer.filter(ImageFilter.GaussianBlur(26))
    head = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(head).ellipse([x1 - 16, y1 - 16, x1 + 16, y1 + 16], fill=(255, 214, 200, 255))
    return Image.alpha_composite(Image.alpha_composite(glow, layer), head.filter(ImageFilter.GaussianBlur(3)))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.png"
    img = sky()
    img = Image.alpha_composite(img, rain(random.Random(7), 260, (90, 260), (2, 4), (25, 70)))  # far rain
    img = Image.alpha_composite(img, city(random.Random(11), HORIZON, 200, 620, (110, 220),
                                          (36, 40, 54, 255), 0.10, (196, 178, 132)).filter(ImageFilter.GaussianBlur(2)))
    img = Image.alpha_composite(img, streak())
    img = Image.alpha_composite(img, city(random.Random(23), S - 40, 260, 720, (170, 330),
                                          (13, 15, 22, 255), 0.13, (232, 206, 150)))
    img = Image.alpha_composite(img, rain(random.Random(31), 170, (180, 460), (4, 8), (60, 140)))  # near rain
    img.convert("RGB").resize((OUT, OUT), Image.LANCZOS).save(out, optimize=True)
    print(out, OUT, "x", OUT)


if __name__ == "__main__":
    main()
