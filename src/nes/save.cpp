#include <pnes/save.hpp>

// stock NES does not support saving to persistent memory
namespace save {
    // save::no_device, not a bare failure: this board has no battery and no
    // device of any kind, which is a thing a game can say to a player. Every
    // call is `done` on its first tick -- there is nothing to wait for, and a
    // caller's `while (!response.done)` loop must terminate.
    __attribute__((weak)) Response Save(const u8*, const std::size_t) { return { .done = true, .err = no_device }; }
    __attribute__((weak)) Response Load(u8*, const std::size_t) { return { .done = true, .err = no_device }; }
    __attribute__((weak)) Response Load(u8*, const std::size_t, const Response*) { return { .done = true, .err = no_device }; }
    __attribute__((weak)) Response CreateSave(const u8*, const std::size_t, const Response*) { return { .done = true, .err = no_device }; }

    // A finished search with nowhere to have looked -- NOT `ok` with no path,
    // which would invite the caller to create a save that CreateSave above
    // then refuses. save::Response has no `path` on this target anyway
    // (save.hpp gates it out under TARGET_NES).
    __attribute__((weak)) Response GetSaveLocation() { return { .done = true, .err = no_device }; }
}
