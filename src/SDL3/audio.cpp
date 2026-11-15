#include <pnes/audio.hpp>
#include <pnes/technology.hpp>
#include <SDL3/SDL.h>

#include <cstdlib>

/*
 * THE SAME DECODER FIVE CONSOLE BACKENDS ALREADY USE -- src/ogc, src/wiiu,
 * src/psp, src/ps2 and src/ps3 each #include <stb_vorbis.c> under exactly
 * these two flags, and each has its own FetchContent_MakeAvailable(stb) in
 * its CMakeLists.txt branch; the desktop branch now has one too. Vendoring a
 * second Vorbis implementation for the one platform that already links a
 * media library would be the worse trade: SDL3's core decodes RIFF/WAVE and
 * nothing else (SDL_LoadWAV), and the Ogg decoders live in SDL_mixer, a whole
 * extra dependency for one function.
 *
 * NO_PUSHDATA_API because nothing here feeds the decoder incrementally, and
 * NO_STDIO because ::LoadOgg hands it bytes SDL already read -- the
 * stb_vorbis_decode_filename path is the only one that needs a FILE*, and
 * taking it would mean two file-access APIs in one function for no gain.
 */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

typedef struct {
    u32 offset;
    u32 length;
    u32 loop_start;
} track_runtime_t;

static track_runtime_t track_info[256];
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;
static SDL_AudioStream *music_stream = nullptr;
static u32 playback_pos = 0;
static int current_track = -1;
static int music_playing = 0;
static int music_paused = 0;

// audio::TrackVolume's setting, as a percentage of full output. Kept here
// rather than only pushed into SDL because it may be set before Init() has
// opened the stream -- ::OpenAudioStreams applies it on the way out.
static u8 music_volume = 100;

// One-shot SFX PCM, converted to the same F32LE/2ch/48kHz format as music
// and concatenated into its own buffer (kept separate from pcm_buffer so
// loading zero SFX costs nothing).
typedef struct {
    u32 offset;   // float index (not bytes) into sfx_pcm_buffer
    u32 length;   // in floats
} sfx_runtime_t;

static sfx_runtime_t sfx_info[256];
static float *sfx_pcm_buffer = nullptr;
static u32 sfx_pcm_buffer_size = 0;   // in floats

static SDL_AudioStream *sfx_stream = nullptr;

// A channel is a fixed voice slot: playing a new SFX on a busy channel cuts
// the old one off, same as FamiStudio's own SFX engine on the NES. Voices
// mix additively in UpdateSfx() below.
// The voice-slot count, taken from audio.hpp rather than declared here, so
// the number this backend allocates is the same one audio::sfxChannels
// publishes to callers -- see its comment on why it differs from the NES's.
static constexpr int N_SFX_VOICES = audio::sfxChannels;
// `paused` is a separate flag and not the absence of `active`, because
// clearing `active` is already how a voice reports that it FINISHED -- the
// mixer drops it and the cursor goes with it. A pause has to keep the cursor
// to be distinguishable from audio::SfxStop.
// `gain` is this backend's reading of audio.hpp's uninterpreted volume byte:
// a linear amplitude coefficient, 0 silent and 255 unity, divided out once at
// ::SfxPlay rather than per sample in the mixer. Stored per VOICE and not per
// effect, because the level belongs to this playing of it -- the same effect
// can be loud on one channel and quiet on another.
struct sfx_voice_t { u32 pos; u32 end; float gain; bool active; bool paused; };
static sfx_voice_t sfx_voices[N_SFX_VOICES];

static u32 track_offsets[256];
static u32 track_lengths[256];

#define BYTES_PER_SECOND (48000 * 2 * sizeof(float))

// Whether `fp` is an Ogg stream, by its own first four bytes rather than by
// its extension: the container is a fact about the file, and a mislabelled one
// should play rather than be rejected for its name. Four bytes at Init per
// entry (six files in this demo), which is why reading them separately costs
// nothing worth saving.
//
// Unreadable or shorter than four bytes answers "not Ogg", which hands the
// file to SDL_LoadWAV -- whose failure is the one that gets logged, with the
// path in it, by the caller. A missing file therefore reports as a missing
// file and not as a container question.
static bool IsOggFile(const char *fp) {
    SDL_IOStream *io = SDL_IOFromFile(fp, "rb");
    if (!io) return false;

    char magic[4];
    const size_t got = SDL_ReadIO(io, magic, sizeof(magic));
    SDL_CloseIO(io);
    return got == sizeof(magic) && SDL_memcmp(magic, "OggS", 4) == 0;
}

