#pragma once
#include <string>

namespace rasm {

enum class TokenKind {
    END,        // 文件结束
    LABEL,      // 标识符（可能是指令、寄存器、标签、伪指令）
    LABEL_DEF,  // 标签定义 xxx:
    INTEGER,    // 数字
    STRING,     // 字符串 "..."
    COMMA,      // ,
    COLON,      // :
    LBRACK,     // [
    RBRACK,     // ]
    PLUS,       // +
    MINUS,      // -
    STAR,       // *
    DIV,        // /
    DOT,        // .
    EQU,        // =
    NEWLINE,    // 逻辑行结束
    MOD,        // %
    AND,        // &
    OR,         // |
    XOR,        // ^
    NOT,        // ~
    LSHIFT,     // <<
    RSHIFT,     // >>
    LPAREN,     // (
    RPAREN,     // )
};

struct Token {
    TokenKind kind = TokenKind::END;
    std::string text;       // 原始文本
    int64_t ival = 0;       // 数字值
    int line = 0;
    int col  = 0;
};

} // namespace rasm
