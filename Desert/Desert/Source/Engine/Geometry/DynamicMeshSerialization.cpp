#include "DynamicMeshSerialization.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <map>
#include <string>
#include <utility>

namespace Desert::Geometry
{
    namespace
    {
        using Common::MakeFormattedError;

        // UE's MAX_NUM_UV_CHANNELS, the most UV layers DynamicMeshAttributeSet is meant to carry.
        constexpr size_t kMaxUVLayers = 8;

        // Saved corner j is mesh corner kCorner[j]: counter-clockwise saved winding <-> UE's clockwise front.
        // The map is its own inverse.
        constexpr std::array<int, 3> kCorner{ 0, 2, 1 };

        Index3i Swizzle( const Index3i& in )
        {
            return { in[kCorner[0]], in[kCorner[1]], in[kCorner[2]] };
        }

        // Elements a live triangle uses, densely renumbered in ascending element ID (-1 = not written).
        template <typename OverlayT>
        std::vector<int> DenseElements( const DynamicMesh3& mesh, const OverlayT& overlay )
        {
            std::vector<int> dense( static_cast<size_t>( overlay.MaxElementID() ), -1 );
            for ( const int t : mesh.TriangleIndicesItr() )
                if ( overlay.IsSetTriangle( t ) )
                {
                    const Index3i tri = overlay.GetTriangle( t );
                    for ( int j = 0; j < 3; ++j )
                        dense[tri[j]] = 0;
                }
            int next = 0;
            for ( int& d : dense )
                if ( d == 0 )
                    d = next++;
            return dense;
        }

        // Writes an overlay; @p value writes one element's floats (the tangent layer appends its sign).
        template <typename OverlayT, typename ValueFn>
        EditMeshOverlaySer WriteOverlay( const DynamicMesh3& mesh, const OverlayT& overlay, ValueFn&& value )
        {
            const std::vector<int> dense = DenseElements( mesh, overlay );
            EditMeshOverlaySer     out;
            for ( int e = 0; e < static_cast<int>( dense.size() ); ++e )
                if ( dense[e] >= 0 )
                    value( e, out.Values );
            out.Triangles.reserve( static_cast<size_t>( mesh.TriangleCount() ) * 3 );
            for ( const int t : mesh.TriangleIndicesItr() )
            {
                if ( !overlay.IsSetTriangle( t ) )
                {
                    out.Triangles.insert( out.Triangles.end(), { -1, -1, -1 } );
                    continue;
                }
                const Index3i saved = Swizzle( overlay.GetTriangle( t ) );
                for ( int j = 0; j < 3; ++j )
                    out.Triangles.push_back( dense[saved[j]] );
            }
            return out;
        }

        template <int N, typename OverlayT>
        EditMeshOverlaySer WritePlain( const DynamicMesh3& mesh, const OverlayT& overlay )
        {
            return WriteOverlay( mesh, overlay,
                                 [&]( int e, std::vector<float>& values )
                                 {
                                     std::array<float, N> data{};
                                     // the raw-float read lives on the base; the vector overlay hides it
                                     static_cast<const typename OverlayT::BaseType&>( overlay ).GetElement(
                                          e, data.data() );
                                     values.insert( values.end(), data.begin(), data.end() );
                                 } );
        }

        glm::vec3 Element3( const DynamicMeshNormalOverlay& overlay, int e )
        {
            glm::vec3 v{};
            static_cast<const DynamicMeshNormalOverlay::BaseType&>( overlay ).GetElement( e, &v.x );
            return v;
        }

