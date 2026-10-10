#include <Engine/Core/Serialize/EditorRegions.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>

#include <spdlog/fmt/fmt.h>

#include <optional>
#include <unordered_set>

namespace Desert::Core::EditorRegions
{
    namespace
    {
        bool Meets( const Rules::CellBounds& a, const Rules::CellBounds& b )
        {
            return a.MinX <= b.MaxX && b.MinX <= a.MaxX && a.MinZ <= b.MaxZ && b.MinZ <= a.MaxZ;
        }

        struct IndexSelection
        {
            std::vector<std::uint64_t> RowIds; // every record of the world, in the index's (descriptor row) order
            RegionSelection            Selection;
        };

        // What `regions` select of the world at `path`, decided over its descriptor index (refreshed first, so an
        // index older than its files is rebuilt rather than used). `regions` null: the whole world.
        Common::ResultStr<IndexSelection> SelectOverIndex( const std::filesystem::path&              path,
                                                           const WorldPartitionSerialized&           settings,
                                                           const std::span<const Rules::CellBounds>* regions )
        {
            auto index = DescriptorIndex::Refresh( path );
            if ( !index )
                return Common::MakeError<IndexSelection>( index.GetError() );
            const DescriptorIndex::DescriptorIndexFile& rows = index.GetValue().Index;
            IndexSelection                              selected;
            selected.RowIds.reserve( rows.Entities.size() );
            for ( const DescriptorIndex::DescriptorRow& row : rows.Entities )
                selected.RowIds.push_back( row.Id );
            if ( regions == nullptr )
            {
                selected.Selection.Records = selected.RowIds;
                return Common::MakeSuccess( std::move( selected ) );
            }
            const std::vector<Rules::EntityDescriptor> descriptors = DescriptorIndex::Descriptors( rows );
            const Rules::WorldPartitionPlan            plan        = Rules::PlanWorldPartition(
                 std::span<const Rules::EntityDescriptor>( descriptors ), settings, RegistryMeshBounds() );
            selected.Selection = SelectRecords( plan, selected.RowIds, *regions );
            return Common::MakeSuccess( std::move( selected ) );
        }

