# The button glyphs (Kenney, CC0) have their art right to the image's edge, at full opacity: drawn
# scaled, the circles' sides come out sliced flat. This gives each a 2-pixel transparent margin
# (60x60 -> 64x64; the margin takes its edge pixels' colour at alpha 0, so scaling pulls in no dark
# fringe); kit_glyph draws them 2 units bigger, so the art stays the size it was. Run once:
#   python3 tools/pad-glyphs.py      (a glyph already 64x64 is left as it is)
import glob, os
from PIL import Image
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
for path in sorted(glob.glob(os.path.join(root, 'data/glyphs/*.png'))):
    im = Image.open(path).convert('RGBA')
    if im.size != (60, 60):
        continue
    P = 2
    out = Image.new('RGBA', (60 + 2 * P, 60 + 2 * P))
    src = im.load(); dst = out.load()
    for y in range(64):
        for x in range(64):
            sx, sy = min(max(x - P, 0), 59), min(max(y - P, 0), 59)
            r, g, b, a = src[sx, sy]
            dst[x, y] = (r, g, b, a if (x - P, y - P) == (sx, sy) else 0)
    out.save(path)
    print('padded', os.path.basename(path))
