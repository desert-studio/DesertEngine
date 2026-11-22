// DesertServer: the dedicated headless server (Docs/contracts/NET-ARCH.md §6). One binary, every role chosen by
// `--role`; NET-MESH-0 runs `--role all`, the mesh of one (§2M.2), which streams one cooked World Partition world
// headless at a fixed 60 Hz step.
//
//   DesertServer --project <file.deproj> --role all --world <scene.desce> --source <x,y,z> [--steps <n>]
//
// --world   the scene whose cooked world (WorldCells::CookedWorldDirectory, `X.dwworld/` beside it) is served;
//           relative to the project's directory. A scene with no cooked world beside it is refused.
// --source  one server-side streaming source, world cm (NET-ARCH §4.1: an NPC schedule or a script's
//           RequestStreaming; players are sources from NET-1).
// --steps   steps to run, then exit 0 (a scenario's run); without it the server runs until killed.

#if !DESERT_SERVER
#error "DesertServer is compiled only by its own target (Server/CMakeLists.txt), which defines DESERT_SERVER"
#endif

#include "ServerWorld.hpp"

#include <ToolMain.hpp>
#include <ToolProject.hpp>

#include <Projects/WorldPartitionSettings.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Desert::Server
{
    namespace
    {
        // NET-ARCH §2M.2. Only the mesh of one runs in one process today; the split roles are their own processes
        // from NET-MESH-1, and asking for one before then is refused by name rather than run as `all`.
        enum class Role
        {
            All,
        };

        struct Options
        {
            std::string                  project;
            Role                         role = Role::All;
            std::string                  world;
            glm::vec3                    source{ 0.0f };
            std::optional<std::uint64_t> steps;
        };

        constexpr double kStepHz = 60.0;

        std::string Usage()
        {
            return "usage: DesertServer --project <file.deproj> --role all --world <scene.desce> --source <x,y,z> "
                   "[--steps <n>]";
        }

        Common::ResultStr<glm::vec3> ParsePoint( std::string_view text )
        {
            glm::vec3   point{};
            std::size_t at = 0;
            for ( int axis = 0; axis < 3; ++axis )
            {
                const std::size_t end = axis < 2 ? text.find( ',', at ) : text.size();
                const std::string part =
                     std::string( text.substr( at, end == std::string_view::npos ? 0 : end - at ) );
                std::size_t taken = 0;
                try
                {
                    point[axis] = std::stof( part, &taken );
                }
                catch ( const std::exception& )
                {
                    taken = 0;
                }
                if ( end == std::string_view::npos || taken == 0 || taken != part.size() )
                    return Common::MakeError<glm::vec3>( std::format( "--source '{}' is not x,y,z in cm", text ) );
                at = end + 1;
            }
            return Common::MakeSuccess( point );
        }

        Common::ResultStr<Options> Parse( const std::vector<std::string>& args )
        {
            Options options;
            bool    roleGiven = false, sourceGiven = false;
            for ( std::size_t i = 0; i < args.size(); ++i )
            {
                const std::string& flag = args[i];
                if ( i + 1 >= args.size() )
                    return Common::MakeError<Options>( std::format( "'{}' needs a value", flag ) );
                const std::string& value = args[++i];
                if ( flag == "--project" )
                    options.project = value;
                else if ( flag == "--world" )
                    options.world = value;
                else if ( flag == "--role" )
                {
                    if ( value != "all" )
                        return Common::MakeError<Options>( std::format(
                             "--role '{}': this build runs the mesh of one only (--role all); the gateway, sim, "
                             "store, coordinator and service roles run as their own processes from NET-MESH-1",
                             value ) );
                    options.role = Role::All;
                    roleGiven    = true;
                }
                else if ( flag == "--source" )
                {
                    auto point = ParsePoint( value );
                    if ( !point )
                        return Common::MakeError<Options>( point.GetError() );
                    options.source = point.GetValue();
                    sourceGiven    = true;
                }
                else if ( flag == "--steps" )
                {
                    std::uint64_t n  = 0;
                    const auto    at = std::from_chars( value.data(), value.data() + value.size(), n );
                    if ( at.ec != std::errc() || at.ptr != value.data() + value.size() || n == 0 )
                        return Common::MakeError<Options>(
                             std::format( "--steps '{}' is not a positive count", value ) );
                    options.steps = n;
                }
                else
                    return Common::MakeError<Options>( std::format( "unknown argument '{}'", flag ) );
            }
            if ( options.project.empty() || options.world.empty() || !roleGiven || !sourceGiven )
                return Common::MakeError<Options>( "--project, --role, --world and --source are required" );
            return Common::MakeSuccess( std::move( options ) );
        }

        int Run( const std::vector<std::string>& args )
        {
            auto parsed = Parse( args );
            if ( !parsed )
            {
                std::cerr << "DesertServer: " << parsed.GetError() << "\n" << Usage() << "\n";
                return 2;
            }
            const Options options = parsed.ExtractValue();

            // The project first (engine directory from the executable, the log in <project>/Saved/Logs): the same
            // unattended open every tool makes (Tools/Shared/ToolProject.hpp).
            const Tools::OpenedToolProject opened = Tools::OpenToolProject( options.project, {} );
            if ( !opened.Refused.empty() )
            {
                std::cerr << "DesertServer: " << opened.Refused << "\n";
                return 3;
            }
            // THE SERVER CREATES NO DEVICE: this target links no RHI (no RenderCore, no Vulkan loader, no Metal) -
            // the line states the build, which `otool -L` / `ldd` of the binary prove.
            LOG_INFO( "[Server] DesertServer (DESERT_SERVER), role all (mesh of one): no device created - this "
                      "build links no RHI, window, audio or UI" );

            std::filesystem::path scene( options.world );
            if ( scene.is_relative() )
                scene = Common::Constants::Path::ProjectDir() / scene;
            auto cooked = Core::ReadCookedWorld( scene.generic_string() );
            if ( !cooked )
            {
                LOG_ERROR( "[Server] world '{}': {}", scene.generic_string(), cooked.GetError() );
                return 4;
            }
            if ( !cooked.GetValue().has_value() )
            {
                LOG_ERROR( "[Server] world '{}' has no cooked world at '{}' (cook it: WorldCook)",
                           scene.generic_string(),
                           Core::WorldCells::CookedWorldDirectory( scene.generic_string() ) );
                return 4;
            }
            Core::CookedWorldStart world = std::move( *cooked.ExtractValue() );
            LOG_INFO( "[Server] cooked index '{}{}' loaded: world '{}', cook hash {:016x}, {} unit(s) ({} "
                      "always-loaded), {} record(s), {} cell file(s)",
                      world.Directory, Core::WorldCells::kIndexFileName, world.Index->SceneName, world.CookHash,
                      world.Index->Units.size(), world.Indexed.Plan.AlwaysLoaded.size(),
                      world.Indexed.RecordIds.size(), world.Index->Files.size() );

            Core::Rules::ResidencySettings settings;
            static_cast<Project::WorldPartitionSettings&>( settings ) = Project::CurrentWorldPartitionSettings();
            auto begun = ServerWorld::Begin( std::move( world ), settings );
            if ( !begun )
            {
                LOG_ERROR( "[Server] {}", begun.GetError() );
                return 5;
            }
            std::unique_ptr<ServerWorld> server = begun.ExtractValue();

            Core::Rules::StreamingSource source;
            source.Position = options.source;
            const std::vector<Core::Rules::StreamingSource> sources{ source };
            LOG_INFO(
                 "[Server] stepping at {:.0f} Hz, {} source(s): server source at ({:.0f}, {:.0f}, {:.0f}) cm{}",
                 kStepHz, sources.size(), source.Position.x, source.Position.y, source.Position.z,
                 options.steps ? std::format( ", {} step(s)", *options.steps ) : std::string( ", until killed" ) );

            // THE FIXED STEP: step n runs at n / 60 s of server time, started on the wall clock's tick for it
            // (a step that overruns starts the next one at once; the clock does not drift with the work).
            using Clock                     = std::chrono::steady_clock;
            const auto              period  = std::chrono::duration<double>( 1.0 / kStepHz );
            const Clock::time_point started = Clock::now();
            double                  worstMs = 0.0, totalMs = 0.0;
            std::size_t             activated = 0, deactivated = 0;
            std::uint64_t           step = 0;
            for ( ; !options.steps || step < *options.steps; ++step )
            {
                std::this_thread::sleep_until( started +
                                               std::chrono::duration_cast<Clock::duration>( period * step ) );
                const Clock::time_point begin  = Clock::now();
                auto                    ticked = server->Step( sources, static_cast<double>( step ) / kStepHz );
                if ( !ticked )
                {
                    LOG_ERROR( "[Server] step {}: {}", step, ticked.GetError() );
                    return 6;
                }
                const double ms = std::chrono::duration<double, std::milli>( Clock::now() - begin ).count();
                worstMs         = std::max( worstMs, ms );
                totalMs += ms;
                activated += ticked.GetValue().UnitsActivated;
                deactivated += ticked.GetValue().UnitsDeactivated;
            }
            const double wall = std::chrono::duration<double>( Clock::now() - started ).count();
            LOG_INFO( "[Server] {} step(s) at {:.0f} Hz: {:.2f} s of server time in {:.2f} s wall; step {:.3f} ms "
                      "mean, {:.3f} ms worst; {} cell activation(s), {} deactivation(s), {} read(s) in flight",
                      step, kStepHz, static_cast<double>( step ) / kStepHz, wall, step ? totalMs / step : 0.0,
                      worstMs, activated, deactivated, server->LoadsInFlight() );

            const std::vector<ResidentCell> cells = server->ResidentCells();
            std::string                     names;
            for ( const ResidentCell& cell : cells )
                names += std::format( "{}{} ({})", names.empty() ? "" : ", ", cell.Name, cell.Records );
            LOG_INFO(
                 "[Server] residency union of {} source(s): {} always-loaded unit(s) + {} cell(s) activated, {} "
                 "cell record(s) held: {}",
                 sources.size(), server->AlwaysLoadedUnits(), cells.size(), server->HeldCellRecords(),
                 names.empty() ? std::string( "none" ) : names );
            return 0;
        }
    } // namespace
} // namespace Desert::Server

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain(
         "DesertServer", argc, argv, []( int count, char** values )
         { return Desert::Server::Run( std::vector<std::string>( values + 1, values + count ) ); } );
}
