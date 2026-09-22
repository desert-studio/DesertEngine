// The texture importer, compiled AS ITSELF.
//
// WHY THIS SUITE EXISTS. Editor/Source/Editor/Import/TextureImporter.cpp was compiled by no test project in
// this repository, which means the branch that decides whether a texture is re-cooked, and the derivation
// that decides what handle every material and scene resolves it by, could both be sabotaged with the whole
// sweep still green. The file reaches nothing but std::filesystem, stb_image and the shared cooked-path
// formula, so nothing stood in the way of compiling it into a test binary except nobody having done it.
//
// WHAT IS ASSERTED, and what each one costs when it breaks:
//
//   1. The handle is DERIVED from the source path (FNV-1a over the canonical path), not drawn at random.
//      A random handle frozen into a .tex is the defect the deleted "back-compat" branch used to preserve:
//      wipe Cooked/ and every material and scene reference to that texture dies. Asserted against the
//      derivation itself, so an importer that invented its own copy of the hash would not agree.
//   2. Re-importing after wiping Cooked/ yields the SAME handle. That is the property in 1 stated as the
//      thing an artist actually does.
//   3. The cooked .tex says what the image is, and the handle written INTO the file is the handle Import
//      returned. Two places obliged to agree, which is the defect class this programme keeps finding.
//   4. The up-to-date branch: a .tex at least as new as its source is left ALONE (bytes unchanged), and a
//      source newer than its .tex is re-cooked. Getting this backwards means either "edits never take" or
//      "every launch re-cooks the whole project".
//   5. CookedMetaPath is the shared formula, including for a texture OUTSIDE the Textures/ directory -
//      the case whose drift is what put the formula in CookPaths in the first place.
//
// A three-pixel BMP written by the test is the input: stb_image reads BMP, and a file the test authors byte
// by byte cannot go stale the way a checked-in fixture can.

#include <Editor/Import/TextureImporter.hpp>
#include <Editor/Import/CookPaths.hpp>

#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <cstring>

#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>

#include <stb_image/stb_image.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using Desert::Editor::TextureImporter;

namespace
{
    void Put16( std::vector<unsigned char>& out, uint16_t v )
    {
        out.push_back( (unsigned char)( v & 0xFF ) );
        out.push_back( (unsigned char)( v >> 8 ) );
    }

    void Put32( std::vector<unsigned char>& out, uint32_t v )
    {
        for ( int i = 0; i < 4; ++i )
            out.push_back( (unsigned char)( ( v >> ( 8 * i ) ) & 0xFF ) );
    }

