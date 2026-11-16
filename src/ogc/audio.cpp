/**
 * @file audio.cpp
 * @brief Music playback for the native GameCube + Wii backend (libogc/ASND).
 *
 * Real Nintendo DSP-ADPCM decode is NOT reachable from this toolchain: ASND
 * and AESND (libogc's two audio APIs) only expose raw-PCM voice formats, and
 * the ADPCM decode microcode Nintendo's own SDK used was never released for
 * homebrew -- devkitPro's dsp.h is just a raw DSP-task loader with no ADPCM
 * task to hand it. So this follows the same shape as the Wii U backend
 * (src/wiiu/audio.cpp): the desktop pc_audio.wav is transcoded to a small
 * Vorbis OGG at build time and #embedded into the binary (see the gc/wii
 * branch of CMakeLists.txt), decoded here with stb_vorbis.
 *
 * Unlike Wii U (which streams via an SDL2 callback on its own audio thread),
 * libogc has no SDL audio device, so playback is polled from audio::Update(),
 * which the engine already calls once per video frame. ASND supports exactly
 * two buffers per voice -- one playing, one queued -- so Update() checks
 * ASND_TestVoiceBufferReady() and decodes the next chunk into whichever of
 * the two ping-ponged buffers isn't currently playing. Buffers are sized to
 * ~0.37s of audio, far more than one frame needs, so a missed Update() call
 * or two (e.g. a GC build's occasional per-frame hitch) doesn't starve
 * playback. GC/Wii RAM (24MB/64MB) also can't hold the whole 47MB source
 * decoded to raw PCM the way Wii U's approach effectively can, which is the
 * other reason this streams instead of decoding once at Init like Wii U does.
 *
 * Full pre-rendered streaming is also the only workflow the DSP-ADPCM
 * question upstream of this landed on: there's no per-note ADPCM sampler
 * here, just a compressed audio stream played back start-to-loop-point.
 *
 * SOUND EFFECTS ARE EMBEDDED TOO, as raw PCM rather than as Vorbis, and are
 * played straight out of .rodata: 48kHz stereo s16 big-endian IS an ASND voice
 * format (VOICE_STEREO_16BIT), so the embedded bytes are the voice buffer --
 * nothing is decoded, nothing is allocated, and the whole table costs ~318KB
 * of .dol for this project (a 0.4s effect is not the 47MB the music is).
 *
 * THE LIST IS GENERATED, BECAUSE THE TABLE IS THE APPLICATION'S. #embed needs
 * a literal path at THIS file's compile time, while the effects come from
 * ::audio::SetSfx at runtime -- so the gc/wii branch of CMakeLists.txt writes
 * sfx_embed.inc from the project's own effect directory, one #embed and one
 * name per file, and ::load_sfx matches those names against ::sfxs. Names and
 * not positions: a glob reordering must not be able to swap two effects.
 *
 * AN SD-CARD ROUTE EXISTS AND IS SWITCHED OFF. The effects can also be loaded
 * from an `assets/` folder on an SD Gecko or SD2SP2 card, which is what this
 * backend did first; that code is still below, behind an `#if 0`, because the
 * reason it lost is not that it was wrong. DOLPHIN CANNOT EMULATE EITHER
 * DEVICE: its GameCube EXI slots offer no SD card at all (the "Gecko" in its
 * device list is the USB Gecko, a serial debug cable, which is a different
 * thing wearing the same name), so a GameCube build loading from a card can
 * never make a sound in the emulator -- and that is where this gets tested.
 * Embedding also costs a GameCube nothing it was not already paying, since the
 * music is in the .dol regardless. The write-up, including the libfat/argv
 * findings and what to do when returning to it, is in
 * .ignore/ogc-sfx-sd-gecko/NOTES.md.
 */
#include "internal.hpp"
#include <pnes/audio.hpp>

#include <asndlib.h>
#include <gctypes.h>
#include <ogc/cache.h>
#include <cstdio>
#include <cstring>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

// The Vorbis-encoded music, baked into the .dol at build time.
static const unsigned char ogg_data[] = {
#embed <pc_audio.ogg>   // resolved via --embed-dir (set in the gc/wii CMake branch)
};
static constexpr int ogg_len = static_cast<int>(sizeof(ogg_data));

// audio.hpp declares these for the SDL3 mixer; this backend streams straight
// into ASND voice buffers instead, but define them so the externs resolve.
float *audio::pcm_buffer = nullptr;
u32 audio::pcm_buffer_size = 0;

