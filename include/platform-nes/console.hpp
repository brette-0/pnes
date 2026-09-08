/**
 * @file console.hpp
 * @brief Debug-build-only: makes sure PC stdout output (log(), see
 *        logger.hpp) is actually visible, even when the game was launched
 *        by something that never attached a terminal (a file manager, a
 *        desktop launcher, etc.) -- see this project's design notes on why
 *        that happens on Linux (no OS-level GUI/console subsystem split the
 *        way Windows has; a "terminal" is just whatever's on the other end
 *        of fd 1, and most GUI launchers don't attach one at all).
 *
 * ::tech::EnsureConsoleOnce() is a no-op in a release build (NDEBUG
 * defined) and a no-op if stdout is already a terminal. Otherwise:
 *
 * - Windows: AllocConsole() -- a genuine OS API for exactly this, tied to
 *   the process's own lifetime. Simple and reliable.
 * - Linux/macOS: there is no equivalent API. The only way to get a visible
 *   window is to launch an actual terminal EMULATOR PROGRAM and connect it
 *   to a FIFO this process then redirects its own stdout/stderr into.
 *   Inherently more fragile than the Windows path: it needs a running GUI
 *   session (silently does nothing over SSH/headless/no session), and on
 *   Linux specifically there's no single guaranteed-installed terminal
 *   emulator, so a short list of common ones is tried in turn. Failure to
 *   find one is not an error -- it just means no console appears, same as
 *   before this ran.
 */
#pragma once

#if !defined(NDEBUG) && (defined(TARGET_LINUX) || defined(TARGET_MAC) || defined(TARGET_WINDOWS))

#include <cstdio>

// Every #include lives HERE, before any namespace opens -- never inside one.
// A header that reopens `namespace std { ... }` while lexically nested
// inside `namespace tech { ... }` doesn't reach the real ::std at all; it
// creates a separate, bogus `tech::std` containing whatever that header
// declared. Confirmed the hard way: an earlier version of this file
// #included <cstdlib>/<string>/etc. from inside `namespace tech`, and
// std::string/std::setvbuf inside it resolved to that broken tech::std
// instead of ::std, both in this project's own build and when compiled
// standalone against a plain libstdc++.
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <spawn.h>
extern char **environ;
#endif

namespace tech {

#if defined(_WIN32)

inline bool StdoutIsTerminal() {
    return _isatty(_fileno(stdout)) != 0;
}

inline void PlatformEnsureConsole() {
    if (!AllocConsole()) return; // e.g. a console is already attached
    // Plain freopen, not freopen_s: this needs to build under both MSVC and
    // MinGW, and freopen is the one form both actually have.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // "freopen may be unsafe" -- no user input involved
#endif
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

#else // POSIX: Linux/macOS

inline bool StdoutIsTerminal() {
    return isatty(STDOUT_FILENO) != 0;
}

/**
 * @brief Spawns a terminal emulator running `tail -f -n +1 <fifoPath>`.
 *
 * Tries a short list of common Linux terminal emulators in turn (the first
 * one posix_spawnp can actually find on PATH wins); on macOS, asks
 * Terminal.app via `open -a Terminal` instead, since there's no comparably
 * universal terminal-emulator binary to spawn directly there.
 *
 * @return true if a terminal was successfully spawned (a reader for
 *         fifoPath now exists or will shortly), false if none could be
 *         started -- not an error, just means no console will appear.
 */
inline bool SpawnTerminalReading(const std::string &fifoPath) {
    const std::string tailCmd = "tail -f -n +1 " + fifoPath;

#if defined(TARGET_MAC)
    // Terminal.app via Launch Services -- there's no single "the" terminal
    // binary on macOS the way xterm/etc. serve that role on Linux.
    pid_t pid = -1;
    const char *argv[] = { "open", "-a", "Terminal", "-n", "--args",
                            "sh", "-c", tailCmd.c_str(), nullptr };
    return posix_spawnp(&pid, "open", nullptr, nullptr,
                         const_cast<char *const *>(argv), environ) == 0;
#else
    // No single guaranteed-installed terminal emulator on Linux -- try a
    // handful of common ones. "-e" is the one flag most of them share for
    // "run this command inside me"; not universal (gnome-terminal's own CLI
    // has changed shape across versions), but covers the common case.
    static const char *kCandidates[] = {
        "x-terminal-emulator", // Debian/Ubuntu's configured default, if any
        "xterm", "konsole", "gnome-terminal", "xfce4-terminal",
        "alacritty", "kitty", nullptr
    };
    for (int i = 0; kCandidates[i]; ++i) {
        pid_t pid = -1;
        const char *argv[] = { kCandidates[i], "-e", "sh", "-c", tailCmd.c_str(), nullptr };
        if (posix_spawnp(&pid, kCandidates[i], nullptr, nullptr,
                          const_cast<char *const *>(argv), environ) == 0) {
            return true;
        }
    }
    return false;
#endif
}

inline void PlatformEnsureConsole() {
    char path[] = "/tmp/pnes_console_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return;
    close(fd);
    unlink(path);          // reclaim the name as a FIFO instead of a plain file
    if (mkfifo(path, 0600) != 0) return;

    if (!SpawnTerminalReading(path)) {
        unlink(path);
        return; // no terminal emulator found (or no GUI session) -- give up quietly
    }

    // O_RDWR, not O_WRONLY: opening a FIFO write-only blocks until a reader
    // attaches, which would stall game startup for however long the
    // terminal emulator takes to launch. Opening read-write never blocks
    // (POSIX-guaranteed), and the kernel buffers writes until the spawned
    // terminal's `tail -f` opens its own end a moment later -- no data lost.
    //
    // path is DELIBERATELY NOT unlinked here, or ever, by this process --
    // confirmed the hard way: unlinking right after OUR OWN open() succeeds
    // races the spawned terminal, which needs a moment to actually launch
    // and run `tail -f <path>`. If our unlink wins that race, tail's own
    // later open() gets ENOENT, so it exits immediately (silently -- no
    // error, kitty's window just closes), and nothing anyone ever reads
    // from our fd's writes again. Leaving the FIFO in place costs nothing
    // (a few bytes in /tmp, reclaimed on reboot like any other /tmp file);
    // it isn't worth chasing a "definitely already opened" signal to clean
    // up automatically.
    const int rw = open(path, O_RDWR);
    if (rw < 0) return;

    dup2(rw, STDOUT_FILENO);
    dup2(rw, STDERR_FILENO);
    if (rw > STDERR_FILENO) close(rw);

    // stdio decided stdout's buffering mode at startup based on the ORIGINAL
    // fd 1 (not a terminal), so it defaulted to fully-buffered -- output
    // would otherwise sit in that buffer indefinitely instead of showing up
    // promptly in the new console.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}

#endif // POSIX

/// Ensures stdout/stderr are visible somewhere: a no-op in release builds,
/// a no-op if stdout is already a terminal, otherwise creates one (see this
/// file's own header comment). Safe to call repeatedly -- only acts once.
inline void EnsureConsoleOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    if (StdoutIsTerminal()) return;
    PlatformEnsureConsole();
}

} // namespace tech

#else

namespace tech {
/// Release build, or a platform this doesn't apply to: fully inert.
inline void EnsureConsoleOnce() {}
} // namespace tech

#endif
