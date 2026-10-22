#pragma once

#include <Engine/Geometry/MeshJacketOperation.hpp>
#include <Engine/Geometry/MeshProjectOperation.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/MeshAttributeOperations.hpp>

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

    // Modeling Mode -> Deform (and Attributes -> Normals, UE UEditNormalsTool, and Tangents, UE UMeshTangentsTool,
    // Geometry/MeshAttributeOperations.hpp, the same base-mesh-processing session): Smooth, Offset and Warp (UE's
    // USmoothMeshTool, UOffsetMeshTool and UMeshSpaceDeformerTool, whose frame is the viewport gizmo on
    // ModelingState::WarpGizmo; operators in Geometry/MeshDeformOperations.hpp). The same session as UE's
    // base-mesh-processing tools and our UVTool:
    //   - start: the tool target (GetToolTargetMesh) is the input, and what the component holds is the "before";
    //   - preview: the operator runs on the input with the panel's settings, again whenever they change, and the
    //     entity draws the result (ECS::SetEditableMesh, no undo step);
    //   - Accept: the previewed mesh stays and ONE undo step records it (Commands::RecordMeshChange);
    //   - Cancel, or another tool / another selection: the entity goes back to what it held.
    // Accept and Cancel are the viewport tool bar's (ActiveToolBar); this tool consumes them while a Deform tool
    // is active, before CubeGrid would read them.
    class MeshProcessingTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene );

        [[nodiscard]] static bool IsMeshProcessingTool( Core::ModelingState::Tool tool );
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
        // Tangents is previewing a mesh: the viewport draws its tangent frames as lines.
        [[nodiscard]] bool HasTangentsSession() const
        {
            return m_Session.has_value() && m_Session->Tool == Core::ModelingState::Tool::Tangents &&
                   m_Session->Preview != nullptr;
        }
        // Hole Fill is open on a mesh: the viewport draws its holes and a click on a hole's edge toggles it.
        [[nodiscard]] bool HasHoleFillSession() const
        {
            return m_Session.has_value() && m_Session->Tool == Core::ModelingState::Tool::HoleFill;
        }
        // UE UHoleFillTool's boundary-loop drawing and click selection (its UPolygonSelectionMechanic over the
        // loops): every hole of the session input as a line loop, the selected ones in the selection colour; a
        // left click within a few pixels of a hole's edge toggles that hole in ModelingState::HoleFillSelected.
        void DrawHoleFillControls( ::Desert::Core::Scene& scene, const glm::mat4& viewProj,
                                   const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool hovered );
        void DrawTangentsVisualization( ::Desert::Core::Scene& scene, const Core::ModelingState& ms,
                                        const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                        const glm::vec2& viewportSize ) const;

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
            std::vector<std::vector<int>>                 Holes;     // Hole Fill: Input's boundary loops' vertices
            // Project: the second selected entity, its mesh, and its object space -> Input's object space.
            Common::UUID                                  Target;
            std::shared_ptr<const Geometry::DynamicMesh3> TargetMesh;
            glm::dmat4                                    TargetToInput{ 1.0 };
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
        Geometry::NormalsSettings m_Normals;
        Geometry::TangentsSettings m_Tangents;
        Geometry::HoleFillSettings m_HoleFill;
        std::vector<int32_t>       m_HoleFillSelected;
        Geometry::JacketSettings   m_Jacket;
        Geometry::ProjectSettings  m_Project;
        Geometry::WarpSettings   m_Warp;
        glm::mat4                m_WarpGizmo{ 1.0f };
        Geometry::LatticeSettings m_Lattice;
        std::vector<glm::dvec3>   m_LatticePoints;
        glm::mat4                 m_LatticeGizmo{ 1.0f }; // the gizmo the selected points were last moved to
    };
} // namespace Desert::Editor::Tools
