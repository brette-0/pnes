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
#include <QComboBox>
#include <QSignalBlocker>
#include <QFontDatabase>
#include <QMenuBar>
#include <QStatusBar>
#include <QAction>
#include <QKeySequence>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QtAlgorithms>
#include <QRegularExpression>
#include <QSet>
#include <QHash>
#include <QBrush>
#include <map>
#include <optional>
#include <utility>

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

// Same reference scaling as the tile grid: 200px at 1080p, 400px at 4K
// (doubled from the original 100px/200px).
constexpr double kSidebarWidthPxAt1080p = 200.0;

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

// QMainWindow subclass that defers to `confirmClose` -- wired up once the
// sidebar's File-menu/dirty-tracking logic exists -- before actually
// closing, so closing the window goes through the same "save changes?"
// prompt as New/Open. Overriding closeEvent() needs no signals/slots, so
// there's no need for Q_OBJECT/moc here despite the QObject-derived base.
class UitkMainWindow : public QMainWindow {
public:
    using QMainWindow::QMainWindow;
    std::function<bool()> confirmClose;

protected:
    void closeEvent(QCloseEvent* event) override {
        if (confirmClose && !confirmClose()) {
            event->ignore();
            return;
        }
        QMainWindow::closeEvent(event);
    }
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
// kPos*Role/kSize*Role hold the *resolved* (computed) values -- the ints the
// canvas and hit-testing actually read. kPos*ExprRole/kSize*ExprRole hold what
// the user actually typed for that property: either a bare literal ("5") or a
// formula referencing other nodes by name ("Player1.pos.x + 1"). The resolver
// (see ExprParser/evalExpr/resolveAll's definition in createSidebar) turns
// the latter into the former.
constexpr int kPosXRole = Qt::UserRole + 2;
constexpr int kPosYRole = Qt::UserRole + 3;
constexpr int kSizeWRole = Qt::UserRole + 4;
constexpr int kSizeHRole = Qt::UserRole + 5;
constexpr int kAlignRole = Qt::UserRole + 6;
constexpr int kPosXExprRole = Qt::UserRole + 7;
constexpr int kPosYExprRole = Qt::UserRole + 8;
constexpr int kSizeWExprRole = Qt::UserRole + 9;
constexpr int kSizeHExprRole = Qt::UserRole + 10;
// Set by the resolver when one or more of a component's properties couldn't
// be resolved -- either a parse error, a reference to an unknown/duplicate
// name, or a dependency cycle that never bottoms out.
constexpr int kErrorRole = Qt::UserRole + 11;
// The last name (including the "[T] " prefix) that passed validation --
// restored verbatim if an edit produces something that isn't a valid,
// unique identifier.
constexpr int kLastValidNameRole = Qt::UserRole + 12;
// Bitmask (bit 0=posX, 1=posY, 2=sizeW, 3=sizeH) of specifically which
// properties failed to resolve, so the properties panel can flag the exact
// field at fault rather than just the node as a whole.
constexpr int kErrorMaskRole = Qt::UserRole + 13;
constexpr char kComponentKind[] = "component";
constexpr char kTextboxPrefix[] = "[T] ";

enum class TextAlign { Left = 0, Center = 1, Right = 2 };

bool isComponentItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kComponentKind);
}

// Walks the whole tree (not just direct children) collecting every component
// node -- shared by the canvas (rendering/hit-testing), name validation, and
// the expression resolver.
QVector<QTreeWidgetItem*> collectComponentItems(QTreeWidgetItem* node) {
    QVector<QTreeWidgetItem*> result;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* n) {
        for (int i = 0; i < n->childCount(); ++i) {
            QTreeWidgetItem* child = n->child(i);
            if (isComponentItem(child)) {
                result.push_back(child);
            }
            visit(child);
        }
    };
    visit(node);
    return result;
}

// --- Property expressions -----------------------------------------------
//
// A property's stored text is a small arithmetic expression over integer
// literals, +/-/*//, parentheses, and identifiers of the form
// `NodeName.pos.x`, `NodeName.pos.y`, `NodeName.size.w`, `NodeName.size.h`
// (referencing another component by its bare name, sans the "[T] " prefix --
// the same name that will identify it in generated C++), plus the two bare
// globals VIEWPORT_TX and VIEWPORT_PY (the viewport's configured tile
// width/height). Node names are therefore constrained to be valid C++
// identifiers and unique, since they're both the expression namespace here
// and the symbol that later code generation will emit.
struct ExprNode {
    enum class Kind { Number, Ident, Add, Sub, Mul, Div, Shl, Shr, Neg };
    Kind kind = Kind::Number;
    long long number = 0;
    QString identName;  // component base name, or "VIEWPORT_TX"/"VIEWPORT_PY"
    int identProp = -1;  // 0=pos.x, 1=pos.y, 2=size.w, 3=size.h; -1 for the bare globals
    std::shared_ptr<ExprNode> a;
    std::shared_ptr<ExprNode> b;
};
using ExprPtr = std::shared_ptr<ExprNode>;

