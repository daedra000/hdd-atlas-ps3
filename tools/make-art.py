# The art, from Rick Patrick's hard disk (art/icon-hdd.png). (draw_svg drew the first icon,
# art/icon_only.svg, before it; kept for that.)
#   art/ICON0.PNG    320x176, Rick Patrick's hard disk (art/icon-hdd.png) in the middle, transparent
#                    round it (the XMB's icon)
#   data/logo.png    the icon and "HDD Atlas", for the top of the app's screens (180x31 there,
#                    made 3x for 1080p)
# art/PIC1.PNG (the XMB's background, 1920x1080) is used as it is.
#   python3 tools/make-art.py
import os, re, sys
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw, ImageFont

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SS = 8		# drawn this much bigger, then made smaller (smooth edges)

def colour(v):
	if not v or v == 'none': return None
	v = v.lstrip('#')
	if len(v) == 3: v = ''.join(c * 2 for c in v)
	return tuple(int(v[i:i + 2], 16) for i in (0, 2, 4)) + (255,)

def draw_svg(path, px):
	tree = ET.parse(path)
	svg = tree.getroot()
	vb = [float(x) for x in re.split(r'[ ,]+', svg.get('viewBox').strip())]
	k = px * SS / vb[2]
	img = Image.new('RGBA', (px * SS, round(vb[3] * k)), (0, 0, 0, 0))
	d = ImageDraw.Draw(img)
	def P(x, y): return ((x - vb[0]) * k, (y - vb[1]) * k)
	def walk(el, inh):
		a = dict(inh)
		for n in ('fill', 'stroke', 'stroke-width', 'stroke-linecap'):
			if el.get(n) is not None: a[n] = el.get(n)
		tag = el.tag.split('}')[-1]
		fill, stroke = colour(a.get('fill', '#000')), colour(a.get('stroke'))
		w = float(a.get('stroke-width', 1)) * k
		f = lambda n: float(el.get(n))
		if tag in ('circle', 'ellipse'):
			rx = f('r') if tag == 'circle' else f('rx')
			ry = f('r') if tag == 'circle' else f('ry')
			(x0, y0), (x1, y1) = P(f('cx') - rx, f('cy') - ry), P(f('cx') + rx, f('cy') + ry)
			if fill: d.ellipse([x0, y0, x1, y1], fill=fill)
			if stroke:		# (an SVG stroke is centred on the edge; PIL's is inside the box)
				h = w / 2
				d.ellipse([x0 - h, y0 - h, x1 + h, y1 + h], outline=stroke, width=round(w))
		elif tag == 'line' and stroke:
			p0, p1 = P(f('x1'), f('y1')), P(f('x2'), f('y2'))
			d.line([p0, p1], fill=stroke, width=round(w))
			if a.get('stroke-linecap') == 'round':
				for x, y in (p0, p1): d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=stroke)
		elif tag not in ('svg', 'g'):
			sys.exit('make-art: %s in the SVG isn\'t drawn here (circles, ellipses and lines only)' % tag)
		for c in el: walk(c, a)
	walk(svg, {})
	img = img.crop(img.getbbox())		# (just what's drawn)
	return img

def smaller(img, w, h):
	return img.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')

# ICON0: Rick Patrick's hard disk (art/icon-hdd.png, "Internal Clear", from softicons.com), trimmed to
# what's drawn, 168 high in the middle
hdd = Image.open(os.path.join(root, 'art/icon-hdd.png')).convert('RGBA')
hdd = hdd.crop(hdd.getchannel('A').getbbox())
s = 168 / hdd.height
icon0 = Image.new('RGBA', (320, 176), (0, 0, 0, 0))
small = smaller(hdd, round(hdd.width * s), 168)
icon0.alpha_composite(small, ((320 - small.width) // 2, 4))
icon0.save(os.path.join(root, 'art/ICON0.PNG'))

# the logo: 180x31 on the screen, made at 3x; the hard disk, then HDD (white) Atlas (teal)
X = 3
W, H = 180 * X, 31 * X
logo = Image.new('RGBA', (W, H), (0, 0, 0, 0))
small = smaller(hdd, round(hdd.width * H / hdd.height), H)
logo.alpha_composite(small, (0, 0))
font = ImageFont.truetype(os.path.join(root, 'data/fonts/Poppins-Bold.ttf'), 26 * X)
d = ImageDraw.Draw(logo)
x = small.width + 10 * X
top = (H - (font.getbbox('HDD')[3] - font.getbbox('HDD')[1])) // 2 - font.getbbox('HDD')[1]
d.text((x, top), 'HDD', font=font, fill=(255, 255, 255, 255))
x += font.getlength('HDD ')
d.text((x, top), 'Atlas', font=font, fill=(0x2e, 0xe6, 0xd6, 255))
end = x + font.getlength('Atlas')
if end > W: sys.exit('make-art: the logo is %d wide, more than %d' % (end, W))
logo.save(os.path.join(root, 'data/logo.png'))

pic = Image.open(os.path.join(root, 'art/PIC1.PNG'))
if pic.size != (1920, 1080): sys.exit('make-art: art/PIC1.PNG is %dx%d, not 1920x1080' % pic.size)
print('art: art/ICON0.PNG (320x176), data/logo.png (%dx%d, its text to x=%d), art/PIC1.PNG checked' % (W, H, end))
