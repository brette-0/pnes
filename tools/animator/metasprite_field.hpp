/**
 * @file metasprite_field.hpp
 * @brief The field a metasprite is composed on: a grid of tile slots that
 *        stamped sprites are placed into.
 */
#pragma once

#include "chr_table.hpp"

#include "tile_image.hpp"

#include <QWidget>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>
#include <vector>

/// Default metasprite extent, in tile slots -- 2x2 is the canonical 16x16
/// subject, and the attribute section changes it per metasprite.
constexpr int kDefaultFieldTilesX = 2;
constexpr int kDefaultFieldTilesY = 2;

/// Range the attribute section offers for a metasprite's extent. The upper
/// bound is a working limit, not a hardware one: 32x32 tiles is already four
/// screens' worth of sprites.
constexpr int kMinFieldTiles = 1;
constexpr int kMaxFieldTiles = 32;

/**
 * @brief Per-sprite mirroring, in the order right-click cycles it.
 *
 * The values are the cycle order (none, V, H, H+V), NOT OAM's bit layout --
 * OAM attribute bit 6 flips horizontally and bit 7 vertically, so horizontal
 * is the lower bit there and the higher one here. Anything emitting OAM bytes
 * has to map between the two rather than cast.
 */
enum class Mirror : int {
    None       = 0,
    Vertical   = 1,
    Horizontal = 2,
    Both       = 3,
};

/// How many values Mirror has -- right-click steps through them modulo this,
/// so the step after Both is back to None.
constexpr int kMirrorModes = 4;

/// Mirror as "none", "V", "H" or "H+V".
[[nodiscard]] QString mirrorName(Mirror mirror);

/// One sprite placed on the field.
struct PlacedSprite {
    int     col = 0;          ///< Slot column; one slot is one tile wide.
    int     row = 0;          ///< Slot row; one slot is one tile TALL, which is
                              ///< 8 or 16 pixels depending on the tile mode.
    int     id = 0;           ///< PPU tile id; the even id of the pair when 8x16.
    int     heightTiles = 1;  ///< Height of the cell's art, in 8-pixel tiles.
    Mirror  mirror = Mirror::None;

    // Where the art came from, kept in pieces rather than as one label: this is
    // what a saved metasprite identifies a tile BY, since an id alone is
    // ambiguous (two entries at the same offset into their VMAs share ids).
    QString entry;            ///< blobman entry.
    QString include;          ///< Include (filename stem) within it.
    int     tile = 0;         ///< Tile index within that include.

    QImage  image;            ///< The cell's own pixels, unmirrored.

    /// The three provenance fields as one line, for the status bar.
    [[nodiscard]] QString source() const {
        return QString("%1::%2 tile %3").arg(entry, include).arg(tile);
    }
};

/**
 * @brief Editable metasprite field.
 *
 * The grid is measured in TILES, not pixels, and a tile is whatever the
 * current chr::TileMode says it is: one slot holds one 8x8 tile in 8x8 mode and
 * one 8x16 tile in 8x16 mode, so a 1x1 metasprite is one sprite either way and
 * nothing about the extent has to be restated when the mode changes.
 *
 * Left-click stamps the current brush (the cell picked in the tile sheet) into
 * the slot under the pointer, right-click cycles that slot's mirroring, and
 * middle-click removes the sprite. Sprites are stored by slot rather than by
 * pixel offset for the same reason the extent is in tiles -- a mode change
 * moves where a slot IS, not which slot a sprite is in.
 */
class MetaspriteField final : public QWidget {
    Q_OBJECT

public:
    explicit MetaspriteField(int zoom, QWidget* parent = nullptr);

    /// Sets the cell that left-click stamps, or clears it when @p cell is null.
    /// @p entry is the name of the blobman entry @p cell belongs to; the cell
    /// knows the rest of its own provenance.
    void setBrush(const chr::Cell* cell, const QString& entry);

    /**
     * @brief Places a sprite directly, for rebuilding a saved metasprite.
     *
     * Bypasses the brush: a load has the cell in hand and the slot, mirroring
     * and provenance already decided. Out-of-range slots are refused, so a file
     * naming a slot outside the extent cannot plant an unreachable sprite.
     *
     * @return Whether the sprite was placed.
     */
    bool addSprite(const chr::Cell& cell, const QString& entry, int col, int row, Mirror mirror);

    /// Sets what one slot means: an 8x8 or an 8x16 tile. Sprites keep their
    /// slots, so the metasprite stretches or squashes vertically with the mode.
    void setTileMode(chr::TileMode mode);

    /**
     * @brief Resizes the metasprite, in tile slots.
     *
     * Sprites outside the new extent are dropped -- there is nowhere for them
     * to be -- and reported through statusChanged().
     */
    void setFieldSize(int tilesX, int tilesY);

    [[nodiscard]] int tilesX() const { return tilesX_; }
    [[nodiscard]] int tilesY() const { return tilesY_; }

    /// Height of one slot in NES pixels: 8, or 16 in 8x16 mode.
    [[nodiscard]] int slotHeightPx() const;

    /// Recolours every sprite. The images are indexed, so this is a
    /// colour-table swap -- nothing is decoded again.
    void setPaletteTable(const QVector<QRgb>& table);

    /// Removes every placed sprite.
    void clearSprites();

    [[nodiscard]] const std::vector<PlacedSprite>& sprites() const { return sprites_; }
    [[nodiscard]] int spriteCount() const { return static_cast<int>(sprites_.size()); }

    /**
     * @brief The metasprite as the PPU would show it: one image the size of the
     *        extent, sprites drawn with their mirroring, no grid, hover marks
     *        or zoom.
     *
     * 1:1 in NES pixels, so whatever displays it chooses its own scale.
     */
    [[nodiscard]] QImage render() const;

    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// A line about what is under the pointer, or what just happened, for the
    /// window's status bar.
    void statusChanged(const QString& text);

    /// The metasprite's contents changed.
    void spritesChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    /// The brush, as its own copy -- the table it came from is re-read on every
    /// reload and mode change, so holding a pointer into it would dangle.
    struct Brush {
        int     id = 0;
        int     heightTiles = 1;
        QString entry;
        QString include;
        int     tile = 0;
        QImage  image;

        [[nodiscard]] QString source() const {
            return QString("%1::%2 tile %3").arg(entry, include).arg(tile);
        }
    };

    /// The grid slot at @p pos, or nothing if @p pos is outside the field.
    [[nodiscard]] std::optional<QPoint> slotAt(const QPoint& pos) const;

    /// Index of the topmost sprite in @p slot, or -1.
    [[nodiscard]] int spriteAt(const QPoint& slot) const;

    /// Where slot @p slot sits in the widget, in device pixels.
    [[nodiscard]] QRect slotRect(const QPoint& slot) const;

    /// Emits statusChanged() for the slot under the pointer.
    void reportSlot(const std::optional<QPoint>& slot);

    int                       zoom_;
    QVector<QRgb>             table_ = paletteTable(nullptr);
    chr::TileMode             mode_ = chr::TileMode::Size8x8;
    int                       tilesX_ = kDefaultFieldTilesX;
    int                       tilesY_ = kDefaultFieldTilesY;
    std::optional<Brush>      brush_;
    std::vector<PlacedSprite> sprites_;
    std::optional<QPoint>     hovered_;
};
