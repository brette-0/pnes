#include "title.hpp"

#ifndef GEN_TARGET_DIR
#error "GEN_TARGET_DIR is not set -- add it to this target's CMakeLists.txt branch."
#endif
#include STRCAT(../../gen/title/GEN_TARGET_DIR/title.hpp)
#include STRCAT(../../gen/gameOptions/GEN_TARGET_DIR/gameOptions.hpp)
#include "../../ui/impl/buttons.hpp"
#include "../../ui/impl/sliders.hpp"
#include "../../ui/impl/singleChoice.hpp"
#include "../../ui/impl/canvas.hpp"
#include "../../ui/impl/sounds.hpp"
#include "../main.hpp"
#include "pnes/logger.hpp"
#include "../banks.hpp"
#include "../graphics/colours.hpp"
#include "../graphics/strings.hpp"
#include "../../chr/chr.h"
#include "../graphics/windows.hpp"
#include "../../animations/gen/mary_standing.hpp"
#include "level.hpp"
#include "level/levels.hpp"
// ::pregame::SaveScreen -- the save flow this page's B press runs. The header
// compiles to nothing off the GameCube (its own TARGET_GC guard), which is why
// there is no #ifdef around the include itself.
#include "pregame.hpp"
#include "pnes/mappers/mmc3.hpp"
#include "pnes/extras/ui/text.hpp"

namespace title {
    static_assert(gen::title::TitleOptions::nOptions == End + 1,
                  "title.uis and title.hpp's titleOptions enum disagree on the menu's options");

    static atomic bool playModeActive = false;
    static atomic bool optionsLocked = false;

    // Which of the options page's two Canvases input belongs to. The page is
    // laid out as two columns -- VIDEO on the left, AUDIO on the right (see
    // demo/ui/options.uis) -- and each column is a Canvas with a Pass() of its
    // own, so "which column is live" is exactly a pointer to one of those two.
    //
    // A pointer rather than a bool-and-a-branch for the same reason ::pNMI is
    // one: the loop below switches it and nothing else in this file has to
    // know which Canvas it names. Seeded to the left column, matching the
    // focus index each Canvas's own Make() seeds to its first component.
    //
    // Nothing calls through it yet -- moving focus WITHIN a column, and what
    // that looks like, is still unimplemented (ui::impl::UpdateCanvas is an
    // empty body). This is only the column switch.
    static void (*pOptionsPass)(u8) = gen::options::videoCanvas::Pass;

    // Whether A has handed the corner sprites over to the live Canvas. False
    // is the column level: they bracket the column's own [R] rect and the page
    // owns them. True is inside the Canvas: its DrawComponentSelection() and
    // DrawUpdate() own them, and they bracket whichever component has focus.
    //
    // Always false by the time options is left (B comes out of the Canvas
    // before it can leave the page), so a return starts at the column again --
    // the column itself is what ::pOptionsPass remembers.
    static atomic bool canvasEntered = false;

    // The frame's press for the live Canvas: posted by the input loop, spent by
    // ::nmi_handler_locked. It cannot be handed straight to Pass() from the
    // loop, because a component's own Draw()/DrawChange() writes the nametable
    // -- a button repaints its box and its label -- and on NES that is only
    // legal in vblank. It is the same constraint that made the menu arrow a
    // sprite (ui/impl/singleChoice.hpp), except here the writes are the point
    // and cannot be designed away, so they move to the handler instead.
    //
    // Only the Canvas's input goes through here. The column switch and the
    // corners are sprite work -- OAM is staged in ordinary RAM and uploaded
    // once a frame -- so those stay in the loop, exactly as ::PlaceSelector
    // does.
    static atomic u8 canvasInput = 0;

    // The frame's CLICK for the options page: posted by the input loop, spent
    // by ::nmi_handler_locked, in plane TILE space (see ::ClickTile).
    //
    // Posted rather than spent in the loop for ::canvasInput's reason exactly
    // -- a component's Click() repaints itself, and a button's repaint writes
    // the nametable, which on NES is only legal in vblank. The ring the click
    // then moves is sprite work and would not need the wait; the component's
    // own tiles are what does.
    //
    // Three scalars and not one `atomic vec2<u16>`: `atomic` is `volatile` on
    // every C++ build (technology.hpp), and a volatile ::vec2 can neither be
    // copied out nor assigned without going through its volatile operator=
    // field by field. The focus ring's own cross-boundary state is split the
    // same way and for the same reason (ui::impl::ringLeft and friends).
    //
    // A pending flag rather than a sentinel coordinate: (0,0) is a real tile,
    // and on a dual-screen target it is a real tile of the page. Nothing else
    // can stand in for "no click this frame" -- which is also why
    // ::input::click reports a flag of its own instead of parking its position
    // somewhere impossible.
    static atomic u16  canvasClickX = 0;
    static atomic u16  canvasClickY = 0;
    static atomic bool canvasClickPending = false;

    // Auto-repeat, for the two bits a Canvas spends on a component's own axis.
    //
    // A slider is adjusted by LEFT/RIGHT, and a press strobe gives exactly one
    // step per press -- fine for a button, useless for walking a handle across
    // a track. So the payload the Canvas gets carries a REPEATED L/R in place
    // of the raw strobe: the press itself, then nothing until the direction has
    // been held ::kCanvasRepeatDelay frames, and one more step every
    // ::kCanvasRepeatTick frames for as long as it stays held.
    //
    // Two constants, not one. The DELAY is what keeps a single tap a single
    // step -- it has to outlast how long a player holds a button they meant to
    // press once -- while the TICK is how fast the handle then walks, and
    // wanting the walk faster than the wait is the normal case (the track is
    // only a handful of cells wide, so one step per 32 frames crossed it in
    // about three seconds).
    //
    // The two bits are replaced, not supplemented -- see ::CanvasRepeat and its
    // use in ::main. Everything else in the payload is still the plain strobe,
    // and the COLUMN-level LEFT/RIGHT (which picks a Canvas, one page apart) is
    // deliberately left on the strobe too: holding a direction there would walk
    // through columns several times a second.
    constexpr u8 kCanvasRepeatBits  = input::LEFT | input::RIGHT;
    constexpr u8 kCanvasRepeatDelay = 16;
    constexpr u8 kCanvasRepeatTick  = 8;

    // Which of the two bits currently owns the repeat, how long since it last
    // fired, and whether the delay has already been served -- which is what
    // picks the threshold that frame count is measured against. Plain statics,
    // not `atomic`: unlike ::canvasInput these never leave the input loop -- it
    // is the only reader and the only writer -- so there is no handler to
    // synchronise with.
    static u8   canvasRepeatHeld  = 0;
    static u8   canvasRepeatFrame = 0;
    static bool canvasRepeating   = false;

    // Hardware sprites one character takes. The designer exported the pose
    // for 8x16 (one slot per column, each slot a vertical tile PAIR), and
    // title mode runs 8x8 -- so each exported slot becomes two sprites here,
    // the top tile of the pair and the bottom one.
    constexpr u8 kPlayerSprites = static_cast<u8>(gen::mary_standing::kSize) * 2;

    // Every exported byte has to name art in the $1000 table, because 8x8
    // sprites take their table from SPRITE_ADDR (set in EnableRendering) and
    // not from the byte. In an 8x16 pattern byte that table is bit 0.
    constexpr bool StandingIsInSpriteTable() {
        for (u16 i = 0; i < gen::mary_standing::kSize; i++) {
            if ((gen::mary_standing::Get()[i] & 1) == 0) return false;
        }
        return true;
    }
    static_assert(StandingIsInSpriteTable(),
                  "the standing pose was exported against a frame in the $0000 pattern table, "
                  "but title mode's 8x8 sprites read $1000 (ppu::ctrl::SPRITE_ADDR)");

    // Slots each character occupies, back to back from the start of OAM.
    constexpr u8 kPlayerOneSlot = 0;
    constexpr u8 kPlayerTwoSlot = kPlayerOneSlot + kPlayerSprites;

    // The menu selection arrow, in the first slot past the characters -- so it
    // tracks ::kPlayerSprites rather than being a number that has to be
    // revisited if the pose grows. ONE slot for every menu on this screen:
    // the title menu and the play-mode menu are alternatives for the same
    // rows, so only one arrow is ever wanted (ui/impl/singleChoice.hpp).
    //
    // Built WITHOUT a second player the slot moves down to 4, which is
    // correct rather than merely harmless: nothing is reserved for a character
    // that is never drawn.
#ifdef PLAYER2_SUPPORTED
    constexpr u8 kSelectorSlot = kPlayerTwoSlot + kPlayerSprites;
#else
    constexpr u8 kSelectorSlot = kPlayerOneSlot + kPlayerSprites;
#endif
    // The options page's focus ring: four box-corner sprites, consecutive from
    // the first slot past the arrow, in ui::impl::eCorner order. Reserved for
    // the same reason the arrow is one slot and not one per menu -- only one
    // control is ever focused, so one set of corners covers both Canvases.
    //
    // Past the arrow rather than before it so the character slots stay at the
    // front of OAM: on NES the slot index IS the sprite's priority, and the
    // characters are the only thing here that can overlap anything.
    constexpr u8 kCornerSlot = kSelectorSlot + 1;
    static_assert(kCornerSlot + ui::impl::kCorners <= 64,
                  "the title screen's OAM reservations run past the sprite table");

    // Where each character's leftmost sprite sits. The pose is kWidth tiles
    // wide, so player two is displaced by twice that -- a two-tile gap of
    // clear space between them, rather than two characters that read as one
    // wide one.
    constexpr oam::oam_t kPlayerOneX = 32;
    constexpr oam::oam_t kPlayerTwoX = kPlayerOneX + gen::mary_standing::kWidth * 8 * 2;

    // The feet-Y math below puts every slot of the pose on one sprite row, so
    // a taller pose would stack its lower rows on top of its upper ones.
    static_assert(gen::mary_standing::kHeight == 1,
                  "the title's standing pose is more than one sprite tall: its rows need "
                  "offsetting by the sprite height, which the Y placement here does not do");

    // Putting a menu's arrow on screen cold, through the menu's own hook:
    // uitk calls it with `from == to` meaning "nothing to unpaint" (see
    // genSingleChoiceDecl), which a sprite does not have to distinguish -- the
    // arrow has one position either way. Moving one needs nothing here at all:
    // the generated Pass() calls DrawChange() itself, so the arrow follows the
    // option wherever input came from, this loop or a Canvas.
    static void DrawTitleSelector() {
        namespace menu = gen::title::TitleOptions;
        menu::DrawChange(menu::option, menu::option);
    }

    static void DrawPlayModeSelector() {
        namespace menu = gen::gameOptions::gameOptions;
        menu::DrawChange(menu::option, menu::option);
    }

    static oam::oam_t Clear(u16 _);
    static oam::oam_t Zero(u16 _);
    static oam::oam_t StandingTile(u16 i);
    static NI void DrawLevelPreview();
    static void PlaceStandingSprites();
    static void ParkStandingSprites();
    static void nmi_handler_drawPlayMode();
    static void nmi_handler_drawMenu();
    static void BuildPaletteFadePhases();
    static void nmi_handler_fadeToOptions();
    static void nmi_handler_lockOptions();
    static void nmi_handler_lockAnim();
    static void nmi_handler_locked();
    static void nmi_handler_lockAnimReverse();
    static void nmi_handler_fadeFromOptions();
#if TARGET_GC
    static void ApplyWidescreenChange(bool on);
#endif

    static u16 kMenuNT;

    static u16 maryGroundNtRow;

    static u16 lockAnimScanline;

    // How many scanlines ::nmi_handler_lockAnim/::nmi_handler_lockAnimReverse
    // walk the scroll split by, per frame. Four, so the scroll half of the
    // transition takes half the frames it used to -- the slide is the bulk of
    // it (a ::SplitRow of 24 is 192px to cover, against the fade's handful of
    // steps), so this is the constant that actually sets how long entering
    // options feels.
    constexpr u16 kScanlinesPerFrame = 4;

    // Where each phase below writes back to, and how long it is. BG_3 (the
    // HUD's palette, ::titleScreenColours) is deliberately absent -- the fade
    // must leave the HUD alone.
    struct FadeRun { u8 offset; u8 count; };
    constexpr FadeRun kFadeEntries[] = {
        {ppu::BG_0,         4}, // BG_0 sub-palette, incl. the shared backdrop
        {ppu::BG_1 + 1,     3}, // BG_1 colours 1-3
        {ppu::SPRITE_0 + 1, 3}, // Mary's sprite colours
    };

