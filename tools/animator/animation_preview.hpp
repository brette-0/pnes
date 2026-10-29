/**
 * @file animation_preview.hpp
 * @brief A small canvas that plays the metasprite being worked on.
 */
#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <vector>

class QTimer;

/// NTSC frame period, in milliseconds. Timers are in NES frames, so this is
/// what one of them is worth -- 60.0988Hz, not a round 60.
constexpr double kNesFrameMs = 1000.0 / 60.0988;

/// One already-composed frame of an animation.
struct PreviewCel {
    QImage  image;     ///< The whole metasprite, as the PPU would show it.
    int     hold = 0;  ///< NES frames to hold; 0 stops the animation here.
    QString name;
};

/**
 * @brief Plays a list of composed frames.
 *
 * The playback rules are the ones the format records and nothing else
 * implements (see metasprite_frame.hpp): frames run in order, the last loops
 * back to the first, and a hold of 0 stops on that frame. This is the only
 * place in the tool those rules are executed, so it is also the only way to see
 * whether an animation reads right.
 *
 * Cels are composed by the caller and handed over whole, so the preview does
 * not need to know about fields, palettes or tile modes -- it re-renders
 * nothing and simply blits.
 */
class AnimationPreview final : public QWidget {
    Q_OBJECT

public:
    explicit AnimationPreview(int zoom, QWidget* parent = nullptr);

    /// Replaces the animation. Playback keeps its position where it still
    /// exists, so editing a frame does not restart the animation under the
    /// user's hands.
    void setCels(std::vector<PreviewCel> cels);

    void setPlaying(bool playing);
    [[nodiscard]] bool isPlaying() const;

    /// Restarts from the first frame.
    void restart();

    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// Playback started or stopped, including stopping itself on a hold of 0.
    void playingChanged(bool playing);

    /// The displayed frame changed: its name, and its 1-based position.
    void frameChanged(const QString& name, int position, int count);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// Advances by one NES frame.
    void tick();

    /// Emits frameChanged() for the current cel.
    void announce();

    int                     box_;        ///< Widget side, in device pixels.
    QTimer*                 clock_ = nullptr;
    std::vector<PreviewCel> cels_;
    std::size_t             current_ = 0;
    int                     remaining_ = 0;  ///< NES frames left on the current cel.
};
