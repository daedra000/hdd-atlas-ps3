# The START menu's icons: data/menu/<name>.png, white on transparent, 96x96 (shown at 32,
# 48 at 1080p). They're Tanah Basah's, from Flaticon (art/menu-icons, black on transparent,
# made white here; the map is their rectangle cut into boxes). The ones drawn below with PIL
# (a 24-unit grid, much bigger, then made smaller) are the stand-ins they replace, used only
# if an art/menu-icons file goes missing.
#   python3 tools/make-menu-icons.py
import math, os
from PIL import Image, ImageDraw

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT = os.path.join(root, 'data', 'menu')
PX, SS = 96, 8
K = PX * SS / 24.0
W = 2.0 * K			# the stroke
WHITE, CLEAR = (255, 255, 255, 255), (0, 0, 0, 0)

def P(x, y): return (x * K, y * K)

class Icon:
	def __init__(self):
		self.img = Image.new('RGBA', (PX * SS, PX * SS), CLEAR)
		self.d = ImageDraw.Draw(self.img)
	def dot(self, x, y, r, c=WHITE):
		self.d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=c)
	def line(self, pts, closed=False, w=W):
		pts = [P(x, y) for x, y in pts]
		if closed: pts = pts + [pts[0]]
		self.d.line(pts, fill=WHITE, width=round(w), joint='curve')
		for x, y in pts: self.d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=WHITE)
	def poly(self, pts, c=WHITE):
		self.d.polygon([P(x, y) for x, y in pts], fill=c)
	def rrect(self, x0, y0, x1, y1, r, fill=None, stroke=True):
		box = [P(x0, y0), P(x1, y1)]
		if fill: self.d.rounded_rectangle(box, r * K, fill=fill)
		if stroke: self.d.rounded_rectangle(box, r * K, outline=WHITE, width=round(W))
	def circle(self, x, y, r, w=W):
		self.d.ellipse([P(x - r, y - r), P(x + r, y + r)], outline=WHITE, width=round(w))
	def save(self, name):
		os.makedirs(OUT, exist_ok=True)
		self.img.resize((PX, PX), Image.LANCZOS).save(os.path.join(OUT, name + '.png'), optimize=True)

def free():
	i = Icon()
	i.line([(4.5, 7), (19.5, 7)])
	i.line([(9.5, 7), (9.5, 4), (14.5, 4), (14.5, 7)])
	i.line([(6.5, 7), (7.5, 20.5), (16.5, 20.5), (17.5, 7)])
	i.line([(10.2, 10.5), (10.2, 17)])
	i.line([(13.8, 10.5), (13.8, 17)])
	i.save('free')

def games():
	i = Icon()
	i.rrect(2.5, 7.5, 21.5, 17.5, 5)
	i.line([(5.8, 12.5), (10.2, 12.5)])
	i.line([(8, 10.3), (8, 14.7)])
	i.dot(15.6, 11.3, 1.3)
	i.dot(18, 13.7, 1.3)
	i.save('games')

def dupes():
	i = Icon()
	i.rrect(8, 3, 21, 16, 2.5)
	i.rrect(3, 8, 16, 21, 2.5, fill=CLEAR, stroke=False)
	i.d.rounded_rectangle([P(3 - 1, 8 - 1), P(16 + 1, 21 + 1)], 3.5 * K, fill=CLEAR)
	i.rrect(3, 8, 16, 21, 2.5)
	i.save('dupes')

def old():
	i = Icon()
	i.line([(5.5, 3), (18.5, 3)])
	i.line([(5.5, 21), (18.5, 21)])
	i.line([(7.5, 3), (7.5, 6.5), (12, 12), (7.5, 17.5), (7.5, 21)])
	i.line([(16.5, 3), (16.5, 6.5), (12, 12), (16.5, 17.5), (16.5, 21)])
	i.poly([(9, 20), (15, 20), (12, 16)])
	i.poly([(10, 7), (14, 7), (12, 9.5)])
	i.save('old')

def folders():
	i = Icon()
	i.line([(3, 19.5), (3, 5), (9, 5), (11, 7.5), (21, 7.5), (21, 19.5)], closed=True)
	i.line([(12, 17), (12, 10.5)])
	i.line([(9.3, 13.2), (12, 10.5), (14.7, 13.2)])
	i.save('folders')

def types():
	i = Icon()
	i.line([(5.5, 2.5), (14, 2.5), (19, 7.5), (19, 21.5), (5.5, 21.5)], closed=True)
	i.line([(14, 2.5), (14, 7.5), (19, 7.5)])
	i.line([(9, 12), (15.5, 12)])
	i.line([(9, 15.2), (15.5, 15.2)])
	i.line([(9, 18.4), (12.5, 18.4)])
	i.save('types')

