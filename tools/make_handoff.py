#!/usr/bin/env python3
"""make_handoff.py <base kit dir> <out.patch>: the fixes of this port that are not Max Payne 2's own, as one patch
against the base (COLD-FEAR-NX-PORT), with paths under kit/. Files the base lacks are added whole."""
import difflib
import os
import sys

# generic: not this game's choices (screen.cpp's default, the xinput diagnostic and .gitignore stay out)
FILES = [
    "ferramentas/recompilador/CMakeLists.txt",
    "ferramentas/recompilar.ps1",
    "ferramentas/build_pc.bat",
    "ferramentas/build_switch.sh",
    "tools/zydis-lite/include/Zydis/Zydis.h",
    "tools/zydis-lite/include/Zydis/ZydisMnemonics.def",
    "tools/zydis-lite/include/Zydis/ZydisMnemonics.inc",
    "tools/zydis-lite/src/decoder.cpp",
    "tools/zydis-lite/test/zl_dump.cpp",
    "tools/zydis-lite/test/compare.py",
    "tools/patch_gcs.py",
    "sdk/XboxXBE-Recomp/xdk/5558/layout.toml",
    "sdk/XboxXBE-Recomp/xdk/5558/signatures.toml",
    "sdk/XboxXBE-Recomp/tools/recomp/codegen_int.cpp",
    "sdk/XboxXBE-Recomp/runtime/kernel/boot.cpp",
    "sdk/XboxXBE-Recomp/runtime/gpu/texture.cpp",
    "sdk/XboxXBE-Recomp/runtime/kernel/objects.cpp",
    "sdk/XboxXBE-Recomp/runtime/xdk/bink.cpp",
    "sdk/XboxXBE-Recomp/runtime/xdk/hle.h",
    "sdk/XboxXBE-Recomp/runtime/gpu/vertex.cpp",
    "sdk/XboxXBE-Recomp/runtime/gpu/vertex.h",
    "sdk/XboxXBE-Recomp/harness/reference_renderer.cpp",
    "sdk/XboxXBE-Recomp/harness/reference_renderer.h",
    "host-switch/CMakeLists.txt",
    "host-switch/soft_backend.cpp",
    "host-switch/input.cpp",
    "host-switch/vulkan.cpp",
    "host-switch/vk_shader.cpp",
    "host-switch/gl_backend.cpp",
    "host-switch/gl_shader.cpp",
    "host-switch/gl_shader.h",
    "tools/make_gl_backend.py",
    "tools/rdc_state.py",
    "tools/rdc_save.py",
    "tools/rdc_vsout.py",
    "third_party/README.md",
    "host-pc/CMakeLists.txt",
    "host-pc/soft_pc.cpp",
    "host-pc/input.cpp",
    # 2026-11-24: input.cpp defines TakeMouse (the raw mouse, fed by the port's main.cpp), so the stub goes
    "host-pc/stubs.cpp",
    # 2026-11-22 afternoon, from the console runs (HANDOFF rows 21-28)
    "sdk/XboxXBE-Recomp/tools/recomp/codegen.h",
    "sdk/XboxXBE-Recomp/tools/recomp/codegen_emit.cpp",
    "sdk/XboxXBE-Recomp/tools/recomp/codegen_fp.cpp",
    "sdk/XboxXBE-Recomp/runtime/cpu/dispatch.cpp",
    "sdk/XboxXBE-Recomp/runtime/core/log.cpp",
    "sdk/XboxXBE-Recomp/runtime/core/panel_chords.h",
    "sdk/XboxXBE-Recomp/runtime/kernel/files.cpp",
    "sdk/XboxXBE-Recomp/runtime/kernel/threads.cpp",
    "sdk/XboxXBE-Recomp/runtime/kernel/kernel.h",
    "sdk/XboxXBE-Recomp/runtime/app/app.h",
    "sdk/XboxXBE-Recomp/runtime/gpu/state.h",
    "host-switch/main.cpp",
    "host-switch/clocks.cpp",
    "host-switch/prof.cpp",
    # 2026-11-23: ended guest threads joined by a reaper (HANDOFF row 33); the detach stub's comment says so; the
    # system pool and stacks backed 8 MB at a time (code memory objects run out at 11)
    "host-switch/stubs.cpp",
    "sdk/XboxXBE-Recomp/runtime/kernel/memory.cpp",
    # 2026-11-24: XC_MAX_OS (0xFF) answered with the EEPROM's user section, the time zone the host's (HANDOFF row 34)
    "sdk/XboxXBE-Recomp/runtime/kernel/xboxkrnl.cpp",
    "tools/log_summary.py",
    "tools/prof_report.py",
    # 2026-11-22 evening: the deko3d renderer (HANDOFF row 29; its compiler is third_party/uam, see its README)
    "host-switch/dk_backend.cpp",
    "tools/run_ryujinx.ps1",
    # 2026-11-23: level 27 at 1020 MHz (HANDOFF row 30): the PGO option (CMake and main.cpp name pgo.cpp), the
    # profile and A/B tools, the converted vertex path's bit-exact test
    "host-switch/pgo.cpp",
    "tools/pgo_collect.py",
    "tools/ab_compare.py",
    "tools/prof_lines.py",
    "tools/prof_callers.py",
    "tools/tests/convert_exact.cpp",
    "tools/tests/convert_exact.sh",
    # 2026-11-23 morning: the render thread for deko3d (HANDOFF row 31): Backend::Adopt, the vsc span in State
    "sdk/XboxXBE-Recomp/runtime/gpu/backend.h",
    "sdk/XboxXBE-Recomp/runtime/gpu/render_thread.cpp",
]


def lines(path):
    if not os.path.exists(path):
        return []
    text = open(path, "rb").read().decode("utf-8", "replace").replace("\r\n", "\n")
    return text.splitlines(keepends=True)


def main():
    base, out = sys.argv[1], sys.argv[2]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    chunks = []
    for f in FILES:
        a = lines(os.path.join(base, f))
        b = lines(os.path.join(root, f))
        if a == b:
            continue
        for x in (a, b):
            if x and not x[-1].endswith("\n"):
                x[-1] += "\n"
        old = "/dev/null" if not a else "a/kit/" + f
        new = "b/kit/" + f
        head = "diff --git a/kit/%s b/kit/%s\n" % (f, f)
        if not a:
            head += "new file mode 100644\n"
        d = list(difflib.unified_diff(a, b, old, new, n=3))
        if d:
            chunks.append(head + "".join(d))
    with open(out, "w", encoding="utf-8", newline="\n") as o:
        o.write("".join(chunks))
    print("%s: %d files" % (out, len(chunks)))


if __name__ == "__main__":
    main()
