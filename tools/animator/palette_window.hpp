/**
 * @file palette_window.hpp
 * @brief The palette editor: a separate window listing the project's palettes.
 */
#pragma once

#include "palette_store.hpp"

#include <QDialog>

class QListWidget;
class QLineEdit;
class QPushButton;
class QToolButton;

/**
 * @brief Creates and edits named palettes.
 *
 * A window of its own rather than a panel: palettes are edited now and then,
 * while the sheet and the fields are in constant use, and both of those want
 * the width. Non-modal, so it can sit open beside the main window while tiles
 * are being placed.
 *
 * Index 0 of every palette is the transparency slot. It is shown, so a palette
 * reads as the four entries the PPU takes, but it is not editable -- a sprite's
 * index 0 shows whatever is behind it.
 */
class PaletteWindow final : public QDialog {
    Q_OBJECT

public:
    PaletteWindow(PaletteStore* store, QWidget* parent = nullptr);

private:
    /// Rebuilds the list from the store, keeping the selection by name.
    void refreshList();

    /// Updates the name field and swatches for the selected palette.
    void refreshEditor();

    /// The palette selected in the list, or an empty string.
    [[nodiscard]] QString selectedName() const;

    /// Opens the NES colour picker for colour @p slot (1-3).
    void pickColour(int slot);

    PaletteStore* store_;
    QListWidget*  list_ = nullptr;
    QLineEdit*    name_ = nullptr;
    QToolButton*  swatches_[pal::kColoursPerPalette] = {};
    QPushButton*  remove_ = nullptr;
};
