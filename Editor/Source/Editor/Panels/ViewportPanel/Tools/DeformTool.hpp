#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace Desert::Core
{
    class Camera;
    class Scene;
} // namespace Desert::Core

namespace Desert::Editor::Tools
{
    class GizmoController;

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

        // Lattice is previewing: the viewport draws its control points and the gizmo on the selected ones.
        [[nodiscard]] bool HasLatticeSession() const
        {
            return m_Session.has_value() && m_Session->Lattice.has_value();
        }
        // UE ULatticeControlPointsMechanic: the lattice's points and edges over the viewport, a click selects
        // the point under the cursor (Shift adds, Ctrl toggles, empty space clears), and the frame gizmo on the
        // selection (ModelingState::LatticeGizmo) carries it - applied by the next Update.
        void DrawLatticeControls( ::Desert::Core::Scene& scene, GizmoController& gizmo,
                                  const std::shared_ptr<::Desert::Core::Camera>& camera, const glm::mat4& viewProj,
                                  const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool hovered );

        // UE UMeshSpaceDeformerTool bDrawVisualization: while Warp bends, the axis slab [Lower, Upper] drawn bent,
        // as the deformer carries it (Geometry::WarpBendGizmoPoint), over the viewport.
        static void DrawWarpVisualization( const Core::ModelingState& ms, const glm::mat4& viewProj,
                                           const glm::vec2& viewportPos, const glm::vec2& viewportSize );

    private:
        struct Session
        {
            Core::ModelingState::Tool                     Tool = Core::ModelingState::Tool::None;
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Input;     // the tool target the operator reads
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held: the "before"
            std::shared_ptr<const Geometry::DynamicMesh3> Preview;   // null while the settings are refused
            std::optional<Geometry::FFDLattice>           Lattice;   // Lattice: the rest lattice over Input
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Recompute( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Restore( ::Desert::Core::Scene& scene );
        [[nodiscard]] bool Accept( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        [[nodiscard]] bool SettingsChanged( const Core::ModelingState& ms ) const;
        // Lattice: (re)builds the rest lattice for the panel's resolution and resets the points to it.
        [[nodiscard]] bool BuildLattice( Core::ModelingState& ms );
        // Lattice: the gizmo moved since the last frame - carry the selected points along.
        void ApplyLatticeGizmo( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        // Lattice: the gizmo goes to the selection's centroid, unturned (UE's mechanic re-centres it).
        void PlaceLatticeGizmo( ::Desert::Core::Scene& scene, Core::ModelingState& ms );

        std::optional<Session> m_Session;
        // The settings the preview was computed with.
        Geometry::SmoothSettings m_Smooth;
        Geometry::OffsetSettings m_Offset;
        Geometry::WarpSettings   m_Warp;
        glm::mat4                m_WarpGizmo{ 1.0f };
        Geometry::LatticeSettings m_Lattice;
        std::vector<glm::dvec3>   m_LatticePoints;
        glm::mat4                 m_LatticeGizmo{ 1.0f }; // the gizmo the selected points were last moved to
    };
} // namespace Desert::Editor::Tools