    // A 24-bit uncompressed BMP of the given size, filled with one colour. Rows are padded to 4 bytes,
    // which is the part a hand-written BMP usually gets wrong.
    void WriteBmp( const fs::path& path, int width, int height, unsigned char red )
    {
        const int rowBytes = width * 3;
        const int padding  = ( 4 - ( rowBytes % 4 ) ) % 4;
        const int pixels   = ( rowBytes + padding ) * height;

        std::vector<unsigned char> file;
        file.push_back( 'B' );
        file.push_back( 'M' );
        Put32( file, (uint32_t)( 14 + 40 + pixels ) );
        Put16( file, 0 );
        Put16( file, 0 );
        Put32( file, 14 + 40 ); // pixel data offset

        Put32( file, 40 ); // DIB header size
        Put32( file, (uint32_t)width );
        Put32( file, (uint32_t)height );
        Put16( file, 1 );  // planes
        Put16( file, 24 ); // bits per pixel
        Put32( file, 0 );  // BI_RGB
        Put32( file, (uint32_t)pixels );
        Put32( file, 2835 );
        Put32( file, 2835 );
        Put32( file, 0 );
        Put32( file, 0 );

        for ( int y = 0; y < height; ++y )
        {
            for ( int x = 0; x < width; ++x )
            {
                file.push_back( 0x20 ); // blue
                file.push_back( 0x40 ); // green
                file.push_back( red );  // red
            }
            for ( int p = 0; p < padding; ++p )
                file.push_back( 0 );
        }

        fs::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary );
        out.write( (const char*)file.data(), (std::streamsize)file.size() );
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // One temporary project per test, with the content and cooked trees pointed at it. SetProjectRoot is
    // the same call the editor makes while parsing --project, so the importer sees exactly the layout it
    // sees in the editor.
    class TextureImport : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // One directory per test, numbered rather than randomised: the tests run in one process, one
            // after another, and TearDown removes it - a random name would only make a failed run's
            // leftovers harder to find.
            m_Root = fs::temp_directory_path() / ( "desert_texture_import_" + std::to_string( ++s_Counter ) );
            fs::remove_all( m_Root );
            fs::create_directories( m_Root / "Resources" / "Assets" / "Textures" );
            Common::Constants::Path::SetProjectRoot( m_Root, "Resources/Assets" );
        }

        void TearDown() override
        {
            std::error_code ec;
            fs::remove_all( m_Root, ec );
        }

        fs::path TexturesDir() const
        {
            return m_Root / "Resources" / "Assets" / "Textures";
        }

        fs::path        m_Root;
        static unsigned s_Counter;
    };

    unsigned TextureImport::s_Counter = 0;

    // Captures everything the logger emits for the duration of one call. The default logger is restored
    // afterwards, because a test that leaves a sink behind silences every test after it. (Same helper as
    // in TextureSlotRoundTrip, for the same DC §1.4 assertions.)
    class LogCapture
    {
    public:
        LogCapture() : m_Previous( spdlog::default_logger() )
        {
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>( m_Stream );
            spdlog::set_default_logger( std::make_shared<spdlog::logger>( "capture", std::move( sink ) ) );
            spdlog::set_level( spdlog::level::trace );
        }

        ~LogCapture()
        {
            spdlog::set_default_logger( m_Previous );
        }

        LogCapture( const LogCapture& )            = delete;
        LogCapture& operator=( const LogCapture& ) = delete;

        std::string Text() const
        {
            return m_Stream.str();
        }

    private:
        std::ostringstream              m_Stream;
        std::shared_ptr<spdlog::logger> m_Previous;
    };
} // namespace

// 1 + 3. The handle is the FNV-1a derivation over the canonical source path, and the .tex written for it
// carries that same handle and the image's real size.
// WHAT THE COOKED FILE SAYS, read through the reader the engine uses. Until B17 these tests looked for
// JSON substrings in the `.tex`; the file is a binary container now, and a test that greps its bytes for
// `"Width":4` would be asserting about a spelling rather than about a value.
namespace
{
    Desert::Assets::Serialization::TextureBinaryHeaderInfo CookedHeader( const fs::path& meta )
    {
        const std::string bytes = ReadAll( meta );
        auto              info  = Desert::Assets::Serialization::DecodeTextureHeader( bytes, meta.string() );
        EXPECT_TRUE( info.IsSuccess() ) << info.GetError();
        return info.IsSuccess() ? info.GetValue() : Desert::Assets::Serialization::TextureBinaryHeaderInfo{};
    }
} // namespace

TEST_F( TextureImport, HandleIsDerivedFromTheSourcePathAndIsWrittenIntoTheCookedFile )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    TextureImporter    importer;
    const Common::UUID handle = importer.Import( source );

    // Against FromCookedPath and no longer against FromKey over the canonical ABSOLUTE string. That
    // absolute form was the last producer of asset identity that keyed on where the project sits, and it
    // is the one that reached the repository: T_Checker.tex carried FNV-1a of a path beginning
    // /Users/<a developer>/. Asserted against the shared derivation, so an importer that grew its own
    // copy would not agree.
    EXPECT_EQ( (uint64_t)handle, (uint64_t)Common::AssetHandle::FromCookedPath( source ) );
    EXPECT_NE( (uint64_t)handle, 0u );

    // And the key really is the source's place inside the project, with no part of the project root in
    // it. Asserted on the string because a failure here says WHAT leaked.
    EXPECT_EQ( Common::AssetHandle::StableKeyForPath( source ), "assets:Textures/T_Test.bmp" );

    const fs::path meta = TextureImporter::CookedMetaPath( source );
    ASSERT_TRUE( fs::exists( meta ) );

    const std::string text   = ReadAll( meta );
    const auto        stored = CookedHeader( meta );
    EXPECT_EQ( (uint64_t)stored.Handle, (uint64_t)handle );
    EXPECT_EQ( stored.Width, 4u );
    EXPECT_EQ( stored.Height, 3u );

    // AND THE PIXELS ARE IN IT. This is the whole change: the file used to be a 133-byte note pointing
    // at a PNG, so a test could only ask about the note. A 4x3 image has a three-level chain
    // (4x3 -> 2x1 -> 1x1) and the container carries every level.
    EXPECT_EQ( stored.LevelCount, 3u );
    EXPECT_EQ( stored.FileSize, text.size() );
    EXPECT_GT( text.size(), 4u * 3u * 4u ) << "the cooked file is smaller than its own base level";

    // SourcePath is the same root-tagged key the handle is hashed from, with no part of this checkout in
    // it. The absolute form is the defect that reached the repository: T_Checker.tex shipped carrying a
    // developer's home directory.
    EXPECT_EQ( stored.SourcePath, "assets:Textures/T_Test.bmp" );
    EXPECT_EQ( text.find( m_Root.generic_string() ), std::string::npos )
         << "the cooked file contains the checkout directory";

    // The `.dds` CookedPath field is gone: it named a file nothing ever wrote or read.
    EXPECT_EQ( text.find( "CookedPath" ), std::string::npos );
}

