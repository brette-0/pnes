// 'platform NES' UI Toolkit

#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QCursor>
#include <QIntValidator>
#include <QFontMetrics>
#include <QtGlobal>
#include <QPainter>
#include <QFont>
#include <QObject>
#include <QEvent>
#include <algorithm>
#include <functional>
#include <QTimer>
#include <QTreeWidget>
#include <QMenu>
#include <QInputDialog>
#include <QMouseEvent>
#include <QColor>
#include <QVector>
#include <QSplitter>
#include <QSpinBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QFontDatabase>

namespace {

// Reference scaling: 8px/tile at 1080p, 16px/tile at 4K -- i.e. tile size
// tracks vertical resolution linearly. Fractional results are fine, visual
// precision isn't a goal here.
constexpr double kTilePxAt1080p = 8.0;
constexpr double kReferenceHeight = 1080.0;

// The grid drives the window's size, but never below this in either
// dimension, so a tiny tile count can't shrink the window to something
// unusable.
constexpr int kMinGridDimensionPx = 100;

// Same reference scaling as the tile grid: 100px at 1080p, 200px at 4K.
constexpr double kSidebarWidthPxAt1080p = 100.0;

// Reserve room for the window's own frame (title bar, borders) when sizing
// against the screen -- without this, a window requested at exactly the
// screen's available height is *taller* than what actually fits once its
// frame is added, and some window managers respond to that by force-tiling
// or otherwise reflowing the oversized window, which can crop or hide parts
// of it (the sidebar included).
constexpr int kWindowChromeMarginPx = 60;

double tilePxForScreen(const QRect& screenGeometry) {
    return kTilePxAt1080p * (screenGeometry.height() / kReferenceHeight);
}

int sidebarWidthForScreen(const QRect& screenGeometry) {
    return qRound(kSidebarWidthPxAt1080p * (screenGeometry.height() / kReferenceHeight));
}

// Re-fits `window` around its current contents and locks it at that size --
// the window is meant to be resized only by the app (as the grid or sidebar
// changes size), never dragged by the user. As a hard backstop against ever
// handing a window manager a window bigger than the screen (the viewport's
// tile limits should already prevent this, but the margin they budget for
// the window's own frame is an estimate) the result is also clamped to the
// screen's available area.
void lockWindowToContents(QMainWindow* window) {
    window->setMinimumSize(0, 0);
    window->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    window->adjustSize();

    QSize size = window->size();
    if (const QScreen* screen = window->screen()) {
        size = size.boundedTo(screen->availableGeometry().size());
    }
    window->setFixedSize(size);
}

// Notifies a callback whenever a watched widget's layout is invalidated --
// e.g. a child becoming visible/hidden, or being added/removed -- so the
// window can be re-fitted any time the *sidebar's* required height changes,
// not just when the viewport's tile counts do. Qt already posts exactly this
// event up the widget tree on any such change, so this just taps into it
// instead of every place in the sidebar that could grow or shrink its
// content needing to remember to trigger a resize itself.
class LayoutChangeNotifier : public QObject {
public:
    LayoutChangeNotifier(std::function<void()> callback, QObject* parent)
        : QObject(parent), callback_(std::move(callback)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::LayoutRequest) {
            callback_();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> callback_;
};

// Component-node data, stashed on the QTreeWidgetItem itself so both the
// sidebar tree, the properties panel, and the canvas can read/write it
// without a separate registry. kKindRole marks which items are components at
// all (as opposed to plain branch/root nodes), so behavior like
// prefix-locked renaming or being drawn on the canvas doesn't leak onto
// nodes it was never meant for.
//
// A textbox is the *origin* of text, not a container of it -- its position
// is a top-left anchor, and size/alignment describe how text flows from
// there, rather than each occupied cell separately holding a copy of it.
constexpr int kKindRole = Qt::UserRole;
constexpr int kTextContentRole = Qt::UserRole + 1;
constexpr int kPosXRole = Qt::UserRole + 2;
constexpr int kPosYRole = Qt::UserRole + 3;
constexpr int kSizeWRole = Qt::UserRole + 4;
constexpr int kSizeHRole = Qt::UserRole + 5;
constexpr int kAlignRole = Qt::UserRole + 6;
constexpr char kComponentKind[] = "component";
constexpr char kTextboxPrefix[] = "[T] ";

enum class TextAlign { Left = 0, Center = 1, Right = 2 };

bool isComponentItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kComponentKind);
}

