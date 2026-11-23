#pragma once

// THE CLIENT'S NET DRIVER (NET-ARCH §2, §5; UE UNetDriver + UPendingNetGame on a client world): the played world
// becomes a network client of a DesertServer. It owns the transport and the ReplicationClient (Desert/Net, the
// same session object the headless bot runs) and maps the replicated pawns onto the Scene:
//   * the own pawn (Owned) is the local player's pawn - Scene::GetPlayerPawn - tagged Autonomous;
//   * every other player's pawn is the level's Default Pawn prefab spawned for it (SpawnPawnPrefabAt), tagged
//     Simulated and added to Scene::GetPlayers as a remote player; destroyed when the server destroys it.
// Every frame: pump, apply spawns/destroys, write each pawn's transform from the replicated state (Sample), and
// send this client's input on the fixed 60 Hz net step - the own pawn's CharacterControllerComponent::MoveInput
// resolved against the view (the same basis PhysicsECSSystem uses), or the scripted walk (--net-walk-cm, the
// headless proof). NET-4: the own pawn is PREDICTED - every input step is simulated here as it is sent (Net::
// MovePredictor, the server runs the same Net::StepPawnMove) and reconciled against the server's acked state.

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Common/Core/ResultStr.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <string>
#include <string_view>

#include <entt/entt.hpp>

namespace Desert::Net
{
    class INetTransport;
    class ReplicationClient;
    struct PawnState;
} // namespace Desert::Net

namespace Desert::Core
{
    class Scene;

    struct ClientNetOptions
    {
        std::string Address;  // --connect ip:port
        std::string TokenHex; // --net-token: the login backend's connect token (DesertNetProbe --issue-token)
        std::string Cooked;   // --net-cooked: the cooked world folder whose World.dwindex hash the server checks
        // --net-walk-cm D --net-walk-dir x,y,z: the scripted input of a headless run - walk until the SERVER's
        // position of the own pawn has moved D cm, then stand (DesertNetProbe --bot's walk).
        float     WalkCm = 0.0f;
        glm::vec3 WalkDir{ 1.0f, 0.0f, 0.0f };
        // --net-walk-mode sprint / --net-jump-every S: the scripted walk sprints, and jumps every S seconds.
        bool      WalkSprint           = false;
        float     WalkJumpEverySeconds = 0.0f;
        // --net-lag-ms / --net-loss: the transport's simulated link (GNS FakePacketLag/Loss).
        int   LagMs       = 0;
        float LossPercent = 0.0f;

        [[nodiscard]] bool Enabled() const
        {
            return !Address.empty();
        }
        // The process's launch options (the editor's / the game's command line sets them before the first frame;
        // Play reads them when it begins, like UE's PIE net mode read at StartPlayInEditorSession).
        static ClientNetOptions& Launch()
        {
            static ClientNetOptions options;
            return options;
        }
    };

    // The own pawn's prediction as the client sees it (the viewport HUD's "Net" row; UE stat net's corrections).
    struct NetPredictionStats
    {
        std::uint32_t Corrections   = 0; // server corrections taken this session (ClientAdjustPosition)
        std::size_t   Unacked       = 0; // predicted input steps the server has not acknowledged yet
        double        ClockDilation = 1.0; // the input step's rate against real time (clock sync)
    };

    class ClientNetDriver
    {
    public:
        // The running session's prediction, nullopt while no client session runs (like MemoryWatch::LastBindless).
        [[nodiscard]] static std::optional<NetPredictionStats> LastStats();
        // Connects (asynchronously: the session begins on a later Tick). Refuses a malformed token, a cooked world
        // without its index, or an address the transport cannot use.
        static Common::ResultStr<std::unique_ptr<ClientNetDriver>> Connect( const ClientNetOptions& options );
        ~ClientNetDriver();

        // Once per frame while Play runs, BEFORE the systems (so physics and the renderer see this frame's poses).
        // The session's first Tick loads the server's pawn prefab into `assets` (UE: the client loads the
        // GameMode's DefaultPawnClass package before it spawns the pawns replicated to it).
        void Tick( Scene& scene, Assets::AssetManager& assets, double deltaSeconds );
        // Play stops: the remote pawns leave the Scene's players (Stop's restore drops their entities).
        void Shutdown( Scene& scene );

