// Desert Runtime — the standalone PLAYER. Runs a project's scene in Play mode, no editor UI.
//
// PACKAGED (UE-style, zero-config): double-click the exe, or run it with no arguments at all. It mounts
// the archive named after itself next to it — MyGame.exe -> MyGame.dpak (else Content.dpak) — and opens
// the project descriptor Project::kPackagedDescriptorName from the archive root. The scene is the
// project's DefaultScene. A package is a folder of just: the binary + one .dpak.
//
// WHY THIS IS THE ONLY WAY A SHIPPED GAME STARTS (П5, decided 2026-09-08; supersedes the defect note
// that stood here). Until this change NOTHING produced that descriptor: PackageGame() wrote
// `<Name>.deproj` LOOSE beside the archive and generated a launcher script carrying
// `--project <Name>.deproj`, so the discovery below had never once selected a real game and a player
// who ran the binary directly got "No game to run". Two conventions were available — teach the packager
// to emit the descriptor, or delete the discovery and require the flag — and the first was chosen for a
// reason that is about the product rather than about the code: A SHIPPED GAME MUST NOT REQUIRE A
// COMMAND LINE. A launcher script is an environment fixture (on macOS it sets VK_ICD_FILENAMES and
// DYLD_* before the image loads, which nothing inside this process can do); it is not allowed to be the
// only thing that knows what game this is. So the descriptor now travels INSIDE the archive, under one
// name shared as a symbol by the packager and this file, and the generated launchers pass no project at
// all. There is no second path: `--project` cannot be produced by packaging.
//
// DEV: pass --project <path/to/.deproj> [--scene <path/to/.desce>] to run a LOOSE ON-DISK project; this
// is the editor's and the developer's door, never a shipped one, and it overrides the discovery above.
// Launch via scripts/MacOS/RunRuntime.sh.

#include <Engine/Desert.hpp>
#include <Engine/EntryPoint.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <Common/Utilities/VFS.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Version.hpp>

#include <filesystem>

#include "PackagedContent.hpp"
#include "RuntimeLayer.hpp"
#include "RuntimeShot.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Desert::Player
{
    static std::string s_SceneOverride;

    class RuntimeApp : public Engine::Application
    {
    public:
        explicit RuntimeApp( const Engine::ApplicationInfo& appinfo ) : Engine::Application( appinfo )
        {
        }

        void OnCreate() override
        {
            PushLayer( std::make_unique<RuntimeLayer>( s_SceneOverride, this ) );
        }

        void OnDestroy() override
        {
        }
    };
} // namespace Desert::Player

// Startup diagnostics go through the LOGGER, not stderr, and that is not a style choice. A packaged
// game is double-clicked from Finder or Explorer with no terminal attached, so anything written to
// stderr is discarded by the window server before a human could see it. Common::Logger::LogInit()
// has already run (EntryPoint's main calls it before CreateApplication) and it writes the same text
// to engine_log.txt — which is the only artefact a player can actually send to support. The console
// still gets it when there is one.
//
// The multi-line refusals are printed as ONE log call on purpose: spdlog interleaves per-call, and a
// message split into six calls is six lines a background thread can cut in half.
[[noreturn]] static void FailStartup( const std::string& message, int exitCode )
{
    LOG_ERROR( "{}", message );
    std::exit( exitCode );
}

