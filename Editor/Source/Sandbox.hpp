#pragma once

#include <Engine/Desert.hpp>
#include <Engine/EntryPoint.hpp>

#include <Editor/Core/CommandLine.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <Engine/Graphic/PipelineCacheFile.hpp>
#include <Editor/Core/StartupRefusal.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Editor/Core/ProjectContext.hpp>
#include <Engine/Project/EngineRegistration.hpp>
#include <Editor/Core/ShotOptions.hpp>
#include <Editor/Core/Control/ControlChannelOptions.hpp>
#include <Engine/Project/StartupLayout.hpp>
#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashScreen.hpp>
#include <Common/Core/Version.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace Desert
{
    class Sandbox : public Engine::Application
    {
    public:
        Sandbox( const Engine::ApplicationInfo& appinfo, std::unique_ptr<Editor::Splash::SplashScreen> splash );

        virtual void OnCreate() override;
        virtual void OnDestroy() override;

    private:
        // Held from before the renderer exists until the editor layer takes it over in OnCreate.
        std::unique_ptr<Editor::Splash::SplashScreen> m_Splash;
    };
} // namespace Desert

std::unique_ptr<Desert::Engine::Application> CreateApplication( int argc, char** argv )
{
    using namespace Desert::Engine;

    // THE WHOLE COMMAND LINE, IN ONE PASS, AND NOTHING FALLS OFF THE END OF IT.
    //
    // This used to be two loops of if/else here — one for `--project`, one for everything else — and both
    // shared a defect the chain's shape makes almost invisible: a token that matched no branch was simply
    // DROPPED. `--sceen` (one transposed letter) parsed to nothing, so the editor loaded the project's
    // default scene, rendered it, wrote a plausible PNG under the name the caller had chosen and exited 0.
    // Measured on the binary before this change: the log read "Loading scene: Desert Sandbox" and the flag
    // was never mentioned. The same silence swallowed a value-taking flag written last, a `--camera` with
    // two components instead of three, and a `--shot-frames` that was not a number.
    //
    // The parse is now a PURE function (Editor/Core/CommandLine.hpp) so the decision can be asserted by a
    // test instead of discovered by launching the editor and looking at what came out. What is left here
    // is only what genuinely needs the process: opening the project, and publishing the result into the
    // two singletons the rest of the editor reads.
    std::vector<std::string> args;
    args.reserve( static_cast<std::size_t>( argc > 1 ? argc - 1 : 0 ) );
    for ( int i = 1; i < argc; ++i )
        args.emplace_back( argv[i] );

    auto parsed = Desert::Editor::ParseCommandLine( args );
    if ( !parsed.IsSuccess() )
    {
        // stderr and a non-zero status, before any engine subsystem exists. There is no logger yet and no
        // frame to spoil, and a caller that reads the exit code learns the truth on the first byte.
        Desert::Editor::RefuseToStart( 2, parsed.GetError() );
    }

    // NOT const: the project is RESOLVED below -- made absolute against where the caller stood, and
    // filled in from the descriptor beside the executable when the caller named none.
    // The parse itself stays pure; this is the one place its result is completed from the disk.
    Desert::Editor::CommandLineOptions options = parsed.ExtractValue();

    // IS ANYBODY SITTING AT THIS EDITOR? Asked ONCE, and every consequence below reads this rather than
    // guessing from a flag. It used to be spelled `options.Shot.Active()` in three places whose own
    // comments were all about unattended runs and none about capture — so a control-channel run, which is
    // the unattended path that no longer needs `--shot` at all, escaped every one of them and filed a
    // throwaway worktree in the developer's registries. See CommandLine.hpp::IsUnattendedSession.
    const bool unattended = Desert::Editor::IsUnattendedSession( options.Shot, !options.ControlSocket.empty() );

    // A FLIGHT'S TWO PATHS ARE THE CALLER'S, resolved against where the caller stood: the working directory
    // moves below, and a route file or a CSV named relative to the shell would otherwise be looked for, or
    // written, inside the engine's resources. The route file is read here, before any frame, because the
    // frame count is its length — a flight that learned its route late would already have counted frames.
    if ( auto& shot = options.Shot; shot.FlightRoute.has_value() )
    {
        shot.FlightCsv = std::filesystem::absolute( shot.FlightCsv ).string();
        if ( !shot.FlightRoute->FilePath.empty() )
        {
            const auto text  = Common::Utils::FileSystem::ReadFileContent( shot.FlightRoute->FilePath );
            auto       route = text ? Desert::Editor::Flight::ParseRouteFile( *shot.FlightRoute, text.GetValue() )
                                    : Common::MakeError<Desert::Editor::Flight::Route>( text.GetError() );
            if ( !route )
            {
                Desert::Editor::RefuseToStart( 2, "--flight: " + route.GetError() );
            }
            shot.FlightRoute = route.ExtractValue();
            Desert::Editor::ArmFlight( shot );
        }
    }

    // ── WHERE THIS PROCESS IS, AND WHAT THAT ANSWERS ────────────────────────────────────────────────
    //
    // A DOWNLOADED BUILD MUST START BY BEING DOUBLE-CLICKED. It did not: the CI artifact is COMPLETE
    // — binaries, `Resources/{Shaders,Fonts,Icons}` and `Desert.deproj` in one directory — and the
    // editor still demanded `DESERT_ROOT` (naming a run script that is not in the drop) and
    // `--project` (for a descriptor lying beside the executable). Both demands were for facts
    // derivable from the one thing every process has for free: its own image path.
    //
    // The three derivations are pure functions in Engine/Project/StartupLayout.hpp so that the
    // decisions can be asserted by Desert/Tests/Engine/StartupLayout instead of discovered by
    // unzipping an artifact. What is left here is only the policy: what to do with each answer.
    const std::filesystem::path executable   = Common::Utils::FileSystem::ExecutablePath();
    const std::filesystem::path executableIn = executable.parent_path();

    bool startedInCheckout = false;

    // 1. THE ENGINE DIRECTORY, BEFORE ANYTHING READS ONE (UE: FPaths::EngineDir). Derived from the
    //    executable's own position, or named by `--engine-dir`, and handed to Common::Constants::Path,
    //    which makes every engine resource path absolute. The process does NOT change directory: it
    //    reads the same files whether it was started from Editor/, from /tmp, from an IDE or by a
    //    double-click, and every path the caller spelled relatively keeps meaning what it meant.
    {
        const Desert::Project::EngineDirLookup engine =
             Desert::Project::ResolveEngineDir( executableIn, options.EngineDir );
        if ( !engine.Explanation.empty() )
        {
            // A REFUSAL, not a half-start. An editor that opens a window it cannot draw into costs
            // whoever downloaded it an afternoon of looking at the wrong thing.
            Desert::Editor::RefuseToStart( 1, "[Engine] " + engine.Explanation );
        }
        Common::Constants::Path::SetEngineDir( engine.Dir );
        startedInCheckout = engine.FromCheckout;
        // The log lives in the engine directory wherever the process was started (a no-op for the
        // run scripts, which start in it) — the one place a developer looks for engine_log.txt.
        Common::Logger::RelocateLogFile( engine.Dir );

        // A binary started where it was built: an IDE passes the run scripts' `--project Desert.deproj`
        // but starts in the solution root, where no such file is. The name then means the one in the
        // checkout's Editor/, which is where the scripts pass it from.
        if ( startedInCheckout && !options.Project.empty() &&
             std::filesystem::path( options.Project ).is_relative() )
        {
            std::error_code existsError;
            if ( !std::filesystem::exists( options.Project, existsError ) )
                options.Project = ( engine.Dir / options.Project ).string();
        }
        // Absolute once, here: every project-derived path (Constants::Path::SetProjectRoot) hangs off
        // this folder, and none of them may depend on the working directory afterwards.
        if ( !options.Project.empty() )
        {
            std::error_code absError;
            if ( const auto resolved = std::filesystem::absolute( options.Project, absError ); !absError )
                options.Project = resolved.lexically_normal().string();
        }
    }

    // 2. THE PROJECT. The editor is PROJECT-DRIVEN and never shows a chooser — picking and creating
    //    projects is the launcher's job (the desert-launcher repository). What it no longer does is
    //    DEMAND the flag for a descriptor it is standing next to: with no `--project`, the single
    //    `.deproj` beside the executable is opened, and none-or-several is a refusal that names what
    //    it found. A development build lands in the "none" branch by construction —
    //    `build/Bin/<Config>/` holds no descriptor — and its run scripts pass the flag as they always
    //    did.
    if ( options.Project.empty() )
    {
        auto beside = Desert::Project::ProjectBesideExecutable( executableIn );
        // A binary started where it was built (an IDE's F5): the development project lives in its
        // checkout's Editor/ (the engine directory) - the same one the run scripts pass.
        if ( !beside.IsSuccess() && startedInCheckout )
        {
            if ( auto inEditor = Desert::Project::ProjectBesideExecutable( Common::Constants::Path::EngineDir() );
                 inEditor.IsSuccess() )
                beside = std::move( inEditor );
        }
        if ( !beside.IsSuccess() )
        {
            Desert::Editor::RefuseToStart( 1, beside.GetError() );
        }
        options.Project = beside.ExtractValue();
    }

    // Opening the project remaps every engine content path into the project folder, so it must
    // happen BEFORE anything engine-side spins up.
    //
    // An UNATTENDED run stays OUT of the recent-projects registry. Those runs happen in agent
    // worktrees that are reclaimed within the hour, and each one used to file itself at the top
    // of the developer's list — which is why the live registry on this machine is mostly dead
    // paths, and why the launcher needs an "unopenable entry" state at all.
    {
        const auto record = unattended ? Desert::Editor::ProjectContext::RecordInRecent::No
                                       : Desert::Editor::ProjectContext::RecordInRecent::Yes;
        if ( !Desert::Editor::ProjectContext::Open( options.Project, record ) )
        {
            Desert::Editor::RefuseToStart( 1, "Could not open project '" + options.Project +
                                                   "' (missing or corrupt .deproj)." );
        }
    }

    // THE CRASH HANDLER, INSTALLED THE MOMENT THE REPORT CAN BE FILED SOMEWHERE USEFUL — after the
    // project is open (so reports land in <project>/Saved/Crashes and travel with the project) and
    // before a single engine subsystem exists. Earlier than this and every editor report would go to
    // %LOCALAPPDATA% with no project named in it; later and the whole of startup — the riskiest code
    // in the process, because it touches the driver first — would fault with nothing written.
    {
        Common::Crash::InstallOptions crashOptions;
        crashOptions.hostName    = "Editor";
        crashOptions.projectRoot = Desert::Project::ProjectContext::Directory();
        if ( const Common::BoolResultStr installed = Common::Crash::Install( crashOptions );
             !installed.IsSuccess() )
        {
            // Refused rather than carried on: a run whose crashes leave nothing behind is exactly
            // the run this task exists to end, and saying so at startup costs one line.
            Desert::Editor::RefuseToStart( 1, "Crash handler: " + installed.GetError() );
        }
    }

    // `--crash-test` is acted on HERE, one statement after the handler is installed, so that what it
    // proves is the handler and not some later subsystem's idea of a fault.
    if ( options.CrashTest.has_value() )
    {
        Common::Crash::TriggerTestCrash( *options.CrashTest );
    }

    // Where this engine is, written down for the launcher — which after L3 has no DESERT_ROOT of
    // its own and no other way to find an engine. Skipped for the same runs the recent list skips:
    // an unattended run inside a worktree would otherwise register that worktree as an installed
    // engine, and reclaiming it a day later would leave the launcher offering to start something
    // that is gone.
    if ( !unattended )
    {
        // THE ENVIRONMENT VARIABLE IS SENIOR, AND IT STAYS SENIOR. It is not a workaround being
        // retired — it is the one way to say "register THAT checkout, not the one I happen to have
        // launched", and an explicit statement by the operator must beat an inference. What it
        // stops being is REQUIRED: with nothing set, the tree is derived from where this executable
        // is, which is how `build/Bin/<Config>/Editor` started directly now registers correctly too.
        const char*                             fromEnvironment = std::getenv( "DESERT_ROOT" );
        const Desert::Project::EngineRootLookup derived         = Desert::Project::DeriveEngineRoot( executable );
        const std::string                       engineRoot =
             fromEnvironment && fromEnvironment[0] ? std::string( fromEnvironment ) : derived.Root;

        if ( engineRoot.empty() )
        {
            // NOT A FAILURE, AND THE WORDING NOW SAYS SO. This is the ordinary state of a
            // downloaded build, and the message it replaced told the reader to start the editor
            // through a run script the drop does not contain — an instruction that cannot be
            // followed, printed by a program that had just started perfectly well.
            std::fprintf( stderr, "[Engine] %s\n", derived.Explanation.c_str() );
        }
        else if ( const auto registered = Desert::Project::RegisterThisEngine(
                       Desert::Editor::ProjectContext::ConfigDirectory(), engineRoot );
                  !registered.IsSuccess() )
        {
            // stderr, not a log line: the consequence is that the LAUNCHER will not list this
            // engine, and the person who needs to know that is the one reading this terminal.
            std::fprintf( stderr, "[Engine] %s\n", registered.GetError().c_str() );
        }
    }

    // Published before the renderer exists: the flags have to be in force for the very first frame, or a
    // measurement would include a few frames of the other configuration.
    Desert::Editor::ShotOptions::Get()                               = options.Shot;
    Desert::Editor::Control::ControlChannelOptions::Get().SocketPath = options.ControlSocket;
    Desert::Graphic::SetViewBudgetOverrideMiB( options.ViewBudgetMiB );

    // The language, before the first frame for the same reason: a run that renders two frames of two
    // languages is not a capture of either. An empty value leaves the process in its source language,
    // which is the state every run had before this flag existed. The tag has already been checked against
    // the compiled locale table by the parse, so this cannot fail for a reason the caller has not been
    // told about — but it is still unwrapped, because a refusal nobody reads is the shape this whole
    // subsystem is built to avoid.
    if ( !options.Language.empty() )
    {
        if ( const auto set = Desert::Localization::Localization::Get().SetLanguage( options.Language ); !set )
        {
            Desert::Editor::RefuseToStart( 2, set.GetError() );
        }
    }

    // GPU timing is OFF unless --gpu-profile asks for it. A run that did not ask to be measured is not
    // measured, and its frame time is the one a budget decision should be taken on.
    Common::Profiling::Profiler::Get().GpuEnabled()    = options.Shot.GpuProfile && options.Shot.GpuTiming;
    Common::Profiling::Profiler::Get().GpuPassScopes() = !options.Shot.GpuFrameOnly;

    // THE SPLASH, AS EARLY AS IT CAN SAY SOMETHING TRUE. Not earlier: the project it names and the cooked
    // picture's path both exist only once the project is open, three blocks up, and a refusal above this
    // line must not flash a window on its way to exiting. Not later: everything below — the window, the
    // Vulkan instance and device, the renderer, the shader preload in OnAttach — is the wait it covers.
    auto splash = Desert::Editor::Splash::SplashScreen::Show( { Desert::Editor::ProjectContext::Current().Name,
                                                                Common::Version::Base(),
                                                                Desert::Editor::Splash::kSplashTexture } );
    // The plan is not made yet — the editor layer that owns it does not exist — so the bar is empty; the
    // renderer's start is not weighed, and the first weighed stage is the shader preload.
    splash->SetProgress( { "Starting the renderer...", "", 0.0 } );

    ApplicationInfo appInfo;
    // The editor's driver pipeline cache lives in ~/.desertengine, one folder per project (PKG1). Before the
    // application: the device reads it while it is being created.
    Desert::Graphic::PipelineCacheFile::DeclareHost( Desert::Graphic::PipelineCacheFile::Host::Editor );
    appInfo.Title = "Desert Engine — " + Desert::Editor::ProjectContext::Current().Name;
    // HIDDEN UNTIL THE EDITOR IS READY: the splash is what is on screen until then, and EditorLayer shows
    // the window on its first real frame (RevealWhenReady).
    appInfo.Visible = false;
    appInfo.VSync = false;
    // THE EDITOR DRAWS ITS OWN TITLE BAR. Its menu bar has carried the project name, the open level, the
    // menus and the engine stats for a long time while the system bar sat above it — two title bars on one
    // window, which is the state У9 photographed before touching anything. What the OS frame also carried
    // (move, minimize/maximize/close, double-click to toggle) is drawn and handled by
    // Editor::UI::WindowChrome; what it cannot give back is listed there.
    appInfo.Decorated = false;
    // Width/Height left as std::nullopt -> start fullscreen at the monitor's native resolution.

    return std::make_unique<Desert::Sandbox>( appInfo, std::move( splash ) );
}
