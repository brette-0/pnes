#include "metasprite_field.hpp"

#include "tile_image.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QColor>
#include <QPalette>
#include <QPen>

#include <algorithm>

namespace {

/// The field is the editing surface, so it is drawn at twice the sheet's zoom
/// -- the sheet is for picking tiles out of a long list, this is for placing
/// them precisely.
constexpr int kFieldZoomFactor = 2;

/// Grid lines, dark enough not to compete with the art but visible against
/// palette index 0 (black).
const QColor kGridColour(0x30, 0x30, 0x30);

/// Field border, a touch brighter than the grid so the extent is obvious.
const QColor kBorderColour(0x60, 0x60, 0x60);

QString formatId(int id, int heightTiles) {
    // An 8x16 sprite occupies the even id and the next one, and the pattern
    // byte that selects it is the even one -- so both are worth showing.
    return heightTiles == 2
        ? QString("$%1-$%2").arg(id, 2, 16, QChar('0')).arg(id + 1, 2, 16, QChar('0'))
        : QString("$%1").arg(id, 2, 16, QChar('0'));
}

/// The sprite's art with its mirroring applied.
///
/// Mirrored on demand rather than stored: a tile is 64 or 128 pixels, a
/// metasprite is a handful of them, and keeping only the unmirrored art means
/// cycling the mirror never touches the image the brush handed over.
QImage mirrored(const PlacedSprite& sprite) {
    const int bits = static_cast<int>(sprite.mirror);
    if (bits == 0) return sprite.image;
    return sprite.image.mirrored(/*horizontal=*/(bits & static_cast<int>(Mirror::Horizontal)) != 0,
                                 /*vertical=*/(bits & static_cast<int>(Mirror::Vertical)) != 0);
}

} // namespace

QString mirrorName(const Mirror mirror) {
    switch (mirror) {
        case Mirror::None:       return "none";
        case Mirror::Vertical:   return "V";
        case Mirror::Horizontal: return "H";
        case Mirror::Both:       return "H+V";
    }
    return "none";
}

MetaspriteField::MetaspriteField(int zoom, QWidget* parent)
    : QWidget(parent), zoom_(std::max(1, zoom) * kFieldZoomFactor) {
    // Palette index 0, so the field's backdrop matches what the PPU shows
    // behind a sprite rather than the desktop theme's window colour.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(table_[0]));
    setPalette(pal);

    // Slot coordinates are reported on hover, which needs move events without
    // a button held down.
    setMouseTracking(true);
}

void MetaspriteField::setBrush(const chr::Cell* cell, const QString& entry) {
    if (!cell) {
        brush_.reset();
        return;
    }
    brush_ = Brush{cell->id,
                   cell->heightTiles,
                   entry,
                   QString::fromStdString(cell->include),
                   cell->tileInInclude,
                   cellImage(*cell, table_)};
}

bool MetaspriteField::addSprite(const chr::Cell& cell, const QString& entry,
                                const int col, const int row, const Mirror mirror) {
    if (col < 0 || row < 0 || col >= tilesX_ || row >= tilesY_) return false;

    PlacedSprite sprite;
    sprite.col         = col;
    sprite.row         = row;
    sprite.id          = cell.id;
    sprite.heightTiles = cell.heightTiles;
    sprite.mirror      = mirror;
    sprite.entry       = entry;
    sprite.include     = QString::fromStdString(cell.include);
    sprite.tile        = cell.tileInInclude;
    sprite.image       = cellImage(cell, table_);

    // One sprite per slot, as when stamping.
    if (const int existing = spriteAt(QPoint(col, row)); existing >= 0) {
        sprites_.erase(sprites_.begin() + existing);
    }
    sprites_.push_back(std::move(sprite));
    update(slotRect(QPoint(col, row)));
    emit spritesChanged();
    return true;
}

void MetaspriteField::setPaletteTable(const QVector<QRgb>& table) {
    table_ = table;

    // The field's backdrop follows index 0, same as the sheet's.
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(table_[0]));
    setPalette(pal);

    for (PlacedSprite& sprite : sprites_) {
        sprite.image.setColorTable(table_);
    }
    if (brush_) brush_->image.setColorTable(table_);
    update();
}

int MetaspriteField::slotHeightPx() const {
    return mode_ == chr::TileMode::Size8x16 ? chr::kTilePixels * 2 : chr::kTilePixels;
}

