// THE WORLD-SCALE SCENE IS A MEASURING STICK, AND THIS IS WHAT MAKES IT ONE.
//
// Docs/World/PROGRAMME.md §1 puts a world-scale scene first because nothing the programme is about
// reproduces on a 267 KB level, so no improvement against it is measurable. That argument only holds while
// the scene is the SAME scene every time it is cut: two runs that differ by a byte are two different
// instruments, and every number measured against the first is incomparable with every number measured
// against the second. "Reproducible" is therefore an assertion here, not a wish - which is exactly how the
// brief put it.
//
// It drives Tools/WorldGen's OWN entry point (RunWorldGen), not a re-implementation of it. A generator
// tested through a second copy of its arithmetic can agree with itself and disagree with the tool that
// actually writes the file.
//
// WHY THE FULL-SIZE PRESET IS NOT WHAT THIS SUITE GENERATES. `world` is 12 MB and 50 179 entities; a suite
// that built it four times would add about twenty seconds to every sweep for a property the `smoke` preset
// proves through the same code path with the same arithmetic. The full preset's DIMENSIONS are pinned
// below instead, which is the part that could silently move under a measurement.
//
// AND WHY THE GENERATED FILE IS CHECKED AGAINST THE CORPUS RULES HERE RATHER THAN BY THE CORPUS. The
// scene's home is outside Editor/Resources/Assets/Scenes (see Docs/World/WORLD_SCENE.md for the
// measurement that decided it), so the nineteen suites that walk that tree never see it. That is a
// deliberate trade and it comes with a debt: every rule those suites enforce over a shipped scene is
// restated here, over the generator's output, so the file cannot quietly become one the engine would
// refuse. Sections 3 to 6 are that debt paid.

#include "WorldGenMain.hpp"
#include "WorldBuild.hpp"

#include <Engine/Core/Serialize/ForeignKeys.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <cctype>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Core::kSceneVersion;
using Desert::Core::kUnitVersion;
using Desert::Core::ParseLoadableScene;
using Desert::Core::SceneIsAtCurrentVersion;
using Desert::Core::SceneSerialized;
using Desert::Core::Rules::PlanSceneStitch;
using Desert::Core::Rules::PrefabRecordPolicy;
using Desert::Core::Rules::StitchPlan;
using Desert::Core::Serialize::MergeSceneDocument;

