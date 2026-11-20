#include "pregame.hpp"
#include "../save.hpp"
#include <pnes/save.hpp>

#ifndef TARGET_NES
#include <cstring>
#endif

#include <intsh>

#include "../main.hpp"   // ::gameMode, eGameModes -- this mode names its successor
#include "../graphics/colours.hpp"
#include "../graphics/windows.hpp"
#include "../../chr/chr.h"
// The arrow: its tile, its palette, its gap, the sway table, and the
// AnimateSelector() this mode's NMI calls. ::PlaceSelector is the one thing
// here that is NOT reused -- see ::PlaceArrow.
#include "../../ui/impl/singleChoice.hpp"
#include "../../ui/impl/sounds.hpp"

#include "pnes/audio.hpp"
#include "pnes/extras/ui/text.hpp"
#include "pnes/input.hpp"
#include "pnes/interrupts.hpp"
#include "pnes/logger.hpp"
#include "pnes/mappers/mmc3.hpp"
#include "pnes/video.hpp"
#include "../../ui/impl/buttons.hpp"
using namespace br0::intsh;

/*
 * TARGET_OGC AND NOT TARGET_GC, AND THE DISTINCTION IS WHAT THIS MODE IS FOR.
 * This gate used to be the GameCube alone because the memory card was the only
 * save device the library could reach; the Wii now saves to the front SD slot
 * or a USB disk through libfat (src/ogc/save.cpp's Wii half), so everything
 * below applies to it unchanged -- a device that may be absent, two of them to
 * choose between, a file that may not exist yet, and every save::eError in the
 * switches below reachable for real.
 *
 * WHAT IS STILL GAMECUBE-ONLY IS NESTED INSIDE, not gated out here, and it is
 * two things: the card's directory-entry metadata (::banner, ::icon,
 * ::gameName, ::gameDesc and the ::PopulateSaveData that copies them, which
 * describe a card_stat nothing else has) and ::ApplyLoadedConfig's widescreen
 * restore (video::ogc::SetWidescreen is a GameCube call, and the Wii's options
 * scene has no widescreen button to restore from). Both carry their own
 * #ifdef TARGET_GC.
 *
 * STILL NOT `#ifndef TARGET_NES`. A battery cart's save IS the cart: there is
 * no device to find, nothing to choose and no file to create, and the MMC3
 * backend answers every call with save::found. A target on the weak backend in
 * src/all/save.cpp is in the opposite position -- it can only ever answer
 * save::no_device -- and a mode whose every branch ends "there is nowhere to
 * save" is not worth booting into either.
 */
#ifdef TARGET_OGC

