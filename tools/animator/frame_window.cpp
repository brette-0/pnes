#include "frame_window.hpp"

#include "tile_image.hpp"

#include <QBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>

#include <algorithm>
#include <map>
#include <string>

namespace {

/// What a window with no entry is shown as. An empty combo item would read as
/// a missing choice rather than the deliberate one it is.
constexpr const char* kNoEntry = "(reserved -- not this project's)";

/// Pattern tables a frame can sit in. Both are PPU addresses, which is how
/// every other part of this project names them.
constexpr std::uint32_t kPatternTables[] = { 0x0000, 0x1000 };

QString addressLabel(const std::uint32_t address) {
    return QString("$%1").arg(address, 4, 16, QChar('0'));
}

QString idLabel(const int id) {
    return QString("$%1").arg(id, 2, 16, QChar('0'));
}

} // namespace

/**
 * @brief The frame as the PPU would address it: 256 tile ids, 16 across.
 *
 * A frame IS a pattern table's worth of ids, and 16 across is how every NES
 * tool draws one -- so a row is one high nibble of the id and a column the
 * low. Each window is banded and labelled with the ids it holds, and the art
 * of whatever entry it names is drawn into it tile by tile, so what is being
 * set up is visible while it is being set up rather than described in a fit
 * note beside it.
 *
 * A window naming no entry is left as the background with its id range shown:
 * reserved ids are a real part of the arrangement, not a gap.
 */
class FramePreview final : public QWidget {
public:
    FramePreview(const chr::Table& tiles, QWidget* parent)
        : QWidget(parent), palette_(paletteTable(nullptr)) {
        // Keyed by entry name and tile-in-entry, which is exactly what a frame
        // resolves an id through -- so drawing a window is a lookup per tile
        // with no searching.
        for (const chr::Cell& cell : tiles.cells) {
            art_[tiles.entries[cell.entry].name][cell.tileInEntry] =
                cellImage(cell, palette_);
        }
        setMinimumSize(sizeHint());
    }

    /// Shows @p frame. Cheap enough to call on every keystroke of an edit.
    void setFrame(const chr::Frame* frame) {
        frame_ = frame ? *frame : chr::Frame{};
        has_ = frame != nullptr;
        update();
    }

    [[nodiscard]] QSize sizeHint() const override {
        return { kColumns * kTilePx + kLabelPx, kFrameRows * kTilePx };
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Base));

        if (!has_ || frame_.windows.empty()) {
            painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
            painter.drawText(rect(), Qt::AlignCenter, "No frame selected");
            return;
        }

        const int windowTiles = chr::kFrameTiles / static_cast<int>(frame_.windows.size());

        for (std::size_t w = 0; w < frame_.windows.size(); ++w) {
            const int base = static_cast<int>(w) * windowTiles;
            const QRect band = bandRect(base, windowTiles);

            // Alternating bands rather than one colour per window: the point
            // is where a window STARTS, and any two neighbours differing is
            // enough to show that at a glance.
            painter.fillRect(band, (w % 2) ? palette().color(QPalette::AlternateBase)
                                           : palette().color(QPalette::Base));

            const auto held = art_.find(frame_.windows[w]);
            if (held != art_.end()) {
                for (int tile = 0; tile < windowTiles; ++tile) {
                    const auto piece = held->second.find(tile);
                    if (piece == held->second.end()) continue;  // past what the entry fills
                    painter.drawImage(tileRect(base + tile), piece->second);
                }
            }

            // The band's own label, down the left: the ids it holds, and the
            // entry in it -- or nothing, for a reserved one.
            painter.setPen(palette().color(QPalette::Text));
            const QString label = QString("$%1").arg(base, 2, 16, QChar('0'));
            painter.drawText(QRect(0, band.top(), kLabelPx - 4, kTilePx),
                             Qt::AlignRight | Qt::AlignVCenter, label);

            painter.setPen(QPen(palette().color(QPalette::Mid), 1));
            painter.drawLine(band.topLeft(), band.topRight());
        }

        // The grid last, over the art, so a tile boundary stays visible
        // against dark pixels.
        painter.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
        for (int column = 1; column < kColumns; ++column) {
            const int x = kLabelPx + column * kTilePx;
            painter.drawLine(x, 0, x, kFrameRows * kTilePx);
        }

        painter.setPen(QPen(palette().color(QPalette::Shadow), 1));
        painter.drawRect(QRect(kLabelPx, 0, kColumns * kTilePx, kFrameRows * kTilePx)
                             .adjusted(0, 0, -1, -1));
    }