    constexpr u8 kFadePhaseCount = 5;

    static u8 paletteFadePhases[kFadePhaseCount][4 + 3 + 3];

    // Current index into ::paletteFadePhases: 0 is full colour, ::kFadePhaseCount - 1
    // is fully black. ::nmi_handler_fadeToOptions counts this up,
    // ::nmi_handler_fadeFromOptions counts it back down.
    static u8 fadePhase;

    // Each phase holds for this many frames before the next one. Two, to keep
    // the fade in step with the doubled scroll (::kScanlinesPerFrame) -- the
    // floor is one, where advancing every vblank reads as a near-instant snap
    // (only ::kFadePhaseCount - 1 steps total) rather than a fade, so this is
    // as fast as it goes while still being one.
    constexpr u8 kFadeStepFrames = 2;

    // Counts frames within the current phase; wraps back to 0 (and advances
    // ::fadePhase) in ::nmi_handler_fadeToOptions/::nmi_handler_fadeFromOptions.
    static u8 fadeFrameCounter;

    // Rows the menu band (everything below the split) is given. The NES lays
    // it out as 30 - 24 = 6 rows, which is what the title/menu text (nametable
    // rows 1-4) is sized for.
    constexpr u8 kMenuBandRows = 6;

    // True on a target whose viewport spans two stacked panels (DS/DSi/3DS --
    // video::panel_count(), video.hpp). There the whole UI is authored straight
    // into nametable 0's lower-panel rows by demo/ui/<scene>_ds.uis, so it is
    // permanently on the bottom screen. That removes the two mechanisms the
    // single-screen layout needs and nothing else:
    //
    //   - the raster split. On one screen the menu lives in a DIFFERENT
    //     nametable and the preview is scrolled so the split line swaps to it
    //     mid-frame (::ApplySplit). With the UI already on its own panel in
    //     nametable 0 there is nothing to swap to: scroll stays {0,0} and the
    //     top panel shows the preview while the bottom shows the UI.
    //   - the palette fade. It exists to cover that same single-screen
    //     transition -- the preview going dark as the options menu takes the
    //     screen. The UI never takes the screen here, so there is nothing to
    //     fade out from under.
    //
    // Everything else (the selector, the menu swap, the lock state machine)
    // is shared verbatim, so this stays a bypass of two mechanisms rather than
    // a second copy of the title screen.
    constexpr bool kUiOnOwnPanel = video::panel_count() > 1;

    // ...but the split is not gone, only repurposed. The top panel draws the
    // level preview's CHR bank and the bottom panel the UI's, and MMC3 maps
    // both into the SAME 512-tile window -- so the bank still has to change
    // between them, and the only place to do that is a scanline IRQ at the
    // panel boundary. ::ApplySplit then switches the bank and nothing else:
    // each panel's scroll already comes from its own band (the backend keeps a
    // per-panel scroll table), so a mid-frame ppu::SetScroll would fight it.
    //
    // Honouring a mid-frame bank change needs the backend to support it, which
    // is why this names its targets rather than following ::kUiOnOwnPanel.
    // src/nds/video.cpp does, without restriction -- it mirrors TILES rather
    // than banks, keying a 1024-entry pool on each tile's resolved CHR address,
    // so a switch of any kind at any scanline (including partway down a tile
    // row, which it synthesises a composite tile for) is reproduced.
    // src/3ds/video.cpp does it at band granularity: it plans the frame before
    // drawing it, and each band records the CHR atlas set its own scanlines
    // resolve to -- so a switch ON a scanline boundary, which is all this split
    // is, lands. Both of those also draw panel 1, which is the other half of
    // what this needs: the bank it switches to is for the bottom screen.
#if defined(TARGET_NDS) || defined(TARGET_CTR)
    constexpr bool kUiBankSplit = kUiOnOwnPanel;
#else
    constexpr bool kUiBankSplit = false;
#endif

    static u8 SplitRow() {
        // A cropping target's panel (DS/DSi 24 rows, GBA 20) is shorter than
        // the NES's 30, but the menu still needs the same 6 rows -- scaling
        // the split with the panel height instead gave the DS an 8-row band,
        // i.e. ~4 empty rows under the UI.
        //
        // This asks ::video::panel_ty(0) -- the PRIMARY PANEL's height -- not
        // viewport_ty(). On the dual-screen handhelds the viewport is now the
        // union of both panels (DS 48 rows, 3DS 60; see their viewport_ty() doc
        // comments, video.hpp), and this menu is a single-screen UI: it has to
        // land on the top panel, so it is the top panel's 24/30 rows that decide
        // the split, exactly as the DS's old 24-row viewport used to.
        if (video::panel_ty(0) < 30) return static_cast<u8>(video::panel_ty(0) - kMenuBandRows);
        return (((viewport_my() + 1) >> 1) - 2) << 2;
    }

    static u16 PreviewScrollYForPixels(const u16 splitPixelRow) {
        // Bottom-anchor the level plane against the TOP panel. The preview is
        // the NES-native 30 rows/240px (2 HUD rows + PopulateNameTableColumns'
        // 28 from row 2, levels.cpp), which is TALLER than the DS's 192px top
        // panel -- so the top of it is deliberately cropped and the scroll is
        // whatever puts the plane's last row on the panel's last row:
        //
        //     scroll = 240 - panel 0 height    (DS 48px/6 rows, 3DS 0)
        //
        // NOT 0. Scrolling to 0 tried to fit the whole 240px plane on a 192px
        // panel and dropped the bottom 48px of the level off the screen, which
        // is the wrong half to lose. 3DS's top panel is already 240px, so it
        // lands on 0 by arithmetic rather than by special case.
        //
        // The scroll is why a *_ds.uis UI anchors its rows at plane row 30 and
        // not at panel 1's own first row: with the plane bottom-anchored, row
        // 30 -- the first row below the level data -- is the first row the
        // LOWER panel shows, on both consoles.
        if constexpr (kUiOnOwnPanel) {
            (void)splitPixelRow;
            return static_cast<u16>(240 - video::panel_rect(0).h);
        }
        // NOT a panel height: this is the NAMETABLE PLANE height, the modulus
        // the preview's Y scroll wraps against. It must stay byte-identical to
        // ::emu::ComputeNtGeometry's own tileH floor (`viewport_ty() < 30 ? 30
        // : viewport_ty()`, src/emu/emu.hpp) and to the quadrant offset uitk
        // bakes into the generated scene code (emitExprCpp, tools/uitk/main.cpp)
        // -- the menu lives in nametable quadrant 1, so if this disagrees with
        // where that quadrant actually starts, the scroll lands the preview on
        // the wrong rows. `viewport_py() < 240 ? 240 : viewport_py()` IS that
        // floor expressed in pixels, since viewport_py() == viewport_ty() << 3,
        // so it tracks the dual-screen heights (DS 384, 3DS 480) for free.
        const u16 ntHeight = video::viewport_py() < 240 ? 240 : video::viewport_py();
        return ntHeight - splitPixelRow;
    }

    static u16 PreviewScrollYFor(const u8 splitRow) {
        return PreviewScrollYForPixels(static_cast<u16>(splitRow) << 3);
    }

    static u16 PreviewScrollY() {
        return PreviewScrollYFor(SplitRow());
    }

    // The ABSOLUTE plane Y the menu band begins at, and so the Y ::ApplySplit
    // writes at the raster split: ::ppu::SetScroll's Y names the plane row
    // sourced AT the scanline the write lands on -- on NES because the
    // four-write sequence copies t into v there and then, and on the emulated
    // backends because ::emu::GenerateBands re-anchors its own row counter to
    // it (`ys_base = scrollY - py`, src/emu/ppu.cpp) -- so there is nothing to
    // add for the scanlines already behind it.
    //
    // ZERO, because demo/ui/title.uis and demo/ui/gameOptions.uis sit in
    // nametable quadrant 1 (UR): its Y offset is 0 and its X offset is the one
    // ::kMenuNT supplies, so the band IS plane rows 0..kMenuBandRows - 1 of the
    // right-hand page.
    //
    // Both scenes were quadrant 3 (BR) before, where the band was plane rows
    // 30.. and this was the plane's own height (240 on a full-height target),
    // reached as `PreviewScrollYForPixels(split) + split` and wrapped back down
    // to a page row by ::SetScroll's own `y >= 240` fold. The tiles have not
    // moved: vertical mirroring (::mmc3::SetMirroring(false), ::main) aliases
    // the lower pages onto the SAME physical nametables as the upper ones, so
    // quadrant 3 row 0 and quadrant 1 row 0 were always the same memory. Only
    // the row the scroll names, and the one uitk bakes into the generated
    // addresses, have changed -- which is the whole of this move.
    constexpr u16 kMenuBandOriginPy = 0;

    // The scroll ui/impl/singleChoice.hpp places the arrow by -- see its
    // ::SelectorScroll declaration for why it has to ask the mode at all.
    //
    // This is the scroll the menu band is drawn under, not the frame's
    // top-of-screen scroll: on a single-screen target ::ApplySplit switches to
    // it at the raster split (X to ::kMenuNT's nametable, Y to
    // ::kMenuBandOriginPy), and the arrow is a sprite sitting over THOSE rows.
    // On a dual-screen one there is no switch -- the UI is plane rows 30+ of
    // nametable 0, read under the frame's own bottom-anchored scroll -- so the
    // X term is zero and the Y term is the scroll already in force. Either way
    // this is the scroll that plane coordinates below the split are seen
    // through, which is exactly what converts one to a screen position.
    //
    // The single-screen Y is NEGATIVE now that the band sits in a TOP quadrant:
    // its plane rows start at 0 but its scanlines start at the split, so the
    // offset between the two is -(split) -- plane row 1 under a split at 192 is
    // screen row 200, not row -40. Returned as the u16 that wraps to it, which
    // is what ::ui::impl::PlaceSelector's own `pos.y * 8 - scroll.y` undoes
    // when it narrows the difference to i16: the modular arithmetic is exact in
    // both 16- and 32-bit `int`, so the arrow lands on the band on NES and off
    // it alike. It is also why the term cannot simply be folded into the plane
    // height the way ::SetScroll folds its own -- 48 (= -192 mod 240) is a real
    // scroll of the PREVIEW, and PlaceSelector does no such fold.
    static vec2<u16> MenuBandScroll() {
        if constexpr (kUiOnOwnPanel) return {0, PreviewScrollY()};
        return {static_cast<u16>(kMenuNT << 3),
                static_cast<u16>(kMenuBandOriginPy - (static_cast<u16>(SplitRow()) << 3))};
    }

    AI inline u8 SplitDelay()   { return ::video::Is50Hz() ? 58 : 88; }
    static void ApplySplit();

    AI inline u8 SplitLatency() { return ::video::Is50Hz() ? 5 : 4; }

    // Same reload-from-row math ::ArmSplitIRQ uses, parametrized so
    // ::nmi_handler_lockAnim can re-arm against its own shrinking split row
    // instead of the title menu's fixed ::SplitRow().
    static void ArmSplitIRQForPixels(const u16 splitPixelRow) {
        // Where the split goes depends on what it is FOR:
        //  - ::kUiBankSplit: the panel boundary, always. Its only job is the
        //    CHR bank change between the two panels, so the caller's own
        //    menu-band geometry is irrelevant.
        //  - ::kUiOnOwnPanel without it: nothing to do, so never armed.
        //  - otherwise: the caller's row, the original single-screen split.
        if constexpr (kUiOnOwnPanel && !kUiBankSplit) {
            (void)splitPixelRow;
            return;
        }
        const u16 row = kUiBankSplit ? static_cast<u16>(video::panel_rect(0).h)
                                     : splitPixelRow;
        const u8 splitReload = row > SplitLatency()
            ? static_cast<u8>(row - SplitLatency()) : 0;
        mmc3::ScheduleScanlineIRQ(splitReload, {0, row});
    }

    static void ArmSplitIRQFor(const u8 splitRow) {
        ArmSplitIRQForPixels(static_cast<u16>(splitRow) << 3);
    }

