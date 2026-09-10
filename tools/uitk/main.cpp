// 'platform NES' UI Toolkit

#include <QApplication>
#include <QMainWindow>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("uitk");
    window.resize(800, 600);
    window.show();

    // QApplication::exec() returns once the window is closed (including via
    // the taskbar/titlebar X, which QMainWindow handles by default -- no
    // closeEvent override needed) or QApplication::quit() is called. Qt
    // tears down its own event loop and widgets on the way out, so no
    // manual buffer flushing is needed here.
    return app.exec();
}

