// AssetClosure — what a build has to ship for one scene to open.
//
//   AssetClosure <project.deproj> [--scene <project-relative .desce>] [--out <list.txt>]
//
// Prints, one per line, the ASSETS-RELATIVE path of every file reachable from the scene through the
// asset reference graph, the scene itself included. With no --scene it uses the project's
// DefaultScene.
//
// WHY THIS EXISTS. `scripts/*/Package.*` used to copy `Editor/Resources` whole — 133 MB, of which
// 129 MB is the sandbox project's authored content: 50 MB of baked cloud volumes, 38 MB of meshes and
// a 37 MB `.gif` that nothing in the repository names. The engine drop is the editor plus the tools
// plus a project to open; it does not need the cloud research corpus. The obvious fix is a list of
// what to copy, and a list is exactly what must not be written down: content moves every week, and a
// list that has drifted fails in the direction where the package still builds, still starts, and
// shows an empty world. So the packagers ask the graph instead.
//
// It is a TOOL and not a mode of the editor because packaging happens in CI, on a machine with no
// display and no Vulkan loader — the editor cannot boot there, and the scan does not need it to. The
// token rule it walks is the editor's own (Editor/Source/Editor/Core/AssetReferencesScan.cpp),
// compiled into this binary rather than restated in it, so "find references" in the editor and "what
// gets shipped" cannot disagree.
//
// Over-approximation is the safe direction and this tool takes it: a token that happens to appear in
// an unrelated file ships one file too many. The dangerous direction — a reference no text scan can
// see — is pinned for the trees this repository actually ships by Desert/Tests/Tools/AssetClosure.

#include <Editor/Core/AssetReferences.hpp>

#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf( stderr, "Usage:\n"
                              "  AssetClosure <project.deproj> [--scene <project-relative .desce>]\n"
                              "               [--out <list.txt>]\n"
                              "\n"
                              "Prints the assets-relative path of every file the scene needs, one per\n"
                              "line, the scene included. Exit 0 only when a non-empty closure was\n"
                              "produced.\n" );
        return 2;
    }

    int Fail( const std::string& what )
    {
        std::fprintf( stderr, "AssetClosure: %s\n", what.c_str() );
        return 1;
    }
} // namespace

int main( int argc, char** argv )
{
    if ( argc < 2 )
        return Usage();

    const fs::path projectPath = fs::path( argv[1] ).lexically_normal();
    std::string    sceneArg;
    std::string    outPath;

    for ( int i = 2; i < argc; ++i )
    {
        if ( std::strcmp( argv[i], "--scene" ) == 0 && i + 1 < argc )
            sceneArg = argv[++i];
        else if ( std::strcmp( argv[i], "--out" ) == 0 && i + 1 < argc )
            outPath = argv[++i];
        else
            return Usage();
    }

    const auto json = Common::Utils::FileSystem::ReadFileContent( projectPath.string() );
    if ( !json )
        return Fail( "cannot read " + projectPath.string() + ": " + json.GetError() );

    const auto project = Common::Project::ReadProjectFile( json.GetValue() );
    if ( !project )
        return Fail( projectPath.string() + " is not a readable project: " + project.GetError() );

    const fs::path projectDir = projectPath.parent_path();
    const fs::path assetsRoot = ( projectDir / project.GetValue().AssetsRoot ).lexically_normal();
    if ( !fs::is_directory( assetsRoot ) )
        return Fail( "AssetsRoot \"" + project.GetValue().AssetsRoot + "\" resolves to " + assetsRoot.string() +
                     ", which is not a directory" );

    // The scene is named RELATIVE TO THE PROJECT in the descriptor and indexed RELATIVE TO THE ASSETS
    // ROOT by the scan; converting between the two here rather than assuming they coincide is the
    // whole reason AssetsRoot is a field and not a constant.
    std::string sceneProjectRel = sceneArg.empty() ? project.GetValue().DefaultScene : sceneArg;
    if ( sceneProjectRel.empty() )
        return Fail( projectPath.string() +
                     " declares no DefaultScene and no --scene was given; there is nothing to close over" );

    std::error_code   ec;
    const fs::path    scenePath = ( projectDir / sceneProjectRel ).lexically_normal();
    const std::string sceneKey  = fs::relative( scenePath, assetsRoot, ec ).generic_string();
    if ( ec || sceneKey.empty() || sceneKey.rfind( "..", 0 ) == 0 )
        return Fail( "scene \"" + sceneProjectRel + "\" resolves to " + scenePath.string() +
                     ", which is outside the assets root " + assetsRoot.string() );
    if ( !fs::exists( scenePath ) )
        return Fail( "scene " + scenePath.string() + " does not exist" );

    Desert::Editor::AssetReferenceIndex index;
    Desert::Editor::BuildAssetReferenceIndex( index, assetsRoot, projectDir );
    if ( index.Entries().empty() )
        return Fail( "the scan of " + assetsRoot.string() + " produced no entries at all" );

    const std::vector<std::string> closure = index.ClosureFrom( sceneKey );
    if ( closure.empty() )
    {
        // The scan walked the tree and the scene was not in it. An empty list here would be read by a
        // packaging script as "this scene needs nothing", which is the failure that ships a package
        // that starts and shows an empty world.
        return Fail( "the scene " + sceneKey + " is not in the index built from " + assetsRoot.string() );
    }

    if ( outPath.empty() )
    {
        for ( const auto& p : closure )
            std::printf( "%s\n", p.c_str() );
    }
    else
    {
        std::ofstream out( outPath, std::ios::binary | std::ios::trunc );
        if ( !out )
            return Fail( "cannot write " + outPath );
        for ( const auto& p : closure )
            out << p << "\n";
        if ( !out )
            return Fail( "writing " + outPath + " failed part-way through" );
    }

    std::fprintf( stderr, "AssetClosure: %zu files reachable from %s\n", closure.size(), sceneKey.c_str() );
    return 0;
}
