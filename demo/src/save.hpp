#pragma once

#include <pnes/save.hpp>

#include "banks.hpp"

// Bumped from 0 when SaveFile gained `config` below: a save written before it
// has a different layout, and ::pregame's version check is what stops it being
// read back as though it didn't.
constexpr u16 saveVersion = 1;

// ::SAVE_DEVICE AND NOT TARGET_OGC, WHICH IS WHAT MAKES THE SAVE FLOW THE
// SWITCH'S TOO. None of these is memory-card-shaped: they are the words a wheel
// of choices is drawn with and the names of the device slots every
// ::save::AccessibleDevices mask has. The Wii's devices are the front SD slot
// and USB mass storage where the GameCube's are card slots A and B, and the
// Switch has one SD card -- which changes the TEXT below and nothing about the
// flow, so the gate is the capability (pnes/save.hpp) and the wording is what
// varies inside it.
//
// ::Card1 / ::Card2 KEEP THEIR NAMES on every console, deliberately. They are
// the labels the choice wheel returns, indexed the way save.hpp's device
// indices are (0 then 1), and renaming them to ::Device1 / ::Device2 would
// touch every arm of pregame's wheels to say the same thing. What they MEAN
// per console is in ::card1Text / ::card2Text (demo/src/modes/pregame.hpp).
//
// ::Card2 IS DECLARED ON A ONE-DEVICE CONSOLE AND NEVER RETURNED THERE, which
// is the same trade save.hpp makes with ::save::busy: the wheel it labels is
// only ever drawn when TWO devices are accessible at once (::TryCreateSave's
// `saveDevices == 0b11`), and on the Switch that mask cannot happen. The arm
// still has to COMPILE, and a dead one costs nothing.
enum eOptions : u8 {
#ifdef SAVE_DEVICE
    Continue,
    Retry,
    No,
    Yes,
    Erase,
    Card1,
    Card2
#endif
};

#ifdef SAVE_DEVICE
constexpr auto saveMissingText      = ::charmap_generic.encode("YOU MAY CREATE A SAVE FILE");
#ifdef TARGET_GC
constexpr auto saveNoDeviceText    = ::charmap_generic.encode("NO MEMORY CARD INSERTED");
constexpr auto saveChooseCard       = ::charmap_generic.encode("CHOOSE CARD");
constexpr auto saveUseCard          = ::charmap_generic.encode("CONFIRM USE CARD");
#elif defined(TARGET_NX)
// THE SWITCH'S THREE, AND ONLY THE FIRST IS EVER DRAWN. There is one SD card
// and no second device to choose between (src/switch/save.cpp's
// ::kDeviceCount), and the save goes to the game's own directory on the card
// the game booted from -- so this console asks the player NOTHING about where a
// save goes or whether to make one (::SAVE_DEVICE_CHOICE, pnes/technology.hpp).
//
// ::saveNoDeviceText IS REAL HERE: a failure is reported on every platform, and
// an SD card the system has taken away is the one thing that can stop a save on
// this one.
//
// THE OTHER TWO ARE THE WORDING A ONE-DEVICE CONSOLE WOULD USE IF IT EVER
// ASKED, kept rather than deleted so that the strings and the flow stay in step
// if a project chooses to ask -- which it does by defining
// ::SAVE_DEVICE_CHOICE. ::saveChooseCard is still REFERENCED (the two-device
// wheel compiles on every ::SAVE_DEVICE target, it just cannot be reached with
// one bit in the mask); ::saveUseCard is referenced only inside the gate.
constexpr auto saveNoDeviceText    = ::charmap_generic.encode("NO SD CARD FOUND");
constexpr auto saveChooseCard       = ::charmap_generic.encode("CHOOSE DEVICE");
constexpr auto saveUseCard          = ::charmap_generic.encode("CONFIRM USE SD CARD");
#else
// THE WII'S THREE, AND ONLY THE WORDING DIFFERS. "MEMORY CARD" is a thing this
// console does not have: its devices are the front SD slot and a USB disk, so
// a prompt naming a memory card would be asking a player to insert something
// the Wii has no socket for. Kept inside the same 32-tile budget the GameCube's
// 4:3 context band sets (see the write-back trio below), which is why it is
// "NO SD CARD OR USB DRIVE" rather than a sentence.
constexpr auto saveNoDeviceText    = ::charmap_generic.encode("NO SD CARD OR USB DRIVE");
constexpr auto saveChooseCard       = ::charmap_generic.encode("CHOOSE DEVICE");
constexpr auto saveUseCard          = ::charmap_generic.encode("CONFIRM USE DEVICE");
#endif
constexpr auto saveBadErrorText     = ::charmap_generic.encode("BAD ERROR CODE");
constexpr auto saveCorruptText     = ::charmap_generic.encode("SAVE FILE CORRUPTED CONFIRM ERASE SAVE");
constexpr auto saveFullText        = ::charmap_generic.encode("NOT ENOUGH FREE SPACE");
constexpr auto saveLargeText       = ::charmap_generic.encode("SAVE FILE IS TOO LARGE FOR GAME");
// The write-back flow's own three, which ::pregame::SaveScreen shows in the
// same context band pregame's messages use. Short enough for one row of the
// narrowest band this scene has (the 4:3 GameCube's 40 - 8 = 32 tiles), so
// none of them depends on the wrapper to be readable.
constexpr auto saveSavingText       = ::charmap_generic.encode("SAVING");
constexpr auto saveWriteFailedText  = ::charmap_generic.encode("COULD NOT SAVE GAME");
constexpr auto saveNoFileText       = ::charmap_generic.encode("NO SAVE FILE TO WRITE TO");
#endif

