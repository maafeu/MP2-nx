"""MPayne2-nx: tables from the console profiler's samples (host-switch/prof.cpp, logs/prof_NNN.bin).

    python tools/prof_report.py prof_NNN.bin build/mp2/MPayne2-nx.elf [--from S] [--to S] [--slot N] [--top 40]

Each 32-byte record: thread slot, time (10 ms units), pc, lr, 5 return addresses, as offsets into the NRO (ELF
addresses). Names come from the ELF's symbol table (devkitPro's aarch64-none-elf-nm, demangled). Self time is where
the pc was; inclusive time counts each function once per sample over pc, lr and the return addresses. Reads only.
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

NM = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe'


def symbols(elf):
    out = subprocess.run([NM, '-n', '-C', '--defined-only', elf], capture_output=True, text=True, errors='replace').stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) < 3 or parts[1] not in 'tTwW':
            continue
        try:
            a = int(parts[0], 16)
        except ValueError:
            continue
        addrs.append(a)
        names.append(parts[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bin')
    ap.add_argument('elf')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    ap.add_argument('--slot', type=int, default=-1)
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    addrs, names = symbols(a.elf)

    def name(off):
        if off == 0xFFFFFFFF:
            return None
        i = bisect.bisect_right(addrs, off) - 1
        return names[i] if i >= 0 else '?'

    data = open(a.bin, 'rb').read()
    n = len(data) // 32
    self_c, incl_c, slots, chains = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    total = 0
    for k in range(n):
        slot, when, pc, lr, *ret = struct.unpack_from('<HHII5I', data, k * 32)
        t = when / 100.0
        if t < a.t0 or t > a.t1 or (a.slot >= 0 and slot != a.slot):
            continue
        total += 1
        slots[slot] += 1
        s = name(pc) or '?'
        self_c[s] += 1
        seen = set()
        for off in (pc, lr, *ret):
            nm = name(off)
            if nm and nm not in seen:
                seen.add(nm)
                incl_c[nm] += 1
        chain = [s] + [x for x in (name(r) for r in ret[:3]) if x]
        chains[' < '.join(chain)] += 1
    if not total:
        sys.exit('no samples in range')
    print('%s: %d samples (%d in the file); by thread slot: %s' % (os.path.basename(a.bin), total, n,
          ', '.join('%d: %.1f%%' % (s, 100.0 * c / total) for s, c in slots.most_common())))
    print('\n-- self time')
    for s, c in self_c.most_common(a.top):
        print('%6.2f%%  %s' % (100.0 * c / total, s[:140]))
    print('\n-- inclusive (pc, lr, return addresses)')
    for s, c in incl_c.most_common(a.top):
        print('%6.2f%%  %s' % (100.0 * c / total, s[:140]))
    print('\n-- chains (pc < 3 return addresses)')
    for s, c in chains.most_common(25):
        print('%6.2f%%  %s' % (100.0 * c / total, s[:220]))
    # a rough split: the game's recompiled code (sub_*) against the rest
    game = sum(c for s, c in self_c.items() if s.startswith('sub_') or s.startswith('__imp__sub_'))
    print('\nthe game\'s recompiled code (sub_*): %.1f%% of the self samples' % (100.0 * game / total))


if __name__ == '__main__':
    main()
