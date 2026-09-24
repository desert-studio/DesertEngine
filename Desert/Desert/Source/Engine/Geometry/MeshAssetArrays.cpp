#include "MeshAssetArrays.hpp"

#include <string>

namespace Desert::Geometry
{
    namespace Ser = Assets::Serialization;

    Ser::MeshAssetData MeshAssetDataFromRender( const RenderMeshData&         render,
                                                std::span<const Common::UUID> slotMaterials,
                                                std::vector<int32_t>          polyGroups )
    {
        Ser::MeshAssetData data;
        data.IsSkinned = false;

        data.StaticVertices.reserve( render.Vertices.size() );
        for ( const Vertex& v : render.Vertices )
            data.StaticVertices.push_back( { v.Position, v.Normal, v.Tangent, v.Bitangent, v.TexCoord } );

        data.Indices.reserve( render.Indices.size() );
        for ( const Index& i : render.Indices )
            data.Indices.push_back( { i.V1, i.V2, i.V3 } );

        data.Submeshes.reserve( render.Submeshes.size() );
        for ( size_t k = 0; k < render.Submeshes.size(); ++k )
        {
            const Submesh&   from = render.Submeshes[k];
            Ser::SubmeshData to;
            to.Name           = from.Name.empty() ? "Section" + std::to_string( k ) : from.Name;
            to.VertexOffset   = from.VertexOffset;
            to.VertexCount    = from.VertexCount;
            to.IndexOffset    = from.IndexOffset;
            to.IndexCount     = from.IndexCount;
            to.Transform      = from.Transform;
            to.BoundingBox    = from.BoundingBox;
            to.MaterialHandle = k < slotMaterials.size() ? slotMaterials[k] : Common::UUID::Null();
            data.Submeshes.push_back( std::move( to ) );
        }

        data.PolyGroups = std::move( polyGroups );
        return data;
    }

    Common::ResultStr<RenderMeshData> RenderFromMeshAssetData( const Ser::MeshAssetData& data )
    {
        if ( data.IsSkinned )
            return Common::MakeFormattedError<RenderMeshData>(
                 "the asset is skinned ({} skinned vertices); a modeling mesh has no bone weights to carry them",
                 data.SkinnedVertices.size() );
        if ( !data.PolyGroups.empty() && data.PolyGroups.size() != data.Indices.size() )
            return Common::MakeFormattedError<RenderMeshData>( "the asset has {} faces and {} polygroup entries",
                                                               data.Indices.size(), data.PolyGroups.size() );

        RenderMeshData render;
        render.Vertices.reserve( data.StaticVertices.size() );
        for ( const Ser::StaticVertexData& v : data.StaticVertices )
            render.Vertices.push_back( { v.Position, v.Normal, v.Tangent, v.Bitangent, v.TexCoord } );
        render.Indices.reserve( data.Indices.size() );
        for ( const Ser::IndexData& i : data.Indices )
            render.Indices.push_back( { i.V1, i.V2, i.V3 } );
        render.Submeshes.reserve( data.Submeshes.size() );
        for ( const Ser::SubmeshData& s : data.Submeshes )
            render.Submeshes.push_back( { s.Name, s.VertexOffset, s.VertexCount, s.IndexOffset, s.IndexCount,
                                          s.Transform, s.BoundingBox } );
        return Common::MakeSuccess( std::move( render ) );
    }
} // namespace Desert::Geometry
