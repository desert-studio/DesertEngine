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

#include <Editor/Import/TextureIntentFile.hpp>

#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>

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

    // A BMP OF STRUCTURED NOISE. The flat one above is what BC7 reproduces best; this is what it
    // reproduces worst, and the cross-check needs both — one image where the measurement says yes and
    // one where it says no, so that "the author asked and the measurement refused" is reachable.
    //
    // The pattern is a hash rather than rand(): the test has to reach the SAME verdict on every machine
    // and in every run, and a seeded generator whose implementation differs between libraries would not.
    void WriteNoiseBmp( const fs::path& path, int width, int height )
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
        Put32( file, 14 + 40 );

        Put32( file, 40 );
        Put32( file, (uint32_t)width );
        Put32( file, (uint32_t)height );
        Put16( file, 1 );
        Put16( file, 24 );
        Put32( file, 0 );
        Put32( file, (uint32_t)pixels );
        Put32( file, 2835 );
        Put32( file, 2835 );
        Put32( file, 0 );
        Put32( file, 0 );

        for ( int y = 0; y < height; ++y )
        {
            for ( int x = 0; x < width; ++x )
            {
                uint32_t h = (uint32_t)( x * 374761393u + y * 668265263u );
                h ^= h >> 13;
                h *= 1274126177u;
                h ^= h >> 16;
                file.push_back( (unsigned char)( h & 0xFFu ) );
                file.push_back( (unsigned char)( ( h >> 8 ) & 0xFFu ) );
                file.push_back( (unsigned char)( ( h >> 16 ) & 0xFFu ) );
            }
            for ( int p = 0; p < padding; ++p )
                file.push_back( 0 );
        }

        fs::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary );
        out.write( (const char*)file.data(), (std::streamsize)file.size() );
    }

    // A FLAT (UN-RLE'd) RADIANCE .hdr. Every pixel is four bytes of RGBE with a large exponent, so the
    // file decodes to values far outside [0,1] -- which is the whole point: it is a source the cook must
    // keep at RGBA32F and must NOT offer a block format to.
    void WriteHdr( const fs::path& path, int width, int height )
    {
        std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string( height ) + " +X " +
                             std::to_string( width ) + "\n";

        std::vector<unsigned char> file( header.begin(), header.end() );
        for ( int i = 0; i < width * height; ++i )
        {
            file.push_back( 200 ); // R mantissa
            file.push_back( 150 ); // G
            file.push_back( 100 ); // B
            file.push_back( 135 ); // shared exponent: 2^(135-128) = 128, so R is about 100.0
        }

        fs::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary );
        out.write( (const char*)file.data(), (std::streamsize)file.size() );
    }

    /// Author an intent beside a source, the way a person would. Written through the same path formula
    /// the cook reads it back with, so a test cannot pass by agreeing with itself about where the file
    /// goes.
    void WriteIntent( const fs::path& source, const std::string& intent )
    {
        const fs::path path = Desert::Editor::TextureIntentPath( source );
        std::ofstream  out( path, std::ios::binary );
        out << "{\"Intent\": \"" << intent << "\"}";
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
    //
    // THROUGH THE FORMAT THE CONTAINER DECLARES, which is no longer always RGBA8: the cook encodes an
    // LDR texture to BC7 when it can show the result holds up. So the equality asserted is not "these
    // bytes are the source's pixels" but "these bytes are what THIS CHECKOUT'S copy of the source
    // encodes to" — which is the claim the test is named after, is still an exact comparison, and is
    // STRONGER than a decode-and-compare would be: it would catch an encoder that reconstructed the
    // right pixels out of different blocks, i.e. a container carrying somebody else's cook.
    //
    // (It has to be exact rather than approximate for a second reason: BC7's mode 6 spends ONE p-bit on
    // all four channels of an endpoint, so an opaque block of an even colour is off by one LSB in
    // colour by construction. `BlockCompression`'s suite pins that trade; a tolerance here would only
    // be hiding it.)
    const auto&                base0 = carried.GetValue().Levels[0];
    std::vector<unsigned char> carriedLevel0(
         carried.GetValue().Pixels.begin() + static_cast<std::ptrdiff_t>( base0.ByteOffset ),
         carried.GetValue().Pixels.begin() + static_cast<std::ptrdiff_t>( base0.ByteOffset + base0.ByteSize ) );

    std::vector<unsigned char> expectedLevel0( pixels, pixels + static_cast<size_t>( w ) * h * 4 );
    if ( Desert::Core::Formats::IsBlockCompressed( carried.GetValue().Format ) )
    {
        auto encoded = Desert::Core::Formats::BlockCompressImage(
             static_cast<uint32_t>( w ), static_cast<uint32_t>( h ), Desert::Core::Formats::ImageFormat::RGBA8F,
             carried.GetValue().Format, expectedLevel0.data(), expectedLevel0.size() );
        ASSERT_TRUE( encoded.IsSuccess() ) << encoded.GetError();
        expectedLevel0 = encoded.ExtractValue();
    }
    EXPECT_EQ( carriedLevel0, expectedLevel0 )
         << "the committed container's base level is not this checkout's source image, encoded";
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

// ── THE AUTHORED INTENT, AND THE FOUR THINGS THAT CAN HAPPEN WHEN IT MEETS THE MEASUREMENT ───────
//
// Every one of these is a behaviour of the COOK and not of a helper, so they run the real
// `TextureImporter::Import` over a real source and read the real cooked container back.

namespace
{
    namespace Fmt = Desert::Core::Formats;

    Fmt::ImageFormat CookedFormat( const fs::path& meta )
    {
        return CookedHeader( meta ).Format;
    }
} // namespace

TEST_F( TextureImport, AnUnmarkedTextureBehavesExactlyAsItDidBeforeTheFieldExisted )
{
    // THE MIGRATION, STATED AS A BEHAVIOUR. No `.detex` means no authored intent, which means the
    // measurement decides alone — the cook this repository has been running — and the container records
    // `Unspecified`. If this ever changes, every texture in every project silently re-cooks.
    const fs::path source = TexturesDir() / "T_Plain.bmp";
    WriteBmp( source, 8, 8, 0x80 );

    TextureImporter importer;
    LogCapture      log;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Unspecified );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::BC7_UNORM ) << "a flat image clears both gates";
    EXPECT_NE( log.Text().find( "on a measurement alone" ), std::string::npos )
         << "the cook has to say that nothing cross-checked this choice\n"
         << log.Text();
}

