"""MPayne2-nx: one table from a port log (PC, Ryujinx or the console).

    python tools/log_summary.py logs/mp2_NNN.log [more logs...]

Each row is one `d3d: Swap N` line (every 300 frames) with the `gl: present` (or the deko3d renderer's `dk: present`)
line that follows it: fps, longest frame, frames over 40 ms, the game's and present's ms a frame, pace (the wait for
the 30 fps cap), the renderer's ms on the title's thread, draws a frame, KB uploaded a frame and read-backs, and with
the deko3d renderer its GPU ms a frame. Level loads (`game: level init`), the native code's timing lines, the renderer
self-test, warnings and any crash are listed after. Reads only.
"""
import re
import statistics
import sys

SWAP = re.compile(r'^\[\s*([\d.]+)\].*d3d: Swap (\d+): ([\d.]+) fps, longest (\d+) ms, (\d+) over 40 ms'
                  r'(?: \((\d+) with game over 30, longest game (\d+)\))?; game ([\d.]+) present ([\d.]+)'
                  r'(?: gpu wait ([\d.]+))? pace ([\d.]+) ms')
GL = re.compile(r'(?:gl|dk): present (\d+), (\d+) draws, (\d+) textures, (\d+) targets, (\d+) programs; a frame: (\d+) KB '
                r'uploaded, (\d+) state calls, (\d+) skipped as unchanged, renderer ([\d.]+) ms on the (?:title\'s|render) thread'
                r'(?:; (\d+) read-backs \(([\d.]+) ms\))?')
DKGPU = re.compile(r'dk: renderer parts .*; GPU ([\d.]+) ms a frame')
TIME = re.compile(r'^\[\s*([\d.]+)\]')


def summarize(path):
    rows, notes, warn_count, last_draws = [], [], {}, None
    pending = None
    for raw in open(path, encoding='utf-8', errors='replace'):
        line = raw.rstrip('\n')
        m = SWAP.match(line)
        if m:
            if pending:
                rows.append(pending)
            g = m.groups()
            pending = dict(t=float(g[0]), frame=int(g[1]), fps=float(g[2]), longest=int(g[3]), over40=int(g[4]),
                           game_over30=int(g[5]) if g[5] else 0, longest_game=int(g[6]) if g[6] else 0,
                           game=float(g[7]), present=float(g[8]), pace=float(g[10]), renderer=None, draws=None,
                           kb=None, readbacks=None, gpu=None)
            continue
        m = DKGPU.search(line)
        if m:
            if pending is not None and pending['gpu'] is None:
                pending['gpu'] = float(m.group(1))
            continue
        m = GL.search(line)
        if m:
            g = m.groups()
            draws = int(g[1])
            if pending is not None and pending['renderer'] is None:
                pending['renderer'] = float(g[8])
                pending['kb'] = int(g[5])
                pending['readbacks'] = int(g[9]) if g[9] else 0
                if last_draws is not None and draws >= last_draws:
                    pending['draws'] = (draws - last_draws) / 300.0
            last_draws = draws
            continue
        if ('game: level' in line or 'native:' in line or 'selftest' in line or 'crash' in line
                or 'test_level' in line or 'gl: error' in line or 'fatal' in line.lower()
                or 'does not compile' in line or 'dk: deko3d renderer' in line):
            notes.append(line[:220])
        if '[warn]' in line or '[error]' in line:
            key = re.sub(r'\d+', 'N', line.split('] ', 3)[-1])[:70]
            warn_count[key] = warn_count.get(key, 0) + 1
    if pending:
        rows.append(pending)

    print('==', path)
    print('%8s %6s %5s %5s %4s %4s %6s %6s %6s %6s %6s %5s %3s %5s' % (
        'time', 'frame', 'fps', 'long', '>40', 'g>30', 'game', 'pres', 'pace', 'rend', 'draws', 'KB', 'rb', 'gpu'))
    for r in rows:
        print('%8.1f %6d %5.1f %5d %4d %4d %6.1f %6.1f %6.1f %6s %6s %5s %3s %5s' % (
            r['t'], r['frame'], r['fps'], r['longest'], r['over40'], r['game_over30'], r['game'], r['present'],
            r['pace'], '%.2f' % r['renderer'] if r['renderer'] is not None else '-',
            '%.0f' % r['draws'] if r['draws'] is not None else '-',
            r['kb'] if r['kb'] is not None else '-', r['readbacks'] if r['readbacks'] is not None else '-',
            '%.1f' % r['gpu'] if r['gpu'] is not None else '-'))
    steady = [r for r in rows if r['frame'] >= 600]
    if steady:
        fps = [r['fps'] for r in steady]
        print('rows from frame 600: %d; fps median %.1f, lowest %.1f; frames over 40 ms %d; game median %.1f ms, '
              'highest %.1f ms' % (len(steady), statistics.median(fps), min(fps), sum(r['over40'] for r in steady),
                                    statistics.median(r['game'] for r in steady), max(r['game'] for r in steady)))
    if notes:
        print('-- notes')
        for n in notes[:80]:
            print(n)
        if len(notes) > 80:
            print('... %d more' % (len(notes) - 80))
    if warn_count:
        print('-- warnings and errors (count, shape)')
        for k, v in sorted(warn_count.items(), key=lambda kv: -kv[1])[:20]:
            print('%6d  %s' % (v, k))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        summarize(p)
