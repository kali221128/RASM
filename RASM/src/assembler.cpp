#include "assembler.h"
#include "lexer.h"
#include "common.h"
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <map>
#include <functional>

namespace rasm {

// 文本预处理：处理 %include "file.asm"
static std::string preprocessIncludes(const std::string& src, const std::string& baseDir) {
    std::string out;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t nl = src.find('\n', pos);
        if (nl == std::string::npos) nl = src.size();
        std::string line = src.substr(pos, nl - pos);
        size_t s = line.find_first_not_of(" \t\r");
        if (s != std::string::npos && line.substr(s, 8) == "%include") {
            size_t q1 = line.find('"', s);
            size_t q2 = line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string path = line.substr(q1 + 1, q2 - q1 - 1);
                std::string full = baseDir + "/" + path;
                std::ifstream f(full, std::ios::binary);
                if (f) {
                    std::stringstream ss; ss << f.rdbuf();
                    out += preprocessIncludes(ss.str(), baseDir);
                }
            }
        } else {
            out += line;
            out += '\n';
        }
        pos = nl + 1;
    }
    return out;
}

// token 流预处理：处理 %macro name N ... %endmacro（带参数）
static std::vector<Token> preprocessMacros(const std::vector<Token>& toks) {
    struct Macro { int nargs; std::vector<Token> body; };
    std::map<std::string, Macro> macros;
    std::vector<Token> out;
    size_t i = 0;
    while (i < toks.size()) {
        if (toks[i].kind == TokenKind::LABEL && toks[i].text == "%macro" && i + 1 < toks.size()
            && toks[i+1].kind == TokenKind::LABEL) {
            std::string name = toks[i+1].text;
            int nargs = 0;
            size_t j = i + 2;
            if (j < toks.size() && toks[j].kind == TokenKind::INTEGER) {
                nargs = (int)toks[j].ival;
                j++;
            }
            std::vector<Token> body;
            while (j < toks.size() && !(toks[j].kind == TokenKind::LABEL && toks[j].text == "%endmacro")) {
                body.push_back(toks[j]);
                j++;
            }
            macros[name] = {nargs, body};
            i = j + 1;
            continue;
        }
        // 宏调用：读 name 后面直到 NEWLINE 的 token，按逗号分割成参数
        if (toks[i].kind == TokenKind::LABEL && macros.count(toks[i].text)) {
            auto& m = macros[toks[i].text];
            // 收集参数（直到 NEWLINE 或 END）
            std::vector<std::vector<Token>> args;
            std::vector<Token> curArg;
            size_t j = i + 1;
            while (j < toks.size() && toks[j].kind != TokenKind::NEWLINE && toks[j].kind != TokenKind::END) {
                if (toks[j].kind == TokenKind::COMMA) {
                    args.push_back(curArg);
                    curArg.clear();
                } else {
                    curArg.push_back(toks[j]);
                }
                j++;
            }
            if (!curArg.empty() || !args.empty()) args.push_back(curArg);
            // 展开宏体，替换 %1 %2 ...
            for (auto& t : m.body) {
                if (t.kind == TokenKind::LABEL && t.text.size() >= 2 && t.text[0] == '%') {
                    // %1 %2 -> 参数索引
                    int idx = 0;
                    for (size_t k = 1; k < t.text.size(); k++) {
                        if (t.text[k] >= '0' && t.text[k] <= '9') idx = idx*10 + (t.text[k]-'0');
                    }
                    if (idx >= 1 && idx <= (int)args.size()) {
                        for (auto& a : args[idx-1]) out.push_back(a);
                        continue;
                    }
                }
                out.push_back(t);
            }
            i = j;
            continue;
        }
        out.push_back(toks[i]);
        i++;
    }
    return out;
}

