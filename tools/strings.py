# Every phrase the app shows: the string literals inside tr(...) and N_(...) in src/
# (adjacent literals joined, C escapes decoded), one per line, as data/lang/en.txt's
# list for translating. Prints how many; with --check <lang>, the ones that language lacks.
#   python3 tools/strings.py [--check es]
import os, re, sys, glob
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
LIT = r'"(?:[^"\\]|\\.)*"'

def unescape(lit):
    out = bytearray()
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != '\\':
            out += c.encode('utf-8'); i += 1; continue
        n = lit[i + 1]
        if n == 'x':
            m = re.match(r'[0-9a-fA-F]{1,2}', lit[i + 2:])
            out.append(int(m.group(0), 16)); i += 2 + len(m.group(0))
        elif n == 'n': out += b'\n'; i += 2
        elif n == 't': out += b'\t'; i += 2
        else: out += n.encode('utf-8'); i += 2
    return out.decode('utf-8')

def call_args(text, start):
    # the text of a call's arguments, from just after its "(" to its matching ")" (strings respected)
    depth, i = 1, start
    while i < len(text) and depth:
        c = text[i]
        if c == '"':
            m = re.match(LIT, text[i:])
            i += len(m.group(0)) if m else 1
            continue
        if c == '(': depth += 1
        elif c == ')': depth -= 1
        i += 1
    return text[start:i - 1]

def phrases():
    # every literal inside tr(...) and N_(...), as a ternary's two too; literals side by side joined
    seen, order = set(), []
    for f in sorted(glob.glob(os.path.join(root, 'src', '*.c'))):
        text = open(f, encoding='utf-8').read()
        for m in re.finditer(r'\b(?:tr|N_)\(', text):
            args = call_args(text, m.end())
            for run in re.finditer(r'(?:' + LIT + r'\s*)+', args):
                parts = re.findall(LIT, run.group(0))
                s = ''.join(unescape(p[1:-1]) for p in parts)
                if s and s not in seen:
                    seen.add(s); order.append((s, os.path.basename(f)))
    return order

def esc(s): return s.replace('\\', '\\\\').replace('\n', '\\n')

if __name__ == '__main__':
    ps = phrases()
    if len(sys.argv) > 2 and sys.argv[1] == '--check':
        have = set()
        for line in open(os.path.join(root, 'data/lang', sys.argv[2] + '.txt'), encoding='utf-8'):
            if line.startswith('#') or '\t' not in line: continue
            have.add(line.split('\t')[0].replace('\\n', '\n').replace('\\\\', '\\'))
        missing = [s for s, _ in ps if s not in have]
        for s in missing: print('missing:', esc(s))
        extra = have - set(s for s, _ in ps)
        for s in sorted(extra): print('unused: ', esc(s))
        print(f'{sys.argv[2]}: {len(ps) - len(missing)} of {len(ps)}')
        sys.exit(1 if missing else 0)
    for s, f in ps: print(esc(s))
    print(len(ps), 'phrases', file=sys.stderr)
