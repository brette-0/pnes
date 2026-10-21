/**
 * @file vrc1.hpp
 * @brief VRC1 mapper: PRG/CHR bank switching and the ::FIXED segment keyword.
 *
 * VRC1 exposes three switchable 8 KiB PRG-ROM windows (::VRC1::window1Control,
 * ::VRC1::window2Control, ::VRC1::window3Control) and two independent 4 KiB
 * CHR-ROM windows (::VRC1::chr0Control, ::VRC1::chr1Control, with the shared
 * high bit in ::VRC1::chrHighBits). ::VRC1::Long and ::VRC1::SwitchCHRBank
 * are the safe entry points for switching either; ::FIXED pins code that
 * must survive a bankswitch into the mapper's always-mapped $E000-$FFFF
 * window.
 *
 * A class rather than a namespace, matching ::mmc3: all members static, no
 * constructor, since there is one physical chip and no object to build. See
 * mmc3.hpp's file comment for the fuller rationale.
 */
#pragma once

#include <intsh>
#include <utility>

#include <pnes/technology.hpp>
#ifndef TARGET_NES
#include <pnes/video.hpp>
#endif

using namespace br0::intsh;

/*
 *  no long reads or long writes (not clean in c++) instead we favor longcalls and require get methods to be created
 *  I don't expect there to be much overhead to this, it will suggest encapsulation to the user
 */

/**
 * @brief Pins a function or variable into VRC1's fixed PRG-ROM bank ($E000-$FFFF).
 *
 * Backed by `.prg_rom_fixed`, the only segment vrc1.ld currently wires to a
 * real, always-mapped region -- see the file comment at the top of vrc1.ld.
 * Expands to nothing off-NES.
 */
#define FIXED CREATE_SEGMENT_KEYWORD(".prg_rom_fixed")

/**
 * @brief VRC1 mapper scoping class: PRG/CHR bank registers and the
 *        banked-call scaffolding. See this file's own header comment.
 *
 * Every member is `static`; the class itself is non-instantiable (all
 * constructors deleted) since the NES target never needs -- and must never
 * create -- an object of it.
 */
class VRC1 {
public:
    VRC1() = delete;
    VRC1(const VRC1 &) = delete;
    VRC1 &operator=(const VRC1 &) = delete;

    /**
     * @brief VRC1's three switchable 8 KiB PRG-select registers ($8000/$A000/$C000).
     *
     * Not `const`: a "write-only register" that can never be written defeats the
     * point. Defined (not just declared) in vrc1.cpp, where the default-bank
     * boot-time init also lives.
     */
    static tech::wo_register<0x8000> window1Control; ///< PRG-select, window 1 ($8000-$9FFF). See above.
    static tech::wo_register<0xa000> window2Control; ///< PRG-select, window 2 ($A000-$BFFF). See ::window1Control.
    static tech::wo_register<0xc000> window3Control; ///< PRG-select, window 3 ($C000-$DFFF). See ::window1Control.

    /**
     * @brief Writes @p ctx into a VRC1 PRG-select register, banking in that window.
     *
     * Thin, named wrapper over ::tech::wo_register's write path (updates the RAM
     * shadow and pokes hardware together, so the two can never drift). @p reg is
     * taken by reference -- by value, the write would land on a throwaway copy:
     * the hardware poke would still happen (the address is a template constant,
     * not tied to which copy calls it), but the shadow update would be lost,
     * leaving the original register's tracked state stale.
     *
     * Not used by ::_reset (vrc1.cpp): that runs before .bss has been zeroed,
     * so window1Control/2/3's shadows aren't valid to write yet regardless of
     * how this compiles -- see ::_reset's own comment for why it pokes hardware
     * directly instead.
     */
    template <u16 addr>
    static constexpr void SwitchBank(tech::wo_register<addr> &reg, u8 ctx) {
#ifdef TARGET_NES
        reg = ctx;
#else
        // Off-NES there is no PRG-ROM to bank (::Long already collapses to a
        // plain call there -- see its own comment), so a direct SwitchBank call
        // on window1Control/2/3 has nothing to do. Unlike ::SwitchCHRBank, this
        // has no bearing on the emu PPU's tile fetch, so there's no shadow state
        // to update either -- just don't poke real hardware.
        (void)reg;
        (void)ctx;
#endif
    }

