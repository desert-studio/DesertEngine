#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/LandscapeGrassTypeAsset.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.degrasstype` assets layer infos name (GR-1), loaded on demand.
     *
     * A layer info names its grass by GUID (LandscapeLayerInfoData::GrassType). The first reader that asks for
     * a handle creates the asset from its registry row and requests the read through the AsyncAssetLoader;
     * until it lands the type is Pending and grows nothing. A type that failed stays Failed for the session and
     * says why once in the log.
     */
    class LandscapeGrassTypeService
    {
    public:
        enum class State
        {
            Pending, // requested, not read yet
            Ready,
            Failed // Error() says why; said once in the log
        };

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The type's data when it is read; nullptr while it is pending or when it failed (StateOf says which).
        const Assets::Serialization::LandscapeGrassTypeData* Get( const Assets::AssetHandle& handle );

        /// Requests the handle when nobody has yet, then answers where it stands.
        State StateOf( const Assets::AssetHandle& handle );

        /// Why a Failed handle failed; empty for any other state.
        [[nodiscard]] std::string ErrorOf( const Assets::AssetHandle& handle ) const;

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );

        std::weak_ptr<Assets::AssetManager>                                                     m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                            m_Requests;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::LandscapeGrassTypeAsset>> m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>                                    m_Failed;
    };
} // namespace Desert::Runtime
