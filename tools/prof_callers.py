#!/usr/bin/env python3
"""prof_callers.py <prof_NNN.bin> <elf> <function name> [--top N]: who called a function in the console profile.

For the samples whose pc lies in the named function (exact symbol name, e.g. memmove), the function holding the
sample's lr (its caller, for a leaf such as memmove) is counted. Symbols without a size in nm's output (assembly
functions) run to the next symbol. Reads only.
"""
import bisect
import collections
import struct
import subprocess
import sys

NM = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe'


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    prof, elf, want = sys.argv[1], sys.argv[2], sys.argv[3]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 15
    out = subprocess.run([NM, '-n', '-S', '-C', '--defined-only', elf], capture_output=True, text=True,
                         errors='replace').stdout
    syms = []
    for line in out.splitlines():
        p = line.split(' ')
        if len(p) >= 4 and len(p[1]) > 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16), int(p[1], 16), ' '.join(p[3:])))
        elif len(p) >= 3 and p[1] in 'tTwW':
            syms.append((int(p[0], 16), 0, ' '.join(p[2:])))
    syms.sort()
    starts = [s[0] for s in syms]

    def span(i):
        a, s, _ = syms[i]
        return a + s if s else (syms[i + 1][0] if i + 1 < len(syms) else a + 4)

    def name(a):
        i = bisect.bisect_right(starts, a) - 1
        return syms[i][2][:90] if i >= 0 and syms[i][0] <= a < span(i) else '?'

    idx = [i for i, s in enumerate(syms) if s[2] == want]
    if not idx:
        sys.exit('no symbol named ' + want)
    lo, hi = syms[idx[0]][0], span(idx[0])
    data = open(prof, 'rb').read()
    callers, n, total = collections.Counter(), 0, 0
    for i in range(0, len(data) - 31, 32):
        pc, lr = struct.unpack_from('<II', data, i + 4)
        total += 1
        if lo <= pc < hi:
            n += 1
            callers[name(lr)] += 1
    print('%s: %d samples (%.2f%% of %d)' % (want, n, 100.0 * n / max(total, 1), total))
    for c, k in callers.most_common(top):
        print('  %5.1f%%  %s' % (100.0 * k / max(n, 1), c))


if __name__ == '__main__':
    main()
