#include "lexer.h"
#include "common.h"
#include <cctype>
#include <cstdio>

namespace rasm {

Lexer::Lexer(const std::string& src) : src_(src) {}

char Lexer::peek() const {
    return pos_ < src_.size() ? src_[pos_] : '\0';
}

char Lexer::get() {
    char c = peek();
    if (c == '\0') return c;
    pos_++;
    if (c == '\n') { line_++; col_ = 1; }
    else col_++;
    return c;
}

void Lexer::skipWsAndComment() {
    while (true) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') { get(); continue; }
        if (c == ';') { // 行注释
            while (peek() != '\0' && peek() != '\n') get();
            continue;
        }
        break;
    }
}

Token Lexer::readIdent() {
    Token t;
    t.kind = TokenKind::LABEL;
    t.line = line_; t.col = col_;
    while (true) {
        char c = peek();
        if (std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '%' || c == '$') {
            t.text.push_back(c);
            get();
        } else break;
    }
    return t;
}

Token Lexer::readNumber() {
    Token t;
    t.kind = TokenKind::INTEGER;
    t.line = line_; t.col = col_;
    // 0x.. hex, 0o.. oct, 0b.. bin, 其他 dec
    if (peek() == '0' && (src_[pos_+1] == 'x' || src_[pos_+1] == 'X')) {
        get(); get();
        uint64_t v = 0;
        while (true) {
            char c = peek();
            if (c >= '0' && c <= '9')      v = v*16 + (c-'0');
            else if (c >= 'a' && c <= 'f') v = v*16 + (c-'a'+10);
            else if (c >= 'A' && c <= 'F') v = v*16 + (c-'A'+10);
            else break;
            get();
        }
        t.ival = (int64_t)v;
    } else if (peek() == '0' && (src_[pos_+1] == 'b' || src_[pos_+1] == 'B')) {
        get(); get();
        uint64_t v = 0;
        while (peek() == '0' || peek() == '1') { v = v*2 + (peek()-'0'); get(); }
        t.ival = (int64_t)v;
    } else {
        int64_t v = 0;
        while (std::isdigit((unsigned char)peek())) {
            v = v*10 + (peek()-'0');
            get();
        }
        t.ival = v;
    }
    // 允许 100h 这种 NASM 风格后缀
    if (peek() == 'h' || peek() == 'H') {
        // 重扫十六进制（前面已经按十进制读了，这里简单处理：回退到开头）
        // 为简化，我们要求用 0x 前缀，h 后缀暂不支持
        throw AsmError("Use 0x prefix for hex numbers (e.g. 0x1F)", line_, col_);
    }
    return t;
}

Token Lexer::readString() {
    Token t;
    t.kind = TokenKind::STRING;
    t.line = line_; t.col = col_;
    char quote = peek(); // " 或 '
    get(); // 吃掉开头的引号
    while (peek() != '\0' && peek() != quote) {
        if (peek() == '\\') {
            get();
            char c = peek();
            if (c == 'n') t.text.push_back('\n');
            else if (c == 't') t.text.push_back('\t');
            else if (c == '\\') t.text.push_back('\\');
            else if (c == '"') t.text.push_back('"');
            else if (c == '\'') t.text.push_back('\'');
            else t.text.push_back(c);
            get();
        } else {
            t.text.push_back(get());
        }
    }
    if (peek() == quote) get();
    return t;
}

Token Lexer::next() {
    skipWsAndComment();
    Token t;
    t.line = line_; t.col = col_;
    char c = peek();
    if (c == '\0') { t.kind = TokenKind::END; return t; }
    if (c == '\n') { get(); t.kind = TokenKind::NEWLINE; return t; }
    if (std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$') return readIdent();
    // % 开头：后面跟字母/数字/下划线是标识符（预处理指令 %macro/%if/宏参数 %1），否则是 mod 运算符
    if (c == '%') {
        get(); // 吃掉 %
        char n = peek();
        if (std::isalnum((unsigned char)n) || n == '_') {
            std::string s = "%";
            while (std::isalnum((unsigned char)peek()) || peek() == '_') s += get();
            t.kind = TokenKind::LABEL;
            t.text = s;
            return t;
        }
        t.kind = TokenKind::MOD;
        return t;
    }
    if (std::isdigit((unsigned char)c)) return readNumber();
    if (c == '"' || c == '\'') return readString();

    get();
    switch (c) {
        case ',': t.kind = TokenKind::COMMA; break;
        case ':': t.kind = TokenKind::COLON; break;
        case '[': t.kind = TokenKind::LBRACK; break;
        case ']': t.kind = TokenKind::RBRACK; break;
        case '+': t.kind = TokenKind::PLUS; break;
        case '-': t.kind = TokenKind::MINUS; break;
        case '*': t.kind = TokenKind::STAR; break;
        case '/': t.kind = TokenKind::DIV; break;
        case '%': t.kind = TokenKind::MOD; break;
        case '&': t.kind = TokenKind::AND; break;
        case '|': t.kind = TokenKind::OR; break;
        case '^': t.kind = TokenKind::XOR; break;
        case '~': t.kind = TokenKind::NOT; break;
        case '(': t.kind = TokenKind::LPAREN; break;
        case ')': t.kind = TokenKind::RPAREN; break;
        case '<':
            if (peek() == '<') { get(); t.kind = TokenKind::LSHIFT; }
            else throw AsmError("Unexpected '<'", line_);
            break;
        case '>':
            if (peek() == '>') { get(); t.kind = TokenKind::RSHIFT; }
            else throw AsmError("Unexpected '>'", line_);
            break;
        case ';':
            // 分号注释：跳到行尾
            while (peek() != '\0' && peek() != '\n') get();
            t.kind = TokenKind::NEWLINE;
            break;
        default:
            throw AsmError(std::string("Unexpected character: '") + c + "'", line_, col_);
    }
    return t;
}

} // namespace rasm