    private:
        ClientNetDriver() = default;
        void      LoadPawnPrefab( Assets::AssetManager& assets );
        void      RequestPawnClosure();
        bool      PawnClosureLanded();
        void      ApplySpawnsAndDestroys( Scene& scene, const Assets::AssetManager& assets );
        void      WriteTransforms( Scene& scene );
        // NET-5: a remote pawn's locomotion variables from its interpolated replicated movement - the proxy's
        // CharacterControllerComponent Velocity/CurrentSpeed and the anim graph parameters kLocomotion* of every
        // AnimationComponent in its hierarchy.
        void      WriteLocomotion( Scene& scene, entt::entity pawn, const Net::PawnState& sample );
        static constexpr std::string_view kLocomotionSpeed   = "Speed";     // planar cm/s
        static constexpr std::string_view kLocomotionFalling = "IsFalling"; // not on the ground
        // NET-2: the server's state of stable objects (cooked records, never spawned) written into the entities
        // this client's streamer made from the same records; a change whose entity is not streamed in waits for
        // it.
        void      ApplyStableState( Scene& scene );
        // NET-3 (§4.3): the WP cell units this client's streamer has Activated, reported to the server as changes
        // (it replicates only what stands in them); and the server's destroyed stable records (§3.3), handed to
        // the streamer, which destroys them and never makes them again.
        void      ReportResidency( Scene& scene );
        void      ApplyStableDestroyed( Scene& scene );
        glm::vec3 InputMove( Scene& scene );
        std::uint8_t InputButtons( Scene& scene );
        // NET-4 (§5.1): the server's newest state for the own pawn against the saved moves; a correction moves the
        // simulation at once and the drawn pawn by a decaying offset (UE smoothed ClientAdjustPosition).
        void         ReconcileOwnPawn();
        [[nodiscard]] glm::vec3 OwnDrawnPosition() const;

        ClientNetOptions                        m_Options;
        std::unique_ptr<Net::INetTransport>     m_Transport;
        std::unique_ptr<Net::ReplicationClient> m_Client;
        std::map<std::uint32_t, entt::entity>   m_Entities; // NetId -> pawn root
        struct PendingStable; // the changes whose entity is not resident yet (ClientNetDriver.cpp)
        std::shared_ptr<PendingStable>          m_PendingStable;
        std::set<std::uint32_t>                 m_ReportedUnits;   // what the server was last told is resident
        std::vector<std::uint64_t>              m_StableDestroyed; // waiting for the streamer to exist
        std::vector<std::uint32_t> m_AcknowledgedUnits;            // the server's cell acknowledgements, likewise
        std::optional<glm::vec3>                m_OwnFirstServerPosition;
        bool                                    m_Walking   = false;
        bool                                    m_InSession = false;
        // The server's pawn prefab as THIS client's AssetManager knows it (LoadPawnPrefab); null until it loaded.
        Assets::AssetHandle                     m_PawnPrefab = Assets::AssetHandle::Null();
        // The prefab file's own read, on the loader's workers (UE FStreamableManager::RequestAsyncLoad of the
        // DefaultPawnClass): pawn spawns wait while it is in flight; destroying the driver releases it.
        Assets::LoadRequest m_PawnPrefabRead;
        bool                m_PawnPrefabReading = false;
        // The pawn prefab's dependency closure, read on the loader's workers from the Welcome on; pawn spawns wait
        // in m_AwaitingSpawn until it has landed (UE FStreamableManager: a replicated actor's class and its
        // references are streamed before the spawn, never loaded inside a frame). ClientNetDriver.cpp.
        struct PawnClosure;
        std::shared_ptr<PawnClosure>            m_PawnClosure;
        std::vector<std::uint32_t>              m_AwaitingSpawn; // NetIds the server spawned, not yet built here
        bool m_Closed = false;
        double                                  m_StepClock = 0.0, m_Clock = 0.0, m_NextLog = 1.0;
        // The own pawn's prediction (NET-4): its saved moves, the movement both sides run, the drawn offset a
        // correction left, and the clock sync (the server's input queue, averaged, and this client's step rate).
        // (Desert/Net is private to the engine: the predictor lives in ClientNetDriver.cpp.)
        struct Prediction;
        std::shared_ptr<Prediction> m_Prediction;
        glm::vec3                   m_SmoothOffset{ 0.0f };
        double                      m_ServerQueue = 2.0, m_Dilation = 1.0, m_NextJump = 0.0;
    };
} // namespace Desert::Core
