#pragma once
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <cstdint>

namespace rasm {

// 符号表：标签名 -> {值, 段编号 0=.text 1=.data}
class SymbolTable {
public:
    void define(const std::string& name, int64_t value, int section = 0);
    bool has(const std::string& name) const;
    int64_t get(const std::string& name) const;
    int sectionOf(const std::string& name) const;
    // 返回所有符号（用于 Map 文件）
    std::vector<std::pair<std::string, std::pair<int64_t, int>>> getAll() const;
private:
    struct Entry { int64_t value; int section; };
    std::unordered_map<std::string, Entry> table_;
};

// 寄存器识别与编码
enum class RegClass { None, GPR32, GPR16, GPR8, Segment, FPU, XMM };

struct RegInfo {
    RegClass cls = RegClass::None;
    uint8_t code = 0;   // x86 寄存器编号 0..7
};

RegInfo parseRegister(const std::string& name);
bool isRegister(const std::string& name);

} // namespace rasm
