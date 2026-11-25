// zydis-lite decoder: 32-bit x86 (Pentium III: integer, x87, MMX, SSE; SSE2 for lengths). See include/Zydis/Zydis.h.
#include <Zydis/Zydis.h>

#include <cstdio>
#include <cstring>

namespace {

const char* const kMnemonicNames[] = {
#define X(n) #n,
#include <Zydis/ZydisMnemonics.def>
#undef X
};

char g_lower[ZYDIS_MNEMONIC_MAX_VALUE][16];
const bool g_lower_ready = [] {
    for (int i = 0; i < ZYDIS_MNEMONIC_MAX_VALUE; i++) {
        const char* s = kMnemonicNames[i];
        int j = 0;
        for (; s[j] && j < 15; j++) g_lower[i][j] = char(s[j] >= 'A' && s[j] <= 'Z' ? s[j] + 32 : s[j]);
        g_lower[i][j] = 0;
    }
    return true;
}();

const ZydisAccessedFlags kFlagsNone = {0, 0, 0, 0, 0};
const ZydisAccessedFlags kFlagsTested = {0x8D5, 0, 0, 0, 0};
const ZydisAccessedFlags kFlagsAll = {0x8D5, 0x3F7FD5, 0, 0, 0};

enum RegClass { kGpr8, kGpr16, kGpr32, kMm, kXmm, kSeg, kCr, kDr, kSt };

ZydisRegister Reg(RegClass c, int i) {
    switch (c) {
        case kGpr8: return ZydisRegister(ZYDIS_REGISTER_AL + i);
        case kGpr16: return ZydisRegister(ZYDIS_REGISTER_AX + i);
        case kGpr32: return ZydisRegister(ZYDIS_REGISTER_EAX + i);
        case kMm: return ZydisRegister(ZYDIS_REGISTER_MM0 + i);
        case kXmm: return ZydisRegister(ZYDIS_REGISTER_XMM0 + i);
        case kSeg: return i < 6 ? ZydisRegister(ZYDIS_REGISTER_ES + i) : ZYDIS_REGISTER_NONE;
        case kCr: return ZydisRegister(ZYDIS_REGISTER_CR0 + i);
        case kDr: return ZydisRegister(ZYDIS_REGISTER_DR0 + i);
        case kSt: return ZydisRegister(ZYDIS_REGISTER_ST0 + i);
    }
    return ZYDIS_REGISTER_NONE;
}

uint16_t ClassWidth(RegClass c) {
    switch (c) {
        case kGpr8: return 8;
        case kGpr16: return 16;
        case kGpr32: return 32;
        case kMm: return 64;
        case kXmm: return 128;
        case kSeg: return 16;
        case kCr: case kDr: return 32;
        case kSt: return 80;
    }
    return 0;
}

constexpr uint8_t R = ZYDIS_OPERAND_ACTION_READ;
constexpr uint8_t W = ZYDIS_OPERAND_ACTION_WRITE;
constexpr uint8_t RW = ZYDIS_OPERAND_ACTION_READWRITE;

struct Dec {
    const uint8_t* p;
    size_t avail;
    size_t pos = 0;
    ZydisDecodedInstruction* in;
    ZydisDecodedOperand* ops;
    bool bad = false;
    // prefixes
    bool opsize = false, adsize = false, lock = false;
    int rep = 0;  // 0, 0xF2 or 0xF3, the last one seen
    bool has_f2 = false, has_f3 = false;
    ZydisRegister seg = ZYDIS_REGISTER_NONE;
    // modrm
    bool has_modrm = false;
    uint8_t mod = 0, reg = 0, rm = 0;
    ZydisDecodedOperandMem mem{};
    int nimm = 0;
    bool lockable = false;  // the instruction allows LOCK with a memory destination

    uint8_t Byte() {
        if (pos >= avail || pos >= ZYDIS_MAX_INSTRUCTION_LENGTH) {
            bad = true;
            return 0;
        }
        return p[pos++];
    }
    uint32_t Bytes(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v |= uint32_t(Byte()) << (8 * i);
        return v;
    }
    unsigned Osz() const { return opsize ? 16 : 32; }
    RegClass Gv() const { return opsize ? kGpr16 : kGpr32; }

    ZydisDecodedOperand& NewOp(ZydisOperandVisibility vis, uint8_t act) {
        auto& o = ops[in->operand_count];
        std::memset(&o, 0, sizeof(o));
        o.id = in->operand_count;
        o.visibility = vis;
        o.actions = act;
        in->operand_count++;
        if (vis != ZYDIS_OPERAND_VISIBILITY_HIDDEN) in->operand_count_visible++;
        return o;
    }
    void AddReg(ZydisRegister r, uint16_t size, uint8_t act, ZydisOperandVisibility vis = ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
        auto& o = NewOp(vis, act);
        o.type = ZYDIS_OPERAND_TYPE_REGISTER;
        o.reg.value = r;
        o.size = size;
    }
    void AddRegC(RegClass c, int i, uint8_t act, ZydisOperandVisibility vis = ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
        AddReg(Reg(c, i), ClassWidth(c), act, vis);
    }
    void Hidden(ZydisRegister r, uint8_t act) {
        uint16_t w = r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI ? 32 : 16;
        AddReg(r, w, act, ZYDIS_OPERAND_VISIBILITY_HIDDEN);
    }

    // reads the modrm byte, the sib and the displacement
    void ModRM() {
        if (has_modrm) return;
        has_modrm = true;
        in->attributes |= ZYDIS_ATTRIB_HAS_MODRM;
        in->raw.modrm.offset = uint8_t(pos);
        uint8_t m = Byte();
        mod = m >> 6;
        reg = (m >> 3) & 7;
        rm = m & 7;
        in->raw.modrm.mod = mod;
        in->raw.modrm.reg = reg;
        in->raw.modrm.rm = rm;
        if (mod == 3) return;
        std::memset(&mem, 0, sizeof(mem));
        mem.type = ZYDIS_MEMOP_TYPE_MEM;
        int disp_bytes = 0;
        if (adsize) {
            static const ZydisRegister base16[8] = {ZYDIS_REGISTER_BX, ZYDIS_REGISTER_BX, ZYDIS_REGISTER_BP, ZYDIS_REGISTER_BP,
                                                    ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI, ZYDIS_REGISTER_BP, ZYDIS_REGISTER_BX};
            static const ZydisRegister index16[8] = {ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI, ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI,
                                                     ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE};
            if (mod == 0 && rm == 6) {
                disp_bytes = 2;
            } else {
                mem.base = base16[rm];
                mem.index = index16[rm];
                if (mem.index) mem.scale = 1;
                disp_bytes = mod == 1 ? 1 : mod == 2 ? 2 : 0;
            }
        } else {
            if (rm == 4) {
                in->attributes |= ZYDIS_ATTRIB_HAS_SIB;
                in->raw.sib.offset = uint8_t(pos);
                uint8_t s = Byte();
                uint8_t sc = s >> 6, idx = (s >> 3) & 7, bs = s & 7;
                in->raw.sib.scale = sc;
                in->raw.sib.index = idx;
                in->raw.sib.base = bs;
                if (idx != 4) {
                    mem.index = Reg(kGpr32, idx);
                    mem.scale = uint8_t(1 << sc);
                }
                if (bs == 5 && mod == 0) disp_bytes = 4;
                else mem.base = Reg(kGpr32, bs);
            } else if (mod == 0 && rm == 5) {
                disp_bytes = 4;
            } else {
                mem.base = Reg(kGpr32, rm);
            }
            if (mod == 1) disp_bytes = 1;
            if (mod == 2) disp_bytes = 4;
        }
        if (disp_bytes) {
            in->raw.disp.offset = uint8_t(pos);
            uint32_t v = Bytes(disp_bytes);
            int64_t s = disp_bytes == 1 ? int64_t(int8_t(v)) : disp_bytes == 2 ? int64_t(int16_t(v)) : int64_t(int32_t(v));
            in->raw.disp.value = s;
            in->raw.disp.size = uint8_t(disp_bytes * 8);
            mem.disp.has_displacement = true;
            mem.disp.value = s;
        }
        bool ss = mem.base == ZYDIS_REGISTER_ESP || mem.base == ZYDIS_REGISTER_EBP || mem.base == ZYDIS_REGISTER_BP;
        mem.segment = seg ? seg : ss ? ZYDIS_REGISTER_SS : ZYDIS_REGISTER_DS;
    }

    void AddMem(uint16_t size, uint8_t act, bool agen = false) {
        auto& o = NewOp(ZYDIS_OPERAND_VISIBILITY_EXPLICIT, act);
        o.type = ZYDIS_OPERAND_TYPE_MEMORY;
        o.mem = mem;
        if (agen) o.mem.type = ZYDIS_MEMOP_TYPE_AGEN;
        o.size = size;
    }
    // the r/m operand: a register of class c, or memory of msize bits
    void AddRM(RegClass c, uint16_t msize, uint8_t act) {
        ModRM();
        if (mod == 3) AddRegC(c, rm, act);
        else AddMem(msize, act);
    }
    void AddE(unsigned bits, uint8_t act) {
        AddRM(bits == 8 ? kGpr8 : bits == 16 ? kGpr16 : kGpr32, uint16_t(bits), act);
    }
    void AddG(unsigned bits, uint8_t act) {
        ModRM();
        AddRegC(bits == 8 ? kGpr8 : bits == 16 ? kGpr16 : kGpr32, reg, act);
    }
    bool NeedMem() {
        ModRM();
        if (mod == 3) bad = true;
        return !bad;
    }
    bool NeedReg() {
        ModRM();
        if (mod != 3) bad = true;
        return !bad;
    }
    // moffs: a displacement-only memory operand without modrm
    void AddMoffs(uint16_t size, uint8_t act) {
        std::memset(&mem, 0, sizeof(mem));
        mem.type = ZYDIS_MEMOP_TYPE_MEM;
        int n = adsize ? 2 : 4;
        in->raw.disp.offset = uint8_t(pos);
        uint32_t v = Bytes(n);
        in->raw.disp.value = int64_t(n == 2 ? int64_t(int16_t(v)) : int64_t(int32_t(v)));
        in->raw.disp.size = uint8_t(n * 8);
        mem.disp.has_displacement = true;
        mem.disp.value = n == 2 ? int64_t(uint16_t(v)) : int64_t(int32_t(v));
        mem.segment = seg ? seg : ZYDIS_REGISTER_DS;
        AddMem(size, act);
    }
    // an immediate of n bytes, sign extended when sx, as an operand of opbits
    void AddImm(int n, bool sx, unsigned opbits, bool relative = false,
                ZydisOperandVisibility vis = ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
        uint8_t off = uint8_t(pos);
        uint32_t v = Bytes(n);
        int64_t s = n == 1 ? int64_t(int8_t(v)) : n == 2 ? int64_t(int16_t(v)) : int64_t(int32_t(v));
        uint64_t u = sx || relative ? uint64_t(s) : uint64_t(v);
        if (nimm < 2) {
            auto& r = in->raw.imm[nimm++];
            r.is_signed = sx || relative;
            r.is_relative = relative;
            r.value.u = u;
            r.size = uint8_t(n * 8);
            r.offset = off;
        }
        auto& o = NewOp(vis, R);
        o.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
        o.imm.is_signed = sx || relative;
        o.imm.is_relative = relative;
        o.imm.value.u = u;
        o.size = uint16_t(opbits);
    }
    void AddConst(uint64_t v, unsigned bits) {  // implicit immediate, e.g. the 1 of shl r, 1
        auto& o = NewOp(ZYDIS_OPERAND_VISIBILITY_IMPLICIT, R);
        o.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
        o.imm.value.u = v;
        o.size = uint16_t(bits);
    }

    void Set(ZydisMnemonic m, ZydisInstructionCategory cat = ZYDIS_CATEGORY_OTHER) {
        in->mnemonic = m;
        in->meta.category = cat;
    }

