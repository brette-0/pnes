// 'pnes' Animation Designer
//
// Loads a blobman manifest (tools/blobman, e.g. demo/chr/config.toml), builds a
// table of every included tile with the PPU tile id the hardware would see
// (chr_table.hpp), and shows it 16-across in the black-RGB placeholder palette
// (tile_canvas.hpp) in a panel down the left of the window. Clicking a tile
// picks it; the tabbed area beside the panel holds the metasprites
// (metasprite_page.hpp), each of which is itself tabbed -- one tab per
// animation frame, with its own name, timer, successor and field
// (metasprite_frame.hpp) that stamps the pick.
//
// A manifest can be named on the command line (`animator demo/chr/config.toml`)
// or opened from File -> Open Manifest...; View -> 8x16 Tiles regroups the
// table into sprite pairs.

#include "chr_table.hpp"
#include "codegen.hpp"
#include "exporter.hpp"
#include "metasprite_page.hpp"
#include "palette_store.hpp"
#include "palette_window.hpp"
#include "project.hpp"
#include "tile_canvas.hpp"

#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QToolButton>
#include <QComboBox>
#include <QLabel>
#include <QCloseEvent>
#include <QBoxLayout>
#include <QWidget>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QStyle>
#include <QFrame>
#include <QSignalBlocker>
#include <QString>
#include <QFileInfo>
#include <QDir>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace {

// Reserve room for the window's own frame when sizing against the screen, as
// uitk does -- a window requested at exactly the available height is taller
// than what fits once its title bar and borders are added.
constexpr int kWindowChromeMarginPx = 60;

/// Fallback window height when the screen is unexpectedly tiny.
constexpr int kMinWindowHeightPx = 320;

/// Starting width for the metasprite area when a page asks for less than this
/// -- a 1x1 or 2x2 metasprite is a very small field, and a column barely wider
/// than its attribute section looks broken. Scaled off the sheet's zoom, which
/// is itself derived from the monitor (so 480px at 1080p, 960px at 4K).
constexpr int kMetaspriteAreaMinPxAt1080p = 480;
constexpr int kZoomAt1080p = 4;


class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(int zoom) : zoom_(zoom) {
        setWindowTitle("pnes Animation Designer");

        // The sheet is a permanent panel, not a document: it is where every
        // stamp comes from, so it stays visible whichever metasprite is being
        // worked on. Only the metasprites are tabbed.
        auto* central = new QWidget(this);
        auto* columns = new QHBoxLayout(central);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(0);
        setCentralWidget(central);

        canvas_ = new TileCanvas(zoom, nullptr);
        connect(canvas_, &TileCanvas::cellHovered, this, &MainWindow::showCell);
        connect(canvas_, &TileCanvas::cellPicked, this, &MainWindow::pickCell);

        sheetScroll_ = new QScrollArea(central);
        // The canvas is exactly 16 cells wide at a fixed zoom, so it never
        // reflows to the viewport: keep it at its sizeHint (widgetResizable
        // off) and let the scroll area scroll it vertically. The horizontal
        // bar is off because the width is the one thing that always fits --
        // the window is sized from it below.
        sheetScroll_->setWidget(canvas_);
        sheetScroll_->setWidgetResizable(false);
        sheetScroll_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        sheetScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        sheetScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sheetScroll_->setFrameShape(QFrame::StyledPanel);
        // Exactly 16 cells plus its scrollbar: the canvas cannot use more
        // width and anything less would clip a column, so the panel is that
        // wide and the metasprite area takes the rest.
        sheetScroll_->setFixedWidth(sheetWidth());

        // The sheet's palette is a VIEW setting, independent of what any
        // metasprite uses: picking tiles is easier in the colours they will be
        // drawn in, but the sheet is not part of any one metasprite.
        sheetPalette_ = new QComboBox(central);
        connect(sheetPalette_, &QComboBox::currentIndexChanged, this, [this] { applySheetPalette(); });

        auto* sheetColumn = new QVBoxLayout;
        sheetColumn->setContentsMargins(0, 0, 0, 0);
        auto* sheetHeader = new QHBoxLayout;
        sheetHeader->addWidget(new QLabel("Palette", central));
        sheetHeader->addWidget(sheetPalette_, 1);
        sheetColumn->addLayout(sheetHeader);
        sheetColumn->addWidget(sheetScroll_, 1);

        auto* sheetPanel = new QWidget(central);
        sheetPanel->setLayout(sheetColumn);
        sheetPanel->setFixedWidth(sheetWidth());
        columns->addWidget(sheetPanel);

