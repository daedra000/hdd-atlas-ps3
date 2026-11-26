# data/lang/<code>.txt from a list of translations, one per line in the order tools/strings.py gives
# the English (tools/lang/<code>.list; "\n" for a new line). Checks the count, and that each line keeps
# the English's % conversions in order (i18n.c leaves out one that doesn't). Once made, the .txt is
# the one to edit: it pairs each English phrase with its translation.
#   python3 tools/make-lang.py es [fr ...]
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strings import phrases, esc
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
NAMES = {'es': 'Spanish', 'pt': 'Portuguese', 'fr': 'French', 'de': 'German', 'it': 'Italian', 'nl': 'Dutch', 'pl': 'Polish',
	'tr': 'Turkish', 'da': 'Danish', 'id': 'Indonesian', 'ar': 'Arabic', 'ru': 'Russian', 'zh': 'Chinese (Simplified)', 'ja': 'Japanese', 'ko': 'Korean'}
SPEC = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?([a-zA-Z]))')
def specs(s): return [m.group(1) for m in SPEC.finditer(s.replace('%%', '')) if m.group(1)]
en = [s for s, _ in phrases()]
bad = 0
for code in sys.argv[1:]:
	lines = open(os.path.join(root, 'tools/lang', code + '.list'), encoding='utf-8').read().split('\n')
	while lines and not lines[-1].strip(): lines.pop()
	if len(lines) != len(en):
		print(f'{code}: {len(lines)} lines, {len(en)} phrases'); bad += 1; continue
	out = [f'# HDD Atlas in {NAMES[code]}: "English<TAB>translation" a line; \\n is a new line.',
		'# Keep every %s, %d... in the same order. Written for HDD Atlas, not yet checked by a native speaker: corrections welcome.']
	for i, (e, t) in enumerate(zip(en, lines)):
		t = t.rstrip('\r').replace('\\n', '\n')
		if specs(e) != specs(t):
			print(f'{code}:{i + 1}: % conversions differ: {esc(e)!r} vs {esc(t)!r}'); bad += 1
		out.append(esc(e) + '\t' + esc(t))
	open(os.path.join(root, 'data/lang', code + '.txt'), 'w', encoding='utf-8').write('\n'.join(out) + '\n')
	print(f'{code}: {len(en)} phrases')
sys.exit(1 if bad else 0)
