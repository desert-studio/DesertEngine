// THE TOOL'S FILE LOOP, driven end to end through RunSceneMigrator — the function main() calls with
// argv. The nine step suites and SceneMigratorEndToEnd prove the migrations; none of them compiles
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

#include <MigratorMain.hpp>
#include <SceneMigration.hpp>

#include <Engine/Assets/MaterialFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Desert::Migration::RunSceneMigrator;
using Desert::Migration::SceneSerialized;

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

    // A scene the tool has real work on: v1, so every later step runs. Serialised through the same
    // rfl::json the tool parses with, so the fixture cannot drift from the format.
    void WriteSceneAtV1( const fs::path& p )
    {
        SceneSerialized scene;
        scene.SceneName    = "WritePathFixture";
        scene.SceneVersion = 1;

        std::ofstream out( p, std::ios::binary );
        out << rfl::json::write( scene );
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

    // The spelling RunSceneMigrator prints for one schema step, composed from the same constants it
    // composes from — asserting a literal "v3->v4" here would be a second statement of the versions.
    std::string StepLabel( int from, int to )
    {
        return "scene v" + std::to_string( from ) + "->v" + std::to_string( to );
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
    WriteSceneAtV1( scene );
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
    WriteSceneAtV1( scene );

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { scene.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( "raised " ), std::string::npos ) << report;
    EXPECT_NE( report.find( "0 failed" ), std::string::npos ) << report;

    EXPECT_EQ( RunTool( { "--check", scene.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( "0 would change" ), std::string::npos ) << report;

    fs::remove_all( dir );
}

// §4.7 — SILENT MIGRATION IS FORBIDDEN, and the three cloud steps were silent until 2026-09-06:
// their Raised flags fed Changed(), the file was rewritten, and the report line jumped from v3
// straight to v6 with nothing in between. A layer whose scalar type became a species, then a path,
// then the first slot of a set travelled the whole way without the operator being told. This test
// runs a v3 scene with exactly that layer through the tool and requires every step of the chain to
// be named with its versions; deleting any of the three print blocks turns it red.
TEST( SceneMigratorWritePath, TheThreeCloudStepsAreNamedInTheReportWithTheirVersions )
{
    const fs::path dir   = MakeTempDir( "desert_migrator_cloud_report" );
    const fs::path scene = dir / "clouds.desce";

    SceneSerialized fixture;
    fixture.SceneName    = "CloudReportFixture";
    fixture.SceneVersion = Desert::Migration::kSceneVersionCloudNoise; // v3: the cloud chain is all ahead
    fixture.UnitVersion  = Desert::Migration::kUnitVersion;            // keep the units axis out of this

    Desert::Assets::EntityData clouds;
    clouds.Tag = "Sky";
    rfl::Generic::Object payload;
    payload["CloudType"]                 = 0.6; // the scalar the v3->v4->v5->v6 chain carries through
    clouds.Components["VolumetricCloud"] = rfl::Generic( std::move( payload ) );
    fixture.Entities.push_back( std::move( clouds ) );

    {
        std::ofstream out( scene, std::ios::binary );
        out << rfl::json::write( fixture );
    }

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { "--check", scene.string() }, report, errors ), 1 ) << report << errors;

    using namespace Desert::Migration;
    for ( const auto& [from, to] : { std::pair{ kSceneVersionCloudNoise, kSceneVersionCloudSpecies },
                                     std::pair{ kSceneVersionCloudSpecies, kSceneVersionCloudType },
                                     std::pair{ kSceneVersionCloudType, kSceneVersionCloudSet } } )
        EXPECT_NE( report.find( StepLabel( from, to ) ), std::string::npos )
             << "step " << StepLabel( from, to ) << " ran silently; report was:\n"
             << report;

    fs::remove_all( dir );
}

// TWO SCENES, ONE SceneName — the collision the cloud material step cannot see. The file it produces
// is named after the scene, and a .desce copied from another and edited keeps the original's name;
// this project's own verification protocol builds A/B pairs exactly that way, and the verify skill
// records that the editor's log prints the NAME and not the path, so the copy is invisible there too.
// Without the guard the second scene's material silently overwrites the first's and both scenes then
// name a file describing only one of them — a whole sky lost with nothing in any log. The run must
// refuse, name both scenes, and leave the second untouched.
TEST( SceneMigratorWritePath, TwoScenesSharingASceneNameRefuseToShareOneCloudMaterial )
{
    const fs::path dir = MakeTempDir( "desert_migrator_material_collision" );

    // No SetProjectRoot any more: the materials land under the root derived from each SCENE's path, so
    // both of these resolve to `dir` and contend for one file exactly as two scenes in one project would.
    // Pointing the global content root at the temp tree used to be how this suite made the write land
    // somewhere it could see — i.e. the suite was working around the defect below rather than exercising
    // it.

    // One authored look field is all it takes to produce a bespoke material rather than the shared
    // default — the collision is a property of the NAME, not of how much was authored.
    const auto writeCloudScene = []( const fs::path& p )
    {
        SceneSerialized fixture;
        fixture.SceneName    = "TwinName";                               // deliberately the same for both files
        fixture.SceneVersion = Desert::Migration::kSceneVersionSSRUnits; // v11: only the cloud step is ahead
        fixture.UnitVersion  = Desert::Migration::kUnitVersion;

        Desert::Assets::EntityData clouds;
        clouds.Tag = "Sky";
        rfl::Generic::Object payload;
        payload["Coverage"]                  = 0.77;
        clouds.Components["VolumetricCloud"] = rfl::Generic( std::move( payload ) );
        fixture.Entities.push_back( std::move( clouds ) );

        std::ofstream out( p, std::ios::binary );
        out << rfl::json::write( fixture );
    };

    const fs::path first  = dir / "first.desce";
    const fs::path second = dir / "second.desce";
    writeCloudScene( first );
    writeCloudScene( second );
    const std::string secondBefore = ReadRaw( second );

    std::string report;
    std::string errors;
    const int   code = RunTool( { first.string(), second.string() }, report, errors );

    EXPECT_EQ( code, 1 ) << report << errors;
    EXPECT_NE( errors.find( second.string() ), std::string::npos )
         << "the refusal must name the scene that was refused; errors were:\n"
         << errors;
    EXPECT_NE( errors.find( first.string() ), std::string::npos )
         << "the refusal must also name the scene that already claimed the file; errors were:\n"
         << errors;
    EXPECT_EQ( ReadRaw( second ), secondBefore ) << "the refused scene was rewritten anyway";

    fs::remove_all( dir );
}

// THE WORKING DIRECTORY IS NOT AN INPUT — the defect this suite's newest test exists for, and the
// fourth instance in one day of "a path resolved from the wrong root" (scene material paths saved
// absolute; a committed `.tex` naming one machine; a thumbnail key naming a checkout).
//
// The v11 -> v12 raise creates a `.demat` and writes its assets-root-relative name INTO the scene. The
// write used to resolve that name against `Constants::Path::ASSETS_PATH`, which with no project open is
// the RELATIVE `Resources/Assets/` — so it resolved against the current directory. Run from the
// repository root over `Editor/Resources/Assets/Scenes/Autosave/X.desce`, the tool reported "wrote
// Materials/M_X_Clouds.demat" and created a whole new `Resources/Assets/` tree at the repository root,
// while the scene named the file relative to the tree it actually lives in. Two DIFFERING files, one
// name, one relative path, two roots — and which one the engine loads decided by where somebody stood.
//
// So the test runs the tool from a FOREIGN working directory, which is the only arrangement that can
// tell the two roots apart: a run made from the right directory passes either way and proves nothing.
// The assertion is the RELATION rather than a spelling — the path the scene now carries, joined to the
// root the scene's own location implies, is the file that exists — plus the negative the defect
// produced: nothing was created under the working directory at all.
TEST( SceneMigratorWritePath, ACloudMaterialLandsBesideItsSceneAndNotUnderTheWorkingDirectory )
{
    const fs::path dir        = MakeTempDir( "desert_migrator_foreign_cwd" );
    const fs::path assets     = dir / "Project" / "Editor" / "Resources" / "Assets";
    const fs::path scenes     = assets / "Scenes" / "Autosave"; // the one subdirectory the repository has
    const fs::path foreignCwd = dir / "Elsewhere";              // where the tool is run FROM
    fs::create_directories( scenes );
    fs::create_directories( foreignCwd );

    const fs::path scene = scenes / "hero_autosave.desce";
    {
        SceneSerialized fixture;
        fixture.SceneName    = "ForeignCwd";
        fixture.SceneVersion = Desert::Migration::kSceneVersionSSRUnits; // v11: only the cloud step is ahead
        fixture.UnitVersion  = Desert::Migration::kUnitVersion;

        Desert::Assets::EntityData clouds;
        clouds.Tag = "Sky";
        rfl::Generic::Object payload;
        payload["Coverage"]                  = 0.61; // one authored value, so the material is bespoke
        clouds.Components["VolumetricCloud"] = rfl::Generic( std::move( payload ) );
        fixture.Entities.push_back( std::move( clouds ) );

        std::ofstream out( scene, std::ios::binary );
        out << rfl::json::write( fixture );
    }

    const fs::path restore = fs::current_path();
    fs::current_path( foreignCwd );

    std::string report;
    std::string errors;
    const int   code = RunTool( { scene.string() }, report, errors );

    fs::current_path( restore );

    EXPECT_EQ( code, 0 ) << report << errors;

    // The relation: whatever the scene now says, read against the root its own path implies, must be the
    // file on disk. Asserting a literal `Materials/M_ForeignCwd_Clouds.demat` here would be a second
    // statement of the naming rule; this asserts that the scene and the disk agree, which is the thing
    // that was false.
    const auto migrated = rfl::json::read<SceneSerialized>( ReadRaw( scene ) );
    ASSERT_TRUE( migrated ) << "the migrated scene no longer parses";
    ASSERT_FALSE( migrated.value().Entities.empty() );

    const auto payload = migrated.value().Entities.front().Components.get( "VolumetricCloud" );
    ASSERT_TRUE( payload.has_value() );
    const auto fields = payload.value().to_object();
    ASSERT_TRUE( fields.has_value() );
    const auto named = fields.value().get( "Material" );
    ASSERT_TRUE( named.has_value() ) << "the raise did not name a material at all";
    const auto stored = named.value().to_string();
    ASSERT_TRUE( stored.has_value() );

    const fs::path root     = Desert::Migration::SceneOutputRoot( scene );
    const fs::path material = ( root / stored.value() ).lexically_normal();
    EXPECT_TRUE( fs::exists( material ) )
         << "the scene names " << stored.value() << ", which under its own root is " << material
         << " — and there is no file there; report was:\n"
         << report;
    EXPECT_EQ( material.lexically_normal(), ( assets / stored.value() ).lexically_normal() )
         << "the material did not land under the scene's own assets root";

    // The defect's own signature: a second content tree conjured at whatever directory the tool ran in.
    EXPECT_FALSE( fs::exists( foreignCwd / "Resources" ) )
         << "the run created a content tree under its working directory; report was:\n"
         << report;

    fs::remove_all( dir );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// THE CLOUD TYPE PASS (AF7v, format 3 -> 4). A v3 file gains the text header with a fresh GUID, which a
// header-only read (the one ContentScan and the registry make) finds without loading the type; a second
// run leaves the file byte-identical, and a version with no step is refused and left untouched.
TEST( SceneMigratorWritePath, ACloudTypeGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    const fs::path dir  = MakeTempDir( "AF7vCloudTypeMigration" );
    const fs::path file = dir / "Stratus.decloudtype";
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"FormatVersion":3,"DisplayName":"Stratus","Shape":{"BaseAltitudeKm":0.3}})";
    }
    const fs::path stale = dir / "Old.decloudtype";
    {
        std::ofstream out( stale, std::ios::binary );
        out << R"({"FormatVersion":2,"Shape":{}})";
    }
    const std::string staleBytes = ReadRaw( stale );

    std::string report, errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 1 ) << "the v2 file was not a failure";
    EXPECT_NE( errors.find( "Old.decloudtype" ), std::string::npos ) << errors;
    EXPECT_EQ( ReadRaw( stale ), staleBytes ) << "a refused file was rewritten";
    fs::remove( stale );

    const std::string raised = ReadRaw( file );
    EXPECT_EQ( raised.find( "FormatVersion" ), std::string::npos ) << raised;
    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto                                    header = Common::Content::ReadAssetHeader( file, recordOnly );
    ASSERT_TRUE( header ) << header.GetError() << "\n" << raised;
    EXPECT_EQ( header.GetValue().Kind, Common::Content::ContentKind::CloudType );
    EXPECT_FALSE( header.GetValue().Guid.IsNull() );
    ASSERT_EQ( header.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Subsystems[0].Version, 4u );
    EXPECT_NE( raised.find( "Stratus" ), std::string::npos ) << raised;

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a v4 file (a second GUID?)";
    EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;
    fs::remove_all( dir );
}

