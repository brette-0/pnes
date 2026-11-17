#include <pnes>
#include <pnes/apu.hpp>
#include <pnes/audio.hpp>
#include <pnes/mappers/mmc3.hpp>
#include "header.hpp"
#include "main.hpp"

#include "modes/level.hpp"
#include "modes/title.hpp"
#include "modes/world.hpp"
#include "../ui/impl/sounds.hpp"
#include "banks.hpp"
#include <pnes/logger.hpp>


u8 scratchpad[5];

atomic u16 frameCounter = 0;

// ReSharper disable once CppUseAuto
atomic eGameModes gameMode = eGameModes::Title;

SYSMEM oam::sprite_t OAMBuffer[64] __attribute__((aligned(256)));

void (*pNMI)();
void (*pIRQ)();

// The ::mmc3::select_latch_scope in each body is NOT optional -- see its
// declaration (pnes/mappers/mmc3.hpp). Both handlers bank CHR (the HUD/menu
// raster split), the mainline banks PRG constantly (every farcall trampoline),
// and MMC3 reaches all of it through one $8000/$8001 select/data port pair. An
// interrupt taken between a writer's two stores re-latches $8000 under it, and
// the writer's $8001 store then hits the wrong register: a PRG bank number
// lands in a CHR register while the PRG window keeps its stale bank, so the
// graphics go to garbage AND the farcall `jsr`s into whatever that stale bank
// holds, executing data until the CPU jams.
//
// THESE ARE THE ONLY TWO VECTORS IN THE ROM -- every mode's handler is reached
// through pNMI/pIRQ -- so guarding here covers level, title and world at once,
// and any mode added later inherits it. That is why the guards belong at the
// vector and not in the individual handlers.

NMI(FIXED) {
    const mmc3::select_latch_scope latch;

    // ::frameCounter is stepped HERE, for the same reason the latch guard is:
    // this is the only NMI vector in the ROM, so one increment covers level,
    // title and world at once and no mode's handler can forget it. Stepped
    // before the dispatch, so a handler reading it sees its own frame's value
    // rather than the previous one's.
    //
    // Loaded and stored through a local rather than `+= 1`: ::frameCounter is
    // `atomic`, which is `volatile` on every C++ target (technology.hpp), and
    // compound assignment to a volatile lvalue is deprecated in C++20 --
    // -Wvolatile warns on it. This is the same two accesses in the same order.
    const u16 frame = static_cast<u16>(frameCounter + 1);
    frameCounter = frame;

    pNMI();
}

IRQ(FIXED) {
    const mmc3::select_latch_scope latch;
    pIRQ();
}

RESET {
    log("RESET: entered");
    apu::DisableFrameIRQ();
    apu::DisableDMCIRQ();

    // One-shot audio device/asset setup (on native backends this decodes
    // and buffers the whole music + SFX library). Must run exactly once at
    // boot, not per level -- see level.cpp's EnterLevelSetup for why it used
    // to live there and what that cost.
    //
    // The effect table is registered BEFORE that, not after: the backends that
    // decode their effects do it inside audio::Init, reading whatever has been
    // registered by then, and no later call makes them go back
    // (include/pnes/audio.hpp). A no-op on NES, where the bank is a ROM symbol
    // the backend names itself -- which is why there is no #if here.
    ui::impl::RegisterSfx();

    log("RESET: audio::Init starting");
    mmc3::CallInBlock<audio_tag>([] {
        mmc3::CallInBlock<audio_data_tag>([] {
            audio::Init(FRAMERATE);
        });
    });
    log("RESET: audio::Init returned");

    while (!quit) {
        log("RESET: %d", gameMode);
        switch (gameMode) {
            case eGameModes::Level:
                log("RESET: entering level::main");
                mmc3::CallInBlock<level_code_tag>(level::main);
                continue;

            case eGameModes::World:
                log("RESET: entering world::main");
                world::main();
                continue;

            case eGameModes::Title:
                log("RESET: entering title::main");
                mmc3::CallInBlock<title_tag>(title::main);
        }
    }
}

