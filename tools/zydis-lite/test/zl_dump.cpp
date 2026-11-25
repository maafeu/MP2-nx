// zl_dump <raw code file> <base va hex>: a linear sweep with zydis-lite, one line per instruction, for comparing
// with llvm-objdump (tools/zydis-lite/test/compare.py). An undecodable byte prints as "(bad)" with length 1.
#include <Zydis/Zydis.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: zl_dump <file> <base va hex>\n");
        return 1;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    uint32_t base = uint32_t(std::strtoul(argv[2], nullptr, 16));
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
    for (size_t o = 0; o < b.size();) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        size_t avail = b.size() - o < 15 ? b.size() - o : 15;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, b.data() + o, avail, &in, ops))) {
            std::printf("%08X 1 (bad)\n", uint32_t(base + o));
            o++;
            continue;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&fmt, &in, ops, in.operand_count_visible, text, sizeof(text), base + o, nullptr);
        std::printf("%08X %d %s\n", uint32_t(base + o), in.length, text);
        o += in.length;
    }
    return 0;
}
