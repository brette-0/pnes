#include "title.hpp"
#include "../main.hpp"
#include "../banks.hpp"
#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../graphics/graphics.hpp"
#include "../graphics/metasprites.hpp"
#include "level.hpp"
#include "level/levels.hpp"
#include "platform-nes/mappers/mmc3.hpp"
#include "platform-nes/extras/ui/singlechoice.hpp"
#include "platform-nes/extras/ui/text.hpp"

namespace title {
    constexpr u8 kMenuOptions   = static_cast<u8>(End + 1);
    constexpr u8 kMenuBoxWidth  = 8;

#if defined(TARGET_NES)
    constexpr u8 kPlayModeOptions  = 2;
    constexpr u8 kPlayModeBoxWidth = 12;   // "MULTIPLAYER"
#else
    constexpr u8 kPlayModeOptions  = 3;
    constexpr u8 kPlayModeBoxWidth = 17;   // "LOCAL MULTIPLAYER"
#endif
    static ui::choice::SingleChoice* pMenu = nullptr;
    static ui::choice::SingleChoice* pMainMenu = nullptr;
    static buffer<u8*>* pMenuChunks = nullptr;
    static u16 menuAddr;
    // Arrow-slot address per menu option -- SingleChoice no longer knows
    // where (or whether) its options are drawn, so title.cpp is the one
    // that lays the text boxes out and remembers where the arrow for each
    // option goes. Indexed the same way optionAddr used to be, by option.
    static u16 menuOptionAddr[kMenuOptions];

    static ui::choice::SingleChoice* pPlayMode = nullptr;
    static buffer<u8*>* pPlayModeChunks = nullptr;
    static vec2<u16> playModePos;
    static u16 playModeAddr;
    static u16 playModeOptionAddr[kPlayModeOptions];
    static u16 menuClearAddr;
    static u16 playModeClearAddr;

    // Arrow-slot addresses for whichever SingleChoice pMenu currently
    // points at -- tracked alongside pMenu itself, swapped in lockstep
    // whenever pMenu switches between the main menu and the play-mode
    // submenu.
    static const u16* pOptionAddr = nullptr;

