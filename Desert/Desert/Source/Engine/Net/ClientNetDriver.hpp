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
// headless proof). Input keeps the step + ack path (InputRing) NET-4 replays from; nothing here predicts yet.

#include <Engine/Assets/AssetManager.hpp>
#include <Common/Core/ResultStr.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <entt/entt.hpp>

namespace Desert::Net
{
    class INetTransport;
    class ReplicationClient;
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

    class ClientNetDriver
    {
    public:
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
        void      ApplySpawnsAndDestroys( Scene& scene, const Assets::AssetManager& assets );
        void      WriteTransforms( Scene& scene );
        glm::vec3 InputMove( Scene& scene );

        ClientNetOptions                        m_Options;
        std::unique_ptr<Net::INetTransport>     m_Transport;
        std::unique_ptr<Net::ReplicationClient> m_Client;
        std::map<std::uint32_t, entt::entity>   m_Entities; // NetId -> pawn root
        std::optional<glm::vec3>                m_OwnFirstServerPosition;
        bool                                    m_Walking   = false;
        bool                                    m_InSession = false;
        // The server's pawn prefab as THIS client's AssetManager knows it (LoadPawnPrefab); null until it loaded.
        Assets::AssetHandle m_PawnPrefab = Assets::AssetHandle::Null();
        bool m_Closed = false;
        double                                  m_StepClock = 0.0, m_Clock = 0.0, m_NextLog = 1.0;
    };
} // namespace Desert::Core
