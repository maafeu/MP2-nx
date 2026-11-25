#!/usr/bin/env python3
"""Compare zl_dump output with llvm-objdump -d -M intel of the same bytes.

usage: compare.py <zl.txt> <llvm.txt> [--show N]

Both are linear sweeps from the section start. Reports where the instruction starts part (lengths differ), and,
where both decode the same bytes, mnemonic and operand differences after spelling normalisation.
"""
import re
import sys
from collections import Counter

ALIAS = {
    "je": "jz", "jne": "jnz", "jae": "jnb", "ja": "jnbe", "jge": "jnl", "jg": "jnle", "jc": "jb", "jnc": "jnb",
    "jnae": "jb", "jna": "jbe", "jnge": "jl", "jng": "jle", "jpe": "jp", "jpo": "jnp",
    "wait": "fwait", "pushal": "pushad", "popal": "popad", "pushfd": "pushfd", "popfd": "popfd",
    "sal": "shl", "xlatb": "xlat", "int1": "int1", "ud1l": "ud1", "ud1": "ud1", "sldt": "sldt",
    "fucompi": "fucomip", "fcompi": "fcomip", "fstp1": "fstp", "fxch4": "fxch", "fxch7": "fxch",
    "fcom2": "fcom", "fcomp3": "fcomp", "fcomp5": "fcomp", "fstp8": "fstp", "fstp9": "fstp",
    "fsetpm": "fnsetpm", "feni8087_nop": "fneni", "fdisi8087_nop": "fndisi", "ffreep": "ffreep",
    "nopw": "nop", "nopl": "nop", "retl": "ret", "lretl": "retf", "lret": "retf", "iretd": "iretd",
}
for cc_l, cc_z in [("e", "z"), ("ne", "nz"), ("ae", "nb"), ("a", "nbe"), ("ge", "nl"), ("g", "nle"),
                   ("c", "b"), ("nae", "b"), ("na", "be"), ("nge", "l"), ("ng", "le"), ("pe", "p"), ("po", "np")]:
    ALIAS["set" + cc_l] = "set" + cc_z
    ALIAS["cmov" + cc_l] = "cmov" + cc_z
    ALIAS["fcmov" + cc_l] = "fcmov" + cc_z
ALIAS["fcmovnae"] = "fcmovb"; ALIAS["fcmovae"] = "fcmovnb"; ALIAS["fcmovna"] = "fcmovbe"; ALIAS["fcmova"] = "fcmovnbe"
ALIAS["fcmove"] = "fcmove"; ALIAS["fcmovne"] = "fcmovne"; ALIAS["fcmovnz"] = "fcmovne"; ALIAS["fcmovz"] = "fcmove"

LLVM_LINE = re.compile(r"^\s*([0-9a-f]+):\s*(.*)$")
STRING_OPS = {m + s for m in ("movs", "stos", "lods", "cmps", "scas", "ins", "outs") for s in "bwd"}
SHIFTS = {"shl", "shr", "sar", "rol", "ror", "rcl", "rcr"}


def same_imms(a, b):
    """the same operands but for immediates written signed in one and unsigned in the other"""
    pa, pb = a.split(","), b.split(",")
    if len(pa) != len(pb):
        return False
    for x, y in zip(pa, pb):
        if x == y:
            continue
        try:
            vx, vy = int(x, 16) if not x.startswith("-") else -int(x[1:], 16), int(y, 16) if not y.startswith("-") else -int(y[1:], 16)
        except ValueError:
            return False
        if (vx - vy) % 256:
            return False
    return True


def norm_ops(s):
    s = re.sub(r"<[^>]*>", "", s).strip().lower()
    s = s.replace(" ", "")
    s = re.sub(r"st\((\d)\)", r"st\1", s)
    s = re.sub(r"(^|,)st(?=,|$)", r"\1st0", s)
    # 4*ecx -> ecx*4
    s = re.sub(r"(\d)\*(e?[a-ds][xip]|e?[sb]p|e?[sd]i)", r"\2*\1", s)
    s = re.sub(r"\*1(?=[\]+-])", "", s)
    s = re.sub(r"0x0*([0-9a-f])", r"0x\1", s)
    s = s.replace("xmmwordptr", "xmmwordptr").replace("tbyteptr", "tbyteptr")
    s = re.sub(r"(ds|ss):\[", "[", s)
    s = s.replace("[+", "[")
    return s


