#pragma once

#include <cstddef>
#include <intsh>

#ifdef TARGET_GC
// card_stat, which is what ::save::GetMetaData / ::save::SetMetaData move. They
// take `void*` rather than naming it, so that this header declares the same two
// functions on every target without a non-GameCube build having to find
// ogc/card.h -- but a GameCube application has to be able to DECLARE one, so
// the type comes in here rather than leaving every call site to include it.
#include <ogc/card.h>
#endif

#include "technology.hpp"

using namespace br0::intsh;

namespace save {

#ifdef TARGET_GC
#define OGC_COMMENT(name) char name [32]
#define OGC_ICON_CI(name) u8 name[1536]
#define OGC_BANNER_CI(name) u8 name[3584]
#endif

    enum eError : u8 {
        found,
        missing,
        no_device,
#ifdef TARGET_GC
        busy,
        exists,
        full,
        too_large,
        denied,
        bad_path,
        failed,
#endif
    };

    /**
     * @brief The outcome of a ticked save operation.
     *
     * ::done AND ::err ANSWER DIFFERENT QUESTIONS, and conflating them is a
     * hang rather than a wrong answer: ::done says the operation is over, ::err
     * says whether it worked. A call that has not finished reports neither, so
     * `while (!response.done)` is the loop and `if (!response.err)` is the
     * test. Both exist on every target, including the NES, so that a game
     * writes one flow with no `#ifdef` in it -- they cost one byte each and
     * fold away entirely where a backend answers with constants.
     */
    struct Response {
        bool done;
        eError err;
    #ifndef TARGET_NES
        /**
         * @brief Where the save is, or nullptr when there is none.
         *
         * Borrowed from the backend and valid only until the next
         * ::GetSaveLocation call -- copy it to keep it. NOT PRESENT ON NES,
         * where a battery cart's save has no name and no device to be on.
         */
        const char* path;
#endif
    };

    /*
     * NO ::AI ON THESE, AND THAT IS LOAD-BEARING.
     *
     * Every one of them is DEFINED OUT OF LINE, in whichever src/<platform>/
     * save.cpp the build selected. `always_inline` on a declaration whose body
     * the compiler cannot see is not a hint -- GCC REJECTS IT OUTRIGHT:
     *
     *   error: inlining failed in call to 'always_inline'
     *          'save::Save(const void*, std::size_t)': function body not available
     *
     * ...which breaks every call site in a build without LTO to make the body
     * visible. That is every Debug config (CMakeLists.txt enables IPO for
     * Release/MinSizeRel/RelWithDebInfo only) and the whole PS3 target, which
     * disables it deliberately. Clang merely ignores the attribute, so the
     * problem is invisible on the NES, PS3 and desktop and fatal on the
     * devkitPPC / devkitARM / devkitA64 / PSP / PS2 toolchains.
     *
     * NOTHING IS LOST BY REMOVING IT. On the NES, where these fold to a
     * constant and the fold actually matters, llvm-mos compiles to LTO bitcode
     * BY DEFAULT -- no -flto anywhere in the flags -- and the NES link hands
     * every demo .cpp to one whole-program invocation. The backend's body is
     * therefore visible at link time either way, and a function whose whole
     * body is `return { true, no_device }` is inlined on its merits at -Os
     * without being ordered to be. A measured `while (!...done)` loop around
     * one of these compiles to nothing at all.
     *
     * The convention this follows is video.hpp's: ::video::Is50Hz() is
     * `AI inline` because its body IS in that header, while
     * ::video::DetectRefresh() is declared plain because its body is not.
     * ::AI belongs on a definition, not on a promise (technology.hpp).
     */
    Response SetMetaData(const void* src);
    Response GetMetaData(void* tar);
    /*
     * `void*` AND NOT `u8*`, WHICH IS ABOUT THE CALL SITE AND NOT ABOUT THIS
     * FILE. A save is a struct -- the demo's SaveFile, with its version, its
     * comment block and its icon -- and every one of these used to be reached
     * through a reinterpret_cast at the point of call:
     *
     *     save::Load(reinterpret_cast<u8*>(&saveData), sizeof saveData, &loc);
     *
     * That cast bought nothing. It did not make the bytes any more
     * byte-shaped, it is the one cast C++ asks a reader to be suspicious of,
     * and it appeared in every game that ever saved anything. `void*` converts
     * implicitly, so the call site is `save::Load(&saveData, sizeof saveData)`
     * and the reinterpret_cast lives once, in the backend, where it is about
     * the staging buffer rather than about the caller's struct.
     *
     * WHAT IT COSTS is the compiler's last objection to a wrong argument:
     * `void*` swallows anything with an address, so passing a pointer to a
     * pointer, or a type whose copy means something, now compiles. @p size is
     * what it was always going to be measured against -- sizeof the object the
     * caller means -- and these move RAW BYTES to a medium with no idea what
     * is in them, so a type that cannot survive a memcpy could not be saved
     * through the old signature either.
     */
    Response Save(const void* src, std::size_t size);
    Response Load(void* tar, std::size_t size);
    Response Load(void* tar, std::size_t size, const Response* response);
    Response CreateSave(const void* src, std::size_t size, const Response* response);
    Response GetSaveLocation();