// 2. Wipe Cooked/ and cook again: the same handle comes back. This is the property that keeps every
// material and every scene resolving after a clean re-cook.
TEST_F( TextureImport, HandleSurvivesWipingTheCookedTree )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 2, 2, 0x11 );

    TextureImporter    first;
    const Common::UUID before = first.Import( source );

    fs::remove_all( Common::Constants::Path::COOKED_PATH );
    ASSERT_FALSE( fs::exists( TextureImporter::CookedMetaPath( source ) ) );

    TextureImporter    second; // a fresh importer, so the in-memory cache cannot be what answers
    const Common::UUID after = second.Import( source );

    EXPECT_EQ( (uint64_t)before, (uint64_t)after );
}

// 2b. THE CROSS-MACHINE PROPERTY, which is what the derivation change bought and what nothing here
// asserted before. Two developers with the same project at unrelated paths cook the same texture and get
// the same id -- so the .tex one of them commits, and every .demat that names the texture by that number,
// still resolve after the other re-cooks. Under the old rule these two differed, which is why the id in
// this repository had a home directory hashed into it.
TEST_F( TextureImport, TheHandleIsTheSameForTheSameProjectInTwoDifferentPlaces )
{
    const fs::path firstSource = TexturesDir() / "T_Test.bmp";
    WriteBmp( firstSource, 2, 2, 0x55 );

    TextureImporter    firstImporter;
    const Common::UUID onOneMachine = firstImporter.Import( firstSource );

    // The same project, checked out somewhere with nothing in common above it.
    const fs::path elsewhere = fs::temp_directory_path() / "desert_texture_import_other_checkout";
    fs::remove_all( elsewhere );
    const fs::path elsewhereTextures = elsewhere / "Resources" / "Assets" / "Textures";
    WriteBmp( elsewhereTextures / "T_Test.bmp", 2, 2, 0x55 );
    Common::Constants::Path::SetProjectRoot( elsewhere, "Resources/Assets" );

    TextureImporter    secondImporter;
    const Common::UUID onAnother = secondImporter.Import( elsewhereTextures / "T_Test.bmp" );

    fs::remove_all( elsewhere );
    Common::Constants::Path::SetProjectRoot( m_Root, "Resources/Assets" ); // TearDown removes m_Root

    EXPECT_EQ( (uint64_t)onOneMachine, (uint64_t)onAnother )
         << "one texture, one project, two checkout locations, two ids. A .tex committed by one developer "
            "then names a different texture than the one the other cooks, and every material slot keyed "
            "on it empties on the first re-cook.";
}

