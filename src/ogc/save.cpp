/**
 * @file save.cpp
 * @brief Memory-card save/load/create/locate for the GameCube (libogc CARD_*),
 *        and the FAT-volume backend that does the same job on the Wii.
 *
 * TWO CONSOLES, TWO HALVES, ONE FILE, because they share nothing but this
 * header comment: the GameCube's save is an EXI memory card reached through
 * CARD_*, the Wii's is a file on the SD card or a USB disk reached through
 * libfat, and a console has exactly one of those. The split is the
 * #ifdef TARGET_GC below, and the Wii half opens with its own header comment
 * covering what differs -- it is blocking where this half is ticked, and its
 * search is path-based where this half's is identity-based.
 *
 * WHAT FOLLOWS, UNTIL THE #else, IS THE GAMECUBE.
 *
 * EVERY ENTRY POINT IS A TICKED STATE MACHINE, not a blocking call: each call
 * advances the sequence by exactly one libogc step and returns `false`, until
 * the step that completes it returns `true` once. A call site drives it the
 * way it drives anything else spanning frames --
 *
 *     while (!save::Save(&state, sizeof state)) { ...one frame... }
 *
 * -- and the same loop runs once through on a battery cart. What ticking buys
 * HERE is that video and audio keep running BETWEEN the card's own blocking
 * calls; it does not make any individual CARD_* call shorter, and CARD_Mount
 * is as slow as it ever was.
 *
 * THE SEARCH IS IDENTITY-BASED, WHICH IS WHY ::save::CreateSave CAN LET THE
 * CALLER NAME THE FILE. CARD_FindFirst with `showall = false` returns only the
 * entries "identified by the company and gamecode string" handed to CARD_Init,
 * so ::save::GetSaveLocation finds this game's save whatever it was called.
 * That pair is ::kGameCode / ::kCompany below, overridable as compile
 * definitions. A file created here is expected to inherit them from CARD_Init
 * -- libogc ships no card.c in this SDK install, so that could not be
 * confirmed from source; if a created save turns out not to be findable
 * afterwards, the fix is CARD_SetStatusEx on card_direntry's own gamecode /
 * company fields right after CARD_Create.
 *
 * @p size IS A PREFIX LENGTH, NOT A FILE SIZE. A card file's length is fixed
 * when it is created (CARD_Create: "size ... must be a multiple of the memory
 * card's sector size") and never changes, so a per-call size cannot describe
 * the file -- it describes how much of the file's first sector this call cares
 * about. ::save::Save writes @p size bytes at offset 0 and zero-fills the rest
 * of the sector; ::save::Load hands back the first @p size bytes. Saving 64
 * and loading 32 back is well defined and gives the first 32.
 *
 * THIS BACKEND RESERVES NOTHING. What a caller passes is the whole of what
 * reaches the card, starting at offset 0. The GameCube does keep a save's
 * comment block, banner and icon INSIDE the save file -- reached through
 * card_direntry's `comment_addr` / `icon_addr` -- but those are the
 * APPLICATION'S bytes to lay out and fill, declared as members at the front of
 * its own save struct. Nothing here places them, offsets past them, or has an
 * opinion about where they are; the one thing this file still owes them is a
 * CARD_SetStatus that tells the directory entry the offsets the application
 * chose, which does not exist yet.
 *
 * A PATH IS `"a:name"` or `"b:name"`: slot letter, colon, then at most 31
 * characters (CARD_FILENAMELEN is 32, terminator included). Anything else is
 * REJECTED rather than defaulted -- defaulting to slot A is how a save lands
 * silently on the wrong card.
 *
 * NO LOGGING, UNLIKE MOST OF THIS LIBRARY: log() resolves on this platform to
 * the generic debug branch of logger.hpp, which writes through std::cout and
 * depends on a console that src/ogc/video.cpp's GX framebuffer owns -- no
 * other TU in this backend prints anything. The real CARD_ERROR_* code lands
 * in ::lastNative instead, alongside the portable ::lastErr a caller is told
 * through save::Response -- a game acts on save::no_device, a developer still
 * sees whether it was CARD_ERROR_NOCARD or CARD_ERROR_WRONGDEVICE.
 */
#include "internal.hpp"
#include <pnes/save.hpp>

/*
 * THE TWO HELPERS BOTH CONSOLES SHARE, and the only code in this file that is
 * not one console's or the other's.
 *
 * WHY SO LITTLE IS SHARED, given the two halves do the same job: nearly every
 * helper in the GameCube half is a piece of a CARD_* state machine -- ::Begin,
 * ::Tick, ::Resolve, ::ProbeMedia, the ::Survey* sweep, ::AdvanceSlot,
 * ::Abort, ::Busy -- and a FAT volume has no asynchronous step to advance, so
 * there is nothing on the Wii side for them to be extended INTO. The parts
 * that could be shared are the ones that touch neither medium: a judgement
 * about a save::eError, and a question about a caller's save::Response.
 *
 * THEY LIVE HERE RATHER THAN IN EACH HALF because they had already drifted
 * once. Both halves ranked outcomes identically (one as ::Rank, one as
 * ::RankErr) and both unwrapped a Response's path, except that the
 * GameCube's rejected an EMPTY path and the Wii's did not -- so a
 * save::Response carrying "" meant "use the default location" on one console
 * and save::bad_path on the other, for the same portable call. One definition
 * is how that stops happening.
 */
namespace {

/**
 * @brief How much an outcome is worth saying, when several devices answer.
 *
 * Three tiers only, because that is all the distinction a caller can act on:
 * nothing was there, something was there but silent, or something answered.
 * Used by the GameCube's ::NoteSlotOutcome and by the Wii's searches, which
 * both keep the best answer any device gave and report it when none of them
 * held the save.
 *
 * save::busy IS UNREACHABLE ON THE WII (nothing there is ticked, so nothing can
 * report "not yet"), which costs the middle tier nothing: it simply never wins
 * a comparison on that console.
 */
constexpr u8 Rank(const save::eError err) {
    switch (err) {
        case save::no_device: return 0;   // nothing in this slot
        case save::busy:      return 1;   // something, but it did not answer
        default:              return 2;   // a real verdict about a real device
    }
}

/**
 * @brief The `path` a caller's save::Response names, or nullptr for none.
 *
 * AN EMPTY STRING IS NOT A PATH, and that is the half the Wii was missing.
 * save.hpp's flows hand a Response straight back from ::save::GetSaveLocation
 * or ::save::GetDeviceStatus, and a Response that found nothing carries a null
 * `path` -- but a caller that built one by hand, or zeroed a buffer and passed
 * that, carries "". Both mean "I am not naming a location", so both have to
 * reach the backend as nullptr: the GameCube then uses ::kDefaultPath (or the
 * remembered location), and the Wii uses ::kDefaultDevice. Treating "" as a
 * location instead meant the Wii refused it as save::bad_path -- a different
 * answer to the same call, which is exactly what a portable API cannot do.
 */
const char* PathOf(const save::Response* const response) {
    return (response && response->path && response->path[0]) ? response->path : nullptr;
}

} // namespace

#ifdef TARGET_GC

#include <ogc/card.h>
#include <ogc/cache.h>
#include <cstddef>
#include <cstring>

