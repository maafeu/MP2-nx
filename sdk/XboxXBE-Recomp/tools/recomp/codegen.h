#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "analysis.h"

class CodeGen {
public:
    CodeGen(const Analysis& an, std::filesystem::path out) : an_(an), out_(std::move(out)) {}
    bool Run();

private:
    struct Op {
        const ZydisDecodedOperand* o = nullptr;
        std::string ea;  // name of the address temp when memory
    };

    std::string EmitFunction(uint32_t entry);
    bool CheckHooks(const std::vector<uint32_t>& funcs) const;
    int RetBytes(const std::vector<uint32_t>& body) const;  // the one ret n of a body, or -1
    void EmitHooks(uint32_t va, bool after);
    void EmitInsn(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, uint32_t next_body);

    // operand helpers
    std::string RegRead(ZydisRegister r) const;
    std::string RegWrite(ZydisRegister r, const std::string& v) const;
    std::string AddrExpr(const ZydisDecodedOperand& o, bool with_segment = true) const;
    std::string Read(const ZydisDecodedOperand& o, const std::string& ea) const;
    std::string Write(const ZydisDecodedOperand& o, const std::string& ea, const std::string& v) const;
    std::string Mem(unsigned bits, const std::string& ea) const;
    std::string CType(unsigned bits, bool is_signed = false) const;

    std::string Cond(ZydisMnemonic m);
    std::string Target(uint32_t t);  // goto or tail call
    std::string Hex(uint32_t v) const;
    std::string FuncName(uint32_t va) const;
    void Line(const std::string& s) { body_ += s; body_ += '\n'; }
    void Unimpl(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    std::string Text(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) const;

    // instruction groups
    bool EmitInteger(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitString(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitX87(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitSse(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitMmx(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);

    const Analysis& an_;
    std::filesystem::path out_;
    std::string body_;
    uint32_t entry_ = 0;
    std::set<uint32_t> body_set_;
    std::set<uint32_t> labels_;
    // cmp/test peephole
    struct Pending {
        bool valid = false;
        bool is_test = false;
        unsigned bits = 32;
    } pending_;
    std::map<std::string, size_t> unimpl_;
    size_t insn_count_ = 0;
    int ret_bytes_ = -1;

    // MaxPayne2-nx: the x87 stack cached in locals within a block (codegen_fp.cpp). From a block's first x87 instruction,
    // x87t holds ctx.top and the eight slots x87t + 0..7 live in x87r0..7, loaded on first read; the depth since then
    // is tracked here, so every st(i) is a fixed local. Labels, jumps, calls, returns, hooks and the x87 instructions
    // that read the stack another way write the dirty slots and ctx.top back first (XcFlush). Each operation computes
    // exactly what it did through ctx.st; only where the values wait between operations changes.
    bool xc_enabled_ = true;  // RECOMP_X87_CACHE=0 in the environment turns it off
    bool xc_on_ = false;
    int xc_rel_ = 0;  // ctx.top = x87t + xc_rel_ (mod 8)
    unsigned xc_loaded_ = 0, xc_dirty_ = 0;
    bool xc_used_ = false;  // this function uses the locals
    void XcFlush();
    std::string XcBegin();
    std::string XcGet(int i, std::string& pre);
    std::string XcSet(int i);
    std::string XcPush(const std::string& value);
    void XcPop() { xc_rel_ = (xc_rel_ + 1) & 7; }
    bool EmitX87Cached(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
};