    // Draws the options panel's own content. Called once at startup and, on a
    // dual-screen target, again on every entry into options mode: there the
    // panel and the title menu are ALTERNATIVE content for the same
    // bottom-panel rows (options_ds.uis and title_ds.uis both anchor at plane
    // row 30), exactly as the title and gameOptions menus already are. On one
    // screen the panel lived in its own nametable and the title menu could
    // never overwrite it, so drawing it once was enough.
    static void DrawOptionsPanel() {
        gen::options::optionsTitle::Draw();
        gen::options::superMarySistersText::Draw();
        gen::options::videoOptions::Draw();
        gen::options::audioText::Draw();

        // The two sections, one call each. A Canvas's DrawComponents() paints
        // every component it holds, in scene order, and the scene's own
        // per-target hiding decides what that is -- so this is the whole of the
        // panel's content on every target, with no #if here: the NES draws its
        // two mix buttons, every other target the two sliders and the stereo
        // button, and the GameCube the widescreen button on top of the video
        // section's own. Each component's Draw() covers its label (see
        // ui/impl/buttons.hpp and ui/impl/sliders.hpp), which is why only the
        // four section-wide headings above are still named individually.
        gen::options::videoCanvas::DrawComponents();
        gen::options::audioCanvas::DrawComponents();
    }

    // The rect the focus corners bracket: the [R] node each Canvas carries for
    // exactly this (see options.uis/options_ds.uis). Derived from
    // ::pOptionsPass rather than tracked beside it -- one pointer is the whole
    // of "which column is selected", and a second copy of that could disagree
    // with it. The rect is copied out whole: on a runtime-viewport target it is
    // `atomic Rect`, so reading it goes through Rect's volatile copy
    // constructor once rather than per field.
    static bool AudioCanvasSelected() {
        return pOptionsPass == gen::options::audioCanvas::Pass;
    }

    static Rect SelectedCanvasRect() {
        if (AudioCanvasSelected()) {
            return gen::options::audioCanvas::audioCanvasRect::rect;
        }
        return gen::options::videoCanvas::videoCanvasRect::rect;
    }

    // Hands the corners to the live Canvas, around the component that holds its
    // focus -- `index`, which its Make() seeded to 0 and its Pass() moves from
    // there. Through the generated DrawComponentSelection() rather than
    // straight to ui::impl: past this point the appearance is the Canvas's, and
    // that hook is where a Canvas wanting a look of its own would get one.
    //
    // `components[]` is the only way to a component chosen at runtime, and its
    // entries are pointers to each component's own rect rather than copies of
    // it (see InvokeableComponant), so the rect is copied out through ::Rect's
    // volatile copy constructor into a local, which is also the only way to
    // reach the hook: `const Rect&` cannot bind a `const volatile Rect` lvalue.
    // Exactly what a generated Pass() does with the same two pointers.
    static void SelectFocusedComponent() {
        if (AudioCanvasSelected()) {
            namespace ac = gen::options::audioCanvas;
            const Rect r = *ac::components[ac::index].rect;
            ac::DrawComponentSelection(r);
            return;
        }
        namespace vc = gen::options::videoCanvas;
        const Rect r = *vc::components[vc::index].rect;
        vc::DrawComponentSelection(r);
    }

    // Spends what the loop posted, in vblank -- see ::canvasInput. The guard is
    // not just for the nametable: a Canvas's Pass() has no early-out of its own
    // (an all-zero payload still reads `index` and still makes the one indirect
    // call the components table costs), and most frames carry no press at all.
    // One frame of the L/R auto-repeat: the bits to hand the Canvas THIS frame,
    // which is the press on the frame it happens, then one step once the
    // direction has been held ::kCanvasRepeatDelay frames, then one more every
    // ::kCanvasRepeatTick frames while it stays held.
    //
    // Called once per iteration of the input loop, unconditionally, because it
    // is a frame counter and counting only in the state that reads it would
    // make the first repeat land however many frames after the press the player
    // happened to enter the Canvas. @p inputs is the raw held state, @p pressed
    // the loop's strobe.
    //
    // The most recent press owns the repeat: pressing the opposite direction
    // re-arms onto it, and releasing that one stops the repeat even if the
    // first is somehow still down. Holding both is a tie the component itself
    // breaks -- a generated slider Pass() tests RIGHT before LEFT.
    static u8 CanvasRepeat(const u8 inputs, const u8 pressed) {
        if (const u8 edge = static_cast<u8>(pressed & kCanvasRepeatBits); edge) {
            canvasRepeatHeld  = edge;
            canvasRepeatFrame = 0;
            canvasRepeating   = false;   // the delay is served before any tick
            return edge;                 // the strobe itself, passed straight through
        }

        const u8 held = static_cast<u8>(inputs & canvasRepeatHeld);
        if (!held) {
            canvasRepeatHeld  = 0;
            canvasRepeatFrame = 0;
            canvasRepeating   = false;
            return 0;
        }

        // Still held: the first repeat waits out ::kCanvasRepeatDelay from the
        // press, every one after it ::kCanvasRepeatTick from the last repeat.
        if (++canvasRepeatFrame < (canvasRepeating ? kCanvasRepeatTick
                                                   : kCanvasRepeatDelay)) return 0;
        canvasRepeatFrame = 0;
        canvasRepeating   = true;
        return held;
    }

#if TARGET_GC
    // The widescreen button's bit, as one call both spenders below read it
    // through. uitk's generated Pass()/Click() flip the bit and repaint the
    // box -- that is the whole of what either can do, because the ENGINE-side
    // consequence of the flip is the game's to own (video.hpp's
    // SetWidescreen: the GameCube is told, not asked). So it is noticed by
    // comparing the bit across the call, wherever that call is made.
    //
    // Not hooked in the button's own Draw() in ui/impl/buttons.hpp, which is
    // the other place hand-written code meets this button: that runs on every
    // ordinary repaint of the panel, including the repaint this very change
    // performs, and re-striding VRAM from inside a paint call would be both
    // wrong and re-entrant.
    static bool WidescreenBit() {
        return gen::options::videoCanvas::enableWidescreenButton::Enabled();
    }
#endif

    // EVERY Button's enabled bit, which uitk packs into one array in the
    // generated header -- one bit per button, in scene order (::button_state).
    // So "a button was toggled" is "that array changed", whichever button did
    // it and whichever section it was in.
    //
    // Observed across the call rather than hooked, because there is nothing to
    // hook: a button's only hand-written entry point is its Draw(), and that
    // runs on an ordinary repaint of the panel as well as on a flip (see
    // ui/impl/buttons.hpp), so a sound there would fire once per button every
    // time the page was painted. The generated Pass()/Click() flip the bit and
    // call Draw(), and that is all either can do -- the same reason
    // ::WidescreenBit exists, generalised to every button rather than the one
    // whose flip also has an engine-side consequence.
    //
    // Sized from the array itself, so a ninth button (which would give it a
    // second byte) needs no change here.
    static constexpr u8 kButtonStateBytes =
        static_cast<u8>(sizeof(gen::options::button_state) /
                        sizeof(gen::options::button_state[0]));

    static void ReadButtonState(u8 (&out)[kButtonStateBytes]) {
        for (u8 i = 0; i < kButtonStateBytes; i++) out[i] = gen::options::button_state[i];
    }

    static bool ButtonStateChanged(const u8 (&before)[kButtonStateBytes]) {
        for (u8 i = 0; i < kButtonStateBytes; i++) {
            if (before[i] != gen::options::button_state[i]) return true;
        }
        return false;
    }

    static void SpendCanvasInput() {
        const u8 payload = canvasInput;
        if (!payload) return;
        canvasInput = 0;
#if TARGET_GC
        const bool wideBefore = WidescreenBit();
#endif
        u8 buttonsBefore[kButtonStateBytes];
        ReadButtonState(buttonsBefore);

        pOptionsPass(payload);

        // The flip, whichever button made it. After the call and not inside
        // the Canvas's own hooks for the reason ::kButtonStateBytes spells
        // out. A slider adjustment posts its own effect from DrawChange and a
        // focus move from DrawUpdate, so a payload that moved the ring or
        // walked a handle has already posted by now -- and this cannot
        // double-post over it, because a press is one of those three things
        // and never two.
        if (ButtonStateChanged(buttonsBefore)) ui::impl::PostSfx(sfx::toggle);
#if TARGET_GC
        if (const bool wideNow = WidescreenBit(); wideNow != wideBefore) {
            ApplyWidescreenChange(wideNow);
        }
#endif
    }

    // Where the pointer is, in the plane TILE space every generated node's
    // `rect` is in -- which is what a Canvas's PassClick() and every
    // component's Click() compare against.
    //
    // ::input::click reports viewport pixels; a plane coordinate is that plus
    // the scroll those rows are read under, divided down by the tile size. The
    // scroll comes from ui::impl::CanvasScroll rather than being recomputed
    // here on purpose: that is the exact value the focus ring's own forward
    // transform uses to decide where a component APPEARS
    // (ui::impl::SelectComponent's `pos.x * 8 - scroll.x`), so taking it from
    // the same place is what guarantees the point this resolves to is the
    // component the player can see under their finger. Two copies of that
    // scroll could disagree; one cannot.
    //
    // The add comes BEFORE the shift, which is the whole of why this is exact
    // rather than approximate. Every rect is tile-ALIGNED in plane space, so
    // flooring a plane pixel lands in the tile that contains it and the test
    // is lossless at the edges. Shifting first -- `(px >> 3) + (scroll >> 3)`
    // -- would quantize the scroll separately and lose up to a whole tile
    // whenever it is not a multiple of 8, which on a scrolled page is most of
    // the time.
    static vec2<u16> ClickTile() {
        const vec2<u16> px = input::click.Get();
        const vec2<u16> scroll = ui::impl::CanvasScroll();
        return { static_cast<u16>((px.x + scroll.x) >> 3),
                 static_cast<u16>((px.y + scroll.y) >> 3) };
    }

    // Hands the page over to the Canvas a click landed in.
    //
    // A click does not navigate: it arrives at a component directly, whichever
    // column that component is in and whether or not the d-pad had entered a
    // column at all. So this does what the two presses it replaces would have
    // done between them -- the LEFT/RIGHT that selects the column, and the A
    // that steps into it -- and does them from the destination rather than
    // towards it.
    //
    // The ring is only re-placed when it is actually in the wrong place. The
    // generated Focus() has already moved it if the click changed which
    // component holds focus WITHIN this Canvas, so the cases left are the ones
    // it cannot know about: the corners were bracketing the column's own [R]
    // rect rather than a component (not yet entered), or they were bracketing a
    // component of the OTHER column. Both are placements this page owns, not
    // the Canvas.
    static void EnterCanvasFromClick(void (* const pass)(u8)) {
        const bool switched = pOptionsPass != pass;
        pOptionsPass = pass;
        if (switched || !canvasEntered) {
            canvasEntered = true;
            SelectFocusedComponent();
        }
    }

    // Spends what the loop posted, in vblank -- see ::canvasClickPending.
    //
    // Both Canvases are offered the point, in page order, and the first to
    // claim it wins: a point is in at most one of them, and a point in neither
    // (the gap between the two columns, a heading, the frame around the page)
    // is a click on nothing and leaves the page exactly as it was. That is why
    // PassClick() reports whether it took the point -- without it this could
    // not tell "nothing there" from "handled".
    //
    // Deliberately not routed through ::pOptionsPass the way
    // ::SpendCanvasInput is. That pointer is which column the D-PAD is on, and
    // a click is not required to respect it -- the whole point of a pointer is
    // that it reaches the other column without walking there first. So the
    // click picks the Canvas and the pointer follows it, rather than the other
    // way around.
    static void SpendCanvasClick() {
        if constexpr (!input::has_click) return;

        if (!canvasClickPending) return;
        canvasClickPending = false;

        const vec2<u16> at{canvasClickX, canvasClickY};
#if TARGET_GC
        const bool wideBefore = WidescreenBit();
#endif
        u8 buttonsBefore[kButtonStateBytes];
        ReadButtonState(buttonsBefore);

        if (gen::options::videoCanvas::PassClick(at)) {
            EnterCanvasFromClick(gen::options::videoCanvas::Pass);
        } else if (gen::options::audioCanvas::PassClick(at)) {
            EnterCanvasFromClick(gen::options::audioCanvas::Pass);
        }

        // A click on a button is the same flip a press is, so it is noticed
        // the same way -- see ::SpendCanvasInput. Posted after
        // ::EnterCanvasFromClick, which is what makes the LATER post win:
        // clicking a button in a section the d-pad had not entered both
        // selects the section and flips the button, and the flip is the thing
        // the player actually did.
        if (ButtonStateChanged(buttonsBefore)) ui::impl::PostSfx(sfx::toggle);
#if TARGET_GC
        if (const bool wideNow = WidescreenBit(); wideNow != wideBefore) {
            ApplyWidescreenChange(wideNow);
        }
#endif
    }

    static void ParkOptionsCorners() {
        ui::impl::ParkCorners();
    }

