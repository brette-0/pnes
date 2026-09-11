/**
 * @file mmc3.hpp
 * @brief MMC3 mapper (mapper 4): PRG/CHR bank switching, scanline IRQ, and
 *        the ::FIXED / ::CARTMEM / ::SYSMEM segment keywords.
 *
 * MMC3 hardware, as this module uses it (PRG mode 0, CHR mode 0 -- the only
 * combination this module supports; see mmc3.cpp's ::_reset):
 *
 *   $8000-$9FFF  8 KiB PRG-ROM, switchable (register R6, via $8000/$8001)
 *   $A000-$BFFF  8 KiB PRG-ROM, switchable (register R7, via $8000/$8001)
 *   $C000-$DFFF  8 KiB PRG-ROM, FIXED to the second-to-last physical bank
 *   $E000-$FFFF  8 KiB PRG-ROM, FIXED to the last physical bank
 *   $6000-$7FFF  up to 8 KiB PRG-RAM ("cartridge memory", see ::CARTMEM)
 *   $0000-$07FF  CHR-ROM, six windows: two 2 KiB (R0/R1), four 1 KiB (R2-R5)
 *
 * Only TWO registers can be pointed anywhere: R6 ($8000) and R7 ($A000).
 * $C000-$DFFF is hardware-fixed to the second-to-last bank regardless of any
 * write, so there is no window3Control -- no register backs one. Its content
 * also shifts as the ROM grows, so unlike ::FIXED nothing should target it
 * through the farcall machinery below.
 *
 * Every register except $A000 (mirroring) and $A001 (PRG-RAM protect) is
 * reached through the same two addresses -- $8000 select, $8001 data. See
 * ::mmc3::Register, the indexed analogue of VRC1's ::wo_register.
 *
 * A class rather than a namespace, with every member static and no
 * constructor: there is one physical chip, so no object to build. The class
 * leaves room for a future emulated MMC3 to hold per-instance state without
 * colliding with this state-free version.
 */
#pragma once

#include <intsh>
#include <utility>

#include <platform-nes/technology.hpp>
#include <platform-nes/types.hpp>

using namespace br0::intsh;

// Alternative nametable wiring (NES2.0 flags 6, bit 3 -- the "four-screen"
// bit; see headers/nes2.hpp's own comment on the same macro, which defines
// this default under TARGET_NES from the CMake -D flag). This file is also
// compiled off-NES, by every emu backend, where CMake may not always pass
// the define through -- so it needs its own fallback default here too, in
// the shared header rather than just mmc3.cpp, since every TU that sees
// ::mmc3's class layout (below) must agree on which members exist. Only
// value 1 has a real implementation (src/emu/mappers/mmc3.cpp's strong
// ::ppu::ReadNametable/::WriteNametable overrides) -- see the static_assert.
#ifndef ALTERNATIVE_NAMETABLE
#define ALTERNATIVE_NAMETABLE 0
#endif

static_assert(ALTERNATIVE_NAMETABLE == 0 || ALTERNATIVE_NAMETABLE == 1,
    "mmc3: ALTERNATIVE_NAMETABLE must be 0 (ordinary switchable 2-screen "
    "mirroring) or 1 (four-screen: +2KiB cartridge VRAM) -- other nonzero "
    "values are reserved for a genuinely different alternative-wiring "
    "scheme, not folded into 1's meaning, and none is implemented yet.");

/**
 * @brief Pins a function or variable into MMC3's fixed PRG-ROM bank
 *        ($E000-$FFFF).
 *
 * Same name and section as VRC1's ::FIXED (mmc3-helper.ld routes
 * `.prg_rom_fixed` to the identical $E000-$FFFF address range VRC1 does),
 * since both chips hardwire that window the same way. Expands to nothing
 * off-NES.
 */
#define FIXED CREATE_SEGMENT_KEYWORD(".prg_rom_fixed")

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

/**
 * @brief MMC3 mapper (mapper 4) scoping class: PRG/CHR bank registers,
 *        scanline IRQ control, mirroring/PRG-RAM registers, and the
 *        banked-call scaffolding. See this file's own header comment.
 *
 * Every member is `static`; the class itself is non-instantiable (all
 * constructors deleted) since the NES target never needs -- and must never
 * create -- an object of it.
 */
class mmc3 {
private:
#ifndef TARGET_NES
    /**
     * @brief Emu-side shadow of MMC3's CHR mode bit (bit 7 of $8000).
     *
     * The real NES side of this module never sets this bit -- ::Register's
     * hardware write path hardcodes ::Detail::kModeBits to 0 permanently (see
     * this file's header comment: "the only combination this module
     * supports"), so on an actual cartridge this is always mode 0. The emu
     * side tracks it as real, settable state anyway (via ::SetCHRMode) so a
     * desktop/console build can model a game that DOES flip it, without
     * requiring the NES-side restriction to be lifted first.
     *
     * true  = large (2 KiB, R0/R1) windows at the front ($0000-$0FFF),
     *         small (1 KiB, R2-R5) windows at the end ($1000-$1FFF) --
     *         hardware's mode 0, and this module's only NES-side mode.
     * false = inverted: small windows at the front, large windows at the end.
     */
    static bool shape;

