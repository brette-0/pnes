#include "title.hpp"

#ifndef GEN_TARGET_DIR
#error "GEN_TARGET_DIR is not set -- add it to this target's CMakeLists.txt branch."
#endif
#include STRCAT(../../gen/title/GEN_TARGET_DIR/title.hpp)
#include STRCAT(../../gen/gameOptions/GEN_TARGET_DIR/gameOptions.hpp)
#include "../../ui/impl/buttons.hpp"
#include "../main.hpp"
#include "pnes/logger.hpp"
#include "../banks.hpp"
#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../../chr/chr.h"
#include "../graphics/metasprites.hpp"
#include "level.hpp"
#include "level/levels.hpp"
#include "pnes/mappers/mmc3.hpp"
#include "pnes/extras/ui/text.hpp"

namespace title {
    static_assert(gen::title::TitleOptions_nOptions == End + 1,
                  "title.uis and title.hpp's titleOptions enum disagree on the menu's options");

    static atomic bool playModeActive = false;
    static atomic bool optionsLocked = false;

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

    static void QueueSelectorDraw(const u16 addr) {
        scratchpad[1] = scratchpad[3] = static_cast<u8>(addr & 0xff);
        scratchpad[2] = scratchpad[4] = static_cast<u8>(addr >> 8);
        scratchpad[0] = 1;
    }


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
    static void BuildPaletteFadePhases();
    static void nmi_handler_fadeToOptions();
    static void nmi_handler_lockOptions();
    static void nmi_handler_lockAnim();
    static void nmi_handler_locked();
    static void nmi_handler_lockAnimReverse();
    static void nmi_handler_fadeFromOptions();

    static u16 kMenuNT;

    static u16 maryGroundNtRow;

    static u16 lockAnimScanline;

    // How many scanlines ::nmi_handler_lockAnim/::nmi_handler_lockAnimReverse
    // walk the scroll split by, per frame.
    constexpr u16 kScanlinesPerFrame = 2;

    // Where each phase below writes back to, and how long it is. BG_3 (the
    // HUD's palette, ::titleScreenColours) is deliberately absent -- the fade
    // must leave the HUD alone.
    struct FadeRun { u8 offset; u8 count; };
    constexpr FadeRun kFadeEntries[] = {
        {ppu::BG_0,         4}, // BG_0 sub-palette, incl. the shared backdrop
        {ppu::BG_1 + 1,     3}, // BG_1 colours 1-3
        {ppu::SPRITE_0 + 1, 3}, // Mary's sprite colours
    };

    constexpr u8 kFadePhaseCount = 5;

    static u8 paletteFadePhases[kFadePhaseCount][4 + 3 + 3];

    // Current index into ::paletteFadePhases: 0 is full colour, ::kFadePhaseCount - 1
    // is fully black. ::nmi_handler_fadeToOptions counts this up,
    // ::nmi_handler_fadeFromOptions counts it back down.
    static u8 fadePhase;

    // Each phase holds for this many frames before the next one -- advancing
    // every vblank reads as a near-instant snap (only ::kFadePhaseCount - 1
    // steps total), not a fade.
    constexpr u8 kFadeStepFrames = 4;

    // Counts frames within the current phase; wraps back to 0 (and advances
    // ::fadePhase) in ::nmi_handler_fadeToOptions/::nmi_handler_fadeFromOptions.
    static u8 fadeFrameCounter;

    // Rows the menu band (everything below the split) is given. The NES lays
    // it out as 30 - 24 = 6 rows, which is what the title/menu text (nametable
    // rows 1-4) is sized for.
    constexpr u8 kMenuBandRows = 6;

    // True on a target whose viewport spans two stacked panels (DS/DSi/3DS --
    // video::panel_count(), video.hpp). There the whole UI is authored straight
    // into nametable 0's lower-panel rows by demo/ui/<scene>_ds.uis, so it is
    // permanently on the bottom screen. That removes the two mechanisms the
    // single-screen layout needs and nothing else:
    //
    //   - the raster split. On one screen the menu lives in a DIFFERENT
    //     nametable and the preview is scrolled so the split line swaps to it
    //     mid-frame (::ApplySplit). With the UI already on its own panel in
    //     nametable 0 there is nothing to swap to: scroll stays {0,0} and the
    //     top panel shows the preview while the bottom shows the UI.
    //   - the palette fade. It exists to cover that same single-screen
    //     transition -- the preview going dark as the options menu takes the
    //     screen. The UI never takes the screen here, so there is nothing to
    //     fade out from under.
    //
    // Everything else (the selector, the menu swap, the lock state machine)
    // is shared verbatim, so this stays a bypass of two mechanisms rather than
    // a second copy of the title screen.
    constexpr bool kUiOnOwnPanel = video::panel_count() > 1;

