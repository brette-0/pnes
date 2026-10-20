#include <pnes/mappers/vrc1.hpp>

tech::wo_register<0x8000> VRC1::window1Control;
tech::wo_register<0xa000> VRC1::window2Control;
tech::wo_register<0xc000> VRC1::window3Control;

extern "C" void _start();

extern "C" FIXED void _reset() {
    tech::poke(0x8000, 0);
    tech::poke(0xa000, 1);
    tech::poke(0xc000, 2);
    _start();
}

/*
 * The Phase 1 fixed-bank smoke test that used to sit here is gone with the
 * always_mapped path it existed to exercise: it tagged a function into
 * .prg_rom_fixed and then asserted the call compiled to no bank switch at
 * all. A bank_layout now always names a real bank, so that shape is no
 * longer expressible -- and the Phase 2+ tests below exercise the real
 * switching path its own comment said would supersede it.
 */

/*
 * BANKED CALL, Phase 2 smoke test: exercises Call<Fn> through a REAL
 * switchable-window round trip (VRC1::Detail::CallInSection), not just the
 * always-mapped short-circuit Phase 1 covered.
 *
 * window_test_tag's section() IS a fixed, hand-picked address (0xdfc0,
 * window index 2 / window3Control) -- a real constexpr literal, matching
 * prg_rom_window_test's ORIGIN in vrc1.ld (see that region's own comment
 * for why: an earlier revision computed this at runtime from a linker
 * symbol instead, measured at ~120 bytes of pure overhead per Call<Fn>
 * call site, since the optimizer never manages to fold the resulting
 * subtract/divide/branch back down to the compile-time constant it
 * actually is). vrc1.ld's ASSERT catches the two going out of sync.
 */
struct window_test_tag {};

template <> struct VRC1::bank_layout<window_test_tag> {
    static constexpr section_t section() {
        return { 0xdfc0, 6 };
    }
};

VRC1_BANKED(".banked_call_window_test", window_test, void, BankedCallWindowTest);

volatile u8 bankedCallWindowTestMarker = 0;

[[gnu::noinline]] void BankedCallWindowTest() {
    bankedCallWindowTestMarker = 1;
}

/*
 * Smoke test: a REAL multi-bank window. window3Control now has two distinct
 * banks that can appear at $C000-$DFFF, so CallInSection must write the correct
 * one each time rather than reasserting the one that was always right. The tag
 * hand-encodes bank 3 in rom_address's high bits as a constexpr literal, so it
 * pays no runtime-resolution cost.
 *
 * It also calls Call<BankedCallWindowTest>() from inside itself: a NESTED
 * cross-bank call, same window, different banks. That is the real test -- it
 * only comes out right if the save/restore threads through a genuine bank VALUE
 * change, not just window-register identity. Get the bank wrong and the bank
 * actually switched in does not contain the code control returns to.
 */
struct bank3_test_tag {};

template <> struct VRC1::bank_layout<bank3_test_tag> {
    static constexpr section_t section() {
        // (3u << 16) is WRONG here: unsigned int is only 16 bits wide on
        // this target, so that shift is undefined behavior (caught by
        // -Wshift-count-overflow) rather than the 0x30000 it looks like.
        // rom_address is u32 -- shift in that width explicitly.
        return { (static_cast<u32>(3) << 16) | 0xc000, 12 };
    }
};

VRC1_BANKED(".prg_rom_bank3", bank3_test, void, BankedCallBank3Test);

volatile u8 bankedCallBank3TestMarker = 0;

[[gnu::noinline]] void BankedCallBank3Test() {
    bankedCallBank3TestMarker = 3;
    VRC1::Call<BankedCallWindowTest>();
}

/*
 * Smoke test: an OVERSIZED domain, bigger than one window and spanning
 * window2Control and window3Control at once. Unlike every earlier tag, size
 * here is read from the linker's SIZEOF() rather than hand-entered -- how big
 * the domain gets is exactly what nobody should have to declare in advance.
 * CallInSection ceil-divides it and hands off to CallInWindows2 when >1.
 *
 * WATCH THE BANK NUMBER. Real banks are file offset / 0x2000 in FULL()'s listed
 * order, NOT "next free after the previous tag". Hand-entering the wrong base
 * mapped in the fixed bank's content instead of this domain's -- garbage
 * execution, found by a runtime crash rather than review.
 */
struct oversized_tag {};

extern "C" const u8 __oversized_domain_size[];

template <> struct VRC1::bank_layout<oversized_tag> {
    static section_t section() {
        // Base bank (4) IS hand-entered and constexpr-safe -- it's the
        // domain's own fixed starting point, same as bank3_test_tag's.
        // Only size is a genuine runtime read; see this tag's own comment.
        return { (static_cast<u32>(4) << 16) | 0xa000, reinterpret_cast<u32>(__oversized_domain_size) };
    }
};

VRC1_BANKED(".prg_rom_oversized", oversized, void, OversizedFuncLow);
VRC1_BANKED(".prg_rom_oversized", oversized, void, OversizedFuncHigh);

volatile u8 oversizedFuncLowMarker = 0;
volatile u8 oversizedFuncHighMarker = 0;

