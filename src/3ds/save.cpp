/**
 * @file save.cpp
 * @brief Save/load/create/locate for the Nintendo 3DS, as a file on the SD
 *        card reached through libctru's archive devoptab.
 *
 * THE FOURTH BACKEND, AND IT IS THE SWITCH'S WITH ONE QUESTION ANSWERED
 * DIFFERENTLY. src/switch/save.cpp is already blocking stdio against a single
 * device the operating system mounted before main() ran, which is exactly this
 * console's situation -- libctru's default __appInit mounts the SD card under
 * the "sdmc" devoptab, open/read/write/stat/mkdir all work on it, and errno is
 * newlib's. So everything below is that file's shape, and the four places it is
 * NOT that file's shape say so where they are:
 *
 *   1. ::Mount calls archiveMountSdmc() where the Switch calls
 *      fsdevMountSdmc(), and ::Mounted asks libsysbase's own device table
 *      (FindDevice) because libctru publishes no "is this archive mounted"
 *      entry point of fsdev's kind.
 *   2. ::Detected EXISTS AT ALL, which is the one real capability this console
 *      has and the Switch does not: the SD card is a card a player can take
 *      out of a running console, and the fs service will say so
 *      (FSUSER_IsSdmcDetected). The Switch can only find out by failing. See
 *      ::save::AccessibleDevices for what that buys and what it costs.
 *   3. ::save::FreeDevices measures with FSUSER_GetSdmcArchiveResource, which
 *      answers in CLUSTERS and has to be multiplied, where the Switch's
 *      fsFsGetFreeSpace answers in bytes.
 *   4. The save lives under sdmc:/3ds/, which is this console's homebrew
 *      convention in the place sdmc:/switch/ is the other one's.
 *
 * WHY THE SD CARD AND NOT A SAVEDATA OR EXTDATA ARCHIVE. The 3DS has both --
 * ARCHIVE_SAVEDATA, the save partition inside a title's own CIA/CCI, and
 * ARCHIVE_EXTDATA, a per-title directory tree on the SD card that the System
 * Settings data manager lists and the system backs up -- and this backend
 * deliberately uses neither. BOTH ARE ADDRESSED BY A TITLE ID. The demo ships
 * as a .3dsx run from the Homebrew Launcher, which executes it inside the
 * launcher's OWN borrowed title (historically Download Play), so a .3dsx has no
 * title id of its own to name: ARCHIVE_SAVEDATA would open the HOST title's
 * save partition -- somebody else's data, and on a card-launched hbmenu not
 * even writable -- and an extdata archive would have to be created against an
 * id this build does not own. sdmc:/3ds/ is what a .3dsx actually has, and it
 * is where every other piece of 3DS homebrew keeps its data.
 *
 * A BUILD THAT BECOMES A CIA WOULD CHANGE THREE THINGS and no more, the same
 * three the Switch's header comment lists for an installed title: ::Mount would
 * archiveMount(ARCHIVE_SAVEDATA, fsMakePath(PATH_EMPTY, ""), kDevice) instead
 * of archiveMountSdmc(), the path constants would lose their "sdmc:/3ds"
 * prefix, and ::Transfer and ::Create would need an
 * archiveCommitSaveData(kDevice) after their fsync, because the savedata
 * partition's journal is not flushed by the file's own close. Nothing else here
 * is about which archive is underneath.
 *
 * NOTHING IS TICKED, FOR THE SWITCH'S AND THE WII'S REASON. The devoptab is
 * blocking and has no "advance this transfer by one step" entry point to build
 * a state machine out of, so every call finishes the whole operation and
 * reports `done` on its first tick. A caller's
 *
 *     while (!response.done) { ...one frame... }
 *
 * loop therefore runs exactly once -- the same loop, with the same meaning, as
 * on the GameCube where it runs several times. save::busy is declared on this
 * target (save.hpp gates the vocabulary on SAVE_DEVICE) so that a game written
 * against the GameCube compiles here, and is never returned.
 *
 * THE COST OF A SAVE IS IPC TO THE fs PROCESS PLUS SD TRAFFIC, and on this
 * console that is slower than on the Switch by a wide margin -- an ARM11 at
 * 268MHz talking to an SD card over a 1-bit-wide bus on an o3DS. A save of one
 * 8KiB unit still lands inside a frame or two rather than needing a screen in
 * front of it (which is why ::SAVE_DEVICE_WAIT is not defined here either --
 * pnes/technology.hpp), but it is the slowest of the three filesystem backends
 * and a game writing a large save should expect to drop a frame. The expensive
 * calls are named where they happen (::Mount, ::Detected, ::save::FreeDevices).
 *
 * THE SEARCH IS PATH-BASED, LIKE THE SWITCH'S AND THE WII'S. There is no
 * equivalent of CARD_FindFirst's gamecode filter on a FAT volume, so a save is
 * found by looking where it was put: the location is fixed by the build
 * (CTR_SAVE_DIR / CTR_SAVE_FILENAME below) and ::ParseDevice accepts ONLY the
 * one complete path this file publishes. A path with any other spelling is
 * save::bad_path rather than a file created where nothing will ever look for it
 * again.
 *
 * @p size IS A PREFIX LENGTH, exactly as it is on the other three consoles, and
 * ::kAllocUnit is what makes that mean the same thing here: a save is rounded
 * up to a whole unit, a prefix shorter than the file reads back, and a prefix
 * longer than the file is save::full.
 *
 * NO LOGGING, for the reason the other three backends give: log() resolves on
 * this platform to logger.hpp's 3DS branch, which writes through the console
 * this backend has no business owning -- and on a .3dsx that console is the top
 * screen the game is drawing. What actually failed lands in ::lastNative
 * instead; see ::save::LastNativeError.
 */