        // The handedness of each tangent element, recovered as UE's VectorUtil::BinormalSign.
        std::vector<float> TangentSigns( const DynamicMesh3& mesh, const DynamicMeshAttributeSet& attributes )
        {
            const DynamicMeshNormalOverlay&  tangents = *attributes.PrimaryTangents();
            std::vector<float>               sign( static_cast<size_t>( tangents.MaxElementID() ), 1.0f );
            std::vector<bool>                decided( sign.size(), false );
            if ( !attributes.HasTangentSpace() )
                return sign;
            const DynamicMeshNormalOverlay& normals    = *attributes.PrimaryNormals();
            const DynamicMeshNormalOverlay& bitangents = *attributes.PrimaryBiTangents();
            for ( const int t : mesh.TriangleIndicesItr() )
            {
                if ( !tangents.IsSetTriangle( t ) || !normals.IsSetTriangle( t ) ||
                     !bitangents.IsSetTriangle( t ) )
                    continue;
                const Index3i et = Swizzle( tangents.GetTriangle( t ) );
                const Index3i en = Swizzle( normals.GetTriangle( t ) );
                const Index3i eb = Swizzle( bitangents.GetTriangle( t ) );
                for ( int j = 0; j < 3; ++j )
                {
                    if ( decided[et[j]] )
                        continue;
                    const float d =
                         glm::dot( glm::cross( Element3( normals, en[j] ), Element3( tangents, et[j] ) ),
                                   Element3( bitangents, eb[j] ) );
                    if ( d == 0.0f )
                        continue;
                    sign[et[j]]    = d < 0.0f ? -1.0f : 1.0f;
                    decided[et[j]] = true;
                }
            }
            return sign;
        }

        struct Reader
        {
            const EditMeshSer&    Saved;
            std::string_view      Owner;
            DynamicMesh3&         Mesh;
            std::vector<Index3i>  SavedTriangles; // saved (counter-clockwise) vertex order per row
            std::vector<int>      TriangleIds;

            template <typename... Args>
            Common::BoolResultStr Fail( fmt::format_string<Args...> format, Args&&... args ) const
            {
                return Common::MakeError<bool>( "mesh of '" + std::string( Owner ) +
                                                "': " + fmt::format( format, std::forward<Args>( args )... ) );
            }

            // Appends the elements (@p stride floats each) and sets every triangle, refusing what an overlay
            // could not have written. @p elementsOut receives the saved element index -> overlay element ID map.
            template <typename OverlayT>
            Common::BoolResultStr Read( OverlayT& overlay, const EditMeshOverlaySer& in, const std::string& name,
                                        int stride, std::vector<int>& elementsOut ) const
            {
                if ( in.Values.size() % static_cast<size_t>( stride ) != 0 )
                    return Fail( "{}: {} values is not a whole number of {}-float elements", name,
                                 in.Values.size(), stride );
                if ( in.Triangles.size() != TriangleIds.size() * 3 )
                    return Fail( "{}: {} element indices for {} triangles (3 each)", name, in.Triangles.size(),
                                 TriangleIds.size() );

                const size_t elements = in.Values.size() / static_cast<size_t>( stride );
                elementsOut.assign( elements, DynamicMesh3::InvalidID );
                for ( size_t e = 0; e < elements; ++e )
                    elementsOut[e] = overlay.AppendElement( &in.Values[e * static_cast<size_t>( stride )] );

                std::vector<int> parent( elements, DynamicMesh3::InvalidID );
                for ( size_t row = 0; row < TriangleIds.size(); ++row )
                {
                    const Index3i c( in.Triangles[row * 3], in.Triangles[row * 3 + 1], in.Triangles[row * 3 + 2] );
                    const int      unset = ( c[0] == -1 ) + ( c[1] == -1 ) + ( c[2] == -1 );
                    if ( unset == 3 )
                        continue;
                    if ( unset != 0 )
                        return Fail( "{}: triangle {} is only partly set ({}, {}, {})", name, row, c[0], c[1],
                                     c[2] );
                    for ( int j = 0; j < 3; ++j )
                    {
                        if ( c[j] < 0 || static_cast<size_t>( c[j] ) >= elements )
                            return Fail( "{}: triangle {} names element {} of {}", name, row, c[j], elements );
                        const int vertex = SavedTriangles[row][j];
                        if ( parent[c[j]] != DynamicMesh3::InvalidID && parent[c[j]] != vertex )
                            return Fail( "{}: element {} is used at vertex {} and at vertex {} (triangle {})",
                                         name, c[j], parent[c[j]], vertex, row );
                        parent[c[j]] = vertex;
                    }
                    if ( c[0] == c[1] || c[1] == c[2] || c[2] == c[0] )
                        return Fail( "{}: triangle {} names one element twice ({}, {}, {})", name, row, c[0], c[1],
                                     c[2] );
                    const Index3i ids( elementsOut[c[0]], elementsOut[c[1]], elementsOut[c[2]] );
                    if ( const MeshResult r = overlay.SetTriangle( TriangleIds[row], Swizzle( ids ) );
                         r != MeshResult::Ok )
                        return Fail( "{}: triangle {} refused by the overlay ({})", name, row,
                                     static_cast<int>( r ) );
                }
                for ( size_t e = 0; e < elements; ++e )
                    if ( parent[e] == DynamicMesh3::InvalidID )
                        return Fail( "{}: element {} of {} is used by no triangle", name, e, elements );
                return Common::MakeSuccess( true );
            }