private:
    /// Ids are laid out 16 across, so an id's row is its high nibble.
    static constexpr int kColumns   = 16;
    static constexpr int kFrameRows = chr::kFrameTiles / kColumns;

    /// Zoom. 8x8 art at 2x is small but readable, and a whole frame at 3x no
    /// longer fits beside the editor's controls on a 1080p screen.
    static constexpr int kTilePx  = chr::kTilePixels * 2;
    static constexpr int kLabelPx = 28;

    [[nodiscard]] static QRect tileRect(const int id) {
        return { kLabelPx + (id % kColumns) * kTilePx, (id / kColumns) * kTilePx,
                 kTilePx, kTilePx };
    }

    /// A window is a whole number of rows whenever it is 16 tiles or more,
    /// which every offered window count makes it.
    [[nodiscard]] static QRect bandRect(const int base, const int tiles) {
        return { kLabelPx, (base / kColumns) * kTilePx, kColumns * kTilePx,
                 std::max(1, tiles / kColumns) * kTilePx };
    }

    QVector<QRgb> palette_;

    /// entry name -> tile-in-entry -> that tile, paintable. A map rather than
    /// a vector because an entry's includes need not fill it, so the tile
    /// indices have holes -- and a hole is drawn as the band, not as a tile.
    std::map<std::string, std::map<int, QImage>> art_;

    chr::Frame frame_;
    bool       has_ = false;
};

const std::vector<int>& FrameWindow::windowCounts() {
    // Every divisor of 256 up to a point where a window is a single tile is
    // technically legal, but a frame of 256 one-tile windows is a list nobody
    // can read. These are the ones that correspond to real bank sizes.
    static const std::vector<int> counts = { 1, 2, 4, 8, 16 };
    return counts;
}

FrameWindow::FrameWindow(std::vector<chr::Frame> frames, std::vector<Entry> entries,
                         const chr::Table& tiles, QWidget* parent)
    : QDialog(parent), frames_(std::move(frames)), entries_(std::move(entries)) {
    setWindowTitle("Frames");

    preview_ = new FramePreview(tiles, this);

    original_.reserve(frames_.size());
    for (const chr::Frame& frame : frames_) {
        original_.push_back(QString::fromStdString(frame.name));
    }

    list_ = new QListWidget(this);
    list_->setMinimumWidth(160);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { refreshEditor(); });

    auto* add = new QPushButton("Add", this);
    connect(add, &QPushButton::clicked, this, &FrameWindow::addFrame);
    remove_ = new QPushButton("Remove", this);
    connect(remove_, &QPushButton::clicked, this, &FrameWindow::removeFrame);

    auto* listButtons = new QHBoxLayout;
    listButtons->addWidget(add);
    listButtons->addWidget(remove_);

    auto* listColumn = new QVBoxLayout;
    listColumn->addWidget(list_);
    listColumn->addLayout(listButtons);

    name_ = new QLineEdit(this);
    connect(name_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (loading_) return;
        const int row = selected();
        if (row < 0) return;
        frames_[static_cast<std::size_t>(row)].name = text.toStdString();
        refreshList();
    });

    table_ = new QComboBox(this);
    for (const std::uint32_t address : kPatternTables) {
        table_->addItem(addressLabel(address), QVariant::fromValue(address));
    }
    connect(table_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        if (loading_ || index < 0) return;
        const int row = selected();
        if (row < 0) return;
        frames_[static_cast<std::size_t>(row)].patternTable = kPatternTables[index];
        refreshWindows();
    });

    count_ = new QComboBox(this);
    for (const int count : windowCounts()) {
        count_->addItem(QString("%1 window%2 of %3 tiles")
                            .arg(count)
                            .arg(count == 1 ? "" : "s")
                            .arg(chr::kFrameTiles / count),
                        count);
    }
    connect(count_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        if (loading_ || index < 0) return;
        const int row = selected();
        if (row < 0) return;

        // Windows are positional, so a shrink drops from the END and a grow
        // appends reserved ones -- anything else would renumber the windows
        // that were already assigned.
        auto& windows = frames_[static_cast<std::size_t>(row)].windows;
        windows.resize(static_cast<std::size_t>(windowCounts()[static_cast<std::size_t>(index)]));
        refreshWindows();
    });

    windowBox_ = new QWidget(this);
    windowRows_ = new QGridLayout(windowBox_);
    windowRows_->setContentsMargins(0, 0, 0, 0);

    auto* form = new QFormLayout;
    form->addRow("Name", name_);
    form->addRow("Pattern table", table_);
    form->addRow("Windows", count_);

    auto* windowGroup = new QGroupBox("Windows, in id order", this);
    auto* windowColumn = new QVBoxLayout(windowGroup);
    windowColumn->addWidget(windowBox_);
    windowColumn->addStretch();

    note_ = new QLabel(this);
    note_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        // Refused rather than repaired: every problem here is a choice only
        // the author can make (which name to change, which entry does fit).
        if (const QString problem = firstProblem(); !problem.isEmpty()) {
            QMessageBox::warning(this, "Frames", problem);
            return;
        }

        renames_.clear();
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            const QString now = QString::fromStdString(frames_[i].name);
            if (!original_[i].isEmpty() && original_[i] != now) {
                renames_.emplace_back(original_[i], now);
            }
        }
        accept();
    });

    auto* editorColumn = new QVBoxLayout;
    editorColumn->addLayout(form);
    editorColumn->addWidget(windowGroup);
    editorColumn->addWidget(note_);
    editorColumn->addStretch();

    auto* previewGroup = new QGroupBox("The frame, 16 ids across", this);
    auto* previewColumn = new QVBoxLayout(previewGroup);
    previewColumn->addWidget(preview_);
    previewColumn->addStretch();

    auto* columns = new QHBoxLayout;
    columns->addLayout(listColumn);
    columns->addLayout(editorColumn, /*stretch=*/1);
    columns->addWidget(previewGroup);

    auto* rows = new QVBoxLayout(this);
    rows->addLayout(columns);
    rows->addWidget(buttons);

    refreshList();
    if (!frames_.empty()) list_->setCurrentRow(0);
    refreshEditor();
}

