// GamePackager — the artifact a PLAYER gets, produced without an editor.
//
//   GamePackager <project.deproj> [--config Shipping|Release|Debug] [--out <dir>] [--no-bundle]
//
// WHY THIS EXISTS, AND WHAT WAS WRONG WITH THE REASON IT DID NOT. Everything this repository
// publishes is the ENGINE DROP — the editor and the tools — and only in Release. Nothing produced a
// game. The reason was written down in three places (scripts/MacOS/Package.sh, scripts/Windows/
// Package.bat and .github/workflows/ci.yml) and said the same thing: "a game is packaged by the
// editor's own PackageGame(), which needs an open project that CI does not have".
//
// HALF OF THAT IS FALSE AND THE OTHER HALF IS A ONE-LINE CALL.
//
//   * "needs the editor" — it does not. `Desert::Editor::PackageGame` reaches no renderer, no
//     VkDevice, no Application and no editor singleton; its cook is shaderc, stb_truetype and a CPU
//     SDF rasterizer. The proof is not an argument, it is a suite that has been green on both
//     platforms for weeks: Desert/Tests/Editor/PackagedContent calls PackageGame six times inside a
//     gtest binary with no window. This tool's file list is that suite's, minus its test halves.
//   * "an open project CI does not have" — `Editor/Desert.deproj` is TRACKED BY GIT, and opening it
//     is `ProjectContext::Open(path)`. CI checks it out on every run.
//
// SO THIS IS NOT A SECOND PACKAGING SYSTEM. It is a second ENTRY POINT to the only one: the whole
// of the work below is `PackageGame( options )`, and the census of what a package contains stays
// where it was (Editor/Packaging/PackagedContentTrees.hpp). A packager that restated any part of
// that would be the defect П5 removed, in a new file.
//
// WHAT IT DOES THAT THE EDITOR DOES FOR ITSELF, and it is exactly one thing: loads the cooked asset
// registry. `PackageGame` compares that registry against the content roots on disk and refuses when
// they disagree (GamePackager.cpp), and in a fresh process the registry is empty — so without the
// load, a project with content is refused as if its registry were stale. The editor does this in
// EditorLayer::OnCreate; a host that does not is a host that has not finished starting.

#include <ToolMain.hpp>

