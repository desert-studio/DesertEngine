#include <Engine/Core/Serialize/EditorRegions.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/ECS/Components.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <memory>
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
        // index older than its files is rebuilt rather than used). `regions` null: the whole world. Composites
        // whose data layers are all unloaded in the editor under `layers` are not selected either way.
        Common::ResultStr<IndexSelection> SelectOverIndex( const std::filesystem::path&              path,
                                                           const WorldPartitionSerialized&           settings,
                                                           const std::span<const Rules::CellBounds>* regions,
                                                           const DataLayerOverrides&                 layers,
                                                           std::span<const std::uint64_t>            pinned )
        {
            auto index = DescriptorIndex::Refresh( path );
            if ( !index )
                return Common::MakeError<IndexSelection>( index.GetError() );
            const DescriptorIndex::DescriptorIndexFile& rows = index.GetValue().Index;
            IndexSelection                              selected;
            selected.RowIds.reserve( rows.Entities.size() );
            for ( const DescriptorIndex::DescriptorRow& row : rows.Entities )
                selected.RowIds.push_back( row.Id );
            // The fast whole world: no layer is unloaded, so every record is held and nothing is planned.
            const std::vector<bool> layerLoaded = DataLayersLoadedInEditor( settings, layers );
            const bool everyLayerLoaded = std::all_of( layerLoaded.begin(), layerLoaded.end(),
                                                       []( const bool loaded ) { return loaded; } );
            if ( regions == nullptr && everyLayerLoaded )
            {
                selected.Selection.Records = selected.RowIds;
                return Common::MakeSuccess( std::move( selected ) );
            }
            // std::vector<bool> is not contiguous: the selection reads one flag per layer from a plain array.
            const std::unique_ptr<bool[]> loadedFlags = std::make_unique<bool[]>( layerLoaded.size() );
            for ( std::size_t layer = 0; layer < layerLoaded.size(); ++layer )
                loadedFlags[layer] = layerLoaded[layer];
            const std::span<const bool> loaded( loadedFlags.get(), layerLoaded.size() );
            const std::vector<Rules::EntityDescriptor> descriptors = DescriptorIndex::Descriptors( rows );
            const Rules::WorldPartitionPlan            plan        = Rules::PlanWorldPartition(
                 std::span<const Rules::EntityDescriptor>( descriptors ), settings, RegistryMeshBounds() );
            // A region load names what the planner refused exactly as the cook and Play do (ReportPlanIssues).
            Rules::ReportPlanIssues( plan, path.string() );
            selected.Selection = regions == nullptr
                                      ? SelectWholeWorld( plan, selected.RowIds, loaded )
                                      : SelectRecords( plan, selected.RowIds, *regions, loaded, pinned );
            return Common::MakeSuccess( std::move( selected ) );
        }

        // `regions` null: the whole world.
        Common::ResultStr<RegionOutcome> Hold( Scene& scene, Assets::AssetManager* assets,
                                               const std::span<const Rules::CellBounds>* regions,
                                               const DataLayerOverrides&                 layers,
                                               std::span<const std::uint64_t> pinned, const UnsavedPolicy policy )
        {
            using Result         = RegionOutcome;
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
            auto selected = SelectOverIndex( path, *scene.GetWorldPartition(), regions, layers, pinned );
            if ( !selected )
                return Common::MakeError<Result>( selected.GetError() );
            const std::vector<std::uint64_t>& rowIds = selected.GetValue().RowIds;

            RegionOutcome outcome;
            outcome.Selection = selected.GetValue().Selection;

            const std::unordered_set<std::uint64_t> wanted( outcome.Selection.Records.begin(),
                                                            outcome.Selection.Records.end() );
            std::vector<Common::UUID>               notLoaded;
            for ( const std::uint64_t id : rowIds )
                if ( !wanted.contains( id ) )
                    notLoaded.emplace_back( id );

            // What the scene holds now, and what of it leaves. Under UnsavedPolicy::Ask a dirty entity that would
            // leave stops the change before anything is read or destroyed.
            SceneSerializer const             serializer( &scene, assets );
            const std::vector<LiveEntity>     live = serializer.LiveEntities();
            // An entity whose record the world's files do not hold (made since the open or last save, e.g. the
            // foliage entity a palette edit creates) has no file to go back to and no region of the files: it
            // stays as it is, unsaved, whatever the regions (UE: an unsaved new actor is not the loader's to
            // unload).
            const std::unordered_set<std::uint64_t> onDisk( rowIds.begin(), rowIds.end() );
            std::unordered_set<std::uint64_t>       held;
            std::vector<Common::UUID>               leaving;
            std::vector<Common::UUID>               unsaved;
            std::vector<UnsavedEntity>              unsavedLeaving; // Ask: the dirty entities that would leave
            for ( const LiveEntity& entity : live )
            {
                const auto record = static_cast<std::uint64_t>( entity.Record );
                if ( !onDisk.contains( record ) )
                {
                    unsaved.push_back( entity.Id );
                    continue;
                }
                if ( static_cast<std::uint64_t>( entity.Id ) == record )
                    held.insert( record );
                if ( wanted.contains( record ) )
                    continue;
                if ( policy == UnsavedPolicy::Ask && scene.Packages()->IsDirty( entity.Id ) )
                {
                    // Every one is named, not the first: the prompt lists what a Save or a Discard decides.
                    const auto found = scene.FindEntityByID( entity.Id );
                    unsavedLeaving.push_back( UnsavedEntity{
                         entity.Id, found.has_value()
                                         ? found->get().GetComponent<ECS::TagComponent>().Tag
                                         : fmt::format( "{}", static_cast<std::uint64_t>( entity.Id ) ) } );
                    continue;
                }
                if ( static_cast<std::uint64_t>( entity.Id ) == record )
                    leaving.push_back( entity.Id );
            }
            // An entity deleted since the last save would come back from its file: the change asks about it too
            // (Discard brings it back where the regions hold it, a Save deletes its file first).
            if ( policy == UnsavedPolicy::Ask )
                for ( const Common::UUID gone : scene.Packages()->GoneSince( live ) )
                    unsavedLeaving.push_back( UnsavedEntity{
                         gone, fmt::format( "{} (deleted)", static_cast<std::uint64_t>( gone ) ) } );
            // UE asks before it unloads a dirty actor: nothing is read or destroyed until the user answers.
            if ( !unsavedLeaving.empty() )
            {
                outcome.Unsaved   = std::move( unsavedLeaving );
                outcome.NotLoaded = scene.Packages()->NotLoadedCount();
                return Common::MakeSuccess( std::move( outcome ) );
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
            scene.Packages()->AdoptRegion( after, notLoaded, unsaved );
            outcome.NotLoaded = notLoaded.size();
            return Common::MakeSuccess( std::move( outcome ) );
        }
    } // namespace

    std::vector<bool> DataLayersLoadedInEditor( const WorldPartitionSerialized& settings,
                                                const DataLayerOverrides&       overrides )
    {
        std::vector<bool> loaded;
        if ( !settings.DataLayers.has_value() )
            return loaded;
        const auto names = []( const std::vector<std::string>& list, const std::string& name )
        { return std::find( list.begin(), list.end(), name ) != list.end(); };
        for ( const DataLayerSerialized& layer : *settings.DataLayers )
        {
            bool state = layer.InitiallyLoadedInEditor;
            if ( names( overrides.NotLoaded, layer.Name ) )
                state = false;
            if ( names( overrides.Loaded, layer.Name ) )
                state = true;
            loaded.push_back( state );
        }
        return loaded;
    }

    DataLayerOverrides WithDataLayerLoaded( const WorldPartitionSerialized& settings, DataLayerOverrides overrides,
                                            const std::string& name, const bool loaded )
    {
        std::erase( overrides.Loaded, name );
        std::erase( overrides.NotLoaded, name );
        const std::optional<std::size_t> layer = Rules::DataLayerIndexOf( settings, name );
        const bool initially = layer.has_value() && ( *settings.DataLayers )[*layer].InitiallyLoadedInEditor;
        if ( loaded && !initially )
            overrides.Loaded.push_back( name );
        if ( !loaded && initially )
            overrides.NotLoaded.push_back( name );
        return overrides;
    }

    namespace
    {
        // Whether a composite's data layers let the editor hold it: no layer, or ANY of its layers loaded (UE
        // UDataLayerManager::ResolveIsLoadedInEditor). A layer index past `layerLoaded` reaches no layer the
        // caller knows of and does not hold the composite back.
        bool LayersHold( const Rules::PlannedComposite& composite, std::span<const bool> layerLoaded )
        {
            if ( composite.DataLayers.empty() )
                return true;
            return std::any_of( composite.DataLayers.begin(), composite.DataLayers.end(),
                                [&]( const std::size_t layer )
                                { return layer >= layerLoaded.size() || layerLoaded[layer]; } );
        }

        // SelectRecords and SelectWholeWorld: `regions` null = the whole world (every composite, placed or not).
        RegionSelection Select( const Rules::WorldPartitionPlan& plan, std::span<const std::uint64_t> rowIds,
                                const std::span<const Rules::CellBounds>* regions,
                                std::span<const bool> layerLoaded, std::span<const std::uint64_t> pinned )
        {
            RegionSelection                         selection;
            const std::unordered_set<std::uint64_t> pins( pinned.begin(), pinned.end() );
            // The whole world starts from every record (a record no composite lists is still the world's);
            // regions start from none.
            std::vector<bool>                     take( rowIds.size(), regions == nullptr );
            std::unordered_set<std::size_t> const always( plan.AlwaysLoaded.begin(), plan.AlwaysLoaded.end() );
            for ( std::size_t c = 0; c < plan.Composites.size(); ++c )
            {
                const Rules::PlannedComposite& composite = plan.Composites[c];
                if ( !LayersHold( composite, layerLoaded ) )
                {
                    ++selection.UnloadedByDataLayers;
                    for ( const std::size_t member : composite.Members )
                        if ( member < take.size() )
                            take[member] = false;
                    continue;
                }
                if ( regions == nullptr )
                {
                    if ( always.contains( c ) )
                        ++selection.AlwaysLoaded;
                    else if ( !composite.Footprint.has_value() )
                        ++selection.Unplaced;
                    else
                        ++selection.InRegions;
                    continue;
                }
                bool hold = false;
                if ( always.contains( c ) )
                {
                    hold = true;
                    ++selection.AlwaysLoaded;
                }
                else if ( !composite.Footprint.has_value() )
                    ++selection.Unplaced;
                else
                {
                    const Rules::CellBounds& footprint = *composite.Footprint;
                    for ( const Rules::CellBounds& region : *regions )
                        if ( Meets( footprint, region ) )
                        {
                            hold = true;
                            ++selection.InRegions;
                            break;
                        }
                }
                // UE's pinned actors loader adapter: a composite with a pinned member is held whole, wherever
                // the regions are.
                if ( !hold && !pins.empty() )
                    for ( const std::size_t member : composite.Members )
                        if ( member < rowIds.size() && pins.contains( rowIds[member] ) )
                        {
                            hold = true;
                            ++selection.Pinned;
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
    } // namespace

    RegionSelection SelectWholeWorld( const Rules::WorldPartitionPlan& plan, std::span<const std::uint64_t> rowIds,
                                      std::span<const bool> layerLoaded )
    {
        return Select( plan, rowIds, nullptr, layerLoaded, {} );
    }

    RegionSelection SelectRecords( const Rules::WorldPartitionPlan& plan, std::span<const std::uint64_t> rowIds,
                                   std::span<const Rules::CellBounds> regions, std::span<const bool> layerLoaded,
                                   std::span<const std::uint64_t> pinned )
    {
        return Select( plan, rowIds, &regions, layerLoaded, pinned );
    }

    Common::ResultStr<RegionOutcome> LoadRegions( Scene& scene, Assets::AssetManager* assets,
                                                  std::span<const Rules::CellBounds> regions,
                                                  const DataLayerOverrides&          layers,
                                                  std::span<const std::uint64_t>     pinned,
                                                  const UnsavedPolicy                policy )
    {
        return Hold( scene, assets, &regions, layers, pinned, policy );
    }

    Common::ResultStr<RegionOutcome> LoadWholeWorld( Scene& scene, Assets::AssetManager* assets,
                                                     const DataLayerOverrides& layers, const UnsavedPolicy policy )
    {
        // The whole world holds every pinned record already.
        return Hold( scene, assets, nullptr, layers, {}, policy );
    }

    Common::ResultStr<OpenRead> ReadForOpen( const std::filesystem::path& path, const HeldRegions& held )
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

        // What a load of `held` holds (LoadRegions( scene, held.Regions, held.DataLayers ) or LoadWholeWorld).
        const std::span<const Rules::CellBounds> regions( held.Regions );
        auto selected = SelectOverIndex( path, *header.GetValue().Scene.WorldPartition,
                                         held.Whole ? nullptr : &regions, held.DataLayers, held.Pinned );
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

    Common::ResultStr<WorldRows> DescribeWorld( Scene& scene )
    {
        using Result         = WorldRows;
        const auto& baseline = scene.Packages()->BaselinePath();
        if ( !baseline )
            return Common::MakeError<Result>(
                 fmt::format( "'{}' was not opened from or saved to its files.", scene.GetSceneName() ) );
        const std::filesystem::path path = *baseline;
        if ( !scene.GetWorldPartition() )
            return Common::MakeError<Result>( fmt::format( "'{}' is not a partitioned world.", path.string() ) );

        auto index = DescriptorIndex::Refresh( path );
        if ( !index )
            return Common::MakeError<Result>( index.GetError() );
        const DescriptorIndex::DescriptorIndexFile& rows        = index.GetValue().Index;
        const std::vector<Rules::EntityDescriptor>  descriptors = DescriptorIndex::Descriptors( rows );
        const Rules::WorldPartitionPlan             plan =
             Rules::PlanWorldPartition( std::span<const Rules::EntityDescriptor>( descriptors ),
                                        *scene.GetWorldPartition(), RegistryMeshBounds() );

        std::vector<std::size_t> compositeOf( rows.Entities.size(), Rules::kNoRecord );
        for ( std::size_t c = 0; c < plan.Composites.size(); ++c )
            for ( const std::size_t member : plan.Composites[c].Members )
                if ( member < compositeOf.size() )
                    compositeOf[member] = c;
        const std::unordered_set<std::size_t> always( plan.AlwaysLoaded.begin(), plan.AlwaysLoaded.end() );

        WorldRows world;
        world.Records.reserve( rows.Entities.size() );
        for ( std::size_t r = 0; r < rows.Entities.size(); ++r )
        {
            const DescriptorIndex::DescriptorRow& row = rows.Entities[r];
            world.Records.push_back( row.Id );
            if ( scene.Packages()->IsLoaded( Common::UUID( row.Id ) ) )
                continue;
            NotLoadedRow entry;
            entry.Id            = row.Id;
            entry.Parent        = row.Descriptor.Parent.has_value() ? *row.Descriptor.Parent : 0;
            entry.Name          = row.Descriptor.Tag;
            entry.Components    = row.Descriptor.Components;
            entry.DataLayers    = row.Descriptor.DataLayers;
            const std::size_t c = compositeOf[r];
            if ( c == Rules::kNoRecord )
                entry.Cell = "not planned";
            else if ( always.contains( c ) )
                entry.Cell = "always loaded";
            else if ( !plan.Composites[c].Footprint.has_value() )
                entry.Cell = "no place";
            else
            {
                const Rules::PlannedComposite& composite = plan.Composites[c];
                entry.Cell =
                     composite.Grid == 0
                          ? fmt::format( "L{} ({}, {})", composite.Level, composite.Cell.X, composite.Cell.Z )
                          : fmt::format( "grid {} L{} ({}, {})", composite.Grid, composite.Level, composite.Cell.X,
                                         composite.Cell.Z );
            }
            world.NotLoaded.push_back( std::move( entry ) );
        }
        return Common::MakeSuccess( std::move( world ) );
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