    // Splits buff into nOptions single-row text boxes on optionSplitter,
    // stacking them downward from pos, and draws each one -- genuinely
    // separate text boxes, same word-wrap rule as ui::text::Make (a word
    // that doesn't fit box width is simply dropped, as every option here
    // is sized to fit its box in one line). Also fills optionAddr[opt]
    // (caller-owned, >= nOptions entries) with the nametable address one
    // tile left of that option's text -- where the caller draws its own
    // selection arrow, since this makes no draw call for it.
    //
    // Returns a heap-allocated array of nOptions buffer<u8*> entries --
    // same row-per-entry shape ui::text::Make returns -- caller owns it
    // (delete[] once done) and can hand it straight to ui::text::Draw.
    static buffer<u8*>* MakeOptionBoxes(
        const u8* buff, const u8 sBuff, const vec2<u16> pos, const u8 boxWidth,
        const u8 wordSplitter, const u8 optionSplitter,
        u16* const optionAddr, const u8 nOptions
    ) {
        const auto rows = new buffer<u8*>[nOptions];
        const u16 arrowCol = pos.x - 2;
        u8 cursor = 0;

        for (u8 opt = 0; opt < nOptions && cursor <= sBuff; opt++) {
            u8 end = cursor;
            while (end < sBuff && *(buff + end) != optionSplitter) end++;

            const auto row = ui::text::Make(buff + cursor, end - cursor, {boxWidth, 1}, wordSplitter);
            rows[opt] = row[0];
            delete[] row;

            optionAddr[opt] = ppu::CartesianToAddress({arrowCol, static_cast<u16>(pos.y + opt)});
            cursor = (end < sBuff) ? end + 1 : end;
        }

        return rows;
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

    static oam::oam_t Clear(u16 _);
    static NI void DrawLevelPreview();
    static void nmi_handler_drawPlayMode();
    static void nmi_handler_drawMenu();

    constexpr u16 kMenuNT = 32;
    constexpr u16 kBottomRightNT = 30;

    static u8 SplitRow() {
        return (((viewport_my() + 1) >> 1) - 2) << 2;
    }

    static u16 PreviewScrollY() {
        return video::viewport_py() - (static_cast<u16>(SplitRow()) << 3);
    }

    constexpr u8 kSplitDelay = REGION ? 90 : 0;
    static void ApplySplit();


    TITLE NI void main() {
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
        InitTitleScreen();

        const u16 menuCol = kMenuNT + (viewport_mx() << 1) - 1 - kMenuBoxWidth;
        ui::choice::SingleChoice menu(kMenuOptions, 0);
        const vec2<u16> menuPos{menuCol, static_cast<u16>(kBottomRightNT + 1)};
        const auto menuChunks = MakeOptionBoxes(
            SIZED_OBJ(msg_menu), menuPos, kMenuBoxWidth,
            chrHUDWhitespace_tile, 0, menuOptionAddr, kMenuOptions
        );
        ui::text::Draw(menuChunks, menuPos, vec2<u8>{kMenuBoxWidth, kMenuOptions}, ui::text::Left);
        QueueSelectorDraw(menuOptionAddr[menu.option]);

        // Free whatever a PREVIOUS visit to the title screen left behind --
        // Make()'s result is heap-allocated and caller-owned (see
        // singlechoice.hpp's own comment on Make()), and pMenuChunks is a
        // file-static that just gets silently overwritten on re-entry
        // otherwise, leaking a fresh buffer<u8*>[kMenuOptions] every single
        // time. Safe on the very first call too: pMenuChunks starts null,
        // and delete[] on a null pointer is a no-op.
        delete[] pMenuChunks;
        pMenuChunks = menuChunks;
        menuClearAddr = ppu::CartesianToAddress({static_cast<u16>(menuCol - 2), static_cast<u16>(kBottomRightNT + 1)});
        menuAddr = ppu::CartesianToAddress({menuCol, static_cast<u16>(kBottomRightNT + 1)});

        const u16 playModeCol = kMenuNT + (viewport_mx() << 1) - 1 - kPlayModeBoxWidth;
        ui::choice::SingleChoice playMode(kPlayModeOptions, 0);
        playModePos = {playModeCol, static_cast<u16>(kBottomRightNT + 1)};
        // Same leak, same fix -- see pMenuChunks's own comment above.
        delete[] pPlayModeChunks;
        pPlayModeChunks = MakeOptionBoxes(
            SIZED_OBJ(msg_playMode), playModePos, kPlayModeBoxWidth,
            chrHUDWhitespace_tile, 0, playModeOptionAddr, kPlayModeOptions
        );

        playModeClearAddr = ppu::CartesianToAddress({static_cast<u16>(playModeCol - 2), static_cast<u16>(kBottomRightNT + 1)});
        pPlayMode = &playMode;
        pMenu = &menu;
        pMainMenu = &menu;
        pOptionAddr = menuOptionAddr;

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

            if (pMenu) {
                const u8 lastOption = pMenu->option;
                pMenu->Pass<true>(pressed);

                if (const u8 newOption  = pMenu->option; newOption != lastOption) {
                    // 3, 4 hold the new arrow of last write (ie, current)
                    // making that addr the upcoming clear is the goal
                    scratchpad[1] = scratchpad[3];
                    scratchpad[2] = scratchpad[4];
                    // write ppu addr of new arrow location for NMI into scratchpad
                    const u16 newOptionAddr = pOptionAddr[newOption];
                    scratchpad[3] = newOptionAddr &  0xff;
                    scratchpad[4] = newOptionAddr >> 8;
                    scratchpad[0] = 1;  // enable 'do update'
                }
            }

            if (pressed & input::A) {
                if (pMenu == pPlayMode) {
#ifdef PLAYER2_SUPPORTED
                    level::multiplayer = pPlayMode->option != 0;
#endif
                    ppu::PPUMASK = 0;
                    gameMode = eGameModes::Level;
                    return;
                }

                switch (menu.option) {
                    case NewGame:
                    case Continue:
                        playModeAddr = ppu::CartesianToAddress(playModePos);
                        pNMI  = nmi_handler_drawPlayMode;
                        pMenu = pPlayMode;
                        pOptionAddr = playModeOptionAddr;
                        break;

                    case Options:
                        break;

#if defined(TARGET_MACOS) || defined(TARGET_WINDOWS) || defined(TARGET_LINUX)
                    case Quit:
                        quit = true;
                        return;
#endif

                    default: ;
                }
            }

            if (pressed & input::B && pMenu == pPlayMode) {
                pNMI = nmi_handler_drawMenu;
                pMenu = pMainMenu;
                pOptionAddr = menuOptionAddr;
            }

            video::WaitForPresent();
            if (quit) return;
        }
    }