std::unique_ptr<Desert::Engine::Application> CreateApplication( int argc, char** argv )
{
    using namespace Desert::Engine;
    namespace fs = std::filesystem;

    std::string projectArg;
    for ( int i = 1; i + 1 < argc; ++i )
    {
        if ( std::strcmp( argv[i], "--project" ) == 0 )
            projectArg = argv[++i];
        else if ( std::strcmp( argv[i], "--scene" ) == 0 )
            Desert::Player::s_SceneOverride = argv[++i];
    }

    // THE ONLY WAY TO PHOTOGRAPH THE PROCESS A PLAYER STARTS. Parsed from a vector rather than from
    // argv in this loop because the parse is then a pure function with a test of its own; a refusal is
    // FATAL rather than a warning, since an unattended capture that silently did not happen leaves a
    // windowed game running with nobody watching it. See RuntimeShot.hpp.
    const std::vector<std::string> shotArgs( argv + ( argc > 0 ? 1 : 0 ), argv + argc );
    if ( const auto parsed = Desert::Player::ParseRuntimeShot( shotArgs, Desert::Player::RuntimeShot::Get() );
         !parsed )
    {
        FailStartup( parsed.GetError(), 2 );
    }

    // DEV: an explicit --project opens the loose on-disk descriptor (overrides packaged discovery).
    if ( !projectArg.empty() && !Desert::Project::ProjectContext::Open( projectArg ) )
    {
        FailStartup( fmt::format( "Could not open the project '{}' — the .deproj file is missing or could not "
                                  "be parsed.",
                                  projectArg ),
                     1 );
    }

    // Content directory: the project's folder (dev) or the executable's own folder (packaged).
    const fs::path exePath = Common::Utils::FileSystem::ExecutablePath();
    const fs::path baseDir = Desert::Project::ProjectContext::HasProject()
                                  ? fs::path( Desert::Project::ProjectContext::Directory() )
                                  : ( exePath.empty() ? fs::current_path() : exePath.parent_path() );

    // Mount the base archive (skipped in dev if there is none — reads stay plain disk reads), then any
    // Patch*.dpak ON TOP in name order (later overrides earlier), so shipping a fix = dropping one pak.
    //
    // THE TWO FAILURES ARE DIFFERENT EVENTS AND BOTH STOP THE GAME. Neither result was looked at
    // before, so a damaged archive of either kind mounted "successfully" and the player went on
    // playing with nothing to see anywhere.
    //
    //   Base archive — the game has no content at all. There is nothing to argue about.
    //
    //   Patch — the game COULD run, on exactly the content the update was published to replace. That
    //   is the worse of the two, because it looks like success: the player installed the fix, the game
    //   started, and the bug they updated for is still there. Continuing would also be actively wrong
    //   once a patch carries a list of DELETIONS (the content manifest, П3) — the deletions would not
    //   be applied, and the game would keep serving files the manifest says must be gone. So a patch
    //   that will not mount is a refusal, not a warning, and the message hands the player back the
    //   choice the engine must not make silently: fetch the update again, or delete it and knowingly
    //   play the old version.
    //
    // The decision itself lives in PackagedContent.cpp, where a test can drive it with a real damaged
    // archive; this site owns only the policy — print, and exit with a code that says which of the two
    // it was.
    const auto content = Desert::Player::MountPackagedContent( baseDir, exePath.stem().string() );
    if ( content.ExitCode != Desert::Player::kContentOk )
        FailStartup( content.Message, content.ExitCode );

    // PACKAGED: the descriptor lives at the archive root under the one name the packager writes it as —
    // open it through the now-mounted VFS. The result is checked here rather than inferred from
    // HasProject() below, because the two failures need opposite advice: a descriptor that is ABSENT
    // means the archive is not a game package, while one that is present and unparseable means the
    // package is damaged — and the old message told everybody to go and add the file that was already
    // there.
    if ( !Desert::Project::ProjectContext::HasProject() )
    {
        const fs::path descriptor = baseDir / Desert::Project::kPackagedDescriptorName;
        if ( !Desert::Project::ProjectContext::Open( descriptor.string() ) &&
             Common::Utils::FileSystem::Exists( descriptor ) )
        {
            FailStartup( fmt::format( "This game's descriptor could not be read, so the game cannot "
                                      "start.\n\n  What went wrong: {} is present but could not be "
                                      "parsed.\n\n  What to do: reinstall the game, or use your store's "
                                      "\"verify/repair files\" option.",
                                      descriptor.string() ),
                         Desert::Player::kContentBaseArchiveFailed );
        }
    }

    if ( !Desert::Project::ProjectContext::HasProject() )
    {
        FailStartup( fmt::format( "No game to run.\n"
                                  "  Packaged: put '{}.dpak' (or 'Content.dpak') containing a '{}' "
                                  "next to the executable.\n"
                                  "  Dev:      pass --project <path/to/.deproj> [--scene <path/to/.desce>].",
                                  exePath.stem().string(), Desert::Project::kPackagedDescriptorName ),
                     1 );
    }

    // Through the logger, so a support ticket's engine_log.txt says which BUILD and which content set
    // the player was actually running — the three facts every "it does not work" report is missing.
    LOG_INFO( "Desert Runtime {} — {} (base archive: {}, {} update(s) mounted)", Common::Version::Full(),
              Desert::Project::ProjectContext::Current().Name,
              content.BasePak.empty() ? std::string( "none — loose files" ) : content.BasePak.string(),
              content.Patches.size() );

    // WHAT THIS MACHINE CAN AFFORD, from this player's own directory — the same schema the editor reads
    // from `~/.desertengine/machine.json`, in the place a packaged game's per-user state belongs (К3).
    // Before the application exists, because SceneRenderer::Init bakes the MSAA sample count into every
    // pipeline it creates and a later load would apply one launch behind.
    //
    // Per PRODUCT, not per install: two games on one machine are two different budgets. An absent file is
    // the ordinary first-run state and leaves the schema defaults standing, which is exactly the picture
    // this game rendered before it had a dial at all.
    Common::Settings::MachineSettings::Load(
         Common::Settings::GameUserDirectory( Desert::Project::ProjectContext::Current().Name ) / "machine.json" );

    ApplicationInfo appInfo;
    appInfo.Title = Desert::Project::ProjectContext::Current().Name;
    appInfo.VSync = true; // a game default: tear-free presentation
    // Width/Height left as std::nullopt -> fullscreen at the monitor's native resolution.

    return std::make_unique<Desert::Player::RuntimeApp>( appInfo );
}
