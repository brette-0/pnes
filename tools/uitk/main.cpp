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
#include <QImage>
#include <QFont>
#include <QObject>
#include <QEvent>
#include <algorithm>
#include <functional>
#include <QTimer>
#include <QTreeWidget>
#include <QAbstractItemModel>
#include <QMenu>
#include <QPushButton>
#include <QGroupBox>
#include <QMouseEvent>
#include <QColor>
#include <QVector>
#include <QSplitter>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QFontDatabase>
#include <QMenuBar>
#include <QStatusBar>
#include <QAction>
#include <QKeySequence>
#include <QShortcut>
#include <QFileDialog>
#include <QInputDialog>
#include <QDir>
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
#include <QTabWidget>
#include <QStackedWidget>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <memory>
#include <cstdio>

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
// Set by the resolver when a negative-space zone's cells overlap another
// node's (or vice versa) -- a spatial-layout problem, distinct from a
// property failing to resolve, so it's tracked separately even though both
// end up shown the same way (red highlight + status-bar banner).
constexpr int kNegSpaceViolationRole = Qt::UserRole + 14;
// The one-character word boundary used when wrapping kTextContentRole across
// a textbox's rows (see wrapTextIntoRows()) -- defaults to a space, but any
// single character is allowed since it's just the character that marks
// where a line break is permitted.
constexpr int kSplitterRole = Qt::UserRole + 15;
// Which Target/Region names (see kTargetNames/kRegionNames) this node should
// disappear on -- a node is hidden if the scene's *current* target/region
// (picked in the sidebar) is in either list. Unlike the scene-wide
// target/region themselves, this is genuinely per-node: each node picks its
// own set independently, and a node's effective visibility also factors in
// every ancestor's own hide lists (see isEffectivelyHidden()) even though
// that inherited part is never itself stored on the child.
constexpr int kHideTargetsRole = Qt::UserRole + 16;
constexpr int kHideRegionsRole = Qt::UserRole + 17;
// SingleChoice-only: index (into its option children, in tree order) that
// should be selected on Make -- baked directly as SingleChoice's
// defaultOption constructor argument, same as nOptions (the child count)
// itself.
constexpr int kDefaultOptionRole = Qt::UserRole + 18;
// Per-textbox (ct or rt) override of the scene's Default Charmap -- empty
// means "use the scene's Default Charmap"; non-empty names a different
// CHARMAP mapname (see technology.hpp's CHARMAP macro) for this node's row
// text/splitter to be encoded through instead.
constexpr int kCharmapOverrideRole = Qt::UserRole + 19;
// ctTextBox-only: when checked, codegen also emits an Erase_<Name> function
// that blanks the box's whole w x h footprint (not just whatever rows its
// current text wraps to) via ppu::WriteRepeatedToNameTable, one row at a
// time -- using the same tile ' ' would encode to via charmapFn (or the raw
// ' ' character when no charmap is set) if it appeared in the box's own
// text, so a Draw_ then Erase_ round-trips exactly. Meaningless for
// rtTextBox, whose text (and therefore its erase tile) isn't known until
// runtime.
constexpr int kProvideErasingRole = Qt::UserRole + 20;
// Compile-time and runtime textboxes are separate kinds -- currently
// identical in behavior, but distinguished now because they'll diverge in
// meaning later (e.g. how their text is ultimately resolved by generated
// code).
constexpr char kCtTextBoxKind[] = "ctTextBox";
constexpr char kRtTextBoxKind[] = "rtTextBox";
constexpr char kNegSpaceKind[] = "negspace";
constexpr char kSingleChoiceKind[] = "singlechoice";
constexpr char kCtTextBoxPrefix[] = "[CT] ";
constexpr char kRtTextBoxPrefix[] = "[RT] ";
constexpr char kNegSpacePrefix[] = "[N] ";
constexpr char kSingleChoicePrefix[] = "[SC] ";

// Every backend platform-nes currently supports, in the order they appear in
// the Target dropdown. Scene-wide, same as the nametable -- one scene targets
// one platform at a time. Kept as display names (matched by text, not index)
// so the .uis format doesn't depend on this list's order.
const QStringList kTargetNames = {"NES", "GBA", "GCN", "DS",     "DSI", "Wii",
                                   "Wii U", "3DS", "Switch", "PSP",  "PC"};

// TV broadcast standard the scene is authored/timed against. Same idea as
// kTargetNames -- scene-wide, matched by name rather than index.
const QStringList kRegionNames = {"NTSC", "PAL"};

// Fixed viewport tile counts for each target, mirroring the
// video::viewport_tx()/viewport_ty() constants each backend actually compiles
// against (include/platform-nes/video.hpp) -- picking a Target sets the
// sidebar's Viewport X/Y to whatever that backend's panel really shows, so a
// scene isn't authored against the wrong resolution by accident. Two targets
// have no fixed panel size and are deliberately left out: GC/Wii
// (video.hpp's TARGET_OGC branch) sizes to whatever TV VIDEO_GetPreferredMode
// reports at runtime -- src/ogc/video.cpp's ogc_world_tx computation, fed a
// standard 640x480 NTSC mode, resolves to 40, the closest thing to a
// canonical default, so GCN/Wii use that; PC (the SDL LANDSCAPE/PORTRAIT
// branches) sizes to whatever the host desktop's own display mode is, which
// has no "the" resolution to pick, so selecting it leaves Viewport X/Y alone.
std::optional<std::pair<int, int>> viewportTilesForTarget(const QString& target) {
    static const QHash<QString, std::pair<int, int>> kSizes{
        {"NES", {32, 30}},    // hardware PPU, fixed 256x240
        {"GBA", {30, 20}},    // 240x160 panel
        {"GCN", {40, 30}},    // runtime TV width; 40 = a standard 640x480 NTSC mode
        {"DS", {32, 24}},     // 256x192 panel
        {"DSI", {32, 24}},    // same main-engine panel as DS
        {"Wii", {40, 30}},    // same runtime-TV path as GCN
        {"Wii U", {52, 30}},  // widescreen, scaled to fill the panel
        {"3DS", {50, 30}},    // 400x240 top screen
        {"Switch", {52, 30}}, // widescreen, scaled to fill the panel
        {"PSP", {60, 30}},    // 480x272 panel, letterboxed to 480x240
    };
    const auto it = kSizes.constFind(target);
    if (it == kSizes.constEnd()) return std::nullopt;
    return *it;
}

enum class TextAlign { Left = 0, Center = 1, Right = 2 };

// Splits `text` into rows of at most `w` characters, breaking only at
// `splitter` boundaries where possible -- a "word" is a maximal run of
// characters between splitters. Words are greedily packed onto the current
// row (rejoined with a single splitter between them) until the next word
// wouldn't fit; that word starts the next row instead. A word that's wider
// than `w` all on its own is never broken up -- it's placed at the start of
// its row regardless and left to overflow past the row's width rather than
// being force-wrapped mid-word.
QStringList wrapTextIntoRows(const QString& text, QChar splitter, int w) {
    const QStringList words = text.split(splitter);
    QStringList rows;
    QString current;
    for (const QString& word : words) {
        if (current.isEmpty()) {
            current = word;
        } else if (current.length() + 1 + word.length() <= w) {
            current += splitter;
            current += word;
        } else {
            rows << current;
            current = word;
        }
    }
    rows << current;
    return rows;
}

bool isCtTextBoxItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kCtTextBoxKind);
}

bool isRtTextBoxItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kRtTextBoxKind);
}

// Either flavor of textbox -- compile-time and runtime textboxes behave
// identically everywhere except kind string, name prefix, and which add-node
// action creates them, so most code just wants "is this a textbox at all."
bool isComponentItem(const QTreeWidgetItem* item) {
    return isCtTextBoxItem(item) || isRtTextBoxItem(item);
}

bool isNegSpaceItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kNegSpaceKind);
}

// A pure grouping node -- no geometry of its own, just a parent for the
// ctTextBox/rtTextBox nodes representing its options. Not a geometry item:
// it's never drawn, dragged, or resolved, same as root/plain branch nodes.
bool isSingleChoiceItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kSingleChoiceKind);
}

// Anything with a position/size -- placeable on the canvas, draggable,
// referenceable in another node's expressions -- regardless of what kind of
// thing it visually is.
bool isGeometryItem(const QTreeWidgetItem* item) { return isComponentItem(item) || isNegSpaceItem(item); }

// Anything that carries a kind-specific name prefix -- every geometry item,
// plus SingleChoice, which has no geometry of its own but is still a genuine
// node (as opposed to root/plain branch nodes, which have no prefix at all).
bool isPrefixedItem(const QTreeWidgetItem* item) { return isGeometryItem(item) || isSingleChoiceItem(item); }

// The "[CT] "/"[RT] "/"[N] "/"[SC] " prefix a node's kind requires, or empty
// for kinds (root, plain branch nodes) that don't have one.
QString requiredPrefixFor(const QTreeWidgetItem* item) {
    if (isCtTextBoxItem(item)) return QString(kCtTextBoxPrefix);
    if (isRtTextBoxItem(item)) return QString(kRtTextBoxPrefix);
    if (isNegSpaceItem(item)) return QString(kNegSpacePrefix);
    if (isSingleChoiceItem(item)) return QString(kSingleChoicePrefix);
    return QString();
}

// Walks the whole tree (not just direct children) collecting every geometry
// node (textbox or negative-space zone) -- shared by the canvas
// (rendering/hit-testing), name validation, and the expression resolver.
QVector<QTreeWidgetItem*> collectComponentItems(QTreeWidgetItem* node) {
    QVector<QTreeWidgetItem*> result;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* n) {
        for (int i = 0; i < n->childCount(); ++i) {
            QTreeWidgetItem* child = n->child(i);
            if (isGeometryItem(child)) {
                result.push_back(child);
            }
            visit(child);
        }
    };
    visit(node);
    return result;
}

// Same walk as collectComponentItems, but over every prefixed node (geometry
// items plus SingleChoice) -- used for name-uniqueness checks, since a
// SingleChoice's name has to be unique against everything else that carries
// a prefix, not just against other geometry.
QVector<QTreeWidgetItem*> collectPrefixedItems(QTreeWidgetItem* node) {
    QVector<QTreeWidgetItem*> result;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* n) {
        for (int i = 0; i < n->childCount(); ++i) {
            QTreeWidgetItem* child = n->child(i);
            if (isPrefixedItem(child)) {
                result.push_back(child);
            }
            visit(child);
        }
    };
    visit(node);
    return result;
}

// Every SingleChoice node in the tree, in document order -- codegen's own
// walk over the grouping nodes ctTextBox/rtTextBox codegen never visits
// directly (collectComponentItems skips them, since they carry no geometry
// of their own).
QVector<QTreeWidgetItem*> collectSingleChoiceItems(QTreeWidgetItem* node) {
    QVector<QTreeWidgetItem*> result;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* n) {
        for (int i = 0; i < n->childCount(); ++i) {
            QTreeWidgetItem* child = n->child(i);
            if (isSingleChoiceItem(child)) {
                result.push_back(child);
            }
            visit(child);
        }
    };
    visit(node);
    return result;
}

// A node is hidden for the current target/region if *it* was set to hide on
// either one, or if any ancestor was -- children never store this
// themselves (their own kHideTargetsRole/kHideRegionsRole entries, if any,
// are independent of a parent's), but they still visually disappear along
// with a hidden parent, so the check walks all the way up to root.
bool isEffectivelyHidden(const QTreeWidgetItem* item, const QString& target, const QString& region) {
    for (const QTreeWidgetItem* n = item; n && isPrefixedItem(n); n = n->parent()) {
        if (n->data(0, kHideTargetsRole).toStringList().contains(target)) return true;
        if (n->data(0, kHideRegionsRole).toStringList().contains(region)) return true;
    }
    return false;
}

// A SingleChoice's options, in tree order -- its direct ctTextBox/rtTextBox
// children (not grandchildren: an option is a leaf, it doesn't itself group
// further options). Hidden-for-target/region children are dropped, same as
// everywhere else in codegen, so a scene never bakes/counts an option that
// isn't actually present on the exported target.
QVector<QTreeWidgetItem*> singleChoiceMembers(const QTreeWidgetItem* scItem, const QString& target,
                                                const QString& region) {
    QVector<QTreeWidgetItem*> result;
    for (int i = 0; i < scItem->childCount(); ++i) {
        QTreeWidgetItem* child = scItem->child(i);
        if (isComponentItem(child) && !isEffectivelyHidden(child, target, region)) {
            result.push_back(child);
        }
    }
    return result;
}

