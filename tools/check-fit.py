# Every translation measured where it's drawn, at the 4:3 layout (the narrowest), with the app's own
# fonts (tools/host/measure.c; CJK as one em a character, as the console's system fonts draw them):
# notes, Free up space's reasons, the menu's descriptions, Settings' help, the lists' header lines.
# Prints what doesn't fit; exits 1 if anything doesn't. Run by tools/host-test.sh.
#   python3 tools/check-fit.py
import re, subprocess, sys, os, glob
sys.path.insert(0, 'tools')
from strings import phrases
P = subprocess.Popen(['build/host/measure'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=dict(os.environ, HOST_DATA=os.getcwd() + '/data'))
cache = {}
def W(font, px, s):
    # CJK: one em a character (the console's system fonts); the rest measured with the app's own
    cjk = sum(1 for ch in s if 0x2E80 <= ord(ch) <= 0x9FFF or 0xAC00 <= ord(ch) <= 0xD7AF or 0xFF00 <= ord(ch) <= 0xFFEF or 0x3000 <= ord(ch) <= 0x303F)
    rest = ''.join(ch for ch in s if not (0x2E80 <= ord(ch) <= 0x9FFF or 0xAC00 <= ord(ch) <= 0xD7AF or 0xFF00 <= ord(ch) <= 0xFFEF or 0x3000 <= ord(ch) <= 0x303F))
    k = (font, px, rest)
    if k not in cache:
        P.stdin.write(f"{font} {px} {rest}\n"); P.stdin.flush()
        line = P.stdout.readline()
        while line.startswith('['): line = P.stdout.readline()
        cache[k] = int(line)
    return cache[k] + cjk * px
LANGS = 'es pt fr de it nl pl tr da id ar ru ja ko zh'.split()
T = {}
for c in LANGS:
    T[c] = {}
    for l in open(f'data/lang/{c}.txt', encoding='utf-8'):
        if l.startswith('#') or '\t' not in l: continue
        a, b = l.rstrip('\n').split('\t', 1)
        T[c][a.replace('\\n', '\n')] = b.replace('\\n', '\n')
def tr(c, s): return T[c].get(s, s) if c != 'en' else s
SAMPLE = {'Report saved: %s (and .csv)': ['/dev_usb000/HDDAtlas/HDDAtlas_usb000_20261110-0908'],
          '%s went from %lld to %lld since %s.': ['Reallocated sectors', '12', '40', '2026-11-10']}
def fill(en, s):
    vals = list(SAMPLE.get(en, []))
    def sub(m):
        if vals: return vals.pop(0)
        t = m.group(0)[-1]
        return {'s': 'Internal HDD', 'd': '123', 'u': '123', 'f': '246.0'}.get(t, '123')
    return re.sub(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z)?[a-zA-Z])', sub, s)
src = ''.join(open(f, encoding='utf-8').read() for f in glob.glob('src/*.c'))
# notes: the phrases inside ui_note(...) and the ones built into a buffer that's then a note
notes = set(re.findall(r'ui_note\((?:[^;]*?)tr\("((?:[^"\\]|\\.)*)"\)', src))
for m in re.finditer(r'ui_note\(kit_btn\(N_\("((?:[^"\\]|\\.)*)"\),\s*N_\("((?:[^"\\]|\\.)*)"\)', src): notes |= {m.group(1), m.group(2)}
notes |= {"Emptied %d old entries from the recycle bin: %s freed.", "A scan of %s is running: one at a time.", "The scan couldn't start (see the log).",
          "Rescanning %s; the rest stays as it was.", "Scan of %s finished in %.1f s.", "Report saved: %s (and .csv)",
          "The report couldn't be written (see the log).", "Once the scan's done.", "Scan this drive first.", "%s went from %lld to %lld since %s.",
          "Installed games: delete them from the XMB (it keeps their icons).", "Music, video and photos: delete them from the XMB (it keeps a list of them).",
          "Saves, trophies and licences: delete them from the XMB.", "The system's own: never deleted.",
          "webMAN needs this to mount a game from an NTFS drive.", "A disc image's decryption key: webMAN needs it to mount it."}
