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

#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <rflcpp/rfl/json.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <gtest/gtest.h>

#include <array>
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

    // A full, valid cloud-type shape (CLTY 3, 4 and 5 share it): the migrator reads the body through the
    // engine's typed CloudTypeData, so a fixture with a partial Shape would be refused as unreadable.
    constexpr const char* kCloudTypeShapeV4 =
         R"("Shape":{"BaseAltitudeKm":1.0,"TopAltitudeKm":3.0,"EdgeTopFraction":0.4,"BaseRampFraction":0.1,)"
         R"("Profile":{"HalfWidth":[0.62,0.60120887,0.5827022,0.56448,0.5465422,0.5288889,0.51152,0.49443555,)"
         R"(0.47763556,0.46112,0.4448889,0.42894223,0.41328,0.39790222,0.3828089,0.368]},"AnvilAltitudeKm":0.0,)"
         R"("AnvilThicknessKm":0.0,"AnvilStrength":0.0,"DetailCharacter":0.6,"DetailFactor":1.0,)"
         R"("DensityFactor":1.0,"ExtinctionFactor":1.0,"PlacementScale":1.0,"PlacementAnisotropy":1.0})";
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
        out << R"({"FormatVersion":3,"DisplayName":"Stratus",)" << kCloudTypeShapeV4 << "}";
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
    // Chained in one run: CLTY 3 -> 4 (the header) and 4 -> 5 (a type naming no volume only moves its version).
    EXPECT_EQ( header.GetValue().Subsystems[0].Version, Desert::Assets::kCloudTypeSchemaVersion );
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

// THE CLOUD NOISE VOLUME PASS (T7g, bare container 1 / 2 -> DCNV 3). A bare "DCNV" file is wrapped in the
// AF1 binary envelope with a fresh GUID; the engine decoder reads the voxels and recipe back unchanged; a
// version-1 file (no origin word) comes back Generated; a second run leaves both files byte-identical.
TEST( SceneMigratorWritePath, ACloudNoiseVolumeIsWrappedInTheEnvelopeOnceAndASecondRunChangesNothing )
{
    Desert::Assets::CloudNoiseVolumeData volume; // the default recipe at its default resolution: a legal volume
    volume.Voxels.resize( static_cast<size_t>( volume.VoxelCount() ) * 4u );
    for ( size_t i = 0; i < volume.Voxels.size(); ++i )
        volume.Voxels[i] = static_cast<unsigned char>( ( i * 31u ) & 0xFFu );
    const std::vector<unsigned char> payload = Desert::Assets::EncodeCloudNoisePayload( volume );

    const fs::path    dir    = MakeTempDir( "T7gCloudNoiseMigration" );
    const fs::path    v2File = dir / "BareV2.dcnv";
    const fs::path    v1File = dir / "BareV1.dcnv";
    const std::string v2 =
         std::string( "DCNV" ) + std::string( "\x02\0\0\0", 4 ) + std::string( payload.begin(), payload.end() );
    // Version 1 is version 2 without the origin word at payload offset 52.
    const std::string v1 = std::string( "DCNV" ) + std::string( "\x01\0\0\0", 4 ) +
                           std::string( payload.begin(), payload.begin() + 52 ) +
                           std::string( payload.begin() + 56, payload.end() );
    for ( const auto& [file, bytes] : { std::pair{ v2File, v2 }, std::pair{ v1File, v1 } } )
    {
        std::ofstream out( file, std::ios::binary );
        out.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
    }

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << report << errors;
    std::vector<std::string> raised;
    for ( const auto& file : { v2File, v1File } )
    {
        raised.push_back( ReadRaw( file ) );
        const auto decoded = Desert::Assets::DecodeCloudNoiseVolume(
             std::vector<unsigned char>( raised.back().begin(), raised.back().end() ) );
        ASSERT_TRUE( decoded ) << file << ": " << decoded.GetError() << "\n" << report << errors;
        EXPECT_FALSE( decoded.GetValue().Guid.IsNull() );
        EXPECT_EQ( decoded.GetValue().Origin, Desert::Assets::CloudNoiseVolumeOrigin::Generated );
        EXPECT_EQ( decoded.GetValue().Params.Seed, volume.Params.Seed );
        EXPECT_EQ( decoded.GetValue().Voxels, volume.Voxels );
    }
    const auto raisedV1 = Desert::Assets::DecodeCloudNoiseVolume(
         std::vector<unsigned char>( raised[1].begin(), raised[1].end() ) );
    ASSERT_TRUE( raisedV1 ) << raisedV1.GetError();
    EXPECT_EQ( Desert::Assets::EncodeCloudNoisePayload( raisedV1.GetValue() ), payload )
         << "the version-1 raise must produce exactly the version-2 payload";

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( v2File ), raised[0] ) << "a second run changed a v3 volume (a second GUID?)";
    EXPECT_EQ( ReadRaw( v1File ), raised[1] ) << "a second run changed a v3 volume (a second GUID?)";
    EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;
    fs::remove_all( dir );
}

