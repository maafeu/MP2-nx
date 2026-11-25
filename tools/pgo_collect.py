#!/usr/bin/env python3
"""pgo_collect.py <pgo folder from the card>: the training run's profiles back at the names GCC gave them.

The instrumented NRO (PORT_PGO=generate) writes pgo/NNNN.gcda and pgo/names.txt ("NNNN <name>", the name being the
object's absolute PC path with .gcda: host-switch/pgo.cpp). Each file is copied to its name, beside the instrumented
build's objects, where the PORT_PGO=use build of the same folder reads it. Prints how many and where; writes nothing
but those .gcda files.
"""
import os
import shutil
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    done, missing = 0, 0
    dirs = set()
    for line in open(os.path.join(src, "names.txt"), encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if len(line) < 6:
            continue
        num, name = line[:4], line[5:]
        f = os.path.join(src, num + ".gcda")
        if not os.path.exists(f) or not name:
            missing += 1
            continue
        dst = os.path.normpath(name.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(f, dst)
        dirs.add(os.path.dirname(dst))
        done += 1
    print("%d profiles placed (%d missing) in %d folders" % (done, missing, len(dirs)))
    for d in sorted(dirs)[:5]:
        print("  " + d)


if __name__ == "__main__":
    main()
