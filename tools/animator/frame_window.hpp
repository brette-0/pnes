/**
 * @file frame_window.hpp
 * @brief The frame editor: the dialog that authors a project's chr::Frame list.
 */
#pragma once

#include "chr_table.hpp"

#include <QDialog>
#include <QString>

#include <cstdint>
#include <utility>
#include <vector>

class FramePreview;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QWidget;

/**
 * @brief Creates and edits the frames a project's sprites draw from.
 *
 * MODAL, unlike the palette editor, and for a reason worth stating: changing a
 * frame renumbers the tile sheet. A palette only recolours what is already
 * there, so its window can sit open while tiles are stamped; a window order or
 * a pattern table moving means the id under the brush is a different id. So
 * this is edited to a finish and applied at once -- the caller reloads the
 * sheet from what ::frames returns.
 *
 * Edits are held in the dialog's own copy. Nothing reaches the project unless
 * the dialog is accepted, and accepting is refused while anything is invalid
 * (chr::loadManifest would reject the same things, but failing there would
 * leave an editor the user cannot see with the project already changed).
 */
class FrameWindow final : public QDialog {
    Q_OBJECT

public:
    /// One entry the manifest offers, for the per-window dropdowns. The size
    /// is carried so a window can say whether what it names actually fits.
    struct Entry {
        QString       name;
        std::uint32_t size = 0;  ///< Bytes the entry reserves.
    };

    /**
     * @param frames  The project's frames as they stand, edited in a copy.
     * @param entries The manifest's entries, in the order the sheet shows them.
     * @param tiles   A table of the SAME manifest loaded with no frames and in
     *                8x8 mode, which is what the preview draws from. Frameless
     *                because a frame being set up has to be able to show art
     *                that no frame reaches yet; 8x8 because a frame is 256
     *                tile ids and a preview of it is a pattern table, whatever
     *                height the project's sprites are. Not retained.
     */
    FrameWindow(std::vector<chr::Frame> frames, std::vector<Entry> entries,
                const chr::Table& tiles, QWidget* parent = nullptr);

    /// The edited frames. Only meaningful once the dialog was accepted.
    [[nodiscard]] const std::vector<chr::Frame>& frames() const { return frames_; }

    /**
     * @brief Frames renamed during this edit, as old name -> new name.
     *
     * A sprite records the frame it draws through by NAME (proj::Sprite::frame),
     * so a rename the placed sprites do not follow would leave every one of
     * them naming a frame that no longer exists -- which fails the export. The
     * caller applies these to its pages; see MetaspriteField::renameFrame.
     */
    [[nodiscard]] const std::vector<std::pair<QString, QString>>& renames() const {
        return renames_;
    }

private:
    /// Window counts offered. Every one divides chr::kFrameTiles, which is the
    /// whole requirement -- a count that did not would leave one window short.
    static const std::vector<int>& windowCounts();

    /// Rebuilds the frame list, keeping the selection by index.
    void refreshList();

    /// Loads the selected frame into the name/table/window controls.
    void refreshEditor();

    /// Rebuilds the per-window rows for the selected frame.
    void refreshWindows();

    /// Index of the selected frame, or -1.
    [[nodiscard]] int selected() const;

    /// What is wrong with the frames as they stand, or an empty string.
    [[nodiscard]] QString firstProblem() const;

    void addFrame();
    void removeFrame();

    std::vector<chr::Frame> frames_;
    std::vector<Entry>      entries_;

    /// Name each frame was loaded with, parallel to ::frames_; empty for one
    /// added in this dialog. What ::renames_ is derived from on accept.
    std::vector<QString>    original_;

    std::vector<std::pair<QString, QString>> renames_;

    QListWidget*  list_ = nullptr;
    QLineEdit*    name_ = nullptr;
    QComboBox*    table_ = nullptr;
    QComboBox*    count_ = nullptr;
    QWidget*      windowBox_ = nullptr;
    QGridLayout*  windowRows_ = nullptr;
    QLabel*       note_ = nullptr;
    QPushButton*  remove_ = nullptr;
    FramePreview* preview_ = nullptr;

    /// Set while a refresh is driving the controls, so their own change
    /// handlers do not write the frame back out from under the load.
    bool loading_ = false;
};
