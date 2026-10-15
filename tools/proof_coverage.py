#!/usr/bin/env python3
"""Count how much of the kernel carries a specification.

    tools/proof_coverage.py                 print the summary tables
    tools/proof_coverage.py --markdown      the body of docs/proof-coverage.md
    tools/proof_coverage.py --list FILE..   every function of FILE with its tier

The scan is textual: it reads `fn` headers and their `requires` / `ensures`
clauses, so it needs neither the compiler nor Z3. It says what is *stated*;
whether the statement holds is the compiler's job (every clause listed here
is proved at build time).

Each function falls into one tier, by its strongest clause:

  none        no `requires` and no `ensures`
  requires    `requires` only: callers must prove it, callers learn nothing back
  frame       `ensures` that only restate an invariant (mem_ok(m), pt_level(t)
              == 1, len unchanged, ...): the resources are accounted for, the
              result is not described
  functional  `ensures` about the result or the changed state (a range, a
              set of possible values, a relation to the arguments)
"""
import glob, os, re, sys, collections

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
FN = re.compile(r'^\s*(?:export\s+)?(?:opaque\s+)?fn\s+(\w+)')
CLAUSE = re.compile(r'\b(requires|ensures)\b(.*?)(?=\brequires\b|\bensures\b|\{\s*$|;\s*$|$)', re.S)
# Calls that restate that a resource or table is still well formed.
FRAME_CALL = re.compile(r'\b(?:\w*ok|pt_level|pt_leases|pt_id|frame_id|pool_size|pt_used'
                        r'|spaces_ok|ptab_ok)\s*\([^()]*\)')
SAME_LEN = re.compile(r'\b(?:blen|len|vlen)\s*\([^()]*\)\s*==\s*old\(\s*(?:blen|len|vlen)\s*\([^()]*\)\s*\)')

# Files whose functions manage memory, or turn a system call's arguments
# into kernel actions: the places where a missing specification costs most.
CRITICAL = collections.OrderedDict([
    ('Memory management', ['space', 'usermem', 'pages', 'pcache', 'lxmem', 'lxanon', 'lxcow',
                           'lazy', 'text', 'lxmount']),
    ('System-call arguments', ['linux', 'lxfd', 'lxsys', 'lxsig', 'lxproc', 'lxthread',
                               'lxrlimit', 'lxprio', 'lxperm', 'lxcred', 'lxopen2', 'lxlink',
                               'lxdir', 'lxsock', 'lxmsg', 'lxipc', 'lxmq', 'lxlock']),
    ('Device drivers (DMA)', ['e1000', 'ena', 'virtq', 'ahci', 'nvme', 'xhci', 'hda', 'rtl8168']),
])


def tier_of(clauses):
    req = [c for k, c in clauses if k == 'requires']
    ens = [c for k, c in clauses if k == 'ensures']
    if not req and not ens:
        return 'none'
    if not ens:
        return 'requires'
    for e in ens:
        rest = SAME_LEN.sub('', FRAME_CALL.sub('', e))
        rest = re.sub(r'[\s&|()]|==\s*\d+|true', '', rest)
        if rest:
            return 'functional'
    return 'frame'


def scan(path):
    lines = open(path).read().split('\n')
    out, i = [], 0
    while i < len(lines):
        m = FN.match(lines[i])
        if not m or lines[i].lstrip().startswith('//'):
            i += 1
            continue
        j, sig = i, []
        while j < len(lines):
            sig.append(re.sub(r'//.*', '', lines[j]))
            if sig[-1].rstrip().endswith(('{', ';')):
                break
            j += 1
        text = ' '.join(sig)
        head = text.split('requires')[0].split('ensures')[0]
        ret = re.search(r'\)\s*->\s*([\w\[\]&]+)', head)
        depth, k = (1, j + 1) if sig[-1].rstrip().endswith('{') else (0, j + 1)
        while depth and k < len(lines):
            depth += lines[k].count('{') - lines[k].count('}')
            k += 1
        clauses = [(a, b.strip()) for a, b in CLAUSE.findall(text)]
        out.append(dict(file=os.path.relpath(path, ROOT), line=i + 1, name=m.group(1),
                        tier=tier_of(clauses), ret=ret.group(1) if ret else None,
                        body=k - j - 1))
        i = k if depth == 0 and k > j else j + 1
    return out


def all_functions():
    fns = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'src', '**', '*.bastio'), recursive=True)):
        fns += scan(p)
    return fns


LISTED = 25
TIERS = ['functional', 'frame', 'requires', 'none']


def stem(f):
    return os.path.splitext(os.path.basename(f))[0]


def table(rows, head):
    out = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    out += ['| ' + ' | '.join(str(c) for c in r) + ' |' for r in rows]
    return '\n'.join(out)


def counts(fns):
    c = collections.Counter(f['tier'] for f in fns)
    n = len(fns)
    return [n] + [c[t] for t in TIERS] + ['%.0f%%' % (100.0 * c['functional'] / n if n else 0)]


def markdown(fns):
    head = ['', 'Functions'] + TIERS + ['Functional']
    out = []
    out.append(table([['**All of `src/`**'] + counts(fns)], head))
    out.append('')
    rows = []
    for area, stems in CRITICAL.items():
        sel = [f for f in fns if stem(f['file']) in stems]
        rows.append([area] + counts(sel))
    out.append(table(rows, ['Area'] + head[1:]))
    out.append('')
    # By file, weakest first.
    byf = collections.defaultdict(list)
    for f in fns:
        byf[f['file']].append(f)
    weak = sorted(byf.items(), key=lambda kv: -(sum(1 for f in kv[1] if f['tier'] in ('none', 'requires'))))
    rows = [['`%s`' % p] + counts(v) for p, v in weak[:25]]
    out.append('### The 25 files with the most functions that promise nothing back\n')
    out.append(table(rows, ['File'] + head[1:]))
    out.append('')
    for area, stems in CRITICAL.items():
        sel = [f for f in fns if stem(f['file']) in stems and f['tier'] != 'functional'
               and f['ret'] == 'u64' and f['body'] >= 20]
        sel.sort(key=lambda f: -f['body'])
        out.append('### %s: the longest `u64`-returning functions whose result is not described\n' % area)
        out.append('%d functions have a body of twenty lines or more and return a `u64` (a count, an address or -errno); '
                   'the longest %d:\n' % (len(sel), min(len(sel), LISTED)))
        rows = [['`%s:%d`' % (f['file'], f['line']), '`%s`' % f['name'], f['tier'], f['ret'], f['body']]
                for f in sel[:LISTED]]
        out.append(table(rows, ['Where', 'Function', 'Tier', 'Returns', 'Lines']) if rows else '_None._')
        out.append('')
    return '\n'.join(out)


def main(argv):
    fns = all_functions()
    if argv[:1] == ['--markdown']:
        print(markdown(fns))
    elif argv[:1] == ['--list']:
        sel = [f for f in fns if any(f['file'].endswith(a) for a in argv[1:])]
        for f in sel:
            print('%s:%d\t%s\t%s' % (f['file'], f['line'], f['tier'], f['name']))
    else:
        c = collections.Counter(f['tier'] for f in fns)
        print('%d functions' % len(fns))
        for t in TIERS:
            print('  %-10s %5d' % (t, c[t]))


if __name__ == '__main__':
    main(sys.argv[1:])