    // ...but the split is not gone, only repurposed. The top panel draws the
    // level preview's CHR bank and the bottom panel the UI's, and MMC3 maps
    // both into the SAME 512-tile window -- so the bank still has to change
    // between them, and the only place to do that is a scanline IRQ at the
    // panel boundary. ::ApplySplit then switches the bank and nothing else:
    // each panel's scroll already comes from its own band (the backend keeps a
    // per-panel scroll table), so a mid-frame ppu::SetScroll would fight it.
    //
    // Gated to the DS for now rather than every dual-screen target: honouring a
    // mid-frame bank change needs the backend to support it. src/nds/video.cpp
    // does, without restriction -- it mirrors TILES rather than banks, keying a
    // 1024-entry pool on each tile's resolved CHR address, so a switch of any
    // kind at any scanline (including partway down a tile row, which it
    // synthesises a composite tile for) is reproduced. The 3DS backend still
    // bakes a single atlas set per frame and does not draw its bottom panel at
    // all, so arming there would switch a bank its bake had already consumed --
    // thrashing eight palette atlases every frame for nothing visible. It joins
    // this when its panel-1 rendering lands.
#if defined(TARGET_NDS)
    constexpr bool kUiBankSplit = kUiOnOwnPanel;
#else
    constexpr bool kUiBankSplit = false;
#endif

    static u8 SplitRow() {
        // A cropping target's panel (DS/DSi 24 rows, GBA 20) is shorter than
        // the NES's 30, but the menu still needs the same 6 rows -- scaling
        // the split with the panel height instead gave the DS an 8-row band,
        // i.e. ~4 empty rows under the UI.
        //
        // This asks ::video::panel_ty(0) -- the PRIMARY PANEL's height -- not
        // viewport_ty(). On the dual-screen handhelds the viewport is now the
        // union of both panels (DS 48 rows, 3DS 60; see their viewport_ty() doc
        // comments, video.hpp), and this menu is a single-screen UI: it has to
        // land on the top panel, so it is the top panel's 24/30 rows that decide
        // the split, exactly as the DS's old 24-row viewport used to.
        if (video::panel_ty(0) < 30) return static_cast<u8>(video::panel_ty(0) - kMenuBandRows);
        return (((viewport_my() + 1) >> 1) - 2) << 2;
    }

    static u16 PreviewScrollYForPixels(const u16 splitPixelRow) {
        // Bottom-anchor the level plane against the TOP panel. The preview is
        // the NES-native 30 rows/240px (2 HUD rows + PopulateNameTableColumns'
        // 28 from row 2, levels.cpp), which is TALLER than the DS's 192px top
        // panel -- so the top of it is deliberately cropped and the scroll is
        // whatever puts the plane's last row on the panel's last row:
        //
        //     scroll = 240 - panel 0 height    (DS 48px/6 rows, 3DS 0)
        //
        // NOT 0. Scrolling to 0 tried to fit the whole 240px plane on a 192px
        // panel and dropped the bottom 48px of the level off the screen, which
        // is the wrong half to lose. 3DS's top panel is already 240px, so it
        // lands on 0 by arithmetic rather than by special case.
        //
        // The scroll is why a *_ds.uis UI anchors its rows at plane row 30 and
        // not at panel 1's own first row: with the plane bottom-anchored, row
        // 30 -- the first row below the level data -- is the first row the
        // LOWER panel shows, on both consoles.
        if constexpr (kUiOnOwnPanel) {
            (void)splitPixelRow;
            return static_cast<u16>(240 - video::panel_rect(0).h);
        }
        // NOT a panel height: this is the NAMETABLE PLANE height, the modulus
        // the preview's Y scroll wraps against. It must stay byte-identical to
        // ::emu::ComputeNtGeometry's own tileH floor (`viewport_ty() < 30 ? 30
        // : viewport_ty()`, src/emu/emu.hpp) and to the quadrant offset uitk
        // bakes into the generated scene code (emitExprCpp, tools/uitk/main.cpp)
        // -- the menu lives in nametable quadrant 1, so if this disagrees with
        // where that quadrant actually starts, the scroll lands the preview on
        // the wrong rows. `viewport_py() < 240 ? 240 : viewport_py()` IS that
        // floor expressed in pixels, since viewport_py() == viewport_ty() << 3,
        // so it tracks the dual-screen heights (DS 384, 3DS 480) for free.
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
        // Where the split goes depends on what it is FOR:
        //  - ::kUiBankSplit: the panel boundary, always. Its only job is the
        //    CHR bank change between the two panels, so the caller's own
        //    menu-band geometry is irrelevant.
        //  - ::kUiOnOwnPanel without it: nothing to do, so never armed.
        //  - otherwise: the caller's row, the original single-screen split.
        if constexpr (kUiOnOwnPanel && !kUiBankSplit) {
            (void)splitPixelRow;
            return;
        }
        const u16 row = kUiBankSplit ? static_cast<u16>(video::panel_rect(0).h)
                                     : splitPixelRow;
        const u8 splitReload = row > kSplitLatency
            ? static_cast<u8>(row - kSplitLatency) : 0;
        mmc3::ScheduleScanlineIRQ(splitReload, {0, row});
    }

