#pragma once

#include <pnes/save.hpp>

struct SaveHeader {
#ifdef TARGET_GC
    OGC_COMMENT(gameName);  // name
    OGC_COMMENT(gameDesc);  // desc
    OGC_BANNER_CI(banner);  // banner in rgba
    OGC_ICON_CI(icon);      // icon frame 1 in ci
#endif
};

struct SaveFile {
    u16 version;
    SaveHeader header;
};

extern SaveFile saveData;

constexpr void UpdateTextForSave(const char* buff, void* target, std::size_t sName);
constexpr void UpdateGameNameForSave(const char* buff, std::size_t sName);
constexpr void UpdateGameDescriptionForSave(const char* buff,  std::size_t sName);