    /**
     * @brief Which save devices are ACCESSIBLE right now, as a bitfield.
     *
     * Bit `n` is set when device `n` holds media this platform can save to --
     * the same index ::GetDeviceStatus takes, so bit 0 is the GameCube's slot A
     * and bit 1 its slot B. Zero means there is nowhere to save at this
     * instant, which is also the permanent answer on every target still on the
     * weak backend in src/all/save.cpp.
     *
     * A MASK AND NOT A COUNT, because a count cannot answer the question a
     * caller actually has. "Two slots" does not say which of the two a save can
     * go to, and a game that learnt a number would have to ask about every
     * index anyway to find out. One call, one byte, and
     * `if (AccessibleDevices() & 0b10)` is the whole of "can I use slot B".
     *
     * THE NUMBER OF SLOTS IS NOT HERE AT ALL, deliberately. It is not a useful
     * quantity on its own: a UI drawing one row per slot walks indices and lets
     * ::GetDeviceStatus report ::no_device past the last one, which is the same
     * answer an empty slot gives and needs no special case.
     *
     * A CLEAR BIT DOES NOT SAY WHY, and that is ::GetDeviceStatus's job --
     * empty, unreadable, unusable by this backend, or simply NOT SETTLED YET.
     * That last one matters: a card inserted a moment ago needs time on the bus
     * and reads as inaccessible until it has had it, so the bit flips on its own
     * between one poll and the next with nothing having been inserted. Poll it
     * the way a UI polls anything -- a few times a second, not every frame --
     * and see ::GetDeviceStatus for what the cost of a poll actually is.
     */
    u8 AccessibleDevices();

    /**
     * @brief Whether save device @p index holds usable media, and where a save
     *        on it would live.
     *
     * THE ONE CALL HERE THAT IS NOT ABOUT A SAVE. Everything else in this
     * header answers "where is my data / did it get there"; this answers "is
     * there anywhere on device @p index to put it", which a game needs BEFORE
     * it has a save to ask about -- a slot picker, a "please insert a memory
     * card in slot B" prompt, a greyed-out row.
     *
     * @p index is 0-based and names the same device ::AccessibleDevices sets bit
     * `index` for: 0 and 1 are slots A and B on the GameCube, and anything else
     * reports ::no_device rather than being refused as a mistake -- which is
     * what lets a caller walk indices until it sees one, with no slot count to
     * ask for. The outcomes:
     *
     *   - `done`, ::found      -- media is present AND this backend can use
     *                             it. `path` names where a save on this device
     *                             goes, ready to hand to ::CreateSave.
     *   - `done`, ::no_device  -- nothing usable: an empty slot, a non-card
     *                             device in it (an SD adapter booting the
     *                             game, say), a card whose directory structure
     *                             is broken, a card formatted for another
     *                             font encoding, or an @p index past the last
     *                             one. See below.
     *   - `done`, ::too_large  -- media is there and this backend cannot
     *                             transfer its sector width.
     *   - NOT `done`, ::busy   -- the answer is not known YET. Ask again next
     *                             frame; see below.
     *
     * TICKED LIKE EVERYTHING ELSE, AND FOR A REAL REASON RATHER THAN
     * SYMMETRY. A card inserted while the game is running takes time to settle
     * on the bus, and the probe reports "busy" until it has -- so a UI that
     * asked once and believed the first answer would show an empty slot for as
     * long as it kept that answer. `while (!response.done)` works as it does
     * everywhere here; a UI that must not block instead re-asks on its own
     * cadence and keeps drawing the last `done` answer it got.
     *
     * IT IS A POLL, NOT A SUBSCRIPTION, and it is also how REMOVAL is noticed:
     * nothing calls back when a card is pulled out between saves. Once every
     * half-second or so is plenty -- each call is a bus transaction, and no
     * card is mounted for it, so this is cheap in a way that nothing else in
     * this header is.
     *
     * `path` IS BORROWED AND OVERWRITTEN BY THE NEXT CALL, like
     * ::GetSaveLocation's -- copy it to keep it -- and it is the only way a
     * portable caller can name a device at all, since the file name a save
     * carries is the build's business and not exposed anywhere else. NOT
     * PRESENT ON NES, where save::Response has no `path` member.
     *
     * A BROKEN CARD IS REPORTED AS NO DEVICE, DELIBERATELY, and there is no
     * ::corrupt in this vocabulary to report it with. The reasoning is that
     * nothing a GAME can do about a damaged card is worth a vocabulary entry:
     * the only repair is a format, which destroys every other game's saves and
     * belongs to the console's own card manager, and a game that cannot save is
     * in the same position whether the slot is empty, holds an SD adapter the
     * game booted from, or holds a card whose directory is unreadable. All of
     * them mean "not somewhere I can save", and a player is served better by
     * "no memory card available" than by a diagnosis they cannot act on.
     *
     * TWO CONSEQUENCES WORTH KNOWING. A card formatted by a Japanese console
     * and read on a US/European one (or the reverse) is also folded in here --
     * it is undamaged, and it is still not somewhere this console can write. And
     * a backend cannot learn any of this from a probe: being broken is only
     * discoverable by MOUNTING, so a slot reads as usable until something
     * mounts it and finds out, after which this call reports it as no device
     * until the card is removed. A sweep by ::FreeDevices is the cheapest way
     * to make that discovery happen on purpose.
     *
     * IT DOES NOT SAY WHETHER THIS GAME'S SAVE IS THERE. ::found means "a
     * card, usable"; whether it already holds a save is ::GetSaveLocation's
     * question, and the two together are what a row like "SLOT A: SAVE FOUND"
     * / "SLOT B: CARD, NO SAVE" / "SLOT B: EMPTY" is made of.
     */
    Response GetDeviceStatus(u8 index);

