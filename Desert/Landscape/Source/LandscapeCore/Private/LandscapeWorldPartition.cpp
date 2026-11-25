#include <LandscapeCore/Public/LandscapeWorldPartition.hpp>

#include <Engine/Core/Serialize/WorldPartitionActivationCost.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <LandscapeCore/Public/LandscapeLayout.hpp>
#include <LandscapeCore/Public/LandscapeSplinesJson.hpp>

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
    } // namespace

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
            Rules::RegisterHLODBuilderComponent(
                 { kLandscapeTileComponent, Rules::HLODExclusion::LandscapeTile } );
            Rules::RegisterHLODBuilderComponent(
                 { kLandscapeSplineComponent, Rules::HLODExclusion::LandscapeSpline } );

            // Activation: a tile builds a heightfield (and its collision) - more than a primitive.
            Rules::RegisterActivationKind(
                 { kLandscapeTileComponent, Rules::ActivationRecordKind::LandscapeTile } );
            return true;
        }();
        (void)kRegistered;
    }
} // namespace Desert::LandscapeCore
