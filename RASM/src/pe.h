#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "assembler.h"

namespace rasm {

// PE 文件生成器（参考 LIEF PE structures 重写）
// 支持：多 import 函数、.data 段、动态 IAT 布局
class PEWriter {
public:
    struct ImportFunc {
        std::string dll;
        std::string func;
        uint32_t iatRva = 0;   // build() 时填
    };

    PEWriter(const Section& text, const Section& data);

    void setEntry(const std::string& sym) { entry_ = sym; }
    void setEntryRva(uint32_t rva) { entryRva_ = rva; }
    void addImport(const std::string& dll, const std::string& func);
    void addReloc(uint32_t rva) { relocs_.push_back(rva); }
    // 资源：type/name 用整数 ID（RT_ICON=3, RT_GROUP_ICON=14, RT_VERSION=16, RT_BITMAP=2）
    void addResource(uint32_t type, uint32_t name, const std::vector<uint8_t>& data);
    // 导出函数
    struct Export { std::string name; uint32_t rva; };
    void addExport(const std::string& name, uint32_t rva) { exports_.push_back({name, rva}); }
    void setGuiMode() { guiMode_ = true; }
    void setDllMode() { dllMode_ = true; }

    // 计算布局，返回 {funcName: VA}（VA = ImageBase + IAT_RVA）
    // 调用此函数后把这些符号注入 assembler 符号表
    std::unordered_map<std::string, uint32_t> layout();

    std::vector<uint8_t> build();

private:
    const Section& text_;
    const Section& data_;
    struct Resource {
        uint32_t type;
        uint32_t name;
        std::vector<uint8_t> data;
    };
    std::vector<ImportFunc> imports_;
    std::vector<uint32_t> relocs_;
    std::vector<Resource> resources_;
    std::vector<Export> exports_;
    std::string entry_ = "_start";
    uint32_t entryRva_ = 0x1000;
    bool guiMode_ = false;
    bool dllMode_ = false;

    static constexpr uint32_t ImageBase = 0x00400000;
    static constexpr uint32_t SectionAlign = 0x1000;
    static constexpr uint32_t FileAlign = 0x200;

    // 布局结果
    uint32_t rdataRva_ = 0x2000;
    uint32_t textRva_  = 0x1000;
    uint32_t dataRva_  = 0x3000;
    bool laidOut_ = false;
};

} // namespace rasm