namespace {

/**
 * @brief The identity every save of this game carries on the card.
 *
 * NOT CARD_Init(NULL, NULL), which is legal but switches off the filtering
 * CARD_FindFirst needs: with no gamecode, a search either finds nothing or
 * finds every other game's saves. Four bytes of gamecode and two of company,
 * exactly, per the directory entry layout (card_direntry).
 *
 * Overridable as compile definitions rather than a CMake knob, because a
 * project that wants its own code says so once in its own build and nothing
 * here has to grow an option for it.
 */
#ifndef OGC_GAME_CODE
#define OGC_GAME_CODE "PNES"
#endif
#ifndef OGC_COMPANY_CODE
#define OGC_COMPANY_CODE "BR"
#endif
#ifndef OGC_FILENAME
#define OGC_FILENAME "pnes_save"
#endif
constexpr char kGameCode[] = OGC_GAME_CODE;
constexpr char kCompany[]  = OGC_COMPANY_CODE;
constexpr char kFileName[] = OGC_FILENAME;

/**
 * @brief Where ::save::CreateSave puts the save when the caller names nowhere.
 *
 * Literal concatenation, so the slot is SPELLED OUT rather than defaulted
 * inside ::ParsePath -- which still refuses a path that does not name one, for
 * the reason its own comment gives. "No path at all" and "a path that forgot
 * its slot" are different mistakes, and only the first has an obvious answer.
 *
 * Slot A because it is the one every GameCube has populated first and the one
 * a single-card player is using. A game that wants to offer the choice passes
 * a path and this is never consulted.
 */
constexpr char kDefaultPath[] = "a:" OGC_FILENAME;
static_assert(sizeof(kDefaultPath) == sizeof(kFileName) + 2, "kDefaultPath must be \"a:\" + the name");
static_assert(sizeof(kGameCode) == 5, "OGC_GAME_CODE must be exactly 4 characters");
static_assert(sizeof(kCompany)  == 3, "OGC_COMPANY_CODE must be exactly 2 characters");
static_assert(sizeof(kFileName) > 1 && sizeof(kFileName) <= CARD_FILENAMELEN,
    "OGC_FILENAME must be 1-31 characters (CARD_FILENAMELEN is 32, terminator included)");

/**
 * @brief Where this game's save goes on each slot, indexed as ::kSlots is.
 *
 * WHAT ::save::GetDeviceStatus HANDS BACK, and the only reason it has a `path`
 * to hand back at all: OGC_FILENAME is a PRIVATE compile definition of this
 * library (CMakeLists.txt sets it with target_compile_definitions(pnes
 * PRIVATE ...)), so nothing a game includes can spell "b:super_mary_sisters"
 * for itself. A caller that wants to create its save on the slot a player
 * picked therefore cannot name that slot -- unless the backend answers with a
 * complete, creatable path, which is what this table is.
 *
 * ::kDefaultPath IS slot A's entry and stays its own object: its static_assert
 * above is what checks the "a:" + name shape, and ::save::CreateSave reads it
 * by name.
 */
constexpr char kSlotBPath[] = "b:" OGC_FILENAME;
static_assert(sizeof(kSlotBPath) == sizeof(kFileName) + 2, "kSlotBPath must be \"b:\" + the name");
constexpr const char* kDevicePath[2] = { kDefaultPath, kSlotBPath };

/**
 * @brief How many memory-card slots the console has. Not a run-time quantity.
 *
 * BACKEND-INTERNAL, AND NOT PART OF THE API: both slots exist on every
 * GameCube ever made whether anything is inserted in them or not, so a slot
 * count says nothing a caller can act on -- ::save::AccessibleDevices answers
 * with a mask of the slots that are USABLE instead, and this is only how far
 * the loop that builds it counts.
 */
constexpr u8 kDeviceCount = 2;
static_assert(kDeviceCount <= 8, "::save::AccessibleDevices returns the mask in a u8");

/** @brief Largest card sector this file is prepared to transfer. */
constexpr u32 kSectorMax = 8192;

/**
 * @brief The buffer every transfer goes through.
 *
 * 32-byte aligned because CARD_Read/CARD_Write both document it, and private
 * because a caller should not have to know that. It is also what makes @p size
 * a prefix: the whole sector moves every time, zero-filled past the caller's
 * bytes, so no stale tail from an earlier, longer save survives.
 */
alignas(32) u8 staging[kSectorMax];

/**
 * @brief The card_stat a metadata operation works through.
 *
 * COPIED IN AND OUT RATHER THAN USED IN PLACE, for the same reason ::Begin
 * snapshots a save's payload up front: the sequence spans frames and the caller
 * is free to carry on using its own object meanwhile. It also keeps
 * ::save::SetMetaData's `const void*` honest -- CARD_SetStatus takes a
 * NON-const card_stat and writes back the offsets it derives from icon_addr
 * (__card_updateiconoffsets' whole job), so handing it the caller's object
 * would mean const_casting away a promise this backend made.
 */
card_stat metaStat;

/** @brief Where ::save::GetMetaData copies ::metaStat once it is filled. */
void* metaOut = nullptr;

/**
 * @brief Mount work area, owned for the life of the program.
 *
 * CARD_WORKAREA_SIZE is 5*8*1024 = 40960 bytes and the card system keeps using
 * it for as long as the card is mounted, so it cannot be a local -- and 40 KiB
 * of the GameCube's 24 MiB is better spent statically than on a heap
 * allocation at the moment a player asks to save.
 */
alignas(32) u8 workarea[CARD_WORKAREA_SIZE];

/** @brief What the in-flight sequence is doing, if anything. */
enum class Op : u8 { None, Saving, Loading, Creating, Finding, GettingMeta, SettingMeta, Surveying };

/**
 * @brief True for the two operations that move a card_stat, not file bytes.
 *
 * They share the whole sequence -- probe, mount, find-or-open, finish -- and
 * differ only in what the Transfer tick calls, which is why they are ops here
 * rather than a second state machine. What they do NOT share is the staging
 * buffer or @p size: a directory entry is not file contents and has no prefix.
 */
constexpr bool IsMeta(Op o) { return o == Op::GettingMeta || o == Op::SettingMeta; }

/** @brief One libogc step per tick, in the order they run. */
enum class Phase : u8 { Probe, Mount, Resolve, Transfer, Finish };

Op          op        = Op::None;
Phase       phase     = Phase::Probe;
s32         chn       = CARD_SLOTA;
u32         sector    = 0;
std::size_t pending   = 0;        ///< @p size as latched on the first tick.
u8*         dest      = nullptr;  ///< Load's @p tar, likewise latched.
card_file   file;
bool        mounted   = false;
bool        opened    = false;
bool        detached  = false;    ///< Set from the detach callback, off-thread.

/**
 * @brief The last outcome, in both currencies.
 *
 * ::lastErr is what a caller is told (save::eError, platform-neutral);
 * ::lastNative is libogc's own code, kept for a debugger and for whatever
 * diagnostic channel this backend eventually grows. Keeping both is the point:
 * a game can act on "no device", and a developer can still see that it was
 * CARD_ERROR_WRONGDEVICE rather than CARD_ERROR_NOCARD.
 */
save::eError lastErr    = save::found;
s32         lastNative = CARD_ERROR_READY;

/** @brief Slots to try, in the order a player expects them tried. */
constexpr s32 kSlots[2] = { CARD_SLOTA, CARD_SLOTB };
/** @brief Which of ::kSlots the sequence is on; 2 once they are exhausted. */
u8 slotIndex = 0;
/** @brief Set when a path pinned the slot, so no searching happens at all. */
bool slotFixed = false;

/**
 * @brief The most informative outcome this sweep has seen, and libogc's code
 *        for it.
 *
 * LAST SLOT WINS WAS A WRONG ANSWER, not just an imprecise one. A sweep used to
 * report whatever the final slot said, so the ordinary one-card console --
 * card in slot A, slot B empty -- answered a search like this:
 *
 *   slot A: mounted, CARD_FindFirst -> CARD_ERROR_NOFILE  ->  save::missing
 *   slot B: empty                                         ->  save::no_device
 *   reported: save::no_device
 *
 * ...which tells a game "there is no memory card" about a console with a
 * memory card in it, and sends it down the "ask for a card" branch instead of
 * the "offer to create a save" one. The informative answer was found first and
 * then thrown away.
 *
 * SO OUTCOMES ARE RANKED (::Rank) AND THE BEST ONE IS REPORTED. ::no_device is
 * the weakest -- it means "nothing is in this slot", which says nothing about
 * the console as a whole; ::busy is better, since something is there and merely
 * did not answer; anything else is a real verdict about a real card and wins.
 * Ties go to the EARLIER slot, so slot A's answer is the one a player hears
 * when both cards say the same thing.
 */
save::eError sweepErr    = save::no_device;
s32          sweepNative = CARD_ERROR_NOCARD;
bool         sweepSeen   = false;

/**
 * @brief Ticks spent waiting for one slot's probe to settle before giving up.
 *
 * A channel that answers CARD_ERROR_BUSY has not decided yet -- a card
 * inserted a moment ago, or a channel touched for the first time this frame
 * (which is how a two-card console reported ::busy for slot B immediately
 * after slot A's sweep). Treating that as an empty slot, which this did, loses
 * a card that was about to answer.
 *
 * CAPPED, because the alternative to giving up is a loop that never ends: a
 * channel wedged in BUSY would otherwise keep a `while (!response.done)`
 * caller pumping frames until the console was switched off. Sixty ticks is a
 * second of settling at 60Hz, far longer than an insertion needs, after which
 * the slot is reported save::failed -- an answer, and an honest one.
 */
constexpr u8 kProbeRetryLimit = 60;
u8 probeRetries = 0;

/**
 * @brief The last SETTLED answer ::save::GetDeviceStatus gave for each slot.
 *
 * TWO JOBS, AND THE SECOND IS THE ONE THAT NEEDED A CACHE AT ALL.
 *
 * The first is the refusal path: a status poll that arrives while a sequence
 * owns the card answers from here rather than probing, because this file cannot
 * prove a probe is safe mid-sequence -- libogc ships no card.c in this SDK
 * install (see this file's header), so whether CARD_ProbeEx can touch an EXI
 * channel another CARD_* call is partway through is unverifiable from source
 * rather than merely unlikely to matter. Answering with what was true a moment
 * ago is the conservative reading, and a card does not usually change slots
 * during the two seconds a save takes.
 *
 * The second is that ::save::busy means "NOT KNOWN YET" and nothing else. A
 * freshly inserted card reports CARD_ERROR_BUSY until it settles on the bus, so
 * a poll that has never had a settled answer must say so and be asked again --
 * and `done` false is how it says it. Once a slot HAS a settled answer, that is
 * what a refused poll returns, with `done` true: a UI redrawing its rows every
 * frame keeps the row it had instead of flickering to "checking..." for the
 * length of a save.
 *
 * ::deviceNative is the libogc code behind it, kept for the same reason
 * ::lastNative is -- these deliberately do NOT write ::lastErr / ::lastNative,
 * which belong to the in-flight sequence and would be the one thing a status
 * poll must never disturb.
 */
save::eError deviceErr[kDeviceCount]    = { save::no_device, save::no_device };
s32          deviceNative[kDeviceCount] = { CARD_ERROR_NOCARD, CARD_ERROR_NOCARD };
bool         deviceKnown[kDeviceCount]  = {};

/**
 * @brief Slots a mount has found unusable, so the probe-only calls can say so.
 *
 * CORRUPTION IS NOT PROBEABLE. CARD_ProbeEx answers "is there a card and how
 * wide are its sectors"; whether that card's directory structure survives is
 * only discoverable by CARD_Mount, which is the expensive operation
 * ::save::GetDeviceStatus exists to avoid. So the verdict is recorded HERE by
 * whoever paid for the mount -- a save, a load, or a ::save::FreeDevices sweep
 * -- and the cheap calls honour it afterwards.
 *
 * ONLY THE TWO HEALTH CODES SET IT (::IsUnusableCode): a mount that failed with
 * CARD_ERROR_BUSY or CARD_ERROR_NOCARD says nothing about the card, and marking
 * those would hide a perfectly good card behind a transient failure.
 *
 * IT IS CLEARED BY THE CARD LEAVING, or by a later mount succeeding. A probe
 * that reports no media means there is nothing in the slot to be broken, so the
 * flag goes; the limitation is a card swapped between two polls with no
 * observed gap, which keeps the old verdict until the next removal is seen. At
 * a UI's polling rate that window is a few frames wide.
 */
bool deviceBroken[kDeviceCount] = {};

/** @brief Mount failures that are verdicts about the card, not about timing. */
constexpr bool IsUnusableCode(s32 err) {
    return err == CARD_ERROR_BROKEN || err == CARD_ERROR_ENCODING;
}

/**
 * @brief The free-space sweep: what it is measuring, and what it has found.
 *
 * FREE SPACE CANNOT BE PROBED, which is the whole reason this exists as a
 * sweep rather than as two more EXI transactions beside ::RefreshDevice.
 * CARD_GetFreeBlocks reads a MOUNTED card's own bookkeeping, so answering "is
 * there room on slot B" costs a CARD_Mount -- the slowest thing this file ever
 * does, and the one it is ticked to keep out of a frame.
 *
 * So ::save::FreeDevices advances this by one step per call and hands back
 * ::freeMask, the result of the last sweep that FINISHED. The published answer
 * is only valid for the size it was measured against and the set of devices
 * that were accessible at the time -- hence ::freeMaskSize and
 * ::freeMaskAccess, which are compared on every call. A mismatch is what
 * silently starts a fresh sweep.
 *
 * ::surveySector IS NOT ::sector, deliberately. A sweep mounts cards while no
 * real operation owns the machine, and latching the save path's sector width
 * from here would leave a save reading a number a menu wrote.
 */
std::size_t surveySize   = 0;   ///< Size the in-flight sweep is measuring for.
u32         surveySector = 0;   ///< Current slot's sector width, sweep-local.
u8          surveyMask   = 0;   ///< Bits accumulated so far by the in-flight sweep.
u8          surveyAccess = 0;   ///< Accessible mask the in-flight sweep began with.

u8          freeMask       = 0;      ///< Last finished sweep's answer.
std::size_t freeMaskSize   = 0;      ///< The size that answer was measured for.
u8          freeMaskAccess = 0;      ///< The accessible mask it was measured against.
bool        freeMaskValid  = false;  ///< False until a sweep has finished.

/**
 * @brief CARD_Init once, for callers that arrive before any save sequence has.
 *
 * ::Begin calls CARD_Init on every sequence it starts, so nothing on the
 * save/load path needs this. ::save::GetDeviceStatus is the first entry point
 * in this file that touches the card system WITHOUT starting a sequence, and a
 * game polling slots on its title screen may well never have saved anything
 * yet -- so the identity pair has to be registered here too.
 *
 * GUARDED RATHER THAN REPEATED, unlike ::Begin's: this one is a poll that runs
 * for as long as a menu is open, and re-registering the same two strings
 * several times a second is work with nothing to show for it.
 */
void EnsureInit() {
    static bool initialised = false;
    if (initialised) return;
    CARD_Init(kGameCode, kCompany);
    initialised = true;
}

/** @brief File name the sequence is acting on, parsed from a path or found. */
char targetName[CARD_FILENAMELEN] = {};

/**
 * @brief The location of the save this game owns, once something has found it.
 *
 * "a:" or "b:" plus the name, which is what ::save::GetSaveLocation hands back
 * and what ::save::Save uses when it is given no path of its own. Lives here,
 * statically, because the API returns a borrowed pointer: the caller must be
 * able to hold it without owning it.
 */
char locationBuf[2 + CARD_FILENAMELEN] = {};
bool haveLocation = false;

/**
 * @brief Card-removal callback, registered with CARD_Mount.
 *
 * Runs from libogc's own EXI context, not from the tick, so it does the least
 * it can: raises a flag the next tick acts on. A sequence spanning frames makes
 * mid-sequence removal genuinely reachable, which is the one new failure mode
 * ticking introduces over a blocking implementation.
 */
void DetachCallback(s32 chan, s32 /*result*/) {
    if (chan == chn) detached = true;
}

/**
 * @brief Translates libogc's card error into the library's own code.
 *
 * THE WHOLE REASON save::eError EXISTS. CARD_ERROR_NOCARD is -3, which in a u8
 * would reach a game as 253; everything here maps onto a name a game can switch
 * on without knowing what a memory card is. Codes with no portable meaning
 * (CARD_ERROR_IOERROR, _FATAL_ERROR, _CANCELED, _UNLOCKED) land on save::failed,
 * with ::lastNative holding what actually happened.
 */
save::eError MapError(s32 err) {
    switch (err) {
        case CARD_ERROR_READY:       return save::found;
        case CARD_ERROR_BUSY:        return save::busy;
        case CARD_ERROR_NOCARD:
        case CARD_ERROR_WRONGDEVICE: return save::no_device;
        case CARD_ERROR_NOFILE:      return save::missing;
        case CARD_ERROR_EXIST:       return save::exists;
        // NO save::corrupt ANY MORE, AND THAT IS A POLICY RATHER THAN A
        // SHORTCUT. A card whose directory structure is broken, and a card
        // formatted for the other font encoding, are both media this console
        // cannot write to and neither is something a game can repair -- the
        // only repair is a format, which destroys other games' saves and
        // belongs to the console's card manager. So they join the empty slot
        // and the SD adapter the game may have booted from: not a device we
        // can save to. ::lastNative still holds which it was, for a developer.
        case CARD_ERROR_BROKEN:
        case CARD_ERROR_ENCODING:    return save::no_device;
        case CARD_ERROR_INSSPACE:
        case CARD_ERROR_NOENT:
        case CARD_ERROR_LIMIT:       return save::full;
        case CARD_ERROR_NOPERM:      return save::denied;
        case CARD_ERROR_NAMETOOLONG: return save::bad_path;
        default:                     return save::failed;
    }
}

/** @brief Offers one slot's outcome to the sweep; the best one is kept. */
void NoteSlotOutcome(const save::eError err, const s32 native) {
    // Strictly greater, so a tie leaves the EARLIER slot's answer in place.
    if (!sweepSeen || Rank(err) > Rank(sweepErr)) {
        sweepErr    = err;
        sweepNative = native;
        sweepSeen   = true;
    }
}

/** @brief Tears down whatever the sequence had open and returns it to idle. */
void Abort() {
    if (opened)  { CARD_Close(&file); opened = false; }
    if (mounted) { CARD_Unmount(chn); mounted = false; }
    op           = Op::None;
    phase        = Phase::Probe;
    pending      = 0;
    dest         = nullptr;
    slotIndex    = 0;
    slotFixed    = false;
    probeRetries = 0;
}

/**
 * @brief Records an outcome and aborts; always false, for `return Fail(...)`.
 *
 * @param err    What the caller is told.
 * @param native libogc's code, where there was one.
 *
 * THE PORTABLE CODE IS PASSED, NOT DERIVED, because this backend fails for
 * reasons libogc has no code for -- a size larger than the medium can hold, a
 * location string that will not parse -- and mapping those through a borrowed
 * CARD_ERROR_* would lose exactly the distinction save::eError exists to make.
 */
bool Fail(save::eError err, s32 native = CARD_ERROR_FATAL_ERROR) {
    lastErr    = err;
    lastNative = native;
    Abort();
    // TRUE, NOT FALSE: `true` means the sequence is OVER, not that it worked.
    // ::Report pairs it with ::lastErr, so the caller gets `done` with the real
    // save::eError and its `while (!response.done)` loop ENDS.
    //
    // It used to return false, and that made every error here unreportable: a
    // failure abandoned the sequence (::Abort clears ::op) while answering "not
    // finished", so the next tick began the same doomed attempt and answered
    // the same way forever. save.hpp names that outcome as one of the two a
    // game cannot recover from -- "not finished yet (its loop never ends)" --
    // and the whole save::eError vocabulary was unreachable because of it.
    return true;
}

/**
 * @brief Ends a sequence that has run out of candidate slots.
 *
 * A SEARCH THAT FOUND NOTHING IS A COMPLETED SEARCH, and that includes finding
 * no card at all -- `true` here, with ::haveLocation false, is what gives a
 * caller's `while (!response.done)` loop something to stop on, and what makes
 * save::Response's `success` (not `done`) the field that says whether a save
 * exists. Getting this wrong is not a wrong answer but a HANG: a GameCube with
 * an empty slot is the ordinary case, and a search that reported "not finished"
 * forever would pump audio and video until the console was switched off.
 *
 * A save or a load that has run out of slots has genuinely failed, and reports
 * the same `true`: the sequence is over either way, and which of the two it was
 * is in ::lastErr, not in this bool. See this file's header comment.
 */
bool Exhausted(save::eError err, s32 native) {
    const bool finding = (op == Op::Finding);
    lastNative = native;
    // NO LAUNDERING OF THE OUTCOME HERE, unlike an earlier revision: with the
    // vocabulary based on save::found / save::missing, a search of a
    // present-but-blank card reporting save::missing IS the answer the caller
    // wants -- "there is no save, create one" -- rather than an error to be
    // turned into a success. An empty SLOT still reports save::no_device, so a
    // game can ask for a memory card instead of offering to start a new save on
    // one that is not there.
    lastErr = err;
    if (finding) haveLocation = false;
    Abort();
    // `finding` is still computed above (a failed search forgets its remembered
    // location), but the RETURN is unconditional for the reason ::Fail's is: a
    // save or load that has run out of slots is finished, and saying otherwise
    // hangs the caller instead of telling it what went wrong. This used to
    // return `finding`, which made a search the only operation here that could
    // report a failure at all.
    return true;
}

/**
 * @brief Splits `"a:name"` into a slot and a file name.
 * @return false for a null path, a missing or unknown slot letter, an empty
 *         name, or a name too long for CARD_FILENAMELEN.
 */
bool ParsePath(const char* path, s32& slotOut, char (&nameOut)[CARD_FILENAMELEN]) {
    if (!path || !path[0] || path[1] != ':') return false;
    switch (path[0]) {
        case 'a': case 'A': slotOut = CARD_SLOTA; break;
        case 'b': case 'B': slotOut = CARD_SLOTB; break;
        default: return false;
    }
    const char* const name = path + 2;
    const std::size_t len  = std::strlen(name);
    if (len == 0 || len >= CARD_FILENAMELEN) return false;
    std::memcpy(nameOut, name, len + 1);
    return true;
}

/** @brief Records slot + name as this game's known location. */
void RememberLocation(s32 slot, const char* name) {
    locationBuf[0] = (slot == CARD_SLOTB) ? 'b' : 'a';
    locationBuf[1] = ':';
    std::strncpy(locationBuf + 2, name, CARD_FILENAMELEN - 1);
    locationBuf[sizeof(locationBuf) - 1] = '\0';
    haveLocation = true;
}

/**
 * @brief Is there media in @p slot this backend could actually transfer?
 *
 * CARD_ProbeEx, not CARD_Probe: sect_size is the one quantity this file cannot
 * guess, since this libogc has no CARD_GetSectorSize and the staging buffer is
 * a fixed-size object.
 *
 * NO MOUNT, WHICH IS WHY ::save::GetDeviceStatus IS CHEAP. A probe is an EXI
 * transaction against the slot; CARD_Mount is the slow part of everything else
 * in this file and the 40 KiB ::workarea belongs to it, and neither is needed
 * to answer "is a card in there".
 *
 * SHARED WITH ::ProbeCurrentSlot ON PURPOSE. The usability test a status query
 * shows a player and the one a save performs on its way to the card MUST be the
 * same test: a UI that drew "SLOT B: READY" over a card the next ::save::Save
 * would refuse is worse than no UI. So the presence check and the sector-width
 * check live here, once, and ::ProbeCurrentSlot adds only what is about the
 * in-flight operation rather than about the card.
 *
 * IT TOUCHES NO SEQUENCE STATE -- not ::lastErr, not ::lastNative, not ::chn or
 * ::sector -- which is what makes it safe to call from a status poll while a
 * save is in flight. The outcome leaves through @p errOut and the return value
 * instead; the caller decides what to record.
 *
 * @param[out] sectorOut The card's sector size, set only on success.
 * @param[out] errOut    The portable code, set only on failure.
 * @return CARD_ERROR_READY, or libogc's code for what is wrong.
 */
s32 ProbeMedia(s32 slot, u32& sectorOut, save::eError& errOut) {
    s32 sect = 0;
    const s32 err = CARD_ProbeEx(slot, nullptr, &sect);

    /*
     * CARD_ERROR_NOCARD WITH A SECTOR SIZE IN HAND IS A CARD, AND THIS IS THE
     * ORDINARY STATE OF ONE. Taking `err != CARD_ERROR_READY` as "nothing
     * there" -- which this did, and which ::ProbeCurrentSlot did before the
     * two shared a probe -- made the whole backend unable to find a memory
     * card AT ALL, on hardware and in Dolphin alike. libogc ships no card.c in
     * this SDK install, so the shape below is read out of libogc.a's own
     * CARD_ProbeEx (powerpc-eabi-objdump -dr, section .text.CARD_ProbeEx):
     *
     *     if (EXI_ProbeEx(chn) <= 0)                 // 0 -> BUSY, <0 -> NOCARD
     *     if (card->attached) { fill; return CARD_ERROR_READY; }
     *     if (EXI_GetState(chn) & 8)   return CARD_ERROR_WRONGDEVICE;
     *     if (!EXI_GetID(chn, 0, &id)) return CARD_ERROR_BUSY;
     *     if (!__card_iscard(id))      return CARD_ERROR_WRONGDEVICE;
     *     if (mem)  *mem  = ...;
     *     if (sect) *sect = card_sector_size[(id >> 9) & 7];
     *     return CARD_ERROR_NOCARD;                  // the -3 saved at the top
     *
     * CARD_ERROR_READY is returned ONLY on the `card->attached` path -- that is,
     * only for a card that is ALREADY MOUNTED. Every sequence here probes
     * before it mounts, so READY was unreachable: the probe refused the slot,
     * the mount never ran, the card never became attached, and the next probe
     * refused it again. A GameCube with a card in slot A reported save::no_device
     * forever.
     *
     * THE DISCRIMINATOR IS THE OUT PARAMETER, NOT THE CODE. Of the three paths
     * that return CARD_ERROR_NOCARD, only the unmounted-card one writes
     * @p sect first; the early `EXI_ProbeEx() < 0` return writes nothing. So a
     * zeroed `sect` coming back still -3 means an empty slot, and a filled one
     * means a card waiting to be mounted. That is why `sect` is zeroed here
     * rather than trusted to be untouched.
     */
    const bool present = (err == CARD_ERROR_READY) ||
                         (err == CARD_ERROR_NOCARD && sect > 0);
    if (!present) {
        errOut = MapError(err);
        return err;
    }

    if (sect <= 0 || static_cast<u32>(sect) > kSectorMax) {
        // A card whose sector is wider than the staging buffer: refused rather
        // than transferred at the wrong width and called a save.
        //
        // save::too_large RATHER THAN save::failed, which is what this branch
        // reported while it was only ever reached on the way to a save. The
        // native code is unchanged, and so is the refusal -- what changed is
        // that a status query now shows this to a player, and "this card is
        // one this game cannot use" is a sentence a UI can write from
        // ::too_large and cannot write from ::failed. It is the same quantity
        // the check below refuses in the other direction: a size against a
        // sector, mismatched.
        errOut = save::too_large;
        return CARD_ERROR_FATAL_ERROR;
    }
    sectorOut = static_cast<u32>(sect);
    return CARD_ERROR_READY;
}

/**
 * @brief Checks the current candidate slot and learns that card's sector size.
 *
 * The card half is ::ProbeMedia's; what is left here is the one test that is
 * about the OPERATION rather than the medium -- whether the caller's payload
 * fits the sector this particular card turned out to have.
 */
bool ProbeCurrentSlot() {
    u32 sect = 0;
    save::eError portable = save::found;
    const s32 err = ProbeMedia(kSlots[slotIndex], sect, portable);
    if (err != CARD_ERROR_READY) {
        lastErr = portable; lastNative = err;
        return false;
    }
    if (op != Op::Finding && static_cast<u32>(pending) > sect) {
        // What a caller passes must fit ONE sector, because ::Tick's transfer
        // moves exactly `sector` bytes from offset 0 -- anything that spilled
        // past that would be silently truncated and reported as a save.
        // ::Begin's cap is against kSectorMax, which is the widest sector this
        // file is built for; a card with a NARROWER one lands here instead.
        // Refused rather than clipped.
        lastErr = save::too_large; lastNative = CARD_ERROR_INSSPACE;
        return false;
    }
    chn    = kSlots[slotIndex];
    sector = sect;
    return true;
}

/**
 * @brief Probes one slot and records what it found.
 * @return The settled outcome, or save::busy when there is not one yet.
 *
 * BOTH PUBLIC DEVICE CALLS GO THROUGH HERE, which is the point:
 * ::save::AccessibleDevices and ::save::GetDeviceStatus must never disagree
 * about a slot, and the cheapest way to guarantee that is for the bit and the
 * code to come out of the same probe and the same cache.
 *
 * save::busy IS NOT CACHED. "Not settled yet" is the absence of an answer, not
 * an answer -- writing it into ::deviceErr would throw away the last true thing
 * known about the slot and make a UI flicker for the length of an insertion.
 *
 * ::sector IS DROPPED ON PURPOSE, even though ::ProbeMedia just learnt it.
 * ::ProbeCurrentSlot asks again when a real operation starts, and latching it
 * from a status poll would mean a menu quietly rewriting state the next save
 * reads.
 */
save::eError RefreshDevice(u8 index) {
    u32 sect = 0;
    save::eError portable = save::found;
    const s32 err = ProbeMedia(kSlots[index], sect, portable);
    if (err == CARD_ERROR_BUSY) return save::busy;

    if (err != CARD_ERROR_READY) {
        // Nothing in the slot (or nothing that is a card), so there is nothing
        // left to be broken: the remembered verdict goes with the card.
        deviceBroken[index] = false;
    }

    deviceErr[index]    = (err == CARD_ERROR_READY) ? save::found : portable;
    deviceNative[index] = err;

    // A card a mount has already found unusable is reported as no device, which
    // a probe could never have discovered on its own. See ::deviceBroken.
    if (deviceBroken[index]) deviceErr[index] = save::no_device;

    deviceKnown[index]  = true;
    return deviceErr[index];
}

/**
 * @brief What is known about @p index without touching the card at all.
 *
 * The answer a status poll gets while a sequence owns the card: the last
 * settled outcome, or save::busy when there has never been one. See
 * ::deviceErr for why a probe is not attempted underneath an in-flight
 * operation.
 */
save::eError CachedDevice(u8 index) {
    return deviceKnown[index] ? deviceErr[index] : save::busy;
}

/**
 * @brief Could the card mounted on ::chn take a write of ::surveySize bytes?
 *
 * TWO WAYS TO SAY YES, and the second is the one a player cares about:
 *
 *   - There are enough FREE blocks for a file that size. What a create needs.
 *   - There is no room, but this game's save is ALREADY THERE and already big
 *     enough. Overwriting an existing file consumes nothing, so a card with
 *     zero free blocks is still somewhere the next ::save::Save will succeed.
 *     A UI that greyed that card out would be refusing the most ordinary save
 *     there is.
 *
 * THE SECOND CHECK COSTS THREE CARD CALLS IN ONE TICK -- find, open, close --
 * where the rest of this file spends one per tick. They are directory
 * operations on an already-mounted card, which is the cheap end of what a card
 * does; the mount that got us here is the expensive part and had its own tick.
 *
 * CARD_GetFreeBlocks' RETURN IS AMBIGUOUS in this libogc: the header says
 * "card error codes or free blocks" (ogc/card.h) while also taking an out
 * parameter, and there is no card.c in this SDK install to settle which it
 * does. Both readings are accepted below -- the out parameter when it was
 * filled, the return value when it was not and is positive -- so a card that
 * reports its free space either way is read correctly, and only a genuine
 * negative error code is treated as one.
 */
bool SlotHasRoom() {
    // A create rounds the caller's prefix up to the card's allocation unit, so
    // that is the unit the free count must be compared in. See ::Resolve.
    const u32 need = (static_cast<u32>(surveySize) + surveySector - 1) / surveySector;

    u16       blocks = 0;
    const s32 err    = CARD_GetFreeBlocks(chn, &blocks);
    if (err >= CARD_ERROR_READY) {
        const u32 freeBlocks = (blocks != 0)
            ? static_cast<u32>(blocks)
            : (err > CARD_ERROR_READY ? static_cast<u32>(err) : 0u);
        if (freeBlocks >= need) return true;
    }

    // No room for a NEW file. The existing one, if this game has one here, is
    // space already paid for -- identity-filtered exactly as ::Resolve's search
    // is, so a save under any name still counts.
    card_dir dir;
    if (CARD_FindFirst(chn, &dir, false) != CARD_ERROR_READY) return false;

    // A LOCAL card_file, not the machine's ::file: nothing about this is the
    // in-flight operation's business, and open and close happen within this one
    // tick, so no abandoned sweep can leave it open across a frame.
    card_file probe;
    if (CARD_Open(chn, dir.filename, &probe) != CARD_ERROR_READY) return false;
    const bool fits = static_cast<u32>(probe.len) >= static_cast<u32>(surveySize);
    CARD_Close(&probe);
    return fits;
}

/** @brief Publishes the finished sweep and returns the machine to idle. */
void SurveyPublish() {
    freeMask       = surveyMask;
    freeMaskSize   = surveySize;
    freeMaskAccess = surveyAccess;
    freeMaskValid  = true;
    Abort();
}

/**
 * @brief Leaves the current slot and moves to the next, publishing at the end.
 *
 * A SLOT THAT FAILED ANYWHERE LEAVES ITS BIT CLEAR and is not retried: an
 * unprobeable or unmountable card is not one a save would reach either, so the
 * answer "you cannot write here" is correct however the step failed.
 */
void SurveyNextSlot() {
    if (mounted) { CARD_Unmount(chn); mounted = false; }
    ++slotIndex;
    phase = Phase::Probe;
    if (slotIndex >= kDeviceCount) SurveyPublish();
}

/** @brief Advances the sweep by exactly one card step. */
void SurveyTick() {
    // A card pulled out from under the sweep makes the partial answer a lie
    // about a machine that has changed. Thrown away rather than published; the
    // next call starts a fresh one.
    if (detached) { Abort(); return; }

    switch (phase) {
        case Phase::Probe: {
            u32          sect     = 0;
            save::eError portable = save::found;
            if (ProbeMedia(kSlots[slotIndex], sect, portable) != CARD_ERROR_READY) {
                SurveyNextSlot();
                return;
            }
            chn          = kSlots[slotIndex];
            surveySector = sect;
            phase        = Phase::Mount;
            return;
        }

        case Phase::Mount: {
            const s32 err = CARD_Mount(chn, workarea, DetachCallback);
            if (err != CARD_ERROR_READY) {
                // A sweep is the cheapest way to go LOOKING for this verdict,
                // since it mounts every accessible slot in turn and a game
                // polling it costs nothing once the answer has settled.
                if (IsUnusableCode(err)) deviceBroken[slotIndex] = true;
                SurveyNextSlot();
                return;
            }
            deviceBroken[slotIndex] = false;
            mounted = true;
            phase   = Phase::Resolve;
            return;
        }

        case Phase::Resolve:
            if (SlotHasRoom()) surveyMask |= static_cast<u8>(1u << slotIndex);
            phase = Phase::Finish;
            return;

        // The unmount is ::SurveyNextSlot's, so that a slot abandoned early and
        // a slot measured in full leave by the same door.
        case Phase::Transfer:
        case Phase::Finish:
        default:
            SurveyNextSlot();
            return;
    }
}

/** @brief Starts a sweep for @p size against the devices in @p accessible. */
void SurveyBegin(std::size_t size, u8 accessible) {
    op           = Op::Surveying;
    phase        = Phase::Probe;
    slotIndex    = 0;
    slotFixed    = false;
    surveySize   = size;
    surveySector = 0;
    surveyMask   = 0;
    surveyAccess = accessible;
    detached     = false;
    EnsureInit();
}

/**
 * @brief Gives up on the current slot and moves to the next candidate.
 * @return false once every candidate has been tried.
 */
bool AdvanceSlot() {
    if (opened)  { CARD_Close(&file); opened = false; }
    if (mounted) { CARD_Unmount(chn); mounted = false; }
    if (slotFixed) return false;
    // A name discovered on the card just rejected says nothing about the next
    // one, so the search starts over there rather than opening a stale name.
    targetName[0] = '\0';
    ++slotIndex;
    return slotIndex < 2;
}

/**
 * @brief The location a caller's ::save::Response names, if it names one.
 *
 * ONLY `path` IS READ. `done` and `success` describe the call that PRODUCED the
 * response and are the caller's business, not this backend's: a response from a
 * search that found nothing carries a null path, which lands on the same code
 * as passing no response at all -- the backend searches for itself. That makes
 * `Load(tar, size, &loc)` safe to write before checking `loc`, which is the
 * whole point of taking the response rather than a bare string.
 */

/** @brief Starts a sequence, latching everything it will need. */
bool Begin(Op want, const u8* src, u8* tar, std::size_t size, const char* path) {
    // `path` here is already unwrapped from the caller's save::Response by the
    // entry points below -- only its `path` field is consumed. See ::PathOf.
    if (want != Op::Finding && !IsMeta(want)) {
        // The widest sector this file is built for. The real limit is the
        // CARD'S sector, which is not known until ::ProbeCurrentSlot has
        // asked -- this is the cheap bound that can be applied before the card
        // is touched at all, and the exact one is enforced there.
        //
        // NOT applied to the metadata ops: they move no file bytes and pass no
        // size, so `size == 0` is their ordinary case and not a mistake.
        if (size == 0 || size > kSectorMax) return Fail(save::too_large);
    }

    slotFixed = false;
    slotIndex = 0;
    targetName[0] = '\0';

    if (path) {
        s32 slot = CARD_SLOTA;
        if (!ParsePath(path, slot, targetName)) return Fail(save::bad_path, CARD_ERROR_NAMETOOLONG);
        slotIndex = (slot == kSlots[0]) ? 0 : 1;
        slotFixed = true;
    } else if (want != Op::Finding && haveLocation) {
        // No path given, but something has already located this game's save:
        // go straight there rather than searching the card again.
        s32 slot = CARD_SLOTA;
        if (ParsePath(locationBuf, slot, targetName)) {
            slotIndex = (slot == kSlots[0]) ? 0 : 1;
            slotFixed = true;
        }
    }

    if (want == Op::Saving || want == Op::Creating) {
        if (!src) return Fail(save::failed);
        // Copied up front, not on the transfer tick: the game is free to carry
        // on mutating its own state across the frames this takes, and what
        // reaches the card is the snapshot it asked to save.
        std::memset(staging, 0, kSectorMax);
        std::memcpy(staging, src, size);
    } else if (want == Op::Loading) {
        if (!tar) return Fail(save::failed);
        dest = tar;
    } else if (want == Op::SettingMeta) {
        if (!src) return Fail(save::failed);
        std::memcpy(&metaStat, src, sizeof metaStat);
        metaOut = nullptr;
    } else if (want == Op::GettingMeta) {
        if (!tar) return Fail(save::failed);
        metaOut = tar;
    }

    op       = want;
    pending  = size;
    phase    = Phase::Probe;
    detached = false;

    // A fresh sweep: no slot has offered an outcome yet, and nothing has been
    // waited on. ::sweepErr starts at the weakest answer so the first real one
    // displaces it (::Rank).
    sweepErr     = save::no_device;
    sweepNative  = CARD_ERROR_NOCARD;
    sweepSeen    = false;
    probeRetries = 0;

    CARD_Init(kGameCode, kCompany);
    return false;
}

/**
 * @brief Opens, finds or creates the file on the mounted card.
 * @return CARD_ERROR_READY once ::file (or ::targetName, when finding) is set.
 */
s32 Resolve() {
    if (op == Op::Finding) {
        // The search proper: entries filtered to this game's own gamecode and
        // company, so a caller-chosen file name is still findable.
        card_dir dir;
        const s32 err = CARD_FindFirst(chn, &dir, false);
        if (err != CARD_ERROR_READY) return err;
        std::memcpy(targetName, dir.filename, CARD_FILENAMELEN - 1);
        targetName[CARD_FILENAMELEN - 1] = '\0';
        return CARD_ERROR_READY;
    }

    if (op == Op::Creating) {
        // The file's length is the caller's prefix rounded up to the card's
        // own allocation unit -- the one place a size becomes a file size.
        const u32 len = ((static_cast<u32>(pending) + sector - 1) / sector) * sector;
        const s32 err = CARD_Create(chn, targetName, len, &file);
        if (err != CARD_ERROR_READY) return err;   // incl. CARD_ERROR_EXIST
        opened = true;
        return CARD_ERROR_READY;
    }

    // Saving or loading with nothing naming the file yet -- no path, and
    // nothing has located this game's save in this session. Same identity
    // search GetSaveLocation runs, so the header's "whatever the backend can
    // locate on its own" is not an empty promise.
    if (targetName[0] == '\0') {
        card_dir dir;
        const s32 found = CARD_FindFirst(chn, &dir, false);
        if (found != CARD_ERROR_READY) return found;
        std::memcpy(targetName, dir.filename, CARD_FILENAMELEN - 1);
        targetName[CARD_FILENAMELEN - 1] = '\0';
    }

    // CARD_Open IS the existence check.
    const s32 err = CARD_Open(chn, targetName, &file);
    if (err != CARD_ERROR_READY) return err;
    opened = true;
    // card_file::len is the file's real length, so the prefix is checked
    // against the file rather than against the caller's word for it.
    if (static_cast<u32>(file.len) < sector ||
        pending > static_cast<std::size_t>(file.len)) {
        return CARD_ERROR_LIMIT;
    }
    // Found by searching, so the next call need not search again.
    RememberLocation(chn, targetName);
    return CARD_ERROR_READY;
}

/** @brief Advances one step; true exactly once, on the tick that completes it. */
bool Tick(Op want, const u8* src, u8* tar, std::size_t size, const char* path) {
    if (op == Op::None) return Begin(want, src, tar, size, path);

    // Two sequences cannot be in flight at once: they share the slot, the
    // mount, the work area and the staging buffer. The second is refused --
    // WITHOUT Fail(), which would abort the sequence that legitimately owns
    // the card and leave the caller that owns IT believing it had failed.
    if (op != want)      { return false; }   // see ::Busy, checked before Tick
    if (detached)        return Fail(save::no_device, CARD_ERROR_NOCARD);
    // @p size is read once, on the first tick, so a changed size would describe
    // bytes nothing ever copied.
    if (want != Op::Finding && pending != size) return Fail(save::failed);

    switch (phase) {
        case Phase::Probe:
            if (!ProbeCurrentSlot()) {
                // NOT SETTLED IS NOT EMPTY: stay on this slot and ask again
                // next tick, up to ::kProbeRetryLimit. Advancing here is what
                // turned a card still coming up on the bus into "no memory
                // card in either slot".
                if (lastErr == save::busy) {
                    if (++probeRetries < kProbeRetryLimit) return false;
                    // Out of patience. The slot gets an answer rather than
                    // another lap: save::busy as a FINISHED outcome would tell
                    // a caller to retry a call that will do this again.
                    lastErr    = save::failed;
                    lastNative = CARD_ERROR_BUSY;
                }
                probeRetries = 0;
                NoteSlotOutcome(lastErr, lastNative);
                if (AdvanceSlot()) return false;      // try the other slot
                return Exhausted(sweepErr, sweepNative);
            }
            probeRetries = 0;
            phase = Phase::Mount;
            return false;

        case Phase::Mount: {
            const s32 err = CARD_Mount(chn, workarea, DetachCallback);
            if (err != CARD_ERROR_READY) {
                // The one place this backend ever learns that a card is
                // unusable rather than absent. Recorded so the probe-only
                // device calls stop offering it (::deviceBroken).
                if (IsUnusableCode(err)) deviceBroken[slotIndex] = true;
                NoteSlotOutcome(MapError(err), err);
                if (AdvanceSlot()) { phase = Phase::Probe; return false; }
                return Exhausted(sweepErr, sweepNative);
            }
            deviceBroken[slotIndex] = false;   // it mounted; whatever it was, it is not that now
            mounted = true;
            phase   = Phase::Resolve;
            return false;
        }

        case Phase::Resolve: {
            const s32 err = Resolve();
            if (err != CARD_ERROR_READY) {
                // Not on this card -- try the next one, unless a path pinned
                // the slot, in which case "not here" is the whole answer.
                //
                // THIS IS THE OUTCOME THE OLD SWEEP THREW AWAY: a mounted card
                // with no save on it answers CARD_ERROR_NOFILE here, which is
                // save::missing -- "there is somewhere to save, and your save
                // is not on it". See ::sweepErr.
                NoteSlotOutcome(MapError(err), err);
                if (AdvanceSlot()) { phase = Phase::Probe; return false; }
                return Exhausted(sweepErr, sweepNative);
            }
            if (op == Op::Finding) {
                RememberLocation(chn, targetName);
                lastErr    = save::found;
                lastNative = CARD_ERROR_READY;
                Abort();
                return true;
            }
            phase = Phase::Transfer;
            return false;
        }

        case Phase::Transfer: {
            // THE DIRECTORY ENTRY, NOT THE FILE. No staging buffer and no cache
            // maintenance: a card_stat is an ordinary object libogc fills with
            // the CPU, and the directory block it ends up in is the card
            // system's own buffer rather than one of ours.
            //
            // ::file.filenum is how both calls address the entry, and ::Resolve
            // has already set it -- CARD_Open and CARD_Create both fill it -- so
            // no separate directory lookup is needed here.
            if (IsMeta(op)) {
                const s32 err = (op == Op::GettingMeta)
                    ? CARD_GetStatus(chn, file.filenum, &metaStat)
                    : CARD_SetStatus(chn, file.filenum, &metaStat);
                if (err != CARD_ERROR_READY) return Fail(MapError(err), err);
                // Only a Get copies back: ::metaOut is null for a Set, whose
                // caller passed a const pointer and is promised no writeback.
                if (metaOut) std::memcpy(metaOut, &metaStat, sizeof metaStat);
                phase = Phase::Finish;
                return false;
            }

            // Cache maintenance around the DMA, both directions. libogc may do
            // this inside card.c -- its source is not part of this SDK install,
            // so it cannot be confirmed here -- and doing it twice costs only
            // cycles, while not doing it costs a wrong save on a dirty line.
            DCFlushRange(staging, sector);
            if (op == Op::Loading) {
                // Whole sector, not `pending`: CARD_Read quantises to
                // CARD_READSIZE (512), and the prefix is taken out of the
                // buffer rather than off the card.
                const s32 err = CARD_Read(&file, staging, sector, 0);
                if (err != CARD_ERROR_READY) return Fail(MapError(err), err);
                DCInvalidateRange(staging, sector);
                std::memcpy(dest, staging, pending);
            } else {
                const s32 err = CARD_Write(&file, staging, sector, 0);
                if (err != CARD_ERROR_READY) return Fail(MapError(err), err);
            }
            phase = Phase::Finish;
            return false;
        }

        case Phase::Finish: {
            // Unmounted every time rather than kept mounted between calls: a
            // card left mounted is a card whose removal goes unnoticed until
            // the next write fails, and saves are rare enough that paying the
            // mount again is the cheaper mistake.
            const s32 close   = CARD_Close(&file);
            opened = false;
            const s32 unmount = CARD_Unmount(chn);
            mounted = false;
            const bool created = (op == Op::Creating);
            // A write may have changed what is free -- a create certainly did,
            // and a save of a size this card had not seen may have too. The
            // next ::save::FreeDevices re-sweeps rather than answering from a
            // mask measured before the card changed underneath it.
            if (op == Op::Saving || op == Op::Creating) freeMaskValid = false;
            const s32 slot = chn;
            Abort();
            if (close != CARD_ERROR_READY)   return Fail(MapError(close), close);
            if (unmount != CARD_ERROR_READY) return Fail(MapError(unmount), unmount);
            // A create is also a locate: the game now knows where its save is
            // without searching for the file it just made.
            if (created) RememberLocation(slot, targetName);
            lastErr    = save::found;
            lastNative = CARD_ERROR_READY;
            return true;
        }
    }

    return Fail(save::failed);
}

} // namespace

