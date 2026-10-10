#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/Math/Ray.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Tools
{
    // Modeling Mode -> Create: places a parametric shape (UE's UAddPrimitiveTool). The geometry is
    // Geometry::ShapeGenerators - the one source every primitive comes from - and the placed entity carries
    // it as its EditMesh (SetEditableMesh), so the Model tools can edit it straight away. One click, one
    // entity, one undo step.
    class CreateShapeTool
    {
    public:
        // The preview follows the cursor while it is over the viewport and a click places there; otherwise
        // the preview stands where the viewport centre looks (`centreRay`), which is where the palette's
        // "place at the viewport centre" puts the shape - the tool is never invisible while it is active.
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const Common::Math::Ray& centreRay,
                     const glm::mat4& viewProj, const glm::vec2& viewportPos, const glm::vec2& viewportSize,
                     bool interactive );

        // The palette's placement: the active tool's shape where `ray` meets the scene or the ground, at the
        // current settings. Refused - with the reason, nothing placed - when Create Shape is not the active
        // tool or the ray meets neither (e.g. the viewport centre looks at the sky).
        [[nodiscard]] static Common::ResultStr<Common::UUID> PlaceAlong( ::Desert::Core::Scene&   scene,
                                                                         const Common::Math::Ray& ray );

        // The shape the settings describe, pivot included, in the entity's own space.
        [[nodiscard]] static Geometry::ShapeMesh Build( const Core::ModelingState::ShapeSettings& settings );

        // Where the settings' placement puts a shape along `ray`: the scene surface under it (bounding-box
        // level, Scene::Raycast) for On Scene, else the ground plane Y = 0. Nothing when the ray meets
        // neither, e.g. looking at the sky.
        [[nodiscard]] static std::optional<glm::vec3> PlacementPoint( const ::Desert::Core::Scene&   scene,
                                                                      const Common::Math::Ray&       ray,
                                                                      Core::ModelingState::Placement place );

        // Creates the entity at `position` with the shape as its EditMesh, selects it and records ONE undo
        // step. Refused, with nothing left in the scene, when the shape does not become a mesh.
        [[nodiscard]] static Common::ResultStr<Common::UUID>
        Place( ::Desert::Core::Scene& scene, const Core::ModelingState::ShapeSettings& settings,
               const Core::ModelingState::OutputSettings& output, const glm::vec3& position );

    private:
        // The preview IS the built shape (UE's UAddPrimitiveTool previews the mesh it will create, not a
        // box): its triangles, and the edges a viewer reads its form by - polygroup borders and open
        // borders, each with the one or two triangles it lies on. Rebuilt only when a setting changes.
        struct FeatureEdge
        {
            glm::vec3 A;
            glm::vec3 B;
            uint32_t  TriA;
            uint32_t  TriB;    // == TriA on an open border
            bool      Feature; // a polygroup or open border (drawn on the near side, not only as silhouette)
        };
        void Rebuild( const Core::ModelingState::ShapeSettings& settings );
        void DrawPreview( const glm::vec3& at, const glm::vec3& eye, const glm::mat4& viewProj,
                          const glm::vec2& viewportPos, const glm::vec2& viewportSize ) const;

        Core::ModelingState::ShapeSettings m_Built;
        Geometry::ShapeMesh                m_Preview;
        std::vector<FeatureEdge>           m_Edges;
        bool                               m_HasBuilt = false;
    };
} // namespace Desert::Editor::Tools