    static void ArmSplitIRQ() {
        const u16 splitPixelRow = static_cast<u16>(SplitRow()) << 3;
        constexpr u8 kSplitLatency = REGION ? 4 : 3;
        const u8 splitReload = splitPixelRow > kSplitLatency
            ? static_cast<u8>(splitPixelRow - kSplitLatency) : 0;
        mmc3::ScheduleScanlineIRQ(splitReload, {0, splitPixelRow});
    }

    void nmi_handler() {
        oam::RefreshSprites(OAMBuffer);
        SelectorUpdate();

        ppu::SetScroll({0, PreviewScrollY()});

        ArmSplitIRQ();
    }

    static void nmi_handler_drawPlayMode() {
        u16 clearAddr = menuClearAddr;
        for (u8 row = 0; row < kMenuOptions; row++) {
            ppu::WriteRepeatedToNameTable(clearAddr, chrHUDWhitespace_tile, kMenuBoxWidth + 2, 0);
            clearAddr = static_cast<u16>(clearAddr + 32);
        }

        ui::text::Draw(pPlayModeChunks, playModeAddr, vec2<u8>{kPlayModeBoxWidth, kPlayModeOptions}, ui::text::Left);
        QueueSelectorDraw(playModeOptionAddr[pPlayMode->option]);
        SelectorUpdate();
        ppu::SetScroll({0, PreviewScrollY()});
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    static void nmi_handler_drawMenu() {
        u16 clearAddr = playModeClearAddr;
        for (u8 row = 0; row < kPlayModeOptions; row++) {
            ppu::WriteRepeatedToNameTable(clearAddr, chrHUDWhitespace_tile, kPlayModeBoxWidth + 2, 0);
            clearAddr = static_cast<u16>(clearAddr + 32);
        }

        ui::text::Draw(pMenuChunks, menuAddr, vec2<u8>{kMenuBoxWidth, kMenuOptions}, ui::text::Left);
        QueueSelectorDraw(menuOptionAddr[pMainMenu->option]);
        SelectorUpdate();
        ppu::SetScroll({0, PreviewScrollY()});
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    void irq_handler() {
        mmc3::AcknowledgeScanlineIRQ();
        tech::SpinWait(kSplitDelay);
        ApplySplit();
    }

    static void ApplySplit() {
        ppu::SetScroll({static_cast<u16>(kMenuNT << 3), static_cast<u16>(SplitRow() << 3)});
    }

    void InitTitleScreen() {
        const vec2<u8> titleBox{static_cast<u8>((viewport_mx() >> 1) - 1), 3};
        const auto titleText = ui::text::Make(
                SIZED_OBJ(msg_title),
                titleBox, chrHUDWhitespace_tile
            );
        ui::text::Draw(
            titleText,
            {kMenuNT + 1, static_cast<u16>(kBottomRightNT + 1)},
            titleBox,
            ui::text::Left
        );

        delete[] titleText;
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

        const u16 groundNtRow = tyBase * 8 + (levelHeight - 2) * 16;
        const i16 rawFeetY    = static_cast<i16>(groundNtRow) - 16 - static_cast<i16>(PreviewScrollY());
        const auto feetY      = static_cast<oam::oam_t>(rawFeetY < 0 ? 0 : rawFeetY);
        OAMBuffer[0].y = feetY; OAMBuffer[1].y = feetY;
        OAMBuffer[0].x = 32;    OAMBuffer[1].x = 40;
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