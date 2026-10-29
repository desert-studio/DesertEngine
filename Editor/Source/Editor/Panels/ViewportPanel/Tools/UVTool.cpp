#include "UVTool.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/RenderSystems/Passes/EditorToolPreviewPass.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshSelection.hpp>
#include <Engine/Geometry/DynamicMeshUVOperations.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        const char* ToolName( MS::Tool tool )
        {
            switch ( tool )
            {
                case MS::Tool::AutoUV:
                    return "AutoUV";
                case MS::Tool::UVProject:
                    return "UV Project";
                case MS::Tool::UVUnwrap:
                    return "UV Unwrap";
                case MS::Tool::UVLayout:
                    return "UV Layout";
                case MS::Tool::UVTransform:
                    return "UV Transform";
                default:
                    return "?";
            }
        }

        ECS::StaticMeshComponent* MeshOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
                return nullptr;
            return &ref->get().GetComponent<ECS::StaticMeshComponent>();
        }

        glm::dmat4 WorldOf( ::Desert::Core::Scene& scene, const Common::UUID& id )
        {
            auto ref = scene.FindEntityByID( id );
            return ref ? glm::dmat4( ref->get().GetWorldTransform() ) : glm::dmat4( 1.0 );
        }

        // ---- The checker preview (UE's Checkerboard material mode) ----
        // A triangle is cut along the checker's cell lines in UV space (u = i / density, v = j / density), so each
        // piece lies in one cell and is drawn in that cell's colour: the checker's edges are exact, not the
        // vertex-colour interpolation a per-vertex checker would give.
        struct ClipVertex
        {
            glm::dvec2 UV;
            glm::dvec3 P;
        };
        using ClipPolygon = std::vector<ClipVertex>;

        // One Sutherland-Hodgman step: @p in cut by the line UV[axis] == c into its two sides.
        void SplitPolygon( const ClipPolygon& in, int axis, double c, ClipPolygon& below, ClipPolygon& above )
        {
            below.clear();
            above.clear();
            for ( size_t i = 0; i < in.size(); ++i )
            {
                const ClipVertex& a  = in[i];
                const ClipVertex& b  = in[( i + 1 ) % in.size()];
                const double      da = a.UV[axis] - c;
                const double      db = b.UV[axis] - c;
                if ( da <= 0.0 )
                    below.push_back( a );
                if ( da >= 0.0 )
                    above.push_back( a );
                if ( ( da < 0.0 && db > 0.0 ) || ( da > 0.0 && db < 0.0 ) )
                {
                    const double t = da / ( da - db );
                    ClipVertex   m{ glm::mix( a.UV, b.UV, t ), glm::mix( a.P, b.P, t ) };
                    m.UV[axis] = c;
                    below.push_back( m );
                    above.push_back( m );
                }
            }
        }

        // Cuts @p poly along every cell line of @p axis it crosses; the pieces go to @p out.
        void CutAlong( ClipPolygon poly, int axis, double density, std::vector<ClipPolygon>& out )
        {
            double lo = poly[0].UV[axis], hi = lo;
            for ( const ClipVertex& v : poly )
            {
                lo = std::min( lo, v.UV[axis] );
                hi = std::max( hi, v.UV[axis] );
            }
            ClipPolygon below, above;
            for ( double k = std::floor( lo * density ) + 1.0; k < hi * density; k += 1.0 )
            {
                SplitPolygon( poly, axis, k / density, below, above );
                if ( below.size() >= 3 )
                    out.push_back( below );
                poly.swap( above );
                if ( poly.size() < 3 )
                    return;
            }
            out.push_back( std::move( poly ) );
        }

        // A triangle crossing more cells than this is drawn in one mid tone: its UVs are stretched beyond what a
        // checker can show (thousands of cells inside one triangle read as grey anyway).
        constexpr double kMaxCellsPerTriangle = 4096.0;

        // ---- UV Project's volume, as edges (world space) ----
        void VolumeEdges( const Geometry::UVProjectionVolume& v, Geometry::UVProjectionType type,
                          std::vector<std::pair<glm::dvec3, glm::dvec3>>& out )
        {
            const glm::dvec3 hx = v.AxisX * ( 0.5 * v.Dimensions.x );
            const glm::dvec3 hy = v.AxisY * ( 0.5 * v.Dimensions.y );
            const glm::dvec3 hz = v.AxisZ * ( 0.5 * v.Dimensions.z );
            const glm::dvec3 o  = v.Origin;
            switch ( type )
            {
                case Geometry::UVProjectionType::Plane:
                {
                    // The projection rectangle, and the direction it projects along (Z).
                    const glm::dvec3 c[4] = { o - hx - hy, o + hx - hy, o + hx + hy, o - hx + hy };
                    for ( int i = 0; i < 4; ++i )
                        out.emplace_back( c[i], c[( i + 1 ) % 4] );
                    out.emplace_back( o, o + hz );
                    break;
                }
                case Geometry::UVProjectionType::Box:
                    for ( int i = 0; i < 4; ++i )
                    {
                        const double     sx = ( i & 1 ) ? 1.0 : -1.0, sy = ( i & 2 ) ? 1.0 : -1.0;
                        const glm::dvec3 p = o + sx * hx + sy * hy;
                        out.emplace_back( p - hz, p + hz ); // along Z
                        out.emplace_back( o + sx * hx - hy + ( sy * hz ),
                                          o + sx * hx + hy + ( sy * hz ) ); // along Y
                        out.emplace_back( o - hx + sx * hy + ( sy * hz ),
                                          o + hx + sx * hy + ( sy * hz ) ); // along X
                    }
                    break;
                case Geometry::UVProjectionType::Cylinder:
                {
                    // Its two caps (the X / Y extents are the diameters) and four lines along the axis.
                    constexpr int kSegments = 48;
                    const auto    rim       = [&]( int i, double side )
                    {
                        const double a = 2.0 * std::numbers::pi * i / kSegments;
                        return o + std::cos( a ) * hx + std::sin( a ) * hy + side * hz;
                    };
                    for ( int i = 0; i < kSegments; ++i )
                        for ( const double side : { -1.0, 1.0 } )
                            out.emplace_back( rim( i, side ), rim( i + 1, side ) );
                    for ( int i = 0; i < kSegments; i += kSegments / 4 )
                        out.emplace_back( rim( i, -1.0 ), rim( i, 1.0 ) );
                    break;
                }
            }
        }
    } // namespace

    bool UVTool::IsUVTool( MS::Tool tool )
    {
        return tool == MS::Tool::AutoUV || tool == MS::Tool::UVProject || tool == MS::Tool::UVUnwrap ||
               tool == MS::Tool::UVLayout || tool == MS::Tool::UVTransform;
    }

    void UVTool::Update( ::Desert::Core::Scene& scene, const glm::dvec3& eyeWorld )
    {
        MS&        ms       = MS::Get();
        const bool uvActive = IsUVTool( ms.ActiveTool );

        // The session ends without Accept when its tool is no longer the active one (the bar's Cancel ends the
        // tool and raises ReqCancel, which was this tool's) or the selection moved to another entity.
        if ( m_Session )
        {
            const auto selected = Core::SelectionManager::GetSelected();
            if ( ms.ActiveTool != m_Session->Tool || !selected.has_value() || *selected != m_Session->Entity ||
                 MeshOf( scene, m_Session->Entity ) == nullptr )
            {
                if ( ms.ActiveTool != m_Session->Tool && ms.ReqCancel )
                    ms.ReqCancel = false;
                Restore( scene );
                m_Session.reset();
            }
        }
        if ( !uvActive )
            return;

        if ( !m_Session )
            Begin( scene, ms );
        if ( m_Session && ( SettingsChanged( ms ) || TransformChanged( scene ) ) )
            Recompute( scene, ms );
        if ( m_Session )
            DrawPreview( scene, eyeWorld );

        if ( ms.ReqAccept )
        {
            ms.ReqAccept         = false;
            const bool endTool   = ms.ReqAcceptEndsTool;
            ms.ReqAcceptEndsTool = false;
            if ( !Accept( scene, ms ) )
                return; // refused: the tool stays open with its preview, and says why
            if ( endTool )
                ms.ActiveTool = MS::Tool::None;
            else
                Begin( scene, ms ); // "Accept and Start New": the committed mesh is the next input
        }
    }

    void UVTool::Begin( ::Desert::Core::Scene& scene, MS& ms )
    {
        m_Session.reset();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
        {
            ms.UVStatus = "Select an entity with a static mesh.";
            return;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, *selected );
        if ( component == nullptr )
        {
            ms.UVStatus = "The selected entity has no static mesh.";
            return;
        }
        auto target = GetToolTargetMesh( *component );
        if ( !target.IsSuccess() )
        {
            ms.UVStatus = target.GetError();
            return;
        }
        m_Session =
             Session{ ms.ActiveTool, *selected, target.GetValue().Mesh, target.GetValue().Committed, nullptr };
        // UV Transform works on the selected triangles of this mesh (UE's UV Editor: the selection), or on all.
        if ( ms.ActiveTool == MS::Tool::UVTransform )
        {
            const auto& selection = Core::MeshElementSelection::Get();
            if ( selection.Entity() == *selected && selection.HasMesh() && !selection.Selection().Empty() )
            {
                const Geometry::ElementSelection tris =
                     Geometry::ConvertSelection( *selection.Mesh(), *selection.Topology(), selection.Selection(),
                                                 Geometry::ElementMode::Triangle );
                for ( const int tid : tris.Ids() )
                    m_Session->Scope.push_back( tid );
            }
        }
        Recompute( scene, ms );
    }

    void UVTool::Recompute( ::Desert::Core::Scene& scene, MS& ms )
    {
        RememberSettings( ms );
        const Geometry::DynamicMesh3&          input = *m_Session->Input;
        Common::ResultStr<Geometry::UVOutcome> out   = Common::MakeError<Geometry::UVOutcome>( "no UV tool" );
        switch ( m_Session->Tool )
        {
            case MS::Tool::AutoUV:
                out = Geometry::AutoUV( input, ms.UVAuto );
                break;
            case MS::Tool::UVProject:
                m_Session->MeshToWorld = WorldOf( scene, m_Session->Entity );
                out = Geometry::ProjectUVs( input, m_Session->MeshToWorld, ms.UVProject );
                break;
            case MS::Tool::UVUnwrap:
                out = Geometry::UnwrapUVs( input, ms.UVUnwrap );
                break;
            case MS::Tool::UVLayout:
                out = Geometry::LayoutUVs( input, ms.UVLayout );
                break;
            case MS::Tool::UVTransform:
                out = Geometry::TransformUVs( input, ms.UVTransform, m_Session->Scope );
                break;
            default:
                break;
        }
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( !out.IsSuccess() )
        {
            // A refused setting shows the input, not a stale preview.
            ms.UVStatus = out.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            m_Session->Volume.reset();
            return;
        }
        auto preview = std::make_shared<const Geometry::DynamicMesh3>( std::move( out.ExtractValue().Mesh ) );
        if ( auto set = ECS::SetEditableMesh( *component, preview ); !set.IsSuccess() )
        {
            ms.UVStatus = set.GetError();
            Restore( scene );
            m_Session->Preview.reset();
            return;
        }
        m_Session->Preview = std::move( preview );
        ms.UVStatus        = out.GetValue().Report;
        m_Session->Volume.reset();
        if ( m_Session->Tool == MS::Tool::UVProject )
            if ( auto volume = Geometry::ResolveUVProjectionVolume( input, WorldOf( scene, m_Session->Entity ),
                                                                    ms.UVProject );
                 volume.IsSuccess() )
                m_Session->Volume = volume.GetValue();
    }

    void UVTool::Restore( ::Desert::Core::Scene& scene )
    {
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr )
            return;
        switch ( PlanMeshRestore( component->GetEditableMesh(), m_Session->Committed ) )
        {
            case MeshRestore::Unchanged:
                break;
            case MeshRestore::Clear:
                ECS::ClearEditableMesh( *component );
                break;
            case MeshRestore::Set:
                if ( auto set = ECS::SetEditableMesh( *component, m_Session->Committed ); !set.IsSuccess() )
                    LOG_ERROR( "[Modeling] {0}: the mesh could not be put back: {1}", ToolName( m_Session->Tool ),
                               set.GetError() );
                break;
        }
    }

    bool UVTool::Accept( ::Desert::Core::Scene& scene, MS& ms )
    {
        const auto refuse = [&]( const std::string& why )
        {
            LOG_ERROR( "[Modeling] {0} Accept refused: {1}", ToolName( ms.ActiveTool ), why );
            ToastManager::Push( std::format( "{} Accept refused: {}", ToolName( ms.ActiveTool ), why ),
                                ToastLevel::Error, 10.0f );
            return false;
        };
        if ( !m_Session )
            return refuse( ms.UVStatus.empty() ? std::string( "no mesh is selected" ) : ms.UVStatus );
        if ( !m_Session->Preview )
            return refuse( ms.UVStatus );
        ECS::StaticMeshComponent* component = MeshOf( scene, m_Session->Entity );
        if ( component == nullptr || component->GetEditableMesh() != m_Session->Preview )
            return refuse( "the entity no longer draws the preview" );
        Commands::RecordMeshChange( m_Session->Entity, std::format( "Modeling: {}", ToolName( m_Session->Tool ) ),
                                    m_Session->Committed );
        LOG_INFO( "[Modeling] {0} accepted: {1}", ToolName( m_Session->Tool ), ms.UVStatus );
        m_Session.reset();
        return true;
    }

    void UVTool::DrawPreview( ::Desert::Core::Scene& scene, const glm::dvec3& eyeWorld ) const
    {
        using Vertex       = Graphic::MaterialDebugLine::LineVertex;
        const MS&  ms      = MS::Get();
        const auto world   = WorldOf( scene, m_Session->Entity );
        const auto normal  = glm::transpose( glm::inverse( glm::dmat3( world ) ) );
        const auto toWorld = [&]( const glm::dvec3& p ) { return glm::dvec3( world * glm::dvec4( p, 1.0 ) ); };
        Render::ToolPreviewMesh preview;

        const Geometry::DynamicMesh3* mesh = m_Session->Preview.get();
        const auto* uvs = mesh != nullptr && mesh->HasAttributes() && mesh->Attributes()->NumUVLayers() > 0
                               ? mesh->Attributes()->PrimaryUV()
                               : nullptr;
        if ( ms.UVShowChecker && uvs != nullptr )
        {
            const double density = static_cast<double>( ms.UVCheckerDensity );
            struct Piece
            {
                ClipPolygon Polygon;
                glm::vec4   Colour;
                double      Depth;
            };
            std::vector<Piece>       pieces;
            std::vector<ClipPolygon> strips, cells;
            for ( int t = 0; t < mesh->MaxTriangleID(); ++t )
            {
                if ( !mesh->IsTriangle( t ) || !uvs->IsSetTriangle( t ) )
                    continue;
                const glm::dvec3 n = normal * mesh->GetTriNormal( t );
                const glm::dvec3 c = toWorld( mesh->GetTriCentroid( t ) );
                if ( glm::dot( n, c - eyeWorld ) >= 0.0 )
                    continue; // back-facing: hidden behind the front of the mesh
                // Headlight shading, so the faces of a box read apart.
                const double nl = glm::length( n );
                const double facing =
                     nl > 0.0 ? std::abs( glm::dot( n / nl, glm::normalize( eyeWorld - c ) ) ) : 1.0;
                const float shade = static_cast<float>( 0.45 + 0.55 * facing );

                const auto tri = mesh->GetTriangle( t );
                glm::vec2  a, b, d;
                uvs->GetTriElements( t, a, b, d );
                ClipPolygon      poly{ { glm::dvec2( a ), toWorld( mesh->GetVertex( tri.A ) ) },
                                       { glm::dvec2( b ), toWorld( mesh->GetVertex( tri.B ) ) },
                                       { glm::dvec2( d ), toWorld( mesh->GetVertex( tri.C ) ) } };
                const glm::dvec2 lo    = glm::min( glm::min( poly[0].UV, poly[1].UV ), poly[2].UV );
                const glm::dvec2 hi    = glm::max( glm::max( poly[0].UV, poly[1].UV ), poly[2].UV );
                const glm::dvec2 span  = ( hi - lo ) * density + 1.0;
                const double     depth = glm::length( c - eyeWorld );
                if ( !( span.x * span.y <= kMaxCellsPerTriangle ) )
                {
                    pieces.push_back(
                         { std::move( poly ), glm::vec4( glm::vec3( 0.55f * shade ), 1.0f ), depth } );
                    continue;
                }
                strips.clear();
                CutAlong( std::move( poly ), 0, density, strips );
                for ( ClipPolygon& strip : strips )
                {
                    cells.clear();
                    CutAlong( std::move( strip ), 1, density, cells );
                    for ( ClipPolygon& cell : cells )
                    {
                        glm::dvec2 centre( 0.0 );
                        for ( const ClipVertex& v : cell )
                            centre += v.UV;
                        centre /= static_cast<double>( cell.size() );
                        const auto  parity = static_cast<long long>( std::floor( centre.x * density ) +
                                                                     std::floor( centre.y * density ) );
                        const float tone   = ( parity & 1 ) != 0 ? 0.18f : 0.82f;
                        pieces.push_back(
                             { std::move( cell ), glm::vec4( glm::vec3( tone * shade ), 1.0f ), depth } );
                    }
                }
            }
            // Far to near: the faces blend in the order a depth write would have kept.
            std::stable_sort( pieces.begin(), pieces.end(),
                              []( const Piece& x, const Piece& y ) { return x.Depth > y.Depth; } );
            for ( const Piece& piece : pieces )
                for ( size_t i = 1; i + 1 < piece.Polygon.size(); ++i )
                    for ( const size_t k : { size_t( 0 ), i, i + 1 } )
                        preview.Triangles.push_back(
                             Vertex{ glm::vec4( glm::vec3( piece.Polygon[k].P ), 1.0f ), piece.Colour } );
        }

        if ( m_Session->Volume )
        {
            std::vector<std::pair<glm::dvec3, glm::dvec3>> edges;
            VolumeEdges( *m_Session->Volume, m_Project.Type, edges );
            const glm::vec4 colour( 1.0f, 0.55f, 0.1f, 1.0f );
            for ( const auto& [from, to] : edges )
            {
                const glm::dvec3 p = from, q = to; // the volume is world space
                preview.Edges.push_back( Vertex{ glm::vec4( glm::vec3( p ), 1.0f ), colour } );
                preview.Edges.push_back( Vertex{ glm::vec4( glm::vec3( q ), 1.0f ), colour } );
                // No surface under a volume edge: the one facing the eye (the pass's smallest lift).
                const glm::dvec3 toEye = eyeWorld - ( p + q ) * 0.5;
                const double     len   = glm::length( toEye );
                preview.EdgeSurfaces.push_back( len > 0.0 ? glm::vec3( toEye / len ) : glm::vec3( 0, 1, 0 ) );
            }
        }
        if ( preview.Triangles.empty() && preview.Edges.empty() )
            Render::ToolPreview::Hide( scene );
        else
            Render::ToolPreview::Show( scene, std::move( preview ) );
    }

    std::optional<glm::mat4> UVTool::ProjectionGizmo( ::Desert::Core::Scene& ) const
    {
        if ( !m_Session || !m_Session->Volume )
            return std::nullopt;
        // World space and orthonormal (ResolveUVProjectionVolume), so no entity scale shears it; the column
        // lengths are the Dimensions, which the gizmo's scale handles change (UE's projection TransformProxy).
        const Geometry::UVProjectionVolume& v    = *m_Session->Volume;
        const auto                          axis = [&]( const glm::dvec3& a, double length )
        { return glm::vec4( glm::vec3( a * length ), 0.0f ); };
        return glm::mat4( axis( v.AxisX, v.Dimensions.x ), axis( v.AxisY, v.Dimensions.y ),
                          axis( v.AxisZ, v.Dimensions.z ), glm::vec4( glm::vec3( v.Origin ), 1.0f ) );
    }

    void UVTool::ApplyProjectionGizmo( ::Desert::Core::Scene&, const glm::mat4& frame )
    {
        if ( !m_Session || !m_Session->Volume )
            return;
        MS&                                 ms = MS::Get();
        const Geometry::UVProjectionVolume& v  = *m_Session->Volume;
        // The bounds centre the volume was placed from: its origin less the offset it was computed with.
        const glm::dvec3 centre = v.Origin - glm::dvec3( m_Project.CenterOffset );
        glm::dvec3       dims;
        glm::dmat3       axes;
        for ( int c = 0; c < 3; ++c )
        {
            const glm::dvec3 column( frame[c] );
            dims[c] = glm::length( column );
            if ( !( dims[c] > 1e-6 ) )
                return; // a collapsed axis: keep the last volume
            axes[c] = column / dims[c];
        }
        // Gram-Schmidt: float round-off of the gizmo's matrix must not skew the rotation read back.
        axes[0]                   = glm::normalize( axes[0] );
        axes[1]                   = glm::normalize( axes[1] - axes[0] * glm::dot( axes[0], axes[1] ) );
        axes[2]                   = glm::cross( axes[0], axes[1] );
        ms.UVProject.CenterOffset = glm::vec3( glm::dvec3( frame[3] ) - centre );
        ms.UVProject.RotationDeg  = Geometry::UVProjectionRotationDeg( axes );
        // A scale drag sets the Dimensions; fitted ones stop following the bounds (UE: the gizmo's scale is
        // the projection's size, whatever initialised it).
        if ( glm::any( glm::greaterThan( glm::abs( dims - v.Dimensions ), glm::dvec3( 1e-3 ) ) ) )
        {
            ms.UVProject.FitToBounds = false;
            ms.UVProject.Dimensions  = glm::vec3( dims );
        }
    }

    bool UVTool::SettingsChanged( const MS& ms ) const
    {
        switch ( m_Session->Tool )
        {
            case MS::Tool::AutoUV:
                return !( ms.UVAuto == m_AutoUV );
            case MS::Tool::UVProject:
                return !( ms.UVProject == m_Project );
            case MS::Tool::UVUnwrap:
                return !( ms.UVUnwrap == m_Unwrap );
            case MS::Tool::UVLayout:
                return !( ms.UVLayout == m_Layout );
            case MS::Tool::UVTransform:
                return !( ms.UVTransform == m_Transform );
            default:
                return false;
        }
    }

    bool UVTool::TransformChanged( ::Desert::Core::Scene& scene ) const
    {
        return m_Session->Tool == MS::Tool::UVProject && WorldOf( scene, m_Session->Entity ) != m_Session->MeshToWorld;
    }

    void UVTool::RememberSettings( const MS& ms )
    {
        m_AutoUV  = ms.UVAuto;
        m_Project = ms.UVProject;
        m_Unwrap  = ms.UVUnwrap;
        m_Layout  = ms.UVLayout;
        m_Transform = ms.UVTransform;
    }
} // namespace Desert::Editor::Tools