// Small hand-rolled recursive-descent parser -- the grammar is deliberately
// tiny (arithmetic + dotted identifiers), so a dependency like a full
// expression-parsing library would be overkill.
class ExprParser {
public:
    explicit ExprParser(const QString& src) : s_(src) {}

    ExprPtr parse(bool& ok) {
        ok = true;
        skipSpace();
        ExprPtr e = parseShift(ok);
        skipSpace();
        if (!ok || pos_ != s_.length()) {
            ok = false;
            return nullptr;
        }
        return e;
    }

private:
    void skipSpace() {
        while (pos_ < s_.length() && s_.at(pos_).isSpace()) ++pos_;
    }
    QChar peek() const { return pos_ < s_.length() ? s_.at(pos_) : QChar(); }
    QChar peek2() const { return pos_ + 1 < s_.length() ? s_.at(pos_ + 1) : QChar(); }

    // C++'s own precedence has shift binding weaker than +/- (so
    // `a + b >> 1` parses the way it would in generated C++ code), hence
    // this sits above parseExpr rather than beside it.
    ExprPtr parseShift(bool& ok) {
        ExprPtr left = parseExpr(ok);
        while (ok) {
            skipSpace();
            const QChar c = peek();
            if ((c != '<' && c != '>') || peek2() != c) break;
            pos_ += 2;
            ExprPtr right = parseExpr(ok);
            if (!ok) return nullptr;
            auto node = std::make_shared<ExprNode>();
            node->kind = (c == '<') ? ExprNode::Kind::Shl : ExprNode::Kind::Shr;
            node->a = left;
            node->b = right;
            left = node;
        }
        return ok ? left : nullptr;
    }

    ExprPtr parseExpr(bool& ok) {
        ExprPtr left = parseTerm(ok);
        while (ok) {
            skipSpace();
            const QChar c = peek();
            if (c != '+' && c != '-') break;
            ++pos_;
            ExprPtr right = parseTerm(ok);
            if (!ok) return nullptr;
            auto node = std::make_shared<ExprNode>();
            node->kind = (c == '+') ? ExprNode::Kind::Add : ExprNode::Kind::Sub;
            node->a = left;
            node->b = right;
            left = node;
        }
        return ok ? left : nullptr;
    }

    ExprPtr parseTerm(bool& ok) {
        ExprPtr left = parseFactor(ok);
        while (ok) {
            skipSpace();
            const QChar c = peek();
            if (c != '*' && c != '/') break;
            ++pos_;
            ExprPtr right = parseFactor(ok);
            if (!ok) return nullptr;
            auto node = std::make_shared<ExprNode>();
            node->kind = (c == '*') ? ExprNode::Kind::Mul : ExprNode::Kind::Div;
            node->a = left;
            node->b = right;
            left = node;
        }
        return ok ? left : nullptr;
    }

    ExprPtr parseFactor(bool& ok) {
        skipSpace();
        const QChar c = peek();
        if (c == '-') {
            ++pos_;
            ExprPtr inner = parseFactor(ok);
            if (!ok) return nullptr;
            auto node = std::make_shared<ExprNode>();
            node->kind = ExprNode::Kind::Neg;
            node->a = inner;
            return node;
        }
        if (c == '(') {
            ++pos_;
            ExprPtr inner = parseExpr(ok);
            skipSpace();
            if (!ok || peek() != ')') {
                ok = false;
                return nullptr;
            }
            ++pos_;
            return inner;
        }
        if (c.isDigit()) {
            const int start = pos_;
            while (pos_ < s_.length() && s_.at(pos_).isDigit()) ++pos_;
            auto node = std::make_shared<ExprNode>();
            node->kind = ExprNode::Kind::Number;
            node->number = s_.mid(start, pos_ - start).toLongLong();
            return node;
        }
        if (c.isLetter() || c == '_') {
            const int start = pos_;
            while (pos_ < s_.length() &&
                   (s_.at(pos_).isLetterOrNumber() || s_.at(pos_) == '_' || s_.at(pos_) == '.')) {
                ++pos_;
            }
            const QString token = s_.mid(start, pos_ - start);
            const QStringList parts = token.split('.');
            auto node = std::make_shared<ExprNode>();
            node->kind = ExprNode::Kind::Ident;
            if (parts.size() == 1) {
                if (token != QLatin1String("VIEWPORT_TX") && token != QLatin1String("VIEWPORT_PY")) {
                    ok = false;
                    return nullptr;
                }
                node->identName = token;
                node->identProp = -1;
                return node;
            }
            if (parts.size() == 3) {
                static const QHash<QString, int> kPropMap{
                    {"pos.x", 0}, {"pos.y", 1}, {"size.w", 2}, {"size.h", 3}};
                const QString propKey = parts.at(1) + "." + parts.at(2);
                if (!kPropMap.contains(propKey)) {
                    ok = false;
                    return nullptr;
                }
                node->identName = parts.at(0);
                node->identProp = kPropMap.value(propKey);
                return node;
            }
            ok = false;
            return nullptr;
        }
        ok = false;
        return nullptr;
    }

