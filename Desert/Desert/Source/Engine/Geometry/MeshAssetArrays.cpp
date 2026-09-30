#include "MeshAssetArrays.hpp"

#include <string>

namespace Desert::Geometry
{
    namespace Ser = Assets::Serialization;

    Ser::MeshAssetData MeshAssetDataFromRender( const RenderMeshData&                       render,
                                                std::span<const Common::Content::AssetGuid> slotMaterials,
                                                std::vector<int32_t>                        polyGroups )
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
            to.Name         = from.Name.empty() ? "Section" + std::to_string( k ) : from.Name;
            to.VertexOffset = from.VertexOffset;
            to.VertexCount  = from.VertexCount;
            to.IndexOffset  = from.IndexOffset;
            to.IndexCount   = from.IndexCount;
            to.Transform    = from.Transform;
            to.BoundingBox  = from.BoundingBox;
            to.MaterialGuid = k < slotMaterials.size() ? slotMaterials[k] : Common::Content::AssetGuid{};
            data.Submeshes.push_back( std::move( to ) );
        }

        data.PolyGroups = std::move( polyGroups );
        // Linear colour to linear RGBA8, rounded: the file's precision is UE's FColor vertex colour.
        data.Colors.reserve( render.Colors.size() );
        for ( const glm::vec4& c : render.Colors )
        {
            const glm::vec4 q = glm::round( glm::clamp( c, 0.0f, 1.0f ) * 255.0f );
            data.Colors.push_back( { static_cast<uint8_t>( q.r ), static_cast<uint8_t>( q.g ),
                                     static_cast<uint8_t>( q.b ), static_cast<uint8_t>( q.a ) } );
        }
        data.UV1 = render.UV1;
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
        const size_t vertexCount = data.StaticVertices.size();
        if ( ( !data.Colors.empty() && data.Colors.size() != vertexCount ) ||
             ( !data.UV1.empty() && data.UV1.size() != vertexCount ) )
            return Common::MakeFormattedError<RenderMeshData>( "the asset has {} vertices, {} colours and {} UV1 "
                                                               "entries (each stream is one per vertex or none)",
                                                               vertexCount, data.Colors.size(), data.UV1.size() );
        render.Colors.reserve( data.Colors.size() );
        for ( const std::array<uint8_t, 4>& c : data.Colors )
            render.Colors.push_back( glm::vec4( c[0], c[1], c[2], c[3] ) / 255.0f );
        render.UV1 = data.UV1;
        render.Submeshes.reserve( data.Submeshes.size() );
        for ( const Ser::SubmeshData& s : data.Submeshes )
            render.Submeshes.push_back( { s.Name, s.VertexOffset, s.VertexCount, s.IndexOffset, s.IndexCount,
                                          s.Transform, s.BoundingBox } );
        return Common::MakeSuccess( std::move( render ) );
    }
} // namespace Desert::Geometry