namespace pregame {

/**
 * @brief The level this mode's effects play at: the player's own SFX setting.
 *
 * ::ui::impl::SfxLevel(), WHICH THIS USED TO REFUSE TO CALL. It was a
 * hard-coded 255, because at BOOT this mode runs before
 * `gen::options::Make()` has resolved the SFX slider and the old SfxLevel()
 * answered nonsense for an unresolved track -- so every ::SfxPlay from here
 * asked for a level that was not the player's.
 *
 * THAT WAS WRONG EVERYWHERE THIS MODE IS NOT THE BOOT MODE, and ::SaveScreen
 * is exactly that place: it is called from the options page's B press, long
 * after the scene resolved, and this mode's frame pump is what spends the
 * mailbox title.cpp posted into. So a player who wound SFX to mute, changed
 * nothing else and backed out heard the deselect effect at FULL VOLUME on the
 * way to the title screen -- the one sound the setting most obviously promises
 * to silence.
 *
 * THE BOOT CASE IS NOW ANSWERED WHERE IT BELONGS: SfxLevel() reports full
 * scale for an unresolved track and ZERO for a muted one, so this mode gets
 * the old behaviour at boot, honours mute even then, and honours the real
 * slider the moment there is one. See its definition in title.cpp.
 */
static u8 SfxLevel() { return ui::impl::SfxLevel(); }

/**
 * @brief One frame of this mode: audio, the sound-effect mailbox, present.
 *
 * THE MAILBOX IS SPENT HERE, which is the arrangement title mode has for the
 * same reason (::ui::impl::pendingSfx, and ::DriveAudio in title.cpp): a hook
 * that knows a sound is wanted stores a byte, and the mainline plays it. No
 * mmc3::CallPairedBlock around it, unlike title's -- this mode exists only on
 * the GameCube, where the audio engine is not a banked blob that has to be
 * mapped over $8000 before it can be called.
 */
static void WaitBody() {
    audio::Update();
    if (const u8 queued = ui::impl::pendingSfx) {
        // Drained whether or not it plays, exactly as title.cpp does it: a
        // posted effect describes something that has already happened on
        // screen, and held back while effects are off it would fire the moment
        // they were switched on.
        ui::impl::pendingSfx = 0;
        // THE LEVEL IS READ AT THE PLAY, not latched when the effect was
        // posted, which is what makes a mute set on the options page apply to
        // the very gesture that left it (::SaveScreen's caller posts the
        // deselect and this spends it).
        if (const u8 level = SfxLevel(); ui::impl::SfxEnabled() && level) {
            audio::SfxPlay(static_cast<u8>(queued - 1), ui::impl::kSfxChannel, level);
        }
    }
    video::WaitForPresent();
}

static bool LoadSave();
static bool ValidateSave();
static bool TryCreateSave();
static void PrintContext(const u8* infoText);
static eOptions ChoiceWheel(const eOptions* pOptions, u8 sOptions);
static void PopulateSaveData();
static void SeedConfigDefaults();
static void MakeScene();
static void ApplyLoadedConfig();
static void InitPregameScreen();
static void ClearOptions();
static void LoadConfig();
void nmi_handler();
void irq_handler();

// ---------------------------------------------------------------------------
// The scene.
//
// HAND-ROLLED RATHER THAN A uitk SCENE, and the difference is smaller than it
// looks: an rtTextBox generates exactly two things -- the geometry below
// (`rect`/`pos` filled by a Make()) and a MakeText() wrapper that is nothing
// but `ui::text::Make<charmap, splitter>(buff, box)`, which is what
// ::PrintContext and ::DrawOptions call directly. Adding demo/ui/pregame.uis
// would move these three rects into the editor and change nothing else; it
// needs a Qt-equipped configure to export, which is why the layout is written
// here for now.
//
// EVERY RECT IS RESOLVED AT RUNTIME, not constexpr, because this target's
// viewport is: video::viewport_tx() on the GameCube returns ::ogc_world_tx,
// which widescreen moves (video.hpp). That is the same reason uitk emits
// `atomic Rect` plus a Make() on runtime-viewport targets instead of baking
// the numbers.
// ---------------------------------------------------------------------------

/// Rows the context message may wrap to, and the width of an option's box.
constexpr u8 kContextRows = 3;
constexpr u8 kOptionCols  = 10;

/// What this scene blanks with: the tile ' ' encodes to, so a Draw and a
/// Clear through the same charmap round-trip exactly.
constexpr u8 kBlankTile = charmap_generic(' ');

/// The wheel's two anchors are all the layout has, so a longer option list
/// would have nowhere to put its third label.
constexpr u8 kMaxOptions = 2;

atomic Rect contextRect;
atomic Rect optionRect[kMaxOptions];

/// The rows ::ui::text::Make built for what is currently on screen, kept so it
/// can be Clear()ed -- Clear needs the same wrapping Draw used, and only the
/// row table it was given holds that.
ui::text::textBuffer* contextRows            = nullptr;
ui::text::textBuffer* optionRows[kMaxOptions] = {};

constexpr eOptions createOpts[] = { Yes, No };
constexpr eOptions errorOpts[] = {Retry, Continue};
constexpr eOptions eraseOpts[] = {Continue, Erase};
constexpr eOptions cardOpts[]  = { Card1, Card2 };

void main() {
    log("pregame::main: entered");
    InitPregameScreen();

    PopulateSaveData();
    SeedConfigDefaults();
    save::Response response = { };

    retry:
    while (response = save::GetSaveLocation(), !response.done) {
        WaitBody();
    }

    // Both currencies, once per attempt: the portable code the switch below
    // acts on, and the CARD_ERROR_* behind it. The library folds four very
    // different situations into save::no_device -- empty slot, a non-card
    // device in the slot, a broken directory, a foreign font encoding -- and
    // this is the only place that difference is visible.
    log("pregame: GetSaveLocation err=%d native=%d", response.err, save::LastNativeError());

    switch (response.err) {
       case save::found: {
            // NOT GATED TO THE GAMECUBE ANY MORE: both consoles answer with a
            // path, and both borrow it. save.hpp says `path` is valid only
            // until the next call, so the copy is the point -- "a:name" on the
            // GameCube, "sd:/dir/save.dat" on the Wii, and the Wii's is the
            // longer of the two, which is why the length comes from strlen
            // rather than from a constant.
            const std::size_t sPath = strlen(response.path);
            if (!savePath) {    // we don't need to free, as it will be free on next load, I think
                savePath = new char[sPath + 1];   // + the terminator strcpy writes
                strcpy(savePath, response.path);
            }
            LoadSave();  // proceed to attempt to load
            break;
        }
        case save::missing:
            PrintContext(saveMissingText.data());
            switch (ChoiceWheel(createOpts, sizeof(createOpts))) {
                case Yes:
                    if (!TryCreateSave()) goto retry;
                    break;

                case No:
                    break;

                default:
                    log("unknown choice selected");
            }

            break;
        case save::no_device:
            PrintContext(saveNoDeviceText.data());
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }

            break;
        case save::busy:
            PrintContext(saveBadErrorText.data());
            // card is reported busy, this should NOT fire under healthy circumstances
            // should show some message, that the card is busy with 'retry' or 'continue without save'
            log("card is busy, this is a serious bug that should be solved");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;
        case save::exists:
            PrintContext(saveBadErrorText.data());
            // should only appear on save creation, not save searching, erroneous
            // should show some message, that the card is erroring with 'retry' or 'continue without save'
            log("cannot find file because it already exists, bad error");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;

        case save::full:
            PrintContext(saveBadErrorText.data());
            // should not fire on try find, if this is being seen its a big error
            // report that an unusual bug is shown, and should be reported immediately
            log("cannot find file because out of storage, bad error");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;
        case save::too_large:
            PrintContext(saveBadErrorText.data());
            // should not fire on load, if this is being seen its a big error
            // report that an unusual bug is shown, and should be reported immediately
            log("cannot find file because it might be too big to be loaded, bad error");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;
        case save::denied:
            PrintContext(saveBadErrorText.data());
            // report operation has been denied, should not occur under typical circumstances
            // report the operation is denied, and should be reported immediately
            log("cannot find file, memory card denied operation");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;
        case save::bad_path:
            PrintContext(saveBadErrorText.data());
            // report operation has bad path, not a user error
            // report the operation is denied, and should be reported immediately
            log("cannot find file, path is bad, bad error");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;
        case save::failed:
            PrintContext(saveBadErrorText.data());
            // unknown error, just let the user retry or continue without save
            log("unknown error, bad error normally");
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    goto retry; // re attempt

                case Continue:
                    break;

                default:
                    log("unknown choice selected");
            }
            break;
    }

    // THE HAND-OFF, AND IT IS UNCONDITIONAL. However the save question came
    // out -- loaded, created, or declined because there is nowhere to save --
    // the player is owed the title screen next, so there is no outcome here
    // that leaves ::gameMode alone and strands the dispatch loop re-entering
    // this mode. main.cpp picks it up as soon as this returns.
    // What the loaded settings mean for the HARDWARE, applied before the title
    // screen draws its first tile. See ::ApplyLoadedConfig.
    ApplyLoadedConfig();

    log("pregame::main: handing over to the title screen");
    gameMode = eGameModes::Title;
}

