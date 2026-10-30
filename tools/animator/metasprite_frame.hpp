/**
 * @file metasprite_frame.hpp
 * @brief One animation frame of a metasprite: its name, timer, successor and
 *        the field holding its sprites.
 */
#pragma once

#include "chr_table.hpp"
#include "metasprite_field.hpp"
#include "tile_image.hpp"

#include <QWidget>
#include <QString>

class QRegularExpressionValidator;

class QCheckBox;
class QLineEdit;
class QScrollArea;
class QSpinBox;

/// Timer range, in NES frames -- a byte of ticks at the console's refresh,
/// which is what a 6502 animation driver counts down.
///
/// PLAYBACK CONVENTION, recorded here because the editor itself does not act on
/// it (there is no preview to act with): frames play in tab order, contiguously,
/// and the last frame loops back to the first. A timer of ::kStopTimer (0) ends
/// the animation on that frame instead of advancing -- which is why 0 is a legal
/// value and why there is no successor field to set. A one-frame animation whose
/// timer is 0 is therefore a still pose.
constexpr int kMinTimerFrames     = 0;
constexpr int kMaxTimerFrames     = 255;
constexpr int kDefaultTimerFrames = 8;

/// A timer of 0 stops the animation on that frame rather than advancing.
constexpr int kStopTimer = 0;

/**
 * @brief An animation frame.
 *
 * Every frame of a metasprite carries its own sprites -- that is what makes it
 * a frame -- plus a name to refer to it by and how long it holds. There is no
 * successor to choose: frames are contiguous, so the one after this is the next
 * tab, and a timer of ::kStopTimer ends the animation here (see the note on
 * ::kMinTimerFrames).
 *
 * The metasprite's extent lives a level up, in MetaspritePage: it is a property
 * of the animation, not of one frame.
 */
/// The name as the editor holds it: only a C++ identifier is typeable in either
/// name field, so a name arriving from an older project file is folded the same
/// way the validator would have forced it to be authored -- and the same way
/// codegen's identifier() folds it on the way to the header.
[[nodiscard]] QString identifierSafe(const QString& name);

/// A validator admitting a C++ identifier and nothing else, parented to
/// @p parent. Both name fields reach the generated header as part of a member
/// name, so both wear it.
[[nodiscard]] QRegularExpressionValidator* identifierValidator(QObject* parent);

class MetaspriteFrame final : public QWidget {
    Q_OBJECT

public:
    /// @param id   Stable identity, unique within the metasprite.
    /// @param zoom The sheet's zoom; the field scales itself from it.
    /// @param name Initial name, also the initial tab label.
    MetaspriteFrame(int id, int zoom, const QString& name, QWidget* parent = nullptr);

    [[nodiscard]] int frameId() const { return id_; }
    [[nodiscard]] QString frameName() const;
    [[nodiscard]] int timerFrames() const;

    /// Whether the export names this frame's bytes publicly -- see
    /// ::setPublicFrame.
    [[nodiscard]] bool isPublicFrame() const;

    [[nodiscard]] MetaspriteField* field() const { return field_; }

    /// Renames the frame, as if typed.
    void setFrameName(const QString& name);

    /// Sets the hold, clamped to the timer's range.
    void setTimerFrames(int frames);

    /// Marks the frame public: the generated header, whose frame table is
    /// otherwise private, names this frame's bytes as well. Nothing in the
    /// editor behaves differently -- it is an attribute of the export, for the
    /// animations a consumer reads a frame of directly instead of playing.
    void setPublicFrame(bool isPublic);

    /// Forwarded to the field.
    void setTileMode(chr::TileMode mode);
    void setPaletteTable(const QVector<QRgb>& table);
    void setBrush(const chr::Cell* cell, const QString& source);

signals:
    /// The name was edited; the page retitles the tab from it.
    void nameChanged(const QString& name);

    /// The timer or the public flag changed.
    void attributesChanged();

    /// A line for the window's status bar.
    void statusChanged(const QString& text);

    /// The frame's sprites changed.
    void spritesChanged();

private:
    int              id_;
    QLineEdit*       name_ = nullptr;
    QSpinBox*        timer_ = nullptr;
    QCheckBox*       public_ = nullptr;
    QScrollArea*     scroll_ = nullptr;
    MetaspriteField* field_ = nullptr;
};
