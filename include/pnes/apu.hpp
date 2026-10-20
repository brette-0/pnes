/**
 * @file apu.hpp
 * @brief 2A03 APU write-only register file and PRESERVE/RESTORE family.
 *
 * Exposes every write-only APU register as a ::wo_register shadow so that
 * reads always return the last value written, and writes go to both the
 * shadow and the hardware port.
 *
 * The ::APU_REGISTERS family lets caller code bracket an ISR that touches
 * the APU with ::PRESERVE / ::RESTORE:
 *
 * @code
 *   PRESERVE(APU_REGISTERS);
 *   // ... APU is now free for other use ...
 *
 *   // inside the IRQ handler:
 *   RESTORE(APU_REGISTERS);
 * @endcode
 *
 * The 2A03 addresses are meaningless off NES, so non-NES targets get an inert
 * stand-in rather than an absent header, keeping every call site compiling
 * unchanged.
 */
#pragma once

#ifdef TARGET_NES

#include <pnes/technology.hpp>

/**
 * @brief 2A03 APU register file as a namespace.
 *
 * Access as `apu::sq1_vol`, `apu::DisableFrameIRQ()`, etc.
 * Addresses $4009 and $400D are unused by the 2A03 and are omitted.
 */
namespace apu {
    extern tech::wo_register<0x4000> sq1_vol;       ///< Pulse 1 — duty / envelope / volume
    extern tech::wo_register<0x4001> sq1_sweep;     ///< Pulse 1 — sweep unit
    extern tech::wo_register<0x4002> sq1_lo;        ///< Pulse 1 — timer low 8 bits
    extern tech::wo_register<0x4003> sq1_hi;        ///< Pulse 1 — length counter + timer high

    extern tech::wo_register<0x4004> sq2_vol;       ///< Pulse 2 — duty / envelope / volume
    extern tech::wo_register<0x4005> sq2_sweep;     ///< Pulse 2 — sweep unit
    extern tech::wo_register<0x4006> sq2_lo;        ///< Pulse 2 — timer low 8 bits
    extern tech::wo_register<0x4007> sq2_hi;        ///< Pulse 2 — length counter + timer high

    extern tech::wo_register<0x4008> tri_linear;    ///< Triangle — linear counter control
    extern tech::wo_register<0x400A> tri_lo;        ///< Triangle — timer low 8 bits
    extern tech::wo_register<0x400B> tri_hi;        ///< Triangle — length counter + timer high

    extern tech::wo_register<0x400C> noise_vol;     ///< Noise — envelope / volume
    extern tech::wo_register<0x400E> noise_lo;      ///< Noise — mode + period
    extern tech::wo_register<0x400F> noise_hi;      ///< Noise — length counter

    extern tech::wo_register<0x4010> dmc_freq;      ///< DMC — IRQ enable, loop, rate index
    extern tech::wo_register<0x4011> dmc_raw;       ///< DMC — direct load (7-bit DAC level)
    extern tech::wo_register<0x4012> dmc_start;     ///< DMC — sample start address ($C000 base)
    extern tech::wo_register<0x4013> dmc_len;       ///< DMC — sample byte count

    extern tech::wo_register<0x4015> snd_chn;       ///< Channel enable / length counter status
    extern tech::wo_register<0x4017> frame_counter; ///< Frame counter mode + IRQ inhibit

    /** @brief Inhibit the frame counter IRQ — forces 5-step mode ($4017 bits 7+6). */
    inline void DisableFrameIRQ() { frame_counter = static_cast<u8>(frame_counter) | 0xC0; }
    /** @brief Allow the frame counter IRQ ($4017 bit 6 clear). */
    inline void EnableFrameIRQ()  { frame_counter = static_cast<u8>(frame_counter) & ~0x40; }

    /** @brief Enable the DMC IRQ ($4010 bit 7). */
    inline void EnableDMCIRQ()    { dmc_freq = static_cast<u8>(dmc_freq) | 0x80; }
    /** @brief Disable the DMC IRQ ($4010 bit 7 clear). */
    inline void DisableDMCIRQ()   { dmc_freq = static_cast<u8>(dmc_freq) & ~0x80; }
}