/**
 * @brief Puts a loaded save's settings into force, for the ones that are a
 *        property of the DISPLAY rather than of the options page.
 *
 * ONLY WIDESCREEN, AND THAT IS THE WHOLE LIST TODAY. The options scene has
 * four controls on the GameCube and they divide cleanly:
 *
 *   - enableWidescreenButton -- changes ::video::viewport_tx(), so every rect
 *     in every scene, every nametable address and the camera itself are
 *     derived from it. It has to be settled before anything is Make()d or
 *     drawn, which is why it is HERE and not in title mode: this is the last
 *     moment at which nothing is on screen to be invalidated.
 *   - musicSlider -- is applied by title mode, because the value it pushes
 *     into ::audio::TrackVolume is a PROPORTION of the slider's travel, and
 *     the travel comes from the rect, which is not resolved until
 *     ::gen::options::Make() has run.
 *   - enableStereo, reduceFlashesButton -- have no engine side at all yet.
 *     Nothing outside their own Draw() reads either bit, so there is nothing
 *     for this function to apply. They are named here rather than left out so
 *     that wiring one up has an obvious home.
 *
 * ::video::ogc::SetWidescreen's @warning says the caller owns rebuilding the
 * screen around it. That is satisfied by WHERE this is called from rather than
 * by anything it does: ::main is finished with its own scene, and
 * ::title::main's next act is a ::ppu::Flush and a full set of Make()s against
 * whatever the viewport is by then. Calling this any earlier -- right after
 * the load, say -- would re-stride the plane under this mode's own context
 * band and error wheels, which is exactly the trap ::title::ApplyWidescreenChange
 * exists to handle.
 *
 * NO "WAS THERE A SAVE" TEST, and none is needed: by the time this runs
 * ::saveData.config is the state whatever its provenance -- a loaded file, or
 * the defaults ::SeedConfigDefaults wrote before the load went looking. On a
 * cold boot the widescreen bit is zero, which reads as "4:3", which is exactly
 * what a default-off button means, so the no-save case needs no branch of its
 * own.
 */
void ApplyLoadedConfig() {
#ifdef TARGET_GC
    const bool wide = gen::options::videoCanvas::enableWidescreenButton::Enabled();
    log("pregame: restoring widescreen=%d from save", wide);
    if (wide) video::ogc::SetWidescreen(true);
#else
    // NOTHING TO APPLY ON THE WII, AND NOT BECAUSE THIS IS UNFINISHED. The list
    // above is one item long and that item is the GameCube's: the Wii's aspect
    // is a SYSTEM setting the console is configured with rather than something
    // a game switches (video::ogc::SetWidescreen is GameCube-only, and
    // pnes/video.hpp gives every other target the inline no-op), so
    // demo/ui/options.uis has no widescreen button in the Wii scene at all --
    // gen/options/wii/options.hpp has no `enableWidescreenButton` to read.
    //
    // THE CALL STAYS UNCONDITIONAL at the one site that makes it, because the
    // reasoning above is about WHICH settings have an engine side, not about
    // whether a loaded save has settings. The sliders are applied by title mode
    // on both consoles for the reason this function's comment gives, and a
    // setting that grows an engine side on the Wii has an obvious home.
    log("pregame: no display settings to restore on this target");
#endif
}

/**
 * @brief The plane TILE this scene is anchored at: the BOTTOM RIGHT nametable.
 *
 * A quadrant offset, the hand-written form of the one uitk bakes into a
 * generated scene (emitExprCpp's ntOffX/ntOffY, tools/uitk/main.cpp) -- this
 * mode has no .uis to carry it, so it is written out here and every rect
 * ::MakeScene resolves is placed through it.
 *
 * X is one page stride in. That stride is ::emu::ComputeNtGeometry's own
 * tileW -- the NES-native 32 floored against this target's viewport and then
 * ROUNDED UP to a whole attribute byte -- because it is what
 * ::ppu::CartesianToAddress divides by to decide which page a coordinate lands
 * on (xy_to_nt_addr, src/emu/ppu.cpp). The GameCube's two widths are both
 * already multiples of four (40 at 4:3, 52 at 16:9), so the rounding changes
 * nothing today; it is here so that an x of 50 on a 50-wide viewport could not
 * quietly resolve into page 0's last two columns, which is the trap
 * title.cpp's ::kMenuNT names and dodges by only ever running where the width
 * is 32.
 *
 * Y is one page height down, the 30-row floor against ::video::viewport_ty().
 * With vertical mirroring -- ::mmc3::SetMirroring(false) in
 * ::InitPregameScreen -- the plane is two pages WIDE and one page TALL
 * (::video::nametable_grid_w()/::nametable_grid_h(), video.hpp), so that row
 * aliases back onto the right-hand page exactly as a real board's wiring would
 * alias $2C00 onto $2400. Writing it out rather than dropping it is the point:
 * it is what makes this scene the BOTTOM right quadrant rather than the top
 * one, in the same vocabulary demo/ui/*.uis and ::ppu::SetScroll's own
 * `y >= 240` fold use, and the camera below is pointed at the same address.
 */
static vec2<u16> SceneOriginTiles() {
    const u16 tw = video::viewport_tx() < 32 ? 32 : video::viewport_tx();
    const u16 th = video::viewport_ty() < 30 ? 30 : video::viewport_ty();
    return {static_cast<u16>((tw + 3) & ~3u), th};
}

/** @brief ::SceneOriginTiles in pixels: the scroll this mode parks the camera at. */
static vec2<u16> SceneOriginPixels() {
    const vec2<u16> o = SceneOriginTiles();
    return {static_cast<u16>(o.x << 3), static_cast<u16>(o.y << 3)};
}

/**
 * @brief Resolves the three boxes against the viewport this run has.
 *
 * What a generated scene's Make() does, written out. The context band sits
 * four tiles in from each edge and straddles the middle row; the two options
 * are ten-tile boxes CENTRED on the quarter and three-quarter points, hence
 * the half-width taken off each anchor. Three quarters is a half plus a
 * quarter so the arithmetic needs no multiply -- the same habit the .uis
 * expressions in this project keep.
 *
 * Every anchor is then offset by ::SceneOriginTiles, so all three rects are in
 * the bottom-right quadrant's own rows and columns. The layout arithmetic
 * stays written against the bare viewport -- "four tiles in", "the quarter
 * point" -- because that is what it means; the quadrant is added once, at the
 * end, exactly as a generated scene adds it.
 */