    /// Current bank number for each CHR register (R0-R5), indexed by
    /// register index -- banks[0]/banks[1] are 2 KiB-granularity (R0/R1),
    /// banks[2]-banks[5] are 1 KiB-granularity (R2-R5). Updated by
    /// ::Register::poke_only whenever chr0Control-chr5Control are written,
    /// off-NES only (see ::Register's own comment). Six entries, one per CHR
    /// register -- window1Control/window2Control (R6/R7) don't participate:
    /// off-NES there's no bank-switched PRG concept (::Long/::Call/::CallInBlock
    /// already degrade to plain calls there), so nothing ever reads a PRG
    /// entry back out.
    static u8 banks[6];

    /// Off-NES write path for ::Register<Index>::poke_only, Index 0-5 only
    /// (see ::Register's own comment) -- records the bank a CHR register was
    /// just written with into ::banks, with no hardware poke (there is no
    /// hardware here). Defined in the emu-side mmc3.cpp
    /// (src/emu/mappers/mmc3.cpp), not the NES-side one.
    static void NotifyCHRWrite(u8 index, u8 bank);
#endif

public:
#ifndef TARGET_NES
    /**
     * @brief Translates a tile's PPU pattern-table address (0x0000-0x1FFF)
     *        into a byte offset in the flat CHR-ROM image, resolving it
     *        through the currently-selected CHR banks and ::shape.
     *
     * An ADDRESS translator, not a fetch: returns an offset for the caller to
     * index CHR-ROM with. Library-internal -- the only caller is this module's
     * strong `ppu::ResolveTile` override. Public only because that caller is a
     * free function, not a member. A game selects banks through
     * ::SwitchCHRBank and never calls this.
     */
    static u32 GetTileLMA(u16 tileVMA);

#if ALTERNATIVE_NAMETABLE == 1
    /**
     * @brief Backing storage for this module's strong ::ppu::ReadNametable/
     *        ::ppu::WriteNametable overrides (src/emu/mappers/mmc3.cpp) --
     *        the software stand-in for the extra 2 KiB VRAM chip a real
     *        four-screen MMC3 board carries. Sized and allocated by the
     *        strong ::ppu::InitCartVRAM override, once, from
     *        ::emu::InitMemory.
     *
     * Only declared under ALTERNATIVE_NAMETABLE == 1: a board without this
     * wiring never routes anywhere but the console's own ::VideoRAM (the
     * weak ::ppu::ReadNametable/::WriteNametable defaults already do that),
     * so it has nothing to back. Public for the same free-function reason as
     * ::GetTileLMA, above -- not part of this class's intended user-facing
     * API.
     */
    static u8* cartVRAM;

    /// Byte length of one nametable row (::video::nametable_row_bytes(),
    /// NOT ::VideoRAM's own allocated size -- see ::ppu::InitCartVRAM's own
    /// doc comment, video.hpp, for why those can differ): everything below
    /// this logical offset answers from ::VideoRAM, everything at or above
    /// it answers from ::cartVRAM (offset by this same amount). Set
    /// alongside ::cartVRAM by ::ppu::InitCartVRAM.
    static unsigned cartVRAMRowBytes;
#endif

    /**
     * @brief Sets the emu-side CHR mode bit (see ::shape).
     *
     * Off-NES only: the real NES side of this module never exposes a way to
     * change this (it hardcodes mode 0, see ::Detail::kModeBits), so this
     * exists purely for a desktop/console build that wants to model a game
     * which does flip it. Calling this on the NES target would have no
     * hardware counterpart to reflect the change, so it isn't declared
     * there.
     *
     * @param largeWindowsAtFront true selects hardware's mode 0 (this
     *                            module's own default and its NES side's
     *                            only mode); false inverts it.
     */
    static void SetCHRMode(bool largeWindowsAtFront);
#endif

    mmc3() = delete;
    mmc3(const mmc3 &) = delete;
    mmc3 &operator=(const mmc3 &) = delete;

    /**
     * @brief One of MMC3's eight indexed bank registers (R0-R7), reached
     *        through the shared $8000 (select) / $8001 (data) port pair.
     *
     * Unlike ::tech::wo_register (one dedicated hardware address per
     * instance), every Register<Index> shares the SAME two physical
     * addresses -- what makes an instance distinct is which index gets
     * latched into $8000 immediately before the $8001 write. Real hardware
     * has eight independent internal latches behind that one port pair, so
     * each instance still owns its own RAM shadow, and get()/::SHADOW/
     * save-restore all work exactly like ::tech::wo_register's.
     *
     * @tparam Index Register select value, 0-7 (R0-R7).
     */
    template <u8 Index>
    class Register {
        atomic u8 shadow_;
    public:
        static_assert(Index <= 7, "MMC3 only has registers R0-R7.");

        Register() = default;                                    ///< trivial: instance lives in .bss
        Register(const Register &o) : shadow_(o.shadow_) {}     ///< snapshot, no poke

