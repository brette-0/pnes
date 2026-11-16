/**
 * @file audio.hpp
 * @brief Music and sound-effect playback API.
 *
 * Provides a uniform interface over two very different audio backends:
 *
 * - **NES**: drives the FamiStudio sound engine directly; tracks and
 *   SFX are raw byte streams in ROM.
 * - **SDL3**: decodes PCM from disk and mixes it into a shared output
 *   buffer.
 *
 * Application code defines ::tracks (and its ::nTracks count off-NES) at file
 * scope itself, registers effects with ::audio::SetSfx off-NES (on the NES the
 * backend names the effect bank itself), then calls the play/update helpers at
 * runtime.
 */
#pragma once
#include <intsh>
using namespace br0::intsh;


/*
 * THE NES BACKEND IS A PROJECT CHOICE, NOT A LIB ONE: pnes ships no
 * audio driver of its own. These are plain prototypes -- a real definition
 * arrives when the project's chosen backend source file is compiled into the
 * build (src/nes/audio/famistudio.cpp on NES, this platform's own audio.cpp
 * elsewhere), the ordinary way any declared-but-not-yet-defined function
 * works. Nothing here needs to know which one, or whether it exists yet.
 *
 * FamiStudio's backend is PLACED BY THE CONSUMING PROJECT, into the same
 * PRG-ROM bank as the audio engine it drives -- that adjacency is what lets
 * it reach the engine with plain calls and contain no bank switching at all.
 * The knob is PNES_AUDIO_SECTION. Nothing that merely CALLS this API
 * needs to know that either -- see famistudio.cpp's own header comment.
 */

/**
 * @brief Marks an entry point the current backend cannot honour.
 *
 * Expands to a function attribute that warns AT THE CALL SITE, so it costs
 * nothing unless the call actually exists -- unlike a `#warning`, which fires
 * on every translation unit that merely includes this header, or a runtime
 * log, which would spend ROM on a function whose whole body is a no-op.
 * Empty where the backend does implement the call, and empty where the
 * compiler has no such attribute.
 */
#if defined(TARGET_NES) && defined(__has_attribute)
#  if __has_attribute(warning)
#    define PNES_SFX_UNSUPPORTED __attribute__((warning( \
         "this NES audio driver cannot stop or pause an effect: FamiStudio's " \
         "engine has no entry point for it, so the call does nothing -- see " \
         ".ignore/famistudio-sfx-stop/NOTES.md")))
#  endif
#endif
#ifndef PNES_SFX_UNSUPPORTED
#  define PNES_SFX_UNSUPPORTED
#endif