void MakeScene() {
    const u16 tx = video::viewport_tx();
    const u16 ty = video::viewport_ty();
    const vec2<u16> origin = SceneOriginTiles();

    contextRect = Rect{{static_cast<u16>(origin.x + 4),
                        static_cast<u16>(origin.y + (ty >> 1) - 2)},
                       {static_cast<u8>(tx - 8), kContextRows}};

    const u16 optionY = static_cast<u16>(origin.y + (ty >> 1) + 2);
    optionRect[0] = Rect{{static_cast<u16>(origin.x + (tx >> 2) - (kOptionCols >> 1)), optionY},
                         {kOptionCols, 1}};
    optionRect[1] = Rect{{static_cast<u16>(origin.x + (tx >> 1) + (tx >> 2) - (kOptionCols >> 1)), optionY},
                         {kOptionCols, 1}};
}

/** @brief Parks every sprite; the provider ::oam::PopulateFromProvider wants. */
static oam::oam_t ParkY(const u16) { return ParkedSpriteY(); }

/**
 * @brief Banks this mode's art in, points the vectors at it and starts the frame.
 *
 * ONE SET OF ART, BANKED TWICE. This mode draws text and one sprite, both out
 * of the `ui` set: R3 (PPU $0400) holds it for the background, which is what
 * every tile `charmap_generic` produces is an index into (windows::ui), and R1
 * (PPU $1800) holds the same bytes again for sprites, which is what the
 * arrow's tile is an index into (windows::uiSprites). Identical to title
 * mode's two switches, minus the level art title also needs for its preview.
 *
 * NO IRQ OF ITS OWN, AND THE PENDING ONE IS CLEARED. The only IRQ source this
 * project arms is MMC3's scanline counter, for title and level mode's raster
 * splits; this mode has no split, so ::mmc3::AcknowledgeScanlineIRQ both
 * disables further ones and clears any left armed by whatever ran before.
 * The APU's frame and DMC IRQs are already off -- main.cpp's RESET does that
 * once at boot -- so there is nothing else to switch off here.
 *
 * ::ppu::ctrl::GEN_NMI is what makes ::nmi_handler run at all, and
 * SPRITE_ADDR is there for the same reason title mode sets it: 8x8 sprites
 * take their pattern table from PPUCTRL, and the arrow lives in the $1000
 * half.
 */
void InitPregameScreen() {
    // Nothing of the previous mode's sprites may survive into this one: every
    // slot is parked, and the arrow is placed by ::DrawOptions when there is a
    // wheel to point at.
    oam::PopulateFromProvider(OAMBuffer, 0, oam::y, ParkY, 64);

    mmc3::SwitchCHRBank<mmc3::BankOf<blob::ui::__lma>()>(mmc3::chr3Control);   // R3, PPU $0400: background
    mmc3::SwitchCHRBank<mmc3::BankOf<blob::ui::__lma>()>(mmc3::chr1Control);   // R1, PPU $1800: the same art as sprites
    mmc3::SetMirroring(false);

    ppu::Flush(kBlankTile, 0xff);

    // BG palette 3 is what the font is drawn in, sprite palette 1 the arrow's
    // (ui/impl/singleChoice.hpp's ::kSelectorAttributes) -- the same three
    // colours in both, exactly as title mode writes them.
    ppu::pal::WriteFromBuffer(ppu::BG_3 + 1, titleScreenColours, 3);
    ppu::pal::WriteFromBuffer(ppu::SPRITE_1 + 1, titleScreenColours, 3);

    mmc3::AcknowledgeScanlineIRQ();

    pNMI = nmi_handler;
    pIRQ = irq_handler;

    MakeScene();

    // The camera, onto the quadrant ::SceneOriginTiles just placed the scene
    // in. Set ONCE, here, and never touched again: this mode has no raster
    // split and no moving viewpoint (see ::nmi_handler), so the frame's own
    // scroll is the whole of its camera, and ::ppu::SetScroll's value persists
    // until something writes another one -- which, inside this mode, nothing
    // does. It is set BEFORE rendering is enabled below so no frame is ever
    // presented looking at the blank top-left page.
    //
    // Note the scroll and the scene's origin are the SAME number by
    // construction, which is what makes plane tiles screen tiles again for
    // ::PlaceArrow: the quadrant offset cancels.
    ppu::SetScroll(SceneOriginPixels());

    ppu::EnableRendering(ppu::ctrl::SPRITE_ADDR | ppu::ctrl::GEN_NMI,
                         ppu::mask::BG_L | ppu::mask::SPRITE_L);
    irq::EnableInterrupts();
}

/**
 * @brief This mode's whole NMI: sway the arrow, upload OAM.
 *
 * A STUB, DELIBERATELY. Title mode's handler re-scrolls, re-banks and re-arms
 * a raster split every frame because its screen is two bands of different art;
 * this one is a flat page of text. Its own scroll IS off the origin -- the
 * scene sits in the bottom-right quadrant (::SceneOriginTiles) -- but that is
 * a camera parked once in ::InitPregameScreen, not a camera that moves, so
 * there is still nothing for a per-frame handler to re-assert. The nametable
 * writes it does make are
 * ::PrintContext's and ::DrawOptions', from the MAINLINE -- which is safe here
 * and would not be on the NES, where a write outside vblank is a glitch rather
 * than a slower write. That is the whole reason this mode can afford to be
 * this simple, and why it is compiled only on the GameCube.
 *
 * What is left is the two things that must happen once a frame: one step of
 * the arrow's sway (::ui::impl::AnimateSelector reads ::frameCounter, which
 * main.cpp's vector has already stepped), and the OAM upload that makes any of
 * it visible.
 */
void nmi_handler() {
    ui::impl::AnimateSelector();
    oam::RefreshSprites(OAMBuffer);
}

/**
 * @brief Nothing is armed to reach this, so it only makes sure of that.
 *
 * ::InitPregameScreen acknowledges the scanline counter and arms nothing, so
 * an IRQ here means one was already pending when the mode started. Clearing it
 * is the whole handler: an unacknowledged MMC3 IRQ re-asserts forever and the
 * console would spend the rest of this mode in this function.
 */
