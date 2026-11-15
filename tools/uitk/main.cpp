// 'pnes' UI Toolkit

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
#include <array>
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

// Set by -n/--name on the CLI; empty in the GUI, which always follows the
// open .uis file's own basename. It overrides the exported scene name, which
// is BOTH the `gen::<scene>` namespace and the written .hpp/.cpp basename
// (see exportOneTarget / generateCode). That lets a per-target variant file
// -- demo/ui/<scene>_ds.uis, overlaid onto the dual-screen targets by the
// uitk-export step in CMakeLists.txt -- export AS <scene>, so it lands at
// demo/gen/<scene>/{ds,dsi,3ds}/<scene>.hpp alongside the other targets'
// output and demo/src keeps its one unchanged
// `#include STRCAT(../../gen/<scene>/GEN_TARGET_DIR/<scene>.hpp)`.
QString g_sceneNameOverride;

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
// charmap (see technology.hpp's tech::nes_str::charmap) for this node's row
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
// Button-only: whether Enable_<Name>/Disable_<Name> (and, indirectly,
// Draw_<Name>) start the scene with the button enabled or disabled -- seeds
// the generated <Name>_enabled flag's initializer.
constexpr int kButtonDefaultEnabledRole = Qt::UserRole + 21;
// Button-only: whether a Toggle_<Name>() is emitted alongside Enable_/
// Disable_ -- not every button needs one (e.g. a plain confirm button is
// only ever Enabled or Disabled from outside, never toggled by itself).
constexpr int kButtonToggleableRole = Qt::UserRole + 23;
// Button-only: whether `<Name>_box` (the vec2<u8> footprint) is emitted at
// all -- a button whose Draw()/etc. implementation
// doesn't need to know its own dimensions (e.g. a fixed-size sprite, or one
// that only ever cares about `<Name>_pos`) has no use for it, so it isn't
// forced on every button the way pos is.
constexpr int kButtonHasBoxRole = Qt::UserRole + 24;
// Canvas-only: which axis moves its focus. Vertical (the default) steps the
// focused index on UP/DOWN and hands everything else to the focused
// component; horizontal steps on LEFT/RIGHT instead. Either way the OTHER
// axis falls through untouched, which is what lets a Canvas hold a Slider or
// a SingleChoice: the component just has to step on the axis the Canvas
// doesn't (see kSliderVerticalRole/kSingleChoiceVerticalRole, genCanvasDecl).
constexpr int kCanvasVerticalRole = Qt::UserRole + 25;
// SingleChoice-only, and the same idea as kCanvasVerticalRole: which axis
// steps its selected option. Vertical (the default) steps on UP/DOWN, a
// stacked list of options; horizontal steps on LEFT/RIGHT, a row of them.
// It is also what lets a SingleChoice sit inside a Canvas -- the two have to
// disagree about the axis for both to be reachable (see canvasComponents).
constexpr int kSingleChoiceVerticalRole = Qt::UserRole + 26;
// SingleChoice-only: whether the derived `rect` covering every option it shows
// is emitted at all (see genSingleChoiceDecl). Off by default, because it is
// not free on a runtime-viewport target -- there it is six bytes of mutable
// storage plus a fold in Make() -- and a menu that nothing measures has no use
// for it. Same arrangement as a Button's kButtonHasBoxRole. Forced on, whatever
// this says, when the menu is a Canvas component: the Canvas's table is built
// out of that rect, so there it isn't a choice (see isCanvasComponent).
constexpr int kSingleChoiceHasRectRole = Qt::UserRole + 27;
// Slider-only, and the same idea as kCanvasVerticalRole: which axis moves its
// handle. Horizontal (the default) steps `amount` on LEFT/RIGHT, a track laid
// along a nametable row; vertical steps on UP/DOWN, a track laid down a
// column. It is what lets a Slider sit inside a Canvas -- the two have to
// disagree about the axis for the handle to be reachable at all (see
// canvasComponents), so a horizontal Canvas wants a vertical Slider.
//
// Defaults to HORIZONTAL, unlike the two roles above: a slider's track is a
// row in every scene written before this existed, and a default of vertical
// would silently re-axis every one of them.
//
// uitk emits no range for a slider (its track length IS its range, see
// genSliderDecl), so this decides which input bits its Pass() spends and is
// also published as a `constexpr bool vertical` in the node's namespace.
// Which dimension of `rect` bounds the handle, and how a cell address steps,
// stay the scene's business -- a vertical slider's Draw()/DrawChange() read
// `size.y` and walk a row at a time where a horizontal one reads `size.x`,
// branching on that constant rather than on a literal of their own.
constexpr int kSliderVerticalRole = Qt::UserRole + 28;
// Compile-time and runtime textboxes are separate kinds -- currently
// identical in behavior, but distinguished now because they'll diverge in
// meaning later (e.g. how their text is ultimately resolved by generated
// code).
constexpr char kCtTextBoxKind[] = "ctTextBox";
constexpr char kRtTextBoxKind[] = "rtTextBox";
constexpr char kNegSpaceKind[] = "negspace";
constexpr char kSingleChoiceKind[] = "singlechoice";
constexpr char kButtonKind[] = "button";
// Like a Button, geometry with no baked appearance -- and, like a Button,
// uitk owns exactly one piece of state for it: a `<name>_amount` byte
// holding the slider's current value, where a Button instead gets a bit in
// the shared enabled mask. Codegen emits that, where it is, how big it is,
// and the forced-to-define Draw_<Name>/DrawChange_<Name> hooks that render
// it -- nothing more. No range or step: those belong to whatever the slider
// controls (see genSliderDecl).
constexpr char kSliderKind[] = "slider";
// A pure grouping node like SingleChoice -- no geometry, no appearance -- but
// where a SingleChoice groups textboxes into one value, a Canvas groups
// INTERACTABLE nodes (toggleable Buttons, Sliders) into one focus ring, so a
// menu can hand a whole screenful of controls input through a single Pass()
// and let the Canvas decide which control it reaches. The only state it owns
// is the focused index (see genCanvasDecl).
constexpr char kCanvasKind[] = "canvas";
// Geometry and nothing else: a named position and footprint, and the Make()
// that resolves them -- no appearance, no state, no Pass(), not even a hook to
// define. It is the Rect every other placeable kind already carries, offered on
// its own, for an area the scene wants named and resolved per target without
// uitk having an opinion about what lives there: a panel hand-written code
// clears, a region a sprite is clamped to, an anchor other nodes write their
// own placement against.
//
// Deliberately NOT a negative space zone, which is also pure geometry: that one
// MEANS something ("nothing may overlap this", and uitk flags whatever does).
// A Rect means nothing at all, which is the point -- and is why it is a kind of
// its own rather than a flag on that one.
constexpr char kRectKind[] = "rect";
constexpr char kCtTextBoxPrefix[] = "[CT] ";
constexpr char kRtTextBoxPrefix[] = "[RT] ";
constexpr char kNegSpacePrefix[] = "[N] ";
constexpr char kSingleChoicePrefix[] = "[SC] ";
constexpr char kButtonPrefix[] = "[B] ";
constexpr char kSliderPrefix[] = "[S] ";
constexpr char kCanvasPrefix[] = "[C] ";
constexpr char kRectPrefix[] = "[R] ";

// Every backend pnes currently supports, in the order they appear in
// the Target dropdown. Scene-wide, same as the quadrant -- one scene targets
// one platform at a time. Kept as display names (matched by text, not index)
// so the .uis format doesn't depend on this list's order.
const QStringList kTargetNames = {"NES", "GBA", "GCN", "DS",     "DSI", "Wii",
                                   "Wii U", "3DS", "Switch", "PSP",  "PC"};

// TV broadcast standard the scene is authored/timed against. Same idea as
// kTargetNames -- scene-wide, matched by name rather than index.
const QStringList kRegionNames = {"NTSC", "PAL"};

// Fixed viewport tile counts for each target, mirroring the
// video::viewport_tx()/viewport_ty() constants each backend actually compiles
// against (include/pnes/video.hpp) -- picking a Target sets the
// sidebar's Viewport X/Y to whatever that backend's panel really shows, so a
// scene isn't authored against the wrong resolution by accident.
//
// On the dual-screen handhelds a target's viewport is the BOUNDING BOX of both
// its panels, not one panel: the width of the widest and the summed height of
// the two, since they are stacked on a shared horizontal edge (see video.hpp's
// TARGET_NDS/TARGET_CTR branches). So DS/DSi author against 32x48 (two 256x192
// panels = 256x384) and the 3DS against 50x60 (a 400x240 top over a 320x240
// bottom = 400x480). Note the 3DS's bottom panel is 40px/5 tiles NARROWER on
// each side than the viewport: scene columns 0-4 and 45-49 below row 30 land in
// the bezel beside it, not on glass. The hinge gap between the panels is not
// modelled, so scene row 23/24 (DS) and 29/30 (3DS) are adjacent here but are
// physically separated by the bezel on hardware -- keep anything that must read
// as one object off those seams.
//
// Two targets have no fixed panel size and are deliberately left out: GC/Wii
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
        {"DS", {32, 48}},     // both 256x192 panels stacked: 256x384
        {"DSI", {32, 48}},    // same two panels as DS
        {"Wii", {40, 30}},    // same runtime-TV path as GCN
        {"Wii U", {52, 30}},  // widescreen, scaled to fill the panel
        {"3DS", {50, 60}},    // 400x240 top over 320x240 bottom: 400x480
        {"Switch", {52, 30}}, // widescreen, scaled to fill the panel
        {"PSP", {60, 30}},    // 480x272 panel, letterboxed to 480x240
    };
    const auto it = kSizes.constFind(target);
    if (it == kSizes.constEnd()) return std::nullopt;
    return *it;
}

// Panel 1 -- the LOWER panel -- as a rect in TILES, mirroring
// video::panel_rect(1) (include/pnes/video.hpp) for the dual-screen targets.
// This is what PANEL1_X/Y/TX/TY resolve to, and it is why one *_ds.uis can
// serve both consoles: the 3DS's bottom panel is 40 tiles wide inset 5 tiles
// from the viewport's left edge, NOT the viewport's own 50, so a layout
// anchored to VIEWPORT_TX would overflow into the bezel there while looking
// fine on the DS.
//
// Targets absent from this table are single-screen, where video::panel_rect()
// ignores its index and hands back the whole viewport; PANEL1_* follows that
// and falls back to the viewport, so a scene authored against PANEL1_* still
// exports sensibly for them rather than failing to resolve.
struct PanelTiles {
    int x, y, tx, ty;
};

std::optional<PanelTiles> panel1TilesForTarget(const QString& target) {
    static const QHash<QString, PanelTiles> kPanels{
        {"DS", {0, 24, 32, 24}},    // 256x192 under a 256x192 top, in a 32x48 viewport
        {"DSI", {0, 24, 32, 24}},   // same two panels as DS
        {"3DS", {5, 30, 40, 30}},   // 320x240 centred under a 400x240 top, in a 50x60 viewport
    };
    const auto it = kPanels.constFind(target);
    if (it == kPanels.constEnd()) return std::nullopt;
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

// Position and size, and nothing else -- see kRectKind. Every other geometry
// kind here is this plus something (text, a Draw() hook, a value byte, an
// exclusion rule); this one is the floor they all stand on, exported on its own.
bool isRectItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kRectKind);
}

// A pure grouping node -- no geometry of its own, just a parent for the
// ctTextBox/rtTextBox nodes representing its options. Not a geometry item:
// it's never drawn, dragged, or resolved, same as root/plain branch nodes.
bool isSingleChoiceItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kSingleChoiceKind);
}

// Has its own position/size, like a textbox, but no baked appearance:
// uitk bakes no appearance for one, only the forced-to-define Draw() hook
// hand-written code must implement, which dispatches on the generated
// Enabled() itself -- a button could be a tickbox, a wide labeled box, or a
// sprite, so there's nothing uitk could bake that would be right for all of
// them (see genButtonDecl). Enabled/disabled is the only state uitk offers memory
// for -- there is no selected/unselected concept of its own.
bool isButtonItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kButtonKind);
}

// Position, size, and a single byte of value -- that's a slider. uitk bakes
// no appearance for one (a slider could be a tile-row track, a sprite bar,
// or a row of discrete notches), only the forced-to-define Draw_<Name>/
// DrawChange_<Name> hooks hand-written code must implement, the same
// arrangement a Button has with its own Draw(). Its state is
// `<name>_amount`, which uitk allocates but never bounds or writes (see
// genSliderDecl).
bool isSliderItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kSliderKind);
}

// A pure grouping node, like SingleChoice -- it has no geometry of its own
// either. What it groups is interactable nodes: the toggleable Buttons and
// Sliders nested under it become its components[] table, and its Pass() moves
// a focused index over them and forwards input to whichever one has focus.
// Children that have no Pass() of their own (a label, a non-toggleable button)
// still get their namespaces and geometry as they would anywhere else -- they
// just cannot take focus (see canvasComponents).
bool isCanvasItem(const QTreeWidgetItem* item) {
    return item && item->data(0, kKindRole).toString() == QLatin1String(kCanvasKind);
}

// Anything with a position/size -- placeable on the canvas, draggable,
// referenceable in another node's expressions -- regardless of what kind of
// thing it visually is.
bool isGeometryItem(const QTreeWidgetItem* item) {
    return isComponentItem(item) || isNegSpaceItem(item) || isButtonItem(item) || isSliderItem(item) ||
           isRectItem(item);
}

// Anything that carries a kind-specific name prefix -- every geometry item,
// plus SingleChoice, which has no geometry of its own but is still a genuine
// node (as opposed to root/plain branch nodes, which have no prefix at all).
bool isPrefixedItem(const QTreeWidgetItem* item) {
    return isGeometryItem(item) || isSingleChoiceItem(item) || isCanvasItem(item);
}

// The "[CT] "/"[RT] "/"[N] "/"[SC] "/"[B] "/"[S] "/"[C] "/"[R] " prefix a node's kind
// requires, or empty for kinds (root, plain branch nodes) that don't have one.
QString requiredPrefixFor(const QTreeWidgetItem* item) {
    if (isCtTextBoxItem(item)) return QString(kCtTextBoxPrefix);
    if (isRtTextBoxItem(item)) return QString(kRtTextBoxPrefix);
    if (isNegSpaceItem(item)) return QString(kNegSpacePrefix);
    if (isSingleChoiceItem(item)) return QString(kSingleChoicePrefix);
    if (isButtonItem(item)) return QString(kButtonPrefix);
    if (isSliderItem(item)) return QString(kSliderPrefix);
    if (isCanvasItem(item)) return QString(kCanvasPrefix);
    if (isRectItem(item)) return QString(kRectPrefix);
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
// items plus SingleChoice and Canvas) -- used for name-uniqueness checks,
// since a grouping node's name has to be unique against everything else that
// carries a prefix, not just against other geometry.
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

// Whether this node has a generated Pass(u8), and a rect to go with it, and so
// can be a Canvas component:
//
//  - a Slider, always;
//  - a Button, only when it is toggleable -- a non-toggleable one has no press
//    uitk could define for it, so it has no Pass() (see genButtonDecl);
//  - a SingleChoice, as long as this target shows at least one of its options.
//    It has no authored geometry, but it does have a derived `rect` covering
//    the options it shows (see genSingleChoiceDecl), which is what a Canvas
//    needs of it.
//
// A Slider and a SingleChoice each step an axis of their own (see
// kSliderVerticalRole/kSingleChoiceVerticalRole), so for both it and the
// Canvas to be reachable the two have to disagree about it: a vertical Canvas
// spends UP/DOWN on focus and hands LEFT/RIGHT to a horizontal slider or menu,
// a horizontal Canvas does the reverse. Agreeing is allowed -- the component is
// still focusable, still takes A/B, and uitk has no business overruling a
// layout -- it just means the Canvas wins the axis and that component cannot be
// adjusted while it is nested.
//
// Deliberately NOT a nested Canvas, which has a Pass() but no rect of its own,
// and would need focus to mean two things at once.
bool hasGeneratedPass(const QTreeWidgetItem* item, const QString& target, const QString& region) {
    if (isSliderItem(item)) return true;
    if (isButtonItem(item)) return item->data(0, kButtonToggleableRole).toBool();
    return isSingleChoiceItem(item) && !singleChoiceMembers(item, target, region).isEmpty();
}

// Whether this node is a component of some Canvas -- i.e. whether a Canvas's
// table will name it. The nearest PREFIXED ancestor is the one that decides:
// anything in between is a transparent branch node, which codegen flattens
// away (see childNamespaceNames), so a node inside one still belongs to the
// Canvas above it. Doesn't check hidden-ness: a hidden Canvas hides everything
// under it (isEffectivelyHidden walks up), so a visible node under a Canvas is
// a node under a visible Canvas.
bool isCanvasComponent(const QTreeWidgetItem* item, const QString& target, const QString& region) {
    const QTreeWidgetItem* parent = item ? item->parent() : nullptr;
    while (parent && !isPrefixedItem(parent)) parent = parent->parent();
    return parent && isCanvasItem(parent) && hasGeneratedPass(item, target, region);
}

// A Canvas's components, in tree order: the nodes nested under it that have a
// Pass() of their own, and so can take its focus (see hasGeneratedPass).
// Everything else under a Canvas -- a label, a non-toggleable button -- keeps
// its namespace and geometry and simply isn't focusable; a Canvas is a focus
// ring, not a container that changes what its children are.
//
// Descends through non-prefixed branch nodes for the same reason codegen does
// (they're transparent, see childNamespaceNames), but stops at any prefixed
// node it takes: a Button's own label children are the Button's business, not
// the Canvas's. Hidden-for-target/region nodes are dropped, same as
// everywhere else, so a Canvas never indexes a component that isn't on the
// exported target -- which also means the same scene can have a different
// component count per target, and that is fine: nothing outside the generated
// namespace depends on the indices.
QVector<QTreeWidgetItem*> canvasComponents(const QTreeWidgetItem* canvasItem, const QString& target,
                                            const QString& region) {
    QVector<QTreeWidgetItem*> result;
    std::function<void(const QTreeWidgetItem*)> visit = [&](const QTreeWidgetItem* n) {
        for (int i = 0; i < n->childCount(); ++i) {
            QTreeWidgetItem* child = n->child(i);
            if (!isPrefixedItem(child)) {
                visit(child);  // transparent branch
                continue;
            }
            if (isEffectivelyHidden(child, target, region)) continue;
            if (hasGeneratedPass(child, target, region)) result.push_back(child);
        }
    };
    visit(canvasItem);
    return result;
}

// --- Property expressions -----------------------------------------------
//
// A property's stored text is a small arithmetic expression over integer
// literals, +/-/*//, parentheses, and identifiers of the form
// `NodeName.pos.x`, `NodeName.pos.y`, `NodeName.size.x`, `NodeName.size.y`,
// `NodeName.textSize`, `NodeName.enabled` (referencing another component by
// its bare name, sans the "[CT] "/"[RT] " prefix -- the same name that will
// identify it in generated C++), plus the two bare globals
// VIEWPORT_TX/VIEWPORT_TY (the viewport's configured size in tiles) and
// VIEWPORT_PX/VIEWPORT_PY (the same, in pixels, at the current display's tile
// scale). `this` is accepted in place of a name anywhere one of the above is
// legal (e.g. `this.textSize`, `this.pos.x`) as a self-reference to whichever
// node the expression being evaluated belongs to -- handled by substitution
// in resolveAllPtr's resolveIdent, not here, since the parser has no notion
// of which node's property it's parsing for. `.textSize` is the referenced
// textbox's text length in characters (its *unwrapped* single-line width) --
// e.g. `sizeWExpr: "this.textSize"` auto-fits a box to its own label, or
// `posXExpr: "(VIEWPORT_TX - this.textSize) / 2"` centers one. It resolves
// only against ctTextBox/rtTextBox nodes, same as pos/size, since only they
// carry text. `.enabled` is 1 if the referenced node is currently visible on
// the sidebar's selected Target/Region (0 if it, or an ancestor, hides on
// either -- see isEffectivelyHidden), so a sibling can conditionally shift
// its own placement around whatever this scene won't actually draw there --
// e.g. `posYExpr: "Other.pos.y + Other.enabled * 8"` sits directly below
// Other only where Other itself is shown. Node names are therefore
// constrained to be valid C++ identifiers and unique, since they're both the
// expression namespace here and the symbol that later code generation will
// emit.
struct ExprNode {
    enum class Kind { Number, Ident, Add, Sub, Mul, Div, Shl, Shr, Neg };
    Kind kind = Kind::Number;
    long long number = 0;
    QString identName;  // component base name, "this", or "VIEWPORT_{T,P}{X,Y}"
    // 0=pos.x, 1=pos.y, 2=size.x, 3=size.y, 4=textSize, 5=enabled; -1 for the
    // bare globals
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
                // PANEL1_* are the LOWER panel's rect in tiles on a
                // dual-screen target (see panel1TilesForTarget) -- the anchor
                // a *_ds.uis layout uses so it lands on the bottom screen of
                // both the DS and the 3DS from one file.
                static const QSet<QString> kViewportIdents{"VIEWPORT_TX", "VIEWPORT_TY", "VIEWPORT_PX",
                                                             "VIEWPORT_PY", "PANEL1_X",  "PANEL1_Y",
                                                             "PANEL1_TX",   "PANEL1_TY"};
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
            if (parts.size() == 2 && parts.at(1) == QLatin1String("enabled")) {
                node->identName = parts.at(0);
                node->identProp = 5;
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
// textSize, or `this` -- is baked to the already-resolved int `resolveIdent`
// hands back, same as a fixed-panel target bakes everything -- EXCEPT where
// `substituteIdent` returns an expression for it: a reference to another
// node's *position* is only "not runtime-variable" if that node's own
// position isn't itself VIEWPORT_*-driven, so the caller passes a hook that
// splices the referenced node's own expression in instead (see
// emitGeomComponent). Baking it would leave the referrer at whatever column
// the editor's viewport happened to resolve to, misaligned from the node it
// is meant to follow at any other runtime viewport size.
QString emitExprCpp(const ExprPtr& node, const std::function<long long(const QString&, int)>& resolveIdent,
                     const std::function<std::optional<QString>(const QString&, int)>& substituteIdent = nullptr) {
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
                if (node->identName == QLatin1String("PANEL1_X"))
                    return QStringLiteral("static_cast<u16>(video::panel_rect(1).x >> 3)");
                if (node->identName == QLatin1String("PANEL1_Y"))
                    return QStringLiteral("static_cast<u16>(video::panel_rect(1).y >> 3)");
                if (node->identName == QLatin1String("PANEL1_TX")) return QStringLiteral("video::panel_tx(1)");
                if (node->identName == QLatin1String("PANEL1_TY")) return QStringLiteral("video::panel_ty(1)");
            }
            if (substituteIdent) {
                if (const auto sub = substituteIdent(node->identName, node->identProp)) return *sub;
            }
            return QString::number(resolveIdent(node->identName, node->identProp));
        case ExprNode::Kind::Neg:
            return QString("-(%1)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent));
        case ExprNode::Kind::Add:
            return QString("(%1 + %2)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent),
                                            emitExprCpp(node->b, resolveIdent, substituteIdent));
        case ExprNode::Kind::Sub:
            return QString("(%1 - %2)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent),
                                            emitExprCpp(node->b, resolveIdent, substituteIdent));
        case ExprNode::Kind::Mul:
            return QString("(%1 * %2)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent),
                                            emitExprCpp(node->b, resolveIdent, substituteIdent));
        case ExprNode::Kind::Div:
            return QString("(%1 / %2)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent),
                                            emitExprCpp(node->b, resolveIdent, substituteIdent));
        case ExprNode::Kind::Shl:
            return QString("(%1 << %2)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent),
                                             emitExprCpp(node->b, resolveIdent, substituteIdent));
        case ExprNode::Kind::Shr:
            return QString("(%1 >> %2)").arg(emitExprCpp(node->a, resolveIdent, substituteIdent),
                                             emitExprCpp(node->b, resolveIdent, substituteIdent));
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
        if (isButtonItem(item)) {
            obj["defaultEnabled"] = item->data(0, kButtonDefaultEnabledRole).toBool();
            obj["toggleable"] = item->data(0, kButtonToggleableRole).toBool();
            obj["hasBox"] = item->data(0, kButtonHasBoxRole).toBool();
        }
        if (isSliderItem(item)) {
            obj["vertical"] = item->data(0, kSliderVerticalRole).toBool();
        }
    } else if (isSingleChoiceItem(item)) {
        obj["kind"] = QStringLiteral("singlechoice");
        obj["defaultOption"] = item->data(0, kDefaultOptionRole).toInt();
        obj["vertical"] = item->data(0, kSingleChoiceVerticalRole).toBool();
        obj["provideBounds"] = item->data(0, kSingleChoiceHasRectRole).toBool();
    } else if (isCanvasItem(item)) {
        obj["kind"] = QStringLiteral("canvas");
        obj["vertical"] = item->data(0, kCanvasVerticalRole).toBool();
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
    const bool isButton = (kind == QLatin1String("button"));
    const bool isSlider = (kind == QLatin1String("slider"));
    // Nothing kind-specific to read back for a Rect -- the geometry block below
    // is the whole of it (see kRectKind).
    const bool isRect = (kind == QLatin1String("rect"));
    if (isComponent || isNegSpace || isButton || isSlider || isRect) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kKindRole,
                       QString(isCtTextBox   ? kCtTextBoxKind
                               : isRtTextBox ? kRtTextBoxKind
                               : isNegSpace  ? kNegSpaceKind
                               : isButton    ? kButtonKind
                               : isRect      ? kRectKind
                                             : kSliderKind));
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
        if (isButton) {
            item->setData(0, kButtonDefaultEnabledRole, obj["defaultEnabled"].toBool(true));
            item->setData(0, kButtonToggleableRole, obj["toggleable"].toBool(false));
            item->setData(0, kButtonHasBoxRole, obj["hasBox"].toBool(true));
        }
        if (isSlider) {
            // Defaults to HORIZONTAL, the axis every slider written before
            // this key existed was generated with -- the opposite default to
            // a SingleChoice's or a Canvas's own `vertical` (see
            // kSliderVerticalRole).
            item->setData(0, kSliderVerticalRole, obj["vertical"].toBool(false));
        }
        item->setData(0, kLastValidNameRole, item->text(0));
    } else if (kind == QLatin1String("singlechoice")) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kKindRole, QString(kSingleChoiceKind));
        item->setData(0, kDefaultOptionRole, obj["defaultOption"].toInt(0));
        // Defaults to vertical, which is the axis every SingleChoice written
        // before this key existed was generated with.
        item->setData(0, kSingleChoiceVerticalRole, obj["vertical"].toBool(true));
        // Off by default: a scene written before this key existed was
        // generated without a rect at all, so absent means off (see
        // kSingleChoiceHasRectRole).
        item->setData(0, kSingleChoiceHasRectRole, obj["provideBounds"].toBool(false));
        item->setData(0, kLastValidNameRole, item->text(0));
    } else if (kind == QLatin1String("canvas")) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kKindRole, QString(kCanvasKind));
        // Defaults to vertical, matching addCanvasNode -- a scene written
        // before this key existed, or by hand without it, gets the layout
        // that can still hold a Slider (see kCanvasVerticalRole).
        item->setData(0, kCanvasVerticalRole, obj["vertical"].toBool(true));
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