// token 流预处理：处理 %if NUM / %elif NUM / %else / %endif
static std::vector<Token> preprocessIf(const std::vector<Token>& toks) {
    std::vector<Token> out;
    size_t i = 0;
    // 栈：每层 {是否激活, 是否已经有真分支}
    struct Level { bool active; bool hadTrue; };
    std::vector<Level> stk;
    while (i < toks.size()) {
        if (toks[i].kind == TokenKind::LABEL && toks[i].text == "%if") {
            // 求值条件：下一个 token 是整数
            bool parentActive = stk.empty() || stk.back().active;
            bool cond = false;
            if (i+1 < toks.size() && toks[i+1].kind == TokenKind::INTEGER) cond = toks[i+1].ival != 0;
            stk.push_back({parentActive && cond, cond});
            i += 2; // 跳过 %if 和条件
            continue;
        }
        if (toks[i].kind == TokenKind::LABEL && toks[i].text == "%elif") {
            if (stk.empty()) { i++; continue; }
            auto& top = stk.back();
            bool parentActive = stk.size() >= 2 ? stk[stk.size()-2].active : true;
            // 只有前面分支都没真过，才求值
            if (!top.hadTrue && i+1 < toks.size() && toks[i+1].kind == TokenKind::INTEGER) {
                bool cond = toks[i+1].ival != 0;
                top.active = parentActive && cond;
                if (cond) top.hadTrue = true;
            } else {
                top.active = false;
            }
            i += 2;
            continue;
        }
        if (toks[i].kind == TokenKind::LABEL && toks[i].text == "%else") {
            if (!stk.empty()) {
                auto& top = stk.back();
                bool parentActive = stk.size() >= 2 ? stk[stk.size()-2].active : true;
                top.active = parentActive && !top.hadTrue;
                top.hadTrue = true;
            }
            i++;
            continue;
        }
        if (toks[i].kind == TokenKind::LABEL && toks[i].text == "%endif") {
            if (!stk.empty()) stk.pop_back();
            i++;
            continue;
        }
        bool active = stk.empty() || stk.back().active;
        if (active) out.push_back(toks[i]);
        i++;
    }
    return out;
}

// token 流预处理：处理 %rep N ... %endrep（重复块）
static std::vector<Token> preprocessRep(const std::vector<Token>& toks) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < toks.size()) {
        if (toks[i].kind == TokenKind::LABEL && toks[i].text == "%rep" && i+1 < toks.size()
            && toks[i+1].kind == TokenKind::INTEGER) {
            int count = (int)toks[i+1].ival;
            size_t j = i + 2;
            std::vector<Token> body;
            while (j < toks.size() && !(toks[j].kind == TokenKind::LABEL && toks[j].text == "%endrep")) {
                body.push_back(toks[j]);
                j++;
            }
            for (int r = 0; r < count; r++) {
                for (auto& t : body) out.push_back(t);
            }
            i = j + 1;
            continue;
        }
        out.push_back(toks[i]);
        i++;
    }
    return out;
}

