#include "tile_image.hpp"

#include <algorithm>
#include <cstdint>

QVector<QRgb> paletteTable(const pal::Palette* palette) {
    const auto rgb = palette ? pal::rgbOf(*palette) : pal::placeholderRgb();

    QVector<QRgb> table;
    table.reserve(static_cast<qsizetype>(rgb.size()));
    for (const std::uint32_t colour : rgb) {
        table.append(qRgb((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, colour & 0xFF));
    }
    return table;
}

QImage cellImage(const chr::Cell& cell, const QVector<QRgb>& table) {
    QImage image(cell.widthPx(), cell.heightPx(), QImage::Format_Indexed8);
    image.setColorTable(table);

    for (int row = 0; row < cell.heightPx(); ++row) {
        const std::uint8_t* source = cell.pixels.data() + static_cast<std::size_t>(row) * cell.widthPx();
        std::copy_n(source, cell.widthPx(), image.scanLine(row));
    }

    return image;
}