    bool Run();
    bool OneByte(uint8_t op);
    bool TwoByte(uint8_t op);
    bool X87(uint8_t op);
    void Flags();
};

const ZydisMnemonic kAlu[8] = {ZYDIS_MNEMONIC_ADD, ZYDIS_MNEMONIC_OR, ZYDIS_MNEMONIC_ADC, ZYDIS_MNEMONIC_SBB,
                               ZYDIS_MNEMONIC_AND, ZYDIS_MNEMONIC_SUB, ZYDIS_MNEMONIC_XOR, ZYDIS_MNEMONIC_CMP};
const ZydisMnemonic kShift[8] = {ZYDIS_MNEMONIC_ROL, ZYDIS_MNEMONIC_ROR, ZYDIS_MNEMONIC_RCL, ZYDIS_MNEMONIC_RCR,
                                 ZYDIS_MNEMONIC_SHL, ZYDIS_MNEMONIC_SHR, ZYDIS_MNEMONIC_SHL, ZYDIS_MNEMONIC_SAR};
const ZydisMnemonic kJcc[16] = {ZYDIS_MNEMONIC_JO, ZYDIS_MNEMONIC_JNO, ZYDIS_MNEMONIC_JB, ZYDIS_MNEMONIC_JNB,
                                ZYDIS_MNEMONIC_JZ, ZYDIS_MNEMONIC_JNZ, ZYDIS_MNEMONIC_JBE, ZYDIS_MNEMONIC_JNBE,
                                ZYDIS_MNEMONIC_JS, ZYDIS_MNEMONIC_JNS, ZYDIS_MNEMONIC_JP, ZYDIS_MNEMONIC_JNP,
                                ZYDIS_MNEMONIC_JL, ZYDIS_MNEMONIC_JNL, ZYDIS_MNEMONIC_JLE, ZYDIS_MNEMONIC_JNLE};
const ZydisMnemonic kSetcc[16] = {ZYDIS_MNEMONIC_SETO, ZYDIS_MNEMONIC_SETNO, ZYDIS_MNEMONIC_SETB, ZYDIS_MNEMONIC_SETNB,
                                  ZYDIS_MNEMONIC_SETZ, ZYDIS_MNEMONIC_SETNZ, ZYDIS_MNEMONIC_SETBE, ZYDIS_MNEMONIC_SETNBE,
                                  ZYDIS_MNEMONIC_SETS, ZYDIS_MNEMONIC_SETNS, ZYDIS_MNEMONIC_SETP, ZYDIS_MNEMONIC_SETNP,
                                  ZYDIS_MNEMONIC_SETL, ZYDIS_MNEMONIC_SETNL, ZYDIS_MNEMONIC_SETLE, ZYDIS_MNEMONIC_SETNLE};
const ZydisMnemonic kCmov[16] = {ZYDIS_MNEMONIC_CMOVO, ZYDIS_MNEMONIC_CMOVNO, ZYDIS_MNEMONIC_CMOVB, ZYDIS_MNEMONIC_CMOVNB,
                                 ZYDIS_MNEMONIC_CMOVZ, ZYDIS_MNEMONIC_CMOVNZ, ZYDIS_MNEMONIC_CMOVBE, ZYDIS_MNEMONIC_CMOVNBE,
                                 ZYDIS_MNEMONIC_CMOVS, ZYDIS_MNEMONIC_CMOVNS, ZYDIS_MNEMONIC_CMOVP, ZYDIS_MNEMONIC_CMOVNP,
                                 ZYDIS_MNEMONIC_CMOVL, ZYDIS_MNEMONIC_CMOVNL, ZYDIS_MNEMONIC_CMOVLE, ZYDIS_MNEMONIC_CMOVNLE};

bool Dec::Run() {
    in->machine_mode = ZYDIS_MACHINE_MODE_LEGACY_32;
    in->stack_width = 32;
    // prefixes
    for (;;) {
        if (pos >= avail) return false;
        uint8_t b = p[pos];
        switch (b) {
            case 0x66: opsize = true; break;
            case 0x67: adsize = true; break;
            case 0xF0: lock = true; break;
            case 0xF2: rep = 0xF2; has_f2 = true; break;
            case 0xF3: rep = 0xF3; has_f3 = true; break;
            case 0x26: seg = ZYDIS_REGISTER_ES; break;
            case 0x2E: seg = ZYDIS_REGISTER_CS; break;
            case 0x36: seg = ZYDIS_REGISTER_SS; break;
            case 0x3E: seg = ZYDIS_REGISTER_DS; break;
            case 0x64: seg = ZYDIS_REGISTER_FS; break;
            case 0x65: seg = ZYDIS_REGISTER_GS; break;
            default: goto done;
        }
        pos++;
        in->raw.prefix_count++;
        if (pos >= ZYDIS_MAX_INSTRUCTION_LENGTH) return false;
    }
done:
    in->operand_width = uint8_t(Osz());
    in->address_width = adsize ? 16 : 32;
    if (opsize) in->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
    if (adsize) in->attributes |= ZYDIS_ATTRIB_HAS_ADDRESSSIZE;
    if (seg) in->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT;
    uint8_t op = Byte();
    if (bad) return false;
    bool ok;
    if (op == 0x0F) {
        in->opcode_map = 1;
        uint8_t op2 = Byte();
        if (bad) return false;
        in->opcode = op2;
        ok = TwoByte(op2);
    } else {
        in->opcode = op;
        ok = OneByte(op);
    }
    if (!ok || bad || in->mnemonic == ZYDIS_MNEMONIC_INVALID) return false;
    if (lock) {
        bool mem_dst = in->operand_count && ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY;
        if (!lockable || !mem_dst) return false;
        in->attributes |= ZYDIS_ATTRIB_HAS_LOCK;
    }
    if (in->meta.category == ZYDIS_CATEGORY_STRINGOP && rep) {
        auto m = in->mnemonic;
        bool cmp = m == ZYDIS_MNEMONIC_CMPSB || m == ZYDIS_MNEMONIC_CMPSW || m == ZYDIS_MNEMONIC_CMPSD ||
                   m == ZYDIS_MNEMONIC_SCASB || m == ZYDIS_MNEMONIC_SCASW || m == ZYDIS_MNEMONIC_SCASD;
        if (rep == 0xF2) in->attributes |= ZYDIS_ATTRIB_HAS_REPNE;
        else in->attributes |= cmp ? ZYDIS_ATTRIB_HAS_REPE : ZYDIS_ATTRIB_HAS_REP;
    }
    in->length = uint8_t(pos);
    Flags();
    return true;
}

void Dec::Flags() {
    switch (in->mnemonic) {
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB:
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_CMP:
        case ZYDIS_MNEMONIC_TEST: case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG:
        case ZYDIS_MNEMONIC_MUL: case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV:
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL:
        case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR: case ZYDIS_MNEMONIC_SHLD:
        case ZYDIS_MNEMONIC_SHRD: case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR:
        case ZYDIS_MNEMONIC_BTC: case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_CMPXCHG:
        case ZYDIS_MNEMONIC_CMPXCHG8B: case ZYDIS_MNEMONIC_XADD: case ZYDIS_MNEMONIC_DAA: case ZYDIS_MNEMONIC_DAS:
        case ZYDIS_MNEMONIC_AAA: case ZYDIS_MNEMONIC_AAS: case ZYDIS_MNEMONIC_AAM: case ZYDIS_MNEMONIC_AAD:
        case ZYDIS_MNEMONIC_ARPL: case ZYDIS_MNEMONIC_LAR: case ZYDIS_MNEMONIC_LSL: case ZYDIS_MNEMONIC_VERR:
        case ZYDIS_MNEMONIC_VERW: case ZYDIS_MNEMONIC_SAHF: case ZYDIS_MNEMONIC_POPF: case ZYDIS_MNEMONIC_POPFD:
        case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC: case ZYDIS_MNEMONIC_CLD:
        case ZYDIS_MNEMONIC_STD: case ZYDIS_MNEMONIC_CLI: case ZYDIS_MNEMONIC_STI: case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD: case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS:
        case ZYDIS_MNEMONIC_COMISD: case ZYDIS_MNEMONIC_UCOMISD: case ZYDIS_MNEMONIC_FCOMI:
        case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD: case ZYDIS_MNEMONIC_SCASB:
        case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: case ZYDIS_MNEMONIC_INT: case ZYDIS_MNEMONIC_INT1:
        case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_INTO: case ZYDIS_MNEMONIC_RSM: case ZYDIS_MNEMONIC_SYSENTER:
        case ZYDIS_MNEMONIC_SYSEXIT: case ZYDIS_MNEMONIC_SALC: case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_RET:
        case ZYDIS_MNEMONIC_RETF:
            in->cpu_flags = &kFlagsAll;
            // a cmpsd with F2/F3 0F is SSE2's: no flags (cmpsd the string op keeps them, decided below)
            if (in->mnemonic == ZYDIS_MNEMONIC_CMPSD && in->meta.category != ZYDIS_CATEGORY_STRINGOP) in->cpu_flags = &kFlagsNone;
            return;
        default:
            break;
    }
    switch (in->meta.category) {
        case ZYDIS_CATEGORY_COND_BR:
            in->cpu_flags = &kFlagsTested;
            return;
        default:
            break;
    }
    for (int i = 0; i < 16; i++)
        if (in->mnemonic == kSetcc[i] || in->mnemonic == kCmov[i]) {
            in->cpu_flags = &kFlagsTested;
            return;
        }
    switch (in->mnemonic) {
        case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVNB:
        case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNU: case ZYDIS_MNEMONIC_FCMOVU:
        case ZYDIS_MNEMONIC_LAHF: case ZYDIS_MNEMONIC_PUSHF: case ZYDIS_MNEMONIC_PUSHFD:
            in->cpu_flags = &kFlagsTested;
            return;
        default:
            in->cpu_flags = &kFlagsNone;
    }
}

bool Dec::OneByte(uint8_t op) {
    unsigned v = Osz();
    RegClass gv = Gv();
    // the eight ALU rows: 00-05, 08-0D, ... 38-3D
    if (op < 0x40 && (op & 7) < 6) {
        ZydisMnemonic m = kAlu[op >> 3];
        bool cmp = m == ZYDIS_MNEMONIC_CMP;
        uint8_t dst = cmp ? R : RW;
        lockable = !cmp;
        Set(m, ZYDIS_CATEGORY_BINARY);
        switch (op & 7) {
            case 0: AddE(8, dst); AddG(8, R); break;
            case 1: AddE(v, dst); AddG(v, R); break;
            case 2: AddG(8, dst); AddE(8, R); break;
            case 3: AddG(v, dst); AddE(v, R); break;
            case 4: AddRegC(kGpr8, 0, dst, ZYDIS_OPERAND_VISIBILITY_IMPLICIT); AddImm(1, false, 8); break;
            case 5: AddRegC(gv, 0, dst, ZYDIS_OPERAND_VISIBILITY_IMPLICIT); AddImm(opsize ? 2 : 4, false, v); break;
        }
        return true;
    }
    if (op >= 0x40 && op < 0x50) {
        Set(op < 0x48 ? ZYDIS_MNEMONIC_INC : ZYDIS_MNEMONIC_DEC, ZYDIS_CATEGORY_BINARY);
        AddRegC(gv, op & 7, RW);
        return true;
    }
    if (op >= 0x50 && op < 0x60) {
        bool push = op < 0x58;
        Set(push ? ZYDIS_MNEMONIC_PUSH : ZYDIS_MNEMONIC_POP, push ? ZYDIS_CATEGORY_PUSH : ZYDIS_CATEGORY_POP);
        AddRegC(gv, op & 7, push ? R : W);
        Hidden(ZYDIS_REGISTER_ESP, RW);
        return true;
    }
    if (op >= 0x70 && op < 0x80) {
        Set(kJcc[op & 15], ZYDIS_CATEGORY_COND_BR);
        AddImm(1, true, 32, true);
        return true;
    }
    if (op >= 0x91 && op <= 0x97) {
        Set(ZYDIS_MNEMONIC_XCHG, ZYDIS_CATEGORY_DATAXFER);
        AddRegC(gv, op & 7, RW);
        AddRegC(gv, 0, RW, ZYDIS_OPERAND_VISIBILITY_IMPLICIT);
        return true;
    }
    if (op >= 0xB0 && op <= 0xB7) {
        Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER);
        AddRegC(kGpr8, op & 7, W);
        AddImm(1, false, 8);
        return true;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER);
        AddRegC(gv, op & 7, W);
        AddImm(opsize ? 2 : 4, false, v);
        return true;
    }
    if (op >= 0xD8 && op <= 0xDF) return X87(op);

    switch (op) {
        case 0x06: case 0x0E: case 0x16: case 0x1E:
            Set(ZYDIS_MNEMONIC_PUSH, ZYDIS_CATEGORY_PUSH);
            AddReg(Reg(kSeg, op >> 3), 32, R);  // pushes a dword in 32-bit code
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0x07: case 0x17: case 0x1F:
            Set(ZYDIS_MNEMONIC_POP, ZYDIS_CATEGORY_POP);
            AddReg(Reg(kSeg, op >> 3), 32, W);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0x27: Set(ZYDIS_MNEMONIC_DAA); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0x2F: Set(ZYDIS_MNEMONIC_DAS); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0x37: Set(ZYDIS_MNEMONIC_AAA); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0x3F: Set(ZYDIS_MNEMONIC_AAS); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0x60:
        case 0x61: {
            bool push = op == 0x60;
            Set(push ? (opsize ? ZYDIS_MNEMONIC_PUSHA : ZYDIS_MNEMONIC_PUSHAD) : (opsize ? ZYDIS_MNEMONIC_POPA : ZYDIS_MNEMONIC_POPAD),
                push ? ZYDIS_CATEGORY_PUSH : ZYDIS_CATEGORY_POP);
            for (int i = 0; i < 8; i++)
                if (i != 4) Hidden(Reg(kGpr32, i), push ? R : W);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        }
        case 0x62:
            Set(ZYDIS_MNEMONIC_BOUND);
            ModRM();
            if (!NeedMem()) return false;
            AddG(v, R);
            AddMem(uint16_t(2 * v), R);
            return true;
        case 0x63:
            Set(ZYDIS_MNEMONIC_ARPL);
            AddE(16, RW);
            AddG(16, R);
            return true;
        case 0x68:
            Set(ZYDIS_MNEMONIC_PUSH, ZYDIS_CATEGORY_PUSH);
            AddImm(opsize ? 2 : 4, false, v);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0x6A:
            Set(ZYDIS_MNEMONIC_PUSH, ZYDIS_CATEGORY_PUSH);
            AddImm(1, true, v);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0x69:
        case 0x6B:
            Set(ZYDIS_MNEMONIC_IMUL, ZYDIS_CATEGORY_BINARY);
            AddG(v, W);
            AddE(v, R);
            if (op == 0x69) AddImm(opsize ? 2 : 4, true, v);
            else AddImm(1, true, v);
            return true;
        case 0x6C: Set(ZYDIS_MNEMONIC_INSB, ZYDIS_CATEGORY_STRINGOP); Hidden(ZYDIS_REGISTER_EDI, RW); Hidden(ZYDIS_REGISTER_DX, R); return true;
        case 0x6D: Set(opsize ? ZYDIS_MNEMONIC_INSW : ZYDIS_MNEMONIC_INSD, ZYDIS_CATEGORY_STRINGOP); Hidden(ZYDIS_REGISTER_EDI, RW); Hidden(ZYDIS_REGISTER_DX, R); return true;
        case 0x6E: Set(ZYDIS_MNEMONIC_OUTSB, ZYDIS_CATEGORY_STRINGOP); Hidden(ZYDIS_REGISTER_ESI, RW); Hidden(ZYDIS_REGISTER_DX, R); return true;
        case 0x6F: Set(opsize ? ZYDIS_MNEMONIC_OUTSW : ZYDIS_MNEMONIC_OUTSD, ZYDIS_CATEGORY_STRINGOP); Hidden(ZYDIS_REGISTER_ESI, RW); Hidden(ZYDIS_REGISTER_DX, R); return true;
        case 0x80: case 0x81: case 0x82: case 0x83: {
            ModRM();
            ZydisMnemonic m = kAlu[reg];
            bool cmp = m == ZYDIS_MNEMONIC_CMP;
            lockable = !cmp;
            Set(m, ZYDIS_CATEGORY_BINARY);
            unsigned w = op == 0x80 || op == 0x82 ? 8 : v;
            AddE(w, cmp ? R : RW);
            if (op == 0x81) AddImm(opsize ? 2 : 4, false, w);
            else if (op == 0x83) AddImm(1, true, w);
            else AddImm(1, false, 8);
            return true;
        }
        case 0x84: case 0x85:
            Set(ZYDIS_MNEMONIC_TEST, ZYDIS_CATEGORY_LOGICAL);
            AddE(op == 0x84 ? 8 : v, R);
            AddG(op == 0x84 ? 8 : v, R);
            return true;
        case 0x86: case 0x87:
            Set(ZYDIS_MNEMONIC_XCHG, ZYDIS_CATEGORY_DATAXFER);
            lockable = true;
            AddE(op == 0x86 ? 8 : v, RW);
            AddG(op == 0x86 ? 8 : v, RW);
            return true;
        case 0x88: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddE(8, W); AddG(8, R); return true;
        case 0x89: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddE(v, W); AddG(v, R); return true;
        case 0x8A: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddG(8, W); AddE(8, R); return true;
        case 0x8B: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddG(v, W); AddE(v, R); return true;
        case 0x8C:
            Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER);
            ModRM();
            if (reg > 5) return false;
            if (mod == 3) AddRegC(gv, rm, W);
            else AddMem(16, W);
            AddRegC(kSeg, reg, R);
            return true;
        case 0x8D:
            Set(ZYDIS_MNEMONIC_LEA, ZYDIS_CATEGORY_MISC);
            ModRM();
            if (!NeedMem()) return false;
            AddG(v, W);
            AddMem(uint16_t(v), 0, true);
            return true;
        case 0x8E:
            Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER);
            ModRM();
            if (reg > 5 || reg == 1) return false;
            AddRegC(kSeg, reg, W);
            AddE(16, R);
            return true;
        case 0x8F:
            ModRM();
            if (reg != 0) return false;
            Set(ZYDIS_MNEMONIC_POP, ZYDIS_CATEGORY_POP);
            AddE(v, W);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0x90:
            if (rep == 0xF3) Set(ZYDIS_MNEMONIC_PAUSE);
            else Set(ZYDIS_MNEMONIC_NOP, ZYDIS_CATEGORY_NOP);
            return true;
        case 0x98: Set(opsize ? ZYDIS_MNEMONIC_CBW : ZYDIS_MNEMONIC_CWDE, ZYDIS_CATEGORY_CONVERT); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0x99:
            Set(opsize ? ZYDIS_MNEMONIC_CWD : ZYDIS_MNEMONIC_CDQ, ZYDIS_CATEGORY_CONVERT);
            Hidden(ZYDIS_REGISTER_EAX, R);
            Hidden(ZYDIS_REGISTER_EDX, W);
            return true;
        case 0x9A:
        case 0xEA: {
            Set(ZYDIS_MNEMONIC_CALL, op == 0x9A ? ZYDIS_CATEGORY_CALL : ZYDIS_CATEGORY_UNCOND_BR);
            if (op == 0xEA) in->mnemonic = ZYDIS_MNEMONIC_JMP;
            uint8_t off = uint8_t(pos);
            uint32_t o32 = Bytes(opsize ? 2 : 4);
            uint8_t off2 = uint8_t(pos);
            uint16_t s16 = uint16_t(Bytes(2));
            in->raw.imm[0] = {false, false, {o32}, uint8_t(opsize ? 16 : 32), off};
            in->raw.imm[1] = {false, false, {s16}, 16, off2};
            nimm = 2;
            auto& o = NewOp(ZYDIS_OPERAND_VISIBILITY_EXPLICIT, R);
            o.type = ZYDIS_OPERAND_TYPE_POINTER;
            o.ptr.segment = s16;
            o.ptr.offset = o32;
            o.size = uint16_t(v + 16);
            return true;
        }
        case 0x9B: Set(ZYDIS_MNEMONIC_FWAIT); return true;
        case 0x9C: Set(opsize ? ZYDIS_MNEMONIC_PUSHF : ZYDIS_MNEMONIC_PUSHFD, ZYDIS_CATEGORY_PUSH); Hidden(ZYDIS_REGISTER_ESP, RW); return true;
        case 0x9D: Set(opsize ? ZYDIS_MNEMONIC_POPF : ZYDIS_MNEMONIC_POPFD, ZYDIS_CATEGORY_POP); Hidden(ZYDIS_REGISTER_ESP, RW); return true;
        case 0x9E: Set(ZYDIS_MNEMONIC_SAHF, ZYDIS_CATEGORY_FLAGOP); Hidden(ZYDIS_REGISTER_EAX, R); return true;
        case 0x9F: Set(ZYDIS_MNEMONIC_LAHF, ZYDIS_CATEGORY_FLAGOP); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0xA0: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddRegC(kGpr8, 0, W); AddMoffs(8, R); return true;
        case 0xA1: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddRegC(gv, 0, W); AddMoffs(uint16_t(v), R); return true;
        case 0xA2: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddMoffs(8, W); AddRegC(kGpr8, 0, R); return true;
        case 0xA3: Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER); AddMoffs(uint16_t(v), W); AddRegC(gv, 0, R); return true;
        case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
            static const ZydisMnemonic b[] = {ZYDIS_MNEMONIC_MOVSB, ZYDIS_MNEMONIC_CMPSB, ZYDIS_MNEMONIC_INVALID,
                                              ZYDIS_MNEMONIC_STOSB, ZYDIS_MNEMONIC_LODSB, ZYDIS_MNEMONIC_SCASB};
            static const ZydisMnemonic w16[] = {ZYDIS_MNEMONIC_MOVSW, ZYDIS_MNEMONIC_CMPSW, ZYDIS_MNEMONIC_INVALID,
                                                ZYDIS_MNEMONIC_STOSW, ZYDIS_MNEMONIC_LODSW, ZYDIS_MNEMONIC_SCASW};
            static const ZydisMnemonic d32[] = {ZYDIS_MNEMONIC_MOVSD, ZYDIS_MNEMONIC_CMPSD, ZYDIS_MNEMONIC_INVALID,
                                                ZYDIS_MNEMONIC_STOSD, ZYDIS_MNEMONIC_LODSD, ZYDIS_MNEMONIC_SCASD};
            int k = (op - 0xA4) >> 1;
            bool wide = op & 1;
            Set(!wide ? b[k] : opsize ? w16[k] : d32[k], ZYDIS_CATEGORY_STRINGOP);
            bool uses_si = k == 0 || k == 1 || k == 4, uses_di = k != 4, uses_ax = k >= 3;
            if (uses_si) Hidden(ZYDIS_REGISTER_ESI, RW);
            if (uses_di) Hidden(ZYDIS_REGISTER_EDI, RW);
            if (uses_ax) Hidden(ZYDIS_REGISTER_EAX, k == 4 ? W : R);
            if (rep) Hidden(ZYDIS_REGISTER_ECX, RW);
            return true;
        }
        case 0xA8: Set(ZYDIS_MNEMONIC_TEST, ZYDIS_CATEGORY_LOGICAL); AddRegC(kGpr8, 0, R, ZYDIS_OPERAND_VISIBILITY_IMPLICIT); AddImm(1, false, 8); return true;
        case 0xA9: Set(ZYDIS_MNEMONIC_TEST, ZYDIS_CATEGORY_LOGICAL); AddRegC(gv, 0, R, ZYDIS_OPERAND_VISIBILITY_IMPLICIT); AddImm(opsize ? 2 : 4, false, v); return true;
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            ModRM();
            Set(kShift[reg], ZYDIS_CATEGORY_SHIFT);
            unsigned w = (op & 1) ? v : 8;
            AddE(w, RW);
            if (op <= 0xC1) AddImm(1, false, 8);
            else if (op <= 0xD1) AddConst(1, 8);
            else AddRegC(kGpr8, 1, R);  // cl
            return true;
        }
        case 0xC2:
            Set(ZYDIS_MNEMONIC_RET, ZYDIS_CATEGORY_RET);
            AddImm(2, false, 16);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0xC3: Set(ZYDIS_MNEMONIC_RET, ZYDIS_CATEGORY_RET); Hidden(ZYDIS_REGISTER_ESP, RW); return true;
        case 0xC4: case 0xC5:
            Set(op == 0xC4 ? ZYDIS_MNEMONIC_LES : ZYDIS_MNEMONIC_LDS);
            if (!NeedMem()) return false;
            AddG(v, W);
            AddMem(uint16_t(v + 16), R);
            return true;
        case 0xC6: case 0xC7:
            ModRM();
            if (reg != 0) return false;
            Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_DATAXFER);
            if (op == 0xC6) { AddE(8, W); AddImm(1, false, 8); }
            else { AddE(v, W); AddImm(opsize ? 2 : 4, false, v); }
            return true;
        case 0xC8:
            Set(ZYDIS_MNEMONIC_ENTER, ZYDIS_CATEGORY_MISC);
            AddImm(2, false, 16);
            AddImm(1, false, 8);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            Hidden(ZYDIS_REGISTER_EBP, RW);
            return true;
        case 0xC9: Set(ZYDIS_MNEMONIC_LEAVE, ZYDIS_CATEGORY_MISC); Hidden(ZYDIS_REGISTER_ESP, RW); Hidden(ZYDIS_REGISTER_EBP, RW); return true;
        case 0xCA: Set(ZYDIS_MNEMONIC_RETF, ZYDIS_CATEGORY_RET); AddImm(2, false, 16); return true;
        case 0xCB: Set(ZYDIS_MNEMONIC_RETF, ZYDIS_CATEGORY_RET); return true;
        case 0xCC: Set(ZYDIS_MNEMONIC_INT3, ZYDIS_CATEGORY_INTERRUPT); return true;
        case 0xCD: Set(ZYDIS_MNEMONIC_INT, ZYDIS_CATEGORY_INTERRUPT); AddImm(1, false, 8); return true;
        case 0xCE: Set(ZYDIS_MNEMONIC_INTO, ZYDIS_CATEGORY_INTERRUPT); return true;
        case 0xCF: Set(opsize ? ZYDIS_MNEMONIC_IRET : ZYDIS_MNEMONIC_IRETD, ZYDIS_CATEGORY_RET); return true;
        case 0xD4: Set(ZYDIS_MNEMONIC_AAM); AddImm(1, false, 8); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0xD5: Set(ZYDIS_MNEMONIC_AAD); AddImm(1, false, 8); Hidden(ZYDIS_REGISTER_EAX, RW); return true;
        case 0xD6: Set(ZYDIS_MNEMONIC_SALC); Hidden(ZYDIS_REGISTER_EAX, W); return true;
        case 0xD7: Set(ZYDIS_MNEMONIC_XLAT); Hidden(ZYDIS_REGISTER_EAX, RW); Hidden(ZYDIS_REGISTER_EBX, R); return true;
        case 0xE0: case 0xE1: case 0xE2: {
            static const ZydisMnemonic l[] = {ZYDIS_MNEMONIC_LOOPNE, ZYDIS_MNEMONIC_LOOPE, ZYDIS_MNEMONIC_LOOP};
            Set(l[op - 0xE0], ZYDIS_CATEGORY_COND_BR);
            AddImm(1, true, 32, true);
            Hidden(ZYDIS_REGISTER_ECX, RW);
            return true;
        }
        case 0xE3:
            Set(adsize ? ZYDIS_MNEMONIC_JCXZ : ZYDIS_MNEMONIC_JECXZ, ZYDIS_CATEGORY_COND_BR);
            AddImm(1, true, 32, true);
            Hidden(ZYDIS_REGISTER_ECX, R);
            return true;
        case 0xE4: Set(ZYDIS_MNEMONIC_IN, ZYDIS_CATEGORY_IO); AddRegC(kGpr8, 0, W); AddImm(1, false, 8); return true;
        case 0xE5: Set(ZYDIS_MNEMONIC_IN, ZYDIS_CATEGORY_IO); AddRegC(gv, 0, W); AddImm(1, false, 8); return true;
        case 0xE6: Set(ZYDIS_MNEMONIC_OUT, ZYDIS_CATEGORY_IO); AddImm(1, false, 8); AddRegC(kGpr8, 0, R); return true;
        case 0xE7: Set(ZYDIS_MNEMONIC_OUT, ZYDIS_CATEGORY_IO); AddImm(1, false, 8); AddRegC(gv, 0, R); return true;
        case 0xE8:
            Set(ZYDIS_MNEMONIC_CALL, ZYDIS_CATEGORY_CALL);
            AddImm(opsize ? 2 : 4, true, 32, true);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0xE9:
            Set(ZYDIS_MNEMONIC_JMP, ZYDIS_CATEGORY_UNCOND_BR);
            AddImm(opsize ? 2 : 4, true, 32, true);
            return true;
        case 0xEB:
            Set(ZYDIS_MNEMONIC_JMP, ZYDIS_CATEGORY_UNCOND_BR);
            AddImm(1, true, 32, true);
            return true;
        case 0xEC: Set(ZYDIS_MNEMONIC_IN, ZYDIS_CATEGORY_IO); AddRegC(kGpr8, 0, W); AddReg(ZYDIS_REGISTER_DX, 16, R); return true;
        case 0xED: Set(ZYDIS_MNEMONIC_IN, ZYDIS_CATEGORY_IO); AddRegC(gv, 0, W); AddReg(ZYDIS_REGISTER_DX, 16, R); return true;
        case 0xEE: Set(ZYDIS_MNEMONIC_OUT, ZYDIS_CATEGORY_IO); AddReg(ZYDIS_REGISTER_DX, 16, R); AddRegC(kGpr8, 0, R); return true;
        case 0xEF: Set(ZYDIS_MNEMONIC_OUT, ZYDIS_CATEGORY_IO); AddReg(ZYDIS_REGISTER_DX, 16, R); AddRegC(gv, 0, R); return true;
        case 0xF1: Set(ZYDIS_MNEMONIC_INT1, ZYDIS_CATEGORY_INTERRUPT); return true;
        case 0xF4: Set(ZYDIS_MNEMONIC_HLT, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0xF5: Set(ZYDIS_MNEMONIC_CMC, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xF6: case 0xF7: {
            ModRM();
            unsigned w = op == 0xF6 ? 8 : v;
            switch (reg) {
                case 0: case 1:
                    Set(ZYDIS_MNEMONIC_TEST, ZYDIS_CATEGORY_LOGICAL);
                    AddE(w, R);
                    AddImm(w == 8 ? 1 : opsize ? 2 : 4, false, w);
                    return true;
                case 2: Set(ZYDIS_MNEMONIC_NOT, ZYDIS_CATEGORY_LOGICAL); lockable = true; AddE(w, RW); return true;
                case 3: Set(ZYDIS_MNEMONIC_NEG, ZYDIS_CATEGORY_BINARY); lockable = true; AddE(w, RW); return true;
                default: {
                    static const ZydisMnemonic md[] = {ZYDIS_MNEMONIC_MUL, ZYDIS_MNEMONIC_IMUL, ZYDIS_MNEMONIC_DIV, ZYDIS_MNEMONIC_IDIV};
                    Set(md[reg - 4], ZYDIS_CATEGORY_BINARY);
                    AddE(w, R);
                    Hidden(ZYDIS_REGISTER_EAX, RW);
                    if (w != 8) Hidden(ZYDIS_REGISTER_EDX, reg >= 6 ? RW : W);
                    return true;
                }
            }
        }
        case 0xF8: Set(ZYDIS_MNEMONIC_CLC, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xF9: Set(ZYDIS_MNEMONIC_STC, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xFA: Set(ZYDIS_MNEMONIC_CLI, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xFB: Set(ZYDIS_MNEMONIC_STI, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xFC: Set(ZYDIS_MNEMONIC_CLD, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xFD: Set(ZYDIS_MNEMONIC_STD, ZYDIS_CATEGORY_FLAGOP); return true;
        case 0xFE:
            ModRM();
            if (reg > 1) return false;
            Set(reg ? ZYDIS_MNEMONIC_DEC : ZYDIS_MNEMONIC_INC, ZYDIS_CATEGORY_BINARY);
            lockable = true;
            AddE(8, RW);
            return true;
        case 0xFF:
            ModRM();
            switch (reg) {
                case 0: case 1:
                    Set(reg ? ZYDIS_MNEMONIC_DEC : ZYDIS_MNEMONIC_INC, ZYDIS_CATEGORY_BINARY);
                    lockable = true;
                    AddE(v, RW);
                    return true;
                case 2:
                    Set(ZYDIS_MNEMONIC_CALL, ZYDIS_CATEGORY_CALL);
                    AddE(v, R);
                    Hidden(ZYDIS_REGISTER_ESP, RW);
                    return true;
                case 3:
                    Set(ZYDIS_MNEMONIC_CALL, ZYDIS_CATEGORY_CALL);
                    if (!NeedMem()) return false;
                    AddMem(uint16_t(v + 16), R);
                    Hidden(ZYDIS_REGISTER_ESP, RW);
                    return true;
                case 4:
                    Set(ZYDIS_MNEMONIC_JMP, ZYDIS_CATEGORY_UNCOND_BR);
                    AddE(v, R);
                    return true;
                case 5:
                    Set(ZYDIS_MNEMONIC_JMP, ZYDIS_CATEGORY_UNCOND_BR);
                    if (!NeedMem()) return false;
                    AddMem(uint16_t(v + 16), R);
                    return true;
                case 6:
                    Set(ZYDIS_MNEMONIC_PUSH, ZYDIS_CATEGORY_PUSH);
                    AddE(v, R);
                    Hidden(ZYDIS_REGISTER_ESP, RW);
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

bool Dec::X87(uint8_t op) {
    ModRM();
    if (bad) return false;
    int i = rm;
    auto st = [&](int k, uint8_t act, ZydisOperandVisibility vis = ZYDIS_OPERAND_VISIBILITY_EXPLICIT) {
        AddRegC(kSt, k, act, vis);
    };
    constexpr auto IMP = ZYDIS_OPERAND_VISIBILITY_IMPLICIT;
    constexpr auto HID = ZYDIS_OPERAND_VISIBILITY_HIDDEN;
    Set(ZYDIS_MNEMONIC_INVALID, ZYDIS_CATEGORY_X87_ALU);
    if (mod != 3) {
        static const ZydisMnemonic arith[8] = {ZYDIS_MNEMONIC_FADD, ZYDIS_MNEMONIC_FMUL, ZYDIS_MNEMONIC_FCOM, ZYDIS_MNEMONIC_FCOMP,
                                               ZYDIS_MNEMONIC_FSUB, ZYDIS_MNEMONIC_FSUBR, ZYDIS_MNEMONIC_FDIV, ZYDIS_MNEMONIC_FDIVR};
        static const ZydisMnemonic iarith[8] = {ZYDIS_MNEMONIC_FIADD, ZYDIS_MNEMONIC_FIMUL, ZYDIS_MNEMONIC_FICOM, ZYDIS_MNEMONIC_FICOMP,
                                                ZYDIS_MNEMONIC_FISUB, ZYDIS_MNEMONIC_FISUBR, ZYDIS_MNEMONIC_FIDIV, ZYDIS_MNEMONIC_FIDIVR};
        struct M {
            ZydisMnemonic m;
            uint16_t size;
            uint8_t act;
        };
        static const M d9[8] = {{ZYDIS_MNEMONIC_FLD, 32, R}, {ZYDIS_MNEMONIC_INVALID, 0, 0}, {ZYDIS_MNEMONIC_FST, 32, W},
                                {ZYDIS_MNEMONIC_FSTP, 32, W}, {ZYDIS_MNEMONIC_FLDENV, 224, R}, {ZYDIS_MNEMONIC_FLDCW, 16, R},
                                {ZYDIS_MNEMONIC_FNSTENV, 224, W}, {ZYDIS_MNEMONIC_FNSTCW, 16, W}};
        static const M db[8] = {{ZYDIS_MNEMONIC_FILD, 32, R}, {ZYDIS_MNEMONIC_FISTTP, 32, W}, {ZYDIS_MNEMONIC_FIST, 32, W},
                                {ZYDIS_MNEMONIC_FISTP, 32, W}, {ZYDIS_MNEMONIC_INVALID, 0, 0}, {ZYDIS_MNEMONIC_FLD, 80, R},
                                {ZYDIS_MNEMONIC_INVALID, 0, 0}, {ZYDIS_MNEMONIC_FSTP, 80, W}};
        static const M dd[8] = {{ZYDIS_MNEMONIC_FLD, 64, R}, {ZYDIS_MNEMONIC_FISTTP, 64, W}, {ZYDIS_MNEMONIC_FST, 64, W},
                                {ZYDIS_MNEMONIC_FSTP, 64, W}, {ZYDIS_MNEMONIC_FRSTOR, 864, R}, {ZYDIS_MNEMONIC_INVALID, 0, 0},
                                {ZYDIS_MNEMONIC_FNSAVE, 864, W}, {ZYDIS_MNEMONIC_FNSTSW, 16, W}};
        static const M df[8] = {{ZYDIS_MNEMONIC_FILD, 16, R}, {ZYDIS_MNEMONIC_FISTTP, 16, W}, {ZYDIS_MNEMONIC_FIST, 16, W},
                                {ZYDIS_MNEMONIC_FISTP, 16, W}, {ZYDIS_MNEMONIC_FBLD, 80, R}, {ZYDIS_MNEMONIC_FILD, 64, R},
                                {ZYDIS_MNEMONIC_FBSTP, 80, W}, {ZYDIS_MNEMONIC_FISTP, 64, W}};
        const M* t = nullptr;
        switch (op) {
            case 0xD8: Set(arith[reg], ZYDIS_CATEGORY_X87_ALU); AddMem(32, R); break;
            case 0xDC: Set(arith[reg], ZYDIS_CATEGORY_X87_ALU); AddMem(64, R); break;
            case 0xDA: Set(iarith[reg], ZYDIS_CATEGORY_X87_ALU); AddMem(32, R); break;
            case 0xDE: Set(iarith[reg], ZYDIS_CATEGORY_X87_ALU); AddMem(16, R); break;
            case 0xD9: t = d9; break;
            case 0xDB: t = db; break;
            case 0xDD: t = dd; break;
            case 0xDF: t = df; break;
        }
        if (t) {
            if (t[reg].m == ZYDIS_MNEMONIC_INVALID) return false;
            Set(t[reg].m, ZYDIS_CATEGORY_X87_ALU);
            AddMem(t[reg].size, t[reg].act);
        }
        return true;
    }
    uint8_t b = uint8_t(0xC0 | reg << 3 | rm);
    switch (op) {
        case 0xD8: {
            static const ZydisMnemonic m[8] = {ZYDIS_MNEMONIC_FADD, ZYDIS_MNEMONIC_FMUL, ZYDIS_MNEMONIC_FCOM, ZYDIS_MNEMONIC_FCOMP,
                                               ZYDIS_MNEMONIC_FSUB, ZYDIS_MNEMONIC_FSUBR, ZYDIS_MNEMONIC_FDIV, ZYDIS_MNEMONIC_FDIVR};
            Set(m[reg], ZYDIS_CATEGORY_X87_ALU);
            if (reg == 2 || reg == 3) { st(i, R); st(0, R, HID); }
            else { st(0, RW, IMP); st(i, R); }
            return true;
        }
        case 0xDC: {
            // the Intel names: DC E0+i is fsubr st(i), st0 and DC E8+i fsub st(i), st0 (likewise fdivr, fdiv)
            static const ZydisMnemonic m[8] = {ZYDIS_MNEMONIC_FADD, ZYDIS_MNEMONIC_FMUL, ZYDIS_MNEMONIC_FCOM, ZYDIS_MNEMONIC_FCOMP,
                                               ZYDIS_MNEMONIC_FSUBR, ZYDIS_MNEMONIC_FSUB, ZYDIS_MNEMONIC_FDIVR, ZYDIS_MNEMONIC_FDIV};
            Set(m[reg], ZYDIS_CATEGORY_X87_ALU);
            if (reg == 2 || reg == 3) { st(i, R); st(0, R, HID); }
            else { st(i, RW); st(0, R, IMP); }
            return true;
        }
        case 0xDE: {
            if (reg == 3) {
                if (rm != 1) return false;
                Set(ZYDIS_MNEMONIC_FCOMPP, ZYDIS_CATEGORY_X87_ALU);
                st(0, R, HID);
                st(1, R, HID);
                return true;
            }
            static const ZydisMnemonic m[8] = {ZYDIS_MNEMONIC_FADDP, ZYDIS_MNEMONIC_FMULP, ZYDIS_MNEMONIC_FCOMP, ZYDIS_MNEMONIC_INVALID,
                                               ZYDIS_MNEMONIC_FSUBRP, ZYDIS_MNEMONIC_FSUBP, ZYDIS_MNEMONIC_FDIVRP, ZYDIS_MNEMONIC_FDIVP};
            Set(m[reg], ZYDIS_CATEGORY_X87_ALU);
            if (reg == 2) { st(i, R); st(0, R, HID); }
            else { st(i, RW); st(0, R, IMP); }
            return true;
        }
        case 0xD9:
            if (reg == 0) { Set(ZYDIS_MNEMONIC_FLD, ZYDIS_CATEGORY_X87_ALU); st(i, R); return true; }
            if (reg == 1) { Set(ZYDIS_MNEMONIC_FXCH, ZYDIS_CATEGORY_X87_ALU); st(i, RW); st(0, RW, HID); return true; }
            if (reg == 3) { Set(ZYDIS_MNEMONIC_FSTP, ZYDIS_CATEGORY_X87_ALU); st(i, W); return true; }  // D9 D8+i, an alias
            switch (b) {
                case 0xD0: Set(ZYDIS_MNEMONIC_FNOP); return true;
                case 0xE0: Set(ZYDIS_MNEMONIC_FCHS); return true;
                case 0xE1: Set(ZYDIS_MNEMONIC_FABS); return true;
                case 0xE4: Set(ZYDIS_MNEMONIC_FTST); return true;
                case 0xE5: Set(ZYDIS_MNEMONIC_FXAM); return true;
                case 0xE8: Set(ZYDIS_MNEMONIC_FLD1); return true;
                case 0xE9: Set(ZYDIS_MNEMONIC_FLDL2T); return true;
                case 0xEA: Set(ZYDIS_MNEMONIC_FLDL2E); return true;
                case 0xEB: Set(ZYDIS_MNEMONIC_FLDPI); return true;
                case 0xEC: Set(ZYDIS_MNEMONIC_FLDLG2); return true;
                case 0xED: Set(ZYDIS_MNEMONIC_FLDLN2); return true;
                case 0xEE: Set(ZYDIS_MNEMONIC_FLDZ); return true;
                case 0xF0: Set(ZYDIS_MNEMONIC_F2XM1); return true;
                case 0xF1: Set(ZYDIS_MNEMONIC_FYL2X); return true;
                case 0xF2: Set(ZYDIS_MNEMONIC_FPTAN); return true;
                case 0xF3: Set(ZYDIS_MNEMONIC_FPATAN); return true;
                case 0xF4: Set(ZYDIS_MNEMONIC_FXTRACT); return true;
                case 0xF5: Set(ZYDIS_MNEMONIC_FPREM1); return true;
                case 0xF6: Set(ZYDIS_MNEMONIC_FDECSTP); return true;
                case 0xF7: Set(ZYDIS_MNEMONIC_FINCSTP); return true;
                case 0xF8: Set(ZYDIS_MNEMONIC_FPREM); return true;
                case 0xF9: Set(ZYDIS_MNEMONIC_FYL2XP1); return true;
                case 0xFA: Set(ZYDIS_MNEMONIC_FSQRT); return true;
                case 0xFB: Set(ZYDIS_MNEMONIC_FSINCOS); return true;
                case 0xFC: Set(ZYDIS_MNEMONIC_FRNDINT); return true;
                case 0xFD: Set(ZYDIS_MNEMONIC_FSCALE); return true;
                case 0xFE: Set(ZYDIS_MNEMONIC_FSIN); return true;
                case 0xFF: Set(ZYDIS_MNEMONIC_FCOS); return true;
                default: return false;
            }
        case 0xDA: {
            if (b == 0xE9) { Set(ZYDIS_MNEMONIC_FUCOMPP); st(0, R, HID); st(1, R, HID); return true; }
            static const ZydisMnemonic m[4] = {ZYDIS_MNEMONIC_FCMOVB, ZYDIS_MNEMONIC_FCMOVE, ZYDIS_MNEMONIC_FCMOVBE, ZYDIS_MNEMONIC_FCMOVU};
            if (reg > 3) return false;
            Set(m[reg], ZYDIS_CATEGORY_X87_ALU);
            st(0, W, IMP);
            st(i, R);
            return true;
        }
        case 0xDB: {
            static const ZydisMnemonic m[4] = {ZYDIS_MNEMONIC_FCMOVNB, ZYDIS_MNEMONIC_FCMOVNE, ZYDIS_MNEMONIC_FCMOVNBE, ZYDIS_MNEMONIC_FCMOVNU};
            if (reg <= 3) { Set(m[reg], ZYDIS_CATEGORY_X87_ALU); st(0, W, IMP); st(i, R); return true; }
            if (reg == 5) { Set(ZYDIS_MNEMONIC_FUCOMI, ZYDIS_CATEGORY_X87_ALU); st(0, R, IMP); st(i, R); return true; }
            if (reg == 6) { Set(ZYDIS_MNEMONIC_FCOMI, ZYDIS_CATEGORY_X87_ALU); st(0, R, IMP); st(i, R); return true; }
            switch (b) {
                case 0xE0: Set(ZYDIS_MNEMONIC_FNENI); return true;
                case 0xE1: Set(ZYDIS_MNEMONIC_FNDISI); return true;
                case 0xE2: Set(ZYDIS_MNEMONIC_FNCLEX); return true;
                case 0xE3: Set(ZYDIS_MNEMONIC_FNINIT); return true;
                case 0xE4: Set(ZYDIS_MNEMONIC_FNSETPM); return true;
                default: return false;
            }
        }
        case 0xDD:
            switch (reg) {
                case 0: Set(ZYDIS_MNEMONIC_FFREE); st(i, W); return true;
                case 1: Set(ZYDIS_MNEMONIC_FXCH, ZYDIS_CATEGORY_X87_ALU); st(i, RW); st(0, RW, HID); return true;  // alias
                case 2: Set(ZYDIS_MNEMONIC_FST, ZYDIS_CATEGORY_X87_ALU); st(i, W); return true;
                case 3: Set(ZYDIS_MNEMONIC_FSTP, ZYDIS_CATEGORY_X87_ALU); st(i, W); return true;
                case 4: Set(ZYDIS_MNEMONIC_FUCOM, ZYDIS_CATEGORY_X87_ALU); st(i, R); st(0, R, HID); return true;
                case 5: Set(ZYDIS_MNEMONIC_FUCOMP, ZYDIS_CATEGORY_X87_ALU); st(i, R); st(0, R, HID); return true;
                default: return false;
            }
        case 0xDF:
            if (b == 0xE0) { Set(ZYDIS_MNEMONIC_FNSTSW); AddReg(ZYDIS_REGISTER_AX, 16, W); return true; }
            if (reg == 0) { Set(ZYDIS_MNEMONIC_FFREEP); st(i, W); return true; }
            if (reg == 5) { Set(ZYDIS_MNEMONIC_FUCOMIP, ZYDIS_CATEGORY_X87_ALU); st(0, R, IMP); st(i, R); return true; }
            if (reg == 6) { Set(ZYDIS_MNEMONIC_FCOMIP, ZYDIS_CATEGORY_X87_ALU); st(0, R, IMP); st(i, R); return true; }
            if (reg == 1) { Set(ZYDIS_MNEMONIC_FXCH, ZYDIS_CATEGORY_X87_ALU); st(i, RW); st(0, RW, HID); return true; }  // alias
            if (reg == 2 || reg == 3) { Set(ZYDIS_MNEMONIC_FSTP, ZYDIS_CATEGORY_X87_ALU); st(i, W); return true; }  // aliases
            return false;
    }
    return false;
}

// SSE / SSE2 operand shapes
enum Shape {
    kNone,
    kVW,      // xmm, xmm/m (msize)
    kWV,      // xmm/m, xmm
    kPQ,      // mm, mm/m64
    kQP,      // mm/m64, mm
    kVQ,      // xmm, mm/m64
    kPW,      // mm, xmm/m64
    kVE,      // xmm, r/m32
    kGW,      // r32, xmm/m
    kGU,      // r32, xmm (register only)
    kGN,      // r32, mm (register only)
    kMV,      // m, xmm (memory only)
    kMP,      // m64, mm (memory only)
    kVM,      // xmm, m (memory only)
    kVU,      // xmm, xmm (register only)
    kEP,      // r/m32, mm
    kEV,      // r/m32, xmm
    kPE,      // mm, r/m32
};

struct SseOp {
    ZydisMnemonic m;
    Shape shape;
    uint16_t msize;
    bool imm8;
};

bool Dec::TwoByte(uint8_t op) {
    unsigned v = Osz();
    RegClass gv = Gv();
    // which mandatory prefix applies: F2/F3 (the last one) before 66
    int mp = rep == 0xF3 ? 3 : rep == 0xF2 ? 2 : opsize ? 1 : 0;

    auto sse = [&](const SseOp& s) -> bool {
        if (s.m == ZYDIS_MNEMONIC_INVALID) return false;
        Set(s.m, ZYDIS_CATEGORY_SSE);
        ModRM();
        if (bad) return false;
        switch (s.shape) {
            case kVW: AddRegC(kXmm, reg, RW); AddRM(kXmm, s.msize, R); break;
            case kWV: AddRM(kXmm, s.msize, W); AddRegC(kXmm, reg, R); break;
            case kPQ: AddRegC(kMm, reg, RW); AddRM(kMm, 64, R); break;
            case kQP: AddRM(kMm, 64, W); AddRegC(kMm, reg, R); break;
            case kVQ: AddRegC(kXmm, reg, RW); AddRM(kMm, 64, R); break;
            case kPW: AddRegC(kMm, reg, W); AddRM(kXmm, s.msize, R); break;
            case kVE: AddRegC(kXmm, reg, RW); AddRM(kGpr32, 32, R); break;
            case kGW: AddRegC(kGpr32, reg, W); AddRM(kXmm, s.msize, R); break;
            case kGU: if (mod != 3) return false; AddRegC(kGpr32, reg, W); AddRegC(kXmm, rm, R); break;
            case kGN: if (mod != 3) return false; AddRegC(kGpr32, reg, W); AddRegC(kMm, rm, R); break;
            case kMV: if (mod == 3) return false; AddMem(s.msize, W); AddRegC(kXmm, reg, R); break;
            case kMP: if (mod == 3) return false; AddMem(64, W); AddRegC(kMm, reg, R); break;
            case kVM: if (mod == 3) return false; AddRegC(kXmm, reg, RW); AddMem(s.msize, R); break;
            case kVU: if (mod != 3) return false; AddRegC(kXmm, reg, RW); AddRegC(kXmm, rm, R); break;
            case kEP: AddRM(kGpr32, 32, W); AddRegC(kMm, reg, R); break;
            case kEV: AddRM(kGpr32, 32, W); AddRegC(kXmm, reg, R); break;
            case kPE: AddRegC(kMm, reg, W); AddRM(kGpr32, 32, R); break;
            case kNone: break;
        }
        if (s.imm8) AddImm(1, false, 8);
        return true;
    };
    // [none, 66, F2, F3]
    auto pick = [&](const SseOp (&t)[4]) { return sse(t[mp]); };
    constexpr ZydisMnemonic NO = ZYDIS_MNEMONIC_INVALID;
    auto ps4 = [&](ZydisMnemonic ps, ZydisMnemonic pd, ZydisMnemonic sd, ZydisMnemonic ss) {
        SseOp t[4] = {{ps, kVW, 128, false}, {pd, kVW, 128, false}, {sd, kVW, 64, false}, {ss, kVW, 32, false}};
        return pick(t);
    };
    // MMX row: mm, mm/m64 without 66; xmm, xmm/m128 with 66; nothing with F2/F3
    auto mmx = [&](ZydisMnemonic m, bool xmm_ok = true) {
        if (mp >= 2) return false;
        if (mp == 1) {
            if (!xmm_ok) return false;
            SseOp s = {m, kVW, 128, false};
            return sse(s);
        }
        SseOp s = {m, kPQ, 64, false};
        bool ok = sse(s);
        in->meta.category = ZYDIS_CATEGORY_MMX;
        return ok;
    };

    if (op >= 0x40 && op <= 0x4F) {
        Set(kCmov[op & 15], ZYDIS_CATEGORY_CMOV);
        AddG(v, RW);
        AddE(v, R);
        return true;
    }
    if (op >= 0x80 && op <= 0x8F) {
        Set(kJcc[op & 15], ZYDIS_CATEGORY_COND_BR);
        AddImm(opsize ? 2 : 4, true, 32, true);
        return true;
    }
    if (op >= 0x90 && op <= 0x9F) {
        Set(kSetcc[op & 15], ZYDIS_CATEGORY_BITBYTE);
        ModRM();
        AddE(8, W);
        return true;
    }
    if (op >= 0xC8 && op <= 0xCF) {
        Set(ZYDIS_MNEMONIC_BSWAP, ZYDIS_CATEGORY_DATAXFER);
        AddRegC(gv, op & 7, RW);
        return true;
    }
    if (op >= 0x19 && op <= 0x1F) {
        Set(ZYDIS_MNEMONIC_NOP, ZYDIS_CATEGORY_NOP);
        AddE(v, 0);
        return true;
    }

    switch (op) {
        case 0x00: {
            ModRM();
            static const ZydisMnemonic m[8] = {ZYDIS_MNEMONIC_SLDT, ZYDIS_MNEMONIC_STR, ZYDIS_MNEMONIC_LLDT, ZYDIS_MNEMONIC_LTR,
                                               ZYDIS_MNEMONIC_VERR, ZYDIS_MNEMONIC_VERW, NO, NO};
            if (m[reg] == NO) return false;
            Set(m[reg], ZYDIS_CATEGORY_SYSTEM);
            if (reg < 2 && mod == 3) AddRegC(gv, rm, W);
            else AddE(16, reg < 2 ? W : R);
            return true;
        }
        case 0x01:
            ModRM();
            if (mod != 3) {
                static const ZydisMnemonic m[8] = {ZYDIS_MNEMONIC_SGDT, ZYDIS_MNEMONIC_SIDT, ZYDIS_MNEMONIC_LGDT, ZYDIS_MNEMONIC_LIDT,
                                                   ZYDIS_MNEMONIC_SMSW, NO, ZYDIS_MNEMONIC_LMSW, ZYDIS_MNEMONIC_INVLPG};
                if (m[reg] == NO) return false;
                Set(m[reg], ZYDIS_CATEGORY_SYSTEM);
                uint16_t sz = reg < 4 ? 48 : reg == 7 ? 8 : 16;
                AddMem(sz, reg == 0 || reg == 1 || reg == 4 ? W : R);
                return true;
            }
            if (reg == 4) { Set(ZYDIS_MNEMONIC_SMSW, ZYDIS_CATEGORY_SYSTEM); AddRegC(gv, rm, W); return true; }
            if (reg == 6) { Set(ZYDIS_MNEMONIC_LMSW, ZYDIS_CATEGORY_SYSTEM); AddRegC(kGpr16, rm, R); return true; }
            return false;
        case 0x02: case 0x03:
            Set(op == 0x02 ? ZYDIS_MNEMONIC_LAR : ZYDIS_MNEMONIC_LSL, ZYDIS_CATEGORY_SYSTEM);
            AddG(v, W);
            AddE(16, R);
            return true;
        case 0x06: Set(ZYDIS_MNEMONIC_CLTS, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0x08: Set(ZYDIS_MNEMONIC_INVD, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0x09: Set(ZYDIS_MNEMONIC_WBINVD, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0x0B: Set(ZYDIS_MNEMONIC_UD2, ZYDIS_CATEGORY_MISC); return true;
        case 0x0D:
            ModRM();
            if (mod == 3) return false;
            Set(reg == 1 ? ZYDIS_MNEMONIC_PREFETCHW : ZYDIS_MNEMONIC_PREFETCH, ZYDIS_CATEGORY_MISC);
            AddMem(8, 0);
            return true;
        case 0x0E: Set(ZYDIS_MNEMONIC_FEMMS, ZYDIS_CATEGORY_MMX); return true;
        case 0x10: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVUPS, kVW, 128}, {ZYDIS_MNEMONIC_MOVUPD, kVW, 128},
                          {ZYDIS_MNEMONIC_MOVSD, kVW, 64}, {ZYDIS_MNEMONIC_MOVSS, kVW, 32}};
            if (!pick(t)) return false;
            ops[0].actions = W;
            return true;
        }
        case 0x11: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVUPS, kWV, 128}, {ZYDIS_MNEMONIC_MOVUPD, kWV, 128},
                          {ZYDIS_MNEMONIC_MOVSD, kWV, 64}, {ZYDIS_MNEMONIC_MOVSS, kWV, 32}};
            return pick(t);
        }
        case 0x12: {
            ModRM();
            if (mp == 0) {
                SseOp s = mod == 3 ? SseOp{ZYDIS_MNEMONIC_MOVHLPS, kVU, 128} : SseOp{ZYDIS_MNEMONIC_MOVLPS, kVM, 64};
                return sse(s);
            }
            if (mp == 1) { SseOp s = {ZYDIS_MNEMONIC_MOVLPD, kVM, 64}; return sse(s); }
            return false;
        }
        case 0x13: case 0x17: {
            bool lo = op == 0x13;
            SseOp t[4] = {{lo ? ZYDIS_MNEMONIC_MOVLPS : ZYDIS_MNEMONIC_MOVHPS, kMV, 64},
                          {lo ? ZYDIS_MNEMONIC_MOVLPD : ZYDIS_MNEMONIC_MOVHPD, kMV, 64}, {NO}, {NO}};
            return pick(t);
        }
        case 0x14: case 0x15: {
            bool lo = op == 0x14;
            SseOp t[4] = {{lo ? ZYDIS_MNEMONIC_UNPCKLPS : ZYDIS_MNEMONIC_UNPCKHPS, kVW, 128},
                          {lo ? ZYDIS_MNEMONIC_UNPCKLPD : ZYDIS_MNEMONIC_UNPCKHPD, kVW, 128}, {NO}, {NO}};
            return pick(t);
        }
        case 0x16: {
            ModRM();
            if (mp == 0) {
                SseOp s = mod == 3 ? SseOp{ZYDIS_MNEMONIC_MOVLHPS, kVU, 128} : SseOp{ZYDIS_MNEMONIC_MOVHPS, kVM, 64};
                return sse(s);
            }
            if (mp == 1) { SseOp s = {ZYDIS_MNEMONIC_MOVHPD, kVM, 64}; return sse(s); }
            return false;
        }
        case 0x18: {
            ModRM();
            static const ZydisMnemonic m[4] = {ZYDIS_MNEMONIC_PREFETCHNTA, ZYDIS_MNEMONIC_PREFETCHT0, ZYDIS_MNEMONIC_PREFETCHT1,
                                               ZYDIS_MNEMONIC_PREFETCHT2};
            if (mod != 3 && reg < 4) {
                Set(m[reg], ZYDIS_CATEGORY_MISC);
                AddMem(8, 0);
            } else {
                Set(ZYDIS_MNEMONIC_NOP, ZYDIS_CATEGORY_NOP);
                AddE(v, 0);
            }
            return true;
        }
        case 0x20: case 0x21: case 0x22: case 0x23: {
            ModRM();
            Set(ZYDIS_MNEMONIC_MOV, ZYDIS_CATEGORY_SYSTEM);
            RegClass c = op & 1 ? kDr : kCr;
            if (op < 0x22) { AddRegC(kGpr32, rm, W); AddRegC(c, reg, R); }
            else { AddRegC(c, reg, W); AddRegC(kGpr32, rm, R); }
            return true;
        }
        case 0x28: case 0x29: {
            bool st = op == 0x29;
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVAPS, st ? kWV : kVW, 128}, {ZYDIS_MNEMONIC_MOVAPD, st ? kWV : kVW, 128}, {NO}, {NO}};
            if (!pick(t)) return false;
            ops[0].actions = W;
            return true;
        }
        case 0x2A: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_CVTPI2PS, kVQ, 64}, {ZYDIS_MNEMONIC_CVTPI2PD, kVQ, 64},
                          {ZYDIS_MNEMONIC_CVTSI2SD, kVE, 32}, {ZYDIS_MNEMONIC_CVTSI2SS, kVE, 32}};
            return pick(t);
        }
        case 0x2B: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVNTPS, kMV, 128}, {ZYDIS_MNEMONIC_MOVNTPD, kMV, 128}, {NO}, {NO}};
            return pick(t);
        }
        case 0x2C: case 0x2D: {
            bool t2 = op == 0x2C;
            SseOp t[4] = {{t2 ? ZYDIS_MNEMONIC_CVTTPS2PI : ZYDIS_MNEMONIC_CVTPS2PI, kPW, 64},
                          {t2 ? ZYDIS_MNEMONIC_CVTTPD2PI : ZYDIS_MNEMONIC_CVTPD2PI, kPW, 128},
                          {t2 ? ZYDIS_MNEMONIC_CVTTSD2SI : ZYDIS_MNEMONIC_CVTSD2SI, kGW, 64},
                          {t2 ? ZYDIS_MNEMONIC_CVTTSS2SI : ZYDIS_MNEMONIC_CVTSS2SI, kGW, 32}};
            return pick(t);
        }
        case 0x2E: case 0x2F: {
            bool u = op == 0x2E;
            SseOp t[4] = {{u ? ZYDIS_MNEMONIC_UCOMISS : ZYDIS_MNEMONIC_COMISS, kVW, 32},
                          {u ? ZYDIS_MNEMONIC_UCOMISD : ZYDIS_MNEMONIC_COMISD, kVW, 64}, {NO}, {NO}};
            if (!pick(t)) return false;
            ops[0].actions = R;
            return true;
        }
        case 0x30: Set(ZYDIS_MNEMONIC_WRMSR, ZYDIS_CATEGORY_SYSTEM); Hidden(ZYDIS_REGISTER_ECX, R); Hidden(ZYDIS_REGISTER_EAX, R); Hidden(ZYDIS_REGISTER_EDX, R); return true;
        case 0x31: Set(ZYDIS_MNEMONIC_RDTSC, ZYDIS_CATEGORY_SYSTEM); Hidden(ZYDIS_REGISTER_EAX, W); Hidden(ZYDIS_REGISTER_EDX, W); return true;
        case 0x32: Set(ZYDIS_MNEMONIC_RDMSR, ZYDIS_CATEGORY_SYSTEM); Hidden(ZYDIS_REGISTER_ECX, R); Hidden(ZYDIS_REGISTER_EAX, W); Hidden(ZYDIS_REGISTER_EDX, W); return true;
        case 0x33: Set(ZYDIS_MNEMONIC_RDPMC, ZYDIS_CATEGORY_SYSTEM); Hidden(ZYDIS_REGISTER_ECX, R); Hidden(ZYDIS_REGISTER_EAX, W); Hidden(ZYDIS_REGISTER_EDX, W); return true;
        case 0x34: Set(ZYDIS_MNEMONIC_SYSENTER, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0x35: Set(ZYDIS_MNEMONIC_SYSEXIT, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0x50: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVMSKPS, kGU, 128}, {ZYDIS_MNEMONIC_MOVMSKPD, kGU, 128}, {NO}, {NO}};
            return pick(t);
        }
        case 0x51: return ps4(ZYDIS_MNEMONIC_SQRTPS, ZYDIS_MNEMONIC_SQRTPD, ZYDIS_MNEMONIC_SQRTSD, ZYDIS_MNEMONIC_SQRTSS);
        case 0x52: return ps4(ZYDIS_MNEMONIC_RSQRTPS, NO, NO, ZYDIS_MNEMONIC_RSQRTSS);
        case 0x53: return ps4(ZYDIS_MNEMONIC_RCPPS, NO, NO, ZYDIS_MNEMONIC_RCPSS);
        case 0x54: return ps4(ZYDIS_MNEMONIC_ANDPS, ZYDIS_MNEMONIC_ANDPD, NO, NO);
        case 0x55: return ps4(ZYDIS_MNEMONIC_ANDNPS, ZYDIS_MNEMONIC_ANDNPD, NO, NO);
        case 0x56: return ps4(ZYDIS_MNEMONIC_ORPS, ZYDIS_MNEMONIC_ORPD, NO, NO);
        case 0x57: return ps4(ZYDIS_MNEMONIC_XORPS, ZYDIS_MNEMONIC_XORPD, NO, NO);
        case 0x58: return ps4(ZYDIS_MNEMONIC_ADDPS, ZYDIS_MNEMONIC_ADDPD, ZYDIS_MNEMONIC_ADDSD, ZYDIS_MNEMONIC_ADDSS);
        case 0x59: return ps4(ZYDIS_MNEMONIC_MULPS, ZYDIS_MNEMONIC_MULPD, ZYDIS_MNEMONIC_MULSD, ZYDIS_MNEMONIC_MULSS);
        case 0x5A: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_CVTPS2PD, kVW, 64}, {ZYDIS_MNEMONIC_CVTPD2PS, kVW, 128},
                          {ZYDIS_MNEMONIC_CVTSD2SS, kVW, 64}, {ZYDIS_MNEMONIC_CVTSS2SD, kVW, 32}};
            return pick(t);
        }
        case 0x5B: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_CVTDQ2PS, kVW, 128}, {ZYDIS_MNEMONIC_CVTPS2DQ, kVW, 128}, {NO},
                          {ZYDIS_MNEMONIC_CVTTPS2DQ, kVW, 128}};
            return pick(t);
        }
        case 0x5C: return ps4(ZYDIS_MNEMONIC_SUBPS, ZYDIS_MNEMONIC_SUBPD, ZYDIS_MNEMONIC_SUBSD, ZYDIS_MNEMONIC_SUBSS);
        case 0x5D: return ps4(ZYDIS_MNEMONIC_MINPS, ZYDIS_MNEMONIC_MINPD, ZYDIS_MNEMONIC_MINSD, ZYDIS_MNEMONIC_MINSS);
        case 0x5E: return ps4(ZYDIS_MNEMONIC_DIVPS, ZYDIS_MNEMONIC_DIVPD, ZYDIS_MNEMONIC_DIVSD, ZYDIS_MNEMONIC_DIVSS);
        case 0x5F: return ps4(ZYDIS_MNEMONIC_MAXPS, ZYDIS_MNEMONIC_MAXPD, ZYDIS_MNEMONIC_MAXSD, ZYDIS_MNEMONIC_MAXSS);
        case 0x60: return mmx(ZYDIS_MNEMONIC_PUNPCKLBW);
        case 0x61: return mmx(ZYDIS_MNEMONIC_PUNPCKLWD);
        case 0x62: return mmx(ZYDIS_MNEMONIC_PUNPCKLDQ);
        case 0x63: return mmx(ZYDIS_MNEMONIC_PACKSSWB);
        case 0x64: return mmx(ZYDIS_MNEMONIC_PCMPGTB);
        case 0x65: return mmx(ZYDIS_MNEMONIC_PCMPGTW);
        case 0x66: return mmx(ZYDIS_MNEMONIC_PCMPGTD);
        case 0x67: return mmx(ZYDIS_MNEMONIC_PACKUSWB);
        case 0x68: return mmx(ZYDIS_MNEMONIC_PUNPCKHBW);
        case 0x69: return mmx(ZYDIS_MNEMONIC_PUNPCKHWD);
        case 0x6A: return mmx(ZYDIS_MNEMONIC_PUNPCKHDQ);
        case 0x6B: return mmx(ZYDIS_MNEMONIC_PACKSSDW);
        case 0x6C: if (mp != 1) return false; return mmx(ZYDIS_MNEMONIC_PUNPCKLQDQ);
        case 0x6D: if (mp != 1) return false; return mmx(ZYDIS_MNEMONIC_PUNPCKHQDQ);
        case 0x6E: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVD, kPE, 32}, {ZYDIS_MNEMONIC_MOVD, kVE, 32}, {NO}, {NO}};
            return pick(t);
        }
        case 0x6F: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVQ, kPQ, 64}, {ZYDIS_MNEMONIC_MOVDQA, kVW, 128}, {NO},
                          {ZYDIS_MNEMONIC_MOVDQU, kVW, 128}};
            if (!pick(t)) return false;
            ops[0].actions = W;
            return true;
        }
        case 0x70: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_PSHUFW, kPQ, 64, true}, {ZYDIS_MNEMONIC_PSHUFD, kVW, 128, true},
                          {ZYDIS_MNEMONIC_PSHUFLW, kVW, 128, true}, {ZYDIS_MNEMONIC_PSHUFHW, kVW, 128, true}};
            if (!pick(t)) return false;
            ops[0].actions = W;
            return true;
        }
        case 0x71: case 0x72: case 0x73: {
            ModRM();
            if (mod != 3 || mp >= 2) return false;
            ZydisMnemonic m = NO;
            if (op == 0x71) m = reg == 2 ? ZYDIS_MNEMONIC_PSRLW : reg == 4 ? ZYDIS_MNEMONIC_PSRAW : reg == 6 ? ZYDIS_MNEMONIC_PSLLW : NO;
            if (op == 0x72) m = reg == 2 ? ZYDIS_MNEMONIC_PSRLD : reg == 4 ? ZYDIS_MNEMONIC_PSRAD : reg == 6 ? ZYDIS_MNEMONIC_PSLLD : NO;
            if (op == 0x73) {
                m = reg == 2 ? ZYDIS_MNEMONIC_PSRLQ : reg == 6 ? ZYDIS_MNEMONIC_PSLLQ : NO;
                if (mp == 1 && reg == 3) m = ZYDIS_MNEMONIC_PSRLDQ;
                if (mp == 1 && reg == 7) m = ZYDIS_MNEMONIC_PSLLDQ;
            }
            if (m == NO) return false;
            Set(m, mp == 1 ? ZYDIS_CATEGORY_SSE : ZYDIS_CATEGORY_MMX);
            AddRegC(mp == 1 ? kXmm : kMm, rm, RW);
            AddImm(1, false, 8);
            return true;
        }
        case 0x74: return mmx(ZYDIS_MNEMONIC_PCMPEQB);
        case 0x75: return mmx(ZYDIS_MNEMONIC_PCMPEQW);
        case 0x76: return mmx(ZYDIS_MNEMONIC_PCMPEQD);
        case 0x77: if (mp) return false; Set(ZYDIS_MNEMONIC_EMMS, ZYDIS_CATEGORY_MMX); return true;
        case 0x7E: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVD, kEP, 32}, {ZYDIS_MNEMONIC_MOVD, kEV, 32}, {NO},
                          {ZYDIS_MNEMONIC_MOVQ, kVW, 64}};
            return pick(t);
        }
        case 0x7F: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVQ, kQP, 64}, {ZYDIS_MNEMONIC_MOVDQA, kWV, 128}, {NO},
                          {ZYDIS_MNEMONIC_MOVDQU, kWV, 128}};
            return pick(t);
        }
        case 0xA0: case 0xA8:
            Set(ZYDIS_MNEMONIC_PUSH, ZYDIS_CATEGORY_PUSH);
            AddReg(op == 0xA0 ? ZYDIS_REGISTER_FS : ZYDIS_REGISTER_GS, 32, R);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0xA1: case 0xA9:
            Set(ZYDIS_MNEMONIC_POP, ZYDIS_CATEGORY_POP);
            AddReg(op == 0xA1 ? ZYDIS_REGISTER_FS : ZYDIS_REGISTER_GS, 32, W);
            Hidden(ZYDIS_REGISTER_ESP, RW);
            return true;
        case 0xA2:
            Set(ZYDIS_MNEMONIC_CPUID, ZYDIS_CATEGORY_MISC);
            Hidden(ZYDIS_REGISTER_EAX, RW);
            Hidden(ZYDIS_REGISTER_ECX, RW);
            Hidden(ZYDIS_REGISTER_EBX, W);
            Hidden(ZYDIS_REGISTER_EDX, W);
            return true;
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
            ZydisMnemonic m = op == 0xA3 ? ZYDIS_MNEMONIC_BT : op == 0xAB ? ZYDIS_MNEMONIC_BTS : op == 0xB3 ? ZYDIS_MNEMONIC_BTR : ZYDIS_MNEMONIC_BTC;
            Set(m, ZYDIS_CATEGORY_BITBYTE);
            lockable = op != 0xA3;
            AddE(v, op == 0xA3 ? R : RW);
            AddG(v, R);
            return true;
        }
        case 0xA4: case 0xA5: case 0xAC: case 0xAD:
            Set(op < 0xAC ? ZYDIS_MNEMONIC_SHLD : ZYDIS_MNEMONIC_SHRD, ZYDIS_CATEGORY_SHIFT);
            AddE(v, RW);
            AddG(v, R);
            if (op & 1) AddRegC(kGpr8, 1, R);
            else AddImm(1, false, 8);
            return true;
        case 0xAA: Set(ZYDIS_MNEMONIC_RSM, ZYDIS_CATEGORY_SYSTEM); return true;
        case 0xAE:
            ModRM();
            if (mod != 3) {
                switch (reg) {
                    case 0: Set(ZYDIS_MNEMONIC_FXSAVE); AddMem(4096, W); return true;
                    case 1: Set(ZYDIS_MNEMONIC_FXRSTOR); AddMem(4096, R); return true;
                    case 2: Set(ZYDIS_MNEMONIC_LDMXCSR, ZYDIS_CATEGORY_SSE); AddMem(32, R); return true;
                    case 3: Set(ZYDIS_MNEMONIC_STMXCSR, ZYDIS_CATEGORY_SSE); AddMem(32, W); return true;
                    case 7: Set(ZYDIS_MNEMONIC_CLFLUSH); AddMem(8, R); return true;
                    default: return false;
                }
            }
            if (reg == 5) { Set(ZYDIS_MNEMONIC_LFENCE); return true; }
            if (reg == 6) { Set(ZYDIS_MNEMONIC_MFENCE); return true; }
            if (reg == 7) { Set(ZYDIS_MNEMONIC_SFENCE); return true; }
            return false;
        case 0xAF:
            Set(ZYDIS_MNEMONIC_IMUL, ZYDIS_CATEGORY_BINARY);
            AddG(v, RW);
            AddE(v, R);
            return true;
        case 0xB0: case 0xB1: {
            unsigned w = op == 0xB0 ? 8 : v;
            Set(ZYDIS_MNEMONIC_CMPXCHG, ZYDIS_CATEGORY_SEMAPHORE);
            lockable = true;
            AddE(w, RW);
            AddG(w, R);
            Hidden(ZYDIS_REGISTER_EAX, RW);
            return true;
        }
        case 0xB2: case 0xB4: case 0xB5:
            Set(op == 0xB2 ? ZYDIS_MNEMONIC_LSS : op == 0xB4 ? ZYDIS_MNEMONIC_LFS : ZYDIS_MNEMONIC_LGS);
            if (!NeedMem()) return false;
            AddG(v, W);
            AddMem(uint16_t(v + 16), R);
            return true;
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:
            Set(op < 0xB8 ? ZYDIS_MNEMONIC_MOVZX : ZYDIS_MNEMONIC_MOVSX, ZYDIS_CATEGORY_DATAXFER);
            AddG(v, W);
            AddE(op & 1 ? 16 : 8, R);
            return true;
        case 0xB9:
            Set(ZYDIS_MNEMONIC_UD1, ZYDIS_CATEGORY_MISC);
            AddG(v, R);
            AddE(v, R);
            return true;
        case 0xBA: {
            ModRM();
            if (reg < 4) return false;
            static const ZydisMnemonic m[4] = {ZYDIS_MNEMONIC_BT, ZYDIS_MNEMONIC_BTS, ZYDIS_MNEMONIC_BTR, ZYDIS_MNEMONIC_BTC};
            Set(m[reg - 4], ZYDIS_CATEGORY_BITBYTE);
            lockable = reg != 4;
            AddE(v, reg == 4 ? R : RW);
            AddImm(1, false, 8);
            return true;
        }
        case 0xBC: case 0xBD:
            Set(op == 0xBC ? ZYDIS_MNEMONIC_BSF : ZYDIS_MNEMONIC_BSR, ZYDIS_CATEGORY_BITBYTE);
            AddG(v, W);
            AddE(v, R);
            return true;
        case 0xC0: case 0xC1: {
            unsigned w = op == 0xC0 ? 8 : v;
            Set(ZYDIS_MNEMONIC_XADD, ZYDIS_CATEGORY_SEMAPHORE);
            lockable = true;
            AddE(w, RW);
            AddG(w, RW);
            return true;
        }
        case 0xC2: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_CMPPS, kVW, 128, true}, {ZYDIS_MNEMONIC_CMPPD, kVW, 128, true},
                          {ZYDIS_MNEMONIC_CMPSD, kVW, 64, true}, {ZYDIS_MNEMONIC_CMPSS, kVW, 32, true}};
            return pick(t);
        }
        case 0xC3:
            if (mp) return false;
            Set(ZYDIS_MNEMONIC_MOVNTI);
            if (!NeedMem()) return false;
            AddMem(32, W);
            AddG(32, R);
            return true;
        case 0xC4: {
            if (mp >= 2) return false;
            Set(ZYDIS_MNEMONIC_PINSRW, mp ? ZYDIS_CATEGORY_SSE : ZYDIS_CATEGORY_MMX);
            ModRM();
            AddRegC(mp ? kXmm : kMm, reg, RW);
            AddRM(kGpr32, 16, R);
            AddImm(1, false, 8);
            return true;
        }
        case 0xC5: {
            if (mp >= 2) return false;
            Set(ZYDIS_MNEMONIC_PEXTRW, mp ? ZYDIS_CATEGORY_SSE : ZYDIS_CATEGORY_MMX);
            if (!NeedReg()) return false;
            AddRegC(kGpr32, reg, W);
            AddRegC(mp ? kXmm : kMm, rm, R);
            AddImm(1, false, 8);
            return true;
        }
        case 0xC6: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_SHUFPS, kVW, 128, true}, {ZYDIS_MNEMONIC_SHUFPD, kVW, 128, true}, {NO}, {NO}};
            return pick(t);
        }
        case 0xC7:
            ModRM();
            if (reg != 1 || mod == 3) return false;
            Set(ZYDIS_MNEMONIC_CMPXCHG8B, ZYDIS_CATEGORY_SEMAPHORE);
            lockable = true;
            AddMem(64, RW);
            Hidden(ZYDIS_REGISTER_EAX, RW);
            Hidden(ZYDIS_REGISTER_EDX, RW);
            Hidden(ZYDIS_REGISTER_EBX, R);
            Hidden(ZYDIS_REGISTER_ECX, R);
            return true;
        case 0xD1: return mmx(ZYDIS_MNEMONIC_PSRLW);
        case 0xD2: return mmx(ZYDIS_MNEMONIC_PSRLD);
        case 0xD3: return mmx(ZYDIS_MNEMONIC_PSRLQ);
        case 0xD4: return mmx(ZYDIS_MNEMONIC_PADDQ);
        case 0xD5: return mmx(ZYDIS_MNEMONIC_PMULLW);
        case 0xD6: {
            if (mp == 1) { SseOp s = {ZYDIS_MNEMONIC_MOVQ, kWV, 64}; return sse(s); }
            if (mp == 3) { SseOp s = {ZYDIS_MNEMONIC_MOVQ2DQ, kVQ, 64}; ModRM(); if (mod != 3) return false; return sse(s); }
            if (mp == 2) { ModRM(); if (mod != 3) return false; Set(ZYDIS_MNEMONIC_MOVDQ2Q, ZYDIS_CATEGORY_SSE); AddRegC(kMm, reg, W); AddRegC(kXmm, rm, R); return true; }
            return false;
        }
        case 0xD7: {
            if (mp >= 2) return false;
            SseOp s = {ZYDIS_MNEMONIC_PMOVMSKB, mp ? kGU : kGN, 64};
            return sse(s);
        }
        case 0xD8: return mmx(ZYDIS_MNEMONIC_PSUBUSB);
        case 0xD9: return mmx(ZYDIS_MNEMONIC_PSUBUSW);
        case 0xDA: return mmx(ZYDIS_MNEMONIC_PMINUB);
        case 0xDB: return mmx(ZYDIS_MNEMONIC_PAND);
        case 0xDC: return mmx(ZYDIS_MNEMONIC_PADDUSB);
        case 0xDD: return mmx(ZYDIS_MNEMONIC_PADDUSW);
        case 0xDE: return mmx(ZYDIS_MNEMONIC_PMAXUB);
        case 0xDF: return mmx(ZYDIS_MNEMONIC_PANDN);
        case 0xE0: return mmx(ZYDIS_MNEMONIC_PAVGB);
        case 0xE1: return mmx(ZYDIS_MNEMONIC_PSRAW);
        case 0xE2: return mmx(ZYDIS_MNEMONIC_PSRAD);
        case 0xE3: return mmx(ZYDIS_MNEMONIC_PAVGW);
        case 0xE4: return mmx(ZYDIS_MNEMONIC_PMULHUW);
        case 0xE5: return mmx(ZYDIS_MNEMONIC_PMULHW);
        case 0xE6: {
            SseOp t[4] = {{NO}, {ZYDIS_MNEMONIC_CVTTPD2DQ, kVW, 128}, {ZYDIS_MNEMONIC_CVTPD2DQ, kVW, 128},
                          {ZYDIS_MNEMONIC_CVTDQ2PD, kVW, 64}};
            return pick(t);
        }
        case 0xE7: {
            SseOp t[4] = {{ZYDIS_MNEMONIC_MOVNTQ, kMP, 64}, {ZYDIS_MNEMONIC_MOVNTDQ, kMV, 128}, {NO}, {NO}};
            return pick(t);
        }
        case 0xE8: return mmx(ZYDIS_MNEMONIC_PSUBSB);
        case 0xE9: return mmx(ZYDIS_MNEMONIC_PSUBSW);
        case 0xEA: return mmx(ZYDIS_MNEMONIC_PMINSW);
        case 0xEB: return mmx(ZYDIS_MNEMONIC_POR);
        case 0xEC: return mmx(ZYDIS_MNEMONIC_PADDSB);
        case 0xED: return mmx(ZYDIS_MNEMONIC_PADDSW);
        case 0xEE: return mmx(ZYDIS_MNEMONIC_PMAXSW);
        case 0xEF: return mmx(ZYDIS_MNEMONIC_PXOR);
        case 0xF1: return mmx(ZYDIS_MNEMONIC_PSLLW);
        case 0xF2: return mmx(ZYDIS_MNEMONIC_PSLLD);
        case 0xF3: return mmx(ZYDIS_MNEMONIC_PSLLQ);
        case 0xF4: return mmx(ZYDIS_MNEMONIC_PMULUDQ);
        case 0xF5: return mmx(ZYDIS_MNEMONIC_PMADDWD);
        case 0xF6: return mmx(ZYDIS_MNEMONIC_PSADBW);
        case 0xF7: {
            if (mp >= 2) return false;
            ModRM();
            if (mod != 3) return false;
            Set(mp ? ZYDIS_MNEMONIC_MASKMOVDQU : ZYDIS_MNEMONIC_MASKMOVQ, ZYDIS_CATEGORY_MMX);
            AddRegC(mp ? kXmm : kMm, reg, R);
            AddRegC(mp ? kXmm : kMm, rm, R);
            Hidden(ZYDIS_REGISTER_EDI, R);
            return true;
        }
        case 0xF8: return mmx(ZYDIS_MNEMONIC_PSUBB);
        case 0xF9: return mmx(ZYDIS_MNEMONIC_PSUBW);
        case 0xFA: return mmx(ZYDIS_MNEMONIC_PSUBD);
        case 0xFB: return mmx(ZYDIS_MNEMONIC_PSUBQ);
        case 0xFC: return mmx(ZYDIS_MNEMONIC_PADDB);
        case 0xFD: return mmx(ZYDIS_MNEMONIC_PADDW);
        case 0xFE: return mmx(ZYDIS_MNEMONIC_PADDD);
        case 0xFF:
            Set(ZYDIS_MNEMONIC_UD0, ZYDIS_CATEGORY_MISC);
            AddG(v, R);
            AddE(v, R);
            return true;
        default:
            return false;
    }
}