// 表达式求值器（递归下降）
// 返回值：计算结果；hasUndefined 输出是否有未定义符号（需要 fixup 回填）
static int64_t evalExpr(const std::vector<Token>& toks, size_t& i, SymbolTable& sym, bool& hasUndefined, int64_t currentOffset = 0) {
    std::function<int64_t()> parseOr, parseXor, parseAnd, parseShift, parseAdd, parseMul, parseUnary, parsePrimary;

    parsePrimary = [&]() -> int64_t {
        if (i >= toks.size()) throw AsmError("Unexpected end of expression");
        Token t = toks[i];
        if (t.kind == TokenKind::INTEGER) { i++; return t.ival; }
        if (t.kind == TokenKind::LPAREN) {
            i++;
            int64_t v = parseOr();
            if (i < toks.size() && toks[i].kind == TokenKind::RPAREN) i++;
            return v;
        }
        if (t.kind == TokenKind::LABEL) {
            i++;
            if (t.text == "$") {
                return currentOffset;
            }
            if (t.text == "$$") {
                // $$ 是段起始（0，因为段内偏移从 0 开始）
                return 0;
            }
            if (sym.has(t.text)) return sym.get(t.text);
            hasUndefined = true;
            return 0;
        }
        // 调试：打印未知 token
        char buf[128];
        snprintf(buf, sizeof(buf), "Unexpected token kind=%d text='%s' line=%d", (int)t.kind, t.text.c_str(), t.line);
        throw AsmError(buf);
    };
    parseUnary = [&]() -> int64_t {
        if (i < toks.size() && toks[i].kind == TokenKind::MINUS) { i++; return -parseUnary(); }
        if (i < toks.size() && toks[i].kind == TokenKind::NOT) { i++; return ~parseUnary(); }
        return parsePrimary();
    };
    parseMul = [&]() -> int64_t {
        int64_t v = parseUnary();
        while (i < toks.size() && (toks[i].kind == TokenKind::STAR ||
               toks[i].kind == TokenKind::DIV || toks[i].kind == TokenKind::MOD)) {
            TokenKind op = toks[i].kind; i++;
            int64_t r = parseUnary();
            if (op == TokenKind::STAR) v *= r;
            else if (op == TokenKind::DIV) v = r ? v / r : 0;
            else v = r ? v % r : 0;
        }
        // ** 幂运算
        while (i < toks.size() && toks[i].kind == TokenKind::STAR &&
               i+1 < toks.size() && toks[i+1].kind == TokenKind::STAR) {
            i += 2;
            int64_t r = parseUnary();
            int64_t res = 1;
            for (int64_t k = 0; k < r; k++) res *= v;
            v = res;
        }
        return v;
    };
    parseAdd = [&]() -> int64_t {
        int64_t v = parseMul();
        while (i < toks.size() && (toks[i].kind == TokenKind::PLUS ||
               toks[i].kind == TokenKind::MINUS)) {
            TokenKind op = toks[i].kind; i++;
            int64_t r = parseMul();
            if (op == TokenKind::PLUS) v += r; else v -= r;
        }
        return v;
    };
    parseShift = [&]() -> int64_t {
        int64_t v = parseAdd();
        while (i < toks.size() && (toks[i].kind == TokenKind::LSHIFT ||
               toks[i].kind == TokenKind::RSHIFT)) {
            TokenKind op = toks[i].kind; i++;
            int64_t r = parseAdd();
            if (op == TokenKind::LSHIFT) v <<= r; else v >>= r;
        }
        return v;
    };
    parseAnd = [&]() -> int64_t {
        int64_t v = parseShift();
        while (i < toks.size() && toks[i].kind == TokenKind::AND) { i++; v &= parseShift(); }
        return v;
    };
    parseXor = [&]() -> int64_t {
        int64_t v = parseAnd();
        while (i < toks.size() && toks[i].kind == TokenKind::XOR) { i++; v ^= parseAnd(); }
        return v;
    };
    parseOr = [&]() -> int64_t {
        int64_t v = parseXor();
        while (i < toks.size() && toks[i].kind == TokenKind::OR) { i++; v |= parseXor(); }
        return v;
    };
    return parseOr();
}

Assembler::Assembler(const std::string& src) : src_(src) {
    cur_ = &text_;
}

