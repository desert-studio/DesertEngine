#pragma once

#include <Engine/Desert.hpp>
#include <Engine/EntryPoint.hpp>

#include <Editor/Core/CommandLine.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Editor/Core/ProjectContext.hpp>
#include <Engine/Project/EngineRegistration.hpp>
#include <Editor/Core/ShotOptions.hpp>
#include <Editor/Core/Control/ControlChannelOptions.hpp>
#include <Engine/Project/StartupLayout.hpp>
#include <Editor/Splash/SplashImage.hpp>
#include <Editor/Splash/SplashScreen.hpp>
#include <Engine/Assets/CookedTexturePath.hpp>
#include <Common/Core/Version.hpp>

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
        std::fprintf( stderr, "%s\n", parsed.GetError().c_str() );
        std::exit( 2 );
    }

    // NOT const: the project is RESOLVED below -- made absolute before the working directory can
    // move, and filled in from the descriptor beside the executable when the caller named none.
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
                std::fprintf( stderr, "--flight: %s\n", route.GetError().c_str() );
                std::exit( 2 );
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

    // 1. THE ENGINE RESOURCES, BEFORE ANYTHING READS ONE. Every engine resource is a path relative
    //    to the WORKING DIRECTORY (Common::Constants::Path), so this either leaves the working
    //    directory alone — which is what every `scripts/*/RunEditor.*` launch gets, because it has
    //    already changed into `Editor/` — or moves to the executable's own folder, which is the drop.
    {
        std::error_code                           cwdError;
        const std::filesystem::path               here = std::filesystem::current_path( cwdError );
        const Desert::Project::ResourceRootLookup resources =
             Desert::Project::ResolveResourceRoot( cwdError ? std::filesystem::path{} : here, executableIn );
        if ( !resources.Explanation.empty() )
        {
            // A REFUSAL, not a half-start. An editor that opens a window it cannot draw into costs
            // whoever downloaded it an afternoon of looking at the wrong thing.
            std::fprintf( stderr, "[Engine] %s\n", resources.Explanation.c_str() );
            std::exit( 1 );
        }
        if ( !resources.WorkingDirectory.empty() )
        {
            // Absolute FIRST. The caller's `--project` (and anything else spelled relatively) was
            // written against the directory this process started in, and moving out from under it
            // would silently reinterpret those paths against a different folder.
            if ( !options.Project.empty() )
            {
                std::error_code             absError;
                const std::filesystem::path resolved = std::filesystem::absolute( options.Project, absError );
                if ( !absError )
                    options.Project = resolved.string();
            }
            std::error_code moveError;
            std::filesystem::current_path( resources.WorkingDirectory, moveError );
            if ( moveError )
            {
                std::fprintf( stderr,
                              "[Engine] the engine resources are in '%s' but this process could "
                              "not work from there: %s\n",
                              resources.WorkingDirectory.c_str(), moveError.message().c_str() );
                std::exit( 1 );
            }
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
        if ( !beside.IsSuccess() )
        {
            std::fprintf( stderr, "%s\n", beside.GetError().c_str() );
            std::exit( 1 );
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
            std::fprintf( stderr, "Could not open project '%s' (missing or corrupt .deproj).\n",
                          options.Project.c_str() );
            std::exit( 1 );
        }
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
            std::fprintf( stderr, "%s\n", set.GetError().c_str() );
            std::exit( 2 );
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
