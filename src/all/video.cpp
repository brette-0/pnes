#include <pnes/video.hpp>

// The one definition of the measured-refresh flag, in src/all because every
// target compiles this directory and every target needs the symbol -- while
// video::DetectRefresh(), which writes it, is necessarily per-backend (it is
// the one part of this that has to talk to real hardware).
//
// Lives in RAM (.bss), so it carries no bank/placement concerns on NES even
// though this TU is not pinned to the fixed bank: only DetectRefresh's CODE
// needs pinning, and that lives in src/nes/video.cpp.
//
// Initialised false -- i.e. 60Hz -- so a backend whose DetectRefresh is a
// no-op, and the window before detection runs, both read as the common case
// rather than as an undefined one.
namespace video {
bool is50Hz = false;
}

/**
 * @brief Weak default: answers 60Hz without measuring.
 *
 * FOR THE TARGETS WHOSE TICK RATE IS NOT A VARIABLE. What matters is the rate
 * the GAME TICKS AT, not what any panel is capable of, because one frame of
 * logic runs per present (video::WaitForPresent):
 *  - DS/DSi, GBA, 3DS, Switch, PSP: ~60Hz with no way for the player to change
 *    it, and each paced by its own hardware vblank -- swiWaitForVBlank,
 *    C3D_FrameEnd, sceDisplayWaitVblankStart, framebufferEnd. Nothing to
 *    detect.
 *  - PS2: the display mode is fixed at NTSC 640x448 with no PAL branch, and
 *    the present is vsync-paced -- so 60Hz by construction (src/ps2/video.cpp's
 *    own header says so).
 *  - DESKTOP (SDL3): ticks on SDL_AddTimer(16, ...) -- a 16ms software timer,
 *    not the monitor's vsync (src/SDL3/video.cpp's irq::init). The monitor's
 *    refresh is therefore the WRONG thing to read here: a 50Hz or 144Hz panel
 *    does not change the tick rate at all. It is also not exactly 60Hz -- 16ms
 *    is 62.5Hz nominal -- which this flag has no way to say and nothing
 *    currently needs it to. If that pacing ever moves to real vsync, this is
 *    the target that then needs a strong definition.
 *
 * Weak, so a backend whose tick rate genuinely varies overrides it with its own
 * strong definition and this one is dropped at link time -- the same
 * weak/strong arrangement ppu::Flush and the save stubs already use. All four
 * that do, and how each gets its answer:
 *   - src/nes/video.cpp   NTSC/PAL/Dendy, counted off PPUSTATUS
 *   - src/ogc/video.cpp   PAL vs PAL60/EURGB60, timed off the PPC timebase
 *   - src/wiiu/video.cpp  the TV mode, timed off OSGetSystemTime
 *   - src/ps3/video.cpp   READ, not timed -- lv2 publishes the rate in
 *                         videoState::displayMode.refreshRates
 *
 * NONE OF THEM CHANGE THE DISPLAY MODE to avoid the question, though two could.
 * Each of those files says why not; the short version is that scaling the game
 * is a smaller imposition than overriding what the player configured.
 */
__attribute__((weak)) bool video::DetectRefresh() { return false; }