namespace
{
    // The repository root, found by walking up until the assets tree appears. Same shape the scene corpus
    // suites use: the working directory of a test binary is not a thing to assume.
    std::string RepoRoot()
    {
        std::string prefix;
        for ( int depth = 0; depth < 8; ++depth )
        {
            if ( std::filesystem::exists( prefix + "Editor/Resources/Assets/Materials" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string AssetsRoot()
    {
        return RepoRoot() + "Editor/Resources/Assets";
    }

    std::filesystem::path Scratch()
    {
        const auto dir = std::filesystem::temp_directory_path() / "DesertWorldSceneGenerator";
        std::filesystem::create_directories( dir );
        return dir;
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        // .rdbuf() is const on the stream itself; the buffer it hands back is what is read from.
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Runs the tool exactly as the command line does, and hands back what it wrote.
    int Generate( const std::filesystem::path& out, const std::vector<std::string>& extra, std::string& bytes )
    {
        std::vector<std::string> args{ "--out", out.string(), "--assets", AssetsRoot() };
        args.insert( args.end(), extra.begin(), extra.end() );

        std::ostringstream reported;
        std::ostringstream refused;
        const int          status = Desert::WorldGen::RunWorldGen( args, reported, refused );
        if ( status == 0 )
            bytes = ReadAll( out );
        else
            bytes = refused.str();
        return status;
    }

    int GenerateSmoke( const std::filesystem::path& out, std::string& bytes,
                       const std::vector<std::string>& extra = {} )
    {
        std::vector<std::string> args{ "--preset", "smoke" };
        args.insert( args.end(), extra.begin(), extra.end() );
        return Generate( out, args, bytes );
    }

    // The generated scene as a JSON object, read through the facade (strict): what MergeSceneDocument takes.
    Common::Json::Object Document( const std::string& json )
    {
        auto parsed = Common::Json::Read<Common::Json::Object>( json );
        EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
        return parsed.IsSuccess() ? parsed.GetValue() : Common::Json::Object{};
    }

    // The generated scene through the strict facade read; a refusal fails the test with its path, and the
    // optional keeps the call sites' has_value()/-> shape.
    std::optional<SceneSerialized> ReadScene( const std::string& bytes )
    {
        auto parsed = Common::Json::Read<SceneSerialized>( bytes );
        EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
        if ( !parsed.IsSuccess() )
            return std::nullopt;
        return std::move( parsed.GetValue() );
    }

    // A counting mint, like the corpus suites use: a scene whose records all carry ids must never call it.
    auto CountingMint( size_t& minted )
    {
        return [&minted]() { return Common::UUID( 1'000'000 + minted++ ); };
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE PROPERTY THE INSTRUMENT'S USEFULNESS RESTS ON
// ---------------------------------------------------------------------------------------------------

// 1a. Regenerating over the SAME file is the same bytes. Architect's decision (AF6k): the world's GUID is
// no longer derived from the spec's name - it is kept from the output file's own header, the same rule
// every other text asset follows, minted fresh only when that file does not yet exist. So it is the FILE,
// regenerated, that pins determinism (and what --verify measures against), not the spec alone.
TEST( WorldSceneGenerator, RegeneratingTheSameFileKeepsItsGuid )
{
    const auto path = Scratch() / "regenerate.desce";
    std::filesystem::remove( path );

    std::string first;
    std::string second;
    ASSERT_EQ( GenerateSmoke( path, first ), 0 ) << first;
    ASSERT_EQ( GenerateSmoke( path, second ), 0 ) << second;

    ASSERT_FALSE( first.empty() );
    EXPECT_EQ( first, second ) << "regenerating over the same file must keep its GUID";
}

// 1b. Two NEVER-BEFORE-WRITTEN files of the one spec are two different assets: if the GUID were still
// derived from the spec's name, these would collide on it. Each gets its own freshly minted GUID instead.
TEST( WorldSceneGenerator, TwoNewFilesOfTheSameSpecGetDifferentGuids )
{
    const auto pathA = Scratch() / "fresh_a.desce";
    const auto pathB = Scratch() / "fresh_b.desce";
    std::filesystem::remove( pathA );
    std::filesystem::remove( pathB );

    std::string a;
    std::string b;
    ASSERT_EQ( GenerateSmoke( pathA, a ), 0 ) << a;
    ASSERT_EQ( GenerateSmoke( pathB, b ), 0 ) << b;

    ASSERT_FALSE( a.empty() );
    EXPECT_NE( a, b ) << "two never-before-written files must not collide on a name-derived GUID";
}

// 1b. And the tool says so itself, before it writes. --verify is what a human running the generator gets
// instead of having to take this suite's word for it on their own machine and their own compiler - the
// determinism argument is about libc++ versus the Windows toolchain, and no test on THIS machine can see
// that. A flag that refuses to write is the part that travels.
TEST( WorldSceneGenerator, TheToolsOwnVerifyFlagPassesAndStillWritesTheFile )
{
    const auto  out = Scratch() / "verified.desce";
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( out, bytes, { "--verify" } ), 0 ) << bytes;
    EXPECT_TRUE( std::filesystem::exists( out ) );
    EXPECT_FALSE( bytes.empty() );
}

// 1c. THE NEGATIVE CONTROL. A determinism test that passes because the generator ignores its inputs proves
// nothing at all, and that is a real shape - a seed threaded to nowhere looks exactly like a seed that
// works. A different seed must produce a different world.
//
// The seed decides the WORLD, not its identity: seed1 and seed1b are two never-before-written files, so
// (1b) each mints its own header GUID. The comparison is therefore of the documents with that one field
// blanked, and the two GUIDs are asserted to differ. The files are removed first because a scratch file
// left by an earlier run keeps its GUID (1a) - a machine that still held files from the name-derived-GUID
// era passed this test with both GUIDs equal, while a clean CI runner failed it.
TEST( WorldSceneGenerator, ADifferentSeedIsADifferentWorldAndTheSameSeedIsNot )
{
    const auto pathOne      = Scratch() / "seed1.desce";
    const auto pathTwo      = Scratch() / "seed2.desce";
    const auto pathOneAgain = Scratch() / "seed1b.desce";
    for ( const auto& path : { pathOne, pathTwo, pathOneAgain } )
        std::filesystem::remove( path );

    std::string one;
    std::string two;
    std::string oneAgain;
    ASSERT_EQ( GenerateSmoke( pathOne, one, { "--seed", "1" } ), 0 ) << one;
    ASSERT_EQ( GenerateSmoke( pathTwo, two, { "--seed", "2" } ), 0 ) << two;
    ASSERT_EQ( GenerateSmoke( pathOneAgain, oneAgain, { "--seed", "1" } ), 0 ) << oneAgain;

    // The header is the document's first member, so the first "Guid" is the header's own.
    const std::string guidKey = R"("Header":{"Kind":"Scene","Guid":")";
    const auto        guidOf  = [&guidKey]( const std::string& json ) -> std::string
    {
        const auto at = json.find( guidKey );
        if ( at == std::string::npos )
            return {};
        const auto begin = at + guidKey.size();
        return json.substr( begin, json.find( '"', begin ) - begin );
    };
    const auto withoutGuid = [&guidKey]( std::string json )
    {
        const auto begin = json.find( guidKey ) + guidKey.size();
        json.erase( begin, json.find( '"', begin ) - begin );
        return json;
    };
    ASSERT_FALSE( guidOf( one ).empty() ) << one;
    ASSERT_FALSE( guidOf( two ).empty() ) << two;
    ASSERT_FALSE( guidOf( oneAgain ).empty() ) << oneAgain;
    EXPECT_NE( guidOf( one ), guidOf( oneAgain ) ) << "two new files must not share a GUID (1b)";

    EXPECT_NE( withoutGuid( one ), withoutGuid( two ) );
    EXPECT_EQ( withoutGuid( one ), withoutGuid( oneAgain ) );
}

// 1d. A PLACE IN THE WORLD HOLDS THE SAME BUILDINGS WHATEVER SIZE THE WORLD IS. The generator seeds each
// cell from its own world coordinate rather than from a stream advanced in iteration order, so a bigger
// world CONTAINS the smaller one instead of being a different one - and that is the property a per-cell
// regeneration needs on the day step 8 streams these cells.
//
// IT FAILED ON ITS FIRST RUN and the fix was in the generator, not here: the seed was taken from the grid
// INDEX, and the grid is centred, so the cell at world (0,0) is index 1 in a 2-cell world and index 16 in
// a 32-cell one. Growing the world regenerated all of it.
TEST( WorldSceneGenerator, TheSamePlaceInTheWorldHoldsTheSameBuildings )
{
    std::string small;
    std::string large;
    ASSERT_EQ( GenerateSmoke( Scratch() / "grow_2.desce", small, { "--cells", "2" } ), 0 ) << small;
    ASSERT_EQ( GenerateSmoke( Scratch() / "grow_4.desce", large, { "--cells", "4" } ), 0 ) << large;

    const auto smallScene = ReadScene( small );
    const auto largeScene = ReadScene( large );
    ASSERT_TRUE( smallScene.has_value() );
    ASSERT_TRUE( largeScene.has_value() );

    // Cell (1,1) of a 2-cell world and cell (2,2) of a 4-cell world are the SAME cell: both are the one
    // whose origin is (0,0), because the grid is centred. Their buildings must be identical in everything
    // but the entity id and the tag, which are positional by construction.
    const auto payloads = []( const SceneSerialized& scene, const std::string& tagPrefix )
    {
        std::vector<std::string> found;
        for ( const auto& entity : scene.Entities )
            if ( entity.Tag.has_value() && entity.Tag->rfind( tagPrefix, 0 ) == 0 )
            {
                auto copy = entity;
                copy.id   = std::nullopt;
                copy.Tag  = std::nullopt;
                found.push_back( Common::Json::Write( copy ) );
            }
        return found;
    };

    if ( !smallScene.has_value() || !largeScene.has_value() )
        FAIL() << "a generated world did not parse back";

    const auto fromSmall = payloads( smallScene.value(), "C01_01_" );
    const auto fromLarge = payloads( largeScene.value(), "C02_02_" );
    ASSERT_FALSE( fromSmall.empty() );
    EXPECT_EQ( fromSmall, fromLarge );
}

// ---------------------------------------------------------------------------------------------------
// 2. THE INSTRUMENT'S DIMENSIONS, PINNED
// ---------------------------------------------------------------------------------------------------

// 2a. THE SHIPPED PRESET IS A REGISTER, NOT A COUNT. What makes this scene a world rather than a big level
// is that it is MUCH bigger than the loading radius: UE's own World Partition example is a 256 m cell and a
// 768 m radius, which is ~29 resident cells. 32 x 32 cells of 256 m is 8192 m on a side - ten and a half
// radii - so the resident set is about 2.8 % of the world. Halve the side and that becomes 11 %, and the
// streaming question stops being askable. These numbers are therefore pinned one by one and the derived
// ones derived, rather than a single entity count that could be satisfied by editing a number.
TEST( WorldSceneGenerator, TheShippedPresetIsTheWorldTheProgrammeArguedFor )
{
    const auto  out = Scratch() / "dimensions.desce";
    std::string bytes;
    // Generated at the shipped preset but with one cell's worth of buildings, so the geometry of the GRID
    // is asserted without writing 12 MB inside a unit test.
    ASSERT_EQ( Generate( out, { "--preset", "world", "--per-cell", "1" }, bytes ), 0 ) << bytes;

    const auto scene = ReadScene( bytes );
    ASSERT_TRUE( scene.has_value() );

    // 32 x 32 cells: 1024 grounds + 1024 buildings + Sun, Sky, Camera.
    EXPECT_EQ( scene->Entities.size(), 32u * 32u * 2u + 3u );

    // The extent, read off the ground tiles rather than trusted: the outermost tile centre is half a cell
    // in from the edge, so the world spans 32 cells of 256 m = 8192 m.
    float minX = 0.0f;
    float maxX = 0.0f;
    for ( const auto& entity : scene->Entities )
    {
        // A record with no Tag or no Translation is not skipped quietly - EVERY record this generator
        // writes states both, so one that does not is a defect in the generator and not a row to pass
        // over. (The explicit has_value() guards are also what make the reads below provably safe to
        // clang-tidy's optional dataflow, which does not follow gtest's ASSERT_ macros.)
        if ( !entity.Tag.has_value() || !entity.Translation.has_value() )
        {
            ADD_FAILURE() << "a record states no Tag or no Translation";
            continue;
        }
        if ( entity.Tag.value().find( "_Ground" ) == std::string::npos )
            continue;
        minX = std::min( minX, entity.Translation.value().x );
        maxX = std::max( maxX, entity.Translation.value().x );
    }
    EXPECT_FLOAT_EQ( maxX - minX, 31.0f * 25600.0f ); // 31 gaps between 32 tile centres
    EXPECT_FLOAT_EQ( ( maxX - minX ) + 25600.0f, 819200.0f ) << "8192 m, 1 unit = 1 cm";
}

// 2b. A CELL'S BUILDINGS STAY INSIDE THEIR CELL. This is the relation the whole plan rests on - PROGRAMME
// §6 says "a pak's region is the assets reachable from the entities of its cell" and the map decision is
// that cells are COMPUTED FROM COORDINATES, never authored. An object whose transform sits in one cell
// while its tag names another would make every one of those derivations wrong, quietly.
TEST( WorldSceneGenerator, EveryObjectSITSInTheCellItsNameClaims )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "cells.desce", bytes, { "--cells", "4" } ), 0 ) << bytes;

    const auto scene = ReadScene( bytes );
    ASSERT_TRUE( scene.has_value() );

    constexpr float kCell   = 25600.0f;
    constexpr int   kHalf   = 2; // --cells 4
    int             checked = 0;

    for ( const auto& entity : scene->Entities )
    {
        // "C<xx>_<zz>_..." and nothing else. Matched on SHAPE rather than on the first letter, because
        // the fixture entity "Camera" also begins with a C and turned the first version of this sweep
        // into a std::stoi exception - a test that dies rather than reports is a test with no verdict.
        if ( !entity.Tag.has_value() )
            continue;
        const std::string& tag = *entity.Tag;
        // std::isdigit takes an int and answers an int, and a char is signed here - so the cast in and
        // the comparison out are both deliberate rather than noise.
        const auto digit = []( char c ) { return std::isdigit( static_cast<unsigned char>( c ) ) != 0; };
        if ( tag.size() < 7 || tag[0] != 'C' || !digit( tag[1] ) || !digit( tag[2] ) || tag[3] != '_' ||
             !digit( tag[4] ) || !digit( tag[5] ) || tag[6] != '_' )
            continue;
        if ( !entity.Translation.has_value() )
        {
            ADD_FAILURE() << tag << " states no Translation";
            continue;
        }
        const int cx = std::stoi( tag.substr( 1, 2 ) );
        const int cz = std::stoi( tag.substr( 4, 2 ) );

        const float originX = static_cast<float>( cx - kHalf ) * kCell;
        const float originZ = static_cast<float>( cz - kHalf ) * kCell;

        EXPECT_GE( entity.Translation.value().x, originX ) << tag;
        EXPECT_LE( entity.Translation.value().x, originX + kCell ) << tag;
        EXPECT_GE( entity.Translation.value().z, originZ ) << tag;
        EXPECT_LE( entity.Translation.value().z, originZ + kCell ) << tag;
        ++checked;
    }
    EXPECT_EQ( checked, 4 * 4 * ( 6 + 1 ) ) << "the sweep found a different world than it generated";
}

// 2c. Buildings stand ON the ground rather than in it: a box of height h is centred at h/2 with the ground
// surface at y = 0. A world whose objects are half-buried reads as a broken importer in every frame taken
// of it, and the frames are the programme's evidence.
TEST( WorldSceneGenerator, EveryBuildingsBaseSitsOnTheGroundPlane )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "standing.desce", bytes ), 0 ) << bytes;

    const auto scene = ReadScene( bytes );
    ASSERT_TRUE( scene.has_value() );

    int standing = 0;
    for ( const auto& entity : scene->Entities )
    {
        if ( !entity.Tag.has_value() || entity.Tag->find( "_B" ) == std::string::npos )
            continue;
        if ( !entity.Translation.has_value() || !entity.Scale.has_value() )
        {
            ADD_FAILURE() << *entity.Tag << " states no transform";
            continue;
        }
        // A primitive cube is 100 cm on a side, so the box's height is Scale.y * 100 and its base is
        // Translation.y - height/2.
        const float base = entity.Translation.value().y - entity.Scale.value().y * 100.0f / 2.0f;
        EXPECT_FLOAT_EQ( base, 0.0f ) << *entity.Tag;
        ++standing;
    }
    EXPECT_GT( standing, 0 );
}

