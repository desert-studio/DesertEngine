// CanonicalText (AF6): the one writer of text assets lays a document out so that git can diff and merge it,
// and never changes a value while doing so.
#include <Common/Content/CanonicalText.hpp>

#include <gtest/gtest.h>
#include <rflcpp/rfl/thirdparty/yyjson.h>

#include <algorithm>
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

// Entity names, UI text and localisation keys are authored in any script. The single-line writer (rfl::json,
// yyjson with no flags) stored non-ASCII as raw UTF-8 and escaped only what JSON requires; the canonical writer
// must spell every string byte for byte the same, or re-laying-out the corpus would silently rewrite names.
TEST( CanonicalText, NonAsciiAndEscapedStringsRoundTripByteForByte )
{
    const std::string cyrillic = "\xD0\x9F\xD1\x83\xD1\x81\xD1\x82\xD1\x8B\xD0\xBD\xD1\x8F"; // "Pustynya"
    const std::string cactus   = "\xF0\x9F\x8C\xB5";                                         // U+1F335
    const std::string value    = cyrillic + " " + cactus + " \\\"q\"\n\t\x01/";
    const std::string key      = "\xD0\x98\xD0\xBC\xD1\x8F"; // "Imya"

    // The single-line form exactly as rfl::json writes it: yyjson over a mutable document, no flags.
    yyjson_mut_doc* doc  = yyjson_mut_doc_new( nullptr );
    yyjson_mut_val* root = yyjson_mut_obj( doc );
    yyjson_mut_doc_set_root( doc, root );
    yyjson_mut_obj_add_strn( doc, root, key.c_str(), value.c_str(), value.size() );
    std::size_t length  = 0;
    char*       written = yyjson_mut_write( doc, YYJSON_WRITE_NOFLAG, &length );
    ASSERT_NE( written, nullptr );
    const std::string singleLine( written, length );
    std::free( written );
    yyjson_mut_doc_free( doc );

    // The non-ASCII bytes stay raw: an escaped \u form would be the same value but not the same file.
    ASSERT_NE( singleLine.find( cyrillic ), std::string::npos ) << singleLine;
    ASSERT_NE( singleLine.find( cactus ), std::string::npos ) << singleLine;

    const auto text = CanonicalJsonText( singleLine );
    ASSERT_TRUE( text ) << text.GetError();
    // The canonical text is the single-line member re-indented, with every byte of key and value unchanged.
    const std::string member = singleLine.substr( 1, singleLine.size() - 2 );
    EXPECT_EQ( text.GetValue(),
               "{\n    " + std::string( member ).insert( member.find( "\":" ) + 2, " " ) + "\n}\n" );
    EXPECT_TRUE( IsCanonicalJsonText( text.GetValue() ) );

    // And it reads back to the very bytes that were authored.
    yyjson_doc* back = yyjson_read( text.GetValue().data(), text.GetValue().size(), YYJSON_READ_NOFLAG );
    ASSERT_NE( back, nullptr );
    yyjson_val* read = yyjson_obj_getn( yyjson_doc_get_root( back ), key.data(), key.size() );
    ASSERT_NE( read, nullptr );
    EXPECT_EQ( std::string( yyjson_get_str( read ), yyjson_get_len( read ) ), value );
    yyjson_doc_free( back );
}

namespace
{
    // Source text with // comments removed, so a census counts code and not prose about code.
    std::string CodeOf( const fs::path& file )
    {
        std::string code;
        for ( const std::string& line : Lines( ReadAll( file ) ) )
        {
            const auto comment = line.find( "//" );
            code += line.substr( 0, comment ) + '\n';
        }
        return code;
    }

    // Every file that WRITES a text asset kind, by the file that does the writing. A file that calls the
    // atomic write primitive with JSON must hold the canonical writer itself, or be named here with the
    // function that already hands it canonical text.
    struct CanonicalAtSource
    {
        const char* File;
        const char* Source; // the file whose serializer produces the canonical text this file writes
    };
    // The two material writers: since AF7 SurfaceMaterialAsset serializes through MaterialFormat's
    // WriteMaterialJson (header-stamped, one writer for the editor and the tools), which holds the call.
    constexpr std::array kCanonicalAtSource{
         CanonicalAtSource{ "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp",
                            "Desert/Desert/Source/Engine/Assets/MaterialFormat.hpp" },
         CanonicalAtSource{ "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.cpp",
                            "Desert/Desert/Source/Engine/Assets/MaterialFormat.hpp" },
         CanonicalAtSource{ "Desert/Desert/Source/Engine/Assets/Prefab/PrefabAsset.cpp",
                            "Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp" },
         CanonicalAtSource{ "Editor/Source/Editor/Panels/SceneProperties/ScenePropertiesPanel.cpp",
                            "Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp" },
         CanonicalAtSource{
              "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/PrefabComponentWidget.cpp",
              "Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp" },
    };