        u8   get() const     { return shadow_; }                          ///< read  = shadow
        void set(const u8 v) { shadow_ = v; poke_only(v); }               ///< write = shadow + hardware
        /**
         * @brief Write hardware only, no shadow update.
         *
         * On NES: the real $8000/$8001 port-pair write. Off-NES: there is no
         * hardware, so this instead updates the emu's own bank-tracking state
         * (::NotifyCHRWrite) for CHR registers (Index 0-5) so ::GetTileLMA can
         * resolve tile fetches correctly; PRG registers (Index 6/7) are a
         * no-op off-NES, since off-NES there's no bank-switched PRG concept
         * to track (::Long/::Call/::CallInBlock already degrade to plain calls
         * there).
         */
        void poke_only(const u8 v) const {
#ifdef TARGET_NES
            tech::poke(0x8000, Detail::kModeBits | Index);
            tech::poke(0x8001, v);
#else
            if constexpr (Index <= 5) NotifyCHRWrite(Index, v);
#endif
        }

        operator u8() const { return shadow_; }
        Register &operator=(const u8 v)          { set(v);         return *this; }
        Register &operator=(const Register &o)   { set(o.shadow_); return *this; } ///< restore path
    };

    /**
     * @brief MMC3's two register-controlled PRG-select windows.
     *
     * Not `const`: same reasoning as VRC1's window1Control etc. -- a write-only
     * register that can never be written defeats the point. Defined (not just
     * declared) in mmc3.cpp, where the default-bank boot-time init also lives.
     * There is no window3Control -- see this file's header comment.
     */
    static Register<6> window1Control; ///< PRG-select R6, window 1 ($8000-$9FFF). See above.
    static Register<7> window2Control; ///< PRG-select R7, window 2 ($A000-$BFFF). See ::window1Control.

    /**
     * @brief MMC3's six CHR-select registers (R0-R5).
     *
     * chr0Control/chr1Control select 2 KiB windows (PPU $0000/$0800 under CHR
     * mode 0), but -- like every CHR register on this chip -- the value
     * written still counts banks in 1 KiB units; the low bit is ignored by
     * hardware (an odd bank can't be selected, since it wouldn't be 2 KiB-
     * aligned), though nothing here masks it away -- the shadow simply holds
     * whatever was written, so a consumer resolving a tile address (e.g.
     * ::GetTileLMA) must mask it off itself, not just multiply by 2 KiB.
     * chr2Control-chr5Control select 1 KiB banks (PPU $1000/$1400/$1800/
     * $1C00), full 8-bit range (up to 256 KiB CHR-ROM). Named chrNControl to
     * match VRC1's chr0Control/chr1Control convention as closely as this
     * chip's extra granularity allows.
     */
    static Register<0> chr0Control; ///< CHR-select R0, 2 KiB @ PPU $0000.
    static Register<1> chr1Control; ///< CHR-select R1, 2 KiB @ PPU $0800.
    static Register<2> chr2Control; ///< CHR-select R2, 1 KiB @ PPU $1000.
    static Register<3> chr3Control; ///< CHR-select R3, 1 KiB @ PPU $1400.
    static Register<4> chr4Control; ///< CHR-select R4, 1 KiB @ PPU $1800.
    static Register<5> chr5Control; ///< CHR-select R5, 1 KiB @ PPU $1C00.

    /**
     * @brief Selects nametable mirroring ($A000).
     *
     * On NES: a direct $A000 write (bit 0: 0 = vertical, 1 = horizontal --
     * opposite polarity from the iNES header's own mirroring bit, a
     * well-known MMC3 gotcha, not a typo). Off-NES: no hardware to write, so
     * this instead writes the emu PPU's own library-visible ::mirroring
     * global (video.hpp) directly, for the emu side to consult later.
     *
     * @param horizontal false selects vertical mirroring (side-by-side
     *                   nametables); true selects horizontal.
     */
    static void SetMirroring(bool horizontal);

    /**
     * @brief PRG-RAM enable/write-protect ($A001). Bit 7 = RAM chip enable
     *        (1 = enabled), bit 6 = write protection (1 = deny writes, 0 =
     *        allow). mmc3.cpp's ::_reset writes 0x80 -- enabled, writable --
     *        at boot.
     */
    static tech::wo_register<0xa001> prgRamProtect;

    /**
     * @brief Scanline IRQ reload latch ($C000) -- the value the counter is set
     *        to whenever it reloads. See ::ScheduleScanlineIRQ.
     */
    static tech::wo_register<0xc000> irqLatch;

    /**
     * @brief Writes @p ctx into an MMC3 bank register, banking in that
     *        window/CHR slice.
     *
     * Thin, named wrapper over ::Register's write path, mirroring VRC1's
     * ::SwitchBank shape exactly. @p reg is taken by reference for the same
     * reason ::SwitchBank documents: by value, the hardware poke would still
     * happen, but the shadow update would be lost.
     *
     * Not used by ::_reset (mmc3.cpp): that runs before .bss has been zeroed,
     * so window1Control/window2Control's shadows aren't valid to write yet --
     * see ::_reset's own comment for why it pokes hardware directly instead.
     */
    template <u8 Index>
    static constexpr void SwitchBank(Register<Index> &reg, u8 ctx) {
        reg = ctx;
    }