void MetaspriteField::setTileMode(const chr::TileMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    // Every slot changed height, so the widget's extent did too -- and the
    // sprites, which are stored by slot, come along for free.
    updateGeometry();
    resize(sizeHint());
    update();
}

void MetaspriteField::setFieldSize(const int tilesX, const int tilesY) {
    const int x = std::clamp(tilesX, kMinFieldTiles, kMaxFieldTiles);
    const int y = std::clamp(tilesY, kMinFieldTiles, kMaxFieldTiles);
    if (x == tilesX_ && y == tilesY_) return;

    tilesX_ = x;
    tilesY_ = y;

    const auto before = sprites_.size();
    sprites_.erase(std::remove_if(sprites_.begin(), sprites_.end(),
                                  [&](const PlacedSprite& sprite) {
                                      return sprite.col >= tilesX_ || sprite.row >= tilesY_;
                                  }),
                   sprites_.end());
    const auto dropped = before - sprites_.size();

    hovered_.reset();
    updateGeometry();
    resize(sizeHint());
    update();

    if (dropped != 0) {
        emit spritesChanged();
        emit statusChanged(QString("Resized to %1x%2 tiles -- %3 sprite%4 outside it %5 removed")
                               .arg(tilesX_)
                               .arg(tilesY_)
                               .arg(dropped)
                               .arg(dropped == 1 ? "" : "s", dropped == 1 ? "was" : "were"));
    } else {
        emit statusChanged(QString("Resized to %1x%2 tiles").arg(tilesX_).arg(tilesY_));
    }
}

void MetaspriteField::clearSprites() {
    if (sprites_.empty()) return;
    sprites_.clear();
    update();
    emit spritesChanged();
    emit statusChanged("Metasprite cleared");
}

QSize MetaspriteField::sizeHint() const {
    return {tilesX_ * chr::kTilePixels * zoom_, tilesY_ * slotHeightPx() * zoom_};
}

QRect MetaspriteField::slotRect(const QPoint& slot) const {
    return {slot.x() * chr::kTilePixels * zoom_,
            slot.y() * slotHeightPx() * zoom_,
            chr::kTilePixels * zoom_,
            slotHeightPx() * zoom_};
}

void MetaspriteField::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    const QRect field(0, 0, sizeHint().width(), sizeHint().height());

    for (const PlacedSprite& sprite : sprites_) {
        // The art is drawn at the TOP of its slot: an 8x8 cell left over in
        // 8x16 mode (an include whose tiles do not pair up) fills half a slot,
        // and the top is where the PPU would put it.
        const QRect slot = slotRect(QPoint(sprite.col, sprite.row));
        const QRect target(slot.left(), slot.top(),
                           sprite.image.width() * zoom_, sprite.image.height() * zoom_);
        if (target.intersects(event->rect())) {
            painter.drawImage(target, mirrored(sprite));
        }
    }

    // Grid over the art, not under it: a sprite's own black pixels are
    // indistinguishable from the backdrop, so the lines are the only thing
    // showing where one sprite ends and the next begins.
    painter.setPen(QPen(kGridColour, 1));
    for (int x = 1; x < tilesX_; ++x) {
        const int px = x * chr::kTilePixels * zoom_;
        painter.drawLine(px, 0, px, field.height() - 1);
    }
    for (int y = 1; y < tilesY_; ++y) {
        const int py = y * slotHeightPx() * zoom_;
        painter.drawLine(0, py, field.width() - 1, py);
    }

    painter.setPen(QPen(kBorderColour, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(field.adjusted(0, 0, -1, -1));

    // The slot the next stamp would land in.
    if (hovered_) {
        painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 1));
        painter.drawRect(slotRect(*hovered_).adjusted(0, 0, -1, -1));
    }
}

std::optional<QPoint> MetaspriteField::slotAt(const QPoint& pos) const {
    if (pos.x() < 0 || pos.y() < 0) return std::nullopt;
    const QPoint slot(pos.x() / (chr::kTilePixels * zoom_), pos.y() / (slotHeightPx() * zoom_));
    if (slot.x() >= tilesX_ || slot.y() >= tilesY_) return std::nullopt;
    return slot;
}