#include "internal.hpp"
#include <pnes/save.hpp>

#include <3ds.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/**
 * @brief The `path` a caller's save::Response names, or nullptr for none.
 *
 * THE SAME FUNCTION src/ogc/save.cpp SHARES BETWEEN ITS TWO HALVES AND
 * src/switch/save.cpp REPEATS, repeated once more rather than hoisted into a
 * header -- hoisting it would be the first shared SOURCE between platform
 * backends in this library, there is still no src/all/save_common.hpp, and one
 * function is not enough to start one with. What matters is that it behaves
 * identically, which is why its reasoning is repeated too --
 *
 * AN EMPTY STRING IS NOT A PATH. save.hpp's flows hand a Response straight
 * back from ::save::GetSaveLocation or ::save::GetDeviceStatus, and one that
 * found nothing carries a null `path`; a caller that built a Response by hand,
 * or zeroed a buffer and passed that, carries "". Both mean "I am not naming a
 * location", so both reach the backend as nullptr and get ::kSaveFile.
 */
const char* PathOf(const save::Response* const response) {
    return (response && response->path && response->path[0]) ? response->path : nullptr;
}

/**
 * @brief Where this game's save lives on the SD card, as the build spells it.
 *
 * TWO NAMES AND NOT ONE PATH, matching WII_SAVE_DIR / NX_SAVE_DIR: the
 * directory has to be created before the file can be, and a mkdir needs it on
 * its own. Compile definitions rather than a CMake-generated header, for the
 * reason the other backends' identity knobs are -- a project sets them once in
 * its own build and nothing here grows an option for it.
 *
 * THE DIRECTORY IS NOT OPTIONAL AND NEITHER IS ITS PARENT. "sdmc:/3ds" is
 * where the Homebrew Launcher looks for .3dsx files and where homebrew keeps
 * its per-app data by convention, so a save belongs at
 * sdmc:/3ds/<game>/<file> and not in the volume root -- an SD card's root on
 * this console also holds the system's own Nintendo 3DS directory, and a bare
 * save file dropped beside it is indistinguishable from litter. ::Create makes
 * both levels.
 */
#ifndef CTR_SAVE_DIR
#define CTR_SAVE_DIR "pnes"
#endif
#ifndef CTR_SAVE_FILENAME
#define CTR_SAVE_FILENAME "pnes_save"
#endif

/**
 * @brief Rejects a name that would be a path rather than a name.
 *
 * CHECKED AT COMPILE TIME because the alternative is discovering it on a
 * player's console: the path below is assembled by the preprocessor, so a
 * CTR_SAVE_DIR containing a slash, a colon or a backslash silently builds a
 * path pointing somewhere else entirely -- possibly at another homebrew app's
 * directory under sdmc:/3ds. A name is one component, and the separators are
 * this file's to add.
 */
constexpr bool IsPlainName(const char* s) {
    if (!*s) return false;
    for (; *s; ++s) {
        if (*s == '/' || *s == '\\' || *s == ':') return false;
    }
    return true;
}
static_assert(IsPlainName(CTR_SAVE_DIR),
    "CTR_SAVE_DIR must be a single non-empty name with no '/', '\\' or ':'");
static_assert(IsPlainName(CTR_SAVE_FILENAME),
    "CTR_SAVE_FILENAME must be a single non-empty name with no '/', '\\' or ':'");
// 255 is FAT's long-name limit per component, which is what this console's SD
// card is formatted as (the system requires FAT32 or, with a patch, exFAT);
// the whole path is well under the fs service's own limit either way.
static_assert(sizeof(CTR_SAVE_DIR) <= 256, "CTR_SAVE_DIR must be at most 255 characters");
static_assert(sizeof(CTR_SAVE_FILENAME) <= 256, "CTR_SAVE_FILENAME must be at most 255 characters");

