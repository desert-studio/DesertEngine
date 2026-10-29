#pragma once
// A LANDSCAPE'S SPLINE MESHES, RECORDED AS STATIC-MESH DRAWS (L9-MESHES; UE: the USplineMeshComponents
// ULandscapeSplineSegment::UpdateSplineMeshes keeps on the landscape).
//
// Not a renderer: MeshECSSystem owns one of these and every segment's meshes go out as the DrawStaticMeshCommand
// a static mesh records, at the identity transform (the splines are world cm). The bent geometry is DERIVED
// from LandscapeComponent::Splines and nothing else — it is rebuilt the frame the splines differ from the ones
// it was built from (an edit, a drag preview, an undo, a load), so no edit path has to remember to regenerate
// it, and it is never saved. A mesh asset still loading is retried every frame until it reads.
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Graphic/Render/Commands/DrawMeshCommand.hpp>
#include <Engine/World/Landscape/LandscapeSplines.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

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
    class LandscapeSplineMeshDraw final
    {
    public:
        /// What the mesh system already knows how to do, handed in so the rules stay its own:
        struct Services
        {
            /// The static mesh asset an id names, parsed; null while it loads or when it names none.
            std::function<const Assets::StaticMeshAsset*( const Common::UUID& mesh )> MeshAsset;
            /// A runtime instance of the material an asset id names; null when it cannot be made.
            std::function<Graphic::MaterialInstancePtr( const Common::UUID& material )> Instance;
            /// The default surface (a built-in shape, a mesh section with no material).
            std::function<Graphic::MaterialInstancePtr()> DefaultInstance;
        };

        /// Records the spline meshes of every visible landscape.
        void Record( entt::registry& registry, Graphic::Render::RenderCommandBuffer& commands,
                     uint32_t materialsVersion, const Services& services );

    private:
        struct Draw
        {
            std::shared_ptr<DynamicMesh>   Mesh;
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
