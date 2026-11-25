#!/usr/bin/env bash
# convert_exact.sh [c++ compiler]: vertex.cpp's ConvertRun against its reference Convert, bit for bit
# (tools/tests/convert_exact.cpp), at -O0, -O2 and -O3. Takes the two functions' lines out of vertex.cpp as they are.
# Default compiler: clang++ on the PATH, else LLVM's in Program Files.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../sdk/XboxXBE-Recomp/runtime/gpu/vertex.cpp"
out=$(mktemp -d)
first=$(grep -n '^constexpr uint32_t kTypeFloat2H' "$src" | cut -d: -f1)
run=$(grep -n '^void ConvertRun' "$src" | cut -d: -f1)
last=$(awk -v b="$run" 'NR > b && /^}/ {print NR; exit}' "$src")
sed -n "${first},${last}p" "$src" > "$out/conv_extract.inc"
cxx=${1:-clang++}
command -v "$cxx" > /dev/null || cxx="/c/Program Files/LLVM/bin/clang++"
status=0
for opt in -O0 -O2 -O3; do
    "$cxx" -std=c++20 $opt -I "$out" "$here/convert_exact.cpp" -o "$out/convert_exact.exe"
    if res=$("$out/convert_exact.exe"); then echo "$opt: $res"; else echo "$opt: $res"; status=1; fi
done
rm -rf "$out"
exit $status
