/**
 * @file prxptr_clang_shim.hpp
 * @brief Neutralises PSL1GHT's ATTRIBUTE_PRXPTR for Clang. Include this after
 *        <ppu-types.h> and before any <rsx/...> header; it is a no-op under GCC.
 *
 * ## The problem
 *
 * PSL1GHT declares the 32-bit pointers that appear inside some lv2/RSX structs
 * with `__attribute__((mode(SI)))`:
 *
 *     #define ATTRIBUTE_PRXPTR  __attribute__((mode(SI)))      // ppu-types.h
 *     u32 *__restrict current ATTRIBUTE_PRXPTR;                // rsx/gcm_sys.h
 *
 * GCC accepts `mode` on a pointer declarator and gives you a 4-byte pointer
 * field, which is what the PS3's PRX ABI wants. Clang restricts the `mode`
 * attribute to integer and floating-point types and hard-errors on a pointer:
 * "mode attribute only supported for integer and floating-point types". So
 * every translation unit that includes <rsx/rsx.h> fails to compile under
 * Clang, and since cmake/PS3.cmake makes Clang this target's compiler (for
 * #embed -- see that file), that would mean the RSX present path could not be
 * built at all.
 *
 * Checked against PSL1GHT master as of 2026-10-12: ATTRIBUTE_PRXPTR is still
 * defined unconditionally, with no Clang accommodation. This is not a stale
 * 2018 toolchain artefact that a newer ps3dev fixes.
 *
 * ## Why dropping the attribute is safe HERE
 *
 * Dropping it changes layout: those fields become 8-byte pointers, so a struct
 * carrying them has a different size and offsets under Clang than in the
 * GCC-built librsx.a/libgcm_sys.a. That is only a problem if this code reads
 * those fields, allocates such a struct, or takes its sizeof. It does none of
 * those, and that was verified rather than assumed:
 *
 *  1. The only PRXPTR-bearing declarations in the headers this backend
 *     includes are gcmContextData's begin/end/current/callback,
 *     gcmConfiguration's localAddress/ioAddress, and gcmInitBody's prototype.
 *  2. src/ps3/video.cpp only ever holds `gcmContextData *` and passes it
 *     straight back to the library. It never dereferences it, never allocates
 *     one (rsxInit does), and never takes its sizeof. gcmConfiguration and
 *     gcmInitBody are not used at all -- rsxInit is the entry point here.
 *  3. Every rsx/gcm function this backend calls is a real library function in
 *     librsx.a/libgcm_sys.a, compiled by ps3dev's own GCC with the correct
 *     layout. In current PSL1GHT the command API is declared through macros
 *     (rsx/rsx_function_macros.h + rsx/commands_inc.h) rather than written
 *     out, so a plain grep for declarations is misleading -- what matters is
 *     that those expansions are prototypes, not bodies.
 *
 *     There IS one piece of header code that dereferences a PRXPTR field:
 *     rsxContextCallback in rsx/rsx_function_macros.h, whose inline asm reads
 *     `context->callback`. It is emitted only when RSX_INTERNAL is 1, and the
 *     installed rsx/commands.h defines RSX_INTERNAL to 0 around both of its
 *     includes, so it is never emitted on this path. That gate is the thing to
 *     re-check if a PSL1GHT bump ever makes this shim misbehave.
 *
 *     The only true inline definitions in rsx/ are rsxAddressToOffset (a
 *     one-line forwarder to the library's gcmAddressToOffset),
 *     rsxGetFixedSint32 and rsxGetFixedUint16 (pure arithmetic) -- none of
 *     which touches a PRXPTR field.
 *
 * So the mismatched layout is never materialised on this side of the ABI: it
 * exists only as a type Clang parses and then only ever handles by address.
 *
 * ## What would break this
 *
 * Reading any field of gcmContextData from a Clang-compiled TU (e.g. hand-
 * rolling command-buffer writes through `context->current` instead of calling
 * the rsx* helpers), using gcmGetConfiguration/gcmConfiguration, or calling
 * gcmInitBody directly. Any of those would read or write at the wrong offsets
 * and would NOT fail to compile -- so if the RSX path ever needs one of them,
 * that TU has to move to ppu-g++ (CMake can compile a single source with the
 * GCC driver and hand the object to the link) rather than keep this shim.
 */
#pragma once

#if defined(__clang__)
// ppu-types.h defines ATTRIBUTE_PRXPTR unconditionally -- there is no #ifndef
// guard around it -- so pre-defining the macro before that header does not
// work; it has to be replaced afterwards. ppu-types.h's own include guard is
// what makes this stick: rsx.h includes it again, that include is a no-op, and
// the definition below is the one the RSX headers are parsed with.
#  if !defined(__PPU_TYPES_H__)
#    error "prxptr_clang_shim.hpp must be included AFTER <ppu-types.h>"
#  endif
#  undef  ATTRIBUTE_PRXPTR
#  define ATTRIBUTE_PRXPTR
#endif