    // Puts the four corner sprites on the selected Canvas's rect -- the page's
    // "this column is live" indicator.
    //
    // Straight through ui::impl::SelectComponent, which is the same call a
    // Canvas's own DrawComponentSelection() makes on a component's rect: one
    // indicator, one set of slots, one placement, whichever of the two is
    // being marked (see ui/impl/canvas.hpp). A component selection therefore
    // takes the corners off the column rect simply by being the more recent
    // call, which is what makes them the SAME sprites rather than two sets that
    // have to agree.
    static void PlaceOptionsCorners() {
        ui::impl::SelectComponent(SelectedCanvasRect());
    }

    // Blanks the WHOLE bottom-panel UI band -- plane row 30 (where every
    // *_ds.uis scene anchors) to the end of the plane. Dual-screen only.
    //
    // The band holds exactly one page at a time: the title UI, or the options
    // page, never both. Whichever is coming in clears the band and draws
    // itself, so neither has to know what the other occupied. That is also why
    // the options page is not drawn at startup on these targets at all -- it
    // stays unloaded until OPTIONS is actually picked. It is the only workable
    // arrangement here: the pages overlap (options_ds spans rows 31-47, the
    // title UI 31-34) and the generated options scene has no Erase_
    // counterparts, since none of its nodes set provideErasing.
    static constexpr u16 kUiBandRow0 = 30;
    static void ClearUiBand() {
        for (u16 r = kUiBandRow0; r < video::viewport_ty(); r++)
            ppu::WriteRepeatedToNameTable({0, r}, (windows::ui + blob::ui::hud_whitespace),
                                          static_cast<u8>(kMenuNT), 0);
    }

    static void BankPreviewTiles() {
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::staticLevelTiles::__lma>()>(mmc3::chr3Control);   // R3, PPU $0400
    }

    static void BankMenuTiles() {
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::ui::__lma>()>(mmc3::chr3Control);   // R3, PPU $0400
    }


    // One frame of the audio engine, and the UI effect waiting on it, under a
    // single bank pair.
    //
    // THE ENGINE HAS TO BE DRIVEN HERE AT ALL, which it was not before: on NES
    // famistudio_sfx_play only writes a stream, and nothing SOUNDS until
    // famistudio_update walks it -- and off NES audio::Update is what refills
    // the effect mix (src/SDL3/audio.cpp's UpdateSfx). This mode never ticked
    // it, because it never played anything; a menu that makes noise is what
    // changes that. It costs a stopped song nothing on either backend
    // (famistudio_update's `song_speed == 0` early path, and an SDL3 stream
    // with no active voice), which is why it is unconditional, exactly as it
    // is in level.cpp's own loop.
    //
    // AND THE EFFECT IS PLAYED HERE because this is where the engine is
    // already mapped -- see ::ui::impl::pendingSfx for why the hooks that want
    // a sound cannot play one themselves. The two sit in ONE
    // CallPairedBlock rather than two: the pair is most of the cost, and
    // whatever the mailbox holds is wanted on this frame, not the next.
    //
    // Nothing in the block may touch ordinary banked code -- with both windows
    // switched, $8000-$BFFF is audio and nothing else (mmc3.hpp's
    // ::CallPairedBlock). Two calls into those banks and one volatile byte of
    // RAM is the whole of it.
    static void DriveAudio() {
        mmc3::CallPairedBlock<audio_tag, audio_data_tag>([] {
            audio::Update();
            if (const u8 queued = ui::impl::pendingSfx) {
                // DRAINED WHETHER OR NOT IT PLAYS, which is why the clear sits
                // above the gate rather than inside it. A posted effect
                // describes something that has ALREADY happened on screen; held
                // back while effects are off, it would fire the instant they
                // were switched on, announcing a menu move made minutes ago.
                ui::impl::pendingSfx = 0;
                if (ui::impl::SfxEnabled()) {
                    audio::SfxPlay(static_cast<u8>(queued - 1), ui::impl::kSfxChannel,
                                   ui::impl::SfxLevel());
                }
            }
        });
    }