// ---------------------------------------------------------------------------------------------------
// 3. THE FILE IS ONE THIS ENGINE LOADS — the SceneVersionGate corpus rules, applied to the generator
// ---------------------------------------------------------------------------------------------------

TEST( WorldSceneGenerator, TheGeneratedSceneIsOneTheLoaderACCEPTS )
{
    const auto  out = Scratch() / "gate.desce";
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( out, bytes ), 0 ) << bytes;

    const auto loadable = ParseLoadableScene( out.string(), bytes );
    ASSERT_TRUE( static_cast<bool>( loadable ) ) << loadable.GetError();
    EXPECT_TRUE( SceneIsAtCurrentVersion( loadable.GetValue() ) );
}

// And it STATES both integers rather than defaulting into them - the second corpus rule, and the one that
// catches a file that is right for the wrong reason.
TEST( WorldSceneGenerator, TheGeneratedSceneStatesBothVersionIntegersExplicitly )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "versions.desce", bytes ), 0 ) << bytes;

    const auto parsed = ReadScene( bytes );
    ASSERT_TRUE( parsed.has_value() );

    // Bound once, then guarded once. Reaching through the Result on every line gives the reader - and
    // the analyser - a fresh expression each time, so neither can tell that the check two lines up was
    // about the same value.
    const SceneSerialized& scene = parsed.value();
    ASSERT_TRUE( scene.Header.has_value() )
         << "an absent version integer reads as version 0, and the loader refuses the file";
    ASSERT_TRUE( scene.Header.has_value() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kSceneVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kUnitSchemaTag ), kUnitVersion );
}