        tabs_ = new QTabWidget(central);
        tabs_->setTabsClosable(true);
        tabs_->setDocumentMode(true);
        connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
        connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::tabChanged);

        // Same reasoning as the frame tabs' own "+": the way to add one should
        // be where the tabs are, not only in a menu.
        auto* addMetasprite = new QToolButton(tabs_);
        addMetasprite->setText("+");
        addMetasprite->setToolTip("New metasprite (Ctrl+N)");
        addMetasprite->setAutoRaise(true);
        connect(addMetasprite, &QToolButton::clicked, this, &MainWindow::newMetasprite);
        tabs_->setCornerWidget(addMetasprite, Qt::TopRightCorner);

        columns->addWidget(tabs_, 1);

        // Palettes outlive any one metasprite, so the store is the window's and
        // everything that draws follows it.
        connect(paletteStore_, &PaletteStore::changed, this, &MainWindow::refreshSheetPalettes);
        // Palettes are saved in the .anim, so editing one is unsaved work like
        // any other.
        connect(paletteStore_, &PaletteStore::changed, this, &MainWindow::markDirty);
        refreshSheetPalettes();

        buildMenus();
        statusBar()->showMessage(summary_);
    }

    /// Whether View -> 8x16 Tiles is ticked.
    void setTallTiles(bool tall) { tallTiles_->setChecked(tall); }

    /**
     * @brief Loads @p path's tiles into the sheet, reporting the outcome.
     * @param quiet Report failures on stderr only, with no modal dialog --
     *              used for the command-line manifest, where a dialog would
     *              pop up before the window is even on screen.
     * @return Whether the manifest loaded.
     */
    bool loadManifest(const QString& path, bool quiet = false) {
        const chr::TileMode mode = tileMode();

        std::string error;
        auto table = chr::loadManifest(path.toStdString(), mode, error);
        if (!table) {
            const QString message = QString("Cannot load %1: %2")
                                        .arg(QFileInfo(path).fileName(), QString::fromStdString(error));
            std::fprintf(stderr, "animator: %s\n", qPrintable(message));
            statusBar()->showMessage(message);
            if (!quiet) {
                QMessageBox::warning(this, "Open Manifest", message);
            }
            return false;
        }

        const bool differentManifest = manifestPath_ != path;
        table_ = std::move(table);
        manifestPath_ = path;
        reload_->setEnabled(true);
        canvas_->setTable(*table_);
        canvas_->resize(canvas_->sizeHint());
        sheetScroll_->verticalScrollBar()->setValue(0);
        // Which manifest the tiles come from is recorded in the .anim, so
        // switching to another one is a change to the project.
        if (differentManifest) markDirty();
        refreshTitle();

        // Cell indices are only meaningful for the table they came from, and
        // this is a different table even when it is the same file -- so the
        // pick is dropped rather than silently pointing at another tile.
        // Already-placed sprites keep their own copy of the art and stay.
        clearPick();

        // A slot on every field means whatever the sheet now says a tile is.
        for (MetaspritePage* page : pages_) {
            page->setTileMode(mode);
        }

        for (const std::string& warning : table_->warnings) {
            std::fprintf(stderr, "animator: warning: %s\n", warning.c_str());
        }

        summary_ = QString("%1: %2 %3 from %4 %5")
                       .arg(QFileInfo(path).fileName())
                       .arg(canvas_->cellCount())
                       .arg(mode == chr::TileMode::Size8x16 ? "cells" : "tiles")
                       .arg(table_->entries.size())
                       .arg(table_->entries.size() == 1 ? "entry" : "entries");
        if (!table_->warnings.empty()) {
            summary_ += QString(" (%1 warning%2 -- see stderr)")
                            .arg(table_->warnings.size())
                            .arg(table_->warnings.size() == 1 ? "" : "s");
        }
        std::fprintf(stderr, "animator: %s\n", qPrintable(summary_));
        statusBar()->showMessage(summary_);
        return true;
    }

    /// Width the sheet panel needs: 16 cells at the current zoom, plus the
    /// vertical scrollbar that is always shown, plus the panel frame.
    [[nodiscard]] int sheetWidth() const {
        const int scrollBarPx = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, sheetScroll_);
        const int framePx = 2 * sheetScroll_->frameWidth();
        return kCellsPerRow * chr::kTilePixels * zoom_ + scrollBarPx + framePx;
    }

    /**
     * @brief Sizes the window to the sheet panel plus room for a metasprite,
     *        as tall as the screen comfortably allows.
     */
    void fitToScreen(const QRect& available) {
        // The menu bar and status bar sit outside the central widget, so the
        // window has to be that much taller than the columns it holds.
        const int chromeHeight = menuBar()->sizeHint().height() + statusBar()->sizeHint().height();

        const int wanted = sheetWidth() + metaspriteAreaWidth();
        const int width = std::min(wanted, available.width() - kWindowChromeMarginPx);
        const int height = std::max(kMinWindowHeightPx,
                                    available.height() - kWindowChromeMarginPx - chromeHeight);
        resize(std::max(1, width), height);
    }

    /// Starting width for the metasprite area: what the current page asks for,
    /// floored at something a field is comfortable in. It is only a starting
    /// point -- the area takes whatever width is left over after the sheet
    /// panel, and the page scrolls when that is less than its field.
    [[nodiscard]] int metaspriteAreaWidth() const {
        const int wanted = tabs_->currentWidget() ? tabs_->currentWidget()->sizeHint().width() : 0;
        const int floorPx = kMetaspriteAreaMinPxAt1080p * zoom_ / kZoomAt1080p;
        return std::max(wanted, floorPx);
    }

    /// Adds a metasprite tab and shows it.
    void newMetasprite() {
        auto* page = new MetaspritePage(zoom_, paletteStore_, tabs_);
        connect(page, &MetaspritePage::statusChanged, this, [this, page](const QString& text) {
            // Only the visible page talks to the status bar; a background tab
            // emitting on its way out would otherwise overwrite the message.
            if (tabs_->currentWidget() == page) {
                statusBar()->showMessage(text);
            }
        });
        connect(page, &MetaspritePage::framesChanged, this, [this, page] { retitle(page); });
        connect(page, &MetaspritePage::nameChanged, this, [this, page] { retitle(page); });
        connect(page, &MetaspritePage::dirtied, this, &MainWindow::markDirty);
        page->setTileMode(tileMode());

        if (pick_) {
            page->setBrush(&table_->cells[*pick_], pickEntry_);
        }

        page->setName(QString("metasprite_%1").arg(++metaspriteCount_));

        const int index = tabs_->addTab(page, page->name());
        pages_.push_back(page);
        // The page made its first frame during construction, before any of the
        // connections above existed, so the label is brought up to date here
        // rather than waiting for the next change.
        retitle(page);
        tabs_->setCurrentIndex(index);
    }

    /// Adds a metasprite with no frames, for a load to fill in.
    MetaspritePage* appendMetasprite() {
        newMetasprite();
        MetaspritePage* page = pages_.empty() ? nullptr : pages_.back();
        if (page) page->clearFrames();
        return page;
    }

