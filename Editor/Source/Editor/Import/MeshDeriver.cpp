// Ported from UE 5.8 Engine/Source/Developer/MeshBuilder/Private/StaticMeshBuilder.cpp (FStaticMeshBuilder::Build:
// build settings applied to the MeshDescription, tangents computed when missing, LODs reduced, render buffers
// written), adapted: the source is our EditMesh (EditMeshSer), the transform is Geometry::TransformMesh, the
// reduction is meshopt (SimplifyLODLevels) and the render buffers are one MeshBinary container.
#include "MeshDeriver.hpp"

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/EditMeshAsset.hpp>
#include <Engine/Geometry/EditMeshNormals.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshXformOperations.hpp>
#include <Engine/Geometry/MeshLOD.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <vector>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    void BakeStaticMeshLODs( Ser::MeshAssetData& data )
    {
        if ( data.IsSkinned )
            return;
        for ( auto& sm : data.Submeshes )
        {
            if ( !sm.LODs.empty() )
                continue; // author LODs already folded in (FoldExternalLODMeshes) -> don't regenerate

            const uint32_t triCount = sm.IndexCount / 3;
            if ( triCount < 8 || sm.VertexCount == 0 ||
                 sm.VertexOffset + sm.VertexCount > data.StaticVertices.size() )
                continue;

            std::vector<float> pos;
            pos.reserve( static_cast<size_t>( sm.VertexCount ) * 3 );
            for ( uint32_t v = 0; v < sm.VertexCount; ++v )
            {
                const auto& p = data.StaticVertices[sm.VertexOffset + v].Position;
                pos.push_back( p.x );
                pos.push_back( p.y );
                pos.push_back( p.z );
            }

            std::vector<Desert::Index> localTris;
            localTris.reserve( triCount );
            const uint32_t triStart = sm.IndexOffset / 3;
            if ( triStart + triCount > data.Indices.size() )
                continue;
            for ( uint32_t t = 0; t < triCount; ++t )
            {
                const auto& idx = data.Indices[triStart + t];
                localTris.push_back( { idx.V1, idx.V2, idx.V3 } );
            }

            const auto levels = Geometry::SimplifyLODLevels( pos.data(), sm.VertexCount, localTris );
            sm.LODs.clear();
            sm.LODs.reserve( levels.size() );
            for ( const auto& lvl : levels )
            {
                std::vector<Ser::IndexData> tris;
                tris.reserve( lvl.size() );
                for ( const auto& tri : lvl )
                    tris.push_back( { tri.V1, tri.V2, tri.V3 } );
                sm.LODs.push_back( std::move( tris ) );
            }
        }
    }

    namespace
    {
        // Source axes -> the engine's (+Y up, centimetres). FromFile and Y: the importer already resolved the
        // file's own hierarchy into Y-up, so only the scale remains; Z: +Z up becomes +Y up (x, y, z) ->
        // (x, z, -y), a rotation, so winding and handedness are kept.
        glm::mat4 SourceToEngine( const Assets::MeshImportSettings& settings )
        {
            glm::mat4 m = glm::scale( glm::mat4( 1.0f ), glm::vec3( settings.UniformScale ) );
            if ( settings.UpAxis == Assets::MeshSourceUpAxis::Z )
                m = glm::rotate( glm::mat4( 1.0f ), glm::radians( -90.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ) * m;
            return m;
        }
    } // namespace

    Common::ResultStr<std::string> BuildMeshPlatformData( const Assets::MeshSourceAsset& asset )
    {
        if ( asset.Source.Skin.has_value() )
            return Common::MakeFormattedError<std::string>(
                 "'{}' is a skinned mesh; its render data is not derived yet (the skeleton's form is AF4f's)",
                 asset.Name );

        auto lifted = Geometry::FromSerialized( asset.Source.Mesh );
        if ( !lifted.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "'{}': the source does not load: {}", asset.Name,
                                                            lifted.GetError() );
        Geometry::EditMesh mesh = lifted.ExtractValue();

        std::vector<int> triangles;
        std::vector<int> materialIds;
        for ( int t = 0; t < mesh.MaxTriangleId(); ++t )
            if ( mesh.IsTriangle( t ) )
            {
                triangles.push_back( t );
                materialIds.push_back( mesh.Attributes().GetMaterialId( t ) );
            }
        if ( mesh.Attributes().Tangents() == nullptr )
        {
            mesh.Attributes().EnableTangents();
            if ( auto r = Geometry::ComputeTangentsAt( mesh, triangles ); !r.IsSuccess() )
                return Common::MakeFormattedError<std::string>( "'{}': tangents: {}", asset.Name, r.GetError() );
        }

        auto transformed = Geometry::TransformMesh( mesh, SourceToEngine( asset.Import.Settings ) );
        if ( !transformed.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "'{}': import transform: {}", asset.Name,
                                                            transformed.GetError() );

        // ToRenderMesh emits one submesh per distinct material ID in ascending order; submesh k draws with the
        // slot that ID names.
        std::ranges::sort( materialIds );
        materialIds.erase( std::unique( materialIds.begin(), materialIds.end() ), materialIds.end() );
        std::vector<Common::Content::AssetGuid> slotMaterials;
        for ( const int id : materialIds )
        {
            if ( id < 0 || static_cast<size_t>( id ) >= asset.Source.MaterialSlots.size() )
                return Common::MakeFormattedError<std::string>(
                     "'{}': a triangle uses material ID {} and the asset has {} material slots", asset.Name, id,
                     asset.Source.MaterialSlots.size() );
            slotMaterials.push_back( asset.Source.MaterialSlots[static_cast<size_t>( id )].Material );
        }

        auto data = Geometry::ToMeshAssetData( transformed.GetValue().Mesh, slotMaterials );
        if ( !data.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "'{}': {}", asset.Name, data.GetError() );
        Ser::MeshAssetData out = data.ExtractValue();
        if ( asset.Import.Settings.LodPolicy == Assets::MeshLodPolicy::Generate )
            BakeStaticMeshLODs( out );
        return Common::MakeSuccess( Ser::EncodeMeshBinary( out ) );
    }
} // namespace Desert::Editor
