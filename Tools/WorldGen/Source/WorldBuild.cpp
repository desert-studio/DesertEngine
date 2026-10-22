#include "WorldBuild.hpp"

#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <Engine/World/Landscape/LandscapeEditLayers.hpp>
#include <Engine/World/Landscape/LandscapeGenerator.hpp>

#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <fstream>
#include <optional>

namespace Desert::WorldGen
{
    namespace
    {
        // splitmix64. Sixteen lines, no state shared between cells, and identical on every compiler -
        // which <random>'s distributions are not (see the header). The constants are Steele/Vigna's.
        constexpr uint64_t Mix( uint64_t x )
        {
            x += 0x9E3779B97F4A7C15ull;
            x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
            x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
            return x ^ ( x >> 31 );
        }

        // A cell's stream, addressed by the cell's PLACE IN THE WORLD rather than advanced by iteration.
        // `draw` is the index of the number inside the cell, so a cell answers the same sequence whoever
        // asks and in whatever order - the property a per-cell regeneration would need on the day step 8
        // streams these cells.
        //
        // THE COORDINATES ARE SIGNED AND CENTRED, NOT GRID INDICES, AND THE FIRST VERSION OF THIS FUNCTION
        // USED INDICES. The grid is centred on the origin, so the cell at world (0,0) is index 16 in a
        // 32-cell world and index 1 in a 2-cell one - and seeding from the index meant that GROWING the
        // world regenerated every building in it, including the ones at places that had not moved. The
        // test WorldSceneGenerator.TheSamePlaceInTheWorldHoldsTheSameBuildings caught it on the first run;
        // the property it checks is not decoration, it is what makes "generate a bigger world" a bigger
        // world and not a different one.
        constexpr uint64_t CellDraw( uint64_t seed, int cx, int cz, int draw )
        {
            return Mix( Mix( Mix( seed ) ^ ( static_cast<uint64_t>( cx ) * 0x0000'0001'0000'0001ull ) ) ^
                        ( static_cast<uint64_t>( cz ) * 0x0000'0100'0000'0FFFull ) ^
                        ( static_cast<uint64_t>( draw ) * 0x1000'0000'0000'0001ull ) );
        }

        // A uniform integer in [lo, hi]. Modulo, deliberately: the bias over a range of a few dozen against
        // a 64-bit draw is under 2^-58, and rejection sampling would make the stream's length depend on the
        // values it produced - which is the one thing a per-cell addressed stream must not do.
        int Range( uint64_t bits, int lo, int hi )
        {
            return lo + static_cast<int>( bits % static_cast<uint64_t>( hi - lo + 1 ) );
        }

        // A component payload written through the ENGINE'S OWN mirror struct rather than by hand, so the
        // key names and their order are the struct's and cannot drift from what the loader reads.
        template <typename T>
        Common::Json::Value AsBlock( const T& value )
        {
            // The value tree the writer's own text would parse back to (FromStruct is proven byte-identical
            // to Write per field kind), without a text round trip that could only fail by being wrong.
            return Common::Json::FromStruct( value );
        }

        Common::Json::Value Num( double v )
        {
            return { v };
        }

        Assets::EntityData MakeEntity( uint64_t id, std::string tag, glm::vec3 translation, glm::vec3 rotation,
                                       glm::vec3 scale )
        {
            Assets::EntityData e;
            e.id          = Common::UUID( id );
            e.Tag         = std::move( tag );
            e.Translation = translation;
            e.Rotation    = rotation;
            e.Scale       = scale;
            return e;
        }

        // A cube from the primitive factory is 100 cm on a side and centred on its origin
        // (Engine/Geometry/PrimitiveMeshFactory.cpp:17), so a scale of S is S metres and a box whose base
        // sits on y = 0 is translated by half its height. Every conversion in this file goes through these
        // two lines rather than being open-coded, because "the cube is 100 units" is the kind of constant
        // that gets remembered wrongly in the second place it is written.
        constexpr float kCubeEdgeCm = 100.0f;

        // Salts the seed for the corpus theme offset, so it is not the same number as any cell's first draw.
        constexpr uint64_t kThemeSalt = 0x7E3A'5C1D'0B29'4F86ull;

        glm::vec3 BoxScale( int widthCm, int heightCm, int depthCm )
        {
            return { static_cast<float>( widthCm ) / kCubeEdgeCm, static_cast<float>( heightCm ) / kCubeEdgeCm,
                     static_cast<float>( depthCm ) / kCubeEdgeCm };
        }
    } // namespace

    Core::SceneSerialized BuildWorld( const WorldSpec& spec, const std::vector<MaterialRef>& buildingMaterials,
                                      const MaterialRef& groundMaterial, const std::vector<PropTheme>& themes,
                                      const Common::Content::AssetGuid& guid, WorldStats& stats )
    {
        Core::SceneSerialized scene;
        scene.SceneName = spec.Name;
        // The GUID is the caller's: kept from the output file's own header when regenerating (see
        // ExistingWorldGuid below), freshly minted for a file that does not exist yet - the same rule every
        // other text asset follows. NOT derived from the spec's name (architect's decision, AF6k): a name
        // is not an identity, and two never-before-written files of one spec must not collide on it.
        scene.Header = Common::Content::MakeTextHeader( Common::Content::ContentKind::Scene, guid,
                                                        Core::SceneTextSubsystems() );

        stats          = WorldStats{};
        stats.Cells    = spec.Cells * spec.Cells;
        stats.ExtentCm = static_cast<int64_t>( spec.Cells ) * spec.CellSizeCm;

        uint64_t nextId = 1000;

        // ------------------------------------------------------------------------------------------
        // Fixtures. Three entities, first in the file so they can be found in an outliner holding fifty
        // thousand rows. A world with no light and no camera is a black frame, and a black frame is
        // indistinguishable from a broken one - which is the whole failure mode the programme's §0.3 is
        // about, one level up.
        // ------------------------------------------------------------------------------------------
        {
            auto sun = MakeEntity( nextId++, "Sun", { -0.3f, -0.55f, -0.78f }, { 0.0f, 0.0f, 0.0f },
                                   { 1.0f, 1.0f, 1.0f } );
            Common::Json::Object light;
            light["Color"] =
                 Common::Json::Value( Common::Json::Value::Array{ Num( 1.0 ), Num( 1.0 ), Num( 1.0 ) } );
            light["Intensity"]               = Num( 1.0 );
            sun.Components["DirectionLight"] = Common::Json::Value( std::move( light ) );
            scene.Entities.push_back( std::move( sun ) );

            auto sky =
                 MakeEntity( nextId++, "Sky", { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } );
            Common::Json::Object skybox;
            Common::Json::Object noSkybox; // SCNE 29 empty reference: {Guid, Path}
            noSkybox["Guid"]         = Common::Json::Value( std::string{} );
            noSkybox["Path"]         = Common::Json::Value( std::string{} );
            skybox["SkyboxHandle"]   = Common::Json::Value( std::move( noSkybox ) );
            skybox["Intensity"]      = Num( 1.0 );
            sky.Components["Skybox"] = Common::Json::Value( std::move( skybox ) );
            Common::Json::Object atmosphere;
            atmosphere["Enabled"]           = Common::Json::Value( true );
            atmosphere["SkyBrightness"]     = Num( 1.0 );
            atmosphere["SunIntensity"]      = Num( 22.0 );
            atmosphere["TimeOfDay"]         = Num( 12.0 );
            sky.Components["SkyAtmosphere"] = Common::Json::Value( std::move( atmosphere ) );
            scene.Entities.push_back( std::move( sky ) );

            // Eye height, at the centre of the world, looking down the street. 170 cm is a person, and
            // the acceptance criterion is a person running - a camera parked at altitude would hide
            // exactly the pop-in the programme's §0.3 forbids.
            auto camera = MakeEntity( nextId++, "Camera", { 0.0f, 170.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
                                      { 1.0f, 1.0f, 1.0f } );
            Common::Json::Object cam;
            cam["AutoActivateForPlayer"] = Common::Json::Value( true );
            cam["FOV"]          = Num( 75.0 );
            cam["Near"]         = Num( 10.0 );
            // Far is the world's DIAGONAL and not a round number pulled from another scene: a far plane
            // shorter than the world silently culls the far half of it, which would look exactly like the
            // frustum culling this programme has not built yet and would be read as one.
            cam["Far"]                  = Num( static_cast<double>( stats.ExtentCm ) * 1.5 );
            camera.Components["Camera"] = Common::Json::Value( std::move( cam ) );
            scene.Entities.push_back( std::move( camera ) );
        }

        // ------------------------------------------------------------------------------------------
        // The world itself.
        //
        // ONE ENTITY PER BUILDING, AND NOT AN InstancedStaticMesh. A single ISM holding fifty thousand
        // transforms is ONE draw call, and the first thing the programme measures (§3) is that we issue a
        // draw call for every object in the scene whether it is on screen or not. An instanced world would
        // hide the defect the instrument exists to expose - and it would also not be a world you can
        // select, move, or stream a piece of, which is what steps 4 and 8 are about.
        // ------------------------------------------------------------------------------------------
        const int half = spec.Cells / 2;

        // A sub-grid of slots inside each cell, so buildings cannot overlap: one per slot, jittered inside
        // it. Overlapping boxes would make the same triangle count read as fewer visible objects.
        const int slots   = static_cast<int>( std::ceil( std::sqrt( static_cast<double>( spec.PerCell ) ) ) );
        const int spacing = spec.CellSizeCm / std::max( slots, 1 );

        for ( int cz = 0; cz < spec.Cells; ++cz )
        {
            for ( int cx = 0; cx < spec.Cells; ++cx )
            {
                const int originX = ( cx - half ) * spec.CellSizeCm;
                const int originZ = ( cz - half ) * spec.CellSizeCm;

                char cellTag[32];
                std::snprintf( cellTag, sizeof( cellTag ), "C%02d_%02d", cx, cz );

                // The ground tile. One per cell rather than one slab for the whole world, because a cell
                // has to be a thing that can be present or absent on its own - that is what step 8 streams
                // and what step 6 packs, and a single world-sized slab would be resident always.
                {
                    // Whole centimetres first, float second. The tile centre is an integer by
                    // construction, and halving inside the cast is an integer division that merely LOOKS
                    // like it produces the float it is assigned to.
                    const int centreX = originX + spec.CellSizeCm / 2;
                    const int centreZ = originZ + spec.CellSizeCm / 2;

                    auto ground =
                         MakeEntity( nextId++, std::string( cellTag ) + "_Ground",
                                     { static_cast<float>( centreX ), -25.0f, static_cast<float>( centreZ ) },
                                     { 0.0f, 0.0f, 0.0f }, BoxScale( spec.CellSizeCm, 50, spec.CellSizeCm ) );

                    Assets::StaticMeshComponentSer mesh;
                    mesh.MaterialPaths              = std::vector<std::string>{ groundMaterial.Path };
                    mesh.MaterialGuids              = std::vector<std::string>{ groundMaterial.Guid };
                    mesh.Primitive                  = Geometry::PrimitiveType::Cube;
                    ground.Components["StaticMesh"] = AsBlock( mesh );
                    scene.Entities.push_back( std::move( ground ) );
                    ++stats.GroundTiles;
                }

                if ( !themes.empty() )
                {
                    // The district's theme: a stripe pattern over the district's SIGNED address (so growing the
                    // world keeps every place's theme, as for the buildings), shifted by the seed. Not a random
                    // draw per district: two neighbouring districts drawing the same theme would hold its assets
                    // across the loading range and the flight would show a plateau where it should show a fall.
                    const int  district = std::max( spec.DistrictCells, 1 );
                    const auto floorDiv = [district]( int v )
                    { return v >= 0 ? v / district : -( ( -v + district - 1 ) / district ); };
                    const int   themeCount = static_cast<int>( themes.size() );
                    const int   offset     = Range( Mix( spec.Seed ^ kThemeSalt ), 0, themeCount - 1 );
                    const int   stripe     = floorDiv( cx - half ) + floorDiv( cz - half ) + offset;
                    const auto& theme =
                         themes[static_cast<size_t>( ( stripe % themeCount + themeCount ) % themeCount )];
                    for ( int i = 0; i < spec.PerCell; ++i )
                    {
                        const int      sx = i % slots;
                        const int      sz = i / slots;
                        const uint64_t d0 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 0 );
                        const uint64_t d1 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 1 );
                        const uint64_t d2 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 2 );

                        const PropRef& prop = theme.Props[static_cast<size_t>(
                             Range( d0, 0, static_cast<int>( theme.Props.size() ) - 1 ) )];

                        // A quarter of the slot either way: the prop stays inside its slot, so inside its tile,
                        // whatever its footprint within half a slot.
                        const float scale = static_cast<float>( prop.ScalePercent ) / 100.0f;

                        // Up to a quarter of the slot either way, but never so far that the prop's own footprint
                        // leaves the slot: a prop across a tile edge is promoted by the partition, and one across
                        // an axis is always-loaded (WP14b measured both). Offsets are whole centimetres.
                        const int  slotHalf = spacing / 2;
                        const auto range    = [&]( float lo, float hi, uint64_t bits, int& offset ) -> bool
                        {
                            const int least =
                                 std::max( -spacing / 4, static_cast<int>( std::ceil( -slotHalf - lo * scale ) ) );
                            const int most =
                                 std::min( spacing / 4, static_cast<int>( std::floor( slotHalf - hi * scale ) ) );
                            if ( least > most )
                                return false;
                            offset = Range( bits, least, most );
                            return true;
                        };
                        int dx = 0;
                        int dz = 0;
                        if ( !range( prop.Mesh.LoXCm, prop.Mesh.HiXCm, d1, dx ) ||
                             !range( prop.Mesh.LoZCm, prop.Mesh.HiZCm, d2, dz ) )
                            ++stats.PropsOverhanging;
                        const int x = originX + sx * spacing + slotHalf + dx;
                        const int z = originZ + sz * spacing + slotHalf + dz;

                        char tag[48];
                        std::snprintf( tag, sizeof( tag ), "%s_P%02d", cellTag, i );
                        auto entity = MakeEntity(
                             nextId++, tag,
                             { static_cast<float>( x ), prop.Mesh.BaseLiftCm * scale, static_cast<float>( z ) },
                             { 0.0f, 0.0f, 0.0f }, { scale, scale, scale } );

                        if ( prop.Mesh.Skinned )
                        {
                            Assets::SkinnedMeshComponentSer mesh;
                            mesh.MeshPath                    = prop.Mesh.Path;
                            mesh.MeshGuid                    = prop.Mesh.Guid;
                            mesh.MaterialPaths               = std::vector<std::string>{ prop.Material.Path };
                            mesh.MaterialGuids               = std::vector<std::string>{ prop.Material.Guid };
                            entity.Components["SkinnedMesh"] = AsBlock( mesh );
                            ++stats.SkinnedProps;
                        }
                        else
                        {
                            Assets::StaticMeshComponentSer mesh;
                            mesh.MeshPath                   = prop.Mesh.Path;
                            mesh.MeshGuid                   = prop.Mesh.Guid;
                            mesh.MaterialPaths              = std::vector<std::string>{ prop.Material.Path };
                            mesh.MaterialGuids              = std::vector<std::string>{ prop.Material.Guid };
                            entity.Components["StaticMesh"] = AsBlock( mesh );
                        }
                        scene.Entities.push_back( std::move( entity ) );
                        ++stats.Props;
                    }
                    continue;
                }

                for ( int i = 0; i < spec.PerCell; ++i )
                {
                    const int sx = i % slots;
                    const int sz = i / slots;

                    const uint64_t d0 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 0 );
                    const uint64_t d1 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 1 );
                    const uint64_t d2 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 2 );
                    const uint64_t d3 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 3 );
                    const uint64_t d4 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 4 );
                    const uint64_t d5 = CellDraw( spec.Seed, cx - half, cz - half, i * 8 + 5 );

                    // Whole metres, so the float that reaches the file is exact (header, second bullet).
                    const int widthCm  = Range( d0, 4, 14 ) * 100;
                    const int depthCm  = Range( d1, 4, 14 ) * 100;
                    const int heightCm = Range( d2, 3, 40 ) * 100;

                    // A QUARTER TURN IS A SWAP OF THE TWO GROUND EDGES, NOT A YAW. A box has fourfold
                    // symmetry, so turning it by 90 degrees is the same box with width and depth exchanged
                    // (and 180 is the box itself). This used to be written as a yaw of 0/90/180/270 into
                    // `Rotation` - which the engine reads in RADIANS (TransformComponent::GetTransform takes
                    // glm::quat of it), so every building but a quarter of them stood at 116, 233 or 350
                    // degrees, overhung its slot, and 1015 of 49152 crossed their tile edge in the `world`
                    // preset (29 of them across an axis, so always-loaded under World Partition). Swapping the
                    // edges keeps every number an integer centimetre (header, second bullet) and keeps the
                    // building inside its slot, because the room below is taken from the edges it actually has on
                    // the ground.
                    const bool turned  = Range( d5, 0, 3 ) % 2 == 1;
                    const int  footXCm = turned ? depthCm : widthCm;
                    const int  footZCm = turned ? widthCm : depthCm;

                    const int slotCentreX = originX + sx * spacing + spacing / 2;
                    const int slotCentreZ = originZ + sz * spacing + spacing / 2;
                    const int roomX       = std::max( ( spacing - footXCm ) / 2, 0 );
                    const int roomZ       = std::max( ( spacing - footZCm ) / 2, 0 );

                    const int x = slotCentreX + Range( d3, -roomX, roomX );
                    const int z = slotCentreZ + Range( d4, -roomZ, roomZ );

                    char tag[48];
                    std::snprintf( tag, sizeof( tag ), "%s_B%02d", cellTag, i );

                    auto building = MakeEntity( nextId++, tag,
                                                { static_cast<float>( x ), static_cast<float>( heightCm ) / 2.0f,
                                                  static_cast<float>( z ) },
                                                { 0.0f, 0.0f, 0.0f }, BoxScale( footXCm, heightCm, footZCm ) );

                    Assets::StaticMeshComponentSer mesh;
                    const auto&                    material = buildingMaterials[static_cast<size_t>(
                         Range( Mix( d5 ), 0, static_cast<int>( buildingMaterials.size() ) - 1 ) )];
                    mesh.MaterialPaths                      = std::vector<std::string>{ material.Path };
                    mesh.MaterialGuids                      = std::vector<std::string>{ material.Guid };
                    mesh.Primitive                          = Geometry::PrimitiveType::Cube;
                    building.Components["StaticMesh"]       = AsBlock( mesh );
                    scene.Entities.push_back( std::move( building ) );
                    ++stats.Buildings;
                }
            }
        }

        // Each record's place among its siblings, stated the way the editor's saver states it (SceneSerializer's
        // SiblingIndexOf: roots numbered in load order, a child by its place in its parent's children, which is
        // load order too). Left unstated, the first editor save of a generated world adds it to EVERY record, and
        // a partitioned world's first save after moving one entity rewrites every entity file (WP16b). A
        // partitioned world then drops the ROOTS' index again (WorldGenMain, --partition): its root order is
        // not persisted.
        std::map<uint64_t, uint32_t> nextUnder; // parent id -> next sibling index; 0 = the roots
        for ( auto& record : scene.Entities )
            record.siblingIndex = nextUnder[record.parent ? static_cast<uint64_t>( *record.parent ) : 0]++;

        stats.Entities = static_cast<int>( scene.Entities.size() );
        return scene;
    }

    namespace
    {
        constexpr uint64_t kLandscapeSalt = 0x4C41'4E44'5343'4150ull; // "LANDSCAP": foliage seeds

        Common::Json::Value Int( int64_t v )
        {
            return Common::Json::Value( v );
        }

        Common::Json::Value Vec3( double x, double y, double z )
        {
            return Common::Json::Value( Common::Json::Value::Array{ Num( x ), Num( y ), Num( z ) } );
        }

        Common::Json::Value Handle( const std::string& guid, const std::string& path )
        {
            Common::Json::Object h;
            h["Guid"] = Common::Json::Value( guid );
            h["Path"] = Common::Json::Value( path );
            return Common::Json::Value( std::move( h ) );
        }

        // In FIELD_Barn.desce's key order (the saver's).
        Common::Json::Value FoliageVolume( const std::vector<FoliageTypeRef>& types, int halfExtentCm,
                                           int heightCm, int seed )
        {
            Common::Json::Object pf;
            pf["Extent"]              = Vec3( halfExtentCm, heightCm, halfExtentCm );
            pf["TileSize"]            = Num( 3000.0 );
            pf["MinimumQuadTreeSize"] = Num( 100.0 );
            pf["NumUniqueTiles"]      = Int( 10 );
            pf["RandomSeed"]          = Int( seed );
            pf["TileOverlap"]         = Num( 0.0 );
            pf["AllowLandscape"]      = Common::Json::Value( true );
            pf["AllowStaticMesh"]     = Common::Json::Value( false );
            Common::Json::Value::Array list;
            for ( const auto& t : types )
                list.push_back( Handle( t.Guid, t.Path ) );
            pf["FoliageTypes"] = Common::Json::Value( std::move( list ) );
            return Common::Json::Value( std::move( pf ) );
        }

        // The three weight planes of one tile from its heights; integer only (header).
        std::vector<World::Landscape::LandscapeWeightLayer> LandscapeWeights( const LandscapeWorldSpec&    spec,
                                                                             const LandscapeContent&      c,
                                                                             const std::vector<uint16_t>& h,
                                                                             uint32_t                     n )
        {
            std::vector<World::Landscape::LandscapeWeightLayer> out( 3 );
            out[0].Name = c.Dirt.LayerName;
            out[1].Name = c.Grass.LayerName;
            out[2].Name = c.Dry.LayerName;
            for ( auto& l : out )
                l.Weights.assign( static_cast<size_t>( n ) * n, uint8_t{ 0 } );
            const int64_t last = static_cast<int64_t>( n ) - 1;
            const auto    at   = [&]( int64_t x, int64_t z ) -> int64_t
            { return h[static_cast<size_t>( std::clamp<int64_t>( z, 0, last ) * n + std::clamp<int64_t>( x, 0, last ) )]; };
            const int64_t runCm      = 2ll * spec.SpacingCm; // central difference
            const int64_t mid        = 32768;
            const int64_t hollowUnit = std::max<int64_t>( 1, ( spec.NoiseHeightCm / 4 ) * 128 / spec.ZScale );
            for ( int64_t z = 0; z <= last; ++z )
                for ( int64_t x = 0; x <= last; ++x )
                {
                    const int64_t dx      = std::llabs( at( x + 1, z ) - at( x - 1, z ) );
                    const int64_t dz      = std::llabs( at( x, z + 1 ) - at( x, z - 1 ) );
                    const int64_t permil  = std::max( dx, dz ) * spec.ZScale * 1000 / ( 128 * runCm );
                    const int64_t dirt    = std::clamp<int64_t>( ( permil - 350 ) * 255 / 250, 0, 255 );
                    const int64_t dryFull = std::clamp<int64_t>( ( mid - at( x, z ) ) * 255 / hollowUnit, 0, 255 );
                    const int64_t dry     = dryFull * ( 255 - dirt ) / 255;
                    const size_t  i       = static_cast<size_t>( z * n + x );
                    out[0].Weights[i]     = static_cast<uint8_t>( dirt );
                    out[2].Weights[i]     = static_cast<uint8_t>( dry );
                    out[1].Weights[i]     = static_cast<uint8_t>( 255 - dirt - dry );
                }
            return out;
        }
    } // namespace

    Common::ResultStr<LandscapeWorld> BuildLandscapeWorld( const LandscapeWorldSpec&         spec,
                                                           const LandscapeContent&           content,
                                                           const std::string&                heightFileKeyPrefix,
                                                           const Common::Content::AssetGuid& guid,
                                                           WorldStats&                       stats )
    {
        namespace LS = World::Landscape;
        LS::LandscapeGenerateSettings gen;
        gen.QuadsPerTile  = static_cast<uint32_t>( spec.QuadsPerTile );
        gen.TilesX        = spec.TilesPerSide;
        gen.TilesZ        = spec.TilesPerSide;
        gen.SpacingCm     = static_cast<float>( spec.SpacingCm );
        gen.ZScale        = static_cast<float>( spec.ZScale );
        gen.Fill          = LS::LandscapeGenerateFill::Noise;
        gen.Seed          = spec.Seed;
        gen.NoiseHeightCm = static_cast<float>( spec.NoiseHeightCm );
        if ( auto valid = LS::ValidateLandscapeGenerate( gen ); !valid )
            return Common::MakeError<LandscapeWorld>( "landscape: " + valid.GetError() );
        auto generated = LS::GenerateLandscape( gen );
        if ( !generated )
            return Common::MakeError<LandscapeWorld>( "landscape: " + generated.GetError() );
        auto g = generated.ExtractValue();

        LandscapeWorld world;
        auto&          scene = world.Scene;
        scene.SceneName      = spec.Name;
        scene.Header         = Common::Content::MakeTextHeader( Common::Content::ContentKind::Scene, guid,
                                                                Core::SceneTextSubsystems() );
        stats                = WorldStats{};
        const int extentCm   = spec.TilesPerSide * spec.QuadsPerTile * spec.SpacingCm;
        stats.ExtentCm       = extentCm;
        stats.Cells          = spec.TilesPerSide * spec.TilesPerSide;

        const std::vector<LS::LandscapeLayerRule> rules = {
             { content.Dirt.LayerName, content.Dirt.Hardness, content.Dirt.NoWeightBlend },
             { content.Grass.LayerName, content.Grass.Hardness, content.Grass.NoWeightBlend },
             { content.Dry.LayerName, content.Dry.Hardness, content.Dry.NoWeightBlend },
        };
        const Common::UUID base( LS::kLandscapeBaseEditLayerGuid );
        const glm::vec3    zero( 0.0f );
        const glm::vec3    one( 1.0f );

        uint64_t nextId = 1000;
        // Fixtures: sun, sky, wind (the grass bends with it), camera at eye height over the world's centre.
        {
            auto                 sun = MakeEntity( nextId++, "Sun", { 0.709f, -0.309f, 0.633f }, zero, one );
            Common::Json::Object light;
            light["Color"]                   = Vec3( 1.0, 0.97, 0.92 );
            light["Intensity"]               = Num( 16.0 );
            light["AtmosphereSunLight"]      = Common::Json::Value( true );
            light["CastShadows"]             = Common::Json::Value( true );
            sun.Components["DirectionLight"] = Common::Json::Value( std::move( light ) );
            scene.Entities.push_back( std::move( sun ) );

            auto                 sky = MakeEntity( nextId++, "Sky", zero, zero, one );
            Common::Json::Object atmosphere;
            atmosphere["Enabled"]           = Common::Json::Value( true );
            atmosphere["SkyBrightness"]     = Num( 1.0 );
            sky.Components["SkyAtmosphere"] = Common::Json::Value( std::move( atmosphere ) );
            scene.Entities.push_back( std::move( sky ) );

            auto                 wind = MakeEntity( nextId++, "Wind Source", zero, zero, one );
            Common::Json::Object w;
            w["Direction"]                = Vec3( 0.8, 0.0, 0.6 );
            w["Speed"]                    = Num( 1500.0 );
            w["PointWind"]                = Common::Json::Value( false );
            w["Radius"]                   = Num( 1000.0 );
            wind.Components["WindSource"] = Common::Json::Value( std::move( w ) );
            scene.Entities.push_back( std::move( wind ) );

            // World (0, 0) is sample (0, 0) of tile (N/2, N/2): the generator centres the map on the origin.
            // (h - mid) * ZScale / 128 is a multiple of 1/32 cm: exact in a float.
            float groundCm = 0.0f;
            for ( const auto& t : g.Tiles )
                if ( t.TileX == spec.TilesPerSide / 2 && t.TileZ == spec.TilesPerSide / 2 )
                    groundCm = static_cast<float>( ( static_cast<int64_t>( t.Heights.Samples()[0] ) - 32768 ) *
                                                   spec.ZScale ) /
                               128.0f;
            auto camera = MakeEntity( nextId++, "Camera", { 0.0f, groundCm + 170.0f, 0.0f }, zero, one );
            Common::Json::Object cam;
            cam["AutoActivateForPlayer"] = Common::Json::Value( true );
            cam["FOV"]                   = Num( 75.0 );
            cam["Near"]                  = Num( 10.0 );
            cam["Far"]                   = Num( static_cast<double>( extentCm ) * 1.5 );
            camera.Components["Camera"]  = Common::Json::Value( std::move( cam ) );
            scene.Entities.push_back( std::move( camera ) );
        }

        // The root, as CreateLandscapeCommand::Redo makes it (LandscapeLayerCommands.cpp:112-135), in the
        // saver's key order (FIELD_Barn.desce:228-260).
        const uint64_t rootId = nextId++;
        {
            auto                 root = MakeEntity( rootId, "Landscape", g.Root.Origin, zero, one );
            Common::Json::Object ls;
            ls["QuadsPerTile"] = Int( spec.QuadsPerTile );
            ls["SpacingCm"]    = Num( spec.SpacingCm );
            ls["ZScale"]       = Num( spec.ZScale );
            Common::Json::Value::Array layers;
            for ( const auto& l : g.EditLayers.Layers )
            {
                Common::Json::Object e;
                e["Guid"]        = Common::Json::Value( std::to_string( static_cast<uint64_t>( l.Guid ) ) );
                e["Name"]        = Common::Json::Value( l.Name );
                e["Visible"]     = Common::Json::Value( l.Visible );
                e["Locked"]      = Common::Json::Value( l.Locked );
                e["HeightAlpha"] = Num( l.HeightAlpha );
                e["WeightAlpha"] = Num( l.WeightAlpha );
                layers.push_back( Common::Json::Value( std::move( e ) ) );
            }
            ls["EditLayers"] = Common::Json::Value( std::move( layers ) );
            ls["Layers"]     = Common::Json::Value( Common::Json::Value::Array{
                 Handle( content.Dirt.Guid, content.Dirt.Path ), Handle( content.Grass.Guid, content.Grass.Path ),
                 Handle( content.Dry.Guid, content.Dry.Path ) } );
            root.Components["Landscape"] = Common::Json::Value( std::move( ls ) );
            Common::Json::Object material;
            material["Material"]                 = Common::Json::Value( content.Material );
            root.Components["LandscapeMaterial"] = Common::Json::Value( std::move( material ) );
            scene.Entities.push_back( std::move( root ) );
        }

        // Foliage volumes over the whole extent, seeds drawn from the spec's seed.
        {
            const int height = spec.NoiseHeightCm * 2;
            auto      meadow = MakeEntity( nextId++, "Meadow Foliage", zero, zero, one );
            meadow.Components["ProceduralFoliage"] = FoliageVolume(
                 content.Meadow, extentCm / 2, height, Range( Mix( spec.Seed ^ kLandscapeSalt ), 1, 1'000'000 ) );
            scene.Entities.push_back( std::move( meadow ) );
            auto trees = MakeEntity( nextId++, "Tree Foliage", zero, zero, one );
            trees.Components["ProceduralFoliage"] =
                 FoliageVolume( content.Trees, extentCm / 2, height,
                                Range( Mix( Mix( spec.Seed ^ kLandscapeSalt ) ), 1, 1'000'000 ) );
            scene.Entities.push_back( std::move( trees ) );
        }

        // Tiles: weights into the Base edit layer, then merged over the whole tile.
        for ( auto& t : g.Tiles )
        {
            const uint64_t id    = nextId++;
            auto&          data  = t.Heights;
            const auto*    layer = data.FindEditLayer( base );
            if ( layer == nullptr )
                return Common::MakeError<LandscapeWorld>( "landscape: a generated tile carries no Base layer" );
            LS::LandscapeEditLayerTileData paint = *layer;
            paint.Weights = LandscapeWeights( spec, content, paint.Heights, data.SamplesX() );
            if ( auto set = data.SetEditLayer( std::move( paint ) ); !set )
                return Common::MakeError<LandscapeWorld>( "landscape: " + set.GetError() );
            const LS::LandscapeRect all{ 0u, 0u, data.SamplesX(), data.SamplesZ() };
            if ( auto merged = LS::MergeLandscapeEditLayers( g.EditLayers, rules, all, data ); !merged )
                return Common::MakeError<LandscapeWorld>( "landscape: " + merged.GetError() );

            char tag[48];
            std::snprintf( tag, sizeof( tag ), "Landscape Tile %d_%d", t.TileX, t.TileZ );
            auto                 entity = MakeEntity( id, tag, zero, zero, one );
            Common::Json::Object tile;
            tile["Landscape"]  = Common::Json::Value( std::to_string( rootId ) );
            tile["TileX"]      = Int( t.TileX );
            tile["TileZ"]      = Int( t.TileZ );
            tile["HeightFile"] = Common::Json::Value( heightFileKeyPrefix + std::to_string( id ) + ".dlht" );
            entity.Components["LandscapeTile"] = Common::Json::Value( std::move( tile ) );
            scene.Entities.push_back( std::move( entity ) );
            world.Tiles.push_back( { id, std::move( data ) } );
            ++stats.GroundTiles;
        }

        std::map<uint64_t, uint32_t> nextUnder;
        for ( auto& record : scene.Entities )
            record.siblingIndex = nextUnder[record.parent ? static_cast<uint64_t>( *record.parent ) : 0]++;
        stats.Entities = static_cast<int>( scene.Entities.size() );
        return Common::MakeSuccess( std::move( world ) );
    }

    std::optional<Common::Content::AssetGuid> ExistingWorldGuid( const std::filesystem::path& outputFile )
    {
        std::ifstream in( outputFile, std::ios::binary );
        if ( !in )
            return std::nullopt;
        const auto object = Common::Content::ReadTextHeaderObject( in );
        if ( !object.IsSuccess() )
            return std::nullopt;
        const auto header = Common::Content::ParseTextHeaderObject( object.GetValue() );
        if ( !header.IsSuccess() )
            return std::nullopt;
        const auto guid = Common::Content::AssetGuidFromText( header.GetValue().Guid );
        if ( !guid.IsSuccess() )
            return std::nullopt;
        return guid.GetValue();
    }
} // namespace Desert::WorldGen
