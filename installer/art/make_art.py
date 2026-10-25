"""The installer's pictures, in the website's colours: the tall image of its first and last page
(wizard.png), the emblem at the top of the others (wizard-small.png) and the icon (icon.ico). They are
committed; run this (any Python with Pillow) only to change them."""
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).parent
NIGHT, STONE = (7, 7, 10), (29, 26, 21)
GOLD, GOLD_DEEP, EMBER, PARCHMENT = (242, 198, 90), (184, 134, 46), (255, 122, 47), (236, 224, 195)
FONT = "C:/Windows/Fonts/georgiab.ttf"


# The emblem's sound wave: the four bars a subtitle card shows while its character speaks, caught
# mid-word: (centre x, height) in the emblem's 40-unit square, centred on y = 20, as in the website's SVG.
BARS = ((13.25, 8), (17.75, 15), (22.25, 11), (26.75, 5.5))
BAR_WIDTH = 2.8


def emblem(size, line=None):
    """The emblem: a hexagon round a speaking sound wave, gold, on transparency (drawn 4x, shrunk)."""
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = line or max(4, s // 18)
    k = s / 40
    hexagon = [(20 * k, 2 * k), (36 * k, 11 * k), (36 * k, 29 * k), (20 * k, 38 * k), (4 * k, 29 * k), (4 * k, 11 * k)]
    d.polygon(hexagon, fill=(20, 18, 15, 255))
    d.line(hexagon + [hexagon[0], hexagon[1]], fill=GOLD, width=w, joint="curve")
    for x, height in BARS:
        half = BAR_WIDTH / 2
        d.rounded_rectangle([(x - half) * k, (20 - height / 2) * k, (x + half) * k, (20 + height / 2) * k],
                            radius=half * k, fill=GOLD)
    return img.resize((size, size), Image.LANCZOS)


def glow(base, center, radius, colour, strength):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).ellipse([center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius],
                                  fill=colour + (strength,))
    base.alpha_composite(layer.filter(ImageFilter.GaussianBlur(radius / 2)))


def wizard(width=410, height=785):
    img = Image.new("RGBA", (width, height), NIGHT + (255,))
    d = ImageDraw.Draw(img)
    for y in range(height):  # stone at the top fading to night
        t = y / height
        d.line([(0, y), (width, y)], fill=tuple(int(STONE[i] * (1 - t) + NIGHT[i] * t) for i in range(3)))
    glow(img, (width // 2, int(height * 0.3)), width * 0.55, EMBER, 70)
    rng = random.Random(7)
    sparks = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sparks)
    for _ in range(46):  # embers drifting up
        x, y, r = rng.uniform(0, width), rng.uniform(height * 0.25, height), rng.uniform(1.2, 3.2)
        if 0.12 * width < x < 0.88 * width and 0.5 * height < y < 0.77 * height:
            continue  # not over the name
        sd.ellipse([x - r, y - r, x + r, y + r], fill=EMBER + (int(rng.uniform(90, 230)),))
    img.alpha_composite(sparks.filter(ImageFilter.GaussianBlur(1.2)))
    img.alpha_composite(sparks)

    mark = emblem(int(width * 0.52))
    img.alpha_composite(mark, ((width - mark.width) // 2, int(height * 0.3 - mark.height / 2)))

    d = ImageDraw.Draw(img)
    def centred(text, y, size, colour, spacing=0):
        font = ImageFont.truetype(FONT, size)
        widths = [d.textlength(c, font=font) for c in text]
        x = (width - sum(widths) - spacing * (len(text) - 1)) / 2
        for c, w in zip(text, widths):
            d.text((x, y), c, font=font, fill=colour)
            x += w + spacing
    centred("SPOKEN", int(height * 0.52), int(width * 0.135), GOLD, spacing=width * 0.012)
    centred("PLAYERS", int(height * 0.6), int(width * 0.135), GOLD, spacing=width * 0.008)
    y = int(height * 0.7)
    d.line([(width * 0.22, y), (width * 0.78, y)], fill=GOLD_DEEP, width=2)
    centred("VOICEPACK", y + int(height * 0.02), int(width * 0.06), PARCHMENT, spacing=width * 0.02)
    # A thin gold frame, as the site's panels have.
    d.rectangle([6, 6, width - 7, height - 7], outline=GOLD_DEEP + (160,), width=2)
    img.convert("RGB").save(HERE / "wizard.png")


def small(size=138):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    mark = emblem(int(size * 0.94))
    img.alpha_composite(mark, ((size - mark.width) // 2, (size - mark.height) // 2))
    img.save(HERE / "wizard-small.png")


def icon():
    emblem(256).save(HERE / "icon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


def addon_icon():
    """The addon's icon (its TOC's IconTexture), where the addon is checked out beside this repository:
    a 64x64 uncompressed TGA, as WoW reads it."""
    to = HERE.parents[2] / "spoken-players-addon" / "SpokenPlayers" / "Textures" / "Icon.tga"
    if to.parent.is_dir():
        emblem(64).save(to, compression=None)
        return to


if __name__ == "__main__":
    wizard()
    small()
    icon()
    addon = addon_icon()
    print("wrote", ", ".join([p.name for p in sorted(HERE.glob("*.png"))] + ["icon.ico"] + ([str(addon)] if addon else [])))
