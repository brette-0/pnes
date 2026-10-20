/**
 * @file pnes.hpp
 * @brief Umbrella header for the pnes library.
 *
 * Pulls in every public subsystem header (types, technology, reset,
 * input, video, audio, interrupts) and establishes the compile-time
 * configuration required by the rest of the library:
 *
 * - Exactly one of ::LANDSCAPE or ::PORTRAIT must be defined to select
 *   nametable mirroring. Defining both, or neither, is a hard error.
 * - ::TARGET_NES selects the 6502/NES backend; when absent the SDL3
 *   desktop backend is used instead.
 *
 * Include this file — not the individual subsystem headers — from
 * application code.
 */
#pragma once
#include <intsh>
using namespace br0::intsh;

#if !defined(__GNUC__) && !defined(__clang__)
#error "pnes requires GCC or Clang. MSVC is not supported."
#endif

#if defined(LANDSCAPE) && defined(PORTRAIT)
#error "Must not define both Mirroring"
#endif

#if !defined(LANDSCAPE) && !defined(PORTRAIT)
#error "Must define Mirroring"
#endif



#ifndef __mos__
    /**
     * @brief Attribute applied to interrupt handlers to suppress recursive
     *        entry on backends that lack llvm-mos's `interrupt_norecurse`.
     *
     * Expands to the real attribute under llvm-mos; to nothing elsewhere.
     */
    #define interrupt_norecurse
#endif

#ifdef TARGET_NES
/**
 * @brief Placeholder for the desktop quit flag on NES builds.
 *
 * The NES backend has no exit condition, so this is a compile-time
 * constant 0 that callers can test uniformly against the SDL3 flag.
 */
#define quit 0
#else
/**
 * @brief Desktop-only exit flag.
 *
 * Set to non-zero by the SDL3 event loop when the user closes the
 * window or requests shutdown. The main loop should poll this each
 * frame and break when it becomes true.
 */
extern int quit;
#endif

#include "types.hpp"
#include "technology.hpp"
#include "input.hpp"
#include "video.hpp"
#include "audio.hpp"
#include "interrupts.hpp"