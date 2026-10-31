#pragma once

#include <Engine/Assets/LandscapeGrassTypeAsset.hpp>
#include <Engine/Runtime/Services/OnDemandAssets.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Runtime
{
    /**
     * @brief The `.degrasstype` assets layer infos name (GRASS-GPU), loaded on demand.
     *
     * A layer info names its grass types by GUID (LandscapeLayerInfoData::GrassTypes). The first reader that asks
     * for a handle creates the asset from its registry row and requests the read through the AsyncAssetLoader;
     * until it lands the type is Pending and grows nothing. A type that failed stays Failed for the session and
     * says why once in the log. A type an eviction sweep released is read again into the same shell on the next
     * ask (OnDemandAssets, the rule every type service shares). The service holds the ASSET, so a hot reload
     * re-read into it is what every reader sees on the next ask (AssetHotReload::PollLandscapeGrassTypes).
     */
    class LandscapeGrassTypeService
    {
    public:
        using State = OnDemandAssets<Assets::LandscapeGrassTypeAsset>::State;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The asset when it is read; nullptr while it is pending or when it failed (StateOf says which).
        Assets::Asset<Assets::LandscapeGrassTypeAsset> AssetOf( const Assets::AssetHandle& handle );

        /// Requests the handle when nobody has yet, then answers where it stands.
        State StateOf( const Assets::AssetHandle& handle );

        /// Why a Failed handle failed; empty for any other state.
        [[nodiscard]] std::string ErrorOf( const Assets::AssetHandle& handle ) const;

        void Clear();

        /// THE MATERIALS A GPU-INSTANCED ID LIST DRAWS WITH (UE: a material's bUsedWithInstancedStaticMeshes /
        /// bUsedWithFoliage, here stated by the assets instead of a flag): the `.demat` rows every registered
        /// `.degrasstype` reaches through the registry's dependency edges (ContentRegistry::Closure) - a Blade
        /// variety's material, a Mesh variety's mesh and that mesh's section materials (the sections
        /// LandscapeECSSystem builds a variety from), and their parents. The grass is the only producer of an id
        /// list (GrassRenderer: GpuInstancedRenderData::CameraIds / CascadeIds; a particle mesh draws its whole
        /// instance buffer), so these are the materials whose SurfaceInstanceIdsVariant the runtime can ask for
        /// and the cook lists (MeshRenderer::MaterialProgramVariants). Each handle is its row's own number (the
        /// one ContentRegistry::Rows hands the cook), listed once, in registry order.
        [[nodiscard]] static std::vector<Assets::AssetHandle> InstanceIdListMaterials();

    private:
        OnDemandAssets<Assets::LandscapeGrassTypeAsset> m_Types{
             Common::Content::ContentKind::LandscapeGrassType, "[Landscape] Grass type",
             "the layers naming it do not grow it", "the layer info names no grass type asset" };
    };
} // namespace Desert::Runtime
