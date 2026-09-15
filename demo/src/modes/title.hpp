#pragma once
#include <platform-nes/technology.hpp>
#include <platform-nes/extras/ui/text.hpp>

namespace title {
    void main();

    void nmi_handler();
    void irq_handler();

    void InitTitleScreen();

    enum titleOptions : u8 {
        NewGame = 0,
        Continue = 1,
        Options  = 2,
#if defined(TARGET_MACOS) || defined(TARGET_WINDOWS) || defined(TARGET_LINUX)
        Quit     = 3,
        End      = Quit
#else
        End      = Options
#endif
    };

    AI auto SelectorUpdate() -> void;
}
