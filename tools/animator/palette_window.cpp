#include "palette_window.hpp"

#include "palette.hpp"

#include <QBoxLayout>
#include <QColor>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>

namespace {

/// Master-palette layout, as every NES tool draws it: 16 columns of 4 rows,
/// so a column is one hue and a row one brightness.
constexpr int kPickerColumns = 16;

/// Swatch size, big enough to judge a colour and to hit with a pointer.
constexpr int kSwatchPx = 24;

QString swatchStyle(const std::uint32_t rgb) {
    return QString("background-color: #%1; border: 1px solid #202020;")
        .arg(rgb, 6, 16, QChar('0'));
}

/// Hex label for a NES colour index, as the hardware numbers it.
QString colourLabel(const std::uint8_t index) {
    return QString("$%1").arg(index, 2, 16, QChar('0')).toUpper();
}

/**
 * @brief Modal picker over the NES master palette.
 *
 * The console's 64 colours are the whole choice there is, so this is a grid of
 * them rather than a general colour dialog -- an arbitrary RGB value is not
 * something a palette could hold.
 */
class NesColourDialog final : public QDialog {
public:
    explicit NesColourDialog(const std::uint8_t current, QWidget* parent)
        : QDialog(parent) {
        setWindowTitle("Pick a NES colour");

        auto* grid = new QGridLayout;
        grid->setSpacing(2);
        for (int index = 0; index < pal::kNesColours; ++index) {
            auto* swatch = new QToolButton(this);
            swatch->setFixedSize(kSwatchPx, kSwatchPx);
            swatch->setStyleSheet(swatchStyle(pal::rgbOf(static_cast<std::uint8_t>(index))));
            swatch->setToolTip(colourLabel(static_cast<std::uint8_t>(index)));
            swatch->setCheckable(true);
            swatch->setChecked(index == current);
            connect(swatch, &QToolButton::clicked, this, [this, index] {
                picked_ = static_cast<std::uint8_t>(index);
                accept();
            });
            grid->addWidget(swatch, index / kPickerColumns, index % kPickerColumns);
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto* rows = new QVBoxLayout(this);
        rows->addLayout(grid);
        rows->addWidget(buttons);
    }

    [[nodiscard]] std::uint8_t picked() const { return picked_; }

private:
    std::uint8_t picked_ = pal::kTransparencyColour;
};

} // namespace

PaletteWindow::PaletteWindow(PaletteStore* store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle("Palettes");
    // Non-modal: the point of a separate window is being able to recolour while
    // looking at the tiles it recolours.
    setModal(false);

    list_ = new QListWidget(this);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { refreshEditor(); });

    auto* add = new QPushButton("New", this);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString name = store_->add("palette");
        refreshList();
        // Select what was just made, which is what the user is about to edit.
        for (int row = 0; row < list_->count(); ++row) {
            if (list_->item(row)->text() == name) list_->setCurrentRow(row);
        }
    });

    remove_ = new QPushButton("Remove", this);
    connect(remove_, &QPushButton::clicked, this, [this] {
        const QString name = selectedName();
        if (name.isEmpty()) return;
        // Metasprites refer to palettes by name, so one still using this falls
        // back to the placeholder view rather than to another palette.
        store_->remove(name);
        refreshList();
    });

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove_);

    auto* editor = new QGroupBox("Palette", this);
    auto* form = new QFormLayout(editor);

    name_ = new QLineEdit(editor);
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const QString from = selectedName();
        if (from.isEmpty() || name_->text().trimmed() == from) return;
        if (!store_->rename(from, name_->text())) {
            // Taken or empty: the field goes back rather than leaving a name on
            // screen that nothing is called.
            name_->setText(from);
            return;
        }
        refreshList();
    });
    form->addRow("&Name", name_);

    for (int slot = 0; slot < pal::kColoursPerPalette; ++slot) {
        swatches_[slot] = new QToolButton(editor);
        swatches_[slot]->setFixedSize(kSwatchPx * 3, kSwatchPx);
        if (slot == 0) {
            // Shown for completeness -- a palette is four entries to the PPU --
            // but index 0 is the backdrop a sprite shows through, so there is
            // nothing to choose.
            swatches_[slot]->setEnabled(false);
            swatches_[slot]->setToolTip("Transparency -- always NES black");
            form->addRow("Transparency", swatches_[slot]);
        } else {
            connect(swatches_[slot], &QToolButton::clicked, this, [this, slot] { pickColour(slot); });
            form->addRow(QString("Colour &%1").arg(slot), swatches_[slot]);
        }
    }

    auto* rows = new QVBoxLayout(this);
    rows->addWidget(new QLabel("Palettes are named and referred to by name, by each metasprite and "
                               "by the tile sheet's own view.", this));
    rows->addWidget(list_, 1);
    rows->addLayout(buttons);
    rows->addWidget(editor);

    // The store can also change from elsewhere (a project being opened), so the
    // window follows it rather than only its own edits.
    connect(store_, &PaletteStore::changed, this, [this] {
        if (list_->count() != static_cast<int>(store_->palettes().size())) refreshList();
        refreshEditor();
    });

    refreshList();
}

QString PaletteWindow::selectedName() const {
    const QListWidgetItem* item = list_->currentItem();
    return item ? item->text() : QString();
}

void PaletteWindow::refreshList() {
    const QString selected = selectedName();

    const QSignalBlocker blocked(list_);
    list_->clear();
    list_->addItems(store_->names());

    int row = 0;
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->text() == selected) row = i;
    }
    if (list_->count() > 0) list_->setCurrentRow(row);

    refreshEditor();
}

void PaletteWindow::refreshEditor() {
    const pal::Palette* palette = store_->find(selectedName());

    name_->setEnabled(palette != nullptr);
    remove_->setEnabled(palette != nullptr);
    for (int slot = 1; slot < pal::kColoursPerPalette; ++slot) {
        swatches_[slot]->setEnabled(palette != nullptr);
    }

    if (!palette) {
        const QSignalBlocker blocked(name_);
        name_->clear();
        for (auto* swatch : swatches_) {
            swatch->setStyleSheet(QString());
            swatch->setText(QString());
        }
        return;
    }

    {
        const QSignalBlocker blocked(name_);
        name_->setText(QString::fromStdString(palette->name));
    }

    const auto rgb = pal::rgbOf(*palette);
    for (int slot = 0; slot < pal::kColoursPerPalette; ++slot) {
        swatches_[slot]->setStyleSheet(swatchStyle(rgb[static_cast<std::size_t>(slot)]));
        swatches_[slot]->setText(colourLabel(slot == 0 ? pal::kTransparencyColour
                                                       : palette->colours[static_cast<std::size_t>(slot - 1)]));
    }
}

void PaletteWindow::pickColour(const int slot) {
    const pal::Palette* palette = store_->find(selectedName());
    if (!palette) return;

    NesColourDialog dialog(palette->colours[static_cast<std::size_t>(slot - 1)], this);
    if (dialog.exec() == QDialog::Accepted) {
        store_->setColour(selectedName(), slot, dialog.picked());
        refreshEditor();
    }
}
