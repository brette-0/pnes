#include "level.hpp"

#include <pnes/apu.hpp>
#include <pnes/mappers/mmc3.hpp>

#include "../banks.hpp"
#include "../main.hpp"

#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../../chr/chr.h"
#include "../graphics/metatiles.hpp"

#include "level/levels.hpp"
#include "level/collision_map.hpp"
#include "level/dynamic.hpp"
#include "pnes/extras/debug/emphasis.hpp"

#include "../../animations/gen/mary_standing.hpp"
#include "../../animations/gen/mary_walking.hpp"
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

#ifdef TARGET_NDS
    // --- lower-panel blanking -----------------------------------------------
    // The DS output is 384 rows but this game's frame is 240, and the follow
    // camera windows those 240 into the TOP panel (::VerticalWindow, main.hpp).
    // One scroll covers a whole band, so with nothing else said the lower panel
    // simply carries on showing the plane below the window -- which is the
    // level's own lower rows. They leak onto the bottom screen.
    //
    // So the lower panel gets raster bands of its own, scrolled to the one part
    // of the plane the level never draws into: the bottom of the RIGHT
    // nametable. Vertical mirroring (SetMirroring(false), EnterLevelSetup) grids
    // the board's two nametables 2-wide, each the viewport's own 32x48 tiles
    // here (emu::ComputeNtGeometry), so the plane is 512x384 while the level
    // occupies only rows 0..29 of it -- and EnterLevelSetup's ppu::Flush has
    // already filled every page with hud_whitespace, so what is left below the
    // level really is blank rather than merely unwritten.
    constexpr u16 kLowerPanelRow = video::panel_rect(1).y;                  // 192
    constexpr u16 kBlankPlaneX   = video::viewport_px();                    // 256: right nametable
    constexpr u16 kBlankPlaneY   = kFrameRows;                              // first plane row below the level
    constexpr u16 kBlankRows     = video::viewport_py() - kBlankPlaneY;     // 144

    // 144 blank rows do not cover a 192-row panel, so when a band runs out of
    // them it is re-pointed at the same plane row instead of being left to wrap
    // onto plane row 0 -- the HUD, and below it the top of the level, i.e. the
    // leak again at the very bottom of the screen. Repeating a uniform blank
    // region is invisible, and re-pointing to the SAME row is exactly the write
    // emu::scrollWritten exists to notice (src/emu/ppu.cpp).
    static atomic u16 nextBlankRow;

    static FIXED void ApplyLowerPanelBlank();
#endif

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
#ifdef PNES_WINDOWED_PANEL
    // Same latch for the vertical follow on a windowed panel: the bump is derived
    // from P1's sprite Y, which has to be the Y the OAM DMA just shipped.
    static atomic oam::oam_t rasterPlayerY;
