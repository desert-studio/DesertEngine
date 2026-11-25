#pragma once

// AN ENTITY DRAWN BY SOMETHING ELSE THAN ITS MESH (UE: a geometry collection's proxy replaces the static mesh's
// while it is fractured). A feature module registers a stand-in draw; MeshECSSystem records every registered one
// first, through the same instanced static path, and does not draw the static mesh of the entities it returns.
// Destruction's fracture pieces are the first (Destruction/Public/FracturePieceDraw.hpp); the Landscape's spline
// meshes (Landscape/Public/LandscapeSplineMeshDraw.hpp) draw beside every entity's and return none.

#include <Engine/Assets/Common.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace Desert::Graphic::Render
{
    class RenderCommandBuffer;
}

namespace Desert::Assets
{
    class StaticMeshAsset;
}

namespace Desert::ECS
{
    class IMeshStandInDraw
    {
    public:
        /// MeshECSSystem's material rules, lent to the stand-in so its draws take materials as a mesh's do.
        struct Services
        {
            std::function<std::vector<Assets::AssetHandle>( const Assets::AssetHandle& mesh )> SourceSlots;
            std::function<Graphic::MaterialInstancePtr( const Assets::AssetHandle& material )> Instance;
            std::function<Graphic::MaterialInstancePtr()>                                     DefaultInstance;
            std::function<Assets::AssetHandle( const Common::Content::AssetGuid& )>            MaterialByGuid;
            std::function<Graphic::Material*( const Graphic::MaterialInstance* )>              SlotMaterial;
            /// The static mesh asset a handle names, parsed; null while it loads or when it names none.
            std::function<const Assets::StaticMeshAsset*( const Assets::AssetHandle& mesh )> MeshAsset;
            /// The material service's handle of a material asset named by its asset handle.
            std::function<Assets::AssetHandle( const Assets::AssetHandle& external )> MaterialByExternal;
        };

        virtual ~IMeshStandInDraw() = default;

        /// Records the stand-in draws of this frame; adds to @p drawnInstead the entities whose mesh is not drawn.
        virtual void Record( entt::registry& registry, Graphic::Render::RenderCommandBuffer& commands,
                             uint32_t materialsVersion, const Services& services,
                             std::unordered_set<entt::entity>& drawnInstead ) = 0;
    };

    using MeshStandInDrawFactory = std::unique_ptr<IMeshStandInDraw> ( * )();

    inline std::vector<MeshStandInDrawFactory>& MeshStandInDraws()
    {
        static std::vector<MeshStandInDrawFactory> factories;
        return factories;
    }

    inline void RegisterMeshStandInDraw( MeshStandInDrawFactory factory )
    {
        MeshStandInDraws().push_back( factory );
    }
} // namespace Desert::ECS
