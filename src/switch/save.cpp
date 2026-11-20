/**
 * @file save.cpp
 * @brief Save/load/create/locate for the Nintendo Switch, as a file on the SD
 *        card reached through libnx's fsdev devoptab.
 *
 * THE THIRD BACKEND, AND THE SHALLOWEST OF THE THREE, which is the whole
 * reason it was the next one written: the GameCube half of src/ogc/save.cpp is
 * a ticked CARD_* state machine, the Wii half of that same file is blocking
 * libfat with two devices and a hotplug story, and this is blocking stdio with
 * ONE device that the operating system has already mounted before main() runs.
 * Everything below is the Wii's shape with the parts the Switch does not have
 * removed -- no driver startup, no insertion poll, no mount ownership -- and
 * the three places it is not the Wii's shape say so where they are.
 *
 * WHY THE SD CARD AND NOT SAVEDATA. The Switch has a real per-title save
 * concept -- an FS savedata image, mounted with fsdevMountSaveData, committed
 * with fsFsCommit, listed by the system's own data manager and included in
 * cloud/archive backups -- and this backend deliberately does not use it. A
 * savedata image belongs to an INSTALLED APPLICATION: it is created from the
 * title's NACP save-data-size fields when the title is installed, and it is
 * addressed by that title's application id and a user account id. The demo
 * ships as an .nro run from hbmenu under the album applet's borrowed title, so
 * there is no application id of its own to name, no savedata image allocated
 * for one, and no account the save belongs to. The SD card is what a homebrew
 * .nro actually has, and sdmc:/switch/<game>/ is where every other piece of
 * homebrew keeps its data -- so that is where this one keeps a save.
 *
 * A BUILD THAT BECOMES AN INSTALLED TITLE WOULD CHANGE THREE THINGS and no
 * more: ::Mount would call fsdevMountSaveData instead of fsdevMountSdmc, the
 * path constants would lose their "sdmc:/switch/<dir>" prefix, and ::Transfer
 * and ::Create would need an fsdevCommitDevice after their fsync, because
 * fsFsCommit is what makes a savedata write durable and libnx does not call it
 * at unmount. Nothing else here is about which filesystem is underneath.
 *
 * NOTHING IS TICKED, FOR THE WII'S REASON. stdio is blocking and fsdev has no
 * "advance this transfer by one step" entry point to build a state machine out
 * of, so every call finishes the whole operation and reports `done` on its
 * first tick. A caller's
 *
 *     while (!response.done) { ...one frame... }
 *
 * loop therefore runs exactly once -- the same loop, with the same meaning, as
 * on the GameCube where it runs several times. save::busy is declared on this
 * target (save.hpp gates the vocabulary on SAVE_DEVICE) so that a game written
 * against the GameCube compiles here, and is never returned.
 *
 * THE COST OF A SAVE IS A FILESYSTEM ROUND TRIP TO THE fs SERVICE, which is
 * fast on this hardware and is still not free: it is IPC to a system process
 * and then eMMC/SD traffic, measured in milliseconds rather than microseconds.
 * A save on this console drops a frame or two, the same honest summary the Wii
 * half gives. The expensive calls are named where they happen (::Mount,
 * ::save::FreeDevices).
 *
 * THE SEARCH IS PATH-BASED, LIKE THE WII'S AND UNLIKE THE GAMECUBE'S. There is
 * no equivalent of CARD_FindFirst's gamecode filter on a FAT volume, so a save
 * is found by looking where it was put: the location is fixed by the build
 * (NX_SAVE_DIR / NX_SAVE_FILENAME below) and ::ParseDevice accepts ONLY the one
 * complete path this file publishes. A path with any other spelling is
 * save::bad_path rather than a file created where nothing will ever look for it
 * again.
 *
 * @p size IS A PREFIX LENGTH, exactly as it is on the other two consoles, and
 * ::kAllocUnit is what makes that mean the same thing here: a save is rounded
 * up to a whole unit, a prefix shorter than the file reads back, and a prefix
 * longer than the file is save::full. See ::kAllocUnit for why a filesystem
 * with gigabytes to spare reproduces a memory card's rule.
 *
 * NO LOGGING, for the reason both halves of the OGC backend give: log()
 * resolves on this platform to the generic debug branch of logger.hpp, which
 * writes through std::cout, and this backend has no business owning the
 * console that writes to. What actually failed lands in ::lastNative instead --
 * see ::save::LastNativeError for the one wrinkle that is this platform's own.
 */
