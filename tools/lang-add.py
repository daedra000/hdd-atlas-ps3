# Adds the phrases a language lacks to data/lang/<code>.txt: tools/lang/<code>.add holds their
# translations, one a line, in the order `tools/strings.py --check <code>` lists them missing. The file
# is rewritten in the app's order, phrases it no longer uses left out; every line's % conversions
# are checked against the English (make-lang.py's check).
#   python3 tools/lang-add.py es [fr ...]
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strings import phrases, esc
from importlib import import_module
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ml = import_module('make-lang'.replace('-', '_')) if False else None
import re
SPEC = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?([a-zA-Z]))')
def specs(s): return [m.group(1) for m in SPEC.finditer(s.replace('%%', '')) if m.group(1)]
def unesc(s): return s.replace('\\n', '\n').replace('\\\\', '\\')
en = [s for s, _ in phrases()]
bad = 0
for code in sys.argv[1:]:
	path = os.path.join(root, 'data/lang', code + '.txt')
	head, have = [], {}
	for line in open(path, encoding='utf-8').read().split('\n'):
		if line.startswith('#'): head.append(line); continue
		if '\t' not in line: continue
		e, t = line.split('\t', 1)
		have[unesc(e)] = unesc(t)
	missing = [e for e in en if e not in have]
	add = open(os.path.join(root, 'tools/lang', code + '.add'), encoding='utf-8').read().split('\n')
	while add and not add[-1].strip(): add.pop()
	if len(add) != len(missing):
		print(f'{code}: {len(add)} lines for {len(missing)} missing'); bad += 1; continue
	for e, t in zip(missing, add):
		t = t.rstrip('\r').replace('\\n', '\n')
		if specs(e) != specs(t): print(f'{code}: % conversions differ: {esc(e)!r} vs {esc(t)!r}'); bad += 1
		have[e] = t
	out = head + [esc(e) + '\t' + esc(have[e]) for e in en]
	open(path, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
	print(f'{code}: {len(missing)} added, {len(en)} phrases')
sys.exit(1 if bad else 0)