    /**
     * @brief Calls @p fn with one of VRC1's three switchable windows temporarily
     *        re-banked, then restores whatever was banked there before returning.
     *
     * @p window is 0, 1 or 2 -- REQUIRED, no default. Which window holds the
     * target code changes which physical bank ends up mapped, so a caller
     * that gets this wrong silently jumps into the wrong bank's content
     * rather than failing to compile; a default here would let that mistake
     * happen by omission instead of by an explicit (and reviewable) choice.
     * Lives in the fixed bank so it is reachable regardless of what is
     * banked elsewhere. @p fn may be any callable whose `fn()` converts to
     * @p TReturn.
     *
     * @note Writes the window's boot-time default bank, so with one bank per
     *       window this reasserts what is already mapped. Multi-bank content
     *       per window will need an explicit target-bank parameter here.
     *
     * @tparam TReturn Result type of the call; not deducible from @p fn, so
     *                 specify it explicitly at the call site, e.g. `Long<int>(...)`.
     * @param fn     Callable to invoke once the selected window is banked in.
     * @param window Which window to switch: 0, 1, or 2; ignored off-NES.
     *
     * @note Off-NES the window registers do not exist, so this collapses to a
     *       plain `return fn();` rather than binding an undefined extern.
     */
    template <typename TReturn, typename TFunc>
    static FIXED TReturn Long(TFunc fn, u8 window);

    /*
     * CHR bankswitching. VRC1 has two independent 4 KiB CHR windows -- PPU
     * $0000-$0FFF ("pattern table 0") and $1000-$1FFF ("pattern table 1"),
     * matching this project's own terminology in video.hpp (::PatternTables,
     * ::CHR_TILES_PER_TABLE) -- each showing one of up to 32 physical 4 KiB
     * banks (128 KiB CHR-ROM max). Each bank number is 5 bits: 4 low bits
     * written to that window's own select register ($E000 / $F000), plus 1
     * high bit packed into a register the two windows *share* ($9000: bit 0
     * for pattern table 0, bit 1 for pattern table 1) -- so setting one
     * window's high bit is a read-modify-write against the other window's
     * current bit, not a plain overwrite.
     */

    /**
     * @brief VRC1's CHR-select registers ($9000/$E000/$F000).
     *
     * chrHighBits ($9000) is shared between both pattern tables (bit 0 = table
     * 0's bank bit 4, bit 1 = table 1's), which is exactly what ::tech::wo_register's
     * shadow exists for: ::SwitchCHRBank reads it back to flip only the calling
     * window's own bit, never the other window's. chr0Control ($E000) and
     * chr1Control ($F000) hold each table's low 4 bank bits. Not `const`,
     * same reasoning as window1Control etc.
     */
    static tech::wo_register<0x9000> chrHighBits; ///< Shared high bank bit for both pattern tables. See above.
    static tech::wo_register<0xe000> chr0Control; ///< CHR-select, pattern table 0 ($0000-$0FFF). See ::chrHighBits.
    static tech::wo_register<0xf000> chr1Control; ///< CHR-select, pattern table 1 ($1000-$1FFF). See ::chrHighBits.

#ifndef TARGET_NES
    /**
     * @brief Emu-side CHR bank storage: index 0 = chr0Control (pattern table 0,
     *        PPU $0000-$0FFF), index 1 = chr1Control (pattern table 1, PPU
     *        $1000-$1FFF). Each holds the full 5-bit bank number directly --
     *        off-NES there's no shared chrHighBits port to split it across, so
     *        ::SwitchCHRBank just stores the whole value here. Consulted by
     *        ::GetTileLMA to resolve a tile fetch. Defined in the emu-side
     *        vrc1.cpp (src/emu/mappers/vrc1.cpp), not the NES-side one.
     */
    static u8 chrBanks[2];

    /**
     * @brief Translates a tile's PPU pattern-table address (0x0000-0x1FFF) into
     *        a byte offset in the flat CHR-ROM image, resolving it through
     *        ::chrBanks.
     *
     * Library-internal: the only caller is this module's own strong
     * `ppu::ResolveTile` override (src/emu/mappers/vrc1.cpp), which the emu
     * PPU (src/emu/ppu.cpp) calls unconditionally for every tile fetch -- see
     * that function's own doc comment (video.hpp) for the weak/strong
     * relationship. Public rather than private purely because its caller is
     * a free function, not a member of this class -- C++ access control has
     * no "same translation unit" exemption. A game selects banks through
     * ::SwitchCHRBank instead.
     */
    static u32 GetTileLMA(u16 tileVMA);
#endif