#include "internal.hpp"
#include <pnes/save.hpp>

#include <switch.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/**
 * @brief The `path` a caller's save::Response names, or nullptr for none.
 *
 * THE SAME FUNCTION src/ogc/save.cpp SHARES BETWEEN ITS TWO HALVES, repeated
 * here rather than hoisted into a header, because hoisting it would be the
 * first shared SOURCE between two platform backends in this library: there is
 * no src/all/save_common.hpp and one function is not enough to start one with.
 * What matters is that it behaves identically, which is why its reasoning is
 * repeated too --
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
 * TWO NAMES AND NOT ONE PATH, matching WII_SAVE_DIR / WII_SAVE_FILENAME: the
 * directory has to be created before the file can be, and a mkdir needs it on
 * its own. Compile definitions rather than a CMake-generated header, for the
 * reason the other backends' identity knobs are -- a project sets them once in
 * its own build and nothing here grows an option for it.
 *
 * THE DIRECTORY IS NOT OPTIONAL AND NEITHER IS ITS PARENT. "sdmc:/switch" is
 * where hbmenu lists .nro files from and where homebrew keeps its per-app data
 * by convention, so a save belongs at sdmc:/switch/<game>/<file> and not in the
 * volume root -- an SD card's root is the player's, and a bare save file
 * dropped in it is indistinguishable from litter. ::Create makes both levels.
 */
#ifndef NX_SAVE_DIR
#define NX_SAVE_DIR "pnes"
#endif
#ifndef NX_SAVE_FILENAME
#define NX_SAVE_FILENAME "pnes_save"
#endif

/**
 * @brief Rejects a name that would be a path rather than a name.
 *
 * CHECKED AT COMPILE TIME because the alternative is discovering it on a
 * player's console: the path below is assembled by the preprocessor, so an
 * NX_SAVE_DIR containing a slash, a colon or a backslash silently builds a path
 * pointing somewhere else entirely -- possibly at another homebrew app's
 * directory under sdmc:/switch. A name is one component, and the separators are
 * this file's to add.
 */
constexpr bool IsPlainName(const char* s) {
    if (!*s) return false;
    for (; *s; ++s) {
        if (*s == '/' || *s == '\\' || *s == ':') return false;
    }
    return true;
}
static_assert(IsPlainName(NX_SAVE_DIR),
    "NX_SAVE_DIR must be a single non-empty name with no '/', '\\' or ':'");
static_assert(IsPlainName(NX_SAVE_FILENAME),
    "NX_SAVE_FILENAME must be a single non-empty name with no '/', '\\' or ':'");
// 255 is FAT's long-name limit per component, which is what the Switch's SD
// card is formatted as (FAT32 or exFAT); the whole path is well under
// FS_MAX_PATH either way.
static_assert(sizeof(NX_SAVE_DIR) <= 256, "NX_SAVE_DIR must be at most 255 characters");
static_assert(sizeof(NX_SAVE_FILENAME) <= 256, "NX_SAVE_FILENAME must be at most 255 characters");

/**
 * @brief The devoptab name libnx mounts the SD card under, without the colon.
 *
 * WITHOUT THE COLON because that is the form the fsdev entry points take
 * (fsdevGetDeviceFileSystem, fsdevMountSdmc's own registration), while every
 * path below carries "sdmc:" because that is the form stdio takes. The two
 * spellings are the same device named for two different APIs.
 */
constexpr char kDevice[]  = "sdmc";
/** @brief hbmenu's own directory, which ::Create makes if a card lacks it. */
constexpr char kHomebrew[] = "sdmc:/switch";
constexpr char kSaveDir[]  = "sdmc:/switch/" NX_SAVE_DIR;
constexpr char kSaveFile[] = "sdmc:/switch/" NX_SAVE_DIR "/" NX_SAVE_FILENAME;

