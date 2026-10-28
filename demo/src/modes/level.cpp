#include "level.hpp"

#include <pnes/apu.hpp>
#include <pnes/mappers/mmc3.hpp>

#include "../banks.hpp"
#include "../main.hpp"

#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../../chr/chr.h"
#include "../graphics/metasprites.hpp"
#include "../graphics/metatiles.hpp"

#include "level/levels.hpp"
#include "level/collision_map.hpp"
#include "level/dynamic.hpp"
#include "pnes/extras/debug/emphasis.hpp"

using namespace demo;
using enum level::eLevelStreamCommands;

namespace level {
    bool paused;

    u8 port1;
    u8 port2;

    u8 lastPort1;
    u8 lastPort2;

    constexpr u8 kHudSplitRow   = 16;
    constexpr u8 kHudSplitMMC3  = 16 - (
            REGION ? 4 : 3
    );
    constexpr u8 kHUDSplitDelay = REGION ? 90 : 0;

    static FIXED void ApplyHudSplit();

#ifdef TARGET_NES
    constexpr u8 kCoinVramRec = 4;
    constexpr u8 kCoinVramCap = 8 * kCoinVramRec;    // 4 coins, 2 records each
#else
    constexpr u8 kCoinVramRec = 3;
    constexpr u8 kCoinVramCap = 16 * kCoinVramRec;   // 4 coins, 4 records each
#endif
    static u8 CoinVram[kCoinVramCap];
    static u8 CoinVramLen;                  // bytes used

    static void CoinVramReset() { CoinVramLen = 0; }
    static bool suppressNmiScroll;

#ifdef TARGET_NES
    ACTORS void CoinVramPush(const u16 address, const u8 v0, const u8 v1) {
        if (CoinVramLen + kCoinVramRec > kCoinVramCap) return;
        CoinVram[CoinVramLen++] = static_cast<u8>(address >> 8);
        CoinVram[CoinVramLen++] = static_cast<u8>(address & 0xFF);
        CoinVram[CoinVramLen++] = v0;
        CoinVram[CoinVramLen++] = v1;
    }
#else
    ACTORS void CoinVramPush(const u16 address, const u8 value) {
        if (CoinVramLen + kCoinVramRec > kCoinVramCap) return;
        CoinVram[CoinVramLen++] = static_cast<u8>(address >> 8);
        CoinVram[CoinVramLen++] = static_cast<u8>(address & 0xFF);
        CoinVram[CoinVramLen++] = value;
    }
#endif

#ifdef TARGET_NES
    AI static void CoinWritePair(const u16 address, const u8 v0, const u8 v1) {
        tech::peek(ppu::raw::PPUSTATUS);
        tech::poke(ppu::raw::PPUADDR, static_cast<u8>(address >> 8));
        tech::poke(ppu::raw::PPUADDR, static_cast<u8>(address & 0xFF));
        tech::poke(ppu::raw::PPUDATA, v0);
        tech::poke(ppu::raw::PPUDATA, v1);
    }
#endif

    // kHudRows lives in levels.hpp so the collision bitmap (producer) and the
    // actor row projection (ClampRow, below) share one origin.

    Player player1;
#ifdef PLAYER2_SUPPORTED
    Player player2;

    // Zero-initialized (BSS): single-player by default until something
    // wires this up, per the doc comment in level.hpp.
    SYSMEM bool multiplayer;
#endif

    i8 lastDeltaScroll;
    atomic u16 lastXWorldSpace;
    static u16 edgeRAbs;
    u16 cameraX;

    static atomic u16 rasterCameraX;
#if defined(TARGET_NDS) || defined(TARGET_GBA)
    // Same latch for the vertical follow on the handhelds: the bump is derived
    // from P1's sprite Y, which has to be the Y the OAM DMA just shipped.
    static atomic oam::oam_t rasterPlayerY;
#endif

    atomic tech::enum_flags<eLevelStreamCommands> levelStreamCommand;
    u8 TileBuffer[56];

