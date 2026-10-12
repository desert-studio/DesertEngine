#pragma once

#include <cstdint>

// AN ICON'S COLOUR RUN AS DATA, apart from the service that bakes it (Runtime/Services/Icon): the UI walk,
// the property editor and the light gizmos read layers, only IconService makes them.
namespace Desert::Text
{
    // One colour run of an icon: the sub-rect of the shared atlas holding its distance field, plus the
    // fill the .svg authored. A plain monochrome icon has exactly one layer (white), so it tints as a
    // whole; a multi-colour icon has one layer per colour, drawn back-to-front in document order.
    struct IconLayer
    {
        float    U0 = 0.0f, V0 = 0.0f;
        float    U1 = 1.0f, V1 = 1.0f;
        uint32_t RGBA = 0xFFFFFFFFu;
    };
} // namespace Desert::Text