// `quadrant`, `target`, `region`, and `linker_prefix` are all scene-wide
// (which quadrant of the 2x2 world grid the whole UI's tile positions resolve into,
// which pnes backend the scene targets, which TV broadcast standard
// it's timed against, and what codegen should emit ahead of every generated,
// non-AI-marked function definition), not per-node properties, so each is
// stored once at the document's top level alongside "version" rather than on
// every node.
bool writeUisFile(const QString& path, const QTreeWidgetItem* rootItem, int quadrant, const QString& target,
                   const QString& region, const QString& linkerPrefix, const QString& bssPrefix,
                   const QString& dataPrefix, const QString& charmap, bool showTileGrid, int tileGridW,
                   int tileGridH) {
    QJsonArray nodes;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        nodes.append(serializeNode(rootItem->child(i)));
    }
    QJsonObject doc;
    doc["version"] = 1;
    doc["quadrant"] = quadrant;
    doc["target"] = target;
    doc["region"] = region;
    doc["linker_prefix"] = linkerPrefix;
    doc["bss_prefix"] = bssPrefix;
    doc["data_prefix"] = dataPrefix;
    doc["charmap"] = charmap;
    // Editor-only canvas aid (see showTileGridCheck/tileGridWSpin/
    // tileGridHSpin above) -- round-tripped here so it's remembered per
    // scene, but never read by generateCode/exportOneTarget.
    doc["showTileGrid"] = showTileGrid;
    doc["tileGridW"] = tileGridW;
    doc["tileGridH"] = tileGridH;
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
bool parseUisFile(const QString& path, QJsonArray& outNodes, int& outQuadrant, QString& outTarget,
                   QString& outRegion, QString& outLinkerPrefix, QString& outBssPrefix, QString& outDataPrefix,
                   QString& outCharmap, bool& outShowTileGrid, int& outTileGridW, int& outTileGridH) {
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
    // "quadrant" since the scene property stopped claiming to name a physical
    // nametable; "nametable" is still read so scenes written before the rename
    // (and the backups beside them) keep loading unchanged. The stored value is
    // the same 0..3 in both: bit 0 is the right-hand column, bit 1 the bottom
    // row, i.e. UL/UR/BL/BR.
    const QJsonObject docObj = doc.object();
    const QJsonValue quadrantVal =
        docObj.contains("quadrant") ? docObj["quadrant"] : docObj["nametable"];
    outQuadrant = std::clamp(quadrantVal.toInt(0), 0, 3);
    outTarget = doc.object()["target"].toString();
    outRegion = doc.object()["region"].toString();
    outLinkerPrefix = doc.object()["linker_prefix"].toString();
    outBssPrefix = doc.object()["bss_prefix"].toString();
    outDataPrefix = doc.object()["data_prefix"].toString();
    outCharmap = doc.object()["charmap"].toString();
    // Editor-only canvas aid -- missing from an older file, so it defaults
    // to off at 1x1 rather than failing the whole parse.
    outShowTileGrid = doc.object()["showTileGrid"].toBool(false);
    outTileGridW = std::max(1, doc.object()["tileGridW"].toInt(1));
    outTileGridH = std::max(1, doc.object()["tileGridH"].toInt(1));
    return true;
}

// --- Code export -----------------------------------------------------------
//
// Turns a resolved scene into a target-specific .hpp/.cpp pair. Two kinds of
// generated function, one per textbox kind, never mixed:
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
//     bake but the box/splitter/charmap -- the generated function is a thin AI
//     (always-inline) wrapper around ui::text::Make, defined inline in the
//     header for the same reason ui::text::Draw's overloads are (an
//     always_inline free function needs vague/COMDAT linkage to be safe
//     across translation units, see text.hpp). No linker_prefix: an
//     always-inline wrapper has no meaningful placement of its own. It takes
//     only the buffer -- Make finds that buffer's end by scanning for the
//     charmap's terminator tile, so there is no length to pass.
//
// Button and Slider nodes emit no function bodies at all, only geometry
// (`<name>_pos`/`<name>_box`, plus a Make_ on a variadic target), one piece
// of state each, and the hooks hand-written code must define. A button's
// state is a bit in the scene-wide `button_state` mask, driven by the
// Enable_/Disable_/Toggle_ uitk emits around it; a slider's is its own
// `<name>_amount` byte, which uitk allocates and then leaves entirely alone
// -- there is no generated setter, range, or clamp, because a slider's
// bounds belong to whatever it controls, not to the scene. The hooks are
// one Draw_<Name> for a button -- which dispatches on its own enabled bit
// (see genButtonDecl) -- and Draw_<Name>/DrawChange_<Name> for a slider:
// first placement and subsequent movement respectively, which are different
// enough in cost to be worth separating (see genSliderDecl).
//
// Negative-space nodes emit nothing themselves -- an editor-only exclusion
// zone with no runtime counterpart at all. We never bundle a component's
// make/draw together, or decide call order for the caller -- codegen only
// emits the wrappers; when and in what order a scene's components actually
// get placed/drawn is up to the game code that calls them. We also never
// draw a SingleChoice's *selection* (its arrow/cursor): that is entirely the
// caller's job, from `<name>_option` and the option's `<name>_options[i]`
// anchor -- codegen's job for one is only to emit its plain
// `<name>_option`/`<name>_nOptions` variables, its options' positions, the
// Make_/Pass_ functions that initialize/update them, and Draw_/Erase_<Name>
// conveniences that just forward to each visible member's own Draw_/Erase_
// (see genSingleChoiceDecl below).
//
// Every generated declaration lives inside `namespace gen::<sceneName>`, so
// two scenes can freely reuse the same component names without colliding.
//
// A SingleChoice's member positions -- and, the same way, a ctTextBox's own
// placement (see genCtTextBoxBody/genCtTextBoxEraseBody) -- are baked very
// differently depending on whether `target` has a fixed panel size known at
// compile time (NES/GBA/PSP) or a variadic one only known once the game is
// actually running (GCN onward, except PSP -- see kVariadicTargets): on a
// fixed target, the position is a plain already-resolved constant; on a
// variadic target it re-derives its stored position expression at runtime via
// video::viewport_*() (see emitExprCpp) since VIEWPORT_TX/TY/PX/PY can't be
// collapsed to one number the way they can for a fixed panel. A SingleChoice
// additionally needs mutable storage for this (`<name>_options`, populated by
// Make_) since it has multiple members computed at once; a ctTextBox has only
// the one position, so its Draw_/Erase_ body just re-evaluates the expression
// inline every call.
//
// Everything a SingleChoice actually needs at runtime -- the running
// `<name>_option` index, the fixed `<name>_nOptions` count, and Make_/Pass_
// to initialize/step it -- lives in `gen::<sceneName>` as plain namespace-
// scope variables and free functions. No wrapper type/instance is emitted:
// there's nothing left for one to own once geometry moved to the caller.

// Fixed nametable quadrant size, mirroring src/nes/video.cpp's xy_to_nt_addr
// (32 tiles wide, 30 tall per quadrant) -- the addressing scheme every
// backend's CartesianToAddress/WriteFromBufferToNameTable shares, independent
// of that target's own visible viewport tile count.
constexpr int kNametableQuadW = 32;
constexpr int kNametableQuadH = 30;

// Targets whose viewport size isn't fixed at compile time -- "gamecube
// onwards", per pnes's own generational split, except PSP (which
// keeps a fixed 480x272/letterboxed panel like NES/GBA). Everything NOT in
// this set bakes a SingleChoice's member positions to plain constants, the
// same way ctTextBox already does; everything in it re-derives them at
// runtime in Make_ (see the generateCode doc comment above).
const QSet<QString> kVariadicTargets{"GCN", "DS", "DSI", "Wii", "Wii U", "3DS", "Switch", "PC"};

// The subset of the above whose viewport is not merely variable in principle
// but genuinely unknown until the game runs: video::viewport_tx() resolves to
// a plain `extern` there (ogc_world_tx / wiiu_world_tx) or to a read through a
// live SDL mode pointer, so an expression over VIEWPORT_* is not a constant
// expression and `inline constexpr Rect rect = {...}` would not compile. Every
// other target's viewport_tx()/ty() really are constexpr literals (DS/DSI 32x48,
// 3DS 50x60, Switch 52x30, GBA 30x20, PSP 60x30, NES 32x30 -- see video.hpp), so
// those keep a `constexpr` Rect even though they still emit the VIEWPORT_*
// expression rather than a baked literal: it folds at compile time there.
//
// This is deliberately NOT the same split as kVariadicTargets. That set decides
// whether placement is EMITTED as a runtime expression; this one decides whether
// the resulting Rect can be `constexpr` STORAGE. A target can be in the first and
// not the second (3DS, Switch, DS, DSI), and the generated code is correct either
// way -- the expression simply folds.
const QSet<QString> kRuntimePosTargets{"GCN", "Wii", "Wii U", "PC"};

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

// The four geometry properties, in the order every expression, role table and
// error mask in this file indexes them: 0 = pos.x, 1 = pos.y, 2 = size.x,
// 3 = size.y. Shared by the editor's own resolver (resolveAll, in
// createSidebar) and by the export-time one below, so the two cannot drift
// about which index means what.
constexpr int kGeomRoles[4] = {kPosXRole, kPosYRole, kSizeWRole, kSizeHRole};
constexpr int kGeomExprRoles[4] = {kPosXExprRole, kPosYExprRole, kSizeWExprRole, kSizeHExprRole};
constexpr long long kGeomDefaults[4] = {0, 0, 1, 1};

// Every node's four geometry properties, resolved for ONE export target.
//
// The editor caches exactly these numbers per node in kPosXRole/kPosYRole/
// kSizeWRole/kSizeHRole -- but it resolves them against whatever the Target
// combo is on, which is the target the scene was AUTHORED on and has nothing
// to do with the target being exported. Reading those roles at export time
// therefore bakes one target's numbers into every target's header: a slider
// sized `(PANEL1_TX >> 2) - 2` came out 6 tiles wide in the 3DS header as well
// as the DS one, because the scene was authored on DS, even though the 3DS's
// bottom panel is 40 tiles wide and the expression says 8 there.
//
// So export resolves its own. Same fixed-point loop as the editor's resolver (a
// property may name another node's property, which may name a third, so passes
// repeat until one makes no progress), same identifier vocabulary, same
// clamping. Two deliberate differences:
//
//  - the viewport comes from the target's own entry in viewportTilesForTarget()
//    and panel1TilesForTarget(), not from the sidebar's Viewport X/Y boxes;
//  - VIEWPORT_PX/PY are tiles * 8, i.e. hardware pixels. The editor multiplies
//    by `tilePx`, the canvas's current zoom, which is a display artifact that
//    has no business reaching generated code.
//
// Anything that cannot be resolved at all (a parse error, an unknown or
// duplicate reference, a dependency cycle) keeps the editor's own cached value
// for that property, which is exactly what export used to emit for it -- a
// broken expression generates what it generated before rather than collapsing
// to zero.
//
// Nothing here writes to the tree: the editor's cached roles stay as the editor
// left them, so exporting for eleven targets in a row doesn't leave the canvas
// drawing the last one's layout.
struct TargetGeometry {
    QHash<const QTreeWidgetItem*, std::array<long long, 4>> values;

    // `prop` is an index into kGeomRoles. Falls back to the node's own cached
    // role, then to the property's default, for a node this resolution never
    // saw (nothing in the export path should ask, but a wrong answer here would
    // be a silently misplaced component rather than a crash).
    long long at(const QTreeWidgetItem* item, int prop) const {
        if (!item || prop < 0 || prop > 3) return 0;
        const auto it = values.constFind(item);
        if (it != values.constEnd()) return (*it)[static_cast<size_t>(prop)];
        const QVariant cached = item->data(0, kGeomRoles[prop]);
        return cached.isValid() ? cached.toLongLong() : kGeomDefaults[prop];
    }

    int x(const QTreeWidgetItem* item) const { return static_cast<int>(at(item, 0)); }
    int y(const QTreeWidgetItem* item) const { return static_cast<int>(at(item, 1)); }
    // Clamped to what a Rect's vec2<u8> size can actually hold, the same clamp
    // genGeometryStorage used to apply to the cached role.
    int w(const QTreeWidgetItem* item) const { return std::clamp<int>(static_cast<int>(at(item, 2)), 1, 255); }
    int h(const QTreeWidgetItem* item) const { return std::clamp<int>(static_cast<int>(at(item, 3)), 1, 255); }
};

TargetGeometry resolveGeometryForTarget(QTreeWidgetItem* rootItem, const QString& target, const QString& region) {
    TargetGeometry out;
    const QVector<QTreeWidgetItem*> components = collectComponentItems(rootItem);

    const auto vp = viewportTilesForTarget(target);
    // A target absent from that table (GC/Wii/PC) has no canonical viewport, so
    // this falls back to the NES-native 32x30 -- the same floor generateCode's
    // own ntOffXExpr/ntOffYExpr clamp to. It only ever feeds a baked literal,
    // and every target absent from the table is variadic, where VIEWPORT_* is
    // emitted as a runtime expression instead of being baked at all.
    const long long vpTx = vp ? vp->first : kNametableQuadW;
    const long long vpTy = vp ? vp->second : kNametableQuadH;
    const long long vpPx = vpTx * 8;
    const long long vpPy = vpTy * 8;
    const auto panel = panel1TilesForTarget(target);

    QHash<QString, QTreeWidgetItem*> byName;
    QSet<QString> duplicateNames;
    for (QTreeWidgetItem* item : components) {
        const QString base = bareName(item);
        if (byName.contains(base)) duplicateNames.insert(base);
        byName.insert(base, item);
    }

    struct Pending {
        QTreeWidgetItem* item;
        int prop;
        ExprPtr ast;
    };
    QVector<Pending> pending;
    std::map<std::pair<const QTreeWidgetItem*, int>, long long> resolved;

    for (QTreeWidgetItem* item : components) {
        for (int prop = 0; prop < 4; ++prop) {
            QString src = item->data(0, kGeomExprRoles[prop]).toString().trimmed();
            if (src.isEmpty()) src = QString::number(kGeomDefaults[prop]);
            bool ok = false;
            ExprPtr ast = ExprParser(src).parse(ok);
            if (!ok) continue;  // keeps the cached value, see TargetGeometry::at
            pending.push_back({item, prop, ast});
        }
    }

    std::function<std::optional<long long>(const QString&, int)> resolveIdent =
        [&](const QString& name, int prop) -> std::optional<long long> {
        if (prop < 0) {
            if (name == QLatin1String("VIEWPORT_TX")) return vpTx;
            if (name == QLatin1String("VIEWPORT_TY")) return vpTy;
            if (name == QLatin1String("VIEWPORT_PX")) return vpPx;
            if (name == QLatin1String("VIEWPORT_PY")) return vpPy;
            if (name.startsWith(QLatin1String("PANEL1_"))) {
                if (name == QLatin1String("PANEL1_X")) return panel ? panel->x : 0;
                if (name == QLatin1String("PANEL1_Y")) return panel ? panel->y : 0;
                if (name == QLatin1String("PANEL1_TX")) return panel ? panel->tx : vpTx;
                if (name == QLatin1String("PANEL1_TY")) return panel ? panel->ty : vpTy;
            }
            return std::nullopt;
        }
        if (duplicateNames.contains(name)) return std::nullopt;
        const auto it = byName.constFind(name);
        if (it == byName.constEnd()) return std::nullopt;
        // .textSize and .enabled depend on nothing that is still resolving, so
        // they are answered directly rather than through the map below -- and
        // .enabled against THIS export's target/region, the same check
        // isEffectivelyHidden makes when deciding to skip the node entirely.
        if (prop == 4) {
            return isComponentItem(*it)
                       ? std::optional<long long>((*it)->data(0, kTextContentRole).toString().length())
                       : std::nullopt;
        }
        if (prop == 5) return isEffectivelyHidden(*it, target, region) ? 0 : 1;
        const auto found = resolved.find({*it, prop});
        return found != resolved.end() ? std::optional<long long>(found->second) : std::nullopt;
    };

    // `this` names whichever node's property is being evaluated -- the parser
    // has no notion of that, so it is substituted per pending entry.
    auto resolveIdentFor = [&resolveIdent](const QTreeWidgetItem* self) {
        const QString selfName = bareName(self);
        return [&resolveIdent, selfName](const QString& name, int prop) {
            return resolveIdent(name == QLatin1String("this") ? selfName : name, prop);
        };
    };

    bool progress = true;
    while (progress && !pending.isEmpty()) {
        progress = false;
        for (int i = pending.size() - 1; i >= 0; --i) {
            const auto v = evalExpr(pending[i].ast, resolveIdentFor(pending[i].item));
            if (!v) continue;
            resolved[{pending[i].item, pending[i].prop}] = *v;
            pending.removeAt(i);
            progress = true;
        }
    }

    for (QTreeWidgetItem* item : components) {
        std::array<long long, 4> vals{};
        for (int prop = 0; prop < 4; ++prop) {
            const auto it = resolved.find({item, prop});
            long long value = (it != resolved.end()) ? it->second
                                                     : item->data(0, kGeomRoles[prop]).toLongLong();
            // Same floors the editor's resolver applies: a position cannot be
            // negative, a size cannot be zero.
            value = (prop >= 2) ? std::max<long long>(1, value) : std::max<long long>(0, value);
            vals[static_cast<size_t>(prop)] = value;
        }
        out.values.insert(item, vals);
    }
    return out;
}

