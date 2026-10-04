#include "title.hpp"

#ifndef GEN_TARGET_DIR
#error "GEN_TARGET_DIR is not set -- add it to this target's CMakeLists.txt branch."
#endif
// gen::title -- this target's own demo/gen/title/<GEN_TARGET_DIR>/title.hpp.
// Relative path, computed via ::STRCAT (platform-nes/technology.hpp), not an
// -I search-path/angle-bracket trick: GEN_TARGET_DIR is the same bareword
// macro (target_compile_definitions(demo ...) in CMakeLists.txt) every
// platform branch sets to its own demo/gen/*/<dir> name, so this one line
// covers every target without a per-target #if ladder.
#include STRCAT(../../gen/title/GEN_TARGET_DIR/title.hpp)
// gen::gameOptions -- demo/ui/gameOptions.uis, same per-target directory scheme
// (gen/gameOptions/<GEN_TARGET_DIR>/gameOptions.hpp).
#include STRCAT(../../gen/gameOptions/GEN_TARGET_DIR/gameOptions.hpp)
// gen::options -- demo/ui/options.uis (the Options screen's buttons), plus the
// hand-written DrawEnabled_/DrawDisabled_ implementations its generated
// Button nodes require (see uitk's genButtonDecl) -- this is the one TU that
// actually includes it, so it's the one place GEN_TARGET_DIR/platform-nes
// context exists for it.
#include "../../ui/impl/buttons.hpp"
#include "../main.hpp"
#include "../banks.hpp"
#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../graphics/graphics.hpp"
#include "../graphics/metasprites.hpp"
#include "level.hpp"
#include "level/levels.hpp"
#include "platform-nes/mappers/mmc3.hpp"
#include "platform-nes/extras/ui/text.hpp"

namespace title {
    // Both menus are generated (demo/ui/title.uis, demo/ui/gameOptions.uis) --
    // which options exist on this target, where they sit and how many there
    // are all come from gen::title / gen::gameOptions, not from anything
    // hand-written here. title.uis lists NewGame, Continue, Options (and Quit
    // on PC) in that order, so an option's index IS its titleOptions value;
    // this fails the build if the scene and the enum ever drift apart.
    static_assert(gen::title::TitleOptions_nOptions == End + 1,
                  "title.uis and title.hpp's titleOptions enum disagree on the menu's options");

    // Which of the two menus currently owns the selector arrow. Written from
    // the main loop, read from the nmi_handler_draw* handlers (hence
    // `atomic`, technology.hpp: volatile on NES, true atomic elsewhere).
    static atomic bool playModeActive = false;

    // Set once Options is chosen and never cleared: a deliberate one-way
    // lock (there's no Options screen built yet -- see ::nmi_handler_lockOptions).
    // Written from the main loop, read from main loop only (unlike
    // playModeActive, no NMI handler reads it), but kept atomic for the same
    // reason/consistency.
    static atomic bool optionsLocked = false;

    // Nametable address of the arrow slot for option `i` of a generated
    // SingleChoice: two tiles left of that option's anchor. Reads .x/.y
    // individually -- `_options` is `atomic` (volatile) on a variadic target,
    // and a volatile vec2 can't be copied whole.
    template <typename Options>
    static u16 ArrowAddr(const Options& options, const u8 i) {
        return ppu::CartesianToAddress({static_cast<u16>(options[i].x - 2), static_cast<u16>(options[i].y)});
    }

    static u8 ActiveOption() {
        return playModeActive ? gen::gameOptions::gameOptions_option : gen::title::TitleOptions_option;
    }

    static u16 TitleArrowAddr() {
        return ArrowAddr(gen::title::TitleOptions_options, gen::title::TitleOptions_option);
    }

    static u16 PlayModeArrowAddr() {
        return ArrowAddr(gen::gameOptions::gameOptions_options, gen::gameOptions::gameOptions_option);
    }

    static u16 ActiveArrowAddr() {
        return playModeActive ? PlayModeArrowAddr() : TitleArrowAddr();
    }

