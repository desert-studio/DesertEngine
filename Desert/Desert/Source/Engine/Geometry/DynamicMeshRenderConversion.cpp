#include "DynamicMeshRenderConversion.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <map>
#include <string>

namespace Desert::Geometry
{
    namespace
    {
        using Common::MakeFormattedError;

        // The render side (and EditMesh) wind a front face counter-clockwise; DynamicMesh3 keeps UE's clockwise
        // front, the convention every ported normal / orientation algorithm assumes. The two conversions swap
        // corners 1 and 2, the one place the conventions meet. The map is its own inverse.
        constexpr int kRenderCorner[3] = { 0, 2, 1 };

        bool Within( const glm::vec3& a, const glm::vec3& b, float tolerance )
        {
            return std::abs( a.x - b.x ) <= tolerance && std::abs( a.y - b.y ) <= tolerance &&
                   std::abs( a.z - b.z ) <= tolerance;
        }
        bool Within( const glm::vec2& a, const glm::vec2& b, float tolerance )
        {
            return std::abs( a.x - b.x ) <= tolerance && std::abs( a.y - b.y ) <= tolerance;
        }

        // The EditMesh conversion's weld (EditMeshConversion.cpp), on DynamicMesh3: a uniform grid of cells
        // one tolerance wide, a point matches only inside its own cell or the 26 around it. Positions are
        // stored as doubles of the render floats, so reading them back as float is exact.
        class PositionWelder
        {
        public:
            explicit PositionWelder( float tolerance ) : m_Tolerance( tolerance )
            {
            }
            int Find( const DynamicMesh3& mesh, const glm::vec3& p ) const
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
                                if ( Within( glm::vec3( mesh.GetVertex( v ) ), p, m_Tolerance ) )
                                    return v;
                        }
                return DynamicMesh3::InvalidID;
            }
            void Add( const glm::vec3& p, int v )
            {
                m_Cells[Cell( p )].push_back( v );
            }

