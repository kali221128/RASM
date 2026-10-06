#pragma once
#include <string>
#include <vector>
#include "token.h"

namespace rasm {

// 词法分析器：把整个源码切成 token 流
class Lexer {
public:
    explicit Lexer(const std::string& src);

    // 取下一个 token，自动跳过空白和注释
    Token next();

    // 回看当前行的原始文本（用于错误报告）
    const std::vector<Token>& tokens() const { return tokens_; }

private:
    char peek() const;
    char get();
    void skipWsAndComment();
    Token readIdent();
    Token readNumber();
    Token readString();

    const std::string& src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_  = 1;
    std::vector<Token> tokens_;
};

} // namespace rasm