void irq_handler() {
    mmc3::AcknowledgeScanlineIRQ();
}

/**
 * @brief Puts the arrow beside @p option, sway included.
 *
 * NOT ::ui::impl::PlaceSelector, THOUGH IT IS ALMOST THE SAME FIVE STORES.
 * That one subtracts ::ui::impl::SelectorScroll(), which title.cpp defines as
 * its own menu band's scroll -- the right transform for a menu sitting on a
 * scrolled-to nametable below a raster split, and the wrong one here. This
 * mode's own camera is ::SceneOriginPixels (::InitPregameScreen), the one
 * scroll the frame is read under, so that is what comes off a plane
 * coordinate to leave a screen one -- and since the scene is anchored at the
 * same origin, the two cancel and the arrow lands where it always did.
 * Subtracted rather than dropped because the two are only equal by
 * construction: moving either the quadrant or the camera alone is exactly the
 * mistake this undoes. Everything else
 * about the arrow is shared: the tile, the palette, the two-tile gap, the sway
 * table, and the OAM slot ::ui::impl::SelectorSlot() names, so
 * ::ui::impl::AnimateSelector sways this mode's arrow with no change at all.
 *
 * The sway is applied here as well as in the NMI for the reason PlaceSelector
 * gives: an arrow that moved mid-cycle would otherwise snap to its unswayed X
 * for one frame and jump back.
 */
static void PlaceArrow(const Rect& option) {
    const u8 slot = ui::impl::SelectorSlot();

    const vec2<u16> scroll = SceneOriginPixels();
    const i16 x = static_cast<i16>(option.pos.x * 8 - ui::impl::kSelectorGap - scroll.x);
    const i16 y = static_cast<i16>(option.pos.y * 8 - scroll.y);
    if (x < 0 || y < 0) {
        // Signed, and parked rather than clamped, same guard as PlaceSelector:
        // a u16 would wrap an off-screen arrow into somewhere visible.
        OAMBuffer[slot].y = ParkedSpriteY();
        return;
    }

    OAMBuffer[slot].tile       = ui::impl::kSelectorTile;
    OAMBuffer[slot].attributes = ui::impl::kSelectorAttributes;
    OAMBuffer[slot].y          = static_cast<oam::oam_t>(y);

    ui::impl::selectorBaseX = static_cast<u16>(x);
    OAMBuffer[slot].x = static_cast<oam::oam_t>(x + ui::impl::SwayOffset(frameCounter));
}

/** @brief The label an option is drawn with. */
static const u8* LabelFor(const eOptions option) {
    switch (option) {
        case Continue: return continueText.data();
        case Retry:    return retryText.data();
        case No:       return noText.data();
        case Yes:      return yesText.data();
        case Erase:    return eraseText.data();
        case Card1:    return card1Text.data();
        case Card2:    return card2Text.data();
    }
    // Unreachable through the option arrays this file declares, and a label is
    // better than a blank row if one is ever added without a case here.
    log("pregame: option %d has no label", option);
    return continueText.data();
}

/** @brief Blanks whatever labels a wheel left on screen, and parks the arrow. */
void ClearOptions() {
    for (u8 i = 0; i < kMaxOptions; i++) {
        if (!optionRows[i]) continue;
        const Rect r = optionRect[i];
        ui::text::Clear(optionRows[i], ppu::CartesianToAddress(r.pos), r.size,
                        ui::text::Centre, kBlankTile);
        // Freed, not just forgotten: ::ui::text::Make allocates a row table per
        // call (`new textBuffer[box.y]`, src/all/extras/ui/text.cpp) and hands
        // ownership to whoever asked. Once at boot that hardly mattered; the
        // save screen runs a wheel every time the player leaves the options
        // page, so the ones it opens have to go back.
        delete[] optionRows[i];
        optionRows[i] = nullptr;
    }
    OAMBuffer[ui::impl::SelectorSlot()].y = ParkedSpriteY();
}

// charmapped error text message explaining status (either, success, error with choice)
//
// CLEARED BEFORE IT IS REPLACED, not space-padded: the messages are different
// lengths and wrap to different numbers of rows, so a longer one leaves a tail
// behind a shorter one. ::ui::text::Clear blanks exactly the rows the previous
// message occupied, which is why ::contextRows is kept rather than rebuilt.
void PrintContext(const u8* infoText) {
    const Rect r = contextRect;
    const u16  addr = ppu::CartesianToAddress(r.pos);

    if (contextRows) {
        ui::text::Clear(contextRows, addr, r.size, ui::text::Centre, kBlankTile);
        // Same ownership as ::ClearOptions frees -- and the same reason it is
        // worth freeing here: the save screen prints into this band once per
        // attempt, so a message replaced is a row table nothing can reach.
        delete[] contextRows;
        contextRows = nullptr;
    }

    contextRows = ui::text::Make<charmap_generic, ' '>(infoText, r.size);
    ui::text::Draw(contextRows, addr, r.size, ui::text::Centre);
}

constexpr u8 kWheelRepeatBits  = input::LEFT | input::RIGHT;
constexpr u8 kWheelRepeatDelay = 16;
constexpr u8 kWheelRepeatTick  = 8;

/**
 * @brief Draws the options and points the arrow at @p opt.
 *
 * The labels are a cold paint -- they never change while a wheel is open, so
 * this runs once per wheel and the SELECTION moving afterwards costs nothing
 * but the arrow's five stores. That is the whole reason the indicator is a
 * sprite in this project (see ui/impl/singleChoice.hpp's own note on it).
 */
static void DrawOptions(const eOptions* const pOptions, const u8 sOptions, const u8 opt) {
    for (u8 i = 0; i < sOptions; i++) {
        const Rect r = optionRect[i];
        optionRows[i] = ui::text::Make<charmap_generic, ' '>(LabelFor(pOptions[i]), r.size);
        ui::text::Draw(optionRows[i], ppu::CartesianToAddress(r.pos), r.size, ui::text::Centre);
    }
    // Copied out of `atomic Rect` storage first: that storage is volatile, so
    // a const Rect& cannot bind to it -- Rect's volatile copy constructor is
    // what makes the read legal (pnes/extras/ui/types.hpp).
    const Rect selected = optionRect[opt];
    PlaceArrow(selected);
}