struct SaveHeader {
#ifdef TARGET_GC
    OGC_COMMENT(gameName);  // name
    OGC_COMMENT(gameDesc);  // desc
    OGC_BANNER_CI(banner);  // banner in rgba
    OGC_ICON_CI(icon);      // icon frame 1 in ci
#endif
};

// The options scene's own settings, generated by uitk and included here as a
// member of the save file rather than declared in the scene's header -- which
// is the whole reason demo/ui/options.uis has Generate BSS Separately set.
// Every button, slider and choice on that page is a field of this struct, so
// changing a setting in the UI writes straight into save data and loading a
// save restores the page already set that way: no copy-in/copy-out step, and
// nothing for hand-written code to keep in sync.
//
// bss.hpp is deliberately guard-less and namespace-less (see tools/uitk's
// BssSplit) because it is pasted into the middle of this struct. The scene's
// generated header reaches each field back through "saveData.config.", which
// is the Symbol Prefix set on the scene, and the prefixes behind the fields'
// names are the node paths that produced them
// (options_videoCanvas_reduceFlashesButton).
//
// Per target, like every other generated file: a scene hides different
// controls on different targets (options.uis has no widescreen button on NES),
// so this struct's own layout is that target's set of settings and nothing
// more.
#ifndef GEN_TARGET_DIR
#error "GEN_TARGET_DIR is not set -- add it to this target's CMakeLists.txt branch."
#endif

// ANY MEMBER ADDED HERE THAT THE NES SIDE READS MUST BE `atomic`. This struct
// lives at a pinned address in battery-backed SRAM (::SAVEMEM) that nothing
// initialises, but C++ still calls it
// zero-initialised and LLVM acts on that: at LTO, a member with no store before
// its first read is folded to zero and its storage dropped. The uitk fields in
// `config` are already `atomic` (tools/uitk's BssSplit::decl), and `version` is
// safe only because nothing on NES ever reads it.
struct SaveFile {
    u16 version;
    SaveHeader header;

    struct Config {
#include STRCAT(../gen/options/GEN_TARGET_DIR/bss.hpp)
    } config;
};


extern SAVEMEM SaveFile saveData;

// WIDENED WITH THE FLOW: a path is not a memory-card concept. On the GameCube
// it is "a:super_mary_sisters", on the Wii "sd:/super_mary_sisters/save.dat"
// and on the Switch "sdmc:/switch/super_mary_sisters/save.dat", and
// ::pregame::main fills it from whatever ::save::GetSaveLocation answered in
// every case.
#ifdef SAVE_DEVICE
extern char* savePath;
#endif

#ifndef TARGET_NES
/**
 * @brief Writes ::saveData back to wherever it already lives.
 *
 * THE WHOLE OF SAVING, from a caller's point of view: it stamps the version,
 * runs ::save::Save to completion and hands back the ::save::Response it
 * finished with. What that response MEANS on screen is not this function's
 * business -- ::pregame::SaveScreen is what turns an error into a message and
 * a retry/continue wheel -- so this neither draws nor decides anything.
 *
 * @p pump IS CALLED ONCE PER WAITED FRAME, and it is the reason this is a
 * function at all rather than a line at the call site. Every save call in this
 * library is TICKED: it reports `done` when the operation is over, and until
 * then the game is expected to keep presenting frames (pnes/save.hpp). So the
 * loop needs the caller's own "one frame of my mode" -- ::pregame::WaitBody,
 * which drives audio, spends the sound-effect mailbox and waits for present --
 * and a save that blocked without it would freeze the screen it is drawn on
 * and silence the music. A null @p pump falls back to a bare
 * ::video::WaitForPresent so a caller that has nothing of its own to tick
 * still cannot hang.
 *
 * THE VERSION IS STAMPED HERE, not left to the caller: ::saveVersion describes
 * the layout of the struct this call is about to write, so the only moment it
 * can be known to be right is the moment the bytes go out. ::pregame's
 * ValidateSave is what reads it back, and a save written without it comes back
 * as version 0 and is judged corrupt on the next boot.
 *
 * NOT COMPILED ON NES, where nothing calls it: the options page it is reached
 * from is not shipped there (see ::title::main's own B handling), and a battery
 * cart's RAM is the save -- ::saveData is already in it, so there is no
 * transfer for this to run. Ungating is this one guard, since every call below
 * is declared on every target.
 */
save::Response SaveGame(void (*pump)());
#endif

constexpr void UpdateTextForSave(const char* buff, void* target, std::size_t sName);
constexpr void UpdateGameNameForSave(const char* buff, std::size_t sName);
constexpr void UpdateGameDescriptionForSave(const char* buff,  std::size_t sName);