#endif

    atomic tech::enum_flags<eLevelStreamCommands> levelStreamCommand;
    u8 TileBuffer[56];

    // Hardware sprites the player takes: the generated metasprite's own slot
    // count, so the OAM populates below cannot drift from the art the designer
    // exported. Every animation of one actor has to agree about that -- the
    // slots are allocated once, at fixed indices -- so the walk is checked
    // against the stand rather than trusted.
    constexpr u8 kPlayerSprites = static_cast<u8>(gen::mary_standing::kSize);
    static_assert(gen::mary_walking::kSize == gen::mary_standing::kSize,
                  "mary's animations disagree about how many sprites she is: one of "
                  "them would run off the end of the slots reserved for her");

    static oam::oam_t Clear(u16 _);
    static oam::oam_t Zero(u16 _);
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

        // The generated animations carry no palette and no mirroring (see their
        // banner), and the level draws every sprite out of sprite palette 0 --
        // which is a zero. The players' own attribute bytes are frame data now
        // that they can face left, and ::FeedOAM rewrites those two (or four)
        // every frame; this clears the whole buffer anyway, because the title
        // screen had its own sprites in here and only the Y column above is
        // reset for it.
        oam::PopulateFromProvider(OAMBuffer, 0, oam::attributes, Zero, 64);

        // Tiles and positions are NOT seeded here: ::FeedOAM owns them, and it
        // is called below once the players have started and have an animation
        // and a screen position to read. Seeding them here as well would be the
        // same bytes written from two places, a frame apart.

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
        // FeedOAM is ::ACTORS now, so it goes through its binding rather than a
        // CallInBlock on this bank -- same form the gameplay loop uses, and one
        // fewer per-lambda trampoline in ::FIXED than CallInBlock would cost.
        mmc3::Call<level::FeedOAM>();
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

            // Banked: window 1 holds this function's own bank right now, so the
            // switch has to happen from the ::FIXED trampoline mmc3::Call uses.
            mmc3::Call<level::FeedOAM>();

            video::WaitForPresent();
        }
    }

    // ::ACTORS, not ::LEVEL_CODE -- see level.hpp's declaration for why, and
    // note everything this reaches has to be resident or FIXED: bank 0 and
    // bank 5 are both window 1, so while this runs the level's own code is
    // not mapped. The Sprite* providers, AdjustSprite*, and oam::OAMFromProvider
    // all sit in untagged .text at $C000, which is always in.
    // NI for the same reason DrainPendingColumn (player.cpp) carries one:
    // mmc3::Call inlines its target into the ::FIXED window trampoline, so
    // without this the whole body lands in the always-mapped $E000-$FFFF bank
    // -- measured, not theoretical: the first cut of this move left
    // prg_rom_actors untouched and grew prg_rom_fixed by 158 bytes. One jsr
    // into the actors bank is the point of moving it there.
    NI ACTORS void FeedOAM() {
#ifdef PLAYER2_SUPPORTED
        const u8 shuffleBegin = multiplayer ? 4 : 2;
#else
        const u8 shuffleBegin = 2;
#endif

        // A 16x16 metasprite mirrors by flipping each tile and swapping the two
        // columns -- for a single row of two slots, writing them to each other's
        // place. Four byte stores, which is why facing costs no second,
        // pre-mirrored copy of every frame in ROM.
        static_assert(gen::mary_standing::kWidth == 2 && gen::mary_standing::kHeight == 1
                   && gen::mary_walking::kWidth  == 2 && gen::mary_walking::kHeight  == 1,
            "FeedOAM mirrors the player by swapping a pair of slots, which assumes a "
            "2x1 metasprite -- a wider or taller one needs row*kWidth + (kWidth-1-col)");

        // Sprite attribute bit 6: flip this sprite horizontally. The rest of the
        // byte stays zero -- palette 0, front priority, no vertical flip.
        constexpr u8 kFlipH = 0x40;

        // Tile and attribute go in directly rather than through
        // ::oam::PopulateFromProvider. The provider form pays an indirect call
        // per slot and needs a function whose body can see the facing, and at
        // two slots that costs more -- in cycles and in this bank's very
        // limited room -- than just writing the four bytes. Y and X keep their
        // providers: those are shared with the setup path and with player 2,
        // and neither depends on facing.
        oam::PopulateFromProvider(OAMBuffer, 1, oam::y, SpriteY, kPlayerSprites);
        oam::PopulateFromProvider(OAMBuffer, 1, oam::x, SpriteX, kPlayerSprites);

        // The animation frame is one tile id per slot, packed -- NOT the sprite_t
        // records ::oam::PopulateFromBuffer walks by the sprite stride, which
        // would read every slot but the first from the wrong byte. Indexed here,
        // where the layout and the facing are both known. The ids are OAM-ready
        // as they stand: the designer folds in the pattern-table half of the CHR
        // entry the art was banked into (see the generated header's banner).
        //
        // X is untouched by the mirror: slot 0 stays at the left of the pair and
        // simply receives what slot 1 was holding, flipped. Swapping the tiles
        // and swapping the positions would cancel out.
        {
            oam::sprite_t* const pair  = &OAMBuffer[1];
            const u8*      const frame = player1.actor.animationFrame;
            // Facing is the actor's own state, latched from input by
            // Player::Update -- read here, never decided here.
            if (player1.actor.horizontalFacingDir) {
                pair[0].tile = frame[1];
                pair[1].tile = frame[0];
                pair[0].attributes = kFlipH;
                pair[1].attributes = kFlipH;
            } else {
                pair[0].tile = frame[0];
                pair[1].tile = frame[1];
                pair[0].attributes = 0;
                pair[1].attributes = 0;
            }
        }

#ifdef PLAYER2_SUPPORTED
        // Left at the Clear() park unless multiplayer is on -- same reason as the
        // setup path's player 2 block.
        if (multiplayer) {
            oam::PopulateFromProvider(OAMBuffer, 1 + kPlayerSprites, oam::y, SpriteY2, kPlayerSprites);
            oam::PopulateFromProvider(OAMBuffer, 1 + kPlayerSprites, oam::x, SpriteX2, kPlayerSprites);
            oam::sprite_t* const pair  = &OAMBuffer[1 + kPlayerSprites];
            const u8*      const frame = player2.actor.animationFrame;
            if (player2.actor.horizontalFacingDir) {
                pair[0].tile = frame[1];
                pair[1].tile = frame[0];
                pair[0].attributes = kFlipH;
                pair[1].attributes = kFlipH;
            } else {
                pair[0].tile = frame[0];
                pair[1].tile = frame[1];
                pair[0].attributes = 0;
                pair[1].attributes = 0;
            }
        }
#endif
    }

    FIXED void nmi_handler() {
        lastPort1 = port1; lastPort2 = port2;
        oam::RefreshSprites(OAMBuffer);

        // Latched here, with the sprite snapshot this DMA just shipped, and read
        // by ApplyHudSplit from the scanline IRQ -- see ::rasterCameraX.
        rasterCameraX = cameraX;
#ifdef PNES_WINDOWED_PANEL
        rasterPlayerY = player1.actor.screen.y;
#endif

        using enum eLevelStreamCommands;
        if (levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_DONE) SHADOW(ppu::PPUMASK) {
            ppu::PPUMASK = 0;
            if (levelStreamCommand & eLevelStreamCommands::STREAM_LEVEL_RIGHT) {
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) + video::viewport_tx() + 0), 2}, TileBuffer, 28, 1);
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) + video::viewport_tx() + 1), 2}, TileBuffer + 28, 28, 1);
                // Unconditional, including on a direction reversal: the
                // reversal's own half-byte problem is fixed where it is created,
                // in DrainPendingColumn (player.cpp), which recomposes the
                // partner half. Skipping the write here instead left the column
                // just revealed wearing the palettes of whatever the plane held
                // there before -- stale attributes, for the 16px until the next
                // column completed the pair.
                ppu::WriteFromBufferToAttributeTable({static_cast<u16>(((lastXWorldSpace >> 3) + video::viewport_tx()) & ~3), 2}, AttributeBuffer, 8, 1);
            } else {
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) - 1), 2}, TileBuffer, 28, 1);
                ppu::WriteFromBufferToNameTable({static_cast<u16>((lastXWorldSpace >> 3) - 2), 2}, TileBuffer + 28, 28, 1);
                // Unconditional, for the same reason as the rightward write.
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