Operand Assembler::parseOperand(std::vector<Token>& toks, size_t& i) {
    Operand op;
    if (i >= toks.size()) throw AsmError("Unexpected end of input");

    // 跳过大小前缀：dword/word/byte/near/far ptr
    while (i < toks.size() && toks[i].kind == TokenKind::LABEL) {
        std::string s = toks[i].text;
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        if (s == "dword" || s == "word" || s == "byte" ||
            s == "near" || s == "far" || s == "ptr") {
            i++;
        } else break;
    }
    if (i >= toks.size()) throw AsmError("Unexpected end after size prefix");

    Token t = toks[i];

    // [ ... ] 内存操作数
    if (t.kind == TokenKind::LBRACK) {
        i++;
        op.kind = OpKind::Mem;
        // 解析 [base + index*scale + disp]
        // 简化：先支持 [disp]、[reg]、[reg+disp]
        int64_t disp = 0;
        while (i < toks.size() && toks[i].kind != TokenKind::RBRACK) {
            Token c = toks[i];
            if (c.kind == TokenKind::LABEL) {
                RegInfo ri = parseRegister(c.text);
                if (ri.cls != RegClass::None) {
                    if (op.reg.cls == RegClass::None) op.reg = ri;
                    else op.index = ri;
                } else {
                    // 符号地址作为 disp
                    if (sym_.has(c.text)) disp += sym_.get(c.text);
                    else op.label = c.text;   // 前向引用，传给 encoder 记 Abs32 reloc
                }
                i++;
            } else if (c.kind == TokenKind::INTEGER) {
                disp += c.ival;
                i++;
            } else if (c.kind == TokenKind::PLUS) {
                i++;
            } else if (c.kind == TokenKind::STAR) {
                i++;
                if (i < toks.size() && toks[i].kind == TokenKind::INTEGER) {
                    op.scale = (int)toks[i].ival;
                    i++;
                }
            } else if (c.kind == TokenKind::MINUS) {
                i++;
                if (i < toks.size() && toks[i].kind == TokenKind::INTEGER) {
                    disp -= toks[i].ival;
                    i++;
                }
            } else {
                i++;
            }
        }
        if (i < toks.size() && toks[i].kind == TokenKind::RBRACK) i++;
        op.imm = disp;
        return op;
    }

    // 标识符：先看是不是寄存器
    if (t.kind == TokenKind::LABEL) {
        RegInfo ri = parseRegister(t.text);
        if (ri.cls != RegClass::None) {
            op.kind = OpKind::Reg;
            op.reg = ri;
            i++;
            return op;
        }
        // 局部标号 .loop: 拼前缀，当 LabelRef
        if (!t.text.empty() && t.text[0] == '.') {
            op.kind = OpKind::LabelRef;
            op.label = currentGlobal_ + t.text;
            i++;
            return op;
        }
        // 其他 LABEL：走表达式收集求值（已定义符号直接算，未定义走 LabelRef）
    }

    // 立即数/表达式：从当前位置收集到逗号/换行为止（跟踪括号深度）
    {
        std::vector<Token> expr;
        int depth = 0;
        while (i < toks.size()) {
            TokenKind k = toks[i].kind;
            if (k == TokenKind::LPAREN) depth++;
            else if (k == TokenKind::RPAREN) {
                if (depth == 0) break;  // 外层括号结束（不属于本表达式）
                depth--;
            } else if (depth == 0 && (k == TokenKind::COMMA || k == TokenKind::NEWLINE)) {
                break;
            }
            expr.push_back(toks[i]);
            i++;
        }
        if (!expr.empty()) {
            bool hasUndefined = false;
            size_t pi = 0;
            int64_t val = evalExpr(expr, pi, sym_, hasUndefined, (int64_t)cur_->data.size());
            if (!hasUndefined) {
                op.kind = OpKind::Imm;
                op.imm = val;
                return op;
            }
            op.kind = OpKind::LabelRef;
            op.label = "_expr_";
            pendingExpr_ = std::move(expr);
            return op;
        }
    }

    throw AsmError("Cannot parse operand: " + t.text, t.line);
}