    // Queues addr as both the clear and write address for the next
    // SelectorUpdate() -- clear-then-arrow onto the same tile nets out to
    // just drawing the arrow, so this is how a caller reuses SelectorUpdate
    // for a first-draw indicator instead of poking the PPU itself.
    static void QueueSelectorDraw(const u16 addr) {
        scratchpad[1] = scratchpad[3] = static_cast<u8>(addr & 0xff);
        scratchpad[2] = scratchpad[4] = static_cast<u8>(addr >> 8);
        scratchpad[0] = 1;
    }

    // scratchpad[3..4] always holds the arrow's current tile (see
    // SelectorUpdate/QueueSelectorDraw).
    static u16 CurrentSelectorAddr() {
        return static_cast<u16>(scratchpad[3] | (scratchpad[4] << 8));
    }

    // Moves the arrow: the tile it is on now becomes the next SelectorUpdate's
    // clear, the new tile its draw.
    static void MoveSelector(const u16 addr) {
        scratchpad[1] = scratchpad[3];
        scratchpad[2] = scratchpad[4];
        scratchpad[3] = static_cast<u8>(addr & 0xff);
        scratchpad[4] = static_cast<u8>(addr >> 8);
        scratchpad[0] = 1;
    }

    static oam::oam_t Clear(u16 _);
    static NI void DrawLevelPreview();
    static void UpdateMarySpriteYForPixels(u16 splitPixelRow);
    static void UpdateMarySpriteY(u8 splitRow);
    static void nmi_handler_drawPlayMode();
    static void nmi_handler_drawMenu();
    static void nmi_handler_lockOptions();
    static void nmi_handler_lockAnim();
    static void nmi_handler_locked();
    static void nmi_handler_lockAnimReverse();

    // Column where the menu/play-mode nametable begins: one nametable's width
    // to the right of nametable A, computed at runtime rather than a
    // compile-time constant: a real console's nametable is a fixed 32 tiles,
    // but a variadic display (LANDSCAPE desktop's runtime window, OGC's
    // runtime TV width, ...) has no such hardware constraint -- its nametable
    // is just its own viewport, whatever size that renders at (see
    // ::emu::ComputeNtGeometry's own doc comment, src/emu/emu.hpp). Floored
    // at 32 for a target whose viewport is instead a CROP of a still-32-wide
    // background (GBA/NDS/DSi). Set once, below, at the top of main() --
    // before ApplySplit() (which reads it) can ever run. The menus' own
    // columns come from the generated scenes, which resolve the same floor.
    static u16 kMenuNT;

    // Mary's feet, in nametable space -- fixed once the level preview loads
    // (DrawLevelPreview), independent of scroll. Cached so
    // ::UpdateMarySpriteY can redo just the scroll half of that same math
    // every step of the reveal animation, without needing the banked level
    // data (::level::levelHeight) still paged in to re-derive it.
    static u16 maryGroundNtRow;

    // Current position through the reveal animation (::nmi_handler_lockAnim)
    // and its reverse (::nmi_handler_lockAnimReverse), in SCANLINES (not
    // tile rows -- stepping one scanline at a time is the whole point of
    // both). Forward counts down from the title menu's own split row
    // (converted to scanlines) to 0; reverse counts back up from 0 to that
    // same value. One scanline every frame (two scanlines' worth of 60fps
    // motion per 30fps-equivalent tick reads as a brisk, still-smooth
    // scroll; this was one scanline every OTHER frame -- half the speed --
    // before). Declared up here (not besides the handlers that actually use
    // it) because ::main's own B-press handling needs it in scope before
    // ::main is defined, to check whether a reveal is actually running.
    static u16 lockAnimScanline;

    // Rows the menu band (everything below the split) is given. The NES lays
    // it out as 30 - 24 = 6 rows, which is what the title/menu text (nametable
    // rows 1-4) is sized for.
    constexpr u8 kMenuBandRows = 6;

    static u8 SplitRow() {
        // A cropping target's panel (DS/DSi 24 rows, GBA 20) is shorter than
        // the NES's 30, but the menu still needs the same 6 rows -- scaling
        // the split with the panel height instead gave the DS an 8-row band,
        // i.e. ~4 empty rows under the UI.
        if (video::viewport_ty() < 30) return static_cast<u8>(video::viewport_ty() - kMenuBandRows);
        return (((viewport_my() + 1) >> 1) - 2) << 2;
    }

