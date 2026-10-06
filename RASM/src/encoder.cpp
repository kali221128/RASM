#include "encoder.h"
#include "common.h"
#include "symbol.h"
#include <stdexcept>
#include <cstring>

namespace rasm {

namespace {

// 构造 ModR/M 字节
inline uint8_t makeModRM(uint8_t mod, uint8_t reg, uint8_t rm) {
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

// 判断 imm 是否能放进 8 位有符号
inline bool fitsInt8(int64_t v) { return v >= -128 && v <= 127; }
inline bool fitsInt16(int64_t v){ return v >= -32768 && v <= 32767; }

// 写 4 字节 disp32；如果 label 非空且未知，写 0 并记录 Abs32 reloc
inline void writeDisp32(Encoded& out, uint32_t d, const std::string& label, SymbolTable& sym) {
    if (!label.empty() && !sym.has(label)) {
        out.bytes.push_back(0);
        out.bytes.push_back(0);
        out.bytes.push_back(0);
        out.bytes.push_back(0);
        out.relocs.push_back({out.bytes.size() - 4, label, 4, Encoded::RelocKind::Abs32});
    } else {
        if (!label.empty()) {
            d = (uint32_t)sym.get(label);
        }
        out.bytes.push_back(d&0xff);
        out.bytes.push_back((d>>8)&0xff);
        out.bytes.push_back((d>>16)&0xff);
        out.bytes.push_back((d>>24)&0xff);
    }
}

// 根据 Mem 操作数生成 ModR/M + SIB + disp
// regField 是 ModR/M 的 reg 位（0-7）
inline void encodeMem(Encoded& out, uint8_t regField, const Operand& mem, SymbolTable& sym) {
    bool hasBase = (mem.reg.cls != RegClass::None);
    bool hasIndex = (mem.index.cls != RegClass::None);
    int64_t disp = mem.imm;

    // 确定 mod
    uint8_t mod;
    if (disp == 0) {
        mod = 0;
    } else if (disp >= -128 && disp <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }

    if (!hasBase && !hasIndex) {
        out.bytes.push_back(makeModRM(0, regField, 5));
        writeDisp32(out, (uint32_t)disp, mem.label, sym);
        return;
    }

    if (!hasIndex) {
        uint8_t rm = mem.reg.code;
        if (rm == 4) {
            out.bytes.push_back(makeModRM(mod, regField, 4));
            uint8_t sib = (0 << 6) | (4 << 3) | rm;
            out.bytes.push_back(sib);
        } else if (rm == 5 && mod == 0) {
            out.bytes.push_back(makeModRM(0, regField, 5));
            writeDisp32(out, (uint32_t)disp, mem.label, sym);
            return;
        } else {
            out.bytes.push_back(makeModRM(mod, regField, rm));
        }
        if (mod == 1) out.bytes.push_back((uint8_t)(disp & 0xff));
        else if (mod == 2) writeDisp32(out, (uint32_t)disp, mem.label, sym);
        return;
    }

    // 有 base 和 index -> SIB
    uint8_t scale = 0;
    if (mem.scale == 2) scale = 1;
    else if (mem.scale == 4) scale = 2;
    else if (mem.scale == 8) scale = 3;

    out.bytes.push_back(makeModRM(mod, regField, 4));
    uint8_t sib = (scale << 6) | ((mem.index.code & 7) << 3) | (mem.reg.code & 7);
    out.bytes.push_back(sib);

    if (mod == 1) out.bytes.push_back((uint8_t)(disp & 0xff));
    else if (mod == 2) writeDisp32(out, (uint32_t)disp, mem.label, sym);
}

} // namespace

Encoded Encoder::encode(const Instr& in, int64_t cur, SymbolTable& sym) {
    Encoded out;
    const auto& m = in.mnemonic;
    const auto& ops = in.ops;

    // ---- 无操作数指令 ----
    if (ops.empty()) {
        if (m == "nop") { out.bytes.push_back(0x90); return out; }
        if (m == "ret") { out.bytes.push_back(0xC3); return out; }
        if (m == "retn"){ out.bytes.push_back(0xC3); return out; }
        if (m == "hlt") { out.bytes.push_back(0xF4); return out; }
        if (m == "int3"){ out.bytes.push_back(0xCC); return out; }
        if (m == "cdq") { out.bytes.push_back(0x99); return out; }
        if (m == "cwd") { out.bytes.push_back(0x66); out.bytes.push_back(0x99); return out; }
        if (m == "cld") { out.bytes.push_back(0xFC); return out; }
        if (m == "std") { out.bytes.push_back(0xFD); return out; }
        if (m == "clc") { out.bytes.push_back(0xF8); return out; }
        if (m == "stc") { out.bytes.push_back(0xF9); return out; }
        if (m == "cli") { out.bytes.push_back(0xFA); return out; }
        if (m == "sti") { out.bytes.push_back(0xFB); return out; }
        if (m == "rep.movsb" || m == "rep movsb" || m == "movsb") { out.bytes.push_back(0xF3); out.bytes.push_back(0xA4); return out; }
        if (m == "rep.movsd" || m == "rep movsd" || m == "movsd") { out.bytes.push_back(0xF3); out.bytes.push_back(0xA5); return out; }
        if (m == "rep.stosb" || m == "rep stosb" || m == "stosb") { out.bytes.push_back(0xF3); out.bytes.push_back(0xAA); return out; }
        if (m == "rep.stosd" || m == "rep stosd" || m == "stosd") { out.bytes.push_back(0xF3); out.bytes.push_back(0xAB); return out; }
        if (m == "cmpsb") { out.bytes.push_back(0xA6); return out; }
        if (m == "cmpsd") { out.bytes.push_back(0xA7); return out; }
        if (m == "lodsb") { out.bytes.push_back(0xAC); return out; }
        if (m == "lodsd") { out.bytes.push_back(0xAD); return out; }
        if (m == "scasb") { out.bytes.push_back(0xAE); return out; }
        if (m == "scasd") { out.bytes.push_back(0xAF); return out; }
        if (m == "lodsb") { out.bytes.push_back(0xAC); return out; }
        if (m == "lodsd") { out.bytes.push_back(0xAD); return out; }
        if (m == "scasb") { out.bytes.push_back(0xAE); return out; }
        if (m == "scasd") { out.bytes.push_back(0xAF); return out; }
        if (m == "pushad") { out.bytes.push_back(0x60); return out; }
        if (m == "popad")  { out.bytes.push_back(0x61); return out; }
        if (m == "pushfd") { out.bytes.push_back(0x9C); return out; }
        if (m == "popfd")  { out.bytes.push_back(0x9D); return out; }
        if (m == "leave")  { out.bytes.push_back(0xC9); return out; }
        if (m == "ret")    { out.bytes.push_back(0xC3); return out; }
        if (m == "retf")   { out.bytes.push_back(0xCB); return out; }
        if (m == "salc")   { out.bytes.push_back(0xD6); return out; }
        if (m == "fld1")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xE8}); return out; }
        if (m == "fldz")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xEE}); return out; }
        if (m == "fldpi")  { out.bytes.insert(out.bytes.end(), {0xD9, 0xEB}); return out; }
        if (m == "fldl2t") { out.bytes.insert(out.bytes.end(), {0xD9, 0xE9}); return out; }
        if (m == "fldln2") { out.bytes.insert(out.bytes.end(), {0xD9, 0xED}); return out; }
        if (m == "finit")  { out.bytes.insert(out.bytes.end(), {0xDB, 0xE3}); return out; }
        if (m == "fwait" || m == "wait") { out.bytes.push_back(0x9B); return out; }
        if (m == "fnop")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xD0}); return out; }
        if (m == "fchs")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xE0}); return out; }
        if (m == "fabs")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xE1}); return out; }
        if (m == "fsqrt")  { out.bytes.insert(out.bytes.end(), {0xD9, 0xFA}); return out; }
        if (m == "fsin")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xFE}); return out; }
        if (m == "fcos")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xFF}); return out; }
        if (m == "fcom")   { out.bytes.insert(out.bytes.end(), {0xD8, 0xD1}); return out; }
        if (m == "fcomp")  { out.bytes.insert(out.bytes.end(), {0xD8, 0xD9}); return out; }
        if (m == "fcompp") { out.bytes.insert(out.bytes.end(), {0xDE, 0xD9}); return out; }
        if (m == "ftst")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xE4}); return out; }
        if (m == "fxam")   { out.bytes.insert(out.bytes.end(), {0xD9, 0xE5}); return out; }
        if (m == "fnclex") { out.bytes.insert(out.bytes.end(), {0xDB, 0xE2}); return out; }
        if (m == "fninit") { out.bytes.insert(out.bytes.end(), {0xDB, 0xE3}); return out; }
        if (m == "fldl2e") { out.bytes.insert(out.bytes.end(), {0xD9, 0xEA}); return out; }
        if (m == "fldlg2") { out.bytes.insert(out.bytes.end(), {0xD9, 0xEC}); return out; }
        if (m == "frndint"){ out.bytes.insert(out.bytes.end(), {0xD9, 0xFC}); return out; }
        if (m == "fxtract"){ out.bytes.insert(out.bytes.end(), {0xD9, 0xF4}); return out; }
        if (m == "fscale") { out.bytes.insert(out.bytes.end(), {0xDD, 0xFC}); return out; }
        if (m == "fprem")  { out.bytes.insert(out.bytes.end(), {0xD9, 0xF8}); return out; }
        if (m == "fprem1") { out.bytes.insert(out.bytes.end(), {0xDB, 0xF5}); return out; }
        if (m == "fdecstp"){ out.bytes.insert(out.bytes.end(), {0xD9, 0xF4}); return out; }
        if (m == "fincstp"){ out.bytes.insert(out.bytes.end(), {0xD9, 0xF7}); return out; }
        // FPU 弹出形式（默认 st1,st0）
        if (m == "faddp")  { out.bytes.insert(out.bytes.end(), {0xDE, 0xC1}); return out; }
        if (m == "fmulp")  { out.bytes.insert(out.bytes.end(), {0xDE, 0xC9}); return out; }
        if (m == "fsubp")  { out.bytes.insert(out.bytes.end(), {0xDE, 0xE9}); return out; }
        if (m == "fdivp")  { out.bytes.insert(out.bytes.end(), {0xDE, 0xF9}); return out; }
        if (m == "fnstsw")  { out.bytes.insert(out.bytes.end(), {0xDF, 0xE0}); return out; }
        if (m == "fnstcw")  { out.bytes.insert(out.bytes.end(), {0xD9, 0x7D}); return out; }
        if (m == "leave")  { out.bytes.push_back(0xC9); return out; }
        if (m == "not")    { /* 单操作数 */ }
        if (m == "neg")    { /* 单操作数 */ }
        throw AsmError("Unknown mnemonic '" + m + "' with no operands", in.line);
    }

    // ---- 单操作数指令 ----
    if (ops.size() == 1) {
        const auto& a = ops[0];
        // ret imm16: C2 iw
        if (m == "ret" && a.kind == OpKind::Imm) {
            out.bytes.push_back(0xC2);
            out.bytes.push_back((uint8_t)(a.imm & 0xff));
            out.bytes.push_back((uint8_t)((a.imm >> 8) & 0xff));
            return out;
        }
        if (m == "push") {
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR32) {
                out.bytes.push_back((uint8_t)(0x50 | a.reg.code));
                return out;
            }
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR16) {
                out.bytes.push_back(0x66);
                out.bytes.push_back((uint8_t)(0x50 | a.reg.code));
                return out;
            }
            if (a.kind == OpKind::Imm) {
                // 能放进 8 位有符号就用 6A ib（2 字节），否则 68 id（5 字节）
                if (a.imm >= -128 && a.imm <= 127) {
                    out.bytes.push_back(0x6A);
                    out.bytes.push_back((uint8_t)(a.imm & 0xff));
                } else {
                    out.bytes.push_back(0x68);
                    uint32_t v = (uint32_t)a.imm;
                    out.bytes.push_back(v & 0xff);
                    out.bytes.push_back((v>>8)&0xff);
                    out.bytes.push_back((v>>16)&0xff);
                    out.bytes.push_back((v>>24)&0xff);
                }
                return out;
            }
            // push 标签地址：push imm32，记 Abs32 reloc
            if (a.kind == OpKind::LabelRef) {
                out.bytes.push_back(0x68);
                out.bytes.push_back(0);
                out.bytes.push_back(0);
                out.bytes.push_back(0);
                out.bytes.push_back(0);
                out.relocs.push_back({out.bytes.size() - 4, a.label, 4, Encoded::RelocKind::Abs32});
                return out;
            }
        }
        if (m == "pop") {
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR32) {
                out.bytes.push_back((uint8_t)(0x58 | a.reg.code));
                return out;
            }
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR16) {
                out.bytes.push_back(0x66);
                out.bytes.push_back((uint8_t)(0x58 | a.reg.code));
                return out;
            }
        }
        if (m == "inc" || m == "dec") {
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR32) {
                out.bytes.push_back((uint8_t)((m=="inc"?0x40:0x48) | a.reg.code));
                return out;
            }
        }
        // bswap r32: 0F C8+r
        if (m == "bswap" && a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR32) {
            out.bytes.insert(out.bytes.end(), {0x0F, (uint8_t)(0xC8 | a.reg.code)});
            return out;
        }
        // not/neg r32: F7 /2, /3
        if (m == "not" || m == "neg") {
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR32) {
                out.bytes.push_back(0xF7);
                uint8_t sub = (m == "not") ? 2 : 3;
                out.bytes.push_back(makeModRM(3, sub, a.reg.code));
                return out;
            }
        }
        // div/idiv/mul r32: F7 /6, /7, /4
        if (m == "div" || m == "idiv" || m == "mul") {
            if (a.kind == OpKind::Reg && a.reg.cls == RegClass::GPR32) {
                out.bytes.push_back(0xF7);
                uint8_t sub = (m == "div") ? 6 : (m == "idiv") ? 7 : 4;
                out.bytes.push_back(makeModRM(3, sub, a.reg.code));
                return out;
            }
        }
        // call [mem] 间接调用：FF /2
        if (m == "call" && a.kind == OpKind::Mem) {
            out.bytes.push_back(0xFF);
            encodeMem(out, 2, a, sym);
            return out;
        }
        // jmp [mem] 间接跳转：FF /4
        if (m == "jmp" && a.kind == OpKind::Mem) {
            out.bytes.push_back(0xFF);
            encodeMem(out, 4, a, sym);
            return out;
        }

        // loop/loope/loopne/jecxz：短跳转 rel8
        if ((m == "loop" || m == "loope" || m == "loopz" || m == "loopne" || m == "loopnz" || m == "jecxz")
            && a.kind == OpKind::LabelRef) {
            uint8_t op;
            if (m == "loop")  op = 0xE2;
            else if (m == "loope" || m == "loopz") op = 0xE1;
            else if (m == "loopne" || m == "loopnz") op = 0xE0;
            else op = 0xE3; // jecxz
            out.bytes.push_back(op);
            out.bytes.push_back(0); // rel8 占位
            if (!sym.has(a.label)) {
                out.relocs.push_back({out.bytes.size()-1, a.label, 1, Encoded::RelocKind::Rel32});
            } else {
                int64_t target = sym.get(a.label);
                int8_t disp = (int8_t)(target - cur - 2);
                out.bytes.back() = (uint8_t)disp;
            }
            return out;
        }
        // 相对跳转：jmp/jz/jnz/call 等
        if (m == "jmp" || m == "call" || m == "jz" || m == "je" ||
            m == "jnz" || m == "jne" || m == "jg" || m == "jge" ||
            m == "jl" || m == "jle" || m == "ja" || m == "jae" ||
            m == "jb" || m == "jbe" || m == "js" || m == "jns") {
            if (a.kind == OpKind::LabelRef) {
                // rel32（先按 32 位编码，Pass 2 时若距离近再缩短为 8 位）
                uint8_t op1 = 0, op2 = 0;
                bool twoByte = true;
                if (m == "jmp")  { op1 = 0xE9; twoByte = false; }
                else if (m == "call") { op1 = 0xE8; twoByte = false; }
                else if (m == "jz"  || m == "je")  { op1 = 0x0F; op2 = 0x84; }
                else if (m == "jnz" || m == "jne") { op1 = 0x0F; op2 = 0x85; }
                else if (m == "jg")  { op1 = 0x0F; op2 = 0x8F; }
                else if (m == "jge") { op1 = 0x0F; op2 = 0x8D; }
                else if (m == "jl")  { op1 = 0x0F; op2 = 0x8C; }
                else if (m == "jle") { op1 = 0x0F; op2 = 0x8E; }
                else if (m == "ja")  { op1 = 0x0F; op2 = 0x87; }
                else if (m == "jae") { op1 = 0x0F; op2 = 0x83; }
                else if (m == "jb")  { op1 = 0x0F; op2 = 0x82; }
                else if (m == "jbe") { op1 = 0x0F; op2 = 0x86; }
                else if (m == "js")  { op1 = 0x0F; op2 = 0x88; }
                else if (m == "jns") { op1 = 0x0F; op2 = 0x89; }

                out.bytes.push_back(op1);
                if (twoByte) out.bytes.push_back(op2);
                size_t dispPos = out.bytes.size();
                int32_t disp = 0;
                if (sym.has(a.label)) {
                    int64_t target = sym.get(a.label);
                    int64_t nextIp = cur + (twoByte ? 6 : 5); // op + 2B opcode + rel32 = 6B; jmp/call = 5B
                    disp = (int32_t)(target - nextIp);
                }
                out.bytes.push_back((uint8_t)(disp & 0xff));
                out.bytes.push_back((uint8_t)((disp>>8)&0xff));
                out.bytes.push_back((uint8_t)((disp>>16)&0xff));
                out.bytes.push_back((uint8_t)((disp>>24)&0xff));
                if (!sym.has(a.label)) {
                    out.relocs.push_back({dispPos, a.label, 4, Encoded::RelocKind::Rel32});
                }
                return out;
            }
        }
        throw AsmError("Bad operands for '" + m + "'", in.line);
    }

    // ---- 两操作数指令 ----
    // 三操作数：cmpps/shufps xmm, xmm, imm8
    if (ops.size() == 3) {
        const auto& dst = ops[0];
        const auto& src1 = ops[1];
        const auto& src2 = ops[2];
        if ((m == "cmpps" || m == "shufps") &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src1.kind == OpKind::Reg && src1.reg.cls == RegClass::XMM &&
            src2.kind == OpKind::Imm) {
            uint8_t op = (m == "cmpps") ? 0xC2 : 0xC6;
            out.bytes.push_back(0x0F);
            out.bytes.push_back(op);
            out.bytes.push_back(makeModRM(3, dst.reg.code, src1.reg.code));
            out.bytes.push_back((uint8_t)(src2.imm & 0xff));
            return out;
        }
    }

    if (ops.size() == 2) {
        const auto& dst = ops[0];
        const auto& src = ops[1];

        // cvtsi2ss xmm, r32: F3 0F 2A /r
        if (m == "cvtsi2ss" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.insert(out.bytes.end(), {0xF3, 0x0F, 0x2A});
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // cvtss2si r32, xmm: F3 0F 2C /r
        if (m == "cvtss2si" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::XMM) {
            out.bytes.insert(out.bytes.end(), {0xF3, 0x0F, 0x2C});
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // SSE2 整数 SIMD：paddb/paddw/paddd/psubb/psubw/psubd
        if ((m == "paddb" || m == "paddw" || m == "paddd" ||
             m == "psubb" || m == "psubw" || m == "psubd" ||
             m == "pcmpeqb") &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::XMM) {
            uint8_t op = 0xFF;
            if (m == "paddb") op = 0xFC;
            else if (m == "paddw") op = 0xFD;
            else if (m == "paddd") op = 0xFE;
            else if (m == "psubb") op = 0xF8;
            else if (m == "psubw") op = 0xF9;
            else if (m == "psubd") op = 0xFA;
            else if (m == "pcmpeqb") op = 0x74;
            else if (m == "pmullw") op = 0xD5;
            else if (m == "punpcklbw") op = 0x60;
            else if (m == "punpckhbw") op = 0x68;
            else if (m == "addsubps") op = 0xD0;
            else if (m == "haddps") op = 0x7C;
            else if (m == "packssdw") op = 0x63;
            // SSE2 整数指令需要 0x66 prefix，SSE3 需要 0xF2
            uint8_t px = (m == "addsubps" || m == "haddps") ? 0xF2 : 0x66;
            out.bytes.push_back(px);
            out.bytes.push_back(0x0F);
            out.bytes.push_back(op);
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // pmuludq xmm,xmm: 0F F4（无 prefix）
        if (m == "pmuludq" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::XMM) {
            out.bytes.insert(out.bytes.end(), {0x0F, 0xF4});
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }

        // adc/sbb [mem], r32: 13 / 19
        if ((m == "adc" || m == "sbb") && dst.kind == OpKind::Mem &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.push_back((uint8_t)(m == "adc" ? 0x13 : 0x19));
            encodeMem(out, src.reg.code, dst, sym);
            return out;
        }
        // 8位寄存器运算：add/or/adc/sbb/and/sub/xor/cmp r8,r8
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR8 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR8) {
            uint8_t op = 0;
            if (m == "add") op = 0x00; else if (m == "or") op = 0x08;
            else if (m == "adc") op = 0x10; else if (m == "sbb") op = 0x18;
            else if (m == "and") op = 0x20; else if (m == "sub") op = 0x28;
            else if (m == "xor") op = 0x30; else if (m == "cmp") op = 0x38;
            if (op) {
                out.bytes.push_back(op);
                out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                return out;
            }
        }

        // psllw/psraw xmm, imm8: 66 0F 71 /6 /4 ib
        if ((m == "psllw" || m == "psraw" || m == "pslld" || m == "psrld") &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Imm) {
            uint8_t sub = 6;  // psllw
            if (m == "psraw") sub = 4;
            else if (m == "pslld") sub = 6;
            else if (m == "psrld") sub = 5;
            out.bytes.insert(out.bytes.end(), {0x66, 0x0F, 0x71});
            out.bytes.push_back(makeModRM(3, sub, dst.reg.code));
            out.bytes.push_back((uint8_t)(src.imm & 0xff));
            return out;
        }

        // FPU x87 指令：fld/fst/fstp/fadd/fmul/fsub/fdiv st(i)
        // 操作数是 st0-st7（解析为 Reg GPR32，code 0-7）
        auto fpuOp = [](const std::string& op) -> int {
            if (op.size() >= 3 && op[0] == 's' && op[1] == 't' && op[2] >= '0' && op[2] <= '7')
                return op[2] - '0';
            return -1;
        };
        if (dst.kind == OpKind::Reg && src.kind == OpKind::Reg) {
            // 尝试从寄存器名解析 st(i)
            // parser 把 st0 当 GPR32? 不一定，先按名字匹配
        }

        // FPU x87 指令：fld/fst/fstp/fadd/fmul/fsub/fdiv st, st(i)
        // ModR/M: mod=11, reg=FPU opcode, r/m=st(i)
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::FPU &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::FPU) {
            uint8_t rm = src.reg.code;
            uint8_t modrm = (uint8_t)(0xC0 | (0 << 3) | rm);  // reg=0 默认
            uint8_t esc = 0xD8;
            if (m == "fld")   { esc = 0xD8; modrm = (uint8_t)(0xC0 | (0 << 3) | rm); }
            else if (m == "fst")  { esc = 0xDD; modrm = (uint8_t)(0xC0 | (2 << 3) | rm); }
            else if (m == "fstp") { esc = 0xDD; modrm = (uint8_t)(0xC0 | (3 << 3) | rm); }
            else if (m == "fadd") { esc = 0xD8; modrm = (uint8_t)(0xC0 | (0 << 3) | rm); }
            else if (m == "fmul") { esc = 0xD8; modrm = (uint8_t)(0xC0 | (1 << 3) | rm); }
            else if (m == "fsub") { esc = 0xD8; modrm = (uint8_t)(0xC0 | (4 << 3) | rm); }
            else if (m == "fdiv") { esc = 0xD8; modrm = (uint8_t)(0xC0 | (6 << 3) | rm); }
            out.bytes.push_back(esc);
            out.bytes.push_back(modrm);
            return out;
        }

        // GPR 补齐：adc/sbb/and/or/xor/cmp  reg,reg
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            uint8_t op = 0;
            if (m == "add") op = 0x01; else if (m == "or") op = 0x09;
            else if (m == "adc") op = 0x11; else if (m == "sbb") op = 0x19;
            else if (m == "and") op = 0x21; else if (m == "sub") op = 0x29;
            else if (m == "xor") op = 0x31; else if (m == "cmp") op = 0x39;
            if (op) {
                out.bytes.push_back(op);
                out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                return out;
            }
        }
        // rol/ror/rcl/rcr/shl/shr/sar  r32, imm8
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Imm) {
            uint8_t sub = 0;
            if (m == "rol") sub = 0; else if (m == "ror") sub = 1;
            else if (m == "rcl") sub = 2; else if (m == "rcr") sub = 3;
            else if (m == "shl") sub = 4; else if (m == "shr") sub = 5;
            else if (m == "sar") sub = 7;
            if (sub) {
                out.bytes.push_back(0xC1);
                out.bytes.push_back(makeModRM(3, sub, dst.reg.code));
                out.bytes.push_back((uint8_t)(src.imm & 0xff));
                return out;
            }
        }
        // cmpxchg r32,r32: 0F B0 /r
        if (m == "cmpxchg" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.insert(out.bytes.end(), {0x0F, 0xB0});
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // xadd r32,r32: 0F C1 /r
        if (m == "xadd" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.insert(out.bytes.end(), {0x0F, 0xC1});
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // shld/shrd r32,r32,imm8: 0F A4 / AD
        if ((m == "shld" || m == "shrd") && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32) {
            out.bytes.insert(out.bytes.end(), {0x0F, (uint8_t)(m == "shld" ? 0xA4 : 0xAD)});
            // 需要第三个操作数（imm8），简化：只支持两操作数形式
            return out;
        }
        // bsf/bsr r32,r32: 0F BC / BD
        if ((m == "bsf" || m == "bsr") && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.insert(out.bytes.end(), {0x0F, (uint8_t)(m == "bsf" ? 0xBC : 0xBD)});
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // bt/bts/btr/btc r32,r32
        if ((m == "bt" || m == "bts" || m == "btr" || m == "btc") &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            uint8_t op = 0xA3;
            if (m == "bts") op = 0xAB;
            else if (m == "btr") op = 0xB3;
            else if (m == "btc") op = 0xBB;
            out.bytes.insert(out.bytes.end(), {0x0F, op});
            out.bytes.push_back(makeModRM(3, src.reg.code, dst.reg.code));
            return out;
        }

        // bound r32, [mem]: 62 /r
        if (m == "bound" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Mem) {
            out.bytes.push_back(0x62);
            encodeMem(out, dst.reg.code, src, sym);
            return out;
        }
        // lss/lfs/lgs r32, [mem]: 0F B2/B0/B1 /r
        if ((m == "lss" || m == "lfs" || m == "lgs") &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Mem) {
            uint8_t op = 0xB2;
            if (m == "lfs") op = 0xB0;
            else if (m == "lgs") op = 0xB1;
            out.bytes.insert(out.bytes.end(), {0x0F, op});
            encodeMem(out, dst.reg.code, src, sym);
            return out;
        }

        // FPU 内存操作数：fld/fst/fstp/fild/fist/fistp [mem]
        if (dst.kind == OpKind::Mem) {
            uint8_t esc = 0xD9, regField = 0;
            if (m == "fld")   { esc = 0xD9; regField = 0; }
            if (m == "fst")   { esc = 0xDD; regField = 2; }
            if (m == "fstp")  { esc = 0xDD; regField = 3; }
            if (m == "fild")  { esc = 0xDF; regField = 0; }
            if (m == "fist")   { esc = 0xDF; regField = 2; }
            if (m == "fistp")  { esc = 0xDF; regField = 3; }
            if (m == "fnstenv"){ esc = 0xD9; regField = 6; }
            if (m == "fnsave") { esc = 0xDD; regField = 6; }
            if (m == "frstor") { esc = 0xDD; regField = 4; }
            if (m == "fiadd")  { esc = 0xDA; regField = 0; }
            if (m == "fimul")  { esc = 0xDA; regField = 1; }
            if (m == "fisub")  { esc = 0xDA; regField = 4; }
            if (m == "fidiv")  { esc = 0xDA; regField = 6; }
            if (m == "fnstsw") { esc = 0xDD; regField = 7; }
            out.bytes.push_back(esc);
            encodeMem(out, regField, dst, sym);
            return out;
        }

        // SSE 补全：movdqa/movdqu/movq/sqrtps/andps/orps/xorps/xorpd
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::XMM) {
            uint8_t op = 0xFF;
            uint8_t prefix = 0x0F;
            if (m == "movdqa") { prefix = 0x66; op = 0x6F; }
            else if (m == "movdqu") { prefix = 0xF3; op = 0x6F; }
            else if (m == "movq") op = 0x6F;
            else if (m == "sqrtps") op = 0x51;
            else if (m == "andps") op = 0x54;
            else if (m == "orps")  op = 0x56;
            else if (m == "xorps") op = 0x57;
            else if (m == "xorpd") { prefix = 0x66; op = 0x57; }
            else if (m == "minps") op = 0x5D;
            else if (m == "maxps") op = 0x5F;
            else if (m == "rcpps") op = 0x53;
            else if (m == "rsqrtps") op = 0x52;
            else if (m == "unpcklps") op = 0x14;
            else if (m == "unpckhps") op = 0x15;
            else if (m == "addss") { prefix = 0xF3; op = 0x58; }
            else if (m == "subss") { prefix = 0xF3; op = 0x5C; }
            else if (m == "mulss") { prefix = 0xF3; op = 0x59; }
            else if (m == "divss") { prefix = 0xF3; op = 0x5E; }
            else if (m == "movss") { prefix = 0xF3; op = 0x10; }
            else if (m == "movsd") { prefix = 0xF2; op = 0x10; }
            if (op != 0xFF) {
                if (prefix != 0x0F) out.bytes.push_back(prefix);
                out.bytes.push_back(0x0F);
                out.bytes.push_back(op);
                out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                return out;
            }
        }

        // FPU 补全：fchs/fabs/fsqrt/fcos/fsin（已在无操作数区处理）

        // SSE 指令：movups/movaps/addps/subps/mulps/divps xmm, xmm
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::XMM) {
            uint8_t opcode = 0x10;  // movups
            if (m == "movups") opcode = 0x10;
            else if (m == "movaps") opcode = 0x28;
            else if (m == "addps")  opcode = 0x58;
            else if (m == "subps")  opcode = 0x5C;
            else if (m == "mulps")  opcode = 0x59;
            else if (m == "divps")  opcode = 0x5E;
            out.bytes.push_back(0x0F);
            out.bytes.push_back(opcode);
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // SSE 内存操作数：movups xmm, [mem]
        if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::XMM &&
            src.kind == OpKind::Mem) {
            uint8_t opcode = 0x10;
            if (m == "movups") opcode = 0x10;
            else if (m == "movaps") opcode = 0x28;
            else if (m == "addps")  opcode = 0x58;
            else if (m == "subps")  opcode = 0x5C;
            else if (m == "mulps")  opcode = 0x59;
            else if (m == "divps")  opcode = 0x5E;
            out.bytes.push_back(0x0F);
            out.bytes.push_back(opcode);
            encodeMem(out, dst.reg.code, src, sym);
            return out;
        }

        // in/out 端口指令
        if (m == "in" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR8 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR16 && src.reg.code == 2 /*dx*/) {
            out.bytes.push_back(0xE4); return out;  // in al, imm8 简化
        }
        if (m == "in" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR16 && src.reg.code == 2) {
            out.bytes.push_back(0xE5); return out;
        }
        if (m == "out" && src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR8 &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR16 && dst.reg.code == 2) {
            out.bytes.push_back(0xE6); return out;
        }
        if (m == "out" && src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32 &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR16 && dst.reg.code == 2) {
            out.bytes.push_back(0xE7); return out;
        }

        // enter imm16, imm8
        if (m == "enter" && dst.kind == OpKind::Imm && src.kind == OpKind::Imm) {
            out.bytes.push_back(0xC8);
            uint16_t sz = (uint16_t)dst.imm;
            out.bytes.push_back(sz & 0xff);
            out.bytes.push_back((sz >> 8) & 0xff);
            out.bytes.push_back((uint8_t)src.imm);
            return out;
        }

        // 段寄存器 mov：mov r32, seg / mov seg, r32
        // 段寄存器编码：es=0, cs=1, ss=2, ds=3, fs=4, gs=5
        auto segCode = [](const std::string& s) -> int {
            if (s == "es") return 0; if (s == "cs") return 1;
            if (s == "ss") return 2; if (s == "ds") return 3;
            if (s == "fs") return 4; if (s == "gs") return 5;
            return -1;
        };
        if (m == "mov") {
            // mov r32, seg: 8C /r
            if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
                src.kind == OpKind::Reg && src.reg.cls == RegClass::Segment) {
                out.bytes.push_back(0x8C);
                out.bytes.push_back(makeModRM(3, src.reg.code, dst.reg.code));
                return out;
            }
            // mov seg, r32: 8E /r
            if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::Segment &&
                src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
                out.bytes.push_back(0x8E);
                out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                return out;
            }
        }

        // MOV 分支
        // movzx r32, r/m8|r/m16 ; movsx r32, r/m8|r/m16
        if (m == "movzx" || m == "movsx") {
            uint8_t opBase = (m == "movzx") ? 0xB6 : 0xBE; // /r 子操作码
            // 目标是 r32
            if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32) {
                // 源是 r/m8
                if ((src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR8) ||
                    (src.kind == OpKind::Mem)) {
                    out.bytes.push_back(0x0F);
                    out.bytes.push_back(opBase);
                    if (src.kind == OpKind::Reg) {
                        out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                    } else {
                        encodeMem(out, dst.reg.code, src, sym);
                    }
                    return out;
                }
                // 源是 r/m16（movzx 用 B7, movsx 用 BF）
                if ((src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR16) ||
                    (src.kind == OpKind::Mem)) {
                    out.bytes.push_back(0x0F);
                    out.bytes.push_back((uint8_t)(opBase + 1)); // B6->B7, BE->BF
                    if (src.kind == OpKind::Reg) {
                        out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                    } else {
                        encodeMem(out, dst.reg.code, src, sym);
                    }
                    return out;
                }
            }
            // movzx/movsx r16, r/m8
            if (dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR16) {
                out.bytes.push_back(0x66);
                out.bytes.push_back(0x0F);
                out.bytes.push_back(opBase);
                if (src.kind == OpKind::Reg) {
                    out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
                } else {
                    encodeMem(out, dst.reg.code, src, sym);
                }
                return out;
            }
        }
        if (m == "mov") {
            // mov r32, label（符号地址作为立即数，记 Abs32 reloc）
            if (dst.kind == OpKind::Reg && src.kind == OpKind::LabelRef &&
                dst.reg.cls == RegClass::GPR32) {
                out.bytes.push_back((uint8_t)(0xB8 | dst.reg.code));
                out.bytes.push_back(0);
                out.bytes.push_back(0);
                out.bytes.push_back(0);
                out.bytes.push_back(0);
                out.relocs.push_back({out.bytes.size() - 4, src.label, 4, Encoded::RelocKind::Abs32});
                return out;
            }
            // mov r, imm（通用：r8/r16/r32）
            if (dst.kind == OpKind::Reg && src.kind == OpKind::Imm) {
                if (dst.reg.cls == RegClass::GPR32) {
                    out.bytes.push_back((uint8_t)(0xB8 | dst.reg.code));
                    uint32_t v = (uint32_t)src.imm;
                    out.bytes.push_back(v&0xff);
                    out.bytes.push_back((v>>8)&0xff);
                    out.bytes.push_back((v>>16)&0xff);
                    out.bytes.push_back((v>>24)&0xff);
                    return out;
                }
                if (dst.reg.cls == RegClass::GPR16) {
                    out.bytes.push_back(0x66);
                    out.bytes.push_back((uint8_t)(0xB8 | dst.reg.code));
                    out.bytes.push_back((uint8_t)(src.imm & 0xff));
                    out.bytes.push_back((uint8_t)((src.imm>>8)&0xff));
                    return out;
                }
                if (dst.reg.cls == RegClass::GPR8) {
                    out.bytes.push_back((uint8_t)(0xB0 | dst.reg.code));
                    out.bytes.push_back((uint8_t)(src.imm & 0xff));
                    return out;
                }
            }
            // mov r, r（通用：根据 cls 选前缀和操作码）
            if (dst.kind == OpKind::Reg && src.kind == OpKind::Reg &&
                dst.reg.cls == src.reg.cls) {
                if (dst.reg.cls == RegClass::GPR32) {
                    out.bytes.push_back(0x89);
                    out.bytes.push_back(makeModRM(3, src.reg.code, dst.reg.code));
                    return out;
                }
                if (dst.reg.cls == RegClass::GPR16) {
                    out.bytes.push_back(0x66);
                    out.bytes.push_back(0x89);
                    out.bytes.push_back(makeModRM(3, src.reg.code, dst.reg.code));
                    return out;
                }
                if (dst.reg.cls == RegClass::GPR8) {
                    out.bytes.push_back(0x88);
                    out.bytes.push_back(makeModRM(3, src.reg.code, dst.reg.code));
                    return out;
                }
            }
            // mov r, [mem]（r8/r16/r32）
            if (dst.kind == OpKind::Reg && src.kind == OpKind::Mem) {
                if (dst.reg.cls == RegClass::GPR32) {
                    out.bytes.push_back(0x8B);
                    encodeMem(out, dst.reg.code, src, sym);
                    return out;
                }
                if (dst.reg.cls == RegClass::GPR16) {
                    out.bytes.push_back(0x66);
                    out.bytes.push_back(0x8B);
                    encodeMem(out, dst.reg.code, src, sym);
                    return out;
                }
                if (dst.reg.cls == RegClass::GPR8) {
                    out.bytes.push_back(0x8A);
                    encodeMem(out, dst.reg.code, src, sym);
                    return out;
                }
            }
            // mov [mem], r
            if (dst.kind == OpKind::Mem && src.kind == OpKind::Reg) {
                if (src.reg.cls == RegClass::GPR32) {
                    out.bytes.push_back(0x89);
                    encodeMem(out, src.reg.code, dst, sym);
                    return out;
                }
                if (src.reg.cls == RegClass::GPR16) {
                    out.bytes.push_back(0x66);
                    out.bytes.push_back(0x89);
                    encodeMem(out, src.reg.code, dst, sym);
                    return out;
                }
                if (src.reg.cls == RegClass::GPR8) {
                    out.bytes.push_back(0x88);
                    encodeMem(out, src.reg.code, dst, sym);
                    return out;
                }
            }
        }

        // LEA r32, [mem]
        if (m == "lea" && dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 && src.kind == OpKind::Mem) {
            out.bytes.push_back(0x8D);
            encodeMem(out, dst.reg.code, src, sym);
            return out;
        }

        // ADD/SUB/CMP/AND/OR/XOR: r32, r32
        static const std::unordered_map<std::string, uint8_t> rrOpcodes = {
            {"add",0x01},{"sub",0x29},{"cmp",0x39},
            {"and",0x21},{"or",0x09},{"xor",0x31},
            {"test",0x85},{"xchg",0x87}
        };
        auto it = rrOpcodes.find(m);
        if (it != rrOpcodes.end() &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.push_back(it->second);
            out.bytes.push_back(makeModRM(3, src.reg.code, dst.reg.code));
            return out;
        }
        // imul r32, r32: 0F AF /r
        if (m == "imul" &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Reg && src.reg.cls == RegClass::GPR32) {
            out.bytes.push_back(0x0F);
            out.bytes.push_back(0xAF);
            out.bytes.push_back(makeModRM(3, dst.reg.code, src.reg.code));
            return out;
        }
        // imul r32, imm: 6B /r ib (imm8) 或 69 /r id (imm32)
        if (m == "imul" &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 &&
            src.kind == OpKind::Imm) {
            if (src.imm >= -128 && src.imm <= 127) {
                out.bytes.push_back(0x6B);
                out.bytes.push_back(makeModRM(3, dst.reg.code, dst.reg.code));
                out.bytes.push_back((uint8_t)(src.imm & 0xff));
            } else {
                out.bytes.push_back(0x69);
                out.bytes.push_back(makeModRM(3, dst.reg.code, dst.reg.code));
                uint32_t v = (uint32_t)src.imm;
                out.bytes.push_back(v&0xff);
                out.bytes.push_back((v>>8)&0xff);
                out.bytes.push_back((v>>16)&0xff);
                out.bytes.push_back((v>>24)&0xff);
            }
            return out;
        }
        // shl/shr/sar r32, imm8: C1 /4, /5, /7
        static const std::unordered_map<std::string, uint8_t> shiftSub = {
            {"shl",4},{"shr",5},{"sar",7}
        };
        it = shiftSub.find(m);
        if (it != shiftSub.end() &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 && src.kind == OpKind::Imm) {
            out.bytes.push_back(0xC1);
            out.bytes.push_back(makeModRM(3, it->second, dst.reg.code));
            out.bytes.push_back((uint8_t)(src.imm & 0xff));
            return out;
        }
        // not/neg r32: F7 /2, /3（单操作数）
        // （在两操作数分支里不会到这里，单操作数分支已处理）
        // ADD/SUB/CMP/AND/OR/XOR: r32, imm32
        static const std::unordered_map<std::string, uint8_t> immSub = {
            {"add",0},{"or",1},{"adc",2},{"sbb",3},{"and",4},
            {"sub",5},{"xor",6},{"cmp",7}
        };
        it = immSub.find(m);
        if (it != immSub.end() &&
            dst.kind == OpKind::Reg && dst.reg.cls == RegClass::GPR32 && src.kind == OpKind::Imm) {
            // 短编码：imm 在 -128..127 时用 83 /0 ib（3 字节）
            if (src.imm >= -128 && src.imm <= 127) {
                out.bytes.push_back(0x83);
                out.bytes.push_back(makeModRM(3, it->second, dst.reg.code));
                out.bytes.push_back((uint8_t)(src.imm & 0xff));
            } else {
                out.bytes.push_back(0x81);
                out.bytes.push_back(makeModRM(3, it->second, dst.reg.code));
                uint32_t v = (uint32_t)src.imm;
                out.bytes.push_back(v&0xff);
                out.bytes.push_back((v>>8)&0xff);
                out.bytes.push_back((v>>16)&0xff);
                out.bytes.push_back((v>>24)&0xff);
            }
            return out;
        }

        throw AsmError("Unsupported operand combination for '" + m + "'", in.line);
    }

    throw AsmError("Unsupported mnemonic or operand count: " + m, in.line);
}

} // namespace rasm