/**
 * @brief The devoptab name libctru mounts the SD card under, WITH the colon.
 *
 * WITH THE COLON, WHICH IS THE OPPOSITE OF THE SWITCH'S ::kDevice and not a
 * slip. The two consumers here take opposite spellings: FindDevice() parses a
 * PATH and looks for the ':' to know where the device name ends (a name with no
 * colon is taken as a relative path and answers the default device instead,
 * which on a .3dsx is "sdmc" too -- so the colonless form would appear to work
 * and would stop telling the truth the moment anything called chdir), while
 * archiveMountSdmc() takes no name at all. The Switch's pair is the other way
 * round: fsdevGetDeviceFileSystem() wants the bare name.
 */
constexpr char kDevice[]   = "sdmc:";
/** @brief The Homebrew Launcher's own directory, which ::Create makes if a card lacks it. */
constexpr char kHomebrew[] = "sdmc:/3ds";
constexpr char kSaveDir[]  = "sdmc:/3ds/" CTR_SAVE_DIR;
constexpr char kSaveFile[] = "sdmc:/3ds/" CTR_SAVE_DIR "/" CTR_SAVE_FILENAME;

/**
 * @brief How far ::save::AccessibleDevices and friends count.
 *
 * ONE, FOR THE SWITCH'S REASON: there is one SD card, and nothing else on this
 * console is somewhere a game may write. The internal NAND holds the system's
 * own titles and savedata and is not reachable from a .3dsx at all without
 * ARCHIVE_NAND_* and permissions an unprivileged homebrew process does not
 * have; a game cartridge's save chip belongs to the cartridge's own title. So
 * the searches collapse exactly as they do on the Switch -- no Rank(), no
 * remembered device index, one candidate for ::save::FreeDevices to measure --
 * while the device VOCABULARY stays save.hpp's, because a portable caller walks
 * it the same way on every console. A slot picker drawn against this backend
 * finds exactly one row.
 */
constexpr u8 kDeviceCount = 1;
static_assert(kDeviceCount <= 8, "::save::AccessibleDevices returns the mask in a u8");

/** @brief The only device: the SD card, as bit 0 and index 0. */
constexpr u8 kSdDevice = 0;

/** @brief Not a device; what ::ParseDevice answers for a path it will not take. */
constexpr u8 kNoDevice = 0xFF;

/**
 * @brief The unit a save file's length is rounded up to.
 *
 * 8192 BECAUSE THAT IS THE GAMECUBE'S SECTOR -- the same constant, for the same
 * reason, as the Wii and Switch halves' ::kAllocUnit. It is what makes @p size
 * mean the same thing on all four consoles: a save file is a whole number of
 * units long, a shorter prefix reads back, and a prefix longer than the file is
 * save::full. Nothing about it is hardware here; the SD card's own cluster size
 * is the filesystem's business, and the one place this file does see that
 * number (::save::FreeDevices, which has to multiply by it) is measuring free
 * space rather than deciding a length.
 *
 * IT IS NOT A LIMIT. A larger save simply occupies more units, so a @p size
 * above 8192 is ordinary rather than save::too_large -- a game sized for the
 * GameCube is a strict subset of what works here, which is the point.
 */
constexpr std::size_t kAllocUnit = 8192;

/**
 * @brief The zero-fill source, in .rodata rather than as an 8 KiB staging copy.
 *
 * The Wii and Switch halves' reasoning exactly: a write() takes a length, so
 * the caller's own bytes go out from where they already are and the padding is
 * this block, written as many times as it takes (::PadTo). 256 bytes of .3dsx,
 * and no snapshot of the caller's save struct.
 */
const u8 kZeros[256] = { };

/**
 * @brief When a failed mount was last attempted, and whether one ever was.
 *
 * A THROTTLE FOR A CALL THAT ALMOST NEVER RUNS, like the Switch's. libctru's
 * default __appInit mounts the SD card before main(), so ::Mount's first
 * question is answered by ::Mounted and no mount is attempted at all in the
 * ordinary case. The retry exists for the cases where that mount did not happen
 * or did not survive: an application with its own __appInit that left the
 * archive out, or a card that was pulled out and pushed back in.
 *
 * THE COST IT GUARDS AGAINST IS A SERVICE ROUND TRIP plus an archive open, and
 * it is throttled because save.hpp invites a UI to poll
 * ::save::AccessibleDevices "a few times a second" -- a failing mount
 * re-attempted on every poll is IPC a menu does not need to pay for on an
 * ARM11. Hotplug still works to the extent the system allows it: the next
 * attempt after the interval finds the card.
 *
 * osGetTime() AND NOT svcGetSystemTick(), which is the cheaper call and the
 * wrong shape: a tick count has to be divided by a clock rate that differs
 * between an o3DS and an n3DS in CTR mode, and this is a two-second throttle
 * rather than a measurement. osGetTime answers milliseconds since the epoch
 * from the shared system config page -- no syscall, no clock constant.
 */
