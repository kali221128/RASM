#include "symbol.h"
#include "common.h"

namespace rasm {

void SymbolTable::define(const std::string& name, int64_t value, int section) {
    auto it = table_.find(name);
    if (it != table_.end()) {
        if (it->second.value != value)
            throw AsmError("Duplicate symbol '" + name + "'");
        return;
    }
    table_[name] = {value, section};
}

bool SymbolTable::has(const std::string& name) const {
    return table_.find(name) != table_.end();
}

int64_t SymbolTable::get(const std::string& name) const {
    auto it = table_.find(name);
    if (it == table_.end())
        throw AsmError("Undefined symbol '" + name + "'");
    return it->second.value;
}

int SymbolTable::sectionOf(const std::string& name) const {
    auto it = table_.find(name);
    if (it == table_.end())
        throw AsmError("Undefined symbol '" + name + "'");
    return it->second.section;
}

std::vector<std::pair<std::string, std::pair<int64_t, int>>> SymbolTable::getAll() const {
    std::vector<std::pair<std::string, std::pair<int64_t, int>>> r;
    for (auto& [name, e] : table_) r.push_back({name, {e.value, e.section}});
    return r;
}

RegInfo parseRegister(const std::string& n) {
    // 32 位通用寄存器
    static const std::unordered_map<std::string, uint8_t> r32 = {
        {"eax",0},{"ecx",1},{"edx",2},{"ebx",3},{"esp",4},{"ebp",5},{"esi",6},{"edi",7}
    };
    // 16 位
    static const std::unordered_map<std::string, uint8_t> r16 = {
        {"ax",0},{"cx",1},{"dx",2},{"bx",3},{"sp",4},{"bp",5},{"si",6},{"di",7}
    };
    // 8 位
    static const std::unordered_map<std::string, uint8_t> r8 = {
        {"al",0},{"cl",1},{"dl",2},{"bl",3},{"ah",4},{"ch",5},{"dh",6},{"bh",7}
    };
    // 段寄存器
    static const std::unordered_map<std::string, uint8_t> seg = {
        {"es",0},{"cs",1},{"ss",2},{"ds",3},{"fs",4},{"gs",5}
    };
    auto lower = n;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);

    auto it = r32.find(lower);
    if (it != r32.end()) return {RegClass::GPR32, it->second};
    it = r16.find(lower);
    if (it != r16.end()) return {RegClass::GPR16, it->second};
    it = r8.find(lower);
    if (it != r8.end()) return {RegClass::GPR8, it->second};
    it = seg.find(lower);
    if (it != seg.end()) return {RegClass::Segment, it->second};
    // FPU st0-st7
    if (lower.size() >= 3 && lower[0] == 's' && lower[1] == 't' &&
        lower[2] >= '0' && lower[2] <= '7' && lower.size() == 3) {
        return {RegClass::FPU, (uint8_t)(lower[2] - '0')};
    }
    // XMM xmm0-xmm15
    if (lower.size() >= 4 && lower.substr(0,3) == "xmm") {
        int v = atoi(lower.c_str() + 3);
        if (v >= 0 && v <= 15) return {RegClass::XMM, (uint8_t)v};
    }
    return {RegClass::None, 0};
}

bool isRegister(const std::string& name) {
    return parseRegister(name).cls != RegClass::None;
}

} // namespace rasm