eOptions ChoiceWheel(const eOptions* const pOptions, const u8 sOptionsIn) {
    // CLAMPED, BECAUSE THE LAYOUT IS THE LIMIT: this scene has two anchors --
    // the quarter and three-quarter points -- so a third option would have
    // nowhere to be drawn and no place for the arrow to point. Every call site
    // in this file passes two; a longer list is a mistake worth hearing about
    // rather than a layout to invent at runtime.
    u8 sOptions = sOptionsIn;
    if (sOptions > kMaxOptions) {
        log("pregame: ChoiceWheel given %d options, showing %d", sOptionsIn, kMaxOptions);
        sOptions = kMaxOptions;
    }

    u8 opt = 0;
    DrawOptions(pOptions, sOptions, opt);

    u8 seed1 = 0, seed2 = 0;
    input::PollControllers(&seed1, &seed2);
    u8 prevInputs = static_cast<u8>(seed1 | seed2);

    u8   repeatHeld  = 0;
    u8   repeatFrame = 0;
    bool repeating   = false;

    while (true) {
        u8 port1 = 0, port2 = 0;
        input::PollControllers(&port1, &port2);
        const u8 inputs  = static_cast<u8>(port1 | port2);
        const u8 pressed = static_cast<u8>(inputs & ~prevInputs);
        prevInputs = inputs;

        u8 step = static_cast<u8>(pressed & kWheelRepeatBits);
        if (step) {
            repeatHeld  = step;
            repeatFrame = 0;
            repeating   = false;   // the delay is served before any tick
        } else if (const u8 held = static_cast<u8>(inputs & repeatHeld); !held) {
            repeatHeld  = 0;
            repeatFrame = 0;
            repeating   = false;
        } else if (++repeatFrame >= (repeating ? kWheelRepeatTick : kWheelRepeatDelay)) {
            repeatFrame = 0;
            repeating   = true;
            step        = held;
        }

        // Clamped at both ends rather than wrapping, which is what the first
        // version did -- swap these for a modulo if a wheel should roll over.
        //
        // SOUNDED ONLY ON A REAL MOVE, which is why the comparison is against
        // the index before the step rather than against the key. An end-stop
        // press is not a move: a player holding LEFT at the leftmost option
        // would otherwise hear the repeat tick away against the wall. Same
        // guard, and the same reason for it, as the DrawChange hooks in
        // ui/impl/singleChoice.hpp.
        const u8 was = opt;
        if (step & input::LEFT) {
            if (opt) --opt;
        } else if (step & input::RIGHT) {
            if (opt + 1 < sOptions) ++opt;
        }
        if (opt != was) {
            ui::impl::PostSfx(sfx::selectnext);
            // The labels stay put; only the sprite moves. No nametable write,
            // so this is safe from the mainline on any frame.
            const Rect moved = optionRect[opt];
            PlaceArrow(moved);
        }

        if (pressed & input::A) {
            // The confirm's own sound, as every other confirm in this project
            // posts it (ui/impl/canvas.hpp). One mailbox slot, and this is the
            // last thing posted on this frame, so it cannot be overwritten by
            // the move above.
            ui::impl::PostSfx(sfx::select);
            break;
        }

        WaitBody();
    }

    // The options go; the context message does NOT. A wheel is answered inside
    // a branch that is still explaining itself, and the next ::PrintContext
    // clears the old message when it has a new one to put there.
    ClearOptions();
    return pOptions[opt];
}

/** @brief Blanks the context band and gives its row table back. */
static void ClearContext() {
    if (!contextRows) return;
    const Rect r = contextRect;
    ui::text::Clear(contextRows, ppu::CartesianToAddress(r.pos), r.size,
                    ui::text::Centre, kBlankTile);
    delete[] contextRows;
    contextRows = nullptr;
}

/**
 * @brief The message a failed write is explained with.
 *
 * The same mapping ::main makes for the same codes, and the same judgement
 * behind it: a player is told what they can act on (no card, a full card, a
 * card with no save on it) and everything else is one "could not save",
 * because nothing a player does differs between a busy card, a denied
 * operation and a bad path. ::save::LastNativeError is where the difference
 * goes -- into the log, for whoever is reading it.
 */
static const u8* SaveErrorText(const save::eError err) {
    switch (err) {
        case save::no_device: return saveNoDeviceText.data();
        case save::missing:   return saveNoFileText.data();
        case save::full:      return saveFullText.data();
        case save::too_large: return saveLargeText.data();
        default:              return saveWriteFailedText.data();
    }
}

/**
 * @brief Shows SAVING in this mode's own quadrant, writes ::saveData, and
 *        offers RETRY / CONTINUE if the write fails.
 *
 * REACHED FROM TITLE MODE, not from ::main -- the options page's settings ARE
 * the save file (demo/src/save.hpp's SaveFile::config is what uitk writes the
 * page's buttons and sliders straight into), so the moment the player leaves
 * that page is the moment there is something new to write. ::title::main calls
 * this on the B that backs out of the page, before the page animates away.
 *
 * IT BORROWS THIS MODE'S SCENE RATHER THAN DEFINING ONE. ::MakeScene's three
 * boxes are already anchored in the bottom-right quadrant
 * (::SceneOriginTiles), already sized against the live viewport, and
 * ::PrintContext/::ChoiceWheel already draw a message with a two-option wheel
 * under it -- which is the whole of this screen. So this is the save flow
 * written once and shown twice, which is also why the layout is re-Made here:
 * the GameCube's viewport may have changed width since boot (the options page
 * has a widescreen button on it, ::title::ApplyWidescreenChange), and every
 * anchor is derived from that width.
 *
 * WHY THAT QUADRANT IS FREE TO BORROW. The title screen's menu band is the
 * right-hand page's rows 0-5 and nothing else (::title::kMenuBandRows, and
 * demo/ui/title.uis' own quadrant), while this scene sits on the middle rows of
 * the same page -- row 13 for the message, 17 for the wheel. So pointing the
 * camera at the page shows the title text still sitting across the top with
 * SAVING under it, and not one tile of either scene is written over the other.
 *
 * THE CAMERA IS NOT PUT BACK, and the caller must not expect it to be: there
 * is no way to read a scroll back out of the PPU, so what the frame looked at
 * before this is not something this function can know. It is not a gap --
 * ::title::nmi_handler_lockAnimReverse, which is where the B that got here
 * hands over next, writes the scroll on every single frame it runs, so the
 * first vblank after this returns re-points the camera on its own. The NMI
 * VECTOR is restored, because that one can be read.
 *
 * @return true when ::saveData reached the medium; false when the player chose
 *         CONTINUE over a failure. Nothing acts on it yet -- the title screen
 *         comes back either way -- but a caller that wanted to refuse to leave
 *         until the save landed has the answer here rather than in a log line.
 */