u64  mountAt    = 0;
bool mountTried = false;
constexpr u64 kMountRetryMs = 2000;

save::eError lastErr    = save::found;
int          lastNative = 0;

/** @brief The last finished ::save::FreeDevices answer and what it describes. */
bool        freeValid = false;
u8          freeMask  = 0;
std::size_t freeSize  = 0;

/** @brief Whether something has found this game's save since it last looked. */
bool haveLocation = false;

/** @brief @p n rounded up to a whole ::kAllocUnit. */
constexpr std::size_t RoundUp(const std::size_t n) {
    return ((n + kAllocUnit - 1) / kAllocUnit) * kAllocUnit;
}

/**
 * @brief Translates the failing call's errno into the library's own code.
 *
 * THE SWITCH'S ::MapErrno, AND IT HAS TO BE, because what reaches here is
 * newlib's errno either way: libctru's archive devoptab translates the fs
 * service's own Result codes into errno inside the devoptab (a missing file is
 * ENOENT, a full card ENOSPC, a write-protected one EACCES), so the mapping a
 * caller sees is the same one libfat and fsdev produced. A game that handles
 * save::full on one console handles it here.
 *
 * ENOENT IS save::missing AND NOT save::bad_path, which is unambiguous only
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
        // The card went away mid-operation, which on this console is the player
        // pulling it out of a running game -- the thing the Switch's equivalent
        // comment says cannot happen there.
        //
        // ENOMEDIUM IS BEHIND A GUARD because newlib declares it only under
        // __LINUX_ERRNO_EXTENSIONS__, which devkitARM does not define, so
        // naming it unconditionally is a compile error rather than a dead
        // branch. It is still worth listing, for a toolchain update that
        // exposes it.
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

/**
 * @brief True when the SD card is mounted under ::kDevice right now.
 *
 * libsysbase's OWN DEVICE TABLE, which is the nearest thing this platform has
 * to the Switch's fsdevGetDeviceFileSystem: FindDevice() is a walk of the
 * devoptab list comparing names, so it cannot touch the medium, cannot fail for
 * a reason that is not "no such device", and costs no IPC. libctru publishes no
 * "is this archive mounted" entry point of its own -- archiveMountSdmc() is
 * write-only in that sense -- and a stat() of the root would be the Wii's
 * workaround, which this does not need.
 */
bool Mounted() {
    return FindDevice(kDevice) >= 0;
}

/**
 * @brief Whether the fs service can see a card in the slot.
 *
 * THE ONE THING THIS BACKEND KNOWS THAT THE SWITCH'S CANNOT. An SD card on this
 * console is a card in a slot on the side of a handheld, and it can be taken
 * out while a game is running -- so "is the device there" is a real question
 * with a real answer, and FSUSER_IsSdmcDetected is the fs process answering it
 * about the physical slot rather than about anybody's mount. The Switch's
 * equivalent can only report whether libnx still has the volume mounted, which
 * stays true after the system has taken the card away, so that backend
 * discovers a removal by failing an operation. This one does not have to.
 *
 * IT IS IPC AND THE MOUNT CHECK IS NOT, which is the cost: one round trip to
 * fs:USER per call, where ::Mounted is a pointer walk. That is still cheap
 * enough for the "a few times a second" poll save.hpp describes -- it is the
 * same order as the GameCube's bus transaction for the same question, and three
 * orders below a mount -- but it is why ::save::AccessibleDevices asks ::Mounted
 * FIRST and only pays for this when the answer is yes.
 *
 * A SERVICE CALL THAT FAILS IS TREATED AS "NO CARD", not as "assume yes".
 * FSUSER_IsSdmcDetected fails when fs:USER is not available to this process at
 * all, and a process that cannot ask the filesystem anything cannot save
 * either -- so the honest answer is the pessimistic one, and the operation that
 * follows would have failed anyway.
 */
bool Detected() {
    bool detected = false;
    if (R_FAILED(FSUSER_IsSdmcDetected(&detected))) return false;
    return detected;
}