        private:
            std::array<int64_t, 3> Cell( const glm::vec3& p ) const
            {
                const float size = m_Tolerance > 0.0f ? m_Tolerance : 1.0f;
                return { static_cast<int64_t>( std::floor( p.x / size ) ),
                         static_cast<int64_t>( std::floor( p.y / size ) ),
                         static_cast<int64_t>( std::floor( p.z / size ) ) };
            }
            float                                              m_Tolerance;
            std::map<std::array<int64_t, 3>, std::vector<int>> m_Cells;
        };

        template <typename Overlay>
        Common::BoolResultStr RequireSet( const DynamicMesh3& mesh, const Overlay& overlay, const char* name )
        {
            for ( const int t : mesh.TriangleIndicesItr() )
                if ( !overlay.IsSetTriangle( t ) )
                    return MakeFormattedError<bool>( "ToRenderMesh: triangle {} is unset in the {} overlay", t,
                                                     name );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<RenderMeshData> ToRenderMesh( const DynamicMesh3& mesh, int uvLayer )
    {
        const DynamicMeshAttributeSet* attributes = mesh.Attributes();
        if ( attributes == nullptr || attributes->PrimaryNormals() == nullptr )
            return MakeFormattedError<RenderMeshData>( "ToRenderMesh: the mesh has no normal overlay" );
        const DynamicMeshNormalOverlay*  normals      = attributes->PrimaryNormals();
        const bool                       tangentSpace = attributes->HasTangentSpace();
        const DynamicMeshNormalOverlay*  tangents     = tangentSpace ? attributes->PrimaryTangents() : nullptr;
        const DynamicMeshNormalOverlay*  bitangents   = tangentSpace ? attributes->PrimaryBiTangents() : nullptr;
        const DynamicMeshUVOverlay*      uvs          = attributes->GetUVLayer( uvLayer );
        if ( attributes->NumUVLayers() > 0 && uvs == nullptr )
            return MakeFormattedError<RenderMeshData>( "ToRenderMesh: UV layer {} requested, the mesh has {}",
                                                       uvLayer, attributes->NumUVLayers() );
        if ( auto r = RequireSet( mesh, *normals, "normal" ); !r.IsSuccess() )
            return Common::MakeError<RenderMeshData>( r.GetError() );
        if ( tangentSpace )
        {
            if ( auto r = RequireSet( mesh, *tangents, "tangent" ); !r.IsSuccess() )
                return Common::MakeError<RenderMeshData>( r.GetError() );
            if ( auto r = RequireSet( mesh, *bitangents, "bitangent" ); !r.IsSuccess() )
                return Common::MakeError<RenderMeshData>( r.GetError() );
        }
        if ( uvs != nullptr )
            if ( auto r = RequireSet( mesh, *uvs, "UV" ); !r.IsSuccess() )
                return Common::MakeError<RenderMeshData>( r.GetError() );

        // Triangles grouped by material, ascending triangle ID inside each group.
        const DynamicMeshMaterialAttribute*  materialIds = attributes->GetMaterialID();
        std::map<int, std::vector<int>>      byMaterial;
        for ( const int t : mesh.TriangleIndicesItr() )
            byMaterial[materialIds != nullptr ? materialIds->GetValue( t ) : 0].push_back( t );

        RenderMeshData out;
        for ( const auto& [material, triangles] : byMaterial )
        {
            Submesh submesh{};
            submesh.Name         = "MaterialID " + std::to_string( material );
            submesh.VertexOffset = static_cast<uint32_t>( out.Vertices.size() );
            submesh.IndexOffset  = static_cast<uint32_t>( out.Indices.size() * 3 ); // uint32_t units
            submesh.Transform    = glm::mat4( 1.0f );

            // (vertex, normal, tangent, bitangent, uv element) -> submesh-local render vertex.
            std::map<std::array<int, 5>, uint32_t> corners;
            for ( const int t : triangles )
            {
                const Index3i  tri = mesh.GetTriangle( t );
                const Index3i  en  = normals->GetTriangle( t );
                const Index3i  et  = tangentSpace ? tangents->GetTriangle( t ) : Index3i::Invalid();
                const Index3i  eb  = tangentSpace ? bitangents->GetTriangle( t ) : Index3i::Invalid();
                const Index3i  eu  = uvs ? uvs->GetTriangle( t ) : Index3i::Invalid();
                Index          index{};
                uint32_t*      slots[3] = { &index.V1, &index.V2, &index.V3 };
                for ( int j = 0; j < 3; ++j )
                {
                    // Render corner j is mesh corner kRenderCorner[j]: the winding flips back to counter-clockwise
                    // here, and the corners are visited in render order so first-use order is the render order.
                    const int                c = kRenderCorner[j];
                    const std::array<int, 5> key{ tri[c], en[c], et[c], eb[c], eu[c] };
                    auto                     found = corners.find( key );
                    if ( found == corners.end() )
                    {
                        Vertex vertex{};
                        vertex.Position = glm::vec3( mesh.GetVertex( tri[c] ) );
                        vertex.Normal   = normals->GetElement( en[c] );
                        if ( tangentSpace )
                        {
                            vertex.Tangent   = tangents->GetElement( et[c] );
                            vertex.Bitangent = bitangents->GetElement( eb[c] );
                        }
                        if ( uvs )
                            vertex.TexCoord = uvs->GetElement( eu[c] );
                        const auto local = static_cast<uint32_t>( out.Vertices.size() - submesh.VertexOffset );
                        out.Vertices.push_back( vertex );
                        out.SourceVertices.push_back( tri[c] );
                        found = corners.emplace( key, local ).first;
                    }
                    *slots[j] = found->second;
                }
                out.Indices.push_back( index );
                out.SourceTriangles.push_back( t );
            }

            submesh.VertexCount = static_cast<uint32_t>( out.Vertices.size() ) - submesh.VertexOffset;
            submesh.IndexCount  = static_cast<uint32_t>( out.Indices.size() * 3 ) - submesh.IndexOffset;
            glm::vec3 lo( 0.0f ), hi( 0.0f );
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

    Common::ResultStr<ImportedDynamicMesh> DynamicMeshFromRenderMesh( const RenderMeshData& render,
                                                                      const WeldOptions&    options )
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
                return MakeFormattedError<ImportedDynamicMesh>(
                     "FromRenderMesh: submesh {} index range ({}, {}) is not whole triangles", i, s.IndexOffset,
                     s.IndexCount );
            const Range r{ s.VertexOffset, s.VertexCount, s.IndexOffset / 3, s.IndexCount / 3,
                           i < render.SubmeshMaterialIds.size() ? render.SubmeshMaterialIds[i]
                                                                : static_cast<int>( i ) };
            if ( uint64_t( r.VertexOffset ) + r.VertexCount > render.Vertices.size() ||
                 uint64_t( r.FirstTriangle ) + r.TriangleCount > render.Indices.size() )
                return MakeFormattedError<ImportedDynamicMesh>(
                     "FromRenderMesh: submesh {} spans vertices [{}, +{}) and triangles [{}, +{}) of {} / {}", i,
                     r.VertexOffset, r.VertexCount, r.FirstTriangle, r.TriangleCount, render.Vertices.size(),
                     render.Indices.size() );
            ranges.push_back( r );
        }

        ImportedDynamicMesh result;
        DynamicMesh3&       mesh = result.Mesh;
        mesh.EnableAttributes();
        DynamicMeshAttributeSet& attributes = *mesh.Attributes();
        attributes.SetNumUVLayers( 1 );
        attributes.EnableTangents();
        attributes.EnableMaterialID();
        DynamicMeshNormalOverlay&      normals     = *attributes.PrimaryNormals();
        DynamicMeshNormalOverlay&      tangents    = *attributes.PrimaryTangents();
        DynamicMeshNormalOverlay&      bitangents  = *attributes.PrimaryBiTangents();
        DynamicMeshUVOverlay&          uvs         = *attributes.GetUVLayer( 0 );
        DynamicMeshMaterialAttribute&  materialIds = *attributes.GetMaterialID();

        PositionWelder welder( options.PositionTolerance );
        const auto     weldVertex = [&]( const glm::vec3& p )
        {
            int v = welder.Find( mesh, p );
            if ( v == DynamicMesh3::InvalidID )
            {
                v = mesh.AppendVertex( glm::dvec3( p ) );
                welder.Add( p, v );
            }
            return v;
        };
        // Per overlay and vertex, the elements already made there; a vertex has a handful, so a scan is the
        // whole lookup. A detached copy of a vertex is a different vertex and gets its own elements.
        std::vector<std::vector<int>> normalAt, tangentAt, bitangentAt, uvAt;
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
                        return MakeFormattedError<ImportedDynamicMesh>(
                             "FromRenderMesh: triangle {} of a submesh names vertex {} of its {}", k, local[j],
                             range.VertexCount );
                    source[j] = &render.Vertices[range.VertexOffset + local[j]];
                }

                // Welded in render order, so vertex IDs come out in the order the EditMesh import makes them. Then
                // mesh corner j is render corner kRenderCorner[j]: counter-clockwise render winding becomes UE's
                // clockwise-front winding (VectorUtil::Normal), so every ported algorithm sees outward normals.
                Index3i welded;
                for ( int j = 0; j < 3; ++j )
                    welded[j] = weldVertex( source[j]->Position );
                const std::array<const Vertex*, 3> renderSource = source;
                Index3i                            corners;
                for ( int j = 0; j < 3; ++j )
                {
                    corners[j] = welded[kRenderCorner[j]];
                    source[j]  = renderSource[kRenderCorner[j]];
                }
                if ( corners.A == corners.B || corners.B == corners.C || corners.C == corners.A )
                {
                    ++result.DroppedDegenerate;
                    continue;
                }
                if ( mesh.FindTriangle( corners.A, corners.B, corners.C ) != DynamicMesh3::InvalidID )
                {
                    ++result.DroppedDuplicate;
                    continue;
                }
                int t = mesh.AppendTriangle( corners );
                if ( t == DynamicMesh3::NonManifoldID )
                {
                    // The render mesh had a third triangle on an edge; DynamicMesh3 cannot share it, so this
                    // triangle stands on its own copies of its already-used corners (a corner welded for this
                    // very triangle is kept, or it would stay behind isolated). DynamicMesh3, unlike EditMesh,
                    // accepts an opposite winding across an edge, so only this case detaches.
                    for ( int j = 0; j < 3; ++j )
                        if ( mesh.GetVtxEdgeCount( corners[j] ) > 0 )
                            corners[j] = mesh.AppendVertex( glm::dvec3( source[j]->Position ) );
                    t = mesh.AppendTriangle( corners );
                    ++result.DetachedTriangles;
                }
                if ( t < 0 )
                    return MakeFormattedError<ImportedDynamicMesh>(
                         "FromRenderMesh: triangle {} of a submesh was refused by the mesh ({})", k, t );
                materialIds.SetValue( t, range.Material );

                Index3i en, et, eb, eu;
                for ( int j = 0; j < 3; ++j )
                {
                    const Vertex& s = *source[j];
                    en[j]           = element( normals, normalAt, corners[j], s.Normal );
                    et[j]           = element( tangents, tangentAt, corners[j], s.Tangent );
                    eb[j]           = element( bitangents, bitangentAt, corners[j], s.Bitangent );
                    eu[j]           = element( uvs, uvAt, corners[j], s.TexCoord );
                }
                // Elements are keyed by their vertex and the three corners are distinct vertices, so these
                // cannot be refused; every test still runs CheckValidity on the result.
                (void)normals.SetTriangle( t, en );
                (void)tangents.SetTriangle( t, et );
                (void)bitangents.SetTriangle( t, eb );
                (void)uvs.SetTriangle( t, eu );
            }
        }
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Geometry