// THE STRING TABLE AND THEME PASSES (T7b, 1 -> 2): the same step as the cloud type's, one table row each.
// A v1 file - with FormatVersion stated, and with it left out ("absent means 1") - gains the header with a
// fresh GUID and the kind's tag at 2; a second run changes nothing; a version with no step is refused and
// left untouched.
namespace
{
    void ExpectTextKindRaisedOnce( const char* extension, Common::Content::ContentKind kind, const char* body )
    {
        const fs::path    dir    = MakeTempDir( ( std::string( "T7bRaise" ) + ( extension + 1 ) ).c_str() );
        const fs::path    stated = dir / ( std::string( "Stated" ) + extension );
        const fs::path    absent = dir / ( std::string( "Absent" ) + extension );
        const fs::path    stale  = dir / ( std::string( "Future" ) + extension );
        const std::string bodyText( body );
        {
            std::ofstream out( stated, std::ios::binary );
            out << R"({"FormatVersion":1,"DisplayName":"Stated",)" << bodyText << "}";
        }
        {
            std::ofstream out( absent, std::ios::binary );
            out << R"({"DisplayName":"Absent",)" << bodyText << "}";
        }
        {
            std::ofstream out( stale, std::ios::binary );
            out << R"({"FormatVersion":7,)" << bodyText << "}";
        }
        const std::string staleBytes = ReadRaw( stale );

        std::string report;
        std::string errors;
        EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 1 ) << "the v7 file was not a failure";
        EXPECT_NE( errors.find( stale.filename().string() ), std::string::npos ) << errors;
        EXPECT_EQ( ReadRaw( stale ), staleBytes ) << "a refused file was rewritten";
        fs::remove( stale );

