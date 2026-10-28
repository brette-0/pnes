#include "metasprite_page.hpp"

#include <QBoxLayout>
#include <QMessageBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

#include <algorithm>

MetaspritePage::MetaspritePage(int zoom, PaletteStore* palettes, QWidget* parent)
    : QWidget(parent), zoom_(zoom), palettes_(palettes) {
    auto* attributes = new QGroupBox("Metasprite", this);
    auto* form = new QFormLayout(attributes);

    name_ = new QLineEdit(attributes);
    // The name is the .msprite filename and the tab label, so it is an
    // attribute of the metasprite rather than something the window owns.
    connect(name_, &QLineEdit::textChanged, this, &MetaspritePage::nameChanged);
    connect(name_, &QLineEdit::textChanged, this, &MetaspritePage::dirtied);

    width_ = new QSpinBox(attributes);
    width_->setRange(kMinFieldTiles, kMaxFieldTiles);
    width_->setValue(tilesX_);
    width_->setSuffix(" tiles");

    height_ = new QSpinBox(attributes);
    height_->setRange(kMinFieldTiles, kMaxFieldTiles);
    height_->setValue(tilesY_);
    height_->setSuffix(" tiles");

    derived_ = new QLabel(attributes);

    // Palette is per metasprite, not per frame: an animation recolouring
    // halfway through is not something the hardware does for free, and it is
    // not what a frame is for.
    palette_ = new QComboBox(attributes);
    connect(palette_, &QComboBox::currentIndexChanged, this, [this] {
        applyPalette();
        emit dirtied();
    });

    form->addRow("&Name", name_);
    form->addRow("&Width", width_);
    form->addRow("&Height", height_);
    form->addRow("Pixels", derived_);
    form->addRow("&Palette", palette_);

    connect(width_, &QSpinBox::valueChanged, this, &MetaspritePage::applySize);
    connect(height_, &QSpinBox::valueChanged, this, &MetaspritePage::applySize);

    frames_ = new QTabWidget(this);
    frames_->setTabsClosable(true);
    frames_->setDocumentMode(true);
    frames_->setTabPosition(QTabWidget::South);  // distinguishes them from the metasprite tabs above
    connect(frames_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeFrame(index); });

    // Adding a frame needs to be visible, not just a menu item: the "+" sits
    // on the frame tab bar itself, where the tabs it adds to are.
    auto* addFrame = new QToolButton(frames_);
    addFrame->setText("+");
    addFrame->setToolTip("New frame (Ctrl+T)");
    addFrame->setAutoRaise(true);
    connect(addFrame, &QToolButton::clicked, this, &MetaspritePage::newFrame);
    frames_->setCornerWidget(addFrame, Qt::TopRightCorner);
    connect(frames_, &QTabWidget::currentChanged, this, [this](int) {
        if (MetaspriteFrame* frame = currentFrame()) {
            emit statusChanged(QString("%1  timer %2  %3")
                                   .arg(frame->frameName())
                                   .arg(frame->timerFrames())
                                   .arg(summary()));
        }
    });

    auto* rows = new QVBoxLayout(this);
    rows->addWidget(attributes);
    rows->addWidget(frames_, 1);

    if (palettes_) {
        connect(palettes_, &PaletteStore::changed, this, &MetaspritePage::refreshPalettes);
    }
    refreshPalettes();

    // An animation with no frames is not useful to look at, so one is made up
    // front -- the same reasoning as the window opening with one metasprite.
    newFrame();
    refreshDerived();
}

MetaspriteFrame* MetaspritePage::currentFrame() const {
    return qobject_cast<MetaspriteFrame*>(frames_->currentWidget());
}

int MetaspritePage::frameCount() const {
    return static_cast<int>(frameList_.size());
}

bool MetaspritePage::hasContent() const {
    for (const MetaspriteFrame* frame : frameList_) {
        if (frame->field()->spriteCount() > 0) return true;
    }
    return false;
}

QString MetaspritePage::name() const {
    return name_->text();
}

void MetaspritePage::setName(const QString& name) {
    name_->setText(name);  // textChanged carries it on to the tab label
}

QString MetaspritePage::paletteName() const {
    // Index 0 is the placeholder entry, which is no palette at all.
    return palette_->currentIndex() <= 0 ? QString() : palette_->currentText();
}

void MetaspritePage::setPaletteName(const QString& name) {
    const int index = name.isEmpty() ? 0 : palette_->findText(name);
    palette_->setCurrentIndex(index >= 0 ? index : 0);
}

void MetaspritePage::refreshPalettes() {
    const QString wanted = paletteName();

    {
        const QSignalBlocker blocked(palette_);
        palette_->clear();
        palette_->addItem("(placeholder)");
        if (palettes_) palette_->addItems(palettes_->names());

        const int index = wanted.isEmpty() ? 0 : palette_->findText(wanted);
        // A palette that has been deleted leaves the metasprite on the
        // placeholder rather than on whichever name took its place.
        palette_->setCurrentIndex(index >= 0 ? index : 0);
    }

    // The colours themselves may have changed even when the list has not.
    applyPalette();
}

void MetaspritePage::applyPalette() {
    const pal::Palette* palette =
        palettes_ && !paletteName().isEmpty() ? palettes_->find(paletteName()) : nullptr;
    const QVector<QRgb> table = paletteTable(palette);
    for (MetaspriteFrame* frame : frameList_) {
        frame->setPaletteTable(table);
    }
}

void MetaspritePage::setSize(const int tilesX, const int tilesY) {
    // The spin boxes are the source of truth, so go through them -- their
    // valueChanged does the rest. Blocked individually so a two-axis resize
    // only reaches the fields once.
    {
        const QSignalBlocker blockWidth(width_);
        width_->setValue(tilesX);
    }
    height_->setValue(tilesY);
    // A height that was already correct emits nothing, so the width change
    // still has to be applied.
    applySize();
}

