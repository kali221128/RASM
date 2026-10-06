#include "pe.h"
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <map>
#include <vector>

namespace rasm {

static inline void put16(uint8_t* p, uint16_t v){ p[0]=v&0xff; p[1]=(v>>8)&0xff; }
static inline void put32(uint8_t* p, uint32_t v){
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}

PEWriter::PEWriter(const Section& text, const Section& data)
    : text_(text), data_(data) {}

void PEWriter::addImport(const std::string& dll, const std::string& func) {
    imports_.push_back({dll, func, 0});
}

std::unordered_map<std::string, uint32_t> PEWriter::layout() {
    std::unordered_map<std::string, uint32_t> result;
    if (imports_.empty()) return result;

    // 按 DLL 分组
    std::vector<std::string> dllOrder;
    std::map<std::string, std::vector<int>> dllFuncs;
    for (uint32_t i = 0; i < imports_.size(); i++) {
        auto& dll = imports_[i].dll;
        if (!dllFuncs.count(dll)) dllOrder.push_back(dll);
        dllFuncs[dll].push_back(i);
    }

    // .rdata 布局（必须与 build() 完全一致）：
    //   impDesc 数组：numDlls*20 + 20(null)
    //   每个 DLL：dllName\0 + ILT(n*4+4) + IAT(n*4+4) + 每个函数的 HintName(2+name+1)
    uint32_t off = (uint32_t)(dllOrder.size() * 20 + 20);
    for (auto& dll : dllOrder) {
        auto& idxs = dllFuncs[dll];
        off += (uint32_t)dll.size() + 1;                    // dllName
        uint32_t iltRva = rdataRva_ + off;
        off += (uint32_t)idxs.size() * 4 + 4;                // ILT
        uint32_t iatRva = rdataRva_ + off;
        off += (uint32_t)idxs.size() * 4 + 4;              // IAT
        for (uint32_t k = 0; k < idxs.size(); k++) {
            // Hint/Name 串：2字节 hint + name + '\0'
            off += 2 + (uint32_t)imports_[idxs[k]].func.size() + 1;
            imports_[idxs[k]].iatRva = iatRva + k*4;
            result[imports_[idxs[k]].func] = ImageBase + iatRva + k*4;
        }
        (void)iltRva;
    }
    laidOut_ = true;
    return result;
}

void PEWriter::addResource(uint32_t type, uint32_t name, const std::vector<uint8_t>& data) {
    resources_.push_back({type, name, data});
}

std::vector<uint8_t> PEWriter::build() {
    if (!laidOut_) layout();

    uint32_t n = (uint32_t)imports_.size();
    bool hasData = !data_.data.empty();
    bool hasRsrc = !resources_.empty();
    bool hasReloc = !relocs_.empty();
    uint32_t numSections = (hasData ? 3 : 2) + (hasRsrc ? 1 : 0) + (hasReloc ? 1 : 0);
    uint32_t relocRva = 0, relocRaw = 0, relocSize = 0;

    // 按 DLL 分组
    std::vector<std::string> dllOrder;
    std::map<std::string, std::vector<int>> dllFuncs;
    for (uint32_t i = 0; i < imports_.size(); i++) {
        auto& dll = imports_[i].dll;
        if (!dllFuncs.count(dll)) dllOrder.push_back(dll);
        dllFuncs[dll].push_back(i);
    }
    uint32_t numDlls = (uint32_t)dllOrder.size();

    // 计算 .rdata 大小
    uint32_t rdataSize = numDlls * 20 + 20;  // impDesc 数组 + null
    for (auto& dll : dllOrder) {
        rdataSize += (uint32_t)dll.size() + 1;           // dllName
        rdataSize += (uint32_t)dllFuncs[dll].size() * 4 + 4;  // ILT
        rdataSize += (uint32_t)dllFuncs[dll].size() * 4 + 4;  // IAT
        for (auto fi : dllFuncs[dll]) {
            rdataSize += 2 + (uint32_t)imports_[fi].func.size() + 1;  // Hint/Name
        }
    }
    // Export Directory 大小
    bool hasExport = !exports_.empty();
    if (hasExport) {
        rdataSize += 40;  // Export Directory
        rdataSize += 8;   // DLL 名称字符串 (assm.dll\0)
        rdataSize += (uint32_t)exports_.size() * 4;  // AddressOfFunctions
        rdataSize += (uint32_t)exports_.size() * 4;  // AddressOfNames
        rdataSize += (uint32_t)exports_.size() * 2;  // AddressOfNameOrdinals
        for (auto& e : exports_) rdataSize += (uint32_t)e.name.size() + 1;  // 名称
    }
    rdataSize = (rdataSize + 15) & ~15;

    // 文件布局
    const uint32_t textRaw = 0x200;
    const uint32_t rdataRaw = 0x400;
    const uint32_t dataRaw = 0x600;
    // .rsrc 布局
    uint32_t rsrcRaw = 0;
    uint32_t rsrcRva = 0;
    uint32_t rsrcSize = 0;
    if (hasRsrc) {
        // rsrcRva 紧跟最后一个段
        rsrcRva = hasData ? 0x4000 : 0x3000;
        // 按 type 分组
        std::vector<uint32_t> types;
        std::map<uint32_t, std::vector<int>> typeGroups;
        for (uint32_t i = 0; i < resources_.size(); i++) {
            uint32_t t = resources_[i].type;
            if (!typeGroups.count(t)) types.push_back(t);
            typeGroups[t].push_back(i);
        }
        // 计算目录树大小
        rsrcSize = 16 + (uint32_t)types.size() * 8;  // 根目录
        for (auto& t : types) {
            rsrcSize += 16 + (uint32_t)typeGroups[t].size() * 8;  // type 子目录
            rsrcSize += (uint32_t)typeGroups[t].size() * (16 + 8);  // name 子目录
        }
        rsrcSize += (uint32_t)resources_.size() * 16;  // 数据条目
        for (auto& r : resources_) rsrcSize += (uint32_t)r.data.size();
        rsrcSize = (rsrcSize + 0x1FF) & ~0x1FF;  // FileAlign 对齐

        uint32_t fileSize = dataRaw;
        if (hasData) fileSize = dataRaw + ((data_.data.size()+0x1FF)&~0x1FF);
        else fileSize = rdataRaw + ((rdataSize+0x1FF)&~0x1FF);
        rsrcRaw = fileSize;
        fileSize += rsrcSize;
    }
    // 先算 .reloc 大小和 RVA（只依赖 relocs_ 和段布局）
    if (hasReloc) {
        std::map<uint32_t, std::vector<uint32_t>> pages;
        for (uint32_t rva : relocs_) pages[rva & ~0xFFF].push_back(rva & 0xFFF);
        relocSize = 0;
        for (auto& [page, entries] : pages) {
            relocSize += 8 + (uint32_t)entries.size() * 2;
        }
        relocSize = (relocSize + 0x1FF) & ~0x1FF;
        relocRva = hasRsrc ? (rsrcRva + ((rsrcSize+0xFFF)&~0xFFF))
                           : (hasData ? 0x4000 : 0x3000);
    }
    uint32_t fileSize;
    if (hasRsrc) fileSize = rsrcRaw + rsrcSize;
    else if (hasData) fileSize = dataRaw + ((data_.data.size()+0x1FF)&~0x1FF);
    else fileSize = rdataRaw + ((rdataSize+0x1FF)&~0x1FF);
    if (hasReloc) {
        // relocRaw 紧跟最后一个段
        if (hasRsrc) relocRaw = rsrcRaw + rsrcSize;
        else if (hasData) relocRaw = dataRaw + ((data_.data.size()+0x1FF)&~0x1FF);
        else relocRaw = rdataRaw + ((rdataSize+0x1FF)&~0x1FF);
        fileSize = relocRaw + relocSize;
    }

    std::vector<uint8_t> exe(fileSize, 0);

    // ---- DOS Header ----
    exe[0x00] = 'M'; exe[0x01] = 'Z';
    put32(exe.data() + 0x3C, 0x40);

    // ---- PE Signature ----
    exe[0x40] = 'P'; exe[0x41] = 'E'; exe[0x42] = 0; exe[0x43] = 0;

    // ---- COFF File Header @ 0x44 ----
    uint8_t* coff = exe.data() + 0x44;
    put16(coff + 0, 0x014C);
    put16(coff + 2, (uint16_t)numSections);
    put32(coff + 4, 0);
    put32(coff + 8, 0);
    put32(coff + 12, 0);
    put16(coff + 16, 0xE0);
    uint16_t chars = hasReloc ? 0x0102 : 0x0103;
    if (dllMode_) chars |= 0x2000;  // IMAGE_FILE_DLL
    put16(coff + 18, chars);

    // ---- Optional Header @ 0x58 ----
    uint8_t* opt = exe.data() + 0x58;
    put16(opt + 0x00, 0x010B);
    opt[0x02] = 9; opt[0x03] = 0;
    uint32_t sizeOfCode = ((text_.data.size() + 0x1FF) / 0x200) * 0x200;
    put32(opt + 0x04, sizeOfCode);
    uint32_t sizeOfInit = ((rdataSize + 0xFFF) & ~0xFFF);
    if (hasData) sizeOfInit += ((data_.data.size() + 0xFFF) & ~0xFFF);
    put32(opt + 0x08, sizeOfInit);
    put32(opt + 0x0C, 0);
    uint32_t entryRva = entryRva_;
    put32(opt + 0x10, entryRva);
    put32(opt + 0x14, textRva_);
    put32(opt + 0x18, dataRva_);
    put32(opt + 0x1C, ImageBase);
    put32(opt + 0x20, SectionAlign);
    put32(opt + 0x24, FileAlign);
    put16(opt + 0x28, 4); put16(opt + 0x2A, 0);
    put16(opt + 0x2C, 0); put16(opt + 0x2E, 0);
    put16(opt + 0x30, 4); put16(opt + 0x32, 0);
    put32(opt + 0x34, 0);
    uint32_t highestEnd = rdataRva_ + ((rdataSize + 0xFFF) & ~0xFFF);
    if (hasData) highestEnd = dataRva_ + ((data_.data.size() + 0xFFF) & ~0xFFF);
    if (hasRsrc) highestEnd = rsrcRva + ((rsrcSize + 0xFFF) & ~0xFFF);
    if (hasReloc) highestEnd = relocRva + ((relocSize + 0xFFF) & ~0xFFF);
    put32(opt + 0x38, highestEnd);
    put32(opt + 0x3C, 0x200);
    put32(opt + 0x40, 0);
    put16(opt + 0x44, guiMode_ ? 2 : 3);  // GUI=2, Console=3
    put16(opt + 0x46, hasReloc ? 0x0140 : 0);  // DYNAMIC_BASE|NX_COMPAT if reloc
    put32(opt + 0x48, 0x100000);
    put32(opt + 0x4C, 0x1000);
    put32(opt + 0x50, 0x100000);
    put32(opt + 0x54, 0x1000);
    put32(opt + 0x58, 0);
    put32(opt + 0x5C, 16);

    // DataDirectories
    uint8_t* dd = opt + 0x60;
    put32(dd + 0, 0); put32(dd + 4, 0);
    // Import Directory
    uint32_t impDescSize = numDlls * 20 + 20;
    put32(dd + 8, rdataRva_); put32(dd + 12, impDescSize);
    // Resource Directory
    if (hasRsrc) {
        put32(dd + 16, rsrcRva); put32(dd + 20, rsrcSize);
    }
    // Base Relocation Directory
    if (hasReloc) {
        put32(dd + 40, relocRva); put32(dd + 44, relocSize);
    }
    for (int i = 2; i < 16; i++) {
        if (i == 2 && hasRsrc) continue;
        if (i == 5 && hasReloc) continue;
        put32(dd + i*8, 0);
        put32(dd + i*8 + 4, 0);
    }

    // ---- Section Table @ 0x138 ----
    uint8_t* sec = exe.data() + 0x138;
    auto writeSection = [&](uint8_t* s, const char* name, uint32_t vsize, uint32_t vaddr,
                            uint32_t rawsize, uint32_t rawaddr, uint32_t chars) {
        std::memset(s, 0, 40);
        std::strcpy((char*)s, name);
        put32(s + 8, vsize);
        put32(s + 12, vaddr);
        put32(s + 16, rawsize);
        put32(s + 20, rawaddr);
        put32(s + 36, chars);
    };
    writeSection(sec + 0, ".text", (uint32_t)text_.data.size(), textRva_,
                 sizeOfCode, textRaw, 0x60000020);
    writeSection(sec + 40, ".rdata", rdataSize, rdataRva_,
                 ((rdataSize+0x1FF)&~0x1FF), rdataRaw, 0x40000040);
    if (hasData) {
        writeSection(sec + 80, ".data", (uint32_t)data_.data.size(), dataRva_,
                     ((data_.data.size()+0x1FF)&~0x1FF), dataRaw, 0xC0000040);
    }
    if (hasRsrc) {
        writeSection(sec + (hasData ? 120 : 80), ".rsrc", rsrcSize, rsrcRva,
                     rsrcSize, rsrcRaw, 0x40000040);
    }
    // .reloc 段（relocSize/relocRaw/relocRva 已在前面算好）
    if (hasReloc) {
        writeSection(sec + ((hasData?120:80) + (hasRsrc?40:0)), ".reloc",
                     relocSize, relocRva, relocSize, relocRaw, 0x42000040);
    }

    // ---- 写 .text ----
    for (size_t i = 0; i < text_.data.size(); i++) exe[textRaw + i] = text_.data[i];

    // ---- 写 .rdata (import 表) ----
    uint8_t* rd = exe.data() + rdataRaw;
    uint32_t off = numDlls * 20 + 20;  // impDesc 数组 + null

    for (uint32_t di = 0; di < numDlls; di++) {
        auto& dll = dllOrder[di];
        auto& idxs = dllFuncs[dll];
        uint32_t base = di * 20;

        // dllName
        uint32_t dllNameRva = rdataRva_ + off;
        std::strcpy((char*)(rd + off), dll.c_str());
        off += (uint32_t)dll.size() + 1;

        // ILT
        uint32_t iltRva = rdataRva_ + off;
        off += (uint32_t)idxs.size() * 4 + 4;

        // IAT
        uint32_t iatRva = rdataRva_ + off;
        off += (uint32_t)idxs.size() * 4 + 4;

        // 写 impDesc
        put32(rd + base + 0, iltRva);
        put32(rd + base + 4, 0);
        put32(rd + base + 8, 0);
        put32(rd + base + 12, dllNameRva);
        put32(rd + base + 16, iatRva);

        // ILT / IAT entries 指向 Hint/Name
        for (uint32_t k = 0; k < idxs.size(); k++) {
            uint32_t hintRva = rdataRva_ + off;
            uint8_t* hn = rd + off;
            put16(hn + 0, 0);
            std::strcpy((char*)(hn + 2), imports_[idxs[k]].func.c_str());
            off += 2 + (uint32_t)imports_[idxs[k]].func.size() + 1;

            put32(rd + (iltRva - rdataRva_) + k*4, hintRva);
            put32(rd + (iatRva - rdataRva_) + k*4, hintRva);
            imports_[idxs[k]].iatRva = iatRva + k*4;
        }
        // null terminator
        put32(rd + (iltRva - rdataRva_) + (uint32_t)idxs.size()*4, 0);
        put32(rd + (iatRva - rdataRva_) + (uint32_t)idxs.size()*4, 0);
    }
    // impDesc null 已清零（exe 初始化为 0）

    // ---- 写 Export Directory ----
    if (hasExport) {
        uint32_t expDirRva = rdataRva_ + off;
        uint32_t expDirOff = off;
        off += 40;  // Export Directory

        uint32_t dllNameRva = rdataRva_ + off;
        std::strcpy((char*)(rd + off), "assm.dll");
        off += 8;

        uint32_t funcTableRva = rdataRva_ + off;
        off += (uint32_t)exports_.size() * 4;

        uint32_t nameTableRva = rdataRva_ + off;
        off += (uint32_t)exports_.size() * 4;

        uint32_t ordTableRva = rdataRva_ + off;
        off += (uint32_t)exports_.size() * 2;

        // 写导出名称字符串和表
        for (uint32_t i = 0; i < exports_.size(); i++) {
            // AddressOfFunctions
            put32(rd + (funcTableRva - rdataRva_) + i*4, exports_[i].rva);
            // AddressOfNames
            uint32_t nameRva = rdataRva_ + off;
            std::strcpy((char*)(rd + off), exports_[i].name.c_str());
            off += (uint32_t)exports_[i].name.size() + 1;
            put32(rd + (nameTableRva - rdataRva_) + i*4, nameRva);
            // AddressOfNameOrdinals
            put16(rd + (ordTableRva - rdataRva_) + i*2, (uint16_t)i);
        }

        // 写 Export Directory
        uint8_t* ed = rd + expDirOff;
        put32(ed + 0, 0);           // Characteristics
        put32(ed + 4, 0);           // TimeDateStamp
        put16(ed + 8, 0);           // MajorVersion
        put16(ed + 10, 0);          // MinorVersion
        put32(ed + 12, dllNameRva);  // Name RVA
        put32(ed + 16, 1);           // Base ordinal
        put32(ed + 20, (uint32_t)exports_.size());  // NumberOfFunctions
        put32(ed + 24, (uint32_t)exports_.size());  // NumberOfNames
        put32(ed + 28, funcTableRva);
        put32(ed + 32, nameTableRva);
        put32(ed + 36, ordTableRva);

        // DataDirectory[0] = Export Directory
        uint8_t* dd = exe.data() + 0x58 + 96;  // Optional Header + DataDirectories
        put32(dd + 0, expDirRva);
        put32(dd + 4, 40);
    }

    // ---- 写 .data ----
    if (hasData) {
        for (size_t i = 0; i < data_.data.size(); i++) exe[dataRaw + i] = data_.data[i];
    }

    // ---- 写 .rsrc（资源目录树）----
    if (hasRsrc) {
        // 按 type 分组
        std::vector<uint32_t> types;
        std::map<uint32_t, std::vector<int>> typeGroups;
        for (uint32_t i = 0; i < resources_.size(); i++) {
            uint32_t t = resources_[i].type;
            if (!typeGroups.count(t)) types.push_back(t);
            typeGroups[t].push_back(i);
        }
        uint8_t* rs = exe.data() + rsrcRaw;
        uint32_t off = 0;
        // 目录树偏移计算
        // 根目录在 off=0
        uint32_t rootOff = off;
        off += 16 + (uint32_t)types.size() * 8;
        // 每个 type 的子目录偏移
        std::vector<uint32_t> typeDirOff(types.size());
        for (uint32_t ti = 0; ti < types.size(); ti++) {
            typeDirOff[ti] = off;
            off += 16 + (uint32_t)typeGroups[types[ti]].size() * 8;
        }
        // 每个 name 的子目录偏移
        std::vector<std::vector<uint32_t>> nameDirOff(types.size());
        for (uint32_t ti = 0; ti < types.size(); ti++) {
            auto& idxs = typeGroups[types[ti]];
            for (uint32_t ni = 0; ni < idxs.size(); ni++) {
                nameDirOff[ti].push_back(off);
                off += 16 + 8;  // 子目录头 + 1 个语言条目
            }
        }
        // 数据条目
        uint32_t dataEntryOff = off;
        off += (uint32_t)resources_.size() * 16;
        // 资源数据
        uint32_t dataOff = off;

        // 写根目录
        put16(rs + rootOff + 12, 0);  // NumberOfNamedEntries
        put16(rs + rootOff + 14, (uint16_t)types.size());  // NumberOfIdEntries
        for (uint32_t ti = 0; ti < types.size(); ti++) {
            uint32_t eoff = rootOff + 16 + ti * 8;
            put32(rs + eoff, types[ti]);  // Type ID
            put32(rs + eoff + 4, typeDirOff[ti] | 0x80000000);  // 子目录
        }

        // 写每个 type 的子目录
        for (uint32_t ti = 0; ti < types.size(); ti++) {
            uint32_t tdo = typeDirOff[ti];
            auto& idxs = typeGroups[types[ti]];
            put16(rs + tdo + 12, 0);
            put16(rs + tdo + 14, (uint16_t)idxs.size());
            for (uint32_t ni = 0; ni < idxs.size(); ni++) {
                uint32_t eoff = tdo + 16 + ni * 8;
                put32(rs + eoff, resources_[idxs[ni]].name);  // Name ID
                put32(rs + eoff + 4, nameDirOff[ti][ni] | 0x80000000);  // 子目录
            }
        }

        // 写每个 name 的子目录（一个语言 ID=0）
        for (uint32_t ti = 0; ti < types.size(); ti++) {
            auto& idxs = typeGroups[types[ti]];
            for (uint32_t ni = 0; ni < idxs.size(); ni++) {
                uint32_t ndo = nameDirOff[ti][ni];
                put16(rs + ndo + 12, 0);
                put16(rs + ndo + 14, 1);  // 1 个语言
                uint32_t eoff = ndo + 16;
                put32(rs + eoff, 0x409);  // 语言 ID (English US)
                put32(rs + eoff + 4, dataEntryOff + idxs[ni] * 16);  // 数据条目
            }
        }

        // 写数据条目
        for (uint32_t i = 0; i < resources_.size(); i++) {
            uint32_t eoff = dataEntryOff + i * 16;
            put32(rs + eoff, rsrcRva + dataOff);  // OffsetToData (RVA)
            put32(rs + eoff + 4, (uint32_t)resources_[i].data.size());  // Size
            put32(rs + eoff + 8, 0);  // CodePage
            put32(rs + eoff + 12, 0);  // Reserved
        }

        // 写资源数据
        uint32_t dpos = dataOff;
        for (auto& r : resources_) {
            for (uint8_t b : r.data) rs[dpos++] = b;
        }
    }

    // ---- 写 .reloc ----
    if (hasReloc) {
        std::map<uint32_t, std::vector<uint32_t>> pages;
        for (uint32_t rva : relocs_) pages[rva & ~0xFFF].push_back(rva & 0xFFF);
        uint8_t* rb = exe.data() + relocRaw;
        uint32_t off = 0;
        for (auto& [page, entries] : pages) {
            put32(rb + off, page);
            uint32_t blockSize = 8 + (uint32_t)entries.size() * 2;
            put32(rb + off + 4, blockSize);
            off += 8;
            for (uint32_t e : entries) {
                uint16_t entry = (uint16_t)((3 << 12) | e);  // type=3 HIGHLOW
                put16(rb + off, entry);
                off += 2;
            }
        }
    }

    // PE 自检
    {
        auto rd = exe.data();
        size_t sz = exe.size();
        // DOS 签名
        if (sz < 2 || rd[0] != 'M' || rd[1] != 'Z') {
            fprintf(stderr, "PE WARN: bad DOS signature\n");
        }
        // e_lfanew
        uint32_t peOff = rd[0x3C] | (rd[0x3D]<<8) | (rd[0x3E]<<16) | (rd[0x3F]<<24);
        if (peOff + 24 > sz) {
            fprintf(stderr, "PE WARN: PE header out of bounds\n");
        }
        // PE 签名
        if (sz < peOff + 4 || rd[peOff] != 'P' || rd[peOff+1] != 'E' || rd[peOff+2] != 0 || rd[peOff+3] != 0) {
            fprintf(stderr, "PE WARN: bad PE signature\n");
        }
        // Optional Header 魔数
        uint16_t optMagic = rd[peOff+24] | (rd[peOff+25]<<8);
        if (optMagic != 0x10B) {
            fprintf(stderr, "PE WARN: bad Optional Header magic 0x%X\n", optMagic);
        }
        // EntryPointRVA
        uint32_t ep = rd[peOff+24+16] | (rd[peOff+24+17]<<8) | (rd[peOff+24+18]<<16) | (rd[peOff+24+19]<<24);
        if (ep < 0x1000 || ep >= 0x1000 + text_.data.size()) {
            fprintf(stderr, "PE WARN: EntryPointRVA 0x%X outside .text\n", ep);
        }
    }

    return exe;
}

} // namespace rasm
