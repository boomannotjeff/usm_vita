"""Builds the VPK's LiveArea images (vita/sce_sys) from the art in vita/art.
   banner.png -> pic0 (960x544) and startup (280x158); hero.png -> bg (840x500); icon.png -> icon0 (128x128).
   Without icon.png / hero.png the banner is cropped instead. LiveArea PNGs must be 8-bit indexed.
   py tools/make_livearea.py"""
import os
from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vita")
art, out = os.path.join(root, "art"), os.path.join(root, "sce_sys")


def load(name):
    p = os.path.join(art, name)
    return Image.open(p).convert("RGB") if os.path.exists(p) else None


def fill(img, w, h, cx=0.5, cy=0.5):
    """scale to cover w x h, then crop around (cx, cy) in 0..1"""
    s = max(w / img.width, h / img.height)
    img = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x = min(max(round(img.width * cx - w / 2), 0), img.width - w)
    y = min(max(round(img.height * cy - h / 2), 0), img.height - h)
    return img.crop((x, y, x + w, y + h))


def save(img, rel):
    """8-bit indexed; an RGBA image keeps its see-through pixels (palette alpha)"""
    p = os.path.join(out, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if img.mode == "RGBA":  # fast octree keeps alpha in the palette (tRNS)
        img.quantize(256, method=Image.Quantize.FASTOCTREE).save(p, optimize=True)
    else:
        img.quantize(256, method=Image.Quantize.MEDIANCUT).save(p, optimize=True)
    print(rel, img.size)


banner = load("banner.png")
icon = load("icon.png") or banner.crop((370, 0, 600, 230))  # Spider-Man's head
hero = load("hero.png") or banner
save(fill(banner, 960, 544), "pic0.png")
save(fill(banner, 280, 158), "livearea/contents/startup.png")
z = round(min(icon.size) * 0.8)  # zoomed in: the middle 80% of the icon art
save(fill(icon.crop(((icon.width - z) // 2, (icon.height - z) // 2, (icon.width + z) // 2, (icon.height + z) // 2)),
          128, 128), "icon0.png")
save(fill(hero, 840, 500), "livearea/contents/bg.png")


def settings_button(w=280, h=64):
    """the Settings button under the gate (template.xml frame3): just the button, no empty space below it"""
    from PIL import ImageDraw, ImageFont
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))  # see-through outside the rounded button
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((2, 2, w - 3, h - 3), radius=14, fill=(24, 30, 72, 255), outline=(200, 30, 40, 255), width=4)
    font = ImageFont.load_default(size=30)  # Aileron (public domain), as the menu itself
    tw = font.getlength("SETTINGS")
    asc, desc = font.getmetrics()
    d.text(((w - tw) / 2, (h - asc - desc) / 2), "SETTINGS", font=font, fill=(255, 255, 255, 255))
    return img


save(settings_button(), "livearea/contents/settings.png")
assert all(Image.open(os.path.join(out, r)).mode == "P" for r in
           ("pic0.png", "icon0.png", "livearea/contents/bg.png", "livearea/contents/startup.png",
            "livearea/contents/settings.png"))