// ---------------------------------------------------------------------------------------------------
// 4. THE FILE IS WHAT THE ENGINE'S SAVER WOULD HAVE WRITTEN — the ForeignKeys corpus rule
// ---------------------------------------------------------------------------------------------------

// A generated scene that gains keys or reorders them the first time a human presses Ctrl+S is a scene
// whose diff is noise, and the whole point of this file is that changes to it are readable. The corpus
// asserts this over every shipped .desce; the generator's output is held to it here.
TEST( WorldSceneGenerator, TheGeneratedSceneIsUnchangedByAWholeDocumentRoundTrip )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "roundtrip.desce", bytes ), 0 ) << bytes;

    const auto document = Document( bytes );
    ASSERT_FALSE( document.size() == 0 );

    const auto nothingIsOurs = []( const std::string& ) { return false; };
    EXPECT_EQ( Common::Json::Write( MergeSceneDocument( document, document, nothingIsOurs ) ),
               Common::Json::Write( document ) );
}

// And the Settings block is the CANONICAL one: exactly the fields this build's SceneSettings declares, no
// more and no fewer. Written this way it cannot carry a viewport debug flag (SceneDebugFields' corpus
// rule) or a key the saver would drop, because the field list is the reflection table itself.
TEST( WorldSceneGenerator, TheSettingsBlockIsTheREFLECTIONTABLEAndNothingElse )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "settings.desce", bytes ), 0 ) << bytes;

    const auto settings = Document( bytes ).get( "Settings" );
    ASSERT_TRUE( settings.has_value() ) << "the generated scene states no Settings block at all";
    const Common::Json::Node block = Common::Json::Root( *settings, Common::Json::Path().Key( "Settings" ) );
    SCOPED_TRACE( "the Settings block must be an object" );
    ASSERT_EQ( block.GetKind(), Common::Json::Kind::Object );

    const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( type, nullptr ) << "the reflection table this suite reads is empty";

    std::set<std::string> declared;
    for ( const auto& field : type->Fields )
        declared.insert( field.Name );

    std::set<std::string> stated;
    block.ForEachMember( [&]( std::string_view key, const Common::Json::Node& ) { stated.emplace( key ); } );
    EXPECT_EQ( stated, declared );
}