int MetaspriteField::spriteAt(const QPoint& slot) const {
    // Back to front: the last placed sprite is the one drawn on top, so it is
    // the one a click acts on.
    for (int i = static_cast<int>(sprites_.size()) - 1; i >= 0; --i) {
        const PlacedSprite& sprite = sprites_[static_cast<std::size_t>(i)];
        if (sprite.col == slot.x() && sprite.row == slot.y()) return i;
    }
    return -1;
}

void MetaspriteField::reportSlot(const std::optional<QPoint>& slot) {
    if (!slot) {
        emit statusChanged(QString("%1x%2 tiles, %3 sprite%4 placed")
                               .arg(tilesX_)
                               .arg(tilesY_)
                               .arg(spriteCount())
                               .arg(spriteCount() == 1 ? "" : "s"));
        return;
    }

    const QString where = QString("slot %1,%2").arg(slot->x()).arg(slot->y());

    if (const int index = spriteAt(*slot); index >= 0) {
        const PlacedSprite& sprite = sprites_[static_cast<std::size_t>(index)];
        emit statusChanged(QString("%1  id %2  mirror %3  %4  -- right-click to cycle mirroring, "
                                   "middle-click to remove")
                               .arg(where,
                                    formatId(sprite.id, sprite.heightTiles),
                                    mirrorName(sprite.mirror),
                                    sprite.source()));
        return;
    }

    if (!brush_) {
        emit statusChanged(QString("%1  empty -- pick a tile in the sheet to stamp one").arg(where));
        return;
    }
    emit statusChanged(QString("%1  empty -- click to stamp id %2 (%3)")
                           .arg(where, formatId(brush_->id, brush_->heightTiles), brush_->source()));
}

void MetaspriteField::mousePressEvent(QMouseEvent* event) {
    const auto slot = slotAt(event->pos());
    if (!slot) {
        QWidget::mousePressEvent(event);
        return;
    }

    switch (event->button()) {
        // Right-click steps the slot's mirroring: none -> V -> H -> H+V and
        // back round, modulo kMirrorModes. It acts on what is already there,
        // so an empty slot has nothing to cycle.
        case Qt::RightButton: {
            const int index = spriteAt(*slot);
            if (index < 0) {
                reportSlot(slot);
                return;
            }
            PlacedSprite& sprite = sprites_[static_cast<std::size_t>(index)];
            sprite.mirror = static_cast<Mirror>((static_cast<int>(sprite.mirror) + 1) % kMirrorModes);
            update(slotRect(*slot));
            emit spritesChanged();
            reportSlot(slot);
            return;
        }

        // Erasing moved here when right-click took over mirroring.
        case Qt::MiddleButton: {
            if (const int index = spriteAt(*slot); index >= 0) {
                sprites_.erase(sprites_.begin() + index);
                update(slotRect(*slot));
                emit spritesChanged();
                reportSlot(slot);
            }
            return;
        }

        case Qt::LeftButton:
            break;

        default:
            QWidget::mousePressEvent(event);
            return;
    }

    if (!brush_) {
        emit statusChanged("No tile picked -- pick one in the tile sheet first");
        return;
    }

    PlacedSprite sprite;
    sprite.col         = slot->x();
    sprite.row         = slot->y();
    sprite.id          = brush_->id;
    sprite.heightTiles = brush_->heightTiles;
    sprite.entry       = brush_->entry;
    sprite.include     = brush_->include;
    sprite.tile        = brush_->tile;
    sprite.image       = brush_->image;

    // One sprite per slot: a stamp replaces whatever was in it. Mirroring is
    // not carried over, since the new tile is a different tile.
    if (const int existing = spriteAt(*slot); existing >= 0) {
        sprites_.erase(sprites_.begin() + existing);
    }

    sprites_.push_back(std::move(sprite));
    update(slotRect(*slot));
    emit spritesChanged();
    reportSlot(slot);
}

void MetaspriteField::mouseMoveEvent(QMouseEvent* event) {
    const auto slot = slotAt(event->pos());
    if (slot != hovered_) {
        // Both the old and the new slot need their outline redrawn.
        for (const auto& which : {hovered_, slot}) {
            if (which) update(slotRect(*which));
        }
        hovered_ = slot;
    }
    reportSlot(slot);
    QWidget::mouseMoveEvent(event);
}

void MetaspriteField::leaveEvent(QEvent* event) {
    if (hovered_) {
        const QPoint slot = *hovered_;
        hovered_.reset();
        update(slotRect(slot));
    }
    reportSlot(std::nullopt);
    QWidget::leaveEvent(event);
}