    // The preview band is scrolled so that its bottom edge lands exactly on
    // the end of the nametable (world row 240), where Y wraps to row 0 of
    // the menu nametable for the band below the split. That's the nametable's
    // own height, NOT video::viewport_py(): the two are the same on the NES/PC
    // (240), but a cropping target (DS/DSi 192px, GBA 160px) has a shorter
    // panel over the same 30-row nametable -- using the panel height left the
    // second band starting at world row 24 (DS) instead of row 0, so none of
    // the menu/title text (nametable rows 1-4) was ever on screen.
    // Pixel-granular core of the three helpers below (::PreviewScrollYFor,
    // ::SplitYFor, ::ArmSplitIRQFor): all three normally only ever get called
    // with a tile-row split (::SplitRow(), always a multiple of 8 scanlines),
    // but ::nmi_handler_lockAnim walks the split up ONE SCANLINE at a time --
    // rounding that to the nearest tile row would make the reveal jump 8
    // scanlines at once every other frame instead of scrolling smoothly.
    static u16 PreviewScrollYForPixels(const u16 splitPixelRow) {
        const u16 ntHeight = video::viewport_py() < 240 ? 240 : video::viewport_py();
        return ntHeight - splitPixelRow;
    }

    static u16 PreviewScrollYFor(const u8 splitRow) {
        return PreviewScrollYForPixels(static_cast<u16>(splitRow) << 3);
    }

    static u16 PreviewScrollY() {
        return PreviewScrollYFor(SplitRow());
    }

    constexpr u8 kSplitDelay = REGION ? 90 : 0;
    static void ApplySplit();

    constexpr u8 kSplitLatency = REGION ? 4 : 3;

    // Same reload-from-row math ::ArmSplitIRQ uses, parametrized so
    // ::nmi_handler_lockAnim can re-arm against its own shrinking split row
    // instead of the title menu's fixed ::SplitRow().
    static void ArmSplitIRQForPixels(const u16 splitPixelRow) {
        const u8 splitReload = splitPixelRow > kSplitLatency
            ? static_cast<u8>(splitPixelRow - kSplitLatency) : 0;
        mmc3::ScheduleScanlineIRQ(splitReload, {0, splitPixelRow});
    }

    static void ArmSplitIRQFor(const u8 splitRow) {
        ArmSplitIRQForPixels(static_cast<u16>(splitRow) << 3);
    }

    // Same row->absolute-Y math ::ApplySplit uses, parametrized for the same
    // reason as ::ArmSplitIRQFor above.
    static u16 SplitYForPixels(const u16 splitPixelRow) {
        return video::viewport_ty() < 30
            ? static_cast<u16>(PreviewScrollYForPixels(splitPixelRow) + splitPixelRow)
            : splitPixelRow;
    }

    static u16 SplitYFor(const u8 splitRow) {
        return SplitYForPixels(static_cast<u16>(splitRow) << 3);
    }


