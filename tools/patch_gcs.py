#!/usr/bin/env python3
"""patch_gcs.py <file.elf|file.nro>...: neutralises the AArch64 GCS and SME system-register instructions that
devkitA64's GCC 16 libgcc carries in its unwinder and SME helpers, in place.

  mrs xN, GCSPR_EL0   (S3_3_C2_C5_1)        -> movz xN, #0
  sysl xN, #3, C7, C7, #1  (GCSPOPM)        -> movz xN, #0
  mrs xN, TPIDR2_EL0  (S3_3_C13_C0_5)       -> movz xN, #0
  msr TPIDR2_EL0, xN                        -> nop

On the Switch's Cortex-A57 none of them runs: libgcc checks the feature first (chkfeat / HWCAP) and takes the other
path. Ryujinx's JIT, though, translates whole functions and stops with "Unknown MRS 0xD53B2521" the first time the
unwinder is compiled, before the game starts. The rewrite changes nothing the console executes. Idempotent.
"""
import struct
import sys

MRS, MSR, SYSL = 0xD5300000, 0xD5100000, 0xD5280000


def sysreg(w):
    return (w >> 5) & 0x7FFF  # op0(1 bit)/op1/CRn/CRm/op2 as encoded


def key(op1, crn, crm, op2, o0=1):
    return (o0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2


GCSPR_EL0 = key(3, 2, 5, 1)
TPIDR2_EL0 = key(3, 13, 0, 5)
GCSPOPM = key(3, 7, 7, 1, 0)  # sysl: op0 = 1, so the o0 bit is 0


def patch_words(buf, start, end):
    n = 0
    for o in range(start, end - 3, 4):
        w = struct.unpack_from("<I", buf, o)[0]
        rt = w & 0x1F
        new = None
        if (w & 0xFFF00000) == MRS and sysreg(w) in (GCSPR_EL0, TPIDR2_EL0):
            new = 0xD2800000 | rt
        elif (w & 0xFFF00000) == MSR and sysreg(w) == TPIDR2_EL0:
            new = 0xD503201F
        elif (w & 0xFFF80000) == SYSL and sysreg(w) & 0x3FFF == GCSPOPM:
            new = 0xD2800000 | rt
        if new is not None:
            struct.pack_into("<I", buf, o, new)
            n += 1
    return n


def text_ranges(buf):
    if buf[:4] == b"\x7fELF":
        shoff = struct.unpack_from("<Q", buf, 0x28)[0]
        shentsize, shnum = struct.unpack_from("<HH", buf, 0x3A)
        for i in range(shnum):
            name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", buf, shoff + i * shentsize)
            if flags & 4 and typ == 1:  # SHF_EXECINSTR, PROGBITS
                yield off, off + size
    elif buf[0x10:0x14] == b"NRO0":
        off, size = struct.unpack_from("<II", buf, 0x20)  # the text segment
        yield max(off, 0x80), off + size  # the header sits in the first 0x80 bytes
    else:
        raise SystemExit("not an ELF or NRO")


def main():
    for path in sys.argv[1:]:
        buf = bytearray(open(path, "rb").read())
        n = sum(patch_words(buf, a, b) for a, b in text_ranges(buf))
        if n:
            open(path, "wb").write(buf)
        print("%s: %d instructions rewritten" % (path, n))


if __name__ == "__main__":
    main()
