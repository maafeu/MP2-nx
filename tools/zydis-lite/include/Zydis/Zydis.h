// zydis-lite: the part of Zydis 4.1's decoder API that XboxXBE-Recomp's tools (xbe_recomp, xbe_symbols) use, for
// 32-bit x86 as the original Xbox's Pentium III runs it (integer, x87, MMX, SSE; SSE2 decoded for lengths).
//
// Written for MP2-nx because the recompiler fetches Zydis from GitHub and this work allows no downloads. It is not
// Zydis: the names and fields match what the tools read, the values follow the Intel manual, and the operand order
// of the x87 forms is the one the code generator expects (docs/MP2-notes.md, "the x86 decoder"). Tested against
// LLVM's disassembler over every instruction of the game's code sections (tools/zydis-lite/test).
#pragma once
#include <cstddef>
#include <cstdint>

typedef uint8_t ZyanU8;
typedef uint16_t ZyanU16;
typedef uint32_t ZyanU32;
typedef uint64_t ZyanU64;
typedef int8_t ZyanI8;
typedef int16_t ZyanI16;
typedef int32_t ZyanI32;
typedef int64_t ZyanI64;
typedef size_t ZyanUSize;
typedef bool ZyanBool;
typedef uint32_t ZyanStatus;

#define ZYAN_STATUS_SUCCESS 0x00100000u
#define ZYDIS_STATUS_DECODING_ERROR 0x80200000u
#define ZYAN_SUCCESS(status) (!((status) & 0x80000000u))
#define ZYAN_FAILED(status) (((status) & 0x80000000u) != 0)

#define ZYDIS_MAX_OPERAND_COUNT 10
#define ZYDIS_MAX_OPERAND_COUNT_VISIBLE 5
#define ZYDIS_MAX_INSTRUCTION_LENGTH 15

enum ZydisMachineMode { ZYDIS_MACHINE_MODE_LEGACY_32 = 4 };
enum ZydisStackWidth { ZYDIS_STACK_WIDTH_32 = 1 };

enum ZydisRegister : uint16_t {
    ZYDIS_REGISTER_NONE = 0,
    ZYDIS_REGISTER_AL, ZYDIS_REGISTER_CL, ZYDIS_REGISTER_DL, ZYDIS_REGISTER_BL,
    ZYDIS_REGISTER_AH, ZYDIS_REGISTER_CH, ZYDIS_REGISTER_DH, ZYDIS_REGISTER_BH,
    ZYDIS_REGISTER_AX, ZYDIS_REGISTER_CX, ZYDIS_REGISTER_DX, ZYDIS_REGISTER_BX,
    ZYDIS_REGISTER_SP, ZYDIS_REGISTER_BP, ZYDIS_REGISTER_SI, ZYDIS_REGISTER_DI,
    ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX, ZYDIS_REGISTER_EBX,
    ZYDIS_REGISTER_ESP, ZYDIS_REGISTER_EBP, ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI,
    ZYDIS_REGISTER_ST0, ZYDIS_REGISTER_ST1, ZYDIS_REGISTER_ST2, ZYDIS_REGISTER_ST3,
    ZYDIS_REGISTER_ST4, ZYDIS_REGISTER_ST5, ZYDIS_REGISTER_ST6, ZYDIS_REGISTER_ST7,
    ZYDIS_REGISTER_MM0, ZYDIS_REGISTER_MM1, ZYDIS_REGISTER_MM2, ZYDIS_REGISTER_MM3,
    ZYDIS_REGISTER_MM4, ZYDIS_REGISTER_MM5, ZYDIS_REGISTER_MM6, ZYDIS_REGISTER_MM7,
    ZYDIS_REGISTER_XMM0, ZYDIS_REGISTER_XMM1, ZYDIS_REGISTER_XMM2, ZYDIS_REGISTER_XMM3,
    ZYDIS_REGISTER_XMM4, ZYDIS_REGISTER_XMM5, ZYDIS_REGISTER_XMM6, ZYDIS_REGISTER_XMM7,
    ZYDIS_REGISTER_ES, ZYDIS_REGISTER_CS, ZYDIS_REGISTER_SS, ZYDIS_REGISTER_DS,
    ZYDIS_REGISTER_FS, ZYDIS_REGISTER_GS,
    ZYDIS_REGISTER_CR0, ZYDIS_REGISTER_CR1, ZYDIS_REGISTER_CR2, ZYDIS_REGISTER_CR3,
    ZYDIS_REGISTER_CR4, ZYDIS_REGISTER_CR5, ZYDIS_REGISTER_CR6, ZYDIS_REGISTER_CR7,
    ZYDIS_REGISTER_CR8, ZYDIS_REGISTER_CR9, ZYDIS_REGISTER_CR10, ZYDIS_REGISTER_CR11,
    ZYDIS_REGISTER_CR12, ZYDIS_REGISTER_CR13, ZYDIS_REGISTER_CR14, ZYDIS_REGISTER_CR15,
    ZYDIS_REGISTER_DR0, ZYDIS_REGISTER_DR1, ZYDIS_REGISTER_DR2, ZYDIS_REGISTER_DR3,
    ZYDIS_REGISTER_DR4, ZYDIS_REGISTER_DR5, ZYDIS_REGISTER_DR6, ZYDIS_REGISTER_DR7,
    ZYDIS_REGISTER_DR8, ZYDIS_REGISTER_DR9, ZYDIS_REGISTER_DR10, ZYDIS_REGISTER_DR11,
    ZYDIS_REGISTER_DR12, ZYDIS_REGISTER_DR13, ZYDIS_REGISTER_DR14, ZYDIS_REGISTER_DR15,
    ZYDIS_REGISTER_EFLAGS, ZYDIS_REGISTER_EIP, ZYDIS_REGISTER_X87CONTROL, ZYDIS_REGISTER_X87STATUS,
    ZYDIS_REGISTER_X87TAG, ZYDIS_REGISTER_MXCSR, ZYDIS_REGISTER_GDTR, ZYDIS_REGISTER_IDTR,
    ZYDIS_REGISTER_LDTR, ZYDIS_REGISTER_TR, ZYDIS_REGISTER_MSR,
    ZYDIS_REGISTER_MAX_VALUE
};
typedef uint16_t ZydisRegisterWidth;

