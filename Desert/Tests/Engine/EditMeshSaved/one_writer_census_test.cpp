// ONE WRITER OF A STATIC MESH'S GEOMETRY - a census over source text (M4).
//
// StaticMeshComponent::EditableMesh is the source of truth and RuntimeMesh is derived from it; the two only
// stay in step because exactly one function writes both (Engine/ECS/EditableMesh.hpp). That sentence is a
// COMMENT, and a comment asserting a guarantee is not the code that keeps it. Before M4 three places wrote
// the render buffer of a static mesh (CubeGrid replaced it, PolyEdit edited it in place, the loader built
// it), so the rule this file guards is exactly the one the tree used to break.
//
// WHAT IS FORBIDDEN: an assignment or reset of `.EditableMesh` anywhere but EditableMesh.cpp, and an
// assignment or reset of `.RuntimeMesh` whose (file, receiver) is not a row of the register below. Every
// OTHER component carries a RuntimeMesh of its own (text, skinned, instanced), and the Details widgets carry
// a `Context::RuntimeMesh` view, so those are rows - named, one per site, with what the receiver is.
// A new site fails with its file and line; a row whose site is gone fails too, so the register cannot rot.
//
// Read as text because the writers live in the Editor, which no suite links.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( std::filesystem::exists( prefix / "Desert/Common/Source/Common/Core/Constants.hpp" ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    struct Row
    {
        const char* File;     // repo-relative
        const char* Receiver; // the expression before `.RuntimeMesh`
        const char* What;     // what that receiver is - why this site is not a second writer
    };

    // The register. Rows are (file, receiver), not counts: see the header.
    const std::vector<Row> kRuntimeMeshWriters = {
         { "Desert/Desert/Source/Engine/ECS/EditableMesh.cpp", "component",
           "THE writer: StaticMeshComponent, derived from EditableMesh in SetEditableMesh / ClearEditableMesh" },
         { "Desert/Desert/Source/Engine/ECS/System/TextECSSystem.hpp", "text",
           "TextComponent's own glyph-quad mesh" },
         { "Editor/Source/Editor/Core/Rigging/RigBuilder.cpp", "out",
           "SkinnedMeshComponent built by Convert to Skinned" },
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditorRegistrations.cpp", "c",
           "InstancedStaticMeshComponent: dropping its primitive cache when the shape or mesh changes" },
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/SkinnedMeshComponentWidget.cpp", "ctx",
           "MeshDetailsWidget::Context, a per-call view, not a component" },
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/StaticMeshComponent.cpp", "ctx",
           "MeshDetailsWidget::Context, a per-call view, not a component" },
    };

    std::string StripLineComments( const std::string& line )
    {
        const size_t at = line.find( "//" );
        return at == std::string::npos ? line : line.substr( 0, at );
    }
} // namespace

TEST( EditMeshSaved, OnlyTheOneWriterAssignsAStaticMeshsGeometry )
{
    const std::filesystem::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not find the repository root from the working directory";

    const std::regex write(
         R"(([A-Za-z_][A-Za-z_0-9]*)\s*(?:\.|->)\s*(RuntimeMesh|EditableMesh)\s*(?:=[^=]|\.reset\s*\())" );

    std::set<std::pair<std::string, std::string>> seen;
    std::vector<std::string>                      violations;
    size_t                                        files = 0;

    for ( const char* base : { "Desert/Desert/Source", "Editor/Source" } )
    {
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root / base ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            ++files;
            const std::string relative = std::filesystem::relative( entry.path(), root ).generic_string();

            std::ifstream in( entry.path() );
            std::string   line;
            int           number = 0;
            while ( std::getline( in, line ) )
            {
                ++number;
                // The regex only runs on the few lines that name either field: std::regex over every line
                // of the tree took 466 s in Debug, which is a census nobody would keep running.
                if ( line.find( "RuntimeMesh" ) == std::string::npos &&
                     line.find( "EditableMesh" ) == std::string::npos )
                    continue;
                const std::string code = StripLineComments( line );
                for ( std::sregex_iterator it( code.begin(), code.end(), write ), end; it != end; ++it )
                {
                    const std::string receiver = ( *it )[1].str();
                    const std::string field    = ( *it )[2].str();
                    const std::string where =
                         relative + ":" + std::to_string( number ) + "  " + receiver + "." + field;

                    if ( field == "EditableMesh" )
                    {
                        if ( relative != kRuntimeMeshWriters.front().File )
                            violations.push_back( where +
                                                  "  - only ECS::SetEditableMesh / ClearEditableMesh may" );
                        continue;
                    }
                    bool registered = false;
                    for ( const Row& row : kRuntimeMeshWriters )
                        if ( relative == row.File && receiver == row.Receiver )
                            registered = true;
                    if ( registered )
                        seen.insert( { relative, receiver } );
                    else
                        violations.push_back( where +
                                              "  - not a register row; a static mesh's render mesh is written by "
                                              "ECS::SetEditableMesh only" );
                }
            }
        }
    }

    EXPECT_GT( files, 500u ) << "the walk found almost nothing - is it reading the right tree?";
    for ( const std::string& v : violations )
        ADD_FAILURE() << v;
    for ( const Row& row : kRuntimeMeshWriters )
        EXPECT_TRUE( seen.count( { row.File, row.Receiver } ) )
             << "register row " << row.File << " / " << row.Receiver
             << " names no write any more - delete the row";
}
