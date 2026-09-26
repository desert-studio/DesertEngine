#pragma once

#include <Engine/Assets/ContentRegistry.hpp>

#include <filesystem>
#include <string>

namespace Desert::Editor
{
    // WHAT A PICKER SHOWS FOR ONE REGISTRY ROW: the display name the file states (the registry's Name tag,
    // read by the scan from the document's top level, never by loading the asset), else the file's stem —
    // exactly what those assets fall back to themselves when the file states none.
    [[nodiscard]] inline std::string PickerDisplayName( const Assets::ContentRegistry::PickerRow& row )
    {
        return row.DisplayName.empty() ? row.Path.stem().string() : row.DisplayName;
    }
} // namespace Desert::Editor