    // Files that serialize and write, but what they write is not an authored text asset: machine and editor
    // settings, the project file, registries, pak/cook products, logs. Named one by one, with the reason.
    struct NotATextAsset
    {
        const char* File;
        const char* Writes;
    };
    constexpr std::array kNotATextAsset{
         NotATextAsset{ "Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
                        ".deproj and the projects registry" },
         NotATextAsset{ "Desert/Desert/Source/Engine/Assets/ContentRegistry.hpp", "the cooked content registry" },
         NotATextAsset{ "Editor/Source/Editor/Packaging/GamePackager.cpp",
                        "package manifest, ICD json, launcher" },
         NotATextAsset{ "Editor/Source/Editor/Core/EditorPreferences.cpp", "editor preferences" },
         NotATextAsset{ "Editor/Source/Editor/Panels/Collections/CollectionsPanel.cpp",
                        "a collection apply record" },
         NotATextAsset{ "Tools/WorldGen/Source/WorldGenMain.cpp", "generated world output" },
         NotATextAsset{ "Tools/AssetRegistryTool/Source/Main.cpp", "the asset registry" },
         NotATextAsset{ "Tools/PakTool/Source/Main.cpp", "pak contents and its listing" },
    };

    std::vector<fs::path> WriterSources()
    {
        std::vector<fs::path> files;
        const fs::path        root = RepoRoot();
        for ( const char* dir : { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Tools" } )
            for ( const auto& entry : fs::recursive_directory_iterator( root / dir ) )
            {
                const std::string ext = entry.path().extension().string();
                if ( entry.is_regular_file() && ( ext == ".cpp" || ext == ".hpp" || ext == ".h" ) )
                    files.push_back( entry.path() );
            }
        return files;
    }
} // namespace

// No statement hands rfl::json's single-line text straight to the write primitive: every text asset reaches
// the disk through CanonicalJsonText, so a save never undoes the layout the corpus was migrated to.
TEST( CanonicalText, EveryTextAssetWriterGoesThroughTheCanonicalWriter )
{
    const fs::path           root = RepoRoot();
    std::vector<std::string> writers;
    for ( const fs::path& file : WriterSources() )
    {
        const std::string code = CodeOf( file );
        const std::string rel  = fs::relative( file, root ).generic_string();

        // One statement: from the previous ';' '{' or '}' to the next ';'.
        for ( std::size_t at = code.find( "WriteContentToFileAtomic" ); at != std::string::npos;
              at             = code.find( "WriteContentToFileAtomic", at + 1 ) )
        {
            const std::size_t begin     = code.find_last_of( ";{}", at ) + 1;
            const std::string statement = code.substr( begin, code.find( ';', at ) - begin );
            EXPECT_FALSE( statement.find( "rfl::json::write" ) != std::string::npos &&
                          statement.find( "CanonicalJsonText" ) == std::string::npos )
                 << rel << " writes rfl::json text to disk without CanonicalJsonText:\n"
                 << statement;
        }

        const bool writesFiles = code.find( "WriteContentToFileAtomic" ) != std::string::npos;
        const bool serializes  = code.find( "rfl::json::write" ) != std::string::npos ||
                                code.find( "Serialize(" ) != std::string::npos ||
                                code.find( "Save()" ) != std::string::npos;
        // A file whose saves go through WriteCanonicalJsonFileAtomic names the canonical writer by that call.
        const bool namesCanonical = code.find( "CanonicalJsonText" ) != std::string::npos ||
                                    code.find( "WriteCanonicalJsonFileAtomic" ) != std::string::npos ||
                                    // PrefabAsset::SaveTo writes WritePrefabJson's text, which is canonical.
                                    code.find( "->SaveTo(" ) != std::string::npos;
        if ( !writesFiles || !serializes || namesCanonical )
            continue;
        writers.push_back( rel );
        const auto row      = std::find_if( kCanonicalAtSource.begin(), kCanonicalAtSource.end(),
                                            [&]( const CanonicalAtSource& r ) { return rel == r.File; } );
        const bool notAsset = std::any_of( kNotATextAsset.begin(), kNotATextAsset.end(),
                                           [&]( const NotATextAsset& r ) { return rel == r.File; } );
        if ( notAsset )
            continue;
        if ( row == kCanonicalAtSource.end() )
        {
            ADD_FAILURE() << rel << " serializes and writes files but never calls CanonicalJsonText; route the "
                          << "text through it, or name the serializer that already does in kCanonicalAtSource";
            continue;
        }
        EXPECT_NE( CodeOf( root / row->Source ).find( "CanonicalJsonText" ), std::string::npos )
             << rel << " relies on " << row->Source << " for canonical text, and that file no longer calls it";
    }
    // Every register row is still a writer: a row whose file stopped writing would pin nothing.
    const auto isWriter = [&]( const char* file )
    { return std::find( writers.begin(), writers.end(), file ) != writers.end(); };
    for ( const CanonicalAtSource& row : kCanonicalAtSource )
        EXPECT_TRUE( isWriter( row.File ) ) << row.File << " no longer serializes and writes; drop its row";
    for ( const NotATextAsset& row : kNotATextAsset )
        EXPECT_TRUE( isWriter( row.File ) ) << row.File << " no longer serializes and writes; drop its row";
}

TEST( CanonicalText, RefusesTextThatIsNotJsonAndNamesWhere )
{
    const auto text = CanonicalJsonText( R"({"a":1,})" );
    ASSERT_FALSE( text );
    EXPECT_NE( text.GetError().find( "at byte" ), std::string::npos ) << text.GetError();
}

// A writer that produced something that is not JSON is a defect in this engine, but the SAVE that met it
// must refuse with the reason and leave the file as it was - never abort with the author's work unsaved,
// never truncate the file it was about to replace, never leave its temporary behind.
TEST( CanonicalText, AFailedWriterRefusesTheSaveAndLeavesTheFileAsItWas )
{
    const auto refused = Common::Content::CanonicalJsonTextOfWriterOutput( R"({"a":1,)" );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "not JSON" ), std::string::npos ) << refused.GetError();

