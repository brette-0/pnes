// log() (include/platform-nes/logger.hpp) is entirely a macro, on every
// platform -- zero-footprint data declarations on TARGET_NES, an immediate
// std::cout write everywhere else. Nothing here needs a runtime definition.