    TITLE NI void main() {
        log("title::main: entered");

#if TARGET_GC
        // The GameCube boots 4:3 and is TOLD otherwise (video.hpp's
        // SetWidescreen), so the engine's aspect and the button's state bit are
        // two copies of one setting -- and the button is the one the player
        // sees, which makes it the authority. Synced once here, BEFORE kMenuNT
        // and every Make()/Draw() below resolve against the viewport, so the
        // first frame is already drawn at the width the page claims. A no-op
        // when the two already agree, which is the ordinary cold-boot case; it
        // earns its keep the moment the setting is restored from anywhere (a
        // memory card, a debug default) instead of starting at the scene's.
        video::ogc::SetWidescreen(gen::options::videoCanvas::enableWidescreenButton::Enabled());
#endif

        kMenuNT = video::viewport_tx() < 32 ? 32 : video::viewport_tx();

        oam::PopulateFromProvider(OAMBuffer, 0, oam::y, Clear, 64);

        // The options page's four corner sprites: tile and attributes, once,
        // never again. Their Y is whatever the clear above left -- parked
        // off-screen (::Clear) -- so none of them is on screen until something
        // places it.
        for (u8 i = 0; i < ui::impl::kCorners; i++) {
            OAMBuffer[kCornerSlot + i].tile       = ui::impl::kCornerTiles[i];
            OAMBuffer[kCornerSlot + i].attributes = ui::impl::kCornerAttributes;
        }
        pIRQ = irq_handler;
        pNMI = nmi_handler;
        log("title::main: pIRQ/pNMI set");

        // One CHR register per packed set. Which register is the game's own
        // knowledge now: under mmc3::chrMode::fineFirst, R2-R5 are the 1 KiB
        // windows at PPU $0000/$0400/$0800/$0C00 and R0/R1 the 2 KiB ones at
        // $1000/$1800, so each set below goes to the register serving the
        // address it was packed for. Change kChrMode and these move with it --
        // nothing checks that they did.
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::dynamicLevelTiles::__lma>()>(mmc3::chr2Control);   // R2, PPU $0000
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::staticLevelTiles::__lma>()>(mmc3::chr3Control);   // R3, PPU $0400
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::ui::__lma>()>(mmc3::chr3Control);   // R3, PPU $0400
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::normalSprites::__lma>()>(mmc3::chr0Control);   // R0, PPU $1000, 2 KiB

        // R1, PPU $1800, 2 KiB: the ui set a SECOND time, so its art is live
        // as sprites as well as in the background window R3 points at. Not a
        // move -- both registers hold it at once, and the same bytes answer to
        // two different tile indices (see windows::ui and windows::uiSprites).
        //
        // R1 being 2 KiB means two things. ui has to sit on a 2 KiB boundary
        // or the bank rounds down, which SwitchCHRBank's compile-time-bank
        // overload asserts; and ui being only 1 KiB leaves the upper half
        // ($1C00-$1FFF) showing whatever follows it in the image, which the
        // overload cannot see. So nothing may be packed after ui -- that
        // leaves the upper half blob padding.
        static_assert(blob::normalSprites::__lma + blob::normalSprites::__size <= blob::ui::__lma &&
                      blob::dynamicLevelTiles::__lma + blob::dynamicLevelTiles::__size <= blob::ui::__lma &&
                      blob::staticLevelTiles::__lma + blob::staticLevelTiles::__size <= blob::ui::__lma,
                      "something is packed after ui, so R1's upper 1 KiB "
                      "($1C00-$1FFF) shows that art instead of padding: give "
                      "it an entry of its own, or re-point R1");
        mmc3::SwitchCHRBank<mmc3::BankOf<blob::ui::__lma>()>(mmc3::chr1Control);

        constexpr u32 padLma = blob::normalSprites::__lma + blob::normalSprites::__size;
        static_assert(padLma + mmc3::kChrBankSize <= blob::dynamicLevelTiles::__lma,
                      "R5 runs past normalSprites into the next entry: give "
                      "PPU $0C00-$0FFF an entry of its own, or re-point it");
        mmc3::SwitchCHRBank<mmc3::BankOf<padLma>()>(mmc3::chr5Control);
        mmc3::SetMirroring(false);
        ppu::Flush((windows::ui + blob::ui::hud_whitespace), 0xff);
        ppu::pal::WriteFromBuffer(13, titleScreenColours, 3);
        // Sprite palette 1, the menu arrow's (ui/impl/singleChoice.hpp's
        // ::kSelectorAttributes). The same three colours BG_3 just took, for
        // the same reason it is excluded from ::kFadeEntries: the arrow is UI,
        // and the fade is there to take the preview away from under it. The
        // arrow parks instead of fading (::nmi_handler_lockOptions), which is
        // what the background tile's erase used to do.
        ppu::pal::WriteFromBuffer(ppu::SPRITE_1 + 1, titleScreenColours, 3);

        log("title::main: before DrawLevelPreview");
        DrawLevelPreview();
        log("title::main: after DrawLevelPreview");

        BuildPaletteFadePhases();

        // The scene's own top-level Make(): every node's rect is resolved
        // against the viewport THIS run has and its pos to a PPU address. uitk
        // emits it on every target -- an empty stub where every node is already
        // `constexpr` (NES) -- so no #if is needed here, not even for the
        // GameCube-only widescreen button, which is simply not in the scene on
        // any other target.
        //
        // The whole scene, NOT its two Canvases: a Canvas's Make() reaches only
        // what is under it, and optionsTitle/videoOptions/superMarySistersText/
        // audioText are not -- they were left unresolved on every
        // runtime-viewport target, holding rect/pos zeroes. Nothing reads them
        // today (their Draw()s compute their own coordinates inline, which is
        // why it never showed), but "nothing may read a node's rect/pos until
        // Make() has run" is the generator's rule and this is where it is kept.
        //
        // It stays here, not in ::DrawOptionsPanel: it resolves anchors and
        // zeroes running state, which must happen once, not on every mode
        // entry.
        // THE LOADED SETTINGS, LIFTED OUT FROM UNDER Make(). Every control on
        // the options page IS a field of ::saveData.config -- uitk's
        // bss_prefix/Symbol Prefix arrangement, see demo/src/save.hpp -- and
        // every component's Make() writes its AUTHORED DEFAULT into that same
        // field (`button_state[0] |= 1 << n`, `amount = 0`; see any
        // demo/gen/options/<target>/options.hpp). The two cannot both be in
        // one byte, so the file's copy comes out first and goes back after.
        //
        // A COPY AND NOT A POINTER, and not a re-read either: there is nowhere
        // else the config exists. ::save::Load wrote it into this struct and
        // Make() is about to overwrite it in place, so the only thing standing
        // between a loaded setting and the default is this local.
        const SaveFile::Config loadedConfig = saveData.config;

        // ON EVERY TARGET NOW, INCLUDING THE NES, and the NES is the one that
        // was wrong. Nothing there CLEARS the save -- `.cartmem` sits outside
        // the range crt0's memset covers -- but `gen::options::Make()` below
        // overwrites it a few hundred cycles later, with six `ora`s that force
        // every authored default back on. The symptom was a cart that forgets
        // its settings on every power cycle, and the cause was this restore
        // being compiled out of the one platform whose storage survives.
        //
        // WHERE THE STATE CAME FROM IS NOT THIS CODE'S CONCERN: a card
        // transfer off NES, the cart's own battery-backed SRAM on it, or the
        // defaults ::pregame::main seeded before it went looking. By the time
        // this runs they are all just the contents of ::saveData.config --
        // nothing to consult,
        // nothing that can have failed. The copy always goes back -- on every
        // target, unconditionally. There is nothing left to ask: whatever is in
        // ::saveData.config when this runs is the state, whether it came off a
        // card, out of BSS, or out of the cart's SRAM, and the defaults the
        // sliders need are seeded by ::pregame::main BEFORE its load rather
        // than chosen here afterwards.
        gen::options::Make();

        // The file wins over the authored defaults -- which is the whole of "a
        // saved option reflects in the UI": every Draw() below dispatches on
        // these same bytes (ui/impl/buttons.hpp's `Enabled() ? Enabled :
        // Disabled`, sliders.hpp's handle position), so putting them back
        // before ::DrawOptionsPanel is what paints the page the way the player
        // left it.
        saveData.config = loadedConfig;

        // EVERYTHING BELOW IS ABOUT SLIDERS, which is why it stops at the NES:
        // that target's scene has none (options.uis hides both, leaving the two
        // DISABLE buttons as its whole audio control), so the namespaces these
        // name do not exist there. The config copy above is the entire restore
        // on NES.
#ifndef TARGET_NES
        namespace ac = gen::options::audioCanvas;
        const Rect mr = ac::musicSlider::rect;
        const Rect sr = ac::SFXSlider::rect;

        // A STORED HANDLE POSITION IS NOT PORTABLE BETWEEN TRACK WIDTHS, and
        // the save holds the position rather than the proportion: a track is
        // ::SliderTravel(rect width) cells wide, the rect comes from
        // ::video::viewport_tx(), and that moves with the GameCube's aspect
        // (6 cells at 4:3, 9 at 16:9) and with a desktop window. Normally the
        // two agree -- ::pregame::ApplyLoadedConfig has already restored the
        // aspect this save was written at, which is exactly why it runs before
        // any of this -- but a save carried over from a build whose scene was a
        // different shape would otherwise park a handle past the end of its own
        // track and draw it outside the widget.
        //
        // Clamped rather than rescaled, because the position is all the file
        // has: with no record of what travel it meant, the nearest legal cell
        // is the most this can honestly recover.
        //
        // IT IS ALSO WHAT RESOLVES "FULL VOLUME" ON A COLD BOOT, and that is
        // why this runs unconditionally now rather than only for a loaded save.
        // ::pregame::main parks both amounts at 0xFF before it goes looking for
        // a card -- "the right-hand end, whatever that turns out to be" -- which
        // no code there could compute, since the track's width comes from a rect
        // that ::gen::options::Make() above has only just resolved. A real save
        // overwrites that 0xFF during the load; nothing overwrites it when
        // there is no save; either way the clamp below lands it on a legal cell.
        const u8 mTravel = ui::impl::SliderTravel(mr.size.x);
        const u8 sTravel = ui::impl::SliderTravel(sr.size.x);
        if (mTravel && ac::musicSlider::amount >= mTravel) {
            ac::musicSlider::amount = static_cast<u8>(mTravel - 1);
        }
        if (sTravel && ac::SFXSlider::amount >= sTravel) {
            ac::SFXSlider::amount = static_cast<u8>(sTravel - 1);
        }

        // The engine told what the handle says: nothing else ever does.
        // ::TrackVolume is only otherwise reached from the slider's own
        // DrawChange (ui/impl/sliders.hpp), i.e. from a MOVE -- so a volume
        // that was merely loaded, or merely defaulted, would leave the mix
        // wherever the backend booted while the handle claimed otherwise.
        audio::TrackVolume(ui::impl::SliderPercent(mr.size.x, ac::musicSlider::amount));
#endif

        // Dual-screen: left unloaded until OPTIONS is picked (::ClearUiBand).
        if constexpr (!kUiOnOwnPanel) DrawOptionsPanel();

        InitTitleScreen();

        // Make() makes each member, snapshots their rects into `options`, and
        // zeroes the menu's running option -- so it has to run here rather
        // than at static-init time. Scene-level again, for ::gen::options'
        // reason above: gen::title::Make() also makes GameTitle, which
        // TitleOptions::Make() does not reach.
        gen::title::Make();
        gen::gameOptions::Make();
        playModeActive = false;

        gen::title::TitleOptions::Draw();
        DrawTitleSelector();

        ppu::SetScroll({0, 0xff});
        // No SPRITE_SIZE: title mode runs 8x8 sprites, unlike level mode,
        // which sets it in its own EnableRendering. SPRITE_ADDR therefore
        // means something here -- in 8x16 it is ignored -- and selects the
        // $1000 pattern table the sprite art is banked into.
        ppu::EnableRendering(ppu::ctrl::SPRITE_ADDR | ppu::ctrl::GEN_NMI, ppu::mask::BG_L | ppu::mask::SPRITE_L);
        irq::EnableInterrupts();
        log("title::main: entering input loop");

        // SEEDED FROM THE PAD, NOT FROM ZERO, AND THAT IS A BUG FIX RATHER THAN
        // A TIDY-UP. `pressed` below is an edge -- `inputs & ~prevInputs` -- so
        // a seed of 0 declares every button CURRENTLY HELD to have gone down on
        // this loop's first frame. Nothing in this mode presses its own buttons,
        // but the mode that hands over to it does: ::pregame::ChoiceWheel breaks
        // out of its wheel on the A edge WITHOUT waiting for the release, so A
        // is still down when ::pregame::main sets ::gameMode and returns. This
        // loop's first frame then read that held A as a fresh press and
        // confirmed whatever the title menu had highlighted -- so answering
        // "CONFIRM USE DEVICE" (or any other wheel) at boot dropped the player
        // straight past NEW GAME / CONTINUE / OPTIONS into the player-count
        // screen, having never seen the menu.
        //
        // One poll, discarded, is the whole fix: a button held when this loop
        // starts is not a press, which is the same rule ::pregame::ChoiceWheel
        // already seeds itself by. It fixes the hand-off from every predecessor
        // at once -- world and level mode also return here with buttons down --
        // rather than making each one drain its own confirm on the way out.
        u8 seed1 = 0, seed2 = 0;
        input::PollControllers(&seed1, &seed2);
        u8 prevInputs = static_cast<u8>(seed1 | seed2);
        while (true) {
            u8 port1, port2;
            input::PollControllers(&port1, &port2);
            const u8 inputs  = port1 | port2;
            const u8 pressed = inputs & static_cast<u8>(~prevInputs); // strobe: only the frame a button goes down
            prevInputs = inputs;
            // Every frame, whatever mode this is -- see ::CanvasRepeat.
            const u8 repeated = CanvasRepeat(inputs, pressed);

            if (!optionsLocked) {
                // Pass() repaints the arrow itself now, through the menu's own
                // DrawChange() hook -- so there is no snapshot-and-compare to
                // do here, and no arrow address for this loop to work out.
                // Which menu is live is still its business, because the two
                // are alternative content for the same rows.
                if (playModeActive) gen::gameOptions::gameOptions::Pass(pressed);
                else                gen::title::TitleOptions::Pass(pressed);

                if (pressed & input::A) {
                    // The SingleChoice confirm, for both menus that share
                    // these rows: A is what picks the option the arrow is on,
                    // whichever of the two is live. Posted before the branch
                    // rather than per case, because it is the GESTURE that
                    // makes the sound and every case below is the same one --
                    // uitk has no Confirm() hook to carry it, since what a
                    // choice MEANS is entirely the scene's (see
                    // ui/impl/singleChoice.hpp).
                    ui::impl::PostSfx(sfx::select);

                    if (playModeActive) {
#ifdef PLAYER2_SUPPORTED
                        level::multiplayer = gen::gameOptions::gameOptions::option != 0;
#endif
                        ppu::PPUMASK = 0;
                        gameMode = eGameModes::Level;
                        // Drained before leaving, because nothing in level
                        // mode spends this mailbox: the effect above would
                        // otherwise sit in it and sound on the next visit to
                        // the title screen. Started here and heard there --
                        // level::main ticks the same engine every frame, so a
                        // stream written now plays out over the handover.
                        DriveAudio();
                        return;
                    }

                    switch (gen::title::TitleOptions::option) {
                        case NewGame:
                        case Continue:
                            playModeActive = true;
                            pNMI = nmi_handler_drawPlayMode;
                            break;

                        case Options:
                            fadePhase = 0;
                            fadeFrameCounter = 0;
                            optionsLocked = true;
                            pNMI = nmi_handler_fadeToOptions;
                            break;

#if defined(TARGET_MACOS) || defined(TARGET_WINDOWS) || defined(TARGET_LINUX)
                        case Quit:
                            quit = true;
                            return;
#endif

                        default: ;
                    }
                }

                // B backs out of the play-mode menu, and sounds like it. The
                // guard is the whole condition, not just the sound's: B on the
                // title menu itself has nothing to back out TO, and a
                // deselection effect on a press that changed nothing would be
                // the one place in this page where the sound and the screen
                // disagree.
                if (pressed & input::B && playModeActive) {
                    ui::impl::PostSfx(sfx::deselect);
                    playModeActive = false;
                    pNMI = nmi_handler_drawMenu;
                }
            } else if (pNMI == nmi_handler_locked) {
                // The options page, and only once it has settled: every branch
                // below either places the corners or repaints a component, and
                // both resolve against the resting scroll, which is not where
                // the frame is until this handler has the screen.
                //
                // Two levels, nested the way the title and play-mode menus
                // already are. At the column level LEFT/RIGHT pick a Canvas and
                // A hands it the corners; inside a Canvas every press goes to
                // its Pass() and B comes back out. B at the column level leaves
                // the page.
                //
                // LEFT/RIGHT belong to the column level alone, and that is what
                // the two levels buy: both Canvases are vertical, so each
                // spends UP/DOWN on its own focus and passes LEFT/RIGHT through
                // to the focused component -- which is how a horizontal slider
                // is adjusted. Sharing those two bits between the column switch
                // and the sliders is the one thing this layout cannot do, so it
                // doesn't: outside a Canvas they switch columns, inside one they
                // reach the slider.
                //
                // A click is offered the page FIRST, and only on a frame the
                // controller said nothing: the two are alternative ways to
                // reach the same controls, and a frame that carries both is a
                // player whose thumb and finger disagree. Deferring to the
                // controller is the safer of the two readings -- a press is
                // unambiguous about which control it means (whatever has
                // focus), where a click still has to be resolved against a
                // layout.
                //
                // `repeated` is in the guard as well as `pressed`. A held
                // direction walking a slider's handle produces no strobe on
                // most of its frames (see ::CanvasRepeat), so testing the
                // strobe alone would let a stray touch land in the middle of a
                // deliberate adjustment.
                //
                // Posted, not spent: ::SpendCanvasClick does the work in
                // vblank. See ::canvasClickPending.
                //
                // `if constexpr` so this costs nothing at all on a target with
                // no pointing device -- `clickPosted` is then a false constant
                // and the branch below is the only one left (pnes/input.hpp,
                // ::input::has_click).
                bool clickPosted = false;
                if constexpr (input::has_click) {
                    if (!pressed && !repeated && input::click.Down()) {
                        const vec2<u16> at = ClickTile();
                        canvasClickX = at.x;
                        canvasClickY = at.y;
                        canvasClickPending = true;
                        clickPosted = true;
                    }
                }

                if (clickPosted) {
                    // Nothing more this frame: the click owns it.
                } else if (!canvasEntered) {
                    if (pressed & (input::LEFT | input::RIGHT)) {
                        void (* const want)(u8) = (pressed & input::LEFT)
                                                      ? gen::options::videoCanvas::Pass
                                                      : gen::options::audioCanvas::Pass;
                        // GUARDED ON A REAL CHANGE, which is the whole reason
                        // the switch is expressed as a comparison now: LEFT
                        // pressed while the left column already has the corners
                        // moves nothing, and an effect there would have the page
                        // announce a move a player did not make -- the same
                        // guard ui/impl/singleChoice.hpp's DrawChange hooks keep,
                        // and for the same reason. ::PlaceOptionsCorners is
                        // skipped with it: it re-brackets whichever column is
                        // selected (::SelectedCanvasRect), so with no change
                        // there is nothing for it to move either.
                        if (want != pOptionsPass) {
                            pOptionsPass = want;
                            // sfx::selectnext, the same effect a focus move
                            // INSIDE a Canvas makes (ui::impl::UpdateCanvas):
                            // stepping between the columns and stepping between
                            // a column's components are one gesture to a player,
                            // and only a slider's value has a direction worth
                            // hearing differently.
                            ui::impl::PostSfx(sfx::selectnext);
                            PlaceOptionsCorners();
                        }
                    } else if (pressed & input::A) {
                        canvasEntered = true;
                        SelectFocusedComponent();
                    } else if (pressed & input::B) {
                        // Leaving the page -- the outermost of the three
                        // things B undoes here, and the same effect for all
                        // three: B is one gesture, and which level of the page
                        // it backed out of is not something a player needs
                        // told in a different sound.
                        ui::impl::PostSfx(sfx::deselect);
#ifdef SAVE_DEVICE
                        // ...and the one place the page's settings are written
                        // out. Every control on it writes straight into
                        // saveData.config (demo/src/save.hpp's SaveFile), so
                        // leaving the page is the moment the file and the
                        // screen stop agreeing, and the moment there is
                        // something to save. ::pregame::SaveScreen writes it
                        // and runs its own RETRY / CONTINUE wheel if the device
                        // will not take it -- blocking with SAVING on screen
                        // only where a write is slow enough to be worth saying
                        // so (::SAVE_DEVICE_WAIT), and drawing nothing at all
                        // where it is not.
                        //
                        // ::SAVE_DEVICE AND NOT TARGET_GC, which is what
                        // makes a Wii -- and now a Switch -- remember its
                        // settings. With this gated to the GameCube, a Wii
                        // player could change anything on this page and
                        // nothing was ever written: the save existed,
                        // ::pregame had loaded it, and the one call that writes
                        // it back was compiled out -- so every boot came up on
                        // the scene's defaults. THAT IS THE BUG THE CAPABILITY
                        // MACRO EXISTS TO STOP HAPPENING A THIRD TIME: every
                        // target with somewhere to write reaches this line
                        // because the gate is the list, rather than a platform
                        // that has to be remembered here. All of them have the
                        // same wheel to offer when the write will not go.
                        //
                        // THE CORNERS GO FIRST, because the save screen is
                        // drawn in a different quadrant and these sprites are
                        // in screen space: left where they are they would
                        // bracket a column that is no longer on screen. The
                        // unwind parks them again on its own first frame
                        // (::nmi_handler_lockAnimReverse), so this is only
                        // about the frames in between.
                        //
                        // The posted deselection survives it: ::pregame's own
                        // frame pump spends the same mailbox this does, so the
                        // sound plays over the save screen rather than being
                        // dropped.
                        //
                        // The return value is deliberately unused. A device
                        // that cannot be written is the player's answer to make
                        // -- they have already chosen CONTINUE in that wheel --
                        // and refusing to leave the page would trap them in it
                        // with nothing in the console to save to.
                        ParkOptionsCorners();
                        (void)pregame::SaveScreen();

                        // THE SAME STROBE SEED THIS LOOP OPENS WITH, for the
                        // same reason, at the other hand-off. ::SaveScreen runs
                        // its own frames and its own RETRY / CONTINUE wheel,
                        // which answers on an A edge and returns WITHOUT
                        // waiting for the release -- so this loop's
                        // ::prevInputs is whatever was held before the save
                        // screen, and a button the wheel consumed would read as
                        // a fresh press on the frame after it returns.
                        // Re-seeding is how the frames it spent are accounted
                        // for; nothing held across it counts as new.
                        {
                            u8 back1 = 0, back2 = 0;
                            input::PollControllers(&back1, &back2);
                            prevInputs = static_cast<u8>(back1 | back2);
                        }
#endif
                        // AFTER the save screen, not before: that call hands
                        // the NMI vector back to whatever it found here, so an
                        // assignment made first would be handed straight back
                        // to ::nmi_handler_locked and the page would never
                        // unwind.
                        pNMI = nmi_handler_lockAnimReverse;
                    }
                } else if (pressed & input::B) {
                    // Back to the column, corners with it. The Canvas keeps its
                    // focus index, so A lands on the same component again.
                    //
                    // The corners are re-placed through ::PlaceOptionsCorners,
                    // which is deliberately the SILENT path onto these sprites
                    // -- so this frame makes one sound, the deselection, and
                    // not a selection of the column on top of it (see
                    // ui/impl/canvas.hpp's ConfirmComponent).
                    ui::impl::PostSfx(sfx::deselect);
                    canvasEntered = false;
                    PlaceOptionsCorners();
                } else {
                    // The Canvas's payload: the plain strobe for every bit but
                    // LEFT/RIGHT, and the repeat in their place. ::CanvasRepeat
                    // already returns the strobe on the press frame, so this
                    // neither doubles a press nor drops one.
                    if (const u8 payload = static_cast<u8>(
                            (pressed & static_cast<u8>(~kCanvasRepeatBits)) | repeated); payload) {
                        canvasInput = payload;
                    }
                }
            }

            video::WaitForPresent();
            {
                static bool firstPresentLogged = false;
                if (!firstPresentLogged) {
                    firstPresentLogged = true;
                    log("title::main: first WaitForPresent returned");
                }
            }
            if (quit) return;

            // After ::WaitForPresent, so the effects the NMI handler's own
            // spenders posted on this frame (::SpendCanvasInput /
            // ::SpendCanvasClick, which are vblank work) are played on it
            // rather than a frame late -- and so are the ones posted by the
            // mainline above, which ran earlier in the same iteration.
            DriveAudio();
        }
    }

