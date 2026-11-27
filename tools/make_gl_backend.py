"""MaxPayne2-nx: keeps host-switch/gl_backend.cpp's copy of vulkan.cpp's console helpers in step with vulkan.cpp.

The OpenGL renderer fills the same constants, keys and palettes from the console's state as the Vulkan one; those
functions live in vulkan.cpp's anonymous namespace, so gl_backend.cpp carries a verbatim copy between the markers
below. Run this after vulkan.cpp changes (python tools/make_gl_backend.py); it rewrites only that block.

First run: host-switch/gl_backend.src.cpp holds the marker line instead of a block; the result is gl_backend.cpp.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VULKAN = ROOT / "host-switch" / "vulkan.cpp"
OUT = ROOT / "host-switch" / "gl_backend.cpp"
SRC = ROOT / "host-switch" / "gl_backend.src.cpp"
MARKER = "//@@VULKAN_HELPERS@@\n"
BEGIN = "// ---- BEGIN vulkan.cpp helpers (tools/make_gl_backend.py copies them; edit vulkan.cpp, not here)\n"
END = "// ---- END vulkan.cpp helpers\n"

# (first line that starts the piece, first line after it) in vulkan.cpp
PIECES = [
    ("// ---- DXT on the CPU", "// ---- device memory"),
    ("struct GuestExtent {", "// ---- layouts:"),
]


def piece(lines, start, stop):
    a = next(i for i, l in enumerate(lines) if l.startswith(start))
    b = next(i for i, l in enumerate(lines) if i > a and l.startswith(stop))
    return lines[a:b]


def main():
    lines = VULKAN.read_text(encoding="utf-8").splitlines(keepends=True)
    block = [BEGIN]
    for start, stop in PIECES:
        block += piece(lines, start, stop)
    block.append(END)
    text = (OUT if OUT.exists() else SRC).read_text(encoding="utf-8")
    if BEGIN in text:
        a = text.index(BEGIN)
        b = text.index(END) + len(END)
        text = text[:a] + "".join(block) + text[b:]
    elif MARKER in text:
        text = text.replace(MARKER, "".join(block))
    else:
        sys.exit("gl_backend: no marker and no block")
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUT.name}: {len(block) - 2} lines of vulkan.cpp helpers")


if __name__ == "__main__":
    main()
