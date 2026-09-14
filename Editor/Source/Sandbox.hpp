#pragma once

#include <Engine/Desert.hpp>
#include <Engine/EntryPoint.hpp>

#include <Editor/Core/CommandLine.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Editor/Core/ProjectContext.hpp>
#include <Engine/Project/EngineRegistration.hpp>
#include <Editor/Core/ShotOptions.hpp>
#include <Editor/Core/Control/ControlChannelOptions.hpp>

#include <Common/Core/Profiler.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Desert
{
    class Sandbox : public Engine::Application
    {
    public:
        Sandbox( const Engine::ApplicationInfo& appinfo );

        virtual void OnCreate() override;
        virtual void OnDestroy() override;
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

    const Desert::Editor::CommandLineOptions options = parsed.ExtractValue();

    // IS ANYBODY SITTING AT THIS EDITOR? Asked ONCE, and every consequence below reads this rather than
    // guessing from a flag. It used to be spelled `options.Shot.Active()` in three places whose own
    // comments were all about unattended runs and none about capture — so a control-channel run, which is
    // the unattended path that no longer needs `--shot` at all, escaped every one of them and filed a
    // throwaway worktree in the developer's registries. See CommandLine.hpp::IsUnattendedSession.
    const bool unattended = Desert::Editor::IsUnattendedSession( options.Shot, !options.ControlSocket.empty() );

    // The editor is PROJECT-DRIVEN: `--project <path/to/.deproj>` is REQUIRED. Picking/creating projects
    // is the launcher's job (the desert-launcher repository) — the editor itself
    // never shows a chooser. Opening the project also remaps every engine content path into the project
    // folder, so it must happen BEFORE anything engine-side spins up.
    if ( !options.Project.empty() )
    {
        // An UNATTENDED run stays OUT of the recent-projects registry. Those runs happen in agent
        // worktrees that are reclaimed within the hour, and each one used to file itself at the top
        // of the developer's list — which is why the live registry on this machine is mostly dead
        // paths, and why the launcher needs an "unopenable entry" state at all.
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
        const char* engineRoot = std::getenv( "DESERT_ROOT" );
        if ( const auto registered = Desert::Project::RegisterThisEngine(
                  Desert::Editor::ProjectContext::ConfigDirectory(), engineRoot ? engineRoot : "" );
             !registered.IsSuccess() )
            // stderr, not a log line: the consequence is that the LAUNCHER will not list this
            // engine, and the person who needs to know that is the one reading this terminal.
            std::fprintf( stderr, "[Engine] %s\n", registered.GetError().c_str() );
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

    if ( !Desert::Editor::ProjectContext::HasProject() )
    {
        std::fprintf( stderr, "No project given. Pass: --project <path/to/.deproj>\n"
                              "(the launcher lives in the desert-launcher repository and starts the editor\n"
                              " with exactly that flag — it is not part of this build.)\n" );
        std::exit( 1 );
    }

    ApplicationInfo appInfo;
    appInfo.Title = "Desert Engine — " + Desert::Editor::ProjectContext::Current().Name;
    appInfo.VSync = false;
    // THE EDITOR DRAWS ITS OWN TITLE BAR. Its menu bar has carried the project name, the open level, the
    // menus and the engine stats for a long time while the system bar sat above it — two title bars on one
    // window, which is the state У9 photographed before touching anything. What the OS frame also carried
    // (move, minimize/maximize/close, double-click to toggle) is drawn and handled by
    // Editor::UI::WindowChrome; what it cannot give back is listed there.
    appInfo.Decorated = false;
    // Width/Height left as std::nullopt -> start fullscreen at the monitor's native resolution.

    return std::make_unique<Desert::Sandbox>( appInfo );
}
