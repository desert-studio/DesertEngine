#include <Engine/Core/HLODMeshBuilder.hpp>

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>
#include <Engine/Geometry/MeshSimplifyOperation.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Core
{
    namespace
    {
        // One source mesh in its own space: render vertices and, per section, its triangles (indices into
        // Vertices, not submesh-local).
        struct SourceMesh
        {
            std::vector<Desert::Vertex>             Vertices;
            std::vector<std::vector<Desert::Index>> Sections;
        };

        // The name a part is reported by: its mesh path, else its GUID, else its primitive.
        std::string PartName( const Assets::InstancedStaticMeshComponentSer& part )
        {
            if ( part.MeshPath.has_value() && !part.MeshPath->empty() )
                return *part.MeshPath;
            if ( part.MeshGuid.has_value() && !part.MeshGuid->empty() )
                return *part.MeshGuid;
            return part.Primitive.has_value() ? std::format( "primitive {}", static_cast<int>( *part.Primitive ) )
                                              : std::string( "a part naming no mesh" );
        }

        Common::ResultStr<SourceMesh> ReadSource( const Assets::InstancedStaticMeshComponentSer& part,
                                                  std::span<const Common::Utils::AssetRegistry>  registries )
        {
            SourceMesh source;
            if ( !part.MeshGuid.has_value() && !part.MeshPath.has_value() )
            {
                if ( !part.Primitive.has_value() )
                    return Common::MakeError<SourceMesh>( "a part names neither a mesh nor a primitive" );
                auto shape = Geometry::MakePrimitive( *part.Primitive );
                if ( !shape.has_value() )
                    return Common::MakeFormattedError<SourceMesh>( "{} has no geometry to merge",
                                                                   PartName( part ) );
                source.Vertices = std::move( shape->Vertices );
                source.Sections.push_back( std::move( shape->Indices ) );
                return Common::MakeSuccess( std::move( source ) );
            }

            const std::string          guidText = part.MeshGuid.value_or( std::string() );
            const std::string          path     = part.MeshPath.value_or( std::string() );
            Common::Content::AssetGuid guid;
            if ( !guidText.empty() )
            {
                auto parsed = Common::Content::AssetGuidFromText( guidText );
                if ( !parsed )
                    return Common::MakeFormattedError<SourceMesh>( "mesh '{}': {}", PartName( part ),
                                                                   parsed.GetError() );
                guid = parsed.GetValue();
            }
            const Common::Utils::AssetRegistryEntry* row = nullptr;
            for ( const auto& registry : registries )
                if ( ( row = registry.FindByGuidReference( guid, path ) ) != nullptr )
                    break;
            if ( row == nullptr )
                return Common::MakeFormattedError<SourceMesh>( "mesh '{}' is in no asset registry",
                                                               PartName( part ) );

            const auto file  = Common::AssetHandle::PathForStableKey( row->Key );
            auto       bytes = Assets::LoadMeshPlatformData( file );
            if ( !bytes )
                return Common::MakeFormattedError<SourceMesh>( "mesh '{}': {}", file.string(), bytes.GetError() );
            auto data = Assets::Serialization::ReadMeshAssetData( bytes.GetValue(), file.string() );
            if ( !data )
                return Common::MakeError<SourceMesh>( data.GetError() );
            auto render = Geometry::RenderFromMeshAssetData( data.GetValue() );
            if ( !render )
                return Common::MakeFormattedError<SourceMesh>( "mesh '{}': {}", file.string(), render.GetError() );

            const Geometry::RenderMeshData& mesh = render.GetValue();
            for ( const Desert::Submesh& submesh : mesh.Submeshes )
            {
                // Submesh-local indices made global, and the submesh's own transform baked, as the draw applies
                // it.
                const auto      base         = static_cast<uint32_t>( source.Vertices.size() );
                const glm::mat3 normalMatrix = glm::inverseTranspose( glm::mat3( submesh.Transform ) );
                for ( uint32_t v = 0; v < submesh.VertexCount; ++v )
                {
                    Desert::Vertex vertex = mesh.Vertices.at( submesh.VertexOffset + v );
                    vertex.Position       = glm::vec3( submesh.Transform * glm::vec4( vertex.Position, 1.0f ) );
                    vertex.Normal         = glm::normalize( normalMatrix * vertex.Normal );
                    vertex.Tangent        = glm::normalize( glm::mat3( submesh.Transform ) * vertex.Tangent );
                    vertex.Bitangent      = glm::normalize( glm::mat3( submesh.Transform ) * vertex.Bitangent );
                    source.Vertices.push_back( vertex );
                }
                std::vector<Desert::Index>& section = source.Sections.emplace_back();
                for ( uint32_t t = submesh.IndexOffset / 3; t < ( submesh.IndexOffset + submesh.IndexCount ) / 3;
                      ++t )
                {
                    const Desert::Index& local = mesh.Indices.at( t );
                    section.push_back( { base + local.V1, base + local.V2, base + local.V3 } );
                }
            }
            return Common::MakeSuccess( std::move( source ) );
        }
    } // namespace

    Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildHLODMesh( const Rules::HLODMeshRequest&                 request,
                   std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result = Assets::StaticMeshComponentSer;

        // Every material the merged mesh binds, first-seen order: the {GUID, path} text pair a slot states.
        std::vector<std::pair<std::string, std::string>> slots;
        const auto slotOf = [&slots]( const std::pair<std::string, std::string>& key ) -> std::size_t
        {
            const auto found = std::find( slots.begin(), slots.end(), key );
            if ( found != slots.end() )
                return static_cast<std::size_t>( found - slots.begin() );
            slots.push_back( key );
            return slots.size() - 1;
        };
        // Per output section (= slot): its vertices and its section-local triangles.
        std::vector<std::vector<Desert::Vertex>> sectionVertices;
        std::vector<std::vector<Desert::Index>>  sectionTriangles;

        for ( const Assets::InstancedStaticMeshComponentSer& part : request.Parts )
        {
            auto source = ReadSource( part, registries );
            if ( !source )
                return Common::MakeError<Result>( source.GetError() );
            const SourceMesh& mesh = source.GetValue();
            for ( std::size_t section = 0; section < mesh.Sections.size(); ++section )
            {
                std::pair<std::string, std::string> key;
                if ( part.MaterialGuids.has_value() && section < part.MaterialGuids->size() )
                    key.first = ( *part.MaterialGuids )[section];
                if ( part.MaterialPaths.has_value() && section < part.MaterialPaths->size() )
                    key.second = ( *part.MaterialPaths )[section];
                const std::size_t slot = slotOf( key );
                if ( sectionVertices.size() <= slot )
                {
                    sectionVertices.resize( slot + 1 );
                    sectionTriangles.resize( slot + 1 );
                }
                for ( const std::array<float, 16>& flat :
                      part.InstanceTransforms.value_or( std::vector<std::array<float, 16>>{} ) )
                {
                    glm::mat4 world;
                    std::memcpy( &world[0][0], flat.data(), sizeof( float ) * 16 );
                    const glm::mat3 linear       = glm::mat3( world );
                    const glm::mat3 normalMatrix = glm::inverseTranspose( linear );
                    // A mirroring instance flips the winding; swap two corners so the face keeps facing out.
                    const bool                   mirrored = glm::determinant( linear ) < 0.0f;
                    std::vector<Desert::Vertex>& vertices = sectionVertices[slot];
                    std::map<uint32_t, uint32_t> local;
                    for ( const Desert::Index& triangle : mesh.Sections[section] )
                    {
                        std::array<uint32_t, 3> corners{ triangle.V1, triangle.V2, triangle.V3 };
                        for ( uint32_t& corner : corners )
                        {
                            const auto [where, fresh] =
                                 local.emplace( corner, static_cast<uint32_t>( vertices.size() ) );
                            if ( fresh )
                            {
                                Desert::Vertex vertex = mesh.Vertices.at( corner );
                                vertex.Position       = glm::vec3( world * glm::vec4( vertex.Position, 1.0f ) );
                                vertex.Normal         = glm::normalize( normalMatrix * vertex.Normal );
                                vertex.Tangent        = glm::normalize( linear * vertex.Tangent );
                                vertex.Bitangent      = glm::normalize( linear * vertex.Bitangent );
                                vertices.push_back( vertex );
                            }
                            corner = where->second;
                        }
                        if ( mirrored )
                            std::swap( corners[1], corners[2] );
                        sectionTriangles[slot].push_back( { corners[0], corners[1], corners[2] } );
                    }
                }
            }
        }

        // The merged render mesh: one submesh per slot, each with its own contiguous vertex range.
        Geometry::RenderMeshData render;
        for ( std::size_t slot = 0; slot < sectionVertices.size(); ++slot )
        {
            if ( sectionTriangles[slot].empty() )
                continue;
            Desert::Submesh submesh{};
            submesh.Name         = std::format( "Section{}", slot );
            submesh.VertexOffset = static_cast<uint32_t>( render.Vertices.size() );
            submesh.VertexCount  = static_cast<uint32_t>( sectionVertices[slot].size() );
            submesh.IndexOffset  = static_cast<uint32_t>( render.Indices.size() * 3 );
            submesh.IndexCount   = static_cast<uint32_t>( sectionTriangles[slot].size() * 3 );
            submesh.Transform    = glm::mat4( 1.0f );
            render.Vertices.insert( render.Vertices.end(), sectionVertices[slot].begin(),
                                    sectionVertices[slot].end() );
            render.Indices.insert( render.Indices.end(), sectionTriangles[slot].begin(),
                                   sectionTriangles[slot].end() );
            render.Submeshes.push_back( std::move( submesh ) );
            render.SubmeshMaterialIds.push_back( static_cast<int>( slot ) );
        }
        if ( render.Indices.empty() )
            return Common::MakeError<Result>( "the parts hold no triangles to merge" );

        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the merged mesh does not weld: {}", imported.GetError() );
        Geometry::DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );

        if ( request.Type == HLODLayerType::MeshSimplify )
        {
            const int target =
                 std::max( 1, static_cast<int>( std::ceil( static_cast<double>( mesh.TriangleCount() ) *
                                                           request.TrianglePercent ) ) );
            // Recomputes the normals too: the collapses moved vertices.
            Geometry::SimplifyToTriangleCount( mesh, target );
        }

        Assets::StaticMeshComponentSer block;
        block.EditMesh = Geometry::ToSerialized( mesh );
        // Slots by MaterialID, which is the slot index; written only when some slot names a material.
        const bool named = std::any_of( slots.begin(), slots.end(), []( const auto& slot )
                                        { return !slot.first.empty() || !slot.second.empty(); } );
        if ( named )
        {
            block.MaterialGuids = std::vector<std::string>{};
            block.MaterialPaths = std::vector<std::string>{};
            for ( const auto& [guid, path] : slots )
            {
                block.MaterialGuids->push_back( guid );
                block.MaterialPaths->push_back( path );
            }
        }
        return Common::MakeSuccess( std::move( block ) );
    }

    namespace
    {
        // The tile a landscape HLOD request names, read once for both its mesh and its baked material's area:
        // the decoded heights and the frame they are laid out by (the root record's composed world position).
        struct LandscapeHLODTileInput
        {
            World::Landscape::LandscapeTileData Heights;
            World::Landscape::LandscapeFrame    Frame;
        };

        Common::ResultStr<LandscapeHLODTileInput>
        ReadLandscapeHLODTile( const Rules::LandscapeHLODRequest& request )
        {
            using Result        = LandscapeHLODTileInput;
            namespace Landscape = World::Landscape;
            if ( request.Tile >= request.Records.size() || request.World.size() != request.Records.size() )
                return Common::MakeError<Result>( "the landscape tile request names no record of its records" );
            const Assets::EntityData& tile = request.Records[request.Tile];
            Common::Json::Issues      issues;
            const auto tileBlock = Rules::Detail::BlockOf( tile, Rules::kLandscapeTileComponent, issues );
            if ( !tileBlock.has_value() )
                return Common::MakeError<Result>( "the record has no readable LandscapeTile block" );
            Common::UUID rootId;
            if ( !Rules::Detail::ReadReference( *tileBlock, "Landscape", rootId, issues ) || rootId.IsNull() )
                return Common::MakeError<Result>( "the tile names no landscape root" );
            const auto root =
                 std::find_if( request.Records.begin(), request.Records.end(),
                               [&]( const Assets::EntityData& record ) { return record.id == rootId; } );
            if ( root == request.Records.end() )
                return Common::MakeFormattedError<Result>( "the tile's landscape root {} is not in the world",
                                                           static_cast<std::uint64_t>( rootId ) );
            const std::size_t rootIndex = static_cast<std::size_t>( root - request.Records.begin() );
            const auto        rootBlock = Rules::Detail::BlockOf( *root, Rules::kLandscapeRootComponent, issues );
            if ( !rootBlock.has_value() )
                return Common::MakeError<Result>( "the tile's root has no readable Landscape block" );

            Landscape::LandscapeRoot frameRoot;
            frameRoot.Origin = glm::vec3( request.World[rootIndex][3] );
            rootBlock->ReadInto( "QuadsPerTile", frameRoot.QuadsPerTile, issues );
            rootBlock->ReadInto( "SpacingCm", frameRoot.SpacingCm, issues );
            rootBlock->ReadInto( "ZScale", frameRoot.ZScale, issues );
            if ( auto valid = Landscape::ValidateLandscapeRoot( frameRoot ); !valid )
                return Common::MakeError<Result>( "the tile's root: " + valid.GetError() );
            std::int32_t tileX = 0;
            std::int32_t tileZ = 0;
            std::string  heightFile;
            tileBlock->ReadInto( "TileX", tileX, issues );
            tileBlock->ReadInto( "TileZ", tileZ, issues );
            tileBlock->ReadInto( "HeightFile", heightFile, issues );
            if ( !issues.empty() )
                return Common::MakeFormattedError<Result>(
                     "the tile or its root does not read: {} wants {}, holds {}", issues.front().Path,
                     issues.front().Expected, issues.front().Found );

            // The loader's own read (ComponentRegistry's LandscapeTile load): the record's content key, resolved
            // the same way, so the stand-in and the near tile read the same file.
            auto decoded = Landscape::ReadLandscapeTileByKey( heightFile );
            if ( !decoded )
                return Common::MakeFormattedError<Result>( "the tile's height file '{}': {}", heightFile,
                                                           decoded.GetError() );
            Landscape::LandscapeTileData heights = decoded.ExtractValue();
            if ( auto matches = Landscape::CheckTileMatchesRoot( heights, frameRoot ); !matches )
                return Common::MakeError<Result>( matches.GetError() );
            const Landscape::LandscapeFrame frame = Landscape::LandscapeTileFrame( frameRoot, tileX, tileZ );
            return Common::MakeSuccess( Result{ std::move( heights ), frame } );
        }
    } // namespace

    Common::ResultStr<LandscapeHLODTileArea> LandscapeHLODTileAreaOf( const Rules::LandscapeHLODRequest& request )
    {
        auto input = ReadLandscapeHLODTile( request );
        if ( !input )
            return Common::MakeError<LandscapeHLODTileArea>( input.GetError() );
        const auto& tileId = request.Records[request.Tile].id;
        if ( !tileId )
            return Common::MakeError<LandscapeHLODTileArea>( "landscape HLOD tile record has no id" );
        const LandscapeHLODTileInput& tile = input.GetValue();
        return Common::MakeSuccess( LandscapeHLODTileArea{
             .Tile    = *tileId,
             .OriginX = tile.Frame.OriginX,
             .OriginZ = tile.Frame.OriginZ,
             .SizeX   = static_cast<float>( tile.Heights.SamplesX() - 1 ) * tile.Frame.SpacingCm,
             .SizeZ   = static_cast<float>( tile.Heights.SamplesZ() - 1 ) * tile.Frame.SpacingCm,
        } );
    }

    Common::ResultStr<Assets::StaticMeshComponentSer>
    BuildLandscapeHLODMesh( const Rules::LandscapeHLODRequest& request )
    {
        using Result        = Assets::StaticMeshComponentSer;
        namespace Landscape = World::Landscape;
        auto input          = ReadLandscapeHLODTile( request );
        if ( !input )
            return Common::MakeError<Result>( input.GetError() );
        const LandscapeHLODTileInput        tile    = input.ExtractValue();
        const Landscape::LandscapeTileData& heights = tile.Heights;
        const Landscape::LandscapeFrame&    frame   = tile.Frame;

        // The visibility layer, when the tile has one: its holes are not drawn by the terrain either.
        const std::vector<std::uint8_t>* visibility = nullptr;
        for ( const Landscape::LandscapeWeightLayer& layer : heights.WeightLayers() )
            if ( layer.Name == Landscape::kLandscapeVisibilityLayerName )
                visibility = &layer.Weights;

        const std::uint32_t samplesX = heights.SamplesX();
        const std::uint32_t samplesZ = heights.SamplesZ();
        const auto          heightAt = [&]( std::uint32_t x, std::uint32_t z )
        { return frame.BaseY + Landscape::LandscapeHeightCm( heights.Sample( x, z ), frame.ZScale ); };
        const auto isHole = [&]( std::uint32_t x, std::uint32_t z )
        {
            const std::size_t at = ( static_cast<std::size_t>( z ) * samplesX ) + x;
            return visibility != nullptr && at < visibility->size() &&
                   Landscape::LandscapeWeightIsHole( ( *visibility )[at] );
        };

        // THE LANDSCAPE LOD THE HLOD IS SEEN AT (UE ComputeRequiredLandscapeLOD, AutomaticLOD policy): the tile's
        // bounding sphere projected from MinVisibleDistance through UE's fixed view for it (90 degree FOV,
        // 1920x1080: ComputeBoundsScreenSize's ScreenMultiple = 0.5 * 1920 / 1080), against GetLODScreenSizeArray
        // over UE's landscape defaults (LOD0ScreenSize 0.5, LOD0DistributionSetting 1.25, CeilLogTwo(quads + 1)
        // LODs). The first LOD whose screen size the tile does not exceed is kept; LOD n keeps every 2^n-th
        // sample.
        float lowest  = heightAt( 0, 0 );
        float highest = lowest;
        for ( std::uint32_t z = 0; z < samplesZ; ++z )
            for ( std::uint32_t x = 0; x < samplesX; ++x )
            {
                lowest  = std::min( lowest, heightAt( x, z ) );
                highest = std::max( highest, heightAt( x, z ) );
            }
        const float     sizeX           = static_cast<float>( samplesX - 1 ) * frame.SpacingCm;
        const float     sizeZ           = static_cast<float>( samplesZ - 1 ) * frame.SpacingCm;
        const float     radius          = 0.5f * std::sqrt( ( sizeX * sizeX ) + ( sizeZ * sizeZ ) +
                                                            ( ( highest - lowest ) * ( highest - lowest ) ) );
        constexpr float kScreenMultiple = 0.5f * 1920.0f / 1080.0f;
        const float   screenSize = 2.0f * kScreenMultiple * radius / std::max( 1.0f, request.MinVisibleDistance );
        std::uint32_t lodCount   = 0;
        while ( ( 1u << lodCount ) < std::max( samplesX, samplesZ ) )
            ++lodCount;
        std::uint32_t lod       = 0;
        float         lodScreen = 0.5f;
        for ( ; lod + 1 < lodCount; ++lod, lodScreen /= 1.25f )
            if ( screenSize > lodScreen )
                break;
        const std::uint32_t stride = 1u << lod;
        const auto          rows   = [stride]( std::uint32_t samples )
        {
            std::vector<std::uint32_t> kept;
            for ( std::uint32_t at = 0; at + 1 < samples; at += stride )
                kept.push_back( at );
            kept.push_back( samples - 1 );
            return kept;
        };
        const std::vector<std::uint32_t> xs = rows( samplesX );
        const std::vector<std::uint32_t> zs = rows( samplesZ );

        Geometry::RenderMeshData  render;
        std::vector<std::int64_t> vertexOf( static_cast<std::size_t>( samplesX ) * samplesZ, -1 );
        const auto                vertexAt = [&]( std::uint32_t x, std::uint32_t z )
        {
            std::int64_t& slot = vertexOf[( static_cast<std::size_t>( z ) * samplesX ) + x];
            if ( slot >= 0 )
                return static_cast<std::uint32_t>( slot );
            const std::uint32_t x0 = x == 0 ? 0 : x - 1;
            const std::uint32_t x1 = std::min( x + 1, samplesX - 1 );
            const std::uint32_t z0 = z == 0 ? 0 : z - 1;
            const std::uint32_t z1 = std::min( z + 1, samplesZ - 1 );
            const float         dX =
                 ( heightAt( x1, z ) - heightAt( x0, z ) ) / ( static_cast<float>( x1 - x0 ) * frame.SpacingCm );
            const float dZ =
                 ( heightAt( x, z1 ) - heightAt( x, z0 ) ) / ( static_cast<float>( z1 - z0 ) * frame.SpacingCm );
            Desert::Vertex vertex{};
            vertex.Position =
                 glm::vec3( frame.OriginX + ( static_cast<float>( x ) * frame.SpacingCm ), heightAt( x, z ),
                            frame.OriginZ + ( static_cast<float>( z ) * frame.SpacingCm ) );
            vertex.Normal    = glm::normalize( glm::vec3( -dX, 1.0f, -dZ ) );
            vertex.Tangent   = glm::normalize( glm::vec3( 1.0f, dX, 0.0f ) );
            vertex.Bitangent = glm::normalize( glm::cross( vertex.Normal, vertex.Tangent ) );
            vertex.TexCoord  = glm::vec2( static_cast<float>( x ) / static_cast<float>( samplesX - 1 ),
                                          static_cast<float>( z ) / static_cast<float>( samplesZ - 1 ) );
            slot             = static_cast<std::int64_t>( render.Vertices.size() );
            render.Vertices.push_back( vertex );
            return static_cast<std::uint32_t>( slot );
        };
        using Sample = std::pair<std::uint32_t, std::uint32_t>;
        std::vector<Sample> ring;
        for ( std::size_t j = 0; j + 1 < zs.size(); ++j )
            for ( std::size_t i = 0; i + 1 < xs.size(); ++i )
            {
                const std::uint32_t x0 = xs[i], x1 = xs[i + 1], z0 = zs[j], z1 = zs[j + 1];
                if ( isHole( x0, z0 ) || isHole( x1, z0 ) || isHole( x0, z1 ) || isHole( x1, z1 ) )
                    continue;
                const bool west  = i == 0;
                const bool east  = i + 2 == xs.size();
                const bool south = j == 0;
                const bool north = j + 2 == zs.size();
                if ( !( ( ( west || east ) && z1 - z0 > 1 ) || ( ( south || north ) && x1 - x0 > 1 ) ) )
                {
                    // Facing +Y: (x0,z0) -> (x0,z1) -> (x1,z0) and (x1,z0) -> (x0,z1) -> (x1,z1).
                    render.Indices.push_back( { vertexAt( x0, z0 ), vertexAt( x0, z1 ), vertexAt( x1, z0 ) } );
                    render.Indices.push_back( { vertexAt( x1, z0 ), vertexAt( x0, z1 ), vertexAt( x1, z1 ) } );
                    continue;
                }
                // A cell on the tile's border: its border side keeps every sample (the near tile's edge exactly,
                // no T-junction crack), fanned from the cell's centre sample in the same winding as above - west
                // up, north east, east down, south west.
                ring.clear();
                for ( std::uint32_t z = z0; z < z1; z += west ? 1 : z1 - z0 )
                    ring.emplace_back( x0, z );
                for ( std::uint32_t x = x0; x < x1; x += north ? 1 : x1 - x0 )
                    ring.emplace_back( x, z1 );
                for ( std::uint32_t z = z1; z > z0; z -= east ? 1 : z1 - z0 )
                    ring.emplace_back( x1, z );
                for ( std::uint32_t x = x1; x > x0; x -= south ? 1 : x1 - x0 )
                    ring.emplace_back( x, z0 );
                const Sample centre{ ( x0 + x1 ) / 2, ( z0 + z1 ) / 2 };
                for ( std::size_t k = 0; k < ring.size(); ++k )
                {
                    const Sample& p = ring[k];
                    const Sample& q = ring[( k + 1 ) % ring.size()];
                    const auto    d = []( std::uint32_t a, std::uint32_t b )
                    { return static_cast<std::int64_t>( a ) - static_cast<std::int64_t>( b ); };
                    const std::int64_t cross = ( d( p.first, centre.first ) * d( q.second, centre.second ) ) -
                                               ( d( p.second, centre.second ) * d( q.first, centre.first ) );
                    if ( cross == 0 )
                        continue; // the centre lies on this border segment: no area to cover
                    render.Indices.push_back( { vertexAt( centre.first, centre.second ),
                                                vertexAt( p.first, p.second ), vertexAt( q.first, q.second ) } );
                }
            }
        if ( render.Indices.empty() )
            return Common::MakeError<Result>( "the tile is all hole; it draws nothing and needs no HLOD" );
        Desert::Submesh submesh{};
        submesh.Name        = "Landscape";
        submesh.VertexCount = static_cast<std::uint32_t>( render.Vertices.size() );
        submesh.IndexCount  = static_cast<std::uint32_t>( render.Indices.size() * 3 );
        submesh.Transform   = glm::mat4( 1.0f );
        render.Submeshes.push_back( std::move( submesh ) );
        render.SubmeshMaterialIds.push_back( 0 );

        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the tile mesh does not weld: {}", imported.GetError() );
        Assets::StaticMeshComponentSer block;
        block.EditMesh = Geometry::ToSerialized( imported.ExtractValue().Mesh );
        return Common::MakeSuccess( std::move( block ) );
    }

    HLODBuilders MakeHLODBuilders( std::span<const Common::Utils::AssetRegistry> registries )
    {
        return HLODBuilders{
             .Mesh = [registries]( const Rules::HLODMeshRequest& request )
             { return BuildHLODMesh( request, registries ); },
             .Landscape = []( const Rules::LandscapeHLODRequest& request )
             { return BuildLandscapeHLODMesh( request ); },
        };
    }
} // namespace Desert::Core
