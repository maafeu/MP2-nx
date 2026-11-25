#!/usr/bin/env python3
"""prof_lines.py <prof_NNN.bin> <elf> <function substring> [--top N]: where inside a function (inlined code
included) the console profiler's samples fell, by source line.

Takes the samples whose pc lies in the named function's symbol range (aarch64-none-elf-nm -S), resolves each distinct
pc with addr2line -i (the innermost inlined function and its line first) and prints the lines and the inlined
functions with their share of the function's samples. Reads only.
"""
import collections
import os
import struct
import subprocess
import sys

BIN = r'C:\devkitPro\devkitA64\bin'


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    prof, elf, want = sys.argv[1], sys.argv[2], sys.argv[3]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 30
    out = subprocess.run([os.path.join(BIN, 'aarch64-none-elf-nm.exe'), '-S', '-C', '--defined-only', elf],
                         capture_output=True, text=True, errors='replace').stdout
    ranges = []
    for line in out.splitlines():
        parts = line.split(' ', 3)
        if len(parts) == 4 and parts[2] in 'tTwW' and want in parts[3]:
            a, s = int(parts[0], 16), int(parts[1], 16)
            ranges.append((a, a + s, parts[3]))
    if not ranges:
        sys.exit('no function matching ' + want)
    data = open(prof, 'rb').read()
    pcs = collections.Counter()
    total = 0
    for i in range(0, len(data) - 31, 32):
        pc = struct.unpack_from('<I', data, i + 4)[0]
        total += 1
        for a, b, _ in ranges:
            if a <= pc < b:
                pcs[pc] += 1
                break
    n = sum(pcs.values())
    print('%s: %d of %d samples (%.2f%%) in %d function(s)' % (want, n, total, 100.0 * n / max(total, 1), len(ranges)))
    addrs = sorted(pcs)
    res = subprocess.run([os.path.join(BIN, 'aarch64-none-elf-addr2line.exe'), '-i', '-f', '-C', '-e', elf] +
                         ['0x%x' % a for a in addrs], capture_output=True, text=True, errors='replace').stdout.splitlines()
    # addr2line -i prints one or more (function, file:line) pairs per address; split by re-running per address is slow,
    # so mark the boundaries with a second pass that has no -i (one pair per address)
    res1 = subprocess.run([os.path.join(BIN, 'aarch64-none-elf-addr2line.exe'), '-f', '-C', '-e', elf] +
                          ['0x%x' % a for a in addrs], capture_output=True, text=True, errors='replace').stdout.splitlines()
    by_line, by_func = collections.Counter(), collections.Counter()
    # innermost frame of each address = the first pair of its -i output, which equals the plain output's pair
    for k, a in enumerate(addrs):
        func, where = res1[2 * k], res1[2 * k + 1]
        where = where.split(' (')[0]
        short = '/'.join(where.replace('\\', '/').split('/')[-2:])
        by_line[(short, func[:60])] += pcs[a]
        by_func[func[:90]] += pcs[a]
    print('-- innermost functions')
    for f, c in by_func.most_common(top):
        print('  %5.1f%%  %s' % (100.0 * c / n, f))
    print('-- lines')
    for (w, f), c in by_line.most_common(top):
        print('  %5.1f%%  %-40s %s' % (100.0 * c / n, w, f))


if __name__ == '__main__':
    main()