namespace {

constexpr s32 VOICE          = 0;  // ASND reserves voice 0, by convention, for a music/OGG player
constexpr int CHANNELS       = 2;  // pc_audio.wav is stereo; this backend doesn't need to generalize past that
constexpr int BUFFER_FRAMES  = 4096;   // ~93ms @ 44.1kHz
constexpr int BUFFER_SHORTS  = BUFFER_FRAMES * CHANNELS;

// ASND voice buffers must be 32-byte aligned and padded to 32 bytes; both
// halves land on a 32-byte boundary since BUFFER_SHORTS*sizeof(i16) is
// itself a multiple of 32.
alignas(32) i16 buf[2][BUFFER_SHORTS];

stb_vorbis *vorbis     = nullptr;
int         src_rate   = 44100;
u32         loop_frame = 0;      // stb_vorbis_seek() target on EOF
int         next_buf   = 0;      // which half of buf[] Update() fills next
bool        playing    = false;
bool        paused     = false;
bool        primed     = false;  // buf[0] was decoded ahead in Init(), not TrackPlay()
// audio::TrackVolume's setting, as a percentage of full output. ASND's own
// scale is 0-MAX_VOLUME (255, asndlib.h) per channel; this is kept as the
// percentage so TrackPlay's ASND_SetVoice and a later ChangeVolumeVoice both
// derive from one number, and so a volume set before Init() still takes.
u8          volume     = 100;
bool        pending    = false;  // buf[next_buf] is decoded and waiting on ASND_AddVoice()

// Fills buf[which] with BUFFER_FRAMES stereo frames, looping back to
// loop_frame on EOF (mid-buffer, if the loop point falls inside this chunk).
// stall_guard bails out on a degenerate/empty stream instead of spinning.
void fill(const int which) {
    i16 *dst = buf[which];
    int got = 0, stalls = 0;
    while (got < BUFFER_FRAMES && stalls < 4) {
        const int n = stb_vorbis_get_samples_short_interleaved(
            vorbis, CHANNELS, dst + got * CHANNELS, (BUFFER_FRAMES - got) * CHANNELS);
        if (n == 0) {
            stb_vorbis_seek(vorbis, loop_frame);
            stalls++;
            continue;
        }
        got += n;
        stalls = 0;
    }
    if (got < BUFFER_FRAMES) {
        memset(dst + got * CHANNELS, 0, static_cast<size_t>(BUFFER_FRAMES - got) * CHANNELS * sizeof(i16));
    }
}

// --- Sound effects -------------------------------------------------------

// Voice 0 is the music (::VOICE), so the effects take the slots above it: one
// ASND voice per audio::sfxChannels slot, which is what lets ::TrackVolume
// scale the song without touching them (asndlib.h scales per voice).
constexpr s32 SFX_VOICE_BASE = VOICE + 1;
constexpr int N_SFX_VOICES   = audio::sfxChannels;
static_assert(SFX_VOICE_BASE + N_SFX_VOICES <= MAX_SND_VOICES,
              "audio::sfxChannels asks for more ASND voices than libogc has "
              "(MAX_SND_VOICES, asndlib.h) once the music has taken voice 0");

// The rate the build encodes assets/sfx/*.raw at. ASND takes a voice's rate as
// its `pitch` argument and resamples per voice, so this does NOT have to match
// the music's (::src_rate, whatever the OGG happens to be) -- but fixing it at
// the DSP's own 48kHz means there is no resampling to do.
constexpr s32 SFX_RATE = 48000;

// Where an effect's PCM lives, once loaded. A zero `len` is the ordinary
// "there was no file" state, not an error: see the file-header note.
struct sfx_runtime_t { u8 *data; u32 len; };
// 256 entries because ::nSfx is a u8, so that is every table an application
// can register -- 2KB of BSS, and no allocation to get wrong. Same shape as
// src/wiiu/audio.cpp and src/switch/audio.cpp.
sfx_runtime_t sfx_info[256] = {};

// One embedded effect: the name the build gave it, and the bytes themselves.
// `data` is .rodata, not an allocation -- see ::load_sfx on why that is safe to
// hand ASND directly.
struct sfx_embed_t { const char *name; const unsigned char *data; u32 len; };

// The generated table: one `alignas(32) sfx_blob_N[]` per effect plus
// ::sfx_embeds listing them. Written by the gc/wii branch of CMakeLists.txt
// into the same directory the music's --embed-dir points at, which is also on
// this file's include path (and nowhere else's).
#include <sfx_embed.inc>

// Whether @p fp names @p name, comparing the FILENAME WITHOUT ITS EXTENSION:
// an application's table says "sfx/Toggle.ogg" (its desktop path, one table
// for every platform) while the build names the embedded blob "Toggle".
//
// MATCHED BY NAME AND NOT BY POSITION, deliberately. The embed order is
// CMake's glob order and ::sfxs' order is the application's; they agree today
// and nothing enforces that they keep agreeing, so a positional join would
// quietly swap two effects the day a file is renamed. A name join either finds
// the effect or does not.
bool names_match(const char *const fp, const char *const name) {
    const char *slash = strrchr(fp, '/');
    const char *const base = slash ? slash + 1 : fp;
    const char *const dot = strrchr(base, '.');
    const size_t len = dot ? static_cast<size_t>(dot - base) : strlen(base);
    return strlen(name) == len && strncmp(base, name, len) == 0;
}

// Points every registered effect at its embedded bytes. Called once, from Init.
void load_sfx() {
    for (u8 i = 0; i < nSfx; i++) {
        for (const sfx_embed_t &e : sfx_embeds) {
            if (!names_match(sfxs[i].fp, e.name)) continue;

            // ROUNDED DOWN to a whole 32 bytes, which is what lets ASND read
            // this array in place. asndlib.h requires a voice buffer to be
            // 32-byte aligned (the generated arrays are) and padded to 32
            // bytes -- so rather than pad the asset and have the build write
            // bytes nobody asked for, the LENGTH handed to the voice is
            // truncated to a multiple of 32 and the DSP never reads past the
            // array. The loss is under 8 stereo frames, 0.16ms, inaudible.
            const u32 len = e.len & ~31u;
            if (len == 0) break;

            // The DSP reads this by DMA, so the CPU's cached copy of it has to
            // be in memory first. .rodata is written once when the loader
            // copies the .dol in, which is exactly the case this guards: the
            // bytes may still be sitting in cache having never been flushed.
            // Once here, at load -- nothing writes them again. The const cast
            // is a cache operation on memory this code does not modify.
            DCFlushRange(const_cast<unsigned char *>(e.data), len);

            sfx_info[i] = { const_cast<unsigned char *>(e.data), len };
            break;
        }
    }
}

#if 0
// ===========================================================================
// DISABLED: loading the effects from an `assets/` folder on an SD Gecko /
// SD2SP2 card instead of embedding them. Kept because it works and is wanted
// eventually -- see this file's header for why it is off (Dolphin emulates no
// GameCube SD device of any kind) and .ignore/ogc-sfx-sd-gecko/NOTES.md for
// the findings behind it and what re-enabling involves.
//
// To bring it back: restore `#include <fat.h>`, `#include <dirent.h>` and
// `#include <malloc.h>` at the top of this file, add `fat` back to the
// target_link_libraries line in the gc/wii CMake branch, point the build's
// raw-PCM output at an assets/sfx/ folder beside the .dol again, and call
// fatInitDefault() before this ::load_sfx in audio::Init.
// ===========================================================================

// "sfx/Toggle.ogg" -> "<root>assets/sfx/Toggle.raw".
//
// THE TABLE'S PATHS ARE THE DESKTOP'S, deliberately: an application declares
// one ::sfxs table for every target (demo/ui/impl/sounds.hpp), so each backend
// rewrites those paths into its own storage instead of the table carrying a
// per-platform spelling. The Switch does the same thing (::romfs_raw_path) and
// so does Wii U; the `assets/` prefix is this backend's, and the extension
// swap is because what ships here is pre-decoded PCM, not the source OGG.
void sfx_path(const char *const root, const char *const fp, char *const out, const size_t cap) {
    snprintf(out, cap, "%sassets/%s", root, fp);
    char *dot = strrchr(out, '.');
    if (dot && (cap - static_cast<size_t>(dot - out)) >= 5) memcpy(dot, ".raw", 5);  // incl. NUL
}

// Whether `<base>assets/<fp>` is openable, which is how a candidate base is
// tested: the file either opens or it does not, and no stat/d_type reading is
// needed to tell a directory from anything else along the way.
bool sfx_exists(const char *const base, const char *const fp) {
    char path[384];
    sfx_path(base, fp, path, sizeof(path));
    FILE *const f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// The directory the executable was launched from, as a base ending in '/', or
// nullptr when the loader did not say.
//
// READ HERE RATHER THAN TRUSTED TO libfat, and that is the whole reason this
// function exists. fatInitDefault() does set the working directory to this
// same place -- but only when the loader filled in the argv struct: dvmInit
// checks __system_argv->argvMagic against ARGV_MAGIC ('_arg', gctypes.h) and
// skips _dvmSetAppWorkingDir entirely when it does not match. The Homebrew
// Channel fills it in; GameCube loaders generally do not, so on GC a relative
// path commonly resolves against a working directory nobody ever set. Reading
// argv directly costs nothing and makes this independent of that.
const char *app_dir() {
    static char dir[320];
    const struct __argv *const a = __system_argv;
    if (!a || a->argvMagic != ARGV_MAGIC || a->argc < 1 || !a->argv || !a->argv[0]) return nullptr;

    snprintf(dir, sizeof(dir), "%s", a->argv[0]);
    char *const slash = strrchr(dir, '/');
    if (!slash) return nullptr;     // a bare filename names no directory
    slash[1] = '\0';                // keep the '/': bases are concatenated as-is
    return dir;
}

// Looks one level down from @p root for a directory holding the effects, and
// writes the base that does into @p base.
//
// THE STANDARD LAYOUT IS A SUBDIRECTORY, not the card root: homebrew ships as
// apps/<name>/{boot.dol,meta.xml,icon.png} on Wii, and a folder per program is
// how people organise a GameCube card too -- so assets/ sits beside the
// executable, one level down, under a name only the person who copied it
// knows. With no argv to name it (see ::app_dir) the only honest way to find it
// is to look, and looking is cheap: this runs once, at Init, and stops at the
// first hit. Non-directories and dot entries cost one failed fopen each.
bool scan_for_sfx(const char *const root, const char *const fp, char *const base, const size_t cap) {
    DIR *const d = opendir(root);
    if (!d) return false;
    bool found = false;
    while (const dirent *const e = readdir(d)) {
        if (e->d_name[0] == '.') continue;          // ".", "..", and dotfiles
        char cand[320];
        snprintf(cand, sizeof(cand), "%s%s/", root, e->d_name);
        if (sfx_exists(cand, fp)) {
            snprintf(base, cap, "%s", cand);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

// Finds the one base every effect is then loaded from, using @p fp (the first
// registered effect) as the probe. Empty string = relative to the working
// directory; nullptr = the effects are not on any mounted device.
//
// RESOLVED ONCE, NOT PER EFFECT, which is the point of returning a base rather
// than opening files here: the search can touch every directory on a card, and
// doing that five times over -- or once per effect in whatever table an
// application registers -- would put a card scan on the boot path for no
// reason. Every effect lives beside the first one.
const char *resolve_sfx_base(const char *const fp) {
    static char base[320];

    // The launch directory first, because when the loader did pass argv it is
    // the right answer and needs no searching.
    if (const char *const dir = app_dir(); dir && sfx_exists(dir, fp)) {
        snprintf(base, sizeof(base), "%s", dir);
        return base;
    }

    // Then relative to the working directory, which covers a loader that set
    // one without argv, and a host that runs this from the right place.
    if (sfx_exists("", fp)) { base[0] = '\0'; return base; }

    // Then the device roots themselves, for a card with assets/ at the top.
    // These are the mount names libfat's own prober uses: "sd" (Wii SD card,
    // and SD2SP2 on GameCube), "carda"/"cardb" (SD Gecko in either memory-card
    // slot, both consoles) and "usb" (Wii USB storage).
    static const char *const kRoots[] = { "sd:/", "carda:/", "cardb:/", "usb:/" };
    for (const char *const root : kRoots) {
        if (sfx_exists(root, fp)) {
            snprintf(base, sizeof(base), "%s", root);
            return base;
        }
    }

    // Then one level down from each root, and one level down from each root's
    // apps/ -- i.e. sd:/<anything>/assets/ and sd:/apps/<anything>/assets/,
    // which is where a folder copied beside the .dol actually ends up.
    for (const char *const root : kRoots) {
        if (scan_for_sfx(root, fp, base, sizeof(base))) return base;
        char apps[320];
        snprintf(apps, sizeof(apps), "%sapps/", root);
        if (scan_for_sfx(apps, fp, base, sizeof(base))) return base;
    }

    return nullptr;
}

// Loads every registered effect into its own buffer. Called once, from Init.
void load_sfx() {
    const char *const base = resolve_sfx_base(sfxs[0].fp);
    if (!base) return;                          // not on any mounted device

    char path[384];
    for (u8 i = 0; i < nSfx; i++) {
        sfx_path(base, sfxs[i].fp, path, sizeof(path));
        FILE *const f = fopen(path, "rb");
        if (!f) continue;                       // no such effect under the base

        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }

        // ASND_SetVoice's buffer "must be aligned and padded to 32 bytes"
        // (asndlib.h), so round the allocation up and zero the tail -- the pad
        // then plays as the silence it is, which is also why the padded length
        // is what gets handed to the voice rather than the file's own. memalign
        // and not new: 32-byte alignment is the DSP's requirement, not a hint.
        const u32 len = (static_cast<u32>(sz) + 31u) & ~31u;
        u8 *const data = static_cast<u8 *>(memalign(32, len));
        if (!data) { fclose(f); continue; }

        const bool ok = fread(data, 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz);
        fclose(f);
        if (!ok) { free(data); continue; }
        memset(data + sz, 0, len - static_cast<u32>(sz));

        // The DSP reads this buffer by DMA, so the CPU's cached copy has to be
        // in memory before any voice points at it. Once here, at load, rather
        // than per play: the buffer is never written again.
        DCFlushRange(data, len);

        sfx_info[i] = { data, len };
    }
}
#endif  // the SD-card route


} // namespace

void audio::Init(u8) {
    ASND_Init();
    ASND_Pause(0);

    int err = 0;
    vorbis = stb_vorbis_open_memory(ogg_data, ogg_len, &err, nullptr);
    if (!vorbis) return;
    src_rate = stb_vorbis_get_info(vorbis).sample_rate;

    // Decode the first buffer here, ahead of TrackPlay(), so that call site's
    // caller (level.cpp calls audio::Init() immediately followed by
    // audio::TrackPlay(0), right as a level starts) doesn't pay the decode
    // cost synchronously at the moment music is meant to start.
    if (nTracks > 0) {
        loop_frame = tracks[0].loop_start > 0.0f
            ? static_cast<u32>(tracks[0].loop_start * static_cast<float>(src_rate))
            : 0;
        fill(0);
        primed = true;
    }

    // AFTER the music is primed, because mounting storage is the slow part of
    // this function and the song is what the caller is waiting on (level.cpp
    // calls TrackPlay the moment this returns). Only when there is something to
    // load: an application that registers no effects should not pay an SD card
    // mount, and fatInitDefault() is the only thing here that touches hardware
    // the music path never needs.
    //
    // NOT GUARDED AGAINST A SECOND CALL, because this backend is not the only
    // thing that may have mounted storage -- an application is free to have
    // called fatInitDefault() itself before Init, and libfat tolerates the
    // repeat. What it is NOT free to do is unmount it afterwards: the effects
    // are read here, once, and nothing reopens them.
    // No device to open and nothing to read: the effects are in the
    // executable, so this is a table walk (::load_sfx) rather than the card
    // mount the disabled route needed. An application that registers no
    // effects still skips it.
    if (nSfx > 0 && sfxs) load_sfx();
}

void audio::Update() {
    if (!vorbis || !playing || paused) return;

    // Decode the next chunk only once there's nowhere else it needs to wait:
    // ASND has exactly one "next" slot, so decoding ahead of confirming that
    // slot is actually free would mean, if ASND_AddVoice() below then fails
    // with SND_BUSY, that chunk gets silently thrown away -- the Vorbis
    // decoder's read position had already moved past it -- and playback
    // skips ahead by however much was lost. That's what produced the
    // startup glitch-and-jump: decode and "confirmed queued" must stay in
    // lockstep, so pending only clears on a successful add, never on the
    // decode itself.
    if (!pending) {
        if (!ASND_TestVoiceBufferReady(VOICE)) return;
        next_buf ^= 1;
        fill(next_buf);
        pending = true;
    }

    if (ASND_AddVoice(VOICE, buf[next_buf], BUFFER_SHORTS * sizeof(i16)) == SND_OK) {
        pending = false;
    }
    // else: voice slot wasn't actually free yet; retry the same already-
    // decoded buf[next_buf] next Update() call instead of decoding further.
}

void audio::TrackPlay(const u8 index) {
    if (!vorbis || index >= nTracks) return;

    ASND_StopVoice(VOICE);

    // Init() already decoded buf[0] for track 0 ahead of time (see its own
    // comment); reuse that instead of re-seeking/re-decoding synchronously
    // here, which is what produced an audible stutter right as playback
    // started. Any other index (or a replay after the primed buffer's
    // already been consumed) still pays the decode cost inline.
    if (!(index == 0 && primed)) {
        stb_vorbis_seek_start(vorbis);
        // tracks[index].loop_start is in seconds (0 = loop the whole track),
        // matching the convention src/wiiu/audio.cpp's load_track() already uses.
        loop_frame = tracks[index].loop_start > 0.0f
            ? static_cast<u32>(tracks[index].loop_start * static_cast<float>(src_rate))
            : 0;
        fill(0);
    }
    primed  = false;
    pending = false;

    next_buf = 0;
    const s32 vol = static_cast<s32>(volume) * MAX_VOLUME / 100;
    ASND_SetVoice(VOICE, VOICE_STEREO_16BIT, src_rate, 0,
                  buf[0], BUFFER_SHORTS * sizeof(i16),
                  vol, vol, nullptr);
    playing = true;
    paused  = false;
}

void audio::TrackStop() {
    ASND_StopVoice(VOICE);
    playing = false;
}

void audio::TrackPause(const u8 pause) {
    (void)pause;  // match the SDL/Wii U backends: toggle regardless of argument
    paused = !paused;
    ASND_PauseVoice(VOICE, paused);
}

// ASND scales the voice in hardware, so this costs one call and nothing per
// sample. Voice 0 is the music voice by convention (::VOICE), so an SFX voice
// added later is untouched by this, as audio.hpp promises.
//
// ASND_ChangeVolumeVoice only applies to a voice that is actually set up; when
// it is not, ::volume alone is enough, because TrackPlay's ASND_SetVoice reads
// it on the way in.
void audio::TrackVolume(const u8 percent) {
    volume = percent > 100 ? 100 : percent;
    const s32 vol = static_cast<s32>(volume) * MAX_VOLUME / 100;
    if (playing) ASND_ChangeVolumeVoice(VOICE, vol, vol);
}

// One voice, one whole buffer, no callback -- which is the entire reason an
// effect needs no per-frame work here while the music needs ::Update(). With a
// NULL callback ASND_SetVoice plays the buffer it is given and then stops the
// voice (asndlib.h's note on the callback: a non-NULL one is what makes a voice
// run forever waiting to be refilled). Setting a voice that is still playing
// "stops one previous voice" per the same header, which is exactly the
// cut-off-the-old-one behaviour audio.hpp promises for a busy channel.
//
// VOICE_STEREO_16BIT IS BIG-ENDIAN (it is #defined to the same 3 as
// VOICE_STEREO_16BIT_BE, asndlib.h), which is why the build writes these files
// as s16BE -- the console's own byte order, and the format voice 0 is already
// fed by stb_vorbis's native-endian output. s16LE assets would need
// VOICE_STEREO_16BIT_LE instead; getting that pair wrong is loud static, not
// silence, so the encode step and this constant have to be read together.
//
// @p volume passes straight through: ASND's per-voice scale is 0-MAX_VOLUME
// (255, asndlib.h), so 0 is silence and 255 is unity with no conversion -- the
// same linear-amplitude reading SDL3, Switch and Wii U give audio.hpp's
// deliberately uninterpreted byte.
void audio::SfxPlay(const u8 index, const u8 channel, const u8 volume) {
    if (index >= nSfx || channel >= N_SFX_VOICES) return;
    const sfx_runtime_t &s = sfx_info[index];
    if (!s.data || s.len == 0) return;   // never loaded: no storage, or no such file
    ASND_SetVoice(SFX_VOICE_BASE + channel, VOICE_STEREO_16BIT, SFX_RATE, 0,
                  s.data, static_cast<s32>(s.len), volume, volume, nullptr);
}

// Unity, and channel 1 -- matching the Switch and Wii U backends. There is no
// separate sample bank off NES (audio.hpp's ::SfxSamplePlay), so this plays an
// ordinary effect at the level it was authored.
void audio::SfxSamplePlay(const u8 index) { audio::SfxPlay(index, 1, 0xFF); }

// Exact here, unlike the NES driver: a voice is ASND state this backend owns,
// so releasing or freezing one is a single call (see audio.hpp's ::SfxStop on
// why the NES cannot). Both ignore an out-of-range channel rather than clamping
// into the music's voice or someone else's slot.
void audio::SfxStop(const u8 channel) {
    if (channel >= N_SFX_VOICES) return;
    ASND_StopVoice(SFX_VOICE_BASE + channel);
}

void audio::SfxPause(const u8 channel, const u8 pause) {
    if (channel >= N_SFX_VOICES) return;
    ASND_PauseVoice(SFX_VOICE_BASE + channel, pause != 0);
}
