/**
 * @file save.cpp
 * @brief Memory-card save/load/create/locate for the GameCube (libogc CARD_*),
 *        and the honest refusal that stands in for it on Wii.
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
#ifndef SAVE_GAMECODE
#define SAVE_GAMECODE "PNES"
#endif
#ifndef SAVE_COMPANY
#define SAVE_COMPANY "BR"
#endif
constexpr char kGameCode[] = SAVE_GAMECODE;
constexpr char kCompany[]  = SAVE_COMPANY;
static_assert(sizeof(kGameCode) == 5, "SAVE_GAMECODE must be exactly 4 characters");
static_assert(sizeof(kCompany)  == 3, "SAVE_COMPANY must be exactly 2 characters");

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
 * @brief Mount work area, owned for the life of the program.
 *
 * CARD_WORKAREA_SIZE is 5*8*1024 = 40960 bytes and the card system keeps using
 * it for as long as the card is mounted, so it cannot be a local -- and 40 KiB
 * of the GameCube's 24 MiB is better spent statically than on a heap
 * allocation at the moment a player asks to save.
 */
alignas(32) u8 workarea[CARD_WORKAREA_SIZE];

/** @brief What the in-flight sequence is doing, if anything. */
enum class Op : u8 { None, Saving, Loading, Creating, Finding };

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
        case CARD_ERROR_BROKEN:
        case CARD_ERROR_ENCODING:    return save::corrupt;
        case CARD_ERROR_INSSPACE:
        case CARD_ERROR_NOENT:
        case CARD_ERROR_LIMIT:       return save::full;
        case CARD_ERROR_NOPERM:      return save::denied;
        case CARD_ERROR_NAMETOOLONG: return save::bad_path;
        default:                     return save::failed;
    }
}

/** @brief Tears down whatever the sequence had open and returns it to idle. */
void Abort() {
    if (opened)  { CARD_Close(&file); opened = false; }
    if (mounted) { CARD_Unmount(chn); mounted = false; }
    op        = Op::None;
    phase     = Phase::Probe;
    pending   = 0;
    dest      = nullptr;
    slotIndex = 0;
    slotFixed = false;
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
    return false;
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
 * Anything else -- a save or a load -- has genuinely failed, and reports
 * `false` because `bool` is all it can report. See this file's header comment.
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
    return finding;
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
 * @brief Checks the current candidate slot and learns that card's sector size.
 *
 * CARD_ProbeEx, not CARD_Probe: sect_size is the one quantity this file cannot
 * guess, since this libogc has no CARD_GetSectorSize and the staging buffer is
 * a fixed-size object.
 */
bool ProbeCurrentSlot() {
    s32 sect = 0;
    const s32 err = CARD_ProbeEx(kSlots[slotIndex], nullptr, &sect);
    if (err != CARD_ERROR_READY) {
        lastErr = MapError(err); lastNative = err;
        return false;
    }
    if (sect <= 0 || static_cast<u32>(sect) > kSectorMax) {
        // A card whose sector is wider than the staging buffer: refused rather
        // than transferred at the wrong width and called a save.
        lastErr = save::failed; lastNative = CARD_ERROR_FATAL_ERROR;
        return false;
    }
    chn    = kSlots[slotIndex];
    sector = static_cast<u32>(sect);
    return true;
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
const char* PathOf(const save::Response* response) {
    return (response && response->path && response->path[0]) ? response->path : nullptr;
}

/** @brief Starts a sequence, latching everything it will need. */
bool Begin(Op want, const u8* src, u8* tar, std::size_t size, const char* path) {
    // `path` here is already unwrapped from the caller's save::Response by the
    // entry points below -- only its `path` field is consumed. See ::PathOf.
    if (want != Op::Finding) {
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
    }

    op       = want;
    pending  = size;
    phase    = Phase::Probe;
    detached = false;
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
                if (AdvanceSlot()) return false;      // try the other slot
                return Exhausted(lastErr, lastNative);
            }
            phase = Phase::Mount;
            return false;

        case Phase::Mount: {
            const s32 err = CARD_Mount(chn, workarea, DetachCallback);
            if (err != CARD_ERROR_READY) {
                if (AdvanceSlot()) { phase = Phase::Probe; return false; }
                return Exhausted(MapError(err), err);
            }
            mounted = true;
            phase   = Phase::Resolve;
            return false;
        }

        case Phase::Resolve: {
            const s32 err = Resolve();
            if (err != CARD_ERROR_READY) {
                // Not on this card -- try the next one, unless a path pinned
                // the slot, in which case "not here" is the whole answer.
                if (AdvanceSlot()) { phase = Phase::Probe; return false; }
                return Exhausted(MapError(err), err);
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
bool Busy(Op want) { return op != Op::None && op != want; }

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

Response Save(const u8* src, const std::size_t size) {
    if (Busy(Op::Saving)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Saving, src, nullptr, size, nullptr));
}

Response Load(u8* tar, const std::size_t size) {
    if (Busy(Op::Loading)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Loading, nullptr, tar, size, nullptr));
}

Response Load(u8* tar, const std::size_t size, const Response* response) {
    if (Busy(Op::Loading)) return { .done = false, .err = busy, .path = nullptr };
    return Report(Tick(Op::Loading, nullptr, tar, size, PathOf(response)));
}

/**
 * @brief Creates the save at the location @p response names, writing @p size
 *        bytes of @p src.
 *
 * A create must name its file -- there is nothing to search for yet -- so a
 * response with no path is refused outright rather than left pending. A
 * response straight out of an unsuccessful ::GetSaveLocation is exactly that:
 * the caller has to put a location of its own choosing in first.
 */
Response CreateSave(const u8* src, const std::size_t size, const Response* response) {
    if (Busy(Op::Creating)) return { .done = false, .err = busy, .path = nullptr };
    if (!PathOf(response))  return { .done = true,  .err = bad_path, .path = nullptr };
    return Report(Tick(Op::Creating, src, nullptr, size, PathOf(response)));
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

} // namespace save

#else // TARGET_WII

/*
 * THE WII HAS NO EXI MEMORY CARD SLOT, so there is nothing here to port: the
 * CARD_* API above addresses hardware this console does not have. Its
 * persistent storage is the front SD slot (libfat) or NAND (ISFS), both a
 * different medium with a different failure set, and neither is implemented.
 *
 * EVERY CALL IS `done` WITH save::missing, never a silent success and never an
 * endless `!done`: a Wii build links, runs, and tells a call site on its first
 * tick that there is no save here. The two answers a game cannot recover from
 * are "succeeded" (its data is gone) and "not finished yet" (its loop never
 * ends).
 *
 * save::missing AND NOT save::no_device, which would be the more precise
 * answer: save.hpp gates every code past ::missing behind TARGET_GC, so this
 * half of the file cannot name one. Widening that gate -- to TARGET_OGC, or
 * dropping it -- is what a real Wii backend would need first.
 */
namespace save {

Response Save(const u8*, const std::size_t) {
    return { .done = true, .err = missing, .path = nullptr };
}

Response Load(u8*, const std::size_t) {
    return { .done = true, .err = missing, .path = nullptr };
}

Response Load(u8*, const std::size_t, const Response*) {
    return { .done = true, .err = missing, .path = nullptr };
}

Response CreateSave(const u8*, const std::size_t, const Response*) {
    return { .done = true, .err = missing, .path = nullptr };
}

// A finished search with nowhere to have looked.
Response GetSaveLocation() {
    return { .done = true, .err = missing, .path = nullptr };
}

} // namespace save

#endif // TARGET_GC
