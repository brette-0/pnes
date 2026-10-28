/**
 * @file tile_image.hpp
 * @brief Bridges chr::Table's palette-index pixels into Qt images.
 *
 * chr_table.hpp and palette.hpp are deliberately Qt-free, so the one
 * conversion from 2bpp palette indices plus a pal::Palette to something
 * paintable lives here, shared by every widget that draws tiles.
 */
#pragma once

#include "chr_table.hpp"
#include "palette.hpp"

#include <QImage>
#include <QRgb>
#include <QVector>

/**
 * @brief @p palette as a Qt colour table, indexed by 2bpp value.
 *
 * A null @p palette gives pal::placeholderRgb() -- black, red, green, blue --
 * so tiles are still legible before any palette has been made, and so a
 * metasprite whose palette was deleted shows something rather than nothing.
 */
[[nodiscard]] QVector<QRgb> paletteTable(const pal::Palette* palette);

/// A cell as its own indexed image, 8 pixels wide and 8 or 16 tall, coloured
/// by @p table. Indexed rather than ARGB so it costs one byte per pixel and a
/// palette change is a colour-table swap rather than a re-decode.
[[nodiscard]] QImage cellImage(const chr::Cell& cell, const QVector<QRgb>& table);
