// DesertServer: the dedicated headless server (Docs/contracts/NET-ARCH.md §6). One binary, every role chosen by
// `--role`; NET-MESH-0 runs `--role all`, the mesh of one (§2M.2), which streams one cooked World Partition world
// headless at a fixed 60 Hz step.
//
//   DesertServer --project <file.deproj> --role all --world <scene.desce> --source <x,y,z> [--steps <n>]
//                [--port <p> --server-id <id> --secret-file <hex file> --pawn <assets:X.deprefab> --spawn <x,y,z>
//                ...]
//
// --world   the scene whose cooked world (WorldCells::CookedWorldDirectory, `X.dwworld/` beside it) is served;
//           relative to the project's directory. A scene with no cooked world beside it is refused.
// --source  one server-side streaming source, world cm (NET-ARCH §4.1: an NPC schedule or a script's
//           RequestStreaming; players are sources from NET-1).
// --steps   steps to run, then exit 0 (a scenario's run); without it the server runs until killed.
// --port    listen for clients (NET-1): the gateway admits a connection whose Hello carries a token signed with
//           --secret-file for --server-id and the served world's cook hash, and the sim spawns a pawn for it at
//           the next --spawn point; every pawn is a streaming source of the residency union (NET-ARCH §4.1).

#if !DESERT_SERVER
#error "DesertServer is compiled only by its own target (Server/CMakeLists.txt), which defines DESERT_SERVER"
#endif

#include "ServerWorld.hpp"

#include <Net/MeshOfOne.hpp>

#include <ToolMain.hpp>
#include <ToolProject.hpp>

#include <Projects/WorldPartitionSettings.hpp>