#ifdef TARGET_NDS
        // The split chain always starts at the HUD split, whatever last frame
        // got through: a band whose IRQ was consumed stale (GenerateBands drops
        // one it has already scrolled past without calling the handler) would
        // otherwise leave this set and make the next frame's first split take
        // the lower-panel branch instead of ApplyHudSplit.
        nextBlankRow = 0;
#endif
        mmc3::ScheduleScanlineIRQ(kHudSplitMMC3, {0, kHudSplitRow});
    }

    // ::FIXED for the same reason as nmi_handler, and it calls ApplyHudSplit
    // directly, which is therefore ::FIXED too.
    FIXED void irq_handler() {
        mmc3::AcknowledgeScanlineIRQ();
        tech::SpinWait(kHUDSplitDelay);
#ifdef TARGET_NDS
        // The frame's splits in raster order: the HUD split first (nextBlankRow
        // is 0 at the top of every frame), then one or more lower-panel bands,
        // each armed by the one before it.
        if (nextBlankRow) { ApplyLowerPanelBlank(); return; }
#endif
        ApplyHudSplit();
    }

    static oam::oam_t Clear(const u16) {
        return ParkedSpriteY();
    }

    static oam::oam_t Zero(const u16) {
        return 0;
    }

    // Provider shims: oam::PopulateFromProvider hands the callback only the sprite
    // index, so bind the player Actor here and forward to the Actor-aware versions.
    //
    // Y goes through ::WindowedSpriteY, the sprite half of the follow camera
    // ApplyHudSplit scrolls the background with. P1 is the camera subject for
    // BOTH players -- P2 rides P1's window, same as the background does, or the
    // two would disagree about which rows the panel is showing.
