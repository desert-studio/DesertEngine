#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshUVOperations.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <optional>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Tools
{
    // Modeling Mode -> UVs: AutoUV, Project, Unwrap and Layout (UE's UParameterizeMeshTool, UUVProjectionTool,
    // URecomputeUVsTool, UUVLayoutTool). Like UE's tools, each is a session on the selected entity's mesh:
    //   - start: the tool target (GetToolTargetMesh) is the input, and what the component holds is the "before";
    //   - preview: the operation runs on the input with the panel's settings, again whenever they change, and the
    //     entity draws the result (ECS::SetEditableMesh, no undo step) - the material shows the new UVs;
    //   - Accept: the previewed mesh stays and ONE undo step records it (Commands::RecordMeshChange); the scene
    //     saves it as the entity's editable mesh;
    //   - Cancel, or another tool / another selection: the entity goes back to what it held.
    //   - the preview is drawn with a checker of its UVs (UE's Checkerboard material mode), by the scene render
    //     (Render::ToolPreview); UV Project also draws its projection volume, and the viewport puts UE's
    //     transform gizmo on it (ProjectionGizmo / ApplyProjectionGizmo), which edits CenterOffset / RotationDeg.
    // Accept and Cancel are the viewport tool bar's (ActiveToolBar) and the palette's; this tool consumes them
    // while a UV tool is active, before CubeGrid would read them.
    class UVTool
    {
    public:
        // @p eyeWorld: this viewport's camera position (the checker's far-to-near order and shading).
        void Update( ::Desert::Core::Scene& scene, const glm::dvec3& eyeWorld );

        // UV Project's volume, for the viewport's gizmo: world space, unit axes (no scale handle - the
        // Dimensions are the panel's). Nothing while no Project session has a volume.
        [[nodiscard]] std::optional<glm::mat4> ProjectionGizmo( ::Desert::Core::Scene& scene ) const;
        // The gizmo moved the volume to @p frame (world): CenterOffset / RotationDeg follow it.
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
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Recompute( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Restore( ::Desert::Core::Scene& scene );
        void               DrawPreview( ::Desert::Core::Scene& scene, const glm::dvec3& eyeWorld ) const;
        [[nodiscard]] bool Accept( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        [[nodiscard]] bool SettingsChanged( const Core::ModelingState& ms ) const;
        void               RememberSettings( const Core::ModelingState& ms );

        std::optional<Session> m_Session;
        // The settings the preview was computed with.
        Geometry::AutoUVSettings    m_AutoUV;
        Geometry::UVProjectSettings m_Project;
        Geometry::UVUnwrapSettings  m_Unwrap;
        Geometry::UVLayoutSettings  m_Layout;
    };
} // namespace Desert::Editor::Tools