    static void ArmSplitIRQ() {
        ArmSplitIRQFor(SplitRow());
    }

    void nmi_handler() {
        ui::impl::AnimateSelector();
        oam::RefreshSprites(OAMBuffer);

        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();

        ArmSplitIRQ();
    }

    // Both swap handlers: swap the two menus' labels, then put the incoming
    // menu's arrow where its option is.
    //
    // The outgoing arrow needs no wipe. It used to: the background tile could
    // sit inside the incoming menu's text, so it had to be cleared BEFORE the
    // redraw or the clear would punch a hole in it. A sprite is not in the
    // nametable at all, has exactly one position, and the DrawChange() below
    // moves it -- so there is nothing to clear and no ordering to get right.
    static void nmi_handler_drawPlayMode() {
        gen::title::TitleOptions::Erase();
        gen::gameOptions::gameOptions::Draw();
        DrawPlayModeSelector();
        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    static void nmi_handler_drawMenu() {
        gen::gameOptions::gameOptions::Erase();
        gen::title::TitleOptions::Draw();
        DrawTitleSelector();
        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();
        ArmSplitIRQ();

        pNMI = nmi_handler;
    }

    static u8 FadeStep(const u8 colour) {
        if ((colour & 0xf0) == 0) return 0x1f;
        return static_cast<u8>(colour - 0x10);
    }

    // Fills ::paletteFadePhases: phase 0 is the title screen's live, non-HUD
    // palette (read from the same ROM arrays DrawLevelPreview uploads it
    // from -- BGColours/maryColors live in the LEVEL_GRAPHICS bank, same
    // reason DrawLevelPreview itself reads them inside CallLevelGraphics),
    // every later phase is the previous one run through ::FadeStep once.
    static void BuildPaletteFadePhases() {
        CallLevelGraphics([] {
            u8* dst = paletteFadePhases[0];
            *dst++ = BGColours[0]; *dst++ = BGColours[1]; *dst++ = BGColours[2]; *dst++ = BGColours[3];
            *dst++ = BGColours[5]; *dst++ = BGColours[6]; *dst++ = BGColours[7];
            *dst++ = maryColors[0]; *dst++ = maryColors[1]; *dst++ = maryColors[2];
        });
        for (u8 phase = 1; phase < kFadePhaseCount; ++phase) {
            for (u8 i = 0; i < sizeof(paletteFadePhases[0]); ++i) {
                paletteFadePhases[phase][i] = FadeStep(paletteFadePhases[phase - 1][i]);
            }
        }
    }

    // Writes one precomputed phase out via ::ppu::pal::WriteFromBuffer (plain
    // RAM source -- no LEVEL_GRAPHICS switch needed here, unlike
    // ::BuildPaletteFadePhases).
    static void WritePaletteFadePhase(const u8 phase) {
        const u8* src = paletteFadePhases[phase];
        for (const FadeRun& run : kFadeEntries) {
            ppu::pal::WriteFromBuffer(run.offset, src, run.count);
            src += run.count;
        }
    }

    // Steps ::fadePhase one phase darker and writes it out. Returns false
    // once ::fadePhase has reached the fully-black last phase.
    static bool StepPaletteFade() {
        if (fadePhase >= kFadePhaseCount - 1) return false;
        WritePaletteFadePhase(++fadePhase);
        return fadePhase < kFadePhaseCount - 1;
    }

    // ::StepPaletteFade's mirror: steps ::fadePhase one phase brighter
    // (toward phase 0, the original colours) and writes it out. Returns
    // false once ::fadePhase is back at 0.
    static bool StepPaletteFadeIn() {
        if (fadePhase == 0) return false;
        WritePaletteFadePhase(--fadePhase);
        return fadePhase != 0;
    }

#if TARGET_GC
    // Flips the GameCube's display aspect and rebuilds the screen around it.
    //
    // ::video::ogc::SetWidescreen moves ::video::viewport_tx() -- 40 columns at
    // 4:3, 52 at 16:9 -- and the viewport width IS the nametable page stride
    // (::video::nametable_plane_bytes, ::emu::ComputeNtGeometry), so the
    // instant it moves every byte already in the plane is at the wrong offset.
    // Nothing there is salvageable and nothing needs to be: this redraws the
    // screen the way ::main first drew it. The ALLOCATION never moves --
    // irq::init() sizes VRAM for the wider of the two aspects -- so nothing
    // anything holds here goes stale. See the @warning on SetWidescreen's
    // declaration; this function is the demo's half of that contract.
    //
    // Reached only from ::nmi_handler_locked, i.e. the options page at rest,
    // which is the one state the button can be pressed in. That is what settles
    // what has to come back here and what must not: in this state the title
    // menu's labels have been ERASED and its arrow parked
    // (::nmi_handler_lockOptions), so neither is restored here --
    // ::nmi_handler_lockAnimReverse still owns putting them back on the way
    // out, and drawing them now would put an arrow over the page.
    static void ApplyWidescreenChange(const bool on) {
        // Rendering off across the re-stride: from the switch to the last write
        // below, every address in the plane means something other than what is
        // on screen. PPUMASK is SAVED AND RESTORED rather than re-derived --
        // ::main's EnableRendering is not the last thing to have set it by the
        // time the page is up, and what the page's rendering state should be is
        // not this function's decision to make.
        const u8 mask = ppu::PPUMASK;
        ppu::PPUMASK = 0;

        video::ogc::SetWidescreen(on);

        // Everything from here down re-derives against the new viewport.
        kMenuNT = video::viewport_tx() < 32 ? 32 : video::viewport_tx();

        ppu::Flush((windows::ui + blob::ui::hud_whitespace), 0xff);

        // Re-resolve every UI anchor, through each scene's OWN top-level
        // Make() -- the one the generator documents as "nothing may read a
        // node's rect/pos until this has run". Not the two Canvas Make()s:
        // those reach only the nodes UNDER a Canvas, and this scene has four
        // outside them (optionsTitle, videoOptions, superMarySistersText,
        // audioText), as gen::title has GameTitle. Picking a subset by hand is
        // what leaves a node holding anchors for the viewport that is gone,
        // and it is not a subset anything here should be curating.
        //
        // Each Make() also ZEROES its scene's running state, which must not
        // happen here: the player is standing on a focused component in a
        // selected column, and the display changing shape is no reason to throw
        // their place in the menu away. The title and play-mode menus are
        // off-screen in this state, but their rects are just as stale, and
        // their option is what the player picked on the way in -- so both go
        // through the same save/restore.
        // The mix sliders' PROPORTION, not their handle positions: slider
        // travel is derived from the rect, which is derived from the viewport
        // width, so the same `amount` means a different fraction either side of
        // this switch (6 positions at 4:3, 9 at 16:9). Captured before the
        // re-Make below and restored onto the new track after it -- otherwise a
        // track the player had set to 100% would come back reading 62% and the
        // music would quietly drop with the aspect.
        namespace ac = gen::options::audioCanvas;
        const u8 musicPct = [] {
            const Rect r = ac::musicSlider::rect;
            return ui::impl::SliderPercent(r.size.x, ac::musicSlider::amount);
        }();
        const u8 sfxPct = [] {
            const Rect r = ac::SFXSlider::rect;
            return ui::impl::SliderPercent(r.size.x, ac::SFXSlider::amount);
        }();

        const u8 videoIndex  = gen::options::videoCanvas::index;
        const u8 audioIndex  = gen::options::audioCanvas::index;
        const u8 titleOption = gen::title::TitleOptions::option;
        const u8 playOption  = gen::gameOptions::gameOptions::option;

        // EVERY BUTTON'S BIT, and this was the bug that made this button a
        // one-way door. The four restores around it are of state that lives
        // OUTSIDE the save file (a Canvas's focus index, a menu's option); the
        // three buttons live INSIDE it, in ::saveData.config, and every
        // component's Make() below rewrites its own field with the default the
        // scene was authored with. enableWidescreenButton's default is OFF --
        // so the press that got here set the bit, this re-Make cleared it, and
        // the player was left looking at a 16:9 screen whose button said
        // ENABLE WIDESCREEN and whose next press re-applied the aspect it was
        // already in. 4:3 was unreachable for the rest of the session.
        //
        // The whole config in one copy rather than the one bit that caused it:
        // the same reasoning holds for every field uitk puts in there, and a
        // list maintained by hand here is a list that goes stale the next time
        // a control is added to the scene.
        const SaveFile::Config cfg = saveData.config;

        gen::options::Make();
        gen::title::Make();
        gen::gameOptions::Make();

        saveData.config = cfg;
        gen::options::videoCanvas::index  = videoIndex;
        gen::options::audioCanvas::index  = audioIndex;
        gen::title::TitleOptions::option  = titleOption;
        gen::gameOptions::gameOptions::option = playOption;

        // The proportions back onto the new, wider or narrower tracks. The
        // handle snaps to the nearest cell, so the percentage the engine is
        // given is the one the UI will read out after the snap -- not the one
        // before it, which the handle would then disagree with.
        {
            const Rect mr = ac::musicSlider::rect;
            const Rect sr = ac::SFXSlider::rect;
            ac::musicSlider::amount = ui::impl::SliderAmountFor(mr.size.x, musicPct);
            ac::SFXSlider::amount   = ui::impl::SliderAmountFor(sr.size.x, sfxPct);
            audio::TrackVolume(ui::impl::SliderPercent(mr.size.x, ac::musicSlider::amount));
        }

        // The preview, the logo and the panel, in ::main's own order.
        //
        // ::DrawLevelPreview is written for a COLD START, so the two things it
        // does for one have to be undone again here -- it is the whole of the
        // preview's construction and splitting those out of it would only move
        // the problem to the one caller that does want them.
        //
        // It uploads the UNFADED palettes, so the fade goes back over the top:
        // ::fadePhase is at the fully-black end in this state and the preview
        // has to stay black behind the page. The three runs it writes (BG_0,
        // BG_1 colours 1-3, SPRITE_0 colours 1-3) are exactly ::kFadeEntries,
        // so the re-write covers all of it with nothing left bright.
        //
        // And it finishes by PLACING the characters on the ground, which is
        // precisely what the options page is not supposed to show: they were
        // parked for the whole page in ::nmi_handler_fadeToOptions, ONCE --
        // nothing re-parks them per frame -- and ::nmi_handler_locked refreshes
        // OAM immediately after this returns, so leaving them placed puts Mary
        // back on screen over the page. (Palette-black would not have saved it
        // either: the fade covers her colours 1-3, not the backdrop.) No
        // ::kUiOnOwnPanel guard on the park, unlike the two existing call
        // sites: the GameCube is single-screen, so the page IS the whole frame
        // and this branch is the only one that exists here.
        DrawLevelPreview();
        WritePaletteFadePhase(fadePhase);
        ParkStandingSprites();
        InitTitleScreen();
        DrawOptionsPanel();

        // The corners last, onto the rects the Make()s above just re-resolved,
        // and in whichever of the page's two states it is actually in -- the
        // same two calls ::nmi_handler_locked's own input paths choose between.
        if (canvasEntered) SelectFocusedComponent();
        else               PlaceOptionsCorners();

        ppu::PPUMASK = mask;
    }
#endif

    static void nmi_handler_fadeToOptions() {
        ui::impl::AnimateSelector();
        oam::RefreshSprites(OAMBuffer);
        mmc3::AcknowledgeScanlineIRQ();

        // ::kUiOnOwnPanel: no fade, and the palette is left untouched -- the
        // state machine advances on this very frame instead.
        bool stillFading = false;
        if constexpr (!kUiOnOwnPanel) {
            stillFading = true;
            if (++fadeFrameCounter >= kFadeStepFrames) {
                fadeFrameCounter = 0;
                stillFading = StepPaletteFade();
            }
        }

        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();

        // Armed, not merely acknowledged. mmc3::AcknowledgeScanlineIRQ is the
        // DISABLE half -- it clears a pending IRQ and leaves generation off --
        // so the ack at the top of this handler, with no ScheduleScanlineIRQ to
        // follow it, used to mean this frame simply had no split: one bank
        // setting for all 240 lines. That was survivable while $0400 held one
        // entry; now that staticLevelTiles and ui share the window it is the
        // visible break when OPTIONS is picked, and specifically a break in the
        // menu band, because ::kFadeEntries deliberately leaves BG_3 out of the
        // fade -- the band below the split stays fully lit the whole way down
        // while BankPreviewTiles has it drawing the preview's tiles.
        ArmSplitIRQ();

        if (!stillFading) {
            // Fully black now, so the characters go off-screen for the whole
            // options page rather than sitting under it: the lock animation
            // below scrolls the ground out from under them, and nothing
            // blanks OAM on its own.
            if constexpr (!kUiOnOwnPanel) ParkStandingSprites();
            pNMI = nmi_handler_lockOptions;
        }
    }

    static void nmi_handler_lockOptions() {
        // Parked, not faded: the labels it points at are being erased on this
        // very frame, and the arrow's palette is deliberately outside the fade
        // (see the ::SPRITE_1 load in ::main). ::nmi_handler_lockAnimReverse
        // places it again when the menu comes back.
        ui::impl::ParkSelector();
        gen::title::TitleOptions::Erase();
        mmc3::AcknowledgeScanlineIRQ();
        oam::RefreshSprites(OAMBuffer);

        // Swap the band over to the options page: clear whatever the title UI
        // left, then draw the page -- which has not been in the nametable at
        // all until now. See ::ClearUiBand.
        if constexpr (kUiOnOwnPanel) {
            ClearUiBand();
            DrawOptionsPanel();
        }

        lockAnimScanline = static_cast<u16>(SplitRow()) << 3;
        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();

        // Same reason as ::nmi_handler_fadeToOptions' arm, and it belongs here
        // at the end rather than beside the ack above: Erase_TitleOptions runs
        // a nametable upload this handler cannot afford to have a split land in
        // the middle of, which is what the ack is for. Arming once those writes
        // are done keeps the band below the split on ui tiles for this frame
        // without exposing them.
        ArmSplitIRQ();

        // ::kUiOnOwnPanel: there is no lock ANIMATION to run. It slides the
        // single-screen split band, and here the UI has its own panel, so
        // ::PreviewScrollYForPixels is a constant and nothing would move --
        // every frame of it is a dead frame, which is the delay entering and
        // leaving options. Skip to the locked state; the scroll/bank/arm this
        // handler just did is exactly what that state wants.
        if constexpr (kUiOnOwnPanel) {
            lockAnimScanline = 0;
            // The page is up as of this frame, so the corners go on it now --
            // around whichever column was selected last time, which
            // ::pOptionsPass still holds (it outlives every visit).
            PlaceOptionsCorners();
            pNMI = nmi_handler_locked;
            return;
        }

        pNMI = nmi_handler_lockAnim;
    }

    static void nmi_handler_lockAnim() {
        ui::impl::AnimateSelector();
        oam::RefreshSprites(OAMBuffer);

        lockAnimScanline = lockAnimScanline > kScanlinesPerFrame
            ? static_cast<u16>(lockAnimScanline - kScanlinesPerFrame) : 0;

        // ui for the WHOLE frame, and no arm: the band above the split is
        // mid-scroll between the preview's rows and the options panel's (both
        // live in the same nametable -- SetMirroring(false) makes options.uis'
        // nametable 2 a mirror of the preview's 0), so one window cannot serve
        // both, and ::ApplySplit's scroll is written against the FIXED
        // ::SplitRow, which is not where this frame's split is. ui is the half
        // that matters: ::nmi_handler_fadeToOptions has already faded BG_0-BG_2
        // to black, so the preview rows render black whatever tiles they fetch,
        // while the options panel is drawn in the one palette the fade skips.
        ppu::SetScroll({0, PreviewScrollYForPixels(lockAnimScanline)});
        BankMenuTiles();

        // The scroll has finished winding up, so the page is now where
        // ::PlaceOptionsCorners resolves it to be -- see the same call in
        // ::nmi_handler_lockOptions, which is the dual-screen path to here.
        if (lockAnimScanline == 0) {
            PlaceOptionsCorners();
            pNMI = nmi_handler_locked;
        }
    }

    static void nmi_handler_locked() {
        ui::impl::AnimateSelector();
        // The only state the selection ring is on screen in: it is placed on
        // the handover into this handler and parked on the way out
        // (::nmi_handler_lockAnimReverse), so this is the one place a frame of
        // its hone belongs.
        ui::impl::AnimateCorners();
        // Before the upload, so a focus move's corners and a component's own
        // repaint both land on the frame the press was made.
        SpendCanvasInput();
        // Beside the press for the same reason the press is here: a click
        // repaints a component's own tiles, and on NES that is vblank work.
        // After it, so a frame carrying both spends the press first -- which
        // cannot happen (the loop posts at most one of the two, see ::main)
        // but costs nothing to make true in the order as well.
        SpendCanvasClick();
        oam::RefreshSprites(OAMBuffer);

        // ::kUiOnOwnPanel: the options panel is NOT the whole frame here. It
        // sits on the bottom panel (options_ds.uis anchors it at plane row 30),
        // and the level preview is still fully visible on the top one -- there
        // is no fade on this path to black it out. So the raster is still
        // required: preview bank for the top panel, and the split at the panel
        // boundary switches to ui for the bottom. Banking ui for the whole
        // frame -- correct on one screen, where the preview really is black --
        // is what drew the level area out of the UI bank.
        if constexpr (kUiOnOwnPanel) {
            ppu::SetScroll({0, PreviewScrollY()});
            BankPreviewTiles();
            ArmSplitIRQ();
            return;
        }

        ppu::SetScroll({0, PreviewScrollYForPixels(0)});

        // The options panel is the entire visible frame here, top to bottom,
        // and nothing re-arms the split -- so one bank setting is both correct
        // and all there is. See ::nmi_handler_lockAnim.
        BankMenuTiles();
    }

    static void nmi_handler_lockAnimReverse() {
        ui::impl::AnimateSelector();
        // Off as the page starts moving away, on the same frame the scroll
        // first steps back towards the title screen -- the corners bracket
        // rows that are no longer the page's, exactly as a stale arrow points
        // at rows that are no longer a menu's (::ParkSelector). What they
        // bracketed is not forgotten: ::pOptionsPass is untouched, so the next
        // visit comes back to the same column.
        ParkOptionsCorners();
        oam::RefreshSprites(OAMBuffer);

        const u16 target = static_cast<u16>(SplitRow()) << 3;
        const u16 next = static_cast<u16>(lockAnimScanline + kScanlinesPerFrame);
        lockAnimScanline = next < target ? next : target;
        // ::kUiOnOwnPanel: nothing to unwind, for ::nmi_handler_lockOptions'
        // reason -- take the completion branch below on the first frame so
        // leaving options is immediate.
        if constexpr (kUiOnOwnPanel) lockAnimScanline = target;

        if (lockAnimScanline >= target) {
            // Swap the band back: the options page goes away entirely and the
            // title UI is redrawn, title text included -- ::ClearUiBand takes
            // that with it, since it sits inside the band at rows 31-32.
            if constexpr (kUiOnOwnPanel) {
                ClearUiBand();
                gen::title::GameTitle::Draw();
            }
            gen::title::TitleOptions::Draw();
            DrawTitleSelector();
            ppu::SetScroll({0, PreviewScrollY()});
            BankPreviewTiles();
            ArmSplitIRQ();

            fadeFrameCounter = 0;
            // Back in place BEFORE ::nmi_handler_fadeFromOptions steps the
            // palette, so they are already standing on the ground when the
            // first frame of colour comes up. The scroll this branch just set
            // is the resting one, which is what ::PlaceStandingSprites reads.
            if constexpr (!kUiOnOwnPanel) PlaceStandingSprites();
            pNMI = nmi_handler_fadeFromOptions;
            return;
        }

        // Unwinding ::nmi_handler_lockAnim, and ui for the same reason: the
        // fade has not been stepped back in yet (::nmi_handler_fadeFromOptions
        // does that, after the branch above hands over), so the preview rows
        // coming back into the top band are still black.
        ppu::SetScroll({0, PreviewScrollYForPixels(lockAnimScanline)});
        BankMenuTiles();
    }

    static void nmi_handler_fadeFromOptions() {
        ui::impl::AnimateSelector();
        oam::RefreshSprites(OAMBuffer);

        // ::kUiOnOwnPanel: no fade, and the palette is left untouched -- the
        // state machine advances on this very frame instead.
        bool stillFading = false;
        if constexpr (!kUiOnOwnPanel) {
            stillFading = true;
            if (++fadeFrameCounter >= kFadeStepFrames) {
                fadeFrameCounter = 0;
                stillFading = StepPaletteFadeIn();
            }
        }

        ppu::SetScroll({0, PreviewScrollY()});
        BankPreviewTiles();
        ArmSplitIRQ();

        if (!stillFading) {
            optionsLocked = false;
            pNMI = nmi_handler;
        }
    }

    void irq_handler() {
        mmc3::AcknowledgeScanlineIRQ();
        tech::SpinWait(SplitDelay());
        ApplySplit();
    }

    static void ApplySplit() {
        if constexpr (kUiBankSplit) {
            // Bank only -- see ::kUiBankSplit. No scroll write: the backend's
            // per-panel scroll table already has each panel's own value, and
            // clobbering it here would move the preview as well.
            BankMenuTiles();
            return;
        }
        ppu::SetScroll({static_cast<u16>(kMenuNT << 3), kMenuBandOriginPy});

        // After the scroll write, not before: that one is the raster-critical
        // half of this handler, and two register writes ahead of it would push
        // it further into the visible line. Nothing below the split fetches
        // from $0400 until the next scanline starts, so the switch has until
        // then to land.
        BankMenuTiles();
    }

    void InitTitleScreen() {
        gen::title::GameTitle::Draw();
    }

    static oam::oam_t Clear(const u16) {
        return ParkedSpriteY();
    }

    static oam::oam_t Zero(const u16) {
        return 0;
    }

    // One tile id per 8x8 slot, derived from the pose's 8x16 pattern bytes:
    // slot i is column i/2, and i%2 picks the top or bottom tile of that
    // column's pair. An 8x16 byte holds the pair in bits 7-1 and the pattern
    // table in bit 0, so masking bit 0 off gives the top tile and +1 the
    // bottom -- which is exactly what the hardware would have fetched.
    static oam::oam_t StandingTile(const u16 i) {
        const u8 pair = gen::mary_standing::Get()[i / 2] & 0xFE;
        return static_cast<oam::oam_t>(pair + (i % 2));
    }

    // Lays one character's slots out: slot i is column i/2 of the exported
    // pose, so the X steps every second slot.
    static void PlaceStandingX(const u8 slot, const oam::oam_t x) {
        for (u16 i = 0; i < kPlayerSprites; i++) {
            OAMBuffer[slot + i].x = static_cast<oam::oam_t>(x + (i / 2) * 8);
        }
    }

    // The other half of the same split: i%2 is the top or bottom tile of the
    // column's pair, so the bottom one sits 8 pixels lower. @p y is the top.
    //
    // Both characters stand on the same ground and the preview's scroll is the
    // only thing that moves them, so both follow a transition by being given
    // the same @p y.
    static void PlaceStandingY(const u8 slot, const oam::oam_t y) {
        for (u16 i = 0; i < kPlayerSprites; i++) {
            OAMBuffer[slot + i].y = static_cast<oam::oam_t>(y + (i % 2) * 8);
        }
    }

    static NI void DrawLevelPreview() {
        using namespace level;

        if (const bool loaded = mmc3::CallInBlock<level_code_tag>([] { return LoadLevel(0); }); !loaded) return;

        // PopulateNameTableColumns always draws at row 2 (kHudRows*2) --
        // see its own comment -- so this mirrors that placement for the
        // feet-Y math below instead of hardcoding a second copy of it.
        constexpr u16 tyBase = kHudRows * 2;

        CallLevelGraphics([] {
            ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - ::tech::nes_str::length(msg_mary)), 0}, SIZED_STR(msg_mary), 0);
        });
        constexpr u8 coinUI[] = {windows::ui + blob::ui::hud_coin, windows::ui + (blob::ui::font + 0), windows::ui + (blob::ui::font + 0)};
        ppu::WriteFromBufferToNameTable({static_cast<u16>(video::viewport_tx() - sizeof(coinUI)), 1}, SIZED_OBJ(coinUI), 0);

        // Metatile columns needed to cover the panel, ROUNDED UP: a viewport
        // whose tile width is not a multiple of 4 ends on half a metatile
        // column (the 3DS's 50 tiles = 25 metatiles, the GBA's 30 = 15), and
        // taking viewport_mx() alone -- then flooring it to an even count, as
        // this did to keep the old attribute-phase counter in step -- left the
        // preview up to two tile columns short of the screen's own edge: a 16px
        // strip of the Flush'd empty tile down the right of the title screen.
        // ::PopulateNameTableColumns now takes each column's attribute half
        // from the column itself (see ::level::attr_half), so an odd count is
        // fine here and carries no parity into the level's own fill later.
        const u16 wantMt    = static_cast<u16>((video::viewport_tx() + 1) >> 1);
        const u16 colsWide  = nColumns < wantMt ? nColumns : wantMt;

        const u16 previewTileCols = static_cast<u16>(colsWide * 2);
        mmc3::CallInBlock<level_code_tag>([previewTileCols] {
            PopulateNameTableColumns(previewTileCols);
        });

        CallLevelGraphics([] {
            // The still pose the designer exported: its ids already carry the
            // pattern-table half their CHR entry is banked into (see the
            // header's banner), so they are OAM-ready as they stand.
            oam::PopulateFromProvider(OAMBuffer, kPlayerOneSlot, oam::tile,
                                      StandingTile, kPlayerSprites);
            // Sprite palette 0, unflipped, in front: the generated frames hold
            // no palette or mirroring, so this is the one place it is decided.
            oam::PopulateFromProvider(OAMBuffer, kPlayerOneSlot, oam::attributes,
                                      Zero, kPlayerSprites);
#ifdef PLAYER2_SUPPORTED
            // Player two, shown only where the build HAS a second player to
            // show -- the same compile-time capability gate level mode uses,
            // not the session's own multiplayer choice: the title screen is
            // advertising what the build can do, before anything is chosen.
            // Same pose and same palette, since it is a second character
            // rather than a different one.
            oam::PopulateFromProvider(OAMBuffer, kPlayerTwoSlot, oam::tile,
                                      StandingTile, kPlayerSprites);
            oam::PopulateFromProvider(OAMBuffer, kPlayerTwoSlot, oam::attributes,
                                      Zero, kPlayerSprites);
#endif
            ppu::pal::WriteFromBuffer(ppu::SPRITE_0 + 1, SIZED_OBJ(maryColors));
            ppu::pal::WriteFromBuffer(0, BGColours, 4);
            ppu::pal::WriteFromBuffer(5, BGColours + 5, 3);
        });

        maryGroundNtRow = tyBase * 8 + (levelHeight - 2) * 16;
        PlaceStandingX(kPlayerOneSlot, kPlayerOneX);
