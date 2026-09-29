// THE TOOL'S FILE LOOP, driven end to end through RunSceneMigrator — the function main() calls with
// argv. The step suite (SceneMeshGuidMigration) proves the v31 -> v32 migration; it does not compile
// the loop that READS the file, WRITES it back and turns failures into the exit code, and that loop
// carried the one defect a pure-function suite can never see: the write opened the scene itself with
// trunc and checked only the OPEN, so any failure after it — full disk, dropped permissions, a killed
// process — left a zero-byte file behind a green "raised ... 0 failed" report. The write is atomic
// now (temp beside the file, verify after close, rename over), and this suite pins the claim that
// matters to an operator pointing the tool at a repository:
//
//   a write that fails costs the RUN its exit code, and costs the FILE nothing.
//
// The failing write is built by blocking the primitive's temp path with a directory — a situation in
// which writing the scene in place would still SUCCEED, so the first test is red against the old
// code (mutation-checked), not merely untested against it.

#include <format>
#include <MigratorMain.hpp>
#include <SceneMigration.hpp>
#include <SettingsCanonical.hpp>

#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <rflcpp/rfl/json.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Json/Carry.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Desert::Migration::RunSceneMigrator;

namespace
{
    fs::path MakeTempDir( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / name;
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    std::string ReadRaw( const fs::path& p )
    {
        std::ifstream      in( p, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // A scene the tool has real work on: v31, the oldest generation it reads, so the v31 -> v32 step runs and
    // the file is rewritten. Written as the header-carrying text the editor saves.
    void WriteSceneAtV31( const fs::path& p )
    {
        std::ofstream( p, std::ios::binary )
             << R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000031",)"
             << R"("Versions":{"SCNE":31,"UNIT":1},"Dependencies":[]},"SceneName":"WritePathFixture","Entities":[]})";
    }

    // One call, all three streams. Returns the exit code; the report and errors come back by
    // reference so the assertions can read them like an operator would.
    int RunTool( const std::vector<std::string>& args, std::string& report, std::string& errors )
    {
        std::ostringstream out;
        std::ostringstream err;
        const int          code = RunSceneMigrator( args, out, err );
        report                  = out.str();
        errors                  = err.str();
        return code;
    }

} // namespace

// THE ERROR PATH. The temp path is blocked, so the atomic write must refuse; the run exits non-zero,
// the failure is NAMED on the error stream, and the scene on disk is byte-identical. Against the old
// in-place write this exact setup succeeds — the scene itself is writable — so the old code exits 0
// with the file rewritten, and every one of the three assertions goes red.
TEST( SceneMigratorWritePath, AFailingWriteExitsNonZeroAndLeavesTheSceneByteIdentical )
{
    const fs::path dir   = MakeTempDir( "desert_migrator_write_fail" );
    const fs::path scene = dir / "old.desce";
    WriteSceneAtV31( scene );
    const std::string before = ReadRaw( scene );

    fs::path temp = scene;
    temp += ".tmp";
    fs::create_directories( temp ); // blocks the primitive's working file

    std::string report;
    std::string errors;
    const int   code = RunTool( { scene.string() }, report, errors );

    EXPECT_EQ( code, 1 ) << report << errors;
    EXPECT_NE( errors.find( "FAIL" ), std::string::npos ) << errors;
    EXPECT_NE( report.find( "1 failed" ), std::string::npos ) << report;
    EXPECT_EQ( ReadRaw( scene ), before ) << "the failed run cost the scene its contents";

    fs::remove_all( dir );
}

// THE SUCCESS PATH, closed by the tool's own --check: the raise is written, the run exits 0, and a
// second run in check mode finds nothing to do — which is the acceptance the tool is run under
// against the whole repository ("0 would change" after a migration proves the write kept what the
// migration produced).
TEST( SceneMigratorWritePath, ARaisedSceneIsWrittenAndASecondCheckFindsNothingToChange )
{
    const fs::path dir   = MakeTempDir( "desert_migrator_write_ok" );
    const fs::path scene = dir / "old.desce";
    WriteSceneAtV31( scene );

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { scene.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( "raised " ), std::string::npos ) << report;
    EXPECT_NE( report.find( "0 failed" ), std::string::npos ) << report;

    EXPECT_EQ( RunTool( { "--check", scene.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( "0 would change" ), std::string::npos ) << report;

    fs::remove_all( dir );
}

// THE LANDSCAPE TILES (LS-15). A v2 tile is "ok" with its heights' CRC; a v1 tile - well formed, the file the
// deleted v1 reader accepted - FAILS by path and version, the run exits 1 and the file is byte-identical.
TEST( SceneMigratorWritePath, AVersionOneLandscapeTileFailsByPathAndNumberAndIsLeftAlone )
{
    namespace LS           = Desert::World::Landscape;
    const fs::path dir     = MakeTempDir( "desert_migrator_tiles" );
    auto           created = LS::LandscapeTileData::Create( 3, 3 );
    ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
    LS::LandscapeTileData tile = std::move( created.GetValue() );
    tile.SetSample( 1, 1, 40000u );
    std::vector<unsigned char> blob = LS::EncodeLandscapeTile( tile );
    const fs::path             v2   = dir / "current.dlht";
    std::ofstream( v2, std::ios::binary )
         .write( reinterpret_cast<const char*>( blob.data() ), static_cast<std::streamsize>( blob.size() ) );

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { "--check", v2.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( std::format( "ok     {} — already at tile v3, heights crc ", v2.string() ) ),
               std::string::npos )
         << report;

    // The v1 form: version 1, no weight-count word, fresh checksum.
    blob[4] = 1u;
    blob.resize( blob.size() - LS::kLandscapeTileTrailerSize - 4u );
    const uint32_t crc = Common::Utils::Crc32c( blob.data(), blob.size() );
    for ( int i = 0; i < 4; ++i )
        blob.push_back( static_cast<unsigned char>( ( crc >> ( 8 * i ) ) & 0xFFu ) );
    const fs::path v1 = dir / "old.dlht";
    std::ofstream( v1, std::ios::binary )
         .write( reinterpret_cast<const char*>( blob.data() ), static_cast<std::streamsize>( blob.size() ) );
    const std::string before = ReadRaw( v1 );

    EXPECT_EQ( RunTool( { v1.string() }, report, errors ), 1 ) << report << errors;
    EXPECT_NE( errors.find( "FAIL   " + v1.string() + " — Landscape tile blob version 1 " ), std::string::npos )
         << errors;
    EXPECT_EQ( ReadRaw( v1 ), before );

    fs::remove_all( dir );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// THE CORPUS IS ALREADY IN THE SAVER'S TEXT (SAVE1). Opening a committed scene and saving it with no edit
// must leave `git status` clean. Every difference such a save produced was stale corpus text, never the
// saver: reflected blocks missing fields added since the file was written (SkyAtmosphere +25,
// DirectionLight +8, PostProcessVolume +4 on Starter), and floats stated wider than a float
// (`80.0000011920929`, which the saver writes `80.0`). The migrator's canonical pass (CanonicaliseScene)
// rewrites both into the saver's bytes; this census holds every committed scene to having been through
// it: read the scene as the loader reads it, canonicalise, write through the scene writer's canonical
// text - and the result must BE the file. A scene that fails is named; the fix is
// `scripts/Dev/migrate.sh --write Editor/Resources/Assets`.
//
// THE GAP, NAMED: the pass covers Settings and the blocks listed in
// Engine/Core/Serialize/ReflectedComponentBlocks.hpp, plus Text (a hand-written block whose writer is
// FromStruct of its mirror struct alone, so the pass reads and writes it through that struct - that is
// where Starter's `80.0000011920929` lived). The other blocks written by hand-written serializers -
// StaticMesh, SkinnedMesh, InstancedStaticMesh, the material slots, Script, Landscape, LandscapeTile,
// Foliage, AnimGraph, CubeGridBlockout, UIRenderTexture, Locomotion, Morph, SocketAttachment,
// Projectile, the flag components - and prefab override records are carried through as the file states
// them, so a stale field in one of those is NOT caught here; only an editor save of the scene shows it.
// A partitioned header (none is committed today) is compared against its joined text, not its bytes.
TEST( SceneMigratorWritePath, EveryCommittedSceneIsAlreadyTheSaversCanonicalText )
{
    fs::path assets;
    for ( fs::path at = fs::current_path(); !at.empty(); at = at.parent_path() )
    {
        if ( fs::is_directory( at / "Editor/Resources/Assets" ) )
        {
            assets = at / "Editor/Resources/Assets";
            break;
        }
        if ( at == at.parent_path() )
            break;
    }
    ASSERT_FALSE( assets.empty() ) << "no Editor/Resources/Assets above " << fs::current_path();

    bool sawOne = false;
    for ( const auto& entry : fs::recursive_directory_iterator( assets ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        sawOne                = true;
        const fs::path& scene = entry.path();

        const std::string bytes    = ReadRaw( scene );
        const auto        document = Common::Json::TextDocument::Parse( bytes );
        ASSERT_TRUE( document ) << scene << ": " << document.GetError();
        const bool isHeader = Desert::Core::ExternalEntities::IsHeader( document.GetValue() );

        const auto text = Desert::Core::ExternalEntities::ReadSceneFileText( scene );
        ASSERT_TRUE( text ) << scene << ": " << text.GetError();
        auto parsed = rfl::json::read<Desert::Migration::SceneSerialized>( text.GetValue() );
        ASSERT_TRUE( parsed ) << scene << ": " << parsed.error().what();

        const auto report = Desert::Migration::CanonicaliseScene( parsed.value() );
        EXPECT_TRUE( report.Refused.empty() ) << scene << ": " << report.Refused;
        EXPECT_EQ( report.BlocksRestated, 0 )
             << scene << ": " << report.BlocksRestated << " reflected block(s) are not in the saver's text ("
             << report.KeysAdded << " key(s) missing, " << report.ValuesRestated
             << " value(s) spelled otherwise) - run scripts/Dev/migrate.sh --write Editor/Resources/Assets";

        const auto rewritten = Common::Json::TextDocument::Parse( rfl::json::write( parsed.value() ) );
        ASSERT_TRUE( rewritten ) << scene << ": " << rewritten.GetError();
        const auto written = Common::Json::WriteCanonical( rewritten.GetValue() );
        ASSERT_TRUE( written ) << scene << ": " << written.GetError();

        std::string expected = bytes;
        if ( isHeader )
        {
            const auto joined = Common::Json::TextDocument::Parse( text.GetValue() );
            ASSERT_TRUE( joined ) << scene << ": " << joined.GetError();
            const auto joinedText = Common::Json::WriteCanonical( joined.GetValue() );
            ASSERT_TRUE( joinedText ) << scene << ": " << joinedText.GetError();
            expected = joinedText.GetValue();
        }
        EXPECT_TRUE( written.GetValue() == expected )
             << scene << ": read, canonicalised and written back, the scene is not its own file";
    }
    EXPECT_TRUE( sawOne ) << "no .desce below " << assets;
}