// --- Property expressions -----------------------------------------------
//
// A property's stored text is a small arithmetic expression over integer
// literals, +/-/*//, parentheses, and identifiers of the form
// `NodeName.pos.x`, `NodeName.pos.y`, `NodeName.size.x`, `NodeName.size.y`,
// `NodeName.textSize` (referencing another component by its bare name, sans
// the "[CT] "/"[RT] " prefix -- the same name that will identify it in
// generated C++), plus the two bare globals VIEWPORT_TX/VIEWPORT_TY (the
// viewport's configured size in tiles) and VIEWPORT_PX/VIEWPORT_PY (the same,
// in pixels, at the current display's tile scale). `this` is accepted in
// place of a name anywhere one of the above is legal (e.g. `this.textSize`,
// `this.pos.x`) as a self-reference to whichever node the expression being
// evaluated belongs to -- handled by substitution in resolveAllPtr's
// resolveIdent, not here, since the parser has no notion of which node's
// property it's parsing for. `.textSize` is the referenced textbox's text
// length in characters (its *unwrapped* single-line width) -- e.g.
// `sizeWExpr: "this.textSize"` auto-fits a box to its own label, or
// `posXExpr: "(VIEWPORT_TX - this.textSize) / 2"` centers one. It resolves
// only against ctTextBox/rtTextBox nodes, same as pos/size, since only they
// carry text. Node names are therefore constrained to be valid C++
// identifiers and unique, since they're both the expression namespace here
// and the symbol that later code generation will emit.
struct ExprNode {
    enum class Kind { Number, Ident, Add, Sub, Mul, Div, Shl, Shr, Neg };
    Kind kind = Kind::Number;
    long long number = 0;
    QString identName;  // component base name, "this", or "VIEWPORT_{T,P}{X,Y}"
    // 0=pos.x, 1=pos.y, 2=size.x, 3=size.y, 4=textSize; -1 for the bare globals
    int identProp = -1;
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
            // Full grammar, not just the additive level -- otherwise a shift
            // inside parentheses (e.g. "(VIEWPORT_TX >> 2) - 1") can never
            // parse: parseExpr() alone has no notion of << / >>, so it stops
            // right before the shift operator and the missing ')' check
            // below fails.
            ExprPtr inner = parseShift(ok);
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
                // VIEWPORT_T{X,Y} are the viewport's size in tiles;
                // VIEWPORT_P{X,Y} are the same in pixels.
                static const QSet<QString> kViewportIdents{"VIEWPORT_TX", "VIEWPORT_TY", "VIEWPORT_PX",
                                                             "VIEWPORT_PY"};
                if (!kViewportIdents.contains(token)) {
                    ok = false;
                    return nullptr;
                }
                node->identName = token;
                node->identProp = -1;
                return node;
            }
            if (parts.size() == 3) {
                static const QHash<QString, int> kPropMap{
                    {"pos.x", 0}, {"pos.y", 1}, {"size.x", 2}, {"size.y", 3}};
                const QString propKey = parts.at(1) + "." + parts.at(2);
                if (!kPropMap.contains(propKey)) {
                    ok = false;
                    return nullptr;
                }
                node->identName = parts.at(0);
                node->identProp = kPropMap.value(propKey);
                return node;
            }
            if (parts.size() == 2 && parts.at(1) == QLatin1String("textSize")) {
                node->identName = parts.at(0);
                node->identProp = 4;
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

// Re-emits `node` as a C++ expression rather than collapsing it to one
// number -- used for a variadic-viewport target's SingleChoice member
// positions, where VIEWPORT_T{X,Y}/VIEWPORT_P{X,Y} aren't a fixed number the
// way they are for a fixed-panel target, but a value only known once the
// generated Make_ function actually runs (see video::viewport_tx() and
// friends). Substituted 1:1 with the matching accessor so the emitted
// arithmetic is exactly the formula the scene author typed. Every other
// identifier -- a cross-reference to another node's own position/size/
// textSize, or `this` -- is NOT runtime-variable (only the viewport itself
// is), so those are baked to the already-resolved int `resolveIdent` hands
// back, same as a fixed-panel target bakes everything.
QString emitExprCpp(const ExprPtr& node, const std::function<long long(const QString&, int)>& resolveIdent) {
    if (!node) return QStringLiteral("0");
    switch (node->kind) {
        case ExprNode::Kind::Number:
            return QString::number(node->number);
        case ExprNode::Kind::Ident:
            if (node->identProp < 0) {
                if (node->identName == QLatin1String("VIEWPORT_TX")) return QStringLiteral("video::viewport_tx()");
                if (node->identName == QLatin1String("VIEWPORT_TY")) return QStringLiteral("video::viewport_ty()");
                if (node->identName == QLatin1String("VIEWPORT_PX")) return QStringLiteral("video::viewport_px()");
                if (node->identName == QLatin1String("VIEWPORT_PY")) return QStringLiteral("video::viewport_py()");
            }
            return QString::number(resolveIdent(node->identName, node->identProp));
        case ExprNode::Kind::Neg:
            return QString("-(%1)").arg(emitExprCpp(node->a, resolveIdent));
        case ExprNode::Kind::Add:
            return QString("(%1 + %2)").arg(emitExprCpp(node->a, resolveIdent), emitExprCpp(node->b, resolveIdent));
        case ExprNode::Kind::Sub:
            return QString("(%1 - %2)").arg(emitExprCpp(node->a, resolveIdent), emitExprCpp(node->b, resolveIdent));
        case ExprNode::Kind::Mul:
            return QString("(%1 * %2)").arg(emitExprCpp(node->a, resolveIdent), emitExprCpp(node->b, resolveIdent));
        case ExprNode::Kind::Div:
            return QString("(%1 / %2)").arg(emitExprCpp(node->a, resolveIdent), emitExprCpp(node->b, resolveIdent));
        case ExprNode::Kind::Shl:
            return QString("(%1 << %2)").arg(emitExprCpp(node->a, resolveIdent), emitExprCpp(node->b, resolveIdent));
        case ExprNode::Kind::Shr:
            return QString("(%1 >> %2)").arg(emitExprCpp(node->a, resolveIdent), emitExprCpp(node->b, resolveIdent));
    }
    return QStringLiteral("0");
}

// .uis ("User Interface Scene") file format: a JSON object holding a flat
// "version" and a "nodes" array -- root's own children, each recursively
// carrying its own "children". Component and negative-space nodes carry
// geometry (the latter has no text/alignment, having no text); anything
// else (currently just "root" and, structurally, future branch nodes)
// round-trips as a plain named node.
QJsonObject serializeNode(const QTreeWidgetItem* item) {
    QJsonObject obj;
    obj["name"] = item->text(0);
    if (isGeometryItem(item)) {
        obj["kind"] = item->data(0, kKindRole).toString();
        obj["posXExpr"] = item->data(0, kPosXExprRole).toString();
        obj["posYExpr"] = item->data(0, kPosYExprRole).toString();
        obj["sizeWExpr"] = item->data(0, kSizeWExprRole).toString();
        obj["sizeHExpr"] = item->data(0, kSizeHExprRole).toString();
        if (isComponentItem(item)) {
            obj["align"] = item->data(0, kAlignRole).toInt();
            obj["text"] = item->data(0, kTextContentRole).toString();
            obj["splitter"] = item->data(0, kSplitterRole).toString();
            obj["charmapOverride"] = item->data(0, kCharmapOverrideRole).toString();
            if (isCtTextBoxItem(item)) {
                obj["provideErasing"] = item->data(0, kProvideErasingRole).toBool();
            }
        }
    } else if (isSingleChoiceItem(item)) {
        obj["kind"] = QStringLiteral("singlechoice");
        obj["defaultOption"] = item->data(0, kDefaultOptionRole).toInt();
    } else {
        obj["kind"] = QStringLiteral("branch");
    }
    if (isPrefixedItem(item)) {
        obj["hideTargets"] = QJsonArray::fromStringList(item->data(0, kHideTargetsRole).toStringList());
        obj["hideRegions"] = QJsonArray::fromStringList(item->data(0, kHideRegionsRole).toStringList());
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
    const QString kind = obj["kind"].toString();
    const bool isCtTextBox = (kind == QLatin1String("ctTextBox"));
    const bool isRtTextBox = (kind == QLatin1String("rtTextBox"));
    const bool isComponent = isCtTextBox || isRtTextBox;
    const bool isNegSpace = (kind == QLatin1String("negspace"));
    if (isComponent || isNegSpace) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kKindRole,
                       QString(isCtTextBox ? kCtTextBoxKind : isRtTextBox ? kRtTextBoxKind : kNegSpaceKind));
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
        if (isComponent) {
            item->setData(0, kAlignRole, obj["align"].toInt());
            item->setData(0, kTextContentRole, obj["text"].toString());
            item->setData(0, kSplitterRole, obj["splitter"].toString(QStringLiteral(" ")));
            item->setData(0, kCharmapOverrideRole, obj["charmapOverride"].toString());
            if (isCtTextBox) {
                item->setData(0, kProvideErasingRole, obj["provideErasing"].toBool(false));
            }
        }
        item->setData(0, kLastValidNameRole, item->text(0));
    } else if (kind == QLatin1String("singlechoice")) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kKindRole, QString(kSingleChoiceKind));
        item->setData(0, kDefaultOptionRole, obj["defaultOption"].toInt(0));
        item->setData(0, kLastValidNameRole, item->text(0));
    }
    if (isPrefixedItem(item)) {
        item->setData(0, kHideTargetsRole, obj["hideTargets"].toArray().toVariantList());
        item->setData(0, kHideRegionsRole, obj["hideRegions"].toArray().toVariantList());
    }
    for (const QJsonValue& child : obj["children"].toArray()) {
        deserializeNode(item, child.toObject());
    }
}

// `nametable`, `target`, `region`, and `linker_prefix` are all scene-wide
// (which physical NES nametable the whole UI's tile positions resolve into,
// which platform-nes backend the scene targets, which TV broadcast standard
// it's timed against, and what codegen should emit ahead of every generated,
// non-AI-marked function definition), not per-node properties, so each is
// stored once at the document's top level alongside "version" rather than on
// every node.
bool writeUisFile(const QString& path, const QTreeWidgetItem* rootItem, int nametable, const QString& target,
                   const QString& region, const QString& linkerPrefix, const QString& bssPrefix,
                   const QString& dataPrefix, const QString& charmap) {
    QJsonArray nodes;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        nodes.append(serializeNode(rootItem->child(i)));
    }
    QJsonObject doc;
    doc["version"] = 1;
    doc["nametable"] = nametable;
    doc["target"] = target;
    doc["region"] = region;
    doc["linker_prefix"] = linkerPrefix;
    doc["bss_prefix"] = bssPrefix;
    doc["data_prefix"] = dataPrefix;
    doc["charmap"] = charmap;
    doc["nodes"] = nodes;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(doc).toJson());
    return true;
}

// Parses a .uis file's node list and scene-wide nametable/target/region/
// linker_prefix without touching any tree -- callers apply it (or don't, on
// failure) themselves, so a corrupt/unreadable file never wipes out whatever
// scene was already open. `target`/`region` are matched by name rather than
// trusted as-is, since the caller (whose combo defines the valid set) is the
// one who knows what a missing or unrecognized value should fall back to.
// `linker_prefix`/`bss_prefix`/`data_prefix`/`charmap` have no such
// validation -- they're free-form text -- and simply default to empty when
// absent (e.g. an older file).
bool parseUisFile(const QString& path, QJsonArray& outNodes, int& outNametable, QString& outTarget,
                   QString& outRegion, QString& outLinkerPrefix, QString& outBssPrefix, QString& outDataPrefix,
                   QString& outCharmap) {
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
    outNametable = std::clamp(doc.object()["nametable"].toInt(0), 0, 3);
    outTarget = doc.object()["target"].toString();
    outRegion = doc.object()["region"].toString();
    outLinkerPrefix = doc.object()["linker_prefix"].toString();
    outBssPrefix = doc.object()["bss_prefix"].toString();
    outDataPrefix = doc.object()["data_prefix"].toString();
    outCharmap = doc.object()["charmap"].toString();
    return true;
}

// --- Code export -----------------------------------------------------------
//
// Turns a resolved scene into a target-specific .hpp/.cpp pair. Two kinds of
// generated function, one per component kind, never mixed:
//
//   - ctTextBox: the text is known at design time, so wrapping/alignment is
//     baked entirely here (same math TileGridWidget::paintEvent uses to
//     preview it) into a self-contained placement function -- one
//     ppu::WriteFromBufferToNameTable call per row, each row's characters
//     kept as a string literal local to that function (never a shared/global
//     buffer -- nothing outside the function needs it). Tagged with the
//     scene's linker_prefix (a placement attribute, e.g. a bank/section) since
//     it's real emitted code+data that has to land wherever the linker
//     expects it, exactly like src/all/extras/ui/text.cpp's own UI_BANK.
//
//   - rtTextBox: the text is only known at runtime, so there's nothing to
//     bake but the box/splitter -- the generated function is a thin AI
//     (always-inline) wrapper around ui::text::Make, defined inline in the
//     header for the same reason ui::text::Draw's overloads are (an
//     always_inline free function needs vague/COMDAT linkage to be safe
//     across translation units, see text.hpp). No linker_prefix: an
//     always-inline wrapper has no meaningful placement of its own.
//
// Negative-space nodes emit nothing themselves -- an editor-only exclusion
// zone with no runtime counterpart at all. We never bundle a component's
// make/draw together, or decide call order for the caller -- codegen only
// emits the wrappers; when and in what order a scene's components actually
// get placed/drawn is up to the game code that calls them. We also never
// emit a draw callback for a SingleChoice: per its own header comment,
// drawing the chosen option is "entirely the caller's job, via plain
// ui::text::Make/Draw" -- codegen's job for one is only to emit the
// ui::choice::SingleChoice instance itself, its options' positions, and the
// Make_ function that constructs it (see genSingleChoice below).
//
// Every generated declaration lives inside `namespace gen::<sceneName>`, so
// two scenes can freely reuse the same component names without colliding.
//
// A SingleChoice's member positions are baked very differently depending on
// whether `target` has a fixed panel size known at compile time (NES/GBA/
// PSP) or a variadic one only known once the game is actually running
// (GCN onward, except PSP -- see kVariadicTargets): on a fixed target,
// `<name>_options` is a `const` array of already-resolved positions, exactly
// like a ctTextBox's own placement is baked; on a variadic target it's
// mutable storage populated by Make_, each element re-deriving its member's
// position expression at runtime via video::viewport_*() (see emitExprCpp)
// since VIEWPORT_TX/TY/PX/PY can't be collapsed to one number the way they
// can for a fixed panel.

// Fixed nametable quadrant size, mirroring src/nes/video.cpp's xy_to_nt_addr
// (32 tiles wide, 30 tall per quadrant) -- the addressing scheme every
// backend's CartesianToAddress/WriteFromBufferToNameTable shares, independent
// of that target's own visible viewport tile count.
constexpr int kNametableQuadW = 32;
constexpr int kNametableQuadH = 30;

// Targets whose viewport size isn't fixed at compile time -- "gamecube
// onwards", per platform-nes's own generational split, except PSP (which
// keeps a fixed 480x272/letterboxed panel like NES/GBA). Everything NOT in
// this set bakes a SingleChoice's member positions to plain constants, the
// same way ctTextBox already does; everything in it re-derives them at
// runtime in Make_ (see the generateCode doc comment above).
const QSet<QString> kVariadicTargets{"GCN", "DS", "DSI", "Wii", "Wii U", "3DS", "Switch", "PC"};

QString bareName(const QTreeWidgetItem* item) {
    return item->text(0).mid(requiredPrefixFor(item).length());
}

// Escapes `s` for use inside a double-quoted C string literal.
QString cStringEscape(const QString& s) {
    QString out;
    out.reserve(s.size());
    for (const QChar ch : s) {
        if (ch == QLatin1Char('\\') || ch == QLatin1Char('"')) out += QLatin1Char('\\');
        out += ch;
    }
    return out;
}

// Escapes `ch` for use inside a single-quoted C char literal.
QString cCharEscape(QChar ch) {
    if (ch == QLatin1Char('\\') || ch == QLatin1Char('\'')) return QString("\\") + ch;
    return QString(ch);
}

struct GeneratedFiles {
    QString hpp;
    QString cpp;
    // Whether `cpp` actually carries any generated definitions, as opposed
    // to just the do-not-edit banner/include/namespace-open-close wrapper
    // every export still formats it with regardless. Every AI-tagged
    // wrapper this tool emits (ctTextBox included, as of Erase_/Draw_
    // becoming AI) defines its body in the .hpp instead, so `cpp` is
    // routinely all-wrapper-no-content -- callers use this instead of
    // string-sniffing `cpp` to decide whether that empty file is even worth
    // writing (see exportOneTarget).
    bool cppHasContent = false;
};

// Mirrors src/nes/video.cpp's xy_to_nt_addr exactly -- the PPU-address math
// behind ::ppu::CartesianToAddress -- so NES export can bake the same VRAM
// address that function would compute, entirely at design time. Only valid
// for the fixed, single-page-wrap nametable addressing NES itself uses;
// never called for any other target (see genCtTextBoxBody).
quint16 nesNtAddr(int x, int y) {
    constexpr quint16 base = 0x2000;
    const quint16 ux = static_cast<quint16>(x);
    const quint16 uy = static_cast<quint16>(y);
    const quint16 nt_h = static_cast<quint16>((ux >> 5 & 1) << 10);
    const quint16 col = ux & 0x1F;
    if (uy < 30) {
        return static_cast<quint16>(base + nt_h + (uy << 5) + col);
    }
    const quint16 nt_v = static_cast<quint16>((uy / 30) << 11);
    const quint16 row = uy % 30;
    return static_cast<quint16>(base + nt_h + nt_v + row * 32 + col);
}

// A ctTextBox's generated pieces: `rootDecls` are namespace-scope
// declarations (the charmap-encoded row data, when a charmap is in use --
// see genCtTextBoxBody) that belong at the root of `gen::<sceneName>`, not
// nested inside the Draw_ function itself; `body` is that function's
// statements.
struct CtTextBoxGen {
    QString rootDecls;
    QString body;
};