    TITLE NI void main() {
        kMenuNT = video::viewport_tx() < 32 ? 32 : video::viewport_tx();

        oam::PopulateFromProvider(OAMBuffer, 0, oam::y, Clear, 64);
        pIRQ = irq_handler;
        pNMI = nmi_handler;

        mmc3::SwitchCHRBank(mmc3::chr0Control, 4);
        mmc3::SwitchCHRBank(mmc3::chr1Control, 5);
        mmc3::SwitchCHRBank(mmc3::chr2Control, 0);
        mmc3::SwitchCHRBank(mmc3::chr3Control, 1);
        mmc3::SwitchCHRBank(mmc3::chr4Control, 6);
        mmc3::SwitchCHRBank(mmc3::chr5Control, 7);
        // MUST be set explicitly, same as EnterLevelSetup's (level.cpp): $A000
        // is one of the MMC3 registers power-on leaves undefined (see
        // mmc3.hpp's own comment on why the mapper's ::_reset doesn't seed
        // it), and title::main is the FIRST code RESET reaches (gameMode
        // starts at Title). With no explicit write here, every nametable/
        // attribute write below (::ppu::Flush onward) races an undefined
        // mirroring arrangement -- fine on hardware/emulators that happen to
        // power up at 0, corrupted on any that don't (e.g. Mesen's
        // "Randomize power-on state").
        mmc3::SetMirroring(false);
        ppu::Flush(chrHUDWhitespace_tile, 0xff);
        ppu::pal::WriteFromBuffer(13, titleScreenColours, 3);

        DrawLevelPreview();

        // Preloaded once, here, with the rest of the UI -- not drawn later
        // when Options is actually picked. demo/ui/options.uis now targets
        // $2800, which under the vertical mirroring just set up is the SAME
        // physical page as the preview's own nametable -- the rows the
        // preview scrolls past rather than a separate nametable, so this has
        // to run AFTER DrawLevelPreview (above), not before, or the
        // preview's own column writes would overwrite this text right back
        // out.
        gen::options::Draw_optionsTitle();
        gen::options::Draw_videoOptions();
        gen::options::Draw_reduceFlashesText();
        gen::options::Draw_superMarySistersText();
#if TARGET_VARIADIC_DISPLAY
        // Make_ resolves this button's anchor against the viewport THIS run
        // has -- on NES/GBA/PSP the position is a `constexpr` instead (see
        // ::TARGET_VARIADIC_DISPLAY), so there's no Make_ to call there.
        gen::options::Make_reduceFlashesButton();
#endif
        gen::options::Draw_reduceFlashesButton();
#if TARGET_GC
        // TARGET_GC is always inside the OGC (variadic) family, so Make_ is
        // unconditional here -- no fixed-panel build ever sets TARGET_GC.
        gen::options::Make_enableWidescreenButton();
        gen::options::Draw_enableWidescreenButton();
#endif

        InitTitleScreen();

        // Make_ zeroes each menu's running option and, on a variadic target,
        // resolves every option's anchor against the viewport THIS run has --
        // so it has to run here rather than at static-init time.
        gen::title::Make_TitleOptions();
        gen::gameOptions::Make_gameOptions();
        playModeActive = false;

        gen::title::Draw_TitleOptions();
        QueueSelectorDraw(TitleArrowAddr());

        ppu::SetScroll({0, 0xff});
        ppu::EnableRendering(ppu::ctrl::SPRITE_ADDR | ppu::ctrl::SPRITE_SIZE | ppu::ctrl::GEN_NMI, ppu::mask::BG_L | ppu::mask::SPRITE_L);
        irq::EnableInterrupts();

        u8 prevInputs = 0;
        while (true) {
            u8 port1, port2;
            input::PollControllers(&port1, &port2);
            const u8 inputs  = port1 | port2;
            const u8 pressed = inputs & static_cast<u8>(~prevInputs); // strobe: only the frame a button goes down
            prevInputs = inputs;

            if (!optionsLocked) {
                const u8 lastOption = ActiveOption();
                if (playModeActive) gen::gameOptions::Pass_gameOptions(pressed);
                else                gen::title::Pass_TitleOptions(pressed);
                if (ActiveOption() != lastOption) MoveSelector(ActiveArrowAddr());

                if (pressed & input::A) {
                    if (playModeActive) {
#ifdef PLAYER2_SUPPORTED
                        level::multiplayer = gen::gameOptions::gameOptions_option != 0;
#endif
                        ppu::PPUMASK = 0;
                        gameMode = eGameModes::Level;
                        return;
                    }

                    switch (gen::title::TitleOptions_option) {
                        case NewGame:
                        case Continue:
                            // State first, NMI handler last: the handler reads it.
                            playModeActive = true;
                            pNMI = nmi_handler_drawPlayMode;
                            break;

                        case Options:
                            // No Options screen built yet -- rather than a
                            // silent no-op, freeze input here for good: same
                            // state-first-NMI-last ordering as above.
                            optionsLocked = true;
                            pNMI = nmi_handler_lockOptions;
                            break;

#if defined(TARGET_MACOS) || defined(TARGET_WINDOWS) || defined(TARGET_LINUX)
                        case Quit:
                            quit = true;
                            return;
#endif

                        default: ;
                    }
                }

                if (pressed & input::B && playModeActive) {
                    playModeActive = false;
                    pNMI = nmi_handler_drawMenu;
                }
            } else if (pressed & input::B && pNMI == nmi_handler_locked) {
                // Only once the reveal has fully finished (pNMI is the
                // steady ::nmi_handler_locked, not still mid-reveal).
                pNMI = nmi_handler_lockAnimReverse;
            }

            video::WaitForPresent();
            if (quit) return;
        }
    }