    /**
     * @brief Selects which 4 KiB CHR-ROM bank appears in a VRC1 pattern table.
     *
     * Single template, mirroring ::SwitchBank's shape: @p addr (deduced from
     * @p reg) picks between chr0Control ($E000, PPU $0000-$0FFF / pattern table
     * 0) and chr1Control ($F000, PPU $1000-$1FFF / pattern table 1).
     *
     * On NES the read-modify-write against chrHighBits' shadow is what keeps the
     * other window's high bit untouched. Off-NES it records the full bank number
     * into ::chrBanks for ::GetTileLMA to resolve.
     *
     * @param reg  chr0Control or chr1Control; any other instantiation is a
     *             compile error.
     * @param bank Target bank, 0-31.
     */
    template <u16 addr>
    static constexpr void SwitchCHRBank(tech::wo_register<addr> &reg, u8 bank) {
        static_assert(addr == 0xe000 || addr == 0xf000,
                      "SwitchCHRBank only valid for chr0Control ($E000) or chr1Control ($F000)");
#ifdef TARGET_NES
        constexpr u8 bit = (addr == 0xe000) ? 0 : 1;
        reg = bank & 0x0F;
        chrHighBits = (chrHighBits.get() & ~(u8(1) << bit)) | (((bank >> 4) & 1) << bit);
#else
        (void)reg;
        constexpr u8 idx = (addr == 0xe000) ? 0 : 1;
        chrBanks[idx] = bank & 0x1F;
        ++ppu::chrGeneration;
#endif
    }

    /**
     * @brief Size of one VRC1 CHR bank: 4 KiB, one whole pattern table.
     */
    static constexpr u32 kChrBankSize = 0x1000;

    /** @brief Highest CHR bank a 5-bit bank number can select (128 KiB). */
    static constexpr u32 kMaxChrBank = 0x1F;

    /**
     * @brief The CHR bank number that holds @p Lma -- a byte offset into the
     *        flat CHR-ROM image -- ready to hand to ::SwitchCHRBank.
     *
     * Pairs with blobman's generated header (tools/blobman): each packed
     * character set there carries an `__lma` giving where its tiles were
     * packed, so a game banks one in by offset rather than by a hand-counted
     * bank number:
     *
     *     VRC1::SwitchCHRBank(VRC1::chr1Control,
     *                         VRC1::BankOf<blob::ui::__lma>());
     *
     * @p Lma is a template parameter, not an argument, so both checks below
     * are compile-time: an offset that is not on a bank boundary cannot be
     * mapped to the start of a pattern table at all, and silently truncating
     * it would bank in the wrong tiles with no diagnostic. The 4 KiB
     * granularity makes that stricter than MMC3's -- a VRC1 character set has
     * to fill a whole pattern table's worth of alignment to be bankable.
     *
     * @tparam Lma Byte offset into CHR ROM; must be a multiple of
     *             ::kChrBankSize.
     */
    template <u32 Lma>
    static constexpr u8 BankOf() {
        static_assert(Lma % kChrBankSize == 0,
                      "BankOf: CHR offset is not on a 4 KiB bank boundary");
        static_assert(Lma / kChrBankSize <= kMaxChrBank,
                      "BankOf: CHR offset is past the 128 KiB VRC1's 5-bit bank number can reach");
        return static_cast<u8>(Lma / kChrBankSize);
    }

    /**
     * @brief Total PPU address space the CHR registers cover: 8 KiB, both
     *        pattern tables ($0000-$1FFF) -- two ::kChrBankSize windows.
     */
    static constexpr u32 kChrSpaceSize = 0x2000;

