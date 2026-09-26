#include "DynamicMeshAsset.hpp"

#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>
#include <Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <map>

namespace Desert::Geometry
{
    namespace Ser = Assets::Serialization;

    namespace
    {
        // THE FILE'S BITANGENT IS A SIGN. Both writers put cross(N, T) * sign there (the EditMesh render
        // conversion per render vertex, the FDynamicMesh3 one through the P6 reader's bitangent elements), and
        // the EditMesh reader keeps only the sign. Welding the stored vectors instead would merge a -0 and a +0
        // component at one vertex (they are within any tolerance) and the file would not write back to the
        // same bytes; so the bitangent overlay is rebuilt here as the P6 reader builds it: one element per
        // (normal element, tangent element, sign) a corner uses, valued cross(N, T) * sign.
        void RebuildBitangentsFromSigns( FDynamicMesh3& mesh, const Ser::MeshAssetData& data )
        {
            FDynamicMeshAttributeSet&        attributes = *mesh.Attributes();
            const FDynamicMeshNormalOverlay& normals    = *attributes.PrimaryNormals();
            const FDynamicMeshNormalOverlay& tangents   = *attributes.PrimaryTangents();
            FDynamicMeshNormalOverlay&       bitangents = *attributes.PrimaryBiTangents();
            bitangents.ClearElements();

            // Mesh corner j of triangle k is render corner kRenderCorner[j] of file face k (the winding swap
            // DynamicMeshRenderConversion.hpp makes on the way in).
            constexpr int                     kRenderCorner[3] = { 0, 2, 1 };
            std::map<std::array<int, 3>, int> made;
            for ( const Ser::SubmeshData& submesh : data.Submeshes )
                for ( uint32_t f = submesh.IndexOffset / 3; f < ( submesh.IndexOffset + submesh.IndexCount ) / 3;
                      ++f )
                {
                    const int                     t    = static_cast<int>( f );
                    const Ser::IndexData&         face = data.Indices[f];
                    const std::array<uint32_t, 3> local{ face.V1, face.V2, face.V3 };
                    const FIndex3i                en = normals.GetTriangle( t );
                    const FIndex3i                et = tangents.GetTriangle( t );
                    FIndex3i                      eb;
                    for ( int j = 0; j < 3; ++j )
                    {
                        const Ser::StaticVertexData& v =
                             data.StaticVertices[submesh.VertexOffset + local[kRenderCorner[j]]];
                        const int sign =
                             glm::dot( glm::cross( v.Normal, v.Tangent ), v.Bitangent ) < 0.0f ? -1 : 1;
                        const auto [it, fresh] = made.emplace( std::array<int, 3>{ en[j], et[j], sign }, 0 );
                        if ( fresh )
                        {
                            const glm::vec3 n = normals.GetElement( en[j] );
                            const glm::vec3 g = tangents.GetElement( et[j] );
                            const glm::vec3 b = glm::cross( n, g ) * static_cast<float>( sign );
                            it->second        = bitangents.AppendElement( b );
                        }
                        eb[j] = it->second;
                    }
                    (void)bitangents.SetTriangle( t, eb );
                }
        }
    } // namespace

    Common::ResultStr<Ser::MeshAssetData>
    DynamicMeshToMeshAssetData( const FDynamicMesh3&                        mesh,
                                std::span<const Common::Content::AssetGuid> slotMaterials )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeFormattedError<Ser::MeshAssetData>(
                 "the mesh has {} vertices and no triangle, and a static mesh with nothing to draw has no bounds",
                 mesh.VertexCount() );

        if ( const FDynamicMeshAttributeSet* attributes = mesh.Attributes() )
        {
            if ( attributes->HasPrimaryColors() )
                return Common::MakeError<Ser::MeshAssetData>(
                     "the mesh carries a vertex colour overlay and a .stmesh vertex has no colour; writing it "
                     "would drop the overlay" );
            if ( attributes->NumUVLayers() > 1 )
                return Common::MakeFormattedError<Ser::MeshAssetData>(
                     "the mesh carries {} UV layers and a .stmesh vertex has one; writing it would drop layers "
                     "1..{}",
                     attributes->NumUVLayers(), attributes->NumUVLayers() - 1 );
            if ( attributes->NumPolygroupLayers() > 0 )
                return Common::MakeFormattedError<Ser::MeshAssetData>(
                     "the mesh carries {} extended polygroup layers and a .stmesh face has one group (the mesh's "
                     "triangle groups); writing it would drop them",
                     attributes->NumPolygroupLayers() );
        }

        auto converted = ToRenderMesh( mesh );
        if ( !converted.IsSuccess() )
            return Common::MakeError<Ser::MeshAssetData>( converted.GetError() );
        const RenderMeshData render = converted.ExtractValue();

        // One group per render triangle, in Indices' order, looked up through the triangle it came from.
        const bool           grouped = mesh.HasTriangleGroups();
        std::vector<int32_t> groups;
        groups.reserve( render.SourceTriangles.size() );
        for ( const int t : render.SourceTriangles )
            groups.push_back( grouped ? mesh.GetTriangleGroup( t ) : 0 );
        return Common::MakeSuccess( MeshAssetDataFromRender( render, slotMaterials, std::move( groups ) ) );
    }

    Common::ResultStr<FDynamicMesh3> DynamicMeshFromMeshAssetData( const Ser::MeshAssetData& data )
    {
        auto arrays = RenderFromMeshAssetData( data );
        if ( !arrays.IsSuccess() )
            return Common::MakeError<FDynamicMesh3>( arrays.GetError() );
        const RenderMeshData render = arrays.ExtractValue();

        auto imported = DynamicMeshFromRenderMesh( render );
        if ( !imported.IsSuccess() )
            return Common::MakeError<FDynamicMesh3>( imported.GetError() );
        ImportedDynamicMesh result = imported.ExtractValue();
        if ( result.DroppedDegenerate != 0 || result.DroppedDuplicate != 0 || result.DetachedTriangles != 0 )
            return Common::MakeFormattedError<FDynamicMesh3>(
                 "the asset's {} faces do not weld back one-to-one ({} degenerate, {} duplicate, {} detached), "
                 "so its per-face polygroups cannot be placed",
                 data.Indices.size(), result.DroppedDegenerate, result.DroppedDuplicate,
                 result.DetachedTriangles );

        // Nothing was dropped or split off, so face k of the file is triangle k of the fresh mesh.
        FDynamicMesh3& mesh = result.Mesh;
        RebuildBitangentsFromSigns( mesh, data );
        mesh.EnableTriangleGroups( 0 );
        for ( size_t k = 0; k < data.PolyGroups.size(); ++k )
            mesh.SetTriangleGroup( static_cast<int>( k ), data.PolyGroups[k] );
        return Common::MakeSuccess( std::move( mesh ) );
    }
} // namespace Desert::Geometry