/**
 * @brief Makes the SD card usable, mounting it if nothing else has.
 *
 * ALREADY MOUNTED IS THE NORMAL CASE, AND THE MOUNT IS NOT OURS. libctru's
 * default startup owns "sdmc" before main() runs and the application's own
 * assets may be open on it (src/3ds/audio.cpp streams the soundtrack out of
 * RomFS, but a project's own data may well be on the card), so there is nothing
 * here to release and no `mountedByUs` flag of the Wii's kind: unmounting a
 * volume this backend did not mount would pull the SD card out from under
 * whatever else is reading it, and unmounting one it DID mount would do the same
 * to the next save. The device stays up for the life of the process either way.
 *
 * THE SLOW CALL IN THIS FILE, in the case where it does run: a mount is IPC to
 * the fs service plus an archive open. Nothing calls it on a status poll --
 * ::save::AccessibleDevices and ::save::GetDeviceStatus stop at ::Mounted and
 * ::Detected -- and it runs when a real operation needs the filesystem, or when
 * ::save::FreeDevices has to ask how much room is left.
 *
 * THE CARD IS CHECKED BEFORE IT IS MOUNTED, which the Switch's counterpart
 * cannot do and which saves the expensive half of this function in the case it
 * exists for: a mount attempted against an empty slot is IPC spent to be told
 * what ::Detected already knew, and it is exactly the case the throttle below
 * would then have to absorb. A card that is present but unreadable still
 * reaches the mount and still fails there -- being broken is only discoverable
 * by mounting, as save.hpp says of every backend.
 */
bool Mount() {
    if (Mounted()) return true;
    if (!Detected()) {
        freeValid = false;
        return false;
    }

    const u64 now = osGetTime();
    if (mountTried && now - mountAt < kMountRetryMs) return false;
    mountTried = true;
    mountAt    = now;

    if (R_FAILED(archiveMountSdmc())) {
        // A mount that failed takes the cached free-space answer with it: the
        // mask described a card this backend can no longer reach.
        freeValid = false;
        return false;
    }
    return true;
}

/**
 * @brief Which device @p path names, or ::kNoDevice.
 *
 * ONE PATH AND ONE DEVICE, which is the stricter half of the path-based search
 * described in this file's header comment. A caller gets ::kSaveFile from
 * ::save::GetDeviceStatus or ::save::GetSaveLocation and hands it back;
 * anything else is a path the next ::save::GetSaveLocation would not look at,
 * so creating a file there would lose it.
 */
u8 ParseDevice(const char* const path) {
    if (!path) return kNoDevice;
    return std::strcmp(path, kSaveFile) == 0 ? kSdDevice : kNoDevice;
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
 * a whole sector and the other two filesystem backends from this same loop.
 *
 * NOT ftruncate(). Extending a file with a truncate would zero the extension
 * and cost one call instead of a few writes, and it would stop the padding from
 * OVERWRITING anything: a save that shrank would leave the previous save's bytes
 * sitting in the unit past it, since the file is already long enough to need no
 * extension at all. Writing the zeros is what makes the unit's whole content
 * this save's. (It is also not reliably there to call: libctru's archive
 * devoptab implements ftruncate, the older sdmc one answered ENOSYS, and this
 * file has no reason to care which one a toolchain mounted.)
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
 * @brief Moves @p size bytes to or from the save on the SD card.
 *
 * ONE FUNCTION FOR BOTH DIRECTIONS, like the Wii's and the Switch's ::Transfer,
 * because everything except the transfer itself is shared and the length check
 * in particular MUST be: a save and a load disagreeing about whether a file is
 * long enough is how a game writes a prefix it can never read back.
 *
 * THE LENGTH CHECK IS THE GAMECUBE'S, DOWN TO THE CODE IT REPORTS. A prefix
 * longer than the file on the medium is save::full with ENOSPC in
 * ::lastNative -- an errno the filesystem did not produce, which is the point:
 * a developer reading it alongside save::full learns that the FILE was too
 * short rather than the card too full. A game whose save struct has outgrown
 * its existing save file hears the same answer on all four consoles.
 *
 * fsync BEFORE close AND NOT INSTEAD OF IT. A save is the one moment a player
 * may close the lid or hold the power button believing their progress is on the
 * card; close() flushes the file, fsync() (FSFILE_Flush underneath) is what
 * pushes it through the fs process's own buffering, and its failure is a failed
 * save rather than a detail -- which is why it is part of the && and not a
 * courtesy call. A CIA-installed build would also need
 * archiveCommitSaveData() here; see this file's header comment on savedata.
 */
save::eError Transfer(const void* const src, void* const tar, const std::size_t size) {
    if (!Mount()) return Fail(save::no_device, ENODEV);

    const int fd = open(kSaveFile, src ? O_WRONLY : O_RDONLY);
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

    haveLocation = true;
    if (src) freeValid = false;
    return Won();
}