// THE SCULPTED CLOUD VOLUME PASS (T7g, bare DCMV 2 -> DCMV 3). A bare "DCMV" version-2 file is wrapped in the
// AF1 binary envelope with a fresh GUID; the engine decoder reads the recipe and voxels back unchanged; a
// second run leaves the file byte-identical. A bare version 1 has no reader anywhere, so it is refused by name.
TEST( SceneMigratorWritePath, ASculptedCloudVolumeIsWrappedInTheEnvelopeOnceAndASecondRunChangesNothing )
{
    Desert::Assets::CloudModellingVolumeData volume; // one default lump: the smallest legal recipe
    volume.Recipe.Blobs.resize( 1 );
    volume.Voxels.resize( Desert::Assets::kCloudModellingVoxelBytes );
    for ( size_t i = 0; i < volume.Voxels.size(); ++i )
        volume.Voxels[i] = static_cast<unsigned char>( ( i * 37u ) & 0xFFu );
    const std::vector<unsigned char> payload = Desert::Assets::EncodeCloudModellingPayload( volume );

    const fs::path dir    = MakeTempDir( "T7gCloudModellingMigration" );
    const fs::path v2File = dir / "BareV2.dcmv";
    {
        const std::string v2 = std::string( "DCMV" ) + std::string( "\x02\0\0\0", 4 ) +
                               std::string( payload.begin(), payload.end() );
        std::ofstream out( v2File, std::ios::binary );
        out.write( v2.data(), static_cast<std::streamsize>( v2.size() ) );
    }

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << report << errors;
    const std::string raised = ReadRaw( v2File );
    const auto        decoded =
         Desert::Assets::DecodeCloudModellingVolume( std::vector<unsigned char>( raised.begin(), raised.end() ) );
    ASSERT_TRUE( decoded ) << decoded.GetError() << "\n" << report << errors;
    EXPECT_FALSE( decoded.GetValue().Guid.IsNull() );
    EXPECT_EQ( decoded.GetValue().Recipe.Blobs.size(), 1u );
    EXPECT_EQ( decoded.GetValue().Voxels, volume.Voxels );
    EXPECT_EQ( Desert::Assets::EncodeCloudModellingPayload( decoded.GetValue() ), payload )
         << "the raise must carry the version-2 payload through byte for byte";

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( v2File ), raised ) << "a second run changed a v3 volume (a second GUID?)";
    EXPECT_EQ( RunTool( { "--check", dir.string() }, report, errors ), 0 ) << report << errors;

    const fs::path v1File = dir / "BareV1.dcmv";
    {
        const std::string v1 = std::string( "DCMV" ) + std::string( "\x01\0\0\0", 4 ) +
                               std::string( payload.begin(), payload.end() );
        std::ofstream out( v1File, std::ios::binary );
        out.write( v1.data(), static_cast<std::streamsize>( v1.size() ) );
    }
    EXPECT_NE( RunTool( { v1File.string() }, report, errors ), 0 ) << report;
    EXPECT_NE( errors.find( "a bare 'DCMV' container version 1" ), std::string::npos ) << errors;
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
            out << R"({"FormatVersion":3,"DisplayName":"Stratus",)" << kCloudTypeShapeV4 << "}";
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

// THE RETARGET PASSES (T7c 1 -> 2, T7f 2 -> 3). RTGT 3 names the source rig by the GUID its `.skeleton`
// states, so the step needs the project layout the engine resolves the rig in: `<project>/Resources/Assets/
// Retargets` beside `<project>/Cooked/Meshes`. The two steps chain in one run, so a v1 file lands at 3.
namespace
{
    struct RetargetProject
    {
        fs::path                   Retargets;
        fs::path                   Rig;
        Common::Content::AssetGuid RigGuid;
    };

