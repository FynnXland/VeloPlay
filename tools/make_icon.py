#!/usr/bin/env python3
"""Draws the VeloPlay mark and writes the files the build uses.

The mark: a play triangle with two fainter copies trailing behind it. The
copies stand for the frames VeloPlay computes between the ones the console
sends. Drawn from numbers instead of exported from a design tool, so the
program icon, the README image and the mark inside the client (see
drawLogo() in src/ui/Widgets.cpp, same numbers) cannot drift apart.

    python3 tools/make_icon.py        # writes assets/veloplay.ico, assets/logo.png, assets/logo.svg
"""
from pathlib import Path

from PIL import Image, ImageDraw

# Everything in units of the tile's edge (0..1).
TILE_RADIUS = 0.225
TILE_TOP = (0x1E, 0x23, 0x2B)
TILE_BOTTOM = (0x12, 0x14, 0x18)
ACCENT = (0x2A, 0x6F, 0xE8)  # the client's accent colour (src/ui/Theme.cpp)
# The triangle's tip points right; `x` is its left edge.
TRIANGLE_HEIGHT = 0.52
TRIANGLE_WIDTH = 0.44
CORNER = 0.035  # rounding of the triangle's corners
LAYERS = [  # left edge, opacity: back to front
    (0.180, 0.28),
    (0.280, 0.55),
    (0.380, 1.00),
]


def triangle(x: float) -> list[tuple[float, float]]:
    top = 0.5 - TRIANGLE_HEIGHT / 2
    return [(x, top), (x + TRIANGLE_WIDTH, 0.5), (x, top + TRIANGLE_HEIGHT)]


def render(size: int) -> Image.Image:
    scale = 8 if size <= 64 else 4
    edge = size * scale
    tile = Image.new("RGBA", (edge, edge), (0, 0, 0, 0))
    gradient = Image.new("RGBA", (edge, edge))
    pixels = ImageDraw.Draw(gradient)
    for y in range(edge):
        t = y / (edge - 1)
        colour = tuple(round(a + (b - a) * t) for a, b in zip(TILE_TOP, TILE_BOTTOM))
        pixels.line([(0, y), (edge, y)], fill=colour + (255,))
    mask = Image.new("L", (edge, edge), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, edge - 1, edge - 1], radius=TILE_RADIUS * edge, fill=255)
    tile.paste(gradient, (0, 0), mask)

    for x, opacity in LAYERS:
        layer = Image.new("L", (edge, edge), 0)
        draw = ImageDraw.Draw(layer)
        # A polygon shrunk by the corner radius, then widened again with a
        # round pen: that rounds the corners.
        points = [(px * edge, py * edge) for px, py in inset(triangle(x), CORNER)]
        draw.polygon(points, fill=255)
        draw.line(points + [points[0], points[1]], fill=255, width=round(2 * CORNER * edge), joint="curve")
        colour = Image.new("RGBA", (edge, edge), ACCENT + (0,))
        colour.putalpha(layer.point(lambda v: round(v * opacity)))
        tile = Image.alpha_composite(tile, Image.composite(colour, Image.new("RGBA", (edge, edge), (0, 0, 0, 0)), mask))
    return tile.resize((size, size), Image.LANCZOS)


def inset(points: list[tuple[float, float]], distance: float) -> list[tuple[float, float]]:
    """Moves every edge of a convex polygon inwards by `distance`."""
    count = len(points)
    cx = sum(p[0] for p in points) / count
    cy = sum(p[1] for p in points) / count
    lines = []
    for i in range(count):
        (x1, y1), (x2, y2) = points[i], points[(i + 1) % count]
        nx, ny = y2 - y1, x1 - x2
        length = (nx * nx + ny * ny) ** 0.5
        nx, ny = nx / length, ny / length
        if (cx - x1) * nx + (cy - y1) * ny < 0:
            nx, ny = -nx, -ny
        lines.append((nx, ny, nx * (x1 + nx * distance) + ny * (y1 + ny * distance)))
    result = []
    for i in range(count):
        a1, b1, c1 = lines[i - 1]
        a2, b2, c2 = lines[i]
        det = a1 * b2 - a2 * b1
        result.append(((c1 * b2 - c2 * b1) / det, (a1 * c2 - a2 * c1) / det))
    return result


def svg() -> str:
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" role="img" aria-label="VeloPlay">',
        '<defs><linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="#{TILE_TOP[0]:02x}{TILE_TOP[1]:02x}{TILE_TOP[2]:02x}"/>'
        f'<stop offset="1" stop-color="#{TILE_BOTTOM[0]:02x}{TILE_BOTTOM[1]:02x}{TILE_BOTTOM[2]:02x}"/>'
        "</linearGradient></defs>",
        f'<rect width="512" height="512" rx="{TILE_RADIUS * 512:.1f}" fill="url(#tile)"/>',
    ]
    accent = f"#{ACCENT[0]:02x}{ACCENT[1]:02x}{ACCENT[2]:02x}"
    for x, opacity in LAYERS:
        points = " ".join(f"{px * 512:.1f},{py * 512:.1f}" for px, py in inset(triangle(x), CORNER))
        parts.append(
            f'<polygon points="{points}" fill="{accent}" stroke="{accent}" stroke-width="{2 * CORNER * 512:.1f}" '
            f'stroke-linejoin="round" opacity="{opacity}"/>'
        )
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "assets"
    out.mkdir(exist_ok=True)
    sizes = [256, 128, 64, 48, 32, 24, 16]
    images = [render(size) for size in sizes]
    images[0].save(out / "veloplay.ico", format="ICO", append_images=images[1:], sizes=[(s, s) for s in sizes])
    render(512).save(out / "logo.png", optimize=True)
    (out / "logo.svg").write_text(svg(), encoding="utf-8")


if __name__ == "__main__":
    main()