    static oam::oam_t Clear(u16 _);
    static oam::oam_t SpriteY(u16 i);
    static oam::oam_t SpriteX(u16 i);
#ifdef PLAYER2_SUPPORTED
    static oam::oam_t SpriteY2(u16 i);
    static oam::oam_t SpriteX2(u16 i);
#endif
    static i16 ClampRow(u16 y);
    static COLD NI void EnterLevelSetup() {
        suppressNmiScroll = true;   // see ::suppressNmiScroll's own comment
        pNMI = nmi_handler;
        pIRQ = irq_handler;

        video::WaitForPresent();

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

        const bool levelLoaded = mmc3::CallInBlock<level_code_tag>([] {
            return LoadLevel(0);
        });
        if (!levelLoaded) {
            irq::reset();    // spin reset on NES, exit on SDL3
        }

        ppu::Flush(blob::ui::hud_whitespace, 0xff);

        oam::PopulateFromProvider(OAMBuffer, 0, oam::y, Clear, 64);


        CallLevelGraphics([] {
            oam::PopulateFromBuffer(OAMBuffer, 1, oam::tile,       msMary, kMarySprites);
            oam::PopulateFromBuffer(OAMBuffer, 1, oam::attributes, msMary, kMarySprites);
#ifdef PLAYER2_SUPPORTED
            if (multiplayer) {
                oam::PopulateFromBuffer(OAMBuffer, 1 + kMarySprites, oam::tile,       msMary2, kMarySprites);
                oam::PopulateFromBuffer(OAMBuffer, 1 + kMarySprites, oam::attributes, msMary2, kMarySprites);
            }
#endif
        });
        oam::PopulateFromProvider(OAMBuffer, 1, oam::y, SpriteY,  kMarySprites);
        oam::PopulateFromProvider(OAMBuffer, 1, oam::x, SpriteX,  kMarySprites);

#ifdef PLAYER2_SUPPORTED
        // player 2: horizontally flipped, OAM slots 5..8 -- left at the
        // Clear() (y=0xf0, off-screen) fill above unless multiplayer is on.
        if (multiplayer) {
            oam::PopulateFromProvider(OAMBuffer, 1 + kMarySprites, oam::y, SpriteY2, kMarySprites);
            oam::PopulateFromProvider(OAMBuffer, 1 + kMarySprites, oam::x, SpriteX2, kMarySprites);
        }
#endif

        video::WaitForPresent();
        CallLevelGraphics([] {
            ppu::pal::WriteFromBuffer(ppu::BG_0,         SIZED_OBJ(BGColours));
            ppu::pal::WriteFromBuffer(13,                SIZED_OBJ(hudColours));   // BG palette 3 (HUD), same colours as the title screen's
            ppu::pal::WriteFromBuffer(ppu::SPRITE_0 + 1, SIZED_OBJ(maryColors));
            ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - ::tech::nes_str::length(msg_mary)), 0}, SIZED_STR(msg_mary), 0);
        });

        constexpr u8 coinUI[] = {blob::ui::hud_coin, blob::ui::font + 0, blob::ui::font + 0};
        ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - sizeof(coinUI)), 1}, SIZED_OBJ(coinUI), 0);

        mmc3::CallInBlock<level_code_tag>([] {
            PopulateNameTableColumns(static_cast<u16>(2 + video::viewport_tx()));
        });

        suppressNmiScroll = false;

        edgeRAbs    = (1 + viewport_mx()) * levelHeight;
        edgeL    = { TileData };
        dynEdgeL = { DynLengths, DynData, 0 }; // dyn backward edge, lockstep w/ edgeL

        ppu::SetScroll({0, 0});


        mmc3::CallPairedBlock<audio_tag, audio_data_tag>([] {
            audio::TrackPlay(0);
        });

        // ColMapSeed is ::LEVEL_CODE too -- same reason and pattern as LoadLevel's
        // wrap above.
        mmc3::CallInBlock<level_code_tag>([] {
            ColMapSeed(0, { TileData }, { DynLengths, DynData, 0 });
        });

        mmc3::CallInBlock<actor_tag>([] {
            player1.Start();
#ifdef PLAYER2_SUPPORTED
            player2.Start();
#endif
        });
        oam::RefreshSprites(OAMBuffer);   /* seed the first frame's sprite snapshot */

        video::WaitForPresent();
        ppu::EnableRendering(ppu::ctrl::SPRITE_SIZE | ppu::ctrl::SPRITE_ADDR, ppu::mask::BG_L | ppu::mask::SPRITE_L);

        apu::DisableFrameIRQ();
        apu::DisableDMCIRQ();
    }

    LEVEL_CODE void main() {
        InColdBank(EnterLevelSetup);
        irq::EnableInterrupts();
        // ReSharper disable once CppDFAEndlessLoop
        while (!quit) {
            input::PollControllers(&port1, &port2);
            if (port1 & input::START && (port1 ^ lastPort1) & input::START) {
                paused = !paused;
            }

            mmc3::Call<level::UpdateActors>();
            ColMapTrack(cameraX >> 4);
            mmc3::CallPairedBlock<audio_tag, audio_data_tag>([] { audio::Update(); });
            video::WaitForPresent();
        }
    }

    FIXED void nmi_handler() {
        lastPort1 = port1; lastPort2 = port2;
        oam::RefreshSprites(OAMBuffer);

        // Latched here, with the sprite snapshot this DMA just shipped, and read
        // by ApplyHudSplit from the scanline IRQ -- see ::rasterCameraX.
        rasterCameraX = cameraX;
#if defined(TARGET_NDS) || defined(TARGET_GBA)
        rasterPlayerY = player1.actor.screen.y;
#endif

        using enum eLevelStreamCommands;
        if (levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_DONE) SHADOW(ppu::PPUMASK) {
            ppu::PPUMASK = 0;
            if (levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_RIGHT) {
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) + video::viewport_tx() + 0), 2}, TileBuffer, 28, 1);
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) + video::viewport_tx() + 1), 2}, TileBuffer + 28, 28, 1);
                if (!(levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_SWAP))
                    ppu::WriteFromBufferToAttributeTable({static_cast<u16>(((lastXWorldSpace >> 3) + video::viewport_tx()) & ~3), 2}, AttributeBuffer, 8, 1);
            } else {
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) - 1), 2}, TileBuffer, 28, 1);
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) - 2), 2}, TileBuffer + 28, 28, 1);
                if (!(levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_SWAP))
                    ppu::WriteFromBufferToAttributeTable({static_cast<u16>(((lastXWorldSpace >> 3) - 2) & ~3), 2}, AttributeBuffer, 8, 1);
            }
        }

        if (CoinVramLen) SHADOW(ppu::PPUMASK) {
            ppu::PPUMASK = 0;
#ifdef TARGET_NES
            ppu::PPUCTRL = ppu::PPUCTRL & ~ppu::ctrl::POLARITY;
            for (u8 i = 0; i < CoinVramLen; i += kCoinVramRec)
                CoinWritePair((CoinVram[i] << 8) | CoinVram[i + 1],
                              CoinVram[i + 2], CoinVram[i + 3]);
#else
            for (u8 i = 0; i < CoinVramLen; i += kCoinVramRec)
                ppu::WriteSingleToNameTable((CoinVram[i] << 8) | CoinVram[i + 1],
                                            CoinVram[i + 2]);
#endif
        }
        CoinVramReset();

        if (!suppressNmiScroll) ppu::SetScroll({0, 0});
        if (levelStreamCommand & STREAM_LEVEL_DONE) {
            levelStreamCommand = {};
        }
        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::ui)>();

        mmc3::ScheduleScanlineIRQ(kHudSplitMMC3, {0, kHudSplitRow});
    }

    // ::FIXED for the same reason as nmi_handler, and it calls ApplyHudSplit
    // directly, which is therefore ::FIXED too.
    FIXED void irq_handler() {
        mmc3::AcknowledgeScanlineIRQ();
        tech::SpinWait(kHUDSplitDelay);
        ApplyHudSplit();
    }

    static oam::oam_t Clear(const u16) {
        return 0xf0;
    }

    // Provider shims: oam::PopulateFromProvider hands the callback only the sprite
    // index, so bind the player Actor here and forward to the Actor-aware versions.
    static oam::oam_t SpriteY(const u16 i)  { return AdjustSpriteY(&player1.actor, i); }
    static oam::oam_t SpriteX(const u16 i)  { return AdjustSpriteX(&player1.actor, i); }
#ifdef PLAYER2_SUPPORTED
    static oam::oam_t SpriteY2(const u16 i) { return AdjustSpriteY(&player2.actor, i); }
    static oam::oam_t SpriteX2(const u16 i) { return AdjustSpriteX(&player2.actor, i); }
#endif

    static i16 ClampRow(const u16 y) {
        if (y & 0x8000) return 0;
        const i16 r = static_cast<i16>(y >> 4) - kHudRows;
        if (r < 0) return 0;
        return r < levelHeight ? r : levelHeight - 1;
    }

    static FIXED void ApplyHudSplit() {
        #if defined(TARGET_NDS) || defined(TARGET_GBA)
            const i16 mid    = static_cast<i16>(video::viewport_py() >> 1);
            const i16 anchor = static_cast<i16>(240 - video::viewport_py());
            const i16 raw    = static_cast<i16>(rasterPlayerY) - mid;
            const i16 bump   = raw < 0 ? 0 : (raw > anchor ? anchor : raw);
            ppu::SetScroll({rasterCameraX, static_cast<u16>(16 + bump)});
        #else
            ppu::SetScroll({rasterCameraX, kHudSplitRow});
        #endif

        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::staticLevelTiles)>();
    }
}