const char* const kRegNames[] = {
    "none", "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh", "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7",
    "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "es", "cs", "ss", "ds", "fs", "gs", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "cr8", "cr9", "cr10",
    "cr11", "cr12", "cr13", "cr14", "cr15", "dr0", "dr1", "dr2", "dr3", "dr4", "dr5", "dr6", "dr7", "dr8", "dr9",
    "dr10", "dr11", "dr12", "dr13", "dr14", "dr15", "eflags", "eip", "x87control", "x87status", "x87tag", "mxcsr",
    "gdtr", "idtr", "ldtr", "tr", "msr"};

}  // namespace

ZyanStatus ZydisDecoderInit(ZydisDecoder* decoder, ZydisMachineMode machine_mode, ZydisStackWidth stack_width) {
    decoder->machine_mode = machine_mode;
    decoder->stack_width = stack_width;
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisDecoderDecodeFull(const ZydisDecoder*, const void* buffer, ZyanUSize length,
                                  ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operands) {
    std::memset(instruction, 0, sizeof(*instruction));
    ZydisDecodedOperand scratch[ZYDIS_MAX_OPERAND_COUNT];
    Dec d;
    d.p = static_cast<const uint8_t*>(buffer);
    d.avail = length;
    d.in = instruction;
    d.ops = operands ? operands : scratch;
    std::memset(d.ops, 0, sizeof(ZydisDecodedOperand) * ZYDIS_MAX_OPERAND_COUNT);
    if (!d.Run()) {
        std::memset(instruction, 0, sizeof(*instruction));
        return ZYDIS_STATUS_DECODING_ERROR;
    }
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisDecoderDecodeInstruction(const ZydisDecoder* decoder, ZydisDecoderContext* context, const void* buffer,
                                         ZyanUSize length, ZydisDecodedInstruction* instruction) {
    return ZydisDecoderDecodeFull(decoder, buffer, length, instruction, context ? context->operands : nullptr);
}

const char* ZydisMnemonicGetString(ZydisMnemonic mnemonic) {
    if (mnemonic >= ZYDIS_MNEMONIC_MAX_VALUE) return nullptr;
    return g_lower[mnemonic];
}

const char* ZydisRegisterGetString(ZydisRegister reg) {
    if (reg >= ZYDIS_REGISTER_MAX_VALUE) return nullptr;
    return kRegNames[reg];
}

ZydisRegisterWidth ZydisRegisterGetWidth(ZydisMachineMode, ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_AL && r <= ZYDIS_REGISTER_BH) return 8;
    if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) return 16;
    if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI) return 32;
    if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7) return 80;
    if (r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7) return 64;
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM7) return 128;
    if (r >= ZYDIS_REGISTER_ES && r <= ZYDIS_REGISTER_GS) return 16;
    if (r >= ZYDIS_REGISTER_CR0 && r <= ZYDIS_REGISTER_DR15) return 32;
    return 32;
}

