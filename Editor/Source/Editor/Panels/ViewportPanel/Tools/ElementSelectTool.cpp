#include "ElementSelectTool.hpp"

#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshSelection.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <memory>
#include <optional>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        using Geometry::ElementMode;

        struct Target
        {
            std::shared_ptr<const Geometry::FDynamicMesh3> Source;
            glm::mat4                                 World{ 1.0f };
        };

        // The selected entity's tool target mesh and world transform; none for a primitive (nothing to lift).
        Target FindTarget( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            if ( static_cast<uint64_t>( id ) == 0 )
                return {};
            auto ref = scene.FindEntityByID( id );
            if ( !ref )
                return {};
            ECS::Entity e = ref->get();
            if ( !e.HasComponent<ECS::StaticMeshComponent>() )
                return {};
            // Any static mesh is a target (UE ToolTarget): its EditableMesh, or its asset lifted.
            auto target = GetToolTargetMesh( e.GetComponent<ECS::StaticMeshComponent>() );
            if ( !target.IsSuccess() )
                return {};
            return { target.GetValue().Mesh,
                     e.HasComponent<ECS::TransformComponent>()
                          ? e.GetComponent<ECS::TransformComponent>().GetTransform()
                          : glm::mat4( 1.0f ) };
        }

        struct Painter
        {
            ImDrawList&                     List;
            const Geometry::FDynamicMesh3&  Mesh;
            const Geometry::FGroupTopology& Topology;
            const glm::mat4&          World;
            const glm::mat4&          ViewProj;
            glm::vec2                 Pos;
            glm::vec2                 Size;

            bool Screen( int v, ImVec2& out ) const
            {
                glm::vec2 px;
                const auto q = Mesh.GetVertex( v );
                if ( !Geometry::ProjectToViewport( glm::vec3( World * glm::vec4( q.X, q.Y, q.Z, 1.0f ) ),
                                                   ViewProj, Pos, Size, px ) )
                    return false;
                out = ImVec2( px.x, px.y );
                return true;
            }
            void Vertex( int v, ImU32 colour, float radius ) const
            {
                ImVec2 p;
                if ( Screen( v, p ) )
                    List.AddCircleFilled( p, radius, colour );
            }
            void Edge( int e, ImU32 colour, float width ) const
            {
                const auto ev = Mesh.GetEdgeV( e );
                ImVec2     a, b;
                if ( Screen( ev.A, a ) && Screen( ev.B, b ) )
                    List.AddLine( a, b, colour, width );
            }
            void Triangle( int t, ImU32 fill, ImU32 outline ) const
            {
                const auto tri = Mesh.GetTriangle( t );
                ImVec2     a, b, c;
                if ( !Screen( tri.A, a ) || !Screen( tri.B, b ) || !Screen( tri.C, c ) )
                    return;
                if ( fill != 0 )
                    List.AddTriangleFilled( a, b, c, fill );
                List.AddTriangle( a, b, c, outline, 1.5f );
            }
            // A whole element of any mode; a polygroup is drawn as its triangles.
            void Element( ElementMode mode, int id, ImU32 fill, ImU32 line ) const
            {
                switch ( mode )
                {
                    case ElementMode::Vertex:
                        Vertex( id, line, 4.5f );
                        break;
                    case ElementMode::Edge:
                        Edge( id, line, 3.0f );
                        break;
                    case ElementMode::Triangle:
                        Triangle( id, fill, line );
                        break;
                    case ElementMode::PolyGroup:
                        break;
                }
            }
        };

        void DrawSelection( const Painter& paint, const Geometry::ElementSelection& selection, ImU32 fill,
                            ImU32 line )
        {
            if ( selection.Mode() == ElementMode::PolyGroup )
            {
                const Geometry::ElementSelection tris =
                     Geometry::ConvertSelection( paint.Mesh, paint.Topology, selection, ElementMode::Triangle );
                for ( const int t : tris.Ids() )
                    paint.Triangle( t, fill, line );
                return;
            }
            for ( const int id : selection.Ids() )
                paint.Element( selection.Mode(), id, fill, line );
        }

        // The ray through the viewport's centre, from the cursor ray's origin (the camera) - what a click in
        // the middle of the viewport would have produced.
        Common::Math::Ray CentreRay( const Common::Math::Ray& cursorRay, const glm::mat4& viewProj )
        {
            glm::vec4 p = glm::inverse( viewProj ) * glm::vec4( 0.0f, 0.0f, 0.5f, 1.0f );
            p /= p.w;
            return Common::Math::Ray( cursorRay.Origin, glm::vec3( p ) - cursorRay.Origin );
        }
    } // namespace

    void ElementSelectTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                    const glm::mat4& viewProj, const glm::vec2& viewportPos,
                                    const glm::vec2& viewportSize, bool interactive )
    {
        auto& state = Core::MeshElementSelection::Get();
        if ( Core::ModelingState::Get().ActiveTool != Core::ModelingState::Tool::ElementSelect )
            return;

        const auto&        selected = Core::SelectionManager::GetSelected();
        const Common::UUID entity   = selected.has_value() ? *selected : Common::UUID::Null();
        const Target       target   = FindTarget( scene, entity );
        state.Track( entity, target.Source );
        if ( !target.Source )
        {
            state.ReqPickCentre = false;
            return;
        }
        // Track built the topology for exactly this mesh (it rebuilds whenever the entity's mesh changes).
        const Geometry::FDynamicMesh3&  mesh     = *state.Mesh();
        const Geometry::FGroupTopology& topology = *state.Topology();

        Geometry::PickView view;
        view.LocalToWorld    = target.World;
        view.ViewProj        = viewProj;
        view.ViewportPos     = viewportPos;
        view.ViewportSize    = viewportSize;
        view.TolerancePixels = kTolerancePixels;

        const bool pickCentre = state.ReqPickCentre;
        state.ReqPickCentre   = false;
        const bool clicked =
             interactive && !::ImGui::IsAnyItemActive() && ::ImGui::IsMouseClicked( ImGuiMouseButton_Left );

        const ImVec2 mouse   = ::ImGui::GetMousePos();
        const bool   hovered = interactive && mouse.x >= viewportPos.x && mouse.y >= viewportPos.y &&
                             mouse.x < viewportPos.x + viewportSize.x && mouse.y < viewportPos.y + viewportSize.y;

        Geometry::ElementHit hover;
        if ( pickCentre )
        {
            const Common::Math::Ray centre = CentreRay( ray, viewProj );
            view.Cursor                    = viewportPos + viewportSize * 0.5f;
            view.RayOrigin                 = centre.Origin;
            view.RayDirection              = centre.Direction;
            hover                          = Geometry::PickElement( mesh, topology, state.Mode(), view );
        }
        else if ( hovered )
        {
            view.Cursor       = glm::vec2( mouse.x, mouse.y );
            view.RayOrigin    = ray.Origin;
            view.RayDirection = ray.Direction;
            hover             = Geometry::PickElement( mesh, topology, state.Mode(), view );
        }

        // THE KNIFE (Alt+K): the next two clicks draw the cut line instead of selecting, and the selected
        // polygroups are cut by the plane that line sweeps into the scene. Esc, or losing the viewport,
        // drops it.
        bool selectClick = clicked;
        if ( m_KnifeArmed )
        {
            selectClick = false;
            if ( !interactive || ::ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
            {
                m_KnifeArmed = false;
                m_KnifeStart.reset();
            }
            else if ( clicked && hovered && !m_KnifeStart )
                m_KnifeStart = glm::vec2( mouse.x, mouse.y );
            else if ( clicked && hovered )
            {
                const auto toNdc = [&]( const glm::vec2& p )
                {
                    return glm::vec2( ( p.x - viewportPos.x ) / viewportSize.x * 2.0f - 1.0f,
                                      1.0f - ( p.y - viewportPos.y ) / viewportSize.y * 2.0f );
                };
                const glm::vec2 from = *m_KnifeStart;
                m_KnifeArmed         = false;
                m_KnifeStart.reset();
                auto plane = Geometry::CutPlaneFromScreenLine( viewProj * target.World, toNdc( from ),
                                                               toNdc( glm::vec2( mouse.x, mouse.y ) ) );
                if ( !plane.IsSuccess() )
                {
                    LOG_WARN( "{0}", plane.GetError() );
                    return;
                }
                Core::MeshOperationArgs args = Core::ArgsFromModelingState();
                args.CutPlane                = plane.GetValue();
                if ( auto applied = Core::ApplyMeshOperation( scene, Core::MeshOperation::Cut, args );
                     !applied.IsSuccess() )
                    LOG_WARN( "{0}", applied.GetError() );
                return; // the mesh was replaced: `mesh` is the old one
            }
            if ( m_KnifeStart && hovered )
                ::ImGui::GetWindowDrawList()->AddLine( ImVec2( m_KnifeStart->x, m_KnifeStart->y ), mouse,
                                                       IM_COL32( 255, 70, 70, 255 ), 2.0f );
        }

        if ( pickCentre || selectClick )
        {
            const ImGuiIO& io       = ::ImGui::GetIO();
            const bool     modifies = !pickCentre && ( io.KeyShift || io.KeyCtrl );
            // A plain click starts over (a click on empty space clears); Shift / Ctrl edit what is there.
            Geometry::ElementSelection next =
                 modifies ? state.Selection() : Geometry::ElementSelection( state.Mode() );
            const char* label = "Mesh Selection: Select";
            if ( hover.IsHit() )
            {
                if ( !pickCentre && io.KeyCtrl )
                {
                    next.Remove( hover.Id );
                    label = "Mesh Selection: Deselect";
                }
                else if ( auto added = next.Add( mesh, hover.Id ); !added.IsSuccess() )
                {
                    // PickElement returned it from this very mesh; a refusal here is a defect worth seeing.
                    LOG_ERROR( "[Mesh Selection] {0}", added.GetError() );
                }
                else if ( !pickCentre && io.KeyShift )
                {
                    label = "Mesh Selection: Add";
                }
            }
            state.Commit( std::move( next ), label );
        }

        // Operation hotkeys, with the cursor over the viewport and no field being typed in. The operation
        // replaces the mesh, so this frame paints nothing more: `mesh` is the old one, the selection the new.
        if ( hovered && !::ImGui::IsAnyItemActive() && !::ImGui::GetIO().WantTextInput )
        {
            const ImGuiIO&                     io = ::ImGui::GetIO();
            std::optional<Core::MeshOperation> operation;
            if ( ::ImGui::IsKeyPressed( ImGuiKey_Delete, false ) )
                operation = Core::MeshOperation::Delete;
            else if ( io.KeyAlt && ::ImGui::IsKeyPressed( ImGuiKey_E, false ) )
                operation = Core::MeshOperation::Extrude;
            else if ( io.KeyAlt && ::ImGui::IsKeyPressed( ImGuiKey_I, false ) )
                operation = Core::MeshOperation::Inset;
            else if ( io.KeyAlt && ::ImGui::IsKeyPressed( ImGuiKey_O, false ) )
                operation = Core::MeshOperation::Offset;
            else if ( io.KeyAlt && ::ImGui::IsKeyPressed( ImGuiKey_B, false ) )
                operation = Core::MeshOperation::Bevel;
            else if ( io.KeyAlt && ::ImGui::IsKeyPressed( ImGuiKey_L, false ) )
                operation = Core::MeshOperation::InsertEdgeLoop;
            else if ( io.KeyAlt && ::ImGui::IsKeyPressed( ImGuiKey_K, false ) )
            {
                m_KnifeArmed = true;
                m_KnifeStart.reset();
                LOG_INFO( "[Mesh Selection] Knife: click the two ends of the cut line (Esc cancels)" );
            }
            if ( operation )
            {
                if ( auto applied = Core::ApplyMeshOperation( scene, *operation, Core::ArgsFromModelingState() );
                     !applied.IsSuccess() )
                    LOG_WARN( "{0}", applied.GetError() );
                return;
            }
        }

        const Painter paint{
             *::ImGui::GetWindowDrawList(), mesh, topology, target.World, viewProj, viewportPos, viewportSize };
        DrawSelection( paint, state.Selection(), IM_COL32( 255, 150, 30, 70 ), IM_COL32( 255, 170, 40, 255 ) );
        if ( hover.IsHit() && !pickCentre )
        {
            Geometry::ElementSelection one( state.Mode() );
            if ( one.Add( mesh, hover.Id ).IsSuccess() )
                DrawSelection( paint, one, 0, IM_COL32( 120, 220, 255, 200 ) );
        }
    }
} // namespace Desert::Editor::Tools