    /**
     * @brief Banks in one packed character set, writing every pattern table
     *        its PPU range touches.
     *
     * Described BY its three placement numbers, hence the name -- where the
     * characters were packed, where the PPU sees them, how far they reach --
     * and nothing else is needed. Those are exactly the three constants
     * blobman emits per packed set (tools/blobman, `__lma`/`__vma`/`__size`),
     * though any source of the same three numbers does just as well. All
     * three are template parameters, so which tables are involved, the offset
     * arithmetic and every ::BankOf call fold away entirely -- what is left
     * is the bare register writes, with literal operands:
     *
     *     VRC1::SwitchCharactersBy<blob::ui::__lma,
     *                              blob::ui::__vma,
     *                              blob::ui::__size>();
     *
     * blobman's header also emits a macro packing those three in this
     * function's own parameter order, which is how a call site usually names
     * a set -- once, rather than three times:
     *
     *     VRC1::SwitchCharactersBy<GetCharacterEntryInformation(blob::ui)>();
     *
     * VRC1's granularity makes this stricter than MMC3's equivalent: a
     * pattern table IS a bank here, so a set must start on a 4 KiB boundary,
     * and the only two shapes that bank at all are one table (@p Size up to
     * 4 KiB) or both (up to 8 KiB).
     *
     * TABLES ARE NOT SUBDIVIDED. A table the set does not fill to the end
     * keeps showing whatever follows it in CHR ROM for the remainder --
     * padding, or the next set along if one is packed there. A set relying on
     * its neighbour filling the rest of a table still wants its own assert
     * that the two are packed adjacently, since this call cannot see that
     * intent.
     *
     * @note Not quite *only* register writes, unavoidably: bank bit 4 lives
     *       in ::chrHighBits, which both tables share, so ::SwitchCHRBank
     *       must read that shadow back to leave the other table's bit alone
     *       (see ::chrHighBits). The mask and shift fold to constants; the
     *       one shadow read does not.
     *
     * @tparam Lma  Byte offset into CHR ROM the characters are packed at.
     * @tparam Vma  PPU address they are seen at; must be $0000 or $1000, a
     *              pattern-table boundary.
     * @tparam Size Their length in bytes.
     */
    template <u32 Lma, u32 Vma, u32 Size>
    static constexpr void SwitchCharactersBy() {
        static_assert(Size > 0, "SwitchCharactersBy: character set is empty");
        static_assert(Vma + Size <= kChrSpaceSize,
                      "SwitchCharactersBy: character set runs past the 8 KiB "
                      "of PPU pattern-table space the CHR registers cover");
        static_assert(Vma % kChrBankSize == 0,
                      "SwitchCharactersBy: Vma is not on a pattern-table "
                      "boundary -- VRC1 banks whole 4 KiB tables ($0000 and "
                      "$1000), and no register can point at a table's interior");

        // pattern table 0 ($0000-$0FFF): reached by anything starting there
        if constexpr (Vma < kChrBankSize) {
            SwitchCHRBank(chr0Control, BankOf<Lma>());
        }

        // pattern table 1 ($1000-$1FFF): reached either by characters based
        // there, or by a set based at $0000 that spills over the halfway
        // point. kChrBankSize - Vma is that spill distance, 0 for the based
        // case.
        if constexpr (Vma + Size > kChrBankSize) {
            SwitchCHRBank(chr1Control, BankOf<Lma + (kChrBankSize - Vma)>());
        }
    }

    /*
     * BANKED CALL scaffolding (see BANKED_CALL_THEORY.txt). Kept here rather
     * than in a separate header because bank_layout<Tag> specializations are
     * meant to sit next to the hardware facts (window1Control etc.) they
     * describe.
     */

    /**
     * @brief One physical bank/segment's location in the linked ROM image.
     *
     * rom_address is LOADADDR()-derived (physical position in the ROM file),
     * deliberately not the CPU-visible VMA: on a switchable window, every bank
     * that ever aliases onto that window shares the same VMA, so only LOADADDR()
     * can tell banks apart.
     */
    struct section_t {
        u32 rom_address;
        u32 size;
    };

