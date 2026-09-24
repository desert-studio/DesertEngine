#include "EditMeshConversion.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <map>
#include <string>

namespace Desert::Geometry
{
    namespace
    {
        using Common::MakeFormattedError;

        template <typename V>
        bool Within( const V& a, const V& b, float tolerance )
        {
            for ( int i = 0; i < a.length(); ++i )
                if ( std::abs( a[i] - b[i] ) > tolerance )
                    return false;
            return true;
        }

        // Positions welded within a tolerance through a uniform grid of cells one tolerance wide: a point can
        // only match inside its own cell or the 26 around it.
        class PositionWelder
        {
        public:
            explicit PositionWelder( float tolerance ) : m_Tolerance( tolerance )
            {
            }
            [[nodiscard]] int Find( const EditMesh& mesh, const glm::vec3& p ) const
            {
                const auto base = Cell( p );
                for ( int dx = -1; dx <= 1; ++dx )
                    for ( int dy = -1; dy <= 1; ++dy )
                        for ( int dz = -1; dz <= 1; ++dz )
                        {
                            const auto it = m_Cells.find( { base[0] + dx, base[1] + dy, base[2] + dz } );
                            if ( it == m_Cells.end() )
                                continue;
                            for ( const int v : it->second )
                                if ( Within( mesh.GetPosition( v ), p, m_Tolerance ) )
                                    return v;
                        }
                return InvalidId;
            }
            void Add( const glm::vec3& p, int v )
            {
                m_Cells[Cell( p )].push_back( v );
            }

        private:
            [[nodiscard]] std::array<int64_t, 3> Cell( const glm::vec3& p ) const
            {
                // A zero tolerance still needs a finite cell; 1 cm cells then hold exact matches only.
                const float size = m_Tolerance > 0.0f ? m_Tolerance : 1.0f;
                return { static_cast<int64_t>( std::floor( p.x / size ) ),
                         static_cast<int64_t>( std::floor( p.y / size ) ),
                         static_cast<int64_t>( std::floor( p.z / size ) ) };
            }
            float                                              m_Tolerance;
            std::map<std::array<int64_t, 3>, std::vector<int>> m_Cells;
        };

