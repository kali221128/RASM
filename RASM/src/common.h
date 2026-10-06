#pragma once
#include <cstdint>
#include <string>
#include <stdexcept>
#include <sstream>

namespace rasm {

// 统一错误类型，带文件行列号
class AsmError : public std::runtime_error {
public:
    AsmError(const std::string& msg, int line = 0, int col = 0)
        : std::runtime_error(format(msg, line, col)), line_(line), col_(col) {}
    int line() const { return line_; }
    int col() const { return col_; }
private:
    static std::string format(const std::string& m, int l, int c) {
        if (l == 0) return m;
        std::ostringstream os;
        os << m << " (line " << l;
        if (c) os << ", col " << c;
        os << ")";
        return os.str();
    }
    int line_, col_;
};

// 小端写数值到字节流
inline void writeU8(std::string& out, uint8_t v)  { out.push_back((char)v); }
inline void writeU16(std::string& out, uint16_t v){
    out.push_back((char)(v & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
}
inline void writeS16(std::string& out, int16_t v){ writeU16(out, (uint16_t)v); }
inline void writeU32(std::string& out, uint32_t v){
    out.push_back((char)(v & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)((v >> 16) & 0xff));
    out.push_back((char)((v >> 24) & 0xff));
}
inline void writeS32(std::string& out, int32_t v){ writeU32(out, (uint32_t)v); }

} // namespace rasm
