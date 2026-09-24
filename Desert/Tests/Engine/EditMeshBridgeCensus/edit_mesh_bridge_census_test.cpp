// "Outside Geometry/EditMesh* and the bridge, nobody includes an EditMesh header" (P8a).
//
// StaticMeshComponent::EditableMesh is an FDynamicMesh3; the operations and tools not yet ported still run
// on EditMesh, and every crossing goes through Engine/Geometry/EditMeshBridge.{hpp,cpp}. The promise that
// makes P8b a deletion and not a hunt is that no other file names an EditMesh header: a tool that includes
// Engine/Geometry/EditMesh.hpp directly has built a second crossing the bridge does not know about.
//
// The census reads the product sources (comments stripped) and fails on every `#include` of a
// Geometry/EditMesh* header other than EditMeshBridge.hpp, from any file that is not itself a
// Geometry/EditMesh* file. The rule is also run against hand-written inputs below, so a census that has
// stopped seeing includes fails instead of passing on an empty answer.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kBridgeHeader = "EditMeshBridge.hpp";
    constexpr const char* kScanRoots[]  = { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source",
                                            "Runtime/Source", "Tools" };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            if ( fs::exists( prefix + "Desert/Desert/Source/Engine/Geometry/EditMeshBridge.hpp" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Not a C++ lexer: a `//` inside a string literal can only HIDE a line from the census, never invent one.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src.compare( i, 2, "//" ) == 0 )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
            }
            else if ( src.compare( i, 2, "/*" ) == 0 )
            {
                const std::size_t end = src.find( "*/", i + 2 );
                i                     = end == std::string::npos ? src.size() : end + 2;
            }
            else
            {
                out.push_back( src[i++] );
            }
        }
        return out;
    }

    std::string Basename( const std::string& path )
    {
        const std::size_t slash = path.find_last_of( '/' );
        return slash == std::string::npos ? path : path.substr( slash + 1 );
    }

    std::string Directory( const std::string& path )
    {
        const std::size_t slash = path.find_last_of( '/' );
        return slash == std::string::npos ? std::string{} : path.substr( 0, slash );
    }

    bool EndsWith( const std::string& s, const std::string& tail )
    {
        return s.size() >= tail.size() && s.compare( s.size() - tail.size(), tail.size(), tail ) == 0;
    }

    bool IsGeometryEditMeshFile( const std::string& path )
    {
        return EndsWith( Directory( path ), "Engine/Geometry" ) && Basename( path ).rfind( "EditMesh", 0 ) == 0;
    }

    // Does `#include <spelled>` in @p includer name a Geometry/EditMesh* header other than the bridge?
    bool NamesEditMeshHeader( const std::string& includer, const std::string& spelled )
    {
        const std::string name = Basename( spelled );
        if ( name.rfind( "EditMesh", 0 ) != 0 || name == kBridgeHeader )
            return false;
        if ( spelled.find( '/' ) != std::string::npos )
            return EndsWith( Directory( spelled ), "Geometry" );
        return EndsWith( Directory( includer ), "Engine/Geometry" ); // a quoted sibling include
    }

    // Every forbidden include in one file, as "path: spelled". @p path is repository-relative.
    std::vector<std::string> Violations( const std::string& path, const std::string& source )
    {
        std::vector<std::string> found;
        if ( IsGeometryEditMeshFile( path ) )
            return found;
        std::istringstream lines( StripComments( source ) );
        for ( std::string line; std::getline( lines, line ); )
        {
            const std::size_t hash = line.find_first_not_of( " \t" );
            if ( hash == std::string::npos || line[hash] != '#' )
                continue;
            const std::size_t word = line.find_first_not_of( " \t", hash + 1 );
            if ( word == std::string::npos || line.compare( word, 7, "include" ) != 0 )
                continue;
            const std::size_t open = line.find_first_of( "\"<", word + 7 );
            if ( open == std::string::npos )
                continue;
            const std::size_t close = line.find( line[open] == '<' ? '>' : '"', open + 1 );
            if ( close == std::string::npos )
                continue;
            const std::string spelled = line.substr( open + 1, close - open - 1 );
            if ( NamesEditMeshHeader( path, spelled ) )
                found.push_back( path + ": " + spelled );
        }
        return found;
    }

    // The crossings that exist outside the bridge today, one named row each with the card that removes it.
    // A row that no longer matches an include fails too, so the register can only shrink by deleting rows.
    struct Known
    {
        const char* Violation;
        const char* Why;
    };

    constexpr Known kKnown[] = {
         { "Desert/Desert/Source/Engine/Geometry/ShapeGenerators.hpp: Engine/Geometry/EditMeshConversion.hpp",
           "the shape generators build an EditMesh; P16 ports them onto FDynamicMesh3" },
         { "Desert/Desert/Source/Engine/Geometry/VoxelBlockout.hpp: Engine/Geometry/EditMeshConversion.hpp",
           "the CubeGrid bake builds an EditMesh; P18 ports it" },
         { "Tools/SceneMigrator/Source/SceneMigration.cpp: Engine/Geometry/EditMeshConversion.hpp",
           "the scene migrator writes the saved EditMesh form of old scenes; the saved form is shared by both "
           "cores, the includes go with the EditMesh files in P8b" },
         { "Tools/SceneMigrator/Source/SceneMigration.cpp: Engine/Geometry/EditMeshSerialization.hpp",
           "as the row above: the migrator's saved-form writer, removed in P8b" },
         { "Desert/Desert/Source/Engine/Geometry/DynamicMeshSelection.hpp: Engine/Geometry/EditMeshSelection.hpp",
           "the ported-core selection (P10) shares ElementSelection and the algorithms with the EditMesh path while "
           "the operations of P11-P19 still run on it; the types move to the ported core in P8b" },
    };

    bool IsKnown( const std::string& violation )
    {
        for ( const Known& k : kKnown )
            if ( violation == k.Violation )
                return true;
        return false;
    }

    struct Scan
    {
        std::size_t              Files = 0;
        std::vector<std::string> Violations;
        bool                     BridgeIncludesEditMesh = false;
    };

    Scan ScanTree( const std::string& root )
    {
        Scan scan;
        for ( const char* sub : kScanRoots )
        {
            const fs::path base = fs::path( root ) / sub;
            if ( !fs::exists( base ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( base ) )
            {
                const std::string ext = entry.path().extension().string();
                if ( !entry.is_regular_file() ||
                     ( ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".inl" && ext != ".mm" ) )
                    continue;
                const std::string rel    = fs::relative( entry.path(), root ).generic_string();
                const std::string source = ReadFile( entry.path() );
                ++scan.Files;
                for ( auto& v : Violations( rel, source ) )
                    scan.Violations.push_back( std::move( v ) );
                if ( Basename( rel ) == kBridgeHeader &&
                     StripComments( source ).find( "Engine/Geometry/EditMesh.hpp" ) != std::string::npos )
                    scan.BridgeIncludesEditMesh = true;
            }
        }
        return scan;
    }
} // namespace

