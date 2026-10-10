#include <platform-nes>
#include <platform-nes/apu.hpp>
#include <platform-nes/audio.hpp>
#include <platform-nes/mappers/mmc3.hpp>
#include "header.hpp"
#include "main.hpp"

#include "modes/level.hpp"
#include "modes/title.hpp"
#include "modes/world.hpp"
#include "banks.hpp"
#include "platform-nes/logger.hpp"


u8 scratchpad[5];

// ReSharper disable once CppUseAuto
atomic eGameModes gameMode = eGameModes::Title;

SYSMEM oam::sprite_t OAMBuffer[64] __attribute__((aligned(256)));

void (*pNMI)();
void (*pIRQ)();

NMI(FIXED) {
    pNMI();
}

IRQ(FIXED) {
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

