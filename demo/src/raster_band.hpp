#pragma once

#include <pnes/technology.hpp>   // AI
#include <pnes/video.hpp>        // ppu::SetColorPriority
#include <intsh>

using namespace br0::intsh;

// ---------------------------------------------------------------------------
// Raster timing bands -- a profiling aid, OFF by default.
//
// Writing the PPUMASK tint bits mid-frame paints the scanlines a section of
// code happens to be executing over, so the screen becomes a live bar chart of
// where the frame goes: green for UpdateActors, blue for ColMapTrack, red for
// audio::Update, and a per-column ramp inside ColMapTrack's slide loops.
//
// ON by default, because profiling a release build is the normal state of this
// project and the bands are the primary readout for it. Define DEMO_NO_RASTER_BANDS
// (or pass -DDEMO_NO_RASTER_BANDS) to turn them off for a build you are actually
// shipping -- that is the only reason to.
//
// Deliberately NOT gated on NDEBUG: you profile a RELEASE build, because a debug
// build's timing is not the timing you are trying to measure. Tying the bands to
// the optimisation level would remove them exactly when they are useful.
//
// Cost when enabled is about 15 cycles a call (read the PPUMASK shadow, mask,
// write back -- src/nes/video.cpp), so roughly 60 cyc/frame for the four
// fixed bands. Negligible either way; the reason to gate them is that a
// released game should not be tinted.
//
// NES only. ::ppu::SetColorPriority exists on the emu backends too, but the
// bands are a raster-timing trick with nothing to show on a backend that
// presents whole frames.
// ---------------------------------------------------------------------------

namespace demo {

#if !defined(DEMO_NO_RASTER_BANDS) && defined(TARGET_NES)
    // One band. Takes the PPUMASK tint bits directly (RED/GREEN/BLUE), so the
    // call sites read as the colour they paint; 0 clears back to untinted.
    AI inline void RasterBand(const u8 tint) { ppu::SetColorPriority(tint); }
#else
    // Shipping build, or a non-NES backend: compiles to nothing. `tint` is taken
    // and ignored rather than #ifdef'd out at every call site, which keeps the
    // call sites readable and keeps them compiling either way.
    AI inline void RasterBand(u8) {}
#endif

}   // namespace demo