    const fs::path dir = fs::temp_directory_path() / "canonical_text_refused_save";
    fs::remove_all( dir );
    fs::create_directories( dir );
    const fs::path    file   = dir / "Asset.demat";
    const std::string before = "{\n  \"kept\": true\n}\n";
    {
        std::ofstream out( file, std::ios::binary );
        out << before;
    }

    const auto saved = Common::Content::WriteCanonicalJsonFileAtomic( file, R"({"kept":false,)" );
    ASSERT_FALSE( saved );
    EXPECT_NE( saved.GetError().find( "Asset.demat" ), std::string::npos ) << saved.GetError();

    // The reader is scoped: on Windows an open stream (no FILE_SHARE_DELETE) makes ANY replace of the
    // file refuse with "Access is denied" - MoveFileExW and ReplaceFileW alike - so a reader left open
    // here refused the good save below for a reason that is this test's, not the writer's.
    std::ostringstream now;
    {
        const std::ifstream in( file, std::ios::binary );
        now << in.rdbuf();
    }
    EXPECT_EQ( now.str(), before );
    EXPECT_EQ( std::distance( fs::directory_iterator( dir ), fs::directory_iterator() ), 1 )
         << "a refused save left a file behind";

    // The same call with good text replaces the file, laid out canonically.
    ASSERT_TRUE( Common::Content::WriteCanonicalJsonFileAtomic( file, R"({"kept":false})" ) );
    // Scoped for the same reason as the reader above: an open stream makes remove_all below refuse on
    // Windows ("being used by another process").
    std::ostringstream after;
    {
        const std::ifstream again( file, std::ios::binary );
        after << again.rdbuf();
    }
    EXPECT_TRUE( IsCanonicalJsonText( after.str() ) );
    EXPECT_NE( after.str().find( "false" ), std::string::npos );
    fs::remove_all( dir );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