#include "ZydisMnemonics.inc"  // enum ZydisMnemonic

enum ZydisInstructionCategory : uint8_t {
    ZYDIS_CATEGORY_INVALID = 0,
    ZYDIS_CATEGORY_OTHER,
    ZYDIS_CATEGORY_BINARY,
    ZYDIS_CATEGORY_LOGICAL,
    ZYDIS_CATEGORY_DATAXFER,
    ZYDIS_CATEGORY_PUSH,
    ZYDIS_CATEGORY_POP,
    ZYDIS_CATEGORY_CALL,
    ZYDIS_CATEGORY_RET,
    ZYDIS_CATEGORY_COND_BR,
    ZYDIS_CATEGORY_UNCOND_BR,
    ZYDIS_CATEGORY_STRINGOP,
    ZYDIS_CATEGORY_X87_ALU,
    ZYDIS_CATEGORY_MMX,
    ZYDIS_CATEGORY_SSE,
    ZYDIS_CATEGORY_SYSTEM,
    ZYDIS_CATEGORY_INTERRUPT,
    ZYDIS_CATEGORY_NOP,
    ZYDIS_CATEGORY_SEMAPHORE,
    ZYDIS_CATEGORY_FLAGOP,
    ZYDIS_CATEGORY_SHIFT,
    ZYDIS_CATEGORY_BITBYTE,
    ZYDIS_CATEGORY_CMOV,
    ZYDIS_CATEGORY_CONVERT,
    ZYDIS_CATEGORY_IO,
    ZYDIS_CATEGORY_MISC,
};

enum ZydisOperandType : uint8_t {
    ZYDIS_OPERAND_TYPE_UNUSED = 0,
    ZYDIS_OPERAND_TYPE_REGISTER,
    ZYDIS_OPERAND_TYPE_MEMORY,
    ZYDIS_OPERAND_TYPE_POINTER,
    ZYDIS_OPERAND_TYPE_IMMEDIATE,
};

enum ZydisOperandVisibility : uint8_t {
    ZYDIS_OPERAND_VISIBILITY_INVALID = 0,
    ZYDIS_OPERAND_VISIBILITY_EXPLICIT,
    ZYDIS_OPERAND_VISIBILITY_IMPLICIT,
    ZYDIS_OPERAND_VISIBILITY_HIDDEN,
};

enum : uint8_t {
    ZYDIS_OPERAND_ACTION_READ = 0x01,
    ZYDIS_OPERAND_ACTION_WRITE = 0x02,
    ZYDIS_OPERAND_ACTION_CONDREAD = 0x04,
    ZYDIS_OPERAND_ACTION_CONDWRITE = 0x08,
    ZYDIS_OPERAND_ACTION_READWRITE = 0x03,
    ZYDIS_OPERAND_ACTION_MASK_READ = 0x05,
    ZYDIS_OPERAND_ACTION_MASK_WRITE = 0x0A,
};
typedef uint8_t ZydisOperandActions;

enum ZydisMemoryOperandType : uint8_t {
    ZYDIS_MEMOP_TYPE_INVALID = 0,
    ZYDIS_MEMOP_TYPE_MEM,
    ZYDIS_MEMOP_TYPE_AGEN,  // lea
};

typedef uint64_t ZydisInstructionAttributes;
#define ZYDIS_ATTRIB_HAS_MODRM (1ull << 0)
#define ZYDIS_ATTRIB_HAS_SIB (1ull << 1)
#define ZYDIS_ATTRIB_HAS_LOCK (1ull << 27)
#define ZYDIS_ATTRIB_HAS_REP (1ull << 28)
#define ZYDIS_ATTRIB_HAS_REPE (1ull << 29)
#define ZYDIS_ATTRIB_HAS_REPZ ZYDIS_ATTRIB_HAS_REPE
#define ZYDIS_ATTRIB_HAS_REPNE (1ull << 30)
#define ZYDIS_ATTRIB_HAS_REPNZ ZYDIS_ATTRIB_HAS_REPNE
#define ZYDIS_ATTRIB_HAS_SEGMENT (1ull << 31)
#define ZYDIS_ATTRIB_HAS_OPERANDSIZE (1ull << 32)
#define ZYDIS_ATTRIB_HAS_ADDRESSSIZE (1ull << 33)