// ---------------------------------------------------------------------------------------------------
// 5. THE FILE STITCHES — the SceneStitch corpus rules
// ---------------------------------------------------------------------------------------------------

// Fifty thousand entities is fifty thousand chances to reuse an id. A duplicate would not fail the load:
// the second record's payload would land on the FIRST claimant and one object would silently be missing
// from the world, which on a scene this size nobody would ever find by looking.
TEST( WorldSceneGenerator, EveryRecordBecomesItsOwnEntityWithNothingShadowedMintedOrUnresolved )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "stitch.desce", bytes, { "--cells", "4" } ), 0 ) << bytes;

    const auto scene = ReadScene( bytes );
    ASSERT_TRUE( scene.has_value() );

    size_t           minted = 0;
    const StitchPlan plan =
         PlanSceneStitch( scene->Entities, CountingMint( minted ), PrefabRecordPolicy::InstantiatedLater );

    EXPECT_EQ( plan.Shadowed, 0u );
    EXPECT_EQ( plan.Minted, 0u );
    EXPECT_EQ( minted, 0u );
    EXPECT_EQ( plan.UnresolvedParents, 0u );
    EXPECT_EQ( plan.Created.size(), scene->Entities.size() );
}

// ---------------------------------------------------------------------------------------------------
// 6. EVERY ASSET IT NAMES EXISTS — the MaterialIdentity corpus rules
// ---------------------------------------------------------------------------------------------------