    /**
     * @brief Hardware facts for one tag/domain. Primary template deliberately
     *        undefined: an un-tagged Tag is a compile error, not a silent default.
     *
     * Every specialization provides `static section_t section()` -- a function,
     * not a data member, so both shapes below share one call convention. There
     * is no `always_mapped` escape hatch; see mmc3.hpp's bank_layout.
     *
     * PREFER `static constexpr section_t section() { return {addr, size}; }` --
     * a hand-entered literal matched against a linker rule giving this domain
     * its own MEMORY region at that same ORIGIN, ideally with a link-time
     * ASSERT catching the two drifting apart. Being a real constant,
     * CallInSection's window/bank resolution folds away entirely: free.
     *
     * A non-constexpr section(), computing from a linker-PROVIDE()'d symbol at
     * runtime, is the fallback for a domain that genuinely cannot be given a
     * fixed address. It costs ~120 bytes per call site, since the compiler will
     * not fold the computation back down through the runtime indirection.
     * Reach for it only when a dedicated region is impossible.
     *
     * If you do: compute fresh in the function body. A stored,
     * dynamically-initialized member is unsafe, because unprioritized dynamic
     * initializers run after every prioritized constructor(N) on this
     * toolchain.
     */
    template <typename Tag> struct bank_layout;

    /// Which bank_layout<Tag> a specific tagged function was registered under.
    /// One specialization per VRC1_BANKED()/VRC1_BANKED_EXTERN() call site,
    /// generated by those macros -- never written by hand. Primary template
    /// deliberately undefined, same reasoning as bank_layout<Tag>.
    template <auto Fn> struct bank_of;

    /**
     * @brief A function pointer that carries its own bank -- the storable form
     *        of what ::Call<Fn> knows at compile time.
     *
     * Same purpose as mmc3::callable_t, whose doc has the full reasoning:
     * ::Call<Fn> needs Fn nameable as a template argument and
     * `table[runtime_index]` never is, so ::GetCallable resolves bank_of<Fn>
     * into a value at the one point Fn is still nameable.
     *
     * Five bytes here against MMC3's four. VRC1's windows are contiguous, so a
     * domain may span two of them, and `windows` records that. It is also the
     * one part of Detail::CallInSection that does NOT constant-fold for an
     * oversized domain, `size` being link-computed, so resolving it here saves
     * real work per call.
     *
     * Bank encoding follows CallInSection: the high half read literally, zero
     * meaning "use the window index". No bank+1 bias, unlike MMC3.
     */
    template <typename Ret, typename... Args>
    struct callable_t {
        Ret (*fn)(Args...);  ///< the target, as an ordinary 16-bit pointer
        u8 window;           ///< 0 = window1Control ($8000), 1 = window2Control, 2 = window3Control
        u8 bank;             ///< value written to the selected window's register
        u8 windows;          ///< how many adjacent windows the domain spans (1, or 2 for an oversized domain)
    };

    /// Return-type extraction for a plain function pointer, used by Call<Fn>.
    /// Also names the matching ::callable_t, so ::GetCallable can spell its own
    /// return type without the caller restating the signature.
    template <typename T> struct function_traits;

    /**
     * @brief Calls a specific, compile-time-known VRC1_BANKED() function,
     *        resolving its bank from bank_of<Fn> automatically -- no manual
     *        window/bank argument, unlike ::Long.
     *
     * @tparam Fn Address of a VRC1_BANKED()/VRC1_BANKED_EXTERN()-tagged
     *            function -- supplied explicitly, e.g. `Call<LoadLevelChunk>(chunkId)`.
     */
    template <auto Fn, typename... Args>
    static FIXED auto Call(Args &&...args) -> typename function_traits<decltype(Fn)>::return_type;

    /**
     * @brief Runs an arbitrary block under Fn's resolved window, instead of
     *        calling Fn itself -- for generic code that takes a runtime
     *        callback parameter (see BANKED_CALL_THEORY.txt's
     *        PopulateFromProvider example). Fn only anchors which bank_of<>
     *        entry to resolve; the block may capture freely.
     */
    template <auto Fn, typename Block>
    static FIXED auto CallBlock(Block &&block) -> decltype(block());

    /**
     * @brief Resolves a tagged function into a storable, self-describing
     *        ::callable_t. The compile-time half of runtime dispatch.
     *
     * Call it wherever the function is still nameable as a literal, then store
     * or pass the result freely -- it needs no tag, no template argument, and
     * no bank_of<> lookup ever again:
     *
     *     VRC1_BANKED(".prg_rom_enemies_a", enemiesA, void, UpdateWalker, Entity*);
     *     VRC1_BANKED(".prg_rom_enemies_b", enemiesB, void, UpdateFlyer,   Entity*);
     *
     *     // two DIFFERENT domains -- no single bank_of<> is right for both.
     *     // Spell the element type: `constexpr auto table[]` cannot deduce
     *     // from a braced list.
     *     constexpr VRC1::callable_t<void, Entity*> table[] = {
     *         VRC1::GetCallable<UpdateWalker>(), VRC1::GetCallable<UpdateFlyer>() };
     *
     *     VRC1::Call(table[entity.type], &entity);   // runtime index, correct bank
     *
     * `constexpr` so such a table lands in ROM fully resolved. That needs the
     * tag's bank_layout<Tag>::section() to be constexpr, the preferred shape
     * anyway; a runtime section() still works but initializes dynamically.
     */
    template <auto Fn>
    static constexpr auto GetCallable() ->
        typename function_traits<decltype(Fn)>::callable_type;

