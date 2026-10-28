#include "palette_store.hpp"

#include <algorithm>

const pal::Palette* PaletteStore::find(const QString& name) const {
    const auto found = std::find_if(palettes_.begin(), palettes_.end(),
                                    [&](const pal::Palette& palette) {
                                        return QString::fromStdString(palette.name) == name;
                                    });
    return found == palettes_.end() ? nullptr : &*found;
}

pal::Palette* PaletteStore::mutableFind(const QString& name) {
    return const_cast<pal::Palette*>(find(name));
}

QStringList PaletteStore::names() const {
    QStringList out;
    out.reserve(static_cast<qsizetype>(palettes_.size()));
    for (const pal::Palette& palette : palettes_) {
        out.append(QString::fromStdString(palette.name));
    }
    return out;
}

QString PaletteStore::add(const QString& name) {
    QString wanted = name.trimmed();
    if (wanted.isEmpty()) wanted = "palette";

    // Names are the key metasprites refer to, so a collision is resolved here
    // rather than left to produce two palettes nothing can tell apart.
    QString unique = wanted;
    for (int suffix = 2; find(unique) != nullptr; ++suffix) {
        unique = QString("%1 %2").arg(wanted).arg(suffix);
    }

    pal::Palette palette;
    palette.name = unique.toStdString();
    palettes_.push_back(std::move(palette));
    emit changed();
    return unique;
}

void PaletteStore::remove(const QString& name) {
    const auto before = palettes_.size();
    palettes_.erase(std::remove_if(palettes_.begin(), palettes_.end(),
                                   [&](const pal::Palette& palette) {
                                       return QString::fromStdString(palette.name) == name;
                                   }),
                    palettes_.end());
    if (palettes_.size() != before) emit changed();
}

bool PaletteStore::rename(const QString& from, const QString& to) {
    const QString wanted = to.trimmed();
    if (wanted.isEmpty() || (wanted != from && find(wanted) != nullptr)) return false;

    pal::Palette* palette = mutableFind(from);
    if (!palette) return false;

    palette->name = wanted.toStdString();
    emit changed();
    return true;
}

void PaletteStore::setColour(const QString& name, const int slot, const std::uint8_t nesIndex) {
    // Slot 0 is the transparency entry and is not the user's to set, so it is
    // not addressable here.
    if (slot < 1 || slot > 3) return;
    if (pal::Palette* palette = mutableFind(name)) {
        palette->colours[static_cast<std::size_t>(slot - 1)] = nesIndex;
        emit changed();
    }
}

void PaletteStore::setPalettes(std::vector<pal::Palette> palettes) {
    palettes_ = std::move(palettes);
    emit changed();
}
