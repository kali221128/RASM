#pragma once
#include <string>
#include <vector>
#include "token.h"
#include "symbol.h"
#include "encoder.h"

namespace rasm {

// 汇编结果：各段字节
struct Section {
    std::string name;
    std::vector<uint8_t> data;
    uint32_t rva = 0;       // 段在内存中的 RVA（链接时填）
    uint32_t vaddr = 0;
    explicit Section(std::string n = "") : name(std::move(n)) {}
};

class Assembler {
public:
    explicit Assembler(const std::string& src);

    // 执行汇编，返回错误信息（空串=成功）
    std::string assemble();

    const Section& text() const { return text_; }
    const Section& data() const { return data_; }
    // 返回所有需要 Base Relocation 的 32 位绝对地址 RVA 列表
    std::vector<uint32_t> absFixupRvas() const;
    // 资源声明：type, name, 文件路径
    struct ResDecl { uint32_t type; uint32_t name; std::string path; };
    const std::vector<ResDecl>& resources() const { return resDecls_; }
    // 导出符号名列表
    const std::vector<std::string>& exports() const { return exportNames_; }
    bool guiMode() const { return guiMode_; }
    const SymbolTable& symbols() const { return sym_; }

    // 预定义符号（由 main.cpp 注入 import 函数地址）
    void defineSymbol(const std::string& name, int64_t value) {
        sym_.define(name, value, -1);  // section=-1 表示绝对地址
    }

    // 设置段基址（VA），由 main.cpp 在 PE layout 后调用
    void setBases(uint32_t textVA, uint32_t dataVA) {
        textBase_ = textVA;
        dataBase_ = dataVA;
    }

    // 设置源文件所在目录（用于 %include）
    void setBaseDir(const std::string& d) { baseDir_ = d; }

private:
    int curSection() const { return (cur_ == &data_) ? 1 : 0; }
    // 从 token 流解析一条操作数
    Operand parseOperand(std::vector<Token>& toks, size_t& i);
    // 解析一行：标签 + 指令/伪指令
    void parseLine(std::vector<Token>& toks, size_t& i);

    const std::string& src_;
    Section text_{".text"};
    Section data_{".data"};    SymbolTable sym_;

    // 当前写入的段指针
    Section* cur_ = nullptr;
    bool guiMode_ = false;

    // 重定位项：(在段中的偏移, 标签名, 宽度, 类型)
    struct Fixup {
        Section* sec;
        size_t offset;
        std::string label;
        int width;
        Encoded::RelocKind kind;
        std::vector<Token> expr;  // 若非空，回填时对这个表达式 token 流求值
    };
    std::vector<Fixup> fixups_;

    uint32_t textBase_ = 0x401000;
    uint32_t dataBase_ = 0x403000;
    std::string baseDir_ = ".";
    std::vector<Token> pendingExpr_;  // 最近一次解析到的表达式 token 流（用于 fixup 回填）
    std::string lastLabel_;  // 最近一次标签名（equ 覆盖用）
    std::string currentGlobal_;  // 当前全局标号（局部标号拼接用）
    std::vector<ResDecl> resDecls_;  // 资源声明
    std::vector<std::string> exportNames_;  // 导出符号
};

} // namespace rasm