namespace {

/**
 * @brief True when a DIFFERENT operation already owns the card.
 *
 * Checked before ::Tick rather than inside it, so the refusal never touches the
 * in-flight sequence's own recorded outcome: the caller being refused hears
 * save::busy, and the caller that legitimately owns the card keeps whatever its
 * own last tick recorded.
 */
bool Busy(Op want) {
    // A SWEEP HAS NO CLAIM ON THE CARD. It is a cache refresh for a menu, and a
    // save that waited for one would wait forever: nothing advances the sweep
    // except ::save::FreeDevices, which a caller looping on `while (!done)` is
    // not calling. So a real operation ABANDONS it and proceeds -- the next
    // ::save::FreeDevices starts a new one, and the only thing lost is a few
    // frames of mask.
    if (op == Op::Surveying && want != Op::Surveying) { Abort(); return false; }
    return op != Op::None && op != want;
}

/** @brief Builds the response for a sequence that has just ticked. */
save::Response Report(bool done) {
    return { .done = done,
             // Only a sequence that is OVER has an outcome to describe; while
             // one is merely unfinished the caller is looking at `done`, and
             // save::found is the neutral value to be holding until then.
             .err  = done ? lastErr : save::found,
             .path = (done && haveLocation) ? locationBuf : nullptr };
}

} // namespace