    /**
     * @brief Selects which CHR-ROM bank appears in one of MMC3's six CHR
     *        windows.
     *
     * A named alias for ::SwitchBank restricted to chr0Control-chr5Control. The
     * hardware needs no different write path here, unlike VRC1; the distinct
     * name just keeps PRG and CHR switches distinguishable at the call site.
     *
     * @param reg  chr0Control through chr5Control; any other instantiation is a
     *             compile error.
     * @param bank Target bank, full 8-bit range (0-255) for every CHR
     *             register, all in 1 KiB units -- for chr0Control/chr1Control
     *             specifically, an odd value's low bit is ignored by hardware
     *             (see ::chr0Control's own comment), so only even values
     *             actually select a distinct 2 KiB window.
     */
    template <u8 Index>
    static constexpr void SwitchCHRBank(Register<Index> &reg, u8 bank) {
        static_assert(Index <= 5, "SwitchCHRBank only valid for chr0Control-chr5Control (R0-R5).");
        reg = bank;
    }

    /**
     * @brief Arms MMC3's scanline IRQ counter to fire after @p scanline more
     *        PPU A12 rising edges (in practice, roughly @p scanline scanlines
     *        with 8x8 sprites/background rendering enabled).
     *
     * Sets the reload latch ($C000), forces a reload ($C001), enables generation
     * ($E001). Does NOT acknowledge an IRQ still pending from a previous split
     * -- call ::AcknowledgeScanlineIRQ first if one might be.
     *
     * @param scanline Reload value for the countdown; 0 fires on the very next
     *                 qualifying PPU address change. Meaningful on NES only --
     *                 it becomes the real $C000 register value.
     * @param position Off-NES ONLY: where the software rasterizer's split
     *                  fires (::irq::irqPosition). Defunct on NES, which counts
     *                  A12 edges and has no pixel-column concept.
     *
     *                  Deliberately NOT derived from @p scanline: that value is
     *                  tuned against real A12-edge-counting latency, so reusing
     *                  it off-NES mixes two timing models and lands the split
     *                  at the wrong row. @p position is the row the caller
     *                  actually means.
     */
    static void ScheduleScanlineIRQ(u8 scanline, vec2<u16> position);

    /**
     * @brief Disables further scanline IRQs and clears any currently pending
     *        one ($E000).
     *
     * Purely the acknowledge/disable half -- it does NOT re-enable or re-arm
     * the counter. Call ::ScheduleScanlineIRQ afterward to schedule the next
     * split; keeping the two separate means acknowledging a handler that
     * genuinely has nothing more to schedule this frame doesn't silently leave
     * the counter armed.
     */
    static void AcknowledgeScanlineIRQ();

    /*
     * BANKED CALL scaffolding, mirroring vrc1.hpp's own (see
     * BANKED_CALL_THEORY.txt) as closely as MMC3's two-window hardware allows.
     * Nested under mmc3:: (unlike VRC1's global versions) per this file's own
     * convention -- use ::MMC3_BANKED / ::MMC3_BANKED_EXTERN (below, outside
     * the class) to register a tagged function; they qualify mmc3::bank_of /
     * mmc3::bank_layout for you.
     */

    /**
     * @brief One physical bank/segment's location in the linked ROM image.
     *
     * rom_address is the LOADADDR-side encoded address, not the CPU-visible VMA,
     * since every bank aliasing onto a switchable window shares one VMA.
     *
     * ENCODING -- differs from VRC1's by one, deliberately:
     *
     *     rom_address = ((bank + 1) << 16) | vma
     *
     * where `vma` is $8000 (window 1 / R6) or $A000 (window 2 / R7), and
     * `bank` is the domain's real physical bank -- its file offset / 0x2000,
     * in the order regions are FULL()'d in the consuming project's own
     * OUTPUT_FORMAT. The linker script must encode the SAME bias in its
     * MEMORY region ORIGIN.
     *
     * VRC1 uses an unbiased `bank << 16`. Two things force the +1 here:
     *
     * 1. BANK 0 WOULD BE UNREPRESENTABLE. CallInSection reads a zero high half
     *    as "no bank encoded" and falls back to the window index. MMC3 domains
     *    start at physical bank 0, so one reached through window 2 would
     *    silently bank in bank 1.
     * 2. LD.LLD REJECTS THE UNBIASED REGION. Ordinary .text lives at
     *    $8000-$DFFF, so an unbiased bank 0 region at ORIGIN 0x8000 overlaps
     *    it and the link fails. Biasing lifts every domain clear of it.
     *
     * The bias is free at runtime: a JSR operand carries 16 bits, so every high
     * bit is discarded by ordinary relocation truncation. Only the linker and
     * this decode ever see it.
     */
    struct section_t {
        u32 rom_address;
        u32 size;
    };

