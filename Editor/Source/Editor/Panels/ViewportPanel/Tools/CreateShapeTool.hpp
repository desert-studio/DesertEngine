#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/Math/Ray.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <glm/glm.hpp>

#include <optional>

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
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool interactive );

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
        // The preview's box is the built shape's; rebuilt only when a setting changes.
        Core::ModelingState::ShapeSettings m_Built;
        Common::Math::AABB                 m_Bounds;
        bool                               m_HasBuilt = false;
    };
} // namespace Desert::Editor::Tools