// Emits one ctTextBox's data + placement function body: a
// ppu::WriteFromBufferToNameTable call per wrapped row -- same wrap/align
// math as TileGridWidget::paintEvent, so what's exported always matches what
// the canvas previewed.
//
// With a scene charmap set (`charmapFn` non-empty), each row's text is
// mapped through it at compile time -- the same ::tech::nes_str::encode
// technology demo/src/graphics/strings.hpp already hand-writes (e.g.
// msg_title) -- and hoisted to a `<dataPrefixTok>inline constexpr auto
// <Name>_rowN = ...` declaration at the namespace root, so it's a single
// rodata object the linker folds regardless of how many TUs include it, not
// a local buffer reallocated on every call. The Draw_ function then just
// points WriteFromBufferToNameTable at it via ::SIZED_OBJ, which also
// already supplies the exact (unterminated) row length `encode<>` produces
// -- no `sizeof(...) - 1` needed the way a plain C string literal wants.
// With no charmap set, this falls back to the previous plain
// `static const char[]` behavior, for a scene that hasn't set one yet.
//
// On NES the panel is fixed, so unlike every other target it never needs the
// vec2<u16> overload's per-tile page-aware address recomputation (see
// src/emu/ppu.cpp's own WriteFromBufferToNameTable) -- the VRAM address for
// each row is exactly as constant as its tile position, so it's baked here
// via nesNtAddr and passed through the address overload instead, the same
// "pay the CartesianToAddress cost once, off the hot path" a caller doing
// this by hand would (see ::ppu::WriteFromBufferToNameTable's own address-
// overload doc comment). Every other target keeps the vec2<u16> form, which
// re-derives the page-aware address per tile -- required once a viewport can
// be wider/taller than one nametable page, which NES's fixed 32x30 panel
// never is.
CtTextBoxGen genCtTextBoxBody(const QTreeWidgetItem* item, const QString& name, int ntOffX, int ntOffY, bool isNes,
                               const QString& dataPrefixTok, const QString& charmapFn) {
    const int x = item->data(0, kPosXRole).toInt() + ntOffX;
    const int y = item->data(0, kPosYRole).toInt() + ntOffY;
    const int w = std::max(1, item->data(0, kSizeWRole).toInt());
    const int h = std::max(1, item->data(0, kSizeHRole).toInt());
    const auto align = static_cast<TextAlign>(item->data(0, kAlignRole).toInt());
    const QString splitterStr = item->data(0, kSplitterRole).toString();
    const QChar splitter = splitterStr.isEmpty() ? QLatin1Char(' ') : splitterStr.at(0);
    const QString text = item->data(0, kTextContentRole).toString();

    QStringList rootLines;
    QStringList bodyLines;
    if (!text.isEmpty()) {
        const QStringList rows = wrapTextIntoRows(text, splitter, w);
        for (int row = 0; row < h && row < rows.size(); ++row) {
            const QString& rowText = rows.at(row);
            if (rowText.isEmpty()) break;
            const int slack = w - rowText.length();
            const int startCol = (align == TextAlign::Left)    ? 0
                                  : (align == TextAlign::Right) ? slack
                                                                 : slack / 2;

            QString sourceArgs;  // the (source, count) args to WriteFromBufferToNameTable
            if (charmapFn.isEmpty()) {
                const QString rowVar = QString("row%1").arg(row);
                bodyLines << QString("    static const char %1[] = \"%2\";")
                                 .arg(rowVar, cStringEscape(rowText));
                sourceArgs = QString("reinterpret_cast<const u8*>(%1), sizeof(%1) - 1").arg(rowVar);
            } else {
                const QString dataName = QString("%1_row%2").arg(name).arg(row);
                rootLines << QString("%1inline constexpr auto %2 = ::tech::nes_str::encode<%3>(\"%4\");")
                                 .arg(dataPrefixTok, dataName, charmapFn, cStringEscape(rowText));
                sourceArgs = QString("SIZED_OBJ(%1)").arg(dataName);
            }

            if (isNes) {
                bodyLines << QString("    ppu::WriteFromBufferToNameTable(0x%1, %2, 0);")
                                 .arg(static_cast<uint>(nesNtAddr(x + startCol, y + row)), 4, 16, QLatin1Char('0'))
                                 .arg(sourceArgs);
            } else {
                bodyLines << QString("    ppu::WriteFromBufferToNameTable(vec2<u16>{%1, %2}, %3, 0);")
                                 .arg(x + startCol)
                                 .arg(y + row)
                                 .arg(sourceArgs);
            }
        }
    }
    return {rootLines.join("\n"), bodyLines.join("\n")};
}

// Emits an Erase_<Name> function body for a ctTextBox that opted into
// Provide Erasing: one ppu::WriteRepeatedToNameTable call per row of the
// box's whole w x h footprint, each filling that row with the tile ' '
// would encode to through `charmapFn` -- the same character genCtTextBoxBody
// would emit if the box's text were a run of spaces -- or the raw ' ' byte
// itself when no charmap is set (charmapFn empty), matching that function's
// own plain-string fallback. Unlike genCtTextBoxBody's rows, this always
// covers the full box, not just whatever rows the current text wraps to, so
// switching to shorter text later doesn't leave stale tiles behind.
QString genCtTextBoxEraseBody(const QTreeWidgetItem* item, int ntOffX, int ntOffY, bool isNes,
                               const QString& charmapFn) {
    const int x = item->data(0, kPosXRole).toInt() + ntOffX;
    const int y = item->data(0, kPosYRole).toInt() + ntOffY;
    const int w = std::max(1, item->data(0, kSizeWRole).toInt());
    const int h = std::max(1, item->data(0, kSizeHRole).toInt());
    const QString tileExpr =
        charmapFn.isEmpty() ? QStringLiteral("' '") : QString("%1(' ')").arg(charmapFn);

    QStringList bodyLines;
    for (int row = 0; row < h; ++row) {
        if (isNes) {
            bodyLines << QString("    ppu::WriteRepeatedToNameTable(0x%1, %2, %3, 0);")
                             .arg(static_cast<uint>(nesNtAddr(x, y + row)), 4, 16, QLatin1Char('0'))
                             .arg(tileExpr)
                             .arg(w);
        } else {
            bodyLines << QString("    ppu::WriteRepeatedToNameTable(vec2<u16>{%1, %2}, %3, %4, 0);")
                             .arg(x)
                             .arg(y + row)
                             .arg(tileExpr)
                             .arg(w);
        }
    }
    return bodyLines.join("\n");
}

// Emits one SingleChoice node's declarations: its options' positions
// (`<name>_options`), its instance storage + reference (`<name>`), and the
// Make_ function that constructs it -- see the generateCode doc comment
// above for why the shape differs between a fixed and a variadic target.
// Everything lives header-only (`inline`), same reasoning as rtTextBox's own
// wrapper: it has to be safely includable from more than one TU. Returns
// empty for a SingleChoice with no (visible) options -- nothing to
// construct.
//
// `bssPrefixTok` is already empty on every target but NES (see
// generateCode) -- it places the instance's own mutable storage, and, on a
// variadic target, its mutable options array.
QString genSingleChoiceDecl(QTreeWidgetItem* scItem, QTreeWidgetItem* rootItem, const QString& target,
                             const QString& region, int ntOffX, int ntOffY, const QString& bssPrefixTok) {
    const QVector<QTreeWidgetItem*> members = singleChoiceMembers(scItem, target, region);
    if (members.isEmpty()) {
        return QString();
    }
    const QString name = bareName(scItem);
    const int nOptions = members.size();
    const int defaultOption = std::clamp(scItem->data(0, kDefaultOptionRole).toInt(), 0, nOptions - 1);
    const bool variadic = kVariadicTargets.contains(target);

    // Cross-reference resolver for a member's position expression (e.g.
    // `Other.pos.x`, `this.textSize`) -- these are never runtime-variable,
    // only VIEWPORT_* is (see emitExprCpp), so they're baked to the
    // already-resolved value every other geometry node's own codegen uses.
    auto resolveBaked = [rootItem](const QTreeWidgetItem* self, const QString& rawName, int prop) -> long long {
        const QString wanted = (rawName == QLatin1String("this")) ? bareName(self) : rawName;
        for (QTreeWidgetItem* other : collectComponentItems(rootItem)) {
            if (bareName(other) != wanted) continue;
            switch (prop) {
                case 0: return other->data(0, kPosXRole).toInt();
                case 1: return other->data(0, kPosYRole).toInt();
                case 2: return other->data(0, kSizeWRole).toInt();
                case 3: return other->data(0, kSizeHRole).toInt();
                case 4: return other->data(0, kTextContentRole).toString().length();
                default: return 0;
            }
        }
        return 0;
    };

    QStringList lines;
    lines << QString("// SingleChoice: %1").arg(name);

    if (variadic) {
        // Mutable, populated by Make_ below -- the viewport size (and
        // therefore every member's resolved position) isn't known until the
        // game is actually running. On a fixed panel there's nothing to
        // store here at all: each member's position is already baked
        // straight into its own Draw_<Name> (see genCtTextBoxBody), so a
        // second copy of the same PPU address in an `_options` array would
        // just be redundant data.
        //
        // `atomic` (technology.hpp: volatile on NES, true atomic elsewhere)
        // because this is non-local mutable memory an aggressively-LTO'd,
        // multithreaded target could otherwise cache/reorder/tear across
        // Make_'s writes and a reader elsewhere -- unlike SingleChoice's own
        // `option` field (already atomic inside ui::choice::SingleChoice
        // itself), nothing else protects this array.
        lines << QString("%1inline atomic vec2<u16> %2_options[%3];").arg(bssPrefixTok, name).arg(nOptions);
    }

    // ui::choice::SingleChoice has no default constructor (and its real
    // constructor isn't constexpr), so it can't be declared directly at
    // namespace scope without a dynamic pre-main initializer -- exactly what
    // an explicit Make_ step is meant to avoid. Instead: raw, correctly-
    // aligned storage (no constructor runs, so it's legitimately placeable
    // via bssPrefixTok) plus a reference alias, constructed in place by
    // Make_ below via placement-new -- no heap involved.
    //
    // Deliberately NOT `atomic` (unlike the mutable `_options` array above):
    // the storage bytes are never touched directly -- only through the
    // instance reference below, whose own `.option` field is already
    // `atomic` inside ui::choice::SingleChoice itself -- and a `volatile`
    // byte array couldn't be reinterpret_cast to the non-volatile instance
    // reference below anyway (that would silently drop volatile, which
    // reinterpret_cast refuses; only const_cast may do that). The one
    // non-local mutable field this instance actually exposes is already
    // protected at its source.
    // alignas must come first: `<attribute> alignas(...) inline` fails to
    // parse at namespace scope (clang: "an attribute list cannot appear
    // here") on both host clang and mos-nes-clang++, but
    // `alignas(...) <attribute> inline` is fine -- global scope doesn't
    // trigger it either way, but every generated decl here lives in
    // namespace gen::<scene>, so it matters.
    lines << QString("alignas(ui::choice::SingleChoice) %1inline u8 %2_storage[sizeof(ui::choice::SingleChoice)];")
                 .arg(bssPrefixTok, name);
    lines << QString("inline ui::choice::SingleChoice& %1 = reinterpret_cast<ui::choice::SingleChoice&>(%1_storage);")
                 .arg(name);

    QStringList makeBody;
    if (variadic) {
        for (int i = 0; i < members.size(); ++i) {
            QTreeWidgetItem* member = members[i];
            auto resolveFor = [&resolveBaked, member](const QString& n, int p) { return resolveBaked(member, n, p); };

            QString xSrc = member->data(0, kPosXExprRole).toString().trimmed();
            if (xSrc.isEmpty()) xSrc = QStringLiteral("0");
            QString ySrc = member->data(0, kPosYExprRole).toString().trimmed();
            if (ySrc.isEmpty()) ySrc = QStringLiteral("0");
            bool okX = false;
            bool okY = false;
            ExprPtr xAst = ExprParser(xSrc).parse(okX);
            ExprPtr yAst = ExprParser(ySrc).parse(okY);
            const QString xCpp =
                okX ? emitExprCpp(xAst, resolveFor) : QString::number(member->data(0, kPosXRole).toInt());
            const QString yCpp =
                okY ? emitExprCpp(yAst, resolveFor) : QString::number(member->data(0, kPosYRole).toInt());
            const QString xFinal = ntOffX ? QString("(%1 + %2)").arg(xCpp).arg(ntOffX) : xCpp;
            const QString yFinal = ntOffY ? QString("(%1 + %2)").arg(yCpp).arg(ntOffY) : yCpp;
            makeBody << QString("    %1_options[%2] = vec2<u16>{static_cast<u16>(%3), static_cast<u16>(%4)};")
                            .arg(name)
                            .arg(i)
                            .arg(xFinal, yFinal);
        }
    }
    makeBody << QString("    new (&%1) ui::choice::SingleChoice(%2, %3);").arg(name).arg(nOptions).arg(defaultOption);

    lines << QString("inline AI void Make_%1() {\n%2\n}").arg(name, makeBody.join("\n"));

    return lines.join("\n") + "\n";
}

// Builds the target-specific .hpp/.cpp pair for `rootItem`'s whole scene.
// Components hidden on `target`/`region` (or under a hidden ancestor -- see
// isEffectivelyHidden) are skipped entirely, so an export never emits a
// function for something the scene itself says shouldn't exist there.
GeneratedFiles generateCode(QTreeWidgetItem* rootItem, const QString& target, const QString& region, int nametable,
                             const QString& linkerPrefix, const QString& bssPrefix, const QString& dataPrefix,
                             const QString& charmap, const QString& sceneName) {
    const int ntOffX = (nametable & 1) * kNametableQuadW;
    const int ntOffY = ((nametable >> 1) & 1) * kNametableQuadH;
    // bss_prefix/data_prefix are placement attributes -- a bank or section a
    // linker script maps to a real region of ROM/RAM. Off NES that concept
    // doesn't exist (CREATE_SEGMENT_KEYWORD-built macros already expand to
    // nothing there), and every OTHER target's generated code has no reason
    // to even reference a macro that's only ever defined for the NES build
    // -- so both are only ever emitted when actually exporting for NES.
    // linker_prefix has no such token here: every generated function is now
    // AI (see the ctTextBox branch below), and AI doesn't compose with a
    // placement attribute -- linker_prefix only still feeds the segment
    // guard below, so *some* real value is still required wherever it's set
    // on a scene, even though nothing in this file's output references it
    // anymore.
    const bool isNes = (target == QLatin1String("NES"));
    const QString bssPrefixTok = (isNes && !bssPrefix.trimmed().isEmpty()) ? (bssPrefix.trimmed() + " ") : QString();
    const QString dataPrefixTok =
        (isNes && !dataPrefix.trimmed().isEmpty()) ? (dataPrefix.trimmed() + " ") : QString();
    // Empty until a scene actually sets a Default Charmap -- a scene that
    // hasn't opted in yet keeps ctTextBox's previous plain
    // `static const char[]` behavior rather than referencing a `charmap_`
    // symbol that doesn't exist (see genCtTextBoxBody). A node's own Charmap
    // Override (kCharmapOverrideRole), when set, takes precedence over this
    // default for that node alone -- see nodeCharmapFn below.
    const QString charmapTrimmed = charmap.trimmed();
    const QString defaultCharmapFn = charmapTrimmed.isEmpty() ? QString() : ("charmap_" + charmapTrimmed);
    // Per-node charmap: an explicit override wins over the scene's default;
    // neither set means "no charmap" (plain-string fallback), same as before
    // Charmap Override existed.
    auto nodeCharmapFn = [&defaultCharmapFn](const QTreeWidgetItem* item) -> QString {
        const QString override_ = item->data(0, kCharmapOverrideRole).toString().trimmed();
        return override_.isEmpty() ? defaultCharmapFn : ("charmap_" + override_);
    };

    QStringList hppDecls;
    QStringList cppDefs;
    bool usesSingleChoice = false;
    // Every charmap_ function actually referenced by this export (default
    // and/or per-node overrides), so the in-scope note below (charmapNote)
    // covers all of them, not just the scene default.
    QSet<QString> usedCharmapFns;

    for (QTreeWidgetItem* item : collectComponentItems(rootItem)) {
        if (!isComponentItem(item)) continue;  // negative-space carries no code
        if (isEffectivelyHidden(item, target, region)) continue;

        const QString name = bareName(item);
        const QString charmapFn = nodeCharmapFn(item);
        if (!charmapFn.isEmpty()) usedCharmapFns.insert(charmapFn);

        if (isCtTextBoxItem(item)) {
            // Actually draws (bakes the ppu::WriteFromBufferToNameTable calls
            // itself), unlike rtTextBox's wrapper below -- named Draw_<Name>
            // so a scene's generated API reads the same way SingleChoice's
            // own Make_<Name> does: the verb up front says what calling it
            // does. AI, like every other generated wrapper here -- body
            // merged into the .hpp declaration, not split into a .cpp
            // definition, because AI itself requires that (see ::AI's own
            // comment in technology.hpp: a force-inlined function's body has
            // to be visible in every TU that calls it, which a separate .cpp
            // definition can never guarantee). No linkerPrefixTok (bank
            // placement) either, for the same reason every other AI-tagged
            // function here skips it: AI's own doc comment says it doesn't
            // compose with a placement attribute -- once a body gets
            // duplicated into every caller, there's no single out-of-line
            // copy left for a section attribute to pin anywhere.
            const CtTextBoxGen gen = genCtTextBoxBody(item, name, ntOffX, ntOffY, isNes, dataPrefixTok, charmapFn);
            if (!gen.rootDecls.isEmpty()) {
                hppDecls << gen.rootDecls;
            }
            hppDecls << QString("inline AI void Draw_%1() {\n%2\n}\n").arg(name, gen.body);

            if (item->data(0, kProvideErasingRole).toBool()) {
                const QString eraseBody = genCtTextBoxEraseBody(item, ntOffX, ntOffY, isNes, charmapFn);
                hppDecls << QString("inline AI void Erase_%1() {\n%2\n}\n").arg(name, eraseBody);
            }
        } else {  // rtTextBox
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());
            const QString splitterStr = item->data(0, kSplitterRole).toString();
            const QChar splitter = splitterStr.isEmpty() ? QLatin1Char(' ') : splitterStr.at(0);
            // Text isn't known until runtime, so there's nothing here to
            // charmap-encode -- but whatever buffer the caller eventually
            // passes in IS expected to already be charmap-encoded (it's
            // handed straight to ui::text::Make, same raw bytes ppu writes
            // land untranslated), so the splitter byte Make compares each
            // character against has to be encoded through the very same
            // charmap too, not left as a raw ASCII literal that would never
            // match an encoded space/boundary byte.
            const QString splitterArg = charmapFn.isEmpty()
                                             ? QString("'%1'").arg(cCharEscape(splitter))
                                             : QString("%1('%2')").arg(charmapFn, cCharEscape(splitter));
            hppDecls << QString("inline AI ui::text::textBuffer* %1(const u8* buff, const u8 sBuff) {\n"
                                 "    return ui::text::Make(buff, sBuff, vec2<u8>{%2, %3}, %4);\n"
                                 "}\n")
                             .arg(name)
                             .arg(w)
                             .arg(h)
                             .arg(splitterArg);
        }
    }

    for (QTreeWidgetItem* scItem : collectSingleChoiceItems(rootItem)) {
        if (isEffectivelyHidden(scItem, target, region)) continue;
        const QString decl = genSingleChoiceDecl(scItem, rootItem, target, region, ntOffX, ntOffY, bssPrefixTok);
        if (!decl.isEmpty()) {
            hppDecls << decl;
            usesSingleChoice = true;
        }
    }

    QStringList hppIncludes{"#include <intsh>", "#include <platform-nes/types.hpp>", "#include <platform-nes/video.hpp>",
                             "#include <platform-nes/extras/ui/text.hpp>"};
    if (usesSingleChoice) {
        hppIncludes << "#include <new>" << "#include <platform-nes/extras/ui/singlechoice.hpp>";
    }

    QString charmapNote;
    if (!usedCharmapFns.isEmpty()) {
        QStringList sorted(usedCharmapFns.begin(), usedCharmapFns.end());
        sorted.sort();
        charmapNote = QString("\n// NOTE: %1 must already be in scope here.\n").arg(sorted.join(", "));
    }

    // Linker/BSS/Data Prefix macros must be fed in from local.cmake (see
    // CMakeLists.txt's DEMO_PLACEMENT_DEFINES) -- #error here if one's
    // missing, same #ifndef/#error pattern as PLATFORM_NES_AUDIO_SECTION/UI.
    QSet<QString> usedSegmentMacros;
    if (isNes) {
        for (const QString& p : {linkerPrefix.trimmed(), bssPrefix.trimmed(), dataPrefix.trimmed()}) {
            if (!p.isEmpty()) usedSegmentMacros.insert(p);
        }
    }
    QString segmentGuards;
    if (!usedSegmentMacros.isEmpty()) {
        QStringList sorted(usedSegmentMacros.begin(), usedSegmentMacros.end());
        sorted.sort();
        QStringList guardLines;
        for (const QString& macroName : sorted) {
            guardLines << QString("#ifndef %1\n"
                                   "#error \"%1 is not set -- add it to local.cmake.\"\n"
                                   "#endif")
                            .arg(macroName);
        }
        segmentGuards = "\n" + guardLines.join("\n") + "\n";
    }
    const QString requirementsNote = charmapNote;

    GeneratedFiles out;
    out.hpp = QString("#pragma once\n\n"
                       "// Generated by uitk from \"%1\" -- do not edit by hand.\n"
                       "%4"
                       "\n%2\n"
                       "%5"
                       "\nusing namespace br0::intsh;\n\n"
                       "namespace gen::%1 {\n\n"
                       "%3"
                       "\n}  // namespace gen::%1\n")
                   .arg(sceneName, hppIncludes.join("\n"), hppDecls.join("\n\n"), requirementsNote, segmentGuards);
    out.cpp = QString("// Generated by uitk from \"%1\" -- do not edit by hand.\n\n"
                       "#include \"%2.hpp\"\n\n"
                       "namespace gen::%1 {\n\n"
                       "%3"
                       "\n}  // namespace gen::%1\n")
                  .arg(sceneName, sceneName, cppDefs.join("\n"));
    out.cppHasContent = !cppDefs.isEmpty();
    return out;
}

