#include "EditMeshAsset.hpp"

#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>

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

        // One group per render triangle, in Indices' order: render triangle i came from EditMesh triangle
        // SourceTriangles[i], which is the only thing that knows the group.
        std::vector<int32_t> groups;
        groups.reserve( render.SourceTriangles.size() );
        for ( const int t : render.SourceTriangles )
            groups.push_back( attributes.GetPolyGroup( t ) );
        return Common::MakeSuccess( MeshAssetDataFromRender( render, slotMaterials, std::move( groups ) ) );
    }

    Common::ResultStr<ImportedEditMesh> FromMeshAssetData( const Ser::MeshAssetData& data )
    {
        auto arrays = RenderFromMeshAssetData( data );
        if ( !arrays.IsSuccess() )
            return Common::MakeError<ImportedEditMesh>( arrays.GetError() );
        const RenderMeshData render = arrays.ExtractValue();

        auto imported = FromRenderMesh( render );
        if ( !imported.IsSuccess() )
            return Common::MakeError<ImportedEditMesh>( imported.GetError() );
        ImportedEditMesh result = imported.ExtractValue();
        if ( result.Mesh.TriangleCount() == 0 )
            return Common::MakeFormattedError<ImportedEditMesh>(
                 "none of the asset's {} faces survives the weld ({} degenerate, {} duplicate), so there is "
                 "nothing to model",
                 data.Indices.size(), result.DroppedDegenerate, result.DroppedDuplicate );

        // Face k of the file is render face k; a dropped face has no triangle and its group goes with it.
        for ( size_t k = 0; k < data.PolyGroups.size() && k < result.TriangleOfFace.size(); ++k )
            if ( const int t = result.TriangleOfFace[k]; t != InvalidId )
                result.Mesh.Attributes().SetPolyGroup( t, data.PolyGroups[k] );
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Geometry