        template <typename Overlay>
        Common::BoolResultStr RequireSet( const EditMesh& mesh, const Overlay& overlay, const char* name )
        {
            for ( const int t : mesh.TriangleIds() )
                if ( !overlay.IsSetTriangle( t ) )
                    return MakeFormattedError<bool>( "ToRenderMesh: triangle {} is unset in the {} layer", t,
                                                     name );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<RenderMeshData> ToRenderMesh( const EditMesh& mesh, int uvLayer )
    {
        const EditMeshAttributes& attributes = mesh.Attributes();
        const NormalOverlay*      normals    = attributes.Normals();
        const TangentOverlay*     tangents   = attributes.Tangents();
        const UVOverlay*          uvs        = attributes.UV( uvLayer );
        if ( normals == nullptr )
            return MakeFormattedError<RenderMeshData>( "ToRenderMesh: the normal layer is disabled" );
        if ( attributes.UVLayerCount() > 0 && uvs == nullptr )
            return MakeFormattedError<RenderMeshData>( "ToRenderMesh: UV layer {} requested, the mesh has {}",
                                                       uvLayer, attributes.UVLayerCount() );
        if ( auto r = RequireSet( mesh, *normals, "normal" ); !r.IsSuccess() )
            return Common::MakeError<RenderMeshData>( r.GetError() );
        if ( tangents != nullptr )
            if ( auto r = RequireSet( mesh, *tangents, "tangent" ); !r.IsSuccess() )
                return Common::MakeError<RenderMeshData>( r.GetError() );
        if ( uvs != nullptr )
            if ( auto r = RequireSet( mesh, *uvs, "UV" ); !r.IsSuccess() )
                return Common::MakeError<RenderMeshData>( r.GetError() );

        // Triangles grouped by material, ascending triangle ID inside each group.
        std::map<int, std::vector<int>> byMaterial;
        for ( const int t : mesh.TriangleIds() )
            byMaterial[attributes.GetMaterialId( t )].push_back( t );

        RenderMeshData out;
        for ( const auto& [material, triangles] : byMaterial )
        {
            Submesh submesh{};
            submesh.Name         = "MaterialID " + std::to_string( material );
            submesh.VertexOffset = static_cast<uint32_t>( out.Vertices.size() );
            submesh.IndexOffset  = static_cast<uint32_t>( out.Indices.size() * 3 ); // uint32 units
            submesh.Transform    = glm::mat4( 1.0f );

            // (vertex, normal element, tangent element, uv element) -> submesh-local render vertex.
            std::map<std::array<int, 4>, uint32_t> corners;
            for ( const int t : triangles )
            {
                const auto& tri = mesh.GetTriangle( t );
                Index       index{};
                uint32_t*   slots[3] = { &index.V1, &index.V2, &index.V3 };
                for ( int j = 0; j < 3; ++j )
                {
                    const int                v  = tri[j];
                    const int                en = normals->GetTriangle( t )[j];
                    const int et = ( tangents != nullptr ) ? tangents->GetTriangle( t )[j] : InvalidId;
                    const int eu = ( uvs != nullptr ) ? uvs->GetTriangle( t )[j] : InvalidId;
                    const std::array<int, 4> key{ v, en, et, eu };
                    auto                     found = corners.find( key );
                    if ( found == corners.end() )
                    {
                        Vertex vertex{};
                        vertex.Position = mesh.GetPosition( v );
                        vertex.Normal   = normals->GetElement( en );
                        if ( tangents != nullptr )
                        {
                            const glm::vec4 tangent = tangents->GetElement( et );
                            vertex.Tangent          = glm::vec3( tangent );
                            vertex.Bitangent        = glm::cross( vertex.Normal, vertex.Tangent ) * tangent.w;
                        }
                        if ( uvs != nullptr )
                            vertex.TexCoord = uvs->GetElement( eu );
                        const auto local = static_cast<uint32_t>( out.Vertices.size() - submesh.VertexOffset );
                        out.Vertices.push_back( vertex );
                        out.SourceVertices.push_back( v );
                        found = corners.emplace( key, local ).first;
                    }
                    *slots[j] = found->second;
                }
                out.Indices.push_back( index );
                out.SourceTriangles.push_back( t );
            }

            submesh.VertexCount = static_cast<uint32_t>( out.Vertices.size() ) - submesh.VertexOffset;
            submesh.IndexCount  = static_cast<uint32_t>( out.Indices.size() * 3 ) - submesh.IndexOffset;
            glm::vec3 lo( 0.0f );
            glm::vec3 hi( 0.0f );
            for ( uint32_t i = 0; i < submesh.VertexCount; ++i )
            {
                const glm::vec3& p = out.Vertices[submesh.VertexOffset + i].Position;
                lo                 = i == 0 ? p : glm::min( lo, p );
                hi                 = i == 0 ? p : glm::max( hi, p );
            }
            submesh.BoundingBox.Min = lo;
            submesh.BoundingBox.Max = hi;
            out.Submeshes.push_back( std::move( submesh ) );
            out.SubmeshMaterialIds.push_back( material );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<ImportedEditMesh> FromRenderMesh( const RenderMeshData& render, const WeldOptions& options )
    {
        struct Range
        {
            uint32_t VertexOffset, VertexCount, FirstTriangle, TriangleCount;
            int      Material;
        };
        std::vector<Range> ranges;
        if ( render.Submeshes.empty() )
            ranges.push_back( { 0, static_cast<uint32_t>( render.Vertices.size() ), 0,
                                static_cast<uint32_t>( render.Indices.size() ), 0 } );
        for ( size_t i = 0; i < render.Submeshes.size(); ++i )
        {
            const Submesh& s = render.Submeshes[i];
            if ( s.IndexOffset % 3 != 0 || s.IndexCount % 3 != 0 )
                return MakeFormattedError<ImportedEditMesh>(
                     "FromRenderMesh: submesh {} index range ({}, {}) is not whole triangles", i, s.IndexOffset,
                     s.IndexCount );
            const Range r{ s.VertexOffset, s.VertexCount, s.IndexOffset / 3, s.IndexCount / 3,
                           i < render.SubmeshMaterialIds.size() ? render.SubmeshMaterialIds[i]
                                                                : static_cast<int>( i ) };
            if ( static_cast<uint64_t>( r.VertexOffset ) + r.VertexCount > render.Vertices.size() ||
                 static_cast<uint64_t>( r.FirstTriangle ) + r.TriangleCount > render.Indices.size() )
                return MakeFormattedError<ImportedEditMesh>(
                     "FromRenderMesh: submesh {} spans vertices [{}, +{}) and triangles [{}, +{}) of {} / {}", i,
                     r.VertexOffset, r.VertexCount, r.FirstTriangle, r.TriangleCount, render.Vertices.size(),
                     render.Indices.size() );
            ranges.push_back( r );
        }

        ImportedEditMesh    result;
        EditMesh&           mesh       = result.Mesh;
        EditMeshAttributes& attributes = mesh.Attributes();
        attributes.EnableNormals();
        attributes.EnableTangents();
        (void)attributes.SetUVLayerCount( 1 );
        NormalOverlay&  normals  = *attributes.Normals();
        TangentOverlay& tangents = *attributes.Tangents();
        UVOverlay&      uvs      = *attributes.UV( 0 );

        PositionWelder welder( options.PositionTolerance );
        const auto     weldVertex = [&]( const glm::vec3& p )
        {
            int v = welder.Find( mesh, p );
            if ( v == InvalidId )
            {
                v = mesh.AppendVertex( p );
                welder.Add( p, v );
            }
            return v;
        };
        // Per overlay and vertex, the elements already made there; a vertex has a handful, so a scan is the
        // whole lookup. A detached copy of a vertex is a different vertex and gets its own elements.
        std::vector<std::vector<int>> normalAt;
        std::vector<std::vector<int>> tangentAt;
        std::vector<std::vector<int>> uvAt;
        const auto element = [&]( auto& overlay, std::vector<std::vector<int>>& at, int v, const auto& value )
        {
            if ( static_cast<int>( at.size() ) <= v )
                at.resize( static_cast<size_t>( v ) + 1 );
            for ( const int e : at[v] )
                if ( Within( overlay.GetElement( e ), value, options.AttributeTolerance ) )
                    return e;
            const int e = overlay.AppendElement( value );
            at[v].push_back( e );
            return e;
        };

        for ( const Range& range : ranges )
        {
            for ( uint32_t k = 0; k < range.TriangleCount; ++k )
            {
                const Index&                  index = render.Indices[range.FirstTriangle + k];
                const std::array<uint32_t, 3> local{ index.V1, index.V2, index.V3 };
                std::array<const Vertex*, 3>  source{};
                for ( int j = 0; j < 3; ++j )
                {
                    if ( local[j] >= range.VertexCount )
                        return MakeFormattedError<ImportedEditMesh>(
                             "FromRenderMesh: triangle {} of a submesh names vertex {} of its {}", k, local[j],
                             range.VertexCount );
                    source[j] = &render.Vertices[range.VertexOffset + local[j]];
                }

                std::array<int, 3> corners{};
                for ( int j = 0; j < 3; ++j )
                    corners[j] = weldVertex( source[j]->Position );
                if ( corners[0] == corners[1] || corners[1] == corners[2] || corners[2] == corners[0] )
                {
                    ++result.DroppedDegenerate;
                    continue;
                }
                if ( mesh.FindTriangle( corners[0], corners[1], corners[2] ) != InvalidId )
                {
                    ++result.DroppedDuplicate;
                    continue;
                }
                int t = InvalidId;
                if ( mesh.AppendTriangle( corners[0], corners[1], corners[2], t ) != EditResult::Ok )
                {
                    // NonManifoldEdge / InconsistentOrientation: the render mesh had it, EditMesh cannot share
                    // that edge, so this triangle stands on its own copies of its corners. A corner nothing
                    // uses yet (welded for this very triangle) is kept rather than copied, or it would stay
                    // behind as an isolated vertex.
                    for ( int j = 0; j < 3; ++j )
                        if ( !mesh.GetVertexEdges( corners[j] ).empty() )
                            corners[j] = mesh.AppendVertex( source[j]->Position );
                    // Every corner is now edge-less, so no edge of the triangle exists yet, and they are distinct.
                    [[maybe_unused]] const EditResult detached =
                         mesh.AppendTriangle( corners[0], corners[1], corners[2], t );
                    assert( detached == EditResult::Ok );
                    ++result.DetachedTriangles;
                }
                attributes.SetMaterialId( t, range.Material );

                std::array<int, 3> en{};
                std::array<int, 3> et{};
                std::array<int, 3> eu{};
                for ( int j = 0; j < 3; ++j )
                {
                    const Vertex& s = *source[j];
                    en[j]           = element( normals, normalAt, corners[j], s.Normal );
                    // The render vertex stores the bitangent itself; the layer stores only its handedness.
                    const float handed =
                         glm::dot( glm::cross( s.Normal, s.Tangent ), s.Bitangent ) < 0.0f ? -1.0f : 1.0f;
                    const glm::vec4 tangent( s.Tangent, handed );
                    et[j] = element( tangents, tangentAt, corners[j], tangent );
                    eu[j] = element( uvs, uvAt, corners[j], s.TexCoord );
                }
                // Elements are keyed by their vertex, and the three corners are distinct vertices, so these
                // cannot be refused; the results are still checked by CheckValidity in every test.
                (void)normals.SetTriangle( mesh, t, en );
                (void)tangents.SetTriangle( mesh, t, et );
                (void)uvs.SetTriangle( mesh, t, eu );
            }
        }
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Geometry
