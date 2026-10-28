/**
 * @file tile_canvas.hpp
 * @brief Widget that draws a chr::Table's cells as a scrollable tile grid.
 */
#pragma once

#include "chr_table.hpp"
#include "tile_image.hpp"

#include <QWidget>
#include <QImage>
#include <QRect>
#include <QSize>

#include <vector>

/// Cells per row. Fixed for now -- a CHR page is conventionally read as a
/// 16x16 tile sheet, and the PPU's own pattern-table addressing makes 16 the
/// natural stride.
constexpr int kCellsPerRow = 16;

/**
 * @brief Integer zoom (screen pixels per NES pixel) for a screen's geometry.
 *
 * Same reference scaling uitk uses -- size tracks vertical resolution
 * linearly, 32px/tile at 1080p and 64px/tile at 4K -- but rounded down to a
 * whole number of screen pixels per NES pixel. A fractional zoom would make
 * nearest-neighbour sampling drop or double pixel rows unevenly, which is very
 * visible on 8x8 art.
 *
 * @param screenGeometry The screen the window is on.
 */
int zoomForScreen(const QRect& screenGeometry);

/**
 * @brief Scrollable 16-cell-wide view of a chr::Table.
 *
 * Composes the table's cells into one 1:1 indexed QImage at load, then paints
 * that scaled by the zoom with
 * nearest-neighbour sampling, so the pixels stay square and hard-edged at any
 * size. Rows are as tall as their tallest cell, which is what lets 8x8 and
 * 8x16 cells share a sheet.
 *
 * Meant to live inside a QScrollArea: the widget reports its full unscrolled
 * size via sizeHint(), which is what gives the scroll area its vertical
 * scrollbar.
 */
class TileCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit TileCanvas(int zoom, QWidget* parent = nullptr);

    /// Replaces the displayed cells with @p table's. The table is not retained
    /// -- everything the widget needs is composed into its own image here.
    void setTable(const chr::Table& table);

    /// Drops all cells, leaving an empty canvas.
    void clear();

    /// Recolours the sheet. The image is indexed, so this is a colour-table
    /// swap -- nothing is decoded again.
    void setPaletteTable(const QVector<QRgb>& table);

    /// Marks @p cellIndex as the picked cell (-1 for none), outlining it.
    void setSelected(int cellIndex);

    /// The picked cell, or -1.
    [[nodiscard]] int selected() const { return selected_; }

    /// Cells currently displayed.
    [[nodiscard]] int cellCount() const { return static_cast<int>(cells_.size()); }

    /// The widget's full, unscrolled extent -- 16 cells wide by however many
    /// rows they fill.
    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// The cell under the pointer changed; @p cellIndex indexes the table last
    /// passed to setTable(), or is -1 when the pointer is over empty space.
    void cellHovered(int cellIndex);

    /// A cell was clicked, making it the pick a metasprite field will stamp.
    void cellPicked(int cellIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    /// Where one cell of the table sits in the sheet, in NES pixels.
    struct Placement {
        QRect rect;
    };

    /// The cell index at @p pos (widget coordinates), or -1 for none.
    [[nodiscard]] int cellAt(const QPoint& pos) const;

    int                    zoom_;
    QVector<QRgb>          table_ = paletteTable(nullptr);
    QImage                 sheet_;         ///< 1:1 NES-pixel indexed image of the whole sheet.
    std::vector<Placement> cells_;         ///< Parallel to the table's cells.
    int                    hovered_ = -1;  ///< Last cell reported through cellHovered().
    int                    selected_ = -1; ///< Picked cell, outlined; -1 for none.
};