ZydisRegister ZydisRegisterGetLargestEnclosing(ZydisMachineMode, ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_AL && r <= ZYDIS_REGISTER_BL) return ZydisRegister(ZYDIS_REGISTER_EAX + (r - ZYDIS_REGISTER_AL));
    if (r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH) return ZydisRegister(ZYDIS_REGISTER_EAX + (r - ZYDIS_REGISTER_AH));
    if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) return ZydisRegister(ZYDIS_REGISTER_EAX + (r - ZYDIS_REGISTER_AX));
    return r;
}

ZyanStatus ZydisFormatterInit(ZydisFormatter* formatter, ZydisFormatterStyle style) {
    formatter->style = style;
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisFormatterFormatInstruction(const ZydisFormatter*, const ZydisDecodedInstruction* in,
                                           const ZydisDecodedOperand* ops, ZyanU8 count, char* buf, ZyanUSize len,
                                           ZyanU64 address, void*) {
    if (!len) return ZYDIS_STATUS_DECODING_ERROR;
    size_t n = 0;
    auto put = [&](const char* s) {
        while (*s && n + 1 < len) buf[n++] = *s++;
        buf[n] = 0;
    };
    char tmp[96];
    if (in->attributes & ZYDIS_ATTRIB_HAS_LOCK) put("lock ");
    if (in->attributes & ZYDIS_ATTRIB_HAS_REP) put("rep ");
    if (in->attributes & ZYDIS_ATTRIB_HAS_REPE) put("repe ");
    if (in->attributes & ZYDIS_ATTRIB_HAS_REPNE) put("repne ");
    put(ZydisMnemonicGetString(in->mnemonic));
    for (int i = 0; i < count; i++) {
        const ZydisDecodedOperand& o = ops[i];
        if (o.visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN) continue;
        put(i ? ", " : " ");
        switch (o.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                put(ZydisRegisterGetString(o.reg.value));
                break;
            case ZYDIS_OPERAND_TYPE_IMMEDIATE:
                if (o.imm.is_relative)
                    std::snprintf(tmp, sizeof(tmp), "0x%X", uint32_t(address + in->length + o.imm.value.s));
                else
                    std::snprintf(tmp, sizeof(tmp), "0x%X", uint32_t(o.imm.value.u));
                put(tmp);
                break;
            case ZYDIS_OPERAND_TYPE_POINTER:
                std::snprintf(tmp, sizeof(tmp), "0x%X:0x%X", o.ptr.segment, o.ptr.offset);
                put(tmp);
                break;
            case ZYDIS_OPERAND_TYPE_MEMORY: {
                const char* sz = o.size == 8 ? "byte" : o.size == 16 ? "word" : o.size == 32 ? "dword" : o.size == 64 ? "qword"
                               : o.size == 80 ? "tbyte" : o.size == 128 ? "xmmword" : nullptr;
                if (sz && o.mem.type != ZYDIS_MEMOP_TYPE_AGEN) {
                    put(sz);
                    put(" ptr ");
                }
                if (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS ||
                    o.mem.segment == ZYDIS_REGISTER_ES || o.mem.segment == ZYDIS_REGISTER_CS) {
                    put(ZydisRegisterGetString(o.mem.segment));
                    put(":");
                }
                put("[");
                bool any = false;
                if (o.mem.base) { put(ZydisRegisterGetString(o.mem.base)); any = true; }
                if (o.mem.index) {
                    if (any) put("+");
                    put(ZydisRegisterGetString(o.mem.index));
                    if (o.mem.scale > 1) { std::snprintf(tmp, sizeof(tmp), "*%d", o.mem.scale); put(tmp); }
                    any = true;
                }
                if (o.mem.disp.has_displacement && (o.mem.disp.value || !any)) {
                    int64_t dv = o.mem.disp.value;
                    if (any && dv < 0) std::snprintf(tmp, sizeof(tmp), "-0x%llX", (unsigned long long)-dv);
                    else std::snprintf(tmp, sizeof(tmp), "%s0x%X", any ? "+" : "", uint32_t(dv));
                    put(tmp);
                }
                put("]");
                break;
            }
            default:
                break;
        }
    }
    return ZYAN_STATUS_SUCCESS;
}