// Both halves, because they fail in opposite directions: a path that resolves to nothing is an object with
// no material, and a GUID that names a different file is an object with the WRONG material - and the
// second is the one a frame does not obviously show. The GUID is read back out of the material file rather
// than compared against a number written here, which is the same reason the generator reads it too.
TEST( WorldSceneGenerator, EveryMaterialTheSceneNamesResolvesAndItsGuidIsThatFilesOwn )
{
    std::string bytes;
    ASSERT_EQ( GenerateSmoke( Scratch() / "materials.desce", bytes ), 0 ) << bytes;

    const auto parsed = Common::Json::Parse( bytes );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto entities = Common::Json::Root( parsed.GetValue() ).Get( "Entities" );
    ASSERT_TRUE( entities.IsSuccess() ) << entities.GetError();

    // Every element of a string array, or the test fails naming the element's path.
    const auto strings = []( const Common::Json::Node& list )
    {
        std::vector<std::string> out;
        list.ForEachElement(
             [&]( std::size_t, const Common::Json::Node& element )
             {
                 const auto text = element.AsString();
                 EXPECT_TRUE( text.IsSuccess() ) << text.GetError();
                 out.push_back( text.IsSuccess() ? text.GetValue() : std::string() );
             } );
        return out;
    };

    int checked = 0;
    entities.GetValue().ForEachElement(
         [&]( std::size_t, const Common::Json::Node& record )
         {
             const auto mesh = record.Find( "StaticMesh" );
             if ( !mesh )
                 return;
             const auto paths = mesh->Get( "MaterialPaths" );
             const auto guids = mesh->Get( "MaterialGuids" );
             ASSERT_TRUE( paths.IsSuccess() ) << "a mesh with no material path: " << paths.GetError();
             ASSERT_TRUE( guids.IsSuccess() ) << "a mesh with no material guid - the editor would add one on "
                                                 "the first save, and the round trip above would stop holding: "
                                              << guids.GetError();
             const auto pathList = strings( paths.GetValue() );
             const auto guidList = strings( guids.GetValue() );
             ASSERT_EQ( pathList.size(), guidList.size() );

             for ( size_t i = 0; i < pathList.size(); ++i )
             {
                 const std::string&          relative = pathList[i];
                 const std::filesystem::path onDisk   = std::filesystem::path( AssetsRoot() ) / relative;
                 ASSERT_TRUE( std::filesystem::exists( onDisk ) ) << relative << " names no file on disk";

                 const auto material = Common::Json::Parse( ReadAll( onDisk ) );
                 ASSERT_TRUE( material.IsSuccess() ) << relative << ": " << material.GetError();
                 const auto header = Common::Json::Root( material.GetValue() ).Get( "Header" );
                 ASSERT_TRUE( header.IsSuccess() ) << relative << " states no Header";
                 const auto guid = header.GetValue().Get( "Guid" );
                 ASSERT_TRUE( guid.IsSuccess() ) << relative << " Header states no Guid";
                 const auto guidText = guid.GetValue().AsString();
                 ASSERT_TRUE( guidText.IsSuccess() ) << relative << ": " << guidText.GetError();

                 // SCNE 27: a slot names the material by its header GUID's TEXT, so the relation is string
                 // equality with the file's own Header.Guid - no number, no register, no lossy accessor.
                 EXPECT_EQ( guidList[i], guidText.GetValue() )
                      << relative << ": the scene's MaterialGuids entry is not this file's header GUID";
                 ++checked;
             }
         } );
    EXPECT_GT( checked, 0 ) << "the sweep found no material reference at all";
}

// ---------------------------------------------------------------------------------------------------
// 7. REFUSALS — a tool that writes something when it was asked for nonsense is worse than one that stops
// ---------------------------------------------------------------------------------------------------

