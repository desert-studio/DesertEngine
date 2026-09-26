#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <filesystem>
#include <string>

namespace Desert::Editor
{
    // WHAT A PICKER SHOWS FOR ONE REGISTRY ROW. The list itself comes from `ContentRegistry::Rows`; only the
    // label may come from the object, because five kinds (UI theme, control rig, retarget, anim graph, cloud
    // type) state a display name INSIDE the file that no registry column carries. When the object is
    // resident its own name is shown — the name every picker showed before it read rows — and when it is
    // not, the file's stem, which is exactly what those assets fall back to themselves when the file states
    // none. Probing never loads and never logs a miss: a row with no object is the normal state the
    // registry exists to serve.
    template <typename TypeAsset>
    [[nodiscard]] std::string PickerDisplayName( const Assets::AssetManager&               assets,
                                                 const Assets::ContentRegistry::PickerRow& row )
    {
        if ( const auto resident = assets.ProbeByHandle<TypeAsset>( row.Handle ) )
            return resident->GetDisplayName();
        return row.Path.stem().string();
    }
} // namespace Desert::Editor