    static void ArmSplitIRQFor(const u8 splitRow) {
        ArmSplitIRQForPixels(static_cast<u16>(splitRow) << 3);
    }

    // Same row->absolute-Y math ::ApplySplit uses, parametrized for the same
    // reason as ::ArmSplitIRQFor above.
    static u16 SplitYForPixels(const u16 splitPixelRow) {
        return video::panel_ty(0) < 30
            ? static_cast<u16>(PreviewScrollYForPixels(splitPixelRow) + splitPixelRow)
            : splitPixelRow;
    }

    static u16 SplitYFor(const u8 splitRow) {
        return SplitYForPixels(static_cast<u16>(splitRow) << 3);
    }

    // Draws the options panel's own content. Called once at startup and, on a
    // dual-screen target, again on every entry into options mode: there the
    // panel and the title menu are ALTERNATIVE content for the same
    // bottom-panel rows (options_ds.uis and title_ds.uis both anchor at plane
    // row 30), exactly as the title and gameOptions menus already are. On one
    // screen the panel lived in its own nametable and the title menu could
    // never overwrite it, so drawing it once was enough.
    static void DrawOptionsPanel() {
        gen::options::Draw_optionsTitle();
        gen::options::Draw_videoOptions();
        gen::options::Draw_reduceFlashesText();
        gen::options::Draw_superMarySistersText();
        gen::options::Draw_reduceFlashesButton();
    }

    // Blanks the WHOLE bottom-panel UI band -- plane row 30 (where every
    // *_ds.uis scene anchors) to the end of the plane. Dual-screen only.
    //
    // The band holds exactly one page at a time: the title UI, or the options
    // page, never both. Whichever is coming in clears the band and draws
    // itself, so neither has to know what the other occupied. That is also why
    // the options page is not drawn at startup on these targets at all -- it
    // stays unloaded until OPTIONS is actually picked. It is the only workable
    // arrangement here: the pages overlap (options_ds spans rows 31-47, the
    // title UI 31-34) and the generated options scene has no Erase_
    // counterparts, since none of its nodes set provideErasing.
    static constexpr u16 kUiBandRow0 = 30;
    static void ClearUiBand() {
        for (u16 r = kUiBandRow0; r < video::viewport_ty(); r++)
            ppu::WriteRepeatedToNameTable({0, r}, blob::ui::hud_whitespace,
                                          static_cast<u8>(kMenuNT), 0);
    }