TEST( WorldSceneGenerator, RefusalsAreNamedAndNothingIsWritten )
{
    const auto      out = Scratch() / "never_written.desce";
    std::error_code ignored;
    std::filesystem::remove( out, ignored );

    std::string said;
    EXPECT_NE( Generate( out, { "--preset", "no-such-preset" }, said ), 0 );
    EXPECT_NE( said.find( "no-such-preset" ), std::string::npos ) << said;
    EXPECT_FALSE( std::filesystem::exists( out ) );

    EXPECT_NE( Generate( out, { "--preset", "smoke", "--cells", "0" }, said ), 0 );
    EXPECT_FALSE( std::filesystem::exists( out ) );

    // An assets root with no materials in it: the generator must say so rather than write a world whose
    // every object names a material that is not there.
    const std::vector<std::string> args{ "--out",    out.string(), "--assets", "/nonexistent-assets-root",
                                         "--preset", "smoke" };
    std::ostringstream             reported;
    std::ostringstream             refused;
    EXPECT_NE( Desert::WorldGen::RunWorldGen( args, reported, refused ), 0 );
    EXPECT_NE( refused.str().find( "CB_White" ), std::string::npos ) << refused.str();
    EXPECT_FALSE( std::filesystem::exists( out ) );
}

// ---------------------------------------------------------------------------------------------------
// WORLD PARTITION: --partition writes the block, and the plan it prints is the plan of the file
// ---------------------------------------------------------------------------------------------------

// Without --partition no block: the flag is the only way a generated world becomes partitioned. With it,
// one grid whose cell IS the generator's tile, and the plan of the written file: the three fixtures (sun,
// sky, camera) always-loaded by component, every ground tile - exactly one tile wide and sitting exactly
// on the tile edges - on level 0 (the half-open rule), and every composite accounted for once.
TEST( WorldSceneGenerator, PartitionWritesOneGridOfTheTileSizeAndThePlanKeepsFixturesLoaded )
{
    std::string plain;
    ASSERT_EQ( GenerateSmoke( Scratch() / "unpartitioned.desce", plain ), 0 ) << plain;
    EXPECT_FALSE( ReadScene( plain )->WorldPartition.has_value() );

    const auto               out = Scratch() / "partitioned.desce";
    const std::vector<std::string> args{ "--out",    out.string(), "--assets",   AssetsRoot(),
                                         "--preset", "smoke",      "--partition" };
    std::ostringstream       reported;
    std::ostringstream       refused;
    ASSERT_EQ( Desert::WorldGen::RunWorldGen( args, reported, refused ), 0 ) << refused.str();

    const auto scene = ReadScene( ReadAll( out ) );
    ASSERT_TRUE( scene.has_value() );
    ASSERT_TRUE( scene->WorldPartition.has_value() );
    ASSERT_EQ( scene->WorldPartition->Grids.size(), 1u ); // NOLINT(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( scene->WorldPartition->Grids[0].CellSize, 25600.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)

    namespace Rules          = Desert::Core::Rules;
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    const auto  plan         = Rules::PlanWorldPartition( scene->Entities, *scene->WorldPartition );
    // NOLINTEND(bugprone-unchecked-optional-access)
    const auto  per          = Rules::CellsPerLevel( plan );
    const auto  why          = Rules::AlwaysLoadedByReason( plan );
    std::size_t sunSkyCamera = 0;
    for ( const std::size_t group : plan.AlwaysLoaded )
    {
        const auto& tag = scene->Entities[plan.Composites[group].Anchor].Tag.value_or( "" );
        if ( plan.Composites[group].Reason == Rules::AlwaysLoadedReason::Component &&
             ( tag == "Sun" || tag == "Sky" || tag == "Camera" ) )
            ++sunSkyCamera;
    }
    EXPECT_EQ( sunSkyCamera, 3u ) << "the three fixtures must be always-loaded by their components";
    EXPECT_EQ( why[static_cast<std::size_t>( Rules::AlwaysLoadedReason::Component )], 3u );

    // Every ground tile on level 0, in the cell its tag names.
    std::size_t tiles = 0;
    for ( std::size_t record = 0; record < scene->Entities.size(); ++record )
    {
        const auto& tag = scene->Entities[record].Tag.value_or( "" );
        if ( tag.size() < 7 || tag.substr( tag.size() - 7 ) != "_Ground" )
            continue;
        for ( const auto& composite : plan.Composites )
        {
            if ( composite.Members.front() != record )
                continue;
            EXPECT_EQ( composite.Reason, Rules::AlwaysLoadedReason::None ) << tag;
            EXPECT_EQ( composite.Level, 0 ) << tag << " is one tile wide and must fit one level-0 cell";
            ++tiles;
        }
    }
    EXPECT_EQ( tiles, 4u );
    ASSERT_FALSE( per.empty() );
    EXPECT_GE( per[0], 4u );

    // And the tool printed the same plan, in the same words the loader logs.
    EXPECT_NE(
         // NOLINTBEGIN(bugprone-unchecked-optional-access)
         reported.str().find( "partition    : " + Rules::SummarisePartition( plan, *scene->WorldPartition ) ),
         // NOLINTEND(bugprone-unchecked-optional-access)
         std::string::npos )
         << reported.str();
}