    /// Hardware facts for one tag/domain: primary template undefined (an
    /// un-tagged Tag is a compile error), every specialization provides
    /// `static section_t section()`. See vrc1.hpp's own bank_layout<Tag> doc
    /// comment for the constexpr-vs-runtime-section() cost tradeoff, which
    /// applies identically here.
    ///
    /// THERE IS NO `always_mapped` ESCAPE HATCH, deliberately. A tag names a
    /// real bank, and having a tag means the call switches to it. Code that is
    /// already in reach -- ::FIXED, or whatever the caller knows is currently
    /// mapped -- is reached by an ordinary call and needs no tag, no layout
    /// and no farcall at all. A layout that claimed to name a bank while
    /// compiling to no bank switch was a contradiction that only existed to
    /// let unbanked code go through the banked spelling.
    template <typename Tag> struct bank_layout;

    /// Which bank_layout<Tag> a specific tagged function was registered under.
    /// One specialization per MMC3_BANKED()/MMC3_BANKED_EXTERN() call site.
    /// Primary template deliberately undefined, same reasoning as
    /// mmc3::bank_layout<Tag>.
    template <auto Fn> struct bank_of;

    /**
     * @brief A function pointer that carries its own bank -- the storable
     *        form of what ::Call<Fn> knows at compile time.
     *
     * ::Call<Fn> needs Fn nameable AT THE CALL SITE as a template argument, and
     * `table[runtime_index]` never is, however the table is declared. But
     * ::bank_of<Fn> is correct for each candidate individually, so ::GetCallable
     * materializes it into an ordinary value at the one point the function is
     * still nameable, and the runtime index then only picks between entries that
     * already describe themselves.
     *
     * WINDOW AND BANK, NOT section_t: Detail::CallInSection never reads `size`
     * and re-derives window and bank from `rom_address` on every call, all of it
     * determined once ::GetCallable runs. Two resolved bytes make an entry 4
     * rather than 10, and take the arithmetic off the call path.
     *
     * A tag always names a real bank, so there is no "already mapped" case to
     * encode -- see ::bank_layout.
     */
    template <typename Ret, typename... Args>
    struct callable_t {
        Ret (*fn)(Args...);  ///< the target, as an ordinary 16-bit pointer
        u8 window;           ///< 0 = window1Control ($8000), 1 = window2Control ($A000)
        u8 bank;             ///< value written to R6/R7, already un-biased
    };

    /// Return-type extraction for a plain function pointer, used by mmc3::Call.
    /// Also names the matching ::callable_t, so ::GetCallable can spell its own
    /// return type without the caller restating the signature.
    template <typename T> struct function_traits;

    /**
     * @brief Calls @p fn with window1Control or window2Control temporarily
     *        re-banked, then restores whatever was banked there before
     *        returning.
     *
     * Manual, non-tag-based counterpart to ::Call -- mirrors VRC1's ::Long
     * exactly, with one fewer valid window (0 or 1 only; MMC3 has no third
     * register-controlled window, see this file's header comment).
     *
     * @tparam TReturn Result type of the call; specify explicitly, e.g.
     *                 `Long<int>(...)`.
     * @param fn     Callable to invoke once the selected window is banked in.
     * @param window Which window to switch: 0 (window1Control/$8000) or 1
     *               (window2Control/$A000); default 0. Off-NES this parameter
     *               is ignored.
     */
    template <typename TReturn, typename TFunc>
    static FIXED TReturn Long(TFunc fn, u8 window = 0);

    /**
     * @brief Calls a specific, compile-time-known MMC3_BANKED() function,
     *        resolving its bank from mmc3::bank_of<Fn> automatically.
     *
     * Identical shape and behaviour to VRC1's ::Call.
     *
     * @tparam Fn Address of an MMC3_BANKED()/MMC3_BANKED_EXTERN()-tagged
     *            function -- supplied explicitly, e.g. `Call<LoadChunk>(id)`.
     */
    template <auto Fn, typename... Args>
    static FIXED auto Call(Args &&...args) -> typename function_traits<decltype(Fn)>::return_type;

    /**
     * @brief Resolves a tagged function into a storable, self-describing
     *        ::callable_t. The compile-time half of runtime dispatch.
     *
     * Call it wherever the function is still nameable, then store or pass the
     * result freely -- no tag, no template argument, no bank_of<> lookup again:
     *
     *     MMC3_BANKED(".prg_rom_enemies_a", enemiesA, void, UpdateWalker, Entity*);
     *     MMC3_BANKED(".prg_rom_enemies_b", enemiesB, void, UpdateFlyer,   Entity*);
     *
     *     // two DIFFERENT domains -- no single bank_of<> is right for both.
     *     // Spell the element type: `constexpr auto table[]` cannot deduce
     *     // from a braced list.
     *     constexpr mmc3::callable_t<void, Entity*> table[] = {
     *         GetCallable<UpdateWalker>(), GetCallable<UpdateFlyer>() };
     *
     *     Call(table[entity.type], &entity);   // runtime index, correct bank
     *
     * `constexpr` so such a table lands in ROM fully resolved. That needs the
     * tag's bank_layout<Tag>::section() to be constexpr, the preferred shape
     * anyway (see ::bank_layout).
     */
    template <auto Fn>
    static constexpr auto GetCallable() ->
        typename function_traits<decltype(Fn)>::callable_type;