void Assembler::parseLine(std::vector<Token>& toks, size_t& i) {
    // 行首可能是 "xxx:" 标签定义
    if (i < toks.size() && toks[i].kind == TokenKind::LABEL &&
        i+1 < toks.size() && toks[i+1].kind == TokenKind::COLON) {
        std::string name = toks[i].text;
        // 局部标号（.loop:）：拼到当前全局标号后
        if (!name.empty() && name[0] == '.') {
            name = currentGlobal_ + name;
        } else {
            currentGlobal_ = name;
        }
        sym_.define(name, (int64_t)cur_->data.size(), curSection());
        lastLabel_ = name;
        i += 2;
    }
    // 无冒号 equ：BUFSIZE equ 1024 / BUFSIZE = 1024
    else if (i+1 < toks.size() && toks[i].kind == TokenKind::LABEL &&
             toks[i+1].kind == TokenKind::LABEL &&
             (toks[i+1].text == "equ" || toks[i+1].text == "=")) {
        std::string name = toks[i].text;
        i += 2;
        std::vector<Token> expr;
        while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
            expr.push_back(toks[i]); i++;
        }
        bool hasU = false;
        size_t pi = 0;
        int64_t v = evalExpr(expr, pi, sym_, hasU, (int64_t)cur_->data.size());
        sym_.define(name, v, 2);  // section=2 表示绝对值（equ），不加段基址
        return;
    }
    if (i >= toks.size() || toks[i].kind == TokenKind::NEWLINE) return;

    // 取助记符
    Token mn = toks[i];
    if (mn.kind != TokenKind::LABEL) { i++; return; }
    std::string mnem = mn.text;
    for (auto& c : mnem) c = (char)tolower((unsigned char)c);
    i++;

    // 伪指令：resb N / resd N（预留空间，补零）
    if (mnem == "resb" || mnem == "resw" || mnem == "resd") {
        std::vector<Token> expr;
        while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
            expr.push_back(toks[i]); i++;
        }
        bool hasU = false; size_t pi = 0;
        int64_t v = evalExpr(expr, pi, sym_, hasU, 0);
        int mul = (mnem == "resb") ? 1 : (mnem == "resw") ? 2 : 4;
        for (int64_t k = 0; k < v * mul; k++) cur_->data.push_back(0);
        return;
    }

    // 伪指令：struct / ends（空操作，块内 db/dd 正常处理）
    if (mnem == "struct" || mnem == "ends") return;

    // 伪指令：global name（导出符号）
    // bits 伪指令：指定 16/32/64 位
    if (mnem == "bits") {
        if (i < toks.size() && toks[i].kind == TokenKind::INTEGER) {
            int b = toks[i].ival;
            if (b != 32) {
                throw AsmError("bits " + std::to_string(b) + " 暂不支持，当前仅支持 bits 32 (x86 PE32)", toks[i].line);
            }
        }
        return;
    }
    // gui 伪指令：使用 Windows GUI 子系统（不弹控制台窗口）
    if (mnem == "gui") {
        guiMode_ = true;
        return;
    }

    if (mnem == "global") {
        if (i < toks.size() && toks[i].kind == TokenKind::LABEL) {
            exportNames_.push_back(toks[i].text);
            i++;
        }
        return;
    }
    // extern name — 声明外部符号
    if (mnem == "extern" || mnem == "external") {
        if (i < toks.size() && toks[i].kind == TokenKind::LABEL) {
            if (!sym_.has(toks[i].text))
                sym_.define(toks[i].text, 0, 2);
            i++;
        }
        return;
    }
    // org — 设置当前段起始（PE 忽略，COM 文件用）
    if (mnem == "org") return;
    // default/cpu/absolute/common/static — NASM 兼容，PE 忽略
    if (mnem == "default" || mnem == "cpu" || mnem == "absolute" ||
        mnem == "common" || mnem == "static") return;

    // 伪指令：%res type, name, "file.bin"
    if (mnem == "%res") {
        // 收集参数：type 数字, name 数字, "file"
        uint32_t type = 0, name = 0;
        std::string path;
        // 第一个参数：type
        {
            std::vector<Token> e;
            while (i < toks.size() && toks[i].kind != TokenKind::COMMA && toks[i].kind != TokenKind::NEWLINE) {
                e.push_back(toks[i]); i++;
            }
            bool hu = false; size_t pi = 0;
            type = (uint32_t)evalExpr(e, pi, sym_, hu, 0);
        }
        if (i < toks.size() && toks[i].kind == TokenKind::COMMA) i++;
        // 第二个参数：name
        {
            std::vector<Token> e;
            while (i < toks.size() && toks[i].kind != TokenKind::COMMA && toks[i].kind != TokenKind::NEWLINE) {
                e.push_back(toks[i]); i++;
            }
            bool hu = false; size_t pi = 0;
            name = (uint32_t)evalExpr(e, pi, sym_, hu, 0);
        }
        if (i < toks.size() && toks[i].kind == TokenKind::COMMA) i++;
        // 第三个参数：字符串文件路径
        if (i < toks.size() && toks[i].kind == TokenKind::STRING) {
            path = toks[i].text;
            i++;
        }
        resDecls_.push_back({type, name, path});
        return;
    }

    // 伪指令：切段
    if (mnem == "section" || mnem == ".section") {
        if (i < toks.size() && toks[i].kind == TokenKind::LABEL) {
            std::string s = toks[i].text;
            if (s == ".text" || s == "text") cur_ = &text_;
            else if (s == ".data" || s == "data") cur_ = &data_;
            i++;
        }
        return;
    }
    if (mnem == ".text") { cur_ = &text_; return; }
    if (mnem == ".data") { cur_ = &data_; return; }

    // 伪指令：数据 db / dw / dd
    if (mnem == "db" || mnem == "byte") {
        while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
            if (toks[i].kind == TokenKind::STRING) {
                for (char c : toks[i].text) cur_->data.push_back((uint8_t)c);
                i++;
            } else if (toks[i].kind == TokenKind::COMMA) {
                i++;
            } else {
                // 收集一个表达式 token 直到逗号/换行/DUP
                std::vector<Token> expr;
                while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE &&
                       toks[i].kind != TokenKind::COMMA) {
                    // DUP 是关键字，作为分隔
                    if (toks[i].kind == TokenKind::LABEL && toks[i].text == "DUP") break;
                    expr.push_back(toks[i]); i++;
                }
                bool hasU = false;
                size_t pi = 0;
                int64_t v = evalExpr(expr, pi, sym_, hasU, (int64_t)cur_->data.size());
                // DUP: N DUP(value)
                if (i < toks.size() && toks[i].kind == TokenKind::LABEL && toks[i].text == "DUP") {
                    i++; // 跳过 DUP
                    if (i < toks.size() && toks[i].kind == TokenKind::LPAREN) i++;
                    std::vector<Token> vexpr;
                    while (i < toks.size() && toks[i].kind != TokenKind::RPAREN &&
                           toks[i].kind != TokenKind::NEWLINE) {
                        vexpr.push_back(toks[i]); i++;
                    }
                    if (i < toks.size() && toks[i].kind == TokenKind::RPAREN) i++;
                    size_t vpi = 0;
                    int64_t dv = evalExpr(vexpr, vpi, sym_, hasU, 0);
                    for (int64_t k = 0; k < v; k++) cur_->data.push_back((uint8_t)(dv & 0xff));
                } else {
                    cur_->data.push_back((uint8_t)(v & 0xff));
                    if (hasU) {
                        fixups_.push_back({cur_, cur_->data.size()-1, "", 1,
                            Encoded::RelocKind::Abs32, expr});
                    }
                }
            }
        }
        return;
    }
    if (mnem == "dd" || mnem == "dword") {
        while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
            if (toks[i].kind == TokenKind::COMMA) { i++; continue; }
            std::vector<Token> expr;
            while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE &&
                   toks[i].kind != TokenKind::COMMA) {
                expr.push_back(toks[i]); i++;
            }
            bool hasU = false;
            size_t pi = 0;
            int64_t v = evalExpr(expr, pi, sym_, hasU, (int64_t)cur_->data.size());
            uint32_t uv = (uint32_t)v;
            cur_->data.push_back(uv&0xff);
            cur_->data.push_back((uv>>8)&0xff);
            cur_->data.push_back((uv>>16)&0xff);
            cur_->data.push_back((uv>>24)&0xff);
            if (hasU) {
                fixups_.push_back({cur_, cur_->data.size()-4, "", 4,
                    Encoded::RelocKind::Abs32, expr});
            }
        }
        return;
    }
    if (mnem == "dw" || mnem == "word") {
        while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
            if (toks[i].kind == TokenKind::COMMA) { i++; continue; }
            std::vector<Token> expr;
            while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE &&
                   toks[i].kind != TokenKind::COMMA) {
                expr.push_back(toks[i]); i++;
            }
            bool hasU = false;
            size_t pi = 0;
            int64_t v = evalExpr(expr, pi, sym_, hasU, (int64_t)cur_->data.size());
            uint16_t uv = (uint16_t)v;
            cur_->data.push_back(uv&0xff);
            cur_->data.push_back((uv>>8)&0xff);
            if (hasU) {
                fixups_.push_back({cur_, cur_->data.size()-2, "", 2,
                    Encoded::RelocKind::Abs32, expr});
            }
        }
        return;
    }
    if (mnem == "times") {
        // times N db expr
        if (i < toks.size() && toks[i].kind == TokenKind::INTEGER) {
            int count = (int)toks[i].ival;
            i++;
            // 下一个 token 应该是 db/dw/dd
            if (i < toks.size() && toks[i].kind == TokenKind::LABEL) {
                std::string dir = toks[i].text;
                for (auto& c : dir) c = tolower((unsigned char)c);
                i++;
                // 收集一个表达式
                std::vector<Token> expr;
                while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
                    expr.push_back(toks[i]); i++;
                }
                bool hasU = false;
                size_t pi = 0;
                int64_t v = evalExpr(expr, pi, sym_, hasU, (int64_t)cur_->data.size());
                for (int r = 0; r < count; r++) {
                    if (dir == "db" || dir == "byte") cur_->data.push_back((uint8_t)(v & 0xff));
                    else if (dir == "dw" || dir == "word") {
                        uint16_t uv = (uint16_t)v;
                        cur_->data.push_back(uv & 0xff);
                        cur_->data.push_back((uv >> 8) & 0xff);
                    } else if (dir == "dd" || dir == "dword") {
                        uint32_t uv = (uint32_t)v;
                        cur_->data.push_back(uv & 0xff);
                        cur_->data.push_back((uv >> 8) & 0xff);
                        cur_->data.push_back((uv >> 16) & 0xff);
                        cur_->data.push_back((uv >> 24) & 0xff);
                    }
                }
            }
        }
        return;
    }
    if (mnem == "equ" || mnem == "=") {
        // name equ expr：name 在前面已被标签定义注册，这里更新它的值
        // 简化：前一行标签已注册为当前位置，equ 覆盖为表达式值
        if (i < toks.size()) {
            std::vector<Token> expr;
            while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
                expr.push_back(toks[i]); i++;
            }
            bool hasU = false;
            size_t pi = 0;
            int64_t v = evalExpr(expr, pi, sym_, hasU, (int64_t)cur_->data.size());
            if (!lastLabel_.empty()) {
                sym_.define(lastLabel_, v);
                lastLabel_.clear();
            }
        }
        return;
    }
    if (mnem == "align") {
        int n = 16;
        if (i < toks.size() && toks[i].kind == TokenKind::INTEGER) n = (int)toks[i].ival;
        while (cur_->data.size() % n != 0) cur_->data.push_back(0);
        return;
    }

    // 真指令
    Instr in;
    in.mnemonic = mnem;
    in.line = mn.line;
    while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) {
        if (toks[i].kind == TokenKind::COMMA) { i++; continue; }
        in.ops.push_back(parseOperand(toks, i));
    }

    Encoded enc = Encoder::encode(in, (int64_t)cur_->data.size(), sym_);
    size_t base = cur_->data.size();
    for (uint8_t b : enc.bytes) cur_->data.push_back(b);

    // 记录 fixup
    for (auto& r : enc.relocs) {
        Fixup f;
        f.sec = cur_;
        f.offset = base + r.offset;
        f.label = r.label;
        f.width = r.width;
        f.kind = r.kind;
        if (r.label == "_expr_" && !pendingExpr_.empty()) {
            f.expr = pendingExpr_;
            f.label = "";
        }
        fixups_.push_back(f);
    }
    pendingExpr_.clear();
}

