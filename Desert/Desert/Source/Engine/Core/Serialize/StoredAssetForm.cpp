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

        // Content that ships WITH the project: the tagged key too. These wrote the path relative to the assets
        // root until PATH1 restated the corpus (`Materials/M.demat` -> `assets:Materials/M.demat`) and deleted
        // the reader that joined a bare spelling to ASSETS_PATH (ComponentRegistry FileForStoredReference).
        if ( type == "MaterialAsset" || type == "CloudModellingVolumeAsset" || type == "ControlRigAsset" ||
             type == "AnimGraphAsset" || type == "UIThemeAsset" || type == "RetargetAsset" ||
             type == "PrefabAsset" || type == "FoliageTypeAsset" || type == "FractureAsset" ||
             type == "WaterWavesAsset" || type == "InputMappingContextAsset" || type == "PhysicsAsset" ||
             type == "SoundAsset" || type == "VFXSystemAsset" || type == "WorldParametersAsset" ||
             type == "WeatherPresetAsset" )
            return StoredAssetForm::StableKey;

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

            case StoredAssetForm::ProjectKey:
                return Common::AssetHandle::IsProjectRelativeKey( key ) ? key : std::string();
        }

        // Unreachable: the switch has no `default:` precisely so that a new enumerator stops
        // compiling here rather than falling through to an empty answer.
        return {};
    }
} // namespace Desert::Core::Serialize