namespace audio {

#ifndef TARGET_NES
/**
 * @brief A single music track.
 *
 * A filesystem path plus a loop point expressed as a byte offset (0 means
 * "loop the whole song").
 *
 * NO NES FORM, for the same reason ::sfx_t has none: it used to carry a
 * `pTracks` pointer under `#ifdef TARGET_NES`, but nothing on the NES ever
 * read a ::music_t. Songs there are not a table -- ::tracks is a single
 * pointer at one FamiStudio export holding every song, and
 * ::TrackPlay's index selects within that export, not within an array
 * this library knows about.
 */
typedef struct {
    const char* fp;               /**< Filesystem path to the PCM/ogg/wav source. */
    float    loop_start;          /**< Byte offset where the loop begins (0 = loop the whole song). */
} music_t;

/**
 * @brief A single sound effect.
 *
 * A filesystem path to a one-shot PCM/ogg/wav source, mirroring ::music_t's
 * desktop shape; each backend decodes it once at init into its own internal
 * mixing buffer, the same way it does for ::tracks.
 *
 * THERE IS NO NES FORM OF THIS TYPE, and there never was a working one. It
 * used to carry `pSFX` and `pSSFX` under `#ifdef TARGET_NES` -- the regular
 * and sample FamiStudio banks -- but nothing on the NES ever read a ::sfx_t:
 * effects there are not a per-effect table at all, just the one engine-wide
 * `sounds` bank that src/nes/audio/famistudio.cpp names for itself. The
 * second pointer was wrong in shape besides, since famistudio_sfx_init takes
 * one bank and the engine reaches DPCM samples through the MUSIC data it was
 * initialised with, not through anything per-effect.
 */
typedef struct {
    const char* fp;          /**< Filesystem path to the PCM/ogg/wav source. */
} sfx_t;
#endif

/**
 * @brief How many sound effects this platform can play at once.
 *
 * The ::SfxPlay channel argument is a slot index, so the valid range is
 * `0 .. sfxChannels - 1`. NOTHING CHECKS IT, on any backend worth relying on:
 * the NES driver passes it straight to the engine, and the backends that do
 * guard it drop the call silently, which is no more useful. The number is
 * published here so callers can be written against it -- `static_assert`, a
 * channel enum, a compile-time round-robin -- rather than guess it.
 *
 * IT DIFFERS BY PLATFORM, DELIBERATELY. On the NES it is the engine's stream
 * count, configured once as PNES_SFX_STREAMS (see CMakeLists.txt, which feeds
 * the same number to demo/famistudio_config.s so the two cannot disagree).
 * Elsewhere it is the fixed voice-slot count the mixing backends allocate.
 * These are not the same quantity and are not worth forcing to agree: an APU
 * stream costs the NES real channel hardware, while a desktop voice costs a
 * cursor and an add.
 */
#ifdef TARGET_NES
#if !defined(FAMISTUDIO_CFG_SFX_STREAMS)
#error "FAMISTUDIO_CFG_SFX_STREAMS is not defined. CMakeLists.txt passes it \
as a compile definition, derived from PNES_SFX_STREAMS -- the same value it \
feeds to the engine's own config. audio::sfxChannels cannot be honest without it."
#endif
constexpr u8 sfxChannels = FAMISTUDIO_CFG_SFX_STREAMS;
#else
constexpr u8 sfxChannels = 4;
#endif

} // namespace audio

#ifndef TARGET_NES
namespace audio {
/** @brief Shared mixing buffer written by the SDL3 audio callback. */
extern float *pcm_buffer;
/** @brief Size of ::pcm_buffer, in samples. */
extern u32 pcm_buffer_size;
}
/**
 * @brief Application-defined track table.
 *
 * DEFINED BY THE APPLICATION, in ordinary C++ -- there is no macro for it.
 * One array of ::audio::music_t and its count, at file scope, once:
 *
 * @code
 *   const audio::music_t tracks[] = {
 *     { "assets/title.ogg", 0.0f },
 *     { "assets/stage1.ogg", 12.5f },
 *   };
 *   const u8 nTracks = sizeof(tracks) / sizeof(audio::music_t);
 * @endcode
 *
 * There used to be a ::TRACKS macro wrapping exactly those two lines. It was
 * removed because it wrapped them DIFFERENTLY per platform -- the NES build's
 * version took a symbol and defined a pointer, nothing like a table -- so one
 * name meant two unrelated things and hid which one a project was getting. A
 * plain definition is self-evident, and the count is one obvious line.
 */
extern const audio::music_t tracks[];
/** @brief Number of entries in ::tracks. */
extern const u8 nTracks;

/**
 * @brief The registered SFX table, and its entry count.
 *
 * LIBRARY STATE, NOT APPLICATION API. These are owned by src/all/audio.cpp
 * and written only by ::audio::SetSfx; they are declared here because every
 * backend reads them from its own translation unit. An application never
 * defines or assigns them -- it calls ::audio::SetSfx and the table it passes
 * is the one these point at.
 *
 * A POINTER AND A COUNT, not an array and a sizeof. There used to be an ::SFX
 * macro that defined `sfxs[]`/`nSfx` in the application, plus a weak
 * zero-entry default in every backend to keep the link working for apps that
 * never invoked it. Registration replaces both: nothing is weak, nothing is
 * defined twice, and an app with no effects simply never calls ::SetSfx --
 * the pointer stays null and the count stays zero.
 *
 * Both are null/zero until ::SetSfx runs, so every reader must bound-check
 * against ::nSfx before indexing ::sfxs.
 */
