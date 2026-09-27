#include "WorldGenMain.hpp"

#include "WorldBuild.hpp"
#include "SettingsCanonical.hpp"

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <chrono>
#include <cmath>
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
            int         CellSizeCm;
            // Furnished from the tracked corpus (kCorpusProps) instead of primitive-cube buildings, in districts
            // of this many cells per side (WorldSpec::DistrictCells).
            bool Corpus;
            int  DistrictCells;
        };

        // `world` is the acceptance instrument. 32 x 32 cells of 256 m is 8192 m on a side - ten and a
        // half times the 768 m load radius UE's own example uses, so the resident set is about 29 of 1024
        // cells (~2.8 %). A world smaller than a few multiples of the radius is not a world: everything is
        // resident all the time and the streaming question cannot even be asked.
        //
        // `smoke` is what the test suite generates. Same code path, same arithmetic, four cells - small
        // enough that a suite can build it, parse it and round-trip it several times per run.
        //
        // `corpus` is the streaming-memory instrument (WP14): 16 x 16 cells of 20 m in districts of 4 x 4 cells
        // (80 m, wider than the 40 m loading range the flight uses), each district furnished from ONE theme of
        // real tracked assets, so a flight along a row crosses themes and resident asset memory has
        // something to rise and fall with. Cells are small because the corpus meshes are probes (tens of
        // centimetres to a couple of metres) - a 256 m tile would hold them as specks. `corpus-smoke` is its
        // suite-sized twin: same content table, four cells.
        constexpr Preset kPresets[] = {
             { "world", "World Grid 8 km", 32, 48, 25600, false, 1 },
             { "smoke", "World Grid Smoke", 2, 6, 25600, false, 1 },
             { "corpus", "World Corpus Flight", 16, 9, 2000, true, 4 },
             { "corpus-smoke", "World Corpus Smoke", 2, 4, 2000, true, 1 },
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
                   ">] [--assets <dir>] [--project <dir>] [--cells N] [--per-cell N] [--cell-size CM] [--seed N] "
                   "[--name <scene name>] [--partition [--partition-cell CM] [--loading-range CM]] [--verify]";
        }

        // ONE FIELD OF A .demat, NAMED AS A TYPE. The generator needs a material's identity and nothing
        // else, and the rest of the file rides in an unknown-key carrier - so this reads the number as a uint64_t
        // through the parser, rather than through rfl::Generic's to_int()/to_double(), both of which are
        // the wrong shape for this value: to_int() truncated 6418972230554417713 to 155908657 in the first
        // run of this tool, and to_double() would round every handle above 2^53. Same defect
        // SceneSerializer.cpp:159 names for SplashSprite, met again on the way in.
        // A texture slot of a .demat, as the material states it: the texture's header GUID and its path.
        struct TextureSlotIdentity
        {
            std::string Name;
            std::string Guid;
            std::string Path;
        };

        struct MaterialIdentityOnly
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            // Read so every texture the material binds is checked to exist and to BE the texture its slot
            // names: a missing texture draws as the default white, which in a memory instrument is a
            // measurement of nothing that looks like a measurement of something.
            std::optional<std::vector<TextureSlotIdentity>> Textures;
            // Everything else a material states: not this tool's to read, carried so the strict read
            // accepts it instead of refusing the file for keys it was never asked about.
            Common::Json::KeyedValues Rest;
        };

        // The material's own file is the only place its identity is written down, so the generator READS
        // it rather than carrying a copy of the number. A hard-coded GUID here would be a second statement
        // of an asset's identity and would go stale silently the day the material is re-imported -
        // MaterialIdentity's corpus rule is exactly that check, one level out.
        // What any header-reading caller outside the engine may ask: record what the file states, judged by
        // the build that loads the body (AssetHeaderReadContext::RecordOnly - this tool links Common only).
        const Common::Content::AssetHeaderReadContext kRecordOnly{ {}, true };

        // A texture slot's file, resolved and checked against the GUID the slot states.
        Common::BoolResultStr CheckTextureSlot( const std::filesystem::path& assetsRoot,
                                                const std::string& material, const TextureSlotIdentity& slot )
        {
            constexpr std::string_view kAssetsScheme = "assets:";
            const std::string          where         = "material '" + material + "' slot '" + slot.Name + "'";
            // An UNBOUND slot states neither (the shader samples its default); half a reference is a defect.
            if ( slot.Path.empty() && slot.Guid.empty() )
                return Common::MakeSuccess( true );
            if ( !slot.Path.starts_with( kAssetsScheme ) )
                return Common::MakeError<bool>( where + " names '" + slot.Path +
                                                "', not an assets: path - the generator cannot resolve it" );
            const auto file   = assetsRoot / slot.Path.substr( kAssetsScheme.size() );
            const auto header = Common::Content::ReadAssetHeader( file, kRecordOnly );
            if ( !header )
                return Common::MakeError<bool>( where + " texture '" + file.string() + "': " + header.GetError() );
            if ( header.GetValue().Kind != Common::Content::ContentKind::Texture )
                return Common::MakeError<bool>( where + " texture '" + file.string() + "' is not a texture file" );
            const std::string stated = Common::Content::AssetGuidToText( header.GetValue().Guid );
            if ( stated != slot.Guid )
                return Common::MakeError<bool>( where + " states GUID " + slot.Guid + " but '" + file.string() +
                                                "' is " + stated );
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<MaterialRef> LoadMaterial( const std::filesystem::path& assetsRoot,
                                                     const std::string&           relative )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( assetsRoot / relative );
            if ( !text.IsSuccess() )
                return Common::MakeError<MaterialRef>( "material '" + relative + "': " + text.GetError() );

            const auto parsed = Common::Json::Read<MaterialIdentityOnly>( text.GetValue() );
            if ( !parsed )
                return Common::MakeError<MaterialRef>( "material '" + relative +
                                                       "' states no readable header: " + parsed.GetError() );
            const auto& header = parsed.GetValue().Header;
            if ( !header )
                return Common::MakeError<MaterialRef>( "material '" + relative + "' has no header" );
            const auto guid = Common::Content::AssetGuidFromText( header->Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeError<MaterialRef>( "material '" + relative + "' states no GUID" );

            for ( const auto& slot : parsed.GetValue().Textures.value_or( std::vector<TextureSlotIdentity>{} ) )
            {
                const auto checked = CheckTextureSlot( assetsRoot, relative, slot );
                if ( !checked )
                    return Common::MakeError<MaterialRef>( checked.GetError() );
            }

            // A scene names a material by its header GUID's text (SCNE 27); the loader folds it to a handle.
            return Common::MakeSuccess<MaterialRef>(
                 { relative, Common::Content::AssetGuidToText( guid.GetValue() ) } );
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

        // A mesh file, read for what a scene must state about it: its header GUID (SCNE 28), its kind, and the
        // bounds it states (to seat it on the ground). Two on-disk forms are tracked and both are read here: an
        // AF1 envelope (`DAST`, bounds in its Meta section - StaticProbe.stmesh) and a cooked render file
        // (`DESTMESH` v3, bounds in its header - the Cooked/Meshes/*.skmesh probes). Kind and GUID come from
        // ReadAssetHeader, which sniffs both - the same entry point the registry cook reads identity through.
        // Every failure names the file: no asset is a refusal, never a stand-in.
        Common::ResultStr<MeshRef> LoadMesh( const std::filesystem::path& projectRoot, const std::string& relative,
                                             bool skinned )
        {
            const auto        file   = projectRoot / relative;
            const std::string where  = "mesh '" + relative + "' (" + file.string() + ")";
            const auto        header = Common::Content::ReadAssetHeader( file, kRecordOnly );
            if ( !header )
                return Common::MakeError<MeshRef>( where + ": " + header.GetError() );
            const auto expected =
                 skinned ? Common::Content::ContentKind::SkinnedMesh : Common::Content::ContentKind::StaticMesh;
            if ( header.GetValue().Kind != expected )
                return Common::MakeError<MeshRef>( where + " is not a " + ( skinned ? "skinned" : "static" ) +
                                                   " mesh file" );
            if ( header.GetValue().Guid.IsNull() )
                return Common::MakeError<MeshRef>( where + " states no GUID" );

            const auto bytes = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !bytes )
                return Common::MakeError<MeshRef>( where + ": " + bytes.GetError() );
            std::optional<std::array<float, 3>> lowest;
            std::optional<std::array<float, 3>> highest;
            if ( const auto cooked = Common::Content::ReadMeshHeaderBounds( bytes.GetValue() ) )
            {
                if ( cooked->Bounds )
                {
                    lowest  = std::array<float, 3>{ cooked->Bounds->Min[0], cooked->Bounds->Min[1],
                                                    cooked->Bounds->Min[2] };
                    highest = std::array<float, 3>{ cooked->Bounds->Max[0], cooked->Bounds->Max[1],
                                                    cooked->Bounds->Max[2] };
                }
            }
            else
            {
                const auto envelope = Common::Content::ReadAssetEnvelope(
                     std::as_bytes( std::span( bytes.GetValue().data(), bytes.GetValue().size() ) ), kRecordOnly );
                if ( !envelope )
                    return Common::MakeError<MeshRef>( where + ": " + envelope.GetError() );
                for ( const auto& section : envelope.GetValue().Sections )
                {
                    if ( section.Tag != Common::Content::EnvelopeSection::Meta )
                        continue;
                    const auto meta = Common::Content::DecodeEnvelopeMeta( section.Bytes );
                    if ( !meta )
                        return Common::MakeError<MeshRef>( where + " Meta: " + meta.GetError() );
                    if ( meta.GetValue().Bounds )
                    {
                        lowest  = meta.GetValue().Bounds->Lo;
                        highest = meta.GetValue().Bounds->Hi;
                    }
                }
            }
            if ( !lowest || !highest )
                return Common::MakeError<MeshRef>( where +
                                                   " states no bounds, so there is nothing to seat it by" );

            MeshRef mesh;
            mesh.Path       = relative;
            mesh.Guid       = Common::Content::AssetGuidToText( header.GetValue().Guid );
            mesh.Skinned    = skinned;
            mesh.BaseLiftCm = -( *lowest )[1];
            mesh.LoXCm      = ( *lowest )[0];
            mesh.HiXCm      = ( *highest )[0];
            mesh.LoZCm      = ( *lowest )[2];
            mesh.HiZCm      = ( *highest )[2];
            return Common::MakeSuccess( std::move( mesh ) );
        }

        // THE CORPUS THE `corpus` PRESETS ARE FURNISHED FROM - tracked files only, spelled as the tracked
        // scenes spell them (mesh paths relative to the project root, materials to the assets root).
        //
        // WHY THESE AND NOT Meshes/base*.fbx. The only textured meshes in the tree are the three base*.fbx,
        // and a scene naming one resolves through the DDC (MeshDerivedData.hpp LoadMeshSourceAsset: a DDC miss
        // is a refusal) - so on a machine that has not imported it the world would not load. The .stmesh files
        // beside them are pre-AF4h leftovers the loader never reads. The TEXTURES those meshes were imported
        // with are tracked and bound by tracked materials, so the texture memory is here all the same: on the
        // cooked skinned probes and the static probe.
        //
        // THEMES ARE CHOSEN FOR WHAT A CELL DEPARTURE CAN FREE (WP13 handover): skinned meshes and
        // custom-shader materials are HLOD-excluded, so their meshes, skeletons, materials and textures are
        // rooted only by the cell that holds them. `Checker` is the control - a static PBR prop whose mesh and
        // material an Instancing HLOD keeps resident after the cell leaves.
        struct CorpusProp
        {
            const char* Theme;
            const char* Mesh;
            bool        Skinned;
            const char* Material;
            int         ScalePercent;
        };

        constexpr CorpusProp kCorpusProps[] = {
             { "Skinned PBR", "Cooked/Meshes/SkinProbe.skmesh", true, "Materials/base_basic_pbr/model.demat",
               100 },
             { "Skinned PBR", "Cooked/Meshes/TwoBoneProbe.skmesh", true, "Materials/base_basic_pbr/model.demat",
               100 },
             { "Skinned shaded", "Cooked/Meshes/TwoBoneProbe.skmesh", true,
               "Materials/base_basic_shaded/model.demat", 100 },
             { "Skinned shaded", "Cooked/Meshes/IKProbe.skmesh", true, "Materials/base_basic_shaded/model.demat",
               100 },
             { "Witness", "Cooked/Meshes/IKProbe.skmesh", true, "Materials/M_NormalWitness.demat", 100 },
             { "Witness", "Resources/Assets/Meshes/StaticProbe.stmesh", false, "Materials/MP_Unlit.demat", 100 },
             { "Checker", "Resources/Assets/Meshes/StaticProbe.stmesh", false, "Materials/M_CheckerFloor.demat",
               100 },
        };

        // The table, resolved: one theme per distinct Theme name in first-appearance order, every file read
        // once. Fails on the first asset that does not resolve, naming it.
        Common::ResultStr<std::vector<PropTheme>> LoadCorpusThemes( const std::filesystem::path& projectRoot,
                                                                    const std::filesystem::path& assetsRoot )
        {
            std::vector<PropTheme> themes;
            for ( const auto& row : kCorpusProps )
            {
                auto mesh = LoadMesh( projectRoot, row.Mesh, row.Skinned );
                if ( !mesh )
                    return Common::MakeError<std::vector<PropTheme>>( mesh.GetError() );
                auto material = LoadMaterial( assetsRoot, row.Material );
                if ( !material )
                    return Common::MakeError<std::vector<PropTheme>>( material.GetError() );

                auto theme = std::find_if( themes.begin(), themes.end(),
                                           [&]( const PropTheme& t ) { return t.Name == row.Theme; } );
                if ( theme == themes.end() )
                    theme = themes.insert( themes.end(), PropTheme{ row.Theme, {} } );
                theme->Props.push_back( { mesh.GetValue(), material.GetValue(), row.ScalePercent } );
            }
            return Common::MakeSuccess( std::move( themes ) );
        }

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
        std::string        projectRoot = "Editor";
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
            else if ( a == "--project" && value( v ) )
                projectRoot = v;
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
        spec.CellSizeCm = cellSize.value_or( preset->CellSizeCm );
        spec.DistrictCells = preset->DistrictCells;
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

        std::vector<PropTheme> themes;
        if ( preset->Corpus )
        {
            auto loaded = LoadCorpusThemes( projectRoot, assets );
            if ( !loaded )
            {
                err << "WorldGen: " << loaded.GetError() << "\n"
                    << "WorldGen: preset '" << presetKey << "' is furnished from tracked assets only; --project '"
                    << projectRoot << "' / --assets '" << assetsRoot
                    << "' must be the project and assets roots.\n";
                return 3;
            }
            themes = loaded.ExtractValue();
        }

        // Regenerating an existing world keeps that file's identity; a first write mints one. Resolved once,
        // so the --verify rebuild below produces the same bytes.
        const Common::Content::AssetGuid worldGuid =
             ExistingWorldGuid( outPath ).value_or( Common::Content::AssetGuid::Generate() );

        const auto build = [&]( std::string& json, WorldStats& stats ) -> bool
        {
            auto scene = BuildWorld( spec, palette, ground.GetValue(), themes, worldGuid, stats );

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

            json = Common::Json::Write( scene );
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

        // A prop that cannot sit inside its slot crosses its tile, and the partition promotes it - to a level
        // whose loading range spans districts, or to always-loaded where it straddles an axis. Its theme's assets
        // would then be resident everywhere, which is the one thing this instrument must not do (WP14b: three
        // textures kept for the whole flight by two props that crossed x = 0).
        if ( stats.PropsOverhanging > 0 )
        {
            err << "WorldGen: " << stats.PropsOverhanging << " corpus prop(s) are wider than their "
                << spec.CellSizeCm / std::max( 1, static_cast<int>( std::ceil( std::sqrt( spec.PerCell ) ) ) )
                << " cm slot and would cross their tile; lower --per-cell or raise --cell-size\n";
            return 8;
        }

        out << "WorldGen wrote " << outPath << "\n"
            << "  scene name   : " << spec.Name << " (preset '" << presetKey << "')\n"
            << "  entities     : " << stats.Entities << "  (" << stats.Buildings << " buildings, "
            << stats.GroundTiles << " ground tiles, " << stats.Props << " corpus props of which "
            << stats.SkinnedProps << " skinned, 3 fixtures)\n"
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
            const auto parsed = Common::Json::Read<Core::SceneSerialized>( json );
            if ( !parsed )
            {
                err << "WorldGen: the written scene does not parse back: " << parsed.GetError() << "\n";
                return 7;
            }
            if ( !parsed.GetValue().WorldPartition.has_value() )
            {
                err << "WorldGen: the written scene parses back without its WorldPartition block\n";
                return 7;
            }
            const auto& scene = parsed.GetValue();
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