    QString s_;
    int pos_ = 0;
};

// Evaluates `node`, deferring to `resolveIdent` for identifiers -- it returns
// nullopt for anything not yet resolvable (an as-yet-unresolved dependency),
// which propagates up as an overall nullopt so the caller's fixed-point loop
// knows to simply try again on a later pass rather than treating it as an
// error immediately.
std::optional<long long> evalExpr(
    const ExprPtr& node, const std::function<std::optional<long long>(const QString&, int)>& resolveIdent) {
    if (!node) return std::nullopt;
    switch (node->kind) {
        case ExprNode::Kind::Number:
            return node->number;
        case ExprNode::Kind::Ident:
            return resolveIdent(node->identName, node->identProp);
        case ExprNode::Kind::Neg: {
            const auto v = evalExpr(node->a, resolveIdent);
            return v ? std::optional<long long>(-*v) : std::nullopt;
        }
        default: {
            const auto a = evalExpr(node->a, resolveIdent);
            const auto b = evalExpr(node->b, resolveIdent);
            if (!a || !b) return std::nullopt;
            switch (node->kind) {
                case ExprNode::Kind::Add: return *a + *b;
                case ExprNode::Kind::Sub: return *a - *b;
                case ExprNode::Kind::Mul: return *a * *b;
                case ExprNode::Kind::Div: return (*b != 0) ? std::optional<long long>(*a / *b) : std::nullopt;
                // Matches C++'s own undefined-behavior boundary: a negative
                // or out-of-range shift count is rejected as unresolvable
                // rather than silently producing a nonsense value.
                case ExprNode::Kind::Shl:
                    return (*b >= 0 && *b < 63) ? std::optional<long long>(*a << *b) : std::nullopt;
                case ExprNode::Kind::Shr:
                    return (*b >= 0 && *b < 63) ? std::optional<long long>(*a >> *b) : std::nullopt;
                default: return std::nullopt;
            }
        }
    }
}

// .uis ("User Interface Scene") file format: a JSON object holding a flat
// "version" and a "nodes" array -- root's own children, each recursively
// carrying its own "children". Only component nodes carry geometry/text;
// anything else (currently just "root" and, structurally, future branch
// nodes) round-trips as a plain named node.
QJsonObject serializeNode(const QTreeWidgetItem* item) {
    QJsonObject obj;
    obj["name"] = item->text(0);
    if (isComponentItem(item)) {
        obj["kind"] = QStringLiteral("component");
        obj["posXExpr"] = item->data(0, kPosXExprRole).toString();
        obj["posYExpr"] = item->data(0, kPosYExprRole).toString();
        obj["sizeWExpr"] = item->data(0, kSizeWExprRole).toString();
        obj["sizeHExpr"] = item->data(0, kSizeHExprRole).toString();
        obj["align"] = item->data(0, kAlignRole).toInt();
        obj["text"] = item->data(0, kTextContentRole).toString();
    } else {
        obj["kind"] = QStringLiteral("branch");
    }
    QJsonArray children;
    for (int i = 0; i < item->childCount(); ++i) {
        children.append(serializeNode(item->child(i)));
    }
    obj["children"] = children;
    return obj;
}

void deserializeNode(QTreeWidgetItem* parent, const QJsonObject& obj) {
    auto* item = new QTreeWidgetItem(parent, QStringList{obj["name"].toString()});
    if (obj["kind"].toString() == QLatin1String("component")) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kKindRole, QString(kComponentKind));
        const QString posXExpr = obj["posXExpr"].toString(QStringLiteral("0"));
        const QString posYExpr = obj["posYExpr"].toString(QStringLiteral("0"));
        const QString sizeWExpr = obj["sizeWExpr"].toString(QStringLiteral("1"));
        const QString sizeHExpr = obj["sizeHExpr"].toString(QStringLiteral("1"));
        item->setData(0, kPosXExprRole, posXExpr);
        item->setData(0, kPosYExprRole, posYExpr);
        item->setData(0, kSizeWExprRole, sizeWExpr);
        item->setData(0, kSizeHExprRole, sizeHExpr);
        // Best-effort literal seed so the canvas has something sane to paint
        // before the resolver's first pass runs (called once by the caller
        // after the whole scene is loaded).
        bool ok = false;
        item->setData(0, kPosXRole, posXExpr.toInt(&ok));
        if (!ok) item->setData(0, kPosXRole, 0);
        ok = false;
        item->setData(0, kPosYRole, posYExpr.toInt(&ok));
        if (!ok) item->setData(0, kPosYRole, 0);
        ok = false;
        item->setData(0, kSizeWRole, sizeWExpr.toInt(&ok));
        if (!ok) item->setData(0, kSizeWRole, 1);
        ok = false;
        item->setData(0, kSizeHRole, sizeHExpr.toInt(&ok));
        if (!ok) item->setData(0, kSizeHRole, 1);
        item->setData(0, kAlignRole, obj["align"].toInt());
        item->setData(0, kTextContentRole, obj["text"].toString());
        item->setData(0, kLastValidNameRole, item->text(0));
    }
    for (const QJsonValue& child : obj["children"].toArray()) {
        deserializeNode(item, child.toObject());
    }
}

