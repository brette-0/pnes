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
#include <QObject>
#include <algorithm>
#include <functional>

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

double tilePxForScreen(const QRect& screenGeometry) {
    return kTilePxAt1080p * (screenGeometry.height() / kReferenceHeight);
}

// A viewport preview: draws a per-tile checkerboard so tile boundaries are
// visible, and otherwise just fills the space it's given (its size is driven
// externally by the sidebar's tile counts, not by its own size hint).
class TileGridWidget : public QWidget {
public:
    explicit TileGridWidget(double tilePx, QWidget* parent = nullptr)
        : QWidget(parent), tilePx_(tilePx) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);

        // tilePx_ is typically fractional (e.g. ~5.93px), so tile boundaries
        // are rounded to the nearest pixel independently rather than
        // accumulated by repeated addition. That keeps every tile a solid,
        // non-overlapping rect -- filling cells (instead of stroking grid
        // lines) means a tile that rounds to 1px narrower than its neighbor
        // is still a flat, uniformly-colored rect, not a sliver where two
        // anti-aliased lines nearly coincide and blend into a darker line.
        int prevX = 0;
        for (int col = 0; prevX < width(); ++col) {
            const int nextX = std::min(width(), qRound((col + 1) * tilePx_));
            int prevY = 0;
            for (int row = 0; prevY < height(); ++row) {
                const int nextY = std::min(height(), qRound((row + 1) * tilePx_));
                const bool light = (col + row) % 2 == 0;
                painter.fillRect(QRect(prevX, prevY, nextX - prevX, nextY - prevY),
                                  light ? QColor(24, 24, 24) : Qt::black);
                prevY = nextY;
            }
            prevX = nextX;
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

Sidebar createSidebar(QWidget* parent) {
    auto* dock = new QDockWidget("Sidebar", parent);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setFixedWidth(100);

    auto* content = new QWidget(dock);
    auto* layout = new QVBoxLayout(content);

    layout->addWidget(new QLabel("Viewport (tx)", content));

    auto* xEdit = new QLineEdit(content);
    auto* yEdit = new QLineEdit(content);
    // Minimum viewport size is 1x1 tiles. QIntValidator's bottom bound only
    // rejects values it's sure can't become valid (e.g. it still lets a bare
    // "0" through, as an intermediate state), so it alone doesn't keep the
    // field >= 1 -- clamp it back to "1" ourselves whenever it dips below
    // that, so the field always shows what it actually resolves to.
    // (editingFinished, the more obvious hook, doesn't reliably fire here --
    // textChanged does.)
    xEdit->setValidator(new QIntValidator(1, 9999, xEdit));
    yEdit->setValidator(new QIntValidator(1, 9999, yEdit));
    auto clampToMin = [](QLineEdit* edit, const QString& text) {
        if (text.toInt() < 1) {
            edit->setText("1");
        }
    };
    QObject::connect(xEdit, &QLineEdit::textChanged, [xEdit, clampToMin](const QString& text) {
        clampToMin(xEdit, text);
    });
    QObject::connect(yEdit, &QLineEdit::textChanged, [yEdit, clampToMin](const QString& text) {
        clampToMin(yEdit, text);
    });

    // Width to fit exactly 4 digits, plus room for the line edit's frame.
    const int digitsWidth = xEdit->fontMetrics().horizontalAdvance(QLatin1String("9999"));
    const int fieldWidth = digitsWidth + 16;
    xEdit->setFixedWidth(fieldWidth);
    yEdit->setFixedWidth(fieldWidth);

    // NES resolution (256x240) is 32x30 tiles -- a sensible default viewport.
    xEdit->setText("32");
    yEdit->setText("30");

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
// size), never dragged by the user.
void lockWindowToContents(QMainWindow* window) {
    window->setMinimumSize(0, 0);
    window->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    window->adjustSize();
    window->setFixedSize(window->size());
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
    QObject::connect(xEdit, &QLineEdit::textChanged, [sync] { (*sync)(); });
    QObject::connect(yEdit, &QLineEdit::textChanged, [sync] { (*sync)(); });

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

    const Sidebar sidebar = createSidebar(&window);
    const ViewportPanel viewport = createViewportPanel(&window, tilePx, sidebar.xEdit, sidebar.yEdit);
    window.setCentralWidget(viewport.grid);
    window.addDockWidget(Qt::RightDockWidgetArea, sidebar.dock);
    viewport.sync();

    window.show();

    // QApplication::exec() returns once the window is closed (including via
    // the taskbar/titlebar X, which QMainWindow handles by default -- no
    // closeEvent override needed) or QApplication::quit() is called. Qt
    // tears down its own event loop and widgets on the way out, so no
    // manual buffer flushing is needed here.
    return app.exec();
}
