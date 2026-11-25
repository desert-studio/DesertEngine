#pragma once
// A LANDSCAPE SPLINE'S MESHES, RECORDED AS STATIC-MESH DRAWS (L9-MESHES; UE: the USplineMeshComponents
// ULandscapeSplineSegment::UpdateSplineMeshes keeps on the ALandscapeSplineActor).
//
// Not a renderer: a module draw MeshECSSystem records (ECS::IMeshStandInDraw, registered by
// Landscape::StartupModule); every segment's meshes go out as the DrawStaticMeshCommand
// a static mesh records, at the identity transform (the splines are world cm). The bent geometry is DERIVED
// from each LandscapeSplineComponent's Splines and nothing else — it is rebuilt the frame the splines differ from
// the ones it was built from (an edit, a drag preview, an undo, a load), so no edit path has to remember to
// regenerate it, and it is never saved. A mesh asset still loading is retried every frame until it reads.
#include <Engine/ECS/System/MeshStandInDraw.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Graphic/Render/Commands/DrawMeshCommand.hpp>
#include <LandscapeCore/Public/LandscapeSplines.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic::Render
{
    class RenderCommandBuffer;
}

namespace Desert
{
    class DynamicMesh;
}
namespace Desert::Assets
{
    class StaticMeshAsset;
}

namespace Desert::ECS
{
    class LandscapeSplineMeshDraw final : public IMeshStandInDraw
    {
    public:
        /// Records every spline segment's meshes; returns no entity (the splines draw beside every mesh).
        void Record( entt::registry& registry, Graphic::Render::RenderCommandBuffer& commands,
                     uint32_t materialsVersion, const Services& services,
                     std::unordered_set<entt::entity>& drawnInstead ) override;

    private:
        struct Draw
        {
            std::shared_ptr<DynamicMesh>    Mesh;
            Graphic::MaterialSlotBindingPtr Slots;
            uint32_t                        Part = 0u;
        };
        struct Built
        {
            World::Landscape::LandscapeSplines Splines;
            bool                               Complete         = false; // every source mesh read
            uint32_t                           MaterialsVersion = 0u;
            std::vector<Draw>                  Draws;
        };
        std::unordered_map<entt::entity, Built> m_Built;
    };
} // namespace Desert::ECS