TEST_F( TextureImport, AnAuthoredColourTextureIsStoredAsBC7AndTheContainerRecordsWhy )
{
    const fs::path source = TexturesDir() / "T_Colour.bmp";
    WriteBmp( source, 8, 8, 0x80 );
    WriteIntent( source, "Colour" );

    TextureImporter importer;
    LogCapture      log;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Colour );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::BC7_UNORM );
    EXPECT_NE( log.Text().find( "authored as Colour" ), std::string::npos ) << log.Text();
    EXPECT_EQ( log.Text().find( "DISAGREE" ), std::string::npos )
         << "the two sources agree about this image and nothing should say otherwise\n"
         << log.Text();
}

TEST_F( TextureImport, AnAuthoredRefusalStandsOverAPassingMeasurementAndTheDisagreementIsNamed )
{
    // DIRECTION ONE OF THE DISAGREEMENT, and the one a cook with only a measurement can never report:
    // the image compresses beautifully and the author has said it must not be compressed. The author
    // wins — a packed or non-visual map that measures well is exactly the case T1's noise rule is about
    // — and the cook says both halves out loud so an authoring mistake is findable.
    const fs::path source = TexturesDir() / "T_Packed.bmp";
    WriteBmp( source, 8, 8, 0x80 );
    WriteIntent( source, "Data" );

    TextureImporter importer;
    LogCapture      log;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Data );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::RGBA8F ) << "the authored refusal stands";
    EXPECT_NE( log.Text().find( "DISAGREE" ), std::string::npos ) << log.Text();
    EXPECT_NE( log.Text().find( "would have given" ), std::string::npos )
         << "the refusal has to quote what the measurement said, or it is not a cross-check\n"
         << log.Text();
}

TEST_F( TextureImport, AFailingMeasurementStandsOverAnAuthoredRequestAndTheDisagreementIsNamed )
{
    // DIRECTION TWO. The author says this is colour; the encode does not reproduce it. Compressing
    // anyway is the silent ruin, refusing quietly hides the authoring mistake, so the cook does neither.
    const fs::path source = TexturesDir() / "T_Noise.bmp";
    WriteNoiseBmp( source, 64, 64 );
    WriteIntent( source, "Colour" );

    TextureImporter importer;
    LogCapture      log;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Colour );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::RGBA8F ) << "the measurement stands";
    EXPECT_NE( log.Text().find( "DISAGREE" ), std::string::npos ) << log.Text();
    EXPECT_NE( log.Text().find( "stored uncompressed" ), std::string::npos ) << log.Text();
}

