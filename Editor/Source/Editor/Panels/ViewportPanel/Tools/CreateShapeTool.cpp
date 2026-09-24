#include "CreateShapeTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <array>
#include <memory>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        bool WorldToScreen( const glm::vec3& world, const glm::mat4& viewProj, const glm::vec2& pos,
                            const glm::vec2& size, ImVec2& out )
        {
            const glm::vec4 clip = viewProj * glm::vec4( world, 1.0f );
            if ( clip.w <= 0.0001f )
                return false; // behind the camera
            const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
            out.x               = pos.x + ( ndc.x * 0.5f + 0.5f ) * size.x;
            out.y               = pos.y + ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * size.y;
            return true;
        }

        // The ray through the middle of the viewport, from the camera the cursor ray starts at.
        Common::Math::Ray CentreRay( const Common::Math::Ray& cursorRay, const glm::mat4& viewProj )
        {
            glm::vec4 p = glm::inverse( viewProj ) * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f );
            p /= p.w;
            return Common::Math::Ray( cursorRay.Origin, glm::vec3( p ) - cursorRay.Origin );
        }
    } // namespace

    Geometry::ShapeMesh CreateShapeTool::Build( const MS::ShapeSettings& s )
    {
        const Geometry::ShapeOptions options{ s.Groups, s.Pivot };
        switch ( s.Kind )
        {
            case MS::Shape::Box:
                return Geometry::MakeBox( { s.Width, s.Height, s.Depth }, glm::ivec3( s.Subdivisions ), options );
            case MS::Shape::Sphere:
                return Geometry::MakeSphere( s.Width, s.Slices, s.Stacks, options );
            case MS::Shape::Cylinder:
                return Geometry::MakeCylinder( s.Width, s.Height, s.Slices, options );
            case MS::Shape::Cone:
                return Geometry::MakeCone( s.Width, s.Height, s.Slices, options );
            case MS::Shape::Capsule:
                // Stacks count pole to pole, as on the Sphere, so each hemisphere takes half.
                return Geometry::MakeCapsule( s.Width, s.Height, s.Slices, std::max( s.Stacks / 2, 1 ), options );
            case MS::Shape::Pyramid:
                return Geometry::MakePyramid( { s.Width, s.Height, s.Depth }, options );
            case MS::Shape::Stairs:
                return Geometry::MakeStairs( s.Width, s.StepDepth, s.StepHeight, s.Steps, options );
        }
        return Geometry::MakeBox( { s.Width, s.Height, s.Depth }, glm::ivec3( s.Subdivisions ), options );
    }

    std::optional<glm::vec3> CreateShapeTool::PlacementPoint( const ::Desert::Core::Scene& scene,
                                                              const Common::Math::Ray& ray, MS::Placement place )
    {
        if ( place == MS::Placement::OnScene )
        {
            ::Desert::Core::RaycastHit hit;
            if ( scene.Raycast( ray, hit ) && hit.Distance > 0.0f )
                return hit.Point;
        }
        // The ground plane: only in front of the camera, and never along a ray parallel to it.
        if ( std::abs( ray.Direction.y ) < 1e-6f )
            return std::nullopt;
        const float t = -ray.Origin.y / ray.Direction.y;
        if ( t <= 0.0f )
            return std::nullopt;
        return ray.Origin + ray.Direction * t;
    }

    Common::ResultStr<Common::UUID> CreateShapeTool::Place( ::Desert::Core::Scene&    scene,
                                                            const MS::ShapeSettings&  settings,
                                                            const MS::OutputSettings& output,
                                                            const glm::vec3&          position )
    {
        auto mesh = Geometry::ShapeToEditMesh( Build( settings ) );
        if ( !mesh.IsSuccess() )
            return Common::MakeFormattedError<Common::UUID>( "the {} was not placed: {}",
                                                             MS::ShapeName( settings.Kind ), mesh.GetError() );

        ECS::Entity entity = scene.CreateNewEntity( MS::ShapeName( settings.Kind ) );
        entity.GetComponent<ECS::TransformComponent>().Translation = position;
        auto& smc = entity.AddComponent<ECS::StaticMeshComponent>();
        if ( auto set = Geometry::Bridge::SetEditableMeshFromEditMesh( smc, mesh.ExtractValue() );
             !set.IsSuccess() )
        {
            scene.DestroyEntity( entity );
            return Common::MakeFormattedError<Common::UUID>( "the {} was not placed: {}",
                                                             MS::ShapeName( settings.Kind ), set.GetError() );
        }
        const Common::UUID id = entity.GetComponent<ECS::UUIDComponent>().UUID;
        // Output: Static Mesh swaps the EditMesh for a new asset BEFORE the creation is recorded, so the one
        // step below creates the finished static-mesh entity. A refused write takes the entity back out:
        // the tool placed nothing rather than something other than what the Output setting asked for.
        if ( output.Type == MS::OutputType::StaticMesh )
            if ( auto written = Commands::OutputStaticMesh( id, output.Folder, output.Name );
                 !written.IsSuccess() )
            {
                scene.DestroyEntity( entity );
                return Common::MakeFormattedError<Common::UUID>(
                     "the {} was not placed: {}", MS::ShapeName( settings.Kind ), written.GetError() );
            }
        // ONE undo step: undo removes the entity (snapshotting it through the scene serializer - the EditMesh
        // or the mesh reference), redo brings it back under the same UUID.
        Commands::NotifyCreated( { id } );
        Core::SelectionManager::SetSelected( id );
        return Common::MakeSuccess( id );
    }

    void CreateShapeTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                  const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                  const glm::vec2& viewportSize, bool interactive )
    {
        auto& ms = MS::Get();
        if ( ms.ActiveTool != MS::Tool::CreateShape )
        {
            ms.ReqPlaceCentre = false;
            return;
        }
        const MS::ShapeSettings& settings = ms.CreateShape;

        if ( ms.ReqPlaceCentre )
        {
            ms.ReqPlaceCentre = false;
            if ( const auto point = PlacementPoint( scene, CentreRay( ray, viewProj ), settings.Place ) )
            {
                if ( auto placed = Place( scene, settings, ms.Output, *point ); !placed.IsSuccess() )
                    LOG_ERROR( "[CreateShape] {0}", placed.GetError() );
            }
            else
            {
                LOG_WARN( "[CreateShape] the viewport centre meets neither the scene nor the ground" );
            }
        }

        const ImVec2 mouse   = ::ImGui::GetMousePos();
        const bool   hovered = interactive && mouse.x >= viewportPos.x && mouse.y >= viewportPos.y &&
                             mouse.x < viewportPos.x + viewportSize.x && mouse.y < viewportPos.y + viewportSize.y;
        if ( !hovered )
            return;
        const auto point = PlacementPoint( scene, ray, settings.Place );
        if ( !point )
            return;

        if ( !m_HasBuilt || !( m_Built == settings ) )
        {
            m_Bounds   = Build( settings ).Bounds();
            m_Built    = settings;
            m_HasBuilt = true;
        }

        // Preview: the box the shape will fill, where a click puts it.
        const glm::vec3                lo      = m_Bounds.Min + *point;
        const glm::vec3                hi      = m_Bounds.Max + *point;
        const std::array<glm::vec3, 8> corners = { glm::vec3( lo.x, lo.y, lo.z ), glm::vec3( hi.x, lo.y, lo.z ),
                                                   glm::vec3( hi.x, lo.y, hi.z ), glm::vec3( lo.x, lo.y, hi.z ),
                                                   glm::vec3( lo.x, hi.y, lo.z ), glm::vec3( hi.x, hi.y, lo.z ),
                                                   glm::vec3( hi.x, hi.y, hi.z ), glm::vec3( lo.x, hi.y, hi.z ) };
        constexpr std::array<std::array<int, 2>, 12> kEdges = { { { 0, 1 },
                                                                  { 1, 2 },
                                                                  { 2, 3 },
                                                                  { 3, 0 },
                                                                  { 4, 5 },
                                                                  { 5, 6 },
                                                                  { 6, 7 },
                                                                  { 7, 4 },
                                                                  { 0, 4 },
                                                                  { 1, 5 },
                                                                  { 2, 6 },
                                                                  { 3, 7 } } };
        ImDrawList*                                  draw   = ::ImGui::GetWindowDrawList();
        for ( const auto& edge : kEdges )
        {
            ImVec2 a;
            ImVec2 b;
            if ( WorldToScreen( corners[edge[0]], viewProj, viewportPos, viewportSize, a ) &&
                 WorldToScreen( corners[edge[1]], viewProj, viewportPos, viewportSize, b ) )
                draw->AddLine( a, b, IM_COL32( 255, 200, 60, 255 ), 1.5f );
        }
        ImVec2 pivot;
        if ( WorldToScreen( *point, viewProj, viewportPos, viewportSize, pivot ) )
            draw->AddCircleFilled( pivot, 4.0f, IM_COL32( 255, 200, 60, 255 ) );

        // Alt + LMB is the camera's orbit, not a placement.
        const ImGuiIO& io = ::ImGui::GetIO();
        if ( !::ImGui::IsAnyItemActive() && !io.KeyAlt && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            if ( auto placed = Place( scene, settings, ms.Output, *point ); !placed.IsSuccess() )
                LOG_ERROR( "[CreateShape] {0}", placed.GetError() );
        }
    }
} // namespace Desert::Editor::Tools
