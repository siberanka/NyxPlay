#!/usr/bin/env python3
"""
NyxPlay branding generator.

Turns the two artwork sources in ``branding/source`` into every icon that the
Android client, the PC client and NyxPlay Host ship with:

* ``nyxplay-app-source.png``  -> client logo (Android + PC)
* ``nyxplay-host-source.png`` -> NyxPlay Host logo and tray icons

The sources are delivered on an opaque dark background. This script removes
that background (keeping soft glows as real alpha), renders master PNGs into
``branding/logo`` and then exports the platform specific assets in place.

Usage::

    python branding/tools/generate_branding.py            # regenerate everything
    python branding/tools/generate_branding.py --masters  # only branding/logo

Requirements: Python 3.9+, Pillow >= 9.1, numpy.
"""

from __future__ import annotations

import argparse
import base64
import io
import math
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
BRANDING = ROOT / "branding"
SOURCE = BRANDING / "source"
LOGO = BRANDING / "logo"
FONT = BRANDING / "fonts" / "Outfit-VF.ttf"

# Brand palette (derived from the artwork)
NIGHT = (11, 12, 33)          # interior of the client icon
NIGHT_DEEP = (6, 6, 18)
LAVENDER = (196, 181, 253)
VIOLET = (139, 92, 246)


# --------------------------------------------------------------------------
# Generic helpers
# --------------------------------------------------------------------------

def _to_array(im: Image.Image) -> np.ndarray:
    return np.asarray(im.convert("RGB")).astype(np.float32)


def _from_rgba(rgb: np.ndarray, alpha: np.ndarray) -> Image.Image:
    rgba = np.dstack([np.clip(rgb, 0, 255), np.clip(alpha, 0, 1) * 255.0])
    return Image.fromarray(rgba.round().astype(np.uint8), "RGBA")


def _flood_outside(mask_passable: np.ndarray) -> np.ndarray:
    """Return the region connected to the image border through passable pixels."""
    h, w = mask_passable.shape
    outside = np.zeros_like(mask_passable, dtype=bool)
    q: deque[tuple[int, int]] = deque()
    for x in range(w):
        for y in (0, h - 1):
            if mask_passable[y, x] and not outside[y, x]:
                outside[y, x] = True
                q.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if mask_passable[y, x] and not outside[y, x]:
                outside[y, x] = True
                q.append((y, x))
    while q:
        y, x = q.popleft()
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < h and 0 <= nx < w and mask_passable[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                q.append((ny, nx))
    return outside


def _feather(mask: np.ndarray, radius: float) -> np.ndarray:
    im = Image.fromarray((mask.astype(np.float32) * 255).astype(np.uint8), "L")
    im = im.filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(im).astype(np.float32) / 255.0


def _crop_to_content(im: Image.Image, pad_ratio: float = 0.0) -> Image.Image:
    bbox = im.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    if not bbox:
        return im
    x0, y0, x1, y1 = bbox
    side = max(x1 - x0, y1 - y0)
    side = int(round(side * (1 + 2 * pad_ratio)))
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(im, (int(round(side / 2 - cx)), int(round(side / 2 - cy))))
    return canvas


def resize(im: Image.Image, size: int | tuple[int, int], sharpen: bool | None = None) -> Image.Image:
    if isinstance(size, int):
        size = (size, size)
    # Work in premultiplied space to avoid dark fringes around soft edges.
    arr = np.asarray(im.convert("RGBA")).astype(np.float32) / 255.0
    premul = arr.copy()
    premul[..., :3] *= premul[..., 3:4]
    channels = [Image.fromarray((premul[..., i] * 255).round().astype(np.uint8), "L") for i in range(4)]
    channels = [c.resize(size, Image.LANCZOS) for c in channels]
    out = np.dstack([np.asarray(c).astype(np.float32) / 255.0 for c in channels])
    a = out[..., 3:4]
    rgb = np.where(a > 1e-4, out[..., :3] / np.maximum(a, 1e-4), 0)
    result = _from_rgba(rgb * 255.0, a[..., 0])
    if sharpen is None:
        sharpen = max(size) <= 64
    if sharpen:
        alpha = result.getchannel("A")
        rgb_im = result.convert("RGB").filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=1))
        result = rgb_im.convert("RGBA")
        result.putalpha(alpha)
    return result