bool SaveScreen() {
    // The vectors, for the span of this screen only. ::nmi_handler is this
    // mode's own stub -- one frame of the arrow's sway and the OAM upload --
    // which is exactly what a wheel needs and all it needs; title mode's
    // handler would re-scroll the frame back onto the options page every
    // vblank and undo the camera below.
    void (* const callerNMI)() = pNMI;
    pNMI = nmi_handler;

    // Nothing may fire a raster split under this screen. Title mode arms one
    // for its menu band and would re-point the scroll mid-frame if it landed;
    // the locked options state leaves none armed, so this is the belt to that
    // brace and costs one register write. The IRQ VECTOR is left alone: it is
    // the caller's, and with nothing armed it is never reached.
    mmc3::AcknowledgeScanlineIRQ();

    MakeScene();
    ppu::SetScroll(SceneOriginPixels());

    bool saved = false;
    while (true) {
        PrintContext(saveSavingText.data());
        // One frame BEFORE the write starts, so SAVING is actually on screen
        // for it: a memory card write that succeeds immediately would
        // otherwise draw the word and take it away again inside the same
        // frame, and the player would see nothing at all.
        WaitBody();

        const save::Response response = SaveGame(WaitBody);
        if (response.err == save::found) {
            saved = true;
            break;
        }

        log("pregame::SaveScreen: save failed err=%d native=%d",
            response.err, save::LastNativeError());

        PrintContext(SaveErrorText(response.err));
        // RETRY re-runs the whole attempt, which is why the loop encloses the
        // message as well as the write: a card inserted in answer to NO MEMORY
        // CARD INSERTED has to be probed again, and ::SaveGame's next call is
        // what does that. CONTINUE leaves without a save, exactly as it does
        // everywhere else in this mode.
        if (ChoiceWheel(errorOpts, sizeof(errorOpts)) != Retry) break;
    }

    // The band goes back to blank tiles. Nothing on screen depends on it --
    // the rows this borrowed are ones the title screen never shows -- but a
    // screen that cleans up after itself is what lets the next visit draw into
    // the same rows without knowing what the last one left.
    ClearContext();

    pNMI = callerNMI;
    return saved;
}

bool LoadSave() {
    save::Response response { };
    while (response = save::Load(&saveData, sizeof(saveData), &response), !response.done) {
        WaitBody();
    }

    if (!ValidateSave()) {
        PrintContext(saveCorruptText.data());
        // print information about the save file being corrupted, handle this

        switch (ChoiceWheel(eraseOpts, sizeof(eraseOpts))) {
            case Erase:
                return false; // re attempt

            case Continue:
                break;

            default:
                log("unknown choice selected");
        }

        // RESTORE BOTH HALVES. CONTINUE means the player chose to carry on
        // with a save this build cannot read, so ::saveData.config holds that
        // file's bytes laid out to some other version's shape -- a slider
        // amount that was a button bitfield, or the reverse.
        // ::PopulateSaveData puts the header and the version back;
        // ::SeedConfigDefaults overwrites the config those foreign bytes
        // landed in, which is what stops them being read as settings. Nothing
        // downstream filters them for us any more -- ::title::main applies
        // whatever is in ::saveData.config unconditionally -- so the
        // correction has to happen here, before it is handed over.
        PopulateSaveData();
        SeedConfigDefaults();
        return true;
    }

    // THE LOAD IS ONLY TRUSTED WHEN BOTH HALVES AGREE. ::ValidateSave above
    // reads ::saveData.version -- but a transfer that never happened leaves
    // whatever was in the struct, and ::PopulateSaveData stamped a valid
    // version into it before this mode ever touched the card, so a failed load
    // validates perfectly well. `err == found` is the other half: it says the
    // bytes on screen came off the medium rather than out of our own BSS.
    if (response.err != save::found) {
        log("pregame: load did not complete (err=%d native=%d) -- keeping the defaults "
            "seeded before it", response.err, save::LastNativeError());
    }

    return true;
}

bool ValidateSave() {
    // run checks on saveData
    if (saveData.version != saveVersion) return false;
    return true;
}