int FrameWindow::selected() const {
    const int row = list_->currentRow();
    return (row >= 0 && static_cast<std::size_t>(row) < frames_.size()) ? row : -1;
}

void FrameWindow::refreshList() {
    const QSignalBlocker blocked(list_);
    const int row = list_->currentRow();

    list_->clear();
    for (const chr::Frame& frame : frames_) {
        const QString name = QString::fromStdString(frame.name);
        list_->addItem(QString("%1  (%2, %3 window%4)")
                           .arg(name.isEmpty() ? "(unnamed)" : name,
                                addressLabel(frame.patternTable))
                           .arg(frame.windows.size())
                           .arg(frame.windows.size() == 1 ? "" : "s"));
    }

    if (row >= 0 && static_cast<std::size_t>(row) < frames_.size()) {
        list_->setCurrentRow(row);
    }
}

void FrameWindow::refreshEditor() {
    const int row = selected();
    const bool has = row >= 0;

    name_->setEnabled(has);
    table_->setEnabled(has);
    count_->setEnabled(has);
    windowBox_->setEnabled(has);
    remove_->setEnabled(has);

    if (!has) {
        const QSignalBlocker blockedName(name_);
        name_->clear();
        refreshWindows();
        return;
    }

    const chr::Frame& frame = frames_[static_cast<std::size_t>(row)];

    loading_ = true;
    name_->setText(QString::fromStdString(frame.name));

    const auto* table = std::find(std::begin(kPatternTables), std::end(kPatternTables),
                                  frame.patternTable);
    table_->setCurrentIndex(table == std::end(kPatternTables)
                                ? 0
                                : static_cast<int>(table - std::begin(kPatternTables)));

    const auto count = std::find(windowCounts().begin(), windowCounts().end(),
                                 static_cast<int>(frame.windows.size()));
    count_->setCurrentIndex(count == windowCounts().end()
                                ? 0
                                : static_cast<int>(count - windowCounts().begin()));
    loading_ = false;

    refreshWindows();
}