namespace save {

/*
 * THE ONE PLACE THE PAYLOAD BECOMES BYTES. save.hpp takes `void*` so that a
 * game can hand over its own save struct without a reinterpret_cast; the
 * backend works in u8 throughout, because what it does with the payload --
 * memcpy into ::staging, DMA a sector -- is byte work by nature. Casting here
 * rather than at every call site is the whole point of the signature: one
 * conversion, in the file that owns the buffer, instead of one per game.
 *
 * static_cast, NOT reinterpret_cast: `void*` to `u8*` is an ordinary pointer
 * conversion and says nothing more alarming than it means.
 */
Response Save(const void* src, const std::size_t size) {
    if (Busy(Op::Saving)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Saving, static_cast<const u8*>(src), nullptr, size, nullptr));
}

Response Load(void* tar, const std::size_t size) {
    if (Busy(Op::Loading)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Loading, nullptr, static_cast<u8*>(tar), size, nullptr));
}

Response Load(void* tar, const std::size_t size, const Response* response) {
    if (Busy(Op::Loading)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Loading, nullptr, static_cast<u8*>(tar), size, PathOf(response)));
}

/**
 * @brief Creates the save at the location @p response names, writing @p size
 *        bytes of @p src.
 *
 * A RESPONSE WITH NO PATH IS NO LONGER REFUSED. It used to be, because there
 * was nothing for a create to search for and no name it could invent; now
 * ::kDefaultPath exists (OGC_FILENAME, set in the build), so "create this
 * game's save" is a complete instruction on its own. A response straight out
 * of an unsuccessful ::GetSaveLocation is exactly that instruction -- the
 * Get-then-Create flow needs no name from the caller at all.
 *
 * A caller that DOES name a location still gets it, which is what keeps the
 * identity-based search worth having: the file can be called anything and
 * ::GetSaveLocation will still find it (see this file's header comment).
 */