    static void BankPreviewTiles() {
        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::staticLevelTiles)>();
    }

    static void BankMenuTiles() {
        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::ui)>();
    }


    TITLE NI void main() {
        log("title::main: entered");
        kMenuNT = video::viewport_tx() < 32 ? 32 : video::viewport_tx();

        oam::PopulateFromProvider(OAMBuffer, 0, oam::y, Clear, 64);
        pIRQ = irq_handler;
        pNMI = nmi_handler;
        log("title::main: pIRQ/pNMI set");

        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::dynamicLevelTiles)>();
        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::staticLevelTiles)>();
        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::ui)>();
        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::normalSprites)>();

        constexpr u32 padLma = blob::normalSprites::__lma + blob::normalSprites::__size;
        static_assert(padLma + 2 * mmc3::kChrBankSize <= blob::dynamicLevelTiles::__lma,
                      "R5/R1 run past normalSprites into the next entry: give "
                      "PPU $0C00-$0FFF and $1800-$1FFF entries of their own, "
                      "or re-point them");
        static_assert(mmc3::BankOf<padLma>() % 2 == 0,
                      "the padding gap must start 2 KiB-aligned: R1 is a 2 KiB "
                      "window and ignores an odd bank's low bit");
        mmc3::SwitchCHRBank(mmc3::chr5Control, mmc3::BankOf<padLma>());
        mmc3::SwitchCHRBank(mmc3::chr1Control, mmc3::BankOf<padLma>());
        mmc3::SetMirroring(false);
        ppu::Flush(blob::ui::hud_whitespace, 0xff);
        ppu::pal::WriteFromBuffer(13, titleScreenColours, 3);

        log("title::main: before DrawLevelPreview");
        DrawLevelPreview();
        log("title::main: after DrawLevelPreview");

        BuildPaletteFadePhases();

#if TARGET_VARIADIC_DISPLAY
        // Make_ resolves this button's anchor against the viewport THIS run
        // has -- on NES/GBA/PSP the position is a `constexpr` instead (see
        // ::TARGET_VARIADIC_DISPLAY), so there's no Make_ to call there. It
        // stays here, not in ::DrawOptionsPanel: it resolves anchors and zeroes
        // running state, which must happen once, not on every mode entry.
        gen::options::Make_reduceFlashesButton();