    /**
     * @brief Calls a ::callable_t, banking in whatever it says it needs.
     *
     * The runtime counterpart to ::Call<Fn>: an ordinary overload rather than a
     * template on Fn, taking the already-resolved value ::GetCallable produced.
     * Same switch-run-restore as every other farcall here, minus the address
     * arithmetic and the size division, which ::GetCallable already did.
     */
    template <typename Ret, typename... Args, typename... Actual>
    static FIXED Ret Call(const callable_t<Ret, Args...> &c, Actual &&...args);

private:
    /**
     * @brief Implementation details not part of VRC1's public interface.
     *        Private nested class purely as a namespacing device -- a class
     *        cannot nest a plain `namespace`, so this plays the same role
     *        the free-standing `namespace vrc1_detail` played before.
     */
    struct Detail {
        /// The three switchable windows' shared geometry: each is WINDOW_SIZE
        /// bytes, starting at WINDOW_BASE ($8000, window1Control's own base) and
        /// running back to back through window3Control -- see the MEMORY block
        /// comment in vrc1.ld. rom_address is an absolute LOADADDR()-derived
        /// address, so it must be rebased against WINDOW_BASE before dividing;
        /// dividing the raw absolute address would be off by WINDOW_BASE/WINDOW_SIZE.
        static constexpr u16 kWindowBase = 0x8000;
        static constexpr u16 kWindowSize = 0x2000;

        /**
         * @brief Implementation of ::Long once the target register is resolved.
         *
         * Not for direct use -- call ::Long. Saves @p reg, switches it to @p bank,
         * invokes @p fn, writes the prior value back.
         *
         * [[gnu::noinline]] IS LOAD-BEARING. A section attribute places only an
         * out-of-line symbol, and a template this small is an easy inlining
         * target -- confirmed, no separate copy existed at all. That is fatal
         * once a caller whose own code lives in the window being switched
         * inlines it: the restore half ends up embedded in the bank just
         * switched away, so the CPU resumes into unrelated bytes. A reproduced
         * crash, not a theory. noinline forces a real fixed-bank symbol, so
         * switch/call/restore never depends on what is mapped elsewhere.
         */
        template <typename TReturn, u16 addr, typename TFunc>
        [[gnu::noinline]] static FIXED TReturn CallInWindow(tech::wo_register<addr> &reg, u8 bank, TFunc fn) {
            // DELIBERATELY NOT ::SHADOW -- same measured reason as mmc3.hpp's
            // CallInWindow, which this mirrors: a shadow scope's live state
            // across fn() forces a soft-stack frame, costing more than the bank
            // switch itself. One register at a link-time constant address needs
            // no general scope, and the hand-saved byte stays on the hardware
            // stack. The restore still goes through set(), shadow and hardware.
            const u8 saved = reg.get();
            SwitchBank(reg, bank);
            if constexpr (__is_same(TReturn, void)) {
                fn();
                reg.set(saved);
            } else {
                TReturn result = fn();
                reg.set(saved);
                return result;
            }
        }