Response CreateSave(const void* src, const std::size_t size, const Response* response) {
    if (Busy(Op::Creating)) return { .done = false, .err = busy, .path = nullptr };
    const char* const named = PathOf(response);
    return Report(Tick(Op::Creating, static_cast<const u8*>(src), nullptr, size, named ? named : kDefaultPath));
}

/**
 * @brief Finds where this game's save lives.
 *
 * A COMPLETED SEARCH THAT FOUND NOTHING IS NOT AN ERROR: it reports `done` with
 * `err == ok` and a null `path`, which is the cue to call ::CreateSave. An empty
 * card slot is different and says so with ::no_device, so a game can ask for a
 * memory card rather than offer to start a new save on one that is not there.
 */
Response GetSaveLocation() {
    if (Busy(Op::Finding)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Finding, nullptr, nullptr, 0, nullptr));
}

/**
 * @brief Reads this save's directory entry into @p tar (a `card_stat`).
 *
 * THE DIRECTORY ENTRY IS NOT IN THE SAVE FILE. It is a 64-byte record in one of
 * the card's own directory blocks, holding the gamecode, company, file name,
 * permissions -- and the OFFSETS of the comment block, banner and icon WITHIN
 * the file. So this call costs a save none of its bytes, and it is the only way
 * to learn or state where an application put its metadata.
 *
 * @p tar IS A `card_stat*`, passed as `void*` so that pnes/save.hpp can declare
 * this on every target without every target's build pulling in ogc/card.h. A
 * GameCube application includes that header anyway (save.hpp does it for the
 * TARGET_GC build) and passes `&stat`.
 *
 * Ticked like everything else here, because it needs the card mounted and the
 * file resolved: the same probe/mount/find-or-open sequence a load runs.
 */
Response GetMetaData(void* tar) {
    if (Busy(Op::GettingMeta)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::GettingMeta, nullptr, static_cast<u8*>(tar), 0, nullptr));
}

/**
 * @brief Writes @p src (a `card_stat`) to this save's directory entry.
 *
 * WHAT IT ACTUALLY CHANGES is five fields -- `banner_fmt`, `icon_addr`,
 * `icon_fmt`, `icon_speed`, `comment_addr` -- plus a `last_modified` stamp.
 * CARD_SetStatus leaves the gamecode, company, file name, permissions, block
 * and length alone, which is why it is the call this backend uses rather than
 * CARD_SetStatusEx: nothing here can disturb the identity pair that
 * ::GetSaveLocation's CARD_FindFirst filters on.
 *
 * TWO VALUES ARE REJECTED OUTRIGHT, both as CARD_ERROR_FATAL_ERROR (so
 * save::failed): an `icon_addr` that is neither 0xFFFFFFFF nor below 512, and a
 * `comment_addr` whose 64-byte block would straddle an 8192-byte boundary
 * (`comment_addr % 8192 > 8128`). An application laying the metadata out at the
 * front of its own save struct satisfies both without trying.
 *
 * ONE TRAP WORTH NAMING: frame 0's icon speed must be NONZERO. libogc forces a
 * frame's format to 0 when its speed is 0, and counts only frames with both set
 * when it places the shared palette -- so a static icon with speed 0 renders as
 * nothing at all. Set the speed bits by hand; CARD_SetIconSpeed and
 * CARD_GetIconSpeed (ogc/card.h) are both defective in this libogc.
 */
Response SetMetaData(const void* src) {
    if (Busy(Op::SettingMeta)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::SettingMeta, static_cast<const u8*>(src), nullptr, 0, nullptr));
}

/**
 * @brief Bit 0 for slot A, bit 1 for slot B, set when the slot holds a card
 *        this backend can save to.
 *
 * ONE PROBE PER SLOT AND NOTHING ELSE -- no mount, no work area, no staging
 * buffer -- so polling both every few frames costs two EXI transactions. See
 * ::ProbeMedia for why that is all it takes.
 *
 * A BIT IS SET ONLY FOR save::found, which means "settled, and usable". A slot
 * still settling after an insertion reads as 0 until it has, with no way to
 * tell it apart from an empty one HERE -- that distinction is what
 * ::GetDeviceStatus exists to make, and a mask has no room for it. A caller
 * that needs to say "checking..." rather than "empty" asks about the slot it
 * cares about; a caller that only needs to know where it can save reads the
 * mask and is done.
 *
 * MID-SEQUENCE IT ANSWERS FROM THE CACHE, same as ::GetDeviceStatus and for
 * the same unverifiable-locking reason (::deviceErr). A save in progress does
 * not make slot B's card vanish, so the bits stay as they last settled.
 */
u8 AccessibleDevices() {
    const bool idle = (op == Op::None);
    if (idle) EnsureInit();

    u8 mask = 0;
    for (u8 i = 0; i < kDeviceCount; ++i) {
        const eError err = idle ? RefreshDevice(i) : CachedDevice(i);
        if (err == found) mask |= static_cast<u8>(1u << i);
    }
    return mask;
}

/**
 * @brief Whether slot @p index holds media this backend can use, and where a
 *        save on it would go.
 *
 * THE ONLY ENTRY POINT HERE THAT IS NOT A TICK OF THE STATE MACHINE, and
 * deliberately so. It needs no mount, no work area, no staging buffer and no
 * file: ::ProbeMedia answers from one EXI transaction, so there is nothing for
 * a sequence to span frames FOR. It still speaks save::Response, because the
 * one thing it shares with the rest of the file is that the answer can be "not
 * yet" -- a card inserted a moment ago reports CARD_ERROR_BUSY until it has
 * settled, and `done` false is how a caller is told to ask again.
 *
 * IT DOES NOT RUN THROUGH ::Busy OR ::Report. Both are about the in-flight
 * sequence's own recorded outcome, and this call has no business reading or
 * writing it -- ::Report would hand back ::lastErr, which belongs to whatever
 * save is in progress, and ::Busy's refusal would be the wrong answer anyway:
 * "a save is running" is not a fact about what is in slot B. A poll that
 * arrives mid-sequence answers from ::deviceErr instead, for the reason its
 * comment gives.
 *
 * AN OUT-OF-RANGE @p index IS ::no_device, NOT A REFUSAL. "There is no device
 * 7" and "there is no card in slot B" are the same answer as far as a caller
 * can act on it, and a loop written as `for (u8 i = 0; i < 4; ++i)` against a
 * console with two slots should quietly see two empty ones rather than
 * something it has to special-case.
 */
