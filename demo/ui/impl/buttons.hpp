#pragma once

#include <pnes/types.hpp>
#include <intsh>

#include "../../chr/chr.h"
#include "../../src/graphics/windows.hpp"
#include STRCAT(../../gen/options/GEN_TARGET_DIR/options.hpp)

// hand written uitk button impls
namespace ui::impl {
    enum eButtonState : u8 {
        Enabled,
        Disabled
    };

    // One nametable row, as a flat-address step. NOT video::viewport_tx():
    // src/emu/ppu.cpp's xy_to_nt_addr lays a page out as `row * geo.tileW +
    // col`, and emu::ComputeNtGeometry's tileW floors the viewport at the
    // NES-native 32 and then rounds it UP to a whole number of attribute bytes
    // (4 tiles). The two agree wherever the viewport is already >= 32 and a
    // multiple of 4 -- NES/DS at 32, the GameCube's 40 and 52, the Switch's 52,
    // the PSP's 60, and PC, whose own viewport_tx() ends in `& ~3u` for exactly
    // this reason -- and disagree on the two where it is not: the 3DS (50 -> 52)
    // and the GBA (30 -> 32).
    //
    // Stepping by the viewport width on those two walked 2 columns short per
    // row: a 4-row button's bottom edge went to +150 instead of +156, which
    // decodes as page row 2 column 46, and its side edges started at column 50
    // -- inside the 2-column margin the 3DS viewport does not show at all. So
    // the top edge was the only part of the frame that landed where it belonged.
    //
    // Duplicated from ComputeNtGeometry rather than shared because video.hpp
    // exposes no page-width accessor: nametable_plane_bytes() computes this
    // very value and keeps it to itself. That header is the right home for it.
    constexpr u16 PageTx() {
        const u16 txMin = video::viewport_tx() < 32 ? 32 : video::viewport_tx();
        return static_cast<u16>((txMin + 3) & ~3);
    }

    constexpr void DrawTextBox(const u16 pos, const vec2<u8>& box) {
        // TODO: create actual graphics and use it normally
        constexpr u8 ULCornerGraphic   = windows::ui + blob::ui::box_ul;
        constexpr u8 URCornerGraphic   = windows::ui + blob::ui::box_ur;
        constexpr u8 BLCornerGraphic   = windows::ui + blob::ui::box_bl;
        constexpr u8 BRCornerGraphic   = windows::ui + blob::ui::box_br;
        constexpr u8 LeftEdgeGraphic   = windows::ui + blob::ui::box_vertical;
        constexpr u8 RightEdgeGraphic  = windows::ui + blob::ui::box_vertical;
        constexpr u8 TopEdgeGraphic    = windows::ui + blob::ui::box_horizontal;
        constexpr u8 BottomEdgeGraphic = windows::ui + blob::ui::box_horizontal;

        // UL
        ppu::WriteSingleToNameTable(pos, ULCornerGraphic);

        // UR
        ppu::WriteSingleToNameTable(pos + box.x - 1, URCornerGraphic);

        // BL
        ppu::WriteSingleToNameTable(pos + PageTx() * (box.y - 1), BLCornerGraphic);

        // BR
        ppu::WriteSingleToNameTable(pos + box.x + PageTx() * (box.y - 1) - 1, BRCornerGraphic);

        // top row
        ppu::WriteRepeatedToNameTable(pos + 1, TopEdgeGraphic, box.x - 2, 0);

        // bottom row
        ppu::WriteRepeatedToNameTable(
            pos + PageTx() * (box.y - 1) + 1, BottomEdgeGraphic,
            box.x - 2, 0
        );

        // left edge
        ppu::WriteRepeatedToNameTable(
            pos + PageTx(), LeftEdgeGraphic,
            box.y - 2, 1
        );

        // right edge
        ppu::WriteRepeatedToNameTable(
            pos + PageTx() + box.x - 1, RightEdgeGraphic,
            box.y - 2, 1
        );
    }

    constexpr void UpdateTextBox(const eButtonState state, const u16 pos, const vec2<u8> &box) {

    }
}

// impl for gen code
namespace gen::options {
    // `pos` is already the button's PPU address -- uitk resolves it in Make(),
    // so there is no CartesianToAddress call left to pay here. `rect` is copied
    // out whole before its size is read: on a runtime-viewport target it is
    // `atomic Rect`, i.e. volatile, so reading it goes through Rect's volatile
    // copy constructor once rather than per field.
    //
    // One Draw() per button, dispatching on the generated Enabled() -- uitk no
    // longer emits a DrawEnabled()/DrawDisabled() pair with a generated branch
    // over it, because a button's two states are two appearances of one widget
    // and the branch belongs where the appearance is decided (see
    // genButtonDecl). `inline`, so this header stays includable from more than
    // one TU; add AI to a definition that should be force-inlined at its call
    // sites (::AI in technology.hpp).
    //
    // Each of these also draws its own label, which is what lets a Canvas's
    // DrawComponents() paint a whole section in one call: that generated
    // function calls each component's Draw() and deliberately stops there,
    // because a button's children are its two ALTERNATIVE labels stacked at one
    // position (DISABLE MUSIC / ENABLE MUSIC) and painting both would put one
    // over the other. Choosing between them is exactly what this hook is for,
    // and a child's namespace is nested inside its parent's, so the label is
    // named unqualified from here. The first child is the enabled appearance
    // throughout -- the order the scene lists them in (see options.uis).
    namespace videoCanvas {
        namespace reduceFlashesButton {
            inline void Draw() {
                const Rect r = rect;
                ui::impl::DrawTextBox(pos, r.size);
                ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
                if (Enabled()) reduceFlashesText::Draw();
                else           enableFlashesText::Draw();
            }
        }

#if TARGET_GC
        // The #if is about the BUTTON, not about the Canvas: this scene hides
        // enableWidescreenButton everywhere but the GameCube, so the namespace
        // itself only exists there -- and videoCanvas::DrawComponents() only
        // names it there, since a hidden node is not in the Canvas's table.
        namespace enableWidescreenButton {
            inline void Draw() {
                const Rect r = rect;
                ui::impl::DrawTextBox(pos, r.size);
                ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
                enableWidescreenText::Draw();
            }
        }
#endif
    }

    namespace audioCanvas {
#if TARGET_NES
        // NES only: the mix there is a pair of on/off buttons rather than the
        // two sliders every other target gets, so options.uis hides these
        // everywhere else and hides the sliders here (see sliders.hpp, which
        // carries the mirror image of this guard).
        namespace disableMusic {
            inline void Draw() {
                const Rect r = rect;
                ui::impl::DrawTextBox(pos, r.size);
                ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
                if (Enabled()) disableMusicText::Draw();
                else           enableMusicText::Draw();
            }
        }

        namespace disableSFX {
            inline void Draw() {
                const Rect r = rect;
                ui::impl::DrawTextBox(pos, r.size);
                ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
                if (Enabled()) disableSFXText::Draw();
                else           enableSFXText::Draw();
            }
        }
#else
        // Everywhere but NES, where the scene hides it: stereo is the one audio
        // control that stays a button on the targets that have sliders.
        namespace enableStereo {
            inline void Draw() {
                const Rect r = rect;
                ui::impl::DrawTextBox(pos, r.size);
                ui::impl::UpdateTextBox(Enabled() ? ui::impl::Enabled : ui::impl::Disabled, pos, r.size);
                if (Enabled()) enableStereoText::Draw();
                else           disableStereoText::Draw();
            }
        }
#endif
    }
}
