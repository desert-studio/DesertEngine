#include <Engine/Core/Serialize/StoredAssetForm.hpp>

#include <Common/Core/AssetHandle.hpp>

namespace Desert::Core::Serialize
{
    std::optional<StoredAssetForm> StoredFormFor( const std::string& type )
    {
        // Tagged keys: the texture slot (Engine/Core/Serialize/TextureSlot.hpp explains why at length)
        // and the three SERVICE types, which are not AssetManager assets at all and whose branches
        // already went through `StableKeyForPath`.
        if ( type == "TextureAsset" || type == "FontAsset" || type == "VideoAsset" || type == "IconAsset" )
            return StoredAssetForm::StableKey;

        // Relative to the assets root: content that ships WITH the project. The material branch was
        // the first to be fixed this way — it wrote the filepath verbatim, which put 22 distinct
        // `/Users/<somebody>/.../Materials/*.demat` into 42 of the 51 scenes in this repository — and
        // the sculpted body, the rig, the graph and the theme each copied the reasoning.
        if ( type == "MaterialAsset" || type == "CloudModellingVolumeAsset" || type == "ControlRigAsset" ||
             type == "AnimGraphAsset" || type == "UIThemeAsset" || type == "RetargetAsset" ||
             type == "PrefabAsset" || type == "FoliageTypeAsset" || type == "FractureAsset" )
            return StoredAssetForm::AssetsRelative;

        // Meshes (static/skinned both resolved handle->path through the MeshAsset base): the tagged stable
        // key, as a texture's (BISTRO-OPEN). The machine path they used to store put a developer's home
        // directory into every scene that held an imported mesh (1296 of them in GI_Bistro_Day). A mesh can
        // live under either content root (the engine's probe meshes are under RESOURCE_PATH), which is why
        // it is the stable key and not the assets-relative form. The GUID beside it stays the identity.
        if ( type == "StaticMeshAsset" || type == "SkinnedMeshAsset" || type == "MeshAsset" )
            return StoredAssetForm::StableKey;

        // Skyboxes store {GUID, project key} since SCNE 29; the absolute path is unreachable by construction.
        if ( type == "SkyboxAsset" )
            return StoredAssetForm::ProjectKey;

        return std::nullopt;
    }

    std::string RenderStoredForm( StoredAssetForm form, const std::string& key )
    {
        switch ( form )
        {
            case StoredAssetForm::StableKey:
                return key;

            case StoredAssetForm::AssetsRelative:
            {
                // The assets tag read out of AssetHandle's own root table rather than spelled again —
                // there is exactly one spelling of "assets" in this repository and it is that table's.
                const std::string prefix = std::string( Common::AssetHandle::AssetsTag() ) + ':';
                if ( key.starts_with( prefix ) )
                    return key.substr( prefix.size() );
                // Under another root, or under none: the key expands to the path it names and that is
                // what goes in the file, which is what the branches this replaces did.
                return Common::AssetHandle::PathForStableKey( key ).string();
            }

            case StoredAssetForm::ProjectKey:
                return Common::AssetHandle::IsProjectRelativeKey( key ) ? key : std::string();
        }

        // Unreachable: the switch has no `default:` precisely so that a new enumerator stops
        // compiling here rather than falling through to an empty answer.
        return {};
    }
} // namespace Desert::Core::Serialize
