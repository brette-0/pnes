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
#include <QScrollArea>
#include <QtGlobal>
#include <QPainter>
#include <QObject>
#include <algorithm>

namespace {

// Reference scaling: 8px/tile at 1080p, 16px/tile at 4K -- i.e. tile size
// tracks vertical resolution linearly. Fractional results are fine, visual
// precision isn't a goal here.
constexpr double kTilePxAt1080p = 8.0;
constexpr double kReferenceHeight = 1080.0;

double tilePxForScreen(const QRect& screenGeometry) {
    return kTilePxAt1080p * (screenGeometry.height() / kReferenceHeight);
}

// A viewport preview: draws a per-tile grid so tile boundaries are visible,
// and otherwise just fills the space it's given (its size is driven
// externally by the sidebar's tile counts, not by its own size hint).
class TileGridWidget : public QWidget {
public:
    explicit TileGridWidget(double tilePx, QWidget* parent = nullptr)
        : QWidget(parent), tilePx_(tilePx) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        painter.setPen(QColor(40, 40, 40));
        for (double x = tilePx_; x < width(); x += tilePx_) {
            painter.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
        for (double y = tilePx_; y < height(); y += tilePx_) {
            painter.drawLine(QPointF(0, y), QPointF(width(), y));
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

// Creates the viewport panel directly in the central area (no separate
// window/frame around it), sized to `tilesX x tilesY` tiles at `tilePx`
// pixels each and kept in sync whenever the sidebar's X/Y fields change. A
// scroll area handles the case where the panel outgrows the visible space.
QScrollArea* createViewportPanel(QWidget* parent, double tilePx, QLineEdit* xEdit, QLineEdit* yEdit) {
    auto* grid = new TileGridWidget(tilePx);

    auto resizeToTiles = [grid, tilePx, xEdit, yEdit] {
        const int tilesX = std::max(1, xEdit->text().toInt());
        const int tilesY = std::max(1, yEdit->text().toInt());
        grid->setFixedSize(qRound(tilesX * tilePx), qRound(tilesY * tilePx));
    };
    QObject::connect(xEdit, &QLineEdit::textChanged, resizeToTiles);
    QObject::connect(yEdit, &QLineEdit::textChanged, resizeToTiles);
    resizeToTiles();

    auto* scrollArea = new QScrollArea(parent);
    scrollArea->setWidget(grid);
    scrollArea->setWidgetResizable(false);
    scrollArea->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return scrollArea;
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
    const QRect screenGeometry = screen->geometry();
    window.resize(screenGeometry.width() >> 1, screenGeometry.height() >> 1);
    const double tilePx = tilePxForScreen(screenGeometry);

    const Sidebar sidebar = createSidebar(&window);
    window.setCentralWidget(createViewportPanel(&window, tilePx, sidebar.xEdit, sidebar.yEdit));
    window.addDockWidget(Qt::RightDockWidgetArea, sidebar.dock);

    window.show();

    // QApplication::exec() returns once the window is closed (including via
    // the taskbar/titlebar X, which QMainWindow handles by default -- no
    // closeEvent override needed) or QApplication::quit() is called. Qt
    // tears down its own event loop and widgets on the way out, so no
    // manual buffer flushing is needed here.
    return app.exec();
}
