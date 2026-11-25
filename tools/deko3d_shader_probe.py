#!/usr/bin/env python3
"""deko3d_shader_probe.py <spirv dir> <out dir>: can the kit's shaders reach deko3d with tools on this PC?

Each SPIR-V file the PC build cached (build/mp2-pc/shadercache, the HLSL that gpu/shaders.cpp and vulkan.cpp generate,
compiled by glslang) goes through SPIRV-Cross (GLSL 4.60, separate images and samplers combined) and then uam,
devkitPro's deko3d shader compiler, into DKSH. Prints how many make it and the first errors: the shader half of a
deko3d back end for the Switch (docs/MP2-notes.md, Decision 4)."""
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CROSS = os.path.join(ROOT, "build", "spirv-cross", "spirv-cross.exe")
UAM = r"C:\devkitPro\tools\bin\uam.exe"


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    ok = cross_fail = uam_fail = 0
    errors = []
    for f in sorted(os.listdir(src)):
        if not f.endswith(".spv"):
            continue
        spv = os.path.join(src, f)
        glsl = os.path.join(out, f[:-4] + ".glsl")
        r = subprocess.run([CROSS, spv, "--version", "460", "--no-es", "--no-420pack-extension",
                            "--combined-samplers-inherit-bindings", "--output", glsl],
                           capture_output=True, text=True)
        if r.returncode:
            cross_fail += 1
            errors.append((f, "spirv-cross", (r.stderr or r.stdout)[:300]))
            continue
        text = open(glsl).read()
        stage = "vert" if "gl_Position" in text else "frag"
        dksh = os.path.join(out, f[:-4] + ".dksh")
        r = subprocess.run([UAM, "-s", stage, "-o", dksh, glsl], capture_output=True, text=True)
        if r.returncode or not os.path.exists(dksh):
            uam_fail += 1
            errors.append((f, "uam " + stage, (r.stderr or r.stdout)[:300]))
            continue
        ok += 1
    total = ok + cross_fail + uam_fail
    print("%d shaders: %d to DKSH, %d failed in SPIRV-Cross, %d failed in uam" % (total, ok, cross_fail, uam_fail))
    for f, where, e in errors[:8]:
        print("  %s (%s): %s" % (f, where, e.strip().replace("\n", " | ")))


if __name__ == "__main__":
    main()