    RetargetProject MakeRetargetProject( const char* name, bool withRig = true )
    {
        const fs::path  dir = MakeTempDir( name );
        RetargetProject project{ dir / "Resources" / "Assets" / "Retargets",
                                 dir / "Cooked" / "Meshes" / "ForeignArm.skeleton",
                                 Common::Content::AssetGuid::Generate() };
        fs::create_directories( project.Retargets );
        fs::create_directories( project.Rig.parent_path() );
        if ( withRig )
        {
            const std::array<Common::Content::SubsystemVersion, 1> versions = {
                 Common::Content::SubsystemVersion{ Desert::Assets::kSkeletonSchemaTag, 1 } };
            std::ofstream out( project.Rig, std::ios::binary );
            out << "{\"" << Common::Content::kTextHeaderMember << "\":"
                << rfl::json::write( Common::Content::MakeTextHeader( Common::Content::ContentKind::Skeleton,
                                                                      project.RigGuid, versions ) )
                << R"(,"Signature":1,"Bones":[]})";
        }
        return project;
    }

    // Everything a retarget states besides its version and its rig, valid for ParseRetarget.
    constexpr const char* kRetargetBody =
         R"("Name":"X","SourceSkeleton":"ForeignArm.skeleton","SourcePelvisBone":"Hip","TargetPelvisBone":"Hip",)"
         R"("SourceRetargetPose":{"BoneOffsets":[],"PelvisOffset":[0,0,0]},)"
         R"("TargetRetargetPose":{"BoneOffsets":[],"PelvisOffset":[0,0,0]},"Chains":[],"BoneRenames":[])";

    void WriteRetargetV2( const fs::path& file, const Common::Content::AssetGuid& guid )
    {
        const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Desert::Assets::kRetargetSchemaTag, 2 } };
        std::ofstream out( file, std::ios::binary );
        out << "{\"" << Common::Content::kTextHeaderMember << "\":"
            << rfl::json::write(
                    Common::Content::MakeTextHeader( Common::Content::ContentKind::Retarget, guid, versions ) )
            << "," << kRetargetBody << "}";
    }

    // The raised file read by the ENGINE'S loader, the rig by {Guid, Path}, the header's one Dependency the
    // rig; then a second run and a check change nothing. Returns the header GUID the file states.
    std::string ExpectRetargetAtV3( const RetargetProject& project, const fs::path& file )
    {
        const std::string raised = ReadRaw( file );
        const auto        parsed = Desert::Assets::Serialization::ParseRetarget( raised );
        EXPECT_TRUE( parsed.IsSuccess() ) << ( parsed.IsSuccess() ? "" : parsed.GetError() ) << "\n" << raised;
        if ( !parsed.IsSuccess() )
            return {};
        const auto& data = parsed.GetValue();
        if ( !data.Header )
        {
            ADD_FAILURE() << "the raised retarget has no header\n" << raised;
            return {};
        }
        EXPECT_EQ( data.SourceSkeleton.Guid, Common::Content::AssetGuidToText( project.RigGuid ) );
        EXPECT_EQ( data.SourceSkeleton.Path, "ForeignArm.skeleton" );
        EXPECT_EQ( data.Header->Dependencies, std::vector<std::string>{ data.SourceSkeleton.Guid } );
        EXPECT_EQ( Desert::Assets::StatedVersion( data.Header, Desert::Assets::kRetargetSchemaTag ), 3 );

        std::string report;
        std::string errors;
        EXPECT_EQ( RunTool( { project.Retargets.string() }, report, errors ), 0 ) << errors;
        EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed an RTGT 3 file";
        EXPECT_EQ( RunTool( { "--check", project.Retargets.string() }, report, errors ), 0 ) << report << errors;
        return data.Header->Guid;
    }
} // namespace

