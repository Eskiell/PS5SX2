#!/usr/bin/env python3
# PS5SX2 (vk-285-118, AI-assisted): reads the [autoprof] lines of session reports or boot logs and names the busy code.
#
#   autoprof.py --elf llvm-pie.elf REPORT_OR_BOOTLOG...   [--by-game] [--top 25]
#
# Each window's eboot buckets are offsets from OrbisEEProfStart at run time; the ELF gives that function's address, so
# bucket + elf(OrbisEEProfStart) is an ELF address, named with the ELF's symbols (llvm-nm -C). Library callers are named
# the same way. With --by-game, windows of the same game (the report's "Game:" line) are added up, weighted by samples.
# Copyright (C) 2026 Spyros
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse, bisect, collections, re, shutil, subprocess, sys

HEAD = re.compile(r'\[autoprof\] #(\d+) (\S+) thread \(speed (\d+)%, load ee (\d+) gs (\d+) vu (\d+)\) \| (\d+) samples in \d+ s, (\d+) skipped'
                  r' \(waits\) \| eboot ([\d.]+)% jit ([\d.]+)% lib ([\d.]+)% other ([\d.]+)% \| ref=(0x[0-9a-f]+)')
BUCKET = re.compile(r'([+-])(0x[0-9a-f]+) ([\d.]+)%')
AREA = re.compile(r'(\w+) ([\d.]+)%')


def symbols(elf):
    nm = shutil.which('llvm-nm') or 'nm'
    out = subprocess.run([nm, '-C', '--defined-only', elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    ref = next((a for a, n in syms if n.startswith('OrbisEEProfStart(')), None)
    if ref is None:
        sys.exit('autoprof.py: no OrbisEEProfStart in ' + elf)
    return [a for a, _ in syms], [n for _, n in syms], ref


def name_of(addrs, names, a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else '?'


def windows(path):
    game = '?'
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('Game: '):
            game = re.sub(r' \(from /mnt/.*\)$', '', line[6:].strip())
        if '[autoprof]' not in line:
            continue
        line = line[line.index('[autoprof]'):].rstrip('\n')
        m = HEAD.match(line)
        if m:
            if cur:
                yield cur
            cur = dict(game=game, file=path, thread=m.group(2), speed=int(m.group(3)), loads=tuple(map(int, m.group(4, 5, 6))),
                       n=int(m.group(7)), eboot=float(m.group(9)), jit=float(m.group(10)), lib=float(m.group(11)), buckets=[], areas={},
                       libs=[], callers=[])
            continue
        if not cur:
            continue
        body = line.split(':', 1)[1] if ':' in line else ''
        if ' eboot:' in line:
            cur['buckets'] = [((-1 if s == '-' else 1) * int(o, 16), float(p)) for s, o, p in BUCKET.findall(body)]
        elif ' jit:' in line:
            cur['areas'] = {a: float(p) for a, p in AREA.findall(body)}
        elif ' lib:' in line:
            libs, _, callers = body.partition('| callers:')
            cur['libs'] = [(int(a, 16), float(p)) for a, p in re.findall(r'(0x[0-9a-f]+) ([\d.]+)%', libs)]
            cur['callers'] = [((-1 if s == '-' else 1) * int(o, 16), float(p)) for s, o, p in BUCKET.findall(callers)]
    if cur:
        yield cur


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--by-game', action='store_true')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()
    addrs, names, ref = symbols(args.elf)
    W = [w for f in args.files for w in windows(f)]
    groups = collections.defaultdict(list)
    for w in W:
        groups[(w['game'], w['thread']) if args.by_game else (w['file'], w['thread'])].append(w)
    for key, ws in sorted(groups.items(), key=lambda kv: -sum(w['n'] for w in kv[1])):
        n = sum(w['n'] for w in ws)
        fn = collections.Counter()
        callers = collections.Counter()
        areas = collections.Counter()
        for w in ws:
            k = w['n'] / n
            for off, p in w['buckets']:
                fn[name_of(addrs, names, ref + off)] += p * k
            for off, p in w['callers']:
                callers[name_of(addrs, names, ref + off)] += p * k
            for a, p in w['areas'].items():
                areas[a] += p * k
        avg = lambda f: sum(w[f] * w['n'] for w in ws) / n
        print(f"== {key[0]} | {key[1]} thread | {len(ws)} window(s), {n} samples, speed {sum(w['speed'] for w in ws) / len(ws):.0f}%"
              f" | eboot {avg('eboot'):.1f}% jit {avg('jit'):.1f}% lib {avg('lib'):.1f}%")
        for name, p in fn.most_common(args.top):
            print(f"  {p:5.1f}%  {name[:150]}")
        if areas:
            print('  jit: ' + ', '.join(f'{a} {p:.1f}%' for a, p in areas.most_common()))
        if callers:
            print('  library time called from: ' + '; '.join(f'{c[:80]} {p:.1f}%' for c, p in callers.most_common(6)))


if __name__ == '__main__':
    main()
