#include "metasprite_frame.hpp"

#include <QBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QAbstractSpinBox>
#include <QSpinBox>
#include <QVariant>

MetaspriteFrame::MetaspriteFrame(const int id, const int zoom, const QString& name, QWidget* parent)
    : QWidget(parent), id_(id) {
    field_ = new MetaspriteField(zoom, nullptr);
    connect(field_, &MetaspriteField::statusChanged, this, &MetaspriteFrame::statusChanged);
    connect(field_, &MetaspriteField::spritesChanged, this, &MetaspriteFrame::spritesChanged);

    auto* attributes = new QGroupBox("Frame", this);
    auto* form = new QFormLayout(attributes);

    name_ = new QLineEdit(name, attributes);
    // The tab label follows the field, so renaming is renaming -- there is no
    // second place to keep in step.
    connect(name_, &QLineEdit::textChanged, this, &MetaspriteFrame::nameChanged);

    timer_ = new QSpinBox(attributes);
    timer_->setRange(kMinTimerFrames, kMaxTimerFrames);
    timer_->setValue(kDefaultTimerFrames);
    // A plain integer entry: no suffix text in the box, and no spin buttons
    // either -- the value is typed. The range still clamps what can be
    // committed, which is why this is a spin box and not a QLineEdit with a
    // validator bolted on.
    timer_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    timer_->setAlignment(Qt::AlignRight);
    connect(timer_, &QSpinBox::valueChanged, this, [this] { emit attributesChanged(); });

    form->addRow("&Name", name_);
    // The unit lives in the label, not in the box -- and so does what 0 means,
    // since that is the only way the editor says it at all.
    form->addRow("&Timer (NES frames, 0 stops)", timer_);

    scroll_ = new QScrollArea(this);
    scroll_->setWidget(field_);
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignCenter);
    scroll_->setFrameShape(QFrame::NoFrame);
    field_->resize(field_->sizeHint());

    auto* rows = new QVBoxLayout(this);
    rows->addWidget(attributes);
    rows->addWidget(scroll_, 1);
}

QString MetaspriteFrame::frameName() const {
    return name_->text();
}

int MetaspriteFrame::timerFrames() const {
    return timer_->value();
}

void MetaspriteFrame::setFrameName(const QString& name) {
    name_->setText(name);  // textChanged carries it on to the tab label
}

void MetaspriteFrame::setTimerFrames(const int frames) {
    timer_->setValue(frames);  // QSpinBox clamps to its own range
}

void MetaspriteFrame::setTileMode(const chr::TileMode mode) {
    field_->setTileMode(mode);
    field_->resize(field_->sizeHint());
}

void MetaspriteFrame::setPaletteTable(const QVector<QRgb>& table) {
    field_->setPaletteTable(table);
}

void MetaspriteFrame::setBrush(const chr::Cell* cell, const QString& source) {
    field_->setBrush(cell, source);
}