TEST( SceneMigratorWritePath, ARetargetV1GainsAHeaderGuidAndItsRigsGuidInOneRun )
{
    const RetargetProject project = MakeRetargetProject( "T7fRetargetV1" );
    const fs::path        file    = project.Retargets / "Arm.retarget";
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"FormatVersion":1,)" << kRetargetBody << "}";
    }
    std::string report;
    std::string errors;
    ASSERT_EQ( RunTool( { project.Retargets.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( "RTGT 2 -> 3" ), std::string::npos ) << report;
    const std::string guid = ExpectRetargetAtV3( project, file );
    EXPECT_FALSE( guid.empty() );
    EXPECT_NE( guid, Common::Content::AssetGuidToText( project.RigGuid ) ) << "the retarget took the rig's GUID";
    fs::remove_all( project.Retargets.parent_path().parent_path().parent_path() );
}

TEST( SceneMigratorWritePath, ARetargetV2NamesItsRigByGuidAndKeepsItsOwn )
{
    const RetargetProject            project = MakeRetargetProject( "T7fRetargetV2" );
    const fs::path                   file    = project.Retargets / "Arm.retarget";
    const Common::Content::AssetGuid own     = Common::Content::AssetGuid::Generate();
    WriteRetargetV2( file, own );

    std::string report;
    std::string errors;
    ASSERT_EQ( RunTool( { project.Retargets.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_EQ( ExpectRetargetAtV3( project, file ), Common::Content::AssetGuidToText( own ) )
         << "the raise minted a second identity for the retarget";
    fs::remove_all( project.Retargets.parent_path().parent_path().parent_path() );
}

TEST( SceneMigratorWritePath, ARetargetV2WhoseRigIsMissingIsRefusedByNameAndUntouched )
{
    const RetargetProject project = MakeRetargetProject( "T7fRetargetNoRig", false );
    const fs::path        file    = project.Retargets / "Arm.retarget";
    WriteRetargetV2( file, Common::Content::AssetGuid::Generate() );
    const std::string before = ReadRaw( file );

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { project.Retargets.string() }, report, errors ), 1 ) << report;
    EXPECT_NE( errors.find( "Arm.retarget" ), std::string::npos ) << errors;
    EXPECT_NE( errors.find( "ForeignArm.skeleton" ), std::string::npos ) << errors;
    EXPECT_EQ( ReadRaw( file ), before ) << "a refused retarget was rewritten";
    fs::remove_all( project.Retargets.parent_path().parent_path().parent_path() );
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

// A .anim at generation 3 (T7e, ANIM 3 -> 4): `Version` is cut, the header spliced in, and the clip's
// SkeletonSignature above INT64_MAX keeps its digits - the rig a clip names by it is matched byte for byte.
TEST( SceneMigratorWritePath, AGenerationThreeClipIsRaisedToAHeaderKeepingItsSkeletonSignature )
{
    const fs::path dir  = MakeTempDir( "T7e3RaiseClip" );
    const fs::path file = dir / "Big.anim";
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Version":3,"Name":"Big","TickRate":{"Denominator":1,"Numerator":24000},)"
               R"("DisplayRate":{"Denominator":1,"Numerator":30},"DurationTicks":24000,)"
               R"("SkeletonSignature":9748021389765177955,"Channels":[],"Notifies":[],)"
               R"("Sections":[{"Blend":0,"EndTick":24000,"Name":"Whole clip","StartTick":0,"Tracks":[],"Weight":[]}]})";
    }
    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;

    const std::string raised = ReadRaw( file );
    EXPECT_NE( raised.find( "\"SkeletonSignature\": 9748021389765177955" ), std::string::npos )
         << "the clip's rig signature was lost (a uint64 above INT64_MAX): " << raised;
    EXPECT_EQ( raised.find( "\"Version\"" ), std::string::npos ) << "the old version member survived: " << raised;
    const Common::Content::AssetHeaderReadContext recordOnly{ {}, true };
    const auto                                    header = Common::Content::ReadAssetHeader( file, recordOnly );
    ASSERT_TRUE( header ) << header.GetError() << "\n" << raised;
    EXPECT_EQ( header.GetValue().Kind, Common::Content::ContentKind::Animation );
    EXPECT_NE( raised.find( "\"ANIM\": 4" ), std::string::npos ) << raised;

    EXPECT_EQ( RunTool( { dir.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( file ), raised ) << "a second run changed a raised clip";
    fs::remove_all( dir );
}

// THE CLOUD TYPE PASS (T7h, CLTY 4 -> 5): the noise volume a type names is looked up relative to the assets
// root its Clouds/Types folder lies under, and named by {Guid, Path}, the GUID the `.dcnv` envelope states and
// the header's one Dependency. A type that names none only moves its version.
namespace
{

    void WriteCloudTypeV4( const fs::path& file, const Common::Content::AssetGuid& guid, const char* noise )
    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Header":{"Kind":"CloudType","Guid":")" << Common::Content::AssetGuidToText( guid )
            << R"(","Versions":{"CLTY":4},"Dependencies":[]},)";
        if ( noise != nullptr )
            out << R"("NoiseVolume":")" << noise << R"(",)";
        out << kCloudTypeShapeV4 << "}";
    }
} // namespace