        const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
        std::vector<std::string>                      guids;
        std::vector<std::string>                      raisedTexts;
        for ( const fs::path& file : { stated, absent } )
        {
            const std::string raised = ReadRaw( file );
            raisedTexts.push_back( raised );
            EXPECT_EQ( raised.find( "FormatVersion" ), std::string::npos ) << raised;
            EXPECT_NE( raised.find( file.stem().string() ), std::string::npos )
                 << "the payload was lost (a uint64 above INT64_MAX?): " << raised;
            const auto header = Common::Content::ReadAssetHeader( file, recordOnly );
            ASSERT_TRUE( header ) << header.GetError() << "\n" << raised;
            EXPECT_EQ( header.GetValue().Kind, kind );
            EXPECT_FALSE( header.GetValue().Guid.IsNull() );
            ASSERT_EQ( header.GetValue().Subsystems.size(), 1u );
            EXPECT_EQ( header.GetValue().Subsystems[0].Version, 2u );
            guids.push_back( Common::Content::AssetGuidToText( header.GetValue().Guid ) );
        }
        EXPECT_NE( guids[0], guids[1] ) << "two files were minted one GUID";

        EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
        EXPECT_EQ( ReadRaw( stated ), raisedTexts[0] ) << "a second run changed a v2 file (a second GUID?)";
        EXPECT_EQ( ReadRaw( absent ), raisedTexts[1] ) << "a second run changed a v2 file (a second GUID?)";
        EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;
        fs::remove_all( dir );
    }
} // namespace

