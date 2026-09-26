// THE EDITOR'S TWO REMAINING Д31-D SITES, AND WHETHER THEIR REFUSAL ARRIVES.
//
// Both were closed by Д35 and both are here because closing them was not enough: "we check now" without a
// run that actually goes red is a belief. Each test builds a write that genuinely cannot happen and
// asserts that the function answers with a refusal naming the file, rather than with its success value.
//
// WHAT THE TWO SITES WERE:
//
//   * WriteJsonToFile — which stood BYTE FOR BYTE TWICE, in ImportManager.cpp and in
//     TextureImporter.cpp, as two definitions of one external-linkage template. Neither checked anything
//     after `out << json`, and the function was void, so the importer carried on as though the cooked
//     `.stmesh` / `.tex` metadata existed. It is one function now (Editor/Import/CookedJsonWrite.hpp)
//     returning a result — the duplication was the finding, because a hole fixed in one copy comes back
//     through the other.
//   * BlendImporter's convert-script writer, which returned the script's PATH as its success value with
//     nothing checked after the insertion. The script is under a kilobyte, i.e. smaller than one filebuf,
//     so on a failure only the flush could see nothing at all reached the disk — and Blender was then
//     launched on an empty file, reporting the failure as broken Python in a file the user never wrote.
//
// THE INJECTION is the primitive's working file, blocked by a directory of the same name. It is the same
// one the cloud suites use, and it is honest for the same reason: the destination stays writable, so a
// "fix" that merely destroyed the destination would not pass either. That the primitive ALSO refuses a
// failure visible only at the flush is proven where that belongs — Desert/Tests/Common/FileSystemWrite,
// with a negative control showing the banned idiom answering green under the identical failure.

#include <Editor/Import/Blend/BlendConvertScript.hpp>
#include <Editor/Import/CookedJsonWrite.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path Scratch()
    {
        const fs::path dir = fs::temp_directory_path() / "desert_cooked_write_refusal";
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    // A destination that already holds something, and a working file that cannot be created.
    std::string BlockTemp( const fs::path& destination, const std::string& previous )
    {
        {
            std::ofstream existing( destination, std::ios::binary | std::ios::trunc );
            existing << previous;
        }
        fs::path temp = destination;
        temp += ".tmp";
        fs::remove_all( temp );
        fs::create_directories( temp );
        return previous;
    }

    std::string ReadRaw( const fs::path& p )
    {
        std::ifstream in( p, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }

    // Something reflect-cpp can write, standing in for MeshAssetData / TextureAssetData. The unit under
    // test is the write, not any particular payload's schema — those have suites of their own.
    struct CookedThing
    {
        std::string Name;
        int         Count = 0;
    };
} // namespace

TEST( CookedWriteRefusal, CookedJsonThatLandsIsReportedAsSuccessAndIsReadable )
{
    // The positive control. A refusal test whose function refuses everything would pass for the wrong
    // reason, and this project has shipped a gate that matched nothing.
    const fs::path dir  = Scratch();
    const fs::path file = dir / "thing.stmesh";

    const auto written = Desert::Editor::WriteCookedJson( CookedThing{ "mesh", 3 }, file );
    ASSERT_TRUE( written ) << written.GetError();

    const std::string text = ReadRaw( file );
    EXPECT_NE( text.find( "mesh" ), std::string::npos ) << text;
    EXPECT_FALSE( fs::exists( fs::path( file ).concat( ".tmp" ) ) ) << "the working file survived";
}

TEST( CookedWriteRefusal, CookedJsonThatCannotBeWrittenIsARefusalNamingTheFile )
{
    const fs::path    dir      = Scratch();
    const fs::path    file     = dir / "thing.stmesh";
    const std::string previous = BlockTemp( file, R"({"the":"metadata that was already cooked"})" );

    const auto written = Desert::Editor::WriteCookedJson( CookedThing{ "mesh", 3 }, file );
    EXPECT_FALSE( written ) << "cooked metadata that was never written reported success";
    EXPECT_NE( written.GetError().find( "thing.stmesh" ), std::string::npos ) << written.GetError();
    EXPECT_EQ( ReadRaw( file ), previous ) << "a failed cook cost the metadata that was already there";
}

TEST( CookedWriteRefusal, CookedJsonSanitisesTheFileNameItWasGiven )
{
    // Pinned because the sanitisation is the one thing this function does BESIDES writing, and it means
    // the file it writes is not always the file it was asked for. (The freshness check in
    // ImportManager::Import asks about the UNSANITISED name, so a source needing sanitisation re-cooks
    // every scan. That is named in CookedJsonWrite.hpp and is not this function's to fix.)
    const fs::path dir = Scratch();

    ASSERT_TRUE( Desert::Editor::WriteCookedJson( CookedThing{ "q", 1 }, dir / "a?b*c.stmesh" ) );
    EXPECT_TRUE( fs::exists( dir / "a_b_c.stmesh" ) );
    EXPECT_FALSE( fs::exists( dir / "a?b*c.stmesh" ) );
}

TEST( CookedWriteRefusal, TheBlendConvertScriptThatLandsIsReturnedAsItsOwnPath )
{
    const fs::path dir    = Scratch();
    const fs::path script = dir / "BlendConvert" / "_convert.py";

    EXPECT_EQ( Desert::Editor::WriteBlendConvertScript( script ), script );
    // The whole script, not a prefix of it: a truncated export script is the failure mode this row was
    // about, and it is invisible unless the tail is checked.
    const std::string text = ReadRaw( script );
    EXPECT_EQ( text, std::string( Desert::Editor::kBlendConvertScript ) );
    EXPECT_NE( text.find( "export_scene.fbx" ), std::string::npos );
}

TEST( CookedWriteRefusal, ABlendConvertScriptThatCannotBeWrittenReturnsNoPath )
{
    const fs::path dir    = Scratch();
    const fs::path script = dir / "BlendConvert" / "_convert.py";
    fs::create_directories( script.parent_path() );
    const std::string previous = BlockTemp( script, "# the script from the previous run" );

    // An EMPTY path is the caller's "do not launch Blender". Returning the path anyway is what turned a
    // failed write into a Blender error message about somebody else's Python.
    EXPECT_TRUE( Desert::Editor::WriteBlendConvertScript( script ).empty() )
         << "a script that was never written was handed back as a path to run";
    EXPECT_EQ( ReadRaw( script ), previous ) << "the failed write cost the script that was already there";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
