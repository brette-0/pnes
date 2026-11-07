#pragma once

#include <pnes/extras/ui/types.hpp>

namespace ui::impl {
    void UpdateCanvas(const Rect& last, const Rect& current) {
        // rewrite the attribs to nt attrib 3 for the entire containing area, always presume that every character
        // is inside an attribute cell, but also do not reach beyond that. Use the Rect.
    }
}