TEST( SceneMigratorWritePath, AStringTableGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    ExpectTextKindRaisedOnce( ".destrings", Common::Content::ContentKind::StringTable,
                              R"("Entries":[{"Key":"menu.play","Forms":{"en":{"other":"PLAY"}}}])" );
}

TEST( SceneMigratorWritePath, AThemeGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    ExpectTextKindRaisedOnce( ".detheme", Common::Content::ContentKind::UITheme,
                              R"("Colors":[{"Name":"Text","Value":[1,1,1,1]}])" );
}

// THE CLOUD LAYOUT PASS (AF7y, T6b2, container 1 -> 2). Written first; RED until the pass exists. A bare
// "DCLY" v1 file is wrapped in the AF1 binary envelope with a fresh GUID, which the header-only read finds;
// the painting's bytes survive unchanged inside it; a second run leaves the file byte-identical.
TEST( SceneMigratorWritePath, ACloudLayoutIsWrappedInTheEnvelopeOnceAndASecondRunChangesNothing )
{
    const fs::path dir  = MakeTempDir( "AF7yCloudLayoutMigration" );
    const fs::path file = dir / "Painted.dclayout";
    std::string    v1   = std::string( "DCLY" ) + std::string( "\x01\0\0\0", 4 );
    v1.resize( 48, '\0' );
    {
        std::ofstream out( file, std::ios::binary );
        out.write( v1.data(), static_cast<std::streamsize>( v1.size() ) );
    }

    std::string report, errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << report << errors;
    const std::string raised = ReadRaw( file );
    ASSERT_NE( raised, v1 ) << "the v1 layout was not raised:\n" << report << errors;
    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto                                    header = Common::Content::ReadAssetHeader( file, recordOnly );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, Common::Content::ContentKind::CloudLayout );
    EXPECT_FALSE( header.GetValue().Guid.IsNull() );

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a v2 layout (a second GUID?)";
    EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;
    fs::remove_all( dir );
}

