#include "EditMeshSerialization.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <string>

namespace Desert::Geometry
{
    namespace
    {
        using Common::MakeFormattedError;

        template <typename T>
        constexpr int Components()
        {
            return static_cast<int>( T::length() );
        }

        template <typename T>
        void Put( std::vector<float>& out, const T& value )
        {
            for ( int i = 0; i < Components<T>(); ++i )
                out.push_back( value[i] );
        }

        template <typename T>
        T Take( const std::vector<float>& in, size_t element )
        {
            T value{};
            for ( int i = 0; i < Components<T>(); ++i )
                value[i] = in[element * Components<T>() + i];
            return value;
        }

        // The mesh handed in is COMPACTED, so element IDs are 0..n-1 and a triangle ID is its row.
        template <typename T>
        EditMeshOverlaySer WriteOverlay( const EditMesh& mesh, const EditMeshOverlay<T>& overlay )
        {
            EditMeshOverlaySer out;
            out.Values.reserve( static_cast<size_t>( overlay.ElementCount() ) * Components<T>() );
            for ( const int e : overlay.ElementIds() )
                Put( out.Values, overlay.GetElement( e ) );
            out.Triangles.reserve( static_cast<size_t>( mesh.TriangleCount() ) * 3 );
            for ( const int t : mesh.TriangleIds() )
                for ( const int e : overlay.GetTriangle( t ) )
                    out.Triangles.push_back( e );
            return out;
        }

