#include <Landscape/Public/LandscapeSplineHLODMesh.hpp>
#include <Landscape/Public/LandscapeSplineMeshes.hpp>

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/MeshAssetArrays.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>
#include <LandscapeCore/Public/LandscapeSplinesJson.hpp>
#include <LandscapeCore/Public/LandscapeWorldPartition.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// THE LANDSCAPE SPLINE'S HLOD BUILDER (UE: the spline's USplineMeshComponents built into their cell's HLOD by the
// Landscape module): registered with the engine's HLOD builder registry through ModuleHooks().HLODBuilders, so
// Engine's HLODMeshBuilder never names the spline meshes.

namespace Desert::Landscape
{
    Common::ResultStr<std::optional<Assets::StaticMeshComponentSer>>
    BuildLandscapeSplineHLODMesh( const Core::Rules::LandscapeSplineHLODRequest& request,
                                  std::span<const Common::Utils::AssetRegistry>  registries )
    {
        using Result = std::optional<Assets::StaticMeshComponentSer>;
        if ( request.Spline >= request.Records.size() )
            return Common::MakeFormattedError<Result>( "record {} is not in the cell", request.Spline );
        Common::Json::Issues issues;
        const auto           block = Core::Rules::Detail::BlockOf(
             request.Records[request.Spline], ::Desert::LandscapeCore::kLandscapeSplineComponent, issues );
        if ( !block.has_value() )
            return Common::MakeError<Result>( "the record has no LandscapeSpline block" );
        World::Landscape::LandscapeSplines splines;
        World::Landscape::ReadLandscapeSplinesJson( *block, splines, issues );
        if ( !issues.empty() )
            return Common::MakeFormattedError<Result>( "its LandscapeSpline block does not read ({} issue(s))",
                                                       issues.size() );

        // One source per distinct entry, kept alive while the segments read it (the sources are spans).
        struct Backing
        {
            std::vector<Desert::Vertex>  Vertices;
            std::vector<Desert::Index>   Indices;
            std::vector<Desert::Submesh> Submeshes;
        };
        std::map<std::string, std::unique_ptr<Backing>> backings;
        const auto                                      sourceOf =
             [&]( const World::Landscape::LandscapeSplineMeshEntry& entry ) -> Common::ResultStr<const Backing*>
        {
            const std::string key = entry.Primitive
                                         ? std::format( "primitive {}", static_cast<int>( *entry.Primitive ) )
                                         : std::format( "mesh {}", static_cast<uint64_t>( entry.Mesh ) );
            if ( const auto found = backings.find( key ); found != backings.end() )
                return Common::MakeSuccess( static_cast<const Backing*>( found->second.get() ) );
            auto backing = std::make_unique<Backing>();
            if ( entry.Primitive )
            {
                auto shape = Geometry::MakePrimitive( *entry.Primitive );
                if ( !shape.has_value() )
                    return Common::MakeFormattedError<const Backing*>( "{} has no geometry", key );
                Desert::Submesh whole{};
                whole.Name        = key;
                whole.VertexCount = static_cast<uint32_t>( shape->Vertices.size() );
                whole.IndexCount  = static_cast<uint32_t>( shape->Indices.size() * 3 );
                whole.Transform   = glm::mat4( 1.0f );
                backing->Vertices = std::move( shape->Vertices );
                backing->Indices  = std::move( shape->Indices );
                backing->Submeshes.push_back( std::move( whole ) );
            }
            else
            {
                const Common::Utils::AssetRegistryEntry* row = nullptr;
                for ( const auto& registry : registries )
                    if ( ( row = registry.FindByHandle( static_cast<uint64_t>( entry.Mesh ) ) ) != nullptr )
                        break;
                if ( row == nullptr )
                    return Common::MakeFormattedError<const Backing*>( "{} is in no asset registry", key );
                const auto file  = Common::AssetHandle::PathForStableKey( row->Key );
                auto       bytes = Assets::LoadMeshPlatformData( file );
                if ( !bytes )
                    return Common::MakeFormattedError<const Backing*>( "mesh '{}': {}", file.string(),
                                                                       bytes.GetError() );
                auto data = Assets::Serialization::ReadMeshAssetData( bytes.GetValue(), file.string() );
                if ( !data )
                    return Common::MakeError<const Backing*>( data.GetError() );
                auto render = Geometry::RenderFromMeshAssetData( data.GetValue() );
                if ( !render )
                    return Common::MakeFormattedError<const Backing*>( "mesh '{}': {}", file.string(),
                                                                       render.GetError() );
                Geometry::RenderMeshData mesh = render.ExtractValue();
                backing->Vertices             = std::move( mesh.Vertices );
                backing->Indices              = std::move( mesh.Indices );
                backing->Submeshes            = std::move( mesh.Submeshes );
            }
            const Backing* kept = backing.get();
            backings.emplace( key, std::move( backing ) );
            return Common::MakeSuccess( static_cast<const Backing*>( kept ) );
        };

        // Every segment's bent meshes appended into one render mesh, world cm (the HLOD record's transform is the
        // identity). One section: the spline meshes draw with the default surface in the HLOD.
        Geometry::RenderMeshData render;
        for ( std::size_t s = 0; s < splines.Segments.size(); ++s )
        {
            std::vector<World::Landscape::LandscapeSplineMeshSource> sources;
            for ( const World::Landscape::LandscapeSplineMeshEntry& entry : splines.Segments[s].SplineMeshes )
            {
                auto source = sourceOf( entry );
                if ( !source )
                    return Common::MakeFormattedError<Result>( "segment {}: {}", s, source.GetError() );
                const Backing* backing = source.GetValue();
                sources.push_back( { backing->Vertices, backing->Indices, backing->Submeshes } );
            }
            if ( sources.empty() )
                continue;
            for ( const World::Landscape::LandscapeSplineDeformedMesh& bent :
                  World::Landscape::BuildLandscapeSplineSegmentMeshes( splines, s, sources ) )
            {
                const auto vertexBase = static_cast<uint32_t>( render.Vertices.size() );
                const auto indexBase  = static_cast<uint32_t>( render.Indices.size() * 3 );
                for ( Desert::Submesh part : bent.Submeshes )
                {
                    part.VertexOffset += vertexBase;
                    part.IndexOffset += indexBase;
                    render.Submeshes.push_back( std::move( part ) );
                    render.SubmeshMaterialIds.push_back( 0 );
                }
                render.Vertices.insert( render.Vertices.end(), bent.Vertices.begin(), bent.Vertices.end() );
                render.Indices.insert( render.Indices.end(), bent.Indices.begin(), bent.Indices.end() );
            }
        }
        if ( render.Indices.empty() )
            return Common::MakeSuccess( Result() );
        auto imported = Geometry::DynamicMeshFromRenderMesh( render );
        if ( !imported )
            return Common::MakeFormattedError<Result>( "the spline meshes do not weld: {}", imported.GetError() );
        Assets::StaticMeshComponentSer out;
        out.EditMesh = Geometry::ToSerialized( imported.ExtractValue().Mesh );
        return Common::MakeSuccess( Result( std::move( out ) ) );
    }

    void AddLandscapeSplineHLODBuilder( Core::Rules::HLODBuilderRegistry& builders )
    {
        builders.SetLandscapeSplineBuilder( std::make_shared<Core::Rules::HLODBuilderLandscapeSplines>(
             []( const Core::Rules::LandscapeSplineHLODRequest& request,
                 std::span<const Common::Utils::AssetRegistry>  registries )
             { return BuildLandscapeSplineHLODMesh( request, registries ); } ) );
    }
} // namespace Desert::Landscape