    /**
     * @brief Calls a ::callable_t, banking in whatever it says it needs.
     *
     * The runtime counterpart to ::Call<Fn>: an ordinary overload rather than
     * a template on Fn, taking the already-resolved value ::GetCallable
     * produced. Same switch-run-restore as every other farcall here, minus the
     * address arithmetic, which ::GetCallable already did.
     */
    template <typename Ret, typename... Args, typename... Actual>
    static FIXED Ret Call(const callable_t<Ret, Args...> &c, Actual &&...args);

    /**
     * @brief Runs @p block with one bank mapped, selected by TAG rather than
     *        by a registered function.
     *
     * ::Call keys off a function that ::MMC3_BANKED / ::MMC3_BIND has bound
     * to a tag; this keys off the tag directly, so nothing has to be bound at
     * all. That matters for code this project does not own -- a third-party
     * audio engine, hand-written assembly, a Rust or C module -- where there
     * may be many entry symbols and binding each one buys nothing. It also
     * covers the case a separate ::CallBlock once did (running an arbitrary
     * block under an already-bound function's window): pass that function's
     * own tag here instead -- one entry point rather than two ways to reach
     * the same bank:
     *
     *     mmc3::CallInBlock<bank002_tag>([]{ engine_update(); });
     *
     * @tparam Tag A bank tag with a ::bank_layout specialization.
     */
    template <typename Tag, typename Block>
    static FIXED auto CallInBlock(Block &&block) -> decltype(block());

    /**
     * @brief Runs @p block with TWO banks mapped at once -- one per window.
     *
     * For a module whose code and data live in different banks and must be
     * mapped together -- an audio engine walking song data, a decompressor
     * reading a banked stream. Two is the hard maximum, and the tags must target
     * DIFFERENT windows or the second switch displaces the first. Enforced
     * wherever both layouts expose a constexpr section().
     *
     *     mmc3::CallPairedBlock<code_tag, data_tag>([]{ engine_update(); });
     *
     * Two nested ::CallInBlock calls, so both banks restore in reverse order and
     * both trampolines sit in the fixed bank. Roughly twice one farcall.
     *
     * WHAT THE BLOCK MAY TOUCH: with both windows switched, nothing of the
     * ordinary program at $8000-$BFFF is mapped. The block may call the two
     * banks and ::FIXED code and touch RAM freely, but not ordinary resident
     * functions. Interrupts stay live, safe only because the vector handlers are
     * themselves pinned to the fixed bank.
     */
    template <typename CodeTag, typename DataTag, typename Block>
    static FIXED auto CallPairedBlock(Block &&block) -> decltype(block());

private:
    /**
     * @brief Implementation details not part of MMC3's public interface.
     *        Private nested class purely as a namespacing device -- a class
     *        cannot nest a plain `namespace`, so this plays the same role
     *        the free-standing `namespace detail` played before.
     */
    struct Detail {
        /**
         * @brief Bit 6 (PRG mode) / bit 7 (CHR A12 invert) of every $8000 write.
         *
         * $8000 packs the register-select index (bits 0-2) together with these
         * two latched mode bits in the SAME byte, so selecting a different
         * index without re-asserting the last mode bits would silently clear
         * whichever one wasn't also specified again. This module fixes both
         * bits at 0 permanently (PRG mode 0, CHR non-inverted -- see this
         * file's header comment) and never exposes a way to change them, so
         * every $8000 write below is simply the bare register index; there is
         * no runtime state to track. If a future revision needs to flip either
         * bit, THIS is the one place that assumption lives.
         */
        static constexpr u8 kModeBits = 0;

        /// Windows 1/2's shared geometry: WINDOW_SIZE bytes each, starting at
        /// WINDOW_BASE ($8000, window1Control's own base). Same constants as
        /// VRC1's -- both chips use 8 KiB windows starting at $8000 -- but only
        /// windowIndex 0/1 are ever valid here (see ::CallInSection's own
        /// comment); MMC3 has no windowIndex-2 register to switch to.
        static constexpr u16 kWindowBase = 0x8000;
        static constexpr u16 kWindowSize = 0x2000;