/**
 * @brief ::save::Save and both ::save::Load overloads, which differ only in
 *        which way the bytes go and whether the caller named a location.
 *
 * NO SEARCH, BECAUSE THERE IS NOWHERE TO SEARCH -- the Switch's reasoning, for
 * the same reason (::kDeviceCount). What survives of the Wii's across-devices
 * flow is the VALIDATION: a named path is checked rather than defaulted,
 * because defaulting a wrong path is how a save lands somewhere it will not be
 * found again.
 */
save::eError SaveOrLoad(const void* const src, void* const tar,
                        const std::size_t size, const char* const path) {
    // size == 0 IS save::too_large, WHICH READS ODDLY AND IS THE GAMECUBE'S
    // ANSWER: ::Begin over there refuses `size == 0 || size > kSectorMax` with
    // that one code, and a portable caller handling it for one console should
    // not meet a different code here for the same mistake. The upper half of
    // that test has no counterpart; see ::kAllocUnit.
    if (size == 0)    return Fail(save::too_large, EINVAL);
    if (!src && !tar) return Fail(save::failed, EINVAL);

    if (path && ParseDevice(path) == kNoDevice) return Fail(save::bad_path, EINVAL);

    return Transfer(src, tar, size);
}

/**
 * @brief ::save::CreateSave: the one operation that makes the file and the
 *        directories holding it.
 *
 * O_EXCL AND NOT A stat() FIRST, because "does it exist" and "create it" have
 * to be one operation for save::exists to mean anything -- and because the
 * GameCube's CARD_Create reports CARD_ERROR_EXIST from inside the create for
 * the same reason.
 *
 * TWO DIRECTORIES, NOT ONE, as on the Switch: the save lives at
 * sdmc:/3ds/<dir>/<file>, and while "3ds" exists on any card that has ever run
 * the Homebrew Launcher, a card freshly formatted for a build that was launched
 * some other way (a CIA, a card redirect) may not have it -- and mkdir does not
 * create parents. An EEXIST from either level is the ordinary case and not an
 * error.
 *
 * A HALF-WRITTEN NEW SAVE IS DELETED. If the card fills up or is taken out
 * between the open and the fsync, what is left is a file this game will find on
 * its next ::save::GetSaveLocation and load as if it were a save -- worse than
 * no file at all, since the player is offered a corrupt game rather than a new
 * one. The unlink is best-effort: its own failure changes nothing about what the
 * caller is told. THE DIRECTORIES ARE LEFT BEHIND, deliberately -- an empty
 * directory is not mistakable for a save, and removing one this call may not
 * have created is a worse mistake than leaving it.
 */
save::eError Create(const void* const src, const std::size_t size, const char* const path) {
    if (size == 0) return Fail(save::too_large, EINVAL);
    if (!src)      return Fail(save::failed, EINVAL);

    if (path && ParseDevice(path) == kNoDevice) return Fail(save::bad_path, EINVAL);
    if (!Mount())                               return Fail(save::no_device, ENODEV);

    for (const char* const dir : { kHomebrew, kSaveDir }) {
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            const int e = errno;
            return Fail(MapErrno(e), e);
        }
    }

    const int fd = open(kSaveFile, O_WRONLY | O_CREAT | O_EXCL, 0666);
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
        unlink(kSaveFile);
        return Fail(MapErrno(ce), ce);
    }
    if (!ok) {
        unlink(kSaveFile);
        return Fail(MapErrno(e), e);
    }

    haveLocation = true;
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
 * file's header comment. The `path` is ::kSaveFile's own storage rather than a
 * buffer this file fills -- a string literal outlives every call, which is
 * stricter than save.hpp's promise that it survives until the next
 * ::GetSaveLocation and costs nothing to honour.
 */
Response Save(const void* src, const std::size_t size) {
    return { .done = true,
             .err  = SaveOrLoad(src, nullptr, size, nullptr),
             .path = haveLocation ? kSaveFile : nullptr };
}

Response Load(void* tar, const std::size_t size) {
    return { .done = true,
             .err  = SaveOrLoad(nullptr, tar, size, nullptr),
             .path = haveLocation ? kSaveFile : nullptr };
}

Response Load(void* tar, const std::size_t size, const Response* response) {
    return { .done = true,
             .err  = SaveOrLoad(nullptr, tar, size, PathOf(response)),
             .path = haveLocation ? kSaveFile : nullptr };
}

Response CreateSave(const void* src, const std::size_t size, const Response* response) {
    return { .done = true,
             .err  = Create(src, size, PathOf(response)),
             .path = haveLocation ? kSaveFile : nullptr };
}

