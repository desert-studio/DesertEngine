#include <Engine/Net/ClientNetDriver.hpp>

#include <chrono>

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
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/World/Partition/StreamingSourceComponent.hpp>

#include <Net/NetTransport.hpp>
#include <Net/MovePrediction.hpp>
#include <Engine/Gameplay/PawnPrefabMovement.hpp>
#include <Engine/Gameplay/PhysicsPawnMoveWorld.hpp>
#include <Engine/Gameplay/ScriptedPawnInput.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/Input/InputKey.hpp>
#include <Engine/Input/LocalPawnInput.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/CharacterMovement.hpp>
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

        // UE NetworkNoSmoothUpdateDistance: a correction farther than this is a relocation, drawn at once.
        constexpr float  kNoSmoothDistanceCm = 384.0f;
        // UE NetworkSimulatedSmoothLocationTime: the drawn offset a correction leaves decays with this time.
        constexpr double kSmoothSeconds = 0.1;
        // Steps one Tick may send and predict: a frame of up to a quarter second (a Debug editor drawing a
        // streamed world) keeps the server's queue fed; a longer hitch is dropped, not replayed in a burst.
        constexpr int kCatchUpSteps = 15;
        // The clock sync (Overwatch's input buffer, UE's client-side time dilation): the server's queue of this
        // client's input steps is read at its FEWEST over a window, not on average - a frame sends its steps in a
        // burst, so the queue saws between that minimum and minimum + the burst, and only the minimum says how
        // close the server came to starving. Target: one step always waiting (each input arrives a step early);
        // the step rate dilates in proportion, a percent per step of error and never more than two (no visible
        // speed change, the reading stays in 0.98-1.02).
        constexpr double kQueueTarget = 1.0, kQueueWindowSeconds = 1.0, kDilationPerStep = 0.01,
                         kMaxDilation = 0.02;

        std::optional<NetPredictionStats>& StatsSlot()
        {
            static std::optional<NetPredictionStats> stats;
            return stats;
        }
    } // namespace

    namespace
    {
        // The game's mode bits of a replicated state (Net::SetMoveFlags: Grounded is bit 0, the mode above it).
        std::uint8_t ModeOf( const Net::PawnState& state )
        {
            Net::PawnMoveState move;
            Net::SetMoveFlags( move, state.Movement );
            return move.Mode;
        }

        // A replicated sample as a move state (CharacterMovement::ApplyMoveState's input): its interpolated
        // velocity - the Hermite's derivative, so the walk cycle's rate follows the path drawn - Grounded and the
        // game's mode bits.
        Net::PawnMoveState MoveStateOf( const Net::PawnState& sample )
        {
            Net::PawnMoveState move;
            Net::SetMoveFlags( move, sample.Movement );
            move.Position = sample.Position;
            move.Velocity = sample.Velocity;
            move.Yaw      = sample.Yaw;
            return move;
        }

        // UE ACharacter on the owner and the proxies alike (Gameplay::DrawnPawn): the pawn's entity at its
        // capsule's CENTRE above the FEET the net layer carries, its meshes lifted while the move mode is crouched.
        // The lift lives on the pawn root's CharacterController (the pawn prefab's; a root without one is drawn
        // at the centre and has no meshes to lift for a crouch it cannot have).
        void PlaceNetPawn( entt::registry& reg, entt::entity pawn, const Gameplay::PawnMoveSettings& settings,
                           const glm::vec3& feet, std::uint8_t mode )
        {
            const Gameplay::DrawnPawn drawn  = Gameplay::DrawnPawnOf( settings, mode );
            const glm::vec3           center = feet + glm::vec3( 0.0f, drawn.CenterAboveFeet, 0.0f );
            if ( auto* cc = reg.try_get<ECS::CharacterControllerComponent>( pawn ) )
                ECS::CharacterMovement::PlaceDrawnPawn( reg, pawn, *cc, center, drawn.MeshLift );
            else
                ECS::SetLocalTranslation( reg, pawn, center );
        }
    } // namespace

    struct ClientNetDriver::Prediction
    {
        Net::MovePredictor Predictor;
        Gameplay::PawnMovement
             Movement; // the pawn prefab's settings and declared actions, as the server reads them
        // The scene's physics world the own pawn is predicted in: the same cells' colliders the server moves it
        // through (PhysicsPawnMoveWorld), and the game's mover over it - the step the server runs. Made with the
        // first prediction, dropped with the world it lives in.
        std::unique_ptr<Gameplay::PhysicsPawnMoveWorld> World;
        std::unique_ptr<Gameplay::CharacterPawnMover>   Mover;
        Physics::PhysicsWorld*                     WorldOf = nullptr;
        bool                                       NoWorldSaid = false; // the missing world is said once per loss
    };

    Net::IPawnMover* ClientNetDriver::PredictionMover( Scene& scene )
    {
        // The scene this driver ticks, not WorldContext::Current(): that is open only around a script call, and
        // the net tick (PlaySession) runs outside any - NET-PREDICT p7: the own pawn was never predicted, drawn
        // at its spawn and its view with it, while the server walked it 300 m.
        const auto*            system  = scene.FindSystem<ECS::PhysicsECSSystem>();
        Physics::PhysicsWorld* physics = system != nullptr ? system->GetPhysicsWorld() : nullptr;
        if ( physics == nullptr && !m_Prediction->NoWorldSaid )
            LOG_ERROR( "[Net] the own pawn cannot be predicted: the scene runs no physics world" );
        m_Prediction->NoWorldSaid = physics == nullptr;
        if ( physics != m_Prediction->WorldOf )
        {
            m_Prediction->Mover.reset();
            m_Prediction->World.reset();
            m_Prediction->WorldOf = nullptr;
            if ( physics != nullptr )
            {
                auto made = Gameplay::PhysicsPawnMoveWorld::Create( *physics, m_Prediction->Movement.Settings );
                if ( !made )
                {
                    LOG_ERROR( "[Net] the own pawn cannot be predicted: {}", made.GetError() );
                    return nullptr;
                }
                m_Prediction->World   = made.ExtractValue();
                m_Prediction->WorldOf = physics;
                m_Prediction->Mover   = std::make_unique<Gameplay::CharacterPawnMover>( m_Prediction->Movement,
                                                                                        *m_Prediction->World );
            }
        }
        return m_Prediction->Mover.get();
    }

    std::optional<NetPredictionStats> ClientNetDriver::LastStats()
    {
        return StatsSlot();
    }

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
             *driver->m_Transport, Net::HelloMessage{ Net::kProtocolVersion, CookHashOf( bytes ), *token, {} },
             log );
        if ( !driver->m_Client->Connect( options.Address, error ) )
            return Common::MakeFormattedError<Result>( "connect {}: {}", options.Address, error );
        if ( options.WalkCm > 0.0f )
            driver->m_Script = std::make_unique<Gameplay::ScriptedPawnInput>( Gameplay::ScriptedWalk{
                 options.WalkCm, options.WalkDir, options.WalkSprint, options.WalkJumpEverySeconds } );
        LOG_INFO( "[Net] Play as client: connecting to {} (cooked world hash {:016x})", options.Address,
                  CookHashOf( bytes ) );
        return Common::MakeSuccess( std::move( driver ) );
    }

    ClientNetDriver::~ClientNetDriver() = default;

    void ClientNetDriver::Tick( Scene& scene, Assets::AssetManager& assets )
    {
        const auto   now          = std::chrono::steady_clock::now();
        const double deltaSeconds = m_LastTick ? std::chrono::duration<double>( now - *m_LastTick ).count() : 0.0;
        m_LastTick                = now;
        m_Clock += deltaSeconds;
        m_Client->Poll();
        using EState = Net::ReplicationClient::EState;
        if ( m_Client->State() == EState::Closed )
        {
            if ( !m_Closed )
                LOG_ERROR( "[Net] session closed: {}", m_Client->CloseReason() );
            m_Closed    = true;
            m_InSession = false;
            if ( auto* of = scene.GetRegistry().try_ctx<WorldStreamerOfScene>(); of != nullptr && of->Streamer )
                of->Streamer->SetNetRevealHold( false ); // no server to acknowledge a cell any more
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

        if ( !m_Prediction )
        {
            m_Prediction = std::make_shared<Prediction>();
            // UE: the own pawn's CharacterMovement is the DefaultPawnClass's - the prefab the server named, read
            // as the server reads it (PawnPrefabMovement.hpp), so both sides step the same capsule.
            const std::string key      = Assets::ContentRegistry::KeyForHandle( m_Client->PawnPrefab() );
            auto              movement =
                 Gameplay::PrefabMovement::ReadPawnPrefabMovement( Common::AssetHandle::PathForStableKey( key ) );
            if ( movement )
                m_Prediction->Movement = movement.GetValue();
            else
                LOG_ERROR( "[Net] the own pawn's movement: {}", movement.GetError() );
        }
        ApplySpawnsAndDestroys( scene, assets );
        ReconcileOwnPawn( scene );
        WriteTransforms( scene );
        ApplyStableDestroyed( scene );
        ApplyStableState( scene );
        ReportResidency( scene );

        // The input on the server's fixed step: one numbered step per 1/60 s of this client's clock, dilated by the
        // clock sync; each step is simulated here as it is sent (the prediction) - none before the own pawn is.
        Net::MovePredictor& predictor = m_Prediction->Predictor;
        m_StepClock += deltaSeconds;
        const double period = m_Dilation / Net::kNetStepHz;
        int          steps  = 0;
        while ( predictor.Started() && m_StepClock >= period && steps < kCatchUpSteps )
        {
            m_StepClock -= period;
            ++steps;
            if ( const auto sent = m_Client->RecordInput( NextInput( scene ) ) )
                if ( Net::IPawnMover* mover = PredictionMover( scene ) )
                    predictor.Predict( *sent, *mover, Net::kNetStepSeconds );
        }
        // This frame's steps go out together with every one the server has not acked yet: one message per frame.
        if ( steps > 0 )
            m_Client->FlushInputs();
        if ( !predictor.Started() || m_StepClock >= period ) // a hitch longer than kCatchUpSteps is not caught up
            m_StepClock = 0.0;

        // The own pawn is drawn between its last two predicted steps, plus what is left of a correction's offset.
        m_SmoothOffset *= static_cast<float>( std::exp( -deltaSeconds / kSmoothSeconds ) );
        if ( predictor.Started() )
            if ( const auto own = m_Entities.find( m_Client->OwnPawn() ); own != m_Entities.end() &&
                                                                          scene.GetRegistry().valid( own->second ) )
            {
                PlaceNetPawn( scene.GetRegistry(), own->second, m_Prediction->Movement.Settings, OwnDrawnFeet(),
                              predictor.Current().Mode );
                // The predicted movement is the own pawn's state (UE: the autonomous proxy's CharacterMovement).
                if ( auto* cc = scene.GetRegistry().try_get<ECS::CharacterControllerComponent>( own->second ) )
                    ECS::CharacterMovement::ApplyMoveState( *cc, predictor.Current() );
            }
        StatsSlot() = NetPredictionStats{ predictor.Corrections(), predictor.Unacked(), 1.0 / m_Dilation };

        if ( m_Clock >= m_NextLog )
        {
            m_NextLog += 1.0;
            std::string line;
            for ( const auto& [id, pawn] : m_Client->Pawns() )
                if ( const auto sample = m_Client->Sample( id ) )
                    line += std::format( " {}{}(player {})=({:.0f},{:.0f},{:.0f})", id, pawn.Owned ? "*" : "",
                                         pawn.Player, sample->Position.X, sample->Position.Y, sample->Position.Z );
            // What this player is shown: the own pawn as drawn (its entity's world position) and the view point
            // (UE GetPlayerViewPoint) - a view that does not travel with the pawn reads here, not in a frame.
            std::string shown;
            if ( const auto own = m_Entities.find( m_Client->OwnPawn() );
                 own != m_Entities.end() && scene.GetRegistry().valid( own->second ) )
            {
                // The entity is the capsule's centre; "drawn own" reads its feet, as the server's position is.
                std::uint8_t mode = predictor.Current().Mode;
                if ( !predictor.Started() )
                    if ( const auto sample = m_Client->Sample( m_Client->OwnPawn() ) )
                        mode = ModeOf( *sample );
                const glm::vec3 center = ECS::Entity( own->second, scene.GetRegistry() ).GetWorldTransform()[3];
                const float above = Gameplay::DrawnPawnOf( m_Prediction->Movement.Settings, mode ).CenterAboveFeet;
                shown += std::format( "; drawn own ({:.0f},{:.0f},{:.0f}) centre y {:.0f}", center.x,
                                      center.y - above, center.z, center.y );
            }
            if ( const auto view = scene.PlayerViewPoint() )
                shown += std::format( "; view ({:.0f},{:.0f},{:.0f})", ( *view )[3].x, ( *view )[3].y, ( *view )[3].z );
            LOG_INFO( "[Net] t={:.1f}s pawns{}; scene players {}; unacked inputs {}; corrections {}; server input "
                      "queue {:.1f}, clock x{:.2f}{}",
                      m_Clock, line.empty() ? std::string( " none" ) : line, scene.GetPlayers().size(),
                      m_Client->PendingInputs(), predictor.Corrections(), m_ServerQueue, 1.0 / m_Dilation, shown );
        }
    }

    // THE SERVER'S PAWN PREFAB, READ OFF THE FRAME BEFORE ANY PAWN IS SPAWNED. The Welcome names it by handle;
    // the open level references only its own assets, so a pawn the level does not name is not registered yet.
    // The handle is resolved to its file through the content registry (handle -> stable key, no type and no prior
    // registration needed), registered as an UNREAD shell and read on the AsyncAssetLoader's workers; its
    // dependency closure is requested when the read lands (RequestPawnClosure), and spawns wait for both - UE's
    // client streams the GameMode's DefaultPawnClass and its references (FStreamableManager) the same way,
    // never inside a frame.
    void ClientNetDriver::LoadPawnPrefab( Assets::AssetManager& assets )
    {
        const Assets::AssetHandle handle( m_Client->PawnPrefab() );
        if ( const auto resident = assets.FindByHandle<Assets::PrefabAsset>( handle );
             resident && resident->IsReadyForUse() )
        {
            m_PawnPrefab = handle;
            RequestPawnClosure();
            return;
        }
        const std::string key = Assets::ContentRegistry::KeyForHandle( m_Client->PawnPrefab() );
        if ( key.empty() )
        {
            LOG_ERROR( "[Net] the server's pawn prefab {:016x} names no content of this client's project",
                       m_Client->PawnPrefab() );
            return;
        }
        // The shell, not the bytes: CreateAsset without a load reads the header only, so the shell already
        // carries the prefab's own identity. The Welcome's number is the path-derived one; a prefab that declares
        // its own identity is registered under that, and the spawns name the asset by the handle it was
        // registered with (which is why this is not CreateFromRegistryRow, which refuses that disagreement).
        const std::filesystem::path path = Common::AssetHandle::PathForStableKey( key );
        const auto prefab = assets.CreateAsset<Assets::PrefabAsset>( path, /*loadAfterCreate=*/false );
        if ( !prefab )
        {
            LOG_ERROR( "[Net] the server's pawn prefab {:016x} ('{}') could not be created",
                       m_Client->PawnPrefab(), key );
            return;
        }
        const auto here     = Assets::AssetHandle( static_cast<std::uint64_t>( prefab->GetMetadata().Handle ) );
        m_PawnPrefabReading = true;
        m_PawnPrefabRead    = Assets::AsyncAssetLoader::Get().Request(
             prefab,
             [this, here, key, welcome = m_Client->PawnPrefab()]( const Assets::Asset<Assets::AssetBase>& read,
                                                                  const Assets::LoadOutcome               outcome,
                                                                  const std::string&                      error )
             {
                 m_PawnPrefabReading = false;
                 if ( outcome == Assets::LoadOutcome::Failed || !read || !read->IsReadyForUse() )
                 {
                     LOG_ERROR( "[Net] the server's pawn prefab {:016x} ('{}') did not load: {}", welcome, key,
                                   error );
                     return;
                 }
                 m_PawnPrefab = here;
                 LOG_INFO( "[Net] read the server's pawn prefab {:016x} off the frame ('{}', handle {:016x} here)",
                              welcome, key, static_cast<std::uint64_t>( here ) );
                 RequestPawnClosure();
             },
             [this] { m_PawnPrefabReading = false; } );
        if ( !m_PawnPrefabRead.IsValid() )
        {
            m_PawnPrefabReading = false; // the loader refused and logged why
            return;
        }
        LOG_INFO( "[Net] reading the server's pawn prefab {:016x} ('{}') off the frame", m_Client->PawnPrefab(),
                  key );
    }

    // What a pawn spawn waits for: the rows of the prefab's closure that a spawn binds at once - meshes and their
    // rigs, materials. Textures stream after the spawn at the mips the view asks (UE: a texture is never part of
    // what an actor's spawn waits for), so they are read here but do not hold the spawn.
    struct ClientNetDriver::PawnClosure
    {
        std::vector<Assets::ContentRegistry::ClosureRow> Rows;
        bool                                             Landed = false;
    };

    void ClientNetDriver::RequestPawnClosure()
    {
        m_PawnClosure       = std::make_shared<PawnClosure>();
        m_PawnClosure->Rows = Assets::ContentRegistry::Closure(
             { { Common::AssetHandle( static_cast<std::uint64_t>( m_PawnPrefab ) ),
                 std::string( Common::Content::KindName( Common::Content::ContentKind::Prefab ) ) } } );
        Runtime::StartClosureReads( m_PawnClosure->Rows );
        LOG_INFO( "[Net] reading the pawn prefab's closure off the frame: {} rows", m_PawnClosure->Rows.size() );
    }

    bool ClientNetDriver::PawnClosureLanded()
    {
        if ( m_PawnPrefabReading )
            return false; // the prefab file itself is still on a worker
        if ( !m_PawnClosure )
            return true; // no prefab loaded: the spawn says why (SpawnPawnPrefabAt's error), nothing to wait for
        if ( m_PawnClosure->Landed )
            return true;
        std::vector<Assets::ContentRegistry::ClosureRow> held;
        for ( const auto& row : m_PawnClosure->Rows )
            if ( row.Kind != Common::Content::KindName( Common::Content::ContentKind::Texture ) )
                held.push_back( row );
        if ( Runtime::ClosureArriving( held, 0.0f ) > 0 )
            return false;
        m_PawnClosure->Landed = true;
        LOG_INFO( "[Net] the pawn prefab's closure has landed ({} rows): pawns spawn from now on",
                  m_PawnClosure->Rows.size() );
        return true;
    }

    void ClientNetDriver::ApplySpawnsAndDestroys( Scene& scene, Assets::AssetManager& assets )
    {
        auto& reg = scene.GetRegistry();
        for ( const Net::NetId id : m_Client->TakeSpawned() )
            m_AwaitingSpawn.push_back( id );
        // A spawn binds the prefab's meshes and materials at once: it waits for their reads to land rather than
        // reading them inside this frame (a destroy before that finds no entity and the held id no pawn).
        std::vector<std::uint32_t> spawning;
        if ( PawnClosureLanded() )
            spawning.swap( m_AwaitingSpawn );
        for ( const Net::NetId id : spawning )
        {
            const auto& pawns = m_Client->Pawns();
            const auto  found = pawns.find( id );
            if ( found == pawns.end() || m_Entities.contains( id ) )
                continue;
            const auto&     replicated = found->second;
            const auto      sample     = m_Client->Sample( id );
            const glm::vec3 at =
                 sample ? ToGlm( sample->Position ) : ToGlm( replicated.History.Newest().Position );
            // UE ACharacter: the pawn spawns at its capsule's centre; the replicated position is its feet.
            const glm::vec3 center =
                 at + glm::vec3( 0.0f,
                                 Gameplay::DrawnPawnOf( m_Prediction->Movement.Settings,
                                                        ModeOf( sample ? *sample : replicated.History.Newest() ) )
                                      .CenterAboveFeet,
                                 0.0f );
            if ( replicated.Owned )
            {
                // The own pawn: the one Play spawned from the level's GameMode, at the server's position; a level
                // that names no pawn (a protocol or streaming-test map) gets the SERVER GameMode's pawn, possessed
                // here (UE: the client possesses the pawn the server spawned for its controller).
                entt::entity pawn = scene.GetPlayerPawn();
                if ( pawn == entt::null || !reg.valid( pawn ) )
                {
                    const auto own =
                         SpawnPawnPrefabAt( scene, assets, m_PawnPrefab, glm::translate( glm::mat4( 1.0f ), center ) );
                    if ( !own )
                    {
                        LOG_ERROR( "[Net] own pawn {} (player {}): the server's pawn prefab {:016x}: {}", id,
                                   replicated.Player, m_Client->PawnPrefab(), own.GetError() );
                        continue;
                    }
                    pawn = own.GetValue().GetHandle();
                    scene.SetPlayerPawn( pawn );
                    // UE APlayerController::Possess -> PawnClientRestart: the pawn's mapping contexts join the
                    // local player's Enhanced Input (its EnhancedInputPlayerComponent), as GameMode's restart
                    // does.
                    if ( auto* scripts = scene.FindSystem<ECS::ScriptSystem>() )
                        scripts->PlayerInput().PossessPawn( reg, assets, pawn );
                    LOG_INFO( "[Net] the level names no pawn: spawned the server's pawn prefab {:016x} as this "
                              "player's",
                              m_Client->PawnPrefab() );
                    // UE APlayerController::Possess -> SetViewTarget(pawn): the player looks through the pawn it
                    // now has, as it would through a pawn Play spawned itself (Scene::ResolveViewTarget).
                    if ( const auto view = scene.ResolveViewTarget(); !view )
                    {
                        LOG_ERROR( "[Net] own pawn {}: {}", id, view.GetError() );
                    }
                    else if ( view.GetValue() != entt::null )
                    {
                        LOG_INFO( "[Net] the player's view is now the possessed pawn's camera (entity {})",
                                  static_cast<std::uint32_t>( view.GetValue() ) );
                    }
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
                 SpawnPawnPrefabAt( scene, assets, m_PawnPrefab, glm::translate( glm::mat4( 1.0f ), center ) );
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
                // Predicted: Tick draws it after this frame's steps. It faces where its controller script turns
                // it. Until the prediction starts it is drawn - and moves - as the server's sample says.
                if ( !m_Prediction->Predictor.Started() )
                {
                    PlaceNetPawn( reg, entity, m_Prediction->Movement.Settings, position, ModeOf( *sample ) );
                    WriteMoveState( scene, entity, *sample );
                }
                continue;
            }
            ECS::LocalTransform local = reg.get<ECS::TransformComponent>( entity ).GetLocal();
            local.Rotation            = { 0.0f, sample->Yaw, 0.0f };
            ECS::SetLocalTransform( reg, entity, local );
            PlaceNetPawn( reg, entity, m_Prediction->Movement.Settings, position, ModeOf( *sample ) );
            WriteMoveState( scene, entity, *sample );
        }
    }

    void ClientNetDriver::WriteMoveState( Scene& scene, entt::entity pawn, const Net::PawnState& sample )
    {
        // NET-5 (§5.3): a simulated proxy's animation runs locally from its replicated movement, never from
        // replicated poses (UE: the proxy's CharacterMovement is filled by replication and its AnimInstance reads
        // it). The graph is LocomotionSystem's, as for every character.
        if ( auto* cc = scene.GetRegistry().try_get<ECS::CharacterControllerComponent>( pawn ) )
            ECS::CharacterMovement::ApplyMoveState( *cc, MoveStateOf( sample ) );
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
            // Activating = made hidden (UE MakingVisible): the server's acknowledgement reveals it.
            if ( units[unit].State == Rules::Residency::Activating ||
                 units[unit].State == Rules::Residency::Activated )
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
        for ( const std::uint32_t unit : m_Client->TakeAcknowledgedUnits() )
            m_AcknowledgedUnits.push_back( unit );
        const auto* of = scene.GetRegistry().try_ctx<WorldStreamerOfScene>();
        if ( of == nullptr || of->Streamer == nullptr )
            return; // the server names only records of cells this client reported, so a streamer made them
        of->Streamer->SetNetRevealHold( true );
        // The delta first, then the acknowledgement that reveals its cell: a destroyed record is never seen.
        if ( !m_StableDestroyed.empty() )
            of->Streamer->DestroyRecordsForNet( std::exchange( m_StableDestroyed, {} ) );
        if ( !m_AcknowledgedUnits.empty() )
            of->Streamer->AcknowledgeUnitsForNet( std::exchange( m_AcknowledgedUnits, {} ) );
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

    Net::InputStep ClientNetDriver::NextInput( Scene& scene )
    {
        const Gameplay::PawnActions& actions = m_Prediction->Movement.Actions;
        auto&                              reg     = scene.GetRegistry();
        const entt::entity                 pawn    = scene.GetPlayerPawn();
        ECS::CharacterControllerComponent* cc      = pawn != entt::null && reg.valid( pawn )
                                                          ? reg.try_get<ECS::CharacterControllerComponent>( pawn )
                                                          : nullptr;
        glm::vec3                          move( 0.0f );
        if ( m_Script )
        {
            // The scripted walk ends on the SERVER's newest position of the own pawn, not the interpolated one.
            std::optional<float> travelled;
            const auto           own = m_Client->Pawns().find( m_Client->OwnPawn() );
            if ( own != m_Client->Pawns().end() && m_OwnFirstServerPosition )
                travelled =
                     glm::length( ToGlm( own->second.History.Newest().Position ) - *m_OwnFirstServerPosition );
            const bool                               walking = m_Script->Walking();
            const Gameplay::ScriptedPawnInput::Frame frame   = m_Script->Next( m_Clock, travelled );
            if ( walking && !m_Script->Walking() )
                LOG_INFO( "[Net] walked {:.0f} cm (server position): stopping at t={:.1f}s", m_Options.WalkCm,
                          m_Clock );
            move = frame.Move;
            // The script's hands are on the player's input layer (UE: an automated run presses keys through
            // UPlayerInput::InputKey, never the movement's flags): Left Shift is held in the local player's
            // Enhanced Input exactly as a keyboard's, so the pawn's mapping context (IMC_Default: Left Shift ->
            // IA_Sprint) fires its declared Sprint action; the jump is the pawn's Jump request (UE
            // ACharacter::Jump, a script's self:jump). Both reach the step through the one builder below.
            if ( frame.Sprint != m_ScriptSprintKeyHeld )
                if ( auto* scripts = scene.FindSystem<ECS::ScriptSystem>() )
                {
                    scripts->PlayerInput().InputKey( Input::InputKey::Of( Common::KeyCode::LeftShift ),
                                                     frame.Sprint );
                    m_ScriptSprintKeyHeld = frame.Sprint;
                    LOG_INFO( "[Net] scripted walk: Left Shift {} in the player's Enhanced Input at t={:.1f}s",
                              frame.Sprint ? "held" : "released", m_Clock );
                }
            if ( frame.Jump && cc != nullptr )
                cc->JumpRequested = true;
        }
        else if ( cc != nullptr )
        {
            const glm::vec2 intent = cc->MoveInput;
            glm::vec3       forward, right;
            NetMoveBasisOfView( scene, forward, right );
            move = forward * intent.y + right * intent.x;
        }
        Net::InputStep input;
        // The one local input builder standalone Play steps with: every declared action held in the player's
        // Enhanced Input (IA_Sprint on Left Shift, ...) or requested by a script (the one-shot Jump is consumed
        // here - the physics step skips a replicated pawn), the swim axis.
        if ( cc != nullptr )
            input = Input::LocalPawnInputStep( scene, pawn, *cc, actions, move );
        input.Move = { move.x, move.y, move.z };
        input.Yaw  = glm::length( move ) > 1e-4f ? std::atan2( -move.x, -move.z ) : 0.0f;
        return input;
    }

    glm::vec3 ClientNetDriver::OwnDrawnFeet() const
    {
        const Net::MovePredictor& predictor = m_Prediction->Predictor;
        const float alpha = static_cast<float>( std::clamp( m_StepClock * Net::kNetStepHz / m_Dilation, 0.0, 1.0 ) );
        return glm::mix( ToGlm( predictor.Previous().Position ), ToGlm( predictor.Current().Position ), alpha ) +
               m_SmoothOffset;
    }

    void ClientNetDriver::ReconcileOwnPawn( Scene& scene )
    {
        Net::MovePredictor& predictor = m_Prediction->Predictor;
        if ( !predictor.Started() )
        {
            // The prediction starts from the server's spawned state (where, and in which movement mode).
            // Not before the first snapshot of it (UE: an autonomous proxy moves once its replicated movement
            // arrived), and not before this client's collision world exists to move it in.
            const auto own = m_Client->Pawns().find( m_Client->OwnPawn() );
            if ( own == m_Client->Pawns().end() || own->second.History.Empty() ||
                 PredictionMover( scene ) == nullptr )
                return;
            // The whole replicated state, the movement mode with it: the server's first move starts from it.
            const Net::PawnState spawned = own->second.History.Sample( 1e12 );
            Net::PawnMoveState   start;
            start.Position = spawned.Position;
            start.Yaw      = spawned.Yaw;
            start.Grounded = ( spawned.Movement & Net::kPawnGrounded ) != 0;
            predictor.Reset( start );
            if ( m_Script )
                m_Script->Start( m_Clock );
            LOG_INFO( "[Net] predicting own pawn net id {} from ({:.0f}, {:.0f}, {:.0f}) cm, {}", own->first,
                      start.Position.X, start.Position.Y, start.Position.Z,
                      start.Grounded ? "grounded" : "falling" );
        }
        const auto authority = m_Client->TakeOwnAuthority();
        if ( !authority )
            return;
        m_QueueWindowMin = std::min( m_QueueWindowMin, static_cast<double>( authority->InputBuffered ) );
        if ( m_Clock >= m_QueueWindowEnd )
        {
            m_ServerQueue = m_QueueWindowMin;
            m_Dilation    = 1.0 + std::clamp( ( m_ServerQueue - kQueueTarget ) * kDilationPerStep, -kMaxDilation,
                                              kMaxDilation );
            m_QueueWindowMin = 1e9;
            m_QueueWindowEnd = m_Clock + kQueueWindowSeconds;
        }
        const glm::vec3 drawnBefore = OwnDrawnFeet();
        Net::IPawnMover* mover       = PredictionMover( scene );
        if ( mover == nullptr )
            return;
        const auto outcome =
             predictor.Reconcile( authority->AckStep, authority->State, *mover, Net::kNetStepSeconds );
        if ( outcome != Net::MovePredictor::EReconcile::Corrected )
            return;
        const Net::PawnMoveState& mispredicted = predictor.LastMispredicted();
        const glm::vec3 offset = drawnBefore - ( OwnDrawnFeet() - m_SmoothOffset );
        const bool      smooth = glm::length( offset ) <= kNoSmoothDistanceCm;
        m_SmoothOffset         = smooth ? offset : glm::vec3( 0.0f );
        LOG_INFO(
             "[Net] correction {} at input step {}: the server has the own pawn at ({:.1f}, {:.1f}, {:.1f}) cm; "
             "{} unacked step(s) replayed; drawn offset {:.1f} cm {}; predicted ({:.1f}, {:.1f}, {:.1f}) v "
             "({:.0f}, "
             "{:.0f}, {:.0f}) {} vs server v ({:.0f}, {:.0f}, {:.0f}) {}; input move ({:.2f}, {:.2f}) actions {}",
             predictor.Corrections(), authority->AckStep, authority->State.Position.X, authority->State.Position.Y,
             authority->State.Position.Z, predictor.Unacked(), glm::length( offset ),
             smooth ? "smoothed" : "beyond the smoothing distance - drawn at once", mispredicted.Position.X,
             mispredicted.Position.Y, mispredicted.Position.Z, mispredicted.Velocity.X, mispredicted.Velocity.Y,
             mispredicted.Velocity.Z, mispredicted.Grounded ? "grounded" : "falling", authority->State.Velocity.X,
             authority->State.Velocity.Y, authority->State.Velocity.Z,
             authority->State.Grounded ? "grounded" : "falling", predictor.LastMispredictedInput().Move.X,
             predictor.LastMispredictedInput().Move.Z,
             m_Prediction->Movement.Actions.Describe( predictor.LastMispredictedInput() ) );
    }

    void ClientNetDriver::Shutdown( Scene& scene )
    {
        StatsSlot().reset();
        for ( const auto& [id, entity] : m_Entities )
            if ( entity != scene.GetPlayerPawn() )
                scene.RemoveRemotePlayer( entity );
        m_Entities.clear();
        m_Client.reset();
        m_Transport.reset();
        LOG_INFO( "[Net] Play as client ended" );
    }
} // namespace Desert::Core
