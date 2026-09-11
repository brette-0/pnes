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

    static ui::choice::SingleChoice* pPlayMode = nullptr;
    static buffer<u8*>* pPlayModeChunks = nullptr;
    static vec2<u16> playModePos;
    static u16 playModeAddr;
    static u16 menuClearAddr;
    static u16 playModeClearAddr;
    static u8 writeBuf[6];
    static atomic u8 writeBufLen = 0;

    static void DrainWriteBuf(const u8* start, const u8* end) {
        for (const u8* p = start; p < end; p += 3) {
            const int addr = (static_cast<int>(p[0]) << 8) | p[1];
            ppu::WriteSingleToNameTable(addr, p[2]);
        }
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
        u8* initCursor = writeBuf;
        ui::choice::SingleChoice menu(TitleUnselect, TitleSelect, kMenuOptions);
        const auto menuChunks = menu.Make(
            SIZED_OBJ(msg_menu),
            {menuCol, static_cast<u16>(kBottomRightNT + 1)},
            {kMenuBoxWidth, kMenuOptions},
            chrHUDWhitespace_tile, 0
        );
        menu.Draw(
            menuChunks,
            {menuCol, static_cast<u16>(kBottomRightNT + 1)},
            kMenuOptions,
            initCursor
        );

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
        DrainWriteBuf(writeBuf, initCursor);

        const u16 playModeCol = kMenuNT + (viewport_mx() << 1) - 1 - kPlayModeBoxWidth;
        ui::choice::SingleChoice playMode(TitleUnselect, TitleSelect, kPlayModeOptions);
        playModePos = {playModeCol, static_cast<u16>(kBottomRightNT + 1)};
        // Same leak, same fix -- see pMenuChunks's own comment above.
        delete[] pPlayModeChunks;
        pPlayModeChunks = playMode.Make(
            SIZED_OBJ(msg_playMode),
            playModePos,
            {kPlayModeBoxWidth, kPlayModeOptions},
            chrHUDWhitespace_tile, 0
        );

        playModeClearAddr = ppu::CartesianToAddress({static_cast<u16>(playModeCol - 2), static_cast<u16>(kBottomRightNT + 1)});
        pPlayMode = &playMode;
        pMenu = &menu;
        pMainMenu = &menu;

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
                u8* cursor = writeBuf;
                pMenu->Pass(pressed, cursor);
                writeBufLen = static_cast<u8>(cursor - writeBuf);
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

        if (writeBufLen) {
            DrainWriteBuf(writeBuf, writeBuf + writeBufLen);
            writeBufLen = 0;
        }

        ppu::SetScroll({0, PreviewScrollY()});

        ArmSplitIRQ();
    }

    static void nmi_handler_drawPlayMode() {
        u16 clearAddr = menuClearAddr;
        for (u8 row = 0; row < kMenuOptions; row++) {
            ppu::WriteRepeatedToNameTable(clearAddr, chrHUDWhitespace_tile, kMenuBoxWidth + 2, 0);
            clearAddr = static_cast<u16>(clearAddr + 32);
        }

        u8 indicatorBuf[3];
        u8* cursor = indicatorBuf;
        pPlayMode->Draw(pPlayModeChunks, playModeAddr, kPlayModeOptions, cursor);
        DrainWriteBuf(indicatorBuf, cursor);
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

        u8 indicatorBuf[3];
        u8* cursor = indicatorBuf;
        pMainMenu->Draw(pMenuChunks, menuAddr, kMenuOptions, cursor);
        DrainWriteBuf(indicatorBuf, cursor);
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
         const auto titleText = ui::text::Make(
                SIZED_OBJ(msg_title),
                {static_cast<u8>((viewport_mx() >> 1) - 1), 3}, chrHUDWhitespace_tile
            );
        ui::text::Draw(
            titleText,
            {kMenuNT + 1, static_cast<u16>(kBottomRightNT + 1)},
            3
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

    auto TitleUnselect(const u16 addr, u8*& buf) -> void {
        *buf++ = static_cast<u8>(addr >> 8);
        *buf++ = static_cast<u8>(addr & 0xFF);
        *buf++ = chrHUDWhitespace_tile;
    }

    auto TitleSelect(const u16 addr, u8*& buf) -> void {
        *buf++ = static_cast<u8>(addr >> 8);
        *buf++ = static_cast<u8>(addr & 0xFF);
        *buf++ = chrArrow_tile;
    }
}