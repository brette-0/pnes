#pragma once

#include <pnes/save.hpp>

enum eOptions : u8 {
#ifdef TARGET_GC
    Continue,
    Retry,
    No,
    Yes,
    Erase,
    Card1,
    Card2
#endif
};

#ifdef TARGET_GC
constexpr auto saveFoundText = ::charmap_generic.encode("SAVE LOADED SUCCESSFULLY");
constexpr auto saveMissingText = ::charmap_generic.encode("YOU MAY CREATE A SAVE FILE");
constexpr auto saveNoDeviceText = ::charmap_generic.encode("NO MEMORY CARD INSERTED");
constexpr auto saveBadErrorText = ::charmap_generic.encode("BAD ERROR CODE");
constexpr auto saveCorruptText = ::charmap_generic.encode("MEMORY CARD CORRUPTED");
constexpr auto saveFullText = ::charmap_generic.encode("NOT ENOUGH FREE SPACE");
constexpr auto saveLargeText = ::charmap_generic.encode("SAVE FILE IS TOO LARGE FOR GAME");
constexpr auto saveCorruptText2 = ::charmap_generic.encode("CONFIRM ERASE FILES");
constexpr auto saveChooseCard = ::charmap_generic.encode("CHOOSE CARD");
constexpr auto saveUseCard = ::charmap_generic.encode("CONFIRM USE CARD");
#endif

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