    /**
     * @brief Which devices could actually TAKE a save of @p size bytes, as a
     *        bitfield.
     *
     * The same bits as ::AccessibleDevices, narrowed by capacity: bit `n` is
     * set when device `n` holds usable media AND a write of @p size bytes to it
     * would have somewhere to go. It is always a subset -- a device that is not
     * accessible cannot be free -- so ::AccessibleDevices answers "where could I
     * save at all" and this one answers "where could I save THIS".
     *
     * A DEVICE THAT ALREADY HOLDS THIS GAME'S SAVE COUNTS AS FREE when that
     * save is big enough to hold @p size, even on a medium with nothing left.
     * Overwriting a file that already exists consumes no new space, so a card
     * with zero room and a large enough save on it is somewhere a save WILL
     * succeed -- and a UI that greyed it out would be refusing the most ordinary
     * save there is.
     *
     * IT IS NOT FREE THE WAY ::AccessibleDevices IS. Free space cannot be
     * probed: the backend has to MOUNT each candidate to ask, which is the
     * slowest thing a memory card does. So this runs as a SWEEP spread over
     * several calls -- each call advances it by one step and returns the mask
     * from the last sweep that FINISHED, never blocking and never mounting more
     * than one device per call. Call it the way a UI redraws, every frame or
     * every few; the answer settles after a few of them and then costs nothing
     * until something invalidates it.
     *
     * ZERO MEANS "NOWHERE, OR NOT YET", and the two are not distinguished. A
     * first call always returns 0 because no sweep has finished; so does a
     * genuinely full pair of cards. A caller that must tell them apart watches
     * for the mask to settle, or asks ::GetDeviceStatus about the slot it cares
     * about. The same is true after anything that could change the answer -- a
     * card inserted or removed, a different @p size, a save or a create that
     * completed -- each of which quietly starts a fresh sweep.
     *
     * A SAVE IN PROGRESS OUTRANKS THE SWEEP. The sweep is a cache refresh with
     * no claim on the card, so a ::Save, ::Load or ::CreateSave that starts
     * while one is running simply abandons it and proceeds; the next call here
     * starts a new one. Nothing has to be sequenced by the caller.
     *
     * @p size IS MEASURED THE WAY ::Save MEASURES IT -- the prefix a call would
     * write, not the file's length on the medium. A @p size of 0, or one no
     * backend could write at all, reports no devices rather than all of them.
     */
    u8 FreeDevices(std::size_t size);

    /**
     * @brief The BACKEND'S OWN code for the last thing that went wrong, for a
     *        developer rather than for a player.
     *
     * save::eError is deliberately small -- a game acts on "there is nowhere to
     * save", not on which of eleven libogc codes produced that. This is the
     * other half of that trade: the platform's raw code, unmapped, for a log
     * line or a watch window.
     *
     * NOT PART OF THE CONTRACT. The VALUE is whatever the platform uses (on the
     * GameCube, one of libogc's CARD_ERROR_*, which are negative), so nothing
     * portable can switch on it and no backend promises any particular number.
     * ::i32 rather than libogc's own `s32`, which is the same width but exists
     * only on a GameCube build -- this declaration is compiled on every target.
     * Zero means "nothing has gone wrong" on every backend, which is also what
     * a platform with no save device answers forever.
     *
     * It describes the last FINISHED operation, so read it after `done`. A
     * status poll never sets it: ::GetDeviceStatus and ::AccessibleDevices
     * deliberately leave it alone so that a menu refreshing itself cannot
     * overwrite what a failed save left behind.
     */
    i32 LastNativeError();
}