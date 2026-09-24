#include "WorldGenMain.hpp"

#include "WorldBuild.hpp"
#include "SettingsCanonical.hpp"

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <ostream>

namespace Desert::WorldGen
{
    namespace
    {
        // THE SHIPPED PRESETS, NAMED IN CODE. The world-scale scene is an instrument, and an instrument
        // whose dimensions are typed on the command line each time is a different instrument every time.
        // Two numbers only ever move together with a new name.
        struct Preset
        {
            const char* Key;
            const char* SceneName;
            int         Cells;
            int         PerCell;
        };

        // `world` is the acceptance instrument. 32 x 32 cells of 256 m is 8192 m on a side - ten and a
        // half times the 768 m load radius UE's own example uses, so the resident set is about 29 of 1024
        // cells (~2.8 %). A world smaller than a few multiples of the radius is not a world: everything is
        // resident all the time and the streaming question cannot even be asked.
        //
        // `smoke` is what the test suite generates. Same code path, same arithmetic, four cells - small
        // enough that a suite can build it, parse it and round-trip it several times per run.
        constexpr Preset kPresets[] = {
             { "world", "World Grid 8 km", 32, 48 },
             { "smoke", "World Grid Smoke", 2, 6 },
        };

        const Preset* FindPreset( const std::string& key )
        {
            for ( const auto& p : kPresets )
                if ( key == p.Key )
                    return &p;
            return nullptr;
        }

        std::string Usage()
        {
            std::string presets;
            for ( const auto& p : kPresets )
                presets += std::string( presets.empty() ? "" : ", " ) + p.Key;
            return "usage: WorldGen --out <scene.desce> [--preset <" + presets +
                   ">] [--assets <dir>] [--cells N] [--per-cell N] [--cell-size CM] [--seed N] "
                   "[--name <scene name>] [--partition [--partition-cell CM] [--loading-range CM]] [--verify]";
        }

        // ONE FIELD OF A .demat, NAMED AS A TYPE. The generator needs a material's identity and nothing
        // else, and rfl ignores the keys it is not asked about - so this reads the number as a uint64_t
        // through the parser, rather than through rfl::Generic's to_int()/to_double(), both of which are
        // the wrong shape for this value: to_int() truncated 6418972230554417713 to 155908657 in the first
        // run of this tool, and to_double() would round every handle above 2^53. Same defect
        // SceneSerializer.cpp:159 names for SplashSprite, met again on the way in.
        struct MaterialIdentityOnly
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        };

        // The material's own file is the only place its identity is written down, so the generator READS
        // it rather than carrying a copy of the number. A hard-coded GUID here would be a second statement
        // of an asset's identity and would go stale silently the day the material is re-imported -
        // MaterialIdentity's corpus rule is exactly that check, one level out.
        Common::ResultStr<MaterialRef> LoadMaterial( const std::filesystem::path& assetsRoot,
                                                     const std::string&           relative )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( assetsRoot / relative );
            if ( !text.IsSuccess() )
                return Common::MakeError<MaterialRef>( "material '" + relative + "': " + text.GetError() );