void FrameWindow::refreshWindows() {
    // Rebuilt rather than reused: the row count is the window count, which is
    // itself one of the things being edited.
    while (QLayoutItem* item = windowRows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const int row = selected();
    if (row < 0) {
        note_->clear();
        preview_->setFrame(nullptr);
        return;
    }

    const chr::Frame& frame = frames_[static_cast<std::size_t>(row)];
    preview_->setFrame(&frame);

    const int windowTiles = frame.windows.empty()
                                ? chr::kFrameTiles
                                : chr::kFrameTiles / static_cast<int>(frame.windows.size());
    const std::uint32_t windowBytes =
        static_cast<std::uint32_t>(windowTiles) * static_cast<std::uint32_t>(chr::kTileBytes);

    for (std::size_t w = 0; w < frame.windows.size(); ++w) {
        const int base = static_cast<int>(w) * windowTiles;
        auto* ids = new QLabel(QString("%1-%2")
                                   .arg(idLabel(base), idLabel(base + windowTiles - 1)),
                               windowBox_);

        auto* pick = new QComboBox(windowBox_);
        pick->addItem(kNoEntry, QString());
        for (const Entry& entry : entries_) {
            pick->addItem(entry.name, entry.name);
        }
        const QString held = QString::fromStdString(frame.windows[w]);
        const int found = held.isEmpty() ? 0 : pick->findData(held);
        if (found < 0) {
            // An entry the manifest no longer has is kept and shown rather
            // than silently dropped to reserved: losing it would be an edit
            // nobody asked for, and the accept check names it.
            pick->addItem(held + "  (not in this manifest)", held);
            pick->setCurrentIndex(pick->count() - 1);
        } else {
            pick->setCurrentIndex(found);
        }

        connect(pick, &QComboBox::currentIndexChanged, this, [this, pick, w](const int index) {
            if (loading_ || index < 0) return;
            const int current = selected();
            if (current < 0) return;
            frames_[static_cast<std::size_t>(current)].windows[w] =
                pick->itemData(index).toString().toStdString();
            refreshWindows();
        });

        auto* fit = new QLabel(windowBox_);
        if (!held.isEmpty()) {
            const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                            [&](const Entry& e) { return e.name == held; });
            if (entry == entries_.end()) {
                fit->setText("missing");
            } else if (entry->size > windowBytes) {
                fit->setText(QString("too big -- %1 of %2 bytes")
                                 .arg(entry->size)
                                 .arg(windowBytes));
            } else if (entry->size < windowBytes) {
                fit->setText(QString("fills %1 of %2 bytes").arg(entry->size).arg(windowBytes));
            } else {
                fit->setText("exact fit");
            }
        }

        windowRows_->addWidget(ids, static_cast<int>(w), 0);
        windowRows_->addWidget(pick, static_cast<int>(w), 1);
        windowRows_->addWidget(fit, static_cast<int>(w), 2);
    }

    const QString problem = firstProblem();
    note_->setText(problem.isEmpty()
                       ? QString("A window is %1 tiles (%2 bytes). Order is id order -- window 0 "
                                 "holds the lowest ids.")
                             .arg(windowTiles)
                             .arg(windowBytes)
                       : QString("Cannot be applied yet: %1").arg(problem));
}

QString FrameWindow::firstProblem() const {
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        const chr::Frame& frame = frames_[i];
        const QString name = QString::fromStdString(frame.name);

        if (name.isEmpty()) return QString("frame %1 has no name.").arg(i);
        for (std::size_t j = 0; j < i; ++j) {
            if (frames_[j].name == frame.name) {
                return QString("two frames are both named '%1'. A sprite names the frame it "
                               "draws through, so the names have to be distinct.")
                    .arg(name);
            }
        }
        if (frame.windows.empty()) {
            return QString("frame '%1' has no windows.").arg(name);
        }

        const auto windowBytes = static_cast<std::uint32_t>(
            (chr::kFrameTiles / static_cast<int>(frame.windows.size())) * chr::kTileBytes);

        for (std::size_t w = 0; w < frame.windows.size(); ++w) {
            if (frame.windows[w].empty()) continue;
            const QString held = QString::fromStdString(frame.windows[w]);

            for (std::size_t v = 0; v < w; ++v) {
                if (frames_[i].windows[v] == frame.windows[w]) {
                    return QString("frame '%1' holds '%2' in two windows, so its tiles would "
                                   "have two ids in one frame.")
                        .arg(name, held);
                }
            }

            const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                            [&](const Entry& e) { return e.name == held; });
            if (entry == entries_.end()) {
                return QString("frame '%1' window %2 names '%3', which this manifest does not "
                               "have.")
                    .arg(name)
                    .arg(w)
                    .arg(held);
            }
            if (entry->size > windowBytes) {
                return QString("'%1' is %2 bytes, which does not fit the %3-byte windows of "
                               "frame '%4'.")
                    .arg(held)
                    .arg(entry->size)
                    .arg(windowBytes)
                    .arg(name);
            }
        }
    }
    return {};
}

void FrameWindow::addFrame() {
    chr::Frame frame;
    // Two windows rather than one: the arrangement a frame usually describes
    // is a pattern table split between the art and something else, and a
    // single 256-tile window is the case that needs saying out loud.
    frame.name = "frame";
    frame.windows.resize(2);

    // Names have to be distinct, so a second "frame" is numbered.
    int suffix = 2;
    while (std::any_of(frames_.begin(), frames_.end(),
                       [&](const chr::Frame& other) { return other.name == frame.name; })) {
        frame.name = "frame" + std::to_string(suffix++);
    }

    frames_.push_back(std::move(frame));
    original_.emplace_back();  // added here, so there is no old name to follow

    refreshList();
    list_->setCurrentRow(static_cast<int>(frames_.size()) - 1);
    refreshEditor();
}

void FrameWindow::removeFrame() {
    const int row = selected();
    if (row < 0) return;

    frames_.erase(frames_.begin() + row);
    original_.erase(original_.begin() + row);

    refreshList();
    if (!frames_.empty()) {
        list_->setCurrentRow(std::min(row, static_cast<int>(frames_.size()) - 1));
    }
    refreshEditor();
}