        /**
         * @brief Implementation of ::mmc3::Long / ::CallInSection once the
         *        target register is resolved. Not for direct use.
         *
         * Same shape, and the same LOAD-BEARING [[gnu::noinline]] reasoning, as
         * vrc1_detail::CallInWindow: `fixed` only places an out-of-line symbol,
         * and inlining this into a caller that itself lives in the window being
         * switched away from is a real, reproduced crash on VRC1 (see
         * BANKED_CALL_THEORY.txt) -- the same failure mode applies here
         * unchanged, since it's a property of "switch a bank out from under
         * code that keeps running in that bank," not anything VRC1-specific.
         */
        template <typename TReturn, u8 Index, typename TFunc>
        [[gnu::noinline]] static FIXED TReturn CallInWindow(Register<Index> &reg, u8 bank, TFunc fn) {
            // DELIBERATELY NOT ::SHADOW, and measured rather than stylistic. A
            // shadow scope keeps more live state across fn() than llvm-mos will
            // hold in registers, so the thunk allocates a soft-stack frame and
            // pays a frame-pointer adjust plus four register pushes and pulls
            // either side -- most of the thunk's cost, against a bank switch of
            // 35 cycles.
            //
            // Nothing here needs a general scope: one register, at a link-time
            // constant address. Saving the byte by hand keeps live state small
            // enough for the hardware stack. The restore still happens after
            // fn() and still goes through set(), i.e. shadow and hardware.
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
         * @brief Resolves a section_t to whichever of window1Control/
         *        window2Control MMC3 needs, then runs @p fn.
         *
         * Same WINDOW/BANK derivation as vrc1_detail::CallInSection: WINDOW
         * comes from the low 16 bits of rom_address (truncated exactly like a
         * real JSR operand would be), BANK defaults to WINDOW unless
         * bank_layout<Tag>::section() hand-encodes an explicit bank in
         * rom_address's high bits.
         *
         * ONLY windowIndex 0 or 1 are valid: no register reaches $C000-$DFFF, so
         * a section() address at $C000 or above is a caller error. It is NOT
         * range-checked -- it falls through to window1Control and silently
         * mis-banks -- so verify hand-entered addresses stay below $C000, the
         * way a linker ASSERT cross-checks hand-entered bank numbers.
         *
         * No CallInWindows2 equivalent: only 16 KiB is register-controlled here,
         * so a domain spanning both windows is unsupported. Add it the way
         * vrc1.hpp does if a real need arises.
         */
        template <typename TReturn, typename TFunc>
        [[gnu::noinline]] static FIXED TReturn CallInSection(const section_t &section, TFunc fn) {
            const u16 vma = static_cast<u16>(section.rom_address);
            const u8 windowIndex = static_cast<u8>((vma - kWindowBase) / kWindowSize);
            // High half is bank + 1, not bank -- see ::section_t's ENCODING
            // note for why this file biases where VRC1 doesn't. Zero still
            // means "nothing encoded", and still falls back to the window
            // index, so a VRC1-style unbiased layout for a nonzero bank keeps
            // working; only bank 0 actually needs the bias to be expressible.
            const u8 encodedBank = static_cast<u8>(section.rom_address >> 16);
            const u8 bank = encodedBank != 0 ? static_cast<u8>(encodedBank - 1) : windowIndex;
            switch (windowIndex) {
                case 1:  return CallInWindow<TReturn>(window2Control, bank, fn);
                default: return CallInWindow<TReturn>(window1Control, bank, fn);
            }
        }
    };
};

template <typename R, typename... A>
struct mmc3::function_traits<R (*)(A...)> {
    using return_type = R;
    using callable_type = mmc3::callable_t<R, A...>;
};

template <typename TReturn, typename TFunc>
FIXED TReturn mmc3::Long(TFunc fn, const u8 window) {
#ifdef TARGET_NES
    switch (window) {
        case 1:  return Detail::CallInWindow<TReturn>(window2Control, 1, fn);
        default: return Detail::CallInWindow<TReturn>(window1Control, 0, fn);
    }
#else
    return fn();
#endif
}

template <auto Fn, typename... Args>
FIXED auto mmc3::Call(Args &&...args) -> typename function_traits<decltype(Fn)>::return_type {
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

template <auto Fn>
constexpr auto mmc3::GetCallable() ->
    typename function_traits<decltype(Fn)>::callable_type {
#ifdef TARGET_NES
    using L = typename bank_of<Fn>::layout;
    const section_t s = L::section();
    // Detail::CallInSection's derivation, done once here instead of per call,
    // including the bank+1 bias (a zero high half means nothing was encoded, and
    // the window index doubles as the bank).
    const u16 vma = static_cast<u16>(s.rom_address);
    const u8 window = static_cast<u8>((vma - Detail::kWindowBase) / Detail::kWindowSize);
    const u8 encodedBank = static_cast<u8>(s.rom_address >> 16);
    return { Fn, window, static_cast<u8>(encodedBank != 0 ? encodedBank - 1 : window) };
#else
    // Off-NES there are no banks and bank_layout<Tag> is typically not even
    // specialized, so resolving here would be a compile error. Call() below
    // ignores these fields off-NES.
    return { Fn, 0, 0 };
#endif
}

template <typename Ret, typename... Args, typename... Actual>
FIXED Ret mmc3::Call(const callable_t<Ret, Args...> &c, Actual &&...args) {
#ifdef TARGET_NES
    // window1Control and window2Control are different types (Register<6> vs
    // Register<7>), so a runtime window choice has to be a branch between
    // instantiations, not a variable.
    auto invoke = [&]() -> Ret { return c.fn(static_cast<Actual &&>(args)...); };
    if (c.window == 1) return Detail::CallInWindow<Ret>(window2Control, c.bank, invoke);
    return Detail::CallInWindow<Ret>(window1Control, c.bank, invoke);
#else
    return c.fn(static_cast<Actual &&>(args)...);
#endif
}

template <typename Tag, typename Block>
FIXED auto mmc3::CallInBlock(Block &&block) -> decltype(block()) {
#ifdef TARGET_NES
    using L = bank_layout<Tag>;
    return Detail::CallInSection<decltype(block())>(L::section(), block);
#else
    return block();
#endif
}

template <typename CodeTag, typename DataTag, typename Block>
FIXED auto mmc3::CallPairedBlock(Block &&block) -> decltype(block()) {
#ifdef TARGET_NES
    // Both layouts' window index must differ -- see the declaration's comment.
    // Only checkable when both section()s are usable in a constant expression
    // (the constexpr shape this project's own domains use); a runtime section()
    // is exempt rather than rejected, since its address genuinely isn't known
    // until link time.
    if constexpr (requires {
            std::integral_constant<u32, bank_layout<CodeTag>::section().rom_address>{};
            std::integral_constant<u32, bank_layout<DataTag>::section().rom_address>{}; }) {
        static_assert(
            (static_cast<u16>(bank_layout<CodeTag>::section().rom_address) & 0xE000) !=
            (static_cast<u16>(bank_layout<DataTag>::section().rom_address) & 0xE000),
            "mmc3::CallPairedBlock: both tags map through the SAME window, so the "
            "second bank would immediately displace the first. Encode one at $8000 "
            "(R6) and the other at $A000 (R7).");
    }
    return CallInBlock<CodeTag>([&]() -> decltype(block()) {
        return CallInBlock<DataTag>(block);
    });
#else
    return block();
#endif
}

/**
 * @brief Declares a tagged, out-of-line C++ function and registers it with
 *        mmc3::bank_of<>, so mmc3::Call can resolve its bank later.
 *
 * Same VRC1_BANKED -> VRC1_BANKED_IMPL macro-expansion-order reasoning as
 * VRC1's ::VRC1_BANKED (see its own comment): forces section_name/tag to
 * their final literal text before # or ## ever sees them. Named MMC3_BANKED
 * rather than plain BANKED specifically so both mapper headers can be seen
 * by the same translation unit without a macro-redefinition clash -- a bare
 * `BANKED` name would collide the moment both this header and vrc1.hpp are
 * included together, so each mapper gets its own prefixed macro
 * (MMC3_BANKED / VRC1_BANKED) expanding to its own qualified class
 * (mmc3:: / VRC1::) instead.
 */
#define MMC3_BANKED(section_name, tag, ret, name, ...) \
    MMC3_BANKED_IMPL(section_name, tag, ret, name, __VA_ARGS__)
#define MMC3_BANKED_IMPL(section_name, tag, ret, name, ...)                      \
    CREATE_SEGMENT_KEYWORD(section_name) ret name(__VA_ARGS__);                 \
    template <> struct mmc3::bank_of<static_cast<ret (*)(__VA_ARGS__)>(&name)> { \
        using layout = mmc3::bank_layout<tag##_tag>;                            \
    }

/**
 * @brief Registers an ALREADY-DECLARED function with mmc3::bank_of<>, so
 *        mmc3::Call can resolve its bank.
 *
 * The namespace-friendly counterpart to ::MMC3_BANKED, which declares the
 * function itself and so can only produce an unqualified name: its declaration
 * would have to sit inside the namespace while the bank_of<> specialization
 * must be at global scope. This macro splits the two -- place the function with
 * a segment keyword where it is defined, then bind it here by qualified name:
 *
 *     namespace ui { UI_BANK void Draw() { ... } }   // placement
 *     MMC3_BIND(ui::Draw, ui_bank);                  // binding, global scope
 *     mmc3::Call<ui::Draw>();                        // call site
 *
 * @p fn is a qualified name, not an address; @p tag the bank tag minus its
 * `_tag` suffix. The function must not be overloaded, since `&fn` has to
 * resolve without a cast -- use ::MMC3_BANKED if it is.
 *
 * PLACEMENT AND BINDING ARE INDEPENDENT and nothing checks they agree: binding
 * to a tag whose bank_layout names a different bank than the section keyword
 * placed it in banks in the wrong 8 KiB and executes garbage. Define the
 * keyword and its tag next to each other so the two can only drift on
 * purpose.
 */
#define MMC3_BIND(fn, tag)                     \
    template <> struct mmc3::bank_of<&fn> {    \
        using layout = mmc3::bank_layout<tag##_tag>; \
    }

/**
 * @brief Registers a function with mmc3::bank_of<> without declaring/
 *        placing it -- for hand-written assembly or a separately-assembled
 *        third-party engine. See VRC1's ::BANKED_EXTERN for the full
 *        reasoning; identical here except for the mmc3:: qualification.
 */
#define MMC3_BANKED_EXTERN(section_name, tag, ret, name, ...) \
    MMC3_BANKED_EXTERN_IMPL(section_name, tag, ret, name, __VA_ARGS__)
#define MMC3_BANKED_EXTERN_IMPL(section_name, tag, ret, name, ...)               \
    extern "C" ret name(__VA_ARGS__);                                           \
    template <> struct mmc3::bank_of<static_cast<ret (*)(__VA_ARGS__)>(&name)> { \
        using layout = mmc3::bank_layout<tag##_tag>;                            \
    }
