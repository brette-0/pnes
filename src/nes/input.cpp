#include <intsh>
using namespace br0::intsh;
#include <pnes/input.hpp>
#include <pnes/technology.hpp>

extern "C" {
__attribute__((used)) u8 pollScratch1;
__attribute__((used)) u8 pollScratch2;
}

AI void input::PollControllers(u8* port1, u8* port2) {
    IO_PORT1 = 1;
    IO_PORT1 = 0;

    __asm__ volatile (
        "ldx #8\n"
        "1:\n"
        "lda $4016\n"
        "lsr a\n"
        "ror pollScratch1\n"
        "lda $4017\n"
        "lsr a\n"
        "ror pollScratch2\n"
        "dex\n"
        "bne 1b\n"
        :
        :
        : "a", "x", "c", "memory"
    );

    *port1 = pollScratch1;
    *port2 = pollScratch2;
}