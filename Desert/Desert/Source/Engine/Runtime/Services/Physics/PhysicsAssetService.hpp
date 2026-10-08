#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/PhysicsAsset.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dephysasset` physics assets ragdolls name (RAG1b), read on demand.
     *
     * FractureService's shape: the first ask creates the asset from its registry row and requests the read
     * through the AsyncAssetLoader; until it lands the asset is pending. The service holds the ASSET and hands
     * out its data as a pointer that keeps the asset alive, so a ragdoll never outlives its description.
     *
     * Every answer is for ONE MESH: an asset authored on another skeleton is refused (CheckSkeleton) — its
     * bodies name bones by name, and a rig that happens to share some names would get a ragdoll that is
     * silently wrong (UE refuses a physics asset whose skeleton differs from the mesh's the same way).
     */
    class PhysicsAssetService
    {
    public:
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The asset when it is read and authored on @p meshSkeleton; success with null while pending; an
        /// error when it cannot be read (said once) or is for another skeleton (named, every ask).
        Common::ResultStr<std::shared_ptr<const Physics::PhysicsAssetData>>
        Get( const Assets::AssetHandle& handle, const Common::Content::AssetGuid& meshSkeleton,
             std::string_view meshSkeletonName );

        /// Refuses, naming both skeletons, an asset whose skeleton GUID is not @p meshSkeleton (or a mesh that
        /// names no skeleton).
        static Common::BoolResultStr CheckSkeleton( const Physics::PhysicsAssetData&  asset,
                                                    const Common::Content::AssetGuid& meshSkeleton,
                                                    std::string_view                  meshSkeletonName );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );

        std::weak_ptr<Assets::AssetManager>                                          m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                 m_Requests;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::PhysicsAsset>> m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>                         m_Failed;
    };
} // namespace Desert::Runtime
