# Generates Scirocco's Windows icons from the mascot art in this folder.
#
#   uv run --no-project --with pillow python tools/brand/make_icons.py
#
# Inputs: scirocco.png (the fox, colour, transparent) and scirocco-mono.png (its one-colour
# silhouette). Outputs keep upstream's file names, so nothing that points at them changes:
#   apollo.ico (repository root)        the application icon: sunshine.exe, installer, uninstaller
#   <web>/images/apollo.ico, logo-apollo-*  the same fox on cream: web UI logo, favicon
#   <web>/images/scirocco-tray.ico      the tray icon: the fox's head in a cream disc
#   <web>/images/apollo-{playing,pausing,locked}*  the tray's states: that disc with a badge
# for both web UIs (web, web-legacy); the tray reads the legacy one's images.
import base64
import io
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
SS = 4
SUBJECT = 0.86
CREAM = ((0xFF, 0xF6, 0xEA), (0xF2, 0xDC, 0xC4))   # top left to bottom right, as on the mock
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
TRAY_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
HEAD = (470, 40, 1240, 810)   # the head, in scirocco.png's pixels
BADGES = {"playing": (0x2E, 0xB8, 0x6E), "pausing": (0x2F, 0x80, 0xED), "locked": (0x6B, 0x72, 0x80)}
WEB_DIRS = [os.path.join(ROOT, "src_assets", "common", "assets", d, "public", "images") for d in ("web", "web-legacy")]


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


def circle_mask(n):
    m = Image.new("L", (n * SS, n * SS), 0)
    ImageDraw.Draw(m).ellipse((0, 0, n * SS - 1, n * SS - 1), fill=255)
    return m.resize((n, n), Image.LANCZOS)


def tray_icon(head, n, state=None):
    """The head in a cream disc; a state adds a coloured badge with its glyph, bottom right."""
    im = cream(n)
    im.alpha_composite(head.resize((n, n), Image.LANCZOS))
    im.putalpha(circle_mask(n))
    if state:
        b = n * SS
        badge = Image.new("RGBA", (b, b), (0, 0, 0, 0))
        d = ImageDraw.Draw(badge)
        r = 0.30 * b
        cx = cy = b - r - 0.01 * b
        ring = 0.05 * b
        d.ellipse((cx - r - ring, cy - r - ring, cx + r + ring, cy + r + ring), fill=(20, 20, 20, 255))
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=BADGES[state] + (255,))
        w, k = (255, 255, 255, 255), r * 0.5
        if state == "playing":
            d.polygon([(cx - k * 0.7, cy - k), (cx - k * 0.7, cy + k), (cx + k, cy)], fill=w)
        elif state == "pausing":
            d.rectangle((cx - k * 0.8, cy - k, cx - k * 0.25, cy + k), fill=w)
            d.rectangle((cx + k * 0.25, cy - k, cx + k * 0.8, cy + k), fill=w)
        else:
            d.rectangle((cx - k * 0.85, cy - k * 0.1, cx + k * 0.85, cy + k), fill=w)
            d.arc((cx - k * 0.55, cy - k, cx + k * 0.55, cy + k * 0.2), 180, 360, fill=w, width=max(1, int(k * 0.28)))
        im.alpha_composite(badge.resize((n, n), Image.LANCZOS))
    return im


def save_ico(draw, sizes, path):
    # Each size drawn on its own: Pillow's ICO writer would otherwise scale the largest down.
    frames = [draw(n) for n in sizes]
    frames[-1].save(path, format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[:-1])


def save_svg(png, path):
    """Upstream's SVGs, kept as files: the bitmap wrapped in one."""
    buf = io.BytesIO()
    png.save(buf, "PNG")
    data = base64.b64encode(buf.getvalue()).decode()
    with open(path, "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
                f'viewBox="0 0 {png.width} {png.height}"><image width="{png.width}" height="{png.height}" '
                f'xlink:href="data:image/png;base64,{data}"/></svg>\n')


def main():
    fox = load_square("scirocco.png")
    head = Image.open(os.path.join(HERE, "scirocco.png")).convert("RGBA").crop(HEAD)
    save_ico(lambda n: app_icon(fox, n), ICO_SIZES, os.path.join(ROOT, "apollo.ico"))
    for web in WEB_DIRS:
        save_ico(lambda n: app_icon(fox, n), ICO_SIZES, os.path.join(web, "apollo.ico"))
        for n in (16, 45):
            app_icon(fox, n).save(os.path.join(web, f"logo-apollo-{n}.png"), optimize=True)
        save_svg(app_icon(fox, 256), os.path.join(web, "logo-apollo.svg"))
        save_ico(lambda n: tray_icon(head, n), TRAY_SIZES, os.path.join(web, "scirocco-tray.ico"))
        for state in BADGES:
            save_ico(lambda n, s=state: tray_icon(head, n, s), TRAY_SIZES, os.path.join(web, f"apollo-{state}.ico"))
            for n in (16, 45):
                tray_icon(head, n, state).save(os.path.join(web, f"apollo-{state}-{n}.png"), optimize=True)
            tray_icon(head, 256, state).save(os.path.join(web, f"apollo-{state}.png"), optimize=True)
            save_svg(tray_icon(head, 256, state), os.path.join(web, f"apollo-{state}.svg"))


if __name__ == "__main__":
    main()