#ifdef PNES_WINDOWED_PANEL
    static oam::oam_t SpriteY(const u16 i)  { return WindowedSpriteY(AdjustSpriteY(&player1.actor, i), player1.actor.screen.y); }
#else
    static oam::oam_t SpriteY(const u16 i)  { return AdjustSpriteY(&player1.actor, i); }
#endif
    static oam::oam_t SpriteX(const u16 i)  { return AdjustSpriteX(&player1.actor, i); }
#ifdef PLAYER2_SUPPORTED
#ifdef PNES_WINDOWED_PANEL
    static oam::oam_t SpriteY2(const u16 i) { return WindowedSpriteY(AdjustSpriteY(&player2.actor, i), player1.actor.screen.y); }
#else
    static oam::oam_t SpriteY2(const u16 i) { return AdjustSpriteY(&player2.actor, i); }
#endif
    static oam::oam_t SpriteX2(const u16 i) { return AdjustSpriteX(&player2.actor, i); }
#endif

    static i16 ClampRow(const u16 y) {
        if (y & 0x8000) return 0;
        const i16 r = static_cast<i16>(y >> 4) - kHudRows;
        if (r < 0) return 0;
        return r < levelHeight ? r : levelHeight - 1;
    }

    static FIXED void ApplyHudSplit() {
        #ifdef PNES_WINDOWED_PANEL
            // The background half of the follow camera: on a target whose panel
            // is shorter than the 240-row frame this game draws into (GBA 160,
            // the DS's gameplay panel 192), the panel is a window onto that frame
            // and this scroll is which rows it shows. ::VerticalWindow (main.hpp)
            // owns the clamp; ::SpriteY shifts sprites by the same call, off the
            // same latched player Y, which is what keeps them glued to this.
            ppu::SetScroll({rasterCameraX,
                            static_cast<u16>(kHudSplitRow + VerticalWindow(rasterPlayerY))});
        #else
            ppu::SetScroll({rasterCameraX, kHudSplitRow});
        #endif

        mmc3::SwitchCharactersBy<GetCharacterEntryInformation(blob::staticLevelTiles)>();

#ifdef TARGET_NDS
        // Arm the first lower-panel band. Last in this handler, after the
        // raster-critical scroll write, for the same reason title.cpp's
        // ApplySplit banks after its own.
        nextBlankRow = kLowerPanelRow;
        mmc3::ScheduleScanlineIRQ(0, {0, kLowerPanelRow});
#endif
    }

#ifdef TARGET_NDS
    // Point this band at the blank bottom of the right nametable, and arm the
    // next one if the panel is not covered yet. The MMC3 scanline latch (the
    // first argument) is NES-only and this is a DS-only split, so the position
    // is the whole request here.
    static FIXED void ApplyLowerPanelBlank() {
        ppu::SetScroll({kBlankPlaneX, kBlankPlaneY});

        const u16 next = static_cast<u16>(nextBlankRow + kBlankRows);
        if (next < video::viewport_py()) {
            nextBlankRow = next;
            mmc3::ScheduleScanlineIRQ(0, {0, next});
        } else {
            nextBlankRow = 0;
        }
    }
#endif
}
