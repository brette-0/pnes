#pragma once
#include <pnes/pnes.hpp>

#include "level/player.hpp"
#include "../main.hpp"

namespace level {
    extern bool paused;



    enum class eLevelStreamCommands : u8 {
        STREAM_LEVEL_LEFT  = 0x00,
        STREAM_LEVEL_RIGHT = 0x01,
        STREAM_LEVEL_DONE  = 0x02,
        STREAM_LEVEL_SWAP  = 0x04,
    };

    extern u8 port1;
    extern u8 port2;

    extern u8 lastPort1;
    extern u8 lastPort2;

    extern u16 cameraX;

    extern i8 lastDeltaScroll;

    extern atomic u16 lastXWorldSpace;

    extern u8 TileBuffer[56];

    extern Player player1;
#ifdef PLAYER2_SUPPORTED
    extern Player player2;

    // Runtime switch, owned by level mode: whether player2 is actually in
    // play this session. Distinct from PLAYER2_SUPPORTED (a compile-time
    // capability gate) -- this is the session-level choice, e.g. from a
    // future title-screen menu. Unwired for now, so it stays false and only
    // player1 is ever active.
    extern bool multiplayer;
#endif

    extern atomic tech::enum_flags<eLevelStreamCommands> levelStreamCommand;

    // Deferred VRAM write queue (coin pickups -> NMI drain).
    // Pushing is done by the player logic; draining is done by the NMI.
    void CoinVramPush(u16 address, u8 value);

    // Updates the whole actor roster (both players today, NPCs later) in one call.
    // ::ACTORS-tagged (player.cpp) -- reached via mmc3::Call<UpdateActors>, never
    // called directly. See banks.hpp's ::actor_tag.
    void UpdateActors();

    // Runs the level-playing mode: bank setup, level load, and the gameplay
    // loop. Called from the top-level mode dispatch while gameMode == Level.
    void main();

    // Called (as an ordinary C++ function, not a raw vector jump) from
    // main.cpp's nmiTrampoline/irqTrampoline whenever gameMode == Level.
    void nmi_handler();
    void irq_handler();
}