// A viewport preview: fills the space it's given with the tile grid (its
// size is driven externally by the sidebar's tile counts, not by its own
// size hint), then draws every component node found in `tree` as a
// rectangular region at its stored position/size. Clicking and dragging a
// drawn region moves it (updating its position live); double-clicking it
// edits its text content. Both act through the tree item's data roles, so
// the sidebar's properties panel and the canvas always agree.
class TileGridWidget : public QWidget {
public:
    TileGridWidget(double tilePx, QTreeWidget* tree, QWidget* parent = nullptr)
        : QWidget(parent), tilePx_(tilePx), tree_(tree) {
        // A monospace font from the OS, sized to fill most of a cell's
        // height -- its glyphs are narrower than they are tall, though, so
        // drawCellGlyph() additionally stretches each one horizontally to
        // fill the (square) cell edge-to-edge.
        glyphFont_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        glyphFont_.setPixelSize(std::max(1, qRound(tilePx_ * 0.75)));
        naturalGlyphWidthPx_ = std::max(1, QFontMetrics(glyphFont_).horizontalAdvance(QLatin1Char('M')));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), Qt::black);

        for (QTreeWidgetItem* item : collectComponents()) {
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());
            const QRect region = componentRect(x, y, w, h);

            painter.fillRect(region, QColor(70, 130, 180));

            const QString text = item->data(0, kTextContentRole).toString();
            if (!text.isEmpty()) {
                painter.setFont(glyphFont_);
                painter.setPen(Qt::white);

                const auto align = static_cast<TextAlign>(item->data(0, kAlignRole).toInt());
                // Text flows row-major through the region at one character
                // per cell, wrapping to the next row after `w` characters
                // (extra characters beyond the region's w*h capacity are
                // dropped). Alignment positions each row's run of
                // characters within that row's w cells.
                for (int row = 0; row < h; ++row) {
                    const QString rowText = text.mid(row * w, w);
                    if (rowText.isEmpty()) {
                        break;
                    }
                    const int slack = w - rowText.length();
                    const int startCol = (align == TextAlign::Left)    ? 0
                                          : (align == TextAlign::Right) ? slack
                                                                         : slack / 2;
                    for (int i = 0; i < rowText.length(); ++i) {
                        drawCellGlyph(painter, tileRect(x + startCol + i, y + row), rowText.at(i));
                    }
                }
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            QTreeWidgetItem* hit = hitTest(event->pos());
            dragItem_ = hit;
            if (hit) {
                tree_->setCurrentItem(hit);
                dragAnchorCell_ = cellAt(event->pos());
                dragOriginX_ = hit->data(0, kPosXRole).toInt();
                dragOriginY_ = hit->data(0, kPosYRole).toInt();
            }
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragItem_ && (event->buttons() & Qt::LeftButton)) {
            const QPoint cell = cellAt(event->pos());
            const int newX = std::max(0, dragOriginX_ + (cell.x() - dragAnchorCell_.x()));
            const int newY = std::max(0, dragOriginY_ + (cell.y() - dragAnchorCell_.y()));
            // setData() drives QTreeWidget::itemChanged, which the caller
            // that installed this grid uses to repaint -- no need to call
            // update() here too.
            dragItem_->setData(0, kPosXRole, newX);
            dragItem_->setData(0, kPosYRole, newY);
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        dragItem_ = nullptr;
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        QTreeWidgetItem* hit = hitTest(event->pos());
        if (!hit) {
            return;
        }
        bool ok = false;
        const QString text = QInputDialog::getText(this, "Textbox content", "Text:", QLineEdit::Normal,
                                                     hit->data(0, kTextContentRole).toString(), &ok);
        if (ok) {
            hit->setData(0, kTextContentRole, text);
            // Text longer than the box is otherwise silently truncated by
            // the row-wrap in paintEvent() -- growing the box to fit
            // instead keeps newly-typed text visible without also having to
            // separately resize it.
            if (text.length() > hit->data(0, kSizeWRole).toInt()) {
                hit->setData(0, kSizeWRole, text.length());
            }
        }
    }