// EVERY BUILDING FITS ITS OWN TILE, AS THE ENGINE READS IT - checked through the partition plan, which
// composes the transform the way the loader does (radians) and takes the primitive cube's corners. The
// defect this pins: yaw was written as 0/90/180/270 into a field read in radians, so three buildings in
// four stood at 116, 233 or 350 degrees and 1015 of the `world` preset's 49152 crossed a tile edge. On an
// 8 x 8 world every composite but the three fixtures must sit on level 0, and no rotation is written.
TEST( WorldSceneGenerator, EveryBuildingFitsItsTileSoNothingIsPromoted )
{
    const auto               out = Scratch() / "fits.desce";
    const std::vector<std::string> args{ "--out",   out.string(), "--assets",   AssetsRoot(),
                                         "--cells", "8",          "--partition" };
    std::ostringstream       reported;
    std::ostringstream       refused;
    ASSERT_EQ( Desert::WorldGen::RunWorldGen( args, reported, refused ), 0 ) << refused.str();

    const auto scene = ReadScene( ReadAll( out ) );
    ASSERT_TRUE( scene.has_value() && scene->WorldPartition.has_value() );
    for ( const auto& entity : scene->Entities )
    {
        if ( entity.Rotation.has_value() )
            EXPECT_EQ( *entity.Rotation, glm::vec3( 0.0f ) ) << entity.Tag.value_or( "" );
    }

    namespace Rules = Desert::Core::Rules;
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    const auto plan = Rules::PlanWorldPartition( scene->Entities, *scene->WorldPartition );
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ( plan.MaxLevel, 0 ) << reported.str();
    EXPECT_EQ( plan.AlwaysLoaded.size(), 3u ) << reported.str();
    EXPECT_EQ( plan.Cells.size(), 64u );
    EXPECT_EQ( plan.PointOnlyRecords, 3u ) << "only the fixtures have no primitive footprint";
}

// A GRID FINER THAN THE TILE: --partition-cell and --loading-range reach the block, and a 256 m ground tile
// under a 128 m grid goes one level up — the WP3 level ladder is exercised by the generated world. Without
// --partition the two flags would shape nothing, and saying nothing about it is refused.
TEST( WorldSceneGenerator, PartitionCellAndLoadingRangeShapeTheGridAndNeedPartition )
{
    const auto               out = Scratch() / "partitioned_fine.desce";
    const std::vector<std::string> args{ "--out",    out.string(),      "--assets",    AssetsRoot(),
                                         "--preset", "smoke",           "--partition", "--partition-cell",
                                         "12800",    "--loading-range", "51200" };
    std::ostringstream       reported;
    std::ostringstream       refused;
    ASSERT_EQ( Desert::WorldGen::RunWorldGen( args, reported, refused ), 0 ) << refused.str();

    const auto scene = ReadScene( ReadAll( out ) );
    ASSERT_TRUE( scene.has_value() );
    ASSERT_TRUE( scene->WorldPartition.has_value() );
    ASSERT_EQ( scene->WorldPartition->Grids.size(), 1u ); // NOLINT(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( scene->WorldPartition->Grids[0].CellSize, 12800.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_FLOAT_EQ( scene->WorldPartition->Grids[0].LoadingRange, 51200.0f );
    // NOLINTEND(bugprone-unchecked-optional-access)

    namespace Rules = Desert::Core::Rules;
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    const auto plan = Rules::PlanWorldPartition( scene->Entities, *scene->WorldPartition );
    // NOLINTEND(bugprone-unchecked-optional-access)
    for ( std::size_t record = 0; record < scene->Entities.size(); ++record )
    {
        const auto& tag = scene->Entities[record].Tag.value_or( "" );
        if ( tag.size() < 7 || tag.substr( tag.size() - 7 ) != "_Ground" )
            continue;
        for ( const auto& composite : plan.Composites )
            if ( composite.Members.front() == record )
                EXPECT_EQ( composite.Level, 1 ) << tag << " is two 128 m cells wide";
    }

    for ( const char* flag : { "--partition-cell", "--loading-range" } )
    {
        const std::vector<std::string> alone{ "--out",    ( Scratch() / "refused.desce" ).string(),
                                              "--assets", AssetsRoot(),
                                              "--preset", "smoke",
                                              flag,       "12800" };
        std::ostringstream       quiet;
        std::ostringstream       why;
        EXPECT_EQ( Desert::WorldGen::RunWorldGen( alone, quiet, why ), 2 ) << flag;
        EXPECT_NE( why.str().find( "--partition" ), std::string::npos ) << why.str();
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
