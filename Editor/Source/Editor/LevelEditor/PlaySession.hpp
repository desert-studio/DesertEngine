#pragma once

// PLAY IN EDITOR (UE: UEditorEngine::PlayInEditor / EndPlayMap, FPlayWorldCommands).
//
// Play / Play from Here / Pause / Resume / Next Frame / Stop on the workspace's ACTIVE scene; from Play to
// Stop the session holds that scene itself, whichever document is active (UE: UEditorEngine::PlayWorld); the authored
// snapshot Stop restores; the world streamer of a partitioned world in Play; the toolbar's playback group and
// the palette's Play entries. A member of EditorLayer BY VALUE, declared after the SceneWorkspace it plays.

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Net/ClientNetDriver.hpp>
#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/PlayWorldCommands.hpp"
#include "Editor/Core/PlayWorldTravel.hpp"
#include "Editor/Widgets/ToolbarLayout.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <Common/Core/Timestep.hpp>

namespace Desert::Core
{
    class Scene; // Scene.hpp: the .cpp that walks the scene includes it
} // namespace Desert::Core

namespace Desert::Editor
{
    class SceneWorkspace;

    class PlaySession
    {
    public:
        PlaySession( SceneWorkspace& workspace, const std::shared_ptr<Assets::AssetManager>& assets );

        void Play( bool fromHere = false, const std::string& playerStartTag = {} );
        // UE's Simulate In Editor: the same session as Play (snapshot, streamer, Stop) with no player - no pawn
        // is spawned or possessed, the editor camera and the editor's overlays stay (PlayRequest::Simulate).
        void Simulate();
        // Restores the authored snapshot. Called between frames only — see RequestStop.
        void Stop();
        // The editor is closing (EditorLayer::OnDetach): what Play holds INTO the played world - the server
        // session, whose predicted pawn is a character in that world's physics, and the world streamer - goes now,
        // before the workspace tears the world down (UE: a world's NetDriver is destroyed before the world,
        // UWorld::DestroyWorld). No snapshot restore: nothing is left to edit.
        void EndForExit();
        // The one executor the toolbar, the palette and the control channel share.
        [[nodiscard]] Common::BoolResultStr Run( PlayWorldCommand command );

        // Stop destroys and recreates render resources, so it is deferred to OnUpdate.
        void RequestStop()
        {
            m_PendingStop = true;
        }
        [[nodiscard]] bool HasPendingRequests() const
        {
            return m_PendingStop;
        }
        void ServiceRequests();

        // The editor's frame boundary for the world's travel (PlayWorldTravel): in Play the queued level is loaded
        // into the PLAYED world and Stop still restores the authored one; outside Play the request is refused.
        // Every document has its own queue; one that is not playing refuses its request. False: nothing was pending.
        [[nodiscard]] Common::BoolResultStr ServiceTravel();

        // The document about to close, or to have its level replaced, is the one playing: Play ends with it, its
        // snapshot is discarded and the world is back in edit mode.
        void EndIfBoundTo( uint64_t sceneViewId );

        [[nodiscard]] bool InPlayMode() const
        {
            return m_State == State::Play;
        }
        [[nodiscard]] const std::string& AuthoredSnapshot() const
        {
            return m_PlayWorld.AuthoredSnapshot();
        }
        [[nodiscard]] Desert::Core::WorldStreamer* Streamer() const
        {
            return m_WorldStreamer.get();
        }

        // Ticks the streamer for @p scene before its systems run; true while Play's time must hold because
        // streaming waits for the cell under the camera (WP12, decision O2).
        [[nodiscard]] bool TickStreaming( Desert::Core::Scene& scene, const Common::Timestep& ts );
        [[nodiscard]] std::vector<::Desert::Core::Rules::StreamingSource> InstrumentStreamingSources() const;

        void DrawPlaybackGroup( const ToolbarLayout::PlaybackGroup& group, float y );
        void AppendPlayCommands( std::vector<PaletteCommand>& commands );

    private:
        enum class StartKind
        {
            Play,
            PlayFromHere,
            Simulate,
        };
        // The scene this session plays; the active one while nothing plays. What a command acts on.
        [[nodiscard]] std::shared_ptr<Desert::Core::Scene> CommandScene() const;
        void Start( StartKind kind, const std::string& playerStartTag );

        // Tears the played world down and plays the level at @p path in it (Runtime's LoadSceneInternal, with
        // the editor's scene): read and version-gated before anything is destroyed, then cleared, loaded,
        // initialised, Play begun and the world streamer started on the new level.
        [[nodiscard]] Common::BoolResultStr LoadIntoPlayWorld( const std::string& path );
        // A Play refused after the world was completed from disk: the scene goes back to the edit world's
        // snapshot and packages.
        void RestoreEditWorld( const std::string& snapshot );
        // The session with the server ends while the world it plays in still exists: its remote pawns leave the
        // played scene's players and its predicted pawn's character leaves that scene's physics world. Every end
        // of Play (Stop, travel, the playing view closed, the editor closing) goes through here.
        void EndNetClient();

        enum class State
        {
            Paused = 0,
            Play,
        };

        SceneWorkspace&                              m_Workspace;
        const std::shared_ptr<Assets::AssetManager>& m_Assets;
        State                                        m_State = State::Paused;
        PlayWorldTravel                              m_PlayWorld; // the authored snapshot + the travelled map
        std::optional<Desert::Core::EntityPackages>  m_EditPackages; // the edit world's, from Play to Stop
        std::weak_ptr<Desert::Core::Scene>           m_PlayScene; // from Play to Stop: the document owns it
        uint64_t                                     m_PlayViewId = 0; // its view (SceneWorkspace::ActiveSceneId)
        std::string                                  m_EditFilePath; // its file, from Play to Stop
        bool                                         m_PendingStop = false;
        std::unique_ptr<Desert::Core::WorldStreamer> m_WorldStreamer;
        // Play as a network client (--connect): the session with the server, for the whole Play.
        std::unique_ptr<Desert::Core::ClientNetDriver> m_NetClient;
        double                                       m_WorldStreamClock = 0.0; // seconds of Play, for retries
    };
} // namespace Desert::Editor