private:
    // Draws one glyph filling `cell` edge-to-edge. glyphFont_ is already
    // sized to fill the cell's height; monospace glyphs are narrower than
    // tall, though, so a per-cell horizontal-only scale stretches the
    // glyph's natural width out to the cell's actual width. Scaling is
    // applied via the painter's transform (translate to the cell's center,
    // scale X only, draw in the now-stretched local coordinate system)
    // rather than distorting the font itself, which Qt can't stretch
    // independently of its point size.
    void drawCellGlyph(QPainter& painter, const QRect& cell, QChar ch) const {
        const double scaleX = static_cast<double>(cell.width()) / naturalGlyphWidthPx_;
        painter.save();
        painter.translate(cell.center());
        painter.scale(scaleX, 1.0);
        const QRect local(-naturalGlyphWidthPx_ / 2, -cell.height() / 2, naturalGlyphWidthPx_, cell.height());
        painter.drawText(local, Qt::AlignCenter, QString(ch));
        painter.restore();
    }

    // tilePx_ is typically fractional (e.g. ~5.93px), so tile boundaries are
    // rounded to the nearest pixel independently rather than accumulated by
    // repeated addition -- that keeps every tile's edge aligned to the same
    // pixel regardless of which cell computes it, instead of drifting.
    QRect tileRect(int col, int row) const {
        const int cols = std::max(1, qRound(width() / tilePx_));
        const int rows = std::max(1, qRound(height() / tilePx_));
        const int left = qRound(col * tilePx_);
        const int top = qRound(row * tilePx_);
        const int right = (col >= cols - 1) ? width() : qRound((col + 1) * tilePx_);
        const int bottom = (row >= rows - 1) ? height() : qRound((row + 1) * tilePx_);
        return QRect(left, top, right - left, bottom - top);
    }

    QRect componentRect(int x, int y, int w, int h) const {
        return tileRect(x, y).united(tileRect(x + w - 1, y + h - 1));
    }

    QPoint cellAt(const QPoint& pos) const {
        const int cols = std::max(1, qRound(width() / tilePx_));
        const int rows = std::max(1, qRound(height() / tilePx_));
        const int col = std::clamp(static_cast<int>(pos.x() / tilePx_), 0, cols - 1);
        const int row = std::clamp(static_cast<int>(pos.y() / tilePx_), 0, rows - 1);
        return {col, row};
    }

    // Walks the whole tree (not just root's direct children) so components
    // nested deeper -- not reachable from the UI yet, but already valid
    // structurally -- are rendered and hit-testable too.
    QVector<QTreeWidgetItem*> collectComponents() const {
        QVector<QTreeWidgetItem*> result;
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* node) {
            for (int i = 0; i < node->childCount(); ++i) {
                QTreeWidgetItem* child = node->child(i);
                if (isComponentItem(child)) {
                    result.push_back(child);
                }
                visit(child);
            }
        };
        visit(tree_->invisibleRootItem());
        return result;
    }

    // Later-added components are drawn on top, so hit-testing prefers the
    // last match for overlapping regions to stay consistent with what's
    // visually on top.
    QTreeWidgetItem* hitTest(const QPoint& pos) const {
        const QPoint cell = cellAt(pos);
        QTreeWidgetItem* match = nullptr;
        for (QTreeWidgetItem* item : collectComponents()) {
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());
            if (cell.x() >= x && cell.x() < x + w && cell.y() >= y && cell.y() < y + h) {
                match = item;
            }
        }
        return match;
    }

    double tilePx_;
    QTreeWidget* tree_;
    QFont glyphFont_;
    int naturalGlyphWidthPx_ = 1;
    QTreeWidgetItem* dragItem_ = nullptr;
    QPoint dragAnchorCell_;
    int dragOriginX_ = 0;
    int dragOriginY_ = 0;
};