/**
 * @brief Finds where this game's save lives.
 *
 * A COMPLETED SEARCH THAT FOUND NOTHING IS NOT AN ERROR -- `done` with
 * save::missing and a null `path`, which is the cue to call ::CreateSave. A
 * card that is not there is different and says save::no_device, so a game can
 * ask for an SD card rather than offer to start a new save on one that is
 * absent. Exactly the other three backends' two answers, from the same
 * vocabulary.
 *
 * IT IS A stat AND NOTHING MORE: the location is fixed by the build, so
 * "finding" the save is asking whether the one path this file publishes exists.
 * A search that finds nothing forgets what it thought it knew, because a `path`
 * handed back afterwards would name a file this very call failed to stat.
 */
Response GetSaveLocation() {
    if (!Mount()) {
        haveLocation = false;
        return { .done = true, .err = Fail(no_device, ENODEV), .path = nullptr };
    }

    int e = 0;
    if (LengthOf(kSaveFile, e) >= 0) {
        haveLocation = true;
        Won();
        return { .done = true, .err = found, .path = kSaveFile };
    }

    haveLocation = false;
    return { .done = true, .err = Fail(MapErrno(e), e), .path = nullptr };
}

/**
 * @brief No counterpart on this console, and not a gap to be filled.
 *
 * These two move a `card_stat` -- a 64-byte entry in a GameCube memory card's
 * own directory, carrying the comment block, banner and icon offsets the system
 * menu draws a save with. A file on an SD card has no directory entry of that
 * shape. The 3DS does have a system that draws a save with a name and an icon --
 * the Home menu and the data manager, which read a title's SMDH -- but that is
 * the TITLE's metadata and not the save's, it is built at package time
 * (ctr_generate_smdh() in this project's CMakeLists.txt) rather than written by
 * a running game, and a .3dsx's saves are not listed there at all. So there is
 * nothing here to read and nothing to state.
 *
 * save::no_device AND NOT save::failed, matching the Wii and Switch halves: the
 * honest content of the answer is that the DEVICE this concept belongs to is not
 * present, which is what src/all/save.cpp says on every target with no card.
 * ::lastNative is ENOSYS so that ::LastNativeError is non-zero -- a developer
 * who called this on the wrong console sees "not implemented" rather than
 * "nothing went wrong".
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
 * ONE BIT, AND IT IS HONEST ABOUT A CARD THE PLAYER TOOK OUT, which is the one
 * place this backend answers better than the Switch's rather than merely
 * differently. The mount check alone would keep saying "yes" after the card was
 * pulled -- a devoptab entry outlives its medium -- so the bit is the mount AND
 * the fs process's own view of the slot (::Detected). A player who removes the
 * card mid-game sees the bit clear on the next poll, and a UI drawing a slot
 * row greys it out without having to attempt a save to find out.
 *
 * THE ORDER OF THE TWO TESTS IS THE COST MODEL. ::Mounted is a devoptab walk
 * and ::Detected is IPC to fs:USER, so the cheap one goes first and the
 * expensive one is paid only when there is something to confirm -- a console
 * with no SD card at all costs a pointer walk per poll, the same as the Switch.
 * A UI may poll this at save.hpp's "a few times a second" either way; polling
 * it every frame would be IPC spent on an answer that cannot change that fast.
 *
 * A MOUNT IS NOT ATTEMPTED FROM HERE, which is what keeps it cheap: a card
 * that was re-inserted reads as inaccessible until something that really needs
 * the filesystem retries the mount (::Mount, on a save, a load, a create, a
 * locate or a free-space measurement). The one thing this cannot say is that
 * the card is UNREADABLE as opposed to absent -- being broken is only
 * discoverable by mounting, which save.hpp's own documentation says of every
 * backend.
 *
 * IT LEAVES ::lastErr ALONE, as save.hpp requires of a status poll: a menu
 * refreshing itself must not overwrite what a failed save left behind for the
 * game to report.
 */
u8 AccessibleDevices() {
    return (Mounted() && Detected()) ? static_cast<u8>(1u << kSdDevice) : 0;
}

/**
 * @brief Whether device @p index holds usable media, and where a save on it
 *        would live.
 *
 * EVERY INDEX BUT 0 IS save::no_device, which is how a caller walking indices
 * finds out there is one device without being told a count -- the loop stops
 * at the first ::no_device, and on this console that is index 1.
 *
 * save::too_large IS UNREACHABLE HERE, as on the Wii and the Switch: that answer
 * is about a card whose sector width the backend cannot transfer, and a
 * filesystem presents no such number. save::busy is unreachable too; the probe
 * is a devoptab walk and one service call, and both always have their answer.
 *
 * A WRITE-PROTECTED CARD STILL ANSWERS save::found, deliberately, even though
 * FSUSER_IsSdmcWritable could say otherwise. This call's vocabulary has no code
 * for "there, and not writable" -- see save.hpp's list of the four outcomes --
 * and the alternatives are both wrong: ::no_device would tell a player to insert
 * a card that is already in, and ::denied is not one of the answers a caller is
 * promised here. The write itself reports save::denied with EACCES, which is
 * where that fact belongs.
 */