            // Tangents: xyz into layer 1, and one bitangent element per (tangent, normal) element pair.
            Common::BoolResultStr ReadTangents( DynamicMeshAttributeSet& attributes,
                                                const std::vector<int>&  normalIds )
            {
                const EditMeshOverlaySer& in = *Saved.Tangents;
                std::vector<int>          tangentIds;
                if ( auto r = Read( *attributes.PrimaryTangents(), ToXyz( in ), "tangents", 3, tangentIds );
                     !r.IsSuccess() )
                    return r;

                std::vector<float> sign( tangentIds.size() );
                for ( size_t e = 0; e < tangentIds.size(); ++e )
                {
                    sign[e] = in.Values[e * 4 + 3];
                    if ( sign[e] != 1.0f && sign[e] != -1.0f )
                        return Fail( "tangents: element {} has handedness {}; only +1 or -1 is expressible", e,
                                     sign[e] );
                }

                const DynamicMeshNormalOverlay&    normals    = *attributes.PrimaryNormals();
                const DynamicMeshNormalOverlay&    tangents   = *attributes.PrimaryTangents();
                DynamicMeshNormalOverlay&          bitangents = *attributes.PrimaryBiTangents();
                std::map<std::pair<int, int>, int> pairs; // (saved tangent, saved normal) -> bitangent element
                std::vector<bool>                  framed( tangentIds.size(), false );
                const std::vector<int>&            normalRows = Saved.Normals->Triangles;
                for ( size_t row = 0; row < TriangleIds.size(); ++row )
                {
                    if ( in.Triangles[row * 3] == -1 )
                        continue;
                    if ( normalRows[row * 3] == -1 )
                        return Fail( "tangents: triangle {} has a tangent but no normal, so no bitangent", row );
                    Index3i eb;
                    for ( int j = 0; j < 3; ++j )
                    {
                        const int st = in.Triangles[row * 3 + j];
                        const int sn = normalRows[row * 3 + j];
                        auto      it = pairs.find( { st, sn } );
                        if ( it == pairs.end() )
                        {
                            const glm::vec3 cross = glm::cross( Element3( normals, normalIds[sn] ),
                                                                Element3( tangents, tangentIds[st] ) );
                            const glm::vec3 b     = cross * sign[st];
                            framed[st]            = framed[st] || glm::dot( cross, b ) != 0.0f;
                            it = pairs.emplace( std::make_pair( st, sn ), bitangents.AppendElement( &b.x ) ).first;
                        }
                        eb[j] = it->second;
                    }
                    if ( const MeshResult r = bitangents.SetTriangle( TriangleIds[row], Swizzle( eb ) );
                         r != MeshResult::Ok )
                        return Fail( "bitangents: triangle {} refused by the overlay ({})", row,
                                     static_cast<int>( r ) );
                }
                for ( size_t e = 0; e < framed.size(); ++e )
                    if ( sign[e] < 0.0f && !framed[e] )
                        return Fail( "tangents: element {} has handedness -1 but no corner with a non-degenerate "
                                     "frame (cross(N, T) = 0), so the sign cannot be carried",
                                     e );
                return Common::MakeSuccess( true );
            }

            static EditMeshOverlaySer ToXyz( const EditMeshOverlaySer& in )
            {
                EditMeshOverlaySer out; // the caller has checked the length is a whole number of 4-float elements
                out.Triangles = in.Triangles;
                for ( size_t i = 0; i < in.Values.size(); i += 4 )
                    out.Values.insert( out.Values.end(), { in.Values[i], in.Values[i + 1], in.Values[i + 2] } );
                return out;
            }
        };
    } // namespace