// MATL 2 -> 3 (T6c3). A v2 `.demat` names its cloud type and layout by the PATH-DERIVED number
// (AssetHandle::FromKey of "assets:<relative>"); the raise finds that number among the files under the
// content root and states the GUID from the file's own header, with the path as a locator. Fixtures made by
// the tool itself: the cloud type and layout passes give the two assets their header GUIDs first.
namespace
{
    struct CloudRoot
    {
        fs::path                   Dir;
        Common::Content::AssetGuid TypeGuid;
        Common::Content::AssetGuid LayoutGuid;
    };

    CloudRoot MakeCloudRoot( const char* name )
    {
        CloudRoot root{ MakeTempDir( name ), {}, {} };
        fs::create_directories( root.Dir / "Clouds" );
        fs::create_directories( root.Dir / "Materials" );
        {
            std::ofstream out( root.Dir / "Clouds" / "Stratus.decloudtype", std::ios::binary );
            out << R"({"FormatVersion":3,"DisplayName":"Stratus","Shape":{"BaseAltitudeKm":0.3}})";
        }
        {
            std::string v1 = std::string( "DCLY" ) + std::string( "\x01\0\0\0", 4 );
            v1.resize( 48, '\0' );
            std::ofstream out( root.Dir / "Clouds" / "Painted.dclayout", std::ios::binary );
            out.write( v1.data(), static_cast<std::streamsize>( v1.size() ) );
        }
        std::string report, errors;
        EXPECT_EQ( RunTool( { ( root.Dir / "Clouds" ).string() }, report, errors ), 0 ) << report << errors;
        const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
        const auto                                    type =
             Common::Content::ReadAssetHeader( root.Dir / "Clouds" / "Stratus.decloudtype", recordOnly );
        const auto layout =
             Common::Content::ReadAssetHeader( root.Dir / "Clouds" / "Painted.dclayout", recordOnly );
        EXPECT_TRUE( type && layout );
        if ( type )
            root.TypeGuid = type.GetValue().Guid;
        if ( layout )
            root.LayoutGuid = layout.GetValue().Guid;
        return root;
    }

    uint64_t OldNumber( const std::string& relative )
    {
        return static_cast<uint64_t>( Common::AssetHandle::FromKey( "assets:" + relative ) );
    }

    void WriteMaterialV2( const fs::path& file, std::vector<Desert::Migration::MaterialTextureV2> slots )
    {
        Desert::Migration::MaterialDataV2 material;
        material.Header = Common::Content::MakeTextHeader(
             Common::Content::ContentKind::Material, Desert::Migration::MigrationGuidForPath( file.filename() ),
             Desert::Migration::MaterialTextSubsystemsV2() );
        material.ShaderName = "CloudRaymarch";
        material.Params.push_back( { "Coverage", glm::vec4( 0.5f, 0.0f, 0.0f, 0.0f ) } );
        material.Textures = std::move( slots );
        std::ofstream out( file, std::ios::binary );
        out << rfl::json::write( material );
    }

    const Desert::Assets::MaterialAssetRef* CloudSlot( const Desert::Assets::MaterialData& m,
                                                       std::string_view                    name )
    {
        for ( const auto& ref : m.CloudAssets )
            if ( ref.Name == name )
                return &ref;
        return nullptr;
    }
} // namespace

