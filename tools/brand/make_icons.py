# Generates Scirocco's Windows icons from the mascot art in this folder.
#
#   uv run --no-project --with pillow python tools/brand/make_icons.py
#
# Inputs: scirocco.png (the fox, colour, transparent) and scirocco-mono.png (its one-colour
# silhouette). Output: apollo.ico at the repository root, the application icon (sunshine.exe, the
# installer and the uninstaller): the fox at 86% of a rounded cream square, at every size Windows
# asks for. The file keeps upstream's name so nothing that points at it changes.
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
SS = 4
SUBJECT = 0.86
CREAM = ((0xFF, 0xF6, 0xEA), (0xF2, 0xDC, 0xC4))   # top left to bottom right, as on the mock
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def load_square(name):
    im = Image.open(os.path.join(HERE, name)).convert("RGBA")
    im = im.crop(im.getchannel("A").getbbox())
    side = max(im.size)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
    return sq


def cream(n):
    small = 64
    g = Image.new("RGBA", (small, small))
    px = g.load()
    a, b = CREAM
    for y in range(small):
        for x in range(small):
            t = (x + y) / (2 * (small - 1))
            px[x, y] = tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)
    return g.resize((n, n), Image.BICUBIC)


def rounded_mask(n, radius):
    m = Image.new("L", (n * SS, n * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, n * SS - 1, n * SS - 1), radius * SS, fill=255)
    return m.resize((n, n), Image.LANCZOS)


def app_icon(fox, n):
    im = cream(n)
    side = round(n * SUBJECT)
    im.alpha_composite(fox.resize((side, side), Image.LANCZOS), ((n - side) // 2, (n - side) // 2))
    im.putalpha(rounded_mask(n, n * 0.22))
    return im


def main():
    fox = load_square("scirocco.png")
    big = app_icon(fox, 256)
    # Each size drawn on its own: Pillow's ICO writer would otherwise scale the 256 down.
    frames = [app_icon(fox, n) for n in ICO_SIZES]
    big.save(os.path.join(ROOT, "apollo.ico"), format="ICO", sizes=[(n, n) for n in ICO_SIZES],
             append_images=frames[:-1])


if __name__ == "__main__":
    main()