    EditMeshSer ToSerialized( const DynamicMesh3& mesh )
    {
        EditMeshSer out;

        std::vector<int> denseVertex( static_cast<size_t>( mesh.MaxVertexID() ), -1 );
        int              next = 0;
        out.Positions.reserve( static_cast<size_t>( mesh.VertexCount() ) * 3 );
        for ( const int v : mesh.VertexIndicesItr() )
        {
            denseVertex[v]    = next++;
            const glm::dvec3 p = mesh.GetVertex( v );
            out.Positions.insert( out.Positions.end(), { static_cast<float>( p.x ), static_cast<float>( p.y ),
                                                         static_cast<float>( p.z ) } );
        }

        const DynamicMeshAttributeSet*      attributes = mesh.Attributes();
        const DynamicMeshMaterialAttribute* materialIds =
             attributes != nullptr ? attributes->GetMaterialID() : nullptr;
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const Index3i saved = Swizzle( mesh.GetTriangle( t ) );
            for ( int j = 0; j < 3; ++j )
                out.Triangles.push_back( denseVertex[saved[j]] );
            out.PolyGroups.push_back( mesh.GetTriangleGroup( t ) );
            out.MaterialIds.push_back( materialIds != nullptr ? materialIds->GetValue( t ) : 0 );
        }
        if ( attributes == nullptr )
            return out;

