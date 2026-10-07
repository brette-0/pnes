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

/**
 * @brief Pins a variable into MMC3's PRG-RAM ("cartridge memory",
 *        $6000-$7FFF).
 *
 * Explicit placement, the same way ::FIXED pins code into $E000-$FFFF: use
 * for state that should live in cartridge RAM specifically (e.g. a battery-
 * backed save struct) rather than wherever ordinary .bss/.data happens to
 * land. Backed by `.cartmem`, NOLOAD (mmc3-helper.ld) -- runtime storage
 * only, nothing is loaded into it from the ROM file. Expands to nothing
 * off-NES.
 */
#define CARTMEM CREATE_SEGMENT_KEYWORD(".cartmem")

/**
 * @brief Pins a variable into the NES's own onboard system RAM, explicitly.
 *
 * Ordinary globals already land in system RAM via .bss/.data's default
 * placement; ::SYSMEM exists for state that specifically must NOT drift
 * into ::CARTMEM or any other region by accident -- an explicit statement
 * of intent, not a different physical destination than the untagged
 * default. Backed by `.sysmem`, NOLOAD (mmc3-helper.ld). Expands to nothing
 * off-NES.
 */
#define SYSMEM CREATE_SEGMENT_KEYWORD(".sysmem")

// On TARGET_NES, TITLE and TITLE_DATA are NOT defined here -- CMakeLists.txt
// injects them as whole compile definitions (from local.cmake's TITLE) onto
// the `demo` target instead, so they reach every demo .cpp/generated header
// without needing this file #included first in the right order, and so a
// developer's local.cmake choice of bank actually takes effect (a #define
// here would silently win over that -D once this header's included, same
// footgun the CallInLevelGraphics fallback below avoids with its own
// TARGET_NES guard). TITLE_DATA is TITLE's own value with ".rodata" appended
// there, same "distinct section NAME, same physical bank as ::COLD/::TITLE"
// reasoning as before (clang/LLD reject code and data sharing one section's
// literal name).
//
// Off NES, nothing generates those -D flags at all (CMakeLists.txt's
// DEMO_PLACEMENT_DEFINES only exists in the nes/nes_pal branch) -- but
// shared code (demo/src/graphics/{colours,strings}.hpp, uitk-generated
// title.cpp, ...) still tags data with TITLE/TITLE_DATA unconditionally, the
// same way it uses ::COLD or ::ACTORS. Those compile everywhere because
// CREATE_SEGMENT_KEYWORD itself is a no-op off NES (technology.hpp) --
// TITLE/TITLE_DATA just need *a* definition to reach that no-op, which
// nothing else was ever providing. The literal section name passed here is
// irrelevant off NES (CREATE_SEGMENT_KEYWORD discards it), so there's no
// bank/placement decision being made -- just filling in a macro every other
// platform never had.
#ifndef TARGET_NES
#define TITLE CREATE_SEGMENT_KEYWORD("")
#define TITLE_DATA CREATE_SEGMENT_KEYWORD("")
#endif

#define LEVEL_CODE CREATE_SEGMENT_KEYWORD(".prg_rom_level_code")

#define LEVEL_DATA CREATE_SEGMENT_KEYWORD(".prg_rom_level_data")

#define LEVEL_DYNAMIC CREATE_SEGMENT_KEYWORD(".prg_rom_level_dynamic")

#define LEVEL_GRAPHICS CREATE_SEGMENT_KEYWORD(".prg_rom_level_graphics")

// OPTIONS/OPTIONS_DATA are NOT defined here -- demo/gen/options/*/options.hpp
// (uitk-generated, from demo/ui/options.uis) is a standalone header that
// never includes banks.hpp, so CMakeLists.txt feeds them in as whole compile
// definitions instead, same mechanism as TITLE/TITLE_DATA/SYSMEM (see its own
// comment, "TITLE / SYSMEM: placement for uitk-generated UI code"). Unlike
// TITLE, there is no per-developer bank choice to make for OPTIONS: it's
// hardcoded in CMakeLists.txt to demo/link.ld's own dedicated
// prg_rom_options bank, not read from local.cmake.

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