MetaspriteFrame* MetaspritePage::appendFrame() {
    newFrame();
    return frameList_.empty() ? nullptr : frameList_.back();
}

void MetaspritePage::clearFrames() {
    // Used by a load to empty a page it just made, so it goes round closeFrame
    // -- there is nothing to confirm, and a prompt here would appear while a
    // project is opening.
    while (frames_->count() > 0) {
        auto* frame = qobject_cast<MetaspriteFrame*>(frames_->widget(0));
        frameList_.erase(std::remove(frameList_.begin(), frameList_.end(), frame), frameList_.end());
        frames_->removeTab(0);
        if (frame) frame->deleteLater();
    }
    emit framesChanged();
}

void MetaspritePage::newFrame() {
    auto* frame = new MetaspriteFrame(frameIdCounter_++, zoom_,
                                      QString("frame %1").arg(++frameNumber_), frames_);
    adopt(frame);

    const int index = frames_->addTab(frame, frame->frameName());
    frames_->setCurrentIndex(index);

    emit framesChanged();
    emit dirtied();
}

void MetaspritePage::adopt(MetaspriteFrame* frame) {
    frame->setTileMode(mode_);
    frame->setPaletteTable(paletteTable(
        palettes_ && !paletteName().isEmpty() ? palettes_->find(paletteName()) : nullptr));
    frame->field()->setFieldSize(tilesX_, tilesY_);
    frame->field()->resize(frame->field()->sizeHint());
    frameList_.push_back(frame);

    connect(frame, &MetaspriteFrame::statusChanged, this, [this, frame](const QString& text) {
        // Only the visible frame talks to the status bar; a background frame
        // emitting on its way out would otherwise overwrite the message.
        if (frames_->currentWidget() == frame) emit statusChanged(text);
    });
    connect(frame, &MetaspriteFrame::nameChanged, this, [this, frame] {
        retitle(frame);
        emit framesChanged();
        emit dirtied();
    });
    connect(frame, &MetaspriteFrame::spritesChanged, this, [this] {
        emit framesChanged();
        emit dirtied();
    });
    connect(frame, &MetaspriteFrame::attributesChanged, this, [this, frame] {
        emit dirtied();
        if (frames_->currentWidget() == frame) {
            emit statusChanged(frame->timerFrames() == kStopTimer
                                   ? QString("%1  timer 0 -- the animation stops on this frame")
                                         .arg(frame->frameName())
                                   : QString("%1  timer %2 NES frames")
                                         .arg(frame->frameName())
                                         .arg(frame->timerFrames()));
        }
    });
}

void MetaspritePage::closeFrame(int index) {
    if (index < 0) index = frames_->currentIndex();
    auto* frame = qobject_cast<MetaspriteFrame*>(frames_->widget(index));
    if (!frame) return;

    // Closing a frame discards its sprites, and nothing undoes it -- so a frame
    // with work in it asks first. An empty one closes without the interruption.
    if (frame->field()->spriteCount() > 0) {
        const auto answer = QMessageBox::question(
            this, "Close frame",
            QString("Close '%1'? Its %2 sprite%3 will be discarded.")
                .arg(frame->frameName())
                .arg(frame->field()->spriteCount())
                .arg(frame->field()->spriteCount() == 1 ? "" : "s"),
            QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Close) return;
    }

    frameList_.erase(std::remove(frameList_.begin(), frameList_.end(), frame), frameList_.end());
    frames_->removeTab(index);
    frame->deleteLater();
    emit framesChanged();
    emit dirtied();
}

QString MetaspritePage::summary() const {
    return QString("%1x%2 tiles, %3 frame%4")
        .arg(tilesX_)
        .arg(tilesY_)
        .arg(frameCount())
        .arg(frameCount() == 1 ? "" : "s");
}

void MetaspritePage::setTileMode(const chr::TileMode mode) {
    mode_ = mode;
    for (MetaspriteFrame* frame : frameList_) {
        frame->setTileMode(mode);
    }
    refreshDerived();
}

void MetaspritePage::setBrush(const chr::Cell* cell, const QString& source) {
    for (MetaspriteFrame* frame : frameList_) {
        frame->setBrush(cell, source);
    }
}

void MetaspritePage::applySize() {
    tilesX_ = width_->value();
    tilesY_ = height_->value();

    // Frames of one animation are the same size by definition, so the extent
    // goes to all of them -- each drops whatever no longer fits and says so.
    for (MetaspriteFrame* frame : frameList_) {
        frame->field()->setFieldSize(tilesX_, tilesY_);
        frame->field()->resize(frame->field()->sizeHint());
    }

    refreshDerived();
    emit framesChanged();
    emit dirtied();
}

void MetaspritePage::refreshDerived() {
    // What the tile extent works out to on screen, and what one slot is -- the
    // only place the 8x8/8x16 distinction shows up in a metasprite's own terms.
    const int slotPx = mode_ == chr::TileMode::Size8x16 ? chr::kTilePixels * 2 : chr::kTilePixels;
    derived_->setText(QString("%1 x %2  (one tile is 8x%3)")
                          .arg(tilesX_ * chr::kTilePixels)
                          .arg(tilesY_ * slotPx)
                          .arg(slotPx));
}

void MetaspritePage::retitle(MetaspriteFrame* frame) {
    const int index = frames_->indexOf(frame);
    if (index < 0) return;
    // An all-whitespace name would leave an unclickable tab, so the tab falls
    // back to the frame's id while the field is empty.
    const QString name = frame->frameName().trimmed();
    frames_->setTabText(index, name.isEmpty() ? QString("frame %1").arg(frame->frameId()) : name);
}

