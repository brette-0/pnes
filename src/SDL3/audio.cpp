#include <pnes/audio.hpp>
#include <pnes/technology.hpp>
#include <SDL3/SDL.h>

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

// Every non-NES backend leaves ::sfxs/::nSfx undefined unless the app calls
// ::SFX(...); this weak zero-entry default keeps the link working until it
// does (mirrors the same trick used on the Switch/Wii U ports).
__attribute__((weak)) extern const audio::sfx_t sfxs[1] = {};
__attribute__((weak)) extern const u8 nSfx = 0;

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
static constexpr int N_SFX_VOICES = 4;
struct sfx_voice_t { u32 pos; u32 end; bool active; };
static sfx_voice_t sfx_voices[N_SFX_VOICES];

static u32 track_offsets[256];
static u32 track_lengths[256];

#define BYTES_PER_SECOND (48000 * 2 * sizeof(float))

// Load one track's source audio into an SDL_LoadWAV-shaped (spec,data,len)
// triple. On success *data is SDL_malloc'd and must be SDL_free'd by the
// caller, exactly like SDL_LoadWAV.
static bool LoadTrack(const char *fp, SDL_AudioSpec *spec, u8 **data, u32 *len) {
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
    if (total == 0) return;

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
}

void audio::SfxPlay(const u8 index, const u8 channel) {
    if (index >= nSfx || channel >= N_SFX_VOICES || sfx_info[index].length == 0) return;
    sfx_voices[channel] = {
        .pos    = sfx_info[index].offset,
        .end    = sfx_info[index].offset + sfx_info[index].length,
        .active = true
    };
}

void audio::SfxSamplePlay(const u8 index) {
    SfxPlay(index, 1);
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
            if (!v.active) continue;
            any_active = true;
            const u32 avail = v.end - v.pos;
            const u32 take = avail < static_cast<u32>(CHUNK_FLOATS) ? avail : static_cast<u32>(CHUNK_FLOATS);
            const float *src = sfx_pcm_buffer + v.pos;
            for (u32 i = 0; i < take; i++) mix[i] += src[i];
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

    // Bind sfx_stream to the same logical device as music_stream rather than
    // opening a second device (mixed additively by SDL, same as two real
    // devices).
    sfx_stream = SDL_CreateAudioStream(&spec, &spec);
    if (!sfx_stream) {
        SDL_Log("AUDIO: SFX stream creation failed: %s", SDL_GetError());
    } else if (!SDL_BindAudioStream(SDL_GetAudioStreamDevice(music_stream), sfx_stream)) {
        SDL_Log("AUDIO: SFX stream bind failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(sfx_stream);
        sfx_stream = nullptr;
    }
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