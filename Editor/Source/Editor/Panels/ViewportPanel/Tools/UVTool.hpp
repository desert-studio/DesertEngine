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
    // Modeling Mode -> UVs: AutoUV, Project, Unwrap and Layout (UE's UParameterizeMeshTool, UUVProjectionTool,
    // URecomputeUVsTool, UUVLayoutTool). Like UE's tools, each is a session on the selected entity's mesh:
    //   - start: the tool target (GetToolTargetMesh) is the input, and what the component holds is the "before";
    //   - preview: the operation runs on the input with the panel's settings, again whenever they change, and the
    //     entity draws the result (ECS::SetEditableMesh, no undo step) - the material shows the new UVs;
    //   - Accept: the previewed mesh stays and ONE undo step records it (Commands::RecordMeshChange); the scene
    //     saves it as the entity's editable mesh;
    //   - Cancel, or another tool / another selection: the entity goes back to what it held.
    // Accept and Cancel are the viewport tool bar's (ActiveToolBar) and the palette's; this tool consumes them
    // while a UV tool is active, before CubeGrid would read them.
    class UVTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene );

        [[nodiscard]] static bool IsUVTool( Core::ModelingState::Tool tool );

    private:
        struct Session
        {
            Core::ModelingState::Tool                     Tool = Core::ModelingState::Tool::None;
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Input;     // the tool target the operation reads
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held: the "before"
            std::shared_ptr<const Geometry::DynamicMesh3> Preview;   // null while the settings are refused
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Recompute( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Restore( ::Desert::Core::Scene& scene );
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