extern const audio::sfx_t* sfxs;
/** @brief Number of entries in ::sfxs; zero until ::audio::SetSfx runs. */
extern u8 nSfx;
#else
/**
 * @brief Pointer to the FamiStudio music bank (NES build).
 *
 * DEFINED BY THE APPLICATION, in ordinary C++ -- there is no macro for it.
 * The export's own symbol is named after the FamiStudio project, so only the
 * project can name it:
 *
 *     extern const u8 _music_data_my_songs[];
 *     CARTMEM const u8* const tracks = _music_data_my_songs;
 *
 * THE PLACEMENT KEYWORD IS NOT OPTIONAL DECORATION, and this header owns no
 * memory: prefix the definition with whatever the project's mapper defines
 * (e.g. mmc3.hpp's ::CARTMEM/::SYSMEM) so the project decides where these two
 * bytes live, same as it decides for everything else it defines. With no
 * prefix the pointer falls through to whatever the linker script's default
 * rule catches -- a project choice made in its own link.ld, never a default
 * this header picks.
 *
 * THE POINTER ITSELF IS const, not merely the bytes it points at, and that is
 * load-bearing. audio::Init hands this value to famistudio_init through an
 * inline-asm "i" (immediate) operand, so it has to be a link-time constant
 * that folds into `ldx #<` / `ldy #>`. A mutable pointer only folds as far as
 * the optimiser can prove nothing writes it -- true today, but true by luck.
 * Top-level const makes it a guarantee, and makes a stray assignment a
 * compile error instead of a silently de-optimised immediate.
 *
 * WHICH IS ALSO WHY THE DECLARATION ABOVE MATTERS. A namespace-scope const
 * object has internal linkage by default; it gets external linkage here only
 * because this `extern` declaration precedes the definition. Define ::tracks
 * without including this header first and it becomes a private copy, leaving
 * the real symbol undefined at link time.
 *
 * A runtime setter is the other thing this rules out: the pointer would become
 * RAM the engine must load through, and "assign before Init" an unenforced
 * ordering contract instead of a link-time fact.
 */
extern const u8* const tracks;

/* THERE IS NO `sfx` HERE, deliberately, and the asymmetry with ::tracks above
 * is the whole point. FamiStudio hardcodes its sound-effect symbol -- always
 * `sounds`, exported as `_sounds`, whatever the project is called -- so the
 * backend can name it without being told, and src/nes/audio/famistudio.cpp
 * does exactly that, privately. The music symbol carries the project's name
 * (`music_data_<project>`), so only the project can name that one, which is
 * why ::tracks stays an extern the application defines.
 */


#endif