/**
 * @brief PRESERVE/RESTORE family covering APU registers except DMC timing
 *        and the channel-enable register.
 *
 * Expand wherever ::PRESERVE / ::RESTORE / ::SHADOW expect a comma-separated
 * list of lvalues. Names every ::apu field except the DMC registers and
 * snd_chn, in struct order -- those are hardware-distinct from the melodic
 * channels and managed directly where needed.
 *
 * `APU_REGISTERS_snapshot` is the flat byte storage ::PRESERVE writes and
 * ::RESTORE reads, sized to match this list exactly.
 *
 * @code
 *   PRESERVE(APU_REGISTERS);   // snapshot all 15 shadows, no poke
 *   // ... in the ISR:
 *   RESTORE(APU_REGISTERS);    // replay shadows + poke hardware
 * @endcode
 */
#define APU_REGISTERS \
    apu::sq1_vol,   apu::sq1_sweep, apu::sq1_lo,  apu::sq1_hi,  \
    apu::sq2_vol,   apu::sq2_sweep, apu::sq2_lo,  apu::sq2_hi,  \
    apu::tri_linear,apu::tri_lo,    apu::tri_hi,                 \
    apu::noise_vol, apu::noise_lo,  apu::noise_hi,               \
    apu::frame_counter

/** @brief Flat snapshot storage for the ::APU_REGISTERS family (15 bytes). */
extern u8 APU_REGISTERS_snapshot[15];

#else

#include <pnes/technology.hpp>

/**
 * @brief Desktop/non-NES stand-in for the 2A03 APU register file.
 *
 * @note This class does nothing on non-NES targets: there is no 2A03 (or
 * equivalent) hardware to address, and the desktop/console audio backends
 * drive sound through FamiStudio/PCM mixing in audio.hpp instead, so no code
 * here ever reads these values back. Each member is a plain in-memory byte
 * with no hardware effect -- present purely so portable call sites (::IRQ
 * handlers, ::PRESERVE / ::RESTORE / ::SHADOW over ::APU_REGISTERS, and direct
 * calls like `apu::DisableFrameIRQ()`) compile unchanged across every target.
 */
namespace apu {
    extern u8 sq1_vol, sq1_sweep, sq1_lo, sq1_hi;
    extern u8 sq2_vol, sq2_sweep, sq2_lo, sq2_hi;
    extern u8 tri_linear, tri_lo, tri_hi;
    extern u8 noise_vol, noise_lo, noise_hi;
    extern u8 dmc_freq, dmc_raw, dmc_start, dmc_len;
    extern u8 snd_chn, frame_counter;

    /** @brief Does nothing on non-NES targets; see the namespace-level @note. */
    inline void DisableFrameIRQ() {}
    /** @brief Does nothing on non-NES targets; see the namespace-level @note. */
    inline void EnableFrameIRQ()  {}
    /** @brief Does nothing on non-NES targets; see the namespace-level @note. */
    inline void EnableDMCIRQ()    {}
    /** @brief Does nothing on non-NES targets; see the namespace-level @note. */
    inline void DisableDMCIRQ()   {}
}

/**
 * @brief PRESERVE/RESTORE family covering APU registers except DMC timing
 *        and snd_chn — same field list as the NES version, over the inert
 *        non-NES ::apu.
 *
 * @note Saving/restoring these fields does nothing observable on non-NES
 * targets: they back no hardware and nothing reads them. Kept in lock-step
 * with the NES field list so a `PRESERVE(APU_REGISTERS)` / `RESTORE(...)` call
 * site needs no ifdefs.
 */
#define APU_REGISTERS \
    apu::sq1_vol,   apu::sq1_sweep, apu::sq1_lo,  apu::sq1_hi,  \
    apu::sq2_vol,   apu::sq2_sweep, apu::sq2_lo,  apu::sq2_hi,  \
    apu::tri_linear,apu::tri_lo,    apu::tri_hi,                 \
    apu::noise_vol, apu::noise_lo,  apu::noise_hi,               \
    apu::frame_counter

/** @brief Flat snapshot storage for the ::APU_REGISTERS family (15 bytes). */
extern u8 APU_REGISTERS_snapshot[15];

#endif // TARGET_NES