def load_llvm(p):
    out = {}
    for line in open(p, encoding="utf-8", errors="replace"):
        m = LLVM_LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        rest = m.group(2).strip()
        if not rest:
            continue
        parts = rest.split("\t")
        parts = [x for x in parts if x != ""]
        prefix = []
        while parts and parts[0] in ("rep", "repne", "repe", "lock", "repz", "repnz"):
            prefix.append(parts.pop(0))
        if not parts:
            continue
        mn = parts[0].strip()
        ops = parts[1] if len(parts) > 1 else ""
        out[addr] = (mn, ops, prefix)
    return out


def load_zl(p):
    out = {}
    for line in open(p):
        a, ln, rest = line.rstrip("\n").split(" ", 2)
        toks = rest.split(" ", 1)
        prefix = []
        while toks and toks[0] in ("rep", "repne", "repe", "lock"):
            prefix.append(toks[0])
            toks = toks[1].split(" ", 1) if len(toks) > 1 else []
        mn = toks[0] if toks else ""
        ops = toks[1] if len(toks) > 1 else ""
        out[int(a, 16)] = (int(ln), mn, ops, prefix)
    return out


def main():
    zl = load_zl(sys.argv[1])
    ll = load_llvm(sys.argv[2])
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 30
    lla = sorted(ll)
    lens = {a: (lla[i + 1] - a) for i, a in enumerate(lla[:-1])}
    desync = []
    mn_diff = Counter()
    op_diff = Counter()
    examples = {}
    same = 0
    for a, (ln, mn, ops, pre) in zl.items():
        if mn == "(bad)":
            continue
        if a not in ll:
            desync.append((a, mn, ops))
            continue
        lmn, lops, lpre = ll[a]
        if a in lens and lens[a] != ln:
            desync.append((a, mn + " len %d vs llvm %d (%s %s)" % (ln, lens[a], lmn, lops), ops))
            continue
        lm = ALIAS.get(lmn, lmn)
        if lm.startswith("(bad)") or lm == "<unknown>":
            desync.append((a, mn + " llvm:bad", ops))
            continue
        if lm != mn:
            key = (mn, lm)
            mn_diff[key] += 1
            examples.setdefault(key, (a, ops, lops))
            continue
        zo, lo = norm_ops(ops), norm_ops(lops.split("#")[0])
        if mn in STRING_OPS:
            lo = zo  # llvm spells out the implicit es:[edi] / [esi] operands
        if mn in SHIFTS and lo.count(",") == 0 and zo.endswith(",0x1"):
            zo = zo[:-4]
        if zo != lo and same_imms(zo, lo):
            lo = zo
        if zo != lo:
            key = (mn, zo.split(",")[0][:0])
            op_diff[mn] += 1
            examples.setdefault(("ops", mn), (a, zo, lo))
            continue
        same += 1
    print("same %d, start/length differences %d, mnemonic differences %d, operand differences %d" %
          (same, len(desync), sum(mn_diff.values()), sum(op_diff.values())))
    for d in desync[:show]:
        print("  desync %08X %s %s" % d)
    for (m, l), c in mn_diff.most_common(show):
        a, zo, lo = examples[(m, l)]
        print("  mnem %-10s llvm %-10s x%-6d e.g. %08X  %s | %s" % (m, l, c, a, zo, lo))
    for m, c in op_diff.most_common(show):
        a, zo, lo = examples[("ops", m)]
        print("  ops  %-10s x%-6d e.g. %08X  %s | %s" % (m, c, a, zo, lo))
    # llvm starts zl never reached (zl took a different path)
    bad_ll = [a for a in lla if a not in zl]
    print("llvm starts missing in zl: %d" % len(bad_ll))


if __name__ == "__main__":
    main()