protected:
    /// Every way the window can close comes through here -- the title bar, File
    /// -> Quit, Ctrl+Q, the session ending -- so this is the one place the offer
    /// to save has to be made.
    void closeEvent(QCloseEvent* event) override {
        if (confirmDiscard("closing")) {
            event->accept();
        } else {
            event->ignore();
        }
    }

private:
    void buildMenus() {
        QMenu* file = menuBar()->addMenu("&File");
        QAction* openProject = file->addAction("Open &Project...");
        openProject->setShortcut(QKeySequence("Ctrl+Shift+O"));
        connect(openProject, &QAction::triggered, this, &MainWindow::chooseProject);
        QAction* saveProject = file->addAction("&Save Project");
        saveProject->setShortcut(QKeySequence::Save);
        connect(saveProject, &QAction::triggered, this, [this] { saveProjectTo(projectPath_); });
        QAction* saveProjectAs = file->addAction("Save Project &As...");
        saveProjectAs->setShortcut(QKeySequence::SaveAs);
        connect(saveProjectAs, &QAction::triggered, this, [this] { saveProjectTo(QString()); });
        file->addSeparator();
        // Exporting is not saving: it writes generated C++ beside nothing in
        // particular and leaves the project exactly as dirty as it was.
        exportOne_ = file->addAction("&Export Header...");
        exportOne_->setShortcut(QKeySequence("Ctrl+E"));
        connect(exportOne_, &QAction::triggered, this, &MainWindow::exportCurrent);
        exportAll_ = file->addAction("Export All &Headers...");
        exportAll_->setShortcut(QKeySequence("Ctrl+Shift+E"));
        connect(exportAll_, &QAction::triggered, this, &MainWindow::exportAll);
        file->addSeparator();
        QAction* open = file->addAction("&Open Manifest...");
        open->setShortcut(QKeySequence::Open);
        connect(open, &QAction::triggered, this, &MainWindow::chooseManifest);
        reload_ = file->addAction("&Reload Manifest");
        reload_->setShortcut(QKeySequence::Refresh);
        reload_->setEnabled(false);
        connect(reload_, &QAction::triggered, this, [this] { loadManifest(manifestPath_); });
        file->addSeparator();
        QAction* newMeta = file->addAction("&New Metasprite");
        newMeta->setShortcut(QKeySequence::New);
        connect(newMeta, &QAction::triggered, this, &MainWindow::newMetasprite);
        closeMeta_ = file->addAction("&Close Metasprite");
        closeMeta_->setShortcut(QKeySequence::Close);
        connect(closeMeta_, &QAction::triggered, this, [this] { closeTab(tabs_->currentIndex()); });
        file->addSeparator();
        newFrame_ = file->addAction("New &Frame");
        newFrame_->setShortcut(QKeySequence("Ctrl+T"));
        connect(newFrame_, &QAction::triggered, this, [this] {
            if (MetaspritePage* page = currentPage()) page->newFrame();
        });
        closeFrame_ = file->addAction("Close &Frame");
        closeFrame_->setShortcut(QKeySequence("Ctrl+Shift+W"));
        connect(closeFrame_, &QAction::triggered, this, [this] {
            if (MetaspritePage* page = currentPage()) page->closeFrame();
        });
        file->addSeparator();
        QAction* quit = file->addAction("&Quit");
        quit->setShortcut(QKeySequence::Quit);
        connect(quit, &QAction::triggered, this, &QWidget::close);

        QMenu* edit = menuBar()->addMenu("&Edit");
        // Sprites belong to a frame, not to the metasprite, so this clears
        // the frame being looked at rather than the whole animation.
        clearFrame_ = edit->addAction("Clear &Frame");
        connect(clearFrame_, &QAction::triggered, this, [this] {
            MetaspritePage* page = currentPage();
            if (MetaspriteFrame* frame = page ? page->currentFrame() : nullptr) {
                frame->field()->clearSprites();
            }
        });

        QMenu* window = menuBar()->addMenu("&Window");
        QAction* palettes = window->addAction("&Palettes...");
        connect(palettes, &QAction::triggered, this, &MainWindow::showPalettes);

        QMenu* view = menuBar()->addMenu("&View");
        tallTiles_ = view->addAction("&8x16 Tiles");
        tallTiles_->setCheckable(true);
        // Regrouping changes the cells themselves, so the manifest is simply
        // re-read -- cheap, and it keeps one code path for building a table.
        connect(tallTiles_, &QAction::toggled, this, [this] {
            // The mode is a project setting, so changing it is unsaved work
            // whether or not a manifest is loaded to re-read.
            markDirty();
            if (!manifestPath_.isEmpty()) loadManifest(manifestPath_);
        });

        tabChanged(tabs_->currentIndex());
    }

