// THE JSON CENSUS (JS1a, owner decision 2026-09-25). Structs meet JSON through ONE door, Common/Json/Json.hpp.
// Naming rfl::json, rfl::Generic or yyjson_ directly, or building JSON out of escaped-quote string pieces, is
// allowed only in: the facade itself, the migrators (Tools/SceneMigrator and any path containing "Migrat" —
// a migrator edits documents of a schema that no longer has a struct), and the NAMED register
// json_census_register.txt, one row per file with its owner task. The check is two-sided: a file outside
// the allowance that uses JSON directly fails, and so does a register row whose file is already clean — the
// row must be deleted with the move, so the register can only shrink. The DESERT_JSON_STRUCT marks are
// counted here too, and two types may not claim one format name.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Common/Source/Common/Json/Json.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Line comments go first: prose that NAMES rfl::json is not a use of it.
    std::string StripLineComments( const std::string& source )
    {
        std::string        out;
        std::istringstream lines( source );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            const auto comment = line.find( "//" );
            if ( comment != std::string::npos && line.find_first_not_of( " \t" ) == comment )
                line.clear();
            out += line;
            out += '\n';
        }
        return out;
    }

    // What counts as talking JSON directly. The hand-built form is an escaped key followed by a colon
    // ("\"Name\":"), the shape of a JSON document assembled from string pieces.
    bool UsesJsonDirectly( const std::string& source )
    {
        static const std::regex kHandBuilt( R"(\\"[A-Za-z_][A-Za-z0-9_]*\\"\s*:)" );
        const std::string       code = StripLineComments( source );
        return code.find( "rfl::json::" ) != std::string::npos ||
               code.find( "rfl::Generic" ) != std::string::npos || code.find( "yyjson_" ) != std::string::npos ||
               std::regex_search( code, kHandBuilt );
    }

    // What counts as walking a value tree past the facade (JS1c): calling the library's own accessors on a
    // Json::Value instead of reading it through a Json::Node. The names are the library's; a file that uses them
    // reads a document without a path, so its errors cannot say which entity and component a bad value is in.
    bool WalksValueTreeDirectly( const std::string& source )
    {
        static const std::regex kAccessor( R"(\.(to_object|to_array|to_int64|to_double|variant)\s*\(\s*\))" );
        return std::regex_search( StripLineComments( source ), kAccessor );
    }

    bool IsAllowedByRule( const std::string& rel )
    {
        return rel.starts_with( "Desert/Common/Source/Common/Json/" ) ||
               rel.starts_with( "Tools/SceneMigrator/" ) || rel.find( "Migrat" ) != std::string::npos ||
               // This suite and the facade's own suite quote the patterns they police.
               rel.starts_with( "Desert/Tests/Common/JsonCensus/" ) ||
               rel.starts_with( "Desert/Tests/Common/JsonFacade/" );
    }

    const std::vector<std::string> kSourceRoots = { "Desert/Common/Source", "Desert/Desert/Source", "Desert/Tests",
                                                    "Editor/Source",        "Runtime/Source",       "Tools" };

    std::vector<std::string> SourceFiles( const std::string& root )
    {
        static const std::set<std::string> kExtensions = { ".cpp", ".hpp", ".h", ".inl", ".mm", ".c" };
        std::vector<std::string>           files;
        for ( const auto& top : kSourceRoots )
        {
            const fs::path base = fs::path( root ) / top;
            if ( !fs::exists( base ) )
                continue;
            for ( auto it = fs::recursive_directory_iterator( base ); it != fs::recursive_directory_iterator();
                  ++it )
            {
                // Generated/ is DesertHeaderTool's output (the generator is a register row); build/ is not source.
                const std::string name = it->path().filename().string();
                if ( it->is_directory() && ( name == "Generated" || name == "build" || name == "ThirdParty" ) )
                {
                    it.disable_recursion_pending();
                    continue;
                }
                if ( it->is_regular_file() && kExtensions.contains( it->path().extension().string() ) )
                    files.push_back( fs::relative( it->path(), root ).generic_string() );
            }
        }
        std::sort( files.begin(), files.end() );
        return files;
    }

    struct Row
    {
        std::string File;
        std::string Why;
    };

    std::vector<Row> Register( const std::string& root, const std::string& file = "json_census_register.txt" )
    {
        std::vector<Row>   rows;
        std::istringstream lines( ReadAll( fs::path( root ) / "Desert/Tests/Common/JsonCensus" / file ) );
        std::string line;
        while ( std::getline( lines, line ) )
        {
            if ( line.empty() || line.starts_with( '#' ) )
                continue;
            const auto bar = line.find( " | " );
            rows.push_back(
                 { line.substr( 0, bar ), bar == std::string::npos ? std::string() : line.substr( bar + 3 ) } );
        }
        return rows;
    }
} // namespace