/**
 * @brief How far ::save::AccessibleDevices and friends count.
 *
 * ONE, AND THAT IS THE BIGGEST DIFFERENCE BETWEEN THIS BACKEND AND THE OTHER
 * TWO. The GameCube has two card slots and the Wii an SD slot and a USB port,
 * so both of those backends carry a device table, a preference order, a
 * remembered location and a best-answer-so-far search across devices. The
 * Switch has one SD card: USB mass storage is not reachable from an
 * unprivileged .nro (it belongs to the system's own fs process), and the
 * internal eMMC's user partitions are not somewhere a game may write.
 *
 * SO THE SEARCHES COLLAPSE, and most of this file is shorter than the Wii's
 * for that one reason: there is no Rank() here because no two devices ever
 * disagree, no remembered device index because index 0 is the only answer, and
 * ::save::FreeDevices has one candidate to measure. What does NOT collapse is
 * the device VOCABULARY -- bit 0, index 0, and a path per device -- because
 * that is save.hpp's shape and a portable caller walks it the same way on every
 * console. A slot picker drawn against this backend finds exactly one row.
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
 * reason, as the Wii half's ::kAllocUnit. It is what makes @p size mean the same
 * thing on all three consoles: a save file is a whole number of units long, a
 * shorter prefix reads back, and a prefix longer than the file is save::full.
 * Nothing about it is hardware here; the SD card's own cluster size is the
 * filesystem's business and this file never sees it.
 *
 * IT IS NOT A LIMIT. A larger save simply occupies more units, so a @p size
 * above 8192 is ordinary rather than save::too_large -- a game sized for the
 * GameCube is a strict subset of what works here, which is the point.
 */
constexpr std::size_t kAllocUnit = 8192;

/**
 * @brief The zero-fill source, in .rodata rather than as an 8 KiB staging copy.
 *
 * The Wii half's reasoning exactly: a write() takes a length, so the caller's
 * own bytes go out from where they already are and the padding is this block,
 * written as many times as it takes (::PadTo). 256 bytes of .nro, and no
 * snapshot of the caller's save struct.
 */
const u8 kZeros[256] = { };

/**
 * @brief When a failed mount was last attempted, and whether one ever was.
 *
 * A THROTTLE FOR A CALL THAT ALMOST NEVER RUNS. libnx's default __appInit
 * mounts the SD card before main() -- fsdevMountSdmc() in its startup path --
 * so ::Mount's first question is answered by ::Mounted and no mount is
 * attempted at all in the ordinary case. The retry exists for the cases where
 * that mount did not happen or did not survive: an application with its own
 * __appInit that left fsdev out, or a card the system has since taken away.
 *
 * THE COST IT GUARDS AGAINST IS A SERVICE ROUND TRIP, not a bus enumeration
 * like the Wii's USB startup(), so it is a smaller problem throttled for the
 * same reason: save.hpp invites a UI to poll ::save::AccessibleDevices "a few
 * times a second", and a failing mount re-attempted on every poll is IPC a
 * menu does not need to pay for. Hotplug still works to the extent the system
 * allows it -- the next attempt after the interval finds the card.
 */
u64  mountAt    = 0;
bool mountTried = false;
constexpr u64 kMountRetryNs = 2000ull * 1000ull * 1000ull;  // 2s

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
 * THE WII HALF'S ::MapErrno, AND IT HAS TO BE, because what reaches here is
 * newlib's errno either way: fsdev translates the fs service's own Result codes
 * into errno inside the devoptab (a missing file is ENOENT, a full card
 * ENOSPC), so the mapping a caller sees is the same one the Wii's libfat
 * produced. A game that handles save::full on one console handles it here.
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
        // The card went away mid-operation, which on this console is the system
        // taking it rather than the player pulling it out from under a running
        // game -- the same outcome either way.
        //
        // ENOMEDIUM IS BEHIND A GUARD because newlib declares it only under
        // __LINUX_ERRNO_EXTENSIONS__, which devkitA64 does not define, so
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

/** @brief True when the SD card is mounted under ::kDevice right now. */
bool Mounted() {
    return fsdevGetDeviceFileSystem(kDevice) != nullptr;
}

/**
 * @brief Makes the SD card usable, mounting it if nothing else has.
 *
 * ALREADY MOUNTED IS THE NORMAL CASE, AND THE MOUNT IS NOT OURS. libnx's
 * default startup owns "sdmc" before main() runs and the application's own
 * assets may be open on it, so there is nothing here to release and no
 * `mountedByUs` flag of the Wii's kind: unmounting a volume this backend did
 * not mount would pull the SD card out from under whatever else is reading it,
 * and unmounting one it DID mount would do the same to the next save. The
 * device stays up for the life of the process either way.
 *
 * THE SLOW CALL IN THIS FILE, in the case where it does run: a mount is IPC to
 * the fs service plus a filesystem open. Nothing calls it on a status poll --
 * ::save::AccessibleDevices and ::save::GetDeviceStatus stop at ::Mounted, so
 * a menu polling them pays one pointer comparison -- and it runs when a real
 * operation needs the filesystem, or when ::save::FreeDevices has to ask how
 * much room is left.
 *
 * fsdevGetDeviceFileSystem IS THE TEST, NOT A stat() OF THE ROOT, which is
 * where this differs from the Wii's ::Mounted for a reason worth keeping: the
 * Wii has to stat because libfat has no "is this devoptab name taken" question,
 * and a stat of a mounted-but-unreadable volume can fail. Asking fsdev
 * directly is one lookup in its own device table, cannot touch the medium, and
 * answers the question actually being asked.
 */