struct Sidebar {
    QDockWidget* dock;
    QTreeWidget* tree;
    QLineEdit* xEdit;
    QLineEdit* yEdit;
};

// `maxTilesX`/`maxTilesY` bound the fields to whatever will actually fit on
// the detected monitor at the current tile scale -- see the call site in
// main() for how those are derived.
Sidebar createSidebar(QWidget* parent, int widthPx, int maxTilesX, int maxTilesY) {
    auto* dock = new QDockWidget("Sidebar", parent);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setFixedWidth(widthPx);

    // NoDockWidgetFeatures means there's no user-facing way to close/hide
    // this dock, so any time it goes invisible it's a Qt layout glitch, not
    // a legitimate state -- most commonly triggered by the window resizes in
    // lockWindowToContents(), which can transiently unmap the dock via a
    // *deferred* Qt layout event. Reacting synchronously to that (as
    // lockWindowToContents also tries) can lose the race against the
    // deferred hide; queuing the re-show instead runs it after any pending
    // layout events have already fired, so it always wins.
    QObject::connect(dock, &QDockWidget::visibilityChanged, dock, [dock](bool visible) {
        if (!visible) {
            QTimer::singleShot(0, dock, [dock] { dock->setVisible(true); });
        }
    });

    // The sidebar's required height isn't fixed -- the properties panel
    // appears/disappears with selection, and nodes get added to the tree --
    // so the window (locked to a fixed size elsewhere) needs to be re-fit
    // any time that happens, not just when the viewport's tile counts
    // change. `parent` is always the QMainWindow here (see main()).
    if (auto* window = qobject_cast<QMainWindow*>(parent)) {
        window->installEventFilter(new LayoutChangeNotifier([window] { lockWindowToContents(window); }, parent));
    }

    auto* content = new QWidget(dock);
    auto* layout = new QVBoxLayout(content);

    // Node tree (Unity-style scene hierarchy), origin top of sidebar.
    auto* tree = new QTreeWidget(content);
    tree->setHeaderHidden(true);

    auto* rootItem = new QTreeWidgetItem(tree, QStringList{"root"});
    tree->addTopLevelItem(rootItem);
    tree->expandItem(rootItem);

    // Renaming a component must never lose the "[T]" prefix that marks it as
    // a textbox -- if an edit strips it, put it back rather than reject the
    // whole edit, so the rest of the typed name survives.
    QObject::connect(tree, &QTreeWidget::itemChanged, [](QTreeWidgetItem* item, int column) {
        if (column != 0 || !isComponentItem(item)) {
            return;
        }
        if (!item->text(0).startsWith(kTextboxPrefix)) {
            item->setText(0, QString(kTextboxPrefix) + item->text(0));
        }
    });

    // --- Properties panel: shows/edits the selected component's geometry
    // and alignment. Hidden entirely (not just grayed out) whenever the
    // selection isn't a component (e.g. root, or nothing selected) -- a
    // root/branch node has no such properties at all, so there's nothing
    // here for it to show.
    auto* properties = new QWidget(content);
    auto* posXSpin = new QSpinBox(properties);
    auto* posYSpin = new QSpinBox(properties);
    auto* sizeWSpin = new QSpinBox(properties);
    auto* sizeHSpin = new QSpinBox(properties);
    auto* alignCombo = new QComboBox(properties);
    posXSpin->setRange(0, 999);
    posYSpin->setRange(0, 999);
    sizeWSpin->setRange(1, 999);
    sizeHSpin->setRange(1, 999);
    alignCombo->addItems({"Left", "Center", "Right"});

    auto* propertiesForm = new QFormLayout(properties);
    // The sidebar is only ~100-200px wide -- a label sharing a row with its
    // field gets squeezed down to nothing legible. Wrapping long rows puts
    // the label on its own row above the field instead, so it's always
    // fully readable.
    propertiesForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    propertiesForm->addRow("Position X", posXSpin);
    propertiesForm->addRow("Position Y", posYSpin);
    propertiesForm->addRow("Size W", sizeWSpin);
    propertiesForm->addRow("Size H", sizeHSpin);
    propertiesForm->addRow("Alignment", alignCombo);
    properties->setVisible(false);

    // Populates the panel's fields from `item` without re-triggering the
    // edit handlers below (which would otherwise write the same values
    // straight back -- harmless, but pointless).
    auto populateFrom = [posXSpin, posYSpin, sizeWSpin, sizeHSpin, alignCombo](QTreeWidgetItem* item) {
        const QSignalBlocker bx(posXSpin);
        const QSignalBlocker by(posYSpin);
        const QSignalBlocker bw(sizeWSpin);
        const QSignalBlocker bh(sizeHSpin);
        const QSignalBlocker ba(alignCombo);
        posXSpin->setValue(item->data(0, kPosXRole).toInt());
        posYSpin->setValue(item->data(0, kPosYRole).toInt());
        sizeWSpin->setValue(item->data(0, kSizeWRole).toInt());
        sizeHSpin->setValue(item->data(0, kSizeHRole).toInt());
        alignCombo->setCurrentIndex(item->data(0, kAlignRole).toInt());
    };

    QObject::connect(tree, &QTreeWidget::currentItemChanged,
                      [properties, populateFrom](QTreeWidgetItem* current, QTreeWidgetItem*) {
                          const bool selected = isComponentItem(current);
                          properties->setVisible(selected);
                          if (selected) {
                              populateFrom(current);
                          }
                      });

    // The selected item's geometry can also change from outside the panel
    // -- dragging it on the canvas, or the text-length auto-grow below --
    // so keep the panel's fields from going stale whenever that happens.
    QObject::connect(tree, &QTreeWidget::itemChanged,
                      [tree, populateFrom](QTreeWidgetItem* item, int column) {
                          if (column == 0 && item == tree->currentItem() && isComponentItem(item)) {
                              populateFrom(item);
                          }
                      });

    auto writeToSelection = [tree](int role, int value) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isComponentItem(item)) {
            item->setData(0, role, value);
        }
    };
    QObject::connect(posXSpin, qOverload<int>(&QSpinBox::valueChanged),
                      [writeToSelection](int v) { writeToSelection(kPosXRole, v); });
    QObject::connect(posYSpin, qOverload<int>(&QSpinBox::valueChanged),
                      [writeToSelection](int v) { writeToSelection(kPosYRole, v); });
    QObject::connect(sizeWSpin, qOverload<int>(&QSpinBox::valueChanged),
                      [writeToSelection](int v) { writeToSelection(kSizeWRole, v); });
    QObject::connect(sizeHSpin, qOverload<int>(&QSpinBox::valueChanged),
                      [writeToSelection](int v) { writeToSelection(kSizeHRole, v); });
    QObject::connect(alignCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                      [writeToSelection](int v) { writeToSelection(kAlignRole, v); });

    // addComponentNode() takes an explicit parent so nesting components
    // under other nodes (not just root) already works -- there's just no UI
    // for it yet, since every add here always targets root, keeping newly
    // added components as root's siblings-of-each-other for now. New
    // textboxes start at the origin (0,0) with a 1x1 footprint; drag them on
    // the canvas or use the properties panel to place/resize them.
    auto componentCounter = std::make_shared<int>(1);
    auto addComponentNode = [componentCounter](QTreeWidgetItem* parent) {
        auto* child = new QTreeWidgetItem(parent, QStringList{QString(kTextboxPrefix) + "Textbox " +
                                                                QString::number((*componentCounter)++)});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kComponentKind));
        child->setData(0, kTextContentRole, QString());
        child->setData(0, kPosXRole, 0);
        child->setData(0, kPosYRole, 0);
        child->setData(0, kSizeWRole, 1);
        child->setData(0, kSizeHRole, 1);
        child->setData(0, kAlignRole, static_cast<int>(TextAlign::Left));
        parent->setExpanded(true);
        return child;
    };

    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(tree, &QTreeWidget::customContextMenuRequested,
                      [tree, rootItem, addComponentNode](const QPoint& pos) {
                          QMenu menu;
                          QAction* addAction = menu.addAction("Add Textbox Component");
                          if (menu.exec(tree->viewport()->mapToGlobal(pos)) == addAction) {
                              tree->setCurrentItem(addComponentNode(rootItem));
                          }
                      });

    // Tree and properties share the space above the viewport controls,
    // resizable against each other; the viewport controls stay pinned to
    // the sidebar's bottom below the splitter.
    auto* splitter = new QSplitter(Qt::Vertical, content);
    splitter->addWidget(tree);
    splitter->addWidget(properties);
    layout->addWidget(splitter, /*stretch=*/1);

    auto* xEdit = new QLineEdit(content);
    auto* yEdit = new QLineEdit(content);
    // Viewport size is bounded to [1, maxTiles] -- 1 so there's always
    // something to render, maxTiles so the grid can't demand a window bigger
    // than the monitor it's on. QIntValidator's bounds only reject values
    // it's sure can't become valid (e.g. it still lets a bare "0" through,
    // or lets you type past the top digit-by-digit if a prefix is
    // plausible), so they alone don't keep the field within range -- clamp
    // it ourselves whenever it strays outside, so the field always shows
    // what it actually resolves to. (editingFinished, the more obvious hook,
    // doesn't reliably fire here -- textChanged does.)
    xEdit->setValidator(new QIntValidator(1, maxTilesX, xEdit));
    yEdit->setValidator(new QIntValidator(1, maxTilesY, yEdit));
    auto clamp = [](QLineEdit* edit, const QString& text, int maxTiles) {
        const int value = text.toInt();
        if (value < 1) {
            edit->setText("1");
        } else if (value > maxTiles) {
            edit->setText(QString::number(maxTiles));
        }
    };
    QObject::connect(xEdit, &QLineEdit::textChanged, [xEdit, clamp, maxTilesX](const QString& text) {
        clamp(xEdit, text, maxTilesX);
    });
    QObject::connect(yEdit, &QLineEdit::textChanged, [yEdit, clamp, maxTilesY](const QString& text) {
        clamp(yEdit, text, maxTilesY);
    });

    // Width to fit exactly 4 digits, plus room for the line edit's frame.
    const int digitsWidth = xEdit->fontMetrics().horizontalAdvance(QLatin1String("9999"));
    const int fieldWidth = digitsWidth + 16;
    xEdit->setFixedWidth(fieldWidth);
    yEdit->setFixedWidth(fieldWidth);

    // NES resolution (256x240) is 32x30 tiles -- a sensible default viewport,
    // clamped down if the monitor can't actually fit that many tiles.
    xEdit->setText(QString::number(std::min(32, maxTilesX)));
    yEdit->setText(QString::number(std::min(30, maxTilesY)));

    layout->addWidget(new QLabel("Viewport (tx)", content));
    auto* form = new QFormLayout();
    form->addRow("X:", xEdit);
    form->addRow("Y:", yEdit);
    layout->addLayout(form);

    dock->setWidget(content);
    return {dock, tree, xEdit, yEdit};
}

