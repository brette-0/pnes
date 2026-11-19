#include "save.hpp"

#ifndef TARGET_NES
#include <cstring>
#endif

#include "pnes/logger.hpp"

CARTMEM SaveFile saveData;

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