// 4a. A .tex at least as new as its source is up to date: the file is left byte for byte alone. Re-cooking
// it anyway would mean every project launch rewrites every texture it has.
TEST_F( TextureImport, CookedMetadataNewerThanTheSourceIsLeftUntouched )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    TextureImporter    first;
    const Common::UUID handle = first.Import( source );

    const fs::path    meta   = TextureImporter::CookedMetaPath( source );
    const std::string cooked = ReadAll( meta );

    // Mark the metadata as newer than its source, which is the state a just-cooked file is in.
    fs::last_write_time( meta, fs::last_write_time( source ) + std::chrono::seconds( 10 ) );

    TextureImporter    second;
    const Common::UUID again = second.Import( source );

    EXPECT_EQ( (uint64_t)again, (uint64_t)handle );
    EXPECT_EQ( ReadAll( meta ), cooked );
}

// 4b. ...and a source EDITED after its .tex was written is re-cooked, with the new size in the file. The
// opposite mistake to 4a, and the one that reads as "my texture change did nothing".
TEST_F( TextureImport, SourceNewerThanTheCookedMetadataIsRecooked )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    TextureImporter first;
    first.Import( source );

    const fs::path meta = TextureImporter::CookedMetaPath( source );
    ASSERT_EQ( CookedHeader( meta ).Width, 4u );

    // The artist edits the texture: same path, different image, later timestamp.
    WriteBmp( source, 7, 5, 0x0A );
    fs::last_write_time( source, fs::last_write_time( meta ) + std::chrono::seconds( 10 ) );

    TextureImporter second;
    second.Import( source );

    EXPECT_EQ( CookedHeader( meta ).Width, 7u );
    EXPECT_EQ( CookedHeader( meta ).Height, 5u );
}

// 4c. FRESHNESS IS ABOUT BYTES, AND THESE ARE THE TWO CASES A TIMESTAMP GETS WRONG. Both are ordinary,
// and both used to be invisible: mtime is not a property of an image's contents, it is a property of the
// last thing that touched the file.
TEST_F( TextureImport, AChangedSourceIsRecookedEvenWhenItsTimestampDidNotMove )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    TextureImporter first;
    first.Import( source );
    const fs::path meta = TextureImporter::CookedMetaPath( source );
    const auto     when = fs::last_write_time( meta );

    // A different image at the same path, with the cook still stamped newer. Under an mtime comparison
    // this is "up to date" for ever, and the artist's edit never reaches the screen.
    WriteBmp( source, 7, 5, 0x0A );
    fs::last_write_time( source, when - std::chrono::seconds( 10 ) );

    TextureImporter second;
    second.Import( source );

    EXPECT_EQ( CookedHeader( meta ).Width, 7u )
         << "a changed source was declared up to date because its timestamp was older than the cook";
}

TEST_F( TextureImport, AnUnchangedSourceIsNotRecookedWhenOnlyItsTimestampMoved )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    TextureImporter first;
    first.Import( source );
    const fs::path    meta   = TextureImporter::CookedMetaPath( source );
    const std::string cooked = ReadAll( meta );

    // Exactly what `git checkout` does: it stamps every file it writes with "now", in an order nobody
    // controls. The bytes did not change, so neither should the cook — and this is what stops a fresh
    // clone from re-cooking every texture in the project on its first launch.
    fs::last_write_time( source, fs::last_write_time( meta ) + std::chrono::seconds( 10 ) );

    TextureImporter second;
    second.Import( source );

    EXPECT_EQ( ReadAll( meta ), cooked )
         << "an untouched image was re-cooked because a checkout moved its timestamp";
}

// 5. The cooked path is the SHARED formula, for a texture in Textures/ and for one outside it. The second
// case is the one that used to escape Cooked/Textures with "../" and silently never get discovered.
TEST_F( TextureImport, CookedMetaPathIsTheSharedFormulaInsideAndOutsideTheTextureDirectory )
{
    const fs::path inside  = TexturesDir() / "Sub" / "T_Test.bmp";
    const fs::path outside = m_Root / "Resources" / "Assets" / "Collections" / "Pack" / "T_Other.bmp";

    EXPECT_EQ( TextureImporter::CookedMetaPath( inside ),
               Desert::Editor::CookPaths::CookedTexture( inside, ".tex" ) );
    EXPECT_EQ( TextureImporter::CookedMetaPath( outside ),
               Desert::Editor::CookPaths::CookedTexture( outside, ".tex" ) );

    // And it really is under the cooked texture tree in both cases - the relation the formula exists for.
    const std::string cookedRoot = Common::Constants::Path::TEXTURE_PATH_COOKED.string();
    EXPECT_EQ( TextureImporter::CookedMetaPath( inside ).string().rfind( cookedRoot, 0 ), 0u );
    EXPECT_EQ( TextureImporter::CookedMetaPath( outside ).string().rfind( cookedRoot, 0 ), 0u );
    EXPECT_EQ( TextureImporter::CookedMetaPath( inside ).extension().string(), ".tex" );
}