// Cross-reference resolver for a position expression's `Other.pos.x`-style
// identifiers -- these are never runtime-variable, only VIEWPORT_* is (see
// emitExprCpp), so they're baked to the already-resolved value every other
// geometry node's own codegen uses -- taken from `geom`, this export target's
// own resolution (see TargetGeometry), never from the editor's cached roles,
// which hold whatever target the scene was last authored on. `.enabled`
// (prop 5) is the one exception that isn't itself stored on the node -- it's
// recomputed here the same way resolveAllPtr's own resolveIdent does, against
// this export's actual target/region rather than whatever happened to be
// selected in the sidebar when the scene was last edited. Shared by
// genCtTextBoxBody/genCtTextBoxEraseBody and genSingleChoiceDecl's own copy of
// the same logic.
long long resolvePositionRef(QTreeWidgetItem* rootItem, const QString& target, const QString& region,
                              const QTreeWidgetItem* self, const QString& rawName, int prop,
                              const TargetGeometry& geom) {
    const QString wanted = (rawName == QLatin1String("this")) ? bareName(self) : rawName;
    for (QTreeWidgetItem* other : collectComponentItems(rootItem)) {
        if (bareName(other) != wanted) continue;
        switch (prop) {
            case 0:
            case 1:
            case 2:
            case 3: return geom.at(other, prop);
            case 4: return other->data(0, kTextContentRole).toString().length();
            case 5: return isEffectivelyHidden(other, target, region) ? 0 : 1;
            default: return 0;
        }
    }
    return 0;
}

// Emits one geometry component of `item` -- `prop` indexes kGeomExprRoles, so
// 0/1 are pos.x/pos.y and 2/3 are size.x/size.y -- as a runtime C++ expression
// when `variadic` is set and the node's stored expression text parses: the same
// VIEWPORT_TX/TY/PX/PY-aware placement a SingleChoice's own members already get
// on a variadic target (see the kVariadicTargets doc comment above). Falls back
// to the literal `geom` resolved for this export target otherwise (a fixed
// target, or an expression that doesn't parse).
//
// Sizes go through this for the same reason positions do: `(VIEWPORT_TX >> 2)`
// is no more a constant on a variadic target than `VIEWPORT_TX >> 1` is, and
// baking it would freeze a slider's track, or a box's footprint, at one
// target's viewport. On the variadic targets whose viewport_*() really are
// constexpr (DS/DSi/3DS/Switch -- see kRuntimePosTargets) the emitted
// expression simply folds, exactly as the position next to it does.
//
// A ctTextBox has exactly one geometry, unlike a SingleChoice's several
// members, so this re-evaluates inline in Draw_/Erase_ rather than needing
// separate mutable storage populated by a Make_.
QString emitGeomComponent(const QTreeWidgetItem* item, int prop, bool variadic, QTreeWidgetItem* rootItem,
                           const QString& target, const QString& region, const TargetGeometry& geom,
                           int depth = 0) {
    if (variadic) {
        QString src = item->data(0, kGeomExprRoles[prop]).toString().trimmed();
        if (src.isEmpty()) src = QStringLiteral("0");
        bool ok = false;
        const ExprPtr ast = ExprParser(src).parse(ok);
        if (ok) {
            auto resolveFor = [rootItem, target, region, item, &geom](const QString& n, int p) {
                return resolvePositionRef(rootItem, target, region, item, n, p, geom);
            };
            // Another node's pos/size: splice in that node's own expression
            // (parenthesized), recursively -- see emitExprCpp. Size references
            // are spliced as well as positions, since a node sized off the
            // viewport is exactly as runtime-variable as one placed off it.
            // Depth-capped so a reference cycle (A.pos.x = B.pos.x,
            // B.pos.x = A.pos.x -- the editor's resolver just leaves those
            // unresolved) falls back to the baked value instead of recursing
            // forever.
            auto substitute = [rootItem, target, region, item, depth, &geom](const QString& n,
                                                                             int p) -> std::optional<QString> {
                if (p < 0 || p > 3 || depth >= 16) return std::nullopt;
                const QString wanted = (n == QLatin1String("this")) ? bareName(item) : n;
                for (QTreeWidgetItem* other : collectComponentItems(rootItem)) {
                    if (bareName(other) != wanted) continue;
                    return "(" + emitGeomComponent(other, p, true, rootItem, target, region, geom, depth + 1) + ")";
                }
                return std::nullopt;
            };
            return emitExprCpp(ast, resolveFor, substitute);
        }
    }
    return QString::number(geom.at(item, prop));
}

