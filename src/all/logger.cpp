// log() (include/platform-nes/logger.hpp) is entirely a macro, on every
// platform -- zero-footprint data declarations on TARGET_NES, an immediate
// std::cout write everywhere else. Nothing here needs a runtime definition.

#ifdef TARGET_NES

#include <intsh>
using namespace br0::intsh;

// Weak default for logger.hpp's silentHeapAmount -- see that header's own
// comment. Plain C++ linkage (no extern "C"), same reason as the header's own
// declaration -- see there.
//
// section(".pnes_log"): `used` alone stops the COMPILER's own dead-code
// elimination, but nothing here is ever read at runtime, which is exactly
// what makes a real link's whole-program --gc-sections pass drop it anyway --
// confirmed the hard way for log()'s own Entry/Arg data (see
// src/nes/mappers/debug-log.ld's header comment). Reusing .pnes_log gets the
// same KEEP()'d, unmapped-to-any-real-region treatment for free: it survives
// in the .elf for lua_log_builder to read back out, costs zero ROM bytes, and
// needs no linker-script changes of its own.
extern const u8 silentHeapAmount __attribute__((weak, used, section(".pnes_log"))) = 128;

#endif