def save_png(im: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path, "PNG", optimize=True)
    print(f"  wrote {path.relative_to(ROOT)}")


def save_ico(master: Image.Image, path: Path, sizes=(16, 20, 24, 32, 40, 48, 64, 128, 256)) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = [resize(master, s) for s in sizes]
    frames[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    print(f"  wrote {path.relative_to(ROOT)}")


def save_icns(master: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    resize(master, 1024, sharpen=False).save(path, format="ICNS")
    print(f"  wrote {path.relative_to(ROOT)}")


def save_svg_wrapper(master: Image.Image, path: Path, size: int = 512, title: str = "NyxPlay") -> None:
    """SVG container around an embedded PNG (keeps the artwork pixel exact)."""
    buf = io.BytesIO()
    resize(master, size, sharpen=False).save(buf, "PNG", optimize=True)
    data = base64.b64encode(buf.getvalue()).decode("ascii")
    svg = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{size}" height="{size}" viewBox="0 0 {size} {size}">\n'
        f'  <title>{title}</title>\n'
        f'  <image width="{size}" height="{size}" xlink:href="data:image/png;base64,{data}"/>\n'
        '</svg>\n'
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(svg, encoding="utf-8", newline="\n")
    print(f"  wrote {path.relative_to(ROOT)}")


def font(size: int, weight: int = 600) -> ImageFont.FreeTypeFont:
    f = ImageFont.truetype(str(FONT), size)
    try:
        f.set_variation_by_axes([weight])
    except Exception:  # pragma: no cover - static font fallback
        pass
    return f


# --------------------------------------------------------------------------
# Background removal
# --------------------------------------------------------------------------

def extract_client_icon(src: Image.Image) -> Image.Image:
    """Rounded square app icon with the outer background removed."""
    a = _to_array(src)
    bg = a[2:12, 2:12].reshape(-1, 3).mean(axis=0)
    dist = np.abs(a - bg).max(axis=2)
    outside = _flood_outside(dist <= 6.0)
    inside = ~outside
    # Close tiny pin holes on the rim and soften the edge by ~1px
    alpha = _feather(inside, 0.9)
    alpha = np.where(inside, np.maximum(alpha, 0.5), alpha)
    alpha = np.clip((alpha - 0.25) / 0.75, 0, 1)
    # Remove the background tint that bleeds into anti-aliased edge pixels
    rgb = a.copy()
    edge = (alpha > 0) & (alpha < 1)
    rgb[edge] = np.clip((a[edge] - bg * (1 - alpha[edge, None])) / np.maximum(alpha[edge, None], 1e-3), 0, 255)
    return _crop_to_content(_from_rgba(rgb, alpha), pad_ratio=0.0)


def extract_client_mark(src: Image.Image) -> Image.Image:
    """Moon artwork without the rounded square frame.

    The artwork keeps its night sky and fades out radially, which makes it
    seamless on top of the ``NIGHT`` colour (adaptive icon background, splash
    screens and dark UI surfaces) while the glowing frame of the square icon
    is dropped completely.
    """
    icon = extract_client_icon(src)
    arr = np.asarray(icon).astype(np.float32)
    rgb, base_alpha = arr[..., :3], arr[..., 3] / 255.0
    h, w = base_alpha.shape
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx - (w - 1) / 2, yy - (h - 1) / 2) / w
    fade = np.clip((0.485 - r) / 0.04, 0, 1)
    fade = fade * fade * (3 - 2 * fade)  # smoothstep
    alpha = fade * base_alpha
    return _from_rgba(rgb, alpha)


def extract_host_logo(src: Image.Image) -> Image.Image:
    """Crescent disc logo with black background and glow converted to alpha."""
    a = _to_array(src)
    h, w, _ = a.shape
    mx = a.max(axis=2)

    # Fit the dark disc using the part of its edge that is not covered by the crescent.
    cx0, cy0 = w / 2, h / 2
    pts = []
    for deg in np.arange(-40, -1, 1.0):
        t = math.radians(deg)
        for r in range(int(w * 0.49), int(w * 0.25), -1):
            x = int(round(cx0 + r * math.cos(t)))
            y = int(round(cy0 + r * math.sin(t)))
            if mx[y, x] > 20:
                pts.append((x, y))
                break
    p = np.array(pts, dtype=np.float64)
    A = np.c_[2 * p[:, 0], 2 * p[:, 1], np.ones(len(p))]
    sol = np.linalg.lstsq(A, (p ** 2).sum(1), rcond=None)[0]
    xc, yc = sol[0], sol[1]
    radius = math.sqrt(sol[2] + xc * xc + yc * yc) - 1.5

    # Anti-aliased disc mask (4x supersampling)
    ss = 4
    big = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(big).ellipse(
        [(xc - radius) * ss, (yc - radius) * ss, (xc + radius) * ss, (yc + radius) * ss], fill=255
    )
    disc = np.asarray(big.resize((w, h), Image.LANCZOS)).astype(np.float32) / 255.0

    # Outside the disc: luminance keyed alpha (black -> transparent) so the glow survives.
    glow_alpha = np.clip(mx / 255.0, 0, 1)
    glow_alpha = np.clip((glow_alpha - 0.02) / 0.98, 0, 1)
    glow_rgb = np.where(glow_alpha[..., None] > 1e-3, a / np.maximum(glow_alpha[..., None], 1e-3), 0)
    glow_rgb = np.clip(glow_rgb, 0, 255)

    alpha = np.maximum(disc, glow_alpha)
    rgb = disc[..., None] * a + (1 - disc[..., None]) * glow_rgb
    # Inside the disc we keep the exact original colours.
    rgb = np.where(disc[..., None] >= 0.999, a, rgb)
    return _crop_to_content(_from_rgba(rgb, alpha), pad_ratio=0.01)


# --------------------------------------------------------------------------
# Derived artwork
# --------------------------------------------------------------------------

def badge(master: Image.Image, kind: str) -> Image.Image:
    """Overlay a small status badge (bottom right) used by the tray icon states."""
    size = master.width
    out = master.copy()
    d = ImageDraw.Draw(out)
    r = int(size * 0.20)
    cx, cy = size - r - int(size * 0.02), size - r - int(size * 0.02)
    colors = {
        "playing": ((34, 197, 94), (255, 255, 255)),
        "pausing": ((245, 158, 11), (255, 255, 255)),
        "locked": ((239, 68, 68), (255, 255, 255)),
    }
    fill, fg = colors[kind]
    ring = int(size * 0.035)
    d.ellipse([cx - r - ring, cy - r - ring, cx + r + ring, cy + r + ring], fill=(8, 6, 24, 255))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill + (255,))
    if kind == "playing":
        s = r * 0.55
        d.polygon([(cx - s * 0.55, cy - s), (cx - s * 0.55, cy + s), (cx + s, cy)], fill=fg)
    elif kind == "pausing":
        bw, bh, gap = r * 0.24, r * 0.95, r * 0.18
        d.rounded_rectangle([cx - gap - bw, cy - bh / 2, cx - gap, cy + bh / 2], radius=bw * 0.3, fill=fg)
        d.rounded_rectangle([cx + gap, cy - bh / 2, cx + gap + bw, cy + bh / 2], radius=bw * 0.3, fill=fg)
    elif kind == "locked":
        bw, bh = r * 1.0, r * 0.75
        top = cy - bh * 0.15
        d.rounded_rectangle([cx - bw / 2, top, cx + bw / 2, top + bh], radius=r * 0.12, fill=fg)
        sw = r * 0.62
        d.arc([cx - sw / 2, top - sw * 0.85, cx + sw / 2, top + sw * 0.25], start=180, end=360,
              fill=fg, width=max(2, int(r * 0.17)))
    return out