TEST_F( TextureImport, ANormalMapIsStoredAsBC5AndNeverAsBC7 )
{
    // T1'S MOST LOAD-BEARING RULE, asserted where it takes effect. The same image with no `.detex` is
    // stored as BC7 (the test above), so this is the authored field CHANGING THE OUTPUT and not merely
    // being recorded.
    const fs::path source = TexturesDir() / "T_Normal.bmp";
    WriteBmp( source, 8, 8, 0x80 );
    WriteIntent( source, "NormalMap" );

    TextureImporter importer;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::NormalMap );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::BC5_UNORM );
    EXPECT_NE( header.Format, Fmt::ImageFormat::BC7_UNORM ) << "BC7 on a normal map is forbidden by T1";
}

TEST_F( TextureImport, AMaskIsStoredAsBC4AndCostsHalfOfWhatBC7Would )
{
    const fs::path source = TexturesDir() / "T_Mask.bmp";
    WriteBmp( source, 8, 8, 0x80 );
    WriteIntent( source, "Mask" );

    TextureImporter importer;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Mask );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::BC4_UNORM );

    // THE SAVING IS THE POINT OF THE INTENT, so it is asserted rather than described: the same chain in
    // BC7 is twice these bytes. `PayloadBytes` is the decoded total, which is what the GPU holds.
    const fs::path colourSource = TexturesDir() / "T_MaskAsColour.bmp";
    WriteBmp( colourSource, 8, 8, 0x80 );
    WriteIntent( colourSource, "Colour" );
    ASSERT_NE( (uint64_t)importer.Import( colourSource ), 0ull );
    const auto colour = CookedHeader( TextureImporter::CookedMetaPath( colourSource ) );
    ASSERT_EQ( colour.Format, Fmt::ImageFormat::BC7_UNORM );
    EXPECT_EQ( header.PayloadBytes * 2u, colour.PayloadBytes );
}

TEST_F( TextureImport, AnIntentFileThatCannotBeUnderstoodStopsTheCookGuessingPastIt )
{
    // A `.detex` WITH A TYPO IS NOT AN ABSENT ONE. Somebody wrote it, so the cook must not fall back to
    // the measurement and compress what may be a normal map; it stores the pixels, names the file and
    // lists the vocabulary.
    const fs::path source = TexturesDir() / "T_Typo.bmp";
    WriteBmp( source, 8, 8, 0x80 );
    WriteIntent( source, "normalmap" ); // the right word, the wrong case

    TextureImporter importer;
    LogCapture      log;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( source ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Unspecified );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::RGBA8F )
         << "an instruction the cook could not read is not permission to compress";
    EXPECT_NE( log.Text().find( "not one of" ), std::string::npos )
         << "the refusal has to list what the words are\n"
         << log.Text();
    EXPECT_NE( log.Text().find( "NormalMap" ), std::string::npos ) << log.Text();
}

TEST_F( TextureImport, EditingTheIntentRecooksASourceWhoseOwnBytesDidNotMove )
{
    // THE HALF THAT MAKES THE AUTHORED FILE TAKE EFFECT AT ALL. Freshness is decided from the SOURCE
    // IMAGE's bytes, and a `.detex` is not one of them — so without the intent inside the cook
    // signature an artist could mark a texture and the cook would answer "up to date" for ever, which
    // is the same invisible staleness the mtime comparison was removed for.
    const fs::path source = TexturesDir() / "T_Switch.bmp";
    WriteBmp( source, 8, 8, 0x80 );

    TextureImporter importer;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );
    const fs::path meta = TextureImporter::CookedMetaPath( source );
    ASSERT_EQ( CookedFormat( meta ), Fmt::ImageFormat::BC7_UNORM );

    // A SECOND IMPORTER, because the first one caches by path in memory and this test is about what is
    // on the disk — which is what the next session sees.
    WriteIntent( source, "Data" );
    TextureImporter second;
    ASSERT_NE( (uint64_t)second.Import( source ), 0ull );

    const auto header = CookedHeader( meta );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Data );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::RGBA8F ) << "the edited intent did not reach the cook";

    // And back again, which is the direction that would still pass if the signature only ever grew.
    WriteIntent( source, "Colour" );
    TextureImporter third;
    ASSERT_NE( (uint64_t)third.Import( source ), 0ull );
    EXPECT_EQ( CookedFormat( meta ), Fmt::ImageFormat::BC7_UNORM );
}