// --- The rule, on inputs whose answer is known. Each is a mutation the census must catch or let pass.

TEST( EditMeshBridgeCensus, AToolIncludingEditMeshDirectlyIsCaught )
{
    const auto v = Violations( "Editor/Source/Editor/Panels/ViewportPanel/Tools/PolyEditTool.cpp",
                               "#include \"PolyEditTool.hpp\"\n#include \"Engine/Geometry/EditMesh.hpp\"\n" );
    ASSERT_EQ( v.size(), 1u );
    EXPECT_NE( v[0].find( "Engine/Geometry/EditMesh.hpp" ), std::string::npos );
}

TEST( EditMeshBridgeCensus, AngleBracketsAndIndentedDirectivesAreCaught )
{
    EXPECT_EQ( Violations( "Editor/Source/Editor/Core/Selection/X.cpp",
                           "  #  include <Engine/Geometry/EditMeshOperations.hpp>\n" )
                    .size(),
               1u );
}

TEST( EditMeshBridgeCensus, ASiblingIncludeFromGeometryIsCaught )
{
    EXPECT_EQ( Violations( "Desert/Desert/Source/Engine/Geometry/DynamicMeshToMeshAsset.cpp",
                           "#include \"EditMeshSelection.hpp\"\n" )
                    .size(),
               1u );
}

TEST( EditMeshBridgeCensus, TheBridgeAndTheEditMeshFilesThemselvesPass )
{
    EXPECT_TRUE( Violations( "Editor/Source/Editor/Core/Selection/X.cpp",
                             "#include \"Engine/Geometry/EditMeshBridge.hpp\"\n" )
                      .empty() );
    EXPECT_TRUE( Violations( "Desert/Desert/Source/Engine/Geometry/EditMeshBridge.hpp",
                             "#include \"Engine/Geometry/EditMesh.hpp\"\n" )
                      .empty() );
    EXPECT_TRUE( Violations( "Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
                             "#include \"EditMeshAttributes.hpp\"\n" )
                      .empty() );
}

TEST( EditMeshBridgeCensus, CommentsAndLookalikeNamesPass )
{
    EXPECT_TRUE( Violations( "Editor/Source/Editor/X.cpp",
                             "// #include \"Engine/Geometry/EditMesh.hpp\"\n/* #include <Engine/Geometry/"
                             "EditMeshSelection.hpp> */\n#include \"Engine/ECS/EditableMesh.hpp\"\n" )
                      .empty() );
}

// --- The tree.

TEST( EditMeshBridgeCensus, NoSourceOutsideTheBridgeIncludesAnEditMeshHeader )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the working directory";
    const Scan scan = ScanTree( root );

    // The instrument must see the one crossing it knows exists, or a clean answer means nothing.
    ASSERT_GT( scan.Files, 500u ) << "the census read too few files to be answering the question";
    ASSERT_TRUE( scan.BridgeIncludesEditMesh ) << "the census does not see the bridge's own EditMesh include";

    std::string list;
    for ( const auto& v : scan.Violations )
        if ( !IsKnown( v ) )
            list += "\n  " + v;
    for ( const Known& k : kKnown )
        EXPECT_NE( std::find( scan.Violations.begin(), scan.Violations.end(), k.Violation ),
                   scan.Violations.end() )
             << "stale register row (the include is gone, delete the row): " << k.Violation;
    EXPECT_TRUE( list.empty() )
         << "EditMesh headers are included outside Geometry/EditMesh* and EditMeshBridge.{hpp,cpp}; include "
            "Engine/Geometry/EditMeshBridge.hpp and go through Geometry::Bridge instead:"
         << list;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