// The importer's own cache answers the second call for the same path, and answers it with the same id.
TEST_F( TextureImport, SecondImportOfTheSamePathReturnsTheSameHandle )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 2, 2, 0x33 );

    TextureImporter    importer;
    const Common::UUID first  = importer.Import( source );
    const Common::UUID second = importer.Import( source );

    EXPECT_EQ( (uint64_t)first, (uint64_t)second );
}

// THE ROUND TRIP THE .tex IN THE REPOSITORY HAS TO SURVIVE: cooked in one checkout, committed, and the
// PIXELS load in another checkout that shares no directory with the first and does not even call its
// assets folder the same thing. Modelled on TextureSlotRoundTrip, which proved the same relation for the
// scene's reference TO the .tex; this is the .tex's own reference to its source image.
TEST_F( TextureImport, ATexCookedInOneCheckoutLoadsItsPixelsInAnother )
{
    // --- the machine that cooks and commits -------------------------------------------------------
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    TextureImporter    importer;
    const Common::UUID cookedAs = importer.Import( source );

    const std::string texBytes = ReadAll( TextureImporter::CookedMetaPath( source ) );

    // --- the machine that checks it out -----------------------------------------------------------
    // Only the COMMITTED bytes travel: the .tex verbatim, and the source image at its place in the
    // project. The assets root is named differently on purpose.
    const fs::path other = fs::temp_directory_path() / "desert_texture_import_checkout_b";
    fs::remove_all( other );
    WriteBmp( other / "Content" / "Textures" / "T_Test.bmp", 4, 3, 0xF0 );
    const fs::path otherTex = other / "Cooked" / "Textures" / "T_Test.tex";
    fs::create_directories( otherTex.parent_path() );
    {
        std::ofstream out( otherTex, std::ios::binary );
        out << texBytes;
    }

    Common::Constants::Path::SetProjectRoot( other, "Content" );

    Desert::Assets::TextureAsset asset( Desert::Assets::AssetPriority::Medium, Common::Filepath( otherTex ) );
    ASSERT_TRUE( asset.Load().IsSuccess() );

    // The resolved path is THIS checkout's copy of the image — asserted as a value, because in this test
    // the writing checkout still exists on the same disk, so "some file opened" would also be true of the
    // old absolute-path behaviour reading the OTHER machine's file.
    const fs::path resolved = fs::path( asset.GetSourcePath() ).lexically_normal();
    EXPECT_EQ( resolved, ( other / "Content" / "Textures" / "T_Test.bmp" ).lexically_normal() );

    // AND THE PIXELS COME OUT OF THE `.tex` ITSELF. This assertion used to decode the SOURCE image with
    // stb_image, which since B17 proves the opposite of what the test is named after: the container was
    // built precisely so that nothing on this path opens the source. The pixels checked here are the
    // ones the GPU upload reads, and they travelled in the committed bytes.
    const auto carried = Desert::Assets::Serialization::DecodeTextureBinary( texBytes, "carried" );
    ASSERT_TRUE( carried.IsSuccess() ) << carried.GetError();
    EXPECT_EQ( carried.GetValue().Width, 4u );
    EXPECT_EQ( carried.GetValue().Height, 3u );
    EXPECT_EQ( carried.GetValue().Levels.size(), 3u ); // 4x3 -> 2x1 -> 1x1

    // Against what the decoder makes of the source on THIS machine, level for level, so a container
    // that carried the right count of the wrong bytes is not mistaken for a working one.
    int      w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load( asset.GetSourcePath().c_str(), &w, &h, &ch, 4 );
    ASSERT_NE( pixels, nullptr ) << "the source path the .tex resolved to does not decode: "
                                 << asset.GetSourcePath();
    ASSERT_EQ( w, 4 );
    ASSERT_EQ( h, 3 );
    const auto& base0 = carried.GetValue().Levels[0];
    EXPECT_EQ( std::memcmp( carried.GetValue().Pixels.data() + base0.ByteOffset, pixels,
                            static_cast<size_t>( w ) * h * 4 ),
               0 )
         << "the committed container's base level is not the source image's pixels";
    stbi_image_free( pixels );

    // One identity across the trip: the handle the loader reads out of the file is the handle the cook
    // returned, because both derive from the same project-relative key.
    EXPECT_EQ( (uint64_t)asset.GetHandle(), (uint64_t)cookedAs );

    fs::remove_all( other );
    Common::Constants::Path::SetProjectRoot( m_Root, "Resources/Assets" ); // TearDown removes m_Root
}