bool Mount() {
    if (Mounted()) return true;

    const u64 now = armTicksToNs(armGetSystemTick());
    if (mountTried && now - mountAt < kMountRetryNs) return false;
    mountTried = true;
    mountAt    = now;

    if (R_FAILED(fsdevMountSdmc())) {
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
 * a whole sector and the Wii half from this same loop.
 *
 * NOT ftruncate(), WHICH fsdev DOES IMPLEMENT. Extending a file with a
 * truncate would zero the extension and cost one call instead of a few writes,
 * and it would stop the padding from OVERWRITING anything: a save that shrank
 * would leave the previous save's bytes sitting in the unit past it, since the
 * file is already long enough to need no extension at all. Writing the zeros is
 * what makes the unit's whole content this save's.
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
 * ONE FUNCTION FOR BOTH DIRECTIONS, like the Wii's ::Transfer, because
 * everything except the transfer itself is shared and the length check in
 * particular MUST be: a save and a load disagreeing about whether a file is
 * long enough is how a game writes a prefix it can never read back.
 *
 * THE LENGTH CHECK IS THE GAMECUBE'S, DOWN TO THE CODE IT REPORTS. A prefix
 * longer than the file on the medium is save::full with ENOSPC in
 * ::lastNative -- an errno the filesystem did not produce, which is the point:
 * a developer reading it alongside save::full learns that the FILE was too
 * short rather than the card too full. A game whose save struct has outgrown
 * its existing save file hears the same answer on all three consoles.
 *
 * fsync BEFORE close AND NOT INSTEAD OF IT. A save is the one moment a player
 * may put the console to sleep or hold the power button believing their
 * progress is on the card; close() flushes the file, fsync() (fsFileFlush
 * underneath) is what pushes it through the fs service's own buffering, and its
 * failure is a failed save rather than a detail -- which is why it is part of
 * the && and not a courtesy call. An INSTALLED title would also need
 * fsdevCommitDevice here; see this file's header comment on savedata.
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
 * NO SEARCH, BECAUSE THERE IS NOWHERE TO SEARCH. The Wii's counterpart tries
 * the remembered device, then device 0, then device 1, and keeps the best
 * answer any of them gave; with one device the remembered location IS the only
 * location and the best answer is the only answer. What survives of that flow
 * is the VALIDATION -- a named path is checked rather than defaulted, because
 * defaulting a wrong path is how a save lands somewhere it will not be found
 * again.
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
 * TWO DIRECTORIES, NOT ONE, and that is this backend's own wrinkle: the save
 * lives at sdmc:/switch/<dir>/<file>, and while "switch" exists on any card
 * that has ever run homebrew, a card freshly formatted for a game that was
 * side-loaded some other way may not have it -- and mkdir does not create
 * parents. An EEXIST from either level is the ordinary case and not an error.
 *
 * A HALF-WRITTEN NEW SAVE IS DELETED. If the card fills up or is taken away
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
 * absent. Exactly the other two backends' two answers, from the same
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
 * shape, and the Switch's own data manager does not list homebrew saves at all
 * (they are not savedata images; see this file's header comment), so there is
 * nothing here to read and nothing to state.
 *
 * save::no_device AND NOT save::failed, matching the Wii half: the honest
 * content of the answer is that the DEVICE this concept belongs to is not
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
 * ONE BIT, AND IT IS A POINTER COMPARISON. Asking fsdev whether "sdmc" is
 * mounted touches neither the card nor the fs service (::Mounted), so this is
 * the cheapest call in the file and a UI may poll it as often as it likes --
 * cheaper than the Wii's two driver questions and far cheaper than the
 * GameCube's bus transaction.
 *
 * A MOUNT IS NOT ATTEMPTED FROM HERE, which is what keeps it that cheap: a
 * card the system has taken away reads as inaccessible until something that
 * really needs the filesystem retries the mount (::Mount, on a save, a load, a
 * create, a locate or a free-space measurement). The one thing this cannot say
 * is that the card is UNREADABLE as opposed to absent -- being broken is only
 * discoverable by mounting, which save.hpp's own documentation says of every
 * backend.
 *
 * IT LEAVES ::lastErr ALONE, as save.hpp requires of a status poll: a menu
 * refreshing itself must not overwrite what a failed save left behind for the
 * game to report.
 */
u8 AccessibleDevices() {
    return Mounted() ? static_cast<u8>(1u << kSdDevice) : 0;
}

/**
 * @brief Whether device @p index holds usable media, and where a save on it
 *        would live.
 *
 * EVERY INDEX BUT 0 IS save::no_device, which is how a caller walking indices
 * finds out there is one device without being told a count -- the loop stops
 * at the first ::no_device, and on this console that is index 1.
 *
 * save::too_large IS UNREACHABLE HERE, as on the Wii: that answer is about a
 * card whose sector width the backend cannot transfer, and a filesystem
 * presents no such number. save::busy is unreachable too; the probe is a
 * pointer comparison and always has its answer.
 */
Response GetDeviceStatus(const u8 index) {
    if (index >= kDeviceCount) return { .done = true, .err = no_device, .path = nullptr };
    if (!Mounted())            return { .done = true, .err = no_device, .path = nullptr };
    return { .done = true, .err = found, .path = kSaveFile };
}

/**
 * @brief Which devices could take a save of @p size bytes.
 *
 * NOT A SWEEP, FOR THE WII'S REASON. The GameCube spreads this over several
 * calls because CARD_GetFreeBlocks needs a mount and a mount is the slowest
 * thing it does; here the measurement is one IPC call to the fs service, there
 * is no ticked form of it to spread, and a partial answer would cost what a
 * whole one costs. So this measures and answers completely, and does NOT return
 * 0 on a first call -- which save.hpp allows for rather than requires ("zero
 * means nowhere, OR not yet"), so a settled answer immediately is strictly more
 * than a caller is promised.
 *
 * fsFsGetFreeSpace AND NOT statvfs, which is the one place this file reaches
 * past the devoptab into libnx's service layer. fsdev does implement statvfs,
 * and what it fills f_frsize / f_bsize / f_bavail with is an implementation
 * detail of a function this file would be depending on the internals of; the
 * service call it is built out of is declared in libnx's own header, answers in
 * BYTES with no unit to multiply by, and cannot be misread. The Wii half uses
 * statvfs because libfat gives it nothing better.
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

    FsFileSystem* const fs = fsdevGetDeviceFileSystem(kDevice);
    if (!fs) return 0;

    s64 avail = 0;
    if (R_FAILED(fsFsGetFreeSpace(fs, "/", &avail)) || avail < 0) return 0;

    const std::size_t want = RoundUp(size);

    int         e       = 0;
    const off_t already = LengthOf(kSaveFile, e);
    const u64   held    = already > 0 ? static_cast<u64>(already) : 0;
    const u64   need    = held >= want ? 0 : want - held;

    freeMask  = static_cast<u64>(avail) >= need ? static_cast<u8>(1u << kSdDevice) : 0;
    freeSize  = size;
    freeValid = true;
    return freeMask;
}

/**
 * @brief The platform's own code for the last thing that went wrong, or 0.
 *
 * AN errno, LIKE THE WII'S AND UNLIKE THE GAMECUBE'S NEGATIVE CARD_ERROR_*.
 * Every failure a caller can provoke here comes back through the devoptab,
 * which has already translated the fs service's Result into errno, so that is
 * what this reports -- and a developer reads it with <cerrno> rather than with
 * libnx's Result macros.
 *
 * WHAT IS NOT REPORTED IS A Result. The two calls in this file that return one
 * rather than setting errno (fsdevMountSdmc, fsFsGetFreeSpace) leave ENODEV or
 * nothing behind instead of the service's own 32-bit code, deliberately: a
 * Result in an i32 is a large negative number that reads as a libogc card
 * error, and save.hpp already says this value is not part of any contract.
 * Mixing two unrelated numbering schemes in one field would make the field
 * useless for the one thing it is for, which is telling a developer what
 * happened. The portable code is still exact -- a mount that fails is
 * save::no_device, which is what it means.
 */
i32 LastNativeError() { return static_cast<i32>(lastNative); }

} // namespace save
