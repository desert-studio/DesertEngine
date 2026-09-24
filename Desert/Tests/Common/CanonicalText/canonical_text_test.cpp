// CanonicalText (AF6): the one writer of text assets lays a document out so that git can diff and merge it,
// and never changes a value while doing so.
#include <Common/Content/CanonicalText.hpp>

#include <gtest/gtest.h>
#include <rflcpp/rfl/thirdparty/yyjson.h>

#include <array>
#include <charconv>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Common::Content::CanonicalJsonText;
using Common::Content::IsCanonicalJsonText;

namespace
{
    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( fs::exists( prefix / "Editor" / "Desert.deproj" ) )
                return fs::absolute( prefix ).lexically_normal();
            prefix /= "..";
        }
        return {};
    }

    std::string ReadAll( const fs::path& file )
    {
        std::ifstream      in( file, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The single-line form rfl::json writes: yyjson's own minified output, the layout the corpus had before.
    std::string Minified( const std::string& json )
    {
        yyjson_doc* doc = yyjson_read( json.data(), json.size(), YYJSON_READ_NOFLAG );
        if ( doc == nullptr )
            return {};
        std::size_t length = 0;
        char*       text   = yyjson_write( doc, YYJSON_WRITE_NOFLAG, &length );
        std::string out( text, length );
        std::free( text );
        yyjson_doc_free( doc );
        return out;
    }

    // Every text asset kind the canonical writer owns, walked from disk under the content roots.
    std::vector<fs::path> TextCorpus()
    {
        std::vector<fs::path> files;
        const fs::path        root = RepoRoot() / "Editor";
        for ( const auto& entry : fs::recursive_directory_iterator( root ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( entry.is_regular_file() && ( ext == ".desce" || ext == ".demat" || ext == ".deprefab" ) )
                files.push_back( entry.path() );
        }
        return files;
    }

    std::vector<std::string> Lines( const std::string& text )
    {
        std::vector<std::string> lines;
        std::istringstream       in( text );
        for ( std::string line; std::getline( in, line ); )
            lines.push_back( line );
        return lines;
    }
} // namespace

// The corpus IS canonical, and going through the old single-line form and back is byte-identical — so the
// single-line file the migration replaced and the canonical file are one document, value for value.
TEST( CanonicalText, EveryCorpusFileIsCanonicalAndRoundTripsThroughTheSingleLineForm )
{
    ASSERT_FALSE( RepoRoot().empty() );
    const auto corpus = TextCorpus();
    ASSERT_GT( corpus.size(), 200u ) << "the walk found too few scenes/materials/prefabs to be the corpus";
    for ( const fs::path& file : corpus )
    {
        const std::string text = ReadAll( file );
        EXPECT_TRUE( IsCanonicalJsonText( text ) ) << file << " is not in canonical layout: run SceneMigrator";
        const std::string oneLine = Minified( text );
        ASSERT_FALSE( oneLine.empty() ) << file << " is not JSON";
        const auto again = CanonicalJsonText( oneLine );
        ASSERT_TRUE( again ) << again.GetError();
        EXPECT_EQ( again.GetValue(), text ) << file << ": write -> parse -> write is not byte-identical";
    }
}

// One field of one entity changed -> exactly one line of the file changed; the rest of the scene is untouched.
TEST( CanonicalText, ChangingOneFieldOfOneEntityChangesOnlyItsLine )
{
    const std::string text = ReadAll( RepoRoot() / "Editor/Resources/Assets/Scenes/ANIM_ClipProbe.desce" );
    ASSERT_FALSE( text.empty() );
    yyjson_doc*     doc      = yyjson_read( text.data(), text.size(), YYJSON_READ_NOFLAG );
    yyjson_mut_doc* mutable_ = yyjson_doc_mut_copy( doc, nullptr );
    yyjson_doc_free( doc );
    yyjson_mut_val* entities = yyjson_mut_obj_get( yyjson_mut_doc_get_root( mutable_ ), "Entities" );
    ASSERT_GT( yyjson_mut_arr_size( entities ), 2u );
    yyjson_mut_val* entity = yyjson_mut_arr_get( entities, 1 );
    yyjson_mut_obj_put( entity, yyjson_mut_str( mutable_, "Tag" ), yyjson_mut_str( mutable_, "Renamed" ) );
    std::size_t length = 0;
    char*       edited = yyjson_mut_write( mutable_, YYJSON_WRITE_NOFLAG, &length );
    const auto  after  = CanonicalJsonText( std::string_view( edited, length ) );
    std::free( edited );
    yyjson_mut_doc_free( mutable_ );
    ASSERT_TRUE( after ) << after.GetError();

    const auto before = Lines( text );
    const auto lines  = Lines( after.GetValue() );
    ASSERT_EQ( before.size(), lines.size() ) << "a one-field edit moved lines";
    std::size_t changed = 0;
    for ( std::size_t i = 0; i < lines.size(); ++i )
        changed += before[i] != lines[i] ? 1 : 0;
    EXPECT_EQ( changed, 1u ) << "a one-field edit changed " << changed << " lines of " << lines.size();
}

// Every double reads back bit-identical, and its spelling is as short as std::to_chars' shortest form.
TEST( CanonicalText, NumbersRoundTripExactlyAndShortest )
{
    const std::array<double, 12> values = { 0.1,
                                            1.0 / 3.0,
                                            -0.0,
                                            static_cast<double>( 0.3f ),
                                            static_cast<double>( FLT_MAX ),
                                            static_cast<double>( FLT_MIN ),
                                            std::numeric_limits<double>::denorm_min(),
                                            DBL_MAX,
                                            DBL_MIN,
                                            1e-7,
                                            123456789.0,
                                            22.0 };
    for ( const double v : values )
    {
        std::array<char, 64> shortest{};
        const auto           end = std::to_chars( shortest.data(), shortest.data() + shortest.size(), v ).ptr;
        const std::string    spelled( shortest.data(), end );
        // A real is stated as a real, as rfl::json states it: "-0" alone would be the INTEGER zero and lose its
        // sign.
        const bool real = spelled.find_first_of( ".eE" ) != std::string::npos;
        const auto text = CanonicalJsonText( "[" + spelled + ( real ? "" : ".0" ) + "]" );
        ASSERT_TRUE( text ) << text.GetError();
        const std::string token = text.GetValue().substr( 1, text.GetValue().size() - 3 );
        const double      back  = std::strtod( token.c_str(), nullptr );
        EXPECT_EQ( std::memcmp( &back, &v, sizeof v ), 0 ) << spelled << " came back as " << token;
        EXPECT_LE( token.size(), spelled.size() + 2 ) << token << " is longer than the shortest " << spelled;
    }
}

TEST( CanonicalText, LayoutRulesArePinned )
{
    const auto text =
         CanonicalJsonText( R"({"a":[1.0,2.0,3.0],"b":{},"c":[],"d":[{"x":1}],"e":[1,2,3,4,5,6,7,8,9]})" );
    ASSERT_TRUE( text ) << text.GetError();
    EXPECT_EQ( text.GetValue(), "{\n"
                                "    \"a\": [1.0, 2.0, 3.0],\n"
                                "    \"b\": {},\n"
                                "    \"c\": [],\n"
                                "    \"d\": [\n"
                                "        {\n"
                                "            \"x\": 1\n"
                                "        }\n"
                                "    ],\n"
                                "    \"e\": [\n"
                                "        1, 2, 3, 4, 5, 6, 7, 8,\n"
                                "        9\n"
                                "    ]\n"
                                "}\n" );
}

TEST( CanonicalText, RefusesTextThatIsNotJsonAndNamesWhere )
{
    const auto text = CanonicalJsonText( R"({"a":1,})" );
    ASSERT_FALSE( text );
    EXPECT_NE( text.GetError().find( "at byte" ), std::string::npos ) << text.GetError();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
