#pragma once

// PLAY IN EDITOR (UE: UEditorEngine::PlayInEditor / EndPlayMap, FPlayWorldCommands).
//
// Play / Play from Here / Pause / Resume / Next Frame / Stop on the workspace's ACTIVE scene; the authored
// snapshot Stop restores; the world streamer of a partitioned world in Play; the toolbar's playback group and
// the palette's Play entries. A member of EditorLayer BY VALUE, declared after the SceneWorkspace it plays.

#include <Engine/Desert.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/PlayWorldCommands.hpp"
#include "Editor/Widgets/ToolbarLayout.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Desert::Editor
{
    class SceneWorkspace;

    class PlaySession
    {
    public:
        PlaySession( SceneWorkspace& workspace, const std::shared_ptr<Assets::AssetManager>& assets );

        void Play( bool fromHere = false, const std::string& playerStartTag = {} );
        // Restores the authored snapshot. Called between frames only — see RequestStop.
        void Stop();
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

        // The document about to close is the one playing: Play ends with it and its snapshot is discarded.
        void EndIfBoundTo( uint64_t sceneViewId );

        [[nodiscard]] bool InPlayMode() const
        {
            return m_State == State::Play;
        }
        [[nodiscard]] const std::string& AuthoredSnapshot() const
        {
            return m_Snapshot;
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
        enum class State
        {
            Paused = 0,
            Play,
        };

        SceneWorkspace&                              m_Workspace;
        const std::shared_ptr<Assets::AssetManager>& m_Assets;
        State                                        m_State = State::Paused;
        std::string                                  m_Snapshot; // serialized scene captured on Play
        bool                                         m_PendingStop = false;
        std::unique_ptr<Desert::Core::WorldStreamer> m_WorldStreamer;
        double                                       m_WorldStreamClock = 0.0; // seconds of Play, for retries
    };
} // namespace Desert::Editor