def map_():
	i = Icon()
	i.rrect(3, 3.5, 21, 20.5, 2.5)		# (its stroke is inside the box: the lines end on its middle)
	i.line([(12, 4.5), (12, 19.5)])
	i.line([(12, 11), (20, 11)])
	i.line([(4, 14), (12, 14)])
	i.line([(16.5, 11), (16.5, 19.5)])
	i.save('map')

def marks():
	i = Icon()
	i.line([(6.5, 3), (17.5, 3), (17.5, 21), (12, 16.5), (6.5, 21)], closed=True)
	i.save('marks')

def changes():
	i = Icon()
	i.line([(8, 20), (8, 4.5)])
	i.line([(4.3, 8.2), (8, 4.5), (11.7, 8.2)])
	i.line([(16, 4), (16, 19.5)])
	i.line([(12.3, 15.8), (16, 19.5), (19.7, 15.8)])
	i.save('changes')

def history():
	i = Icon()
	i.line([(3, 20.5), (21, 20.5)])
	for x0, y0 in ((4.5, 13), (10.5, 6.5), (16.5, 10)):
		i.d.rounded_rectangle([P(x0, y0), P(x0 + 3.2, 18.3)], 0.9 * K, fill=WHITE)
	i.save('history')

def export():
	i = Icon()
	i.line([(4, 13.5), (4, 20.5), (20, 20.5), (20, 13.5)])
	i.line([(12, 15.5), (12, 3.5)])
	i.line([(7.8, 7.7), (12, 3.5), (16.2, 7.7)])
	i.save('export')

def health():
	i = Icon()
	pts = []
	for k in range(120):
		t = 2 * math.pi * k / 120
		x = 16 * math.sin(t) ** 3
		y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
		pts.append((12 + x * 0.56, 11.2 - y * 0.56))
	i.line(pts, closed=True)
	i.line([(6.3, 12), (9, 12), (10.4, 9.2), (12.6, 15), (14.1, 10.8), (15.2, 12), (17.7, 12)], w=1.7 * K)
	i.save('health')

def settings():
	i = Icon()
	teeth, ro, ri = 8, 10.2, 7.6
	pts = []
	for k in range(teeth):
		a = 2 * math.pi * k / teeth
		for da, r in ((-0.24, ri), (-0.15, ro), (0.15, ro), (0.24, ri)):
			pts.append((12 + r * math.cos(a + da), 12 + r * math.sin(a + da)))
		b = a + math.pi / teeth
		pts.append((12 + ri * math.cos(b), 12 + ri * math.sin(b)))
	i.poly(pts)
	i.dot(12, 12, 6.2)
	i.dot(12, 12, 3.1, CLEAR)
	i.save('settings')

def buttons():
	i = Icon()
	for x, y in ((12, 5), (19, 12), (12, 19), (5, 12)):		# (solid, like the Flaticon set)
		i.dot(x, y, 3.6)
	i.save('buttons')

def about():
	i = Icon()
	i.circle(12, 12, 9.5)
	i.dot(12, 7.6, 1.4)
	i.line([(12, 11), (12, 16.8)])
	i.save('about')

# the start screen's: Scan (a magnifier), Last scan (a ring chart), Tools (four boxes); solid, as the rest
def scan():
	i = Icon()
	i.circle(10, 10, 6.6, w=3.0 * K)
	i.line([(15, 15), (20.5, 20.5)], w=3.4 * K)
	i.save('scan')

def last():
	i = Icon()
	i.dot(12, 12, 10)
	i.dot(12, 12, 4.6, CLEAR)
	i.d.pieslice([P(0, 0), P(24, 24)], -90, -18, fill=CLEAR)		# (a slice out, a hair apart)
	i.d.pieslice([P(1.4, 1.4), P(22.6, 22.6)], -84, -24, fill=WHITE)
	i.dot(12, 12, 4.6, CLEAR)
	i.save('last')

def tools():
	i = Icon()
	for x, y in ((3, 3), (13, 3), (3, 13), (13, 13)):
		i.d.rounded_rectangle([P(x, y), P(x + 8, y + 8)], 2.2 * K, fill=WHITE)
	i.save('tools')

# 0.9's: Identical files (a page with "="), Size filter (a funnel), Recycle bin (a bin with an arrow back)
def same():
	i = Icon()
	i.poly([(5, 2), (14.5, 2), (19.5, 7), (19.5, 22), (5, 22)])
	i.poly([(14.5, 2), (14.5, 7), (19.5, 7)], CLEAR)
	i.d.rounded_rectangle([P(8, 10.5), P(16.5, 12.8)], 1.1 * K, fill=CLEAR)
	i.d.rounded_rectangle([P(8, 15.2), P(16.5, 17.5)], 1.1 * K, fill=CLEAR)
	i.save('same')

