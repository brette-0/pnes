#include "metasprite_frame.hpp"

#include <QBoxLayout>
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QAbstractSpinBox>
#include <QSpinBox>
#include <QVariant>

QString identifierSafe(const QString& name) {
    // The same fold codegen's identifier() applies, so what the editor shows is
    // what the header will be named after: anything that cannot sit in a C++
    // identifier becomes an underscore, and a leading digit gains one.
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                     || (ch >= '0' && ch <= '9') || ch == '_';
        out += ok ? ch : QChar('_');
    }
    if (!out.isEmpty() && out.front() >= '0' && out.front() <= '9') out.prepend('_');
    return out;
}

QRegularExpressionValidator* identifierValidator(QObject* parent) {
    // A C++ identifier and nothing else. The empty field matches this only
    // partially, so QLineEdit calls it intermediate and still lets the name be
    // cleared; a leading digit is rejected outright, as is any character --
    // '#', '.', '-', brackets, braces, parentheses, a space -- that could not
    // appear in the member name the export builds from it.
    return new QRegularExpressionValidator(QRegularExpression("[A-Za-z_][A-Za-z0-9_]*"), parent);
}

MetaspriteFrame::MetaspriteFrame(const int id, const int zoom, const QString& name, QWidget* parent)
    : QWidget(parent), id_(id) {
    field_ = new MetaspriteField(zoom, nullptr);
    connect(field_, &MetaspriteField::statusChanged, this, &MetaspriteFrame::statusChanged);
    connect(field_, &MetaspriteField::spritesChanged, this, &MetaspriteFrame::spritesChanged);

    auto* attributes = new QGroupBox("Frame", this);
    auto* form = new QFormLayout(attributes);

    name_ = new QLineEdit(attributes);
    // The name reaches the generated header as part of a member name, so the
    // field only accepts what a C++ identifier accepts: the validator stops a
    // space or a '.' from being typed or pasted in the first place, rather than
    // letting codegen fold it into an underscore behind the author's back.
    name_->setValidator(identifierValidator(name_));
    name_->setText(identifierSafe(name));
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

    // An export attribute, not a playback one: the editor and the preview treat
    // a public frame exactly like any other. The checkbox says what it does to
    // the header rather than carrying the bare word "public", which on its own
    // would read as something about the frame itself.
    public_ = new QCheckBox("Name this frame's bytes in the generated header", attributes);
    public_->setToolTip("The generated header's frame table is private. A public frame also\n"
                        "gets a kTiles_<name> of its own -- for animations a consumer reads a\n"
                        "frame of directly instead of playing, where there is no timer to count.");
    connect(public_, &QCheckBox::toggled, this, [this] { emit attributesChanged(); });

    form->addRow("&Name", name_);
    // The unit lives in the label, not in the box -- and so does what 0 means,
    // since that is the only way the editor says it at all.
    form->addRow("&Timer (NES frames, 0 stops)", timer_);
    form->addRow("P&ublic", public_);

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

bool MetaspriteFrame::isPublicFrame() const {
    return public_->isChecked();
}

void MetaspriteFrame::setFrameName(const QString& name) {
    // setText bypasses the validator, so a load is folded here instead: the
    // name the field ends up holding is the name that gets saved back.
    name_->setText(identifierSafe(name));  // textChanged carries it on to the tab label
}

void MetaspriteFrame::setTimerFrames(const int frames) {
    timer_->setValue(frames);  // QSpinBox clamps to its own range
}

void MetaspriteFrame::setPublicFrame(const bool isPublic) {
    public_->setChecked(isPublic);
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