TEST( SceneMigratorWritePath, AMatl2MaterialNamesItsCloudAssetsByHeaderGuidAndASecondRunChangesNothing )
{
    const CloudRoot root = MakeCloudRoot( "T6c3MaterialV3" );
    ASSERT_FALSE( root.TypeGuid.IsNull() );
    ASSERT_FALSE( root.LayoutGuid.IsNull() );
    const fs::path file = root.Dir / "Materials" / "M_Sky.demat";
    WriteMaterialV2( file, { { "CloudType1", OldNumber( "Clouds/Stratus.decloudtype" ) },
                             { "CloudType2", 0 },
                             { "LayoutMask", OldNumber( "Clouds/Painted.dclayout" ) } } );

    std::string report, errors;
    ASSERT_EQ( RunTool( { ( root.Dir / "Materials" ).string() }, report, errors ), 0 ) << report << errors;
    const std::string raised = ReadRaw( file );
    const auto        parsed = Desert::Assets::ParseMaterialJson( file.string(), raised );
    ASSERT_TRUE( parsed ) << parsed.GetError() << "\n" << raised;
    const auto& m = parsed.GetValue();
    EXPECT_TRUE( m.Textures.empty() ) << "a cloud slot landed among the samplers";

    const auto* type = CloudSlot( m, "CloudType1" );
    ASSERT_NE( type, nullptr ) << raised;
    EXPECT_EQ( type->Guid, Common::Content::AssetGuidToText( root.TypeGuid ) );
    EXPECT_EQ( type->Path, "assets:Clouds/Stratus.decloudtype" );
    const auto* mask = CloudSlot( m, "LayoutMask" );
    ASSERT_NE( mask, nullptr ) << raised;
    EXPECT_EQ( mask->Guid, Common::Content::AssetGuidToText( root.LayoutGuid ) );
    const auto* empty = CloudSlot( m, "CloudType2" );
    ASSERT_NE( empty, nullptr ) << "an authored empty slot was dropped instead of stated empty";
    EXPECT_TRUE( empty->Guid.empty() && empty->Path.empty() );
    EXPECT_FLOAT_EQ( m.GetFloat( "Coverage" ), 0.5f );

    EXPECT_EQ( RunTool( { ( root.Dir / "Materials" ).string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a MATL 3 file";
    EXPECT_EQ( RunTool( { "--check", ( root.Dir / "Materials" ).string() }, report, errors ), 0 )
         << report << errors;
    fs::remove_all( root.Dir );
}

TEST( SceneMigratorWritePath, AMatl2NumberNoFileReachesIsRefusedByNameAndTheFileIsUntouched )
{
    const CloudRoot root = MakeCloudRoot( "T6c3MaterialLost" );
    const fs::path  file = root.Dir / "Materials" / "M_Lost.demat";
    WriteMaterialV2( file, { { "CloudType1", 12345 } } );
    const std::string before = ReadRaw( file );

    std::string report, errors;
    EXPECT_EQ( RunTool( { ( root.Dir / "Materials" ).string() }, report, errors ), 1 ) << report << errors;
    EXPECT_NE( errors.find( "M_Lost.demat" ), std::string::npos ) << errors;
    EXPECT_NE( errors.find( "CloudType1" ), std::string::npos ) << errors;
    EXPECT_NE( errors.find( "12345" ), std::string::npos ) << errors;
    EXPECT_EQ( ReadRaw( file ), before ) << "a refused material was rewritten";
    fs::remove_all( root.Dir );
}

TEST( SceneMigratorWritePath, AMatl2NumberNamingAnAssetOfAnotherKindIsRefused )
{
    const CloudRoot root = MakeCloudRoot( "T6c3MaterialKind" );
    const fs::path  file = root.Dir / "Materials" / "M_Swapped.demat";
    // The layout's number in a cloud TYPE slot: the number is known, the slot cannot take it.
    WriteMaterialV2( file, { { "CloudType1", OldNumber( "Clouds/Painted.dclayout" ) } } );
    const std::string before = ReadRaw( file );

    std::string report, errors;
    EXPECT_EQ( RunTool( { ( root.Dir / "Materials" ).string() }, report, errors ), 1 ) << report << errors;
    EXPECT_NE( errors.find( "cloud layout" ), std::string::npos ) << errors;
    EXPECT_EQ( ReadRaw( file ), before );
    fs::remove_all( root.Dir );
}

// THE CONTROL RIG AND RETARGET PASSES (T7c, 1 -> 2): two more rows of the same step.
TEST( SceneMigratorWritePath, AControlRigGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    ExpectTextKindRaisedOnce( ".derig", Common::Content::ContentKind::ControlRig,
                              R"("Name":"R","Controls":[],"Drives":[])" );
}

TEST( SceneMigratorWritePath, ARetargetGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    ExpectTextKindRaisedOnce( ".retarget", Common::Content::ContentKind::Retarget,
                              R"("Name":"X","SourceSkeleton":"ForeignArm.skeleton")" );
}

// THE ANIM GRAPH PASS (T7d, 0 -> 1): generation 0 stated no version member at all, so every headerless file
// is raised; a second run changes nothing.
TEST( SceneMigratorWritePath, AnAnimGraphGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    const fs::path dir  = MakeTempDir( "T7dRaiseAnimGraph" );
    const fs::path file = dir / "Locomotion.danimgraph";
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Name":"Locomotion","Entry":"Idle","Parameters":[],"States":[]})";
    }
    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;

    const std::string raised = ReadRaw( file );
    EXPECT_NE( raised.find( "Locomotion" ), std::string::npos )
         << "the payload was lost (a uint64 above INT64_MAX?): " << raised;
    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto                                    header = Common::Content::ReadAssetHeader( file, recordOnly );
    ASSERT_TRUE( header ) << header.GetError() << "\n" << raised;
    EXPECT_EQ( header.GetValue().Kind, Common::Content::ContentKind::AnimGraph );
    EXPECT_FALSE( header.GetValue().Guid.IsNull() );
    ASSERT_EQ( header.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Subsystems[0].Version, 1u );

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a v1 file (a second GUID?)";
    EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;
    fs::remove_all( dir );
}

// THE SKELETON PASS (T7e, 0 -> 1): generation 0 stated no version member at all, so every headerless file is
// raised; a second run changes nothing.
TEST( SceneMigratorWritePath, ASkeletonGainsAHeaderGuidOnceAndASecondRunChangesNothing )
{
    const fs::path dir  = MakeTempDir( "T7eRaiseSkeleton" );
    const fs::path file = dir / "Rig.skeleton";
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Signature":9748021389765177955,"Bones":[]})";
    }
    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;

    const std::string raised = ReadRaw( file );
    EXPECT_NE( raised.find( "\"Signature\": 9748021389765177955" ), std::string::npos )
         << "the payload was lost (a uint64 above INT64_MAX?): " << raised;
    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto                                    header = Common::Content::ReadAssetHeader( file, recordOnly );
    ASSERT_TRUE( header ) << header.GetError() << "\n" << raised;
    EXPECT_EQ( header.GetValue().Kind, Common::Content::ContentKind::Skeleton );
    EXPECT_FALSE( header.GetValue().Guid.IsNull() );
    ASSERT_EQ( header.GetValue().Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Subsystems[0].Version, 1u );

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a v1 file (a second GUID?)";
    EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;
    fs::remove_all( dir );
}

// A ROW WITH A VERSION MEMBER KEEPS A uint64 ABOVE INT64_MAX (T7e2): the member is cut out of the source text
// and the header spliced in, never a round trip through rfl::Generic, which reads every integer as int64 and
// wrote a clip's SkeletonSignature back negative. The theme stands in for any row that states its version; the
// nested member of the same name proves only the TOP-LEVEL one is cut.
TEST( SceneMigratorWritePath, ARowWithAVersionMemberKeepsAUint64AboveInt64Max )
{
    const fs::path dir  = MakeTempDir( "T7e2RaiseUint64" );
    const fs::path file = dir / "Big.detheme";
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Nested":{"FormatVersion":5},"FormatVersion":1,"Signature":9748021389765177955})";
    }
    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;

    const std::string raised = ReadRaw( file );
    EXPECT_NE( raised.find( "\"Signature\": 9748021389765177955" ), std::string::npos )
         << "the payload was lost (a uint64 above INT64_MAX): " << raised;
    EXPECT_NE( raised.find( "\"FormatVersion\": 5" ), std::string::npos ) << "a nested member was cut: " << raised;
    EXPECT_EQ( raised.find( "\"FormatVersion\": 1" ), std::string::npos ) << raised;
    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto                                    header = Common::Content::ReadAssetHeader( file, recordOnly );
    ASSERT_TRUE( header ) << header.GetError() << "\n" << raised;
    EXPECT_EQ( header.GetValue().Kind, Common::Content::ContentKind::UITheme );

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a raised file";
    fs::remove_all( dir );
}
