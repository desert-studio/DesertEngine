#include "DynamicMeshAsset.hpp"

#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace Desert::Geometry
{
    namespace Ser = Assets::Serialization;

    namespace
    {
        // THE FILE'S BITANGENT IS A SIGN. The writer puts cross(N, T) * sign there (through the P6
        // reader's bitangent elements), and only the sign is meaningful on the way back. Welding the stored
        // vectors instead would merge a -0 and a +0 component at one vertex (they are within any tolerance) and
        // the file would not write back to the same bytes; so the bitangent overlay is rebuilt here as the P6
        // reader builds it: one element per (normal element, tangent element, sign) a corner uses, valued cross(N,
        // T) * sign.
        void RebuildBitangentsFromSigns( DynamicMesh3& mesh, const Ser::MeshAssetData& data,
                                         const std::vector<int>& triangleOfFace )
        {
            DynamicMeshAttributeSet&        attributes = *mesh.Attributes();
            const DynamicMeshNormalOverlay& normals    = *attributes.PrimaryNormals();
            const DynamicMeshNormalOverlay& tangents   = *attributes.PrimaryTangents();
            DynamicMeshNormalOverlay&       bitangents = *attributes.PrimaryBiTangents();
            bitangents.ClearElements();

            // Mesh corner j of triangle TriangleOfFace[k] is render corner kRenderCorner[j] of file face k (the
            // winding swap DynamicMeshRenderConversion.hpp makes on the way in).
            constexpr int                     kRenderCorner[3] = { 0, 2, 1 };
            std::map<std::array<int, 3>, int> made;
            for ( const Ser::SubmeshData& submesh : data.Submeshes )
                for ( uint32_t f = submesh.IndexOffset / 3; f < ( submesh.IndexOffset + submesh.IndexCount ) / 3;
                      ++f )
                {
                    const int t = triangleOfFace[f];
                    if ( t == DynamicMesh3::InvalidID )
                        continue; // a dropped face has no triangle and no bitangent
                    const Ser::IndexData&         face = data.Indices[f];
                    const std::array<uint32_t, 3> local{ face.V1, face.V2, face.V3 };
                    const Index3i                 en = normals.GetTriangle( t );
                    const Index3i                 et = tangents.GetTriangle( t );
                    Index3i                       eb;
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
    DynamicMeshToMeshAssetData( const DynamicMesh3&                         mesh,
                                std::span<const Common::Content::AssetGuid> slotMaterials )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeFormattedError<Ser::MeshAssetData>(
                 "the mesh has {} vertices and no triangle, and a static mesh with nothing to draw has no bounds",
                 mesh.VertexCount() );

        if ( const DynamicMeshAttributeSet* attributes = mesh.Attributes() )
        {
            // The colour overlay and UV layer 1 are written: they are the .stmesh's optional Colors / UV1
            // streams (MeshBinary v4), which ToRenderMesh fills. Only layers past 1 have no home in the file.
            if ( attributes->NumUVLayers() > 2 )
                return Common::MakeFormattedError<Ser::MeshAssetData>(
                     "the mesh carries {} UV layers and a .stmesh has two (the vertex's UV 0 and the UV1 stream); "
                     "writing it would drop layers 2..{}",
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

    Common::ResultStr<ImportedDynamicMesh> DynamicMeshFromMeshAssetData( const Ser::MeshAssetData& data )
    {
        auto arrays = RenderFromMeshAssetData( data );
        if ( !arrays.IsSuccess() )
            return Common::MakeError<ImportedDynamicMesh>( arrays.GetError() );
        const RenderMeshData render = arrays.ExtractValue();

        auto imported = DynamicMeshFromRenderMesh( render );
        if ( !imported.IsSuccess() )
            return Common::MakeError<ImportedDynamicMesh>( imported.GetError() );
        ImportedDynamicMesh result = imported.ExtractValue();
        if ( result.Mesh.TriangleCount() == 0 )
            return Common::MakeFormattedError<ImportedDynamicMesh>(
                 "none of the asset's {} faces survives the weld ({} degenerate, {} duplicate), so there is "
                 "nothing to model",
                 data.Indices.size(), result.DroppedDegenerate, result.DroppedDuplicate );

        // Face k of the file is render face k; a dropped face has no triangle and its group goes with it.
        DynamicMesh3& mesh = result.Mesh;
        RebuildBitangentsFromSigns( mesh, data, result.TriangleOfFace );
        mesh.EnableTriangleGroups( 0 );
        for ( size_t k = 0; k < data.PolyGroups.size() && k < result.TriangleOfFace.size(); ++k )
            if ( const int t = result.TriangleOfFace[k]; t != DynamicMesh3::InvalidID )
                mesh.SetTriangleGroup( t, data.PolyGroups[k] );
        return Common::MakeSuccess( std::move( result ) );
    }

    int MaxMaterialId( const DynamicMesh3& mesh )
    {
        const auto* materials = mesh.HasAttributes() ? mesh.Attributes()->GetMaterialID() : nullptr;
        int         top       = -1;
        if ( materials )
            for ( const int t : mesh.TriangleIndicesItr() )
                top = std::max( top, static_cast<int>( materials->GetValue( t ) ) );
        return top;
    }

    Common::BoolResultStr RemapMaterialIds( DynamicMesh3& mesh, std::span<const int> newIdOfOld )
    {
        auto* materials = mesh.HasAttributes() ? mesh.Attributes()->GetMaterialID() : nullptr;
        if ( !materials )
            return Common::MakeSuccess( true );
        for ( const int t : mesh.TriangleIndicesItr() )
            if ( const int32_t id = materials->GetValue( t );
                 id < 0 || static_cast<size_t>( id ) >= newIdOfOld.size() )
                return Common::MakeFormattedError<bool>(
                     "triangle {} carries MaterialID {}, outside the {}-entry slot table", t, id,
                     newIdOfOld.size() );
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const int32_t next = newIdOfOld[static_cast<size_t>( materials->GetValue( t ) )];
            materials->SetValue( t, &next );
        }
        return Common::MakeSuccess( true );
    }

    std::vector<int> DistinctMaterialIds( const DynamicMesh3& mesh )
    {
        const auto* materials = mesh.HasAttributes() ? mesh.Attributes()->GetMaterialID() : nullptr;
        if ( !materials )
            return { 0 };
        std::vector<int> ids;
        for ( const int t : mesh.TriangleIndicesItr() )
            ids.push_back( static_cast<int>( materials->GetValue( t ) ) );
        std::sort( ids.begin(), ids.end() );
        ids.erase( std::unique( ids.begin(), ids.end() ), ids.end() );
        return ids;
    }

    Common::BoolResultStr MapMaterialIds( DynamicMesh3& mesh, const std::function<int( int )>& map )
    {
        DynamicMeshAttributeSet* attributes = mesh.Attributes();
        if ( attributes == nullptr )
            return Common::MakeError<bool>( "the mesh has no attribute set to carry its materials" );
        if ( !attributes->HasMaterialID() )
            attributes->EnableMaterialID();
        auto* materials = attributes->GetMaterialID();
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const int32_t next = static_cast<int32_t>( map( static_cast<int>( materials->GetValue( t ) ) ) );
            materials->SetValue( t, &next );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Geometry
