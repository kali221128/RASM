#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include "assembler.h"
#include "pe.h"

using namespace rasm;

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "RASM - x86-32 Assembler\n");
        fprintf(stderr, "Usage: %s <input.asm> <output.exe>\n", argv[0]);
        return 1;
    }
    std::string src = readFile(argv[1]);
    if (src.empty()) {
        fprintf(stderr, "Error: cannot read '%s'\n", argv[1]);
        return 1;
    }

    // 1. 先创建 PEWriter，注册 import 函数，计算 IAT 布局
    PEWriter pe(Section(".text"), Section(".data"));
    pe.addImport("kernel32.dll", "GetStdHandle");
    pe.addImport("kernel32.dll", "WriteFile");
    pe.addImport("kernel32.dll", "ExitProcess");
    pe.addImport("user32.dll", "MessageBoxA");
    auto importVAs = pe.layout();

    // 2. 创建 Assembler，注入 import 符号
    Assembler as(src);
    // 计算源文件目录（用于 %include）
    {
        std::string in(argv[1]);
        size_t slash = in.find_last_of("/\\");
        if (slash != std::string::npos) as.setBaseDir(in.substr(0, slash));
        else as.setBaseDir(".");
    }
    for (auto& [name, va] : importVAs) {
        as.defineSymbol("__imp_" + name, va);
    }
    // 段基址 VA（imageBase=0x400000, textRva=0x1000, dataRva=0x3000）
    as.setBases(0x401000, 0x403000);

    // 3. 汇编
    std::string err = as.assemble();
    if (!err.empty()) {
        fprintf(stderr, "Assembly error: %s\n", err.c_str());
        return 1;
    }

    // 4. 重新创建 PEWriter（用实际段数据）
    PEWriter peOut(as.text(), as.data());
    peOut.addImport("kernel32.dll", "GetStdHandle");
    peOut.addImport("kernel32.dll", "WriteFile");
    peOut.addImport("kernel32.dll", "ExitProcess");
    peOut.addImport("user32.dll", "MessageBoxA");
    // 注册资源
    {
        std::string baseDir(argv[1]);
        size_t slash = baseDir.find_last_of("/\\");
        baseDir = (slash != std::string::npos) ? baseDir.substr(0, slash) : ".";
        for (auto& rd : as.resources()) {
            std::string p = baseDir + "/" + rd.path;
            std::ifstream f(p, std::ios::binary);
            if (f) {
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                peOut.addResource(rd.type, rd.name, data);
            }
        }
    }
    // 注册 Base Relocations
    for (uint32_t rva : as.absFixupRvas()) {
        peOut.addReloc(rva);
    }
    if (as.guiMode()) peOut.setGuiMode();
    // 自动检测 GUI：如果 import 了 user32.dll 且没有显式 Console 模式，自动切 GUI
    if (!as.guiMode()) {
        // 检查是否引用了 MessageBoxA
        if (as.symbols().has("__imp_MessageBoxA")) {
            peOut.setGuiMode();
        }
    }
    // 自动检测 DLL 模式：有 global 导出时
    if (!as.exports().empty()) {
        peOut.setDllMode();
    }
    // 注册导出函数
    for (auto& name : as.exports()) {
        if (as.symbols().has(name)) {
            int sec = as.symbols().sectionOf(name);
            int64_t val = as.symbols().get(name);
            uint32_t rva = (sec == 0) ? (0x1000 + (uint32_t)val) : (0x3000 + (uint32_t)val);
            peOut.addExport(name, rva);
        }
    }
    peOut.layout();
    peOut.setEntry("_start");
    // 设置 entry RVA
    if (as.symbols().has("_start")) {
        int sec = as.symbols().sectionOf("_start");
        int64_t val = as.symbols().get("_start");
        uint32_t rva = (sec == 0) ? (0x1000 + (uint32_t)val) : (0x3000 + (uint32_t)val);
        peOut.setEntryRva(rva);
    } else {
        // 自动入口：.text 段开头（RVA 0x1000）
        peOut.setEntryRva(0x1000);
    }

    auto exe = peOut.build();

    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", argv[2]);
        return 1;
    }
    out.write((const char*)exe.data(), exe.size());
    out.close();
    if (out.fail()) {
        fprintf(stderr, "Error: failed writing '%s'\n", argv[2]);
        return 1;
    }

    printf("OK: %s -> %s (%u bytes, .text=%u, .data=%u)\n",
           argv[1], argv[2], (unsigned)exe.size(),
           (unsigned)as.text().data.size(), (unsigned)as.data().data.size());

    // 生成 Map 文件
    {
        std::string mapPath(argv[2]);
        size_t dot = mapPath.find_last_of(".");
        if (dot != std::string::npos) mapPath = mapPath.substr(0, dot);
        mapPath += ".map";
        std::ofstream mf(mapPath);
        mf << "RASM Map File\n";
        mf << "ImageBase = 0x00400000\n\n";
        mf << "Address         Symbol\n";
        for (auto& [name, val] : as.symbols().getAll()) {
            int64_t v = val.first;
            int sec = val.second;
            uint32_t rva = (sec == 0) ? (0x1000 + (uint32_t)v) : (sec == 1) ? (0x3000 + (uint32_t)v) : (uint32_t)v;
            mf << "0x" << std::hex << rva << "        " << name << "\n";
        }
        printf("Map: %s\n", mapPath.c_str());
    }

    // 生成列表文件 .lst（.text 段十六进制转储）
    {
        std::string lstPath(argv[2]);
        size_t dot = lstPath.find_last_of(".");
        if (dot != std::string::npos) lstPath = lstPath.substr(0, dot);
        lstPath += ".lst";
        std::ofstream lf(lstPath);
        lf << "RASM Listing File\n";
        lf << ".text section (RVA 0x1000, " << std::dec << as.text().data.size() << " bytes):\n\n";
        for (size_t off = 0; off < as.text().data.size(); off += 16) {
            lf << "0x" << std::hex << (0x1000 + off) << ": ";
            for (size_t j = 0; j < 16 && off + j < as.text().data.size(); j++) {
                lf << std::hex << (int)as.text().data[off + j] << " ";
            }
            lf << "\n";
        }
        printf("List: %s\n", lstPath.c_str());
    }

    for (auto& [name, va] : importVAs) {
        printf("  import __imp_%s = 0x%08X\n", name.c_str(), va);
    }
    return 0;
}