TEST( SceneMigratorWritePath, ACloudTypeV4NamesItsNoiseVolumeByTheVolumesEnvelopeGuid )
{
    const fs::path assets = MakeTempDir( "T7hCloudTypeV4" ) / "Resources" / "Assets";
    const fs::path types  = assets / "Clouds" / "Types";
    fs::create_directories( types );

    Desert::Assets::CloudNoiseVolumeData volume; // the default recipe: a legal volume
    volume.Guid        = Common::Content::AssetGuid::Generate();
    const auto encoded = Desert::Assets::EncodeCloudNoiseVolume( volume );
    if ( !encoded )
    {
        ADD_FAILURE() << encoded.GetError();
        return;
    }
    {
        std::ofstream     out( assets / "Clouds" / "Fine.dcnv", std::ios::binary );
        const std::string bytes( encoded.GetValue().begin(), encoded.GetValue().end() );
        out.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
    }
    const Common::Content::AssetGuid own = Common::Content::AssetGuid::Generate();
    WriteCloudTypeV4( types / "Wisp.decloudtype", own, "Clouds/Fine.dcnv" );
    WriteCloudTypeV4( types / "Plain.decloudtype", Common::Content::AssetGuid::Generate(), nullptr );

    std::string report;
    std::string errors;
    ASSERT_EQ( RunTool( { types.string() }, report, errors ), 0 ) << report << errors;
    EXPECT_NE( report.find( "CLTY 4 -> 5" ), std::string::npos ) << report;

    const auto wisp = Desert::Assets::ParseCloudType( ReadRaw( types / "Wisp.decloudtype" ) );
    if ( !wisp )
    {
        ADD_FAILURE() << wisp.GetError();
        return;
    }
    const Desert::Assets::CloudTypeData& wispType = wisp.GetValue();
    if ( !wispType.Header )
    {
        ADD_FAILURE() << "the migrated type states no header";
        return;
    }
    const std::string volumeGuid = Common::Content::AssetGuidToText( volume.Guid );
    EXPECT_EQ( wispType.Header->Guid, Common::Content::AssetGuidToText( own ) ) << "a second identity";
    EXPECT_EQ( wispType.NoiseVolume, ( Desert::Assets::AssetGuidRef{ volumeGuid, "Clouds/Fine.dcnv" } ) );
    EXPECT_EQ( wispType.Header->Dependencies, std::vector<std::string>{ volumeGuid } );

    const auto plain = Desert::Assets::ParseCloudType( ReadRaw( types / "Plain.decloudtype" ) );
    if ( !plain )
    {
        ADD_FAILURE() << plain.GetError();
        return;
    }
    const Desert::Assets::CloudTypeData& plainType = plain.GetValue();
    if ( !plainType.Header )
    {
        ADD_FAILURE() << "the migrated type states no header";
        return;
    }
    EXPECT_FALSE( plainType.NoiseVolume.has_value() );
    EXPECT_TRUE( plainType.Header->Dependencies.empty() );

    const std::string raised = ReadRaw( types / "Wisp.decloudtype" );
    EXPECT_EQ( RunTool( { types.string() }, report, errors ), 0 ) << errors;
    EXPECT_EQ( ReadRaw( types / "Wisp.decloudtype" ), raised ) << "a second run changed a CLTY 5 file";
    fs::remove_all( assets.parent_path().parent_path() );
}

TEST( SceneMigratorWritePath, ACloudTypeV4WhoseNoiseVolumeIsMissingIsRefusedByNameAndUntouched )
{
    const fs::path types = MakeTempDir( "T7hCloudTypeNoVolume" ) / "Resources" / "Assets" / "Clouds" / "Types";
    fs::create_directories( types );
    WriteCloudTypeV4( types / "Wisp.decloudtype", Common::Content::AssetGuid::Generate(), "Clouds/Gone.dcnv" );
    const std::string before = ReadRaw( types / "Wisp.decloudtype" );

    std::string report;
    std::string errors;
    EXPECT_EQ( RunTool( { types.string() }, report, errors ), 1 ) << report;
    EXPECT_NE( errors.find( "Wisp.decloudtype" ), std::string::npos ) << errors;
    EXPECT_NE( errors.find( "Gone.dcnv" ), std::string::npos ) << errors;
    EXPECT_EQ( ReadRaw( types / "Wisp.decloudtype" ), before ) << "a refused cloud type was rewritten";
    fs::remove_all( types.parent_path().parent_path().parent_path().parent_path() );
}
