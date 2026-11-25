#!/usr/bin/env python3
"""ab_compare.py <log A> <log B> [...]: the console runs of an A/B, window by window, at the clock each ran at.

For each log (a run with test_level and test_level_after: New Game's chapter 1, then level NN): the steady part of
chapter 1 (from 150 s after its level init, past the graphic novel, to the next level init) and of level NN (from 60
s after its init to the
end), each as the median of the `d3d: Swap` rows (fps, game ms, present ms), of the renderer's ms (`dk:`/`gl:`
present lines) and of the CPU clock the port measured (`threads: ... CPU at N MHz`). A window whose clock moved
(Horizon-OC raised it mid-run once) is split at the change. Compare only windows at the same clock. Reads only.
"""
import re
import statistics
import sys

TIME = re.compile(r'^\[\s*([\d.]+)\]')
SWAP = re.compile(r'd3d: Swap \d+: ([\d.]+) fps, longest (\d+) ms.*?; game ([\d.]+) present ([\d.]+)')
REND = re.compile(r'(?:gl|dk): present \d+, .*renderer ([\d.]+) ms on the (?:title|render)')
CLOCK = re.compile(r'CPU at (\d+) MHz')
LEVEL = re.compile(r'game: level init (\S+)')


def rows_of(path):
    swaps, rends, clocks, levels = [], [], [], []
    for line in open(path, encoding='utf-8', errors='replace'):
        t = TIME.match(line)
        if not t:
            continue
        ts = float(t.group(1))
        m = SWAP.search(line)
        if m:
            swaps.append((ts, float(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4))))
            continue
        m = REND.search(line)
        if m:
            rends.append((ts, float(m.group(1))))
            continue
        m = CLOCK.search(line)
        if m:
            clocks.append((ts, int(m.group(1))))
            continue
        m = LEVEL.search(line)
        if m:
            levels.append((ts, m.group(1)))
    return swaps, rends, clocks, levels


def clock_at(clocks, ts):
    best = None
    for c in clocks:
        if c[0] <= ts:
            best = c[1]
    return best


def bucket(mhz):
    return 'unknown' if mhz is None else ('~1020' if mhz < 1300 else ('~1785' if mhz > 1600 else str(mhz)))


def window(name, swaps, rends, clocks, t0, t1):
    out = {}
    for s in swaps:
        if t0 <= s[0] < t1:
            out.setdefault(bucket(clock_at(clocks, s[0])), {'s': [], 'r': []})['s'].append(s)
    for r in rends:
        if t0 <= r[0] < t1:
            out.setdefault(bucket(clock_at(clocks, r[0])), {'s': [], 'r': []})['r'].append(r[1])
    lines = []
    for clk, d in sorted(out.items()):
        if len(d['s']) < 3:
            continue
        med = lambda i: statistics.median(x[i] for x in d['s'])
        lines.append('  %-9s %-6s rows %3d  fps %5.1f  game %5.2f ms  present %4.2f ms  renderer %5.2f ms  longest %3d ms' % (
            name, clk, len(d['s']), med(1), med(3), med(4), statistics.median(d['r']) if d['r'] else float('nan'),
            max(x[2] for x in d['s'])))
    return lines


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        swaps, rends, clocks, levels = rows_of(path)
        print('==', path, ' levels:', ', '.join('%s@%.0f' % (n, t) for t, n in levels))
        end = swaps[-1][0] + 1 if swaps else 0
        for i, (t, n) in enumerate(levels):
            t1 = levels[i + 1][0] if i + 1 < len(levels) else end
            # New Game's level init comes before its graphic novel (about 2 minutes): chapter 1's play from 150 s;
            # a level changed to later is loaded and playing about 40 s after its init
            lead = 150 if i == 0 else 45
            for l in window(n[:9], swaps, rends, clocks, t + lead, t1):
                print(l)


if __name__ == '__main__':
    main()
