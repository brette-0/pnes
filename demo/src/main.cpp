#include <pnes>
#include <pnes/apu.hpp>
#include <pnes/audio.hpp>
#include <pnes/mappers/mmc3.hpp>
#include "header.hpp"
#include "main.hpp"

#include "modes/level.hpp"
#include "modes/title.hpp"
#include "modes/world.hpp"
#include "banks.hpp"
#include <pnes/logger.hpp>


u8 scratchpad[5];

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
    log("RESET: audio::Init starting");
    mmc3::CallInBlock<audio_tag>([] {
        mmc3::CallInBlock<audio_data_tag>([] {
            audio::Init(REGION);
        });
    });
    log("RESET: audio::Init returned");

    while (!quit) {
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

