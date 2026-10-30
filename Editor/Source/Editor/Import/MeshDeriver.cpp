// Ported from UE 5.8 Engine/Source/Developer/MeshBuilder/Private/StaticMeshBuilder.cpp (FStaticMeshBuilder::Build:
// build settings applied to the MeshDescription, tangents computed when missing, LODs reduced, render buffers
// written), adapted: the source is our DynamicMesh3 (its saved form), the transform is Geometry::TransformMesh,
// the reduction is meshopt (SimplifyLODLevels), the render buffers are one MeshBinary container, and authored LOD
// source models (UE SourceModels[k>0]) are folded as extra index sets of LOD0's sections.
#include "MeshDeriver.hpp"
#include "SourceToEngine.hpp"
#include "TextureImporter.hpp"

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/DynamicMeshXformOperations.hpp>
#include <Engine/Geometry/MeshRegionOperation.hpp>
#include <Engine/Geometry/DisplacementSurface.hpp>
#include <Engine/Geometry/MeshLOD.hpp>
#include <Engine/Geometry/ClusterDag.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    void BakeMeshLODs( Ser::MeshAssetData& data )
    {
        const size_t vertexCount = data.IsSkinned ? data.SkinnedVertices.size() : data.StaticVertices.size();
        for ( auto& sm : data.Submeshes )
        {
            if ( !sm.LODs.empty() )
                continue; // authored LODs already folded in -> don't regenerate

            const uint32_t triCount = sm.IndexCount / 3;
            if ( triCount < 8 || sm.VertexCount == 0 || sm.VertexOffset + sm.VertexCount > vertexCount )
                continue;

            std::vector<float> pos;
            pos.reserve( static_cast<size_t>( sm.VertexCount ) * 3 );
            std::vector<float> attrs;
            attrs.reserve( static_cast<size_t>( sm.VertexCount ) * Geometry::kSimplifyAttributeCount );
            for ( uint32_t v = 0; v < sm.VertexCount; ++v )
            {
                const auto append = [&]( const auto& vertex )
                {
                    pos.push_back( vertex.Position.x );
                    pos.push_back( vertex.Position.y );
                    pos.push_back( vertex.Position.z );
                    Geometry::AppendSimplifyAttributes( vertex.Normal, vertex.TexCoord, attrs );
                };
                if ( data.IsSkinned )
                    append( data.SkinnedVertices[sm.VertexOffset + v] );
                else
                    append( data.StaticVertices[sm.VertexOffset + v] );
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

            const auto levels = Geometry::SimplifyLODLevels( pos.data(), sm.VertexCount, localTris, attrs.data() );
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

    // VIRT-GEOM: each submesh's cluster LOD DAG (Geometry/ClusterDag.hpp) and, when @p cutLods, the discrete LOD
    // chain CUT from it at kClusterDagCutErrors (no second simplification: the DAG is the one reduction). The cook
    // refuses a DAG whose cut 0 is not LOD0 or whose error is not monotone - the two invariants the renderer's
    // LOD rule stands on.
    Common::BoolResultStr BuildMeshClusterDags( Ser::MeshAssetData& data, const std::string& name,
                                                const bool cutLods )
    {
        // A triangle up to rotation (the winding kept): its smallest rotation, so a cluster-local reorder
        // compares.
        const auto Canonical = []( const std::vector<uint32_t>& tris )
        {
            std::vector<std::array<uint32_t, 3>> out;
            out.reserve( tris.size() / 3 );
            for ( size_t i = 0; i + 2 < tris.size(); i += 3 )
            {
                const std::array<uint32_t, 3> a{ tris[i], tris[i + 1], tris[i + 2] }, b{ a[1], a[2], a[0] },
                     c{ a[2], a[0], a[1] };
                out.push_back( std::min( { a, b, c } ) );
            }
            std::sort( out.begin(), out.end() );
            return out;
        };
        for ( size_t s = 0; s < data.Submeshes.size(); ++s )
        {
            Ser::SubmeshData& sm = data.Submeshes[s];
            if ( sm.IndexCount < 3 || sm.VertexCount == 0 ||
                 size_t( sm.VertexOffset ) + sm.VertexCount > data.StaticVertices.size() ||
                 size_t( sm.IndexOffset / 3 ) + sm.IndexCount / 3 > data.Indices.size() )
                return Common::MakeFormattedError<bool>(
                     "'{}' submesh {} ('{}'): no triangles or ranges outside the mesh", name, s, sm.Name );
            std::vector<float> pos, attrs;
            pos.reserve( size_t( sm.VertexCount ) * 3 );
            attrs.reserve( size_t( sm.VertexCount ) * Geometry::kSimplifyAttributeCount );
            glm::vec3 lo( std::numeric_limits<float>::max() ), hi( -std::numeric_limits<float>::max() );
            for ( uint32_t v = 0; v < sm.VertexCount; ++v )
            {
                const Ser::StaticVertexData& vertex = data.StaticVertices[sm.VertexOffset + v];
                pos.insert( pos.end(), { vertex.Position.x, vertex.Position.y, vertex.Position.z } );
                Geometry::AppendSimplifyAttributes( vertex.Normal, vertex.TexCoord, attrs );
                lo = glm::min( lo, vertex.Position );
                hi = glm::max( hi, vertex.Position );
            }
            std::vector<uint32_t> tris;
            tris.reserve( sm.IndexCount );
            for ( uint32_t t = 0; t < sm.IndexCount / 3; ++t )
            {
                const Ser::IndexData& f = data.Indices[sm.IndexOffset / 3 + t];
                tris.insert( tris.end(), { f.V1, f.V2, f.V3 } );
            }
            auto built = Geometry::BuildClusterDag( pos, sm.VertexCount, tris, attrs.data(),
                                                    Geometry::ClusterDagSettings{} );
            if ( !built.IsSuccess() )
                return Common::MakeFormattedError<bool>( "'{}' submesh {} ('{}'): {}", name, s, sm.Name,
                                                         built.GetError() );
            sm.ClusterDag            = built.ExtractValue();
            const float boundsRadius = 0.5f * glm::length( hi - lo );
            if ( Canonical( Geometry::CutClusterDag( sm.ClusterDag, 0.0f, boundsRadius ) ) != Canonical( tris ) )
                return Common::MakeFormattedError<bool>( "'{}' submesh {} ('{}'): the DAG's cut 0 is not LOD0",
                                                         name, s, sm.Name );
            uint32_t rootTriangles = 0;
            for ( const Geometry::ClusterRecord& c : sm.ClusterDag.Clusters )
            {
                const Geometry::ClusterGroupRecord& g = sm.ClusterDag.Groups[c.Group];
                if ( !( c.LodError < g.ParentLodError ) )
                    return Common::MakeFormattedError<bool>( "'{}' submesh {} ('{}'): level {} cluster error {} "
                                                             "is not below its group's parent error {}",
                                                             name, s, sm.Name, c.Level, c.LodError,
                                                             g.ParentLodError );
                if ( g.ParentLodError == Geometry::kClusterRootError )
                    rootTriangles = std::max( rootTriangles, c.TriangleCount );
            }
            LOG_INFO(
                 "[VirtGeomCook] '{}' submesh {} ('{}'): {} triangles -> {} clusters, {} groups, {} levels, {} "
                 "hierarchy nodes, largest root cluster {} triangles",
                 name, s, sm.Name, tris.size() / 3, sm.ClusterDag.Clusters.size(), sm.ClusterDag.Groups.size(),
                 sm.ClusterDag.LevelCount, sm.ClusterDag.Hierarchy.size(), rootTriangles );

            sm.LODs.clear();
            if ( !cutLods )
                continue;
            size_t previous = tris.size() / 3;
            for ( size_t k = 1; k < std::size( Geometry::kClusterDagCutErrors ); ++k )
            {
                const std::vector<uint32_t> cut =
                     Geometry::CutClusterDag( sm.ClusterDag, Geometry::kClusterDagCutErrors[k], boundsRadius );
                // A cut no coarser than the level before it is not a level: the DAG has nothing to drop there.
                if ( cut.size() / 3 >= previous )
                    continue;
                previous = cut.size() / 3;
                std::vector<Ser::IndexData> faces;
                faces.reserve( previous );
                for ( size_t i = 0; i + 2 < cut.size(); i += 3 )
                    faces.push_back( { cut[i], cut[i + 1], cut[i + 2] } );
                sm.LODs.push_back( std::move( faces ) );
            }
        }
        return Common::MakeSuccess( true );
    }

    namespace
    {
        // One source model built to render form: its MeshAssetData (submesh j draws material slot MaterialIds[j];
        // ToRenderMesh emits one submesh per distinct material ID, ascending) and the IDs themselves.
        struct BuiltModel
        {
            Ser::MeshAssetData Data;
            std::vector<int>   MaterialIds;
        };

        Common::ResultStr<BuiltModel> BuildModel( const Assets::MeshSourceAsset& asset, const size_t lod )
        {
            auto lifted = Geometry::DynamicMeshFromSerialized( asset.Source.Models[lod].Mesh, asset.Name );
            if ( !lifted.IsSuccess() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: the source does not load: {}",
                                                               asset.Name, lod, lifted.GetError() );
            Geometry::DynamicMesh3 mesh = lifted.ExtractValue();
            if ( !mesh.HasAttributes() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: the source has no attribute set",
                                                               asset.Name, lod );

            BuiltModel built;
            built.MaterialIds = Geometry::DistinctMaterialIds( mesh );
            // UE builds tangents when the source has none (FStaticMeshBuilder: ComputeTangents on a
            // MeshDescription without them).
            if ( auto r = Geometry::EnsureTangentSpace( mesh, "Mesh Deriver" ); !r.IsSuccess() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: tangents: {}", asset.Name, lod,
                                                               r.GetError() );

            auto transformed = Geometry::TransformMesh( mesh, SourceToEngine( asset.Import.Settings ) );
            if ( !transformed.IsSuccess() )
                return Common::MakeFormattedError<BuiltModel>( "'{}' LOD{}: import transform: {}", asset.Name, lod,
                                                               transformed.GetError() );

            std::vector<Common::Content::AssetGuid> slotMaterials;
            for ( const int id : built.MaterialIds )
            {
                if ( id < 0 || static_cast<size_t>( id ) >= asset.Source.MaterialSlots.size() )
                    return Common::MakeFormattedError<BuiltModel>(
                         "'{}' LOD{}: a triangle uses material ID {} and the asset has {} material slots",
                         asset.Name, lod, id, asset.Source.MaterialSlots.size() );
                slotMaterials.push_back( asset.Source.MaterialSlots[static_cast<size_t>( id )].Material );
            }

            auto data = Geometry::DynamicMeshToMeshAssetData( transformed.GetValue().Mesh, slotMaterials );
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
        // A virtual-geometry mesh's LODs are cut from its cluster DAG over LOD0 (one reduction, one vertex copy):
        // its authored models are not folded, they stay in the source as with LodPolicy::None.
        const bool         virtualGeometry = asset.Import.Settings.VirtualGeometry;
        Ser::MeshAssetData out;
        if ( generate && !virtualGeometry && asset.Source.Models.size() > 1 )
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
        if ( virtualGeometry )
        {
            const auto dags = BuildMeshClusterDags( out, asset.Name, generate );
            if ( !dags.IsSuccess() )
                return Common::MakeError<std::string>( dags.GetError() );
        }
        else if ( generate )
            BakeMeshLODs( out );
        return Common::MakeSuccess( Ser::EncodeMeshBinary( out ) );
    }

    Common::ResultStr<std::string> BuildDisplacedMeshBytes( const Assets::DisplacedMeshBuildInput& input )
    {
        if ( !input.Topology )
            return Common::MakeError<std::string>( "displaced mesh: no displacement topology in the input" );
        const Geometry::DisplacementTopology& topology = *input.Topology;

        // One decode per distinct height map, shared by the sections that read it: the COOKED chain, as the
        // GPU samples it, so the bake and collision match the runtime displacement (BC, mip filter).
        std::map<std::filesystem::path, std::shared_ptr<const Geometry::DisplacementHeightImage>> images;
        std::vector<Geometry::DisplacementSurfaceMaterial> materials( input.Submeshes.size() );
        for ( size_t s = 0; s < input.Submeshes.size() && s < input.Heights.size(); ++s )
        {
            const auto& height = input.Heights[s];
            if ( !height )
                continue;
            auto& image = images[height->TextureAsset];
            if ( !image )
            {
                auto platform = TextureImporter::DecodePlatformMips( height->TextureAsset );
                if ( !platform.IsSuccess() )
                    return Common::MakeFormattedError<std::string>( "displaced mesh: section {} height: {}", s,
                                                                    platform.GetError() );
                std::vector<Geometry::DisplacementHeightImage::Mip> mips;
                for ( TextureImporter::PlatformMip& mip : platform.ExtractValue() )
                    mips.push_back( { mip.Width, mip.Height, std::move( mip.Texels ) } );
                auto made = Geometry::MakeDisplacementHeightImage( std::move( mips ) );
                if ( !made.IsSuccess() )
                    return Common::MakeFormattedError<std::string>( "displaced mesh: section {} height '{}': {}", s,
                                                                    height->TextureAsset.generic_string(),
                                                                    made.GetError() );
                image = made.ExtractValue();
            }
            Geometry::DisplacementSurfaceMaterial& m = materials[s];
            m.Image                                  = image;
            m.Channel                                = height->Channel;
            m.UVSet                                  = height->UVSet;
            m.ScaleCm                                = height->ScaleCm;
            m.Center                                 = height->Center;
            m.Tiling                                 = height->Tiling;
            m.Offset                                 = height->Offset;
            m.Rotation                               = height->Rotation;
            m.HasTransform                           = height->HasTransform;
        }

        std::vector<uint8_t> levels;
        if ( input.EdgeLengthCm )
        {
            uint32_t clamped = 0;
            auto     forEdge = Geometry::DisplacementLevelsForEdge( topology, *input.EdgeLengthCm, &clamped );
            if ( !forEdge.IsSuccess() )
                return Common::MakeError<std::string>( forEdge.GetError() );
            levels = forEdge.ExtractValue();
            if ( clamped != 0 )
                LOG_WARN( "[DisplacedMesh] {} of {} clusters cannot dice to {} cm (the fixed-point barycentric "
                          "limit): they bake at their maximum level",
                          clamped, levels.size(), *input.EdgeLengthCm );
        }
        else
            levels = Geometry::DisplacementMaxLevels( topology );

        const Geometry::DisplacementSurfaceSource source{ input.Vertices, input.Streams, input.Indices,
                                                          input.Submeshes };
        auto surface = Geometry::BuildDisplacedSurface( topology, source, materials, levels );
        if ( !surface.IsSuccess() )
            return Common::MakeError<std::string>( surface.GetError() );
        const Geometry::DisplacedSurface& built = surface.GetValue();

        // Weld bitwise-equal vertices per submesh: both clusters of a border emit its points, and an unwelded
        // border would be cut open by the LOD simplifier.
        Ser::MeshAssetData out;
        const bool         streams = !built.Streams.empty();
        for ( size_t s = 0; s < built.Submeshes.size(); ++s )
        {
            const Submesh& sm = built.Submeshes[s];
            using Key         = std::array<uint32_t, 17>;
            std::map<Key, uint32_t> welded;
            std::vector<uint32_t>   remap( sm.VertexCount );
            const auto              firstVertex = static_cast<uint32_t>( out.StaticVertices.size() );
            for ( uint32_t v = 0; v < sm.VertexCount; ++v )
            {
                const Vertex& vertex = built.Vertices[sm.VertexOffset + v];
                Key           key{};
                std::memcpy( key.data(), &vertex, sizeof( Vertex ) );
                if ( streams )
                    std::memcpy( key.data() + 14, &built.Streams[sm.VertexOffset + v], sizeof( MeshVertexStreams ) );
                const auto [it, inserted] = welded.emplace(
                     key, static_cast<uint32_t>( out.StaticVertices.size() ) - firstVertex );
                remap[v] = it->second;
                if ( !inserted )
                    continue;
                out.StaticVertices.push_back(
                     { vertex.Position, vertex.Normal, vertex.Tangent, vertex.Bitangent, vertex.TexCoord } );
                if ( streams )
                {
                    out.Colors.push_back( built.Streams[sm.VertexOffset + v].Color );
                    out.UV1.push_back( built.Streams[sm.VertexOffset + v].UV1 );
                }
            }
            const auto firstIndex = static_cast<uint32_t>( out.Indices.size() * 3 );
            for ( uint32_t t = 0; t < sm.IndexCount / 3; ++t )
            {
                const Index& tri = built.Indices[sm.IndexOffset / 3 + t];
                out.Indices.push_back( { remap[tri.V1], remap[tri.V2], remap[tri.V3] } );
            }
            Ser::SubmeshData data;
            data.Name         = sm.Name;
            data.VertexOffset = firstVertex;
            data.VertexCount  = static_cast<uint32_t>( out.StaticVertices.size() ) - firstVertex;
            data.IndexOffset  = firstIndex;
            data.IndexCount   = sm.IndexCount;
            data.Transform    = sm.Transform;
            data.BoundingBox  = sm.BoundingBox;
            out.Submeshes.push_back( std::move( data ) );
        }
        if ( input.BakeLODs )
            BakeMeshLODs( out );
        return Common::MakeSuccess( Ser::EncodeMeshBinary( out ) );
    }
} // namespace Desert::Editor