#ifdef PLAYER2_SUPPORTED
        PlaceStandingX(kPlayerTwoSlot, kPlayerTwoX);
#endif
        PlaceStandingSprites();
    }

    // Placed once, at the resting split, and never moved again. The lock
    // animation walks the scroll past them, which used to be tracked per
    // frame -- it does not need to be: ::nmi_handler_fadeToOptions only hands
    // over to ::nmi_handler_lockAnim once the fade has finished, and
    // ::kFadeEntries includes SPRITE_0 + 1, so the characters are already
    // fully black by the time anything scrolls.
    static void PlaceStandingSprites() {
        // On a dual-screen target the preview really is scrolled (the
        // bottom-anchor above), so Mary's sprite has to be offset by it like
        // any other target's -- pinning this to 0 would float her 48px off the
        // ground on the DS. The 0 is only for a single-screen CROPPING panel
        // (GBA), where the preview is not scrolled at all.
        const u16 spriteScroll = (!kUiOnOwnPanel && video::panel_ty(0) < 30)
            ? 0
            : PreviewScrollY();
        const i16 rawFeetY    = static_cast<i16>(maryGroundNtRow) - 16 - static_cast<i16>(spriteScroll);
        const auto feetY      = rawFeetY < 0 ? ParkedSpriteY() : static_cast<oam::oam_t>(rawFeetY);
        PlaceStandingY(kPlayerOneSlot, feetY);
#ifdef PLAYER2_SUPPORTED
        PlaceStandingY(kPlayerTwoSlot, feetY);
#endif
    }

    // Parks every character off-screen, undone by ::PlaceStandingSprites.
    // Only on the fading path: where the options panel has a screen of its own
    // (::kUiOnOwnPanel) there is no fade, the preview stays fully visible
    // beside it, and parking them would just make the characters vanish.
    static void ParkStandingSprites() {
        PlaceStandingY(kPlayerOneSlot, ParkedSpriteY());
#ifdef PLAYER2_SUPPORTED
        PlaceStandingY(kPlayerTwoSlot, ParkedSpriteY());
#endif
    }

}

