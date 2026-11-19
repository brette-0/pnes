#pragma once

#include <pnes/save.hpp>

// See the matching guard in pregame.cpp for why this is TARGET_GC and not
// `#ifndef TARGET_NES`: the mode's whole vocabulary is memory-card-shaped.
#ifdef TARGET_GC

constexpr auto retryText        = ::charmap_generic.encode("RETRY");
constexpr auto continueText     = ::charmap_generic.encode("CONTINUE");
constexpr auto yesText          = ::charmap_generic.encode("YES");
constexpr auto noText           = ::charmap_generic.encode("NO");
constexpr auto eraseText        = ::charmap_generic.encode("ERASE ALL");
constexpr auto card1Text        = ::charmap_generic.encode("CARD 1");
constexpr auto card2Text        = ::charmap_generic.encode("CARD 2");

// user rtTextBoxes to draw this text, its GC so its compute is OK

namespace pregame {
    /**
     * @brief Finds, loads or creates the save, then hands over to the title.
     *
     * The FIRST mode off the NES (main.cpp's ::gameMode starts here), because
     * every other mode is entitled to assume ::saveData means something. It
     * sets ::gameMode to eGameModes::Title on its way out, so main.cpp's
     * dispatch loop goes straight on to the title screen.
     *
     * NOT PRESENT ON NES. A battery cart has no device to find, no card to
     * choose and no metadata to lay out -- the MMC3 backend answers every save
     * call with ::save::found and the save simply IS the cart -- so an NES
     * build boots to the title directly and none of this is compiled.
     */
    void main();

#ifdef TARGET_GC
    extern const OGC_BANNER_CI(banner);
    extern const OGC_ICON_CI(icon);
    extern const char gameName[32];
    extern const char gameDesc[32];
#endif
}

#endif // TARGET_GC