def adaptive_foreground(mark: Image.Image, px: int) -> Image.Image:
    """108dp adaptive icon foreground: artwork inside the 66dp safe zone."""
    canvas = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    content = int(round(px * 76 / 108))
    art = resize(mark, content, sharpen=False)
    off = (px - content) // 2
    canvas.alpha_composite(art, (off, off))
    return canvas


def monochrome(mark: Image.Image) -> Image.Image:
    arr = np.asarray(mark).astype(np.float32)
    lum = arr[..., :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    alpha = np.clip((lum - 90) / 120, 0, 1) * (arr[..., 3] / 255.0)
    white = np.full(arr[..., :3].shape, 255, dtype=np.float32)
    return _from_rgba(white, alpha)


def banner(mark: Image.Image, size=(320, 180), subtitle: str | None = None) -> Image.Image:
    w, h = size
    ss = 3
    W, H = w * ss, h * ss
    bg = Image.new("RGB", (W, H), NIGHT_DEEP)
    arr = np.asarray(bg).astype(np.float32)
    yy, xx = np.mgrid[0:H, 0:W]
    g = np.exp(-(((xx - W * 0.28) / (W * 0.45)) ** 2 + ((yy - H * 0.5) / (H * 0.75)) ** 2))
    arr = arr + g[..., None] * np.array([38, 26, 92], dtype=np.float32)
    bg = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    art = resize(mark, int(H * 0.72), sharpen=False)
    margin = int(min(W, H) * 0.08)
    bg.alpha_composite(art, (margin, (H - art.height) // 2))
    d = ImageDraw.Draw(bg)
    tx = margin + art.width + int(margin * 0.6)
    avail = W - tx - margin
    # Fit the word mark into the remaining space
    size = int(H * 0.30)
    while size > 8:
        title_font = font(size, 650)
        if d.textlength("NyxPlay", font=title_font) <= avail:
            break
        size -= 2
    d.text((tx, H * 0.5), "NyxPlay", font=title_font, fill=(244, 240, 255), anchor="lm" if not subtitle else "ls")
    if subtitle:
        sub_size = max(8, int(size * 0.36))
        sub_font = font(sub_size, 500)
        while sub_size > 8 and d.textlength(subtitle, font=sub_font) > avail:
            sub_size -= 1
            sub_font = font(sub_size, 500)
        d.text((tx + ss, H * 0.5 + sub_size * 1.5), subtitle, font=sub_font, fill=LAVENDER, anchor="ls")
    return bg.resize((w, h), Image.LANCZOS)


def installer_logo(mark: Image.Image, size: int) -> Image.Image:
    return resize(mark, size, sharpen=False)


# --------------------------------------------------------------------------
# Exporters
# --------------------------------------------------------------------------

def export_android(icon: Image.Image, mark: Image.Image) -> None:
    print("Android client")
    res = ROOT / "android" / "app" / "src" / "main" / "res"
    densities = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
    mono = monochrome(mark)
    for name, scale in densities.items():
        folder = res / f"mipmap-{name}"
        legacy = int(round(48 * scale))
        fg = int(round(108 * scale))
        save_png(resize(icon, legacy), folder / "ic_launcher.png")
        save_png(adaptive_foreground(mark, fg), folder / "ic_launcher_foreground.png")
        save_png(adaptive_foreground(mono, fg), folder / "ic_launcher_monochrome.png")
    save_png(resize(icon, 512, sharpen=False), ROOT / "android" / "app" / "src" / "main" / "ic_launcher-web.png")
    save_png(banner(mark).convert("RGB"), res / "drawable-xhdpi" / "atv_banner.png")
    save_png(resize(mark, 512, sharpen=False), res / "drawable-nodpi" / "nyxplay_logo.png")
    # Store / fastlane assets
    store = ROOT / "android" / "fastlane" / "metadata" / "android" / "en-US" / "images"
    if store.exists():
        save_png(resize(icon, 512, sharpen=False), store / "icon.png")
        save_png(banner(mark, (1024, 500), "Game streaming for Android").convert("RGB"), store / "featureGraphic.png")
    background_xml = res / "values" / "ic_launcher_background.xml"
    background_xml.write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
        '  <color name="ic_launcher_background">#0B0C21</color>\n</resources>\n',
        encoding="utf-8", newline="\n")
    print(f"  wrote {background_xml.relative_to(ROOT)}")


def export_pc(icon: Image.Image, mark: Image.Image) -> None:
    print("PC client")
    app = ROOT / "pc" / "app"
    save_ico(icon, app / "nyxplay.ico")
    save_icns(icon, app / "nyxplay.icns")
    save_svg_wrapper(icon, app / "res" / "nyxplay.svg", 512, "NyxPlay")
    save_png(resize(icon, 256, sharpen=False), app / "res" / "nyxplay.png")
    save_png(resize(mark, 256, sharpen=False), app / "res" / "nyxplay-mark.png")
    save_png(installer_logo(icon, 64), app / "nyxplay_wix.png")


def export_host(logo: Image.Image) -> None:
    print("NyxPlay Host")
    host = ROOT / "host"
    images = host / "src_assets" / "common" / "assets" / "web" / "public" / "images"
    save_ico(logo, host / "nyxplay-host.ico")
    save_icns(logo, host / "nyxplay-host.icns")
    save_png(resize(logo, 512, sharpen=False), host / "nyxplay-host.png")
    save_svg_wrapper(logo, host / "nyxplay-host.svg", 256, "NyxPlay Host")

    tray_sizes = (16, 20, 24, 32, 40, 48, 64, 256)
    save_ico(logo, images / "nyxplay-host.ico", tray_sizes)
    save_png(resize(logo, 16), images / "logo-nyxplay-host-16.png")
    save_png(resize(logo, 45), images / "logo-nyxplay-host-45.png")
    save_png(resize(logo, 256, sharpen=False), images / "logo-nyxplay-host.png")
    save_svg_wrapper(logo, images / "logo-nyxplay-host.svg", 256, "NyxPlay Host")
    for kind in ("playing", "pausing", "locked"):
        state = badge(logo, kind)
        save_ico(state, images / f"nyxplay-host-{kind}.ico", tray_sizes)
        save_png(resize(state, 16), images / f"nyxplay-host-{kind}-16.png")
        save_png(resize(state, 45), images / f"nyxplay-host-{kind}-45.png")
        save_png(resize(state, 512, sharpen=False), images / f"nyxplay-host-{kind}.png")
        save_svg_wrapper(state, images / f"nyxplay-host-{kind}.svg", 256, f"NyxPlay Host ({kind})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--masters", action="store_true", help="only render branding/logo masters")
    args = parser.parse_args()

    client_src = Image.open(SOURCE / "nyxplay-app-source.png")
    host_src = Image.open(SOURCE / "nyxplay-host-source.png")

    print("Masters")
    icon = resize(extract_client_icon(client_src), 1024, sharpen=False)
    mark = resize(extract_client_mark(client_src), 1024, sharpen=False)
    host_logo = resize(extract_host_logo(host_src), 1024, sharpen=False)
    save_png(icon, LOGO / "nyxplay-app-icon.png")
    save_png(mark, LOGO / "nyxplay-app-mark.png")
    save_png(host_logo, LOGO / "nyxplay-host-logo.png")
    for kind in ("playing", "pausing", "locked"):
        save_png(badge(host_logo, kind), LOGO / f"nyxplay-host-{kind}.png")
    save_png(banner(mark, (1280, 640), "Low latency game streaming").convert("RGB"), LOGO / "nyxplay-social.png")

    if args.masters:
        return
    export_android(icon, mark)
    export_pc(icon, mark)
    export_host(host_logo)


if __name__ == "__main__":
    main()