/*
 * OversizedFuncLow calls OversizedFuncHigh as an ORDINARY, BARE C++ call --
 * no Call<Fn>, no BANKED_CALL machinery at all. This is the actual point of
 * an oversized domain: once Call<OversizedFuncLow>() has mapped BOTH its
 * windows in (CallInWindows2), everything inside the domain is just
 * ordinary, contiguous, normally-addressable code -- BANKED_CALL_THEORY.txt's
 * "intra-domain calls stay bare, always" rule. Getting CallInWindows2 wrong
 * (e.g. only switching one of the two windows, or switching the wrong bank
 * for the second one) wouldn't just reassert an already-correct value the
 * way Phase 2/3's tests could tolerate -- OversizedFuncHigh's JSR target
 * would show whatever ACTUALLY happens to be banked into window3Control at
 * the time, not necessarily bank 5's real code. (This is exactly what
 * happened when the base bank below was still wrong -- see this domain's
 * own comment in vrc1.ld.)
 */
[[gnu::noinline]] void OversizedFuncLow() {
    oversizedFuncLowMarker = 1;
    // Padding: inline asm .fill, INSIDE this function's own body -- not a
    // separate global array. A data array tagged into .prg_rom_oversized
    // conflicts with the section already being classified executable by
    // OversizedFuncLow/High themselves (confirmed: "section type conflict"
    // at compile time, GNU/Clang won't mix code and data flags on one
    // named section). Padding as code, inside an already-guaranteed-to-
    // survive function (this one is Call<>'d directly), sidesteps both
    // problems: no section-type mismatch, and no separate gnu::used/retain
    // needed to keep it from being dead-code-eliminated. Specifically
    // sized to push OversizedFuncHigh's own address past the 0x2000 (one
    // window) mark within the domain, so this test genuinely exercises two
    // windows instead of coincidentally fitting in one -- vrc1.ld's ASSERT
    // on __oversized_domain_size is the build-time guard confirming that.
    // 0xea is 6502 NOP -- never executed (falls after the call below), but
    // an innocuous choice of filler byte regardless.
    __asm__ volatile(".fill 8300, 1, 0xea");
    OversizedFuncHigh();
}

/*
 * Also nests a Call<Fn> into a DIFFERENT, smaller (single-window) domain
 * from partway through the oversized one, on top of the bare intra-domain
 * call above -- the save/restore composition case Phase 4's own roadmap
 * entry calls out: CallInWindows2's two SHADOW-based register switches
 * (window2Control AND window3Control) have to stay correctly saved while
 * this NESTED, single-window Call<> temporarily repurposes JUST
 * window3Control for BankedCallWindowTest's own bank (2), leaving
 * window2Control (still bank 4, this domain's own) completely untouched
 * throughout -- then restore window3Control back to bank 5, not whatever
 * BankedCallWindowTest happened to leave behind.
 */
[[gnu::noinline]] void OversizedFuncHigh() {
    oversizedFuncHighMarker = 1;
    VRC1::Call<BankedCallWindowTest>();
}

/**
 * @brief Re-syncs window1Control/2/3's RAM shadows to match what ::_reset
 *        already poked into hardware.
 *
 * Runs as an ordinary global constructor -- i.e. after crt0's .bss zeroing,
 * which is what makes writing through ::VRC1::SwitchBank safe here but not
 * in ::_reset (see its comment). By this point the switchable windows are
 * already correctly banked by hardware; this only brings the RAM shadows
 * (read by wo_register::get()) into agreement with them, so later code
 * that reads e.g. window2Control.get() sees 1, not a stale post-.bss-zero 0.
 */
__attribute__((constructor(101)))
static void SyncBankShadows() {
    VRC1::SwitchBank(VRC1::window1Control, 0);
    VRC1::SwitchBank(VRC1::window2Control, 1);
    VRC1::SwitchBank(VRC1::window3Control, 2);
    // Safe here specifically because it comes AFTER the three SwitchBank
    // calls above: CallInSection's SHADOW-based save/restore (inside
    // CallInWindow) needs window1/2/3Control's RAM shadows to already
    // reflect real hardware state, which is exactly what this function
    // exists to establish -- see this function's own doc comment.
    VRC1::Call<BankedCallWindowTest>();
    // Nested cross-bank call: switches window3Control to bank 3, runs,
    // and (from BankedCallBank3Test's own body) nests a Call<> back to
    // bank 2's BankedCallWindowTest before returning -- see
    // BankedCallBank3Test's own comment for why this is the real test.
    VRC1::Call<BankedCallBank3Test>();
    // Oversized domain: maps window2Control=bank4 AND window3Control=bank5
    // simultaneously (CallInWindows2), runs OversizedFuncLow, which bare-
    // calls OversizedFuncHigh (both windows still mapped, ordinary
    // addressing) and nests a single-window Call<> before either window
    // gets restored -- see OversizedFuncLow/High's own comments.
    VRC1::Call<OversizedFuncLow>();
}

tech::wo_register<0x9000> VRC1::chrHighBits;
tech::wo_register<0xe000> VRC1::chr0Control;
tech::wo_register<0xf000> VRC1::chr1Control;
