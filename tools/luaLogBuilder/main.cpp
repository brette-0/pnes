// lua_log_builder -- reads a linked NES demo.elf (llvm-mos, ELF32) and writes
// logdata.lua next to the shipped demo.nes ROM, for tools/pnes.lua to load
// in Mesen. See include/platform-nes/logger.hpp for what it's reading and
// why: log() call sites cost zero ROM bytes, so the only trace of them is
// (a) compiler-generated (file, line, message) metadata sitting in the
// linked .elf's .pnes_log section (never in demo.nes itself), and (b) the
// same .elf's ordinary DWARF .debug_line table, which this tool uses to turn
// that (file, line) into a real instruction address -- nothing here ever
// opens a .cpp/.hpp source file.
//
// The remaining problem this tool solves is turning that address into a
// flat, bank-switching-agnostic byte offset into demo.nes ("LMA"). The
// naive approach -- trusting the .elf's own section file offsets -- is
// WRONG for this toolchain: llvm-mos produces demo.nes directly from the
// project's own linker-script OUTPUT_FORMAT{FULL(region)...} ordering (see
// src/nes/mappers/mmc3-helper.ld's own comments), which is project-specific
// and unrelated to demo.nes.elf's own internal section layout. Instead,
// each real (ALLOC, non-debug) section's own byte CONTENT is located
// directly inside demo.nes by search -- content is identical between the
// two files regardless of layout, so this never needs to know a project's
// OUTPUT_FORMAT ordering, any mapper's bank-encoding scheme, or anything
// else project/mapper-specific. Verified against a real build before
// writing this: every PRG-ROM/CHR-ROM section content-matched demo.nes at
// exactly one position.
//
// Usage: lua_log_builder --elf <path/to/demo.nes.elf> --output <path/to/logdata.lua>
//        [--rom <path/to/demo.nes>]   (default: --elf path with a trailing
//                                      ".elf" stripped, matching this
//                                      project's own naming convention)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

std::vector<u8> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

u16 Read16(const u8 *p) { u16 v; std::memcpy(&v, p, 2); return v; }
u32 Read32(const u8 *p) { u32 v; std::memcpy(&v, p, 4); return v; }

std::string ReadCString(const std::vector<u8> &data, std::size_t off) {
    const auto *start = reinterpret_cast<const char *>(data.data() + off);
    return std::string(start); // stops at the embedded NUL, same as strlen
}

// ---------------------------------------------------------------------
// Minimal ELF32 reader: just enough to enumerate sections and symbols.
// ---------------------------------------------------------------------

struct Section {
    std::string name;
    u32 type = 0;
    u32 flags = 0;
    u32 addr = 0;
    u32 offset = 0;
    u32 size = 0;
    u32 link = 0;
};

struct Symbol {
    std::string name;
    u32 value = 0;
    u32 size = 0;
    u16 shndx = 0;
};

constexpr u32 SHT_SYMTAB = 2;
constexpr u32 SHT_STRTAB = 3;
constexpr u32 SHF_ALLOC = 0x2;

struct Elf {
    std::vector<u8> data;
    std::vector<Section> sections;
    std::vector<Symbol> symbols;