/**
 * @brief Devices that could take a write of @p size, as a bitfield.
 *
 * ONE STEP OF THE SWEEP PER CALL, and the answer handed back is the last
 * FINISHED sweep's -- see ::surveySize for why free space cannot simply be
 * probed the way presence can. Nothing here blocks and nothing mounts more
 * than one card per call.
 *
 * ZERO IS "NOWHERE, OR NOT YET". A first call cannot have an answer, so it
 * returns 0 while starting the sweep that will have one; so does a pair of
 * genuinely full cards. The ambiguity is the price of a bitfield, and
 * ::GetDeviceStatus is where a caller goes to tell the two apart for a slot it
 * cares about.
 *
 * THE CACHE IS KEYED ON THE QUESTION, not just on time: a mask measured for
 * 2048 bytes says nothing about 4096, and one measured while only slot A held a
 * card says nothing once a second is inserted. Both are compared on every call,
 * and either mismatch starts a fresh sweep rather than returning an answer to a
 * question nobody asked.
 */
/**
 * @brief ::lastNative, the CARD_ERROR_* behind the last finished operation.
 *
 * The reason this backend keeps two error values at all (see ::lastErr): a game
 * acts on save::no_device, and a developer still needs to see whether that was
 * CARD_ERROR_NOCARD, CARD_ERROR_WRONGDEVICE (an SD adapter in the slot),
 * CARD_ERROR_BROKEN or CARD_ERROR_ENCODING -- four very different situations
 * that this library deliberately reports as one.
 *
 * NOT the device cache's ::deviceNative: that one belongs to a status poll, and
 * a menu asking about slot B every few frames must not be able to overwrite
 * what a failed save left here.
 */
i32 LastNativeError() { return static_cast<i32>(lastNative); }

u8 FreeDevices(const std::size_t size) {
    // Refused by ::Begin whatever the card says, so no device can take it and
    // there is nothing to go and measure. Reported as a settled 0 rather than
    // by starting a sweep that could only agree.
    if (size == 0 || size > kSectorMax) return 0;

    // Cheap, and it doubles as the invalidation this needs: a card inserted or
    // removed since the last sweep changes this mask, and a changed mask is a
    // stale answer. While a sweep or a save is in flight this reads the device
    // cache rather than probing, which is what keeps it safe to call here.
    const u8   accessible = AccessibleDevices();
    const bool fresh      = freeMaskValid && freeMaskSize == size && freeMaskAccess == accessible;

    if (op == Op::Surveying) {
        if (surveySize == size && surveyAccess == accessible) {
            SurveyTick();                 // our sweep: advance it
            return fresh ? freeMask : 0;
        }
        // The question changed under the sweep. Abandoned rather than finished,
        // since its answer would be published against the wrong key.
        Abort();
    }

    // A real operation owns the card. No sweep can start and none can run;
    // whatever was last measured is the best answer available.
    if (op != Op::None) return fresh ? freeMask : 0;

    if (fresh) return freeMask;

    // Nothing accessible: the answer is 0 and it is SETTLED, not pending --
    // publishing it here spares a sweep that would mount nothing and find
    // nothing, and gives ::freeMaskValid something true to say.
    if (accessible == 0) {
        freeMask       = 0;
        freeMaskSize   = size;
        freeMaskAccess = 0;
        freeMaskValid  = true;
        return 0;
    }

    SurveyBegin(size, accessible);
    return 0;
}

Response GetDeviceStatus(const u8 index) {
    if (index >= kDeviceCount) return { .done = true, .err = no_device, .path = nullptr };

    // A sequence owns the card: answer from what last settled rather than
    // probing underneath it (::deviceErr). Idle, it is one probe.
    eError err = busy;
    if (op != Op::None) {
        err = CachedDevice(index);
    } else {
        EnsureInit();
        err = RefreshDevice(index);
    }

    // save::busy IS THE ONE OUTCOME THAT IS NOT `done`: the bus has not decided
    // yet, so there is nothing to report and the caller is being asked to come
    // back. Every other code is a finished answer, including the ordinary
    // ::no_device of an empty slot.
    if (err == busy) return { .done = false, .err = busy, .path = nullptr };

    return { .done = true,
             .err  = err,
             .path = (err == found) ? kDevicePath[index] : nullptr };
}

} // namespace save

#else // TARGET_WII

/*
 * THE WII SAVES TO A FAT VOLUME, AND THAT CHANGES THE SHAPE OF THE BACKEND
 * RATHER THAN JUST ITS CALLS. There is no EXI memory card slot on this console
 * -- the CARD_* API above addresses hardware it does not have -- so persistent
 * storage here is the front SD slot or USB mass storage, both reached as
 * ordinary FAT volumes through libfat. What that medium costs and what it
 * refuses are different from a memory card's, and three things follow:
 *
 * NOTHING IS TICKED, BECAUSE NOTHING CAN BE. libfat is a blocking API: there is
 * no "advance this transfer by one step" entry point to build a state machine
 * out of, the way CARD_Read's completion callback let the GameCube half keep
 * video and audio running between steps. So every call here finishes the whole
 * operation and reports `done` on its first tick, and a caller's
 *
 *     while (!response.done) { ...one frame... }
 *
 * loop runs exactly once -- the same loop, with the same meaning, running
 * through in one pass as it does on a battery cart. The cost lands inside one
 * frame instead of being spread over several, and the honest summary is that a
 * save on this console drops frames where a save on the GameCube does not. The
 * expensive parts are named where they happen (::Mount, ::save::FreeDevices).
 *
 * save::busy IS THEREFORE NEVER RETURNED. It is declared on this target --
 * save.hpp gates the vocabulary on TARGET_OGC now, for the Wii's sake -- so a
 * game written against the GameCube still compiles, but no operation here is
 * ever in flight across two calls and nothing can collide with one.
 *
 * THE SEARCH IS PATH-BASED, NOT IDENTITY-BASED, which is the one place this
 * backend is STRICTER than the GameCube's. CARD_FindFirst filters the card's
 * directory by gamecode and company, so the GameCube half can let a caller name
 * the file anything and still find it afterwards. A FAT volume has no such
 * filter: a save is found by looking where it was put. So the location is fixed
 * by the build (WII_SAVE_DIR / WII_SAVE_FILENAME below) and ::ParseDevice
 * accepts ONLY the two complete paths this file publishes -- the ones
 * ::save::GetDeviceStatus and ::save::GetSaveLocation hand out. A path with any
 * other spelling is save::bad_path rather than a file created where nothing
 * will ever look for it again.
 *
 * @p size IS A PREFIX LENGTH HERE TOO, AND THAT IS A DELIBERATE COST. A FAT
 * file could be exactly @p size bytes long, which is what a filesystem makes
 * easy -- but then a game that grew its save struct by one member would find
 * every existing save too short for the next ::save::Load, on a console where
 * the medium has gigabytes to spare. The GameCube's contract exists because
 * CARD_Create fixes a file's length at a multiple of the card's sector; this
 * file reproduces it with ::kAllocUnit, so a save written here is rounded up
 * the same way, a prefix shorter than the file reads back the same way, and the
 * one failure a portable caller has to handle is the same failure in the same
 * place. A few unused KiB on an SD card is the whole price.
 *
 * NO LOGGING, for the reason the GameCube half gives: log() resolves on this
 * platform to the generic debug branch of logger.hpp, which writes through
 * std::cout and depends on a console src/ogc/video.cpp's GX framebuffer owns.
 * The failing call's own errno lands in ::lastNative instead, which is what
 * ::save::LastNativeError reports.
 */

#include <fat.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

/**
 * @brief Where this game's save lives on a volume, as the build spells it.
 *
 * TWO NAMES AND NOT ONE PATH, because the directory has to be created before
 * the file can be and a mkdir needs it on its own. Compile definitions rather
 * than a CMake-generated header, matching OGC_GAME_CODE and friends above: a
 * project sets them once in its own build and nothing here grows an option for
 * it.
 *
 * THE DIRECTORY IS NOT OPTIONAL. Writing to a volume root is what every other
 * piece of Wii homebrew also declines to do -- an SD card's root belongs to the
 * player and to the Homebrew Channel, and a bare save file dropped in it is
 * indistinguishable from litter. One directory per game, named after the game.
 */
#ifndef WII_SAVE_DIR
#define WII_SAVE_DIR "pnes"
#endif
#ifndef WII_SAVE_FILENAME
#define WII_SAVE_FILENAME "pnes_save"
#endif

/**
 * @brief Rejects a name that would be a path rather than a name.
 *
 * CHECKED AT COMPILE TIME because the alternative is discovering it on a
 * player's console: "sd:/" WII_SAVE_DIR "/" WII_SAVE_FILENAME is assembled by
 * the preprocessor, so a WII_SAVE_DIR containing a slash, a colon or a
 * backslash silently builds a path pointing somewhere else entirely -- possibly
 * at another game's directory. A name is one component, and the separators are
 * this file's to add.
 */
constexpr bool IsPlainName(const char* s) {
    if (!*s) return false;
    for (; *s; ++s) {
        if (*s == '/' || *s == '\\' || *s == ':') return false;
    }
    return true;
}
static_assert(IsPlainName(WII_SAVE_DIR),
    "WII_SAVE_DIR must be a single non-empty name with no '/', '\\' or ':'");
static_assert(IsPlainName(WII_SAVE_FILENAME),
    "WII_SAVE_FILENAME must be a single non-empty name with no '/', '\\' or ':'");
// 255 is FAT's long-name limit per component; the whole path is well under
// newlib's PATH_MAX either way.
static_assert(sizeof(WII_SAVE_DIR) <= 256, "WII_SAVE_DIR must be at most 255 characters");
static_assert(sizeof(WII_SAVE_FILENAME) <= 256, "WII_SAVE_FILENAME must be at most 255 characters");

constexpr char kSdDir[]   = "sd:/"  WII_SAVE_DIR;
constexpr char kUsbDir[]  = "usb:/" WII_SAVE_DIR;
constexpr char kSdFile[]  = "sd:/"  WII_SAVE_DIR "/" WII_SAVE_FILENAME;
constexpr char kUsbFile[] = "usb:/" WII_SAVE_DIR "/" WII_SAVE_FILENAME;

/**
 * @brief How far ::save::AccessibleDevices and friends count.
 *
 * TWO, AND THE ORDER IS THE POLICY. Device 0 is the front SD slot and device 1
 * is USB mass storage, which is the order a search tries them in and therefore
 * the order a save lands in when the caller names nowhere: SD first, because it
 * is the slot every Wii has and the one a player who has a save already has it
 * in. The indices are what save.hpp documents, so they cannot be reordered to
 * change that preference -- ::kDefaultDevice says it separately instead.
 */
constexpr u8 kDeviceCount = 2;
static_assert(kDeviceCount <= 8, "::save::AccessibleDevices returns the mask in a u8");

constexpr const char* kDevicePath[kDeviceCount] = { kSdFile, kUsbFile };
constexpr const char* kDeviceDir[kDeviceCount]  = { kSdDir,  kUsbDir  };
constexpr const char* kVolume[kDeviceCount]     = { "sd",    "usb"    };
constexpr const char* kVolumeRoot[kDeviceCount] = { "sd:/",  "usb:/"  };

/** @brief Where ::save::CreateSave puts a save when the caller names nowhere. */
constexpr u8 kDefaultDevice = 0;

/** @brief Not a device; what ::ParseDevice answers for a path it will not take. */
constexpr u8 kNoDevice = 0xFF;

/**
 * @brief The two drivers, indexed as everything else here is.
 *
 * NOT A POINTER TO const DISC_INTERFACE BY ACCIDENT: libogc declares
 * `__io_wiisd` const and `__io_usbstorage` non-const, so a table of
 * `const DISC_INTERFACE*` is the one type both fit without casting either.
 * Nothing here writes through them -- startup() and isInserted() are reached
 * through the vtable-ish function pointers, which are const members.
 */
const DISC_INTERFACE* const kIface[kDeviceCount] = { &__io_wiisd, &__io_usbstorage };

/**
 * @brief The unit a save file's length is rounded up to.
 *
 * 8192 BECAUSE THAT IS THE GAMECUBE'S SECTOR, which is the only reason this
 * quantity exists at all: it is what makes @p size mean the same thing on both
 * consoles (see this half's header comment). A FAT volume has no sector this
 * backend must respect -- libfat handles clusters -- so nothing here is
 * hardware, and a build that changed it would change only how much slack a
 * save file carries.
 *
 * IT IS NOT A LIMIT, UNLIKE THE GAMECUBE'S. ::kSectorMax bounds a transfer up
 * there because the card's sector is as much as one CARD_Read can move; here a
 * larger save simply occupies more units, so a @p size above 8192 is ordinary
 * rather than save::too_large. A game sized for the GameCube is a strict subset
 * of what works here.
 */
constexpr std::size_t kAllocUnit = 8192;

/**
 * @brief The zero-fill source, in .rodata rather than as an 8 KiB staging copy.
 *
 * THE GAMECUBE HALF NEEDS A WHOLE SECTOR IN RAM because CARD_Write moves a
 * sector at a time and the payload has to be assembled before the transfer
 * starts. A write() takes a length, so the caller's own bytes go out directly
 * from where they already are and the padding is this block, written as many
 * times as it takes (::PadTo). 256 bytes of .dol, and no snapshot of the
 * caller's save struct -- which is the other difference: there are no frames
 * between the snapshot and the write for the game to mutate its state in.
 */