// DC §1.4: a source that does not decode produces NO cooked file, a null handle, and a log line with the
// path and stb's reason. It used to fall through and freeze the uninitialized width/height into a .tex
// that mtime then declared up to date for ever, in silence.
TEST_F( TextureImport, AFileThatDoesNotDecodeCooksNothingAndSaysWhy )
{
    const fs::path source = TexturesDir() / "T_Bad.bmp";
    fs::create_directories( source.parent_path() );
    {
        std::ofstream out( source, std::ios::binary );
        out << "this is not an image";
    }

    TextureImporter importer;
    std::string     text;
    Common::UUID    handle = Common::UUID( 1ull );
    {
        LogCapture log;
        handle = importer.Import( source );
        text   = log.Text();
    }

    EXPECT_EQ( (uint64_t)handle, 0u );
    EXPECT_FALSE( fs::exists( TextureImporter::CookedMetaPath( source ) ) )
         << "a cooked file was written for an image that never decoded";
    EXPECT_NE( text.find( "T_Bad.bmp" ), std::string::npos )
         << "the failure did not name the file that failed.\nlogged: " << text;

    // The failure is not cached: fix the image, import again in the same session, and it cooks.
    WriteBmp( source, 2, 2, 0x77 );
    const Common::UUID fixed = importer.Import( source );
    EXPECT_EQ( (uint64_t)fixed, (uint64_t)Common::AssetHandle::FromCookedPath( source ) );
    EXPECT_TRUE( fs::exists( TextureImporter::CookedMetaPath( source ) ) );
}

// THE RELATION THE mtime BRANCH USED TO SKIP: the handle Import returns must be the handle the cooked
// file STORES, because the runtime takes its identity from the file (TextureAsset::Load), not from this
// return value. A stale .tex newer than its source is exactly what git manufactures — checkout stamps
// both files with "now" — so under the old branch a wrong stored handle was up to date for ever.
TEST_F( TextureImport, AStaleCookedFileNewerThanItsSourceIsRestampedToAgreeWithTheReturnedHandle )
{
    const fs::path source = TexturesDir() / "T_Test.bmp";
    WriteBmp( source, 4, 3, 0xF0 );

    const fs::path meta = Desert::Editor::CookPaths::CookedTexture( source, ".tex" );
    fs::create_directories( meta.parent_path() );
    {
        // The retired JSON manifest, byte for byte the shape that shipped in this repository, carrying a
        // machine-bound SourcePath and a handle nothing derives. It is now ALSO the stale-cook case, so
        // one fixture drives both: a container reader must not parse it and the importer must replace it.
        std::ofstream out( meta, std::ios::binary );
        out << R"({"Handle":12345,"SourcePath":")" << source.generic_string()
            << R"(","Width":4,"Height":3,"Channels":4,"Format":"RGBA8F"})";
    }
    fs::last_write_time( meta, fs::last_write_time( source ) + std::chrono::seconds( 10 ) );

    TextureImporter    importer;
    const Common::UUID returned = importer.Import( source );

    const auto stored = CookedHeader( meta );
    EXPECT_EQ( (uint64_t)stored.Handle, (uint64_t)returned )
         << "Import returned one handle and left another one in the file; every reference minted from the "
            "return value now misses.";
    EXPECT_NE( (uint64_t)stored.Handle, 12345u );

    // The machine-bound SourcePath went with it: one re-cook replaces a pre-portability `.tex` in place.
    EXPECT_EQ( stored.SourcePath, "assets:Textures/T_Test.bmp" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