public:
    /// Records that there is work a save would keep, and shows it in the title
    /// bar the way every editor does.
    void markDirty() {
        if (dirty_) return;
        dirty_ = true;
        refreshTitle();
    }

    /// Forgets that there is unsaved work. Called after a save or a load, and
    /// once at startup -- the metasprite the window opens with is scaffolding,
    /// not something the user would want to be asked about.
    void markClean() {
        dirty_ = false;
        refreshTitle();
    }

private:
    void refreshTitle() {
        QString title = "pnes Animation Designer";
        if (!projectPath_.isEmpty()) {
            title += QString(" -- %1").arg(QFileInfo(projectPath_).fileName());
        } else if (!manifestPath_.isEmpty()) {
            title += QString(" -- %1").arg(QFileInfo(manifestPath_).fileName());
        }
        if (dirty_) title += " *";
        setWindowTitle(title);
    }

    /**
     * @brief Offers to save before something that discards unsaved work.
     *
     * @param action What is about to happen, for the prompt ("closing", "opening
     *               another project").
     * @return Whether to go ahead. False means the user cancelled, or chose to
     *         save and the save did not happen (a cancelled Save As, or a write
     *         that failed) -- in which case nothing should be discarded.
     */
    bool confirmDiscard(const QString& action) {
        if (!dirty_) return true;

        const auto answer = QMessageBox::warning(
            this, "Unsaved changes",
            QString("This project has unsaved changes to its metasprites, palettes or settings.\n\n"
                    "Save before %1?")
                .arg(action),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

        if (answer == QMessageBox::Cancel) return false;
        if (answer == QMessageBox::Discard) return true;

        saveProjectTo(projectPath_);
        // saveProjectTo clears the flag only when it actually wrote, so this
        // covers a cancelled Save As dialog and a failed write alike.
        return !dirty_;
    }

    /// Opens the palette editor, or brings it forward if it is already open.
    void showPalettes() {
        if (!paletteWindow_) {
            paletteWindow_ = new PaletteWindow(paletteStore_, this);
        }
        paletteWindow_->show();
        paletteWindow_->raise();
        paletteWindow_->activateWindow();
    }

    /// Rebuilds the sheet's palette dropdown, keeping the choice by name.
    void refreshSheetPalettes() {
        const QString wanted = sheetPalette_->currentIndex() <= 0 ? QString()
                                                                  : sheetPalette_->currentText();
        {
            const QSignalBlocker blocked(sheetPalette_);
            sheetPalette_->clear();
            sheetPalette_->addItem("(placeholder)");
            sheetPalette_->addItems(paletteStore_->names());

            const int index = wanted.isEmpty() ? 0 : sheetPalette_->findText(wanted);
            sheetPalette_->setCurrentIndex(index >= 0 ? index : 0);
        }
        // The colours may have changed even when the list has not.
        applySheetPalette();
    }

    void applySheetPalette() {
        const QString name =
            sheetPalette_->currentIndex() <= 0 ? QString() : sheetPalette_->currentText();
        canvas_->setPaletteTable(paletteTable(name.isEmpty() ? nullptr : paletteStore_->find(name)));
    }

    /// The page on the current tab, or nullptr when there are no tabs.
    [[nodiscard]] MetaspritePage* currentPage() const {
        return qobject_cast<MetaspritePage*>(tabs_->currentWidget());
    }

    /// How the sheet is currently grouping tiles, which is also what a slot on
    /// every field means.
    [[nodiscard]] chr::TileMode tileMode() const {
        return tallTiles_->isChecked() ? chr::TileMode::Size8x16 : chr::TileMode::Size8x8;
    }

    void closeTab(int index) {
        QWidget* widget = tabs_->widget(index);
        if (!widget) return;
        if (auto* page = qobject_cast<MetaspritePage*>(widget)) {
            // Closing a metasprite discards every frame in it, and nothing
            // undoes that, so one with sprites in it asks first. Suppressed
            // while a project is being opened, where the tabs being closed are
            // the ones just replaced.
            if (!replacing_ && page->hasContent()) {
                const auto answer = QMessageBox::question(
                    this, "Close metasprite",
                    QString("Close '%1'? Its %2 frame%3 and their sprites will be discarded.")
                        .arg(page->name())
                        .arg(page->frameCount())
                        .arg(page->frameCount() == 1 ? "" : "s"),
                    QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
                if (answer != QMessageBox::Close) return;
            }
            markDirty();
            pages_.erase(std::remove(pages_.begin(), pages_.end(), page), pages_.end());
        }
        tabs_->removeTab(index);
        widget->deleteLater();
    }

    void tabChanged(int) {
        // QTabWidget emits currentChanged for the first tab added, which
        // happens in the constructor before buildMenus() has made the actions
        // this would enable.
        if (!closeMeta_ || !clearFrame_) return;

        MetaspritePage* page = currentPage();
        closeMeta_->setEnabled(page != nullptr);
        newFrame_->setEnabled(page != nullptr);
        exportOne_->setEnabled(page != nullptr);
        exportAll_->setEnabled(!pages_.empty());
        closeFrame_->setEnabled(page && page->currentFrame() != nullptr);
        clearFrame_->setEnabled(page && page->currentFrame() != nullptr);
        statusBar()->showMessage(page ? page->summary() : summary_);
    }

    /// A metasprite tab's label carries its frame count -- the one number
    /// worth seeing without switching to it. The frame tabs inside carry their
    /// own names.
    void retitle(MetaspritePage* page) {
        const int index = tabs_->indexOf(page);
        if (index < 0) return;
        // The label is built from the page's own name, so a rename needs
        // nothing parsed back out of the old label.
        const QString name = page->name().trimmed();
        tabs_->setTabText(index, QString("%1 (%2)")
                                     .arg(name.isEmpty() ? QString("metasprite") : name)
                                     .arg(page->frameCount()));
        // Closing the last frame of a metasprite has to grey out the actions
        // that need one.
        closeFrame_->setEnabled(page->currentFrame() != nullptr);
        clearFrame_->setEnabled(page->currentFrame() != nullptr);
    }

    /// A cell on the sheet became the pick; every field stamps it from now on.
    void pickCell(int index) {
        if (!table_ || index < 0 || static_cast<std::size_t>(index) >= table_->cells.size()) return;

        pick_ = index;
        const chr::Cell&  cell  = table_->cells[static_cast<std::size_t>(index)];
        const chr::Entry& entry = table_->entries[cell.entry];
        pickEntry_ = QString::fromStdString(entry.name);

        for (MetaspritePage* page : pages_) {
            page->setBrush(&cell, pickEntry_);
        }
        statusBar()->showMessage(QString("Picked %1::%2 tile %3  id %4")
                                     .arg(pickEntry_, QString::fromStdString(cell.include))
                                     .arg(cell.tileInInclude)
                                     .arg(formatCellId(cell)));
    }

    void clearPick() {
        pick_.reset();
        pickEntry_.clear();
        canvas_->setSelected(-1);
        for (MetaspritePage* page : pages_) {
            page->setBrush(nullptr, QString());
        }
    }

    /// The cell a saved sprite names. entry/include/tile identifies the art
    /// exactly; a tile id alone does not, since two entries at the same offset
    /// into their VMAs share ids. The id is the fallback for a manifest that
    /// has been reorganised since the save.
    /// The loaded sheet's cell for a saved sprite, by the same rule the
    /// headless export uses (proj::findCell) -- so opening a project and
    /// exporting it from a script resolve every sprite to the same cell.
    [[nodiscard]] const chr::Cell* findCell(const std::string& entry, const std::string& include,
                                            int tile, int id) const {
        return table_ ? proj::findCell(*table_, entry, include, tile, id) : nullptr;
    }

    static QString formatCellId(const chr::Cell& cell) {
        // An 8x16 cell occupies two consecutive ids, so both are worth showing
        // -- and the byte that selects the pair is neither of them when the
        // entry's VMA is the $1000 half, so that is shown beside them rather
        // than left for someone to work out from the VMA in the same line.
        return cell.heightTiles == 2
            ? QString("$%1-$%2, OAM $%3")
                  .arg(cell.id, 2, 16, QChar('0'))
                  .arg(cell.id + 1, 2, 16, QChar('0'))
                  .arg(cell.patternByte, 2, 16, QChar('0'))
            : QString("$%1").arg(cell.id, 2, 16, QChar('0'));
    }

    /// Status line for the hovered cell, or the table summary when @p index is
    /// -1 (pointer over empty space).
    void showCell(int index) {
        if (!table_ || index < 0 || static_cast<std::size_t>(index) >= table_->cells.size()) {
            statusBar()->showMessage(summary_);
            return;
        }

        const chr::Cell&  cell  = table_->cells[static_cast<std::size_t>(index)];
        const chr::Entry& entry = table_->entries[cell.entry];
        statusBar()->showMessage(QString("%1::%2  tile %3  id %4  (entry VMA $%5, %6x%7)")
                                     .arg(QString::fromStdString(entry.name),
                                          QString::fromStdString(cell.include))
                                     .arg(cell.tileInInclude)
                                     .arg(formatCellId(cell))
                                     .arg(entry.vma, 4, 16, QChar('0'))
                                     .arg(cell.widthPx())
                                     .arg(cell.heightPx()));
    }

public:
    // ---- project files ----

    /// Assembles the project from the window's own state.
    [[nodiscard]] proj::Project buildProject(const QString& path) const {
        proj::Project project;
        project.path = path.toStdString();
        project.mode = tileMode();
        project.metaspritesDir = metaspritesDir_;

        // The manifest path is stored relative to the .anim, so a project and
        // its art can be moved or checked out anywhere together.
        std::error_code ec;
        std::filesystem::path base = project.path.parent_path();
        if (base.empty()) base = ".";
        const auto relative = std::filesystem::relative(
            std::filesystem::absolute(manifestPath_.toStdString()), std::filesystem::absolute(base), ec);
        project.chrManifest = (ec || relative.empty()) ? manifestPath_.toStdString() : relative.generic_string();

        project.palettes = paletteStore_->palettes();

        for (const MetaspritePage* page : pages_) {
            project.metasprites.push_back(buildMetasprite(page));
        }

        return project;
    }

    /// One metasprite as the window has it -- what a save writes and what an
    /// export generates from, so the two cannot disagree about what a frame is.
    [[nodiscard]] proj::Metasprite buildMetasprite(const MetaspritePage* page) const {
        proj::Metasprite metasprite;
        metasprite.palette = page->paletteName().toStdString();
        metasprite.name      = page->name().toStdString();
        metasprite.savedStem = page->savedStem().toStdString();
        metasprite.tilesX = page->tilesX();
        metasprite.tilesY = page->tilesY();

        for (const MetaspriteFrame* frame : page->frames()) {
            proj::Frame out;
            out.name  = frame->frameName().toStdString();
            out.timer = frame->timerFrames();
            out.isPublic = frame->isPublicFrame();

            for (const PlacedSprite& sprite : frame->field()->sprites()) {
                proj::Sprite saved;
                saved.col     = sprite.col;
                saved.row     = sprite.row;
                saved.id      = sprite.id;
                saved.patternByte = sprite.patternByte;
                saved.entry   = sprite.entry.toStdString();
                saved.include = sprite.include.toStdString();
                saved.tile    = sprite.tile;
                saved.flipH   = static_cast<int>(sprite.mirror) &
                                static_cast<int>(Mirror::Horizontal);
                saved.flipV   = static_cast<int>(sprite.mirror) &
                                static_cast<int>(Mirror::Vertical);
                out.sprites.push_back(std::move(saved));
            }

            metasprite.frames.push_back(std::move(out));
        }

        return metasprite;
    }

    /// Saves to @p path, asking for one when it is empty.
    void saveProjectTo(QString path) {
        if (manifestPath_.isEmpty()) {
            QMessageBox::warning(this, "Save Project",
                                 "Open a blobman manifest first -- a project records which one its "
                                 "tiles come from.");
            return;
        }

        if (path.isEmpty()) {
            path = QFileDialog::getSaveFileName(
                this, "Save Project", QDir::currentPath(),
                QString("Animator projects (*%1);;All files (*)").arg(proj::kProjectExtension));
            if (path.isEmpty()) return;
            // A missing extension is added rather than refused: the dialog's
            // filter is a hint, not something it enforces.
            if (!path.endsWith(proj::kProjectExtension)) path += proj::kProjectExtension;
        }

        proj::Project project = buildProject(path);
        std::string error;
        if (!proj::save(project, error)) {
            const QString message = QString("Cannot save %1: %2")
                                        .arg(QFileInfo(path).fileName(), QString::fromStdString(error));
            std::fprintf(stderr, "animator: %s\n", qPrintable(message));
            QMessageBox::warning(this, "Save Project", message);
            return;
        }

        // save() reports the stem each metasprite ended up as, which is what a
        // later rename moves from.
        for (std::size_t i = 0; i < pages_.size() && i < project.metasprites.size(); ++i) {
            pages_[i]->setSavedStem(QString::fromStdString(project.metasprites[i].savedStem));
        }

        projectPath_ = path;
        markClean();
        statusBar()->showMessage(QString("Saved %1 (%2 metasprite%3) beside %4/")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(pages_.size())
                                     .arg(pages_.size() == 1 ? "" : "s")
                                     .arg(QString::fromStdString(metaspritesDir_)));
    }

    // ---- generated headers ----

    /// Where an export starts looking: a `gen` folder beside the project when
    /// there is one -- that is where generated code goes in this repo -- and
    /// the project's own folder otherwise.
    [[nodiscard]] QString exportDir() const {
        const QString base = projectPath_.isEmpty() ? QDir::currentPath()
                                                    : QFileInfo(projectPath_).absolutePath();
        const QString gen = QDir(base).filePath("gen");
        return QFileInfo(gen).isDir() ? gen : base;
    }

    /// Writes the metasprite being edited as one generated header.
    void exportCurrent() {
        MetaspritePage* page = currentPage();
        if (!page) {
            QMessageBox::warning(this, "Export Header", "There is no metasprite to export.");
            return;
        }
        if (page->frameCount() == 0) {
            QMessageBox::warning(this, "Export Header",
                                 QString("'%1' has no frames -- there is no animation to export.")
                                     .arg(page->name()));
            return;
        }

        const proj::Metasprite metasprite = buildMetasprite(page);
        // The file is named after the metasprite, as the class inside it is:
        // the name is the identity here as much as it is in the .msprite.
        const QString suggested = QDir(exportDir()).filePath(QString::fromStdString(
            codegen::identifier(metasprite.name) + codegen::kHeaderExtension));

        QString path = QFileDialog::getSaveFileName(
            this, "Export Header", suggested,
            QString("C++ headers (*%1);;All files (*)").arg(codegen::kHeaderExtension));
        if (path.isEmpty()) return;
        // Same as the project dialogs: a missing extension is added rather than
        // refused, since the filter is a hint and not a rule.
        if (!path.endsWith(codegen::kHeaderExtension)) path += codegen::kHeaderExtension;

        std::string error;
        // The banner names the project the metasprite came from, when it has
        // been saved into one -- the .msprite is what a regeneration reads.
        const std::string source =
            projectPath_.isEmpty() ? std::string() : QFileInfo(projectPath_).fileName().toStdString();
        if (!codegen::writeHeader(metasprite, path.toStdString(), source, error)) {
            const QString message = QString("Cannot export %1: %2")
                                        .arg(QFileInfo(path).fileName(), QString::fromStdString(error));
            std::fprintf(stderr, "animator: %s\n", qPrintable(message));
            QMessageBox::warning(this, "Export Header", message);
            return;
        }

        // An export is not a save: the project is left exactly as dirty as it
        // was, since none of this is in the .anim.
        statusBar()->showMessage(QString("Exported %1 (%2 frame%3 of %4)")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(page->frameCount())
                                     .arg(page->frameCount() == 1 ? "" : "s")
                                     .arg(page->name()));
    }

    /// Writes every metasprite into one folder, a header each, named after the
    /// metasprite.
    void exportAll() {
        if (pages_.empty()) {
            QMessageBox::warning(this, "Export All Headers", "There are no metasprites to export.");
            return;
        }

        const QString dir = QFileDialog::getExistingDirectory(this, "Export All Headers", exportDir());
        if (dir.isEmpty()) return;

        const proj::Project project = buildProject(projectPath_);
        std::vector<std::string> skipped;
        std::string error;
        if (!codegen::writeAll(project, dir.toStdString(), skipped, error)) {
            const QString message = QString("Cannot export into %1: %2")
                                        .arg(QDir(dir).dirName(), QString::fromStdString(error));
            std::fprintf(stderr, "animator: %s\n", qPrintable(message));
            QMessageBox::warning(this, "Export All Headers", message);
            return;
        }

        for (const std::string& note : skipped) {
            std::fprintf(stderr, "animator: warning: %s\n", note.c_str());
        }

        const std::size_t written = project.metasprites.size() - skipped.size();
        QString message = QString("Exported %1 header%2 into %3/")
                              .arg(written)
                              .arg(written == 1 ? "" : "s")
                              .arg(QDir(dir).dirName());
        if (!skipped.empty()) {
            message += QString(" (%1 skipped -- see stderr)").arg(skipped.size());
        }
        statusBar()->showMessage(message);
    }

    void chooseProject() {
        if (!confirmDiscard("opening another project")) return;
        const QString path = QFileDialog::getOpenFileName(
            this, "Open Project", QDir::currentPath(),
            QString("Animator projects (*%1);;All files (*)").arg(proj::kProjectExtension));
        if (!path.isEmpty()) openProject(path);
    }

    /// Replaces everything in the window with @p path's contents.
    bool openProject(const QString& path) {
        std::string error;
        const auto project = proj::load(path.toStdString(), error);
        if (!project) {
            const QString message = QString("Cannot open %1: %2")
                                        .arg(QFileInfo(path).fileName(), QString::fromStdString(error));
            std::fprintf(stderr, "animator: %s\n", qPrintable(message));
            QMessageBox::warning(this, "Open Project", message);
            return false;
        }

        for (const std::string& warning : project->warnings) {
            std::fprintf(stderr, "animator: warning: %s\n", warning.c_str());
        }

        // The mode has to be in place before the manifest is read, since it is
        // what decides how cells are grouped -- and set without a signal, or
        // the toggle would re-read the manifest from under this.
        {
            const QSignalBlocker blocked(tallTiles_);
            tallTiles_->setChecked(project->mode == chr::TileMode::Size8x16);
        }

        const auto manifest = proj::resolve(*project, project->chrManifest);
        if (!loadManifest(QString::fromStdString(manifest.string()))) {
            // loadManifest has already said why; the project is not opened,
            // since every sprite in it names tiles from that manifest.
            return false;
        }

        replacing_ = true;
        while (tabs_->count() > 0) {
            closeTab(0);
        }
        replacing_ = false;
        // Palettes go in before the pages, so every page's dropdown already
        // has the name its metasprite asks for.
        paletteStore_->setPalettes(project->palettes);
        metaspriteCount_ = 0;
        metaspritesDir_ = project->metaspritesDir;
        projectPath_ = path;

        int missing = 0;
        for (const proj::Metasprite& metasprite : project->metasprites) {
            MetaspritePage* page = appendMetasprite();
            if (!page) continue;
            page->setName(QString::fromStdString(metasprite.name));
            page->setPaletteName(QString::fromStdString(metasprite.palette));
            page->setSavedStem(QString::fromStdString(metasprite.savedStem));
            page->setSize(metasprite.tilesX, metasprite.tilesY);

            for (const proj::Frame& frame : metasprite.frames) {
                MetaspriteFrame* built = page->appendFrame();
                if (!built) continue;
                built->setFrameName(QString::fromStdString(frame.name));
                built->setTimerFrames(frame.timer);
                built->setPublicFrame(frame.isPublic);

                for (const proj::Sprite& sprite : frame.sprites) {
                    const chr::Cell* cell =
                        findCell(sprite.entry, sprite.include, sprite.tile, sprite.id);
                    if (!cell) {
                        ++missing;
                        continue;
                    }
                    const auto mirror = static_cast<Mirror>(
                        (sprite.flipH ? static_cast<int>(Mirror::Horizontal) : 0) |
                        (sprite.flipV ? static_cast<int>(Mirror::Vertical) : 0));
                    if (!built->field()->addSprite(*cell, QString::fromStdString(sprite.entry),
                                                   sprite.col, sprite.row, mirror)) {
                        ++missing;
                    }
                }
            }

            if (page->frameCount() == 0) page->newFrame();  // never leave a metasprite frameless
        }

        if (tabs_->count() > 0) tabs_->setCurrentIndex(0);

        markClean();
        QString message = QString("Opened %1: %2 metasprite%3")
                              .arg(QFileInfo(path).fileName())
                              .arg(project->metasprites.size())
                              .arg(project->metasprites.size() == 1 ? "" : "s");
        if (missing != 0) {
            message += QString(" -- %1 sprite%2 could not be placed (see stderr)")
                           .arg(missing)
                           .arg(missing == 1 ? "" : "s");
            std::fprintf(stderr,
                         "animator: warning: %d sprite(s) named art or slots this manifest and "
                         "extent do not have\n", missing);
        }
        if (!project->warnings.empty()) {
            message += QString(" (%1 file warning%2 -- see stderr)")
                           .arg(project->warnings.size())
                           .arg(project->warnings.size() == 1 ? "" : "s");
        }
        statusBar()->showMessage(message);
        return true;
    }

private:
    void chooseManifest() {
        const QString start =
            manifestPath_.isEmpty() ? QDir::currentPath() : QFileInfo(manifestPath_).absolutePath();
        const QString path = QFileDialog::getOpenFileName(this, "Open Manifest", start,
                                                          "blobman manifests (*.toml);;All files (*)");
        if (!path.isEmpty()) {
            loadManifest(path);
        }
    }

    int                           zoom_;
    QTabWidget*                   tabs_ = nullptr;
    QComboBox*                    sheetPalette_ = nullptr;
    PaletteStore*                 paletteStore_ = new PaletteStore(this);
    PaletteWindow*                paletteWindow_ = nullptr;
    QScrollArea*                  sheetScroll_ = nullptr;
    TileCanvas*                   canvas_ = nullptr;
    QAction*                      reload_ = nullptr;
    QAction*                      closeMeta_ = nullptr;
    QAction*                      newFrame_ = nullptr;
    QAction*                      closeFrame_ = nullptr;
    QAction*                      clearFrame_ = nullptr;
    QAction*                      exportOne_ = nullptr;
    QAction*                      exportAll_ = nullptr;
    QAction*                      tallTiles_ = nullptr;
    std::optional<chr::Table>     table_;
    std::vector<MetaspritePage*>  pages_;
    std::optional<int>            pick_;        ///< Picked cell index into table_->cells.
    QString                       pickEntry_;   ///< blobman entry the pick belongs to.
    QString                       projectPath_; ///< The .anim being edited, if any.
    std::string                   metaspritesDir_ = proj::kDefaultMetaspritesDir;
    int                           metaspriteCount_ = 0;
    bool                          dirty_ = false;
    bool                          replacing_ = false;  ///< True while a project load swaps the tabs out.
    QString                       manifestPath_;
    QString                       summary_ = "No manifest loaded";
};

} // namespace

