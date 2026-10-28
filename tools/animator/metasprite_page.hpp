/**
 * @file metasprite_page.hpp
 * @brief One metasprite tab: its attributes, and a tab per animation frame.
 */
#pragma once

#include "chr_table.hpp"
#include "metasprite_frame.hpp"
#include "palette_store.hpp"

#include <QWidget>
#include <QString>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTabWidget;

/**
 * @brief A metasprite: extent, plus the frames of its animation.
 *
 * The attribute section owns the extent, in TILES. That is deliberately
 * indifferent to 8x8 versus 8x16: a 1x1 metasprite is one tile, which is an
 * 8x8 sprite or an 8x16 one depending on the mode the sheet is in. The pixel
 * extent is derived and shown, never entered. It applies to every frame --
 * frames of one animation are the same size by definition -- so a resize goes
 * to all of them.
 *
 * Each frame is a tab of its own (MetaspriteFrame), renamable, with its own
 * timer. Frames are contiguous -- tab order IS play order -- so nothing here
 * links one frame to another.
 */
class MetaspritePage final : public QWidget {
    Q_OBJECT

public:
    /// @param palettes The project's palettes; the page follows them, so a
    ///                  recolour or a rename reaches its frames on its own.
    MetaspritePage(int zoom, PaletteStore* palettes, QWidget* parent = nullptr);

    /// The metasprite's name. It is what its .msprite file is named after, and
    /// what the window puts on the tab.
    [[nodiscard]] QString name() const;
    void setName(const QString& name);

    /// The file stem this metasprite was last saved as, so a rename moves that
    /// file instead of leaving it behind. Empty until it has been saved or
    /// loaded. The page only carries it; proj::save is what acts on it.
    [[nodiscard]] QString savedStem() const { return savedStem_; }
    void setSavedStem(const QString& stem) { savedStem_ = stem; }

    /// The palette this metasprite is drawn with, by name. Empty means the
    /// placeholder colours -- which is also what a name no palette answers to
    /// falls back to, so deleting a palette cannot blank a metasprite.
    [[nodiscard]] QString paletteName() const;
    void setPaletteName(const QString& name);

    /// Sets the extent, in tiles, as if typed into the attribute section.
    void setSize(int tilesX, int tilesY);

    [[nodiscard]] int tilesX() const { return tilesX_; }
    [[nodiscard]] int tilesY() const { return tilesY_; }

    /// Every frame, in tab order -- which is play order.
    [[nodiscard]] const std::vector<MetaspriteFrame*>& frames() const { return frameList_; }

    /// Adds a frame and returns it, for a load to fill in.
    MetaspriteFrame* appendFrame();

    /// Closes every frame, leaving the metasprite empty.
    void clearFrames();

    /// The frame being edited, or nullptr when every frame has been closed.
    [[nodiscard]] MetaspriteFrame* currentFrame() const;

    [[nodiscard]] int frameCount() const;

    /// Whether any frame holds a sprite -- i.e. whether closing this would
    /// discard work.
    [[nodiscard]] bool hasContent() const;

    /// Adds a frame, names it after its position and shows it.
    void newFrame();

    /// Closes the frame on tab @p index, or the current one when @p index < 0.
    void closeFrame(int index = -1);

    /// A line describing the metasprite as a whole, for the status bar.
    [[nodiscard]] QString summary() const;

    /// Forwarded to every frame's field, and reflected in the attribute readout.
    void setTileMode(chr::TileMode mode);

    /// Forwarded to every frame's field.
    void setBrush(const chr::Cell* cell, const QString& source);

signals:
    /// A line for the window's status bar, from the visible frame or the
    /// attributes.
    void statusChanged(const QString& text);

    /// The metasprite was renamed.
    void nameChanged(const QString& name);

    /// A frame was added, removed or renamed, or its sprites changed.
    void framesChanged();

    /// Anything about this metasprite that a save would record changed: its
    /// name, extent, palette, or any frame's name, timer or sprites. The window
    /// tracks unsaved work off this.
    void dirtied();

private:
    /// Applies the attribute section's extent to every frame.
    void applySize();

    /// Rebuilds the palette dropdown from the store, keeping the choice by
    /// name, and pushes the resulting colours to every frame.
    void refreshPalettes();

    /// Pushes the selected palette's colours to every frame.
    void applyPalette();

    /// Refreshes the derived "pixels" readout from a frame's own geometry.
    void refreshDerived();

    /// Sets the tab label of @p frame to its name.
    void retitle(MetaspriteFrame* frame);

    /// Wires one newly created frame up.
    void adopt(MetaspriteFrame* frame);

    int                           zoom_;
    chr::TileMode                 mode_ = chr::TileMode::Size8x8;
    int                           tilesX_ = kDefaultFieldTilesX;
    int                           tilesY_ = kDefaultFieldTilesY;
    int                           frameIdCounter_ = 0;  ///< Hands out MetaspriteFrame ids.
    int                           frameNumber_ = 0;   ///< Numbers default frame names.
    QTabWidget*                   frames_ = nullptr;
    QLineEdit*                    name_ = nullptr;
    QComboBox*                    palette_ = nullptr;
    PaletteStore*                 palettes_ = nullptr;
    QString                       savedStem_;
    QSpinBox*                     width_ = nullptr;
    QSpinBox*                     height_ = nullptr;
    QLabel*                       derived_ = nullptr;
    std::vector<MetaspriteFrame*> frameList_;         ///< Every frame, tab order.
};