        /**
         * @brief Like CallInWindow, but for a domain spanning TWO adjacent
         *        windows: switches windowIndexBase to bankBase and the next
         *        window to bankBase+1, runs @p fn with both mapped, restores
         *        both on any exit path.
         *
         * The three windows are physically contiguous, so adjacent ones drive
         * one flat region -- but only two adjacent PAIRS exist to enumerate,
         * hence no general N-window loop.
         */
        // [[gnu::noinline]] for CallInWindow's reason, doubly so: this body
        // nests two more CallInWindow calls, all of which must stay one real
        // fixed-bank unit whatever bank the caller lives in.
        template <typename TReturn, typename TFunc>
        [[gnu::noinline]] static FIXED TReturn CallInWindows2(u8 windowIndexBase, u8 bankBase, TFunc fn) {
            switch (windowIndexBase) {
                case 0:
                    return CallInWindow<TReturn>(window1Control, bankBase, [&]() -> TReturn {
                        return CallInWindow<TReturn>(window2Control, static_cast<u8>(bankBase + 1), fn);
                    });
                case 1:
                default:
                    return CallInWindow<TReturn>(window2Control, bankBase, [&]() -> TReturn {
                        return CallInWindow<TReturn>(window3Control, static_cast<u8>(bankBase + 1), fn);
                    });
            }
        }

        /**
         * @brief Resolves a section_t (rom_address+size) to whatever register(s)
         *        writes VRC1 needs, then runs @p fn.
         *
         * WINDOW comes from the low 16 bits of rom_address -- the CPU-visible
         * VMA, exactly what an absolute JSR truncates to. BANK is separate: with
         * no high bits set, bank == window index, the one-bank-per-window
         * default; where bank_layout hand-encodes a real bank number in the high
         * bits, that wins. So un-encoded tags keep working while explicit-bank
         * tags get a distinct bank.
         *
         * WINDOW COUNT is `size` ceil-divided by kWindowSize. Unlike
         * rom_address, size is genuinely link-computed for an oversized domain,
         * so this division costs real runtime bytes -- an accepted cost of
         * dynamic sizing, not a missed optimization.
         *
         * [[gnu::noinline]] for the same load-bearing reason as CallInWindow's,
         * and this is the entry point Call<Fn>/CallBlock<Fn> invoke, so it
         * matters most.
         */
        template <typename TReturn, typename TFunc>
        [[gnu::noinline]] static FIXED TReturn CallInSection(const section_t &section, TFunc fn) {
            const u16 vma = static_cast<u16>(section.rom_address);
            const u8 windowIndex = static_cast<u8>((vma - kWindowBase) / kWindowSize);
            const u8 explicitBank = static_cast<u8>(section.rom_address >> 16);
            const u8 bank = explicitBank != 0 ? explicitBank : windowIndex;
            const u8 windowCount = static_cast<u8>((section.size + kWindowSize - 1) / kWindowSize);
            if (windowCount > 1) {
                return CallInWindows2<TReturn>(windowIndex, bank, fn);
            }
            switch (windowIndex) {
                case 1:  return CallInWindow<TReturn>(window2Control, bank, fn);
                case 2:  return CallInWindow<TReturn>(window3Control, bank, fn);
                default: return CallInWindow<TReturn>(window1Control, bank, fn);
            }
        }
    };
};

template <typename R, typename... A>
struct VRC1::function_traits<R (*)(A...)> {
    using return_type = R;
    using callable_type = VRC1::callable_t<R, A...>;
};

template <typename TReturn, typename TFunc>
FIXED TReturn VRC1::Long(TFunc fn, const u8 window) {
#ifdef TARGET_NES
    switch (window) {
        case 1:  return Detail::CallInWindow<TReturn>(window2Control, 1, fn);
        case 2:  return Detail::CallInWindow<TReturn>(window3Control, 2, fn);
        default: return Detail::CallInWindow<TReturn>(window1Control, 0, fn);
    }
#else
    return fn();
#endif
}

template <auto Fn, typename... Args>
FIXED auto VRC1::Call(Args &&...args) -> typename function_traits<decltype(Fn)>::return_type {
    using TReturn = typename function_traits<decltype(Fn)>::return_type;
#ifdef TARGET_NES
    using L = typename bank_of<Fn>::layout;
    return Detail::CallInSection<TReturn>(
        L::section(),
        [&]() -> TReturn { return Fn(std::forward<Args>(args)...); });
#else
    return Fn(std::forward<Args>(args)...);
#endif
}

template <auto Fn, typename Block>
FIXED auto VRC1::CallBlock(Block &&block) -> decltype(block()) {
#ifdef TARGET_NES
    using L = typename bank_of<Fn>::layout;
    return Detail::CallInSection<decltype(block())>(
        L::section(), std::forward<Block>(block));
#else
    return block();
#endif
}

