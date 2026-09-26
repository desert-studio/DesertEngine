// Ported from UE 5.8 Engine/Source/Developer/MeshBuilder/Private/StaticMeshBuilder.cpp (FStaticMeshBuilder::Build:
// build settings applied to the MeshDescription, tangents computed when missing, LODs reduced, render buffers
// written), adapted: the source is our EditMesh (EditMeshSer), the transform is Geometry::TransformMesh, the
// reduction is meshopt (SimplifyLODLevels), the render buffers are one MeshBinary container, and authored LOD
// source models (UE SourceModels[k>0]) are folded as extra index sets of LOD0's sections.
#include "MeshDeriver.hpp"

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/Geometry/MeshLOD.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    namespace
    {
        // Bakes each static submesh's LOD triangle sets (meshopt) into SubmeshData.LODs, so the load path skips
        // the simplification pass. Submeshes that already carry LODs (folded from authored source models) are
        // kept.
        void BakeStaticMeshLODs( Ser::MeshAssetData& data )
        {
            if ( data.IsSkinned )
                return;
            for ( auto& sm : data.Submeshes )
            {
                if ( !sm.LODs.empty() )
                    continue; // authored LODs already folded in -> don't regenerate

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

        // One source model built to render form: its MeshAssetData (submesh j draws material slot MaterialIds[j];
        // ToRenderMesh emits one submesh per distinct material ID, ascending) and the IDs themselves.
        struct BuiltModel
        {
            Ser::MeshAssetData Data;
            std::vector<int>   MaterialIds;
        };

        Common::ResultStr<BuiltModel> BuildModel( const Assets::MeshSourceAsset& asset, const size_t lod )
        {
            auto lifted = Geometry::Bridge::EditMeshFromSavedForm( asset.Source.Models[lod].Mesh );
            if ( !lifted.IsSuccess() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: the source does not load: {}",
                                                               asset.Name, lod, lifted.GetError() );
            Geometry::EditMesh mesh = lifted.ExtractValue();

            std::vector<int> triangles;
            BuiltModel       built;
            for ( int t = 0; t < mesh.MaxTriangleId(); ++t )
                if ( mesh.IsTriangle( t ) )
                {
                    triangles.push_back( t );
                    built.MaterialIds.push_back( mesh.Attributes().GetMaterialId( t ) );
                }
            if ( mesh.Attributes().Tangents() == nullptr )
            {
                mesh.Attributes().EnableTangents();
                if ( auto r = Geometry::ComputeTangentsAt( mesh, triangles ); !r.IsSuccess() )
                    return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: tangents: {}", asset.Name, lod,
                                                                   r.GetError() );
            }

            auto transformed = Geometry::TransformMesh( mesh, SourceToEngine( asset.Import.Settings ) );
            if ( !transformed.IsSuccess() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: import transform: {}", asset.Name, lod,
                                                               transformed.GetError() );

            std::ranges::sort( built.MaterialIds );
            built.MaterialIds.erase( std::unique( built.MaterialIds.begin(), built.MaterialIds.end() ),
                                     built.MaterialIds.end() );
            std::vector<Common::Content::AssetGuid> slotMaterials;
            for ( const int id : built.MaterialIds )
            {
                if ( id < 0 || static_cast<size_t>( id ) >= asset.Source.MaterialSlots.size() )
                    return Common::MakeFormattedError<BuiltModel>(
                         "'{}' LOD{}: a triangle uses material ID {} and the asset has {} material slots",
                         asset.Name, lod, id, asset.Source.MaterialSlots.size() );
                slotMaterials.push_back( asset.Source.MaterialSlots[static_cast<size_t>( id )].Material );
            }

            auto data = Geometry::Bridge::MeshAssetDataFromEditMesh( transformed.GetValue().Mesh, slotMaterials );
            if ( !data.IsSuccess() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: {}", asset.Name, lod,
                                                               data.GetError() );
            built.Data = data.ExtractValue();
            // A section is named by its material slot: the slot table is the one place the source names its
            // sections, so the render form repeats it rather than inventing a second name. An unnamed slot keeps
            // the bridge's "MaterialID <n>" (the source format allows unnamed slots).
            for ( size_t j = 0; j < built.Data.Submeshes.size() && j < built.MaterialIds.size(); ++j )
            {
                const std::string& slotName =
                     asset.Source.MaterialSlots[static_cast<size_t>( built.MaterialIds[j] )].Name;
                if ( !slotName.empty() )
                    built.Data.Submeshes[j].Name = slotName;
            }
            return Common::MakeSuccess( std::move( built ) );
        }

        // Folds the authored LOD models into LOD0's render form: LOD0 submesh j (material m) keeps its vertex
        // block and faces, and each model k >= 1 appends its material-m vertices INTO that block and its
        // material-m faces, offset to them and still submesh-local, as LOD level k. The draw path therefore treats
        // them exactly like simplified LODs (index sets drawn with baseVertex = Submesh.VertexOffset). Sections
        // pair by material slot, not by name: a source model has no node names, only slots.
        Common::ResultStr<Ser::MeshAssetData> FoldSourceModelLODs( const std::string& name, BuiltModel lod0,
                                                                   const std::vector<BuiltModel>& lods )
        {
            for ( size_t k = 0; k < lods.size(); ++k )
                for ( const int id : lods[k].MaterialIds )
                    if ( std::ranges::find( lod0.MaterialIds, id ) == lod0.MaterialIds.end() )
                        return Common::MakeFormattedError<Ser::MeshAssetData>(
                             "'{}' LOD{} draws material slot {}, which LOD0 does not use; a LOD section folds "
                             "into "
                             "the LOD0 section of its slot",
                             name, k + 1, id );

            Ser::MeshAssetData&                data = lod0.Data;
            std::vector<Ser::StaticVertexData> vertices;
            std::vector<Ser::IndexData>        indices;
            std::vector<int32_t>               groups;
            std::vector<Ser::SubmeshData>      submeshes;
            vertices.reserve( data.StaticVertices.size() );
            indices.reserve( data.Indices.size() );
            for ( size_t j = 0; j < data.Submeshes.size(); ++j )
            {
                const Ser::SubmeshData& base = data.Submeshes[j];
                Ser::SubmeshData        out  = base;
                out.VertexOffset             = static_cast<uint32_t>( vertices.size() );
                out.IndexOffset              = static_cast<uint32_t>( indices.size() * 3 );
                out.LODs.clear();
                vertices.insert( vertices.end(), data.StaticVertices.begin() + base.VertexOffset,
                                 data.StaticVertices.begin() + base.VertexOffset + base.VertexCount );
                const uint32_t firstFace = base.IndexOffset / 3;
                indices.insert( indices.end(), data.Indices.begin() + firstFace,
                                data.Indices.begin() + firstFace + base.IndexCount / 3 );
                if ( !data.PolyGroups.empty() )
                    groups.insert( groups.end(), data.PolyGroups.begin() + firstFace,
                                   data.PolyGroups.begin() + firstFace + base.IndexCount / 3 );

                for ( size_t k = 0; k < lods.size(); ++k )
                {
                    const auto at = std::ranges::find( lods[k].MaterialIds, lod0.MaterialIds[j] );
                    if ( at == lods[k].MaterialIds.end() )
                        continue;
                    // A level is its index in LODs, so a section missing from LOD k cannot reappear at k+1: its
                    // k+1 faces would be drawn at distance k.
                    if ( out.LODs.size() != k )
                        return Common::MakeFormattedError<Ser::MeshAssetData>(
                             "'{}': material slot {} has no faces in LOD{} but has faces in LOD{}", name,
                             lod0.MaterialIds[j], out.LODs.size() + 1, k + 1 );
                    const Ser::MeshAssetData& from = lods[k].Data;
                    const Ser::SubmeshData&   lod =
                         from.Submeshes[static_cast<size_t>( at - lods[k].MaterialIds.begin() )];
                    const uint32_t vBase = out.VertexCount;
                    vertices.insert( vertices.end(), from.StaticVertices.begin() + lod.VertexOffset,
                                     from.StaticVertices.begin() + lod.VertexOffset + lod.VertexCount );
                    out.VertexCount += lod.VertexCount;
                    std::vector<Ser::IndexData> faces;
                    faces.reserve( lod.IndexCount / 3 );
                    for ( uint32_t t = 0; t < lod.IndexCount / 3; ++t )
                    {
                        const Ser::IndexData& f = from.Indices[lod.IndexOffset / 3 + t];
                        faces.push_back( { f.V1 + vBase, f.V2 + vBase, f.V3 + vBase } );
                    }
                    out.LODs.push_back( std::move( faces ) );
                }
                submeshes.push_back( std::move( out ) );
            }
            data.StaticVertices = std::move( vertices );
            data.Indices        = std::move( indices );
            data.PolyGroups     = std::move( groups );
            data.Submeshes      = std::move( submeshes );
            return Common::MakeSuccess( std::move( data ) );
        }
    } // namespace

    Common::ResultStr<std::string> BuildMeshPlatformData( const Assets::MeshSourceAsset& asset )
    {
        if ( asset.Source.Skin.has_value() )
            return Common::MakeFormattedError<std::string>(
                 "'{}' is a skinned mesh; its render data is not derived yet (the skeleton's form is AF4f's)",
                 asset.Name );
        if ( asset.Source.Models.empty() )
            return Common::MakeFormattedError<std::string>( "'{}' has no source model", asset.Name );

        auto lod0 = BuildModel( asset, 0 );
        if ( !lod0.IsSuccess() )
            return Common::MakeError<std::string>( lod0.GetError() );

        // LodPolicy::None is LOD0 only: the authored models stay in the source, unused, so switching the policy
        // back re-derives them without a re-import.
        const bool         generate = asset.Import.Settings.LodPolicy == Assets::MeshLodPolicy::Generate;
        Ser::MeshAssetData out;
        if ( generate && asset.Source.Models.size() > 1 )
        {
            std::vector<BuiltModel> lods;
            for ( size_t k = 1; k < asset.Source.Models.size(); ++k )
            {
                auto built = BuildModel( asset, k );
                if ( !built.IsSuccess() )
                    return Common::MakeError<std::string>( built.GetError() );
                lods.push_back( built.ExtractValue() );
            }
            auto folded = FoldSourceModelLODs( asset.Name, lod0.ExtractValue(), lods );
            if ( !folded.IsSuccess() )
                return Common::MakeError<std::string>( folded.GetError() );
            out = folded.ExtractValue();
        }
        else
            out = std::move( lod0.ExtractValue().Data );
        // Sections the authored models left without LODs are simplified; folded ones are kept as authored.
        if ( generate )
            BakeStaticMeshLODs( out );
        return Common::MakeSuccess( Ser::EncodeMeshBinary( out ) );
    }
} // namespace Desert::Editor