#include <Editor/Packaging/GamePackager.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <Common/Core/Logger.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // EXIT CODES ARE THE ANSWER A SCRIPT READS, and there are three of them because the packager
    // answers three questions and not two (GamePackager.hpp explains the distinction at length): a
    // package exists, and everything the cook was asked to produce is in it, are different facts. A
    // build that shipped with content that will be rebuilt on the player's machine at every start is
    // not a failure to stop the world for, and it is not a green build either.
    constexpr int kOk           = 0;
    constexpr int kRefused      = 1;
    constexpr int kUsage        = 2;
    constexpr int kIncomplete   = 3;

    int Usage()
    {
        std::fprintf( stderr,
                      "Usage:\n"
                      "  GamePackager <project.deproj> [--config Shipping|Release|Debug]\n"
                      "               [--out <dir>] [--no-bundle]\n"
                      "\n"
                      "Packages the project into a self-contained game for THIS host - the player\n"
                      "binary plus one content archive - using the editor's own PackageGame(), with\n"
                      "no editor and no GPU.\n"
                      "\n"
                      "  --config     which Runtime binary to ship. Default Shipping.\n"
                      "  --out        output directory. Default 'Build/Output' beside the project.\n"
                      "  --no-bundle  macOS: produce a plain folder instead of a .app.\n"
                      "\n"
                      "Exit: 0 packaged and complete, 3 packaged with cook failures, 1 refused,\n"
                      "      2 bad command line.\n" );
        return kUsage;
    }

    int Fail( const std::string& what )
    {
        std::fprintf( stderr, "GamePackager: %s\n", what.c_str() );
        return kRefused;
    }

    bool IsKnownConfig( const std::string& config )
    {
        for ( const char* known : Desert::Editor::kPackageConfigs )
        {
            if ( config == known )
                return true;
        }
        return false;
    }

    std::string KnownConfigs()
    {
        std::string all;
        for ( const char* known : Desert::Editor::kPackageConfigs )
        {
            if ( !all.empty() )
                all += ", ";
            all += known;
        }
        return all;
    }
} // namespace

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "GamePackager", argc, argv,
                                   []( int count, char** args ) -> int
                                   {
        if ( count < 2 )
            return Usage();

        const std::string descriptor = args[1];
        if ( descriptor.rfind( "--", 0 ) == 0 )
            return Usage();

        Desert::Editor::PackageOptions options;
        std::string                    outputDir;

        // EVERY TOKEN IS RECOGNISED OR THE RUN STOPS, on the rule Editor/Core/CommandLine.hpp states
        // for the editor: an unrecognised flag that is silently dropped turns a typo into a package
        // built with the wrong configuration, and nothing says so.
        for ( int i = 2; i < count; ++i )
        {
            const std::string flag = args[i];
            const bool        last = i + 1 >= count;
            if ( flag == "--config" && !last )
            {
                options.Config = args[++i];
                if ( !IsKnownConfig( options.Config ) )
                    return Fail( "--config " + options.Config + " is not a configuration a package can be "
                                 "cut from (" + KnownConfigs() + ")" );
            }
            else if ( flag == "--out" && !last )
            {
                outputDir = args[++i];
            }
            else if ( flag == "--no-bundle" )
            {
                options.MacAppBundle = false;
            }
            else
            {
                std::fprintf( stderr, "GamePackager: unrecognised argument '%s'\n", flag.c_str() );
                return Usage();
            }
        }

        std::error_code ec;
        const fs::path  deproj = fs::absolute( descriptor, ec );
        if ( ec || !fs::is_regular_file( deproj, ec ) )
            return Fail( "'" + descriptor + "' is not a .deproj file this process can read" );

        // THE WORKING DIRECTORY IS PART OF THE PACKAGER'S INPUT, and pretending otherwise would make
        // this tool work here and refuse in CI. PackageGame looks for the Runtime binary at
        // `../build/Bin/<Config>/<name>` — RELATIVE, because the editor's own working directory is
        // the project folder. So this stands where the editor stands. (The tests do the same:
        // packaged_content_test.cpp sets current_path to the project before every call.)
        fs::current_path( deproj.parent_path(), ec );
        if ( ec )
            return Fail( "could not work from '" + deproj.parent_path().string() + "': " + ec.message() );

        Common::Logger::LogInit();

        // NOT in the recent-projects registry: a build machine opening a project is not a person
        // opening one, and every CI run would otherwise file its checkout at the top of somebody's
        // list. Same rule, same reason, as the editor's own unattended runs.
        if ( !Desert::Project::ProjectContext::Open( deproj.string(),
                                                     Desert::Project::ProjectContext::RecordInRecent::No ) )
            return Fail( "could not open '" + deproj.string() + "' (missing or corrupt .deproj)" );

        if ( outputDir.empty() )
            outputDir = ( deproj.parent_path() / "Build" / "Output" ).string();
        options.OutputDir = outputDir;

        // The one thing the editor does for itself that a bare process does not (see the header).
        const auto registry = Desert::Assets::ContentRegistry::Load();
        if ( !registry )
            return Fail( "the cooked asset registry: " + registry.GetError() );

        const Desert::Editor::PackageResult result = Desert::Editor::PackageGame( options );
        if ( !result.Success )
            return Fail( result.Message );

        std::fprintf( stdout, "GamePackager: %s\n  package:  %s\n  manifest: %s\n", result.Message.c_str(),
                      result.PackageDir.c_str(),
                      result.ManifestPath.empty() ? "(none)" : result.ManifestPath.c_str() );

        if ( !result.Complete() )
        {
            // NAMED, NOT SWALLOWED. A package whose cook lost artifacts starts and then rebuilds them
            // on the player's machine at every launch; the person who can fix that is the one reading
            // this, not the player.
            std::fprintf( stderr,
                          "GamePackager: the package exists but the cook did not complete - %zu asset(s) "
                          "could not be read/compiled and %zu produced artifact(s) did not reach the "
                          "disk. See engine_log.txt beside the project for which.\n",
                          result.CookFailures, result.CookUnwritten );
            return kIncomplete;
        }
        return kOk;
                                   } );
}