TEST( JsonCensus, TheDetectorSeesEveryForm )
{
    EXPECT_TRUE( UsesJsonDirectly( "auto r = rfl::json::read<Foo>( text );" ) );
    EXPECT_TRUE( UsesJsonDirectly( "rfl::Generic g;" ) );
    EXPECT_TRUE( UsesJsonDirectly( "yyjson_doc* doc = nullptr;" ) );
    EXPECT_TRUE( UsesJsonDirectly( "std::string s = \"{\\\"Name\\\": \" + name;" ) );
    EXPECT_FALSE( UsesJsonDirectly( "    // rfl::json::write is what the facade calls\nint x = 0;" ) );
    EXPECT_FALSE( UsesJsonDirectly( "auto r = Common::Json::Read<Foo>( text );" ) );
    EXPECT_FALSE( IsAllowedByRule( "Editor/Source/Editor/NewPanel.cpp" ) );
    EXPECT_TRUE( IsAllowedByRule( "Tools/SceneMigrator/Source/SceneMigration.cpp" ) );

    EXPECT_TRUE( WalksValueTreeDirectly( "const auto object = block.to_object();" ) );
    EXPECT_TRUE( WalksValueTreeDirectly( "auto a = v.to_array ( );" ) );
    EXPECT_TRUE( WalksValueTreeDirectly( "if ( auto i = g.to_int64(); i ) {}" ) );
    EXPECT_TRUE( WalksValueTreeDirectly( "double d = g.to_double().value();" ) );
    EXPECT_TRUE( WalksValueTreeDirectly( "std::get_if<Object>( &g.variant() );" ) );
    EXPECT_FALSE( WalksValueTreeDirectly( "    // g.to_object() is what Node replaces\nint x = 0;" ) );
    EXPECT_FALSE( WalksValueTreeDirectly( "node.Find( \"Settings\" )->AsNumber();" ) );
}

// JS1c: a value tree is read through Json::Node (Common/Json/Document.hpp), which carries the path every issue
// names. Same two-sided register as above, in its own file so each row names the JS1c step that removes it.
TEST( JsonCensus, ValueTreesAreReadThroughNodes )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";

    const auto                 rows = Register( root, "json_tree_access_register.txt" );
    std::map<std::string, int> registered;
    for ( const Row& row : rows )
    {
        EXPECT_FALSE( row.Why.empty() ) << row.File << ": a register row must say who removes it and why";
        EXPECT_EQ( registered[row.File]++, 0 ) << row.File << " is registered twice";
    }

    std::set<std::string> users;
    for ( const std::string& rel : SourceFiles( root ) )
    {
        if ( IsAllowedByRule( rel ) || !WalksValueTreeDirectly( ReadAll( fs::path( root ) / rel ) ) )
            continue;
        users.insert( rel );
        EXPECT_TRUE( registered.contains( rel ) )
             << rel << " walks a Json::Value with the library's accessors (.to_object() / .to_array() / "
             << ".to_int64() / .to_double() / .variant()). Read it through Json::Node (Common/Json/Document.hpp).";
    }
    EXPECT_FALSE( users.empty() ) << "the tree-access detector found nothing at all: it has gone blind";

    for ( const Row& row : rows )
        EXPECT_TRUE( users.contains( row.File ) )
             << row.File
             << " no longer walks value trees directly (or no longer exists): delete its register row.";
}

TEST( JsonCensus, JsonIsSpokenOnlyThroughTheFacade )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";

    const auto                 rows = Register( root );
    std::map<std::string, int> registered;
    for ( const Row& row : rows )
    {
        EXPECT_FALSE( row.Why.empty() ) << row.File << ": a register row must say who removes it and why";
        EXPECT_EQ( registered[row.File]++, 0 ) << row.File << " is registered twice";
    }

    std::set<std::string> users;
    for ( const std::string& rel : SourceFiles( root ) )
    {
        if ( IsAllowedByRule( rel ) || !UsesJsonDirectly( ReadAll( fs::path( root ) / rel ) ) )
            continue;
        users.insert( rel );
        EXPECT_TRUE( registered.contains( rel ) )
             << rel << " talks JSON directly (rfl::json / rfl::Generic / yyjson_ / hand-built JSON). Go through "
             << "Common/Json/Json.hpp (Json::Read/Write/ReadFile/WriteFileAtomic).";
    }

    for ( const Row& row : rows )
        EXPECT_TRUE( users.contains( row.File ) )
             << row.File << " no longer talks JSON directly (or no longer exists): delete its register row.";
}

TEST( JsonCensus, EveryFormatNameHasOneType )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    static const std::regex kMark(
         R"(DESERT_JSON_STRUCT\(\s*([A-Za-z_:0-9]+)\s*,\s*"([^"]+)\"\s*,\s*(\d+)\s*\))" );
    std::map<std::string, std::vector<std::string>> byFormat;
    for ( const std::string& rel : SourceFiles( root ) )
    {
        if ( rel.starts_with( "Desert/Common/Source/Common/Json/" ) )
            continue;
        const std::string code = StripLineComments( ReadAll( fs::path( root ) / rel ) );
        for ( auto it = std::sregex_iterator( code.begin(), code.end(), kMark ); it != std::sregex_iterator();
              ++it )
            byFormat[( *it )[2].str()].push_back( rel + ":" + ( *it )[1].str() );
    }

    EXPECT_TRUE( byFormat.contains( "MachineSettings" ) ) << "the census lost the marks it is meant to count";
    for ( const auto& [format, types] : byFormat )
        EXPECT_EQ( types.size(), 1u ) << "format \"" << format << "\" is claimed by " << types.size() << " types";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