const u8 kZeros[256] = { };

/** @brief True once this device's driver has come up. */
bool started[kDeviceCount] = { };
/** @brief True while the mount under ::kVolume is one THIS file made. */
bool mountedByUs[kDeviceCount] = { };

/**
 * @brief When a failed startup() was last attempted, and whether one ever was.
 *
 * WHY A THROTTLE EXISTS AT ALL: __io_usbstorage's startup() enumerates the USB
 * bus and waits for a device to answer, which on a Wii with nothing plugged in
 * takes seconds rather than microseconds. save.hpp invites a UI to poll
 * ::save::AccessibleDevices "a few times a second", and an unthrottled poll
 * would therefore freeze a menu for as long as the player left it open.
 *
 * So a FAILED startup is not retried for ::kStartupRetryMs. Hotplug still
 * works -- the next attempt after the interval finds the device -- it just
 * costs up to two seconds of latency instead of up to two seconds of every
 * frame. A SUCCESSFUL startup is remembered in ::started and never repeated
 * until the medium is seen to leave (::Inserted).
 */
u64  startupAt[kDeviceCount]    = { };
bool startupTried[kDeviceCount] = { };
constexpr u32 kStartupRetryMs   = 2000;

/**
 * @brief Devices this backend has tried to mount and could not.
 *
 * THE SAME POLICY THE GAMECUBE HALF APPLIES TO A BROKEN CARD, for the same
 * reason: a volume whose filesystem libfat will not mount is not somewhere this
 * game can save, and save.hpp deliberately has no code for "the medium is
 * damaged" because nothing a game can do about it is worth one. An SD card
 * formatted exFAT, a card with no partition table libfat recognises, a card
 * whose FAT is corrupt -- all of them report save::no_device, and
 * ::save::LastNativeError still holds the errno for a developer.
 *
 * IT IS CLEARED BY THE MEDIUM LEAVING (::Inserted), which is what makes a
 * reformat recoverable without restarting the game. The limitation is the
 * GameCube half's: a card swapped between two polls with no observed gap keeps
 * the old verdict until the next removal is seen.
 */
bool deviceBroken[kDeviceCount] = { };

/** @brief The device holding this game's save, once something has found it. */
bool haveLocation = false;
u8   locationDev  = 0;

save::eError lastErr    = save::found;
int          lastNative = 0;

/** @brief The last finished ::save::FreeDevices answer and what it describes. */
bool        freeValid  = false;
u8          freeMask   = 0;
u8          freeAccess = 0;
std::size_t freeSize   = 0;

/** @brief @p n rounded up to a whole ::kAllocUnit. */
constexpr std::size_t RoundUp(const std::size_t n) {
    return ((n + kAllocUnit - 1) / kAllocUnit) * kAllocUnit;
}

/**
 * @brief Translates the failing call's errno into the library's own code.
 *
 * THE SAME JOB ::MapError DOES FOR libogc's CARD_ERROR_*, with the same
 * reasoning: a game acts on "there is nowhere to save", not on which of
 * newlib's numbers produced that. Anything with no portable meaning lands on
 * save::failed and ::lastNative keeps what actually happened.
 *
 * ENOENT IS save::missing AND NOT save::bad_path, which is only unambiguous
 * because the path is this file's own: the directory and file names come from
 * the build and the volume is known to be mounted before any of them is opened,
 * so the one thing ENOENT can mean is that this game's save is not there yet --
 * the cue for ::save::CreateSave. A caller's misspelt path never reaches a
 * syscall at all; ::ParseDevice refuses it first.
 */
save::eError MapErrno(const int e) {
    switch (e) {
        case 0:            return save::found;
        case ENOENT:       return save::missing;
        case EEXIST:       return save::exists;
        case ENOSPC:
        case EDQUOT:       return save::full;
        case EACCES:
        case EPERM:
        case EROFS:        return save::denied;
        case EINVAL:
        case ENAMETOOLONG: return save::bad_path;
        // The medium went away mid-operation, which is the ordinary way a save
        // fails on a console whose storage a player can pull out by hand.
        //
        // ENOMEDIUM IS BEHIND A GUARD because newlib declares it only under
        // __LINUX_ERRNO_EXTENSIONS__ (sys/errno.h), which devkitPPC does not
        // define -- so naming it unconditionally is a compile error rather than
        // a dead branch. It is still worth listing: it is the errno for exactly
        // this situation, and a toolchain update that exposes it should map it
        // without anyone having to notice.
        case ENODEV:
        case ENXIO:
#ifdef ENOMEDIUM
        case ENOMEDIUM:
#endif
                           return save::no_device;
        default:           return save::failed;
    }
}

/** @brief Records an outcome and returns it, for `return Fail(...)`. */
save::eError Fail(const save::eError err, const int native) {
    lastErr    = err;
    lastNative = native;
    return err;
}

/** @brief Records success: save::found, and no platform code to report. */
save::eError Won() {
    lastErr    = save::found;
    lastNative = 0;
    return save::found;
}

/** @brief Drops a mount this file made, leaving the application's alone. */
void Release(const u8 d) {
    if (!mountedByUs[d]) return;
    fatUnmount(kVolume[d]);
    mountedByUs[d] = false;
}

/**
 * @brief Brings device @p d's driver up, at most once per ::kStartupRetryMs
 *        while it keeps failing.
 */
bool Started(const u8 d) {
    if (started[d]) return true;

    const u64 now = gettime();
    if (startupTried[d] &&
        ticks_to_millisecs(diff_ticks(startupAt[d], now)) < kStartupRetryMs) {
        return false;
    }
    startupTried[d] = true;
    startupAt[d]    = now;

    if (!kIface[d]->startup()) return false;
    started[d] = true;
    return true;
}

/**
 * @brief Is there media in device @p d?
 *
 * WHAT MAKES ::save::GetDeviceStatus CHEAP, and the counterpart of the
 * GameCube half's CARD_ProbeEx: isInserted() asks the driver, not the
 * filesystem, so no volume is mounted to answer it and nothing about the FAT is
 * read. A status poll costs this and no more.
 *
 * REMOVAL IS NOTICED HERE AND NOWHERE ELSE, which is why it has side effects: a
 * medium that has gone takes its mount with it (a devoptab pointing at absent
 * hardware is worse than no devoptab), clears the "will not mount" verdict so
 * that a reformatted card is usable again, and drops ::started so the driver is
 * brought back up when something is inserted next -- an SD card swap needs that
 * re-init, and the throttle in ::Started keeps the retries cheap.
 */
bool Inserted(const u8 d) {
    if (d >= kDeviceCount) return false;
    if (!Started(d)) return false;
    if (kIface[d]->isInserted()) return true;

    Release(d);
    deviceBroken[d] = false;
    started[d]      = false;
    freeValid       = false;
    return false;
}

/** @brief True when something has a usable volume mounted under ::kVolume. */
bool Mounted(const u8 d) {
    struct stat st;
    return stat(kVolumeRoot[d], &st) == 0;
}

/**
 * @brief Makes device @p d's volume usable, mounting it if nothing else has.
 *
 * THE APPLICATION MAY HAVE MOUNTED IT FIRST, and this is the whole reason the
 * check is a stat() of the volume root rather than a flag of our own: a game
 * that called fatInitDefault() for its own assets already owns "sd:", a second
 * mount under the same name would fail, and unmounting it when a save finished
 * would pull the volume out from under the game. So a mount that was already
 * there is USED AND NOT OWNED -- ::Release only ever drops one this file made.
 *
 * THE SLOW CALL IN THIS FILE. A mount reads the partition table and the FAT's
 * boot sector; it is the Wii's equivalent of CARD_Mount, with the difference
 * that there is nothing to tick it with. Nothing calls it on a status poll:
 * ::save::GetDeviceStatus and ::save::AccessibleDevices stop at ::Inserted,
 * and this runs when a real operation needs the filesystem, or when
 * ::save::FreeDevices has to ask the filesystem how much room is left.
 */
bool Mount(const u8 d) {
    if (Mounted(d)) return true;
    if (fatMountSimple(kVolume[d], kIface[d])) {
        mountedByUs[d] = true;
        return true;
    }
    // Being unmountable is a verdict about the medium, not a transient: libfat
    // has seen the sectors and declined them.
    deviceBroken[d] = true;
    return false;
}

/**
 * @brief Which device @p path names, or ::kNoDevice.
 *
 * ONLY THE PATHS THIS FILE PUBLISHES, which is the stricter half of the
 * path-based search described in this half's header comment. A caller gets
 * these from ::save::GetDeviceStatus or ::save::GetSaveLocation and hands one
 * back; anything else is a path the next ::save::GetSaveLocation would not
 * look at, so creating a file there would lose it.
 */
u8 ParseDevice(const char* const path) {
    if (!path) return kNoDevice;
    for (u8 d = 0; d < kDeviceCount; ++d) {
        if (std::strcmp(path, kDevicePath[d]) == 0) return d;
    }
    return kNoDevice;
}