        // `regions` null: the whole world.
        Common::ResultStr<RegionOutcome> Hold( Scene& scene, Assets::AssetManager* assets,
                                               const std::span<const Rules::CellBounds>* regions )
        {
            using Result = RegionOutcome;
            const auto& baseline = scene.Packages()->BaselinePath();
            if ( !baseline )
                return Common::MakeError<Result>( fmt::format(
                     "'{}' was not opened from or saved to its files: a region loads from the world on disk.",
                     scene.GetSceneName() ) );
            const std::filesystem::path path = *baseline;
            if ( !scene.GetWorldPartition() )
                return Common::MakeError<Result>(
                     fmt::format( "'{}' is not a partitioned world: it has no regions.", path.string() ) );

            // The world as its files describe it - nothing of an entity outside the regions is read.
            auto selected = SelectOverIndex( path, *scene.GetWorldPartition(), regions );
            if ( !selected )
                return Common::MakeError<Result>( selected.GetError() );
            const std::vector<std::uint64_t>& rowIds = selected.GetValue().RowIds;

            RegionOutcome outcome;
            outcome.Selection = selected.GetValue().Selection;

            const std::unordered_set<std::uint64_t> wanted( outcome.Selection.Records.begin(),
                                                            outcome.Selection.Records.end() );
            std::vector<Common::UUID> notLoaded;
            for ( const std::uint64_t id : rowIds )
                if ( !wanted.contains( id ) )
                    notLoaded.emplace_back( id );

            // What the scene holds now, and what of it leaves. A dirty entity refuses the change before anything
            // is read or destroyed.
            SceneSerializer                   serializer( &scene, assets );
            const std::vector<LiveEntity>     live = serializer.LiveEntities();
            std::unordered_set<std::uint64_t> held;
            std::vector<Common::UUID>         leaving;
            for ( const LiveEntity& entity : live )
            {
                const auto record = static_cast<std::uint64_t>( entity.Record );
                if ( static_cast<std::uint64_t>( entity.Id ) == record )
                    held.insert( record );
                if ( wanted.contains( record ) )
                    continue;
                if ( scene.Packages()->IsDirty( entity.Id ) )
                    return Common::MakeError<Result>( fmt::format(
                         "'{}': entity {} differs from its file and would leave the loaded regions. Save it first. "
                         "Nothing was loaded or unloaded.",
                         path.string(), static_cast<std::uint64_t>( entity.Id ) ) );
                if ( static_cast<std::uint64_t>( entity.Id ) == record )
                    leaving.push_back( entity.Id );
            }

            // Read and parsed before the scene is touched: a file that cannot be read changes nothing.
            auto text = ExternalEntities::ReadSceneRegionText( path, wanted );
            if ( !text )
                return Common::MakeError<Result>( text.GetError() );
            auto loadable = ParseLoadableScene( path.string(), text.GetValue() );
            if ( !loadable )
                return Common::MakeError<Result>( loadable.GetError() );
            LoadableScene                   parsed = loadable.ExtractValue();
            std::vector<Assets::EntityData> arriving;
            for ( Assets::EntityData& record : parsed.Scene.Entities )
                if ( record.id.has_value() && !held.contains( static_cast<std::uint64_t>( *record.id ) ) )
                    arriving.push_back( std::move( record ) );

            for ( const Common::UUID id : leaving )
            {
                const auto found = scene.FindEntityByID( id );
                if ( !found.has_value() )
                    continue; // went with a parent that left before it
                const ECS::Entity entity = found->get();
                scene.DestroyEntity( entity );
                ++outcome.Unloaded;
            }
            if ( !arriving.empty() )
            {
                auto made = serializer.InstantiateRecords( arriving, scene.GetSceneName(), nullptr );
                if ( !made )
                    return Common::MakeError<Result>( fmt::format( "'{}': {}", path.string(), made.GetError() ) );
            }
            outcome.Loaded = arriving.size();

            // The document a save merges foreign keys from is the held part's, as an open of it would have made.
            scene.SetLoadedDocument( std::move( parsed.Document ) );
            const std::vector<LiveEntity> after = serializer.LiveEntities();
            scene.Packages()->AdoptRegion( after, notLoaded );
            outcome.NotLoaded = notLoaded.size();
            return Common::MakeSuccess( std::move( outcome ) );
        }
    } // namespace

    RegionSelection SelectRecords( const Rules::WorldPartitionPlan& plan, std::span<const std::uint64_t> rowIds,
                                   std::span<const Rules::CellBounds> regions )
    {
        RegionSelection          selection;
        std::vector<bool>        take( rowIds.size(), false );
        std::unordered_set<std::size_t> always( plan.AlwaysLoaded.begin(), plan.AlwaysLoaded.end() );
        for ( std::size_t c = 0; c < plan.Composites.size(); ++c )
        {
            const Rules::PlannedComposite& composite = plan.Composites[c];
            bool                           hold      = false;
            if ( always.contains( c ) )
            {
                hold = true;
                ++selection.AlwaysLoaded;
            }
            else if ( !composite.Footprint.has_value() )
                ++selection.Unplaced;
            else
                for ( const Rules::CellBounds& region : regions )
                    if ( Meets( *composite.Footprint, region ) )
                    {
                        hold = true;
                        ++selection.InRegions;
                        break;
                    }
            if ( hold )
                for ( const std::size_t member : composite.Members )
                    if ( member < take.size() )
                        take[member] = true;
        }
        for ( std::size_t r = 0; r < rowIds.size(); ++r )
            if ( take[r] )
                selection.Records.push_back( rowIds[r] );
        return selection;
    }

    Common::ResultStr<RegionOutcome> LoadRegions( Scene& scene, Assets::AssetManager* assets,
                                                  std::span<const Rules::CellBounds> regions )
    {
        return Hold( scene, assets, &regions );
    }

    Common::ResultStr<RegionOutcome> LoadWholeWorld( Scene& scene, Assets::AssetManager* assets )
    {
        return Hold( scene, assets, nullptr );
    }