// EFLAGS bits, as Zydis's ZYDIS_CPUFLAG masks
typedef uint32_t ZydisCPUFlags;
struct ZydisAccessedFlags {
    ZydisCPUFlags tested;
    ZydisCPUFlags modified;
    ZydisCPUFlags set_0;
    ZydisCPUFlags set_1;
    ZydisCPUFlags undefined;
};

struct ZydisDecodedOperandReg {
    ZydisRegister value;
};

struct ZydisDecodedOperandMem {
    ZydisMemoryOperandType type;
    ZydisRegister segment;
    ZydisRegister base;
    ZydisRegister index;
    ZyanU8 scale;
    struct {
        ZyanBool has_displacement;
        ZyanI64 value;
    } disp;
};

struct ZydisDecodedOperandPtr {
    ZyanU16 segment;
    ZyanU32 offset;
};

struct ZydisDecodedOperandImm {
    ZyanBool is_signed;
    ZyanBool is_relative;
    union {
        ZyanU64 u;
        ZyanI64 s;
    } value;
};

struct ZydisDecodedOperand {
    ZyanU8 id;
    ZydisOperandVisibility visibility;
    ZydisOperandActions actions;
    ZyanU16 size;  // bits: register width, memory access, immediate operand width
    ZydisOperandType type;
    union {
        ZydisDecodedOperandReg reg;
        ZydisDecodedOperandMem mem;
        ZydisDecodedOperandPtr ptr;
        ZydisDecodedOperandImm imm;
    };
};

struct ZydisDecodedInstructionRawImm {
    ZyanBool is_signed;
    ZyanBool is_relative;
    union {
        ZyanU64 u;
        ZyanI64 s;
    } value;
    ZyanU8 size;    // bits, 0 = none
    ZyanU8 offset;  // bytes from the instruction start
};

struct ZydisDecodedInstruction {
    ZydisMachineMode machine_mode;
    ZydisMnemonic mnemonic;
    ZyanU8 length;
    ZyanU8 opcode_map;  // 0 one byte, 1 0F
    ZyanU8 opcode;
    ZyanU8 stack_width;
    ZyanU8 operand_width;
    ZyanU8 address_width;
    ZyanU8 operand_count;
    ZyanU8 operand_count_visible;
    ZydisInstructionAttributes attributes;
    const ZydisAccessedFlags* cpu_flags;
    struct {
        ZydisInstructionCategory category;
    } meta;
    struct {
        ZyanU8 prefix_count;
        struct {
            ZyanU8 mod, reg, rm, offset;
        } modrm;
        struct {
            ZyanU8 scale, index, base, offset;
        } sib;
        struct {
            ZyanI64 value;
            ZyanU8 size;    // bits
            ZyanU8 offset;  // bytes
        } disp;
        ZydisDecodedInstructionRawImm imm[2];
    } raw;
};

struct ZydisDecoder {
    ZydisMachineMode machine_mode;
    ZydisStackWidth stack_width;
};

// what DecodeInstruction keeps for DecodeOperands; the operands are decoded at once here
struct ZydisDecoderContext {
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
};

enum ZydisFormatterStyle { ZYDIS_FORMATTER_STYLE_INTEL = 1 };
struct ZydisFormatter {
    ZydisFormatterStyle style;
};

ZyanStatus ZydisDecoderInit(ZydisDecoder* decoder, ZydisMachineMode machine_mode, ZydisStackWidth stack_width);
ZyanStatus ZydisDecoderDecodeFull(const ZydisDecoder* decoder, const void* buffer, ZyanUSize length,
                                  ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operands);
ZyanStatus ZydisDecoderDecodeInstruction(const ZydisDecoder* decoder, ZydisDecoderContext* context,
                                         const void* buffer, ZyanUSize length, ZydisDecodedInstruction* instruction);
const char* ZydisMnemonicGetString(ZydisMnemonic mnemonic);
const char* ZydisRegisterGetString(ZydisRegister reg);
ZydisRegisterWidth ZydisRegisterGetWidth(ZydisMachineMode mode, ZydisRegister reg);
ZydisRegister ZydisRegisterGetLargestEnclosing(ZydisMachineMode mode, ZydisRegister reg);
ZyanStatus ZydisFormatterInit(ZydisFormatter* formatter, ZydisFormatterStyle style);
ZyanStatus ZydisFormatterFormatInstruction(const ZydisFormatter* formatter, const ZydisDecodedInstruction* instruction,
                                           const ZydisDecodedOperand* operands, ZyanU8 operand_count, char* buffer,
                                           ZyanUSize length, ZyanU64 runtime_address, void* user_data);