template <auto Fn>
constexpr auto VRC1::GetCallable() ->
    typename function_traits<decltype(Fn)>::callable_type {
#ifdef TARGET_NES
    using L = typename bank_of<Fn>::layout;
    const section_t s = L::section();
    // Detail::CallInSection's derivation, done once here instead of per call:
    // same un-biased bank encoding (zero high half means the window index
    // doubles as the bank) and the same ceil-division of size.
    const u16 vma = static_cast<u16>(s.rom_address);
    const u8 window = static_cast<u8>((vma - Detail::kWindowBase) / Detail::kWindowSize);
    const u8 explicitBank = static_cast<u8>(s.rom_address >> 16);
    const u8 windows = static_cast<u8>((s.size + Detail::kWindowSize - 1) / Detail::kWindowSize);
    return { Fn, window, static_cast<u8>(explicitBank != 0 ? explicitBank : window), windows };
#else
    // Off-NES there are no banks and bank_layout<Tag> is typically not even
    // specialized, so resolving here would be a compile error. Call() below
    // ignores these fields off-NES.
    return { Fn, 0, 0, 1 };
#endif
}

template <typename Ret, typename... Args, typename... Actual>
FIXED Ret VRC1::Call(const callable_t<Ret, Args...> &c, Actual &&...args) {
#ifdef TARGET_NES
    // window1Control/2/3 are different types, so a runtime window choice has to
    // be a branch between instantiations rather than a variable -- the shape
    // Detail::CallInSection ends in, minus the arithmetic preceding it there.
    auto invoke = [&]() -> Ret { return c.fn(static_cast<Actual &&>(args)...); };
    if (c.windows > 1) return Detail::CallInWindows2<Ret>(c.window, c.bank, invoke);
    switch (c.window) {
        case 1:  return Detail::CallInWindow<Ret>(window2Control, c.bank, invoke);
        case 2:  return Detail::CallInWindow<Ret>(window3Control, c.bank, invoke);
        default: return Detail::CallInWindow<Ret>(window1Control, c.bank, invoke);
    }
#else
    return c.fn(static_cast<Actual &&>(args)...);
#endif
}

/**
 * @brief Declares a tagged, out-of-line C++ function and registers it with
 *        VRC1::bank_of<>, so VRC1::Call/VRC1::CallBlock can resolve its bank
 *        later.
 *
 * section_name is whatever literal the caller's linker script uses; this
 * library enforces no naming convention. tag is a separate identifier-safe
 * token keying bank_of<>/bank_layout<Tag>, kept apart because real section
 * names are not valid identifiers. The _IMPL indirection is load-bearing: it
 * expands section_name/tag to final text before # or ## sees them. Named
 * VRC1_BANKED, not BANKED, so both mapper headers can be included together.
 */
#define VRC1_BANKED(section_name, tag, ret, name, ...) \
    VRC1_BANKED_IMPL(section_name, tag, ret, name, __VA_ARGS__)
#define VRC1_BANKED_IMPL(section_name, tag, ret, name, ...)                      \
    CREATE_SEGMENT_KEYWORD(section_name) ret name(__VA_ARGS__);                 \
    template <> struct VRC1::bank_of<static_cast<ret (*)(__VA_ARGS__)>(&name)> { \
        using layout = VRC1::bank_layout<tag##_tag>;                            \
    }

/**
 * @brief Registers a function with VRC1::bank_of<> without declaring/placing it.
 *
 * For functions this codegen never emits -- hand-written assembly, or an
 * engine assembled separately. Registration only: placement belongs to whatever
 * assembled the symbol, and must be kept in sync with section_name by hand.
 *
 * Carries its own `extern "C"` because every real user exposes an unmangled
 * symbol, and because the bank_of<> specialization below it is a template,
 * which can never sit inside an `extern "C"` block.
 */
#define VRC1_BANKED_EXTERN(section_name, tag, ret, name, ...) \
    VRC1_BANKED_EXTERN_IMPL(section_name, tag, ret, name, __VA_ARGS__)
#define VRC1_BANKED_EXTERN_IMPL(section_name, tag, ret, name, ...)               \
    extern "C" ret name(__VA_ARGS__);                                           \
    template <> struct VRC1::bank_of<static_cast<ret (*)(__VA_ARGS__)>(&name)> { \
        using layout = VRC1::bank_layout<tag##_tag>;                            \
    }