    static void ArmSplitIRQ() {
        ArmSplitIRQFor(SplitRow());
    }

    void nmi_handler() {
        oam::RefreshSprites(OAMBuffer);
        SelectorUpdate();

        ppu::SetScroll({0, PreviewScrollY()});

        ArmSplitIRQ();
    }

    // Both swap handlers: wipe the old arrow first (its tile can sit inside
    // the incoming menu's text, so clearing it after the redraw would punch a
    // hole in it), swap the two menus' labels, then draw the arrow fresh --
    // QueueSelectorDraw's clear-then-arrow onto one tile.
    static void nmi_handler_drawPlayMode() {
        ppu::WriteSingleToNameTable(CurrentSelectorAddr(), chrHUDWhitespace_tile);
        gen::title::Erase_TitleOptions();
        gen::gameOptions::Draw_gameOptions();
        QueueSelectorDraw(PlayModeArrowAddr());
        SelectorUpdate();
        ppu::SetScroll({0, PreviewScrollY()});
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    static void nmi_handler_drawMenu() {
        ppu::WriteSingleToNameTable(CurrentSelectorAddr(), chrHUDWhitespace_tile);
        gen::gameOptions::Erase_gameOptions();
        gen::title::Draw_TitleOptions();
        QueueSelectorDraw(TitleArrowAddr());
        SelectorUpdate();
        ppu::SetScroll({0, PreviewScrollY()});
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    // Current split position through the reveal animation below, in
    // SCANLINES (not tile rows like ::SplitRow() -- stepping this one
    // One-shot: entered once Options is picked (see ::main). Clears the
    // title menu's arrow/text (nametable B, $2C00+) -- nothing is drawn in
    // its place, this deliberately has no Options MENU built yet, just the
    // screen behind it -- and disables the title menu's own scanline split
    // for good: demo/ui/options.uis now lives at $2800, the SAME physical
    // page as the preview's own nametable (nametable A) under the vertical
    // mirroring ::main sets up, so revealing it is just a matter of
    // continuing nametable A's own Y-scroll further -- no X-scroll switch,
    // no split, no IRQ at all. Hands off to the reveal animation below.
    static void nmi_handler_lockOptions() {
        ppu::WriteSingleToNameTable(CurrentSelectorAddr(), chrHUDWhitespace_tile);
        gen::title::Erase_TitleOptions();
        mmc3::AcknowledgeScanlineIRQ();
        oam::RefreshSprites(OAMBuffer);

        lockAnimScanline = static_cast<u16>(SplitRow()) << 3;
        ppu::SetScroll({0, PreviewScrollY()});

        pNMI = nmi_handler_lockAnim;
    }

    // The reveal itself: every frame, the Y scroll bumps one scanline
    // further into the preview's own nametable -- the exact same Y-wrap
    // ::nmi_handler already relies on every frame, just walked past its
    // usual resting point (::SplitRow()) instead of holding there. Runs all
    // the way down to 0, i.e. ::PreviewScrollYForPixels(0) == one full
    // nametable height (240): the point where ::ppu::SetScroll's own wrap
    // case lands on the nametable BELOW the preview's -- a real, distinct
    // page (this board links four-screen VRAM, ALTERNATIVE_NAMETABLE==1:
    // see mmc3.cpp), which is where the preloaded Options text actually
    // lives. Hands off to ::nmi_handler_locked, which just holds the scroll
    // there.
    static void nmi_handler_lockAnim() {
        oam::RefreshSprites(OAMBuffer);

        if (lockAnimScanline != 0) --lockAnimScanline;

        ppu::SetScroll({0, PreviewScrollYForPixels(lockAnimScanline)});
        UpdateMarySpriteYForPixels(lockAnimScanline);

        if (lockAnimScanline == 0) pNMI = nmi_handler_locked;
    }

    // Steady state once the reveal finishes: nothing left to scroll, Y is
    // just pinned at the nametable-below boundary (::PreviewScrollYForPixels(0),
    // same value ::nmi_handler_lockAnim's last step already set -- the
    // preview's own ::UpdateMarySpriteYForPixels(0) already hid her once her
    // feet scrolled past the top, same as it would on any other frame -- no
    // special-casing needed here). Sprites still refresh every frame so OAM
    // doesn't go stale.
    static void nmi_handler_locked() {
        oam::RefreshSprites(OAMBuffer);
        ppu::SetScroll({0, PreviewScrollYForPixels(0)});
    }

    // Entered on B, but only once ::main sees pNMI == ::nmi_handler_locked
    // (the reveal has actually finished, not still mid-animation) -- mirrors
    // ::nmi_handler_lockAnim exactly, just counting ::lockAnimScanline back
    // UP from 0 instead of down, so the preview/Options boundary retraces
    // the same scanline-at-a-time path backward. ::lockAnimScanline is
    // already 0 (that's how we got to ::nmi_handler_locked in the first
    // place), so there's no separate one-shot entry step the way
    // ::nmi_handler_lockOptions is for the forward direction -- ::main just
    // drops straight into this.
    static void nmi_handler_lockAnimReverse() {
        oam::RefreshSprites(OAMBuffer);

        const u16 target = static_cast<u16>(SplitRow()) << 3;
        if (lockAnimScanline < target) ++lockAnimScanline;

        if (lockAnimScanline >= target) {
            // Back to the title menu's own steady split: redraw the labels
            // the arrow this time (QueueSelectorDraw's clear-then-arrow onto
            // one tile is the SAME "first-draw indicator" trick ::main's own
            // initial arrow draw uses -- nothing to clear first, there's no
            // stale arrow left anywhere on screen to punch a hole in) and
            // re-arm the split ::nmi_handler relies on every frame.
            gen::title::Draw_TitleOptions();
            QueueSelectorDraw(TitleArrowAddr());
            SelectorUpdate();
            ppu::SetScroll({0, PreviewScrollY()});
            ArmSplitIRQ();

            optionsLocked = false;
            pNMI = nmi_handler;
            return;
        }

        ppu::SetScroll({0, PreviewScrollYForPixels(lockAnimScanline)});
        UpdateMarySpriteYForPixels(lockAnimScanline);
    }

    void irq_handler() {
        mmc3::AcknowledgeScanlineIRQ();
        tech::SpinWait(kSplitDelay);
        ApplySplit();
    }

    static void ApplySplit() {
        // The NES/PC/GX/3DS ports ignore this Y write (a plain mid-frame Y
        // write is a no-op on real hardware) and the Y counter simply carries
        // on from the frame's scroll to world row 240, which wraps onto the
        // menu nametable's row 0. The DS/GBA backends honour the handler's Y
        // (their gameplay follow camera needs that -- see ApplyHudSplit,
        // level.cpp), so here it has to name that same row explicitly: 240,
        // not the split row. See ::SplitYFor for the shared math.
        ppu::SetScroll({static_cast<u16>(kMenuNT << 3), SplitYFor(SplitRow())});
    }

    void InitTitleScreen() {
        // Generated by uitk from demo/ui/title.uis (see demo/gen/title/) --
        // this target's own build-selected variant, reached via the
        // angle-bracket <title.hpp> above. Every target's GameTitle draw is
        // now sourced from this one scene, not hand-duplicated here.
        gen::title::Draw_GameTitle();
    }

    static oam::oam_t Clear(const u16) {
        return 0xf0;
    }

    static NI void DrawLevelPreview() {
        using namespace level;

        if (const bool loaded = mmc3::CallInBlock<level_code_tag>([] { return LoadLevel(0); }); !loaded) return;

        // PopulateNameTableColumns always draws at row 2 (kHudRows*2) --
        // see its own comment -- so this mirrors that placement for the
        // feet-Y math below instead of hardcoding a second copy of it.
        constexpr u16 tyBase = kHudRows * 2;

        CallLevelGraphics([] {
            ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - sizeof(msg_mary)), 0}, SIZED_OBJ(msg_mary), 0);
        });
        constexpr u8 coinUI[] = {chrHUDCoin_tile, chrFont_tile + 0, chrFont_tile + 0};
        ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - sizeof(coinUI)), 1}, SIZED_OBJ(coinUI), 0);

        const u16 colsWide  = nColumns < viewport_mx() ? nColumns : viewport_mx();
        const u16 blockCols = colsWide & ~static_cast<u16>(1);

        const u16 previewTileCols = static_cast<u16>(blockCols * 2);
        mmc3::CallInBlock<level_code_tag>([previewTileCols] {
            PopulateNameTableColumns(previewTileCols);
        });

        CallLevelGraphics([] {
            oam::PopulateFromBuffer(OAMBuffer, 0, oam::tile,       msMary, kMarySprites);
            oam::PopulateFromBuffer(OAMBuffer, 0, oam::attributes, msMary, kMarySprites);
            ppu::pal::WriteFromBuffer(ppu::SPRITE_0 + 1, SIZED_OBJ(maryColors));
            ppu::pal::WriteFromBuffer(0, BGColours, 4);
            ppu::pal::WriteFromBuffer(5, BGColours + 5, 3);
        });

        maryGroundNtRow = tyBase * 8 + (levelHeight - 2) * 16;
        OAMBuffer[0].x = 32; OAMBuffer[1].x = 40;
        UpdateMarySpriteY(SplitRow());
    }

    // Re-derives Mary's screen Y from ::maryGroundNtRow and whatever split
    // position is live right now -- the scroll-dependent half of the math
    // DrawLevelPreview used to do once, now shared with
    // ::nmi_handler_lockAnim (via the Pixels form, for the same one-
    // scanline-at-a-time reason as ::PreviewScrollYForPixels) so her feet
    // track the band's rising split instead of staying nailed to its
    // original position.
    //
    // Once the band scrolls far enough that her feet would go above row 0,
    // she's hidden outright rather than clamped to row 0: ::maryGroundNtRow
    // is wherever she stands within the ORIGINAL preview band, which is
    // nowhere near the full scanline range the reveal animation walks
    // through -- clamping instead of hiding left her pinned at the top edge
    // for the back half of the animation while the background kept
    // scrolling out from under her, well past the point her own ground had
    // scrolled off-screen.
    static void UpdateMarySpriteYForPixels(const u16 splitPixelRow) {
        // On the NES/PC/etc. the sprite Y is plain screen space, so the band's
        // scroll is subtracted here. The cropping backends (DS/DSi/GBA) instead
        // subtract the frame's scroll from EVERY sprite themselves (their
        // window transform -- see build_sprites in src/nds/video.cpp), so
        // subtracting it here as well counted it twice and put Mary at the top
        // of the screen; those targets want the un-scrolled Y.
        const u16 spriteScroll = video::viewport_ty() < 30 ? 0 : PreviewScrollYForPixels(splitPixelRow);
        const i16 rawFeetY    = static_cast<i16>(maryGroundNtRow) - 16 - static_cast<i16>(spriteScroll);
        const auto feetY      = rawFeetY < 0 ? static_cast<oam::oam_t>(0xf0) : static_cast<oam::oam_t>(rawFeetY);
        OAMBuffer[0].y = feetY; OAMBuffer[1].y = feetY;
    }

    static void UpdateMarySpriteY(const u8 splitRow) {
        UpdateMarySpriteYForPixels(static_cast<u16>(splitRow) << 3);
    }

    AI auto SelectorUpdate() -> void {
        if (const u8 updateFlag = scratchpad[0]; !updateFlag) return;

        const u16 clearAddr  = scratchpad[1] | (scratchpad[2] << 8);
        const u16 arrowAddr  = scratchpad[3] | (scratchpad[4] << 8);
        ppu::WriteSingleToNameTable(clearAddr, chrHUDWhitespace_tile);
        ppu::WriteSingleToNameTable(arrowAddr, chrArrow_tile);
        scratchpad[0] = 0;  // clear update flag, update is done
    }
}