bool writeUisFile(const QString& path, const QTreeWidgetItem* rootItem) {
    QJsonArray nodes;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        nodes.append(serializeNode(rootItem->child(i)));
    }
    QJsonObject doc;
    doc["version"] = 1;
    doc["nodes"] = nodes;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(doc).toJson());
    return true;
}

// Parses a .uis file's node list without touching any tree -- callers apply
// it (or don't, on failure) themselves, so a corrupt/unreadable file never
// wipes out whatever scene was already open.
bool parseUisFile(const QString& path, QJsonArray& outNodes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    outNodes = doc.object()["nodes"].toArray();
    return true;
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
            // Dragging always overwrites the position with a plain literal,
            // even if it was previously a formula -- there's no sensible way
            // to "drag" a computed value, so direct manipulation just
            // replaces it. setData() drives QTreeWidget::itemChanged, which
            // both the resolver and the caller that installed this grid
            // (repainting) are hooked to -- no need to call update() here.
            dragItem_->setData(0, kPosXExprRole, QString::number(newX));
            dragItem_->setData(0, kPosYExprRole, QString::number(newY));
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
            // separately resize it. Like a drag, this overwrites any
            // existing width formula with a plain literal.
            if (text.length() > hit->data(0, kSizeWRole).toInt()) {
                hit->setData(0, kSizeWExprRole, QString::number(text.length()));
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
    QVector<QTreeWidgetItem*> collectComponents() const { return collectComponentItems(tree_->invisibleRootItem()); }

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

    // `parent` is always the UitkMainWindow constructed in main().
    auto* window = static_cast<UitkMainWindow*>(parent);

    // The sidebar's required height isn't fixed -- the properties panel
    // appears/disappears with selection, and nodes get added to the tree --
    // so the window (locked to a fixed size elsewhere) needs to be re-fit
    // any time that happens, not just when the viewport's tile counts
    // change.
    window->installEventFilter(new LayoutChangeNotifier([window] { lockWindowToContents(window); }, window));

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
    // whole edit, so the rest of the typed name survives. The name after the
    // prefix also has to be a valid, unique C++ identifier: it's both the
    // namespace expressions reference other nodes through, and the symbol
    // that later code generation will emit, so anything else is reverted
    // outright to the last name that was valid.
    QObject::connect(tree, &QTreeWidget::itemChanged, [](QTreeWidgetItem* item, int column) {
        if (column != 0 || !isComponentItem(item)) {
            return;
        }
        QString text = item->text(0);
        if (!text.startsWith(kTextboxPrefix)) {
            text = QString(kTextboxPrefix) + text;
        }
        const QString base = text.mid(QString(kTextboxPrefix).length());
        static const QRegularExpression kIdentRe(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        bool valid = kIdentRe.match(base).hasMatch();
        if (valid) {
            for (QTreeWidgetItem* other : collectComponentItems(item->treeWidget()->invisibleRootItem())) {
                if (other != item && other->text(0) == text) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            const QString last = item->data(0, kLastValidNameRole).toString();
            if (item->text(0) != last) {
                // setText() re-enters this handler synchronously; the nested
                // call sees an already-valid name and returns having stored
                // it, so this call has nothing left to do once it returns.
                item->setText(0, last);
            }
            return;
        }
        if (item->text(0) != text) {
            item->setText(0, text);
        }
        item->setData(0, kLastValidNameRole, text);
    });

    // --- File menu: New / Open / Save / Save As, plus unsaved-changes
    // tracking so those and closing the window never silently discard work.
    // An empty currentPath means "no file yet" -- a new scene doesn't ask
    // for a name until it's actually saved.
    auto currentPath = std::make_shared<QString>();
    auto dirty = std::make_shared<bool>(false);
    // Suppresses dirty-marking while a scene is being rebuilt wholesale
    // (New/Open) -- that goes through the same setData()/itemChanged path a
    // real edit would, but starting or having just opened a scene isn't
    // itself an unsaved change.
    auto loading = std::make_shared<bool>(false);

    // Forward-declared the same way ViewportPanel's `sync` is: connections
    // below can capture and call through this pointer immediately, but the
    // actual resolution logic is only assigned once xEdit/yEdit (needed for
    // VIEWPORT_TX/VIEWPORT_PY) exist, further down.
    auto resolveAllPtr = std::make_shared<std::function<void()>>([] {});

    auto updateTitle = [window, currentPath, dirty] {
        const QString name =
            currentPath->isEmpty() ? QStringLiteral("Untitled") : QFileInfo(*currentPath).fileName();
        window->setWindowTitle(QString("uitk - %1%2").arg(name, *dirty ? "*" : ""));
    };
    updateTitle();

    QObject::connect(tree, &QTreeWidget::itemChanged, [dirty, loading, updateTitle](QTreeWidgetItem*, int) {
        if (!*loading) {
            *dirty = true;
            updateTitle();
        }
    });

    // Any structural or property change can affect what's resolvable (a
    // rename changes the identifier other nodes reference; any property edit
    // can be someone else's dependency) -- re-resolve everything on every
    // change. Skipped while a scene is being rebuilt wholesale (New/Open),
    // which call resolveAllPtr explicitly once after they finish instead.
    QObject::connect(tree, &QTreeWidget::itemChanged, [resolveAllPtr, loading](QTreeWidgetItem*, int) {
        if (!*loading) {
            (*resolveAllPtr)();
        }
    });

    auto doSaveAs = [window, rootItem, currentPath, dirty, updateTitle]() {
        QString path = QFileDialog::getSaveFileName(window, "Save Scene", QString(), "UI Scene (*.uis)");
        if (path.isEmpty()) {
            return false;
        }
        if (!path.endsWith(".uis", Qt::CaseInsensitive)) {
            path += ".uis";
        }
        if (!writeUisFile(path, rootItem)) {
            QMessageBox::warning(window, "Save Failed", "Could not write file:\n" + path);
            return false;
        }
        *currentPath = path;
        *dirty = false;
        updateTitle();
        return true;
    };

    auto doSave = [rootItem, currentPath, dirty, updateTitle, doSaveAs, window]() {
        if (currentPath->isEmpty()) {
            return doSaveAs();
        }
        if (!writeUisFile(*currentPath, rootItem)) {
            QMessageBox::warning(window, "Save Failed", "Could not write file:\n" + *currentPath);
            return false;
        }
        *dirty = false;
        updateTitle();
        return true;
    };

    // Shared by New, Open, and closing the window -- returns whether it's
    // OK to proceed (false only when the user picks Cancel, or picks Save
    // and the save itself is cancelled/fails).
    auto confirmDiscard = [window, currentPath, dirty, doSave]() {
        if (!*dirty) {
            return true;
        }
        const QString name =
            currentPath->isEmpty() ? QStringLiteral("Untitled") : QFileInfo(*currentPath).fileName();
        const auto choice = QMessageBox::question(
            window, "Unsaved Changes", QString("Save changes to \"%1\" before continuing?").arg(name),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (choice == QMessageBox::Cancel) {
            return false;
        }
        return choice == QMessageBox::Discard || doSave();
    };
    window->confirmClose = confirmDiscard;

    auto doNew = [rootItem, currentPath, dirty, loading, updateTitle, confirmDiscard, resolveAllPtr] {
        if (!confirmDiscard()) {
            return;
        }
        *loading = true;
        qDeleteAll(rootItem->takeChildren());
        *loading = false;
        (*resolveAllPtr)();
        currentPath->clear();
        *dirty = false;
        updateTitle();
    };

    auto doOpen = [window, tree, rootItem, currentPath, dirty, loading, updateTitle, confirmDiscard,
                   resolveAllPtr] {
        if (!confirmDiscard()) {
            return;
        }
        const QString path = QFileDialog::getOpenFileName(window, "Open Scene", QString(), "UI Scene (*.uis)");
        if (path.isEmpty()) {
            return;
        }
        // Parse before touching the tree, so a corrupt/unreadable file never
        // wipes out whatever scene was already open.
        QJsonArray nodes;
        if (!parseUisFile(path, nodes)) {
            QMessageBox::warning(window, "Open Failed", "Could not read file:\n" + path);
            return;
        }
        *loading = true;
        qDeleteAll(rootItem->takeChildren());
        for (const QJsonValue& node : nodes) {
            deserializeNode(rootItem, node.toObject());
        }
        *loading = false;
        (*resolveAllPtr)();
        tree->expandItem(rootItem);
        *currentPath = path;
        *dirty = false;
        updateTitle();
    };

    auto* fileMenu = window->menuBar()->addMenu("&File");
    auto addFileAction = [fileMenu](const QString& text, QKeySequence::StandardKey key, auto&& handler) {
        QAction* action = fileMenu->addAction(text);
        action->setShortcut(key);
        QObject::connect(action, &QAction::triggered, handler);
    };
    addFileAction("New", QKeySequence::New, doNew);
    addFileAction("Open...", QKeySequence::Open, doOpen);
    fileMenu->addSeparator();
    addFileAction("Save", QKeySequence::Save, doSave);
    addFileAction("Save As...", QKeySequence::SaveAs, doSaveAs);

    // --- Properties panel: shows/edits the selected component's geometry
    // and alignment. Hidden entirely (not just grayed out) whenever the
    // selection isn't a component (e.g. root, or nothing selected) -- a
    // root/branch node has no such properties at all, so there's nothing
    // here for it to show. Position/size fields accept either a plain
    // literal ("5") or a formula referencing other nodes by name
    // ("Other.pos.x + 1") -- the resolver (assigned to *resolveAllPtr below,
    // once xEdit/yEdit exist) turns whichever was typed into the resolved
    // int the canvas actually uses.
    auto* properties = new QWidget(content);
    auto* posXEdit = new QLineEdit(properties);
    auto* posYEdit = new QLineEdit(properties);
    auto* sizeWEdit = new QLineEdit(properties);
    auto* sizeHEdit = new QLineEdit(properties);
    auto* alignCombo = new QComboBox(properties);
    const QString exprHint = "A number, or a formula like Other.pos.x + 1.\n"
                              "Operators: + - * / << >> and parentheses.\n"
                              "VIEWPORT_TX / VIEWPORT_PY refer to the viewport size.";
    posXEdit->setToolTip(exprHint);
    posYEdit->setToolTip(exprHint);
    sizeWEdit->setToolTip(exprHint);
    sizeHEdit->setToolTip(exprHint);
    alignCombo->addItems({"Left", "Center", "Right"});

    auto* propertiesForm = new QFormLayout(properties);
    // The sidebar is only ~100-200px wide -- a label sharing a row with its
    // field gets squeezed down to nothing legible. Wrapping long rows puts
    // the label on its own row above the field instead, so it's always
    // fully readable.
    propertiesForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    propertiesForm->addRow("Position X", posXEdit);
    propertiesForm->addRow("Position Y", posYEdit);
    propertiesForm->addRow("Size W", sizeWEdit);
    propertiesForm->addRow("Size H", sizeHEdit);
    propertiesForm->addRow("Alignment", alignCombo);
    properties->setVisible(false);

    // Populates the panel's fields from `item` without re-triggering the
    // edit handlers below (which would otherwise write the same values
    // straight back -- harmless, but pointless).
    auto populateFrom = [posXEdit, posYEdit, sizeWEdit, sizeHEdit, alignCombo](QTreeWidgetItem* item) {
        const QSignalBlocker bx(posXEdit);
        const QSignalBlocker by(posYEdit);
        const QSignalBlocker bw(sizeWEdit);
        const QSignalBlocker bh(sizeHEdit);
        const QSignalBlocker ba(alignCombo);
        auto exprOr = [item](int exprRole, int fallback) {
            const QString s = item->data(0, exprRole).toString();
            return s.isEmpty() ? QString::number(fallback) : s;
        };
        posXEdit->setText(exprOr(kPosXExprRole, 0));
        posYEdit->setText(exprOr(kPosYExprRole, 0));
        sizeWEdit->setText(exprOr(kSizeWExprRole, 1));
        sizeHEdit->setText(exprOr(kSizeHExprRole, 1));
        alignCombo->setCurrentIndex(item->data(0, kAlignRole).toInt());
    };

    // Flags the exact field(s) the resolver couldn't work out for the
    // current selection with a red border -- independent of populateFrom
    // (and never skipped for having focus), since restyling a border
    // doesn't clobber whatever the user is mid-typing the way overwriting
    // its text would.
    auto updateErrorHighlight = [posXEdit, posYEdit, sizeWEdit, sizeHEdit](QTreeWidgetItem* item) {
        const int mask = item ? item->data(0, kErrorMaskRole).toInt() : 0;
        static const QString kErrorStyle = QStringLiteral("border: 1px solid red;");
        posXEdit->setStyleSheet((mask & 1) ? kErrorStyle : QString());
        posYEdit->setStyleSheet((mask & 2) ? kErrorStyle : QString());
        sizeWEdit->setStyleSheet((mask & 4) ? kErrorStyle : QString());
        sizeHEdit->setStyleSheet((mask & 8) ? kErrorStyle : QString());
    };

    QObject::connect(tree, &QTreeWidget::currentItemChanged,
                      [properties, populateFrom, updateErrorHighlight](QTreeWidgetItem* current, QTreeWidgetItem*) {
                          const bool selected = isComponentItem(current);
                          properties->setVisible(selected);
                          if (selected) {
                              populateFrom(current);
                              updateErrorHighlight(current);
                          }
                      });

    // The selected item's geometry can also change from outside the panel
    // -- dragging it on the canvas, the text-length auto-grow, or the
    // resolver recomputing a formula -- so keep the panel's fields from
    // going stale whenever that happens. Skipped while a field has focus so
    // an unrelated change elsewhere doesn't clobber an in-progress edit.
    QObject::connect(
        tree, &QTreeWidget::itemChanged,
        [tree, populateFrom, updateErrorHighlight, posXEdit, posYEdit, sizeWEdit,
         sizeHEdit](QTreeWidgetItem* item, int column) {
            if (column != 0 || item != tree->currentItem() || !isComponentItem(item)) {
                return;
            }
            updateErrorHighlight(item);
            if (posXEdit->hasFocus() || posYEdit->hasFocus() || sizeWEdit->hasFocus() || sizeHEdit->hasFocus()) {
                return;
            }
            populateFrom(item);
        });

    // Committed on editingFinished (Enter, or losing focus), not on every
    // keystroke -- a formula is only meaningful once fully typed.
    auto writeToSelection = [tree](int exprRole, const QString& value) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isComponentItem(item)) {
            item->setData(0, exprRole, value);
        }
    };
    QObject::connect(posXEdit, &QLineEdit::editingFinished,
                      [writeToSelection, posXEdit] { writeToSelection(kPosXExprRole, posXEdit->text()); });
    QObject::connect(posYEdit, &QLineEdit::editingFinished,
                      [writeToSelection, posYEdit] { writeToSelection(kPosYExprRole, posYEdit->text()); });
    QObject::connect(sizeWEdit, &QLineEdit::editingFinished,
                      [writeToSelection, sizeWEdit] { writeToSelection(kSizeWExprRole, sizeWEdit->text()); });
    QObject::connect(sizeHEdit, &QLineEdit::editingFinished,
                      [writeToSelection, sizeHEdit] { writeToSelection(kSizeHExprRole, sizeHEdit->text()); });
    QObject::connect(alignCombo, qOverload<int>(&QComboBox::currentIndexChanged), [tree](int v) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isComponentItem(item)) {
            item->setData(0, kAlignRole, v);
        }
    });

    // addComponentNode() takes an explicit parent so nesting components
    // under other nodes (not just root) already works -- there's just no UI
    // for it yet, since every add here always targets root, keeping newly
    // added components as root's siblings-of-each-other for now. New
    // textboxes start at the origin (0,0) with a 1x1 footprint; drag them on
    // the canvas or use the properties panel to place/resize them.
    auto componentCounter = std::make_shared<int>(1);
    auto addComponentNode = [componentCounter](QTreeWidgetItem* parent) {
        // No space in the default name -- it has to already be a valid C++
        // identifier, since it's usable immediately in another node's
        // expression.
        const QString name = QString(kTextboxPrefix) + "Textbox" + QString::number((*componentCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kComponentKind));
        child->setData(0, kTextContentRole, QString());
        child->setData(0, kPosXExprRole, QStringLiteral("0"));
        child->setData(0, kPosYExprRole, QStringLiteral("0"));
        child->setData(0, kSizeWExprRole, QStringLiteral("1"));
        child->setData(0, kSizeHExprRole, QStringLiteral("1"));
        child->setData(0, kPosXRole, 0);
        child->setData(0, kPosYRole, 0);
        child->setData(0, kSizeWRole, 1);
        child->setData(0, kSizeHRole, 1);
        child->setData(0, kAlignRole, static_cast<int>(TextAlign::Left));
        child->setData(0, kLastValidNameRole, name);
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

    // --- Expression resolution: turns every component's stored pos/size
    // expression text into the resolved int the canvas reads, by repeatedly
    // attempting whatever hasn't resolved yet until a full pass makes no new
    // progress -- the same "lazy declaration" idea as forward references in
    // a compiler, applied to nodes that can reference each other in any
    // order or before they're otherwise fully set up. Whatever's still
    // unresolved once progress stalls (a parse error, an unknown or
    // duplicate name, or a dependency cycle) is flagged via kErrorRole and
    // shown in red with an explanatory tooltip, rather than silently left
    // with a stale or wrong value.
    auto resolving = std::make_shared<bool>(false);
    *resolveAllPtr = [window, rootItem, xEdit, yEdit, resolving]() {
        if (*resolving) {
            // Re-entrant call: our own setData() calls below re-fire
            // itemChanged, which is also wired to call this function. The
            // in-flight call already accounts for everything.
            return;
        }
        *resolving = true;

        const long long viewportTx = xEdit->text().toInt();
        const long long viewportPy = yEdit->text().toInt();

        const QVector<QTreeWidgetItem*> components = collectComponentItems(rootItem);

        QHash<QString, QTreeWidgetItem*> byName;
        QSet<QString> duplicateNames;
        for (QTreeWidgetItem* item : components) {
            const QString base = item->text(0).mid(QString(kTextboxPrefix).length());
            if (byName.contains(base)) {
                duplicateNames.insert(base);
            }
            byName.insert(base, item);
        }

        struct Pending {
            QTreeWidgetItem* item;
            int prop;
            ExprPtr ast;
        };
        static constexpr int kRoles[4] = {kPosXRole, kPosYRole, kSizeWRole, kSizeHRole};
        static constexpr int kExprRoles[4] = {kPosXExprRole, kPosYExprRole, kSizeWExprRole, kSizeHExprRole};
        static constexpr long long kDefaults[4] = {0, 0, 1, 1};

        QVector<Pending> pending;
        std::map<std::pair<QTreeWidgetItem*, int>, long long> resolved;
        // Bitmask per errored item: bit `prop` set means that property
        // specifically failed to resolve -- lets the properties panel flag
        // the exact field at fault, not just the node as a whole.
        QHash<QTreeWidgetItem*, int> errorMask;

        for (QTreeWidgetItem* item : components) {
            for (int prop = 0; prop < 4; ++prop) {
                QString src = item->data(0, kExprRoles[prop]).toString().trimmed();
                if (src.isEmpty()) {
                    src = QString::number(kDefaults[prop]);
                }
                bool ok = false;
                ExprPtr ast = ExprParser(src).parse(ok);
                if (!ok) {
                    errorMask[item] |= (1 << prop);
                    continue;
                }
                pending.push_back({item, prop, ast});
            }
        }

        std::function<std::optional<long long>(const QString&, int)> resolveIdent =
            [&](const QString& name, int prop) -> std::optional<long long> {
            if (prop < 0) {
                if (name == QLatin1String("VIEWPORT_TX")) return viewportTx;
                if (name == QLatin1String("VIEWPORT_PY")) return viewportPy;
                return std::nullopt;
            }
            if (duplicateNames.contains(name)) {
                return std::nullopt;
            }
            const auto it = byName.find(name);
            if (it == byName.end()) {
                return std::nullopt;
            }
            const auto found = resolved.find({it.value(), prop});
            return found != resolved.end() ? std::optional<long long>(found->second) : std::nullopt;
        };

        bool progress = true;
        while (progress && !pending.isEmpty()) {
            progress = false;
            for (int i = pending.size() - 1; i >= 0; --i) {
                const auto v = evalExpr(pending[i].ast, resolveIdent);
                if (v) {
                    resolved[{pending[i].item, pending[i].prop}] = *v;
                    pending.removeAt(i);
                    progress = true;
                }
            }
        }
        // Anything left after progress stalls can never resolve on its own
        // -- an unknown/duplicate reference or a dependency cycle -- same
        // bucket as an outright parse error.
        for (const Pending& p : pending) {
            errorMask[p.item] |= (1 << p.prop);
        }
        for (const QString& dup : duplicateNames) {
            if (QTreeWidgetItem* item = byName.value(dup)) {
                errorMask[item] |= 0xF;  // ambiguous which property -- flag all of them
            }
        }

        QStringList errorNames;
        for (QTreeWidgetItem* item : components) {
            for (int prop = 0; prop < 4; ++prop) {
                const auto it = resolved.find({item, prop});
                long long value = (it != resolved.end()) ? it->second : kDefaults[prop];
                value = (prop >= 2) ? std::max<long long>(1, value) : std::max<long long>(0, value);
                if (item->data(0, kRoles[prop]).toLongLong() != value) {
                    item->setData(0, kRoles[prop], static_cast<int>(value));
                }
            }
            const int mask = errorMask.value(item, 0);
            const bool hasError = mask != 0;
            // Only touch item-level roles/appearance when the error state
            // actually changed -- setData() unconditionally re-fires
            // itemChanged even when the value is identical, and doing that
            // on every single resolve pass would falsely mark the scene
            // dirty just from redundant no-op writes.
            if (item->data(0, kErrorMaskRole).toInt() != mask) {
                item->setData(0, kErrorMaskRole, mask);
                item->setData(0, kErrorRole, hasError);
                QFont font = item->font(0);
                font.setBold(hasError);
                item->setFont(0, font);
                item->setForeground(0, hasError ? QBrush(Qt::red) : QBrush());
                item->setBackground(0, hasError ? QBrush(QColor(90, 20, 20)) : QBrush());
                item->setToolTip(0, hasError
                                         ? QStringLiteral("Cannot resolve one or more properties -- check for "
                                                           "typos, unknown/duplicate names, or a dependency cycle.")
                                         : QString());
            }
            if (hasError) {
                errorNames << item->text(0).mid(QString(kTextboxPrefix).length());
            }
        }

        // A red status-bar banner is the loud, hard-to-miss alert a subtle
        // tree-item color change alone wasn't -- it persists (no timeout)
        // until every error is fixed.
        if (errorNames.isEmpty()) {
            window->statusBar()->clearMessage();
            window->statusBar()->setStyleSheet(QString());
        } else {
            window->statusBar()->setStyleSheet(
                QStringLiteral("QStatusBar{background:#7a1f1f;color:white;font-weight:bold;}"));
            window->statusBar()->showMessage(
                QStringLiteral("⚠ Cannot resolve: %1").arg(errorNames.join(QStringLiteral(", "))));
        }

        *resolving = false;
    };
    (*resolveAllPtr)();

    // VIEWPORT_TX/VIEWPORT_PY change whenever these fields do, so anything
    // referencing them needs a fresh resolution pass too.
    QObject::connect(xEdit, &QLineEdit::textChanged, [resolveAllPtr](const QString&) { (*resolveAllPtr)(); });
    QObject::connect(yEdit, &QLineEdit::textChanged, [resolveAllPtr](const QString&) { (*resolveAllPtr)(); });

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

    UitkMainWindow window;
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