    static Elf Load(const std::string &path) {
        Elf elf;
        elf.data = ReadFile(path);
        const u8 *d = elf.data.data();

        if (elf.data.size() < 52 || std::memcmp(d, "\x7f""ELF", 4) != 0)
            throw std::runtime_error(path + " is not an ELF file");
        if (d[4] != 1) // EI_CLASS: ELFCLASS32
            throw std::runtime_error(path + " is not ELF32 (llvm-mos always emits ELF32)");

        const u32 e_shoff = Read32(d + 0x20);
        const u16 e_shentsize = Read16(d + 0x2e);
        const u16 e_shnum = Read16(d + 0x30);
        const u16 e_shstrndx = Read16(d + 0x32);

        struct RawShdr { u32 name, type, flags, addr, offset, size, link, info, addralign, entsize; };
        std::vector<RawShdr> raw(e_shnum);
        for (u16 i = 0; i < e_shnum; ++i) {
            const u8 *p = d + e_shoff + std::size_t(i) * e_shentsize;
            raw[i] = { Read32(p), Read32(p + 4), Read32(p + 8), Read32(p + 12),
                       Read32(p + 16), Read32(p + 20), Read32(p + 24), Read32(p + 28),
                       Read32(p + 32), Read32(p + 36) };
        }

        const u32 shstr_off = raw[e_shstrndx].offset;
        elf.sections.resize(e_shnum);
        for (u16 i = 0; i < e_shnum; ++i) {
            elf.sections[i] = { ReadCString(elf.data, shstr_off + raw[i].name),
                                 raw[i].type, raw[i].flags, raw[i].addr,
                                 raw[i].offset, raw[i].size, raw[i].link };
        }

        // Symbol table: first SHT_SYMTAB section found, string table via its sh_link.
        for (u16 i = 0; i < e_shnum; ++i) {
            if (raw[i].type != SHT_SYMTAB) continue;
            const u32 strtab_off = raw[raw[i].link].offset;
            const u32 n = raw[i].size / 16; // sizeof(Elf32_Sym) == 16
            elf.symbols.reserve(n);
            for (u32 s = 0; s < n; ++s) {
                const u8 *p = d + raw[i].offset + std::size_t(s) * 16;
                const u32 name_off = Read32(p);
                const u32 value = Read32(p + 4);
                const u32 size = Read32(p + 8);
                const u16 shndx = Read16(p + 14);
                if (name_off == 0) continue;
                elf.symbols.push_back({ ReadCString(elf.data, strtab_off + name_off), value, size, shndx });
            }
            break;
        }
        return elf;
    }

    const Section *FindSection(const std::string &name) const {
        for (auto &s : sections) if (s.name == name) return &s;
        return nullptr;
    }

    // Section whose [addr, addr+size) contains `address`, restricted to real
    // (ALLOC, non-debug, non-.pnes_log) content -- see this file's header
    // comment on why .pnes_log itself must never be a resolution candidate.
    const Section *FindContaining(u32 address) const {
        for (auto &s : sections) {
            if (!(s.flags & SHF_ALLOC) || s.size == 0) continue;
            if (s.name == ".pnes_log" || s.name.rfind(".debug", 0) == 0) continue;
            if (address >= s.addr && address < s.addr + s.size) return &s;
        }
        return nullptr;
    }
};

// ---------------------------------------------------------------------
// .pnes_log reader. Layout is hand-decoded to match logger.hpp's Entry
// exactly (const char *file; u32 line; const char *message; const Arg *args;
// u8 arg_count; u8 kind;) AS COMPILED FOR THIS TARGET: llvm-mos pointers are
// 2 bytes (real 6502 hardware addresses), so a pointer field holding one of
// this section's own fake 0x04000000-based addresses is truncated to that
// address's low 16 bits -- which is exactly the byte offset from the
// section's own start, since debug-log.ld pins the section's ORIGIN to
// 0x04000000 (zero low bits) for precisely this reason. Confirmed against a
// real build: Entry symbols are 12 bytes (2 + 4 + 2 + 2 + 1 + 1), not the 18
// a native 4-byte-pointer, padded-struct host would guess -- this target
// packs struct fields with no alignment padding at all.
//
// Arg::address is DIFFERENT: it's a real, live CPU address (e.g. somewhere
// in NES RAM), not a .pnes_log-relative offset, since it must still be
// meaningful to tools/pnes.lua at emulation time, long after .pnes_log
// itself has been discarded. It's read here as a plain 16-bit value, with
// no offset math against the section's own base.
// ---------------------------------------------------------------------

// Mirrors logger.hpp's PNES_LOG_KIND_LOG/PNES_LOG_KIND_PAUSE exactly.
constexpr u8 PNES_LOG_KIND_LOG = 0;
constexpr u8 PNES_LOG_KIND_PAUSE = 1;

struct LogArg {
    u16 address = 0;
    u8 size = 0;
    bool is_signed = false;
};

struct LogEntry {
    std::string file;
    u32 line = 0;
    std::string message;
    std::vector<LogArg> args;
    u8 kind = PNES_LOG_KIND_LOG;
};