TEST_F( TextureImport, AnUnauthoredSourceIsStillNotRecookedWhenNothingChanged )
{
    // The other side of the test above: folding the intent into the signature must not make every
    // unmarked texture re-cook on every scan. `Unspecified` is zero in the high half, so the signature
    // of an unmarked texture is the number this field already held.
    const fs::path source = TexturesDir() / "T_Stable.bmp";
    WriteBmp( source, 8, 8, 0x80 );

    TextureImporter importer;
    ASSERT_NE( (uint64_t)importer.Import( source ), 0ull );
    const fs::path    meta  = TextureImporter::CookedMetaPath( source );
    const std::string first = ReadAll( meta );

    TextureImporter second;
    ASSERT_NE( (uint64_t)second.Import( source ), 0ull );
    EXPECT_EQ( ReadAll( meta ), first ) << "an unmarked texture re-cooked for no reason";
}

TEST_F( TextureImport, AnExtendedRangeSourceIsOfferedNoBlockFormatWhateverTheIntentSays )
{
    // THE DEFECT THIS TEST EXISTS FOR WAS WRITTEN AND CAUGHT BEFORE IT SHIPPED, and it is worth naming
    // because it is the shape of mistake the authored field invites. The cook's old guard read
    // `blockFormat == BC7_UNORM`, which meant "LDR" for as long as BC7 was the only thing an LDR source
    // could become. Rewriting the branch around the policy turned that into "whatever block format the
    // cook found", and an `.hdr` source found BC6H -- a second BC6H call site with no rendered frame to
    // weigh it against, graded by a function that walks float buffers as bytes.
    //
    // The guard is on the SOURCE FORMAT now, and this pins it from the outside: whatever an author
    // writes in the `.detex`, an extended-range source comes out of the cook at its own range.
    // UNAUTHORED FIRST, AND THAT IS THE ONE THAT MATTERS. `BlockPolicyForIntent` refuses an
    // extended-range source by name, so an AUTHORED one was never in danger; the hole was the branch
    // that runs when nobody said anything, where the measurement decides alone and would have been
    // grading BC6H blocks against a float chain read as bytes.
    const fs::path unmarked = TexturesDir() / "T_Range.hdr";
    WriteHdr( unmarked, 8, 8 );

    TextureImporter importer;
    std::string     unmarkedLog;
    {
        LogCapture log;
        ASSERT_NE( (uint64_t)importer.Import( unmarked ), 0ull );
        unmarkedLog = log.Text();
    }

    // NO BLOCK FORMAT IS EVEN ATTEMPTED, which is the observable half and the one that separates the
    // guard from luck. Without it the cook encodes the whole chain as BC6H and grades it by walking two
    // FLOAT buffers as bytes -- and the number that comes out happens to fail the gates on this image,
    // so the cooked file is right for a reason nobody chose. The wasted encode and the meaningless
    // decibels in the log are what the guard actually removes, so they are what this asserts.
    EXPECT_EQ( unmarkedLog.find( "dB" ), std::string::npos )
         << "an extended-range source was graded against a block format\n"
         << unmarkedLog;

    const auto header = CookedHeader( TextureImporter::CookedMetaPath( unmarked ) );
    EXPECT_EQ( header.Format, Fmt::ImageFormat::RGBA32F )
         << "an extended-range source must keep its range; a cook that stored it as a block format "
            "clamped or re-encoded the one property the file exists for";
    EXPECT_FALSE( Fmt::IsBlockCompressed( header.Format ) );
    EXPECT_EQ( header.Intent, Fmt::TextureIntent::Unspecified );

    // And with an intent authored, where the policy's own refusal is the thing being checked. The
    // intent is still RECORDED -- it is the block format that is refused, not the field.
    const fs::path marked = TexturesDir() / "T_RangeMarked.hdr";
    WriteHdr( marked, 8, 8 );
    WriteIntent( marked, "Colour" );
    ASSERT_NE( (uint64_t)importer.Import( marked ), 0ull );

    const auto markedHeader = CookedHeader( TextureImporter::CookedMetaPath( marked ) );
    EXPECT_EQ( markedHeader.Format, Fmt::ImageFormat::RGBA32F );
    EXPECT_EQ( markedHeader.Intent, Fmt::TextureIntent::Colour );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