            const auto parsed = rfl::json::read<MaterialIdentityOnly>( text.GetValue() );
            if ( !parsed.has_value() )
                return Common::MakeError<MaterialRef>( "material '" + relative + "' states no readable header" );
            if ( !parsed.value().Header )
                return Common::MakeError<MaterialRef>( "material '" + relative + "' has no header" );
            const auto guid = Common::Content::AssetGuidFromText( parsed.value().Header->Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeError<MaterialRef>( "material '" + relative + "' states no GUID" );

            // The handle a scene names it by: its GUID through the one fold the engine uses.
            return Common::MakeSuccess<MaterialRef>(
                 { relative, static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) } );
        }

        // THE WORLD'S PALETTE. Four untextured colours for the buildings and the one textured material
        // this repository owns for the ground.
        //
        // WHY SO FEW, SAID OUT LOUD: the whole project ships three textures and one non-terrain material
        // that names one of them. So this scene can reproduce the DRAW-CALL and START-UP halves of the
        // programme and cannot, today, reproduce the texture-memory half (step 5) - there is not enough
        // distinct texture data in the tree to fill a budget with. The palette is a parameter of this
        // function and not a constant inside the generator precisely so that the day textured materials
        // exist, this list grows and the world is regenerated, with no change to the generator.
        constexpr const char* kBuildingMaterials[] = {
             "Materials/CB_White.demat",
             "Materials/CB_Orange.demat",
             "Materials/CB_Green.demat",
             "Materials/CB_Red.demat",
        };
        constexpr const char* kGroundMaterial = "Materials/M_CheckerFloor.demat";

        bool ParseInt( const std::string& text, int& out )
        {
            try
            {
                size_t     consumed = 0;
                const long value    = std::stol( text, &consumed );
                if ( consumed != text.size() )
                    return false;
                out = static_cast<int>( value );
                return true;
            }
            catch ( ... )
            {
                return false;
            }
        }
    } // namespace

    int RunWorldGen( const std::vector<std::string>& args, std::ostream& out, std::ostream& err )
    {
        std::string        outPath;
        std::string        assetsRoot = "Editor/Resources/Assets";
        std::string        presetKey  = "world";
        std::string        nameOverride;
        std::optional<int> cells;
        std::optional<int> perCell;
        std::optional<int> cellSize;
        std::optional<int> seed;
        std::optional<int> partitionCell;
        std::optional<int> loadingRange;
        bool               verify    = false;
        bool               partition = false;

        for ( size_t i = 0; i < args.size(); ++i )
        {
            const std::string& a     = args[i];
            const auto         value = [&]( std::string& into ) -> bool
            {
                if ( i + 1 >= args.size() )
                    return false;
                into = args[++i];
                return true;
            };

            std::string v;
            if ( a == "--verify" )
                verify = true;
            else if ( a == "--partition" )
                partition = true;
            else if ( a == "--out" && value( v ) )
                outPath = v;
            else if ( a == "--assets" && value( v ) )
                assetsRoot = v;
            else if ( a == "--preset" && value( v ) )
                presetKey = v;
            else if ( a == "--name" && value( v ) )
                nameOverride = v;
            else if ( a == "--cells" && value( v ) )
            {
                int n = 0;
                if ( !ParseInt( v, n ) )
                {
                    err << "WorldGen: --cells '" << v << "' is not an integer\n";
                    return 2;
                }
                cells = n;
            }
            else if ( a == "--per-cell" && value( v ) )
            {
                int n = 0;
                if ( !ParseInt( v, n ) )
                {
                    err << "WorldGen: --per-cell '" << v << "' is not an integer\n";
                    return 2;
                }
                perCell = n;
            }
            else if ( a == "--cell-size" && value( v ) )
            {
                int n = 0;
                if ( !ParseInt( v, n ) )
                {
                    err << "WorldGen: --cell-size '" << v << "' is not an integer\n";
                    return 2;
                }
                cellSize = n;
            }
            else if ( ( a == "--partition-cell" || a == "--loading-range" ) && value( v ) )
            {
                int n = 0;
                if ( !ParseInt( v, n ) )
                {
                    err << "WorldGen: " << a << " '" << v << "' is not an integer\n";
                    return 2;
                }
                ( a == "--partition-cell" ? partitionCell : loadingRange ) = n;
            }
            else if ( a == "--seed" && value( v ) )
            {
                int n = 0;
                if ( !ParseInt( v, n ) )
                {
                    err << "WorldGen: --seed '" << v << "' is not an integer\n";
                    return 2;
                }
                seed = n;
            }
            else
            {
                err << "WorldGen: unrecognised argument '" << a << "'\n" << Usage() << "\n";
                return 2;
            }
        }

        if ( outPath.empty() )
        {
            err << "WorldGen: --out is required\n" << Usage() << "\n";
            return 2;
        }

        // A grid shape with no grid to shape would be a flag that does nothing, and say nothing about it.
        if ( !partition && ( partitionCell.has_value() || loadingRange.has_value() ) )
        {
            err << "WorldGen: --partition-cell and --loading-range shape the WorldPartition block, which only "
                   "--partition writes\n";
            return 2;
        }
        if ( partitionCell.value_or( 1 ) <= 0 || loadingRange.value_or( 1 ) <= 0 )
        {
            err << "WorldGen: --partition-cell and --loading-range must be positive\n";
            return 2;
        }

        const Preset* preset = FindPreset( presetKey );
        if ( preset == nullptr )
        {
            err << "WorldGen: unknown preset '" << presetKey << "'\n" << Usage() << "\n";
            return 2;
        }

        WorldSpec spec;
        spec.Name       = nameOverride.empty() ? preset->SceneName : nameOverride;
        spec.Cells      = cells.value_or( preset->Cells );
        spec.PerCell    = perCell.value_or( preset->PerCell );
        spec.CellSizeCm = cellSize.value_or( spec.CellSizeCm );
        spec.Seed       = static_cast<uint64_t>( seed.value_or( 1 ) );

        if ( spec.Cells <= 0 || spec.PerCell <= 0 || spec.CellSizeCm <= 0 )
        {
            err << "WorldGen: --cells, --per-cell and --cell-size must all be positive\n";
            return 2;
        }

        const std::filesystem::path assets( assetsRoot );
        std::vector<MaterialRef>    palette;
        for ( const char* relative : kBuildingMaterials )
        {
            auto material = LoadMaterial( assets, relative );
            if ( !material.IsSuccess() )
            {
                err << "WorldGen: " << material.GetError() << "\n"
                    << "WorldGen: --assets '" << assetsRoot << "' does not look like the assets root.\n";
                return 3;
            }
            palette.push_back( material.GetValue() );
        }
        auto ground = LoadMaterial( assets, kGroundMaterial );
        if ( !ground.IsSuccess() )
        {
            err << "WorldGen: " << ground.GetError() << "\n";
            return 3;
        }

        // Regenerating an existing world keeps that file's identity; a first write mints one. Resolved once,
        // so the --verify rebuild below produces the same bytes.
        const Common::Content::AssetGuid worldGuid =
             ExistingWorldGuid( outPath ).value_or( Common::Content::AssetGuid::Generate() );

        const auto build = [&]( std::string& json, WorldStats& stats ) -> bool
        {
            auto scene = BuildWorld( spec, palette, ground.GetValue(), worldGuid, stats );

            // --partition: the world states a WorldPartition block. By default its level-0 cell IS the
            // generator's tile, so every ground tile is exactly one cell and what the plan promotes is what
            // really crosses a tile edge. --partition-cell makes the grid finer than the tile (the engine's
            // own 128 m default under a 256 m tile puts every ground tile one level up, which is the WP3
            // level ladder under load). The loading range defaults to the 768 m radius the preset's own
            // sizing argument is made against (see kPresets): about 29 of 1024 tiles resident.
            if ( partition )
            {
                Core::WorldPartitionGridSerialized grid;
                grid.CellSize        = static_cast<float>( partitionCell.value_or( spec.CellSizeCm ) );
                grid.LoadingRange    = static_cast<float>( loadingRange.value_or( 76800 ) );
                scene.WorldPartition = Core::WorldPartitionSerialized{ { grid } };
            }

            // The Settings block, written the way the ENGINE'S SAVER writes it - every field this build
            // declares, in the registry's order, through the same serializer. Reusing SceneMigrator's
            // canonicaliser rather than hand-writing 51 fields is the difference between a scene that
            // survives its first save unchanged and one that silently gains keys.
            const auto report = Migration::CanonicaliseSettings( scene.Settings );
            if ( report.Refused )
            {
                err << "WorldGen: the reflection table was not available, so no Settings block could be "
                       "written. The scene would load on defaults with nothing said about it.\n";
                return false;
            }

            json = rfl::json::write( scene );
            return true;
        };

        WorldStats  stats;
        std::string json;
        const auto  started = std::chrono::steady_clock::now();
        if ( !build( json, stats ) )
            return 4;
        const auto elapsedMs =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started )
                  .count();

        if ( verify )
        {
            // THE CLAIM THE SCENE'S USEFULNESS RESTS ON, CHECKED RATHER THAN ASSERTED. A measuring stick
            // that is a different length each time it is cut measures nothing: every number the programme
            // reports against this file is only comparable with the next if the file is the same file.
            WorldStats  again;
            std::string second;
            if ( !build( second, again ) )
                return 4;
            if ( second != json )
            {
                err << "WorldGen: NOT REPRODUCIBLE - two runs of the same spec produced different bytes ("
                    << json.size() << " vs " << second.size() << "). Refusing to write.\n";
                return 5;
            }
            out << "verify: two builds of the same spec are byte-identical (" << json.size() << " bytes)\n";
        }

        const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( outPath, json );
        if ( !written.IsSuccess() )
        {
            err << "WorldGen: " << written.GetError() << "\n";
            return 6;
        }

        out << "WorldGen wrote " << outPath << "\n"
            << "  scene name   : " << spec.Name << " (preset '" << presetKey << "')\n"
            << "  entities     : " << stats.Entities << "  (" << stats.Buildings << " buildings, "
            << stats.GroundTiles << " ground tiles, 3 fixtures)\n"
            << "  cells        : " << stats.Cells << " of " << spec.CellSizeCm / 100 << " m\n"
            << "  extent       : " << stats.ExtentCm / 100 << " m square\n"
            << "  bytes        : " << json.size() << "\n"
            << "  schema       : SceneVersion " << Core::kSceneVersion << ", UnitVersion " << Core::kUnitVersion
            << "\n"
            << "  generated in : " << elapsedMs << " ms\n";

        if ( partition )
        {
            // THE PLAN OF THE FILE AS WRITTEN, not of the tree in memory: the text is parsed back the way
            // the loader parses it, so what is reported is what a load of this file will partition.
            const auto parsed = rfl::json::read<Core::SceneSerialized>( json );
            if ( !parsed.has_value() || !parsed.value().WorldPartition.has_value() )
            {
                err << "WorldGen: the written scene does not parse back with its WorldPartition block\n";
                return 7;
            }
            const auto& scene = parsed.value();
            const auto  plan  = Core::Rules::PlanWorldPartition( scene.Entities, *scene.WorldPartition );
            out << "  partition    : " << Core::Rules::SummarisePartition( plan, *scene.WorldPartition ) << "\n";
            if ( plan.MaxLevelComposite != Core::Rules::kNoRecord && plan.MaxLevel > 0 )
            {
                const auto& anchor = scene.Entities[plan.Composites[plan.MaxLevelComposite].Anchor];
                out << "  widest       : '" << anchor.Tag.value_or( "Entity" ) << "' went up to level "
                    << plan.MaxLevel << "\n";
            }
            // Named, up to a screenful; the count is in the summary line above.
            constexpr std::size_t kNamed = 16;
            for ( std::size_t index = 0; index < plan.AlwaysLoaded.size() && index < kNamed; ++index )
            {
                const auto& held = plan.Composites[plan.AlwaysLoaded[index]];
                const auto& who =
                     scene.Entities[held.Because != Core::Rules::kNoRecord ? held.Because : held.Anchor];
                out << "  always-loaded: '" << who.Tag.value_or( "Entity" ) << "' ("
                    << ( held.Reason == Core::Rules::AlwaysLoadedReason::Component ? "component"
                         : held.Reason == Core::Rules::AlwaysLoadedReason::Author  ? "author"
                         : held.Reason == Core::Rules::AlwaysLoadedReason::NoFit   ? "no fit"
                                                                                   : "no grid" )
                    << ")\n";
            }
            if ( plan.AlwaysLoaded.size() > kNamed )
                out << "  always-loaded: ... and " << plan.AlwaysLoaded.size() - kNamed << " more\n";
        }
        return 0;
    }
} // namespace Desert::WorldGen