int main(int argc, char** argv) {
    // --export-all switches into the headless export (exporter::run) instead
    // of opening anything -- parsed by hand, ahead of QApplication ever
    // touching argv, because the export needs no Qt at all: every piece of it
    // (proj, chr, codegen) is Qt-free, so this path works over ssh, from a
    // script, or in a CI job with no display server. It is the same
    // exporter::run the standalone `animator-export` binary is, so the two
    // write identical headers; see exporter.hpp.
    {
        // The GUI's own switches, so --help from this binary describes the
        // whole of it and not just the export half.
        static constexpr const char* kGuiUsage =
            "  -t, --tall         open a bare manifest with 8x16 tile grouping\n"
            "\n"
            "Without -E/--export-all the designer opens, on the project or "
            "manifest given.\n";
        exporter::Options options;
        switch (exporter::parseArgs(argc, argv, options, /*exportImplied=*/false, kGuiUsage)) {
            case exporter::Parse::Export: return exporter::run(options);
            case exporter::Parse::Help:   return 0;
            case exporter::Parse::Error:  return 1;
            case exporter::Parse::NoExport: break;  // open the window as usual
        }
    }

    QApplication app(argc, argv);

    // Everything QApplication didn't claim (its own -style, -platform, ... are
    // stripped from argv by its constructor): an optional -t/--tall for 8x16
    // grouping, and one file to open -- a project (.anim) or a blobman
    // manifest, told apart by extension, since both are TOML.
    bool tall = false;
    QString file;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "-t" || arg == "--tall") {
            tall = true;
        } else if (file.isEmpty()) {
            file = arg;
        } else {
            std::fprintf(stderr, "animator: unexpected argument '%s'\n", qPrintable(arg));
            return 1;
        }
    }
    const bool isProject = file.endsWith(proj::kProjectExtension, Qt::CaseInsensitive);

    // Scale against the screen the pointer is on, like uitk -- on a mixed-DPI
    // multi-monitor setup that is the one the window is about to open on.
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) {
        screen = app.primaryScreen();
    }
    const QRect geometry = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
    const QRect available = screen ? screen->availableGeometry() : geometry;

    MainWindow window(zoomForScreen(geometry));
    // A project carries its own mode, so -t/--tall only applies to a bare
    // manifest.
    if (!isProject) window.setTallTiles(tall);
    if (!file.isEmpty() && !isProject) {
        window.loadManifest(file, /*quiet=*/true);
    }
    // One metasprite tab to start with, so the field is there to use rather
    // than something to go and create first. The sheet beside it is always
    // visible, so nothing has to be switched to in order to pick a tile. A
    // project replaces the lot, so it is opened instead.
    if (isProject) {
        window.openProject(file);
    } else {
        window.newMetasprite();
    }
    // Nothing done up to here is the user's unsaved work, so the window starts
    // clean -- otherwise quitting straight after launch would ask about a save.
    window.markClean();
    window.fitToScreen(available);
    window.show();

    return app.exec();
}
