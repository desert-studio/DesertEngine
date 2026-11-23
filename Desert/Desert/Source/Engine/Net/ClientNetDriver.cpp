#include <Engine/Net/ClientNetDriver.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/CookHash.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/Net/NetRoleComponent.hpp>
#include <Engine/Physics/CharacterControllerComponent.hpp>
#include <Engine/Reflection/FieldValue.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>
#include <Engine/World/Partition/StreamingSourceComponent.hpp>

#include <Net/NetTransport.hpp>
#include <Net/ReplicationClient.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Desert::Core
{
    namespace
    {
        std::optional<std::vector<std::byte>> FromHex( std::string_view hex )
        {
            if ( hex.empty() || hex.size() % 2 != 0 )
                return std::nullopt;
            const auto nibble = []( char c ) -> int
            {
                if ( c >= '0' && c <= '9' )
                    return c - '0';
                if ( c >= 'a' && c <= 'f' )
                    return c - 'a' + 10;
                if ( c >= 'A' && c <= 'F' )
                    return c - 'A' + 10;
                return -1;
            };
            std::vector<std::byte> bytes( hex.size() / 2 );
            for ( std::size_t i = 0; i < bytes.size(); ++i )
            {
                const int hi = nibble( hex[2 * i] ), lo = nibble( hex[2 * i + 1] );
                if ( hi < 0 || lo < 0 )
                    return std::nullopt;
                bytes[i] = static_cast<std::byte>( hi * 16 + lo );
            }
            return bytes;
        }

        glm::vec3 ToGlm( const Net::NetVec3& v )
        {
            return { v.X, v.Y, v.Z };
        }

        // The view's ground-plane basis, as PhysicsECSSystem resolves a character's MoveInput (one rule: a pawn
        // walks where its camera looks, whether its movement runs here or on the server).
        void NetMoveBasisOfView( const Scene& scene, glm::vec3& forward, glm::vec3& right )
        {
            forward = { 0.0f, 0.0f, -1.0f };
            right   = { 1.0f, 0.0f, 0.0f };
            if ( auto cam = scene.GetMainCamera().lock() )
            {
                const glm::mat4 c2w = glm::inverse( cam->GetViewMatrix() );
                forward             = -glm::vec3( c2w[2] );
                right               = glm::vec3( c2w[0] );
            }
            forward.y = 0.0f;
            right.y   = 0.0f;
            if ( glm::length( forward ) > 1e-4f )
                forward = glm::normalize( forward );
            if ( glm::length( right ) > 1e-4f )
                right = glm::normalize( right );
        }
    } // namespace

    Common::ResultStr<std::unique_ptr<ClientNetDriver>> ClientNetDriver::Connect( const ClientNetOptions& options )
    {
        using Result     = std::unique_ptr<ClientNetDriver>;
        const auto token = FromHex( options.TokenHex );
        if ( !token )
            return Common::MakeFormattedError<Result>( "--net-token is not a hex connect token" );
        const std::filesystem::path index =
             std::filesystem::path( options.Cooked ) / std::string( WorldCells::kIndexFileName );
        std::ifstream file( index, std::ios::binary );
        if ( !file )
            return Common::MakeFormattedError<Result>( "--net-cooked '{}' has no {}: the server checks the cooked "
                                                       "world's hash at Hello",
                                                       options.Cooked, WorldCells::kIndexFileName );
        const std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( file ) ),
                                                std::istreambuf_iterator<char>() );

        std::unique_ptr<ClientNetDriver> driver( new ClientNetDriver() );
        driver->m_Options         = options;
        const Net::NetLogSink log = []( std::string_view line ) { LOG_INFO( "[Net] {}", line ); };
        driver->m_Transport       = Net::CreateGnsTransport( log );
        if ( !driver->m_Transport )
            return Common::MakeFormattedError<Result>( "the network transport could not start" );
        std::string error;
        if ( !driver->m_Transport->SetSimulator( { options.LagMs, options.LossPercent, 0.f }, error ) )
            return Common::MakeFormattedError<Result>( "network simulator: {}", error );
        driver->m_Client = std::make_unique<Net::ReplicationClient>(
             *driver->m_Transport, Net::HelloMessage{ Net::kProtocolVersion, CookHashOf( bytes ), *token }, log );
        if ( !driver->m_Client->Connect( options.Address, error ) )
            return Common::MakeFormattedError<Result>( "connect {}: {}", options.Address, error );
        driver->m_Walking = options.WalkCm > 0.0f;
        LOG_INFO( "[Net] Play as client: connecting to {} (cooked world hash {:016x})", options.Address,
                  CookHashOf( bytes ) );
        return Common::MakeSuccess( std::move( driver ) );
    }

    ClientNetDriver::~ClientNetDriver() = default;

    void ClientNetDriver::Tick( Scene& scene, Assets::AssetManager& assets, const double deltaSeconds )
    {
        m_Clock += deltaSeconds;
        m_Client->Poll();
        using EState = Net::ReplicationClient::EState;
        if ( m_Client->State() == EState::Closed )
        {
            if ( !m_Closed )
                LOG_ERROR( "[Net] session closed: {}", m_Client->CloseReason() );
            m_Closed    = true;
            m_InSession = false;
            return;
        }
        if ( m_Client->State() != EState::InSession )
            return;
        if ( !m_InSession )
        {
            LOG_INFO( "[Net] in session with {}", m_Options.Address );
            LoadPawnPrefab( assets );
        }
        m_InSession = true;

        ApplySpawnsAndDestroys( scene, assets );
        WriteTransforms( scene );
        ApplyStableDestroyed( scene );
        ApplyStableState( scene );
        ReportResidency( scene );

        // The input on the server's fixed step: one numbered step per 1/60 s of this client's clock.
        m_StepClock += deltaSeconds;
        const double period = 1.0 / Net::kNetStepHz;
        int          steps  = 0;
        while ( m_StepClock >= period && steps < 8 )
        {
            m_StepClock -= period;
            ++steps;
            const glm::vec3 move = InputMove( scene );
            const float     yaw  = glm::length( move ) > 1e-4f ? std::atan2( -move.x, -move.z ) : 0.0f;
            m_Client->SendInput( { move.x, move.y, move.z }, yaw );
        }
        if ( m_StepClock >= period ) // a hitch longer than 8 steps: the server's clock is not ours to catch up
            m_StepClock = 0.0;

        if ( m_Clock >= m_NextLog )
        {
            m_NextLog += 1.0;
            std::string line;
            for ( const auto& [id, pawn] : m_Client->Pawns() )
                if ( const auto sample = m_Client->Sample( id ) )
                    line += std::format( " {}{}(player {})=({:.0f},{:.0f},{:.0f})", id, pawn.Owned ? "*" : "",
                                         pawn.Player, sample->Position.X, sample->Position.Y, sample->Position.Z );
            LOG_INFO( "[Net] t={:.1f}s pawns{}; scene players {}; unacked inputs {}", m_Clock,
                      line.empty() ? std::string( " none" ) : line, scene.GetPlayers().size(),
                      m_Client->PendingInputs() );
        }
    }

    // THE SERVER'S PAWN PREFAB, LOADED BEFORE ANY PAWN IS SPAWNED. The Welcome names it by handle; the open level
    // references only its own assets, so a pawn the level does not name is not registered yet. The handle is
    // resolved to its file through the content registry (handle -> stable key, no type and no prior registration
    // needed) and registered + loaded here, once per session - UE's client loads the GameMode's DefaultPawnClass
    // the same way.
    void ClientNetDriver::LoadPawnPrefab( Assets::AssetManager& assets )
    {
        const Assets::AssetHandle handle( m_Client->PawnPrefab() );
        if ( assets.FindByHandle<Assets::PrefabAsset>( handle ) )
        {
            m_PawnPrefab = handle;
            return;
        }
        const std::string key = Assets::ContentRegistry::KeyForHandle( m_Client->PawnPrefab() );
        if ( key.empty() )
        {
            LOG_ERROR( "[Net] the server's pawn prefab {:016x} names no content of this client's project",
                       m_Client->PawnPrefab() );
            return;
        }
        const std::filesystem::path path   = Common::AssetHandle::PathForStableKey( key );
        const auto                  prefab = assets.CreateAsset<Assets::PrefabAsset>( path );
        if ( !prefab || !prefab->IsReadyForUse() )
        {
            LOG_ERROR( "[Net] the server's pawn prefab {:016x} ('{}') did not load", m_Client->PawnPrefab(), key );
            return;
        }
        // The Welcome's number is the path-derived one; a prefab that declares its own identity is registered
        // under that, so the spawns below name the asset by the handle it was registered with.
        m_PawnPrefab = Assets::AssetHandle( static_cast<std::uint64_t>( prefab->GetMetadata().Handle ) );
        LOG_INFO( "[Net] loaded the server's pawn prefab {:016x} ('{}', handle {:016x} here)",
                  m_Client->PawnPrefab(), key, static_cast<std::uint64_t>( m_PawnPrefab ) );
    }

    void ClientNetDriver::ApplySpawnsAndDestroys( Scene& scene, const Assets::AssetManager& assets )
    {
        auto& reg = scene.GetRegistry();
        for ( const Net::NetId id : m_Client->TakeSpawned() )
        {
            const auto& pawns = m_Client->Pawns();
            const auto  found = pawns.find( id );
            if ( found == pawns.end() || m_Entities.contains( id ) )
                continue;
            const auto&     replicated = found->second;
            const auto      sample     = m_Client->Sample( id );
            const glm::vec3 at =
                 sample ? ToGlm( sample->Position ) : ToGlm( replicated.History.Sample( 1e12 ).Position );
            if ( replicated.Owned )
            {
                // The own pawn: the one Play spawned from the level's GameMode, at the server's position; a level
                // that names no pawn (a protocol or streaming-test map) gets the SERVER GameMode's pawn, possessed
                // here (UE: the client possesses the pawn the server spawned for its controller).
                entt::entity pawn = scene.GetPlayerPawn();
                if ( pawn == entt::null || !reg.valid( pawn ) )
                {
                    const auto own =
                         SpawnPawnPrefabAt( scene, assets, m_PawnPrefab, glm::translate( glm::mat4( 1.0f ), at ) );
                    if ( !own )
                    {
                        LOG_ERROR( "[Net] own pawn {} (player {}): the server's pawn prefab {:016x}: {}", id,
                                   replicated.Player, m_Client->PawnPrefab(), own.GetError() );
                        continue;
                    }
                    pawn = own.GetValue().GetHandle();
                    scene.SetPlayerPawn( pawn );
                    LOG_INFO( "[Net] the level names no pawn: spawned the server's pawn prefab {:016x} as this "
                              "player's",
                              m_Client->PawnPrefab() );
                }
                else if ( scene.GetSettings().DefaultPawn != m_PawnPrefab )
                {
                    LOG_WARN(
                         "[Net] the level's pawn prefab {:016x} is not the server's {:016x}: the server moves "
                         "it all the same",
                         static_cast<std::uint64_t>( scene.GetSettings().DefaultPawn ), m_Client->PawnPrefab() );
                }
                reg.emplace_or_replace<ECS::NetRoleComponent>( pawn, ECS::ENetRole::Autonomous, id,
                                                               replicated.Player );
                m_Entities[id] = pawn;
                LOG_INFO( "[Net] possessed own pawn net id {} (player {}) at ({:.0f}, {:.0f}, {:.0f}) cm", id,
                          replicated.Player, at.x, at.y, at.z );
                continue;
            }
            const auto spawned =
                 SpawnPawnPrefabAt( scene, assets, m_PawnPrefab, glm::translate( glm::mat4( 1.0f ), at ) );
            if ( !spawned )
            {
                LOG_ERROR( "[Net] remote pawn {} (player {}): {}", id, replicated.Player, spawned.GetError() );
                continue;
            }
            const entt::entity pawn = spawned.GetValue().GetHandle();
            // A client streams only around its own pawn (NET-ARCH §4.3): another player's pawn is no source here,
            // or the cells around it would stay resident and keep it relevant (UE: only the local PlayerController
            // is a streaming source).
            if ( reg.has<ECS::StreamingSourceComponent>( pawn ) )
                reg.remove<ECS::StreamingSourceComponent>( pawn );
            reg.emplace_or_replace<ECS::NetRoleComponent>( pawn, ECS::ENetRole::Simulated, id, replicated.Player );
            scene.AddRemotePlayer( pawn, replicated.Player );
            m_Entities[id] = pawn;
            LOG_INFO( "[Net] remote pawn net id {} (player {}) spawned at ({:.0f}, {:.0f}, {:.0f}) cm", id,
                      replicated.Player, at.x, at.y, at.z );
        }
        for ( const Net::NetId id : m_Client->TakeDestroyed() )
        {
            const auto found = m_Entities.find( id );
            if ( found == m_Entities.end() )
                continue;
            const entt::entity pawn = found->second;
            m_Entities.erase( found );
            if ( !reg.valid( pawn ) )
                continue;
            if ( scene.RemoveRemotePlayer( pawn ) )
                scene.DestroyEntity( ECS::Entity( pawn, reg ) );
            else
                reg.remove<ECS::NetRoleComponent>( pawn ); // the own pawn stays the player's
            LOG_INFO( "[Net] pawn net id {} left", id );
        }
    }

    void ClientNetDriver::WriteTransforms( Scene& scene )
    {
        auto& reg = scene.GetRegistry();
        for ( const auto& [id, entity] : m_Entities )
        {
            if ( !reg.valid( entity ) || !reg.has<ECS::TransformComponent>( entity ) )
                continue;
            const auto sample = m_Client->Sample( id );
            if ( !sample )
                continue;
            const glm::vec3 position = ToGlm( sample->Position );
            if ( id == m_Client->OwnPawn() )
            {
                if ( !m_OwnFirstServerPosition )
                    m_OwnFirstServerPosition = position;
                ECS::SetLocalTranslation( reg, entity, position );
                continue; // the own pawn faces where its controller script turns it
            }
            ECS::LocalTransform local = reg.get<ECS::TransformComponent>( entity ).GetLocal();
            local.Translation         = position;
            local.Rotation            = { 0.0f, sample->Yaw, 0.0f };
            ECS::SetLocalTransform( reg, entity, local );
        }
    }

    struct ClientNetDriver::PendingStable
    {
        std::vector<Net::ReplicationClient::StableChange> Changes;
    };

    namespace
    {
        bool ApplyStableChange( Scene& scene, const Net::ReplicationClient::StableChange& change );
    }

    void ClientNetDriver::ReportResidency( Scene& scene )
    {
        const auto* of = scene.GetRegistry().try_ctx<WorldStreamerOfScene>();
        if ( of == nullptr || of->Streamer == nullptr )
            return; // the played world is not streamed: nothing is a cell
        const WorldStreamer&               streamer = *of->Streamer;
        const Rules::WorldPartitionPlan&   plan     = streamer.Plan();
        const auto&                        units    = streamer.Residency().Units;
        std::set<std::uint32_t>            now;
        for ( std::size_t unit = plan.AlwaysLoaded.size(); unit < units.size(); ++unit )
            if ( units[unit].State == Rules::Residency::Activated )
                now.insert( static_cast<std::uint32_t>( unit ) );
        if ( now == m_ReportedUnits )
            return;
        Net::CellResidencyMessage     change;
        std::vector<Net::StableNetId> gone;
        std::string                   loaded, unloaded;
        for ( const std::uint32_t unit : now )
            if ( !m_ReportedUnits.contains( unit ) )
            {
                change.Loaded.push_back( unit );
                loaded += std::format( "{}{}", loaded.empty() ? "" : ", ", Rules::DescribeResidencyUnit( plan, unit ) );
            }
        for ( const std::uint32_t unit : m_ReportedUnits )
            if ( !now.contains( unit ) )
            {
                change.Unloaded.push_back( unit );
                unloaded +=
                     std::format( "{}{}", unloaded.empty() ? "" : ", ", Rules::DescribeResidencyUnit( plan, unit ) );
                for ( const std::size_t record : Rules::ResidencyUnitMembers( plan, unit ) )
                    gone.push_back( static_cast<std::uint64_t>( streamer.RecordId( record ) ) );
            }
        m_Client->ReportResidency( change, gone );
        m_ReportedUnits = std::move( now );
        if ( !change.Loaded.empty() )
            LOG_INFO( "[Net] residency report: {} cell(s) resident here: {}", change.Loaded.size(), loaded );
        if ( !change.Unloaded.empty() )
            LOG_INFO( "[Net] residency report: {} cell(s) unloaded here ({} record(s) leave the relevant set): {}",
                      change.Unloaded.size(), gone.size(), unloaded );
    }

    void ClientNetDriver::ApplyStableDestroyed( Scene& scene )
    {
        for ( const Net::StableNetId id : m_Client->TakeStableDestroyed() )
            m_StableDestroyed.push_back( id );
        if ( m_StableDestroyed.empty() )
            return;
        const auto* of = scene.GetRegistry().try_ctx<WorldStreamerOfScene>();
        if ( of == nullptr || of->Streamer == nullptr )
            return; // the server names only records of cells this client reported, so a streamer made them
        of->Streamer->DestroyRecordsForNet( std::exchange( m_StableDestroyed, {} ) );
    }

    void ClientNetDriver::ApplyStableState( Scene& scene )
    {
        if ( !m_PendingStable )
            m_PendingStable = std::make_shared<PendingStable>();
        std::vector<Net::ReplicationClient::StableChange>& pending = m_PendingStable->Changes;
        std::vector<Net::ReplicationClient::StableChange>  changes = std::exchange( pending, {} );
        for ( Net::ReplicationClient::StableChange& change : m_Client->TakeStableChanges() )
            changes.push_back( std::move( change ) );
        for ( Net::ReplicationClient::StableChange& change : changes )
        {
            if ( ApplyStableChange( scene, change ) )
                continue;
            // Its record is not streamed in here yet: the change waits, replacing an older one of the same field.
            const auto same = std::find_if( pending.begin(), pending.end(),
                                            [&]( const auto& p )
                                            {
                                                return p.Id == change.Id &&
                                                       p.Field.Component == change.Field.Component &&
                                                       p.Field.Field == change.Field.Field;
                                            } );
            if ( same == pending.end() )
                pending.push_back( std::move( change ) );
            else if ( change.Field.Version > same->Field.Version )
                *same = std::move( change );
        }
    }

    // One field of a stable object into the entity whose UUID is the record id (WorldStreamer makes every record's
    // entity with that id). False = no such entity yet (its cell is not resident); a field that cannot be applied
    // is logged by name and dropped - never written silently and never retried forever.
    namespace
    {
        bool ApplyStableChange( Scene& scene, const Net::ReplicationClient::StableChange& change )
        {
            const auto found = scene.FindEntityByID( Common::UUID( change.Id ) );
            if ( !found )
                return false;
            auto&                   reg    = scene.GetRegistry();
            const entt::entity      entity = found->get().GetHandle();
            const Net::FieldUpdate& field  = change.Field;

            if ( field.Component == Net::kTransformFragment )
            {
                const auto* quantized = std::get_if<Net::QuantizedTransform>( &field.Value );
                if ( quantized == nullptr || !reg.has<ECS::TransformComponent>( entity ) )
                {
                    LOG_ERROR(
                         "[Net] stable {}: the transform fragment is not a transform, or the entity has none",
                         change.Id );
                    return true;
                }
                const Net::NetTransform t     = Net::DequantizeTransform( *quantized );
                ECS::LocalTransform     local = reg.get<ECS::TransformComponent>( entity ).GetLocal();
                local.Translation             = ToGlm( t.Position );
                local.Rotation =
                     glm::eulerAngles( glm::quat( t.Rotation[3], t.Rotation[0], t.Rotation[1], t.Rotation[2] ) );
                ECS::SetLocalTransform( reg, entity, local );
                LOG_INFO( "[Net] stable {} transform v{} applied: {}", change.Id, field.Version,
                          Net::ReplicatedValueText( field.Value ) );
                return true;
            }

            const ECS::ReflectedComponent* row  = ECS::FindReflectedComponent( field.Component );
            const Reflection::TypeInfo*    type = row != nullptr ? row->Type() : nullptr;
            if ( type == nullptr )
            {
                LOG_ERROR( "[Net] stable {}: '{}' is not a reflected component of this build", change.Id,
                           field.Component );
                return true;
            }
            if ( !row->Has( reg, entity ) )
            {
                LOG_ERROR( "[Net] stable {}: the entity has no {} component", change.Id, field.Component );
                return true;
            }
            const auto info =
                 std::find_if( type->Fields.begin(), type->Fields.end(),
                               [&]( const Reflection::FieldInfo& f ) { return f.Name == field.Field; } );
            if ( info == type->Fields.end() || !info->Meta.Replicated )
            {
                LOG_ERROR( "[Net] stable {}: {}.{} is {} - refused", change.Id, field.Component, field.Field,
                           info == type->Fields.end() ? "no field of the component" : "not marked Replicated" );
                return true;
            }
            if ( std::holds_alternative<Net::QuantizedTransform>( field.Value ) )
            {
                LOG_ERROR( "[Net] stable {}: {}.{} carries a transform - refused", change.Id, field.Component,
                           field.Field );
                return true;
            }
            using FT                      = Reflection::FieldType;
            const Reflection::Value value = std::visit(
                 [&]( const auto& v ) -> Reflection::Value
                 {
                     using T = std::decay_t<decltype( v )>;
                     if constexpr ( std::is_same_v<T, bool> )
                         return Reflection::Value::Bool( v );
                     else if constexpr ( std::is_same_v<T, std::int32_t> )
                         return info->Type == FT::Float    ? Reflection::Value::Float( static_cast<float>( v ) )
                                : info->Type == FT::Double ? Reflection::Value::Double( static_cast<double>( v ) )
                                                           : Reflection::Value::Int( v );
                     else if constexpr ( std::is_same_v<T, float> )
                         return info->Type == FT::Double ? Reflection::Value::Double( static_cast<double>( v ) )
                                                         : Reflection::Value::Float( v );
                     else
                         return Reflection::Value::Bool( false ); // a transform names kTransformFragment (above)
                 },
                 field.Value );
            if ( const Common::BoolResultStr written =
                      Reflection::WriteField( *info, row->Data( reg, entity ), value );
                 !written.IsSuccess() )
            {
                LOG_ERROR( "[Net] stable {}: {}.{} = {} refused: {}", change.Id, field.Component, field.Field,
                           Net::ReplicatedValueText( field.Value ), written.GetError() );
                return true;
            }
            row->NotifyChanged( reg, entity ); // registry.patch: on_update fires as for any other edit
            LOG_INFO( "[Net] stable {} {}.{} v{} applied = {}", change.Id, field.Component, field.Field,
                      field.Version, Net::ReplicatedValueText( field.Value ) );
            return true;
        }
    } // namespace

    glm::vec3 ClientNetDriver::InputMove( Scene& scene )
    {
        auto& reg = scene.GetRegistry();
        if ( m_Walking )
        {
            // The scripted walk ends on the SERVER's newest position of the own pawn, not the interpolated one.
            const auto own = m_Client->Pawns().find( m_Client->OwnPawn() );
            if ( own != m_Client->Pawns().end() && m_OwnFirstServerPosition )
            {
                const glm::vec3 newest = ToGlm( own->second.History.Sample( 1e12 ).Position );
                if ( glm::length( newest - *m_OwnFirstServerPosition ) >= m_Options.WalkCm )
                {
                    m_Walking = false;
                    LOG_INFO( "[Net] walked {:.0f} cm (server position): stopping at t={:.1f}s", m_Options.WalkCm,
                              m_Clock );
                }
            }
            if ( m_Walking )
                return glm::length( m_Options.WalkDir ) > 1e-4f ? glm::normalize( m_Options.WalkDir )
                                                                : glm::vec3( 0.0f );
            return glm::vec3( 0.0f );
        }
        if ( m_Options.WalkCm > 0.0f )
            return glm::vec3( 0.0f ); // the scripted walk is over: the pawn stands
        const entt::entity pawn = scene.GetPlayerPawn();
        if ( pawn == entt::null || !reg.valid( pawn ) || !reg.has<ECS::CharacterControllerComponent>( pawn ) )
            return glm::vec3( 0.0f );
        const glm::vec2 intent = reg.get<ECS::CharacterControllerComponent>( pawn ).MoveInput;
        glm::vec3       forward, right;
        NetMoveBasisOfView( scene, forward, right );
        return forward * intent.y + right * intent.x;
    }

    void ClientNetDriver::Shutdown( Scene& scene )
    {
        for ( const auto& [id, entity] : m_Entities )
            if ( entity != scene.GetPlayerPawn() )
                scene.RemoveRemotePlayer( entity );
        m_Entities.clear();
        m_Client.reset();
        m_Transport.reset();
        LOG_INFO( "[Net] Play as client ended" );
    }
} // namespace Desert::Core