/** @brief write() until @p n bytes are gone or something refuses them. */
bool WriteAll(const int fd, const void* const src, std::size_t n) {
    const u8* p = static_cast<const u8*>(src);
    while (n) {
        const ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

/** @brief read() until @p n bytes have arrived or the file runs out. */
bool ReadAll(const int fd, void* const tar, std::size_t n) {
    u8* p = static_cast<u8*>(tar);
    while (n) {
        const ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

/**
 * @brief Zero-fills from @p from to @p to.
 *
 * WHAT MAKES @p size A PREFIX. The whole allocation unit is written every time,
 * zero past the caller's bytes, so no tail of an earlier and longer save
 * survives inside it -- the same guarantee the GameCube half gets from staging
 * a whole sector.
 */
bool PadTo(const int fd, std::size_t from, const std::size_t to) {
    while (from < to) {
        const std::size_t span = to - from;
        const std::size_t n    = span < sizeof kZeros ? span : sizeof kZeros;
        if (!WriteAll(fd, kZeros, n)) return false;
        from += n;
    }
    return true;
}

/** @brief The length of @p path, or -1 with @p errOut set. */
off_t LengthOf(const char* const path, int& errOut) {
    struct stat st;
    if (stat(path, &st) != 0) {
        errOut = errno;
        return -1;
    }
    errOut = 0;
    return st.st_size;
}

/**
 * @brief Moves @p size bytes to or from the save on device @p d.
 *
 * ONE FUNCTION FOR BOTH DIRECTIONS because everything except the transfer
 * itself is shared, and the length check in particular MUST be: a save and a
 * load disagreeing about whether a file is long enough is how a game writes a
 * prefix it can never read back.
 *
 * THE LENGTH CHECK IS THE GAMECUBE'S, DOWN TO THE CODE IT REPORTS.
 * ::Resolve up there refuses a prefix longer than card_file::len with
 * CARD_ERROR_LIMIT, which ::MapError turns into save::full; this refuses a
 * prefix longer than the FAT file with save::full for the same reason -- so a
 * game whose save struct has outgrown its existing save file hears the same
 * answer on both consoles rather than one answer per backend. ::lastNative
 * holds ENOSPC, which is the nearest errno and is NOT one the filesystem
 * produced; a developer reading it alongside save::full learns that the file was
 * too short rather than the card too full.
 */
save::eError Transfer(const u8 d, const void* const src, void* const tar, const std::size_t size) {
    if (!Mount(d)) return Fail(save::no_device, ENODEV);

    const int fd = open(kDevicePath[d], src ? O_WRONLY : O_RDONLY);
    if (fd < 0) {
        const int e = errno;
        return Fail(MapErrno(e), e);
    }

    const off_t len = lseek(fd, 0, SEEK_END);
    if (len < 0) {
        const int e = errno;
        close(fd);
        return Fail(MapErrno(e), e);
    }
    if (static_cast<std::size_t>(len) < kAllocUnit || size > static_cast<std::size_t>(len)) {
        close(fd);
        return Fail(save::full, ENOSPC);
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        const int e = errno;
        close(fd);
        return Fail(MapErrno(e), e);
    }

    // fsync BEFORE close AND NOT INSTEAD OF IT: a save is the one moment a
    // player may switch the console off believing their progress is on the
    // card, and libfat's cache is free to hold a dirty sector until something
    // asks. close() flushes the file; fsync() is what also pushes the cache
    // through to the medium, and its failure is a failed save rather than a
    // detail -- which is why it is part of the && and not a courtesy call.
    const bool ok = src ? (WriteAll(fd, src, size) &&
                           PadTo(fd, size, RoundUp(size)) &&
                           fsync(fd) == 0)
                        : ReadAll(fd, tar, size);
    const int e = ok ? 0 : errno;
    if (close(fd) != 0 && ok) {
        const int ce = errno;
        return Fail(MapErrno(ce), ce);
    }
    if (!ok) return Fail(MapErrno(e), e);
    return Won();
}

/**
 * @brief ::save::Save and both ::save::Load overloads, which differ only in
 *        which way the bytes go and whether the caller named a device.
 *
 * WITH NO PATH IT SEARCHES, and the order is the remembered location first,
 * then device 0, then device 1 -- ::kDefaultDevice's preference, applied to
 * reads as well as writes. A remembered location that no longer holds the file
 * (the player swapped cards, or deleted the save from the Homebrew Channel)
 * is FORGOTTEN AND THE SEARCH CONTINUES: save::missing from the remembered
 * device is the one outcome worth retrying elsewhere, while a real failure
 * there is reported rather than papered over by trying a different card.
 */
save::eError SaveOrLoad(const void* const src, void* const tar,
                        const std::size_t size, const char* const path) {
    // size == 0 IS save::too_large, WHICH READS ODDLY AND IS THE GAMECUBE'S
    // ANSWER. ::Begin refuses `size == 0 || size > kSectorMax` with that one
    // code, and a portable caller handling it for one console should not meet a
    // different code here for the same mistake. The upper half of that test has
    // no counterpart: see ::kAllocUnit.
    if (size == 0)   return Fail(save::too_large, EINVAL);
    if (!src && !tar) return Fail(save::failed, EINVAL);

    if (path) {
        const u8 d = ParseDevice(path);
        if (d == kNoDevice) return Fail(save::bad_path, EINVAL);
        if (!Inserted(d) || deviceBroken[d]) return Fail(save::no_device, ENODEV);

        const save::eError err = Transfer(d, src, tar, size);
        if (err == save::found) {
            haveLocation = true;
            locationDev  = d;
            if (src) freeValid = false;
        }
        return err;
    }

    if (haveLocation && Inserted(locationDev) && !deviceBroken[locationDev]) {
        const save::eError err = Transfer(locationDev, src, tar, size);
        if (err == save::found) {
            if (src) freeValid = false;
            return err;
        }
        if (err != save::missing) return err;
        haveLocation = false;
    }

    save::eError best       = save::no_device;
    int          bestNative = ENODEV;
    bool         seen       = false;
    for (u8 d = 0; d < kDeviceCount; ++d) {
        if (!Inserted(d) || deviceBroken[d]) continue;

        const save::eError err = Transfer(d, src, tar, size);
        if (err == save::found) {
            haveLocation = true;
            locationDev  = d;
            if (src) freeValid = false;
            return err;
        }
        // Strictly greater, so a tie leaves the EARLIER device's answer in
        // place -- SD's verdict is the one a player is told about.
        if (!seen || Rank(err) > Rank(best)) {
            best       = err;
            bestNative = lastNative;
            seen       = true;
        }
    }
    return Fail(best, bestNative);
}

/**
 * @brief ::save::CreateSave: the one operation that makes the file and the
 *        directory holding it.
 *
 * O_EXCL AND NOT A stat() FIRST, because "does it exist" and "create it" have
 * to be one operation for save::exists to mean anything -- and because the
 * GameCube's CARD_Create reports CARD_ERROR_EXIST from inside the create for
 * the same reason.
 *
 * A HALF-WRITTEN NEW SAVE IS DELETED. If the medium fills up or is pulled out
 * between the open and the fsync, what is left is a file this game will find on
 * its next ::save::GetSaveLocation and load as if it were a save -- worse than
 * no file at all, since the player is offered a corrupt game rather than a new
 * one. The unlink is best-effort: it is a second chance to touch a medium that
 * has just failed, and its own failure changes nothing about what the caller is
 * told.
 */
save::eError Create(const void* const src, const std::size_t size, const char* const path) {
    if (size == 0) return Fail(save::too_large, EINVAL);
    if (!src)      return Fail(save::failed, EINVAL);

    u8 d = kDefaultDevice;
    if (path) {
        d = ParseDevice(path);
        if (d == kNoDevice) return Fail(save::bad_path, EINVAL);
    }
    if (!Inserted(d) || deviceBroken[d]) return Fail(save::no_device, ENODEV);
    if (!Mount(d))                       return Fail(save::no_device, ENODEV);

    if (mkdir(kDeviceDir[d], 0777) != 0 && errno != EEXIST) {
        const int e = errno;
        return Fail(MapErrno(e), e);
    }

    const int fd = open(kDevicePath[d], O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        const int e = errno;
        return Fail(MapErrno(e), e);
    }

    const bool ok = WriteAll(fd, src, size) &&
                    PadTo(fd, size, RoundUp(size)) &&
                    fsync(fd) == 0;
    const int e = ok ? 0 : errno;
    if (close(fd) != 0 && ok) {
        const int ce = errno;
        unlink(kDevicePath[d]);
        return Fail(MapErrno(ce), ce);
    }
    if (!ok) {
        unlink(kDevicePath[d]);
        return Fail(MapErrno(e), e);
    }

    haveLocation = true;
    locationDev  = d;
    freeValid    = false;
    return Won();
}

} // namespace

namespace save {

/**
 * @brief The response for an operation that has just finished, which is every
 *        one of them on this console.
 *
 * `done` IS ALWAYS TRUE, and that is this backend's whole async story: see this
 * half's header comment. The `path` is ::kDevicePath's own storage rather than a
 * buffer this file fills -- a string literal outlives every call, which is
 * stricter than save.hpp's promise that it survives until the next
 * ::GetSaveLocation and costs nothing to honour.
 */
Response Save(const void* src, const std::size_t size) {
    return { .done = true,
             .err  = SaveOrLoad(src, nullptr, size, nullptr),
             .path = haveLocation ? kDevicePath[locationDev] : nullptr };
}

Response Load(void* tar, const std::size_t size) {
    return { .done = true,
             .err  = SaveOrLoad(nullptr, tar, size, nullptr),
             .path = haveLocation ? kDevicePath[locationDev] : nullptr };
}

Response Load(void* tar, const std::size_t size, const Response* response) {
    return { .done = true,
             .err  = SaveOrLoad(nullptr, tar, size, PathOf(response)),
             .path = haveLocation ? kDevicePath[locationDev] : nullptr };
}

Response CreateSave(const void* src, const std::size_t size, const Response* response) {
    return { .done = true,
             .err  = Create(src, size, PathOf(response)),
             .path = haveLocation ? kDevicePath[locationDev] : nullptr };
}

/**
 * @brief Finds where this game's save lives.
 *
 * A COMPLETED SEARCH THAT FOUND NOTHING IS NOT AN ERROR -- `done` with
 * save::missing and a null `path`, which is the cue to call ::CreateSave. An
 * empty slot with no medium in it is different and says save::no_device, so a
 * game can ask for an SD card rather than offer to start a new save on one that
 * is not there. Exactly the GameCube half's two answers, from the same
 * vocabulary.
 */
Response GetSaveLocation() {
    eError best       = no_device;
    int    bestNative = ENODEV;
    bool   seen       = false;

    for (u8 d = 0; d < kDeviceCount; ++d) {
        if (!Inserted(d) || deviceBroken[d]) continue;
        if (!Mount(d)) {
            if (!seen || Rank(no_device) > Rank(best)) {
                best       = no_device;
                bestNative = ENODEV;
                seen       = true;
            }
            continue;
        }

        int e = 0;
        if (LengthOf(kDevicePath[d], e) >= 0) {
            haveLocation = true;
            locationDev  = d;
            Won();
            return { .done = true, .err = found, .path = kDevicePath[d] };
        }

        const eError err = MapErrno(e);
        if (!seen || Rank(err) > Rank(best)) {
            best       = err;
            bestNative = e;
            seen       = true;
        }
    }

    // A search that found nothing forgets what it thought it knew: a `path`
    // handed back now would name a file this very call failed to stat.
    haveLocation = false;
    Fail(best, bestNative);
    return { .done = true, .err = best, .path = nullptr };
}

/**
 * @brief No counterpart on this console, and not a gap to be filled.
 *
 * These two move a `card_stat` -- a 64-byte entry in a GameCube memory card's
 * own directory, carrying the comment block, banner and icon offsets the system
 * menu draws a save with. A FAT volume has no directory entry of that shape and
 * the Wii's system menu does not list homebrew saves at all, so there is
 * nothing here to read and nothing to state.
 *
 * save::no_device AND NOT save::failed: the honest content of the answer is
 * that the DEVICE this concept belongs to is not present, which is the same
 * thing src/all/save.cpp says on every target with no card. ::lastNative is
 * ENOSYS so that ::LastNativeError is non-zero -- a developer who called this
 * on the wrong console sees "not implemented" rather than "nothing went wrong".
 */
Response GetMetaData(void*) {
    return { .done = true, .err = Fail(no_device, ENOSYS), .path = nullptr };
}

Response SetMetaData(const void*) {
    return { .done = true, .err = Fail(no_device, ENOSYS), .path = nullptr };
}

/**
 * @brief Which devices hold media right now, as a bitfield.
 *
 * NO MOUNT AND NO FILESYSTEM READ, which is what makes this pollable: it is
 * ::Inserted twice, one driver question each. The first call on a Wii with
 * nothing in the USB port pays ::Started's enumeration; after that the throttle
 * keeps it to once every ::kStartupRetryMs.
 *
 * IT LEAVES ::lastErr ALONE, as save.hpp requires of a status poll: a menu
 * refreshing itself must not overwrite what a failed save left for the game to
 * report.
 */
u8 AccessibleDevices() {
    u8 mask = 0;
    for (u8 d = 0; d < kDeviceCount; ++d) {
        if (Inserted(d) && !deviceBroken[d]) mask = static_cast<u8>(mask | (1u << d));
    }
    return mask;
}

/**
 * @brief Whether device @p index holds usable media, and where a save on it
 *        would live.
 *
 * save::too_large IS UNREACHABLE HERE, unlike on the GameCube: that answer is
 * about a card whose sector width the backend cannot transfer, and a FAT volume
 * presents no such number -- libfat deals in clusters and this file never sees
 * them. save::busy is unreachable too; the probe is synchronous.
 *
 * NOT `done` NEVER HAPPENS, so a UI that polls this on its own cadence and a
 * caller that loops `while (!done)` both work, the latter running once.
 */
Response GetDeviceStatus(const u8 index) {
    if (index >= kDeviceCount)                     return { .done = true, .err = no_device, .path = nullptr };
    if (!Inserted(index) || deviceBroken[index])   return { .done = true, .err = no_device, .path = nullptr };
    return { .done = true, .err = found, .path = kDevicePath[index] };
}

/**
 * @brief Which devices could take a save of @p size bytes.
 *
 * NOT A SWEEP, AND THE REASON IS THE SAME ONE THAT MAKES EVERYTHING ELSE HERE
 * BLOCKING. The GameCube half spreads this over several calls because
 * CARD_GetFreeBlocks needs a mount and a mount is the slowest thing it does;
 * statvfs() needs a mount too, and on top of it libfat may have to walk the FAT
 * to count free clusters -- but there is no ticked form of either call to
 * spread, so a partial answer would cost exactly as much as a whole one and be
 * worth less. This measures every accessible device and answers completely.
 *
 * SO IT DOES NOT RETURN 0 ON A FIRST CALL, which save.hpp allows for rather
 * than requires ("zero means nowhere, OR not yet"): a settled answer
 * immediately is strictly more than a caller is promised.
 *
 * THE FIRST CALL AFTER A MOUNT IS THE EXPENSIVE ONE. libfat caches the free
 * cluster count once it has counted, so repeated polls are cheap; the count
 * itself, on a large card, is long enough to drop frames. The cached mask here
 * is the second line of defence -- it is reused until @p size changes, the set
 * of accessible devices changes, or a completed save, create or removal
 * invalidates it, each of which is what save.hpp says starts a fresh sweep.
 *
 * AN EXISTING SAVE BIG ENOUGH FOR @p size COUNTS AS FREE even on a full card,
 * because overwriting a file consumes no new space -- save.hpp's rule, and the
 * ordinary case for a player whose card has one game's save on it and no room
 * for another.
 */
u8 FreeDevices(const std::size_t size) {
    if (size == 0) return 0;

    const u8 access = AccessibleDevices();
    if (freeValid && freeSize == size && freeAccess == access) return freeMask;

    const std::size_t want = RoundUp(size);
    u8 mask = 0;

    for (u8 d = 0; d < kDeviceCount; ++d) {
        if (!(access & (1u << d))) continue;
        if (!Mount(d)) continue;

        struct statvfs vfs;
        if (statvfs(kVolumeRoot[d], &vfs) != 0) continue;

        // f_frsize is the fragment size free blocks are counted in; libfat
        // reports it, but a zero would make the whole product zero and read as
        // "full", so f_bsize stands in for it.
        const u64 unit  = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        const u64 avail = static_cast<u64>(vfs.f_bavail) * unit;

        int         e       = 0;
        const off_t already = LengthOf(kDevicePath[d], e);
        const u64   held    = already > 0 ? static_cast<u64>(already) : 0;
        const u64   need    = held >= want ? 0 : want - held;

        if (avail >= need) mask = static_cast<u8>(mask | (1u << d));
    }

    freeMask   = mask;
    freeSize   = size;
    freeAccess = access;
    freeValid  = true;
    return mask;
}

/** @brief The errno the last finished operation left behind, or 0. */
i32 LastNativeError() { return static_cast<i32>(lastNative); }

} // namespace save

#endif // TARGET_GC
