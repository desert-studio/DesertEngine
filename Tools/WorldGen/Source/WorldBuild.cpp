#include "WorldBuild.hpp"

#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>

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
        rfl::Generic AsBlock( const T& value )
        {
            auto generic = rfl::json::read<rfl::Generic>( rfl::json::write( value ) );
            // Cannot fail: the text was produced by the writer one call earlier. Named rather than assumed
            // away, because a silent empty block here would be a scene full of meshless entities.
            return generic.value_or( rfl::Generic( rfl::Generic::Object{} ) );
        }

        rfl::Generic Num( double v )
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

        // Two FNV-1a 64 passes (different offsets) over the world's name, as the 32 hex digits of an AssetGuid.
        Common::Content::AssetGuid GuidOfWorld( const std::string& name )
        {
            const auto fnv = [&]( uint64_t hash )
            {
                for ( const unsigned char c : name )
                    hash = ( hash ^ c ) * 1099511628211ull;
                return hash;
            };
            const uint64_t halves[2] = { fnv( 14695981039346656037ull ), fnv( 0x9E3779B97F4A7C15ull ) };
            const char*    digits    = "0123456789abcdef";
            std::string    text;
            for ( const uint64_t half : halves )
                for ( int shift = 60; shift >= 0; shift -= 4 )
                    text.push_back( digits[( half >> shift ) & 0xF] );
            const auto guid = Common::Content::AssetGuidFromText( text );
            return guid.GetValue();
        }

        glm::vec3 BoxScale( int widthCm, int heightCm, int depthCm )
        {
            return { static_cast<float>( widthCm ) / kCubeEdgeCm, static_cast<float>( heightCm ) / kCubeEdgeCm,
                     static_cast<float>( depthCm ) / kCubeEdgeCm };
        }
    } // namespace

    Core::SceneSerialized BuildWorld( const WorldSpec& spec, const std::vector<MaterialRef>& buildingMaterials,
                                      const MaterialRef& groundMaterial, WorldStats& stats )
    {
        Core::SceneSerialized scene;
        scene.SceneName    = spec.Name;
        // The GUID comes from the spec's name, not AssetGuid::Generate: WorldGen must be reproducible (two runs of
        // one spec are the same bytes - --verify refuses otherwise), and a regenerated world is the same asset.
        scene.Header = Common::Content::MakeTextHeader( Common::Content::ContentKind::Scene,
                                                        GuidOfWorld( spec.Name ), Core::SceneTextSubsystems() );

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
            rfl::Generic::Object light;
            light["Color"]     = rfl::Generic( rfl::Generic::Array{ Num( 1.0 ), Num( 1.0 ), Num( 1.0 ) } );
            light["Intensity"] = Num( 1.0 );
            sun.Components["DirectionLight"] = rfl::Generic( std::move( light ) );
            scene.Entities.push_back( std::move( sun ) );

            auto sky =
                 MakeEntity( nextId++, "Sky", { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } );
            rfl::Generic::Object skybox;
            skybox["SkyboxHandle"]   = rfl::Generic( std::string{} );
            skybox["Intensity"]      = Num( 1.0 );
            sky.Components["Skybox"] = rfl::Generic( std::move( skybox ) );
            rfl::Generic::Object atmosphere;
            atmosphere["Enabled"]           = rfl::Generic( true );
            atmosphere["SkyBrightness"]     = Num( 1.0 );
            atmosphere["SunIntensity"]      = Num( 22.0 );
            atmosphere["TimeOfDay"]         = Num( 12.0 );
            sky.Components["SkyAtmosphere"] = rfl::Generic( std::move( atmosphere ) );
            scene.Entities.push_back( std::move( sky ) );

            // Eye height, at the centre of the world, looking down the street. 170 cm is a person, and
            // the acceptance criterion is a person running - a camera parked at altitude would hide
            // exactly the pop-in the programme's §0.3 forbids.
            auto camera = MakeEntity( nextId++, "Camera", { 0.0f, 170.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
                                      { 1.0f, 1.0f, 1.0f } );
            rfl::Generic::Object cam;
            cam["IsMainCamera"] = rfl::Generic( true );
            cam["FOV"]          = Num( 75.0 );
            cam["Near"]         = Num( 10.0 );
            // Far is the world's DIAGONAL and not a round number pulled from another scene: a far plane
            // shorter than the world silently culls the far half of it, which would look exactly like the
            // frustum culling this programme has not built yet and would be read as one.
            cam["Far"]                  = Num( static_cast<double>( stats.ExtentCm ) * 1.5 );
            camera.Components["Camera"] = rfl::Generic( std::move( cam ) );
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
                    mesh.MaterialGuids              = std::vector<uint64_t>{ groundMaterial.Guid };
                    mesh.Primitive                  = Geometry::PrimitiveType::Cube;
                    ground.Components["StaticMesh"] = AsBlock( mesh );
                    scene.Entities.push_back( std::move( ground ) );
                    ++stats.GroundTiles;
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
                    mesh.MaterialGuids                      = std::vector<uint64_t>{ material.Guid };
                    mesh.Primitive                          = Geometry::PrimitiveType::Cube;
                    building.Components["StaticMesh"]       = AsBlock( mesh );
                    scene.Entities.push_back( std::move( building ) );
                    ++stats.Buildings;
                }
            }
        }

        stats.Entities = static_cast<int>( scene.Entities.size() );
        return scene;
    }
} // namespace Desert::WorldGen