// One Ogg Vorbis file, decoded whole into interleaved S16 at its own sample
// rate and channel count.
//
// WHOLE, NOT STREAMED, which is the right shape here and not a shortcut: the
// caller converts whatever this returns to the mixer's F32LE/2ch/48kHz and
// concatenates it into one buffer at Init (::BuildPCMBuffer /
// ::BuildSfxPCMBuffer), so a streaming decoder would be pulled dry in that
// same loop anyway. The console backends that DO stream do it because 24-64MB
// of console RAM cannot hold the decoded result (see src/ogc/audio.cpp's own
// header); a desktop can.
//
// THE COPY INTO SDL_malloc'd MEMORY IS NOT WASTE. stb_vorbis allocates its
// output with the CRT's malloc, and this function's contract -- the one
// ::LoadTrack documents and SDL_LoadWAV sets -- is that *data is SDL_malloc'd
// and the caller SDL_free's it. Those are only the same allocator by
// coincidence on most platforms, and handing SDL_free a malloc'd pointer is
// undefined wherever they differ. One memcpy of a UI blip at startup buys a
// contract that holds everywhere, and keeps every caller unchanged.
static bool LoadOgg(const char *fp, SDL_AudioSpec *spec, u8 **data, u32 *len) {
    size_t fileLen = 0;
    void *file = SDL_LoadFile(fp, &fileLen);
    if (!file) return false;

    int channels = 0;
    int rate = 0;
    short *pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(
        static_cast<const unsigned char *>(file), static_cast<int>(fileLen),
        &channels, &rate, &pcm);
    SDL_free(file);

    // Negative is stb_vorbis's own failure report; zero frames is a valid
    // decode of an empty stream, which has nothing to play and must not reach
    // SDL_malloc(0).
    if (frames <= 0 || !pcm || channels <= 0) {
        if (pcm) free(pcm);
        SDL_SetError("stb_vorbis could not decode %s", fp);
        return false;
    }

    const u32 bytes = static_cast<u32>(frames) * static_cast<u32>(channels)
                      * static_cast<u32>(sizeof(short));
    u8 *out = static_cast<u8 *>(SDL_malloc(bytes));
    if (!out) {
        free(pcm);
        return false;
    }
    SDL_memcpy(out, pcm, bytes);
    free(pcm);

    spec->format   = SDL_AUDIO_S16LE;
    spec->channels = channels;
    spec->freq     = rate;
    *data = out;
    *len  = bytes;
    return true;
}

// Load one track's source audio into an SDL_LoadWAV-shaped (spec,data,len)
// triple. On success *data is SDL_malloc'd and must be SDL_free'd by the
// caller, exactly like SDL_LoadWAV.
//
// The seam this comment always described: every caller hands the result to
// SDL_ConvertAudioSamples, so a decoder only has to answer in that triple to
// be usable, and which one ran is nothing the rest of this file knows. The WAV
// path is still the plain SDL_LoadWAV call it always was, taken whenever the
// file is not an Ogg stream -- so music that already worked takes byte for
// byte the same route it did before.
static bool LoadTrack(const char *fp, SDL_AudioSpec *spec, u8 **data, u32 *len) {
    if (IsOggFile(fp)) return LoadOgg(fp, spec, data, len);
    return SDL_LoadWAV(fp, spec, data, len);
}

