#pragma once

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/SubsurfaceProfileAsset.hpp>
#include <Engine/Graphic/SubsurfaceProfiles/SubsurfaceProfileRegistry.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dsssprofile` profiles materials name, and the atlas rows they occupy (SSS-PROFILE).
     *
     * MaterialService asks IndexFor when it builds a material of the SubsurfaceProfile template; the renderer reads
     * Registry() to upload the atlas. READ SYNCHRONOUSLY, as the weather presets: a profile is a few hundred bytes.
     */
    class SubsurfaceProfileService
    {
    public:
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The profile's atlas row (reading the asset on first ask), or the reason it has none. Refused: an empty
        /// or malformed GUID, a GUID the project has no profile for, an unreadable profile, a full atlas.
        Common::ResultStr<uint32_t> IndexFor( const Assets::AssetGuidRef& reference );

        [[nodiscard]] Graphic::SubsurfaceProfileRegistry& Registry()
        {
            return m_Registry;
        }

        void Clear();

    private:
        std::weak_ptr<Assets::AssetManager>                                                    m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::SubsurfaceProfileAsset>> m_Held;
        Graphic::SubsurfaceProfileRegistry                                                     m_Registry;
    };
} // namespace Desert::Runtime
