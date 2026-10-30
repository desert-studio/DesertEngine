#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshUVOperations.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Tools
{
    // Modeling Mode -> UVs: AutoUV, Project, Unwrap, Layout and Transform (UE's UParameterizeMeshTool,
    // UUVProjectionTool, URecomputeUVsTool, UUVLayoutTool, the UV Editor's UUVEditorTransformTool). Like UE's
    // tools, each is a session on the selected entity's mesh:
    //   - start: the tool target (GetToolTargetMesh) is the input, and what the component holds is the "before";
    //   - preview: the operation runs on the input with the panel's settings, again whenever they change, and the
    //     entity draws the result (ECS::SetEditableMesh, no undo step) - the material shows the new UVs;
    //   - Accept: the previewed mesh stays and ONE undo step records it (Commands::RecordMeshChange); the scene
    //     saves it as the entity's editable mesh;
    //   - Cancel, or another tool / another selection: the entity goes back to what it held.
    //   - the preview is drawn with a checker of its UVs (UE's Checkerboard material mode), by the scene render
    //     (Render::ToolPreview); UV Project also draws its projection volume, and the viewport puts UE's
    //     transform gizmo on it (ProjectionGizmo / ApplyProjectionGizmo), which edits CenterOffset / RotationDeg /
    //     Dimensions.
    // Accept and Cancel are the viewport tool bar's (ActiveToolBar) and the palette's; this tool consumes them
    // while a UV tool is active, before CubeGrid would read them.
    class UVTool
    {
    public:
        // @p eyeWorld: this viewport's camera position (the checker's far-to-near order and shading).
        void Update( ::Desert::Core::Scene& scene, const glm::dvec3& eyeWorld );

        // UV Project's volume, for the viewport's gizmo: world space, axes scaled by the Dimensions (the
        // gizmo's scale handles). Nothing while no Project session has a volume.
        [[nodiscard]] std::optional<glm::mat4> ProjectionGizmo( ::Desert::Core::Scene& scene ) const;
        // The gizmo moved the volume to @p frame (world): CenterOffset / RotationDeg / Dimensions follow it.
        void ApplyProjectionGizmo( ::Desert::Core::Scene& scene, const glm::mat4& frame );

        [[nodiscard]] static bool IsUVTool( Core::ModelingState::Tool tool );

    private:
        struct Session
        {
            Core::ModelingState::Tool                     Tool = Core::ModelingState::Tool::None;
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Input;     // the tool target the operation reads
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held: the "before"
            std::shared_ptr<const Geometry::DynamicMesh3> Preview;   // null while the settings are refused
            std::optional<Geometry::UVProjectionVolume>   Volume;    // UV Project: the volume Preview used
            glm::dmat4 MeshToWorld{ 1.0 }; // UV Project: the entity transform Preview was projected through
            std::vector<int32_t> Scope;              // UV Transform: the selected triangles at Begin; empty = all
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Recompute( ::Desert::Core::Scene& scene, Core::ModelingState& ms, Session& session );
        static void        Restore( ::Desert::Core::Scene& scene, const Session& session );
        void DrawPreview( ::Desert::Core::Scene& scene, const glm::dvec3& eyeWorld, const Session& session ) const;
        [[nodiscard]] bool Accept( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        [[nodiscard]] bool SettingsChanged( const Core::ModelingState& ms ) const;
        // UV Project projects in world space: moving the entity mid-session moves the mesh through the volume.
        [[nodiscard]] static bool TransformChanged( ::Desert::Core::Scene& scene, const Session& session );
        void               RememberSettings( const Core::ModelingState& ms );

        std::optional<Session> m_Session;
        // The settings the preview was computed with.
        Geometry::AutoUVSettings    m_AutoUV;
        Geometry::UVProjectSettings m_Project;
        Geometry::UVUnwrapSettings  m_Unwrap;
        Geometry::UVLayoutSettings  m_Layout;
        Geometry::UVTransformSettings m_Transform;
    };
} // namespace Desert::Editor::Tools
