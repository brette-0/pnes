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

using namespace br0::intsh;

// TARGET_GC, NOT `#ifndef TARGET_NES`, AND THAT IS NARROWER THAN IT LOOKS.
// This mode is skipped on the NES because a battery cart has nothing to find --
// but it is compiled ONLY on the GameCube because that is as far as its
// vocabulary reaches: eOptions' enumerators, every save*Text and ::savePath are
// all inside `#ifdef TARGET_GC` in demo/src/save.hpp, since the memory card is
// the one medium with slots to choose between and metadata to lay out.
//
// Widening this to every non-NES target is a one-line change HERE and a
// decision THERE: ungate the enum and the strings in demo/src/save.hpp, and
// flip this guard along with the three in main.hpp/main.cpp that match it.
#ifdef TARGET_GC

namespace pregame {

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
        if (ui::impl::SfxEnabled()) {
            audio::SfxPlay(static_cast<u8>(queued - 1), ui::impl::kSfxChannel,
                           ui::impl::SfxLevel());
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
static void MakeScene();
static void InitPregameScreen();
static void ClearOptions();
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
#ifdef TARGET_GC
            const u8 sPath = strlen(response.path);
            if (!savePath) {    // we don't need to free, as it will be free on next load, I think
                savePath = new char[sPath + 1];   // + the terminator strcpy writes
                strcpy(savePath, response.path);
            }
#endif
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
    log("pregame::main: handing over to the title screen");
    gameMode = eGameModes::Title;
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
 */
void MakeScene() {
    const u16 tx = video::viewport_tx();
    const u16 ty = video::viewport_ty();

    contextRect = Rect{{4, static_cast<u16>((ty >> 1) - 2)},
                       {static_cast<u8>(tx - 8), kContextRows}};

    const u16 optionY = static_cast<u16>((ty >> 1) + 2);
    optionRect[0] = Rect{{static_cast<u16>((tx >> 2) - (kOptionCols >> 1)), optionY},
                         {kOptionCols, 1}};
    optionRect[1] = Rect{{static_cast<u16>((tx >> 1) + (tx >> 2) - (kOptionCols >> 1)), optionY},
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

    ppu::EnableRendering(ppu::ctrl::SPRITE_ADDR | ppu::ctrl::GEN_NMI,
                         ppu::mask::BG_L | ppu::mask::SPRITE_L);
    irq::EnableInterrupts();
}

/**
 * @brief This mode's whole NMI: sway the arrow, upload OAM.
 *
 * A STUB, DELIBERATELY. Title mode's handler re-scrolls, re-banks and re-arms
 * a raster split every frame because its screen is two bands of different art;
 * this one is a flat page of text. The nametable writes it does make are
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
 * scrolled-to nametable below a raster split, and the wrong one here, where
 * the page is unscrolled and plane tiles ARE screen tiles. Everything else
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

    const i16 x = static_cast<i16>(option.pos.x * 8 - ui::impl::kSelectorGap);
    const i16 y = static_cast<i16>(option.pos.y * 8);
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

        PopulateSaveData(); // restore defaults
    }

    return true;
}

bool ValidateSave() {
    // run checks on saveData
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

            case Continue:
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

        while (response = save::CreateSave(&saveData, sizeof(saveData), &response), !response.done) {
            WaitBody();
        }

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
void PopulateSaveData() {
#ifdef TARGET_GC
    memcpy(saveData.header.banner,      banner,     sizeof(banner));
    memcpy(saveData.header.icon,        icon,       sizeof(icon));
    memcpy(saveData.header.gameName,    gameName,   sizeof(gameName));
    memcpy(saveData.header.gameDesc,    gameDesc,   sizeof(gameDesc));
#endif
}

#ifdef TARGET_GC
const OGC_BANNER_CI(banner) = {
    // embed
};

const OGC_ICON_CI(icon) = {
    // embed
};

#ifdef REGION_JP
const char gameName[32] = "\x00";   // shift-JIS
const char gameDesc[32] = "\x00";   // shift-JIS
#else
const char gameName[32] = "Super Mary Sisters\x00";   // ASCII
const char gameDesc[32] = "It has Mary in it.\x00";   // ASCII
#endif
#endif

} // namespace pregame

// GLOBAL, NOT pregame::savePath, because that is where it is DECLARED --
// demo/src/save.hpp, beside ::saveData, which is the save data this path
// belongs to. The mode that fills it is this one; the thing it describes is not
// the mode's.
char* savePath;

#endif // TARGET_GC