static void BuildPCMBuffer() {
    // first pass — measure total converted size
    constexpr SDL_AudioSpec target = {
        .format = SDL_AUDIO_F32LE,
        .channels = 2,
        .freq = 48000
    };

    auto total = 0;
    for (auto i = 0; i < nTracks; i++) {
        SDL_AudioSpec spec;
        u8 *data;
        u32 len;
        if (LoadTrack(tracks[i].fp, &spec, &data, &len)) {
            u8 *converted = nullptr;
            auto converted_len = 0;
            if (SDL_ConvertAudioSamples(&spec, data, static_cast<int>(len),
                                         &target, &converted, &converted_len)) {
                total += converted_len;
                SDL_free(converted);
            }
            SDL_free(data);
        }
    }

    audio::pcm_buffer = static_cast<float *>(SDL_malloc(total));
    audio::pcm_buffer_size = total;

    // second pass — load, convert, and fill runtime info
    auto offset = 0;
    for (u8 i = 0; i < nTracks; i++) {
        SDL_AudioSpec spec;
        u8 *data;
        u32 len;

        if (!LoadTrack(tracks[i].fp, &spec, &data, &len)) {
            SDL_Log("Failed to load %s: %s", tracks[i].fp, SDL_GetError());
            track_info[i].offset = 0;
            track_info[i].length = 0;
            track_info[i].loop_start = 0;
            continue;
        }

        u8 *converted = nullptr;
        auto converted_len = 0;
        if (SDL_ConvertAudioSamples(&spec, data, static_cast<int>(len),
                                     &target, &converted, &converted_len)) {
            SDL_memcpy(reinterpret_cast<u8 *>(audio::pcm_buffer) + offset, converted, converted_len);

            track_info[i].offset = offset;
            track_info[i].length = converted_len;

            if (tracks[i].loop_start > 0.0f) {
                auto loop_bytes = static_cast<u32>(tracks[i].loop_start * BYTES_PER_SECOND);
                if (loop_bytes > static_cast<u32>(converted_len)) loop_bytes = 0;
                track_info[i].loop_start = offset + loop_bytes;
            } else {
                track_info[i].loop_start = offset;
            }

            offset += converted_len;
            SDL_free(converted);
        }

        SDL_free(data);
    }
}


// Load and convert every declared SFX (mirrors BuildPCMBuffer's two-pass
// shape) into one concatenated float buffer, and fill sfx_info with each
// entry's offset/length in floats.
static void BuildSfxPCMBuffer() {
    constexpr SDL_AudioSpec target = {
        .format = SDL_AUDIO_F32LE,
        .channels = 2,
        .freq = 48000
    };

    auto total = 0;
    for (auto i = 0; i < nSfx; i++) {
        SDL_AudioSpec spec;
        u8 *data;
        u32 len;
        if (LoadTrack(sfxs[i].fp, &spec, &data, &len)) {
            u8 *converted = nullptr;
            auto converted_len = 0;
            if (SDL_ConvertAudioSamples(&spec, data, static_cast<int>(len),
                                         &target, &converted, &converted_len)) {
                total += converted_len;
                SDL_free(converted);
            }
            SDL_free(data);
        }
    }
    // SILENT UNTIL NOW, and that silence is what made this hard to diagnose:
    // zero total with effects registered means every single one failed to load,
    // which is indistinguishable at the speaker from "no effects registered"
    // and from "effects play but are inaudible". Each of those wants a
    // different fix, so each says so.
    if (total == 0) {
        if (nSfx) {
            SDL_Log("AUDIO: all %u registered SFX failed to load -- nothing will play. "
                    "Paths are relative to the CURRENT WORKING DIRECTORY, not the "
                    "executable, and the build copies them next to the executable.",
                    static_cast<unsigned>(nSfx));
        }
        return;
    }

    sfx_pcm_buffer = static_cast<float *>(SDL_malloc(total));
    sfx_pcm_buffer_size = total / sizeof(float);

    auto offset = 0;
    for (u8 i = 0; i < nSfx; i++) {
        SDL_AudioSpec spec;
        u8 *data;
        u32 len;

        if (!LoadTrack(sfxs[i].fp, &spec, &data, &len)) {
            SDL_Log("Failed to load %s: %s", sfxs[i].fp, SDL_GetError());
            sfx_info[i].offset = 0;
            sfx_info[i].length = 0;
            continue;
        }

        u8 *converted = nullptr;
        auto converted_len = 0;
        if (SDL_ConvertAudioSamples(&spec, data, static_cast<int>(len),
                                     &target, &converted, &converted_len)) {
            SDL_memcpy(reinterpret_cast<u8 *>(sfx_pcm_buffer) + offset, converted, converted_len);
            sfx_info[i].offset = offset / sizeof(float);
            sfx_info[i].length = converted_len / sizeof(float);
            offset += converted_len;
            SDL_free(converted);
        }

        SDL_free(data);
    }

    u8 decoded = 0;
    for (u8 i = 0; i < nSfx; i++) {
        if (sfx_info[i].length) decoded++;
    }
    SDL_Log("AUDIO: %u/%u SFX decoded into %u frames on %d voices",
            static_cast<unsigned>(decoded), static_cast<unsigned>(nSfx),
            sfx_pcm_buffer_size / 2u, N_SFX_VOICES);
}