def filter_():
	i = Icon()
	i.poly([(3, 3.5), (21, 3.5), (14, 12.5), (14, 20.5), (10, 18.5), (10, 12.5)])
	i.save('filter')

def bin_():
	i = Icon()
	i.d.rounded_rectangle([P(3.5, 5), P(20.5, 8)], 1.2 * K, fill=WHITE)
	i.d.rounded_rectangle([P(9.5, 2.5), P(14.5, 5.5)], 1 * K, fill=WHITE)
	i.poly([(5.5, 9.3), (18.5, 9.3), (17.4, 22), (6.6, 22)])
	# an arrow coming back round, cut out of it
	i.d.arc([P(8.4, 11.6), P(15.6, 18.8)], 200, 470, fill=CLEAR, width=round(1.9 * K))
	i.poly([(7.4, 12.0), (11.2, 11.4), (8.9, 15.0)], CLEAR)
	i.save('bin')

# 0.9.1's: Recently added (a clock with a plus: new lately)
def recent():
	i = Icon()
	i.circle(10.5, 12.5, 8.2)
	i.line([(10.5, 8), (10.5, 12.5), (13.8, 14.6)])
	i.dot(19, 5.5, 4.6, CLEAR)		# (a gap round the plus)
	i.d.rounded_rectangle([P(15.6, 4.6), P(22.4, 6.4)], 0.9 * K, fill=WHITE)
	i.d.rounded_rectangle([P(18.1, 2.1), P(19.9, 8.9)], 0.9 * K, fill=WHITE)
	i.save('recent')

for f in (free, games, dupes, old, folders, types, map_, marks, changes, history, export, health, settings, buttons, about, scan, last, tools,
		same, filter_, bin_, recent):
	f()

# Tanah Basah's (Flaticon), over the drawn ones
ART = os.path.join(root, 'art', 'menu-icons')
FLATICON = {'folders': '001-upload', 'free': '002-trash-bin', 'old': '003-hourglass', 'games': '004-joystick',
	'dupes': '005-paper', 'types': '006-list', 'map': '007-rectangle', 'marks': '008-bookmark',
	'changes': '009-time-management', 'history': '010-history', 'export': '012-share',
	'health': '013-first-aid', 'settings': '014-setting', 'about': '015-information',
	'buttons': '001-controller', 'scan': '002-searching', 'last': '003-pie-chart', 'tools': '016-app',
	'same': '005-paper', 'filter': '017-funnel', 'recent': '018-plus', 'bin': '020-recycle-bin'}

def white(src):
	a = Image.open(src).convert('RGBA').getchannel('A')
	img = Image.new('RGBA', a.size, WHITE)
	img.putalpha(a)
	return img

# Identical files: Duplicates' pages (the same icon: Tanah Basah's set has it once), an "=" cut
# into the front page so the two tiles side by side aren't alike
def equals(img):
	a = img.getchannel('A')
	d = ImageDraw.Draw(a)
	d.rounded_rectangle([222, 196, 374, 228], 13, fill=0)
	d.rounded_rectangle([222, 262, 374, 294], 13, fill=0)
	img.putalpha(a)
	return img

def boxes(img):
	# the rectangle as a map: boxes, with clear gaps between them
	a = img.getchannel('A')
	x0, y0, x1, y1 = a.getbbox()
	w, h, g = x1 - x0, y1 - y0, max(6, (x1 - x0) // 22)
	d = ImageDraw.Draw(a)
	mx = x0 + int(w * 0.52)
	d.rectangle([mx - g // 2, y0, mx + g // 2, y1], fill=0)						# left | right
	d.rectangle([x0, y0 + int(h * 0.62) - g // 2, mx, y0 + int(h * 0.62) + g // 2], fill=0)	# left: top / bottom
	d.rectangle([mx, y0 + int(h * 0.45) - g // 2, x1, y0 + int(h * 0.45) + g // 2], fill=0)	# right: top / bottom
	rx = mx + int((x1 - mx) * 0.55)
	d.rectangle([rx - g // 2, y0 + int(h * 0.45), rx + g // 2, y1], fill=0)			# right bottom: two
	img.putalpha(a)
	return img

for name, src in FLATICON.items():
	path = os.path.join(ART, src + '.png')
	if not os.path.exists(path): continue
	img = white(path)
	if name == 'map': img = boxes(img)
	if name == 'same': img = equals(img)
	img.resize((PX, PX), Image.LANCZOS).save(os.path.join(OUT, name + '.png'), optimize=True)
print('menu icons: ' + OUT)