// Creates the viewport panel widget (not yet parented), sized to
// `tilesX x tilesY` tiles at `tilePx` pixels each (floored at
// kMinGridDimensionPx per axis). The grid drives the whole window's size and
// is kept in sync whenever the sidebar's X/Y fields change -- but that only
// works once the grid is actually installed as `window`'s central widget, so
// the caller must install it before invoking the returned sync function the
// first time.
struct ViewportPanel {
    TileGridWidget* grid;
    std::function<void()> sync;
};

ViewportPanel createViewportPanel(QMainWindow* window, double tilePx, QTreeWidget* tree, QLineEdit* xEdit,
                                   QLineEdit* yEdit) {
    auto* grid = new TileGridWidget(tilePx, tree);

    // Any change to a component's data (position, size, alignment, text,
    // name) goes through the tree item's setData(), which Qt reports via
    // itemChanged regardless of what triggered it -- so this one connection
    // keeps the canvas in sync with the tree, the properties panel, and
    // dragging on the canvas itself, without each of those needing to know
    // about the grid directly.
    QObject::connect(tree, &QTreeWidget::itemChanged, grid, [grid](QTreeWidgetItem*, int) { grid->update(); });

    auto sync = std::make_shared<std::function<void()>>();
    *sync = [grid, tilePx, xEdit, yEdit, window] {
        const int tilesX = std::max(1, xEdit->text().toInt());
        const int tilesY = std::max(1, yEdit->text().toInt());
        const int gridWidth = std::max(kMinGridDimensionPx, qRound(tilesX * tilePx));
        const int gridHeight = std::max(kMinGridDimensionPx, qRound(tilesY * tilePx));
        grid->setFixedSize(gridWidth, gridHeight);
        lockWindowToContents(window);
    };

    // Typing "32" fires textChanged twice in the same instant, and the
    // clamp() calls in createSidebar can fire it again -- each resulting in
    // a full window resize via lockWindowToContents(). Coalescing same-tick
    // changes into a single resize (via a zero-delay singleShot, which runs
    // once the current burst of signals has finished) cuts down how often
    // that resize -- the thing that can transiently unmap the sidebar dock
    // -- happens at all.
    auto* debounce = new QTimer(window);
    debounce->setSingleShot(true);
    debounce->setInterval(0);
    QObject::connect(debounce, &QTimer::timeout, window, [sync] { (*sync)(); });
    QObject::connect(xEdit, &QLineEdit::textChanged, debounce, [debounce] { debounce->start(); });
    QObject::connect(yEdit, &QLineEdit::textChanged, debounce, [debounce] { debounce->start(); });

    return {grid, [sync] { (*sync)(); }};
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("uitk");

    // QGuiApplication::primaryScreen() is unreliable under Wayland, which has
    // no "primary output" protocol -- the compositor's enumeration order
    // decides it, not the monitor the user actually cares about. Use the
    // screen under the cursor instead, falling back to primaryScreen() if
    // that ever comes back null (e.g. cursor position unavailable).
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) {
        screen = app.primaryScreen();
    }
    const double tilePx = tilePxForScreen(screen->geometry());
    const int sidebarWidthPx = sidebarWidthForScreen(screen->geometry());

    // Bound the viewport to what can actually be rendered on the detected
    // monitor: its available area (screen minus taskbars/docks), minus the
    // sidebar's own fixed width, converted from pixels to tiles at the
    // current scale.
    const QRect available = screen->availableGeometry();
    const int maxTilesX = std::max(
        1, static_cast<int>((available.width() - sidebarWidthPx - kWindowChromeMarginPx) / tilePx));
    const int maxTilesY =
        std::max(1, static_cast<int>((available.height() - kWindowChromeMarginPx) / tilePx));

    const Sidebar sidebar = createSidebar(&window, sidebarWidthPx, maxTilesX, maxTilesY);
    const ViewportPanel viewport =
        createViewportPanel(&window, tilePx, sidebar.tree, sidebar.xEdit, sidebar.yEdit);
    window.setCentralWidget(viewport.grid);
    window.addDockWidget(Qt::RightDockWidgetArea, sidebar.dock);
    viewport.sync();

    window.show();

    auto* captureTimer = new QTimer(&window);
    QObject::connect(captureTimer, &QTimer::timeout, [&window, screen] {
        screen->grabWindow(window.winId()).save("/tmp/uitk_capture.png");
    });
    captureTimer->start(200);

    // QApplication::exec() returns once the window is closed (including via
    // the taskbar/titlebar X, which QMainWindow handles by default -- no
    // closeEvent override needed) or QApplication::quit() is called. Qt
    // tears down its own event loop and widgets on the way out, so no
    // manual buffer flushing is needed here.
    return app.exec();
}
