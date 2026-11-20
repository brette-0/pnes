#pragma once

#include <pnes/save.hpp>

// See the matching guard in pregame.cpp for why this is ::SAVE_DEVICE and not
// `#ifndef TARGET_NES`: the mode needs a save device that can be absent, and
// the macro (pnes/save.hpp) is the one list of the targets that have one --
// the GameCube, the Wii and the Switch.
#ifdef SAVE_DEVICE

constexpr auto retryText        = ::charmap_generic.encode("RETRY");
constexpr auto continueText     = ::charmap_generic.encode("CONTINUE");
constexpr auto yesText          = ::charmap_generic.encode("YES");
constexpr auto noText           = ::charmap_generic.encode("NO");
constexpr auto eraseText        = ::charmap_generic.encode("ERASE ALL");
// THE TWO DEVICE LABELS, AND THE ONLY PART OF THIS WHEEL THAT IS PER CONSOLE.
// ::Card1 / ::Card2 are device indices 0 and 1 (demo/src/save.hpp), and what
// sits at those indices is what changes: slots A and B on the GameCube, the
// front SD slot and USB mass storage on the Wii, and on the Switch index 0
// alone. Same wheel, same indices, the player's own words for what they are
// putting their save on.
#ifdef TARGET_GC
constexpr auto card1Text        = ::charmap_generic.encode("CARD 1");
constexpr auto card2Text        = ::charmap_generic.encode("CARD 2");
#elif defined(TARGET_NX)
// ONE DEVICE, AND ::card2Text LABELS AN INDEX THAT DOES NOT EXIST HERE. The
// wheel these label is only drawn where the player picks a device
// (::SAVE_DEVICE_CHOICE) and two are accessible at once, neither of which is
// true on this console -- so these are the words a Switch player would see if
// it ever did, and saying "SD CARD" twice is a less wrong thing to show than
// naming hardware the console has no port for. See demo/src/save.hpp's note on
// ::Card2.
constexpr auto card1Text        = ::charmap_generic.encode("SD CARD");
constexpr auto card2Text        = ::charmap_generic.encode("SD CARD");
#else
constexpr auto card1Text        = ::charmap_generic.encode("SD CARD");
constexpr auto card2Text        = ::charmap_generic.encode("USB DRIVE");
#endif

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
     * NOT PRESENT ON NES. A battery cart has no device to find, nothing to
     * choose between and no metadata to lay out -- the MMC3 backend answers
     * every save call with ::save::found and the save simply IS the cart -- so
     * an NES build boots to the title directly and none of this is compiled.
     * The Wii runs all of it: it has two devices that may each be absent, which
     * is the situation this mode exists for. So does the Switch, with one
     * device that may be absent -- which is the same situation minus the
     * choice, and the flow handles it by never drawing the wheel.
     */
    void main();

    /**
     * @brief Writes ::saveData, shows SAVING while it goes where that is worth
     *        showing, and offers RETRY / CONTINUE if it fails.
     *
     * The other half of this mode, for the other end of the session: ::main
     * finds or creates the save before anything has been played, and this
     * writes it back when the player has changed something worth keeping. It
     * is a FUNCTION AND NOT A MODE because it is drawn over whatever mode
     * called it -- it borrows this mode's scene, its frame pump and its NMI
     * handler, points the camera at the bottom-right quadrant the scene lives
     * in, and gives the NMI vector back on the way out.
     *
     * Blocks until the player is done with it, so it must be called from a
     * MAINLINE and never from a handler. See its definition in pregame.cpp for
     * what it does and does not restore.
     */
    bool SaveScreen();

#ifdef TARGET_GC
    extern const OGC_BANNER_CI(banner);
    extern const OGC_ICON_CI(icon);
    extern const char gameName[32];
    extern const char gameDesc[32];
#endif
}

#endif // SAVE_DEVICE