Response GetDeviceStatus(const u8 index) {
    if (index >= kDeviceCount)      return { .done = true, .err = no_device, .path = nullptr };
    if (!Mounted() || !Detected())  return { .done = true, .err = no_device, .path = nullptr };
    return { .done = true, .err = found, .path = kSaveFile };
}

/**
 * @brief Which devices could take a save of @p size bytes.
 *
 * NOT A SWEEP, FOR THE SWITCH'S REASON. The GameCube spreads this over several
 * calls because CARD_GetFreeBlocks needs a mount and a mount is the slowest
 * thing it does; here the measurement is one IPC call to the fs service, there
 * is no ticked form of it to spread, and a partial answer would cost what a
 * whole one costs. So this measures and answers completely, and does NOT return
 * 0 on a first call -- which save.hpp allows for rather than requires ("zero
 * means nowhere, OR not yet"), so a settled answer immediately is strictly more
 * than a caller is promised.
 *
 * FSUSER_GetSdmcArchiveResource AND NOT statvfs, for the reason the Switch
 * reaches past its devoptab too: what libctru's statvfs fills f_frsize /
 * f_bsize / f_bavail with is an implementation detail of a function this file
 * would then be depending on the internals of, and the two devoptabs that have
 * carried "sdmc" over this library's life do not agree about it. The service
 * call is declared in libctru's own header and answers the question directly.
 *
 * IT ANSWERS IN CLUSTERS, WHICH IS THE ONE WRINKLE: freeClusters and
 * clusterSize are both u32, their product on a large card overflows 32 bits
 * (an exFAT card with 32KiB clusters passes 4GiB free at 131072 clusters), so
 * the multiply is widened to u64 before it happens rather than after. A
 * truncated product would report a nearly-empty 64GB card as full, which is the
 * failure this cast exists to prevent.
 *
 * AN EXISTING SAVE BIG ENOUGH FOR @p size COUNTS AS FREE even on a full card,
 * because overwriting a file consumes no new space -- save.hpp's rule, and the
 * ordinary case for a player whose card is packed with other homebrew.
 *
 * THE CACHE IS THE SAME ONE, invalidated by a completed save or create, by a
 * mount that failed, and by a different @p size. There is no "set of accessible
 * devices" for it to be invalidated by: with one device, a card that went away
 * is a ::Mount that fails on the next measurement anyway.
 */
u8 FreeDevices(const std::size_t size) {
    if (size == 0) return 0;
    if (freeValid && freeSize == size) return freeMask;
    if (!Mount()) return 0;

    FS_ArchiveResource res = { };
    if (R_FAILED(FSUSER_GetSdmcArchiveResource(&res))) return 0;

    const u64 avail = static_cast<u64>(res.freeClusters) * static_cast<u64>(res.clusterSize);

    const std::size_t want = RoundUp(size);

    int         e       = 0;
    const off_t already = LengthOf(kSaveFile, e);
    const u64   held    = already > 0 ? static_cast<u64>(already) : 0;
    const u64   need    = held >= want ? 0 : want - held;

    freeMask  = avail >= need ? static_cast<u8>(1u << kSdDevice) : 0;
    freeSize  = size;
    freeValid = true;
    return freeMask;
}

/**
 * @brief The platform's own code for the last thing that went wrong, or 0.
 *
 * AN errno, LIKE THE WII'S AND THE SWITCH'S AND UNLIKE THE GAMECUBE'S NEGATIVE
 * CARD_ERROR_*. Every failure a caller can provoke here comes back through the
 * archive devoptab, which has already translated the fs service's Result into
 * errno, so that is what this reports -- and a developer reads it with <cerrno>
 * rather than with libctru's R_* macros.
 *
 * WHAT IS NOT REPORTED IS A Result. The three calls in this file that return
 * one rather than setting errno (archiveMountSdmc, FSUSER_IsSdmcDetected,
 * FSUSER_GetSdmcArchiveResource) leave ENODEV or nothing behind instead of the
 * service's own 32-bit code, deliberately: a 3DS Result in an i32 is a large
 * negative number that reads as a libogc card error, and save.hpp already says
 * this value is not part of any contract. Mixing two unrelated numbering
 * schemes in one field would make the field useless for the one thing it is
 * for, which is telling a developer what happened. The portable code is still
 * exact -- a mount that fails is save::no_device, which is what it means.
 */
i32 LastNativeError() { return static_cast<i32>(lastNative); }

} // namespace save