// Emits one ctTextBox's data + placement function body: a
// ppu::WriteFromBufferToNameTable call per wrapped row -- same wrap/align
// math as TileGridWidget::paintEvent, so what's exported always matches what
// the canvas previewed.
//
// With a scene charmap set (`charmapFn` non-empty), each row's text is
// mapped through it at compile time -- the same `<charmap>.encode(...)`
// technology demo/src/graphics/strings.hpp already hand-writes (e.g.
// msg_title) -- and hoisted to a `<dataPrefixTok>inline constexpr auto
// <Name>_rowN = ...` declaration at the namespace root, so it's a single
// rodata object the linker folds regardless of how many TUs include it, not
// a local buffer reallocated on every call. The Draw_ function then just
// points WriteFromBufferToNameTable at it via ::SIZED_STR, not ::SIZED_OBJ:
// `encode()` terminates, and the terminator is a mapped tile like any other
// (it is whatever the charmap maps '\0' to, not a byte a nametable write
// ignores), so ::SIZED_OBJ would draw one tile too many. ::SIZED_STR drops
// exactly that one byte -- the same `sizeof(...) - 1` the plain C string
// literal fallback below does by hand. With no charmap set, this falls back
// to that previous plain `static const char[]` behavior, for a scene that
// hasn't set one yet.
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
CtTextBoxGen genCtTextBoxBody(const QTreeWidgetItem* item, int ntOffX, int ntOffY, bool isNes,
                               bool variadic, QTreeWidgetItem* rootItem, const QString& target,
                               const QString& region, const QString& dataPrefixTok, const QString& charmapFn,
                               const QString& ntOffXExpr, const QString& ntOffYExpr,
                               const TargetGeometry& geom) {
    const int x = geom.x(item) + ntOffX;
    const int y = geom.y(item) + ntOffY;
    // The wrap below is laid out at generation time, so the box's own size is
    // baked even on a variadic target (unlike its position) -- but baked from
    // THIS target's resolution, not the editor's.
    const int w = geom.w(item);
    const int h = geom.h(item);
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
                // Node-local now: this declaration lands inside the node's
                // own namespace (see emitNodeNamespace), so it needs no
                // `<name>_` disambiguator -- gen::options::audioText::row0.
                const QString dataName = QString("row%1").arg(row);
                rootLines << QString("%1inline constexpr auto %2 = %3.encode(\"%4\");")
                                 .arg(dataPrefixTok, dataName, charmapFn, cStringEscape(rowText));
                sourceArgs = QString("SIZED_STR(%1)").arg(dataName);
            }

            if (isNes) {
                bodyLines << QString("    ppu::WriteFromBufferToNameTable(0x%1, %2, 0);")
                                 .arg(static_cast<uint>(nesNtAddr(x + startCol, y + row)), 4, 16, QLatin1Char('0'))
                                 .arg(sourceArgs);
            } else if (variadic) {
                // Re-derive the box's own VIEWPORT_TX/TY-based placement
                // expression at runtime (see emitGeomComponent) instead of
                // baking the value it happened to resolve to in the editor --
                // otherwise this row's arrow-anchor (SingleChoice's own
                // `_options[i]`, itself expression-driven) and its drawn text
                // only line up at whatever viewport size was open in uitk
                // when this was exported. ntOffXExpr/ntOffYExpr (not the
                // fixed ntOffX/ntOffY ints -- see generateCode's own comment
                // on them) keep the nametable-quadrant term itself runtime-
                // resolved too, for the same reason.
                const QString xExpr = emitGeomComponent(item, 0, true, rootItem, target, region, geom);
                const QString yExpr = emitGeomComponent(item, 1, true, rootItem, target, region, geom);
                QString xFinal = startCol ? QString("(%1 + %2)").arg(xExpr).arg(startCol) : xExpr;
                QString yFinal = row ? QString("(%1 + %2)").arg(yExpr).arg(row) : yExpr;
                if (ntOffXExpr != QLatin1String("0")) xFinal = QString("(%1 + %2)").arg(xFinal, ntOffXExpr);
                if (ntOffYExpr != QLatin1String("0")) yFinal = QString("(%1 + %2)").arg(yFinal, ntOffYExpr);
                bodyLines << QString("    ppu::WriteFromBufferToNameTable("
                                      "vec2<u16>{static_cast<u16>(%1), static_cast<u16>(%2)}, %3, 0);")
                                 .arg(xFinal, yFinal, sourceArgs);
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
QString genCtTextBoxEraseBody(const QTreeWidgetItem* item, int ntOffX, int ntOffY, bool isNes, bool variadic,
                               QTreeWidgetItem* rootItem, const QString& target, const QString& region,
                               const QString& charmapFn, const QString& ntOffXExpr, const QString& ntOffYExpr,
                               const TargetGeometry& geom) {
    const int x = geom.x(item) + ntOffX;
    const int y = geom.y(item) + ntOffY;
    const int w = geom.w(item);
    const int h = geom.h(item);
    // With a charmap, the blank tile is bound to a local `constexpr` first
    // rather than passed inline as `charmap_x(' ')`: an inline call in a plain
    // argument position is only *allowed* to fold at compile time, and an
    // unoptimised (-O0) build doesn't -- it emits a real call to the charmap
    // function, whose fall-through for an unmapped character is a deliberate
    // undefined reference (tech::nes_str::unmapped), so the whole demo then
    // fails to link over a character that was never unmapped.
    const QString tileExpr = charmapFn.isEmpty() ? QStringLiteral("' '") : QStringLiteral("blank");

    QStringList bodyLines;
    if (!charmapFn.isEmpty()) {
        bodyLines << QString("    constexpr auto blank = %1(' ');").arg(charmapFn);
    }
    for (int row = 0; row < h; ++row) {
        if (isNes) {
            bodyLines << QString("    ppu::WriteRepeatedToNameTable(0x%1, %2, %3, 0);")
                             .arg(static_cast<uint>(nesNtAddr(x, y + row)), 4, 16, QLatin1Char('0'))
                             .arg(tileExpr)
                             .arg(w);
        } else if (variadic) {
            // Same reasoning as genCtTextBoxBody's own variadic branch: this
            // has to erase exactly where the current Draw_ call landed, which
            // only holds if both re-derive the same expression at runtime --
            // ntOffXExpr/ntOffYExpr included.
            const QString xExpr = emitGeomComponent(item, 0, true, rootItem, target, region, geom);
            const QString yExpr = emitGeomComponent(item, 1, true, rootItem, target, region, geom);
            QString xFinal = xExpr;
            QString yFinal = row ? QString("(%1 + %2)").arg(yExpr).arg(row) : yExpr;
            if (ntOffXExpr != QLatin1String("0")) xFinal = QString("(%1 + %2)").arg(xFinal, ntOffXExpr);
            if (ntOffYExpr != QLatin1String("0")) yFinal = QString("(%1 + %2)").arg(yFinal, ntOffYExpr);
            bodyLines << QString("    ppu::WriteRepeatedToNameTable("
                                  "vec2<u16>{static_cast<u16>(%1), static_cast<u16>(%2)}, %3, %4, 0);")
                             .arg(xFinal, yFinal, tileExpr)
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

// The bare names of the child namespaces emitted INSIDE `item`'s own namespace,
// in document order -- i.e. exactly the nodes a parent's Make() has to drive.
// Mirrors emitNodeNamespace's walk, so the list is never out of step with what
// was actually emitted: a non-prefixed branch is transparent (its prefixed
// descendants land at this level too), a hidden node is emitted nowhere, and
// neither is a SingleChoice left with no visible options.
//
// Every parent Make() calls every one of these unconditionally, so a node with
// a parent is made once by its own namespace's caller and again by each
// ancestor above it -- a whole-scene Make() therefore makes a button's label
// twice. That is the intended trade: making geometry again is a few stores of
// values that cannot have changed, whereas a made/not-made flag would cost
// permanent storage per node, a branch on every Make(), and a state that goes
// stale the moment a caller makes a subtree directly (which callers do -- see
// genSingleChoiceDecl). Redundant work is both the cheaper and the safer half.
QStringList childNamespaceNames(const QTreeWidgetItem* item, const QString& target, const QString& region) {
    QStringList names;
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* child = item->child(i);
        if (!isPrefixedItem(child)) {
            names += childNamespaceNames(child, target, region);  // transparent branch
            continue;
        }
        if (isEffectivelyHidden(child, target, region)) continue;
        if (isSingleChoiceItem(child) && singleChoiceMembers(child, target, region).isEmpty()) continue;
        names << bareName(child);
    }
    return names;
}

// Emits the geometry every placeable node now carries, in the three shapes a
// target can need it (see kRuntimePosTargets): `rect` -- the node's anchor AND
// footprint as one ::Rect (pnes/extras/ui/types.hpp) -- `pos`, the same anchor
// already resolved to a PPU address, and the `Make()` that fills whichever of
// the two could not be established ahead of time.
//
//  - NES: both are compile-time constants. `rect` holds the cartesian tile
//    position; `pos` is the logical nametable address nesNtAddr bakes, which is
//    mirroring-independent on real hardware (the board's wiring routes the
//    write AFTER the address, so the address itself never moves). Make() is an
//    empty stub, emitted purely so a caller never needs a #if around the call.
//  - A fixed-viewport emu target (GBA/PSP/DS/DSI/3DS/Switch): `rect` is still
//    `constexpr` -- viewport_tx()/ty() are real constexpr literals there, so a
//    VIEWPORT_*-driven expression folds -- but `pos` is NOT, because the emu
//    backend's own CartesianToAddress (src/emu/ppu.cpp's xy_to_nt_addr) folds
//    the runtime ::mirroring flag into the offset it returns. So Make() resolves
//    just the address.
//  - A runtime-viewport target (kRuntimePosTargets): neither is knowable ahead
//    of time, so both are mutable storage and Make() fills both.
//
// `atomic` on the mutable forms, same reasoning as the `<name>_pos` this
// replaces: it is non-local mutable memory an aggressively-LTO'd target could
// otherwise cache or reorder across Make()'s write and a reader elsewhere.
// `atomic` is `volatile` on every C++ build (technology.hpp), which an implicit
// copy constructor/assignment operator is not viable on -- ::Rect carries its
// own volatile-qualified pair for exactly that (pnes/extras/ui/types.hpp), so
// Make() builds a plain local and stores that in one go rather than writing
// through the volatile object field by field.
QStringList genGeometryStorage(const QTreeWidgetItem* item, int ntOffX, int ntOffY, bool isNes, bool variadic,
                                bool runtimePos, QTreeWidgetItem* rootItem, const QString& target,
                                const QString& region, const QString& ntOffXExpr, const QString& ntOffYExpr,
                                const TargetGeometry& geom) {
    const int x = geom.x(item) + ntOffX;
    const int y = geom.y(item) + ntOffY;
    const int w = geom.w(item);
    const int h = geom.h(item);

    // The anchor as C++: a baked literal on a fixed panel, or the node's own
    // VIEWPORT_*-aware expression on a variadic one (see emitGeomComponent).
    // ntOffXExpr/ntOffYExpr rather than the fixed ntOffX/ntOffY ints for the
    // quadrant term, same reason every other variadic emission here uses them.
    QString xCpp = QString::number(x);
    QString yCpp = QString::number(y);
    // And the footprint the same way, but only where it has to be: a size over
    // VIEWPORT_*/PANEL1_* is no more constant on a variadic target than a
    // position is, and baking it would pin a slider's track or a box's
    // footprint to one target's viewport. A size that doesn't name the viewport
    // at all -- which is nearly every one -- stays the baked literal it has
    // always been, so the emitted size only ever changes where the scene asked
    // it to vary. The "does it name the viewport" test is whether the emitted
    // expression reached for a video:: accessor, which is the only way one gets
    // in there (see emitExprCpp); everything else folded to arithmetic on
    // constants that `w`/`h` already hold.
    //
    // static_cast<u8> because Rect::size is a vec2<u8>: brace-initialising that
    // from an int expression is a narrowing conversion, an error in aggregate
    // init unless the value is a constant that fits (true on the folding
    // targets, not on a genuinely runtime one). The cast also means a
    // viewport-driven size is truncated rather than clamped past 255 -- a tile
    // count that large is off every panel this targets anyway, and the frozen
    // literal it replaces was no more right.
    QString wCpp = QString::number(w);
    QString hCpp = QString::number(h);
    if (variadic) {
        xCpp = emitGeomComponent(item, 0, true, rootItem, target, region, geom);
        yCpp = emitGeomComponent(item, 1, true, rootItem, target, region, geom);
        if (ntOffXExpr != QLatin1String("0")) xCpp = QString("(%1 + %2)").arg(xCpp, ntOffXExpr);
        if (ntOffYExpr != QLatin1String("0")) yCpp = QString("(%1 + %2)").arg(yCpp, ntOffYExpr);
        xCpp = QString("static_cast<u16>(%1)").arg(xCpp);
        yCpp = QString("static_cast<u16>(%1)").arg(yCpp);
        const QString wExpr = emitGeomComponent(item, 2, true, rootItem, target, region, geom);
        const QString hExpr = emitGeomComponent(item, 3, true, rootItem, target, region, geom);
        if (wExpr.contains(QLatin1String("video::"))) wCpp = QString("static_cast<u8>(%1)").arg(wExpr);
        if (hExpr.contains(QLatin1String("video::"))) hCpp = QString("static_cast<u8>(%1)").arg(hExpr);
    }
    const QString rectLiteral = QString("Rect{{%1, %2}, {%3, %4}}").arg(xCpp, yCpp, wCpp, hCpp);

    // This node's own Make() drives its children's too, every time -- see
    // childNamespaceNames for why they are made unconditionally. Own geometry
    // is resolved first: a child's placement expression that names this node is
    // spliced in textually (emitGeomComponent), never read back out of `rect`,
    // but ordering it this way means a child Make() that ever did read the
    // parent reads a resolved one.
    QStringList childMakes;
    for (const QString& kid : childNamespaceNames(item, target, region)) {
        childMakes << QString("    %1::Make();").arg(kid);
    }
    const QString childTail = childMakes.isEmpty() ? QString() : ("\n" + childMakes.join("\n"));

    QStringList lines;
    if (runtimePos) {
        lines << QString("inline atomic Rect rect;");
        lines << QString("inline atomic u16 pos;");
        lines << QString("inline AI void Make() {\n"
                          "    const Rect r = %1;\n"
                          "    rect = r;\n"
                          "    pos = ppu::CartesianToAddress(r.pos);%2\n"
                          "}")
                     .arg(rectLiteral, childTail);
        return lines;
    }

    lines << QString("inline constexpr Rect rect = %1;").arg(rectLiteral);
    if (isNes) {
        lines << QString("inline constexpr u16 pos = 0x%1;")
                     .arg(static_cast<uint>(nesNtAddr(x, y)), 4, 16, QLatin1Char('0'));
        // Nothing of its own left to resolve -- emitted anyway so every target
        // exposes the same call, and no caller needs a #if to decide whether it
        // exists. It still makes the children, whose own Make()s are equally
        // empty here; the whole chain inlines away to nothing.
        lines << (childMakes.isEmpty() ? QString("inline AI void Make() {}")
                                        : QString("inline AI void Make() {\n%1\n}").arg(childMakes.join("\n")));
    } else {
        lines << QString("inline atomic u16 pos;");
        lines << (childMakes.isEmpty()
                      ? QString("inline AI void Make() { pos = ppu::CartesianToAddress(rect.pos); }")
                      : QString("inline AI void Make() {\n"
                                 "    pos = ppu::CartesianToAddress(rect.pos);\n%1\n}")
                            .arg(childMakes.join("\n")));
    }
    return lines;
}

// Emits one SingleChoice node's own declarations: the runtime-indexable
// `options` anchor table, optionally the `rect` covering all of them, the
// `option`/`nOptions` variables, the Make()/Pass() functions that
// initialize/step them, and the forced-to-define DrawChange() that Pass()
// repaints the selection indicator through. Its members' own namespaces are
// emitted separately, immediately BEFORE this, by emitNodeNamespace -- which is
// what lets Draw() below name them. Everything lives header-only (`inline`),
// same reasoning as rtTextBox's own wrapper: it has to be safely includable
// from more than one TU. Returns empty for a SingleChoice with no (visible)
// options -- nothing to construct.
//
// `options` duplicates what each member's own `rect` already holds, and does so
// deliberately: a member's rect is reachable only by name, and placing the
// selection indicator means indexing by a value (`option`) that isn't known
// until runtime. So this is the indexable VIEW over those rects, built from
// them rather than from a second copy of the placement math -- on a fixed
// target it is a constexpr aggregate of the members' own constexpr rects, and
// on a runtime-viewport one Make() copies each across after making it.
//
// `bssPrefixTok` is already empty on every target but NES (see generateCode) --
// it places the instance's own mutable storage, and, on a runtime-viewport
// target, its mutable options array.
QString genSingleChoiceDecl(QTreeWidgetItem* scItem, QTreeWidgetItem* rootItem, const QString& target,
                             const QString& region, const QString& bssPrefixTok, const QString& dataPrefixTok,
                             bool runtimePos) {
    const QVector<QTreeWidgetItem*> members = singleChoiceMembers(scItem, target, region);
    if (members.isEmpty()) {
        return QString();
    }
    const int nOptions = members.size();
    const int defaultOption = std::clamp(scItem->data(0, kDefaultOptionRole).toInt(), 0, nOptions - 1);
    const bool scVertical = scItem->data(0, kSingleChoiceVerticalRole).toBool();

    QStringList lines;

    if (runtimePos) {
        // Mutable, populated by Make() below -- the viewport size (and
        // therefore every member's resolved rect) isn't known until the game
        // is actually running.
        //
        // `atomic` (technology.hpp: volatile on NES, true atomic elsewhere)
        // because this is non-local mutable memory an aggressively-LTO'd,
        // multithreaded target could otherwise cache/reorder/tear across
        // Make()'s writes and a reader elsewhere -- unlike `option` below
        // (already atomic itself), nothing else protects this array.
        lines << QString("%1inline atomic Rect options[%2];").arg(bssPrefixTok).arg(nOptions);
    } else {
        // Fixed panel: every member's rect is already a compile-time constant,
        // so this is a plain constexpr table (ROM, under `dataPrefixTok` on
        // NES) built straight out of them -- no second copy of the placement
        // math, and nothing for Make() to populate.
        QStringList entries;
        for (QTreeWidgetItem* member : members) {
            entries << QString("%1::rect").arg(bareName(member));
        }
        lines << QString("%1inline constexpr Rect options[%2] = {%3};")
                     .arg(dataPrefixTok)
                     .arg(nOptions)
                     .arg(entries.join(", "));
    }

    // The running option index -- `atomic` (technology.hpp: volatile on NES,
    // true atomic elsewhere) because it's read from wherever the caller's
    // Pass() result matters (often a draw path outside the code that last
    // called Pass() itself), same reasoning as the mutable `options` array
    // above. `nOptions` never changes once generated, so it's a plain
    // compile-time constant instead.
    lines << QString("%1inline atomic u8 option = %2;").arg(bssPrefixTok).arg(defaultOption);
    lines << QString("inline constexpr u8 nOptions = %1;").arg(nOptions);

    // Which axis this menu steps (kSingleChoiceVerticalRole), as a value the
    // scene can read rather than a fact it has to remember from the editor.
    // Pass() below already bakes the axis into the two bits it spends, so this
    // exists for the hand-written side: a draw hook that lays options down a
    // column or across a row, or a static_assert that this menu disagrees with
    // the Canvas holding it. `constexpr`, so it addresses nothing and costs no
    // ROM -- the generated code itself never reads it.
    lines << QString("inline constexpr bool vertical = %1;")
                 .arg(scVertical ? QLatin1String("true") : QLatin1String("false"));

    // The whole menu's footprint: the smallest Rect covering every option it
    // shows. A SingleChoice has no geometry of its own to place -- this is
    // derived, not authored -- but something that treats the menu as one thing
    // needs one Rect for it: a Canvas holding this menu paints focus over
    // exactly this area (see genCanvasDecl), and a scene framing or clearing
    // the menu wants the same bounds.
    //
    // Opt-in, because it is only free on a fixed-viewport target. There it
    // folds at compile time and a `constexpr` nothing addresses costs no ROM;
    // on a runtime-viewport one it is six bytes of mutable storage and a fold
    // in every Make(), which a menu nobody measures should not be paying.
    // Being a Canvas component overrides the switch rather than erroring on
    // it: that table is built out of this rect, so "off" there is not a
    // preference uitk could honour -- it would just fail to compile, naming a
    // generated line rather than the thing to fix.
    //
    // When it is off, nothing is emitted at all -- no empty stub, unlike a
    // Make(), which every kind has so no call site needs a #if around it. A
    // rect is a value, not a call, and the honest absence of one is a compile
    // error at whatever tried to read it, naming this menu.
    //
    // Folded out of `options` rather than recomputed from the placement math,
    // so it cannot disagree with the rects it covers, and so an option hidden
    // on this target is absent from the bounds for free. It follows the same
    // fixed/runtime split as `options` itself: a compile-time fold over a
    // constexpr table, or, on a runtime-viewport target, mutable storage the
    // Make() below folds once the table is filled.
    const bool wantRect =
        scItem->data(0, kSingleChoiceHasRectRole).toBool() || isCanvasComponent(scItem, target, region);
    const QString boundsFold =
        QString("    u16 x0 = options[0].pos.x;\n"
                 "    u16 y0 = options[0].pos.y;\n"
                 "    u16 x1 = static_cast<u16>(options[0].pos.x + options[0].size.x);\n"
                 "    u16 y1 = static_cast<u16>(options[0].pos.y + options[0].size.y);\n"
                 "    for (u8 i = 1; i < nOptions; ++i) {\n"
                 "        const Rect r = options[i];\n"
                 "        if (r.pos.x < x0) x0 = r.pos.x;\n"
                 "        if (r.pos.y < y0) y0 = r.pos.y;\n"
                 "        if (static_cast<u16>(r.pos.x + r.size.x) > x1) x1 = static_cast<u16>(r.pos.x + r.size.x);\n"
                 "        if (static_cast<u16>(r.pos.y + r.size.y) > y1) y1 = static_cast<u16>(r.pos.y + r.size.y);\n"
                 "    }\n");
    const QString boundsValue = QString("Rect{{x0, y0}, {static_cast<u8>(x1 - x0), static_cast<u8>(y1 - y0)}}");
    if (wantRect) {
        if (runtimePos) {
            lines << QString("%1inline atomic Rect rect;").arg(bssPrefixTok);
        } else {
            // An immediately-invoked constexpr lambda, so the fold sits with
            // the table it folds instead of needing a named helper in the
            // library for the one place that uses it. Legal in a header: C++17
            // onward requires a closure type in an inline variable's
            // initializer to be the same entity in every TU that sees it.
            lines << QString("%1inline constexpr Rect rect = [] {\n%2    return %3;\n}();")
                         .arg(dataPrefixTok, boundsFold, boundsValue);
        }
    }

    // Make() makes every child namespace this node holds first -- its options,
    // plus anything else nested under it -- then snapshots the options' rects.
    // It makes them
    // itself rather than assuming someone else already did, because a caller is
    // expected to be able to reach for just this menu's Make() (the demo's
    // title.cpp does exactly that), and `options` would otherwise copy rects
    // nothing had filled in yet on a runtime-viewport target. The scene-level
    // Make() also lists the members, so on a whole-scene make each one is made
    // twice -- a parent always makes its children, and repeating a few stores
    // beats tracking what has already been made (see generateCode).
    QStringList makeBody;
    for (const QString& kid : childNamespaceNames(scItem, target, region)) {
        makeBody << QString("    %1::Make();").arg(kid);
    }
    if (runtimePos) {
        for (int i = 0; i < members.size(); ++i) {
            // Whole-Rect load out of the member's own atomic before storing
            // it into this one, through ::Rect's volatile-qualified copy
            // constructor and assignment operator (see types.hpp).
            makeBody << QString("    { const Rect r = %1::rect; options[%2] = r; }")
                            .arg(bareName(members[i]))
                            .arg(i);
        }
        if (wantRect) {
            // Folded here, after the table is filled and out of the table
            // itself -- the same fold the fixed-target `rect` above does at
            // compile time, re-indented one level to sit inside Make()'s own
            // scope.
            QString nestedFold = boundsFold;
            nestedFold.replace(QLatin1String("\n    "), QLatin1String("\n        "));
            makeBody << QString("    {\n    %1        rect = %2;\n    }").arg(nestedFold, boundsValue);
        }
    }
    makeBody << QString("    option = %1;").arg(defaultOption);

    lines << QString("inline AI void Make() {\n%1\n}").arg(makeBody.join("\n"));

    // Forced-to-define, like a Slider's DrawChange() and a Canvas's
    // DrawUpdate(): the Pass() below moves `option`, and repainting the
    // indicator that marks it is the scene's business. uitk knows where every
    // option sits (`options` above) and nothing whatever about what marks one
    // -- the demo's is a BG tile two cells left of the option's anchor
    // (demo/ui/impl/singleChoice.hpp), but it could as easily be a sprite, a
    // palette swap, or an attribute change, and there is no appearance here
    // that would be right for every menu. Same reasoning as a Button's
    // Draw().
    //
    // Declared for EVERY SingleChoice, unlike the Draw()/Erase() pair below,
    // which need every option to be a ctTextBox: an indicator is placed out
    // of `options`, which exists whatever the options are made of, so an
    // rtTextBox menu has a cursor to move just the same.
    //
    // Both indices, rather than the Slider's single "moving to" value. A
    // Slider can get away with one because `amount` still holds the old
    // position when DrawChange() runs -- the hook is what stores the new one.
    // Here Pass() owns `option` (it is the clamped value the rest of the
    // scene reads back, and a Canvas may be what called in), so by the time
    // the hook runs the old index is gone unless it is passed. Indices and
    // not Rects: the hook indexes `options` itself, which is a constexpr ROM
    // table on a fixed target, and two u8s are a far cheaper call on 6502
    // than two copies of a six-byte Rect.
    //
    // `from == to` is the cold paint -- there is no previous indicator to
    // unpaint, so a hook that clears `from` and draws `to` does the right
    // thing on one tile without a second entry point. Pass() never calls it
    // that way (it only ever reports a real move); a scene putting the menu
    // on screen for the first time does.
    lines << QString("void DrawChange(u8 from, u8 to);");

    // Clamps `option` against the two direction bits of the menu's own axis --
    // UP/DOWN for a vertical list, LEFT/RIGHT for a row (see
    // kSingleChoiceVerticalRole). Only those two: the other axis is left
    // untouched so something outside can spend it, which is exactly what lets
    // a Canvas hold this menu and still move its own focus (see
    // genCanvasDecl). The clamp logic itself is what
    // ui::choice::SingleChoice::Pass used to carry as a template member.
    //
    // The ends clamp rather than wrap, same as a Canvas's focus and for the
    // same reason: a menu can build wrapping on top of clamping, but cannot
    // take it back out.
    //
    // Then it repaints, through DrawChange() above -- and ONLY on a frame
    // that actually moved: an end-stop press changes nothing, so there is
    // nothing to unpaint and redraw, and the `from == to` call it would make
    // is reserved for a cold paint. This is what makes the menu work when a
    // Canvas is what fed it (genCanvasDecl's Pass forwards the frame to
    // `components[index].func`): a Canvas's own DrawUpdate() fires on a focus
    // MOVE and knows nothing about a focused component stepping internally,
    // and Pass() returns void, so a caller outside could not even tell that
    // it had. Painting here means the menu behaves the same whether a Canvas
    // or the scene itself drove it, exactly as a Slider already does.
    //
    // `option` is read once into a local: it is `atomic` (volatile on NES),
    // and the body needs it three times.
    const QString scBack = scVertical ? QStringLiteral("input::UP") : QStringLiteral("input::LEFT");
    const QString scFwd = scVertical ? QStringLiteral("input::DOWN") : QStringLiteral("input::RIGHT");
    lines << QString("inline AI void Pass(const u8 inputs) {\n"
                      "    const u8 from = option;\n"
                      "\n"
                      "    if (inputs & %1) {\n"
                      "        if (from == 0) return;\n"
                      "        const u8 to = static_cast<u8>(from - 1);\n"
                      "        option = to;\n"
                      "        DrawChange(from, to);\n"
                      "        return;\n"
                      "    }\n"
                      "\n"
                      "    if (inputs & %2) {\n"
                      "        if (from == nOptions - 1) return;\n"
                      "        const u8 to = static_cast<u8>(from + 1);\n"
                      "        option = to;\n"
                      "        DrawChange(from, to);\n"
                      "        return;\n"
                      "    }\n"
                      "}")
                 .arg(scBack, scFwd);

    // The pointer hook. Takes a point in the same TILE space the `options`
    // table is in -- plane tiles from the world origin -- and reports whether
    // it landed on one of this menu's options.
    //
    // Tested against each OPTION's rect, not the menu's own derived bounds
    // (`rect` above): the bounds are the smallest box covering every option,
    // which on a menu with uneven label widths includes space no option
    // occupies, and a click there belongs to nothing. Per-option is also what
    // makes the hit meaningful at all -- WHICH option was clicked is the whole
    // answer, where a Button's hit is just "yes".
    //
    // A click selects outright rather than stepping towards, exactly as a
    // Slider's Click() lands the handle where the point is instead of nudging
    // it (see genSliderDecl). So there is no clamp and no end-stop here: every
    // index this can produce came from the table itself.
    //
    // DrawChange() only on a real move, the same rule ::Pass follows and for
    // the same reason -- `from == to` is reserved for a scene's cold paint,
    // and a click on the already-selected option has nothing to unpaint. It
    // still reports the hit, because the point WAS this menu's and a Canvas
    // must not go on offering it to the components after this one.
    //
    // On a runtime-viewport target `options` is `atomic Rect[]` (volatile),
    // which ::Rect::Contains cannot be called on, so each entry is copied into
    // a local through ::Rect's volatile copy constructor -- the same thing a
    // Canvas's Pass() does with its own components table. On a fixed-viewport
    // target the table is `constexpr Rect[]` and is indexed in place, for
    // genButtonDecl's Click() reason: a six-byte copy per option tested, to
    // ask about compile-time constants, is the one cost here that scales with
    // the option count.
    lines << (runtimePos
                  ? QString("inline AI bool Click(const vec2<u16> at) {\n"
                             "    const u8 from = option;\n"
                             "\n"
                             "    for (u8 i = 0; i < nOptions; i++) {\n"
                             "        const Rect r = options[i];\n"
                             "        if (!r.Contains(at)) continue;\n"
                             "        if (i == from) return true;\n"
                             "        option = i;\n"
                             "        DrawChange(from, i);\n"
                             "        return true;\n"
                             "    }\n"
                             "\n"
                             "    return false;\n"
                             "}")
                  : QString("inline AI bool Click(const vec2<u16> at) {\n"
                             "    const u8 from = option;\n"
                             "\n"
                             "    for (u8 i = 0; i < nOptions; i++) {\n"
                             "        if (!options[i].Contains(at)) continue;\n"
                             "        if (i == from) return true;\n"
                             "        option = i;\n"
                             "        DrawChange(from, i);\n"
                             "        return true;\n"
                             "    }\n"
                             "\n"
                             "    return false;\n"
                             "}"));

    // Draw()/Erase(): the whole option list's labels in one call. Each member's
    // own Draw()/Erase() only exists on the targets that don't hide it (see
    // generateCode), so a caller wanting "every option this target shows" would
    // otherwise need a per-target #if ladder that just restates the scene's
    // hide lists -- exactly the drift this scene format exists to remove. This
    // is labels only: the selection indicator and the call order relative to
    // anything else in the scene stay the caller's job. Emitted only when every
    // member is a ctTextBox (an rtTextBox has no Draw), and Erase() only when
    // every member also opted into Provide Erasing -- a partial Erase() would
    // silently leave stale tiles behind, so its absence (a compile error at the
    // call site) is the safer failure.
    bool allDrawable = true;
    bool allErasable = true;
    for (QTreeWidgetItem* member : members) {
        if (!isCtTextBoxItem(member)) {
            allDrawable = false;
            allErasable = false;
            break;
        }
        if (!member->data(0, kProvideErasingRole).toBool()) allErasable = false;
    }
    if (allDrawable) {
        QStringList drawBody;
        QStringList eraseBody;
        for (QTreeWidgetItem* member : members) {
            drawBody << QString("    %1::Draw();").arg(bareName(member));
            eraseBody << QString("    %1::Erase();").arg(bareName(member));
        }
        lines << QString("inline AI void Draw() {\n%1\n}").arg(drawBody.join("\n"));
        if (allErasable) {
            lines << QString("inline AI void Erase() {\n%1\n}").arg(eraseBody.join("\n"));
        }
    }

    return lines.join("\n") + "\n";
}


// Emits one Button node's declarations: its geometry (see genGeometryStorage),
// the Enable()/Disable() (and, if opted in, bool-returning Toggle()) functions
// that write its enabled bit, the Enabled() that reads it, and the Pass() that
// flips it. uitk provides no appearance of its own for a button -- only the
// forced-to-define Draw() hook hand-written code must supply: a button could
// look like anything from a 1x1 tickbox to a sprite, so there's no one
// appearance uitk could bake that would suit all of them, and no
// selected/unselected state of its own either -- enabled or disabled is all the
// memory this toolkit offers.
//
// ONE Draw(), not the DrawEnabled()/DrawDisabled() pair this used to emit with
// a generated dispatcher over it. A button's two states are two appearances of
// one widget, not two operations, and nearly every implementation of the pair
// shared a body that differed by which tile or attribute it wrote -- so the
// branch belongs inside the hook, off Enabled(), where it is written once. It
// also means the branch lives once in the out-of-line Draw() rather than being
// inlined into every call site, and leaves the scene free to force-inline its
// own definition (`AI inline void Draw()`, see ::AI in technology.hpp) exactly
// as it could the two hooks before.
//
// Leaving Draw() undefined is only ever caught at link time, and only once
// something actually calls it (directly, or transitively via Pass() or a
// Canvas's DrawComponents()) -- the closest thing to "forced by compile" this
// toolchain's codegen can offer without inventing a wholly new enforcement
// mechanism (see the uitk research notes on this: no existing node kind emits a
// declaration-only function).
//
// Everything here lands inside this node's own `namespace <name>`, so the verbs
// carry no `_<name>` suffix: `gen::options::reduceFlashesButton::Draw()`, not
// `gen::options::Draw_reduceFlashesButton()`. See emitNodeNamespace.
//
// kButtonHasBoxRole no longer gates anything in codegen: a Rect carries its
// footprint inseparably from its anchor, so size is always present now. The
// role is left on the node for the editor's own use.
//
// A button's enabled/disabled bit is never its own standalone variable --
// every button in the scene shares one `button_state` byte array (see
// generateCode, which sizes it to exactly fit however many buttons the
// scene actually has and assigns each one a fixed bit index before calling
// this), so `bitIndex`/`stateArrayName` say which byte and bit within that
// shared array belong to this particular button. That array lives at the
// scene's own namespace scope, one level up from (or several above) this
// node's, and is reached by ordinary enclosing-namespace lookup. One bool per
// button used to cost one whole byte (`atomic bool` is never smaller, and is
// often padded further); eight buttons now cost exactly one byte between them.
QString genButtonDecl(const QTreeWidgetItem* item, int ntOffX, int ntOffY, bool isNes, bool variadic,
                       bool runtimePos, QTreeWidgetItem* rootItem, const QString& target, const QString& region,
                       const QString& ntOffXExpr, const QString& ntOffYExpr, int bitIndex,
                       const QString& stateArrayName, const TargetGeometry& geom) {
    const bool toggleable = item->data(0, kButtonToggleableRole).toBool();
    const int byteIndex = bitIndex / 8;
    const int bitInByte = bitIndex % 8;
    const QString byteExpr = QString("%1[%2]").arg(stateArrayName).arg(byteIndex);
    const QString maskExpr = QString("(1u << %1)").arg(bitInByte);

    QStringList lines =
        genGeometryStorage(item, ntOffX, ntOffY, isNes, variadic, runtimePos, rootItem, target, region,
                            ntOffXExpr, ntOffYExpr, geom);

    lines << QString("inline AI void Enable() { %1 |= %2; }").arg(byteExpr, maskExpr);
    lines << QString("inline AI void Disable() { %1 &= static_cast<u8>(~%2); }").arg(byteExpr, maskExpr);
    if (toggleable) {
        // Not `return (state ^= mask) & mask;` -- reading the value of an
        // assignment whose left operand is volatile (`atomic` is `volatile` on
        // every C++ target, see technology.hpp) is deprecated in C++20 and
        // warns under -Wvolatile. The flipped value goes through a local
        // instead, which is the same two accesses to the state byte in the same
        // order, just without using the assignment's result.
        lines << QString("inline AI bool Toggle() {\n"
                          "    const u8 next = static_cast<u8>(%1 ^ %2);\n"
                          "    %1 = next;\n"
                          "    return next & %2;\n"
                          "}")
                     .arg(byteExpr, maskExpr);
    }
    // The button's own bit, as a value rather than a mutation -- the one thing
    // Enable()/Disable()/Toggle() could not previously give hand-written code.
    // Draw() below is defined by the scene and has to dispatch on the state
    // somehow, and `button_state` is a shared array indexed by a bit number
    // uitk assigns (see this function's doc comment), not something a hook
    // could reasonably reach into itself.
    //
    // `bool`, not the raw masked byte: the bit's position is uitk's business,
    // and `if (Enabled())` is the only thing a caller wants from it. Returns
    // the masked value directly, same as Toggle() does.
    lines << QString("inline AI bool Enabled() { return %1 & %2; }").arg(byteExpr, maskExpr);

    // Forced-to-define: no body here, deliberately -- see the doc comment
    // above. Hand-written game code must define this somewhere linked into the
    // final binary, and dispatch on Enabled() itself.
    lines << QString("void Draw();");

    // The input hook, and the reason the mutators above exist: one Pass(u8)
    // per interactive node, so a menu loop can hand the frame's input to
    // whichever component has focus without a per-kind call signature to
    // remember -- a SingleChoice's Pass(u8) (see genSingleChoiceDecl), a
    // slider's (genSliderDecl), and this.
    //
    // Generated, body and all, unlike the draw hooks above: a toggleable
    // button has exactly one thing it can do to the only state uitk owns for
    // it (that one bit), so there is nothing here left for hand-written code
    // to decide. What the press MEANS is still entirely hand-written -- it
    // lands in the Draw() this calls.
    //
    // Emitted ONLY for a toggleable button, alongside Toggle() itself. Press
    // is a flip and nothing else: a button that can't flip has no press this
    // tool could define for it -- "apply" (Enable() and leave it there) is a
    // guess about what the scene wants, and generating a plausible-looking
    // Pass() that turns out to be the wrong half of the setting is worse than
    // not having one. Calling Pass() on a non-toggleable button is therefore
    // a compile error naming the button, and driving it stays a matter of the
    // Enable()/Disable()/Draw() above.
    //
    // `payload` is the frame's input mask (input::Buttons, see
    // pnes/input.hpp), the newly-pressed bits rather than the held ones --
    // the same thing a scene already passes to a SingleChoice's Pass(). The
    // d-pad bits are masked off and a frame carrying only those returns
    // without doing anything: direction moves the focus BETWEEN components,
    // so a frame that only moved the cursor must not also press the button it
    // landed on. Everything else counts as a press -- A, B, START, SELECT --
    // because which of them a scene actually polls is the scene's business,
    // and a button that only answered to A would quietly break any menu
    // driven by another one.
    //
    // Then it acts: flip the bit, then repaint through the one hook there is.
    // Toggle() still returns the bit's new value, but nothing here can use it
    // -- Draw() reads the state itself (via Enabled()), so a press costs one
    // more read of that `atomic` (volatile on NES) byte than it did when there
    // were two hooks to dispatch between. That is the price of the scene having
    // one Draw() to write instead of two, and it is an LDA/AND on the only
    // target where it is measurable.
    if (toggleable) {
        // Toggle() returns the bit's NEW value, so the press and the repaint
        // share one read-modify-write of the shared state byte.
        lines << QString("inline AI void Pass(const u8 payload) {\n"
                          "    const u8 wanted = payload & static_cast<u8>(~(input::UP | input::DOWN |\n"
                          "        input::LEFT | input::RIGHT\n"
                          "    ));\n"
                          "\n"
                          "    if (!wanted) return;\n"
                          "    Toggle();\n"
                          "    Draw();\n"
                          "}");

        // The pointer hook, and the click half of ::Pass above. Takes a point
        // in the same TILE space this node's own `rect` is in -- plane tiles
        // from the world origin, which is what ::Rect::Contains compares
        // against -- and reports whether that point was this button's.
        //
        // A press on a button is a flip and nothing else, whichever device
        // made it (see ::Pass's own comment), so a hit does exactly what a
        // press does: Toggle() then the one Draw() there is. That is the whole
        // of why this can be generated at all, and why a slider's equivalent
        // cannot be the same shape -- there, WHERE in the rect the point
        // landed is the value (see genSliderDecl).
        //
        // Emitted only for a toggleable button, on the same condition and for
        // the same reason ::Pass is: a button that cannot flip has no press
        // this tool could define for it, and a click is still a press.
        //
        // The return value is what lets a Canvas stop at the first component
        // that claims the point instead of testing the rest (see
        // genCanvasDecl's PassClick), and it is the only thing that could
        // report a hit -- the state after a flip says nothing about whether
        // this button is the one that moved.
        //
        // On a runtime-viewport target `rect` is `atomic Rect` (volatile, see
        // genGeometryStorage) and ::Rect::Contains is a non-volatile member
        // that cannot be called on it, so the rect is copied into a local
        // first -- ::Rect has a volatile copy constructor for exactly this.
        //
        // On a fixed-viewport target it is a `constexpr Rect` and Contains is
        // called on it directly. Not merely tidier: ::Rect is six bytes, and
        // on the one target where that copy is real work (NES, which is always
        // fixed) it is six bytes onto the stack to ask a question about a
        // compile-time constant. The direct form folds the whole test against
        // literals instead.
        lines << (runtimePos
                      ? QString("inline AI bool Click(const vec2<u16> at) {\n"
                                 "    const Rect r = rect;\n"
                                 "    if (!r.Contains(at)) return false;\n"
                                 "    Toggle();\n"
                                 "    Draw();\n"
                                 "    return true;\n"
                                 "}")
                      : QString("inline AI bool Click(const vec2<u16> at) {\n"
                                 "    if (!rect.Contains(at)) return false;\n"
                                 "    Toggle();\n"
                                 "    Draw();\n"
                                 "    return true;\n"
                                 "}"));
    }

    return lines.join("\n") + "\n";
}


// Emits one Slider node's declarations: its geometry (see genGeometryStorage),
// the `amount` byte holding its current value, the two forced-to-define hooks
// that render it -- Draw() for the initial placement (at whatever `amount`
// already holds) and DrawChange(u8 amt) for moving a slider that is already on
// screen -- and the generated Pass(u8 payload) that turns a frame's input into
// a DrawChange() call.
//
// Everything lands inside this node's own `namespace <name>`, so these read as
// `gen::options::musicSlider::Draw(...)` / `::amount`. See emitNodeNamespace.
//
// Unlike a Button there is no Enable()/Disable()/Toggle() and no
// enabled/disabled pair of draw hooks -- a slider isn't a two-state thing,
// so there is nothing for a generated mutator to do that wouldn't be wrong
// for some slider. `amount` is storage and nothing else: uitk allocates it
// (prefixed, see `bssPrefixTok`) and never generates code that reads or writes
// it, since a slider's range, step, and clamping are properties of what it
// controls rather than of the scene.
//
// `bssPrefixTok` is already empty on every target but NES (see generateCode) --
// it places `amount`, which is the only storage a slider has that
// genGeometryStorage doesn't already own.
//
// A slider's footprint is never optional the way a Button's `hasBox` used to
// be: a slider's track length IS its range -- DrawSliderBody lays that many
// cells down, and the hooks clamp against the same width to know how far the
// handle can travel (demo/ui/impl/sliders.hpp) -- and a Rect carries it
// inseparably from the anchor regardless. Which dimension of that Rect is the
// track is the slider's own axis (kSliderVerticalRole): a vertical slider's
// hooks measure `size.y` where a horizontal one measures `size.x`. uitk emits
// no range either way -- the axis reaches the scene as the two input bits
// Pass() spends and as the `vertical` constant below, and what to measure
// against it stays the hooks' decision.
QString genSliderDecl(const QTreeWidgetItem* item, int ntOffX, int ntOffY, bool isNes, bool variadic,
                       bool runtimePos, QTreeWidgetItem* rootItem, const QString& target, const QString& region,
                       const QString& bssPrefixTok, const QString& ntOffXExpr, const QString& ntOffYExpr,
                       const TargetGeometry& geom) {
    const bool vertical = item->data(0, kSliderVerticalRole).toBool();

    QStringList lines =
        genGeometryStorage(item, ntOffX, ntOffY, isNes, variadic, runtimePos, rootItem, target, region,
                            ntOffXExpr, ntOffYExpr, geom);

    // The slider's holding value -- the one piece of state uitk does own for
    // one. `atomic` (technology.hpp: volatile on NES, a true atomic
    // elsewhere) for the same reason a SingleChoice's `option` is: whatever
    // moves it is rarely the same code that later reads it back to draw.
    // Carries bssPrefixTok so it lands in the scene's declared RAM section,
    // exactly like that `option`.
    //
    // uitk deliberately emits nothing else around it -- no range, no step,
    // no clamping, no setter. A slider's bounds are a property of what it is
    // actually controlling (a volume in 0..15, a speed in 0..3, a raw 0..255
    // byte), which the scene has no way to know, so the only thing that
    // would be right for every slider is the storage itself. Writing and
    // bounding it is the caller's job.
    lines << QString("%1inline atomic u8 amount = 0;").arg(bssPrefixTok);

    // Which axis the handle moves along (kSliderVerticalRole). The generated
    // Pass() below already bakes it into the two input bits it spends, so this
    // is here for the hooks uitk does NOT write: Draw() and DrawChange() have
    // to know whether the track runs along `rect.size.x` or down
    // `rect.size.y`, and whether stepping a cell means +1 or +<row stride>.
    // Emitting it keeps that decision sourced from the scene instead of
    // duplicated as a literal in demo/ui/impl/sliders.hpp, where it could
    // drift from what the editor says. `constexpr`, so it addresses nothing
    // and costs no ROM.
    lines << QString("inline constexpr bool vertical = %1;")
                 .arg(vertical ? QLatin1String("true") : QLatin1String("false"));

    // Forced-to-define, same as a Button's Draw() -- hand-written game code
    // must define both of these somewhere linked
    // into the final binary. They're split because putting a slider on
    // screen and moving one already on screen are different amounts of
    // work, not two cases of one operation: Draw() lays down the whole
    // track (both end caps and every cell between them, the full `rect.size`
    // footprint) and then puts the handle at `amount`, whereas
    // DrawChange() touches exactly two cells -- it restores the one the
    // handle is leaving and marks the one it's arriving at. That's the
    // DrawSliderBody/DrawSliderHandle split in demo/ui/impl/sliders.hpp, and
    // the reason a scene can't just call Draw() again on every nudge: a full
    // redraw per frame is a whole row of PPU writes where two would do.
    //
    // Only DrawChange() takes anything, and only the amount being moved TO.
    // Draw() takes nothing: `amount` is already the byte uitk allocated for
    // this slider, so a starting position is something the caller writes
    // there (it is zero-initialised otherwise) rather than hands in -- same
    // arrangement a Button has, where Draw() dispatches off the state uitk
    // owns instead of being told it. That also means a scene can repaint a
    // slider it has lost the tiles for (a nametable reload, a mode re-entry)
    // without having kept the value itself.
    //
    // DrawChange()'s one parameter is the new value because moving a handle
    // needs both cells and only one of them is in storage: the hook reads the
    // old position out of `amount`, clears that cell, stores the new value,
    // and draws the cell it now names (Clear/assign/Draw -- see
    // demo/ui/impl/sliders.hpp). Dropping the parameter too would cost a
    // second byte to remember the last-painted position, which is why the
    // value being moved to stays an argument.
    lines << QString("void Draw();");
    lines << QString("void DrawChange(u8 amt);");

    // The third forced-to-define hook, and the only one a POINTER needs: which
    // cell of this slider's own footprint was clicked, counted from `rect.pos`
    // along the slider's axis -- 0 for the first, `rect.size - 1` for the last.
    //
    // A cell and not an `amount`, which is the whole reason this exists rather
    // than Click() calling DrawChange() directly. `amount` is a position in
    // whatever the implementation's HANDLE can occupy, and that is not the same
    // span as the rect: an implementation that caps its track draws an end
    // graphic at each extreme and lets the handle travel only between them (see
    // ui::impl::SliderTravel and DrawSliderBody, demo/ui/impl/sliders.hpp), so
    // its `amount` 0 sits one cell INSIDE the rect and its last amount one cell
    // short of the far edge. uitk cannot know that inset. It is a property of
    // the graphics, it differs per implementation -- a capless track would have
    // none at all -- and inventing one here would silently land every click a
    // cell off on exactly the layout this toolkit's own demo uses.
    //
    // So this hands over the one thing uitk does know for certain, the cell the
    // point fell in, and the implementation maps it onto its own range. That
    // mapping is also where a click on a cap gets to mean something: a cell the
    // handle cannot occupy is still a direction to travel in, and the nearest
    // position to it is the natural answer (ui::impl::SliderAmountForCell).
    //
    // Which is the other half of why this is not DrawChange(): a PRESS past the
    // end must do nothing (Pass() leans on DrawChange's own guard to reject
    // it), while a CLICK past the end should approach it. Two different answers
    // to an out-of-range value, so two different entry points.
    lines << QString("void DrawClick(u8 cell);");

    // The input hook -- the same uniform entry point a Button and a
    // SingleChoice get (see genButtonDecl), so a menu loop can route the
    // frame's input to whatever has focus without a per-kind signature to
    // remember.
    //
    // Generated, body and all, unlike the two draw hooks above. The step is
    // one cell per press and isn't configurable, because a slider's track
    // length IS its range (see the `amount` comment above): the handle can
    // only ever sit on a cell, so one cell is the only step that both moves
    // it and lands somewhere it can be drawn. A control wanting 0..255 out of
    // a 16-cell track scales what it reads out of `amount`, rather than
    // stepping it by 16 and skipping fifteen handle positions.
    //
    // No clamping here either: DrawChange() already rejects anything past the
    // track, and u8 arithmetic can only leave the range by one in each
    // direction (0 - 1 wraps to 255, size.x + 1 is one past the end), so both
    // ends land in the guard it has to apply anyway. That also means the ends
    // behave the same whether a nudge came through here or straight from a
    // caller's own DrawChange().
    //
    // `payload` is the frame's input mask (input::Buttons, see
    // pnes/input.hpp), the newly-pressed bits. Only the two bits of the
    // slider's own axis are spent (kSliderVerticalRole) -- LEFT/RIGHT for a
    // track along a row, UP/DOWN for one down a column -- and the other pair
    // is left untouched for something outside to use. That is what lets a
    // Canvas hold a slider and still move its own focus, in either
    // orientation: a vertical Canvas wants a horizontal slider, a horizontal
    // Canvas a vertical one (see genCanvasDecl). `amount` is read once into a
    // local -- it is `atomic` (volatile on NES), and DrawChange() reads it
    // again itself.
    const QString slBack = vertical ? QStringLiteral("input::UP") : QStringLiteral("input::LEFT");
    const QString slFwd = vertical ? QStringLiteral("input::DOWN") : QStringLiteral("input::RIGHT");
    lines << QString("inline AI void Pass(const u8 payload) {\n"
                      "    const u8 cur = amount;\n"
                      "\n"
                      "    if      (payload & %1) DrawChange(static_cast<u8>(cur + 1));\n"
                      "    else if (payload & %2) DrawChange(static_cast<u8>(cur - 1));\n"
                      "}")
                 .arg(slFwd, slBack);

    // The pointer hook. Takes a point in the same TILE space this node's own
    // `rect` is in -- plane tiles from the world origin, what ::Rect::Contains
    // compares against -- and reports whether that point was this slider's.
    //
    // A click on a slider is NOT the press ::Pass spends. A press is a nudge
    // and carries only a direction, so it steps one cell; a click names a cell
    // outright, so the handle goes THERE. That is the whole difference between
    // the two hooks, and it is why this one cannot forward to Pass() the way a
    // Button's Click() can forward to a flip.
    //
    // The cell is the offset of the point along the slider's own axis
    // (kSliderVerticalRole): a track running along a row measures `at.x` from
    // `rect.pos.x`, one down a column `at.y` from `rect.pos.y` -- the same
    // dimension `vertical` tells a hook to measure, and the same dimension
    // Pass() bakes its two input bits from.
    //
    // No clamp, and none needed, which is the point of testing Contains first:
    // a point inside the rect is at most `size - 1` cells along it and at
    // least 0, so the subtraction cannot leave the track in either direction.
    // DrawChange()'s own guard (demo/ui/impl/sliders.hpp) is therefore never
    // what stops this one, unlike the +1/-1 in Pass() which deliberately
    // leans on it.
    //
    // `amount` is not read here at all, and not written either: DrawChange()
    // is the hook that stores the new value (see the `amount` comment above),
    // exactly as it is when Pass() calls it.
    //
    // The cell is the offset of the point along the slider's own axis
    // (kSliderVerticalRole) from the rect's own anchor: a track running along a
    // row measures `at.x` from `rect.pos.x`, one down a column `at.y` from
    // `rect.pos.y`. Testing Contains() first is what bounds it -- a point
    // inside the rect is at most `size - 1` cells along it and at least 0, so
    // the subtraction cannot leave the footprint and ::DrawClick is never
    // handed a cell this slider does not have.
    //
    // Note this is NOT an `amount`, and the unit change is the point: see
    // ::DrawClick's own comment above for why uitk hands over a cell and lets
    // the implementation place the handle.
    //
    // On a runtime-viewport target `rect` is `atomic Rect` (volatile), and
    // both ::Rect::Contains and the `pos` read are non-volatile members -- so
    // it is copied into a local once, through ::Rect's volatile copy
    // constructor, and that one copy serves both uses. On a fixed-viewport
    // target it is a `constexpr Rect` used in place, for the reason
    // genButtonDecl's Click() spells out: the copy would be six bytes of stack
    // to ask about a compile-time constant.
    const QString slAxis = vertical ? QStringLiteral("y") : QStringLiteral("x");
    lines << (runtimePos
                  ? QString("inline AI bool Click(const vec2<u16> at) {\n"
                             "    const Rect r = rect;\n"
                             "    if (!r.Contains(at)) return false;\n"
                             "    DrawClick(static_cast<u8>(at.%1 - r.pos.%1));\n"
                             "    return true;\n"
                             "}").arg(slAxis)
                  : QString("inline AI bool Click(const vec2<u16> at) {\n"
                             "    if (!rect.Contains(at)) return false;\n"
                             "    DrawClick(static_cast<u8>(at.%1 - rect.pos.%1));\n"
                             "    return true;\n"
                             "}").arg(slAxis));

    return lines.join("\n") + "\n";
}


// Whether `item` -- known to be a Canvas component (see hasGeneratedPass) --
// has a nullary Draw() in its own namespace on this target.
//
// A Slider always does (forced-to-define, see genSliderDecl) and so does a
// Button (generated, dispatching off its state bit, see genButtonDecl). A
// SingleChoice only does when every option it shows is a ctTextBox: an
// rtTextBox's text isn't known until runtime, so there is nothing for uitk to
// draw, and that menu's own Draw() is omitted rather than half-emitted (see
// genSingleChoiceDecl). This is what decides whether a Canvas can offer
// DrawComponents() at all.
bool componentHasDraw(QTreeWidgetItem* item, const QString& target, const QString& region) {
    if (isSingleChoiceItem(item)) {
        const QVector<QTreeWidgetItem*> members = singleChoiceMembers(item, target, region);
        if (members.isEmpty()) return false;
        for (QTreeWidgetItem* member : members) {
            if (!isCtTextBoxItem(member)) return false;
        }
        return true;
    }
    return isSliderItem(item) || isButtonItem(item);
}

// Emits one Canvas node's own declarations: the `components` table over the
// interactable nodes nested under it, the `index` holding which of them has
// focus, the Make() that drives its children, the forced-to-define
// DrawUpdate() that repaints a focus move and DrawComponentSelection() that
// paints one, the DrawComponents() that paints every component in one call,
// and the generated Pass() that turns a frame's input into either a focus move
// or a call into the focused component. Its children's own namespaces are emitted separately,
// immediately BEFORE this, by emitNodeNamespace -- which is what lets the
// table below name them.
//
// A Canvas has no geometry and no appearance: it is a focus ring over the
// Sliders, toggleable Buttons and SingleChoice menus nested under it (see
// hasGeneratedPass), and the only state it owns is `index`. That is deliberately the whole of it -- a Canvas
// never draws, never positions anything, and never changes what its children
// are. Nesting a Button under one is exactly the same Button, with the same
// rect and the same Pass(); the Canvas just decides when that Pass() is the
// one input reaches.
//
// Never returns empty, unlike genSingleChoiceDecl: a Canvas with nothing
// focusable in it still has children to make, so it still gets a namespace and
// a Make(). What it loses is the table, the index, DrawUpdate(),
// DrawComponentSelection(), DrawComponents() and Pass() -- there is no focus to hold or move, and nothing
// in the ring to draw.
//
// There is deliberately no Erase() counterpart to DrawComponents(). A Canvas
// exists to hold more UI than a scene tends to clear in one go, and only a
// SingleChoice has an Erase() at all (and only when every option opted into
// Provide Erasing) -- a canvas-wide one could not be built out of parts that
// mostly don't have it.
//
// `bssPrefixTok` is empty on every target but NES (see generateCode), and
// places `index` in the scene's declared RAM section.
//
// There is deliberately no dataPrefixTok counterpart for the components table,
// which every other generated constant takes. The table is the one piece of a
// scene's data that is read by the scene's DRIVER rather than by the scene's
// own code: a menu loop calls Pass(), Pass() indexes the table, and that loop
// lives wherever the mode that owns it lives. On NES the scene's data section
// is a switchable bank, so a table pinned there is only readable while that
// bank happens to be mapped -- and nothing in a generated scene maps it, or
// could know when to. The entries are not scene content either; they are
// pointers INTO it (see InvokeableComponant). So the table goes wherever the
// target's ordinary constant data goes, which is resident, and the bank a
// scene declares stays for the things that scene's own code reads.
QString genCanvasDecl(QTreeWidgetItem* canvasItem, const QString& target, const QString& region,
                       const QString& bssPrefixTok) {
    const QVector<QTreeWidgetItem*> comps = canvasComponents(canvasItem, target, region);
    const int size = comps.size();
    const bool vertical = canvasItem->data(0, kCanvasVerticalRole).toBool();

    QStringList lines;

    if (size > 0) {
        // The indexable view over the components: each entry is a pointer to
        // the node's own rect and a pointer to the node's own Pass(), so the
        // table is a compile-time constant on every target and never a second
        // copy of anything (see InvokeableComponant). `size` is named rather
        // than baked into the subscript so a caller -- and the Pass() below --
        // can bound-check against the same constant the table was built from.
        lines << QString("inline constexpr u8 size = %1;").arg(size);
        QStringList entries;
        for (QTreeWidgetItem* comp : comps) {
            entries << QString("    {&%1::rect, &%1::Pass},").arg(bareName(comp));
        }
        lines << QString("inline constexpr InvokeableComponant components[size] {\n%1\n};")
                     .arg(entries.join("\n"));

        // Which component has focus. `atomic` (technology.hpp: volatile on
        // NES, a true atomic elsewhere) for the same reason a SingleChoice's
        // `option` is -- whatever moves the focus is rarely the same code that
        // later reads it back -- and under bssPrefixTok so it lands in the
        // scene's declared RAM section, exactly like that `option`.
        //
        // Starts at 0: the first component in document order. There is no
        // editor knob for a starting index the way a SingleChoice has a
        // default option, because a Canvas's component list is filtered per
        // target (a hidden control is not in it, see canvasComponents), so an
        // authored index would mean a different control on different targets.
        lines << QString("%1inline atomic u8 index = 0;").arg(bssPrefixTok);

        // Which axis moves the focus (kCanvasVerticalRole), same arrangement
        // as a Slider's or a SingleChoice's own `vertical`: Pass() has the
        // axis baked in, and this is the value the hand-written side can read
        // -- a DrawUpdate() that slides a cursor sprite along the ring, or a
        // static_assert that a nested component disagrees with this Canvas
        // about the axis (agreement compiles, but leaves that component
        // unadjustable -- see hasGeneratedPass). Emitted only where it means
        // something: a Canvas with nothing focusable has no Pass() and no
        // focus to move. `constexpr`, so it costs no ROM.
        lines << QString("inline constexpr bool vertical = %1;")
                     .arg(vertical ? QLatin1String("true") : QLatin1String("false"));

        // Forced-to-define, like every other draw hook uitk emits: moving
        // focus has to unpaint one component and paint another, and what that
        // looks like is entirely the scene's business (an attribute change, a
        // cursor sprite, a palette swap -- see ui::impl::UpdateCanvas in
        // demo/ui/impl/canvas.hpp). uitk knows only the two rects involved.
        //
        // Takes plain `const Rect&`s, not the `const volatile Rect*`s the
        // table holds: Pass() copies each out into a local first (::Rect has a
        // volatile copy constructor for exactly this), so the hook gets a
        // settled value it can read as many times as it likes rather than
        // storage that is re-read on every access.
        lines << QString("void DrawUpdate(const Rect& last, const Rect& current);");

        // The other half of that, and the only half some indicators have:
        // paint the selection HERE. Forced-to-define as well, and taking one
        // rect -- the component that now holds the focus -- because an
        // indicator that need not unpaint anything (a sprite, which has one
        // position, moves with a store) has nothing to do with a `last`, and
        // on a cold paint there is no `last` to have.
        //
        // Nothing generated calls it, deliberately. Pass() reports MOVES, and
        // only moves, through DrawUpdate(); DrawComponents() paints no focus at
        // all (see its own comment). So putting the first selection on screen
        // stays the scene's call, and this is the entry point for it -- as well
        // as the natural thing for a DrawUpdate() to forward to once it has
        // dealt with `last`, which is what keeps the two hooks from being two
        // copies of one appearance.
        //
        // Emitted beside DrawUpdate rather than in place of it, and on the
        // same condition, so a single-component Canvas gets one too: there
        // focus cannot move and Pass() never calls DrawUpdate at all, but there
        // is still a selection to show.
        lines << QString("void DrawComponentSelection(const Rect& current);");
    }

    // Children first, then the focus seed, same contract as every other
    // Make(): a parent always makes its children (see childNamespaceNames).
    // `index` is re-seeded here rather than only in its initializer so that
    // making a scene twice puts the focus back where the scene starts.
    QStringList makeBody;
    for (const QString& kid : childNamespaceNames(canvasItem, target, region)) {
        makeBody << QString("    %1::Make();").arg(kid);
    }
    if (size > 0) makeBody << QString("    index = 0;");
    lines << (makeBody.isEmpty() ? QString("inline AI void Make() {}")
                                  : QString("inline AI void Make() {\n%1\n}").arg(makeBody.join("\n")));

    if (size == 0) {
        return lines.join("\n") + "\n";
    }

    // Paints the whole focus ring in one call: every component's own Draw(), in
    // the table's order, which is the scene's order.
    //
    // Make() first, always. A component's Draw() reads its own `rect`/`pos`,
    // and on a runtime-viewport target those are storage that Make() fills
    // (genGeometryStorage) -- drawing before making paints at whatever address
    // zero-initialised storage names. Make() already makes every child, so
    // "Make() then DrawComponents()" is the whole entry sequence for a Canvas.
    //
    // Unrolled direct calls, not a walk over `components[]`: that table carries
    // rects and Pass() only, and giving it a Draw() pointer per entry would cost
    // ROM AND force an out-of-line copy of every component's Draw -- the same
    // price InvokeableComponant's own comment describes for &Pass, except here
    // there is nothing to buy with it. The call order is known at compile time,
    // so direct calls let each AI'd body inline exactly as a hand-written
    // sequence would.
    //
    // What a component's Draw() covers is that component's business, not this
    // one's. A SingleChoice draws every option it shows (genSingleChoiceDecl);
    // a Slider draws its track and handle; a Button's own hand-written Draw()
    // dispatches on Enabled() to paint whichever state it is in -- which is
    // exactly why this does NOT also walk each component's children. A
    // button's children are typically its two alternative labels, stacked at one
    // position (demo/ui/options.uis: DISABLE MUSIC / ENABLE MUSIC), so drawing
    // them all would paint one over the other; choosing between them is what
    // those two hooks exist for, and a child's namespace is nested INSIDE its
    // parent's (gen::options::reduceFlashesButton::reduceFlashesText), so a
    // hook can call `<label>::Draw()` unqualified from the parent's own scope.
    //
    // Omitted entirely -- no empty stub -- when any component has no Draw() of
    // its own to call (see componentHasDraw). A partial DrawComponents() that
    // quietly skipped a menu would be worse than its absence, which is a compile
    // error at the call site naming this Canvas.
    //
    // Nothing here paints focus. Make() seeds `index` to 0 but DrawUpdate() is
    // the hook that makes a component look focused, and its contract is the two
    // rects of a MOVE -- so the initial highlight stays the scene's call, next
    // to the rest of its first-frame drawing.
    bool allComponentsDrawable = true;
    for (QTreeWidgetItem* comp : comps) {
        if (!componentHasDraw(comp, target, region)) {
            allComponentsDrawable = false;
            break;
        }
    }
    if (allComponentsDrawable) {
        QStringList drawBody;
        for (QTreeWidgetItem* comp : comps) {
            drawBody << QString("    %1::Draw();").arg(bareName(comp));
        }
        lines << QString("inline AI void DrawComponents() {\n%1\n}").arg(drawBody.join("\n"));
    }

    // The input hook, and the point of the whole node: one Pass(u8) that a
    // menu loop can hand a frame's input to without knowing which control is
    // focused, or even what kind of control it is. `payload` is the
    // newly-pressed bits (input::Buttons, see pnes/input.hpp), the same
    // convention a Button, a Slider and a SingleChoice already take.
    //
    // A direction along the Canvas's own axis moves the focus and stops there
    // -- it is never also delivered to a component, because the press that
    // leaves a control is not a press that control should act on. Everything
    // else goes straight through to the focused component's own Pass(), which
    // is what lets a Canvas hold a Slider or a SingleChoice: a vertical Canvas
    // spends UP/DOWN on focus and hands LEFT/RIGHT to a horizontal component, a
    // horizontal Canvas spends LEFT/RIGHT and hands UP/DOWN to a vertical one
    // (see kSliderVerticalRole/kSingleChoiceVerticalRole). A Canvas whose
    // component agrees with it about the axis wins that axis and leaves the
    // component unadjustable -- a property of the layout, and the author's
    // call, not something uitk overrules.
    //
    // The ends don't wrap. A focus ring that jumps from the last control back
    // to the first is a different control scheme, not a better one, and it is
    // the one a scene cannot undo: clamping leaves a menu free to treat the
    // ends as "nothing happened" (as this does) or to hand UP at the top to
    // something else entirely. SingleChoice's Pass() clamps for the same
    // reason.
    //
    // `index` is read once into a local: it is `atomic` (volatile on NES), so
    // every mention of it would otherwise be a fresh load, and the body needs
    // it three times.
    if (size == 1) {
        // One component means there is nowhere for focus to move, so neither
        // direction bit is special and the whole press belongs to that
        // component. Emitted as its own shape rather than left to the general
        // body below, whose `index == size - 1` guard would be a comparison
        // against zero that is always true -- dead code the compiler is right
        // to warn about.
        lines << QString("inline AI void Pass(const u8 payload) {\n"
                          "    components[0].func(payload);\n"
                          "}");

        // The pointer half of the same thing, and the same shortcut: with one
        // component there is nowhere for focus to move, so a hit needs no
        // ::Focus call and the whole hook is that component's own Click().
        // `index` is 0 and cannot be anything else -- Make() seeds it there
        // and no Pass() above ever leaves it.
        lines << QString("inline AI bool PassClick(const vec2<u16> at) {\n"
                          "    return %1::Click(at);\n"
                          "}")
                     .arg(bareName(comps[0]));
        return lines.join("\n") + "\n";
    }

    const QString back = vertical ? QStringLiteral("input::UP") : QStringLiteral("input::LEFT");
    const QString fwd = vertical ? QStringLiteral("input::DOWN") : QStringLiteral("input::RIGHT");
    lines << QString("inline AI void Pass(const u8 payload) {\n"
                      "    const u8 from = index;\n"
                      "\n"
                      "    if (payload & %1) {\n"
                      "        if (from == 0) return;\n"
                      "        index = static_cast<u8>(from - 1);\n"
                      "        const Rect last = *components[from].rect;\n"
                      "        const Rect current = *components[from - 1].rect;\n"
                      "        DrawUpdate(last, current);\n"
                      "        return;\n"
                      "    }\n"
                      "\n"
                      "    if (payload & %2) {\n"
                      "        if (from == size - 1) return;\n"
                      "        index = static_cast<u8>(from + 1);\n"
                      "        const Rect last = *components[from].rect;\n"
                      "        const Rect current = *components[from + 1].rect;\n"
                      "        DrawUpdate(last, current);\n"
                      "        return;\n"
                      "    }\n"
                      "\n"
                      "    components[from].func(payload);\n"
                      "}")
                 .arg(back, fwd);

    // Moves the focus to a component chosen by INDEX, and repaints the ring
    // that marks it.
    //
    // What ::Pass does on a direction press, minus the direction: Pass knows
    // focus can only go one step either way and bakes that in, while a pointer
    // can land anywhere, so the move has to be expressible as a destination.
    // Both go through the same DrawUpdate() hook with the same two rects, so
    // the ring cannot tell which drove it.
    //
    // Early-outs when the destination already has focus. That is not just an
    // optimisation: DrawUpdate()'s contract is a MOVE, and an indicator that
    // unpaints `last` before painting `current` (see genCanvasDecl's own
    // DrawUpdate comment) would be handed the same rect twice and could blank
    // what it then redraws. A scene wanting a cold paint has
    // DrawComponentSelection() for it.
    //
    // Not bounds-checked, deliberately: nothing outside this namespace is
    // meant to call it, and the one caller below passes a literal index from
    // the generated chain. A `to >= size` guard would be dead code on every
    // scene uitk emits.
    lines << QString("inline AI void Focus(const u8 to) {\n"
                      "    const u8 from = index;\n"
                      "    if (from == to) return;\n"
                      "\n"
                      "    index = to;\n"
                      "    const Rect last = *components[from].rect;\n"
                      "    const Rect current = *components[to].rect;\n"
                      "    DrawUpdate(last, current);\n"
                      "}");

    // The pointer hook, and the counterpart to ::Pass: one call a scene can
    // hand a clicked point to without knowing which control is under it, or
    // what kind of control it is.
    //
    // @p at is in TILE space -- plane tiles from the world origin, the space
    // every component's `rect` is in. A caller holding a viewport pixel (what
    // ::input::click reports) puts it there by adding the scroll its scene is
    // read under and dividing by the tile size; the scroll is the scene's and
    // cannot be known here (see demo/ui/impl/canvas.hpp's CanvasScroll).
    //
    // Unrolled direct calls, not a walk over `components[]`: that table
    // carries rects and Pass() only, and giving it a Click() pointer per entry
    // would cost ROM and force an out-of-line copy of every component's
    // Click -- the same trade DrawComponents() already describes, and here
    // there is likewise nothing to buy with it. Each component's Click() tests
    // its own rect and so needs no entry in any table to be reachable.
    //
    // First claim wins, and the rest are not tested. Two components whose
    // rects overlap is a layout the editor permits and this resolves in
    // document order -- the same order ::Pass walks focus in, so a click
    // behaves like the press the player could have reached that control with.
    //
    // ACTS FIRST, then moves the focus. A component's Click() has already
    // repainted itself by the time ::Focus runs, which is the order an
    // indicator that paints into the scene needs: the ring goes on over the
    // component's own fresh tiles, where the reverse order would let the
    // component's Draw() wipe it. For the demo's sprite ring the order is
    // immaterial -- both land before the one OAM upload that frame -- and it
    // costs nothing to be right for both.
    //
    // Reports whether ANY component took the point, so a scene can tell a
    // click that hit this Canvas from one that landed in the gaps between its
    // controls (the row a focus ring needs, the padding inside the Canvas's
    // own rect) or on a different Canvas entirely. A scene with two of them
    // offers the point to each in turn and stops at the first that says yes.
    {
        QStringList clickBody;
        for (int i = 0; i < comps.size(); ++i) {
            clickBody << QString("    if (%1::Click(at)) { Focus(%2); return true; }")
                             .arg(bareName(comps[i]))
                             .arg(i);
        }
        lines << QString("inline AI bool PassClick(const vec2<u16> at) {\n%1\n\n    return false;\n}")
                     .arg(clickBody.join("\n"));
    }

    return lines.join("\n") + "\n";
}

// Builds the target-specific .hpp/.cpp pair for `rootItem`'s whole scene.
// Components hidden on `target`/`region` (or under a hidden ancestor -- see
// isEffectivelyHidden) are skipped entirely, so an export never emits a
// function for something the scene itself says shouldn't exist there.
GeneratedFiles generateCode(QTreeWidgetItem* rootItem, const QString& target, const QString& region, int quadrant,
                             const QString& linkerPrefix, const QString& bssPrefix, const QString& dataPrefix,
                             const QString& charmap, const QString& sceneName) {
    // Every node's geometry, resolved for THIS target rather than read back out
    // of the editor's cached roles, which hold the target the scene was last
    // authored on (see TargetGeometry). Computed once here and handed to every
    // emitter below, so one export is one resolution.
    const TargetGeometry geom = resolveGeometryForTarget(rootItem, target, region);

    const int ntOffX = (quadrant & 1) * kNametableQuadW;
    const int ntOffY = ((quadrant >> 1) & 1) * kNametableQuadH;
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
    const bool variadic = kVariadicTargets.contains(target);
    // Whether this target's Rect can be `constexpr` storage at all -- see
    // kRuntimePosTargets. Strictly narrower than `variadic`: DS/DSI/3DS/Switch
    // emit runtime VIEWPORT_* expressions like any variadic target, but those
    // expressions fold, so their Rect is still a compile-time constant.
    const bool runtimePos = kRuntimePosTargets.contains(target);
    // Runtime counterpart of ntOffX/ntOffY, for a variadic target's own
    // Draw_/Erase_/Make_ bodies (genCtTextBoxBody/genCtTextBoxEraseBody/
    // genSingleChoiceDecl): on a variadic target one nametable quadrant is
    // that target's own runtime viewport width/height -- floored at the
    // NES-native 32x30 minimum, matching ::emu::ComputeNtGeometry
    // (src/emu/emu.hpp) and ::CartesianToAddress's internal xy_to_nt_addr
    // (src/emu/ppu.cpp) -- NOT the fixed kNametableQuadW/H literal ntOffX/
    // ntOffY themselves bake in, which is only correct for a target whose
    // panel really is a fixed 32x30 (NES, and every non-variadic target
    // here). Baking that same fixed literal into a variadic target's
    // generated expression is exactly the "assumes 32 tiles" bug this
    // exists to avoid -- see title.cpp's own kMenuNT for the hand-written
    // version of the identical fix. "0" (not emitted as a call at all) when
    // this scene's quadrant index doesn't need an offset on that axis.
    const QString ntOffXExpr = !(quadrant & 1)             ? QStringLiteral("0")
                                : variadic                    ? QStringLiteral("(video::viewport_tx() < 32 ? 32 : video::viewport_tx())")
                                                               : QString::number(kNametableQuadW);
    const QString ntOffYExpr = !((quadrant >> 1) & 1)      ? QStringLiteral("0")
                                : variadic                    ? QStringLiteral("(video::viewport_ty() < 30 ? 30 : video::viewport_ty())")
                                                               : QString::number(kNametableQuadH);
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
    // Whether anything in this scene has a generated Pass() -- i.e. whether
    // the emitted header names input:: at all, and so needs pnes/input.hpp.
    bool usesInput = false;
    // Every charmap_ function actually referenced by this export (default
    // and/or per-node overrides), so the in-scope note below (charmapNote)
    // covers all of them, not just the scene default.
    QSet<QString> usedCharmapFns;

    // Every visible Button in the scene, in document order -- fixes each
    // one's bit index into the shared `button_state` array below before any
    // button's own Enable_/Disable_/Draw_ (see genButtonDecl) gets emitted,
    // since they all share one array sized to exactly fit them all.
    static const QString kButtonStateArrayName = QStringLiteral("button_state");
    QVector<QTreeWidgetItem*> visibleButtons;
    for (QTreeWidgetItem* item : collectComponentItems(rootItem)) {
        if (isButtonItem(item) && !isEffectivelyHidden(item, target, region)) {
            visibleButtons.push_back(item);
        }
    }
    QHash<const QTreeWidgetItem*, int> buttonBitIndex;
    for (int i = 0; i < visibleButtons.size(); ++i) {
        buttonBitIndex[visibleButtons[i]] = i;
    }
    if (!visibleButtons.isEmpty()) {
        const int byteCount = (visibleButtons.size() + 7) / 8;
        QVector<unsigned> byteDefaults(byteCount, 0u);
        for (int i = 0; i < visibleButtons.size(); ++i) {
            if (visibleButtons[i]->data(0, kButtonDefaultEnabledRole).toBool()) {
                byteDefaults[i / 8] |= (1u << (i % 8));
            }
        }
        QStringList byteLiterals;
        for (unsigned b : byteDefaults) {
            byteLiterals << QString("0x%1").arg(b, 2, 16, QLatin1Char('0'));
        }
        hppDecls << QString("// Every Button's enabled/disabled bit, packed into the fewest bytes that fit\n"
                             "// them all -- one bit per button, in the order they appear in the scene.\n"
                             "inline atomic u8 %1[%2] = {%3};\n")
                        .arg(kButtonStateArrayName)
                        .arg(byteCount)
                        .arg(byteLiterals.join(", "));
    }

    // One recursive walk, emitting a nested `namespace <nodeName>` per node so a
    // generated symbol's qualified name retraces the scene tree that produced
    // it: gen::options::reduceFlashesButton::reduceFlashesText::Draw(). Plain
    // branch nodes (kind "branch") are transparent -- they carry no prefix, so
    // their names were never validated as C++ identifiers and can't safely
    // become namespaces; their children are emitted at the branch's own level.
    //
    // Children are emitted BEFORE their parent's own declarations, because a
    // parent can name them (a SingleChoice's Draw() calls each member's Draw())
    // while the reverse never happens -- a child's placement expression that
    // references its parent is spliced in textually by emitGeomComponent, not
    // resolved through a symbol.
    //
    // `makeAll` collects the qualified name of every node the scene-level
    // Make() should drive, in document order -- every node, with no exceptions.
    // A SingleChoice's members are listed here even though its own Make() makes
    // them too (see genSingleChoiceDecl), so they are made twice. That is
    // deliberate: a parent's Make() always calls its children's, unconditionally.
    // Making geometry twice is a handful of stores; the alternative -- a
    // made/not-made flag per node -- costs permanent storage, a branch on every
    // Make(), and a state that can go stale if a caller makes a subtree
    // directly. Redundant work is the cheaper and the safer of the two.
    QStringList makeAll;
    std::function<QStringList(QTreeWidgetItem*, const QString&)> emitNodeNamespace =
        [&](QTreeWidgetItem* node, const QString& qualPrefix) -> QStringList {
        QStringList emitted;
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if (!isPrefixedItem(child)) {
                emitted += emitNodeNamespace(child, qualPrefix);  // transparent branch
                continue;
            }
            if (isEffectivelyHidden(child, target, region)) continue;

            const QString name = bareName(child);
            const QString qual = qualPrefix.isEmpty() ? name : (qualPrefix + "::" + name);

            QStringList body = emitNodeNamespace(child, qual);

            if (isButtonItem(child)) {
                body << genButtonDecl(child, ntOffX, ntOffY, isNes, variadic, runtimePos, rootItem, target, region,
                                       ntOffXExpr, ntOffYExpr, buttonBitIndex.value(child), kButtonStateArrayName,
                                       geom);
                // Only a toggleable button gets a Pass(), and so only one of
                // those puts input:: in the header -- see genButtonDecl.
                if (child->data(0, kButtonToggleableRole).toBool()) usesInput = true;
                makeAll << qual;
            } else if (isSliderItem(child)) {
                body << genSliderDecl(child, ntOffX, ntOffY, isNes, variadic, runtimePos, rootItem, target, region,
                                       bssPrefixTok, ntOffXExpr, ntOffYExpr, geom);
                usesInput = true;
                makeAll << qual;
            } else if (isSingleChoiceItem(child)) {
                const QString decl =
                    genSingleChoiceDecl(child, rootItem, target, region, bssPrefixTok, dataPrefixTok, runtimePos);
                if (decl.isEmpty()) continue;  // no visible options -- nothing to construct
                body << decl;
                usesInput = true;
                makeAll << qual;
            } else if (isCanvasItem(child)) {
                body << genCanvasDecl(child, target, region, bssPrefixTok);
                // A Canvas names input:: only once it has somewhere to move
                // focus TO: with one component its Pass() just forwards the
                // payload, and with none it has no Pass() at all. Either way
                // any component it does hold is a slider or a toggleable
                // button, which has already set this itself.
                if (canvasComponents(child, target, region).size() > 1) usesInput = true;
                makeAll << qual;
            } else {
                // Every remaining prefixed kind is a geometry node -- ctTextBox,
                // rtTextBox, negative space, Rect -- and each now carries the
                // same rect/pos/Make() the placeable kinds above do. For a
                // Rect that is the whole of its namespace: neither textbox
                // branch below fires for one, and it has no state, no hook and
                // no Pass() of its own, so what it exports is exactly the
                // constexpr-where-it-can-be geometry every other node already
                // got (see kRectKind and genGeometryStorage).
                QStringList own = genGeometryStorage(child, ntOffX, ntOffY, isNes, variadic, runtimePos, rootItem,
                                                      target, region, ntOffXExpr, ntOffYExpr, geom);
                makeAll << qual;

                const QString charmapFn = nodeCharmapFn(child);
                if (!charmapFn.isEmpty() && isComponentItem(child)) usedCharmapFns.insert(charmapFn);

                if (isCtTextBoxItem(child)) {
                    // Actually draws (bakes the ppu::WriteFromBufferToNameTable
                    // calls itself), unlike rtTextBox's wrapper below. AI, like
                    // every other generated wrapper here -- body merged into the
                    // .hpp declaration, not split into a .cpp definition, because
                    // AI itself requires that (see ::AI's own comment in
                    // technology.hpp: a force-inlined function's body has to be
                    // visible in every TU that calls it, which a separate .cpp
                    // definition can never guarantee). No linkerPrefixTok (bank
                    // placement) either, for the same reason every other
                    // AI-tagged function here skips it: AI's own doc comment says
                    // it doesn't compose with a placement attribute -- once a body
                    // gets duplicated into every caller, there's no single
                    // out-of-line copy left for a section attribute to pin.
                    const CtTextBoxGen gen = genCtTextBoxBody(child, ntOffX, ntOffY, isNes, variadic, rootItem,
                                                                target, region, dataPrefixTok, charmapFn,
                                                                ntOffXExpr, ntOffYExpr, geom);
                    if (!gen.rootDecls.isEmpty()) own << gen.rootDecls;
                    own << QString("inline AI void Draw() {\n%1\n}").arg(gen.body);

                    if (child->data(0, kProvideErasingRole).toBool()) {
                        const QString eraseBody = genCtTextBoxEraseBody(child, ntOffX, ntOffY, isNes, variadic,
                                                                          rootItem, target, region, charmapFn,
                                                                          ntOffXExpr, ntOffYExpr, geom);
                        own << QString("inline AI void Erase() {\n%1\n}").arg(eraseBody);
                    }
                } else if (isRtTextBoxItem(child)) {
                    const int w = geom.w(child);
                    const int h = geom.h(child);
                    const QString splitterStr = child->data(0, kSplitterRole).toString();
                    const QChar splitter = splitterStr.isEmpty() ? QLatin1Char(' ') : splitterStr.at(0);
                    // Text isn't known until runtime, so there's nothing here to
                    // charmap-encode -- but whatever buffer the caller eventually
                    // passes in IS expected to already be charmap-encoded (it's
                    // handed straight to ui::text::Make, same raw bytes ppu writes
                    // land untranslated), so the splitter byte Make compares each
                    // character against has to be encoded through the very same
                    // charmap too, not left as a raw ASCII literal that would never
                    // match an encoded space/boundary byte. Same for the terminator
                    // Make scans for to find the buffer's end, which is why there is
                    // no length parameter here.
                    //
                    // Make takes the charmap itself and lifts both bytes out of it,
                    // and takes it as a template parameter so it stays compile-time
                    // only (see text.hpp) -- the splitter travels as a `char` NTTP for
                    // the same reason, so the charmap never gets called at runtime.
                    // With no charmap set, a scene's buffers hold untranslated bytes,
                    // which is exactly ui::text::ascii -- the identity charmap, whose
                    // splitter is the raw ASCII character and whose terminator is a
                    // plain 0x00. There is no second, byte-taking Make to reach for.
                    //
                    // Named MakeText, not Make: this node's geometry already owns
                    // Make() (see genGeometryStorage), and two unrelated operations
                    // sharing a verb by overload would read as one.
                    const QString mapExpr = charmapFn.isEmpty() ? QStringLiteral("ui::text::ascii") : charmapFn;
                    const QString callExpr = QString("ui::text::Make<%1, '%2'>(buff, vec2<u8>{%3, %4})")
                                                 .arg(mapExpr, cCharEscape(splitter)).arg(w).arg(h);
                    own << QString("inline AI ui::text::textBuffer* MakeText(const u8* buff) {\n"
                                    "    return %1;\n"
                                    "}")
                                 .arg(callExpr);
                }
                body << own.join("\n\n");
            }

            emitted << QString("namespace %1 {\n\n%2\n}  // namespace %1\n").arg(name, body.join("\n\n"));
        }
        return emitted;
    };

    hppDecls += emitNodeNamespace(rootItem, QString());

    // One call that makes the whole scene. Every node now carries geometry, so
    // without this a caller would have to name each one individually just to
    // get its rect/pos resolved -- and would silently miss any node added to
    // the scene later. Document order, which is also the order the namespaces
    // above were emitted in.
    if (!makeAll.isEmpty()) {
        QStringList callLines;
        for (const QString& qual : makeAll) callLines << QString("    %1::Make();").arg(qual);
        hppDecls << QString("// Resolves every node's geometry and seeds every menu's running state.\n"
                             "// On a runtime-viewport target nothing may read a node's rect/pos until\n"
                             "// this (or that node's own Make()) has run; on a fixed one it is still\n"
                             "// emitted, so no call site needs a #if to decide whether it exists.\n"
                             "inline AI void Make() {\n%1\n}")
                        .arg(callLines.join("\n"));
    }

    QStringList hppIncludes{"#include <intsh>", "#include <pnes/types.hpp>", "#include <pnes/video.hpp>",
                             "#include <pnes/extras/ui/text.hpp>", "#include <pnes/extras/ui/types.hpp>"};
    if (usesInput) {
        // input::UP/DOWN/LEFT/RIGHT, for the generated Pass() of every
        // interactive node kind -- SingleChoice, Button, Slider.
        hppIncludes << "#include <pnes/input.hpp>";
    }

    QString charmapNote;
    if (!usedCharmapFns.isEmpty()) {
        QStringList sorted(usedCharmapFns.begin(), usedCharmapFns.end());
        sorted.sort();
        charmapNote = QString("\n// NOTE: %1 must already be in scope here.\n").arg(sorted.join(", "));
    }

    // Linker/BSS/Data Prefix macros must be fed in from local.cmake (see
    // CMakeLists.txt's DEMO_PLACEMENT_DEFINES) -- #error here if one's
    // missing, same #ifndef/#error pattern as PNES_AUDIO_SECTION/UI.
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
                   QCheckBox* showTileGridCheck, QSpinBox* tileGridWSpin, QSpinBox* tileGridHSpin,
                   QWidget* parent = nullptr)
        : QWidget(parent), tilePx_(tilePx), tree_(tree), targetCombo_(targetCombo), regionCombo_(regionCombo),
          showTileGridCheck_(showTileGridCheck), tileGridWSpin_(tileGridWSpin), tileGridHSpin_(tileGridHSpin) {
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

        // uitk draws no real appearance for a button (that's entirely
        // its hand-written Draw()'s job -- see genButtonDecl), but the
        // editor still needs *something* visible to place/drag/select.
        // Only the edge/corner tiles of the box are painted -- a 1-tile-wide
        // frame, same as a real tile-based button border would spend tiles
        // on -- leaving every interior tile (row/col strictly between the
        // edges) untouched, so a wide/tall button previews as hollow rather
        // than a solid block.
        for (QTreeWidgetItem* item : collectComponents()) {
            if (!isButtonItem(item)) {
                continue;
            }
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());

            for (int row = 0; row < h; ++row) {
                const bool edgeRow = (row == 0 || row == h - 1);
                for (int col = 0; col < w; ++col) {
                    const bool edgeCol = (col == 0 || col == w - 1);
                    if (!edgeRow && !edgeCol) {
                        continue;  // interior -- left hollow
                    }
                    painter.fillRect(tileRect(x + col, y + row), QColor(120, 90, 20));
                }
            }

            painter.setFont(glyphFont_);
            painter.setPen(Qt::white);
            const QString name = bareName(item);
            for (int i = 0; i < name.length() && i < w; ++i) {
                drawCellGlyph(painter, tileRect(x + i, y), name.at(i));
            }
        }

        // Same deal as a button -- uitk bakes no appearance for a slider
        // either (Draw_<Name>/DrawChange_<Name> own that entirely, see
        // genSliderDecl), so
        // this is purely something to see and grab in the editor. Drawn as a
        // solid track rather than a hollow frame: a slider's whole footprint
        // IS its range (the handle travels across every cell of it), so
        // previewing the interior as empty would misrepresent what the box
        // means, unlike a button's border.
        for (QTreeWidgetItem* item : collectComponents()) {
            if (!isSliderItem(item)) {
                continue;
            }
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());

            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    painter.fillRect(tileRect(x + col, y + row), QColor(60, 100, 60));
                }
            }

            painter.setFont(glyphFont_);
            painter.setPen(Qt::white);
            const QString name = bareName(item);
            for (int i = 0; i < name.length() && i < w; ++i) {
                drawCellGlyph(painter, tileRect(x + i, y), name.at(i));
            }
        }

        // A Rect has no appearance to preview at all -- it is pure geometry
        // (see kRectKind) -- so it is drawn as an outline and nothing else:
        // one thin frame around its footprint, no fill, so whatever shares
        // those cells stays readable underneath it. Drawn after every kind
        // that does have a look of its own, for that same reason: a Rect is
        // usually an area AROUND other nodes, and filling or covering them
        // would misrepresent a node that paints nothing on the target.
        for (QTreeWidgetItem* item : collectComponents()) {
            if (!isRectItem(item)) {
                continue;
            }
            const int x = item->data(0, kPosXRole).toInt();
            const int y = item->data(0, kPosYRole).toInt();
            const int w = std::max(1, item->data(0, kSizeWRole).toInt());
            const int h = std::max(1, item->data(0, kSizeHRole).toInt());

            const QRect region = tileRect(x, y).united(tileRect(x + w - 1, y + h - 1));
            painter.setPen(QPen(QColor(150, 110, 200), 1, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(region.adjusted(0, 0, -1, -1));

            painter.setFont(glyphFont_);
            painter.setPen(QColor(190, 160, 230));
            const QString name = bareName(item);
            for (int i = 0; i < name.length() && i < w; ++i) {
                drawCellGlyph(painter, tileRect(x + i, y), name.at(i));
            }
        }

        // Editor-only alignment aid (see showTileGridCheck/tileGridWSpin/
        // tileGridHSpin) -- drawn last, on top of everything else, as thin
        // translucent lines every tileGridWSpin_/tileGridHSpin_ tiles. Purely
        // visual: it reads nothing but those three widgets, and nothing it
        // draws here is ever part of what gets exported (see generateCode,
        // which never touches this widget at all).
        if (showTileGridCheck_ && showTileGridCheck_->isChecked()) {
            const int cols = std::max(1, qRound(width() / tilePx_));
            const int rows = std::max(1, qRound(height() / tilePx_));
            const int stepX = std::max(1, tileGridWSpin_->value());
            const int stepY = std::max(1, tileGridHSpin_->value());
            painter.setPen(QPen(QColor(255, 255, 0, 160), 1));
            for (int col = stepX; col < cols; col += stepX) {
                const int lineX = tileRect(col, 0).left();
                painter.drawLine(lineX, 0, lineX, height());
            }
            for (int row = stepY; row < rows; row += stepY) {
                const int lineY = tileRect(0, row).top();
                painter.drawLine(0, lineY, width(), lineY);
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
    //
    // A Rect loses to everything, for the opposite reason to a textbox
    // winning: it is an outline with nothing inside it (paintEvent()), so the
    // cells it covers are almost always meant for whatever it is drawn around.
    // Clicking one of those selects that node; a Rect is still grabbable
    // anywhere nothing else is, and always selectable from the tree.
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
        QTreeWidgetItem* rectMatch = nullptr;
        for (QTreeWidgetItem* item : collectComponents()) {
            if (!contains(item)) {
                continue;
            }
            if (isComponentItem(item)) {
                componentMatch = item;
            } else if (isRectItem(item)) {
                rectMatch = item;
            } else {
                negSpaceMatch = item;
            }
        }
        if (componentMatch) return componentMatch;
        return negSpaceMatch ? negSpaceMatch : rectMatch;
    }

    double tilePx_;
    QTreeWidget* tree_;
    QComboBox* targetCombo_;
    QComboBox* regionCombo_;
    QCheckBox* showTileGridCheck_;
    QSpinBox* tileGridWSpin_;
    QSpinBox* tileGridHSpin_;
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

    // Editor-only tile-grid overlay controls (see showTileGridCheck above) --
    // exposed so createViewportPanel's TileGridWidget can read them, same as
    // targetCombo/regionCombo.
    QCheckBox* showTileGridCheck;
    QSpinBox* tileGridWSpin;
    QSpinBox* tileGridHSpin;

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

    // Which quadrant of the 2x2 world grid -- UL, UR, BL or BR -- the whole
    // scene's tile positions resolve into in-game: a scene-wide setting, not a
    // per-node one, since a UI lives in one quadrant. UR shifts every position
    // one viewport width right, BL one viewport height down, BR both.
    //
    // This names a position in the world, NOT a physical nametable. It used to
    // read "0 ($2000)".."3 ($2C00)", which implied uitk knew which chip the
    // write would land in -- it doesn't, and can't: off-NES the emu backend
    // folds the runtime ::mirroring flag into the offset CartesianToAddress
    // returns, and on hardware the board's wiring routes the write after the
    // address. Making sure the game's actual VRAM/mirroring state matches what
    // a scene was authored against is the caller's job, not this tool's.
    //
    // Created here, ahead of the File menu below, so Save/Open can already
    // capture it; it's laid out alongside the viewport size fields further
    // down.
    auto* quadrantCombo = new QComboBox(content);
    quadrantCombo->addItems({"UL", "UR", "BL", "BR"});
    quadrantCombo->setToolTip("Which quadrant of the 2x2 world grid the whole scene's tile positions "
                                "resolve into in-game: UR shifts every position one viewport width "
                                "right, BL one viewport height down, BR both. Ensuring the game's "
                                "VRAM and mirroring state matches is the caller's job.");

    // Which pnes backend the whole scene targets -- also scene-wide,
    // for the same reason: a scene isn't a mix of platforms, it's authored
    // against one.
    auto* targetCombo = new QComboBox(content);
    targetCombo->addItems(kTargetNames);
    targetCombo->setToolTip("Which pnes backend this scene targets.");

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

    // Same idea as Linker Prefix, but for the scene's mutable storage: a
    // SingleChoice's own instance state (and, on a variadic-viewport target,
    // its mutable options array) plus every Slider's `<Name>_amount` byte
    // -- code/rodata and BSS commonly need different placement
    // attributes on NES (see demo/src/banks.hpp's TITLE vs TITLE_DATA for
    // why: LLD rejects code and data sharing one literal section name), so
    // this is a second, independent free-form field rather than reusing
    // Linker Prefix for both. Empty by default, and -- like Linker Prefix --
    // only ever emitted when exporting for NES.
    auto* bssPrefixEdit = new QLineEdit(content);
    bssPrefixEdit->setToolTip("Emitted immediately before a SingleChoice's own instance storage (and, on a "
                               "variadic-viewport target, its mutable options array), and before every "
                               "Slider's <Name>_amount byte -- e.g. a RAM-section "
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

    // The mapname whose `charmap_<name>.encode(...)` encodes every
    // ctTextBox's row text, and whose `charmap_<name>(...)` encodes a
    // rtTextBox's splitter byte (see genCtTextBoxBody/generateCode) --
    // `charmap_<name>` itself is project code (an object built with
    // technology.hpp's tech::nes_str::charmap, e.g.
    // demo/src/graphics/charmaps.hpp's charmap_generic), not something uitk
    // can generate, so this only ever names it. Left empty, a scene falls
    // back to its previous plain `static const char[]` behavior instead of
    // referencing a symbol that doesn't exist. This is
    // only the scene-wide default -- any individual textbox can name a
    // different mapname via its own Charmap Override property
    // (kCharmapOverrideRole), which wins over this for that node alone.
    auto* charmapEdit = new QLineEdit(content);
    charmapEdit->setToolTip("Default mapname of the charmap (tech::nes_str::charmap, technology.hpp) every "
                             "ctTextBox's row text -- and every rtTextBox's splitter byte -- is encoded "
                             "through, e.g. \"generic\" for "
                             "charmap_generic. charmap_<name> must already be defined and in scope wherever "
                             "the generated header is included. Left empty, text is emitted as a plain, "
                             "unencoded C string instead (unless a node's own Charmap Override sets one). "
                             "A textbox's Charmap Override property replaces this default for that node.");

    // Purely a canvas aid, scene-wide like the fields above -- toggles an
    // overlay of grid lines every `tileGridWSpin`/`tileGridHSpin` tiles over
    // the viewport (see TileGridWidget::paintEvent) to help line up
    // components by eye. Unlike everything else in this group, none of this
    // ever reaches generateCode/exportOneTarget -- it round-trips through the
    // .uis file (so it's remembered per scene) but has no effect on, and
    // never appears in, the generated .hpp/.cpp. Default 1x1 (every tile
    // boundary); either axis can be grown up to the viewport's own tile
    // count (e.g. 8x1, to check a single row/column alignment).
    auto* showTileGridCheck = new QCheckBox(content);
    showTileGridCheck->setToolTip("Overlays grid lines on the canvas every N tiles (set below) -- an editor-only "
                                   "aid with no effect on exported code.");
    auto* tileGridWSpin = new QSpinBox(content);
    tileGridWSpin->setRange(1, std::max(1, maxTilesX));
    tileGridWSpin->setValue(1);
    tileGridWSpin->setSuffix(" tiles");
    tileGridWSpin->setToolTip("Grid overlay spacing, in tiles, along X.");
    auto* tileGridHSpin = new QSpinBox(content);
    tileGridHSpin->setRange(1, std::max(1, maxTilesY));
    tileGridHSpin->setValue(1);
    tileGridHSpin->setSuffix(" tiles");
    tileGridHSpin->setToolTip("Grid overlay spacing, in tiles, along Y.");

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
        if (!g_sceneNameOverride.isEmpty()) return g_sceneNameOverride;
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

    auto doSaveAs = [window, rootItem, currentPath, dirty, updateTitle, quadrantCombo, targetCombo, regionCombo,
                     linkerPrefixEdit, bssPrefixEdit, dataPrefixEdit, charmapEdit, showTileGridCheck,
                     tileGridWSpin, tileGridHSpin]() {
        QString path = QFileDialog::getSaveFileName(window, "Save Scene", QString(), "UI Scene (*.uis)");
        if (path.isEmpty()) {
            return false;
        }
        if (!path.endsWith(".uis", Qt::CaseInsensitive)) {
            path += ".uis";
        }
        if (!writeUisFile(path, rootItem, quadrantCombo->currentIndex(), targetCombo->currentText(),
                           regionCombo->currentText(), linkerPrefixEdit->text(), bssPrefixEdit->text(),
                           dataPrefixEdit->text(), charmapEdit->text(), showTileGridCheck->isChecked(),
                           tileGridWSpin->value(), tileGridHSpin->value())) {
            QMessageBox::warning(window, "Save Failed", "Could not write file:\n" + path);
            return false;
        }
        *currentPath = path;
        *dirty = false;
        updateTitle();
        return true;
    };

    auto doSave = [rootItem, currentPath, dirty, updateTitle, doSaveAs, window, quadrantCombo, targetCombo,
                   regionCombo, linkerPrefixEdit, bssPrefixEdit, dataPrefixEdit, charmapEdit, showTileGridCheck,
                   tileGridWSpin, tileGridHSpin]() {
        if (currentPath->isEmpty()) {
            return doSaveAs();
        }
        if (!writeUisFile(*currentPath, rootItem, quadrantCombo->currentIndex(), targetCombo->currentText(),
                           regionCombo->currentText(), linkerPrefixEdit->text(), bssPrefixEdit->text(),
                           dataPrefixEdit->text(), charmapEdit->text(), showTileGridCheck->isChecked(),
                           tileGridWSpin->value(), tileGridHSpin->value())) {
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
                               quadrantCombo, targetCombo, regionCombo, linkerPrefixEdit, bssPrefixEdit,
                               dataPrefixEdit, charmapEdit, showTileGridCheck, tileGridWSpin,
                               tileGridHSpin](const QString& path) -> bool {
        // Parse before touching the tree, so a corrupt/unreadable file never
        // wipes out whatever scene was already open.
        QJsonArray nodes;
        int quadrant = 0;
        QString target;
        QString region;
        QString linkerPrefix;
        QString bssPrefix;
        QString dataPrefix;
        QString charmap;
        bool showTileGrid = false;
        int tileGridW = 8;
        int tileGridH = 8;
        if (!parseUisFile(path, nodes, quadrant, target, region, linkerPrefix, bssPrefix, dataPrefix, charmap,
                           showTileGrid, tileGridW, tileGridH)) {
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
        quadrantCombo->setCurrentIndex(quadrant);
        targetCombo->setCurrentIndex(targetIndex);
        regionCombo->setCurrentIndex(regionIndex);
        linkerPrefixEdit->setText(linkerPrefix);
        bssPrefixEdit->setText(bssPrefix);
        dataPrefixEdit->setText(dataPrefix);
        charmapEdit->setText(charmap);
        showTileGridCheck->setChecked(showTileGrid);
        tileGridWSpin->setValue(std::min(tileGridW, tileGridWSpin->maximum()));
        tileGridHSpin->setValue(std::min(tileGridH, tileGridHSpin->maximum()));
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
    auto exportOneTarget = [window, rootItem, regionCombo, quadrantCombo, linkerPrefixEdit, bssPrefixEdit,
                             dataPrefixEdit, charmapEdit, sceneName,
                             writeIfChanged](const QString& dir, const QString& target) -> bool {
        const QString targetDir = dir + "/" + target.toLower();
        if (!QDir().mkpath(targetDir)) return false;
        const GeneratedFiles files = generateCode(rootItem, target, regionCombo->currentText(),
                                                    quadrantCombo->currentIndex(), linkerPrefixEdit->text(),
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
    QObject::connect(quadrantCombo, qOverload<int>(&QComboBox::currentIndexChanged),
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
    QObject::connect(showTileGridCheck, &QCheckBox::toggled, [markDirtyFromCombo](bool) { markDirtyFromCombo(); });
    QObject::connect(tileGridWSpin, qOverload<int>(&QSpinBox::valueChanged),
                      [markDirtyFromCombo](int) { markDirtyFromCombo(); });
    QObject::connect(tileGridHSpin, qOverload<int>(&QSpinBox::valueChanged),
                      [markDirtyFromCombo](int) { markDirtyFromCombo(); });

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

    // Button-only (see kButtonDefaultEnabledRole/kButtonToggleableRole).
    auto* buttonDefaultEnabledCheck = new QCheckBox(properties);
    auto* buttonToggleableCheck = new QCheckBox(properties);
    auto* buttonHasBoxCheck = new QCheckBox(properties);
    buttonDefaultEnabledCheck->setToolTip("Whether the scene starts with this button Enabled or Disabled.");
    buttonToggleableCheck->setToolTip("Also generate a Toggle_<Name>() that flips and returns the enabled flag.");
    buttonHasBoxCheck->setToolTip("Also generate a <Name>_box (vec2<u8>) constant with this button's Size X/Y.\n"
                                   "Leave unchecked if your Draw()/etc. implementation has "
                                   "no use for its own footprint (e.g. a fixed-size sprite).");

    // SingleChoice-only (see kSingleChoiceVerticalRole).
    auto* choiceVerticalCheck = new QCheckBox(properties);
    choiceVerticalCheck->setToolTip("Vertical: the options are a stacked list, stepped with UP/DOWN.\n"
                                     "Unchecked: a row, stepped with LEFT/RIGHT. Inside a Canvas, set this\n"
                                     "to the opposite of the Canvas's own axis so both can be moved.");

    auto* choiceHasRectCheck = new QCheckBox(properties);
    choiceHasRectCheck->setToolTip("Also generate a <Name>::rect covering every option this target shows.\n"
                                    "Leave unchecked unless something measures the menu as a whole -- on a\n"
                                    "runtime-viewport target it costs six bytes and a fold in Make(). Implied,\n"
                                    "whatever this says, when the menu sits inside a Canvas.");

    // Canvas-only (see kCanvasVerticalRole).
    auto* canvasVerticalCheck = new QCheckBox(properties);
    canvasVerticalCheck->setToolTip("Vertical: focus steps with UP/DOWN and everything else (including\n"
                                     "LEFT/RIGHT) reaches the focused component. Unchecked: focus steps\n"
                                     "with LEFT/RIGHT instead, and UP/DOWN fall through. Either way, a\n"
                                     "component nested here wants the opposite axis to this one.");

    // Slider-only (see kSliderVerticalRole).
    auto* sliderVerticalCheck = new QCheckBox(properties);
    sliderVerticalCheck->setToolTip("Vertical: the handle moves with UP/DOWN, a track down a column --\n"
                                     "your Draw()/DrawChange() measure the range off Size Y. Unchecked:\n"
                                     "LEFT/RIGHT along a row, measured off Size X. Inside a Canvas, set\n"
                                     "this to the opposite of the Canvas's own axis so both can be moved.");

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
                              "PANEL1_X/Y/TX/TY = the lower panel's rect in tiles (dual-screen targets).\n"
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
    charmapOverrideEdit->setToolTip("Charmap mapname (tech::nes_str::charmap, technology.hpp) to encode this "
                                     "node's row text/splitter through, in place of the scene's Default "
                                     "Charmap.\n"
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
    choiceForm->addRow("Vertical", choiceVerticalCheck);
    choiceForm->addRow("Provide Bounds", choiceHasRectCheck);

    auto* canvasGroup = new QGroupBox("Canvas", properties);
    auto* canvasForm = new QFormLayout(canvasGroup);
    canvasForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    canvasForm->addRow("Vertical", canvasVerticalCheck);

    auto* sliderGroup = new QGroupBox("Slider", properties);
    auto* sliderForm = new QFormLayout(sliderGroup);
    sliderForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    sliderForm->addRow("Vertical", sliderVerticalCheck);

    auto* buttonGroup = new QGroupBox("Button", properties);
    auto* buttonForm = new QFormLayout(buttonGroup);
    buttonForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    buttonForm->addRow("Default Enabled", buttonDefaultEnabledCheck);
    buttonForm->addRow("Toggleable", buttonToggleableCheck);
    buttonForm->addRow("Has Box", buttonHasBoxCheck);

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
    propertiesLayout->addWidget(canvasGroup);
    propertiesLayout->addWidget(sliderGroup);
    propertiesLayout->addWidget(buttonGroup);
    propertiesLayout->addWidget(visibilityGroup);
    properties->setVisible(false);

    // Populates the panel's fields from `item` without re-triggering the
    // edit handlers below (which would otherwise write the same values
    // straight back -- harmless, but pointless).
    auto populateFrom = [posXEdit, posYEdit, sizeWEdit, sizeHEdit, alignCombo, textEdit, splitterEdit,
                         charmapOverrideEdit, provideErasingCheck, defaultOptionSpin, choiceVerticalCheck,
                         choiceHasRectCheck, canvasVerticalCheck, sliderVerticalCheck,
                         buttonDefaultEnabledCheck,
                         buttonToggleableCheck, buttonHasBoxCheck, hideTargetsButton,
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
        const QSignalBlocker bscv(choiceVerticalCheck);
        const QSignalBlocker bscr(choiceHasRectCheck);
        const QSignalBlocker bcv(canvasVerticalCheck);
        const QSignalBlocker bsv(sliderVerticalCheck);
        const QSignalBlocker bbe(buttonDefaultEnabledCheck);
        const QSignalBlocker bbt(buttonToggleableCheck);
        const QSignalBlocker bbb(buttonHasBoxCheck);
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
        choiceVerticalCheck->setChecked(item->data(0, kSingleChoiceVerticalRole).toBool());
        choiceHasRectCheck->setChecked(item->data(0, kSingleChoiceHasRectRole).toBool());
        canvasVerticalCheck->setChecked(item->data(0, kCanvasVerticalRole).toBool());
        sliderVerticalCheck->setChecked(item->data(0, kSliderVerticalRole).toBool());
        buttonDefaultEnabledCheck->setChecked(item->data(0, kButtonDefaultEnabledRole).toBool());
        buttonToggleableCheck->setChecked(item->data(0, kButtonToggleableRole).toBool());
        buttonHasBoxCheck->setChecked(item->data(0, kButtonHasBoxRole).toBool());

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
        [properties, geometryGroup, componentGroup, erasingGroup, choiceGroup, canvasGroup, sliderGroup,
         buttonGroup, visibilityGroup, populateFrom,
         updateErrorHighlight](QTreeWidgetItem* current, QTreeWidgetItem*) {
            const bool selected = isPrefixedItem(current);
            properties->setVisible(selected);
            // Geometry means nothing for SingleChoice or Canvas -- both are pure
            // grouping nodes, no position/size of their own. Alignment/text/splitter only
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
            canvasGroup->setVisible(isCanvasItem(current));
            sliderGroup->setVisible(isSliderItem(current));
            buttonGroup->setVisible(isButtonItem(current));
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
         splitterEdit, charmapOverrideEdit, provideErasingCheck, defaultOptionSpin, choiceVerticalCheck,
         choiceHasRectCheck, canvasVerticalCheck, sliderVerticalCheck, buttonDefaultEnabledCheck,
         buttonToggleableCheck, buttonHasBoxCheck](QTreeWidgetItem* item, int column) {
            if (column != 0 || item != tree->currentItem() || !isPrefixedItem(item)) {
                return;
            }
            updateErrorHighlight(item);
            if (posXEdit->hasFocus() || posYEdit->hasFocus() || sizeWEdit->hasFocus() || sizeHEdit->hasFocus() ||
                textEdit->hasFocus() || splitterEdit->hasFocus() || charmapOverrideEdit->hasFocus() ||
                provideErasingCheck->hasFocus() || defaultOptionSpin->hasFocus() ||
                choiceVerticalCheck->hasFocus() || choiceHasRectCheck->hasFocus() ||
                canvasVerticalCheck->hasFocus() || sliderVerticalCheck->hasFocus() ||
                buttonDefaultEnabledCheck->hasFocus() ||
                buttonToggleableCheck->hasFocus() || buttonHasBoxCheck->hasFocus()) {
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
    QObject::connect(choiceVerticalCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isSingleChoiceItem(item)) {
            item->setData(0, kSingleChoiceVerticalRole, checked);
        }
    });
    QObject::connect(choiceHasRectCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isSingleChoiceItem(item)) {
            item->setData(0, kSingleChoiceHasRectRole, checked);
        }
    });
    QObject::connect(canvasVerticalCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isCanvasItem(item)) {
            item->setData(0, kCanvasVerticalRole, checked);
        }
    });
    QObject::connect(sliderVerticalCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isSliderItem(item)) {
            item->setData(0, kSliderVerticalRole, checked);
        }
    });
    QObject::connect(buttonDefaultEnabledCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isButtonItem(item)) {
            item->setData(0, kButtonDefaultEnabledRole, checked);
        }
    });
    QObject::connect(buttonToggleableCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isButtonItem(item)) {
            item->setData(0, kButtonToggleableRole, checked);
        }
    });
    QObject::connect(buttonHasBoxCheck, &QCheckBox::toggled, [tree](bool checked) {
        QTreeWidgetItem* item = tree->currentItem();
        if (isButtonItem(item)) {
            item->setData(0, kButtonHasBoxRole, checked);
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

    // A Rect is the smallest node there is: geometry, a name, and the hide
    // lists every prefixed node carries -- no properties of its own at all, so
    // selecting one shows the Geometry and Visibility groups and nothing else
    // (the properties panel is driven by per-kind predicates, so that needs no
    // wiring here). Same 1x1-at-the-origin start as every other placeable kind.
    auto rectCounter = std::make_shared<int>(1);
    auto addRectNode = [rectCounter](QTreeWidgetItem* parent) {
        const QString name = QString(kRectPrefix) + "Rect" + QString::number((*rectCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kRectKind));
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
        child->setData(0, kSingleChoiceVerticalRole, true);
        child->setData(0, kSingleChoiceHasRectRole, false);
        child->setData(0, kLastValidNameRole, name);
        parent->setExpanded(true);
        return child;
    };

    // A canvas, like a single choice, is nothing but a parent node -- no
    // geometry -- but it does have one property: which axis its focus moves
    // along. Defaults to vertical, a stacked menu (see kCanvasVerticalRole),
    // which pairs with the horizontal default a new Slider gets so the two are
    // immediately compatible. Its Button/Slider children are added onto it
    // via the context menu below, same as a SingleChoice's options.
    auto canvasCounter = std::make_shared<int>(1);
    auto addCanvasNode = [canvasCounter](QTreeWidgetItem* parent) {
        const QString name = QString(kCanvasPrefix) + "Canvas" + QString::number((*canvasCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kCanvasKind));
        child->setData(0, kCanvasVerticalRole, true);
        child->setData(0, kLastValidNameRole, name);
        parent->setExpanded(true);
        return child;
    };

    // A button has geometry like a textbox (see addTextBoxNode above), plus
    // its own enabled flag/selector/toggle flags -- defaults to enabled, no
    // external selector, non-toggleable, same as a fresh ctTextBox defaults
    // to Left/empty text.
    auto buttonCounter = std::make_shared<int>(1);
    auto addButtonNode = [buttonCounter](QTreeWidgetItem* parent) {
        const QString name = QString(kButtonPrefix) + "Button" + QString::number((*buttonCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kButtonKind));
        child->setData(0, kPosXExprRole, QStringLiteral("0"));
        child->setData(0, kPosYExprRole, QStringLiteral("0"));
        child->setData(0, kSizeWExprRole, QStringLiteral("1"));
        child->setData(0, kSizeHExprRole, QStringLiteral("1"));
        child->setData(0, kPosXRole, 0);
        child->setData(0, kPosYRole, 0);
        child->setData(0, kSizeWRole, 1);
        child->setData(0, kSizeHRole, 1);
        child->setData(0, kButtonDefaultEnabledRole, true);
        child->setData(0, kButtonToggleableRole, false);
        child->setData(0, kButtonHasBoxRole, true);
        child->setData(0, kLastValidNameRole, name);
        parent->setExpanded(true);
        return child;
    };

    // A slider's only state is its generated `<name>_amount` byte, which
    // always starts at 0 and has no editor-side knob (see genSliderDecl) --
    // so the one kind-specific default to seed here is its axis, which
    // starts horizontal, the track-along-a-row layout (kSliderVerticalRole).
    // Note that is the OPPOSITE default to a Canvas's own `vertical`, which
    // is deliberate: a slider dropped onto a fresh Canvas is adjustable
    // straight away rather than fighting it for LEFT/RIGHT.
    auto sliderCounter = std::make_shared<int>(1);
    auto addSliderNode = [sliderCounter](QTreeWidgetItem* parent) {
        const QString name = QString(kSliderPrefix) + "Slider" + QString::number((*sliderCounter)++);
        auto* child = new QTreeWidgetItem(parent, QStringList{name});
        child->setFlags(child->flags() | Qt::ItemIsEditable);
        child->setData(0, kKindRole, QString(kSliderKind));
        child->setData(0, kSliderVerticalRole, false);
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
        [tree, rootItem, addCtTextBoxNode, addRtTextBoxNode, addNegativeSpaceNode, addRectNode,
         addSingleChoiceNode, addCanvasNode, addButtonNode, addSliderNode, deleteNode](const QPoint& pos) {
            QTreeWidgetItem* clicked = tree->itemAt(pos);
            const bool onSingleChoice = isSingleChoiceItem(clicked);
            // A Button can likewise take ctTextBox/rtTextBox children (e.g.
            // a label), same reasoning as SingleChoice's own options below --
            // so it gets the same textbox-only menu when right-clicked.
            const bool onButton = isButtonItem(clicked);
            // A Slider takes textbox children for the same reason -- a
            // volume slider's "MUSIC" label belongs with the slider, not
            // loose at root, so its position can be written relative to it.
            const bool onSlider = isSliderItem(clicked);
            const bool onTextBoxContainer = onSingleChoice || onButton || onSlider;
            // A Canvas holds the interactables it rings -- Buttons, Sliders,
            // and whole SingleChoice menus -- and can hold textboxes too (a
            // label belongs with the group it labels), so right-clicking one
            // targets all of those at it. It isn't a textbox-only container
            // like the three above, hence its own flag rather than joining
            // them.
            const bool onCanvas = isCanvasItem(clicked);
            // A single choice's own children are its ctTextBox/rtTextBox
            // options, so right-clicking one targets textbox adds at it
            // rather than at root -- same as every other add here, which
            // still always targets root until there's UI for nesting more
            // generally. It (and a Button or Slider, for its label) houses
            // only textboxes, so Negative Space, Single Choice, Button,
            // Slider, and Canvas aren't offered there at all. A Canvas is the
            // one container that takes the interactables as well.
            QTreeWidgetItem* textBoxParent = (onTextBoxContainer || onCanvas) ? clicked : rootItem;
            // Where an interactable lands: inside the Canvas when one was
            // right-clicked, at root otherwise.
            QTreeWidgetItem* interactableParent = onCanvas ? clicked : rootItem;
            QMenu menu;
            QAction* addCtTextboxAction = menu.addAction("Add Compile-Time Textbox");
            QAction* addRtTextboxAction = menu.addAction("Add Runtime Textbox");
            // Offered wherever a textbox is, and for the same reason: a Rect
            // is passive geometry, so it can sit in any container without
            // changing what that container is. A SingleChoice ignores it when
            // collecting its options, and a Canvas when building its component
            // table -- neither looks at anything but the kinds it wants.
            QAction* addRectAction = menu.addAction("Add Rect");
            QAction* addNegSpaceAction =
                (onTextBoxContainer || onCanvas) ? nullptr : menu.addAction("Add Negative Space");
            QAction* addSingleChoiceAction = onTextBoxContainer ? nullptr : menu.addAction("Add Single Choice");
            // Offered on a Canvas (that is what a Canvas is for) but not
            // inside a textbox-only container.
            QAction* addButtonAction = onTextBoxContainer ? nullptr : menu.addAction("Add Button");
            QAction* addSliderAction = onTextBoxContainer ? nullptr : menu.addAction("Add Slider");
            // No nested Canvases yet: unlike a SingleChoice, which brings its
            // own axis and a rect covering its options, a Canvas has neither,
            // so focus inside focus needs a design of its own first.
            QAction* addCanvasAction =
                (onTextBoxContainer || onCanvas) ? nullptr : menu.addAction("Add Canvas");
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
            } else if (chosen && chosen == addRectAction) {
                tree->setCurrentItem(addRectNode(textBoxParent));
            } else if (chosen && chosen == addNegSpaceAction) {
                tree->setCurrentItem(addNegativeSpaceNode(rootItem));
            } else if (chosen && chosen == addSingleChoiceAction) {
                tree->setCurrentItem(addSingleChoiceNode(interactableParent));
            } else if (chosen && chosen == addButtonAction) {
                tree->setCurrentItem(addButtonNode(interactableParent));
            } else if (chosen && chosen == addSliderAction) {
                tree->setCurrentItem(addSliderNode(interactableParent));
            } else if (chosen && chosen == addCanvasAction) {
                tree->setCurrentItem(addCanvasNode(rootItem));
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
    *resolveAllPtr = [window, rootItem, xEdit, yEdit, tilePx, resolving, targetCombo, regionCombo]() {
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

        QVector<Pending> pending;
        std::map<std::pair<QTreeWidgetItem*, int>, long long> resolved;
        // Bitmask per errored item: bit `prop` set means that property
        // specifically failed to resolve -- lets the properties panel flag
        // the exact field at fault, not just the node as a whole.
        QHash<QTreeWidgetItem*, int> errorMask;

        for (QTreeWidgetItem* item : components) {
            for (int prop = 0; prop < 4; ++prop) {
                QString src = item->data(0, kGeomExprRoles[prop]).toString().trimmed();
                if (src.isEmpty()) {
                    src = QString::number(kGeomDefaults[prop]);
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
                // PANEL1_* -- the lower panel on a dual-screen target, else
                // the whole viewport (panel1TilesForTarget's own comment).
                if (name.startsWith(QLatin1String("PANEL1_"))) {
                    const auto panel = panel1TilesForTarget(targetCombo->currentText());
                    if (name == QLatin1String("PANEL1_X")) return panel ? panel->x : 0;
                    if (name == QLatin1String("PANEL1_Y")) return panel ? panel->y : 0;
                    if (name == QLatin1String("PANEL1_TX")) return panel ? panel->tx : viewportTilesX;
                    if (name == QLatin1String("PANEL1_TY")) return panel ? panel->ty : viewportTilesY;
                }
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
            // .enabled (prop 5) is 1/0 for whether the referenced node is
            // currently visible on the sidebar's selected Target/Region --
            // i.e. the same check isEffectivelyHidden() makes at export time
            // to decide whether to skip a node entirely, but here as a value
            // other expressions can build conditional placement on (e.g.
            // `this.pos.x + (1 - Other.enabled) * 10`). Answered directly,
            // like .textSize, rather than through the `resolved` fixed-point
            // map -- it depends only on the node's own hide lists (plus its
            // ancestors') and the combos below, never on another node's
            // still-resolving pos/size.
            if (prop == 5) {
                return isEffectivelyHidden(it.value(), targetCombo->currentText(), regionCombo->currentText())
                           ? 0
                           : 1;
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
                long long value = (it != resolved.end()) ? it->second : kGeomDefaults[prop];
                value = (prop >= 2) ? std::max<long long>(1, value) : std::max<long long>(0, value);
                values[prop] = value;
                if (item->data(0, kGeomRoles[prop]).toLongLong() != value) {
                    item->setData(0, kGeomRoles[prop], static_cast<int>(value));
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

    // `.enabled` (see ExprNode/resolveIdent above) reads whichever
    // Target/Region is currently selected, so anything referencing it needs
    // a fresh resolution pass whenever either changes too -- a node's own
    // Hide Targets/Hide Regions lists already trigger one via the tree's
    // itemChanged signal (see resolveAllPtr's connection above), since
    // those live on the tree item itself.
    QObject::connect(targetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                      [resolveAllPtr](int) { (*resolveAllPtr)(); });
    QObject::connect(regionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                      [resolveAllPtr](int) { (*resolveAllPtr)(); });

    auto* globalGroup = new QGroupBox("Global", content);
    auto* globalForm = new QFormLayout(globalGroup);
    globalForm->addRow("Viewport X (tx):", xEdit);
    globalForm->addRow("Viewport Y (tx):", yEdit);
    globalForm->addRow("Target:", targetCombo);
    globalForm->addRow("Region:", regionCombo);
    layout->addWidget(globalGroup);

    // "Scene" holds everything that's a property of *this* scene's export
    // (placement attributes + the quadrant it targets) rather than of the
    // viewport/Target/Region concepts above -- split out now, ahead of any
    // actual need, so a later per-scene field has an obvious home instead of
    // getting wedged into Global alongside Viewport/Target/Region.
    auto* sceneGroup = new QGroupBox("Scene", content);
    auto* sceneForm = new QFormLayout(sceneGroup);
    sceneForm->addRow("Quadrant:", quadrantCombo);
    sceneForm->addRow("Linker Prefix:", linkerPrefixEdit);
    sceneForm->addRow("BSS Prefix:", bssPrefixEdit);
    sceneForm->addRow("Data Prefix:", dataPrefixEdit);
    sceneForm->addRow("Default Charmap:", charmapEdit);
    layout->addWidget(sceneGroup);

    // Separate from Global/Scene (which are both exported in one way or
    // another) specifically so nothing in here is ever mistaken for an
    // export setting: Editor holds purely visual canvas aids, round-tripped
    // through the .uis file for convenience but never read by
    // generateCode/exportOneTarget.
    auto* editorGroup = new QGroupBox("Editor", content);
    auto* editorForm = new QFormLayout(editorGroup);
    editorForm->addRow("Show Tile Grid:", showTileGridCheck);
    editorForm->addRow("Grid Width:", tileGridWSpin);
    editorForm->addRow("Grid Height:", tileGridHSpin);
    layout->addWidget(editorGroup);

    // Global/Scene/Editor are scene-wide properties, not the selected node's
    // -- they only make sense to show/edit while the root (the scene itself)
    // is selected, same as how the node Properties panel above only shows
    // for a prefixed node.
    auto updateGlobalSceneVisibility = [globalGroup, sceneGroup, editorGroup, rootItem](QTreeWidgetItem* current) {
        const bool onRoot = (current == rootItem);
        globalGroup->setVisible(onRoot);
        sceneGroup->setVisible(onRoot);
        editorGroup->setVisible(onRoot);
    };
    QObject::connect(tree, &QTreeWidget::currentItemChanged,
                      [updateGlobalSceneVisibility](QTreeWidgetItem* current, QTreeWidgetItem*) {
                          updateGlobalSceneVisibility(current);
                      });
    updateGlobalSceneVisibility(tree->currentItem());

    return {content, tree, xEdit, yEdit, targetCombo, regionCombo,
             showTileGridCheck, tileGridWSpin, tileGridHSpin,
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
                                   QLineEdit* yEdit, QComboBox* targetCombo, QComboBox* regionCombo,
                                   QCheckBox* showTileGridCheck, QSpinBox* tileGridWSpin, QSpinBox* tileGridHSpin) {
    auto* grid = new TileGridWidget(tilePx, tree, targetCombo, regionCombo, showTileGridCheck, tileGridWSpin,
                                     tileGridHSpin);

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

    // Same idea for the tile-grid overlay itself -- toggling it, or changing
    // its spacing, only touches these three widgets directly (see
    // TileGridWidget::paintEvent), so it needs its own repaint hookup too.
    QObject::connect(showTileGridCheck, &QCheckBox::toggled, grid, [grid](bool) { grid->update(); });
    QObject::connect(tileGridWSpin, qOverload<int>(&QSpinBox::valueChanged), grid, [grid](int) { grid->update(); });
    QObject::connect(tileGridHSpin, qOverload<int>(&QSpinBox::valueChanged), grid, [grid](int) { grid->update(); });

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
                                 sidebar.regionCombo, sidebar.showTileGridCheck, sidebar.tileGridWSpin,
                                 sidebar.tileGridHSpin));

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
    //   -n, --name         scene name to export AS (default: the input file's
    //                      basename) -- see g_sceneNameOverride
    bool cliExport = false;
    bool cliExportAll = false;
    QString cliInput;
    QString cliOutput;
    QString cliTarget;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const bool wantsValue = (arg == "-i" || arg == "--input" || arg == "-o" || arg == "--output" ||
                                  arg == "-t" || arg == "--target" || arg == "-n" || arg == "--name");
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
        } else if (arg == "-n" || arg == "--name") {
            g_sceneNameOverride = QString::fromLocal8Bit(argv[++i]);
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