// A viewport preview: fills the space it's given with the tile grid (its
// size is driven externally by the sidebar's tile counts, not by its own
// size hint), then draws every component node found in `tree` as a
// rectangular region at its stored position/size. Clicking and dragging a
// drawn region moves it (updating its position live); double-clicking it
// edits its text content. Both act through the tree item's data roles, so
// the sidebar's properties panel and the canvas always agree.
class TileGridWidget : public QWidget {
    static constexpr double kGlyphSupersample = 4.0;

public:
    TileGridWidget(double tilePx, QTreeWidget* tree, QComboBox* targetCombo, QComboBox* regionCombo,
                   QWidget* parent = nullptr)
        : QWidget(parent), tilePx_(tilePx), tree_(tree), targetCombo_(targetCombo), regionCombo_(regionCombo) {
        // A monospace font from the OS, sized to fill most of a cell's
        // height -- its glyphs are narrower than they are tall, though, so
        // drawCellGlyph() additionally stretches each one horizontally to
        // fill the (square) cell edge-to-edge. Rasterized several times
        // larger than the cell (kGlyphSupersample) and then scaled back down
        // with SmoothPixmapTransform -- at typical tile sizes (a handful of
        // pixels), rendering directly at the target size produces glyphs too
        // small for FreeType to hint/antialias legibly.
        glyphFont_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        glyphFont_.setPixelSize(std::max(1, qRound(tilePx_ * 0.75 * kGlyphSupersample)));
        naturalGlyphWidthPx_ = std::max(1, QFontMetrics(glyphFont_).horizontalAdvance(QLatin1Char('M')));
        naturalGlyphHeightPx_ = std::max(1, QFontMetrics(glyphFont_).height());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), Qt::black);

        // Negative-space zones paint first, one flat red cell at a time --
        // they're an exclusion zone, not a widget, so nothing (no text, no
        // border styling) draws on top of them here besides whatever
        // (legitimately or not) overlaps them.
        for (QTreeWidgetItem* item : collectComponents()) {
            if (!isNegSpaceItem(item)) {
                continue;
            }
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());
            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    painter.fillRect(tileRect(x + col, y + row), QColor(200, 0, 0));
                }
            }
        }

        for (QTreeWidgetItem* item : collectComponents()) {
            if (!isComponentItem(item)) {
                continue;
            }
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
                const QString splitterStr = item->data(0, kSplitterRole).toString();
                const QChar splitter = splitterStr.isEmpty() ? QLatin1Char(' ') : splitterStr.at(0);
                // Text is wrapped into words at `splitter` boundaries (see
                // wrapTextIntoRows()) and flows one row of words per cell
                // row, top to bottom; rows beyond the region's h are
                // dropped. Alignment positions each row's run of characters
                // within that row's w cells.
                const QStringList rows = wrapTextIntoRows(text, splitter, w);
                for (int row = 0; row < h && row < rows.size(); ++row) {
                    const QString& rowText = rows.at(row);
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


private:
    // Renders `ch` once at its natural (unstretched) size into a cached
    // image, rather than drawing it fresh into every cell. drawCellGlyph()
    // used to stretch glyphs by scaling the *painter* (translate + scale +
    // drawText) -- but a non-uniform painter scale forces Qt's FreeType
    // backend to rasterize a correspondingly distorted glyph outline, which
    // it can fail to do for some glyphs at some squish ratios (logged as
    // "render glyph failed" and silently skipped) -- something this project
    // hit in practice once a wider target (e.g. GCN's 40-tile viewport,
    // narrower per-tile cells than NES's 32) pushed scaleX far enough from
    // 1.0. Caching an unscaled render here and stretching the resulting
    // *bitmap* in drawCellGlyph instead sidesteps FreeType entirely for the
    // stretch step -- image scaling can't fail the way glyph rasterization
    // can.
    const QImage& glyphImage(QChar ch) const {
        auto it = glyphCache_.find(ch);
        if (it != glyphCache_.end()) {
            return it.value();
        }
        QImage image(naturalGlyphWidthPx_, naturalGlyphHeightPx_, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(glyphFont_);
        p.setPen(Qt::white);
        p.drawText(image.rect(), Qt::AlignCenter, QString(ch));
        p.end();
        return glyphCache_.insert(ch, image).value();
    }

    // Draws one glyph filling `cell` edge-to-edge -- monospace glyphs are
    // narrower than they are tall, so the cached natural-size image (see
    // glyphImage()) is stretched horizontally (and, incidentally, by
    // whatever small vertical amount separates its natural height from the
    // cell's) to fill the cell.
    void drawCellGlyph(QPainter& painter, const QRect& cell, QChar ch) const {
        painter.drawImage(cell, glyphImage(ch));
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
    // structurally -- are rendered and hit-testable too. Anything hidden for
    // the scene's current Target/Region (or nested under something that is)
    // is filtered out here rather than in the caller, so paintEvent() and
    // hitTest() -- the only two places this is used -- automatically agree
    // that a hidden node is neither seen nor clickable.
    QVector<QTreeWidgetItem*> collectComponents() const {
        const QString target = targetCombo_->currentText();
        const QString region = regionCombo_->currentText();
        QVector<QTreeWidgetItem*> result;
        for (QTreeWidgetItem* item : collectComponentItems(tree_->invisibleRootItem())) {
            if (!isEffectivelyHidden(item, target, region)) {
                result.push_back(item);
            }
        }
        return result;
    }

    // Later-added components are drawn on top, so hit-testing prefers the
    // last match for overlapping regions to stay consistent with what's
    // visually on top. Textboxes always paint over negative-space zones
    // (see paintEvent()), so they're likewise preferred here -- clicking on
    // a textbox sitting in a violated zone selects/drags the textbox, not
    // the exclusion zone underneath it.
    QTreeWidgetItem* hitTest(const QPoint& pos) const {
        const QPoint cell = cellAt(pos);
        auto contains = [&cell](QTreeWidgetItem* item) {
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());
            return cell.x() >= x && cell.x() < x + w && cell.y() >= y && cell.y() < y + h;
        };
        QTreeWidgetItem* componentMatch = nullptr;
        QTreeWidgetItem* negSpaceMatch = nullptr;
        for (QTreeWidgetItem* item : collectComponents()) {
            if (!contains(item)) {
                continue;
            }
            if (isComponentItem(item)) {
                componentMatch = item;
            } else {
                negSpaceMatch = item;
            }
        }
        return componentMatch ? componentMatch : negSpaceMatch;
    }

    double tilePx_;
    QTreeWidget* tree_;
    QComboBox* targetCombo_;
    QComboBox* regionCombo_;
    QFont glyphFont_;
    int naturalGlyphWidthPx_ = 1;
    int naturalGlyphHeightPx_ = 1;
    mutable QHash<QChar, QImage> glyphCache_;
    QTreeWidgetItem* dragItem_ = nullptr;
    QPoint dragAnchorCell_;
    int dragOriginX_ = 0;
    int dragOriginY_ = 0;
};

struct Sidebar {
    QWidget* content;
    QTreeWidget* tree;
    QLineEdit* xEdit;
    QLineEdit* yEdit;
    QComboBox* targetCombo;
    QComboBox* regionCombo;

    // Exposed for the headless CLI export path (see runCliExport), which
    // needs the same load/resolve/export machinery the File/Export menu
    // actions use, but driven by argv instead of dialogs.
    std::function<QString()> sceneName;
    std::function<bool(const QString&)> loadScene;
    std::function<bool()> hasUnresolvedErrors;
    std::function<bool(const QString&, const QString&)> exportOneTarget;

    // Exposed so a global File menu (built once, outside createSidebar) can
    // act on whichever tab happens to be active -- see TabManager/
    // createFileMenu in main().
    std::function<bool()> save;
    std::function<bool()> saveAs;
    std::function<bool()> confirmDiscard;

    // What TabManager shows as this scene's tab label: "Untitled"/the
    // saved file's name, "*"-suffixed while dirty -- the same text that used
    // to go straight into the (single, scene-wide) window title.
    std::function<QString()> displayName;
};

// `maxTilesX`/`maxTilesY` bound the fields to whatever will actually fit on
// the detected monitor at the current tile scale -- see the call site in
// main() for how those are derived. `tilePx` is that same scale, needed here
// (not just by the canvas) so VIEWPORT_PX/VIEWPORT_PY can be computed.
// `onTitleChanged` is invoked whenever this scene's name or dirty state might
// have changed -- the caller (TabManager, or runCliExport's no-op) is what
// actually decides where that's displayed (a tab label, in TabManager's
// case).
Sidebar createSidebar(UitkMainWindow* window, int maxTilesX, int maxTilesY, double tilePx,
                       std::function<void()> onTitleChanged) {
    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);

    // Node tree (Unity-style scene hierarchy), origin top of sidebar.
    auto* tree = new QTreeWidget(content);
    tree->setHeaderHidden(true);

    auto* rootItem = new QTreeWidgetItem(tree, QStringList{"scene"});
    tree->addTopLevelItem(rootItem);
    tree->expandItem(rootItem);
    tree->setCurrentItem(rootItem);

    // Renaming a prefixed node (geometry, or SingleChoice) must never lose the
    // "[CT] "/"[RT] "/"[N] "/"[SC] " prefix that marks its kind -- if an edit
    // strips it, put it back rather than reject the whole edit, so the rest
    // of the typed name survives. The name after the prefix also has to be a
    // valid, unique C++ identifier: it's both the namespace expressions
    // reference other nodes through, and the symbol that later code
    // generation will emit, so anything else is reverted outright to the
    // last name that was valid.
    QObject::connect(tree, &QTreeWidget::itemChanged, [](QTreeWidgetItem* item, int column) {
        if (column != 0 || !isPrefixedItem(item)) {
            return;
        }
        const QString prefix = requiredPrefixFor(item);
        QString text = item->text(0);
        if (!text.startsWith(prefix)) {
            text = prefix + text;
        }
        const QString base = text.mid(prefix.length());
        static const QRegularExpression kIdentRe(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        bool valid = kIdentRe.match(base).hasMatch();
        if (valid) {
            for (QTreeWidgetItem* other : collectPrefixedItems(item->treeWidget()->invisibleRootItem())) {
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

    // Which physical NES nametable ($2000/$2400/$2800/$2C00) the whole
    // scene's tile positions resolve into in-game -- a scene-wide setting,
    // not a per-node one, since the PPU only ever fetches a tile from one of
    // four 32x30 quadrants at a time (see ppu::CartesianToAddress /
    // xy_to_nt_addr, src/nes/video.cpp: nt_h from x>>5, nt_v from y/30) and
    // this whole UI lives in one such quadrant. Created here, ahead of the
    // File menu below, so Save/Open can already capture it; it's laid out
    // alongside the viewport size fields further down.
    auto* nametableCombo = new QComboBox(content);
    nametableCombo->addItems({"0 ($2000)", "1 ($2400)", "2 ($2800)", "3 ($2C00)"});
    nametableCombo->setToolTip("Which physical NES nametable the whole scene's tile positions "
                                "resolve into in-game -- the same $2000/$2400/$2800/$2C00 quadrant "
                                "ppu::CartesianToAddress selects from a tile coordinate.");

    // Which platform-nes backend the whole scene targets -- also scene-wide,
    // for the same reason: a scene isn't a mix of platforms, it's authored
    // against one.
    auto* targetCombo = new QComboBox(content);
    targetCombo->addItems(kTargetNames);
    targetCombo->setToolTip("Which platform-nes backend this scene targets.");

    // Which TV broadcast standard the scene is timed against -- also
    // scene-wide, for the same reason as target: one scene, one region.
    auto* regionCombo = new QComboBox(content);
    regionCombo->addItems(kRegionNames);
    regionCombo->setToolTip("Which TV broadcast standard this scene is authored/timed against.");

    // A free-form string, scene-wide like nametable/target/region, emitted by
    // codegen ahead of every generated function definition that isn't itself
    // tagged as AI-authored -- e.g. a section/placement attribute -- so those
    // definitions land wherever the linker is meant to put them rather than
    // codegen's default placement. Empty by default: most scenes don't need one.
    auto* linkerPrefixEdit = new QLineEdit(content);
    linkerPrefixEdit->setToolTip("Emitted immediately before every generated function definition that "
                                  "isn't marked AI -- e.g. a linker-section attribute -- so codegen places "
                                  "it wherever the linker expects it. Left empty, nothing is emitted. "
                                  "Only ever emitted when exporting for NES -- every other target's "
                                  "generated code never references it.");

    // Same idea as Linker Prefix, but for a SingleChoice's own mutable
    // storage (and, on a variadic-viewport target, its mutable options
    // array) -- code/rodata and BSS commonly need different placement
    // attributes on NES (see demo/src/banks.hpp's TITLE vs TITLE_DATA for
    // why: LLD rejects code and data sharing one literal section name), so
    // this is a second, independent free-form field rather than reusing
    // Linker Prefix for both. Empty by default, and -- like Linker Prefix --
    // only ever emitted when exporting for NES.
    auto* bssPrefixEdit = new QLineEdit(content);
    bssPrefixEdit->setToolTip("Emitted immediately before a SingleChoice's own instance storage (and, on a "
                               "variadic-viewport target, its mutable options array) -- e.g. a RAM-section "
                               "placement attribute. Left empty, nothing is emitted. Only ever emitted when "
                               "exporting for NES.");

    // Third placement field, same NES-only rule as Linker/BSS Prefix, but
    // for a ctTextBox's own charmap-encoded row data (see genCtTextBoxBody)
    // -- rodata, same as code, but LLD still rejects code and data sharing
    // one literal section name (demo/src/banks.hpp's TITLE vs TITLE_DATA is
    // exactly this split, hand-written), so it needs its own independent
    // field rather than reusing Linker Prefix.
    auto* dataPrefixEdit = new QLineEdit(content);
    dataPrefixEdit->setToolTip("Emitted immediately before a ctTextBox's own charmap-encoded row data -- e.g. "
                                "a ROM-section placement attribute (see demo/src/graphics/strings.hpp's "
                                "TITLE_DATA for the hand-written equivalent). Left empty, nothing is emitted. "
                                "Only ever emitted when exporting for NES.");

    // The mapname passed to ::tech::nes_str::encode<charmap_<name>> for
    // every ctTextBox's row text, and to charmap_<name> directly for a
    // rtTextBox's splitter byte (see genCtTextBoxBody/generateCode) --
    // `charmap_<name>` itself is project code (defined via technology.hpp's
    // CHARMAP macro, e.g. demo/src/graphics/charmaps.hpp's charmap_generic),
    // not something uitk can generate, so this only ever names it. Left
    // empty, a scene falls back to its previous plain `static const char[]`
    // behavior instead of referencing a symbol that doesn't exist. This is
    // only the scene-wide default -- any individual textbox can name a
    // different mapname via its own Charmap Override property
    // (kCharmapOverrideRole), which wins over this for that node alone.
    auto* charmapEdit = new QLineEdit(content);
    charmapEdit->setToolTip("Default mapname of the CHARMAP (technology.hpp) every ctTextBox's row text -- and "
                             "every rtTextBox's splitter byte -- is encoded through, e.g. \"generic\" for "
                             "charmap_generic. charmap_<name> must already be defined and in scope wherever "
                             "the generated header is included. Left empty, text is emitted as a plain, "
                             "unencoded C string instead (unless a node's own Charmap Override sets one). "
                             "A textbox's Charmap Override property replaces this default for that node.");

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

    // Scene name generated files (and the .cpp's #include of its own .hpp)
    // are keyed by -- the saved file's basename, or "scene" before a scene's
    // ever been saved once. Also what the tree's root item displays (see
    // updateTitle below), in place of a literal "root" label, so the tree
    // always shows what the exported gen::<sceneName> namespace will
    // actually be called.
    auto sceneName = [currentPath] {
        return currentPath->isEmpty() ? QStringLiteral("scene") : QFileInfo(*currentPath).completeBaseName();
    };

    // What this scene's tab should be labeled -- distinct from sceneName()
    // above (which always names the exported namespace, "scene" default and
    // all): "Untitled" before the first save, the saved file's name after,
    // "*"-suffixed whenever there are unsaved changes.
    auto displayName = [currentPath, dirty] {
        const QString name =
            currentPath->isEmpty() ? QStringLiteral("Untitled") : QFileInfo(*currentPath).fileName();
        return name + (*dirty ? QStringLiteral("*") : QString());
    };

    auto updateTitle = [rootItem, tree, sceneName, onTitleChanged] {
        // Blocked so relabeling root doesn't itself re-trigger the
        // itemChanged handler below (which would mark the scene dirty and
        // call back into updateTitle for a purely cosmetic rename).
        {
            const QSignalBlocker blocker(tree);
            rootItem->setText(0, sceneName());
        }
        onTitleChanged();
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

    auto doSaveAs = [window, rootItem, currentPath, dirty, updateTitle, nametableCombo, targetCombo, regionCombo,
                     linkerPrefixEdit, bssPrefixEdit, dataPrefixEdit, charmapEdit]() {
        QString path = QFileDialog::getSaveFileName(window, "Save Scene", QString(), "UI Scene (*.uis)");
        if (path.isEmpty()) {
            return false;
        }
        if (!path.endsWith(".uis", Qt::CaseInsensitive)) {
            path += ".uis";
        }
        if (!writeUisFile(path, rootItem, nametableCombo->currentIndex(), targetCombo->currentText(),
                           regionCombo->currentText(), linkerPrefixEdit->text(), bssPrefixEdit->text(),
                           dataPrefixEdit->text(), charmapEdit->text())) {
            QMessageBox::warning(window, "Save Failed", "Could not write file:\n" + path);
            return false;
        }
        *currentPath = path;
        *dirty = false;
        updateTitle();
        return true;
    };

    auto doSave = [rootItem, currentPath, dirty, updateTitle, doSaveAs, window, nametableCombo, targetCombo,
                   regionCombo, linkerPrefixEdit, bssPrefixEdit, dataPrefixEdit, charmapEdit]() {
        if (currentPath->isEmpty()) {
            return doSaveAs();
        }
        if (!writeUisFile(*currentPath, rootItem, nametableCombo->currentIndex(), targetCombo->currentText(),
                           regionCombo->currentText(), linkerPrefixEdit->text(), bssPrefixEdit->text(),
                           dataPrefixEdit->text(), charmapEdit->text())) {
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

    // Parses `path` and rebuilds the tree/combos from it wholesale -- the
    // guts of Open, factored out so the headless CLI export path (see
    // runCliExport) can load a scene the exact same way without going
    // through a file-picker dialog or the unsaved-changes prompt.
    auto loadSceneFromFile = [tree, rootItem, currentPath, dirty, loading, updateTitle, resolveAllPtr,
                               nametableCombo, targetCombo, regionCombo, linkerPrefixEdit, bssPrefixEdit,
                               dataPrefixEdit, charmapEdit](const QString& path) -> bool {
        // Parse before touching the tree, so a corrupt/unreadable file never
        // wipes out whatever scene was already open.
        QJsonArray nodes;
        int nametable = 0;
        QString target;
        QString region;
        QString linkerPrefix;
        QString bssPrefix;
        QString dataPrefix;
        QString charmap;
        if (!parseUisFile(path, nodes, nametable, target, region, linkerPrefix, bssPrefix, dataPrefix, charmap)) {
            return false;
        }
        // An unrecognized or missing target/region (an older file, or a
        // hand-edited one) falls back to the first entry rather than leaving
        // the combo on whatever it happened to already be showing.
        const int targetIndex = std::max(0, static_cast<int>(kTargetNames.indexOf(target)));
        const int regionIndex = std::max(0, static_cast<int>(kRegionNames.indexOf(region)));
        *loading = true;
        qDeleteAll(rootItem->takeChildren());
        for (const QJsonValue& node : nodes) {
            deserializeNode(rootItem, node.toObject());
        }
        nametableCombo->setCurrentIndex(nametable);
        targetCombo->setCurrentIndex(targetIndex);
        regionCombo->setCurrentIndex(regionIndex);
        linkerPrefixEdit->setText(linkerPrefix);
        bssPrefixEdit->setText(bssPrefix);
        dataPrefixEdit->setText(dataPrefix);
        charmapEdit->setText(charmap);
        *loading = false;
        (*resolveAllPtr)();
        tree->expandItem(rootItem);
        *currentPath = path;
        *dirty = false;
        updateTitle();
        return true;
    };

    // Refuses to export while any component can't currently be resolved --
    // exporting a scene with an unresolved position/size would just bake in
    // whatever stale/default value happened to be sitting in kPosXRole etc.,
    // silently wrong rather than loudly refused.
    auto hasUnresolvedErrors = [rootItem]() {
        for (const QTreeWidgetItem* item : collectComponentItems(rootItem)) {
            if (item->data(0, kErrorRole).toBool()) return true;
        }
        return false;
    };

    // Writes `content` to `path`, but only touches the file (and its mtime)
    // when the content actually differs -- and always logs which happened to
    // stdout. A GUI export has QMessageBox for outcomes, but a headless CLI
    // export (see runCliExport, invoked from CMake's uitk-export target and
    // CI's codegen-check job) has nothing *but* this to tell a caller what
    // was and wasn't updated -- that's the log a `cmake --build --target
    // uitk-export` run shows.
    auto writeIfChanged = [](const QString& path, const QString& content) -> bool {
        QFile file(path);
        const bool existedBefore = file.exists();
        if (existedBefore && file.open(QIODevice::ReadOnly)) {
            const bool unchanged = (QString::fromUtf8(file.readAll()) == content);
            file.close();
            if (unchanged) {
                std::fprintf(stdout, "  unchanged  %s\n", qPrintable(path));
                return true;
            }
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "  FAILED     %s\n", qPrintable(path));
            return false;
        }
        file.write(content.toUtf8());
        std::fprintf(stdout, "  %s    %s\n", existedBefore ? "updated" : "created", qPrintable(path));
        return true;
    };

    // Writes one target's generated .hpp/.cpp pair into `dir`/<target-lower>/,
    // matching the gen/<target>/... layout the #include STRCAT(...) convention
    // (technology.hpp) expects on the consuming side.
    auto exportOneTarget = [window, rootItem, regionCombo, nametableCombo, linkerPrefixEdit, bssPrefixEdit,
                             dataPrefixEdit, charmapEdit, sceneName,
                             writeIfChanged](const QString& dir, const QString& target) -> bool {
        const QString targetDir = dir + "/" + target.toLower();
        if (!QDir().mkpath(targetDir)) return false;
        const GeneratedFiles files = generateCode(rootItem, target, regionCombo->currentText(),
                                                    nametableCombo->currentIndex(), linkerPrefixEdit->text(),
                                                    bssPrefixEdit->text(), dataPrefixEdit->text(),
                                                    charmapEdit->text(), sceneName());
        const bool hppOk = writeIfChanged(targetDir + "/" + sceneName() + ".hpp", files.hpp);

        // Only write the .cpp when codegen actually put something in it
        // (see GeneratedFiles::cppHasContent) -- every wrapper this tool
        // emits is AI now, body-in-header, so most scenes never need one at
        // all. If an earlier export (before AI, or before the scene's last
        // ctTextBox lost its only cpp-side content) left a stale .cpp
        // behind, remove it rather than leaving dead generated code around;
        // if there's genuinely nothing to write and nothing on disk either,
        // leave the directory alone instead of creating an empty file.
        const QString cppPath = targetDir + "/" + sceneName() + ".cpp";
        bool cppOk = true;
        if (files.cppHasContent) {
            cppOk = writeIfChanged(cppPath, files.cpp);
        } else if (QFile::exists(cppPath)) {
            cppOk = QFile::remove(cppPath);
            std::fprintf(stdout, cppOk ? "  removed    %s\n" : "  FAILED     %s\n", qPrintable(cppPath));
        }
        return hppOk && cppOk;
    };


    // Persisted scene state, same as any tree edit -- but neither combo is a
    // tree item, so each needs its own dirty-marking hookup. Guarded by
    // `loading` for the same reason the tree's is: New/Open set it while
    // rebuilding the scene wholesale, which isn't itself an unsaved change.
    auto markDirtyFromCombo = [dirty, loading, updateTitle] {
        if (!*loading) {
            *dirty = true;
            updateTitle();
        }
    };
    QObject::connect(nametableCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                      [markDirtyFromCombo](int) { markDirtyFromCombo(); });
    QObject::connect(targetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                      [markDirtyFromCombo](int) { markDirtyFromCombo(); });
    QObject::connect(regionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                      [markDirtyFromCombo](int) { markDirtyFromCombo(); });
    QObject::connect(linkerPrefixEdit, &QLineEdit::textChanged,
                      [markDirtyFromCombo](const QString&) { markDirtyFromCombo(); });
    QObject::connect(bssPrefixEdit, &QLineEdit::textChanged,
                      [markDirtyFromCombo](const QString&) { markDirtyFromCombo(); });
    QObject::connect(dataPrefixEdit, &QLineEdit::textChanged,
                      [markDirtyFromCombo](const QString&) { markDirtyFromCombo(); });
    QObject::connect(charmapEdit, &QLineEdit::textChanged,
                      [markDirtyFromCombo](const QString&) { markDirtyFromCombo(); });

    // --- Properties panel: shows/edits the selected node's geometry,
    // alignment, and hide-on-Target/Region lists. Hidden entirely (not just
    // grayed out) whenever the selection isn't a prefixed node (e.g. root, or
    // nothing selected) -- a root/branch node has no such properties at all,
    // so there's nothing here for it to show; individual rows within it are
    // further hidden per-kind (e.g. SingleChoice has no geometry). Position/
    // size fields accept either a plain literal ("5") or a formula
    // referencing other nodes by name ("Other.pos.x + 1") -- the resolver
    // (assigned to *resolveAllPtr below, once xEdit/yEdit exist) turns
    // whichever was typed into the resolved int the canvas actually uses.
    auto* properties = new QWidget(content);
    auto* posXEdit = new QLineEdit(properties);
    auto* posYEdit = new QLineEdit(properties);
    auto* sizeWEdit = new QLineEdit(properties);
    auto* sizeHEdit = new QLineEdit(properties);
    auto* alignCombo = new QComboBox(properties);
    auto* textEdit = new QLineEdit(properties);
    auto* splitterEdit = new QLineEdit(properties);
    auto* charmapOverrideEdit = new QLineEdit(properties);
    // ctTextBox-only -- see kProvideErasingRole.
    auto* provideErasingCheck = new QCheckBox(properties);
    // SingleChoice-only: which option child (0-based, in tree order) Make
    // selects by default -- clamped against the child count at export time,
    // not here, since the count can change (options added/removed) after
    // this is set.
    auto* defaultOptionSpin = new QSpinBox(properties);
    defaultOptionSpin->setRange(0, 255);
    defaultOptionSpin->setToolTip("Which option (0-based, in tree order) this SingleChoice starts on.\n"
                                    "Clamped to the number of option children it actually has on export.");

    // Hide-on-Target/Region: a checkable-menu button rather than a list
    // widget, so a multi-select fits the sidebar's width without eating a
    // block of vertical space the way an always-expanded checklist would.
    auto* hideTargetsButton = new QPushButton(properties);
    auto* hideTargetsMenu = new QMenu(hideTargetsButton);
    QVector<QAction*> hideTargetActions;
    for (const QString& name : kTargetNames) {
        QAction* action = hideTargetsMenu->addAction(name);
        action->setCheckable(true);
        hideTargetActions.push_back(action);
    }
    hideTargetsButton->setMenu(hideTargetsMenu);
    hideTargetsButton->setToolTip("Hide this node (and its children) whenever the scene's current "
                                   "Target is one of the checked platforms.");

    auto* hideRegionsButton = new QPushButton(properties);
    auto* hideRegionsMenu = new QMenu(hideRegionsButton);
    QVector<QAction*> hideRegionActions;
    for (const QString& name : kRegionNames) {
        QAction* action = hideRegionsMenu->addAction(name);
        action->setCheckable(true);
        hideRegionActions.push_back(action);
    }
    hideRegionsButton->setMenu(hideRegionsMenu);
    hideRegionsButton->setToolTip("Hide this node (and its children) whenever the scene's current "
                                   "Region is one of the checked broadcast standards.");

    // Summarizes which of `actions` are currently checked onto `button`'s own
    // label (e.g. "GBA, PSP", or "(none)") so the selection is visible
    // without opening the menu.
    auto updateHideButtonSummary = [](QPushButton* button, const QVector<QAction*>& actions) {
        QStringList checked;
        for (QAction* action : actions) {
            if (action->isChecked()) checked << action->text();
        }
        button->setText(checked.isEmpty() ? QStringLiteral("(none)") : checked.join(QStringLiteral(", ")));
    };
    // Suppresses the hide actions' write-back (below) while populateFrom is
    // driving their checked state from the selected item -- same purpose as
    // the QSignalBlockers on the other fields, but QAction::toggled needs an
    // explicit guard since a QSignalBlocker would have to be built per-action.
    auto suppressHideWrite = std::make_shared<bool>(false);

    const QString exprHint = "A number, or a formula like Other.pos.x + 1.\n"
                              "Operators: + - * / << >> and parentheses.\n"
                              "VIEWPORT_TX/TY = viewport size in tiles, VIEWPORT_PX/PY = in pixels.\n"
                              "Other.textSize / this.textSize = that textbox's text length in tiles.\n"
                              "'this' refers to the node the formula is on, e.g. this.textSize.";
    posXEdit->setToolTip(exprHint);
    posYEdit->setToolTip(exprHint);
    sizeWEdit->setToolTip(exprHint);
    sizeHEdit->setToolTip(exprHint);
    alignCombo->addItems({"Left", "Center", "Right"});
    splitterEdit->setMaxLength(1);
    splitterEdit->setToolTip("The single character that marks a word boundary when wrapping text "
                              "onto the next row (default: space).");
    charmapOverrideEdit->setToolTip("CHARMAP mapname (technology.hpp) to encode this node's row text/splitter "
                                     "through, in place of the scene's Default Charmap.\n"
                                     "Leave blank to use the scene's Default Charmap.");
    provideErasingCheck->setToolTip("Also generate an Erase_<Name>() function that blanks this box's whole "
                                     "footprint with its charmap's ' ' tile, via ppu::WriteRepeatedToNameTable.");

    // Grouped into "Geometry" (any geometry node's position/size),
    // "Properties" (textbox-only: alignment/text/splitter), and "Visibility"
    // (every prefixed kind's hide-on-Target/Region) -- whole groups are
    // shown/hidden together per the selected node's kind (see the
    // currentItemChanged handler below), rather than toggling individual
    // rows within a single shared form.
    auto* propertiesLayout = new QVBoxLayout(properties);
    propertiesLayout->setContentsMargins(0, 0, 0, 0);

    auto* geometryGroup = new QGroupBox("Geometry", properties);
    auto* geometryForm = new QFormLayout(geometryGroup);
    // The sidebar is only ~100-200px wide -- a label sharing a row with its
    // field gets squeezed down to nothing legible. Wrapping long rows puts
    // the label on its own row above the field instead, so it's always
    // fully readable.
    geometryForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    geometryForm->addRow("Position X", posXEdit);
    geometryForm->addRow("Position Y", posYEdit);
    geometryForm->addRow("Size X", sizeWEdit);
    geometryForm->addRow("Size Y", sizeHEdit);

    auto* componentGroup = new QGroupBox("Properties", properties);
    auto* componentForm = new QFormLayout(componentGroup);
    componentForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    componentForm->addRow("Alignment", alignCombo);
    componentForm->addRow("Text", textEdit);
    componentForm->addRow("Splitter", splitterEdit);
    componentForm->addRow("Charmap Override", charmapOverrideEdit);

    auto* choiceGroup = new QGroupBox("Choice", properties);
    auto* choiceForm = new QFormLayout(choiceGroup);
    choiceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    choiceForm->addRow("Default Option", defaultOptionSpin);

    // ctTextBox-only, unlike componentGroup above (shared with rtTextBox) --
    // a separate group so its visibility can be toggled independently of
    // Alignment/Text/Splitter/Charmap Override.
    auto* erasingGroup = new QGroupBox("Erasing", properties);
    auto* erasingForm = new QFormLayout(erasingGroup);
    erasingForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    erasingForm->addRow("Provide Erasing", provideErasingCheck);

    auto* visibilityGroup = new QGroupBox("Visibility", properties);
    auto* visibilityForm = new QFormLayout(visibilityGroup);
    visibilityForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    visibilityForm->addRow("Hide on Target", hideTargetsButton);
    visibilityForm->addRow("Hide on Region", hideRegionsButton);

    propertiesLayout->addWidget(geometryGroup);
    propertiesLayout->addWidget(componentGroup);
    propertiesLayout->addWidget(erasingGroup);
    propertiesLayout->addWidget(choiceGroup);
    propertiesLayout->addWidget(visibilityGroup);
    properties->setVisible(false);

    // Populates the panel's fields from `item` without re-triggering the
    // edit handlers below (which would otherwise write the same values
    // straight back -- harmless, but pointless).
    auto populateFrom = [posXEdit, posYEdit, sizeWEdit, sizeHEdit, alignCombo, textEdit, splitterEdit,
                         charmapOverrideEdit, provideErasingCheck, defaultOptionSpin, hideTargetsButton,
                         hideTargetActions, hideRegionsButton, hideRegionActions, updateHideButtonSummary,
                         suppressHideWrite](QTreeWidgetItem* item) {
        const QSignalBlocker bx(posXEdit);
        const QSignalBlocker by(posYEdit);
        const QSignalBlocker bw(sizeWEdit);
        const QSignalBlocker bh(sizeHEdit);
        const QSignalBlocker ba(alignCombo);
        const QSignalBlocker bt(textEdit);
        const QSignalBlocker bs(splitterEdit);
        const QSignalBlocker bc(charmapOverrideEdit);
        const QSignalBlocker be(provideErasingCheck);
        const QSignalBlocker bd(defaultOptionSpin);
        auto exprOr = [item](int exprRole, int fallback) {
            const QString s = item->data(0, exprRole).toString();
            return s.isEmpty() ? QString::number(fallback) : s;
        };
        posXEdit->setText(exprOr(kPosXExprRole, 0));
        posYEdit->setText(exprOr(kPosYExprRole, 0));
        sizeWEdit->setText(exprOr(kSizeWExprRole, 1));
        sizeHEdit->setText(exprOr(kSizeHExprRole, 1));
        alignCombo->setCurrentIndex(item->data(0, kAlignRole).toInt());
        textEdit->setText(item->data(0, kTextContentRole).toString());
        const QString splitter = item->data(0, kSplitterRole).toString();
        splitterEdit->setText(splitter.isEmpty() ? QStringLiteral(" ") : splitter);
        charmapOverrideEdit->setText(item->data(0, kCharmapOverrideRole).toString());
        provideErasingCheck->setChecked(item->data(0, kProvideErasingRole).toBool());
        defaultOptionSpin->setValue(item->data(0, kDefaultOptionRole).toInt());

        *suppressHideWrite = true;
        const QStringList hideTargets = item->data(0, kHideTargetsRole).toStringList();
        for (QAction* action : hideTargetActions) {
            action->setChecked(hideTargets.contains(action->text()));
        }
        const QStringList hideRegions = item->data(0, kHideRegionsRole).toStringList();
        for (QAction* action : hideRegionActions) {
            action->setChecked(hideRegions.contains(action->text()));
        }
        *suppressHideWrite = false;
        updateHideButtonSummary(hideTargetsButton, hideTargetActions);
        updateHideButtonSummary(hideRegionsButton, hideRegionActions);
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

    QObject::connect(
        tree, &QTreeWidget::currentItemChanged,
        [properties, geometryGroup, componentGroup, erasingGroup, choiceGroup, visibilityGroup, populateFrom,
         updateErrorHighlight](QTreeWidgetItem* current, QTreeWidgetItem*) {
            const bool selected = isPrefixedItem(current);
            properties->setVisible(selected);
            // Geometry means nothing for SingleChoice -- it's a pure grouping
            // node, no position/size of its own. Alignment/text/splitter only
            // mean something for a textbox's text -- neither SingleChoice nor
            // a negative-space zone has any text. Provide Erasing is
            // ctTextBox-only: rtTextBox's text isn't known until runtime, so
            // there's no fixed erase tile to bake. Default Option only means
            // something for SingleChoice itself. Visibility (hide-on-
            // Target/Region) applies to every prefixed kind, so that group is
            // never toggled off here.
            geometryGroup->setVisible(isGeometryItem(current));
            componentGroup->setVisible(isComponentItem(current));
            erasingGroup->setVisible(isCtTextBoxItem(current));
            choiceGroup->setVisible(isSingleChoiceItem(current));
            visibilityGroup->setVisible(selected);
            if (selected) {
                populateFrom(current);
                updateErrorHighlight(current);
            }
        });

    // The selected item's geometry/hide lists can also change from outside
    // the panel -- dragging it on the canvas, or the resolver recomputing a
    // formula -- so keep the panel's fields from going stale whenever that
    // happens. Skipped while a field has focus so an unrelated change
    // elsewhere doesn't clobber an in-progress edit.
    QObject::connect(
        tree, &QTreeWidget::itemChanged,
        [tree, populateFrom, updateErrorHighlight, posXEdit, posYEdit, sizeWEdit, sizeHEdit, textEdit,
         splitterEdit, charmapOverrideEdit, provideErasingCheck, defaultOptionSpin](QTreeWidgetItem* item,
                                                                                     int column) {
            if (column != 0 || item != tree->currentItem() || !isPrefixedItem(item)) {
                return;
            }
            updateErrorHighlight(item);
            if (posXEdit->hasFocus() || posYEdit->hasFocus() || sizeWEdit->hasFocus() || sizeHEdit->hasFocus() ||
                textEdit->hasFocus() || splitterEdit->hasFocus() || charmapOverrideEdit->hasFocus() ||
                provideErasingCheck->hasFocus() || defaultOptionSpin->hasFocus()) {
                return;
            }
            populateFrom(item);
        });

    // Committed on editingFinished (Enter, or losing focus), not on every
    // keystroke -- a formula is only meaningful once fully typed.
    auto writeToSelection = [tree](int exprRole, const QString& value) {
        QTreeWidgetItem* item = tree->currentItem();
        // Position/size apply to any geometry node -- textbox or negative
        // space -- with identical expression support (VIEWPORT_TX/PY,
        // references to other nodes, etc.); only alignment is textbox-only.
        if (isGeometryItem(item)) {
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
    QObject::connect(textEdit, &QLineEdit::editingFinished, [tree, textEdit] {
        QTreeWidgetItem* item = tree->currentItem();
        if (isComponentItem(item)) {
            item->setData(0, kTextContentRole, textEdit->text());
        }
    });
    QObject::connect(splitterEdit, &QLineEdit::editingFinished, [tree, splitterEdit] {
        QTreeWidgetItem* item = tree->currentItem();
        if (isComponentItem(item)) {
            const QString text = splitterEdit->text();
            item->setData(0, kSplitterRole, text.isEmpty() ? QStringLiteral(" ") : text);
        }
    });
    QObject::connect(charmapOverrideEdit, &QLineEdit::editingFinished, [tree, charmapOverrideEdit] {
        QTreeWidgetItem* item = tree->currentItem();
        if (isComponentItem(item)) {
            item->setData(0, kCharmapOverrideRole, charmapOverrideEdit->text().trimmed());
        }
    });
    QObject::connect(provideErasingCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isCtTextBoxItem(item)) {
            item->setData(0, kProvideErasingRole, checked);
        }
    });
    QObject::connect(defaultOptionSpin, qOverload<int>(&QSpinBox::valueChanged), [tree](int v) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isSingleChoiceItem(item)) {
            item->setData(0, kDefaultOptionRole, v);
        }
    });
    // Applies to every prefixed kind (geometry or SingleChoice) -- unlike the
    // other fields above, checking/unchecking one entry writes the whole
    // list back immediately rather than waiting on editingFinished, matching
    // how the other combos here commit on selection rather than on blur.
    auto writeHideSelection = [tree, suppressHideWrite](int role, QPushButton* button,
                                                          const QVector<QAction*>& actions,
                                                          const std::function<void(QPushButton*, const QVector<QAction*>&)>& updateSummary) {
        if (*suppressHideWrite) {
            return;
        }
        QTreeWidgetItem* item = tree->currentItem();
        if (!isPrefixedItem(item)) {
            return;
        }
        QStringList checked;
        for (QAction* action : actions) {
            if (action->isChecked()) checked << action->text();
        }
        item->setData(0, role, checked);
        updateSummary(button, actions);
    };
    for (QAction* action : hideTargetActions) {
        QObject::connect(action, &QAction::toggled, [writeHideSelection, hideTargetsButton, hideTargetActions,
                                                       updateHideButtonSummary](bool) {
            writeHideSelection(kHideTargetsRole, hideTargetsButton, hideTargetActions, updateHideButtonSummary);
        });
    }
    for (QAction* action : hideRegionActions) {
        QObject::connect(action, &QAction::toggled, [writeHideSelection, hideRegionsButton, hideRegionActions,
                                                       updateHideButtonSummary](bool) {
            writeHideSelection(kHideRegionsRole, hideRegionsButton, hideRegionActions, updateHideButtonSummary);
        });
    }
    // addCtTextBoxNode()/addRtTextBoxNode()/addNegativeSpaceNode() take an explicit parent so
    // nesting under other nodes (not just root) already works -- there's
    // just no UI for it yet, since every add here always targets root,
    // keeping newly added nodes as root's siblings-of-each-other for now.
    // New nodes start at the origin (0,0) with a 1x1 footprint; drag them on
    // the canvas or use the properties panel to place/resize them.
    // Shared by both textbox flavors -- they differ only in kind string and
    // name prefix, so one counter (shared across both) numbers default names
    // for either, keeping "TextboxN" identifiers unique across the pair
    // rather than each flavor restarting at 1 and colliding once the prefix
    // is stripped off for name resolution.
    auto textBoxCounter = std::make_shared<int>(1);
    auto addTextBoxNode = [textBoxCounter](QTreeWidgetItem* parent, const char* kind, const QString& prefix) {
        // No space in the default name -- it has to already be a valid C++
        // identifier, since it's usable immediately in another node's
        // expression.
        const QString name = prefix + "Textbox" + QString::number((*textBoxCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kind));
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
        child->setData(0, kSplitterRole, QStringLiteral(" "));
        child->setData(0, kLastValidNameRole, name);
        parent->setExpanded(true);
        return child;
    };
    auto addCtTextBoxNode = [addTextBoxNode](QTreeWidgetItem* parent) {
        return addTextBoxNode(parent, kCtTextBoxKind, QString(kCtTextBoxPrefix));
    };
    auto addRtTextBoxNode = [addTextBoxNode](QTreeWidgetItem* parent) {
        return addTextBoxNode(parent, kRtTextBoxKind, QString(kRtTextBoxPrefix));
    };

    auto negSpaceCounter = std::make_shared<int>(1);
    auto addNegativeSpaceNode = [negSpaceCounter](QTreeWidgetItem* parent) {
        const QString name = QString(kNegSpacePrefix) + "NegativeSpace" + QString::number((*negSpaceCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kNegSpaceKind));
        child->setData(0, kPosXExprRole, QStringLiteral("0"));
        child->setData(0, kPosYExprRole, QStringLiteral("0"));
        child->setData(0, kSizeWExprRole, QStringLiteral("1"));
        child->setData(0, kSizeHExprRole, QStringLiteral("1"));
        child->setData(0, kPosXRole, 0);
        child->setData(0, kPosYRole, 0);
        child->setData(0, kSizeWRole, 1);
        child->setData(0, kSizeHRole, 1);
        child->setData(0, kLastValidNameRole, name);
        parent->setExpanded(true);
        return child;
    };

    // A single choice is nothing but a parent node at this stage -- no
    // geometry, no properties -- so it just needs a name and its kind; its
    // ctTextBox/rtTextBox option children are added onto it the same way as
    // onto root, via the context menu below. It's still a genuine node
    // though, not a plain branch, so it carries the "[SC] " prefix and
    // participates in the same name validation as every other prefixed node.
    auto singleChoiceCounter = std::make_shared<int>(1);
    auto addSingleChoiceNode = [singleChoiceCounter](QTreeWidgetItem* parent) {
        const QString name = QString(kSingleChoicePrefix) + "SingleChoice" + QString::number((*singleChoiceCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kSingleChoiceKind));
        child->setData(0, kLastValidNameRole, name);
        parent->setExpanded(true);
        return child;
    };

    // Deleting a node is structural (unlike every other edit here, which
    // goes through setData()/itemChanged), so it has to explicitly do what
    // itemChanged would otherwise trigger automatically: mark the scene
    // dirty, and re-resolve everything, since any other node's expression
    // that referenced the deleted one by name now refers to nothing and
    // needs to be (re-)flagged as unresolvable. Root is never deletable --
    // it's the one node the scene can't be without.
    auto deleteNode = [rootItem, dirty, updateTitle, resolveAllPtr](QTreeWidgetItem* item) {
        if (!item || item == rootItem) {
            return;
        }
        delete item;
        *dirty = true;
        updateTitle();
        (*resolveAllPtr)();
    };

    // Scoped to the tree (and its inline-rename editor) specifically, not
    // the whole window -- otherwise Delete would also fire while, say, a
    // properties field has focus.
    auto* deleteShortcut = new QShortcut(QKeySequence::Delete, tree);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(deleteShortcut, &QShortcut::activated,
                      [tree, deleteNode] { deleteNode(tree->currentItem()); });

    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(
        tree, &QTreeWidget::customContextMenuRequested,
        [tree, rootItem, addCtTextBoxNode, addRtTextBoxNode, addNegativeSpaceNode, addSingleChoiceNode,
         deleteNode](const QPoint& pos) {
            QTreeWidgetItem* clicked = tree->itemAt(pos);
            const bool onSingleChoice = isSingleChoiceItem(clicked);
            // A single choice's own children are its ctTextBox/rtTextBox
            // options, so right-clicking one targets textbox adds at it
            // rather than at root -- same as every other add here, which
            // still always targets root until there's UI for nesting more
            // generally. It houses only textboxes, so Negative Space and
            // nested Single Choice aren't offered there at all.
            QTreeWidgetItem* textBoxParent = onSingleChoice ? clicked : rootItem;
            QMenu menu;
            QAction* addCtTextboxAction = menu.addAction("Add Compile-Time Textbox");
            QAction* addRtTextboxAction = menu.addAction("Add Runtime Textbox");
            QAction* addNegSpaceAction = onSingleChoice ? nullptr : menu.addAction("Add Negative Space");
            QAction* addSingleChoiceAction = onSingleChoice ? nullptr : menu.addAction("Add Single Choice");
            QAction* deleteAction = nullptr;
            if (clicked && clicked != rootItem) {
                menu.addSeparator();
                deleteAction = menu.addAction("Delete");
            }
            QAction* chosen = menu.exec(tree->viewport()->mapToGlobal(pos));
            if (chosen == addCtTextboxAction) {
                tree->setCurrentItem(addCtTextBoxNode(textBoxParent));
            } else if (chosen == addRtTextboxAction) {
                tree->setCurrentItem(addRtTextBoxNode(textBoxParent));
            } else if (chosen && chosen == addNegSpaceAction) {
                tree->setCurrentItem(addNegativeSpaceNode(rootItem));
            } else if (chosen && chosen == addSingleChoiceAction) {
                tree->setCurrentItem(addSingleChoiceNode(rootItem));
            } else if (chosen && chosen == deleteAction) {
                deleteNode(clicked);
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

    // Picking a Target sets Viewport X/Y to that backend's real panel size
    // (see viewportTilesForTarget()) -- xEdit/yEdit's own textChanged handler
    // above still clamps the result against the monitor's maxTiles, same as
    // any other edit to these fields. Fires unconditionally, including while
    // New/Open are programmatically driving the combo (`loading`) and not
    // just on a user-picked change: viewport tile counts are never persisted
    // in the .uis file (see writeUisFile/parseUisFile) since they're a
    // property of the *target*, not the scene, so re-deriving them from
    // whatever target a freshly-opened or -reset scene ends up on is exactly
    // what should happen rather than leaving stale tiles from whatever scene
    // was open before. Targets with no fixed panel (see
    // viewportTilesForTarget()) leave Viewport X/Y untouched.
    QObject::connect(targetCombo, &QComboBox::currentTextChanged, [xEdit, yEdit, maxTilesX, maxTilesY](const QString& target) {
        const auto tiles = viewportTilesForTarget(target);
        if (!tiles) {
            return;
        }
        xEdit->setText(QString::number(std::min(tiles->first, maxTilesX)));
        yEdit->setText(QString::number(std::min(tiles->second, maxTilesY)));
    });

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
    *resolveAllPtr = [window, rootItem, xEdit, yEdit, tilePx, resolving]() {
        if (*resolving) {
            // Re-entrant call: our own setData() calls below re-fire
            // itemChanged, which is also wired to call this function. The
            // in-flight call already accounts for everything.
            return;
        }
        *resolving = true;

        const long long viewportTilesX = xEdit->text().toInt();
        const long long viewportTilesY = yEdit->text().toInt();
        // Same rounding TileGridWidget itself uses to turn tile counts into
        // actual pixel dimensions at the current display's scale.
        const long long viewportPixelsX = qRound(viewportTilesX * tilePx);
        const long long viewportPixelsY = qRound(viewportTilesY * tilePx);

        const QVector<QTreeWidgetItem*> components = collectComponentItems(rootItem);

        QHash<QString, QTreeWidgetItem*> byName;
        QSet<QString> duplicateNames;
        for (QTreeWidgetItem* item : components) {
            const QString base = item->text(0).mid(requiredPrefixFor(item).length());
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
                if (name == QLatin1String("VIEWPORT_TX")) return viewportTilesX;
                if (name == QLatin1String("VIEWPORT_TY")) return viewportTilesY;
                if (name == QLatin1String("VIEWPORT_PX")) return viewportPixelsX;
                if (name == QLatin1String("VIEWPORT_PY")) return viewportPixelsY;
                return std::nullopt;
            }
            if (duplicateNames.contains(name)) {
                return std::nullopt;
            }
            const auto it = byName.find(name);
            if (it == byName.end()) {
                return std::nullopt;
            }
            // .textSize (prop 4) is the node's text length -- known up front
            // from its stored text content, unlike pos/size (0-3) which are
            // themselves still-resolving expressions -- so it's answered
            // directly rather than via the `resolved` fixed-point map, and
            // only for textboxes (a negspace zone, the only other geometry
            // kind sharing `byName`, carries no text).
            if (prop == 4) {
                return isComponentItem(it.value())
                           ? std::optional<long long>(it.value()->data(0, kTextContentRole).toString().length())
                           : std::nullopt;
            }
            const auto found = resolved.find({it.value(), prop});
            return found != resolved.end() ? std::optional<long long>(found->second) : std::nullopt;
        };

        // `this` is a self-reference to whichever node's own property is
        // currently being evaluated -- the parser has no notion of that (see
        // the ExprNode comment above), so it's resolved here by substituting
        // in that node's real name before delegating to resolveIdent, per
        // pending entry (each may belong to a different node).
        auto resolveIdentFor = [&resolveIdent](QTreeWidgetItem* self) {
            const QString selfName = self->text(0).mid(requiredPrefixFor(self).length());
            return [&resolveIdent, selfName](const QString& name, int prop) {
                return resolveIdent(name == QLatin1String("this") ? selfName : name, prop);
            };
        };

        bool progress = true;
        while (progress && !pending.isEmpty()) {
            progress = false;
            for (int i = pending.size() - 1; i >= 0; --i) {
                const auto v = evalExpr(pending[i].ast, resolveIdentFor(pending[i].item));
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

        QHash<QTreeWidgetItem*, QRect> rects;
        for (QTreeWidgetItem* item : components) {
            long long values[4];
            for (int prop = 0; prop < 4; ++prop) {
                const auto it = resolved.find({item, prop});
                long long value = (it != resolved.end()) ? it->second : kDefaults[prop];
                value = (prop >= 2) ? std::max<long long>(1, value) : std::max<long long>(0, value);
                values[prop] = value;
                if (item->data(0, kRoles[prop]).toLongLong() != value) {
                    item->setData(0, kRoles[prop], static_cast<int>(value));
                }
            }
            rects[item] = QRect(static_cast<int>(values[0]), static_cast<int>(values[1]),
                                 static_cast<int>(values[2]), static_cast<int>(values[3]));
        }

        // Negative-space zones are exclusion zones: any other geometry node
        // whose (now fully resolved) cells overlap one is a violation,
        // reported against both the zone and whatever's intruding on it.
        QHash<QTreeWidgetItem*, QStringList> violationPartners;
        for (QTreeWidgetItem* negItem : components) {
            if (!isNegSpaceItem(negItem)) {
                continue;
            }
            for (QTreeWidgetItem* other : components) {
                if (other == negItem || isNegSpaceItem(other)) {
                    continue;
                }
                if (rects.value(negItem).intersects(rects.value(other))) {
                    violationPartners[negItem] << other->text(0).mid(requiredPrefixFor(other).length());
                    violationPartners[other] << negItem->text(0).mid(requiredPrefixFor(negItem).length());
                }
            }
        }

        QStringList errorNames;
        QStringList violationDescriptions;
        for (QTreeWidgetItem* item : components) {
            const int mask = errorMask.value(item, 0);
            const bool hasExprError = mask != 0;
            const bool hasViolation = violationPartners.contains(item);
            const bool hasProblem = hasExprError || hasViolation;
            // Only touch item-level roles/appearance when the error state
            // actually changed -- setData() unconditionally re-fires
            // itemChanged even when the value is identical, and doing that
            // on every single resolve pass would falsely mark the scene
            // dirty just from redundant no-op writes.
            const bool maskChanged = item->data(0, kErrorMaskRole).toInt() != mask;
            const bool violationChanged = item->data(0, kNegSpaceViolationRole).toBool() != hasViolation;
            if (maskChanged || violationChanged) {
                item->setData(0, kErrorMaskRole, mask);
                item->setData(0, kNegSpaceViolationRole, hasViolation);
                item->setData(0, kErrorRole, hasProblem);
                QFont font = item->font(0);
                font.setBold(hasProblem);
                item->setFont(0, font);
                item->setForeground(0, hasProblem ? QBrush(Qt::red) : QBrush());
                item->setBackground(0, hasProblem ? QBrush(QColor(90, 20, 20)) : QBrush());
                QStringList tooltipLines;
                if (hasExprError) {
                    tooltipLines << QStringLiteral(
                        "Cannot resolve one or more properties -- check for typos, unknown/duplicate names, or a "
                        "dependency cycle.");
                }
                if (hasViolation) {
                    tooltipLines << QStringLiteral("Violated negative space: overlaps %1")
                                        .arg(violationPartners.value(item).join(QStringLiteral(", ")));
                }
                item->setToolTip(0, tooltipLines.join(QStringLiteral("\n")));
            }
            if (hasExprError) {
                errorNames << item->text(0).mid(requiredPrefixFor(item).length());
            }
            // Each violation is symmetric (recorded against both partners),
            // so only report it once, keyed off the negative-space side.
            if (hasViolation && isNegSpaceItem(item)) {
                violationDescriptions << QStringLiteral("%1 overlaps %2")
                                             .arg(item->text(0).mid(requiredPrefixFor(item).length()),
                                                  violationPartners.value(item).join(QStringLiteral(", ")));
            }
        }

        // A red status-bar banner is the loud, hard-to-miss alert a subtle
        // tree-item color change alone wasn't -- it persists (no timeout)
        // until every error is fixed.
        QStringList bannerParts;
        if (!errorNames.isEmpty()) {
            bannerParts << QStringLiteral("Cannot resolve: %1").arg(errorNames.join(QStringLiteral(", ")));
        }
        if (!violationDescriptions.isEmpty()) {
            bannerParts << QStringLiteral("Violated negative space: %1")
                                .arg(violationDescriptions.join(QStringLiteral("; ")));
        }
        if (bannerParts.isEmpty()) {
            window->statusBar()->clearMessage();
            window->statusBar()->setStyleSheet(QString());
        } else {
            window->statusBar()->setStyleSheet(
                QStringLiteral("QStatusBar{background:#7a1f1f;color:white;font-weight:bold;}"));
            window->statusBar()->showMessage(QStringLiteral("⚠ %1").arg(bannerParts.join(QStringLiteral(" | "))));
        }

        *resolving = false;
    };
    (*resolveAllPtr)();

    // All four VIEWPORT_* globals derive from these fields, so anything
    // referencing any of them needs a fresh resolution pass too.
    QObject::connect(xEdit, &QLineEdit::textChanged, [resolveAllPtr](const QString&) { (*resolveAllPtr)(); });
    QObject::connect(yEdit, &QLineEdit::textChanged, [resolveAllPtr](const QString&) { (*resolveAllPtr)(); });

    auto* globalGroup = new QGroupBox("Global", content);
    auto* globalForm = new QFormLayout(globalGroup);
    globalForm->addRow("Viewport X (tx):", xEdit);
    globalForm->addRow("Viewport Y (tx):", yEdit);
    globalForm->addRow("Target:", targetCombo);
    globalForm->addRow("Region:", regionCombo);
    layout->addWidget(globalGroup);

    // "Scene" holds everything that's a property of *this* scene's export
    // (placement attributes + the nametable it targets) rather than of the
    // viewport/Target/Region concepts above -- split out now, ahead of any
    // actual need, so a later per-scene field has an obvious home instead of
    // getting wedged into Global alongside Viewport/Target/Region.
    auto* sceneGroup = new QGroupBox("Scene", content);
    auto* sceneForm = new QFormLayout(sceneGroup);
    sceneForm->addRow("Nametable:", nametableCombo);
    sceneForm->addRow("Linker Prefix:", linkerPrefixEdit);
    sceneForm->addRow("BSS Prefix:", bssPrefixEdit);
    sceneForm->addRow("Data Prefix:", dataPrefixEdit);
    sceneForm->addRow("Default Charmap:", charmapEdit);
    layout->addWidget(sceneGroup);

    // Global/Scene are scene-wide properties, not the selected node's -- they
    // only make sense to show/edit while the root (the scene itself) is
    // selected, same as how the node Properties panel above only shows for a
    // prefixed node.
    auto updateGlobalSceneVisibility = [globalGroup, sceneGroup, rootItem](QTreeWidgetItem* current) {
        const bool onRoot = (current == rootItem);
        globalGroup->setVisible(onRoot);
        sceneGroup->setVisible(onRoot);
    };
    QObject::connect(tree, &QTreeWidget::currentItemChanged,
                      [updateGlobalSceneVisibility](QTreeWidgetItem* current, QTreeWidgetItem*) {
                          updateGlobalSceneVisibility(current);
                      });
    updateGlobalSceneVisibility(tree->currentItem());

    return {content, tree, xEdit, yEdit, targetCombo, regionCombo,
             sceneName, loadSceneFromFile, hasUnresolvedErrors, exportOneTarget,
             doSave, doSaveAs, confirmDiscard, displayName};
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
                                   QLineEdit* yEdit, QComboBox* targetCombo, QComboBox* regionCombo) {
    auto* grid = new TileGridWidget(tilePx, tree, targetCombo, regionCombo);

    // Any change to a component's data (position, size, alignment, text,
    // name) goes through the tree item's setData(), which Qt reports via
    // itemChanged regardless of what triggered it -- so this one connection
    // keeps the canvas in sync with the tree, the properties panel, and
    // dragging on the canvas itself, without each of those needing to know
    // about the grid directly.
    QObject::connect(tree, &QTreeWidget::itemChanged, grid, [grid](QTreeWidgetItem*, int) { grid->update(); });

    // Hide-on-Target/Region is evaluated against whichever Target/Region is
    // *currently* selected, so a node can appear or disappear the moment
    // either combo changes, without anything on the tree itself changing.
    QObject::connect(targetCombo, qOverload<int>(&QComboBox::currentIndexChanged), grid, [grid](int) { grid->update(); });
    QObject::connect(regionCombo, qOverload<int>(&QComboBox::currentIndexChanged), grid, [grid](int) { grid->update(); });

    // Deleting (or otherwise structurally adding/removing) a node doesn't
    // go through setData() at all, so itemChanged alone never fires for it
    // -- without this, a deleted component's drawn cell would keep showing
    // until something unrelated happened to repaint the canvas. QTreeWidget
    // delegates to a real QAbstractItemModel underneath, which does report
    // structural changes regardless of what API triggered them.
    QObject::connect(tree->model(), &QAbstractItemModel::rowsRemoved, grid, [grid] { grid->update(); });
    QObject::connect(tree->model(), &QAbstractItemModel::rowsInserted, grid, [grid] { grid->update(); });
    QObject::connect(tree->model(), &QAbstractItemModel::modelReset, grid, [grid] { grid->update(); });

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

// Owns the tabbed editor: one QTabWidget page (a scene's viewport grid) and
// one QStackedWidget page (that same scene's sidebar content) per open
// scene, added and removed together so their indices always stay aligned --
// no separate bookkeeping needed to keep "tab 2" and "sidebar page 2"
// pointing at the same scene. Everything here is what's genuinely per-scene
// (see createSidebar); the dock/tab-widget chrome itself, and the
// screen-derived sizing every tab shares, is constructed once in main() and
// handed in.
struct TabManager {
    UitkMainWindow* window;
    QTabWidget* tabs;
    QStackedWidget* sidebarStack;
    double tilePx;
    int maxTilesX;
    int maxTilesY;

    std::vector<std::shared_ptr<Sidebar>> sidebars;
    std::vector<std::shared_ptr<ViewportPanel>> viewports;

    Sidebar* active() { return sidebars[tabs->currentIndex()].get(); }

    // Adds a fresh, blank-scene tab, makes it current, and returns it --
    // this *is* what "New" means now that New always opens a new tab rather
    // than resetting the current one in place.
    Sidebar* newTab() {
        // Filled in below, once the Sidebar this callback belongs to
        // actually exists -- createSidebar invokes it once synchronously
        // during construction (before that's possible), which the null
        // check below just no-ops.
        auto sidebarBox = std::make_shared<std::shared_ptr<Sidebar>>();
        Sidebar sidebar = createSidebar(window, maxTilesX, maxTilesY, tilePx, [this, sidebarBox] {
            if (!*sidebarBox) return;
            const int i = sidebarStack->indexOf((*sidebarBox)->content);
            if (i < 0) return;
            tabs->setTabText(i, (*sidebarBox)->displayName());
        });
        auto sidebarPtr = std::make_shared<Sidebar>(sidebar);
        *sidebarBox = sidebarPtr;

        auto viewportPtr = std::make_shared<ViewportPanel>(
            createViewportPanel(window, tilePx, sidebar.tree, sidebar.xEdit, sidebar.yEdit, sidebar.targetCombo,
                                 sidebar.regionCombo));

        // Pushed before either widget is added below, so the currentChanged
        // handler main() wires up (which addTab can trigger synchronously)
        // always finds a matching entry for whatever index it's given.
        sidebars.push_back(sidebarPtr);
        viewports.push_back(viewportPtr);

        sidebarStack->addWidget(sidebarPtr->content);
        const int index = tabs->addTab(viewportPtr->grid, sidebarPtr->displayName());
        tabs->setCurrentIndex(index);
        return sidebarPtr.get();
    }

    // File > Open: always lands in a new tab, never replaces the current
    // one. Closes that tab again on failure rather than leaving a blank
    // scene behind for a file that didn't load.
    void openTab() {
        const QString path = QFileDialog::getOpenFileName(window, "Open Scene", QString(), "UI Scene (*.uis)");
        if (path.isEmpty()) return;
        const int index = tabs->count();
        Sidebar* sidebar = newTab();
        if (!sidebar->loadScene(path)) {
            QMessageBox::warning(window, "Open Failed", "Could not read file:\n" + path);
            removeTabAt(index);
        }
    }

    // Closes tab `index` if it isn't the only one open and its scene isn't
    // dirty (or the user confirms discarding/saving it) -- false either way
    // means nothing was closed. Always keeping at least one tab open avoids
    // an editor with nothing in it to show.
    bool closeTabAt(int index) {
        if (index < 0 || index >= static_cast<int>(sidebars.size())) return false;
        if (sidebars.size() <= 1) return false;
        if (!sidebars[index]->confirmDiscard()) return false;
        removeTabAt(index);
        return true;
    }
    bool closeActive() { return closeTabAt(tabs->currentIndex()); }

    // Run when the window itself is closing -- every open scene gets the
    // same unsaved-changes prompt Close Tab would give it, in tab order,
    // stopping at the first Cancel.
    bool confirmCloseAll() {
        for (const auto& sidebar : sidebars) {
            if (!sidebar->confirmDiscard()) return false;
        }
        return true;
    }

private:
    // The actual removal, shared by closeTabAt (which gates it on
    // confirmDiscard) and openTab's cleanup on a failed load (which doesn't
    // need to, since a scene that never loaded was never dirtied). Signals
    // are blocked around the two widget removals because tabs and
    // sidebarStack briefly disagree on indices mid-removal -- Qt would
    // otherwise fire currentChanged (see main()) with an index that's only
    // valid for one of the two -- and the current tab's sidebar/viewport are
    // instead explicitly re-synced afterward, once both are back in step.
    void removeTabAt(int index) {
        QWidget* grid = viewports[index]->grid;
        QWidget* content = sidebars[index]->content;
        {
            const QSignalBlocker blocker(tabs);
            tabs->removeTab(index);
            sidebarStack->removeWidget(content);
        }
        grid->deleteLater();
        content->deleteLater();
        sidebars.erase(sidebars.begin() + index);
        viewports.erase(viewports.begin() + index);

        const int current = tabs->currentIndex();
        if (current >= 0) {
            sidebarStack->setCurrentIndex(current);
            viewports[current]->sync();
        }
    }
};

// Builds the File menu once, against the real window -- every action
// dispatches through `tabManager.active()` (or adds/removes a tab outright),
// looked up fresh each time it's invoked rather than bound to one scene, so
// it stays correct as the active tab changes.
void createFileMenu(UitkMainWindow* window, TabManager& tabManager) {
    auto* fileMenu = window->menuBar()->addMenu("&File");
    auto addFileAction = [fileMenu](const QString& text, QKeySequence::StandardKey key, auto&& handler) {
        QAction* action = fileMenu->addAction(text);
        action->setShortcut(key);
        QObject::connect(action, &QAction::triggered, handler);
    };
    addFileAction("New", QKeySequence::New, [&tabManager] { tabManager.newTab(); });
    addFileAction("Open...", QKeySequence::Open, [&tabManager] { tabManager.openTab(); });
    fileMenu->addSeparator();
    addFileAction("Save", QKeySequence::Save, [&tabManager] { tabManager.active()->save(); });
    addFileAction("Save As...", QKeySequence::SaveAs, [&tabManager] { tabManager.active()->saveAs(); });
    addFileAction("Close Tab", QKeySequence::Close, [&tabManager] { tabManager.closeActive(); });

    auto showExportFailed = [window] {
        QMessageBox::warning(window, "Export Failed",
                              "Cannot export: one or more components have unresolved properties. "
                              "Fix the errors flagged in the sidebar first.");
    };

    auto doExportTarget = [window, &tabManager, showExportFailed] {
        Sidebar* sidebar = tabManager.active();
        if (sidebar->hasUnresolvedErrors()) {
            showExportFailed();
            return;
        }
        bool ok = false;
        const QString target =
            QInputDialog::getItem(window, "Export Target", "Target:", kTargetNames, 0, false, &ok);
        if (!ok) return;
        const QString dir = QFileDialog::getExistingDirectory(window, "Export Target To (gen/ root)");
        if (dir.isEmpty()) return;
        if (!sidebar->exportOneTarget(dir, target)) {
            QMessageBox::warning(window, "Export Failed", "Could not write generated files to:\n" + dir);
        }
    };

    auto doExportAll = [window, &tabManager, showExportFailed] {
        Sidebar* sidebar = tabManager.active();
        if (sidebar->hasUnresolvedErrors()) {
            showExportFailed();
            return;
        }
        const QString dir = QFileDialog::getExistingDirectory(window, "Export All To (gen/ root)");
        if (dir.isEmpty()) return;
        QStringList failed;
        for (const QString& target : kTargetNames) {
            if (!sidebar->exportOneTarget(dir, target)) failed << target;
        }
        if (!failed.isEmpty()) {
            QMessageBox::warning(window, "Export Failed",
                                  "Could not write generated files for:\n" + failed.join(", "));
        }
    };

    fileMenu->addSeparator();
    QObject::connect(fileMenu->addAction("Export Target..."), &QAction::triggered, doExportTarget);
    QObject::connect(fileMenu->addAction("Export All..."), &QAction::triggered, doExportAll);
}

// Matches `target` against kTargetNames case-insensitively (a CLI caller
// shouldn't have to get "NES" vs "nes" exactly right) and returns the
// canonically-cased entry -- what exportOneTarget's own Target-name
// comparisons (and its lower-cased output directory) expect. Empty if
// `target` doesn't match any known target.
QString canonicalTargetName(const QString& target) {
    for (const QString& t : kTargetNames) {
        if (t.compare(target, Qt::CaseInsensitive) == 0) return t;
    }
    return QString();
}

// Headless -e/-E CLI export path. Builds the exact same sidebar
// (tree/combos/resolver) the interactive GUI does -- so a CLI export always
// produces byte-identical output to what File > Export would write for the
// same .uis file -- but never shows the window or enters the event loop.
// `outputPath` is the gen/ root; exportOneTarget (see createSidebar) writes
// each target's pair under `outputPath/<target-lower>/<sceneName>.hpp|.cpp`.
int runCliExport(int argc, char** argv, bool exportAll, const QString& inputPath, const QString& outputPath,
                  const QString& target) {
    QApplication app(argc, argv);

    UitkMainWindow window;
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) {
        screen = app.primaryScreen();
    }
    const double tilePx = tilePxForScreen(screen->geometry());
    const int sidebarWidthPx = sidebarWidthForScreen(screen->geometry());
    const QRect available = screen->availableGeometry();
    const int maxTilesX = std::max(
        1, static_cast<int>((available.width() - sidebarWidthPx - kWindowChromeMarginPx) / tilePx));
    const int maxTilesY =
        std::max(1, static_cast<int>((available.height() - kWindowChromeMarginPx) / tilePx));

    const Sidebar sidebar = createSidebar(&window, maxTilesX, maxTilesY, tilePx, [] {});

    if (!sidebar.loadScene(inputPath)) {
        std::fprintf(stderr, "uitk: could not read scene file: %s\n", qPrintable(inputPath));
        return 1;
    }
    if (sidebar.hasUnresolvedErrors()) {
        std::fprintf(stderr,
                      "uitk: cannot export %s: one or more components have unresolved properties\n",
                      qPrintable(inputPath));
        return 1;
    }

    std::fprintf(stdout, "uitk: exporting %s (scene \"%s\") to %s\n", qPrintable(inputPath),
                 qPrintable(sidebar.sceneName()), qPrintable(outputPath));

    if (exportAll) {
        QStringList failed;
        for (const QString& t : kTargetNames) {
            std::fprintf(stdout, "%s:\n", qPrintable(t));
            if (!sidebar.exportOneTarget(outputPath, t)) failed << t;
        }
        if (!failed.isEmpty()) {
            std::fprintf(stderr, "uitk: could not write generated files for: %s\n",
                          qPrintable(failed.join(", ")));
            return 1;
        }
        std::fprintf(stdout, "uitk: exported %d target(s) successfully\n", static_cast<int>(kTargetNames.size()));
        return 0;
    }

    const QString canonicalTarget = canonicalTargetName(target);
    if (canonicalTarget.isEmpty()) {
        std::fprintf(stderr, "uitk: unknown target \"%s\" -- expected one of: %s\n", qPrintable(target),
                      qPrintable(kTargetNames.join(", ")));
        return 1;
    }
    std::fprintf(stdout, "%s:\n", qPrintable(canonicalTarget));
    if (!sidebar.exportOneTarget(outputPath, canonicalTarget)) {
        std::fprintf(stderr, "uitk: could not write generated files to: %s\n", qPrintable(outputPath));
        return 1;
    }
    std::fprintf(stdout, "uitk: export complete\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // -e/--export and -E/--export-all switch into the headless CLI export
    // path (runCliExport) instead of the normal GUI -- parsed by hand, ahead
    // of QApplication ever touching argv, so this works from a script or CI
    // job with no display server at all (QT_QPA_PLATFORM is forced to
    // "offscreen" below unless the caller already set it).
    //
    //   -e, --export       export a single target (requires -t/--target)
    //   -E, --export-all   export every target kTargetNames lists
    //   -i, --input        the .uis scene file to load
    //   -o, --output       output root; writes <output>/<target>/<name>.hpp|.cpp
    //   -t, --target       target to export (only with -e/--export)
    bool cliExport = false;
    bool cliExportAll = false;
    QString cliInput;
    QString cliOutput;
    QString cliTarget;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const bool wantsValue = (arg == "-i" || arg == "--input" || arg == "-o" || arg == "--output" ||
                                  arg == "-t" || arg == "--target");
        if (wantsValue && i + 1 >= argc) {
            std::fprintf(stderr, "uitk: %s requires an argument\n", qPrintable(arg));
            return 1;
        }
        if (arg == "-e" || arg == "--export") {
            cliExport = true;
        } else if (arg == "-E" || arg == "--export-all") {
            cliExportAll = true;
        } else if (arg == "-i" || arg == "--input") {
            cliInput = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "-o" || arg == "--output") {
            cliOutput = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "-t" || arg == "--target") {
            cliTarget = QString::fromLocal8Bit(argv[++i]);
        }
    }

    if (cliExport || cliExportAll) {
        if (cliExport && cliExportAll) {
            std::fprintf(stderr, "uitk: -e/--export and -E/--export-all are mutually exclusive\n");
            return 1;
        }
        if (cliInput.isEmpty()) {
            std::fprintf(stderr, "uitk: -e/-E requires -i/--input <scene.uis>\n");
            return 1;
        }
        if (cliOutput.isEmpty()) {
            std::fprintf(stderr, "uitk: -e/-E requires -o/--output <output dir>\n");
            return 1;
        }
        if (cliExport && cliTarget.isEmpty()) {
            std::fprintf(stderr, "uitk: -e/--export requires -t/--target <target> "
                                  "(use -E/--export-all to export every target instead)\n");
            return 1;
        }
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        return runCliExport(argc, argv, cliExportAll, cliInput, cliOutput, cliTarget);
    }

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

    // The dock is created once, shared by every scene tab -- its content is
    // a QStackedWidget holding each tab's sidebar, index-aligned with the
    // central QTabWidget (see TabManager). NoDockWidgetFeatures means
    // there's no user-facing way to close/hide it, so any time it goes
    // invisible it's a Qt layout glitch, not a legitimate state -- most
    // commonly triggered by the window resizes in lockWindowToContents(),
    // which can transiently unmap the dock via a *deferred* Qt layout
    // event. Reacting synchronously to that (as lockWindowToContents also
    // tries) can lose the race against the deferred hide; queuing the
    // re-show instead runs it after any pending layout events have already
    // fired, so it always wins.
    auto* sidebarDock = new QDockWidget("Sidebar", &window);
    sidebarDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    sidebarDock->setFixedWidth(sidebarWidthPx);
    QObject::connect(sidebarDock, &QDockWidget::visibilityChanged, sidebarDock, [sidebarDock](bool visible) {
        if (!visible) {
            QTimer::singleShot(0, sidebarDock, [sidebarDock] { sidebarDock->setVisible(true); });
        }
    });
    auto* sidebarStack = new QStackedWidget(sidebarDock);
    sidebarDock->setWidget(sidebarStack);

    auto* tabs = new QTabWidget(&window);
    tabs->setTabsClosable(true);
    window.setCentralWidget(tabs);
    window.addDockWidget(Qt::RightDockWidgetArea, sidebarDock);

    TabManager tabManager{&window, tabs, sidebarStack, tilePx, maxTilesX, maxTilesY};
    createFileMenu(&window, tabManager);
    window.confirmClose = [&tabManager] { return tabManager.confirmCloseAll(); };

    // The sidebar's required height isn't fixed -- the properties panel
    // appears/disappears with selection, and nodes get added to the tree --
    // so the window (locked to a fixed size elsewhere) needs to be re-fit
    // any time that happens, not just when the viewport's tile counts (or
    // the active tab) change.
    window.installEventFilter(new LayoutChangeNotifier([&window] { lockWindowToContents(&window); }, &window));

    // Switching tabs shows that scene's sidebar and re-fits the window to
    // its viewport size, same as changing Viewport X/Y already does for the
    // scene that's active.
    QObject::connect(tabs, &QTabWidget::currentChanged, &window, [&tabManager](int index) {
        if (index < 0) return;
        tabManager.sidebarStack->setCurrentIndex(index);
        tabManager.viewports[index]->sync();
    });
    QObject::connect(tabs, &QTabWidget::tabCloseRequested, &window,
                      [&tabManager](int index) { tabManager.closeTabAt(index); });

    tabManager.newTab();

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
