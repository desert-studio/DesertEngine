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

#include <MigratorMain.hpp>
#include <SceneMigration.hpp>

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
#include <Common/Content/ShaderAssetHeader.hpp>
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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
