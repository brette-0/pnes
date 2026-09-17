#pragma once
#include <platform-nes/platform-nes.hpp>
#include <platform-nes/mappers/mmc3.hpp>
#include "modes/level.hpp"

struct audio_tag      {};
struct audio_data_tag {};
struct level_code_tag {};
struct cold_tag  {};
struct title_tag {};
struct actor_tag {};

inline u8 LevelDataBank(const u16 levelIndex) {
    (void)levelIndex;
    return 1;
}

inline u8 LevelDynamicBank(const u16 levelIndex) {
    (void)levelIndex;
    return 6;
}

inline u8 LevelGraphicsBank(const u16 levelIndex) {
    (void)levelIndex;
    return 7;
}

#ifdef TARGET_NES
template <> struct mmc3::bank_layout<level_code_tag> {
    static constexpr mmc3::section_t section() { return { 0x00018000u, 0x2000u }; }
};

template <> struct mmc3::bank_layout<audio_tag> {
    static constexpr mmc3::section_t section() { return { 0x00038000u, 0x2000u }; }
};

template <> struct mmc3::bank_layout<audio_data_tag> {
    static constexpr mmc3::section_t section() { return { 0x0004a000u, 0x2000u }; }
};

template <> struct mmc3::bank_layout<cold_tag> {
    static constexpr mmc3::section_t section() { return { 0x00058000u, 0x2000u }; }
};

template <> struct mmc3::bank_layout<title_tag> {
    static constexpr mmc3::section_t section() { return { 0x00058000u, 0x2000u }; }
};

template <> struct mmc3::bank_layout<actor_tag> {
    static constexpr mmc3::section_t section() { return { 0x00068000u, 0x2000u }; }
};

#endif

#define COLD CREATE_SEGMENT_KEYWORD(".prg_rom_cold")

// TITLE, TITLE_DATA and SYSMEM are NOT defined here -- CMakeLists.txt injects
// them as whole compile definitions (from local.cmake's TITLE/SYSMEM) onto
// the `demo` target, so they reach every demo .cpp/generated header without
// needing this file #included first in the right order. TITLE_DATA is
// TITLE's own value with ".rodata" appended there, same "distinct section
// NAME, same physical bank as ::COLD/::TITLE" reasoning as before (clang/LLD
// reject code and data sharing one section's literal name).

#define LEVEL_CODE CREATE_SEGMENT_KEYWORD(".prg_rom_level_code")

#define LEVEL_DATA CREATE_SEGMENT_KEYWORD(".prg_rom_level_data")

#define LEVEL_DYNAMIC CREATE_SEGMENT_KEYWORD(".prg_rom_level_dynamic")

#define LEVEL_GRAPHICS CREATE_SEGMENT_KEYWORD(".prg_rom_level_graphics")

// ONE shared switch/restore body for window 2 (R7), defined once in
// banks.cpp -- ordinary (non-template) function, so it exists exactly once
// in the final binary no matter how many call sites use it. This replaces
// mmc3::CallInBlock<Tag>, whose CallInWindow/CallInSection are templated on
// the caller's own lambda type: every distinct call site there instantiated
// its own out-of-line trampoline (and BOTH the R6 and R7 variants, since
// CallInSection's window dispatch is a runtime switch), which is what blew
// prg_rom_fixed's budget the first time this was tried.
#ifdef TARGET_NES
FIXED void CallInLevelGraphics(void (*fn)(void*), void* ctx);
#else
inline void CallInLevelGraphics(void (*fn)(void*), void* ctx) { fn(ctx); }
#endif

// Thin per-callsite shim: F is passed through ctx by pointer, so only this
// small invoke-and-unpack wrapper is instantiated per lambda type -- none of
// the actual register save/switch/restore logic, which lives once in
// ::CallInLevelGraphics above.
template <typename F>
auto CallLevelGraphics(F&& f) -> decltype(f()) {
    using Ret = decltype(f());
    if constexpr (__is_same(Ret, void)) {
        CallInLevelGraphics([](void* ctx) { (*static_cast<F*>(ctx))(); }, &f);
    } else {
        struct Result { F* fn; Ret value; } result{ &f, Ret{} };
        CallInLevelGraphics([](void* ctx) {
            auto* r = static_cast<Result*>(ctx);
            r->value = (*r->fn)();
        }, &result);
        return result.value;
    }
}

template <typename Block>
decltype(auto) InColdBank(Block &&block) {
    return mmc3::CallInBlock<cold_tag>(static_cast<Block &&>(block));
}

#define ACTORS CREATE_SEGMENT_KEYWORD(".prg_rom_actors")

MMC3_BIND(level::UpdateActors, actor);