// @p volume is a linear amplitude coefficient here: 0 silence, 255 unity. That
// reading is THIS BACKEND'S, declared here because audio.hpp deliberately
// declines to (see its own note) -- a float is the natural shape for it once
// the mixer is floats anyway, and a desktop pays nothing for the multiply.
void audio::SfxPlay(const u8 index, const u8 channel, const u8 volume) {
    // Each rejection named, rather than one shared early-out. All three mean
    // "no sound", and from the outside they are the same event -- but one is a
    // bad index, one a bad channel, and one an effect that never loaded, and
    // only the last of those is an asset problem.
    if (index >= nSfx) {
        SDL_Log("AUDIO: SfxPlay(%u) past the %u registered effects",
                static_cast<unsigned>(index), static_cast<unsigned>(nSfx));
        return;
    }
    if (channel >= N_SFX_VOICES) {
        SDL_Log("AUDIO: SfxPlay channel %u past this backend's %d voices",
                static_cast<unsigned>(channel), N_SFX_VOICES);
        return;
    }
    if (sfx_info[index].length == 0) {
        SDL_Log("AUDIO: SfxPlay(%u) has no decoded samples (\"%s\" did not load)",
                static_cast<unsigned>(index), sfxs[index].fp);
        return;
    }
    sfx_voices[channel] = {
        .pos    = sfx_info[index].offset,
        .end    = sfx_info[index].offset + sfx_info[index].length,
        // 255 is unity, so the divisor is 255 and not 256: a byte of 0xFF has
        // to come out as exactly 1.0f, or full volume would be imperceptibly
        // short of the level the same sound plays at through ::SfxSamplePlay.
        .gain   = static_cast<float>(volume) / 255.0f,
        .active = true
    };
}

// Unity, spelled out: there is no sample bank to carry a level of its own off
// NES (audio.hpp's ::SfxSamplePlay), so this plays the effect at the level the
// asset was authored at.
void audio::SfxSamplePlay(const u8 index) {
    SfxPlay(index, 1, 0xFF);
}

// Exact here, unlike the NES driver: a voice is a cursor this backend owns, so
// releasing or freezing one is local state (see audio.hpp's ::SfxStop on why
// the NES cannot). Both ignore an out-of-range channel rather than clamping
// into someone else's slot; the range is audio::sfxChannels.
void audio::SfxStop(const u8 channel) {
    if (channel >= N_SFX_VOICES) return;
    sfx_voices[channel].active = false;
    sfx_voices[channel].paused = false;
}

void audio::SfxPause(const u8 channel, const u8 pause) {
    if (channel >= N_SFX_VOICES) return;
    sfx_voices[channel].paused = pause != 0;
}


// Sum every active voice into one chunk and push it to sfx_stream (a second
// device stream, mixed with music_stream by the OS/SDL the same way two
// independent audio sources ever mix -- additively, at the device).
static void UpdateSfx() {
    if (!sfx_stream) return;
    constexpr int CHUNK_FLOATS = 2048;  // ~21ms at 48kHz/2ch

    while (SDL_GetAudioStreamQueued(sfx_stream) <= static_cast<int>(CHUNK_FLOATS * sizeof(float) * 4)) {
        float mix[CHUNK_FLOATS] = {};
        bool any_active = false;

        for (auto &v : sfx_voices) {
            if (!v.active || v.paused) continue;
            any_active = true;
            const u32 avail = v.end - v.pos;
            const u32 take = avail < static_cast<u32>(CHUNK_FLOATS) ? avail : static_cast<u32>(CHUNK_FLOATS);
            const float *src = sfx_pcm_buffer + v.pos;
            // Additive as before, scaled by this voice's own gain. Read into a
            // local so the loop does not reload it per sample.
            const float gain = v.gain;
            for (u32 i = 0; i < take; i++) mix[i] += src[i] * gain;
            v.pos += take;
            if (v.pos >= v.end) v.active = false;
        }

        if (!any_active) break;
        SDL_PutAudioStreamData(sfx_stream, mix, sizeof(mix));
    }
}

