#include "CreateShapeTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/RenderSystems/Passes/EditorToolPreviewPass.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        // The shortest turn taking the shape's up axis (+Y) onto `normal` (UE's FFrame3d built from the hit
        // normal); a normal straight down turns it over about X.
        glm::quat UpOnto( const glm::vec3& normal )
        {
            const glm::vec3 up( 0.0f, 1.0f, 0.0f );
            const glm::vec3 n = glm::normalize( normal );
            const float     d = glm::dot( up, n );
            if ( d > 0.99999f )
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            if ( d < -0.99999f )
                return glm::angleAxis( glm::pi<float>(), glm::vec3( 1.0f, 0.0f, 0.0f ) );
            return glm::angleAxis( std::acos( std::clamp( d, -1.0f, 1.0f ) ),
                                   glm::normalize( glm::cross( up, n ) ) );
        }
    } // namespace

    Geometry::ShapeMesh CreateShapeTool::Build( const MS::ShapeSettings& s )
    {
        const Geometry::ShapeOptions options{ s.Groups, s.Pivot };
        switch ( s.Kind )
        {
            case MS::Shape::Box:
                return Geometry::MakeBox(
                     { s.Box.Width, s.Box.Height, s.Box.Depth },
                     { s.Box.WidthSubdivisions, s.Box.HeightSubdivisions, s.Box.DepthSubdivisions }, options );
            case MS::Shape::Sphere:
                return Geometry::MakeSphere( s.Sphere, options );
            case MS::Shape::Cylinder:
                return Geometry::MakeCylinder( s.Cylinder, options );
            case MS::Shape::Cone:
                return Geometry::MakeCone( s.Cone, options );
            case MS::Shape::Capsule:
                return Geometry::MakeCapsule( s.Capsule, options );
            case MS::Shape::Pyramid:
                return Geometry::MakePyramid( { s.Pyramid.Width, s.Pyramid.Height, s.Pyramid.Depth }, options );
            case MS::Shape::Stairs:
                return Geometry::MakeStairs( s.Stairs, options );
            case MS::Shape::Torus:
                return Geometry::MakeTorus( s.Torus, options );
            case MS::Shape::Arrow:
                return Geometry::MakeArrow( s.Arrow, options );
            case MS::Shape::Disc:
                return Geometry::MakeDisc( s.Disc, options );
            case MS::Shape::Rectangle:
                return Geometry::MakeRectangle( s.Rectangle, options );
        }
        return Geometry::MakeBox( { s.Box.Width, s.Box.Height, s.Box.Depth }, glm::ivec3( 1 ), options );
    }

    std::optional<CreateShapeTool::Spot> CreateShapeTool::PlacementSpot( const ::Desert::Core::Scene& scene,
                                                                         const Common::Math::Ray&     ray,
                                                                         const MS::ShapeSettings&     settings )
    {
        if ( settings.Place == MS::Placement::OnScene )
        {
            // UE ToolSceneQueriesUtil::FindNearestVisibleObjectHit traces COMPLEX: the shape lands on the
            // triangle under the cursor and Align to Normal takes that triangle's normal.
            ::Desert::Core::RaycastHit hit;
            if ( scene.RaycastComplex( ray, hit ) && hit.Distance > 0.0f )
                return Spot{ hit.Point,
                             settings.AlignToNormal ? UpOnto( hit.Normal ) : glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) };
        }
        // The ground plane: only in front of the camera, and never along a ray parallel to it.
        if ( std::abs( ray.Direction.y ) < 1e-6f )
            return std::nullopt;
        const float t = -ray.Origin.y / ray.Direction.y;
        if ( t <= 0.0f )
            return std::nullopt;
        return Spot{ ray.Origin + ray.Direction * t, glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) };
    }

    Common::ResultStr<Common::UUID> CreateShapeTool::Place( ::Desert::Core::Scene&    scene,
                                                            const MS::ShapeSettings&  settings,
                                                            const MS::OutputSettings& output, const Spot& spot )
    {
        auto mesh = Geometry::ShapeToEditMesh( Build( settings ) );
        if ( !mesh.IsSuccess() )
            return Common::MakeFormattedError<Common::UUID>( "the {} was not placed: {}",
                                                             MS::ShapeName( settings.Kind ), mesh.GetError() );

        const ECS::Entity entity = scene.CreateNewEntity( MS::ShapeName( settings.Kind ) );
        auto&             transform = entity.GetComponent<ECS::TransformComponent>();
        transform.Translation       = spot.Point;
        transform.Rotation          = glm::eulerAngles( spot.Rotation );
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

    Common::ResultStr<Common::UUID> CreateShapeTool::PlaceAlong( ::Desert::Core::Scene&   scene,
                                                                 const Common::Math::Ray& ray )
    {
        const auto& ms = MS::Get();
        if ( ms.ActiveTool != MS::Tool::CreateShape )
            return Common::MakeError<Common::UUID>( "the Create Shape tool is not active" );
        const auto spot = PlacementSpot( scene, ray, ms.CreateShape );
        if ( !spot )
            return Common::MakeFormattedError<Common::UUID>(
                 "the {} was not placed: the viewport centre meets neither the scene nor the ground",
                 MS::ShapeName( ms.CreateShape.Kind ) );
        return Place( scene, ms.CreateShape, ms.Output, *spot );
    }

    void CreateShapeTool::Rebuild( const MS::ShapeSettings& settings )
    {
        m_Preview  = Build( settings );
        m_Built    = settings;
        m_HasBuilt = true;
        m_Edges.clear();

        // Edges are keyed by POSITION, not vertex index: the generators split vertices along hard edges
        // (a box face owns its four corners), so two faces meet at an edge whose index pairs differ.
        using Key      = std::array<int64_t, 3>;
        const auto key = []( const glm::vec3& p ) -> Key {
            return { std::llround( p.x * 1000.0f ), std::llround( p.y * 1000.0f ), std::llround( p.z * 1000.0f ) };
        };
        std::map<std::pair<Key, Key>, std::vector<uint32_t>> shared;
        for ( uint32_t t = 0; t < m_Preview.Indices.size(); ++t )
        {
            const auto&                   tri = m_Preview.Indices[t];
            const std::array<uint32_t, 3> v   = { tri.V1, tri.V2, tri.V3 };
            for ( int e = 0; e < 3; ++e )
            {
                Key a = key( m_Preview.Vertices[v[e]].Position );
                Key b = key( m_Preview.Vertices[v[( e + 1 ) % 3]].Position );
                if ( b < a )
                    std::swap( a, b );
                shared[{ a, b }].push_back( t );
            }
        }
        for ( const auto& [edge, tris] : shared )
        {
            const auto&     tri  = m_Preview.Indices[tris.front()];
            const glm::vec3 p0   = m_Preview.Vertices[tri.V1].Position;
            const glm::vec3 p1   = m_Preview.Vertices[tri.V2].Position;
            const glm::vec3 p2   = m_Preview.Vertices[tri.V3].Position;
            const auto      find = [&]( const Key& k ) { return key( p0 ) == k ? p0 : key( p1 ) == k ? p1 : p2; };
            const bool      border = tris.size() != 2;
            const bool      grouped =
                 !border && !m_Preview.Groups.empty() && m_Preview.Groups[tris[0]] != m_Preview.Groups[tris[1]];
            m_Edges.push_back( { find( edge.first ), find( edge.second ), tris.front(),
                                 border ? tris.front() : tris[1], border || grouped } );
        }
    }

    void CreateShapeTool::ShowPreview( const ::Desert::Core::Scene& scene, const Spot& spot,
                                       const glm::vec3& eye ) const
    {
        using Vertex         = Graphic::MaterialDebugLine::LineVertex;
        const glm::mat3 turn = glm::mat3_cast( spot.Rotation );
        const auto      at   = [&]( const glm::vec3& local ) { return spot.Point + turn * local; };

        // An open shape (Disc, Rectangle) is seen from both sides; a closed one shows its near side only.
        const bool closed = std::none_of( m_Edges.begin(), m_Edges.end(),
                                          []( const FeatureEdge& e ) { return e.TriA == e.TriB; } );
        struct Face
        {
            std::array<glm::vec3, 3> Corners;
            float                    Depth;
            float                    Light;
        };
        std::vector<Face> faces;
        std::vector<char> front( m_Preview.Indices.size(), 0 );
        faces.reserve( m_Preview.Indices.size() );
        for ( size_t t = 0; t < m_Preview.Indices.size(); ++t )
        {
            const auto&                   tri = m_Preview.Indices[t];
            const std::array<uint32_t, 3> v   = { tri.V1, tri.V2, tri.V3 };
            Face                          face{};
            glm::vec3                     centre( 0.0f );
            glm::vec3                     normal( 0.0f );
            for ( int k = 0; k < 3; ++k )
            {
                face.Corners[k] = at( m_Preview.Vertices[v[k]].Position );
                centre += face.Corners[k] / 3.0f;
                normal += turn * m_Preview.Vertices[v[k]].Normal;
            }
            const glm::vec3 toEye  = eye - centre;
            const float     facing = glm::dot( normal, toEye );
            front[t]               = facing > 0.0f ? 1 : 0;
            if ( closed && facing <= 0.0f )
                continue;
            const float len = glm::length( normal ) * glm::length( toEye );
            face.Light      = len > 0.0f ? std::abs( facing ) / len : 1.0f;
            face.Depth      = glm::length( toEye );
            faces.push_back( face );
        }
        // Far to near: the translucent faces blend in the order a depth write would have kept.
        std::sort( faces.begin(), faces.end(), []( const Face& a, const Face& b ) { return a.Depth > b.Depth; } );

        Render::ToolPreviewMesh mesh;
        mesh.Triangles.reserve( faces.size() * 3 );
        for ( const Face& face : faces )
        {
            // Lit from the eye, as the overlay was: a face square to the view is the brightest.
            const float     l = 0.35f + 0.65f * face.Light;
            const glm::vec4 colour( 1.0f * l, 0.78f * l, 0.24f * l, 0.55f );
            for ( const glm::vec3& corner : face.Corners )
                mesh.Triangles.push_back( Vertex{ glm::vec4( corner, 1.0f ), colour } );
        }
        // The form's edges: polygroup and open borders on the near side, and the silhouette.
        const glm::vec4 edgeColour( 1.0f, 0.86f, 0.47f, 1.0f );
        for ( const FeatureEdge& edge : m_Edges )
        {
            const bool a = front[edge.TriA] != 0;
            const bool b = front[edge.TriB] != 0;
            if ( closed ? !( a || b ) || !( edge.Feature || a != b ) : !edge.Feature )
                continue;
            mesh.Edges.push_back( Vertex{ glm::vec4( at( edge.A ), 1.0f ), edgeColour } );
            mesh.Edges.push_back( Vertex{ glm::vec4( at( edge.B ), 1.0f ), edgeColour } );
        }
        Render::ToolPreview::Show( scene, std::move( mesh ) );
    }

    void CreateShapeTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                  const Common::Math::Ray& centreRay, const glm::vec2& viewportPos,
                                  const glm::vec2& viewportSize, bool interactive )
    {
        const auto& ms = MS::Get();
        m_Cursor       = ToolCursor::None;
        if ( ms.ActiveTool != MS::Tool::CreateShape )
        {
            Render::ToolPreview::Hide( scene );
            return;
        }
        const MS::ShapeSettings& settings = ms.CreateShape;

        const ImVec2 mouse   = ::ImGui::GetMousePos();
        const bool   hovered = interactive && mouse.x >= viewportPos.x && mouse.y >= viewportPos.y &&
                             mouse.x < viewportPos.x + viewportSize.x && mouse.y < viewportPos.y + viewportSize.y;
        // Under the cursor while it is over the viewport; else where the viewport centre looks.
        const auto spot = PlacementSpot( scene, hovered ? ray : centreRay, settings );
        m_Cursor        = spot ? ToolCursor::Place : ToolCursor::Unavailable;
        if ( !spot )
        {
            Render::ToolPreview::Hide( scene );
            return;
        }

        if ( !m_HasBuilt || !( m_Built == settings ) )
            Rebuild( settings );
        ShowPreview( scene, *spot, ray.Origin );

        // Alt + LMB is the camera's orbit, not a placement.
        const ImGuiIO& io = ::ImGui::GetIO();
        if ( hovered && !::ImGui::IsAnyItemActive() && !io.KeyAlt &&
             ::ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            if ( auto placed = Place( scene, settings, ms.Output, *spot ); !placed.IsSuccess() )
                LOG_ERROR( "[CreateShape] {0}", placed.GetError() );
        }
    }
} // namespace Desert::Editor::Tools
