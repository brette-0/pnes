#pragma once


#ifdef TARGET_GC
constexpr auto retryText        = ::charmap_generic.encode("RETRY");
constexpr auto continueText     = ::charmap_generic.encode("CONTINUE");
constexpr auto yesText          = ::charmap_generic.encode("YES");
constexpr auto noText           = ::charmap_generic.encode("NO");
constexpr auto eraseText        = ::charmap_generic.encode("ERASE ALL");

// user rtTextBoxes to draw this text, its GC so its compute is OK
#endif

void TryFindSaveAndLoadOrCreate();