        template <typename T>
        Common::BoolResultStr ReadOverlay( EditMesh& mesh, EditMeshOverlay<T>& overlay, const EditMeshOverlaySer& in,
                                           const std::string& name, const std::vector<int>& triangleIds )
        {
            if ( in.Values.size() % Components<T>() != 0 )
                return MakeFormattedError<bool>( "EditMesh {}: {} values is not a whole number of {}-float elements",
                                                 name, in.Values.size(), Components<T>() );
            if ( in.Triangles.size() != triangleIds.size() * 3 )
                return MakeFormattedError<bool>( "EditMesh {}: {} element indices for {} triangles (3 each)", name,
                                                 in.Triangles.size(), triangleIds.size() );

            const size_t elements = in.Values.size() / Components<T>();
            for ( size_t e = 0; e < elements; ++e )
                (void)overlay.AppendElement( Take<T>( in.Values, e ) );

            for ( size_t row = 0; row < triangleIds.size(); ++row )
            {
                const std::array<int, 3> corners{ in.Triangles[row * 3], in.Triangles[row * 3 + 1],
                                                  in.Triangles[row * 3 + 2] };
                const int unset = static_cast<int>( corners[0] == InvalidId ) +
                                  static_cast<int>( corners[1] == InvalidId ) +
                                  static_cast<int>( corners[2] == InvalidId );
                if ( unset == 3 )
                    continue;
                if ( unset != 0 )
                    return MakeFormattedError<bool>( "EditMesh {}: triangle {} is only partly set ({}, {}, {})", name,
                                                     row, corners[0], corners[1], corners[2] );
                for ( const int e : corners )
                    if ( e < 0 || static_cast<size_t>( e ) >= elements )
                        return MakeFormattedError<bool>( "EditMesh {}: triangle {} names element {} of {}", name, row,
                                                         e, elements );
                const EditResult result = overlay.SetTriangle( mesh, triangleIds[row], corners );
                if ( result != EditResult::Ok )
                    return MakeFormattedError<bool>( "EditMesh {}: triangle {} refused ({}, {}, {}): {}", name, row,
                                                     corners[0], corners[1], corners[2], ToString( result ) );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    EditMeshSer ToSerialized( const EditMesh& source )
    {
        EditMesh mesh = source;
        (void)mesh.Compact();

        EditMeshSer out;
        out.Positions.reserve( static_cast<size_t>( mesh.VertexCount() ) * 3 );
        for ( const int v : mesh.VertexIds() )
            Put( out.Positions, mesh.GetPosition( v ) );

        const EditMeshAttributes& attributes = mesh.Attributes();
        out.Triangles.reserve( static_cast<size_t>( mesh.TriangleCount() ) * 3 );
        for ( const int t : mesh.TriangleIds() )
        {
            for ( const int v : mesh.GetTriangle( t ) )
                out.Triangles.push_back( v );
            out.PolyGroups.push_back( attributes.GetPolyGroup( t ) );
            out.MaterialIds.push_back( attributes.GetMaterialId( t ) );
        }

        if ( const auto* normals = attributes.Normals() )
            out.Normals = WriteOverlay( mesh, *normals );
        if ( const auto* tangents = attributes.Tangents() )
            out.Tangents = WriteOverlay( mesh, *tangents );
        if ( const auto* colors = attributes.Colors() )
            out.Colors = WriteOverlay( mesh, *colors );
        for ( int layer = 0; layer < attributes.UVLayerCount(); ++layer )
            out.UVs.push_back( WriteOverlay( mesh, *attributes.UV( layer ) ) );
        return out;
    }

    Common::ResultStr<EditMesh> FromSerialized( const EditMeshSer& saved )
    {
        if ( saved.Positions.size() % 3 != 0 )
            return MakeFormattedError<EditMesh>( "EditMesh: {} position floats is not a whole number of vertices",
                                                 saved.Positions.size() );
        if ( saved.Triangles.size() % 3 != 0 )
            return MakeFormattedError<EditMesh>( "EditMesh: {} triangle indices is not a whole number of triangles",
                                                 saved.Triangles.size() );
        const size_t vertexCount   = saved.Positions.size() / 3;
        const size_t triangleCount = saved.Triangles.size() / 3;
        if ( saved.PolyGroups.size() != triangleCount || saved.MaterialIds.size() != triangleCount )
            return MakeFormattedError<EditMesh>( "EditMesh: {} triangles but {} polygroups and {} material IDs",
                                                 triangleCount, saved.PolyGroups.size(), saved.MaterialIds.size() );
        if ( saved.UVs.size() > static_cast<size_t>( EditMeshAttributes::MaxUVLayers ) )
            return MakeFormattedError<EditMesh>( "EditMesh: {} UV layers, at most {}", saved.UVs.size(),
                                                 EditMeshAttributes::MaxUVLayers );

        EditMesh mesh;
        for ( size_t v = 0; v < vertexCount; ++v )
            (void)mesh.AppendVertex( Take<glm::vec3>( saved.Positions, v ) );

        std::vector<int> triangleIds;
        triangleIds.reserve( triangleCount );
        for ( size_t row = 0; row < triangleCount; ++row )
        {
            const int a = saved.Triangles[row * 3], b = saved.Triangles[row * 3 + 1], c = saved.Triangles[row * 3 + 2];
            for ( const int v : { a, b, c } )
                if ( v < 0 || static_cast<size_t>( v ) >= vertexCount )
                    return MakeFormattedError<EditMesh>( "EditMesh: triangle {} names vertex {} of {}", row, v,
                                                         vertexCount );
            int              t      = InvalidId;
            const EditResult result = mesh.AppendTriangle( a, b, c, t );
            if ( result != EditResult::Ok )
                return MakeFormattedError<EditMesh>( "EditMesh: triangle {} ({}, {}, {}) refused: {}", row, a, b, c,
                                                     ToString( result ) );
            triangleIds.push_back( t );
            mesh.Attributes().SetPolyGroup( t, saved.PolyGroups[row] );
            mesh.Attributes().SetMaterialId( t, saved.MaterialIds[row] );
        }

        EditMeshAttributes& attributes = mesh.Attributes();
        if ( saved.Normals )
        {
            attributes.EnableNormals();
            if ( auto r = ReadOverlay( mesh, *attributes.Normals(), *saved.Normals, "normals", triangleIds );
                 !r.IsSuccess() )
                return Common::MakeError<EditMesh>( r.GetError() );
        }
        if ( saved.Tangents )
        {
            attributes.EnableTangents();
            if ( auto r = ReadOverlay( mesh, *attributes.Tangents(), *saved.Tangents, "tangents", triangleIds );
                 !r.IsSuccess() )
                return Common::MakeError<EditMesh>( r.GetError() );
        }
        if ( saved.Colors )
        {
            attributes.EnableColors();
            if ( auto r = ReadOverlay( mesh, *attributes.Colors(), *saved.Colors, "colors", triangleIds );
                 !r.IsSuccess() )
                return Common::MakeError<EditMesh>( r.GetError() );
        }
        if ( !attributes.SetUVLayerCount( static_cast<int>( saved.UVs.size() ) ) )
            return MakeFormattedError<EditMesh>( "EditMesh: {} UV layers refused", saved.UVs.size() );
        for ( size_t layer = 0; layer < saved.UVs.size(); ++layer )
            if ( auto r = ReadOverlay( mesh, *attributes.UV( static_cast<int>( layer ) ), saved.UVs[layer],
                                       "UV layer " + std::to_string( layer ), triangleIds );
                 !r.IsSuccess() )
                return Common::MakeError<EditMesh>( r.GetError() );

        if ( auto valid = mesh.CheckValidity(); !valid.IsSuccess() )
            return MakeFormattedError<EditMesh>( "EditMesh: the saved mesh is not valid: {}", valid.GetError() );
        return Common::MakeSuccess( std::move( mesh ) );
    }
} // namespace Desert::Geometry
