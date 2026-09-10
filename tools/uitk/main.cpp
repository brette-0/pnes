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
#include <algorithm>
#include <functional>
#include <QTimer>

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

constexpr int kSidebarWidthPx = 100;

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

// Placeholder tile content: cycles through these characters, row-major, one
// per cell, until real content is wired up. Kept as a plain char array for
// now -- CJK/symbol characters will need this to become QString/QChar (a
// char can't hold them), but that's for when actual content is decided.
constexpr char kPlaceholderChars[] = "abcdef";
constexpr int kPlaceholderCharCount = sizeof(kPlaceholderChars) - 1;  // drop the trailing '\0'

// A viewport preview: renders one character per tile using QPainter's normal
// (vector, antialiased) text path rather than a rasterized/bitmap font, so
// glyphs stay crisp at arbitrary tile sizes instead of picking up the same
// kind of aliasing the pixel grid itself had. Otherwise just fills the space
// it's given (its size is driven externally by the sidebar's tile counts,
// not by its own size hint).
class TileGridWidget : public QWidget {
public:
    explicit TileGridWidget(double tilePx, QWidget* parent = nullptr)
        : QWidget(parent), tilePx_(tilePx) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);

        QFont font = painter.font();
        // Leave headroom around the glyph so it doesn't touch cell edges.
        font.setPixelSize(std::max(1, qRound(tilePx_ * 0.75)));
        painter.setFont(font);
        painter.setPen(Qt::white);

        // tilePx_ is typically fractional (e.g. ~5.93px), so tile boundaries
        // are rounded to the nearest pixel independently rather than
        // accumulated by repeated addition -- that keeps every tile a solid,
        // non-overlapping rect instead of drifting by a pixel here and there.
        const int cols = std::max(1, qRound(width() / tilePx_));
        const int rows = std::max(1, qRound(height() / tilePx_));

        int prevY = 0;
        for (int row = 0; row < rows; ++row) {
            const int nextY = (row == rows - 1) ? height() : qRound((row + 1) * tilePx_);
            int prevX = 0;
            for (int col = 0; col < cols; ++col) {
                const int nextX = (col == cols - 1) ? width() : qRound((col + 1) * tilePx_);
                const QRect cell(prevX, prevY, nextX - prevX, nextY - prevY);

                painter.fillRect(cell, Qt::black);
                const int charIndex = (row * cols + col) % kPlaceholderCharCount;
                painter.drawText(cell, Qt::AlignCenter, QString(QChar(kPlaceholderChars[charIndex])));

                prevX = nextX;
            }
            prevY = nextY;
        }
    }

private:
    double tilePx_;
};

struct Sidebar {
    QDockWidget* dock;
    QLineEdit* xEdit;
    QLineEdit* yEdit;
};

// `maxTilesX`/`maxTilesY` bound the fields to whatever will actually fit on
// the detected monitor at the current tile scale -- see the call site in
// main() for how those are derived.
Sidebar createSidebar(QWidget* parent, int maxTilesX, int maxTilesY) {
    auto* dock = new QDockWidget("Sidebar", parent);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setFixedWidth(kSidebarWidthPx);

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

    auto* content = new QWidget(dock);
    auto* layout = new QVBoxLayout(content);

    layout->addWidget(new QLabel("Viewport (tx)", content));

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

    auto* form = new QFormLayout();
    form->addRow("X:", xEdit);
    form->addRow("Y:", yEdit);
    layout->addLayout(form);

    layout->addStretch();

    dock->setWidget(content);
    return {dock, xEdit, yEdit};
}

// Re-fits `window` around its current contents and locks it at that size --
// the window is meant to be resized only by the app (as the grid changes
// size), never dragged by the user. As a hard backstop against ever handing
// a window manager a window bigger than the screen (the viewport's tile
// limits should already prevent this, but the margin they budget for the
// window's own frame is an estimate) the result is also clamped to the
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

ViewportPanel createViewportPanel(QMainWindow* window, double tilePx, QLineEdit* xEdit, QLineEdit* yEdit) {
    auto* grid = new TileGridWidget(tilePx);

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

    // Bound the viewport to what can actually be rendered on the detected
    // monitor: its available area (screen minus taskbars/docks), minus the
    // sidebar's own fixed width, converted from pixels to tiles at the
    // current scale.
    const QRect available = screen->availableGeometry();
    const int maxTilesX = std::max(
        1, static_cast<int>((available.width() - kSidebarWidthPx - kWindowChromeMarginPx) / tilePx));
    const int maxTilesY =
        std::max(1, static_cast<int>((available.height() - kWindowChromeMarginPx) / tilePx));

    const Sidebar sidebar = createSidebar(&window, maxTilesX, maxTilesY);
    const ViewportPanel viewport = createViewportPanel(&window, tilePx, sidebar.xEdit, sidebar.yEdit);
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
