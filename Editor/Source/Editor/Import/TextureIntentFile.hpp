#pragma once

// WHERE A TEXTURE SOURCE'S ASSET LIVES — `<stem>.detex` beside the source image.
//
// The `.detex` is the texture ASSET (an envelope, `Assets::TextureSourceAsset`): identity, the import
// settings (Intent, ColorSpace) and the source bytes. The authored intent is one of those settings, edited
// in the asset's Details. The pre-asset JSON `{"Intent": ...}` sidecar that once sat at the same path is not
// read by anything; the importer and SceneMigrator refuse it by path
// (`Assets::IsLegacyTextureIntentSidecar`, UE LinkerLoad.cpp:1537-1546).
//
// `.detex` is the AUTHORED asset beside the source under the content tree; `.tex` is the COOKED platform data
// under `Cooked/`. They never live in the same directory.

#include <Engine/Assets/TextureSourceAsset.hpp>

#include <filesystem>

namespace Desert::Editor
{
    /// THE ASSET PATH FOR @p source: the same directory, the same stem, `.detex`. One function, so the
    /// importer, the enumeration and the material import cannot disagree about where to look.
    ///
    /// `replace_extension` and not string concatenation: a source named `T_Rock.Colour.png` has the
    /// stem `T_Rock.Colour`, and the two spellings differ there.
    [[nodiscard]] inline std::filesystem::path TextureIntentPath( const std::filesystem::path& source )
    {
        std::filesystem::path path = source;
        path.replace_extension( Assets::kTextureAssetExtension );
        return path;
    }
} // namespace Desert::Editor
