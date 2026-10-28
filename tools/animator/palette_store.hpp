/**
 * @file palette_store.hpp
 * @brief The project's palettes, and who to tell when they change.
 */
#pragma once

#include "palette.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

/**
 * @brief Every palette in the project, keyed by name.
 *
 * Names are the key: a metasprite records which palette it uses by name, and
 * the sheet's own view does too, so they are kept unique and non-empty here
 * rather than every reader having to cope with duplicates.
 */
class PaletteStore final : public QObject {
    Q_OBJECT

public:
    explicit PaletteStore(QObject* parent = nullptr) : QObject(parent) {}

    [[nodiscard]] const std::vector<pal::Palette>& palettes() const { return palettes_; }

    /// The palette called @p name, or nullptr.
    [[nodiscard]] const pal::Palette* find(const QString& name) const;

    /// Every name, in store order, for filling a dropdown.
    [[nodiscard]] QStringList names() const;

    /// Adds a palette, uniquifying @p name if it is taken or empty.
    /// @return The name it ended up with.
    QString add(const QString& name);

    void remove(const QString& name);

    /// @return Whether the rename happened; false if @p to is empty or taken.
    bool rename(const QString& from, const QString& to);

    /// Sets colour @p slot (1-3; 0 is the fixed transparency entry) of @p name.
    void setColour(const QString& name, int slot, std::uint8_t nesIndex);

    /// Replaces the lot, for a project load.
    void setPalettes(std::vector<pal::Palette> palettes);

signals:
    /// A palette was added, removed, renamed or recoloured. Everything drawing
    /// with a palette redraws on this.
    void changed();

private:
    [[nodiscard]] pal::Palette* mutableFind(const QString& name);

    std::vector<pal::Palette> palettes_;
};