notes = {n.encode().decode('unicode_escape').encode('latin-1').decode('utf-8') if '\\x' in n else n for n in notes}
bad = []
fails = 0
for en in sorted(notes):
    for c in ['en'] + LANGS:
        s = fill(en, tr(c, en))
        w = W(1, 19, s)
        nb = len(s.encode())
        if w > 800 or nb > 199: bad.append((c, w, nb, s))
print(f"notes: {len(notes)} phrases x 16 languages; too wide (>800 px at 4:3) or too long (>199 bytes):")
for c, w, nb, s in bad: print(f"  {c} {w}px {nb}B  {s}"); fails += 1
# Free up space's reasons: the row's second line at 4:3
def block(path, start):
    s = open(path, encoding='utf-8').read()
    i = s.index(start); j = s.index('};', i)
    return [m.encode().decode('unicode_escape').encode('latin-1').decode('utf-8') if '\\x' in m else m for m in re.findall(r'N_\("((?:[^"\\]|\\.)*)"\)', s[i:j])]

whys = block('src/ui_list.c', 'static const char *const whys[NUM_WHYS] = {')		# (the reasons, as the code has them)
print("Free up space reasons, cut at 4:3:")
for c in ['en'] + LANGS:
    rw = max(W(1, 22, '999.99 GB'), max(W(0, 17, tr(c, x)) for x in ("Can be deleted", "From the XMB", "Kept")))
    maxw = 888 - rw - 24 - 178
    for en in whys:
        s = tr(c, en)
        w = W(0, 17, s)
        if w > maxw: print(f"  {c} {w}>{maxw}px  {s}"); fails += 1

def is_cjk(ch): o = ord(ch); return 0x2E80 <= o <= 0x9FFF or 0xAC00 <= o <= 0xD7AF or 0xFF00 <= o <= 0xFFEF or 0x3000 <= o <= 0x303F
def lines_needed(font, px, s, width):
    # kit_wrap's way, near enough: words (CJK: characters) onto lines while they fit
    toks = re.findall(r'[⺀-鿿가-힯＀-￯　-〿]|[^\s⺀-鿿가-힯＀-￯　-〿]+\s*|\s+', s)
    n, cur = 1, ''
    for t in toks:
        if W(font, px, (cur + t).rstrip()) <= width or not cur: cur += t
        else: n += 1; cur = t.lstrip()
    return n
SW, LW = 960, 840
print("Menu descriptions (2 lines at 18 px) and Settings help (2 lines at 17 px), at 4:3:")
descs = block('src/ui_menu.c', 'static const char *const descs[NUM_ENTRIES] = {') + ["After the next scan of this drive.", "The internal HDD only.", "Scan this drive first.",
        "The drive as boxes sized by the space they take: Circle goes into one."]
helps = block('src/ui_menu.c', 'static const char *const helps[NUM_SETTINGS] = {')
for c in ['en'] + LANGS:
    for en in descs:
        k = lines_needed(0, 18, tr(c, en), LW)
        if k > 2: print(f"  desc {c} {k} lines: {tr(c, en)}"); fails += 1
    for en in helps:
        k = lines_needed(0, 17, tr(c, en), LW)
        if k > 2: print(f"  help {c} {k} lines: {tr(c, en)}"); fails += 1
print("List header lines (shrunk to 14 px, then cut), at 4:3:")
says = block('src/ui_list.c', 'static const char *const says[] = {N_("What') + block('src/ui_list.c', 'static const char *const says_o[] = {') + \
       block('src/ui_list.c', 'static const char *const says[] = {N_("Each')
for c in ['en'] + LANGS:
    r2w = max(W(0, 17, tr(c, x)) for x in ("Largest first", "Smallest first", "By name", "Newest first", "Oldest first"))
    maxw = (SW - 60 - r2w - 24) - 60
    for en in says:
        w = W(0, 14, tr(c, en))
        if w > maxw: print(f"  {c} {w}>{maxw}px at 14 px: {tr(c, en)}"); fails += 1

print("fit: all PASS" if not fails else f"fit: {fails} don't fit")
sys.exit(1 if fails else 0)