#endif
        // Dual-screen: left unloaded until OPTIONS is picked (::ClearUiBand).
        if constexpr (!kUiOnOwnPanel) DrawOptionsPanel();
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
        log("title::main: entering input loop");

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
                            playModeActive = true;
                            pNMI = nmi_handler_drawPlayMode;
                            break;

                        case Options:
                            fadePhase = 0;
                            fadeFrameCounter = 0;
                            optionsLocked = true;
                            pNMI = nmi_handler_fadeToOptions;
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
                pNMI = nmi_handler_lockAnimReverse;
            }

            video::WaitForPresent();
            {
                static bool firstPresentLogged = false;
                if (!firstPresentLogged) {
                    firstPresentLogged = true;
                    log("title::main: first WaitForPresent returned");
                }
            }
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
        BankPreviewTiles();

        ArmSplitIRQ();
    }

    // Both swap handlers: wipe the old arrow first (its tile can sit inside
    // the incoming menu's text, so clearing it after the redraw would punch a
    // hole in it), swap the two menus' labels, then draw the arrow fresh --
    // QueueSelectorDraw's clear-then-arrow onto one tile.
    static void nmi_handler_drawPlayMode() {
        ppu::WriteSingleToNameTable(CurrentSelectorAddr(), blob::ui::hud_whitespace);
        gen::title::Erase_TitleOptions();
        gen::gameOptions::Draw_gameOptions();
        QueueSelectorDraw(PlayModeArrowAddr());
        SelectorUpdate();
        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    static void nmi_handler_drawMenu() {
        ppu::WriteSingleToNameTable(CurrentSelectorAddr(), blob::ui::hud_whitespace);
        gen::gameOptions::Erase_gameOptions();
        gen::title::Draw_TitleOptions();
        QueueSelectorDraw(TitleArrowAddr());
        SelectorUpdate();
        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    static u8 FadeStep(const u8 colour) {
        if ((colour & 0xf0) == 0) return 0x1f;
        return static_cast<u8>(colour - 0x10);
    }

    // Fills ::paletteFadePhases: phase 0 is the title screen's live, non-HUD
    // palette (read from the same ROM arrays DrawLevelPreview uploads it
    // from -- BGColours/maryColors live in the LEVEL_GRAPHICS bank, same
    // reason DrawLevelPreview itself reads them inside CallLevelGraphics),
    // every later phase is the previous one run through ::FadeStep once.
    static void BuildPaletteFadePhases() {
        CallLevelGraphics([] {
            u8* dst = paletteFadePhases[0];
            *dst++ = BGColours[0]; *dst++ = BGColours[1]; *dst++ = BGColours[2]; *dst++ = BGColours[3];
            *dst++ = BGColours[5]; *dst++ = BGColours[6]; *dst++ = BGColours[7];
            *dst++ = maryColors[0]; *dst++ = maryColors[1]; *dst++ = maryColors[2];
        });
        for (u8 phase = 1; phase < kFadePhaseCount; ++phase) {
            for (u8 i = 0; i < sizeof(paletteFadePhases[0]); ++i) {
                paletteFadePhases[phase][i] = FadeStep(paletteFadePhases[phase - 1][i]);
            }
        }
    }

    // Writes one precomputed phase out via ::ppu::pal::WriteFromBuffer (plain
    // RAM source -- no LEVEL_GRAPHICS switch needed here, unlike
    // ::BuildPaletteFadePhases).
    static void WritePaletteFadePhase(const u8 phase) {
        const u8* src = paletteFadePhases[phase];
        for (const FadeRun& run : kFadeEntries) {
            ppu::pal::WriteFromBuffer(run.offset, src, run.count);
            src += run.count;
        }
    }

    // Steps ::fadePhase one phase darker and writes it out. Returns false
    // once ::fadePhase has reached the fully-black last phase.
    static bool StepPaletteFade() {
        if (fadePhase >= kFadePhaseCount - 1) return false;
        WritePaletteFadePhase(++fadePhase);
        return fadePhase < kFadePhaseCount - 1;
    }

    // ::StepPaletteFade's mirror: steps ::fadePhase one phase brighter
    // (toward phase 0, the original colours) and writes it out. Returns
    // false once ::fadePhase is back at 0.
    static bool StepPaletteFadeIn() {
        if (fadePhase == 0) return false;
        WritePaletteFadePhase(--fadePhase);
        return fadePhase != 0;
    }

    static void nmi_handler_fadeToOptions() {
        oam::RefreshSprites(OAMBuffer);
        mmc3::AcknowledgeScanlineIRQ();

        // ::kUiOnOwnPanel: no fade, and the palette is left untouched -- the
        // state machine advances on this very frame instead.
        bool stillFading = false;
        if constexpr (!kUiOnOwnPanel) {
            stillFading = true;
            if (++fadeFrameCounter >= kFadeStepFrames) {
                fadeFrameCounter = 0;
                stillFading = StepPaletteFade();
            }
        }

        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();

        // Armed, not merely acknowledged. mmc3::AcknowledgeScanlineIRQ is the
        // DISABLE half -- it clears a pending IRQ and leaves generation off --
        // so the ack at the top of this handler, with no ScheduleScanlineIRQ to
        // follow it, used to mean this frame simply had no split: one bank
        // setting for all 240 lines. That was survivable while $0400 held one
        // entry; now that staticLevelTiles and ui share the window it is the
        // visible break when OPTIONS is picked, and specifically a break in the
        // menu band, because ::kFadeEntries deliberately leaves BG_3 out of the
        // fade -- the band below the split stays fully lit the whole way down
        // while BankPreviewTiles has it drawing the preview's tiles.
        ArmSplitIRQ();

        if (!stillFading) pNMI = nmi_handler_lockOptions;
    }

    static void nmi_handler_lockOptions() {
        ppu::WriteSingleToNameTable(CurrentSelectorAddr(), blob::ui::hud_whitespace);
        gen::title::Erase_TitleOptions();
        mmc3::AcknowledgeScanlineIRQ();
        oam::RefreshSprites(OAMBuffer);

        // Swap the band over to the options page: clear whatever the title UI
        // left, then draw the page -- which has not been in the nametable at
        // all until now. See ::ClearUiBand.
        if constexpr (kUiOnOwnPanel) {
            ClearUiBand();
            DrawOptionsPanel();
        }

        lockAnimScanline = static_cast<u16>(SplitRow()) << 3;
        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();

        // Same reason as ::nmi_handler_fadeToOptions' arm, and it belongs here
        // at the end rather than beside the ack above: Erase_TitleOptions runs
        // a nametable upload this handler cannot afford to have a split land in
        // the middle of, which is what the ack is for. Arming once those writes
        // are done keeps the band below the split on ui tiles for this frame
        // without exposing them.
        ArmSplitIRQ();

        // ::kUiOnOwnPanel: there is no lock ANIMATION to run. It slides the
        // single-screen split band, and here the UI has its own panel, so
        // ::PreviewScrollYForPixels is a constant and nothing would move --
        // every frame of it is a dead frame, which is the delay entering and
        // leaving options. Skip to the locked state; the scroll/bank/arm this
        // handler just did is exactly what that state wants.
        if constexpr (kUiOnOwnPanel) {
            lockAnimScanline = 0;
            pNMI = nmi_handler_locked;
            return;
        }

        pNMI = nmi_handler_lockAnim;
    }

    static void nmi_handler_lockAnim() {
        oam::RefreshSprites(OAMBuffer);

        lockAnimScanline = lockAnimScanline > kScanlinesPerFrame
            ? static_cast<u16>(lockAnimScanline - kScanlinesPerFrame) : 0;

        // ui for the WHOLE frame, and no arm: the band above the split is
        // mid-scroll between the preview's rows and the options panel's (both
        // live in the same nametable -- SetMirroring(false) makes options.uis'
        // nametable 2 a mirror of the preview's 0), so one window cannot serve
        // both, and ::ApplySplit's scroll is written against the FIXED
        // ::SplitRow, which is not where this frame's split is. ui is the half
        // that matters: ::nmi_handler_fadeToOptions has already faded BG_0-BG_2
        // to black, so the preview rows render black whatever tiles they fetch,
        // while the options panel is drawn in the one palette the fade skips.
        ppu::SetScroll({0, PreviewScrollYForPixels(lockAnimScanline)});
        BankMenuTiles();
        UpdateMarySpriteYForPixels(lockAnimScanline);

        if (lockAnimScanline == 0) pNMI = nmi_handler_locked;
    }

    static void nmi_handler_locked() {
        oam::RefreshSprites(OAMBuffer);

        // ::kUiOnOwnPanel: the options panel is NOT the whole frame here. It
        // sits on the bottom panel (options_ds.uis anchors it at plane row 30),
        // and the level preview is still fully visible on the top one -- there
        // is no fade on this path to black it out. So the raster is still
        // required: preview bank for the top panel, and the split at the panel
        // boundary switches to ui for the bottom. Banking ui for the whole
        // frame -- correct on one screen, where the preview really is black --
        // is what drew the level area out of the UI bank.
        if constexpr (kUiOnOwnPanel) {
            ppu::SetScroll({0, PreviewScrollY()});
            BankPreviewTiles();
            ArmSplitIRQ();
            return;
        }

        ppu::SetScroll({0, PreviewScrollYForPixels(0)});

        // The options panel is the entire visible frame here, top to bottom,
        // and nothing re-arms the split -- so one bank setting is both correct
        // and all there is. See ::nmi_handler_lockAnim.
        BankMenuTiles();
    }

    static void nmi_handler_lockAnimReverse() {
        oam::RefreshSprites(OAMBuffer);

        const u16 target = static_cast<u16>(SplitRow()) << 3;
        const u16 next = static_cast<u16>(lockAnimScanline + kScanlinesPerFrame);
        lockAnimScanline = next < target ? next : target;
        // ::kUiOnOwnPanel: nothing to unwind, for ::nmi_handler_lockOptions'
        // reason -- take the completion branch below on the first frame so
        // leaving options is immediate.
        if constexpr (kUiOnOwnPanel) lockAnimScanline = target;

        if (lockAnimScanline >= target) {
            // Swap the band back: the options page goes away entirely and the
            // title UI is redrawn, title text included -- ::ClearUiBand takes
            // that with it, since it sits inside the band at rows 31-32.
            if constexpr (kUiOnOwnPanel) {
                ClearUiBand();
                gen::title::Draw_GameTitle();
            }
            gen::title::Draw_TitleOptions();
            QueueSelectorDraw(TitleArrowAddr());
            SelectorUpdate();
            ppu::SetScroll({0, PreviewScrollY()});
            BankPreviewTiles();
            ArmSplitIRQ();

            fadeFrameCounter = 0;
            pNMI = nmi_handler_fadeFromOptions;
            return;
        }

        // Unwinding ::nmi_handler_lockAnim, and ui for the same reason: the
        // fade has not been stepped back in yet (::nmi_handler_fadeFromOptions
        // does that, after the branch above hands over), so the preview rows
        // coming back into the top band are still black.
        ppu::SetScroll({0, PreviewScrollYForPixels(lockAnimScanline)});
        BankMenuTiles();
        UpdateMarySpriteYForPixels(lockAnimScanline);
    }

    static void nmi_handler_fadeFromOptions() {
        oam::RefreshSprites(OAMBuffer);
        SelectorUpdate();

        // ::kUiOnOwnPanel: no fade, and the palette is left untouched -- the
        // state machine advances on this very frame instead.
        bool stillFading = false;
        if constexpr (!kUiOnOwnPanel) {
            stillFading = true;
            if (++fadeFrameCounter >= kFadeStepFrames) {
                fadeFrameCounter = 0;
                stillFading = StepPaletteFadeIn();
            }
        }

        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();
        ArmSplitIRQ();

        if (!stillFading) {
            optionsLocked = false;
            pNMI = nmi_handler;
        }
    }

    void irq_handler() {
        mmc3::AcknowledgeScanlineIRQ();
        tech::SpinWait(kSplitDelay);
        ApplySplit();
    }

    static void ApplySplit() {
        if constexpr (kUiBankSplit) {
            // Bank only -- see ::kUiBankSplit. No scroll write: the backend's
            // per-panel scroll table already has each panel's own value, and
            // clobbering it here would move the preview as well.
            BankMenuTiles();
            return;
        }
        ppu::SetScroll({static_cast<u16>(kMenuNT << 3), SplitYFor(SplitRow())});

        // After the scroll write, not before: that one is the raster-critical
        // half of this handler, and two register writes ahead of it would push
        // it further into the visible line. Nothing below the split fetches
        // from $0400 until the next scanline starts, so the switch has until
        // then to land.
        BankMenuTiles();
    }

    void InitTitleScreen() {
        gen::title::Draw_GameTitle();
    }

    static oam::oam_t Clear(const u16) {
        return ParkedSpriteY();
    }

    static NI void DrawLevelPreview() {
        using namespace level;

        if (const bool loaded = mmc3::CallInBlock<level_code_tag>([] { return LoadLevel(0); }); !loaded) return;

        // PopulateNameTableColumns always draws at row 2 (kHudRows*2) --
        // see its own comment -- so this mirrors that placement for the
        // feet-Y math below instead of hardcoding a second copy of it.
        constexpr u16 tyBase = kHudRows * 2;

        CallLevelGraphics([] {
            ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - ::tech::nes_str::length(msg_mary)), 0}, SIZED_STR(msg_mary), 0);
        });
        constexpr u8 coinUI[] = {blob::ui::hud_coin, blob::ui::font + 0, blob::ui::font + 0};
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

    static void UpdateMarySpriteYForPixels(const u16 splitPixelRow) {
        // On a dual-screen target the preview really is scrolled (the
        // bottom-anchor above), so Mary's sprite has to be offset by it like
        // any other target's -- pinning this to 0 would float her 48px off the
        // ground on the DS. The 0 is only for a single-screen CROPPING panel
        // (GBA), where the preview is not scrolled at all.
        const u16 spriteScroll = (!kUiOnOwnPanel && video::panel_ty(0) < 30)
            ? 0
            : PreviewScrollYForPixels(splitPixelRow);
        const i16 rawFeetY    = static_cast<i16>(maryGroundNtRow) - 16 - static_cast<i16>(spriteScroll);
        const auto feetY      = rawFeetY < 0 ? ParkedSpriteY() : static_cast<oam::oam_t>(rawFeetY);
        OAMBuffer[0].y = feetY; OAMBuffer[1].y = feetY;
    }

    static void UpdateMarySpriteY(const u8 splitRow) {
        UpdateMarySpriteYForPixels(static_cast<u16>(splitRow) << 3);
    }

    AI auto SelectorUpdate() -> void {
        if (const u8 updateFlag = scratchpad[0]; !updateFlag) return;

        const u16 clearAddr  = scratchpad[1] | (scratchpad[2] << 8);
        const u16 arrowAddr  = scratchpad[3] | (scratchpad[4] << 8);
        ppu::WriteSingleToNameTable(clearAddr, blob::ui::hud_whitespace);
        ppu::WriteSingleToNameTable(arrowAddr, blob::ui::arrow);
        scratchpad[0] = 0;  // clear update flag, update is done
    }
}