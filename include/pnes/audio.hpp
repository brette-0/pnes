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
 * Application code should use the ::TRACKS and ::SFX macros to register
 * assets at file scope, then call the play/update helpers at runtime.
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

namespace audio {

/**
 * @brief A single music track.
 *
 * On the NES, a track is a pointer to FamiStudio song data baked into
 * ROM. On desktop builds, a track is a filesystem path plus a loop
 * point expressed as a byte offset (0 means "loop the whole song").
 */
typedef struct {
#ifdef TARGET_NES
    const u8 *pTracks;       /**< Pointer to FamiStudio song data in ROM. */
#else
    const char* fp;               /**< Filesystem path to the PCM/ogg/wav source. */
    float    loop_start;          /**< Byte offset where the loop begins (0 = loop the whole song). */
#endif
} music_t;

/**
 * @brief A single sound effect.
 *
 * NES effects reference two FamiStudio sound banks (regular and
 * sample). Desktop/console-port effects are a filesystem path to a
 * one-shot PCM/ogg/wav source, mirroring ::music_t's desktop shape;
 * each backend decodes it once at init into its own internal mixing
 * buffer, the same way it does for ::tracks.
 */
typedef struct {
#ifdef TARGET_NES
    const u8* pSFX;          /**< Pointer to the FamiStudio SFX bank. */
    const u8* pSSFX;         /**< Pointer to the FamiStudio sample-SFX bank. */
#else
    const char* fp;          /**< Filesystem path to the PCM/ogg/wav source. */
#endif
} sfx_t;

} // namespace audio

#ifndef TARGET_NES
namespace audio {
/** @brief Shared mixing buffer written by the SDL3 audio callback. */
extern float *pcm_buffer;
/** @brief Size of ::pcm_buffer, in samples. */
extern u32 pcm_buffer_size;
}
/** @brief Application-defined track table; defined via ::TRACKS. */
extern const audio::music_t tracks[];
/** @brief Number of entries in ::tracks. */
extern const u8 nTracks;

/**
 * @brief Declares the application's track table (desktop build).
 *
 * Expands to definitions of ::tracks and ::nTracks. Use once at file
 * scope with a brace-enclosed list of ::audio::music_t initialisers.
 *
 * @code
 *   TRACKS(
 *     { "assets/title.ogg", 0.0f },
 *     { "assets/stage1.ogg", 12.5f },
 *   )
 * @endcode
 */
#define TRACKS(...)                         \
    const audio::music_t  tracks[] = {__VA_ARGS__ }; \
    const u8 nTracks = sizeof(tracks) / sizeof(audio::music_t);

/** @brief Application-defined SFX table; defined via ::SFX. Each backend
 *         provides a weak zero-entry default, so apps that declare no SFX
 *         still link. */
extern const audio::sfx_t sfxs[];
/** @brief Number of entries in ::sfxs. */
extern const u8 nSfx;

/**
 * @brief Declares the application's one-shot SFX table (desktop/console-port build).
 *
 * Expands to definitions of ::sfxs and ::nSfx. Use once at file scope with
 * a brace-enclosed list of ::audio::sfx_t initialisers.
 *
 * @code
 *   SFX(
 *     { "assets/jump.wav" },
 *     { "assets/hit.wav" },
 *   )
 * @endcode
 */
#define SFX(...)                            \
    const audio::sfx_t sfxs[] = {__VA_ARGS__ }; \
    const u8 nSfx = sizeof(sfxs) / sizeof(audio::sfx_t);
#else
/** @brief Pointer to the FamiStudio music bank (NES build). */
extern const u8* tracks;
/** @brief Pointer to the FamiStudio SFX bank (NES build). */
extern const u8* sfx;
/**
 * @brief Binds a FamiStudio music bank for the NES build.
 *
 * Expands to a plain global definition with NO placement of its own -- this
 * header owns no memory. Prefix the invocation with whatever placement
 * keyword the project's mapper defines (e.g. mmc3.hpp's ::CARTMEM/::SYSMEM),
 * so the project decides where these two bytes live, same as it decides for
 * everything else it defines:
 *
 *     CARTMEM TRACKS(_music_data);
 *
 * With no prefix at all, the pointer falls through to whatever the linker
 * script's default rule catches -- a project choice made in its own link.ld,
 * never a default this header picks for you.
 *
 * @param ptr Symbol or address of the FamiStudio music export.
 */
#define TRACKS(ptr)                     \
    const u8* tracks = (const u8*)(ptr)

/**
 * @brief Binds a FamiStudio SFX bank for the NES build.
 *
 * Same placement contract as ::TRACKS -- prefix with a placement keyword:
 *
 *     CARTMEM SFX(_sounds);
 *
 * @param ptr Symbol or address of the FamiStudio SFX export.
 */
#define SFX(ptr)                        \
    const u8* sfx = (const u8*)(ptr)

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
 * @brief Plays a sound effect on a specific mixer channel.
 *
 * A channel is a fixed voice slot: starting a new effect on a channel that
 * is still playing cuts the old one off, matching FamiStudio's own SFX
 * engine on the NES. Channels are otherwise independent and mix together.
 *
 * @param index   SFX bank index.
 * @param channel Target audio channel.
 */
void SfxPlay(u8 index, u8 channel) ;

/**
 * @brief Plays a sample-based sound effect (DPCM on NES).
 * @param index SFX sample index.
 */
void SfxSamplePlay(u8 index);

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