bool TryCreateSave() {
    bool useCard2 = false;
    const u8 saveDevices = save::AccessibleDevices();
    if (!saveDevices) {
        PrintContext(saveNoDeviceText.data());
        switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
            case Retry:
                return false; // re attempt

            case Continue:
                break;

            default:
                log("unknown choice selected");
                break;  // something bad happened
        }
    }

    if (saveDevices == 0b11) {
        PrintContext(saveChooseCard.data());
        switch (ChoiceWheel(cardOpts, sizeof(cardOpts))) {
            case Card1:
                useCard2 = false;
                break;

            // Card2, NOT Continue. ::cardOpts is { Card1, Card2 }, so the
            // right-hand option comes back as Card2 -- which matched neither
            // case, fell to the default, logged "unknown choice selected" and
            // left ::useCard2 false. Picking card 2 created the save on card 1.
            case Card2:
                useCard2 = true;
                break;

            default:
                log("unknown choice selected");
                break;  // something bad happened
        }

        create:
        save::Response response { };

        while (response = save::GetDeviceStatus(useCard2), !response.done) {
            WaitBody();
        }

        // CHECKED BEFORE IT IS HANDED ON, because a response that names no
        // device does not stop a create -- it redirects it. save::CreateSave
        // reads only `path`, and a null one means "wherever this game's save
        // goes by default", which is slot A whatever the player chose. So a
        // device that stopped being usable between the sweep and here would
        // silently put the save on the other card.
        if (response.err != save::found) {
            PrintContext(saveNoDeviceText.data());
            log("pregame: card %d unusable before create (err=%d native=%d)",
                static_cast<u8>(useCard2 + 1), response.err, save::LastNativeError());
            switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                case Retry:
                    return false; // re attempt

                case Continue:
                    break;  // no save load

                default:
                    log("unknown choice selected");
                    break;  // something bad happened
            }
            return true;
        }

        while (response = save::CreateSave(&saveData, sizeof(saveData), &response), !response.done) {
            WaitBody();
        }

        log("pregame: create on %s -> err=%d native=%d",
            response.path ? response.path : "(no path)", response.err, save::LastNativeError());

        switch (response.err) {
            case save::found:
                break;
            case save::missing:
                PrintContext(saveBadErrorText.data());
                log("cannot create file because it does not exist, bad error");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::no_device:
                break;
            case save::busy:
                PrintContext(saveBadErrorText.data());
                log("cannot create file, card busy, bad error");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::exists:
                PrintContext(saveBadErrorText.data());
                log("cannot create file because it already exists, should have been verified earlier");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::
                full:PrintContext(saveFullText.data());
                log("Should have known at this point, bad error");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::too_large:
                PrintContext(saveBadErrorText.data());
                log("cannot create file because the file that we haven't confirmed to exist may be too large to load which isn't the operation at hand, bad error");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::denied:
                PrintContext(saveBadErrorText.data());
                log("cannot create file, memory card denied operation");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::bad_path:
                PrintContext(saveBadErrorText.data());
                log("cannot create file, bad path");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return true; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
            case save::failed:
                PrintContext(saveBadErrorText.data());
                log("cannot create file, unknown reason");
                switch (ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        return false; // re attempt

                    case Continue:
                        break;  // no save load

                    default:
                        log("unknown choice selected");
                        break;  // something bad happened
                }
                break;
        }

    } else {
        useCard2 = saveDevices - 1;
        PrintContext(saveUseCard.data());
        // TODO: string interpolation, emit message 'slot %d is available, wish to save?"
        switch (ChoiceWheel(createOpts, sizeof(createOpts))) {
            case Yes:
                goto create;

            case No:
                break;  // no save load

            default:
                log("unknown choice selected");
                break;  // something bad happened
        }
    }

    return true;    //for no retry
    // using this information, we can present what memory cards are accessible, should work on something here nicely
}

// populate saved data with its basic information for creation
//
// THE VERSION IS PART OF THAT INFORMATION, and it was missing: every field
// below is metadata the card shows, and ::saveVersion is what ::ValidateSave
// reads back to decide the file is this build's. Without it a save created here
// went to the card with the zero its BSS started on and came back "SAVE FILE
// CORRUPTED" on the very next boot. ::SaveGame stamps it for the same reason on
// the write-back path (demo/src/save.cpp).
/**
 * @brief Writes the scene's own defaults into ::saveData.config.
 *
 * RUN BEFORE THE LOAD, NOT AFTER, which is the whole trick: a real save simply
 * overwrites what this wrote, so nothing downstream has to know whether one
 * arrived. The old arrangement seeded defaults AFTER the load in
 * ::title::main, which is why it needed a ::saveLoaded flag to say when
 * seeding was allowed -- and why a build that forgot the flag on one platform
 * overwrote that platform's save every boot.
 *
 * ZERO IS THE DEFAULT FOR EVERYTHING EXCEPT THE SLIDERS. Every button's
 * cleared bit and every menu's option 0 are already what the scene wants, so
 * the memset is the bulk of the work.
 *
 * 0xFF AND NOT A REAL HANDLE POSITION, because a real one cannot be computed
 * here: `amount` is a cell index into a track whose width comes from the
 * slider's rect, and that rect is resolved by ::gen::options::Make() in title
 * mode, long after this mode has handed over. 0xFF means "the right-hand end,
 * whatever it turns out to be", and title's clamp -- the same one every loaded
 * save already goes through -- lands it on the last legal cell once the track
 * exists.
 */
void SeedConfigDefaults() {
    memset(&saveData.config, 0, sizeof(saveData.config));
    gen::options::audioCanvas::musicSlider::amount = 0xFF;
    gen::options::audioCanvas::SFXSlider::amount   = 0xFF;
}

void PopulateSaveData() {
    saveData.version = saveVersion;
#ifdef TARGET_GC
    memcpy(saveData.header.banner,      banner,     sizeof(banner));
    memcpy(saveData.header.icon,        icon,       sizeof(icon));
    memcpy(saveData.header.gameName,    gameName,   sizeof(gameName));
    memcpy(saveData.header.gameDesc,    gameDesc,   sizeof(gameDesc));
#endif
}

#ifdef TARGET_GC
const OGC_BANNER_CI(banner) = {
#embed <card/banner.ci>
};

const OGC_ICON_CI(icon) = {
#embed <card/icon.ci>
};

#ifdef REGION_JP
const char gameName[32] = "\x00";   // shift-JIS todo: the japanese are hungry for letters
const char gameDesc[32] = "\x00";   // shift-JIS
#else
const char gameName[32] = "Super Mary Sisters\x00";   // ASCII
const char gameDesc[32] = "It has Mary in it.\x00";   // ASCII
#endif
#endif

} // namespace pregame

static void LoadConfig() {

}

// GLOBAL, NOT pregame::savePath, because that is where it is DECLARED --
// demo/src/save.hpp, beside ::saveData, which is the save data this path
// belongs to. The mode that fills it is this one; the thing it describes is not
// the mode's.
char* savePath;

#endif // TARGET_OGC