// What ui/impl/singleChoice.hpp declared and left to the mode: this screen's
// OAM layout and the scroll its menu band is read under. Defined here, at the
// end, because both forward to ::title internals -- and in ui::impl rather
// than in ::title because that is the namespace the hooks calling them live
// in. Non-static, unlike everything they reach for: an inline function in that
// header is what calls these, so they need linkage, and this is the one
// translation unit that includes it.
namespace ui::impl {
    u8 SelectorSlot() { return title::kSelectorSlot; }
    vec2<u16> SelectorScroll() { return title::MenuBandScroll(); }
    u8 CornerSlot() { return title::kCornerSlot; }

    // The scroll the options page is read at REST -- the split fully wound up
    // on a single screen, and the frame's own bottom-anchored scroll on a
    // dual-screen one, where that overload ignores its argument. Which is why
    // the corners are only ever placed once the page has settled, never
    // mid-animation.
    //
    // No X term, unlike ::SelectorScroll: options.uis sits in nametable
    // quadrant 2, whose X offset is 0, and the locked frame scrolls X to 0 --
    // the title menu's quadrant 1 is what that scroll's X is for.
    vec2<u16> CanvasScroll() { return {0, title::PreviewScrollYForPixels(0)}; }

    // The SFX on/off button, NES only -- and the bit reads as "the feature is
    // ON", so effects play exactly while it is set: set is what paints DISABLE
    // SFX, i.e. the action the box offers (ui/impl/buttons.hpp), and the
    // button's Default Enabled has it set at boot -- out of its own Make() now
    // that button_state lives in the save file (demo/src/save.hpp) and so
    // carries no initializer of its own. The identical polarity, and
    // the identical reading, as the music bit level.cpp checks before TrackPlay.
    //
    // ALWAYS ON EVERYWHERE ELSE, because there is no such button to read:
    // options.uis hides disableSFX on every target but NES, so the namespace
    // does not exist off it. Not a gap -- those targets get a SFX SLIDER
    // instead, and a slider wound to zero is their mute, expressed through
    // ::SfxLevel rather than through this.
    bool SfxEnabled() {
#ifdef TARGET_NES
        return gen::options::audioCanvas::disableSFX::Enabled();
#else
        return true;
#endif
    }

    // The level an effect plays at, as audio::SfxPlay's per-call volume byte.
    //
    // THE SFX SLIDER'S OWN POSITION, read at the moment of playing rather than
    // pushed when the handle moved -- which is what makes the handle the only
    // copy of this setting (see sliders.hpp's SFXSlider note). An effect already
    // sounding keeps the level it started with, because the backends latch it
    // per voice; the next one picks up wherever the slider is now.
    //
    // A CONSTANT ON NES, and the value is irrelevant: that driver discards the
    // byte entirely (FamiStudio has no per-effect level, see
    // src/nes/audio/famistudio.cpp) and the scene has no slider there to read.
    // The on/off button above is that platform's whole SFX control.
    u8 SfxLevel() {
#ifdef TARGET_NES
        return 0;
#else
        namespace ac = gen::options::audioCanvas;
        const Rect r = ac::SFXSlider::rect;
        // AN UNRESOLVED TRACK IS NOT A SETTING, and this is the branch that
        // lets ::pregame ask this question at all. `rect` is zero until
        // gen::options::Make() has run in this mode, and a cell index cannot
        // become a proportion without a width: ::SliderTravel(0) underflows to
        // 254, so a mid-handle save read against it comes back as 1% -- near
        // silence for a player who set nothing of the kind. pregame used to
        // dodge that by playing its own effects at a hard-coded 255 and never
        // consulting this, which is exactly what made MUTE leak -- its frame
        // pump also spends the mailbox for ::pregame::SaveScreen, so backing
        // out of this page with the slider at zero played the deselect at full
        // volume on the way to the title screen.
        //
        // WHAT IS KNOWABLE WITHOUT THE TRACK IS THE END STOPS. Amount 0 is the
        // left cap at every width -- ::SliderPercent maps it to 0 whatever the
        // travel -- so mute is honoured before the scene exists; anything else
        // reads as full scale, which is the value pregame was using outright
        // and the right answer for the boot-time effects it was chosen for.
        if (!r.size.x) return ac::SFXSlider::amount ? 255 : 0;
        return SliderCoefficient(r.size.x, ac::SFXSlider::amount);
#endif
    }
}