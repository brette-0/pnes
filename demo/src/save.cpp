#include "save.hpp"

#ifndef TARGET_NES
#include <cstring>
#endif

#include "pnes/logger.hpp"
#ifndef TARGET_NES
#include "pnes/video.hpp"   // the ::SaveGame fallback pump
#endif

CARTMEM SaveFile saveData;

#ifndef TARGET_NES
// NOT in CARTMEM, deliberately: this describes THIS BOOT's load, not the save.
// Parked in the save file it would be written to the card and read back as
// "already loaded" by a later boot that had not loaded anything.
bool saveLoaded = false;
#endif

#ifndef TARGET_NES
save::Response SaveGame(void (* const pump)()) {
    // Before the bytes, not after: this is what makes the file readable next
    // boot (see this function's declaration, save.hpp).
    saveData.version = saveVersion;

    // NO DEVICE PRE-CHECK, DELIBERATELY. ::save::AccessibleDevices would be
    // the obvious guard and it is the wrong one: the weak backend every target
    // but the GameCube still links answers 0 forever (src/all/save.cpp), and
    // the NES battery path overrides ::save::Save without overriding that --
    // so a pre-check would refuse to save on exactly the platforms whose save
    // works. ::save::Save reports ::save::no_device itself when there is
    // nowhere to write, which is the same answer one frame later and true
    // everywhere.
    save::Response response { };
    while (response = save::Save(&saveData, sizeof(saveData)), !response.done) {
        if (pump) pump();
        else      video::WaitForPresent();
    }

    log("SaveGame: err=%d native=%d", response.err, save::LastNativeError());
    return response;
}
#endif

constexpr void UpdateTextForSave(const char* buff, void* target, const std::size_t sName) {
#ifdef TARGET_GC
    if (sName > 32 || !sName) {
#if !NDEBUG
        log("text change in saved memory request was either zero or above 32 bytes in size");
#endif
        return;
    }
#if !NDEBUG
    if (buff[sName - 1] != '\0') {
        log("text change in saved memory request was not null terminated");
        return;
    }
#endif

    memcpy(target, buff, sName);
#endif
}

constexpr void UpdateGameNameForSave(const char* buff, const std::size_t sName) {
#ifdef TARGET_GC
    UpdateTextForSave(buff, saveData.header.gameName, sName);
#endif
}

constexpr void UpdateGameDescriptionForSave(const char* buff, const std::size_t sName) {
#ifdef TARGET_GC
    UpdateTextForSave(buff, saveData.header.gameDesc, sName);
#endif
}