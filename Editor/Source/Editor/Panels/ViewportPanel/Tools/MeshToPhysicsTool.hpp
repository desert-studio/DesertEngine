#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/ECS/VolumeComponent.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <Editor/Core/Selection/ModelingState.hpp>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Tools
{
    // Modeling Mode -> Collision: UE's Mesh To Collision (USetCollisionGeometryTool) and Mesh To Volume
    // (UMeshToVolumeTool) as TOOLS, not instant buttons - the same session as every other modeling tool:
    //   - Begin: the selected entity's mesh is fitted (the collider shape, or the convex hull of the volume) and
    //     the result is only PREVIEWED (ToolPreview lines in the preview colour; nothing in the scene, no undo
    //     step);
    //   - Accept (the viewport tool bar / "Active tool: Accept"): the collider is added to the entity, or the
    //     volume entity replaces it - ONE undo step;
    //   - Cancel, another tool, or another selection: the preview goes, the entity is exactly as it was.
    // The session is one per editor (ModelingState is), so the panel's button, the palette command and the
    // viewport that updates it all see the same one.
    class MeshToPhysicsTool
    {
    public:
        // Opens Mesh To Collision with @p shape on the single selected entity; a refusal (no / several entities
        // selected, no mesh, the mesh still loading) carries its reason and opens nothing.
        static Common::ResultStr<bool> BeginCollision( ::Desert::Core::Scene& scene, const char* name,
                                                       Physics::ShapeType shape );
        // Opens Mesh To Volume of @p type on the single selected entity (refused as BeginCollision, and for an
        // entity that is already a volume).
        static Common::ResultStr<bool> BeginVolume( ::Desert::Core::Scene& scene, const char* name,
                                                    ECS::VolumeType type );

        // Per frame, from the viewport: shows the preview, resolves the tool bar's Accept / Cancel, and ends the
        // session when its tool or the selection changed.
        static void Update( ::Desert::Core::Scene& scene );

        // The open session's tool (Tool::None when none is open).
        [[nodiscard]] static Core::ModelingState::Tool OpenTool();
    };
} // namespace Desert::Editor::Tools
