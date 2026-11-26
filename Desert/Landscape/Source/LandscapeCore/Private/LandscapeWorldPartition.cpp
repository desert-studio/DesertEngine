#include <LandscapeCore/Public/LandscapeWorldPartition.hpp>

#include <Engine/Core/Serialize/WorldPartitionActivationCost.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <LandscapeCore/Public/LandscapeLayout.hpp>
#include <LandscapeCore/Public/LandscapeSplinesJson.hpp>

#include <Engine/Assets/AssetGuidRef.hpp>

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Desert::LandscapeCore
{
    namespace
    {
        namespace Rules     = Desert::Core::Rules;
        namespace Landscape = Desert::World::Landscape;

        // A tile keeps its root (the frame) and its own coordinate. A tile whose block does not read is kept with
        // no root: it has no place, and is listed so (WorldPartitionPlan::UnplacedFramedRecords).
        bool DescribeTile( const Common::Json::Node* block, Rules::DescriptorFacts& facts,
                           Common::Json::Issues& issues )
        {
            if ( block == nullptr )
                return true;
            Common::UUID root;
            if ( Rules::Detail::ReadReference( *block, "Landscape", root, issues ) && !root.IsNull() )
                facts.Frame = static_cast<std::uint64_t>( root );
            std::int32_t tileX = 0;
            std::int32_t tileZ = 0;
            block->ReadInto( "TileX", tileX, issues );
            block->ReadInto( "TileZ", tileZ, issues );
            facts.Values = { { "TileX", static_cast<double>( tileX ) },
                             { "TileZ", static_cast<double>( tileZ ) } };
            return true;
        }

        // A tile's rectangle is its root's: the root's world position is the origin, its tiling the grid.
        std::optional<Common::Math::AABB> TileFootprint( const Rules::DescriptorFacts& own,
                                                         const Rules::DescriptorFacts* frame,
                                                         const glm::mat4&              frameWorld )
        {
            if ( frame == nullptr )
                return std::nullopt;
            Landscape::LandscapeRoot root;
            root.Origin       = glm::vec3( frameWorld[3] );
            root.QuadsPerTile = static_cast<std::uint32_t>(
                 frame->ValueOr( "QuadsPerTile", static_cast<double>( Landscape::kLandscapeDefaultTileQuads ) ) );
            root.SpacingCm =
                 static_cast<float>( frame->ValueOr( "SpacingCm", Landscape::kLandscapeDefaultSpacingCm ) );
            root.ZScale = static_cast<float>( frame->ValueOr( "ZScale", Landscape::kLandscapeDefaultZScale ) );
            if ( !Landscape::ValidateLandscapeRoot( root ) )
                return std::nullopt;
            const Landscape::LandscapeTileRect rect =
                 Landscape::LandscapeTileBounds( root, static_cast<std::int32_t>( own.ValueOr( "TileX", 0.0 ) ),
                                                 static_cast<std::int32_t>( own.ValueOr( "TileZ", 0.0 ) ) );
            Common::Math::AABB box;
            box.Min = glm::vec3( rect.MinX, rect.MinY, rect.MinZ );
            box.Max = glm::vec3( rect.MaxX, rect.MaxY, rect.MaxZ );
            return box;
        }

        // A spline states the world box around its band; one whose block does not read, or that has no control
        // point, keeps no facts and is placed by its position as any record.
        bool DescribeSpline( const Common::Json::Node* block, Rules::DescriptorFacts& facts,
                             Common::Json::Issues& issues )
        {
            if ( block == nullptr )
                return false;
            Landscape::LandscapeSplines splines;
            Landscape::ReadLandscapeSplinesJson( *block, splines, issues );
            facts.WorldBox = Landscape::LandscapeSplinesBounds( splines );
            return facts.WorldBox.has_value();
        }

        std::optional<Common::Math::AABB> SplineFootprint( const Rules::DescriptorFacts& own,
                                                           const Rules::DescriptorFacts*, const glm::mat4& )
        {
            return own.WorldBox;
        }

        // A root states its tiling for its tiles; it places nothing itself (its own footprint is its position).
        bool DescribeRoot( const Common::Json::Node* block, Rules::DescriptorFacts& facts,
                           Common::Json::Issues& issues )
        {
            if ( block == nullptr )
                return false;
            std::uint32_t quads   = Landscape::kLandscapeDefaultTileQuads;
            float         spacing = Landscape::kLandscapeDefaultSpacingCm;
            float         zScale  = Landscape::kLandscapeDefaultZScale;
            block->ReadInto( "QuadsPerTile", quads, issues );
            block->ReadInto( "SpacingCm", spacing, issues );
            block->ReadInto( "ZScale", zScale, issues );
            facts.Values = { { "QuadsPerTile", static_cast<double>( quads ) },
                             { "SpacingCm", static_cast<double>( spacing ) },
                             { "ZScale", static_cast<double>( zScale ) } };
            return true;
        }
        // The key the saved-HLOD checksum hashes for one {Guid, Path} reference: the GUID text, else the path.
        [[nodiscard]] std::string ContentKeyOf( const Assets::AssetGuidRef& reference )
        {
            return !reference.Guid.empty() ? reference.Guid : reference.Path;
        }

        // A tile's HLOD inputs (Rules::HLODBuilderComponent::Inputs): what ReadLandscapeTileFrame reads, the root's
        // place left to its TRS chain (Placed) so no composed matrix enters the text.
        Common::ResultStr<Rules::HLODModuleInputs> TileInputs( std::span<const Assets::EntityData> records,
                                                               std::span<const glm::mat4> world, std::size_t index )
        {
            auto read = ReadLandscapeTileFrame( records, world, index );
            if ( !read )
                return Common::MakeError<Rules::HLODModuleInputs>( read.GetError() );
            const LandscapeTileRead& tile = read.GetValue();
            Rules::HLODModuleInputs  inputs;
            inputs.Text = std::format( "tile quads {} spacing {:a} z {:a} at {} {} height {} material {} layers",
                                       tile.Root.QuadsPerTile, tile.Root.SpacingCm, tile.Root.ZScale, tile.TileX,
                                       tile.TileZ, tile.HeightFile, tile.MaterialKey );
            for ( const std::string& layer : tile.LayerKeys )
                inputs.Text += " " + layer;
            inputs.Placed = { tile.RootIndex };
            inputs.Content.push_back( tile.HeightFile );
            if ( !tile.MaterialKey.empty() )
                inputs.Content.push_back( tile.MaterialKey );
            for ( const std::string& layer : tile.LayerKeys )
                if ( !layer.empty() )
                    inputs.Content.push_back( layer );
            return Common::MakeSuccess( std::move( inputs ) );
        }

        // A spline's HLOD inputs: its splines as the spline builder reads them (ReadLandscapeSplinesJson), written
        // back canonical, and the static meshes they lay (by asset handle).
        Common::ResultStr<Rules::HLODModuleInputs> SplineInputs( std::span<const Assets::EntityData> records,
                                                                 std::span<const glm::mat4>, std::size_t index )
        {
            Common::Json::Issues issues;
            const auto block = Rules::Detail::BlockOf( records[index], kLandscapeSplineComponent, issues );
            if ( !block.has_value() )
                return Common::MakeError<Rules::HLODModuleInputs>( "the record has no readable LandscapeSpline block" );
            Landscape::LandscapeSplines splines;
            Landscape::ReadLandscapeSplinesJson( *block, splines, issues );
            if ( !issues.empty() )
                return Common::MakeFormattedError<Rules::HLODModuleInputs>(
                     "its LandscapeSpline block does not read: {} wants {}, holds {}", issues.front().Path,
                     issues.front().Expected, issues.front().Found );
            Rules::HLODModuleInputs inputs;
            inputs.Text = "spline " + Common::Json::Write( Common::Json::Value( Landscape::WriteLandscapeSplinesJson( splines ) ) );
            for ( const Landscape::LandscapeSplineSegment& segment : splines.Segments )
                for ( const Landscape::LandscapeSplineMeshEntry& entry : segment.SplineMeshes )
                    if ( !entry.Mesh.IsNull() )
                        inputs.Content.push_back(
                             std::format( "handle:{}", static_cast<std::uint64_t>( entry.Mesh ) ) );
            return Common::MakeSuccess( std::move( inputs ) );
        }
    } // namespace

    Common::ResultStr<LandscapeTileRead> ReadLandscapeTileFrame( std::span<const Assets::EntityData> records,
                                                                 std::span<const glm::mat4> world, std::size_t index )
    {
        using Result = LandscapeTileRead;
        if ( index >= records.size() || world.size() != records.size() )
            return Common::MakeError<Result>( "the landscape tile names no record of its records" );
        const Assets::EntityData& tile = records[index];
        Common::Json::Issues      issues;
        const auto                tileBlock = Rules::Detail::BlockOf( tile, kLandscapeTileComponent, issues );
        if ( !tileBlock.has_value() )
            return Common::MakeError<Result>( "the record has no readable LandscapeTile block" );
        Common::UUID rootId;
        if ( !Rules::Detail::ReadReference( *tileBlock, "Landscape", rootId, issues ) || rootId.IsNull() )
            return Common::MakeError<Result>( "the tile names no landscape root" );
        const auto root = std::find_if( records.begin(), records.end(),
                                        [&]( const Assets::EntityData& record ) { return record.id == rootId; } );
        if ( root == records.end() )
            return Common::MakeFormattedError<Result>( "the tile's landscape root {} is not in the world",
                                                       static_cast<std::uint64_t>( rootId ) );
        Result read;
        read.RootIndex       = static_cast<std::size_t>( root - records.begin() );
        const auto rootBlock = Rules::Detail::BlockOf( *root, kLandscapeRootComponent, issues );
        if ( !rootBlock.has_value() )
            return Common::MakeError<Result>( "the tile's root has no readable Landscape block" );
        read.Root.Origin = glm::vec3( world[read.RootIndex][3] );
        rootBlock->ReadInto( "QuadsPerTile", read.Root.QuadsPerTile, issues );
        rootBlock->ReadInto( "SpacingCm", read.Root.SpacingCm, issues );
        rootBlock->ReadInto( "ZScale", read.Root.ZScale, issues );
        if ( auto valid = Landscape::ValidateLandscapeRoot( read.Root ); !valid )
            return Common::MakeError<Result>( "the tile's root: " + valid.GetError() );
        std::vector<Assets::AssetGuidRef> layers;
        rootBlock->ReadInto( "Layers", layers, issues );
        for ( const Assets::AssetGuidRef& layer : layers )
            read.LayerKeys.push_back( ContentKeyOf( layer ) );
        if ( const auto look = Rules::Detail::BlockOf( *root, kLandscapeMaterialComponent, issues ); look.has_value() )
        {
            Assets::AssetGuidRef material;
            look->ReadInto( "Material", material, issues );
            read.MaterialKey = ContentKeyOf( material );
        }
        tileBlock->ReadInto( "TileX", read.TileX, issues );
        tileBlock->ReadInto( "TileZ", read.TileZ, issues );
        tileBlock->ReadInto( "HeightFile", read.HeightFile, issues );
        if ( !issues.empty() )
            return Common::MakeFormattedError<Result>( "the tile or its root does not read: {} wants {}, holds {}",
                                                       issues.front().Path, issues.front().Expected,
                                                       issues.front().Found );
        return Common::MakeSuccess( std::move( read ) );
    }

    void StartupModule()
    {
        static const bool kRegistered = []
        {
            // Loading (Rules::ComponentLoadingRows). A root owns the FRAME every tile is placed by, not a place of
            // its own; UE loads ALandscape the same way (not spatially loaded) and streams the proxies. The root's
            // look travels with the root: every tile of it is drawn with it. A tile's rectangle and a spline's
            // band are their footprints.
            Rules::RegisterComponentLoading( { kLandscapeRootComponent, Rules::ComponentLoading::Global } );
            Rules::RegisterComponentLoading( { kLandscapeMaterialComponent, Rules::ComponentLoading::Global } );
            Rules::RegisterComponentLoading( { kLandscapeTileComponent, Rules::ComponentLoading::Spatial } );
            Rules::RegisterComponentLoading( { kLandscapeSplineComponent, Rules::ComponentLoading::Spatial } );

            // References (Rules::EntityReferences). A tile names its root, and it is OBSERVATION ON PURPOSE
            // (landscape analysis A1): as containment every tile of a landscape would be one composite, and the
            // whole terrain one footprint - the opposite of why a landscape is cut into tiles. A tile whose root
            // is not loaded has no frame and is placed nowhere (UnplacedFramedRecords); it does not drag the root
            // in. A spline names its root the same way (UE: ALandscapeSplineActor is its own spatially loaded
            // actor in a partitioned world); its points are world cm, so its band places it whether the root is
            // loaded or not.
            Rules::RegisterEntityReference(
                 { kLandscapeTileComponent, kLandscapeRootComponent, Rules::ReferenceKind::Observation } );
            Rules::RegisterEntityReference(
                 { kLandscapeSplineComponent, kLandscapeRootComponent, Rules::ReferenceKind::Observation } );

            // Descriptors - tile, spline, root: the order the rules read them in (their Issues keep that order).
            Rules::RegisterDescriptorHook( Rules::DescriptorHook{ kLandscapeTileComponent, kLandscapeRootComponent,
                                                                  &DescribeTile, &TileFootprint } );
            Rules::RegisterDescriptorHook(
                 Rules::DescriptorHook{ kLandscapeSplineComponent, {}, &DescribeSpline, &SplineFootprint } );
            Rules::RegisterDescriptorHook(
                 Rules::DescriptorHook{ kLandscapeRootComponent, {}, &DescribeRoot, nullptr } );

            // HLOD: a tile draws terrain from its height file and a spline meshes bent along it - neither a mesh
            // block; BuildCellHLOD hands them to the landscape and spline builders (WP-FAR-6).
            // Each states what its builder reads (TileInputs, SplineInputs), which the saved-HLOD checksum folds.
            Rules::RegisterHLODBuilderComponent(
                 { kLandscapeTileComponent, Rules::HLODExclusion::LandscapeTile, &TileInputs } );
            Rules::RegisterHLODBuilderComponent(
                 { kLandscapeSplineComponent, Rules::HLODExclusion::LandscapeSpline, &SplineInputs } );

            // Activation: a tile builds a heightfield (and its collision) - more than a primitive.
            Rules::RegisterActivationKind(
                 { kLandscapeTileComponent, Rules::ActivationRecordKind::LandscapeTile } );
            return true;
        }();
        (void)kRegistered;
    }
} // namespace Desert::LandscapeCore
