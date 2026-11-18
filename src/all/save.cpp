/**
 * @file save.cpp
 * @brief The weak "no save device here" answer every target links against
 *        until a real backend overrides it.
 *
 * WHY THIS EXISTS. pnes/save.hpp declares the save API on EVERY target, so a
 * game writes one flow with no #ifdef in it -- but only two backends exist
 * (src/ogc/save.cpp for the GameCube memory card, and the MMC3 battery path in
 * src/nes/mappers/mmc3.cpp). Without this file the other eleven platform
 * families would compile a call to ::save::Save perfectly happily and then
 * fail at LINK with an undefined symbol, which is the worst place to find out:
 * a portable game is correct, and the error names a mangled symbol in a library
 * the author did not write.
 *
 * WEAK, NOT A #ifdef CHAIN. The same weak/strong arrangement
 * ::video::DetectRefresh already uses (src/all/video.cpp:54 is the weak false,
 * and src/nes, src/ogc, src/wiiu and src/ps3 each define a strong one): a
 * backend that can really save defines these NON-weak in its own translation
 * unit and the linker prefers it, so nothing here has to know which targets
 * those are. Adding a backend means writing it, not editing this file.
 *
 * save::no_device, NOT save::missing OR save::found, and the distinction
 * matters to a player rather than to a programmer:
 *   - no_device  -- "there is nowhere to save on this system". True here.
 *   - missing    -- "there is somewhere, and this game's save is not on it",
 *                   which would invite a game to offer to CREATE one, and
 *                   ::CreateSave would then refuse it too. A loop of "no save
 *                   found, shall I make one?" / "no".
 *   - found      -- would tell a game its data is safe. It is not; there is
 *                   nowhere for it to have gone.
 * It is also one of the three codes save.hpp declares unconditionally -- every
 * code past ::no_device is gated behind TARGET_GC -- so it is the most specific
 * answer this file is able to give on an arbitrary target.
 *
 * `done` IS TRUE ON EVERY ONE, and that is not decoration. A caller's
 * `while (!response.done)` loop has nothing to wait for and must terminate on
 * its first turn; `{ }` would zero-initialise `done` to false and hang forever.
 * The `path` member (absent on NES) is left null by the designated initialisers,
 * which is what "there is no save and nowhere for one to be" means.
 */
#include <pnes/save.hpp>

namespace save {

__attribute__((weak)) Response Save(const u8*, const std::size_t) { return { .done = true, .err = no_device }; }
__attribute__((weak)) Response Load(u8*, const std::size_t) { return { .done = true, .err = no_device }; }
__attribute__((weak)) Response Load(u8*, const std::size_t, const Response*) { return { .done = true, .err = no_device }; }
__attribute__((weak)) Response CreateSave(const u8*, const std::size_t, const Response*) { return { .done = true, .err = no_device }; }

// A finished search with nowhere to have looked.
__attribute__((weak)) Response GetSaveLocation() { return { .done = true, .err = no_device }; }

// The GameCube's directory entry has no counterpart anywhere else: these two
// move a card_stat, which only a memory card has. A target with no card has
// nothing to read and nothing to state.
__attribute__((weak)) Response GetMetaData(void*) { return { .done = true, .err = no_device }; }
__attribute__((weak)) Response SetMetaData(const void*) { return { .done = true, .err = no_device }; }

}