static void OpenAudioStreams() {
    if (music_stream) return;  // already opened

    constexpr SDL_AudioSpec spec = {
        .format = SDL_AUDIO_F32LE,
        .channels = 2,
        .freq = 48000
    };
    music_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!music_stream) {
        SDL_Log("AUDIO: stream creation failed: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(music_stream);
    // Whatever volume was set before the device existed (see ::music_volume).
    SDL_SetAudioStreamGain(music_stream, static_cast<float>(music_volume) / 100.0f);

    // OPENED THE SAME WAY MUSIC IS, and that symmetry is the point.
    //
    // This used to be SDL_CreateAudioStream + SDL_BindAudioStream onto
    // music_stream's own logical device -- which is a legal arrangement, but it
    // made the effects path differ from the music path in the one place neither
    // could be observed: a plain stream is created UNBOUND and silent, and
    // whether binding it to a logical device that another stream already owns
    // leaves it fed is a question about SDL's device plumbing rather than about
    // this file. Music worked and effects did not, and that was the only
    // structural difference between them.
    //
    // So there is no difference any more. SDL_OpenAudioDeviceStream opens the
    // device, creates the stream and binds the two together as one act, and
    // SDL_ResumeAudioStreamDevice starts it -- exactly the four lines above,
    // against the same default playback device and the same spec. Two device
    // streams on one physical device is the ordinary way to mix two
    // independent sources; the OS does it for two applications, and it does it
    // here for music and effects.
    //
    // Callback nullptr on both, so both are fed by pushing: ::Update refills
    // music and ::UpdateSfx refills this one, both from the frame loop, with no
    // audio-thread callback to make either reentrant.
    sfx_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!sfx_stream) {
        SDL_Log("AUDIO: SFX stream creation failed: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(sfx_stream);
}

void audio::Init(u8) {
    OpenAudioStreams();

    BuildPCMBuffer();
    BuildSfxPCMBuffer();
}

void audio::Update() {
    if (music_stream && music_playing && !music_paused && current_track >= 0) {
        const auto queued = SDL_GetAudioStreamQueued(music_stream);
        if (queued <= 32768) {
            const track_runtime_t *t = &track_info[current_track];
            const auto end = t->offset + t->length;

            // feed enough for several frames
            auto to_feed = 32768 - queued;
            while (to_feed > 0) {
                const auto remaining = end - playback_pos;
                const auto chunk = remaining < 16384 ? remaining : 16384;

                SDL_PutAudioStreamData(music_stream,
                    reinterpret_cast<u8 *>(pcm_buffer) + playback_pos, static_cast<int>(chunk));
                playback_pos += chunk;
                to_feed -= static_cast<int>(chunk);

                if (playback_pos >= end) {
                    playback_pos = t->loop_start;
                }
            }
        }
    }

    UpdateSfx();
}

void audio::TrackPlay(const u8 index) {
    if (index >= nTracks) return;
    current_track = index;
    playback_pos = track_info[index].offset;
    music_playing = 1;
    music_paused = 0;
    if (music_stream) SDL_ClearAudioStream(music_stream);
}

void audio::TrackStop() {
    music_playing = 0;
    current_track = -1;
    if (music_stream) SDL_ClearAudioStream(music_stream);
}

void audio::TrackPause(const u8 pause) {
    (void)pause;
    music_paused = !music_paused;
}

// SDL3 gives every stream its own gain, so this is one call and no per-sample
// work at all. It lands on the MUSIC stream only: ::sfx_stream is a separate
// stream bound to the same device (see ::OpenAudioStreams), so effects keep
// playing at full level, which is what audio.hpp promises.
void audio::TrackVolume(const u8 percent) {
    music_volume = percent > 100 ? 100 : percent;
    if (music_stream) {
        SDL_SetAudioStreamGain(music_stream, static_cast<float>(music_volume) / 100.0f);
    }
}