        if ( attributes->NumNormalLayers() >= 1 )
            out.Normals = WritePlain<3>( mesh, *attributes->PrimaryNormals() );
        if ( attributes->NumNormalLayers() >= 2 )
        {
            const DynamicMeshNormalOverlay&  tangents = *attributes->PrimaryTangents();
            const std::vector<float>         sign     = TangentSigns( mesh, *attributes );
            out.Tangents                              = WriteOverlay( mesh, tangents,
                                                                      [&]( int e, std::vector<float>& values )
                                                                      {
                                             const glm::vec3 t = Element3( tangents, e );
                                             values.insert( values.end(), { t.x, t.y, t.z, sign[e] } );
                                         } );
        }
        if ( attributes->HasPrimaryColors() )
            out.Colors = WritePlain<4>( mesh, *attributes->PrimaryColors() );
        for ( int layer = 0; layer < attributes->NumUVLayers(); ++layer )
            out.UVs.push_back( WritePlain<2>( mesh, *attributes->GetUVLayer( layer ) ) );
        return out;
    }

    Common::ResultStr<DynamicMesh3> DynamicMeshFromSerialized( const EditMeshSer& saved, std::string_view owner )
    {
        const auto fail = [&]( const std::string& message ) { return Common::MakeError<DynamicMesh3>( message ); };
        const std::string prefix = "mesh of '" + std::string( owner ) + "': ";

        if ( saved.Positions.size() % 3 != 0 )
            return MakeFormattedError<DynamicMesh3>( "{}{} position floats is not a whole number of vertices",
                                                     prefix, saved.Positions.size() );
        if ( saved.Triangles.size() % 3 != 0 )
            return MakeFormattedError<DynamicMesh3>( "{}{} triangle indices is not a whole number of triangles",
                                                     prefix, saved.Triangles.size() );
        const size_t vertexCount   = saved.Positions.size() / 3;
        const size_t triangleCount = saved.Triangles.size() / 3;
        if ( saved.PolyGroups.size() != triangleCount || saved.MaterialIds.size() != triangleCount )
            return MakeFormattedError<DynamicMesh3>( "{}{} triangles but {} polygroups and {} material IDs",
                                                     prefix, triangleCount, saved.PolyGroups.size(),
                                                     saved.MaterialIds.size() );
        if ( saved.UVs.size() > kMaxUVLayers )
            return MakeFormattedError<DynamicMesh3>( "{}{} UV layers, at most {}", prefix, saved.UVs.size(),
                                                     kMaxUVLayers );
        if ( saved.Tangents && !saved.Normals )
            return fail( prefix + "a tangent layer without a normal layer has no bitangent" );

        DynamicMesh3 mesh;
        mesh.EnableTriangleGroups();
        mesh.EnableAttributes();
        DynamicMeshAttributeSet& attributes = *mesh.Attributes();
        attributes.SetNumNormalLayers( saved.Tangents ? 3 : ( saved.Normals ? 1 : 0 ) );
        attributes.SetNumUVLayers( static_cast<int>( saved.UVs.size() ) );
        if ( saved.Colors )
            attributes.EnablePrimaryColors();
        attributes.EnableMaterialID();

        for ( size_t v = 0; v < vertexCount; ++v )
            (void)mesh.AppendVertex(
                 glm::dvec3( saved.Positions[v * 3], saved.Positions[v * 3 + 1], saved.Positions[v * 3 + 2] ) );

        Reader reader{ saved, owner, mesh, {}, {} };
        reader.SavedTriangles.reserve( triangleCount );
        reader.TriangleIds.reserve( triangleCount );
        for ( size_t row = 0; row < triangleCount; ++row )
        {
            const Index3i tri( saved.Triangles[row * 3], saved.Triangles[row * 3 + 1],
                               saved.Triangles[row * 3 + 2] );
            for ( int j = 0; j < 3; ++j )
                if ( tri[j] < 0 || static_cast<size_t>( tri[j] ) >= vertexCount )
                    return MakeFormattedError<DynamicMesh3>( "{}triangle {} names vertex {} of {}", prefix, row,
                                                             tri[j], vertexCount );
            if ( tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0] )
                return MakeFormattedError<DynamicMesh3>( "{}triangle {} ({}, {}, {}) is degenerate", prefix, row,
                                                         tri[0], tri[1], tri[2] );
            const int t = mesh.AppendTriangle( Swizzle( tri ), saved.PolyGroups[row] );
            if ( t == DynamicMesh3::NonManifoldID )
                return MakeFormattedError<DynamicMesh3>(
                     "{}triangle {} ({}, {}, {}) would make a non-manifold edge", prefix, row, tri[0], tri[1],
                     tri[2] );
            if ( t == DynamicMesh3::DuplicateTriangleID )
                return MakeFormattedError<DynamicMesh3>( "{}triangle {} ({}, {}, {}) repeats an earlier triangle",
                                                         prefix, row, tri[0], tri[1], tri[2] );
            if ( t < 0 )
                return MakeFormattedError<DynamicMesh3>( "{}triangle {} ({}, {}, {}) refused ({})", prefix, row,
                                                         tri[0], tri[1], tri[2], t );
            attributes.GetMaterialID()->SetValue( t, saved.MaterialIds[row] );
            reader.SavedTriangles.push_back( tri );
            reader.TriangleIds.push_back( t );
        }

        const auto check = [&]( const Common::BoolResultStr& r ) -> std::optional<std::string>
        {
            if ( r.IsSuccess() )
                return std::nullopt;
            return r.GetError();
        };
        std::vector<int> normalIds;
        std::vector<int> scratch;
        if ( saved.Normals )
            if ( auto e = check(
                      reader.Read( *attributes.PrimaryNormals(), *saved.Normals, "normals", 3, normalIds ) ) )
                return fail( *e );
        if ( saved.Tangents )
        {
            if ( saved.Tangents->Values.size() % 4 != 0 )
                return MakeFormattedError<DynamicMesh3>(
                     "{}tangents: {} values is not a whole number of 4-float elements", prefix,
                     saved.Tangents->Values.size() );
            if ( auto e = check( reader.ReadTangents( attributes, normalIds ) ) )
                return fail( *e );
        }
        if ( saved.Colors )
            if ( auto e =
                      check( reader.Read( *attributes.PrimaryColors(), *saved.Colors, "colors", 4, scratch ) ) )
                return fail( *e );
        for ( size_t layer = 0; layer < saved.UVs.size(); ++layer )
            if ( auto e =
                      check( reader.Read( *attributes.GetUVLayer( static_cast<int>( layer ) ), saved.UVs[layer],
                                          "UV layer " + std::to_string( layer ), 2, scratch ) ) )
                return fail( *e );

        if ( !mesh.CheckValidity( {}, ValidityCheckFailMode::ReturnOnly ) )
            return fail( prefix + "the rebuilt mesh fails DynamicMesh3::CheckValidity" );
        return Common::MakeSuccess( std::move( mesh ) );
    }
} // namespace Desert::Geometry