#include <Engine/Reflection/ReplicatedFields.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
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
            std::optional<std::uint16_t> port;
            std::uint64_t                serverId = 0;
            std::string                  secretFile;
            std::vector<glm::vec3>       spawns;
            std::string pawn; // the GameMode's pawn prefab, a stable key (assets:Prefabs/X.deprefab)
        };

        constexpr double kStepHz = 60.0;

        std::string Usage()
        {
            return "usage: DesertServer --project <file.deproj> --role all --world <scene.desce> --source <x,y,z> "
                   "[--steps <n>] "
                   "[--port <p> --server-id <id> --secret-file <file> --pawn <assets:...deprefab> --spawn <x,y,z> "
                   "[--spawn ...]]";
        }

        Common::ResultStr<glm::vec3> ParsePoint( std::string_view text, std::string_view flag )
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
                    return Common::MakeError<glm::vec3>( std::format( "{} '{}' is not x,y,z in cm", flag, text ) );
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
                    auto point = ParsePoint( value, flag );
                    if ( !point )
                        return Common::MakeError<Options>( point.GetError() );
                    options.source = point.GetValue();
                    sourceGiven    = true;
                }
                else if ( flag == "--spawn" )
                {
                    auto point = ParsePoint( value, flag );
                    if ( !point )
                        return Common::MakeError<Options>( point.GetError() );
                    options.spawns.push_back( point.GetValue() );
                }
                else if ( flag == "--secret-file" )
                    options.secretFile = value;
                else if ( flag == "--pawn" )
                    options.pawn = value;
                else if ( flag == "--port" || flag == "--server-id" )
                {
                    std::uint64_t n  = 0;
                    const auto    at = std::from_chars( value.data(), value.data() + value.size(), n );
                    if ( at.ec != std::errc() || at.ptr != value.data() + value.size() ||
                         ( flag == "--port" && ( n == 0 || n > 65535 ) ) )
                        return Common::MakeError<Options>( std::format( "{} '{}' is not a valid number", flag, value ) );
                    if ( flag == "--port" )
                        options.port = static_cast<std::uint16_t>( n );
                    else
                        options.serverId = n;
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
            if ( options.port && ( options.secretFile.empty() || options.serverId == 0 || options.spawns.empty() ||
                                   options.pawn.empty() ) )
                return Common::MakeError<Options>(
                     "--port needs --server-id, --secret-file, --pawn (the prefab every "
                     "player's pawn is) and at least one --spawn (where pawns start)" );
            return Common::MakeSuccess( std::move( options ) );
        }

        // THE SERVER CONSOLE (UE's server console commands): lines on stdin, read on their own thread and run on
        // the step thread between two steps. NET-2's commands write the replicated state of stable objects (§3.2):
        //   set <record id> <Component>.<Field> <true|false|int|float>   a reflected field marked Replicated
        //   transform <record id> <x> <y> <z> [<rx> <ry> <rz>]            the transform fragment, cm and degrees
        //   stat                                                         the replication stat line
        class ServerConsole
        {
        public:
            void Start()
            {
                std::thread(
                     [this]
                     {
                         std::string line;
                         while ( std::getline( std::cin, line ) )
                         {
                             const std::scoped_lock lock( m_Mutex );
                             m_Lines.push_back( line );
                         }
                     } )
                     .detach();
            }
            std::vector<std::string> Take()
            {
                const std::scoped_lock lock( m_Mutex );
                return std::exchange( m_Lines, {} );
            }

        private:
            std::mutex               m_Mutex;
            std::vector<std::string> m_Lines;
        };

        // true / false, an int, or a float (strtof: libc++ has no floating from_chars); nullopt = none of them.
        std::optional<Net::ReplicatedValue> ParseReplicatedValue( const std::string& text )
        {
            if ( text == "true" || text == "false" )
                return Net::ReplicatedValue( text == "true" );
            if ( text.find_first_of( ".eE" ) == std::string::npos )
            {
                std::int32_t value      = 0;
                const auto [end, error] = std::from_chars( text.data(), text.data() + text.size(), value );
                if ( error == std::errc() && end == text.data() + text.size() )
                    return Net::ReplicatedValue( value );
            }
            char*       end   = nullptr;
            const float value = std::strtof( text.c_str(), &end );
            if ( end == text.c_str() + text.size() && !text.empty() && std::isfinite( value ) )
                return Net::ReplicatedValue( value );
            return std::nullopt;
        }

        // One console line; the answer (or the refusal) is logged.
        void RunConsoleLine( const std::string& line, const std::unordered_set<std::uint64_t>& records,
                             Net::IReplicationStore& store, const Net::Gateway& gateway )
        {
            std::istringstream words( line );
            std::string        command;
            words >> command;
            if ( command.empty() )
                return;
            if ( command == "stat" )
            {
                const Net::Gateway::Stats& s = gateway.ReplicationStats();
                LOG_INFO(
                     "[Server] net stat: {} spawn(s) sent (dynamic pawns); {} stable object(s) replicated as "
                     "state, 0 as spawns: {} ObjectState packet(s), {} field update(s), {} acked, {} lost and "
                     "re-sent",
                     s.Spawns, store.StableObjects().size(), s.StablePackets, s.StableFields, s.Acked, s.Resent );
                return;
            }
            std::uint64_t id = 0;
            std::string   idText;
            words >> idText;
            const auto [idEnd, idError] = std::from_chars( idText.data(), idText.data() + idText.size(), id );
            if ( idError != std::errc() || idEnd != idText.data() + idText.size() || !records.contains( id ) )
            {
                LOG_ERROR( "[Server] console '{}': '{}' is not a record id of the cooked world", line, idText );
                return;
            }
            if ( command == "set" )
            {
                std::string path, valueText;
                words >> path >> valueText;
                const std::size_t dot = path.find( '.' );
                if ( dot == std::string::npos || dot == 0 || dot + 1 == path.size() || valueText.empty() )
                {
                    LOG_ERROR( "[Server] console '{}': usage set <record id> <Component>.<Field> <value>", line );
                    return;
                }
                // UE replicates only UPROPERTY(Replicated): the server refuses any other field by name, before
                // it versions anything, so a typo or an unmarked field never reaches a client (the client's own
                // check in ClientNetDriver stays as the second gate).
                const std::string component = path.substr( 0, dot ), field = path.substr( dot + 1 );
                const Reflection::ReplicatedField* replicated = Reflection::FindReplicatedField( component, field );
                if ( replicated == nullptr )
                {
                    LOG_ERROR( "[Server] console '{}': refused - {}.{} is not a Replicated field (PROPERTY(Replicated) "
                               "of a component block); nothing sent",
                               line, component, field );
                    return;
                }
                const std::optional<Net::ReplicatedValue> value = ParseReplicatedValue( valueText );
                if ( !value )
                {
                    LOG_ERROR( "[Server] console '{}': '{}' is not true, false, an int or a float", line,
                               valueText );
                    return;
                }
                const bool boolField = replicated->Type == Reflection::FieldType::Bool;
                if ( boolField != std::holds_alternative<bool>( *value ) )
                {
                    LOG_ERROR( "[Server] console '{}': refused - {}.{} is {}, '{}' is not; nothing sent", line,
                               component, field, boolField ? "a bool" : "a number", valueText );
                    return;
                }
                const std::uint32_t version =
                     store.WriteStable( id, component, field, *value );
                LOG_INFO( "[Server] console: stable {} {} = {} (version {})", id, path,
                          Net::ReplicatedValueText( *value ), version );
                return;
            }
            if ( command == "transform" )
            {
                float v[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
                int   read = 0;
                while ( read < 6 && ( words >> v[read] ) )
                    ++read;
                if ( read != 3 && read != 6 )
                {
                    LOG_ERROR( "[Server] console '{}': usage transform <record id> <x> <y> <z> [<rx> <ry> <rz>]",
                               line );
                    return;
                }
                // The editor's Rotation: Euler degrees, composed as the entity's LocalTransform does (glm::quat).
                const glm::quat            q = glm::quat( glm::radians( glm::vec3( v[3], v[4], v[5] ) ) );
                Net::NetTransform          transform{ { v[0], v[1], v[2] }, { q.x, q.y, q.z, q.w } };
                const Net::ReplicatedValue value( Net::QuantizeTransform( transform ) );
                const std::uint32_t        version =
                     store.WriteStable( id, Net::kTransformFragment, Net::kTransformFragment, value );
                LOG_INFO( "[Server] console: stable {} transform = {} (version {})", id,
                          Net::ReplicatedValueText( value ), version );
                return;
            }
            LOG_ERROR( "[Server] console '{}': unknown command (set, transform, stat)", line );
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

            // THE MESH OF ONE (NET-ARCH §2M.2): gateway + sim + store in this process, behind the interfaces the
            // split roles of NET-MESH-1 keep.
            const Net::NetLogSink netLog = []( std::string_view line ) { LOG_INFO( "[Net] {}", line ); };
            std::vector<std::byte>                     secret;
            std::unique_ptr<Net::INetTransport>        transport;
            std::unique_ptr<Net::InProcessReplicationStore> store;
            std::unique_ptr<Net::Gateway>              gateway;
            std::unique_ptr<Net::PawnSimulation>       sim;
            if ( options.port )
            {
                std::string error;
                secret = Net::ReadTokenSecretFile( options.secretFile, error );
                if ( secret.empty() )
                {
                    LOG_ERROR( "[Server] {}", error );
                    return 7;
                }
                transport = Net::CreateGnsTransport( netLog );
                if ( !transport || !transport->Listen( *options.port, error ) )
                {
                    LOG_ERROR( "[Server] cannot listen on port {}: {}", *options.port, error );
                    return 7;
                }
                // THE SERVER'S GAMEMODE PAWN (UE DefaultPawnClass): named by its stable key, so the handle in the
                // Welcome is the one every client's registry gives that prefab (AssetHandle::FromKey).
                const std::filesystem::path pawnPath = Common::AssetHandle::PathForStableKey( options.pawn );
                if ( !Common::AssetHandle::IsProjectRelativeKey( options.pawn ) ||
                     pawnPath.extension() != ".deprefab" || !std::filesystem::is_regular_file( pawnPath ) )
                {
                    LOG_ERROR( "[Server] --pawn '{}' is not a prefab of this project (looked at '{}')",
                               options.pawn, pawnPath.string() );
                    return 7;
                }
                const Common::AssetHandle pawnPrefab = Common::AssetHandle::FromKey( options.pawn );
                store   = std::make_unique<Net::InProcessReplicationStore>();
                gateway                              = std::make_unique<Net::Gateway>(
                     *transport, *store, Net::HelloPolicy{ server->World().CookHash, options.serverId, secret, 0 },
                     static_cast<std::uint64_t>( pawnPrefab ), netLog );
                Net::PawnSimulation::Settings simSettings;
                for ( const glm::vec3& spawn : options.spawns )
                    simSettings.SpawnPoints.push_back( { spawn.x, spawn.y, spawn.z } );
                sim = std::make_unique<Net::PawnSimulation>( *store, *gateway, std::move( simSettings ), netLog );
                gateway->Bind( *sim );
                LOG_INFO(
                     "[Server] listening on port {} (server id {}, cook hash {:016x}); {} spawn point(s); pawn {} "
                     "({:016x})",
                     *options.port, options.serverId, server->World().CookHash, options.spawns.size(),
                     options.pawn, static_cast<std::uint64_t>( pawnPrefab ) );
            }

            // THE STABLE NET IDS (§3.2): every record id of the cooked index - what the console may address.
            std::unordered_set<std::uint64_t> records;
            for ( const auto& unit : server->World().Index->Units )
                records.insert( unit.Ids.begin(), unit.Ids.end() );
            ServerConsole console;
            if ( transport )
                console.Start();

            // THE SOURCES: the server's own + every player's pawn (§4.1 - the residency union of all of them).
            Core::Rules::StreamingSource serverSource;
            serverSource.Position = options.source;
            std::vector<Core::Rules::StreamingSource> sources{ serverSource };
            std::vector<std::string>                  sourceNames{ std::format(
                 "server ({:.0f}, {:.0f}, {:.0f})", serverSource.Position.x, serverSource.Position.y,
                 serverSource.Position.z ) };
            const auto gatherSources = [&]
            {
                sources.resize( 1 );
                sourceNames.resize( 1 );
                if ( !sim )
                    return;
                for ( const Net::PawnSimulation::Source& player : sim->Sources() )
                {
                    Core::Rules::StreamingSource source;
                    source.Position = { player.Position.X, player.Position.Y, player.Position.Z };
                    sources.push_back( source );
                    sourceNames.push_back( std::format( "player {} account {} ({:.0f}, {:.0f}, {:.0f})", player.Player,
                                                        player.Account, player.Position.X, player.Position.Y,
                                                        player.Position.Z ) );
                }
            };
            const auto logUnion = [&]( std::string_view why )
            {
                const std::vector<ResidentCell> cells = server->ResidentCells();
                std::string                     names, listed;
                for ( const ResidentCell& cell : cells )
                    names += std::format( "{}{} ({})", names.empty() ? "" : ", ", cell.Name, cell.Records );
                for ( const std::string& name : sourceNames )
                    listed += std::format( "{}{}", listed.empty() ? "" : "; ", name );
                LOG_INFO( "[Server] residency union ({}) of {} source(s) [{}]: {} always-loaded unit(s) + {} "
                          "cell(s) activated, {} cell record(s) held: {}",
                          why, sources.size(), listed, server->AlwaysLoadedUnits(), cells.size(),
                          server->HeldCellRecords(), names.empty() ? std::string( "none" ) : names );
            };
            LOG_INFO( "[Server] stepping at {:.0f} Hz, server source at ({:.0f}, {:.0f}, {:.0f}) cm{}", kStepHz,
                      serverSource.Position.x, serverSource.Position.y, serverSource.Position.z,
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
                const Clock::time_point begin = Clock::now();
                if ( transport )
                {
                    for ( const std::string& line : console.Take() )
                        RunConsoleLine( line, records, *store, *gateway );
                    transport->Poll( *gateway );
                    sim->Step( 1.0 / kStepHz );
                    if ( step % Net::kSnapshotEverySteps == 0 )
                        gateway->SendSnapshots( static_cast<std::uint32_t>( step ) );
                }
                const std::size_t playersBefore = sources.size() - 1;
                gatherSources();
                auto ticked = server->Step( sources, static_cast<double>( step ) / kStepHz );
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
                // The union is logged when a player joins or leaves and every 5 s while players are in.
                if ( sources.size() - 1 != playersBefore )
                    logUnion( std::format( "step {}: {} player(s)", step, sources.size() - 1 ) );
                else if ( sources.size() > 1 && step % 300 == 0 )
                    logUnion( std::format( "step {}", step ) );
                if ( transport && sources.size() > 1 && step % 300 == 0 )
                    RunConsoleLine( "stat", records, *store, *gateway );
            }
            const double wall = std::chrono::duration<double>( Clock::now() - started ).count();
            LOG_INFO( "[Server] {} step(s) at {:.0f} Hz: {:.2f} s of server time in {:.2f} s wall; step {:.3f} ms "
                      "mean, {:.3f} ms worst; {} cell activation(s), {} deactivation(s), {} read(s) in flight",
                      step, kStepHz, static_cast<double>( step ) / kStepHz, wall, step ? totalMs / step : 0.0,
                      worstMs, activated, deactivated, server->LoadsInFlight() );

            logUnion( "final" );
            if ( transport )
                RunConsoleLine( "stat", records, *store, *gateway );
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
