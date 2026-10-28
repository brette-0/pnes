#include "tile_canvas.hpp"
#include "tile_image.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QColor>
#include <QPen>
#include <QPalette>

#include <algorithm>

namespace {

// Reference scaling, as in uitk: tile size tracks vertical resolution
// linearly, 32px/tile at 1080p -> 64px/tile at 4K.
constexpr double kTilePxAt1080p   = 32.0;
constexpr double kReferenceHeight = 1080.0;

} // namespace

int zoomForScreen(const QRect& screenGeometry) {
    const double tilePx = kTilePxAt1080p * (screenGeometry.height() / kReferenceHeight);
    return std::max(1, static_cast<int>(tilePx / chr::kTilePixels));
}

TileCanvas::TileCanvas(int zoom, QWidget* parent)
    : QWidget(parent), zoom_(std::max(1, zoom)) {
    // Index 0 of the palette, so space beyond the last row matches the tiles'
    // own backdrop instead of the desktop theme's window colour.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(table_[0]));
    setPalette(pal);

    // Tile ids are reported on hover, which needs move events without a button
    // held down.
    setMouseTracking(true);
}

void TileCanvas::clear() {
    sheet_ = QImage();
    cells_.clear();
    hovered_ = -1;
    selected_ = -1;
    updateGeometry();
    update();
}

void TileCanvas::setTable(const chr::Table& table) {
    clear();
    if (table.cells.empty()) return;

    // Lay the cells out first -- rows are as tall as their tallest cell, so
    // the sheet's height isn't known until every cell has a place.
    cells_.resize(table.cells.size());
    int y = 0;
    for (std::size_t first = 0; first < table.cells.size(); first += kCellsPerRow) {
        const std::size_t last = std::min(first + kCellsPerRow, table.cells.size());

        int rowHeight = 0;
        for (std::size_t i = first; i < last; ++i) {
            rowHeight = std::max(rowHeight, table.cells[i].heightPx());
        }

        for (std::size_t i = first; i < last; ++i) {
            const chr::Cell& cell = table.cells[i];
            const int x = static_cast<int>(i - first) * chr::kTilePixels;
            cells_[i].rect = QRect(x, y, cell.widthPx(), cell.heightPx());
        }

        y += rowHeight;
    }

    // One 8-bit palette index per NES pixel, drawn at 1:1 here and scaled only
    // at paint time -- so a zoom change costs nothing but a repaint.
    sheet_ = QImage(kCellsPerRow * chr::kTilePixels, y, QImage::Format_Indexed8);
    sheet_.setColorTable(table_);
    sheet_.fill(0);

    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        const chr::Cell& cell = table.cells[i];
        const QRect&     rect = cells_[i].rect;

        for (int row = 0; row < rect.height(); ++row) {
            std::uint8_t* scanline = sheet_.scanLine(rect.top() + row) + rect.left();
            const std::uint8_t* source = cell.pixels.data() + static_cast<std::size_t>(row) * cell.widthPx();
            std::copy_n(source, rect.width(), scanline);
        }
    }

    updateGeometry();
    update();
}

QSize TileCanvas::sizeHint() const {
    const int width = kCellsPerRow * chr::kTilePixels * zoom_;
    const int height = sheet_.isNull() ? chr::kTilePixels * zoom_ : sheet_.height() * zoom_;
    return {width, height};
}

void TileCanvas::paintEvent(QPaintEvent* event) {
    if (sheet_.isNull()) return;

    QPainter painter(this);
    // Nearest-neighbour: Qt smooths scaled images by default, which would blur
    // 8x8 pixel art into mush.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    // Only the exposed strip needs redrawing -- the scroll area repaints a
    // narrow band per wheel notch, not the whole sheet, which matters once the
    // sheet is tall.
    const QRect target(0, 0, sheet_.width() * zoom_, sheet_.height() * zoom_);
    const QRect dirty = event->rect().intersected(target);
    if (dirty.isEmpty()) return;

    // Map the dirty screen rect back to source pixels, rounding outward so a
    // partially exposed NES pixel at either edge is still drawn.
    const QRect source(dirty.left() / zoom_,
                       dirty.top() / zoom_,
                       (dirty.right() / zoom_) - (dirty.left() / zoom_) + 1,
                       (dirty.bottom() / zoom_) - (dirty.top() / zoom_) + 1);
    const QRect snapped(source.left() * zoom_,
                        source.top() * zoom_,
                        source.width() * zoom_,
                        source.height() * zoom_);
    painter.drawImage(snapped, sheet_, source);

    // The pick is outlined rather than tinted, so none of the tile's own
    // pixels are hidden by the marker.
    if (selected_ >= 0 && static_cast<std::size_t>(selected_) < cells_.size()) {
        const QRect cell = cells_[static_cast<std::size_t>(selected_)].rect;
        const QRect outline(cell.left() * zoom_, cell.top() * zoom_,
                            cell.width() * zoom_, cell.height() * zoom_);
        // One screen pixel wide whatever the zoom, inset by half a pen width so
        // the stroke lands inside the cell instead of straddling its edge.
        painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(outline.adjusted(0, 0, -1, -1));
    }
}

void TileCanvas::setPaletteTable(const QVector<QRgb>& table) {
    table_ = table;

    // The backdrop follows index 0, so space beyond the last row keeps matching
    // what a sprite's own index 0 shows.
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(table_[0]));
    setPalette(pal);

    if (!sheet_.isNull()) sheet_.setColorTable(table_);
    update();
}

void TileCanvas::setSelected(int cellIndex) {
    if (cellIndex == selected_) return;
    const int previous = selected_;
    selected_ = (cellIndex >= 0 && static_cast<std::size_t>(cellIndex) < cells_.size()) ? cellIndex : -1;

    // Repaint just the two cells involved -- the sheet can be thousands of
    // tiles tall, and a pick changes eight pixels of outline.
    for (const int index : {previous, selected_}) {
        if (index < 0) continue;
        const QRect cell = cells_[static_cast<std::size_t>(index)].rect;
        update(QRect(cell.left() * zoom_, cell.top() * zoom_,
                     cell.width() * zoom_, cell.height() * zoom_));
    }
}

int TileCanvas::cellAt(const QPoint& pos) const {
    const QPoint nes(pos.x() / zoom_, pos.y() / zoom_);
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i].rect.contains(nes)) return static_cast<int>(i);
    }
    return -1;
}

void TileCanvas::mouseMoveEvent(QMouseEvent* event) {
    const int cell = cellAt(event->pos());
    if (cell != hovered_) {
        hovered_ = cell;
        emit cellHovered(cell);
    }
    QWidget::mouseMoveEvent(event);
}

void TileCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (const int cell = cellAt(event->pos()); cell >= 0) {
            setSelected(cell);
            emit cellPicked(cell);
        }
    }
    QWidget::mousePressEvent(event);
}

void TileCanvas::leaveEvent(QEvent* event) {
    if (hovered_ != -1) {
        hovered_ = -1;
        emit cellHovered(-1);
    }
    QWidget::leaveEvent(event);
}