std::vector<uint32_t> Assembler::absFixupRvas() const {
    std::vector<uint32_t> r;
    for (auto& f : fixups_) {
        if (f.kind == Encoded::RelocKind::Abs32 && f.width == 4) {
            uint32_t secBase = (f.sec == &data_) ? (dataBase_ - 0x400000) : (textBase_ - 0x400000);
            r.push_back(secBase + (uint32_t)f.offset);
        }
    }
    return r;
}

std::string Assembler::assemble() {
    try {
        // 预处理 %include
        std::string src = preprocessIncludes(src_, baseDir_);

        Lexer lex(src);
        std::vector<Token> toks;
        while (true) {
            Token t = lex.next();
            toks.push_back(t);
            if (t.kind == TokenKind::END) break;
        }

        // 预处理 %macro
        toks = preprocessMacros(toks);
        // 预处理 %if/%elif/%else/%endif
        toks = preprocessIf(toks);
        // 预处理 %rep N ... %endrep
        toks = preprocessRep(toks);

        size_t i = 0;
        std::string errors;
        while (i < toks.size()) {
            if (toks[i].kind == TokenKind::NEWLINE) { i++; continue; }
            try {
                parseLine(toks, i);
            } catch (AsmError& e) {
                if (!errors.empty()) errors += "\n";
                errors += e.what();
                // 跳到下一个 NEWLINE
                while (i < toks.size() && toks[i].kind != TokenKind::NEWLINE) i++;
            }
        }
        if (!errors.empty()) return errors;

        // 回填 fixup
        for (auto& f : fixups_) {
            int64_t v;
            int sec;
            if (!f.expr.empty()) {
                // 表达式 fixup：重新求值
                bool hasU = false;
                size_t pi = 0;
                v = evalExpr(f.expr, pi, sym_, hasU, (int64_t)f.offset);
                // 找表达式里第一个 LABEL 的 section
                sec = 0;
                for (auto& t : f.expr) {
                    if (t.kind == TokenKind::LABEL && t.text != "$" && sym_.has(t.text)) {
                        sec = sym_.sectionOf(t.text);
                        break;
                    }
                }
            } else {
                if (!sym_.has(f.label)) continue;
                v = sym_.get(f.label);
                sec = sym_.sectionOf(f.label);
            }
            size_t off = f.offset;
            int64_t value;
            if (f.kind == Encoded::RelocKind::Rel32) {
                value = v - (int64_t)off - 4;
            } else {
                if (sec == 0) value = (int64_t)textBase_ + v;
                else if (sec == 1) value = (int64_t)dataBase_ + v;
                else value = v;
            }
            if (f.width == 4) {
                f.sec->data[off]     = (uint8_t)(value & 0xff);
                f.sec->data[off + 1] = (uint8_t)((value>>8)&0xff);
                f.sec->data[off + 2] = (uint8_t)((value>>16)&0xff);
                f.sec->data[off + 3] = (uint8_t)((value>>24)&0xff);
            } else if (f.width == 2) {
                f.sec->data[off]     = (uint8_t)(value & 0xff);
                f.sec->data[off + 1] = (uint8_t)((value>>8)&0xff);
            } else if (f.width == 1) {
                f.sec->data[off]     = (uint8_t)(value & 0xff);
            }
        }

        // 检查未解析的 fixup
        for (auto& f : fixups_) {
            if (!f.expr.empty()) continue;
            if (!sym_.has(f.label)) {
                throw AsmError("Undefined symbol '" + f.label + "'");
            }
        }

        // 短跳转优化：jcc rel32 (0F 8x cd) 如果 |disp|<=127，收紧为 rel8 (7x cb)
        {
            auto& d = text_.data;
            // 收集需要收紧的位置
            struct Spot { size_t off; uint8_t shortOp; int8_t disp; };
            std::vector<Spot> spots;
            for (size_t p = 0; p + 6 <= d.size(); ) {
                if (d[p] == 0x0F && (d[p+1] & 0xF0) == 0x80) {
                    int32_t disp = (int32_t)(d[p+2] | (d[p+3]<<8) | (d[p+4]<<16) | (d[p+5]<<24));
                    if (disp >= -128 && disp <= 127) {
                        // 0F 8x -> 7x
                        uint8_t shortOp = 0x70 | (d[p+1] & 0x0F);
                        spots.push_back({p, shortOp, (int8_t)disp});
                        p += 6;
                        continue;
                    }
                }
                p++;
            }
            // 从后往前处理，避免偏移变化
            for (auto it = spots.rbegin(); it != spots.rend(); ++it) {
                size_t p = it->off;
                // rel8 disp = rel32 disp + 4（因为 rel32 next_ip = p+6，rel8 next_ip = p+2）
                int8_t disp8 = (int8_t)(it->disp + 4);
                d[p] = it->shortOp;
                d[p+1] = (uint8_t)disp8;
                d.erase(d.begin() + p + 2, d.begin() + p + 6);
                // 更新 fixups_：偏移 > p 的 fixup 前移 4
                for (auto& f : fixups_) {
                    if (f.sec == &text_ && f.offset > p) f.offset -= 4;
                }
            }
        }
        return "";
    } catch (AsmError& e) {
        std::string msg = e.what();
        int eline = e.line();
        if (eline > 0) {
            int cur = 1;
            size_t pos = 0;
            std::string srcLine;
            while (pos < src_.size()) {
                size_t nl = src_.find('\n', pos);
                if (nl == std::string::npos) nl = src_.size();
                if (cur == eline) { srcLine = src_.substr(pos, nl - pos); break; }
                pos = nl + 1;
                cur++;
            }
            if (!srcLine.empty()) {
                msg += "\n  --> line " + std::to_string(eline) + ": " + srcLine;
            } else {
                msg += "\n  --> line " + std::to_string(eline);
            }
        }
        return msg;
    }
}

} // namespace rasm