    Common::ResultStr<OpenRead> ReadForOpen( const std::filesystem::path& path )
    {
        using Result = OpenRead;
        OpenRead read;
        auto     byRegion = ExternalEntities::ReadsByRegion( path );
        if ( !byRegion )
            return Common::MakeError<Result>( byRegion.GetError() );
        if ( !byRegion.GetValue() )
        {
            auto whole = ExternalEntities::ReadSceneFileText( path );
            if ( !whole )
                return Common::MakeError<Result>( whole.GetError() );
            read.Text = whole.ExtractValue();
            return Common::MakeSuccess( std::move( read ) );
        }

        // The partition's settings, from the header alone (no record is read).
        auto headerText = ExternalEntities::ReadSceneRegionText( path, {} );
        if ( !headerText )
            return Common::MakeError<Result>( headerText.GetError() );
        auto header = ParseLoadableScene( path.string(), headerText.GetValue() );
        if ( !header )
            return Common::MakeError<Result>( header.GetError() );
        if ( !header.GetValue().Scene.WorldPartition )
            return Common::MakeError<Result>( fmt::format(
                 "'{}' lists its entities in files but states no WorldPartition block.", path.string() ) );

        // What a load with no region holds (LoadRegions( scene, {} )).
        const std::span<const Rules::CellBounds> none;
        auto selected = SelectOverIndex( path, *header.GetValue().Scene.WorldPartition, &none );
        if ( !selected )
            return Common::MakeError<Result>( selected.GetError() );
        read.Selection = selected.GetValue().Selection;

        const std::unordered_set<std::uint64_t> wanted( read.Selection.Records.begin(),
                                                        read.Selection.Records.end() );
        for ( const std::uint64_t id : selected.GetValue().RowIds )
            if ( !wanted.contains( id ) )
                read.NotLoaded.emplace_back( id );
        auto text = ExternalEntities::ReadSceneRegionText( path, wanted );
        if ( !text )
            return Common::MakeError<Result>( text.GetError() );
        read.Text     = text.ExtractValue();
        read.ByRegion = true;
        return Common::MakeSuccess( std::move( read ) );
    }

    Common::ResultStr<std::size_t> MakeNotLoadedForPlay( Scene& scene, Assets::AssetManager* assets )
    {
        using Result               = std::size_t;
        const EntityPackages& held = *scene.Packages();
        if ( held.NotLoadedCount() == 0 )
            return Common::MakeSuccess<Result>( 0 );
        const auto& baseline = held.BaselinePath();
        if ( !baseline )
            return Common::MakeError<Result>( fmt::format(
                 "'{}' holds part of a world but names no files to read the rest from.", scene.GetSceneName() ) );
        const std::filesystem::path path = *baseline;

        // Which records are on disk only: the index lists the world, the packages say what is not held.
        auto index = DescriptorIndex::Refresh( path );
        if ( !index )
            return Common::MakeError<Result>( index.GetError() );
        std::unordered_set<std::uint64_t> missing;
        for ( const DescriptorIndex::DescriptorRow& row : index.GetValue().Index.Entities )
            if ( !held.IsLoaded( Common::UUID( row.Id ) ) )
                missing.insert( row.Id );
        if ( missing.empty() )
            return Common::MakeSuccess<Result>( 0 );

        // Read and parsed before the scene is touched: a file that cannot be read changes nothing.
        auto text = ExternalEntities::ReadSceneRegionText( path, missing );
        if ( !text )
            return Common::MakeError<Result>( text.GetError() );
        auto loadable = ParseLoadableScene( path.string(), text.GetValue() );
        if ( !loadable )
            return Common::MakeError<Result>( loadable.GetError() );
        LoadableScene                   parsed = loadable.ExtractValue();
        std::vector<Assets::EntityData> arriving;
        for ( Assets::EntityData& record : parsed.Scene.Entities )
            if ( record.id.has_value() && missing.contains( static_cast<std::uint64_t>( *record.id ) ) )
                arriving.push_back( std::move( record ) );
        if ( arriving.empty() )
            return Common::MakeSuccess<Result>( 0 );

        SceneSerializer serializer( &scene, assets );
        auto            made = serializer.InstantiateRecords( arriving, scene.GetSceneName(), nullptr );
        if ( !made )
            return Common::MakeError<Result>( fmt::format( "'{}': {}", path.string(), made.GetError() ) );
        return Common::MakeSuccess<Result>( arriving.size() );
    }
} // namespace Desert::Core::EditorRegions