namespace audio {

/**
 * @brief Starts playback of the track at the given table index.
 * @param index Index into the application-declared track table.
 */
void TrackPlay(u8 index);

/**
 * @brief Pauses or resumes the currently playing track.
 * @param pause Non-zero to pause; zero to resume.
 */
void TrackPause(u8 pause);

/** @brief Stops the currently playing track and releases its resources. */
void TrackStop();

/**
 * @brief Scales the music output.
 *
 * @p percent is a proportion of full output: 0 is silence, 100 is the
 * backend's own unattenuated level, and anything above 100 is clamped to it --
 * no backend here amplifies past unity, which buys clipping rather than
 * loudness.
 *
 * LINEAR IN AMPLITUDE, deliberately. It is the fraction a volume slider reads
 * out (`current / max`), not a perceptual curve: 50 is half the amplitude,
 * which sounds rather louder than half. A game wanting a dB taper applies it
 * to the number before passing it, where the UI that chose it can say so.
 *
 * MUSIC ONLY, so a sound effect still plays at full level while the music is
 * turned down -- every backend with both keeps them on separate voices or
 * mixes the effects in after the scale. The one exception is the PS2, whose
 * audsrv volume is device-wide; it has no SFX path today, and the day it gets
 * one this is where that shows up.
 *
 * Persistent and independent of playback state: it survives ::TrackPlay,
 * ::TrackPause and ::TrackStop, applies to whatever plays next, and may be set
 * before ::Init (each backend applies it when it opens its device). The
 * default is 100, so a game that never calls this hears what it always did.
 *
 * A no-op where there is nothing to scale -- the silent 3DS/GBA/DS stubs -- and
 * on the NES, whose engine has no master level at all (FamiStudio's volume
 * lives per note in the song data, and the NES options page offers an on/off
 * button instead of a slider for exactly that reason).
 *
 * @param percent 0-100. Values above 100 are clamped.
 */
void TrackVolume(u8 percent);

/**
 * @brief Plays a sound effect on a specific mixer channel.
 *
 * A channel is a fixed voice slot: starting a new effect on a channel that
 * is still playing cuts the old one off, matching FamiStudio's own SFX
 * engine on the NES. Channels are otherwise independent and mix together.
 *
 * @p volume HAS NO MEANING AT THIS LAYER, DELIBERATELY. It is a byte this API
 * carries and does not interpret: what it scales, over what range, and whether
 * it does anything at all are each the BACKEND's to define and to document in
 * its own file. Nothing here converts it, clamps it or compares it, and there
 * is no default -- a default would be this header picking a unity value, which
 * is exactly the meaning it is refusing to pick.
 *
 * THE ALTERNATIVE WAS A MASTER ::SfxVolume SETTER, and this is the better
 * primitive for the same reason a per-note volume beats a per-channel one: a
 * caller that wants one level for everything passes the same byte every time,
 * while a caller that wants a quieter footstep than a gunshot cannot get that
 * out of a setter without calling it twice around every play. It also keeps the
 * level out of backend state, so nothing has to survive ::Init, a device
 * reopen, or a platform with no persistent mixer to hold it.
 *
 * WHAT EACH BACKEND DOES WITH IT TODAY:
 *   - SDL3, Switch, Wii U: 0 is silence and 255 is unity, applied per voice as
 *     a linear amplitude coefficient (so it is the fraction a slider reads out,
 *     scaled to a whole byte -- see ::TrackVolume on why linear and not
 *     perceptual).
 *   - GC/Wii: the same reading, and the same range, but applied by ASND rather
 *     than in software -- its own per-voice scale is 0-MAX_VOLUME (255), so the
 *     byte is passed through untouched.
 *   - NES: DISCARDED. FamiStudio's engine has no per-effect level -- volume
 *     lives per note inside the effect data -- so the parameter is read by
 *     nothing and the value is irrelevant. Pass whatever is cheapest to
 *     materialise; a literal costs one immediate at the call site.
 *   - The silent stubs (PSP, PS2, PS3, 3DS, DS, GBA): ignored along with the
 *     rest of the call.
 *
 * NO WARNING ATTRIBUTE ON THE NES CASE, unlike ::SfxStop and ::SfxPause. Those
 * cannot do the thing they are named for at all, so a call site there is asking
 * for something it will not get. This one plays the effect correctly and only
 * declines to scale it, which is a documented property of the parameter rather
 * than a failed call -- and ::PNES_SFX_UNSUPPORTED would fire on every NES
 * effect ever played.
 *
 * @param index   SFX bank index.
 * @param channel Target audio channel.
 * @param volume  Backend-defined level. See above; discarded on NES.
 */
void SfxPlay(u8 index, u8 channel, u8 volume) ;

/**
 * @brief Stops the effect on a channel, discarding its position.
 *
 * NOT SUPPORTED ON THE NES, and a call site there gets a compiler warning via
 * ::PNES_SFX_UNSUPPORTED rather than silence. FamiStudio's engine exposes
 * eight entry points and not one of them cancels an effect -- music gets
 * ::TrackStop and ::TrackPause, effects get nothing, so the NES driver is a
 * no-op by necessity, not by choice. Workarounds exist and are written up in
 * .ignore/famistudio-sfx-stop/NOTES.md; a different NES audio driver could
 * implement this for real, which is why it lives in the API rather than only
 * in the backends that manage it.
 *
 * Elsewhere it is exact: the channel's voice slot is released and the effect
 * stops at once. Stopping an idle channel does nothing and is not an error.
 *
 * @param channel Slot to stop, `0 .. sfxChannels - 1`. Unchecked.
 */
PNES_SFX_UNSUPPORTED void SfxStop(u8 channel);

/**
 * @brief Pauses or resumes the effect on a channel, keeping its position.
 *
 * Same NES caveat as ::SfxStop, and the same warning on a call site there.
 * Note that pause is the weaker loss on the NES even in principle: effects
 * are a handful of frames each, so a paused one is indistinguishable from a
 * stopped one.
 *
 * Elsewhere the voice keeps its cursor while paused and resumes from it, so
 * it is distinct from ::SfxStop. Pausing an idle channel does nothing.
 *
 * @param channel Slot to act on, `0 .. sfxChannels - 1`. Unchecked.
 * @param pause   Non-zero to pause, zero to resume.
 */
PNES_SFX_UNSUPPORTED void SfxPause(u8 channel, u8 pause);

/**
 * @brief Plays a sample-based sound effect.
 *
 * ON THE NES THIS IS A GENUINELY DIFFERENT ROUTINE, not ::SfxPlay with a
 * fixed channel: it calls the engine's DPCM entry point, which drives the DMC
 * channel. @p index therefore selects from the DPCM SAMPLE LIST the engine
 * was initialised with -- carried by the MUSIC export, not the SFX export --
 * so it is NOT an index into the same table ::SfxPlay uses. It is a no-op
 * unless FAMISTUDIO_CFG_DPCM_SUPPORT is enabled.
 *
 * EVERYWHERE ELSE THERE IS NO SEPARATE SAMPLE BANK: every effect is already a
 * decoded one-shot. The backends that implement it forward to
 * ::SfxPlay(index, 1), so @p index means the same thing it does there, and the
 * silent stubs ignore it. Portable code that wants one sound on both should
 * therefore call ::SfxPlay, and reserve this for a NES build's DPCM effects.
 *
 * @param index DPCM sample index on the NES; an ::sfxs index elsewhere.
 */
void SfxSamplePlay(u8 index);

#ifndef TARGET_NES
/**
 * @brief Registers the application's one-shot SFX table.
 *
 * CALL IT BEFORE ::Init, NOT AFTER. Backends that decode their effects up
 * front do so inside ::Init, reading whatever this has registered; register
 * nothing and they load nothing, and no later call makes them go back. The
 * table itself must outlive every audio call, so it is normally a file-scope
 * array, never a local:
 *
 * @code
 *   constexpr audio::sfx_t mySfx[] = {
 *     { "assets/jump.wav" },
 *     { "assets/hit.wav" },
 *   };
 *   audio::SetSfx(mySfx, std::size(mySfx));
 *   audio::Init();
 * @endcode
 *
 * NOT DECLARED ON THE NES, where the effect bank is a ROM symbol the backend
 * names for itself -- see the ::tracks comment above for why that works for
 * effects and not for music. Portable code guards the call with
 * `#ifndef TARGET_NES`, the same way it already guards the table.
 *
 * @param table Entries to register, or nullptr for none. Not copied: the
 *              caller keeps ownership and must keep it alive.
 * @param count Number of entries in @p table.
 */
void SetSfx(const sfx_t* table, u8 count);
#endif

/**
 * @brief Initializes the audio backend; must be called before any other audio function.
 * @param region Playback region: 0 for NTSC (default), 1 for PAL.
 */
void Init(u8 region = 0);

/**
 * @brief Runs @p block with everything the audio backend needs mapped.
 *
 * THE BACKEND'S REQUIREMENT LIVES HERE, not in any game: the engine walks its
 * song data while executing, so data and code must be mapped together and
 * cannot share a window. Map only the code bank and the engine reads whatever
 * else is at that address -- silence, noise or a crash, with no diagnostic.
 *
 * THE MAPPER IS A TEMPLATE PARAMETER, not an include, which is what keeps this
 * in the portable header. Nothing here names a mapper, a project's tag types,
 * or knows a bank exists: @p Mapper supplies CallPairedBlock, @p CodeTag/
 * @p DataTag are the project's own bank map, and off-NES the whole thing is
 * the block itself:
 *
 *     audio::InBanks<MyMapper, MyCodeTag, MyDataTag>([] { audio::Update(); });
 *
 * and that line compiles to a plain call on SDL3, GBA, Switch and the rest.
 */
template <typename Mapper, typename CodeTag, typename DataTag, typename Block>
decltype(auto) InBanks(Block &&block) {
#ifdef TARGET_NES
    return Mapper::template CallPairedBlock<CodeTag, DataTag>(
        static_cast<Block &&>(block));
#else
    return block();
#endif
}

/**
 * @brief Drives the audio engine forward by one frame.
 *
 * Must be called once per video frame to advance the FamiStudio player
 * (NES) or to refill the PCM mix buffer (desktop).
 */
void Update();

} // namespace audio