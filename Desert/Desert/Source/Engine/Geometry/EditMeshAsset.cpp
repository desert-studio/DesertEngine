#include "EditMeshAsset.hpp"

#include <Engine/Geometry/EditMeshConversion.hpp>

#include <string>

namespace Desert::Geometry
{
    namespace Ser = Assets::Serialization;

    Common::ResultStr<Ser::MeshAssetData>
    ToMeshAssetData( const EditMesh& mesh, std::span<const Common::Content::AssetGuid> slotMaterials )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeFormattedError<Ser::MeshAssetData>(
                 "the mesh has {} vertices and no triangle, and a static mesh with nothing to draw has no bounds",
                 mesh.VertexCount() );

        const EditMeshAttributes& attributes = mesh.Attributes();
        if ( attributes.Colors() != nullptr )
            return Common::MakeError<Ser::MeshAssetData>(
                 "the mesh carries a vertex colour layer and a .stmesh vertex has no colour; writing it would "
                 "drop the layer" );
        if ( attributes.UVLayerCount() > 1 )
            return Common::MakeFormattedError<Ser::MeshAssetData>(
                 "the mesh carries {} UV layers and a .stmesh vertex has one; writing it would drop layers 1..{}",
                 attributes.UVLayerCount(), attributes.UVLayerCount() - 1 );

        auto converted = ToRenderMesh( mesh );
        if ( !converted.IsSuccess() )
            return Common::MakeError<Ser::MeshAssetData>( converted.GetError() );
        const RenderMeshData render = converted.ExtractValue();

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
            to.MaterialGuid   = k < slotMaterials.size() ? slotMaterials[k] : Common::Content::AssetGuid{};
            data.Submeshes.push_back( std::move( to ) );
        }

        // One group per face of Indices, in Indices' order: render triangle i came from EditMesh triangle
        // SourceTriangles[i], which is the only thing that knows the group.
        data.PolyGroups.reserve( render.SourceTriangles.size() );
        for ( const int t : render.SourceTriangles )
            data.PolyGroups.push_back( attributes.GetPolyGroup( t ) );

        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<EditMesh> FromMeshAssetData( const Ser::MeshAssetData& data )
    {
        if ( data.IsSkinned )
            return Common::MakeFormattedError<EditMesh>(
                 "the asset is skinned ({} skinned vertices); an EditMesh has no bone weights to carry them",
                 data.SkinnedVertices.size() );
        if ( !data.PolyGroups.empty() && data.PolyGroups.size() != data.Indices.size() )
            return Common::MakeFormattedError<EditMesh>( "the asset has {} faces and {} polygroup entries",
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

        auto imported = FromRenderMesh( render );
        if ( !imported.IsSuccess() )
            return Common::MakeError<EditMesh>( imported.GetError() );
        ImportedEditMesh result = imported.ExtractValue();
        if ( result.DroppedDegenerate != 0 || result.DroppedDuplicate != 0 || result.DetachedTriangles != 0 )
            return Common::MakeFormattedError<EditMesh>(
                 "the asset's {} faces do not weld back one-to-one ({} degenerate, {} duplicate, {} detached), "
                 "so its per-face polygroups cannot be placed",
                 data.Indices.size(), result.DroppedDegenerate, result.DroppedDuplicate,
                 result.DetachedTriangles );

        // Nothing was dropped, so face k of the file is triangle k of the fresh mesh.
        for ( size_t k = 0; k < data.PolyGroups.size(); ++k )
            result.Mesh.Attributes().SetPolyGroup( static_cast<int>( k ), data.PolyGroups[k] );
        return Common::MakeSuccess( std::move( result.Mesh ) );
    }
} // namespace Desert::Geometry
