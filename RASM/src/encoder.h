#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "token.h"
#include "symbol.h"

namespace rasm {

// 操作数类型
enum class OpKind {
    Reg,        // 寄存器
    Imm,        // 立即数
    Mem,        // 内存 [base + index*scale + disp]
    LabelRef,   // 对某标签的引用（用于相对跳转）
};

struct Operand {
    OpKind kind = OpKind::Imm;
    RegInfo reg;            // Reg / Mem.base
    RegInfo index;          // Mem.index
    int scale = 1;          // Mem.scale = 1,2,4,8
    int64_t imm = 0;        // Imm / Mem.disp
    std::string label;      // LabelRef
};

// 一条指令的 IR
struct Instr {
    std::string mnemonic;   // 小写助记符
    std::vector<Operand> ops;
    int line = 0;
};

// 编码结果
struct Encoded {
    std::vector<uint8_t> bytes;
    // 记录哪些位置是重定位/前向引用，Pass 2 回填
    enum class RelocKind { Rel32, Abs32 };
    struct Reloc { size_t offset; std::string label; int width; RelocKind kind; };
    std::vector<Reloc> relocs;
};

class Encoder {
public:
    // 编码一条指令
    // currentRva: 当前指令在段内的偏移（用于计算相对跳转）
    static Encoded encode(const Instr& in, int64_t currentRva, class SymbolTable& sym);
};

} // namespace rasm
