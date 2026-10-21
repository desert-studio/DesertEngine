#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>

#include <memory>
#include <optional>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Tools
{
    // Modeling Mode -> Deform: Smooth, Offset and Warp (UE's USmoothMeshTool, UOffsetMeshTool and
    // UMeshSpaceDeformerTool, whose frame is the viewport gizmo on ModelingState::WarpGizmo;
    // operators in Geometry/MeshDeformOperations.hpp). The same session as UE's base-mesh-processing tools and
    // our UVTool:
    //   - start: the tool target (GetToolTargetMesh) is the input, and what the component holds is the "before";
    //   - preview: the operator runs on the input with the panel's settings, again whenever they change, and the
    //     entity draws the result (ECS::SetEditableMesh, no undo step);
    //   - Accept: the previewed mesh stays and ONE undo step records it (Commands::RecordMeshChange);
    //   - Cancel, or another tool / another selection: the entity goes back to what it held.
    // Accept and Cancel are the viewport tool bar's (ActiveToolBar); this tool consumes them while a Deform tool
    // is active, before CubeGrid would read them.
    class DeformTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene );

        [[nodiscard]] static bool IsDeformTool( Core::ModelingState::Tool tool );
        // Warp is previewing: the viewport draws its frame gizmo (ModelingState::WarpGizmo).
        [[nodiscard]] bool HasWarpSession() const
        {
            return m_Session.has_value() && m_Session->Tool == Core::ModelingState::Tool::Warp;
        }

    private:
        struct Session
        {
            Core::ModelingState::Tool                     Tool = Core::ModelingState::Tool::None;
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Input;     // the tool target the operator reads
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held: the "before"
            std::shared_ptr<const Geometry::DynamicMesh3> Preview;   // null while the settings are refused
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Recompute( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Restore( ::Desert::Core::Scene& scene );
        [[nodiscard]] bool Accept( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        [[nodiscard]] bool SettingsChanged( const Core::ModelingState& ms ) const;

        std::optional<Session> m_Session;
        // The settings the preview was computed with.
        Geometry::SmoothSettings m_Smooth;
        Geometry::OffsetSettings m_Offset;
        Geometry::WarpSettings   m_Warp;
        glm::mat4                m_WarpGizmo{ 1.0f };
    };
} // namespace Desert::Editor::Tools