std::vector<LogEntry> ReadPnesLog(const Elf &elf) {
    const Section *sec = elf.FindSection(".pnes_log");
    std::vector<LogEntry> out;
    if (!sec) return out; // no log()/pause() calls anywhere in this build

    auto stringAt = [&](u16 lowOffset) {
        return ReadCString(elf.data, sec->offset + lowOffset);
    };

    for (auto &sym : elf.symbols) {
        if (sym.name.find("_pnes_log_entry_") == std::string::npos) continue;
        if (sym.size != 12) {
            std::cerr << "lua_log_builder: warning: " << sym.name
                      << " is " << sym.size << " bytes, expected 12 -- skipping "
                      << "(logger.hpp's Entry layout and this tool's reader have drifted apart)\n";
            continue;
        }
        const u32 entryOff = sec->offset + (sym.value - sec->addr);
        const u8 *p = elf.data.data() + entryOff;
        const u16 fileLow = Read16(p);
        const u32 line = Read32(p + 2);
        const u16 msgLow = Read16(p + 6);
        const u16 argsLow = Read16(p + 8);
        const u8 argCount = p[10];
        const u8 kind = p[11];

        LogEntry entry{ stringAt(fileLow), line, stringAt(msgLow), {}, kind };
        const u8 *argBase = elf.data.data() + sec->offset + argsLow;
        for (u8 i = 0; i < argCount; ++i) {
            const u8 *a = argBase + i * 4;
            entry.args.push_back({ Read16(a), a[2], a[3] != 0 });
        }
        out.push_back(std::move(entry));
    }
    return out;
}

// ---------------------------------------------------------------------
// Minimal DWARF5 .debug_line reader -- just the line-number program, built
// into a (file, line) -> address table. Only what this project's llvm-mos
// toolchain actually emits (DWARF32, version 5) is supported.
// ---------------------------------------------------------------------

struct Reader {
    const u8 *p;
    const u8 *end;

    u8 U8() { if (p >= end) throw std::runtime_error("DWARF: read past end"); return *p++; }
    u16 U16() { u16 v = Read16(p); p += 2; return v; }
    u32 U32() { u32 v = Read32(p); p += 4; return v; }
    std::string CStr() {
        const char *s = reinterpret_cast<const char *>(p);
        std::size_t n = std::strlen(s);
        p += n + 1;
        return std::string(s, n);
    }
    std::vector<u8> Bytes(std::size_t n) { auto v = std::vector<u8>(p, p + n); p += n; return v; }

