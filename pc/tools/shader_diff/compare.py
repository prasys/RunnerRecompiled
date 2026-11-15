#!/usr/bin/env python3
"""Compares the runner's results of the two translations and prints / writes the report.

A component matches when |a - b| <= ATOL + RTOL * max(|a|, |b|), or both are NaN, or both are the same infinity.
Classes per shader and input set (worst wins): interface (an output exists on one side only), discard (one side
killed the pixel), nonfinite (NaN/Inf on one side only, or different ones), value (finite, outside tolerance).
"""
import argparse
import array
import collections
import json
import math
import os

SETS, W, H, VSN, SLOTS = 4, 8, 8, 16, 40
SETNAME = ('benign', 'signed', 'edge', 'zero')
ATOL, RTOL = 1e-4, 1e-3
SENT = -7777.0
RANK = {'match': 0, 'value': 1, 'nonfinite': 2, 'discard': 3, 'interface': 4}


def load(path):
    a = array.array('f')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    return a


def cmpv(a, b):
    """-> (class, abs error)"""
    an, bn = math.isnan(a), math.isnan(b)
    if an or bn:
        return ('match', 0.0) if an and bn else ('nonfinite', math.inf)
    if math.isinf(a) or math.isinf(b):
        return ('match', 0.0) if a == b else ('nonfinite', math.inf)
    d = abs(a - b)
    return ('match' if d <= ATOL + RTOL * max(abs(a), abs(b)) else 'value', d)


def compare_ps(job):
    s, d = job['sides']['swarc'], job['sides']['dxvk']
    a, b = load(s['out']), load(d['out'])
    n = W * H * SETS
    res = []
    for st in range(SETS):
        worst, err, detail = 'match', 0.0, ''
        if s['colors'] != d['colors']:
            worst, detail = 'interface', 'colour outputs %s vs %s' % (s['colors'], d['colors'])
        nonfin = 0
        for ci, c in enumerate(s['colors']):
            if c not in d['colors'] or c > 3:
                continue
            di = d['colors'].index(c)
            for p in range(st * W * H, (st + 1) * W * H):
                x = a[ci * n * 4 + p * 4: ci * n * 4 + p * 4 + 4]
                y = b[di * n * 4 + p * 4: di * n * 4 + p * 4 + 4]
                kx, ky = all(v == SENT for v in x), all(v == SENT for v in y)
                if kx or ky:
                    if kx != ky and RANK['discard'] > RANK[worst]:
                        worst, detail = 'discard', 'pixel %d: %s killed it' % (p % (W * H), 'swarc' if kx else 'dxvk')
                    continue
                for k in range(4):
                    if not (math.isfinite(x[k]) and math.isfinite(y[k])):
                        nonfin += 1
                    cl, e = cmpv(x[k], y[k])
                    if RANK[cl] > RANK[worst] or (cl == worst and cl != 'match' and e > err):
                        worst, detail = cl, 'oC%d.%s pixel %d: swarc %g dxvk %g' % (c, 'rgba'[k], p % (W * H), x[k], y[k])
                    if cl == 'value':
                        err = max(err, e)
        if s['depth'] or d['depth']:
            if s['depth'] != d['depth']:
                worst, detail = 'interface', 'depth output on one side'
            oa, ob = len(s['colors']) * n * 4, len(d['colors']) * n * 4
            for p in range(st * W * H, (st + 1) * W * H):
                cl, e = cmpv(a[oa + p], b[ob + p])
                if RANK[cl] > RANK[worst]:
                    worst, detail = cl, 'oDepth pixel %d: swarc %g dxvk %g' % (p % (W * H), a[oa + p], b[ob + p])
        res.append((worst, err, detail, nonfin))
    return res


def compare_vs(job):
    s, d = job['sides']['swarc'], job['sides']['dxvk']
    a, b = load(s['out']), load(d['out'])
    keys = [('position', 15)] + [('%d.%d' % (u, i), m) for u, i, m in job['dcl_out'] if not (u == 0 and i == 0)]
    res = []
    for st in range(SETS):
        worst, err, detail, nonfin = 'match', 0.0, '', 0
        for key, mask in keys:
            if key not in s['outputs'] or key not in d['outputs']:
                if key in s['outputs'] or key in d['outputs']:
                    worst, detail = 'interface', 'output %s on one side only' % key
                continue
            for v in range(VSN):
                base = (st * VSN + v) * SLOTS * 4
                for k in range(4):
                    if not mask & (1 << k):
                        continue
                    x, y = a[base + s['outputs'][key] * 4 + k], b[base + d['outputs'][key] * 4 + k]
                    if not (math.isfinite(x) and math.isfinite(y)):
                        nonfin += 1
                    cl, e = cmpv(x, y)
                    if RANK[cl] > RANK[worst] or (cl == worst and cl != 'match' and e > err):
                        worst, detail = cl, '%s.%s vertex %d: swarc %g dxvk %g' % (key, 'xyzw'[k], v, x, y)
                    if cl == 'value':
                        err = max(err, e)
        res.append((worst, err, detail, nonfin))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True)
    ap.add_argument('--json', help='write per-shader results here')
    a = ap.parse_args()
    rows = []
    with open(os.path.join(a.work, 'jobs.jsonl')) as f:
        for line in f:
            job = json.loads(line)
            if not all(os.path.exists(job['sides'][x]['out']) for x in ('swarc', 'dxvk')):
                rows.append({'name': job['name'], 'stage': job['stage'], 'size': job['size'],
                             'features': job['features'], 'sets': [['norun', 0, '', 0]] * SETS})
                continue
            r = compare_ps(job) if job['stage'] == 'ps' else compare_vs(job)
            rows.append({'name': job['name'], 'stage': job['stage'], 'size': job['size'], 'features': job['features'],
                         'sets': [list(x) for x in r]})
    if a.json:
        with open(a.json, 'w') as f:
            for r in rows:
                f.write(json.dumps(r) + '\n')
    for stage in ('ps', 'vs'):
        rs = [r for r in rows if r['stage'] == stage]
        print('== %s: %d shaders' % (stage, len(rs)))
        for st in range(SETS):
            c = collections.Counter(r['sets'][st][0] for r in rs)
            print('  set %d %-7s %s' % (st, SETNAME[st], dict(c)))
        allm = sum(all(x[0] == 'match' for x in r['sets']) for r in rs)
        print('  match in all sets: %d' % allm)
        for st in range(SETS):
            bad = [r for r in rs if r['sets'][st][0] != 'match']
            if not bad:
                continue
            print('  set %d: features of the %d differing shaders (differing / shaders with the feature)' % (st, len(bad)))
            tot = collections.Counter(f for r in rs for f in r['features'])
            cb = collections.Counter(f for r in bad for f in r['features'])
            print('    ' + ', '.join('%s %d/%d' % (f, cb[f], tot[f]) for f in sorted(tot, key=lambda f: -cb[f] / tot[f])[:24]))
            for r in sorted(bad, key=lambda r: r['size'])[:6]:
                print('    %s %d bytes: %s: %s' % (r['name'], r['size'], r['sets'][st][0], r['sets'][st][2]))


if __name__ == '__main__':
    main()
