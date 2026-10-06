#include "level.hpp"

#include <platform-nes/apu.hpp>
#include <platform-nes/mappers/mmc3.hpp>

#include "../banks.hpp"
#include "../main.hpp"

#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../graphics/graphics.hpp"
#include "../graphics/metasprites.hpp"
#include "../graphics/metatiles.hpp"

#include "level/levels.hpp"
#include "level/collision_map.hpp"
#include "level/dynamic.hpp"

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
        // NOTE:: if we are stuffed for cpu time, lower this and hand-write ASM
    constexpr u8 kHUDSplitDelay = REGION ? 90 : 0;

    // Forward-declared so irq_handler can call it directly once the MMC3
    // scanline IRQ fires below the HUD (see kHudSplitLatch / irq_handler).
    static FIXED void ApplyHudSplit();

    constexpr u8 kCoinVramCap = 48;         // 16 tile writes = 4 coins (2x2 each)
    static u8 CoinVram[kCoinVramCap];
    static u8 CoinVramLen;                  // bytes used

    static void CoinVramReset() { CoinVramLen = 0; }

    // Guards nmi_handler's own ppu::SetScroll call against EnterLevelSetup's
    // raw, unprotected PPUADDR/PPUDATA uploads (ppu::Flush, the OAM/palette/
    // name-table writes, PopulateNameTableColumns). On real NES hardware,
    // PPUSCROLL ($2005) and PPUADDR ($2006) share the PPU's internal write
    // toggle and loopy_t/loopy_v address registers. NMI generation is
    // already running throughout EnterLevelSetup (inherited from title --
    // see that function's own comment), so every one of those multi-byte
    // VRAM uploads is live to being interrupted mid-sequence: if
    // nmi_handler's unconditional SetScroll lands in the middle of one, its
    // two PPUSCROLL writes clobber the VRAM address the interrupted write
    // was relying on for its remaining bytes, landing them at an unrelated
    // address carrying whatever value happened to be mid-flight (observed:
    // a stray palette-0 byte landing on an attribute cell
    // PopulateNameTableColumns had already walked past -- hence a grey
    // coin, and only ever right after the title->level transition, where
    // NMI is live but none of this setup work is NMI-safe).
    //
    // Sidesteps NMI rather than disabling it (NMI can't be masked by SEI on
    // 6502): nmi_handler still runs every vblank and ::irq::nmi_done still
    // gets set (by the NMI vector macro itself, unconditionally, not by
    // this handler), so ::video::WaitForPresent keeps working throughout.
    static bool suppressNmiScroll;

    // ACTORS: sole caller is PushCoinVram (player.cpp), now in the actors bank --
    // CoinVram/CoinVramLen stay put (RAM, not bank-sensitive); only the code that
    // writes them needs to be reachable. See banks.hpp's ::actor_tag comment.
    ACTORS void CoinVramPush(const u16 address, const u8 value) {
        if (CoinVramLen + 3 > kCoinVramCap) return;        // full: drop (next frame retries)
        CoinVram[CoinVramLen++] = static_cast<u8>(address >> 8);
        CoinVram[CoinVramLen++] = static_cast<u8>(address & 0xFF);
        CoinVram[CoinVramLen++] = value;
    }

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

    // World-pixel boundary of the most recently streamed column (multiple of 16).
    // Doubles as scroll hysteresis: a new column builds only once the camera
    // passes a full 16px column beyond it, so jitter can't rapidly toggle
    // BuildNext/BuildPrev. Read by the NMI to place the nametable write.
    atomic u16 lastXWorldSpace;

    // Right edge's absolute metatile offset from the level start. Tracked so the
    // left edge can be parked exactly kEdgeGap metatiles behind it without an O(n)
    // re-walk: edgeL's target is derived from this counter, not from a one-shot seek.
    static u16 edgeRAbs;

    // Camera scroll origin in *pixels* (left edge of the viewport in world space).
    // The actor's sub-pixel worldSpace is canonical; this is derived/maintained from
    // it each frame because the PPU scroll register is integer-pixel only.
    u16 cameraX;

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

    // ::COLD, not ::FIXED: boot now establishes R6/R7 before this runs, so no
    // remaining call here can unmap itself mid-execution. Farcalled into the
    // COLD bank (banks.hpp's ::cold_tag) since it's one-time level-entry
    // setup, not worth permanently occupying the level domain's tight 16 KiB.
    // Safe to call from inside level's own domain: the CallInBlock trampoline
    // is ::FIXED, and only window 1 moves -- window 2 (ColMapStamp et al.)
    // stays mapped throughout.
    //
    // NI: the only call site, so without it LTO inlines the whole body into
    // the FIXED trampoline instead of ::COLD (confirmed empirically:
    // prg_rom_cold measured 0 bytes, prg_rom_fixed ballooned ~5.2 KiB).
    static COLD NI void EnterLevelSetup() {
        suppressNmiScroll = true;   // see ::suppressNmiScroll's own comment
        pNMI = nmi_handler;
        pIRQ = irq_handler;

        // Wait for one real NMI before touching any hardware state: NMI
        // generation is already running by this point (title leaves
        // PPUCTRL's GEN_NMI bit set -- it only clears PPUMASK on exit), and
        // pNMI now points at nmi_handler (just above), so this is safe the
        // instant it's reached.
        video::WaitForPresent();

        mmc3::SwitchCHRBank(mmc3::chr0Control, 4);
        mmc3::SwitchCHRBank(mmc3::chr1Control, 5);
        mmc3::SwitchCHRBank(mmc3::chr2Control, 0);
        mmc3::SwitchCHRBank(mmc3::chr3Control, 1);
        mmc3::SwitchCHRBank(mmc3::chr4Control, 6);
        mmc3::SwitchCHRBank(mmc3::chr5Control, 7);
        mmc3::SetMirroring(false);

        // LoadLevel is ::LEVEL_CODE; ::COLD can't reach a different bank-0
        // domain by a plain call, so this needs an explicit farcall -- same
        // pattern as Player::Reset's wrap below, naming level_code_tag.
        const bool levelLoaded = mmc3::CallInBlock<level_code_tag>([] {
            return LoadLevel(0);
        });
        if (!levelLoaded) {
            irq::reset();    // spin reset on NES, exit on SDL3
        }

        ppu::Flush(chrHUDWhitespace_tile, 0xff);

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

        // Palette RAM is a special case, unlike the nametable/attribute
        // writes below: the PPU reads the backdrop-color entry ($3F00 and
        // its mirrors) live, every pixel clock, EVEN WITH RENDERING OFF --
        // disabling rendering stops the PPU from fetching nametable/
        // attribute/pattern data (so those writes really are invisible
        // until rendering comes back on), but it does not stop the raster
        // scan itself. A palette write landing mid-frame is visible
        // immediately, as a horizontal band starting at whatever scanline
        // happened to be current -- and by this point (CHR switches, the
        // LoadLevel farcall, ::Flush's own long clear loop) we're well
        // past the entry wait above, arbitrarily far into whatever frame
        // is currently scanning. Fresh sync immediately before the write
        // that actually needs it, not just once at entry.
        video::WaitForPresent();
        CallLevelGraphics([] {
            ppu::pal::WriteFromBuffer(ppu::BG_0,         SIZED_OBJ(BGColours));
            ppu::pal::WriteFromBuffer(13,                SIZED_OBJ(hudColours));   // BG palette 3 (HUD), same colours as the title screen's
            ppu::pal::WriteFromBuffer(ppu::SPRITE_0 + 1, SIZED_OBJ(maryColors));
            ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - sizeof(msg_mary)), 0}, SIZED_OBJ(msg_mary), 0);
        });

        constexpr u8 coinUI[] = {chrHUDCoin_tile, chrFont_tile + 0, chrFont_tile + 0};
        ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - sizeof(coinUI)), 1}, SIZED_OBJ(coinUI), 0);

        // PopulateNameTableColumns is ::LEVEL_CODE too -- same reason and
        // pattern as LoadLevel's wrap above. Leaves edgeR/dynEdgeR seeded
        // at the level start; edgeRAbs/edgeL/dynEdgeL below pick up from
        // there for the backward-scroll edge.
        mmc3::CallInBlock<level_code_tag>([] {
            PopulateNameTableColumns(static_cast<u16>(2 + video::viewport_tx()));
        });

        // Last of the raw, unprotected VRAM uploads above -- safe from here
        // on for nmi_handler to touch PPUSCROLL again every frame.
        suppressNmiScroll = false;

        edgeRAbs    = (1 + viewport_mx()) * levelHeight;
        edgeL    = { TileData };
        dynEdgeL = { DynLengths, DynData, 0 }; // dyn backward edge, lockstep w/ edgeL

        ppu::SetScroll({0, 0});

        // Farcalled: module+engine share one bank, songs+SFX another, both
        // mapped for the duration -- see banks.hpp's ::audio_tag. Nested
        // CallInBlock, not CallPairedBlock: the engine walks audio_data_tag
        // while running, so it must still be mapped when audio_tag's block
        // runs -- entering audio_data_tag from inside audio_tag's own block
        // is what guarantees that ordering.
        //
        // audio::Init itself is NOT called here: it's one-shot device/asset
        // setup (on native backends it decodes and buffers the whole music
        // + SFX library), not per-level work. Calling it on every level entry
        // re-decoded and re-buffered all of that from scratch each time --
        // see RESET in main.cpp, which now runs it once at boot instead.
        mmc3::CallInBlock<audio_tag>([] {
            mmc3::CallInBlock<audio_data_tag>([] {
                audio::TrackPlay(0);
            });
        });

        // ColMapSeed is ::LEVEL_CODE too -- same reason and pattern as LoadLevel's
        // wrap above.
        mmc3::CallInBlock<level_code_tag>([] {
            ColMapSeed(0, { TileData }, { DynLengths, DynData, 0 });
        });

        // Player::Reset is ::ACTORS; ::COLD can't reach a different bank-5
        // domain by a plain call, so this needs an explicit farcall. This
        // block has nothing to do with UpdateActors, so name the tag
        // directly rather than routing through an unrelated bound function.
        mmc3::CallInBlock<actor_tag>([] {
            player1.Reset();
#ifdef PLAYER2_SUPPORTED
            player2.Reset();
#endif
        });
        oam::RefreshSprites(OAMBuffer);   /* seed the first frame's sprite snapshot */

        // Wait for a fresh VBlank before flipping PPUMASK on: everything
        // above ran as ordinary sequential code with nothing synced to the
        // PPU's frame boundary, so without this the write lands wherever
        // in the current scanline execution happens to reach here --
        // rendering can pop on mid-frame, a visible one-frame tear. Safe
        // here specifically because NMI generation is already running
        // (inherited from title, which never clears it) and pNMI has
        // pointed at nmi_handler since this function's own first line, not
        // because ::EnableRendering has run yet (it hasn't -- that's the
        // next line).
        video::WaitForPresent();
        ppu::EnableRendering(ppu::ctrl::SPRITE_SIZE | ppu::ctrl::SPRITE_ADDR, ppu::mask::BG_L | ppu::mask::SPRITE_L);

        apu::DisableFrameIRQ();
        apu::DisableDMCIRQ();
    }

    // LEVEL_CODE: farcalled in by main.cpp's mmc3::CallInBlock<level_code_tag>,
    // which then holds window 1 for the session -- this loop doesn't return in
    // ordinary play. Window 2's own content is chosen at runtime by level
    // index; see banks.hpp's ::LevelDataBank / level::LoadLevel.
    LEVEL_CODE void main() {
        InColdBank(EnterLevelSetup);
        irq::EnableInterrupts();
        // ReSharper disable once CppDFAEndlessLoop
        while (!quit) {
            input::PollControllers(&port1, &port2);
            if (port1 & input::START && (port1 ^ lastPort1) & input::START) {
                paused = !paused;
            }

            // Both players (and future NPCs) update behind ONE farcall into
            // the actors bank, not one window switch per actor.
            ppu::SetColorPriority(0x40);   // green band:        UpdateActors
            mmc3::Call<level::UpdateActors>();
            ppu::SetColorPriority(0x80);   // blue band:         ColMapTrack (slides override internally)

            ColMapTrack(cameraX >> 4);

            ppu::SetColorPriority(0x20);   // red band:          Update
            mmc3::CallInBlock<audio_tag>([] {
                mmc3::CallInBlock<audio_data_tag>([] { audio::Update(); });
            });
            ppu::SetColorPriority(0);

            video::WaitForPresent();
        }
    }

    // ::FIXED, not banked: an interrupt fires with whatever banks happen to
    // be mapped, so anything a vector can reach has to live somewhere no
    // register can displace.
    FIXED void nmi_handler() {
        lastPort1 = port1; lastPort2 = port2;
        oam::RefreshSprites(OAMBuffer);

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
            for (u8 i = 0; i < CoinVramLen; i += 3)
                ppu::WriteSingleToNameTable((CoinVram[i] << 8) | CoinVram[i + 1], CoinVram[i + 2]);
        }
        CoinVramReset();

        if (!suppressNmiScroll) ppu::SetScroll({0, 0});
        if (levelStreamCommand & STREAM_LEVEL_DONE) {
            levelStreamCommand = {};
        }

        ppu::SetColorPriority(0);
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
            const i16 raw    = static_cast<i16>(player1.actor.screen.y) - mid;
            const i16 bump   = raw < 0 ? 0 : (raw > anchor ? anchor : raw);
            ppu::SetScroll({cameraX, static_cast<u16>(16 + bump)});
        #else
            ppu::SetScroll({cameraX, kHudSplitRow});
        #endif
    }
}