    u64 ULEB() {
        u64 result = 0; unsigned shift = 0; u8 b;
        do { b = U8(); result |= u64(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
        return result;
    }
    i32 SLEB() {
        i64 result = 0; unsigned shift = 0; u8 b;
        do { b = U8(); result |= i64(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) result |= -(i64(1) << shift);
        return static_cast<i32>(result);
    }
};

// DW_FORM values actually seen from this toolchain's DWARF5 file/dir tables.
constexpr u64 DW_FORM_string = 0x08;
constexpr u64 DW_FORM_strp = 0x0e;
constexpr u64 DW_FORM_udata = 0x0f;
constexpr u64 DW_FORM_data1 = 0x0b;
constexpr u64 DW_FORM_data2 = 0x05;
constexpr u64 DW_FORM_data4 = 0x06;
constexpr u64 DW_FORM_data8 = 0x07;
constexpr u64 DW_FORM_data16 = 0x1e;
constexpr u64 DW_FORM_line_strp = 0x1f;
constexpr u64 DW_FORM_block = 0x09;

constexpr u64 DW_LNCT_path = 1;
constexpr u64 DW_LNCT_directory_index = 2;

/// One directory/file-table attribute value: a string for path-shaped forms,
/// a number for everything else (in particular DW_FORM_udata, how
/// DW_LNCT_directory_index is encoded).
struct FormValue {
    std::optional<std::string> str;
    std::optional<u64> num;
};

/// Reads one attribute value per its DW_FORM. `debugStr`/`lineStr` are the
/// section bytes DW_FORM_strp/DW_FORM_line_strp offsets index into.
FormValue ReadFormValue(Reader &r, u64 form,
                         const std::vector<u8> &debugStr, const std::vector<u8> &lineStr) {
    switch (form) {
        case DW_FORM_string: return { r.CStr(), std::nullopt };
        case DW_FORM_strp: { u32 off = r.U32(); return { debugStr.empty() ? std::string() : ReadCString(debugStr, off), std::nullopt }; }
        case DW_FORM_line_strp: { u32 off = r.U32(); return { lineStr.empty() ? std::string() : ReadCString(lineStr, off), std::nullopt }; }
        case DW_FORM_udata: return { std::nullopt, r.ULEB() };
        case DW_FORM_data1: return { std::nullopt, r.U8() };
        case DW_FORM_data2: return { std::nullopt, r.U16() };
        case DW_FORM_data4: return { std::nullopt, r.U32() };
        case DW_FORM_data8: r.Bytes(8); return {};
        case DW_FORM_data16: r.Bytes(16); return {};
        case DW_FORM_block: { u64 n = r.ULEB(); r.Bytes(n); return {}; }
        default:
            throw std::runtime_error("DWARF: unsupported form 0x" + std::to_string(form) +
                                      " in file/directory table (unexpected DWARF producer/version)");
    }
}

struct LineRow { u32 file; u32 line; u32 address; };

/// One compilation unit's worth of resolved file paths and (file,line)->address rows.
struct LineTable {
    std::vector<std::string> files; // index == DWARF file index for this CU
    std::vector<LineRow> rows;      // in ascending address order (as emitted)
};

LineTable ParseLineProgram(Reader r, const std::vector<u8> &debugStr, const std::vector<u8> &lineStr) {
    LineTable table;
    const u8 *unitStart = r.p;
    const u32 unitLength = r.U32();
    const u8 *unitEnd = unitStart + 4 + unitLength;
    const u16 version = r.U16();
    if (version != 5)
        throw std::runtime_error("DWARF: .debug_line unit is version " + std::to_string(version) +
                                  ", only version 5 is supported");
    r.U8(); // address_size
    r.U8(); // segment_selector_size
    const u32 headerLength = r.U32();
    const u8 *programStart = r.p + headerLength;
    const u8 minInstLength = r.U8();
    r.U8(); // max_ops_per_instruction (always 1 on this target; VLIW op_index tracking is skipped)
    r.U8(); // default_is_stmt
    const i32 lineBase = static_cast<i32>(static_cast<std::int8_t>(r.U8()));
    const u8 lineRange = r.U8();
    const u8 opcodeBase = r.U8();
    std::vector<u8> stdOpcodeLengths(opcodeBase - 1);
    for (auto &v : stdOpcodeLengths) v = r.U8();

    auto readEntryFormatTable = [&](std::vector<std::pair<u64, u64>> &formats) {
        const u8 count = r.U8();
        formats.resize(count);
        for (auto &[type, form] : formats) { type = r.ULEB(); form = r.ULEB(); }
    };

    std::vector<std::pair<u64, u64>> dirFormats;
    readEntryFormatTable(dirFormats);
    const u64 dirCount = r.ULEB();
    std::vector<std::string> directories(dirCount);
    for (u64 i = 0; i < dirCount; ++i) {
        for (auto &[type, form] : dirFormats) {
            FormValue v = ReadFormValue(r, form, debugStr, lineStr);
            if (type == DW_LNCT_path && v.str) directories[i] = *v.str;
        }
    }

    // DWARF5 gives each file entry BOTH a name and a directory index; a name
    // can be relative (needs directories[dirIndex] prepended) even when
    // OTHER entries for the exact same physical file are already absolute --
    // confirmed on a real build: __FILE__'s absolute-path entry (index 0)
    // had zero line-table rows referencing it, while a second, dir-relative
    // entry for the identical file (by content -- same md5) is what the
    // actual line program uses. Every entry must therefore be resolved
    // correctly, not just the one that happens to already start with '/'.
    std::vector<std::pair<u64, u64>> fileFormats;
    readEntryFormatTable(fileFormats);
    const u64 fileCount = r.ULEB();
    table.files.resize(fileCount);
    for (u64 i = 0; i < fileCount; ++i) {
        std::string name;
        u64 dirIndex = 0;
        for (auto &[type, form] : fileFormats) {
            FormValue v = ReadFormValue(r, form, debugStr, lineStr);
            if (type == DW_LNCT_path && v.str) name = *v.str;
            else if (type == DW_LNCT_directory_index && v.num) dirIndex = *v.num;
        }
        if (!name.empty() && name.front() == '/') table.files[i] = name;
        else if (dirIndex < directories.size()) table.files[i] = directories[dirIndex] + "/" + name;
        else table.files[i] = name;
    }

    // ---- Line number program ----
    r.p = const_cast<u8 *>(programStart);
    struct State { u32 address = 0; u32 file = 1; u32 line = 1; bool endSeq = false; } st;
    st.file = version >= 5 ? 0 : 1; // DWARF5 file numbering starts at 0

    auto appendRow = [&] { table.rows.push_back({ st.file, st.line, st.address }); };

    while (r.p < unitEnd) {
        const u8 opcode = r.U8();
        if (opcode == 0) {
            const u64 len = r.ULEB();
            const u8 *extEnd = r.p + len;
            const u8 sub = r.U8();
            if (sub == 1) { // DW_LNE_end_sequence
                st.endSeq = true;
                appendRow();
                st = State{};
                st.file = 0;
            } else if (sub == 2) { // DW_LNE_set_address
                st.address = r.U32();
            } else {
                // DW_LNE_define_file / DW_LNE_set_discriminator / vendor ext: skip.
            }
            r.p = const_cast<u8 *>(extEnd);
        } else if (opcode < opcodeBase) {
            switch (opcode) {
                case 1: appendRow(); break;                                   // DW_LNS_copy
                case 2: st.address += static_cast<u32>(r.ULEB()) * minInstLength; break; // advance_pc
                case 3: st.line = static_cast<u32>(static_cast<i32>(st.line) + r.SLEB()); break; // advance_line
                case 4: st.file = static_cast<u32>(r.ULEB()); break;           // set_file
                case 5: r.ULEB(); break;                                      // set_column
                case 6: break;                                                // negate_stmt
                case 7: break;                                                // set_basic_block
                case 8: {                                                     // const_add_pc
                    const u8 adjusted = 255 - opcodeBase;
                    st.address += (adjusted / lineRange) * minInstLength;
                    break;
                }
                case 9: st.address += r.U16(); break;                         // fixed_advance_pc
                case 10: break;                                               // set_prologue_end
                case 11: break;                                               // set_epilogue_begin
                case 12: r.ULEB(); break;                                     // set_isa
                default:
                    for (u8 i = 0; i < stdOpcodeLengths[opcode - 1]; ++i) r.ULEB();
            }
        } else {
            const u8 adjusted = opcode - opcodeBase;
            st.address += (adjusted / lineRange) * minInstLength;
            st.line = static_cast<u32>(static_cast<i32>(st.line) + lineBase + (adjusted % lineRange));
            appendRow();
        }
    }

    r.p = const_cast<u8 *>(unitEnd);
    return table;
}

std::vector<LineTable> ParseDebugLine(const Elf &elf) {
    std::vector<LineTable> tables;
    const Section *sec = elf.FindSection(".debug_line");
    if (!sec) return tables;
    const Section *strSec = elf.FindSection(".debug_str");
    const Section *lineStrSec = elf.FindSection(".debug_line_str");
    std::vector<u8> debugStr, lineStr;
    if (strSec) debugStr.assign(elf.data.begin() + strSec->offset, elf.data.begin() + strSec->offset + strSec->size);
    if (lineStrSec) lineStr.assign(elf.data.begin() + lineStrSec->offset, elf.data.begin() + lineStrSec->offset + lineStrSec->size);

    const u8 *base = elf.data.data() + sec->offset;
    const u8 *end = base + sec->size;
    const u8 *cursor = base;
    while (cursor < end) {
        Reader r{ cursor, end };
        const u32 unitLength = Read32(cursor);
        const u8 *unitEnd = cursor + 4 + unitLength;
        tables.push_back(ParseLineProgram(r, debugStr, lineStr));
        cursor = unitEnd;
    }
    return tables;
}

/// Resolves (file, line) to the nearest real instruction address: an exact
/// (file,line) match if one exists, else the smallest address among rows in
/// the same file whose line is >= the target (the next real statement the
/// compiler kept a line-table entry for -- see logger.hpp's own header
/// comment on why log() itself can never have its own row).
std::optional<u32> ResolveAddress(const std::vector<LineTable> &tables,
                                   const std::string &file, u32 line) {
    std::optional<u32> exact, nearest;
    u32 nearestLine = 0xffffffffu;
    for (auto &t : tables) {
        // Find every file index in this CU whose resolved path matches.
        for (u32 fi = 0; fi < t.files.size(); ++fi) {
            if (t.files[fi] != file) continue;
            for (auto &row : t.rows) {
                if (row.file != fi) continue;
                if (row.line == line) {
                    if (!exact || row.address < *exact) exact = row.address;
                } else if (row.line > line && row.line < nearestLine) {
                    nearestLine = row.line;
                    nearest = row.address;
                } else if (row.line > line && row.line == nearestLine) {
                    if (!nearest || row.address < *nearest) nearest = row.address;
                }
            }
        }
    }
    return exact ? exact : nearest;
}

// ---------------------------------------------------------------------
// Content-fingerprint address -> flat demo.nes byte offset. See this file's
// header comment for why this, not .elf section file offsets, is correct.
// ---------------------------------------------------------------------

class RomLocator {
public:
    RomLocator(std::vector<u8> romBytes) : rom_(std::move(romBytes)) {}

    /// Returns the flat byte offset in demo.nes for `address`, which must
    /// fall inside `sec` (an ALLOC section from the same .elf). Caches one
    /// content-fingerprint search per section, since every log entry in the
    /// same section reuses it.
    std::optional<u32> Locate(const Elf &elf, const Section &sec, u32 address) {
        auto it = sectionBase_.find(sec.name);
        if (it == sectionBase_.end()) {
            it = sectionBase_.emplace(sec.name, FindSectionBase(elf, sec)).first;
        }
        if (!it->second) return std::nullopt;
        return *it->second + (address - sec.addr);
    }

private:
    std::optional<u32> FindSectionBase(const Elf &elf, const Section &sec) {
        const std::size_t needleLen = std::min<std::size_t>(sec.size, 256);
        if (needleLen == 0) return std::nullopt;
        const u8 *needle = elf.data.data() + sec.offset;

        std::optional<std::size_t> found;
        std::size_t count = 0;
        for (std::size_t i = 0; i + needleLen <= rom_.size(); ++i) {
            if (std::memcmp(rom_.data() + i, needle, needleLen) == 0) {
                ++count;
                if (!found) found = i;
                if (count > 1) break;
            }
        }
        if (count != 1) {
            std::cerr << "lua_log_builder: warning: " << sec.name
                      << "'s content did not match demo.nes exactly once (matches=" << count
                      << ") -- log points resolving into this section will be skipped\n";
            return std::nullopt;
        }
        std::cerr << "lua_log_builder: located " << sec.name << " (addr 0x" << std::hex << sec.addr
                   << std::dec << ") at demo.nes offset " << *found << "\n";
        return static_cast<u32>(*found);
    }

    std::vector<u8> rom_;
    std::unordered_map<std::string, std::optional<u32>> sectionBase_;
};

std::string LuaEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

struct Args {
    std::string elfPath;
    std::string outputPath;
    std::string romPath;
};

Args ParseArgs(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--elf" || arg == "-e") a.elfPath = next();
        else if (arg == "--output" || arg == "-o") a.outputPath = next();
        else if (arg == "--rom" || arg == "-r") a.romPath = next();
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (a.elfPath.empty() || a.outputPath.empty())
        throw std::runtime_error("usage: lua_log_builder --elf <demo.nes.elf> --output <logdata.lua> [--rom <demo.nes>]");
    if (a.romPath.empty()) {
        // This project's own convention: the debug ELF is named "<rom>.elf".
        const std::string suffix = ".elf";
        if (a.elfPath.size() > suffix.size() &&
            a.elfPath.compare(a.elfPath.size() - suffix.size(), suffix.size(), suffix) == 0) {
            a.romPath = a.elfPath.substr(0, a.elfPath.size() - suffix.size());
        } else {
            throw std::runtime_error("--elf does not end in .elf; pass --rom explicitly");
        }
    }
    return a;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Args args = ParseArgs(argc, argv);
        std::cerr << "lua_log_builder: reading " << args.elfPath << "\n";

        const Elf elf = Elf::Load(args.elfPath);
        std::cerr << "lua_log_builder: parsed ELF -- " << elf.sections.size() << " section(s), "
                  << elf.symbols.size() << " symbol(s)\n";

        const std::vector<LogEntry> entries = ReadPnesLog(elf);
        if (entries.empty()) {
            std::cerr << "lua_log_builder: no log()/pause() call sites found (.pnes_log is empty or absent)\n";
        } else {
            std::cerr << "lua_log_builder: found " << entries.size() << " log()/pause() call site(s) in .pnes_log:\n";
            for (auto &e : entries) {
                std::cerr << "  " << e.file << ":" << e.line << " "
                          << (e.kind == PNES_LOG_KIND_PAUSE ? "[pause]" : "\"" + e.message + "\"");
                if (!e.args.empty()) {
                    std::cerr << " (" << e.args.size() << " arg(s):";
                    for (auto &a : e.args)
                        std::cerr << " [addr=0x" << std::hex << a.address << std::dec
                                  << " size=" << int(a.size) << (a.is_signed ? " signed]" : " unsigned]");
                    std::cerr << ")";
                }
                std::cerr << "\n";
            }
        }

        const std::vector<LineTable> lineTables = ParseDebugLine(elf);
        if (lineTables.empty() && !entries.empty()) {
            std::cerr << "lua_log_builder: warning: no .debug_line data in " << args.elfPath
                      << " -- was it built with -g? Every log() call site will be unresolved.\n";
        } else if (!lineTables.empty()) {
            std::cerr << "lua_log_builder: parsed " << lineTables.size() << " DWARF line table(s)\n";
        }

        std::cerr << "lua_log_builder: reading " << args.romPath << "\n";
        RomLocator locator(ReadFile(args.romPath));

        std::ofstream out(args.outputPath, std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + args.outputPath);
        out << "-- Generated by lua_log_builder. Do not edit; regenerated on every NES build.\n";
        out << "return {\n";

        int resolved = 0, unresolved = 0;
        for (auto &e : entries) {
            auto address = ResolveAddress(lineTables, e.file, e.line);
            if (!address) {
                std::cerr << "lua_log_builder: warning: could not resolve " << e.file << ":" << e.line
                          << " (\"" << e.message << "\") to any address -- skipped\n";
                ++unresolved;
                continue;
            }
            const Section *sec = elf.FindContaining(*address);
            if (!sec) {
                std::cerr << "lua_log_builder: warning: " << e.file << ":" << e.line
                          << " resolved to address 0x" << std::hex << *address << std::dec
                          << ", which is in no known ROM section -- skipped\n";
                ++unresolved;
                continue;
            }
            auto lma = locator.Locate(elf, *sec, *address);
            if (!lma) { ++unresolved; continue; }

            std::cerr << "lua_log_builder: " << e.file << ":" << e.line << " -> address 0x" << std::hex
                      << *address << std::dec << " (" << sec->name << ") -> lma " << *lma << "\n";
            out << "  { lma = " << *lma
                << ", kind = \"" << (e.kind == PNES_LOG_KIND_PAUSE ? "pause" : "log") << "\""
                << ", file = \"" << LuaEscape(e.file) << "\""
                << ", line = " << e.line
                << ", message = \"" << LuaEscape(e.message) << "\"";
            if (!e.args.empty()) {
                out << ", args = {";
                for (auto &a : e.args) {
                    out << " { address = " << a.address << ", size = " << int(a.size)
                        << ", signed = " << (a.is_signed ? "true" : "false") << " },";
                }
                out << " }";
            }
            out << " },\n";
            ++resolved;
        }

        out << "}\n";
        std::cerr << "lua_log_builder: wrote " << args.outputPath << " -- "
                  << resolved << " log point(s) resolved, " << unresolved << " skipped\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "lua_log_builder: error: " << e.what() << "\n";
        return 1;
    }
}
