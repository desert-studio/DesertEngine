#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/MeshAttributeOperations.hpp>

#include <Engine/Geometry/MeshDeformOperations.hpp>
#include <Engine/Geometry/ProgressCancel.hpp>

#include <chrono>
#include <future>
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
        // UE FBackgroundComputeSource's per-compute FProgressCancel: the token the running operator polls,
        // raised when that compute is superseded (a settings change) or its session ends, so the worker stops.
        struct CancelOnDrop
        {
            std::shared_ptr<Geometry::ProgressCancel> Token;

            CancelOnDrop() = default;
            explicit CancelOnDrop( std::shared_ptr<Geometry::ProgressCancel> token ) : Token( std::move( token ) ) {}
            CancelOnDrop( CancelOnDrop&& ) noexcept = default;
            CancelOnDrop& operator=( CancelOnDrop&& other ) noexcept
            {
                Raise();
                Token = std::move( other.Token );
                return *this;
            }
            CancelOnDrop( const CancelOnDrop& )            = delete;
            CancelOnDrop& operator=( const CancelOnDrop& ) = delete;
            ~CancelOnDrop() { Raise(); }
            void Raise()
            {
                if ( Token )
                    Token->Cancel();
            }
        };

        struct Session
        {
            Core::ModelingState::Tool                     Tool = Core::ModelingState::Tool::None;
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Input;     // the tool target the operator reads
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held: the "before"
            std::shared_ptr<const Geometry::DynamicMesh3> Preview;   // null while the settings are refused
            std::optional<Geometry::FFDLattice>           Lattice;   // Lattice: the rest lattice over Input
            // VoxBoolean (UE UBaseCreateFromSelectedTool): Entity is input A and draws the preview, Other is
            // input B, OtherInput its mesh baked into A's object space, OtherCommitted what B's component holds.
            Common::UUID                                  Other;
            std::shared_ptr<const Geometry::DynamicMesh3> OtherInput;
            std::shared_ptr<const Geometry::DynamicMesh3> OtherCommitted;
            std::string NameA; // VoxBoolean's A / VoxMorph's input: names the result
            // UE UBaseCreateFromSelectedTool hides its inputs while the preview shows: B is hidden for the session
            // (A draws the preview) and put back by Restore - on Cancel, on a selection change, and before Accept
            // records its undo step. OtherHidden: B was hidden by the session; OtherVisibility: B's
            // VisibilityComponent before that (nullopt = it had none, so Restore removes the one added).
            bool                OtherHidden = false;
            std::optional<bool> OtherVisibility;
            // UE UMeshOpPreviewWithBackgroundCompute: the operator runs on a JobSystem worker on copies of the
            // input and the settings; the last preview stays on screen until it lands. A settings change while it
            // runs cancels it (PendingCancel) and starts the next one at once; dropping the session cancels it
            // too. A cancelled compute's future is dropped, so its result goes nowhere (the task owns its inputs).
            std::future<Common::ResultStr<Geometry::DeformOutcome>> Pending;
            CancelOnDrop                                            PendingCancel;
            std::chrono::steady_clock::time_point                   PendingStarted;
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        // Starts the operator in the background with the panel's settings (they become the "computed with" ones).
        void               Recompute( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        // The background operator finished: its mesh becomes the preview, or its refusal the status.
        void               ApplyResult( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        // VoxBoolean: hides input B for the session (Restore shows it again).
        void               HideOther( ::Desert::Core::Scene& scene );
        void               Restore( ::Desert::Core::Scene& scene );
        [[nodiscard]] bool Accept( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        // VoxBoolean's and VoxMorph's Accept (UE UBaseCreateFromSelectedTool::OnShutdown): the preview becomes a
        // NEW entity (a copy of A carrying the result), the inputs are deleted / hidden / kept - all one undo
        // step.
        [[nodiscard]] bool AcceptCreateFromSelected( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        // The session's inputs are still what the scene selection is (one entity, or A and B for VoxBoolean).
        [[nodiscard]] bool SelectionStillMatches() const;
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
        Geometry::VoxWrapSettings m_VoxWrap;
        Geometry::VoxBooleanSettings m_VoxBoolean;
        Geometry::VoxMorphSettings   m_VoxMorph;
        Geometry::NormalsSettings m_Normals;
        Geometry::TangentsSettings m_Tangents;
        Geometry::WarpSettings   m_Warp;
        glm::mat4                m_WarpGizmo{ 1.0f };
        Geometry::LatticeSettings m_Lattice;
        std::vector<glm::dvec3>   m_LatticePoints;
        glm::mat4                 m_LatticeGizmo{ 1.0f }; // the gizmo the selected points were last moved to
    };
} // namespace Desert::Editor::Tools
