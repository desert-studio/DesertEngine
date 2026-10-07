#include "Editor/LevelEditor/PlaySession.hpp"

#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/ShotOptions.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <Common/Core/Profiler.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneLoadPhases.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/ECS/Components.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        // One slot of the segmented playback group. Drawn by hand rather than with ImGui::Button because a
        // segmented group rounds only its OUTER corners, which a Button cannot, and because a disabled slot must
        // keep its place and its fill (dimmed) — the group's shape is the same in every state.
        bool PlaybackSlotButton( const char* id, const char* glyph, const ImVec2& pos, const ImVec2& size,
                                 ImDrawFlags corners, const ImVec4& fill, const ImVec4& glyphColour, bool enabled,
                                 const char* tooltip )
        {
            namespace ImGui = ::ImGui;

            ImGui::SetCursorScreenPos( pos );
            if ( !enabled )
                ImGui::BeginDisabled();
            const bool clicked = ImGui::InvisibleButton( id, size );
            const bool hovered = enabled && ImGui::IsItemHovered();
            const bool held    = enabled && ImGui::IsItemActive();
            if ( tooltip != nullptr && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                ImGui::SetTooltip( "%s", tooltip );
            if ( !enabled )
                ImGui::EndDisabled();

            float lift = 0.0f; // pressed darkens, hover lifts, rest is the fill as authored
            if ( held )
                lift = -0.06f;
            else if ( hovered )
                lift = 0.08f;
            const float  alpha = enabled ? 1.0f : 0.45f;
            const auto   shade = [&]( float c ) { return std::clamp( c + lift, 0.0f, 1.0f ); };
            ImDrawList*  draw  = ImGui::GetWindowDrawList();
            const ImVec2 max( pos.x + size.x, pos.y + size.y );
            draw->AddRectFilled(
                 pos, max,
                 ImGui::GetColorU32( ImVec4( shade( fill.x ), shade( fill.y ), shade( fill.z ), fill.w * alpha ) ),
                 ImGui::GetStyle().FrameRounding, corners );

            const ImVec2 textSize = ImGui::CalcTextSize( glyph );
            draw->AddText(
                 ImVec2( pos.x + ( size.x - textSize.x ) * 0.5f, pos.y + ( size.y - textSize.y ) * 0.5f ),
                 ImGui::GetColorU32(
                      ImVec4( glyphColour.x, glyphColour.y, glyphColour.z, glyphColour.w * alpha ) ),
                 glyph );
            return clicked && enabled;
        }
    } // namespace

    PlaySession::PlaySession( SceneWorkspace& workspace, const std::shared_ptr<Assets::AssetManager>& assets )
         : m_Workspace( workspace ), m_Assets( assets )
    {
    }

    void PlaySession::Play( bool fromHere, const std::string& playerStartTag )
    {
        using SceneState  = ::Desert::Core::Scene::SceneState;
        const auto& scene = m_Workspace.ActiveScene();
        if ( scene->GetState() != SceneState::Edit )
            return;
        // Snapshot the authored scene so Stop can restore it exactly (play-time edits are discarded). Timed
        // like a load: it is the other half of the round trip Stop pays.
        Desert::Core::SceneLoadPhases       phases( "Play start" );
        const Desert::Core::SceneSerializer serializer( scene.get(), m_Assets.get() );
        m_Snapshot = serializer.SerializeToJson();
        phases.Lap( "write the Play snapshot", scene->GetAllEntities().size() );
        // The pawn is spawned AFTER the snapshot, so Stop's restore has never heard of it, and BEFORE the
        // streamer, which may unload the cell the PlayerStart stands in.
        Desert::Core::PlayRequest request;
        request.PlayerStartTag = playerStartTag;
        if ( fromHere && scene->GetActiveCamera() )
        {
            // Stand where the editor camera is, facing where it faces - yaw only.
            const glm::mat4 camWorld = glm::inverse( scene->GetActiveCamera()->GetViewMatrix() );
            const glm::vec3 forward  = -glm::vec3( camWorld[2] );
            const float     yaw      = std::atan2( -forward.x, -forward.z );
            request.SpawnAt          = glm::translate( glm::mat4( 1.0f ), glm::vec3( camWorld[3] ) ) *
                              glm::rotate( glm::mat4( 1.0f ), yaw, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        }
        if ( const auto began = Desert::Core::BeginPlay( *scene, *m_Assets, request ); !began )
        {
            LOG_ERROR( "[Scene] Play refused: {0}", began.GetError() );
            Editor::ToastManager::Push( std::format( "Play refused: {}", began.GetError() ),
                                        Editor::ToastLevel::Error );
            m_Snapshot.clear();
            return;
        }
        phases.Lap( "spawn the player's pawn", scene->GetAllEntities().size() );
        auto streamer =
             Desert::Core::WorldStreamer::Begin( *scene, *m_Assets, m_Snapshot, InstrumentStreamingSources() );
        phases.Lap( "begin the world streamer", scene->GetAllEntities().size() );
        phases.LogSummary();
        if ( !streamer )
        {
            LOG_ERROR( "[Scene] Play refused: {0}", streamer.GetError() );
            Editor::ToastManager::Push( "Play refused: the world could not stream — see the log",
                                        Editor::ToastLevel::Error );
            // BeginPlay already spawned and entered Play; the refusal takes both back.
            if ( const entt::entity pawn = scene->GetPlayerPawn(); pawn != entt::null )
                scene->DestroyEntity( ECS::Entity( pawn, scene->GetRegistry() ) );
            scene->SetPlayerPawn( entt::null );
            scene->SetPlayFromHere( false );
            scene->SetState( SceneState::Edit );
            m_Snapshot.clear();
            return;
        }
        m_WorldStreamer    = streamer.ExtractValue();
        m_WorldStreamClock = 0.0;
#if DESERT_DEV_INSTRUMENTS
        if ( m_WorldStreamer && ShotOptions::Get().StreamDelayTicks > 0 )
        {
            m_WorldStreamer->SetDebugLoadDelayTicks( ShotOptions::Get().StreamDelayTicks );
            LOG_WARN( "[WorldPartition] DEV: every cell read is reported {0} frame(s) late (--stream-delay-ticks)",
                      ShotOptions::Get().StreamDelayTicks );
        }
#endif
        // An undo stack recorded against the authored scene must not fire into Play or the restored state.
        CommandHistory::Get().Clear();
        m_State = State::Play;
    }

    void PlaySession::Stop()
    {
        using SceneState  = ::Desert::Core::Scene::SceneState;
        const auto& scene = m_Workspace.ActiveScene();
        if ( scene->GetState() == SceneState::Edit || m_Snapshot.empty() )
            return;

        Desert::Core::SceneLoadPhases phases( "Stop restore" );
        EngineContext::GetInstance().GetDevice()->WaitIdle();
        CommandHistory::Get().Clear(); // anything recorded during Play targets entities about to be rebuilt
        m_WorldStreamer.reset();       // before Clear: the snapshot below brings every cell back
        phases.Lap( "wait for the GPU, drop the undo history and the streamer", 0 );
        const std::size_t outgoing = scene->GetAllEntities().size();
        scene->Clear();
        phases.Lap( "clear the played scene", outgoing );

        const Desert::Core::SceneSerializer serializer( scene.get(), m_Assets.get() );
        // NOT A FILE: named "<Play snapshot>" so a failure says which of the two "loading a scene" broke.
        if ( const auto restored = serializer.DeserializeFromJson( m_Snapshot, "<Play snapshot>" ); !restored )
        {
            LOG_ERROR( "[Scene] Play snapshot could not be restored: {0}", restored.GetError() );
            Editor::ToastManager::Push( "Play snapshot could not be restored — see the log",
                                        Editor::ToastLevel::Error );
        }
        const std::size_t incoming = scene->GetAllEntities().size();
        phases.Lap( "deserialize the snapshot (its own phases are logged above)", incoming );
        if ( const auto inited = scene->Init(); !inited.IsSuccess() )
        {
            LOG_ERROR( "[EditorLayer] scene failed to initialise after Play: {}", inited.GetError() );
            Editor::ToastManager::Push( "Scene could not be initialised after Play — see the log",
                                        Editor::ToastLevel::Error );
        }
        phases.Lap( "initialise the scene", incoming );

        m_Workspace.ActiveSceneReplaced();
        phases.Lap( "rebuild the render registry, drop the old world's selection", incoming );
        phases.LogSummary();

        scene->SetState( SceneState::Edit );
        // The session ends with the world: without this the editor kept reporting Play (MCP state "playing")
        // after every Stop — only a closed scene view used to end it (EndIfBoundTo).
        m_State = State::Paused;
        m_Snapshot.clear();
    }

    void PlaySession::ServiceRequests()
    {
        if ( m_PendingStop )
        {
            m_PendingStop = false;
            Stop();
        }
    }

    void PlaySession::EndIfBoundTo( uint64_t sceneViewId )
    {
        // The snapshot Stop would restore is of a scene about to cease existing, and Stop acts on whatever
        // document is active — leaving the state alone would strand the editor in Play.
        if ( m_State != State::Play || m_Workspace.ActiveSceneId() != sceneViewId )
            return;
        LOG_INFO( "[Editor] Scene view #{} was playing when it was closed — play mode ends with it and "
                  "its snapshot is discarded.",
                  sceneViewId );
        m_State       = State::Paused;
        m_PendingStop = false;
        m_Snapshot.clear();
        m_WorldStreamer.reset();
    }

    bool PlaySession::TickStreaming( Desert::Core::Scene& scene, const Common::Timestep& ts )
    {
        // BEFORE the systems run, so no system sees an entity whose cell has just left.
        if ( m_WorldStreamer && m_WorldStreamer->Streams( scene ) )
        {
            DESERT_PROFILE_SCOPE( "WorldStreamer::Tick" );
            m_WorldStreamClock += ts.GetSeconds();
            if ( auto streamed = m_WorldStreamer->Tick( m_WorldStreamClock, InstrumentStreamingSources() );
                 !streamed )
            {
                // The world stays as it is now, and Play goes on in it; streaming does not.
                LOG_ERROR( "[Scene] world streaming stopped: {0}", streamed.GetError() );
                Editor::ToastManager::Push( "World streaming stopped — see the log", Editor::ToastLevel::Error );
                m_WorldStreamer.reset();
            }
        }
        return m_WorldStreamer && m_WorldStreamer->Streams( scene ) && m_WorldStreamer->BlocksPlay();
    }

    std::vector<::Desert::Core::Rules::StreamingSource> PlaySession::InstrumentStreamingSources() const
    {
        const auto& scene = m_Workspace.ActiveScene();
        const auto* camera =
             scene ? dynamic_cast<::Desert::Core::EditorCamera*>( scene->GetActiveCamera().get() ) : nullptr;
        if ( !ShotOptions::Get().FlightRoute || camera == nullptr )
            return {};
        ::Desert::Core::Rules::StreamingSource source;
        source.Position = camera->GetPosition();
        return { source };
    }

    Common::BoolResultStr PlaySession::Run( const PlayWorldCommand command )
    {
        using SceneState       = ::Desert::Core::Scene::SceneState;
        using Command          = PlayWorldCommand;
        const auto&      scene = m_Workspace.ActiveScene();
        const SceneState state = scene->GetState();
        switch ( command )
        {
            case Command::Play:
            case Command::PlayFromHere:
                if ( state == SceneState::Edit )
                    Play( /*fromHere=*/command == Command::PlayFromHere );
                return PaletteCommandOutcome( state == SceneState::Edit && scene->GetState() != SceneState::Edit,
                                              "the scene is not playing; either it was already playing or "
                                              "Play refused (the log says why)." );
            case Command::Pause:
                if ( state == SceneState::Play )
                    scene->SetState( SceneState::Paused );
                return PaletteCommandOutcome( state == SceneState::Play, state == SceneState::Paused
                                                                              ? "the world is already paused."
                                                                              : "nothing is playing, so there is "
                                                                                "nothing to pause." );
            case Command::Resume:
                if ( state == SceneState::Paused )
                    scene->SetState( SceneState::Play );
                return PaletteCommandOutcome( state == SceneState::Paused, "the world is not paused, so there "
                                                                           "is nothing to resume." );
            case Command::NextFrame:
                // Consumed by the next Scene::OnUpdate: the world advances one frame and holds again.
                return PaletteCommandOutcome( scene->RequestSingleFrame(),
                                              "the world is not paused; Next Frame steps a PAUSED world." );
            case Command::Stop:
                // Deferred to OnUpdate (between frames) — see RequestStop.
                if ( state != SceneState::Edit )
                    RequestStop();
                return PaletteCommandOutcome( state != SceneState::Edit, "nothing is playing, so there is "
                                                                         "nothing to stop." );
        }
        return Common::MakeError( "unknown play-world command" );
    }

    void PlaySession::DrawPlaybackGroup( const ToolbarLayout::PlaybackGroup& group, float y )
    {
        namespace ImGui  = ::ImGui;
        using SceneState = ::Desert::Core::Scene::SceneState;
        using Slot       = ToolbarLayout::PlaybackSlot;

        // UE's Level Editor group: Play | Options | Pause | Next Frame | Stop. Every slot is present in every
        // state and the ones that do not apply are greyed out, so the group never changes shape or position.
        const SceneState state   = m_Workspace.ActiveScene()->GetState();
        const bool       editing = state == SceneState::Edit;
        const bool       paused  = state == SceneState::Paused;

        const float  h       = ImGui::GetFrameHeight();
        const ImVec4 accent  = ThemeManager::GetSelectedColor();
        const ImVec4 neutral = ImGui::GetStyleColorVec4( ImGuiCol_Button );
        const ImVec4 white( 1.0f, 1.0f, 1.0f, 1.0f );
        const ImVec4 icon = ThemeManager::GetIconColor();

        const auto at   = [&]( Slot slot ) { return ImVec2( group[slot].X, y ); };
        const auto size = [&]( Slot slot ) { return ImVec2( group[slot].Width, h ); };

        using Command  = PlayWorldCommand;
        const auto run = [this]( const Command command )
        {
            // A refusal the button could not have offered is still logged, never swallowed.
            if ( const auto outcome = Run( command ); !outcome )
                LOG_ERROR( "[Toolbar] {}: {}", CommandInfo( command ).Label, outcome.GetError() );
        };

        if ( PlaybackSlotButton( "##Play", ICON_MDI_PLAY, at( Slot::Play ), size( Slot::Play ),
                                 ImDrawFlags_RoundCornersLeft, editing ? accent : neutral, editing ? white : icon,
                                 editing, editing ? "Play (the pawn spawns at the PlayerStart)" : "Playing" ) )
            run( Command::Play );

        if ( PlaybackSlotButton( "##PlayOptions", ICON_MDI_CHEVRON_DOWN, at( Slot::Options ),
                                 size( Slot::Options ), ImDrawFlags_RoundCornersNone, editing ? accent : neutral,
                                 editing ? white : icon, editing, "Play options" ) )
            ImGui::OpenPopup( "PlayModes" );
        ImGui::SetNextWindowPos( ImVec2( group[Slot::Play].X, y + h ) );
        if ( ImGui::BeginPopup( "PlayModes" ) )
        {
            if ( ImGui::MenuItem( ICON_MDI_PLAY " Play", nullptr, false ) )
                run( Command::Play );
            if ( ImGui::MenuItem( ICON_MDI_CAMERA " Play from Here", nullptr, false ) )
                run( Command::PlayFromHere );
            ImGui::EndPopup();
        }

        // One slot, two commands (UE shows Pause while running and Resume while paused).
        const Command pauseOrResume = paused ? Command::Resume : Command::Pause;
        if ( PlaybackSlotButton( "##Pause", paused ? ICON_MDI_PLAY : ICON_MDI_PAUSE, at( Slot::Pause ),
                                 size( Slot::Pause ), ImDrawFlags_RoundCornersNone, paused ? accent : neutral,
                                 paused ? white : icon, !editing,
                                 std::string( CommandInfo( pauseOrResume ).Label ).c_str() ) )
            run( pauseOrResume );

        if ( PlaybackSlotButton( "##NextFrame", ICON_MDI_STEP_FORWARD, at( Slot::NextFrame ),
                                 size( Slot::NextFrame ), ImDrawFlags_RoundCornersNone, neutral, icon, paused,
                                 paused ? "Next Frame (advance the paused world by one frame)"
                                        : "Next Frame (pause the world first)" ) )
            run( Command::NextFrame );

        if ( PlaybackSlotButton( "##Stop", ICON_MDI_STOP, at( Slot::Stop ), size( Slot::Stop ),
                                 ImDrawFlags_RoundCornersRight, neutral,
                                 editing ? icon : ThemeManager::GetErrorColor(), !editing, "Stop" ) )
            run( Command::Stop );

        // Leave the cursor on the group's row, after its last slot, as a SameLine chain would.
        ImGui::SetCursorScreenPos( ImVec2( group[Slot::Stop].X + group[Slot::Stop].Width, y ) );
    }

    void PlaySession::AppendPlayCommands( std::vector<PaletteCommand>& commands )
    {
        // THE PLAY SESSION, the toolbar group's slots (UE FPlayWorldCommands). Separate entries rather than
        // toggles, so a script that asks for Stop is told when there was nothing playing. Same executor as
        // the toolbar.
        for ( const PlayWorldCommand command : kPlayWorldCommandOrder )
            commands.push_back( { std::string( CommandInfo( command ).Context ),
                                  std::string( CommandInfo( command ).Label ),
                                  [this, command] { return Run( command ); } } );
        // Play at one TAGGED start - the control channel's spelling of `--player-start <tag>`.
        auto& registry = m_Workspace.ActiveScene()->GetRegistry();
        for ( const auto entity : registry.view<ECS::PlayerStartComponent>() )
        {
            const std::string tag = registry.get<ECS::PlayerStartComponent>( entity ).Data.Tag;
            if ( tag.empty() )
                continue;
            commands.push_back(
                 { "Action", std::format( "Play at Player Start '{}'", tag ), [this, entity]
                   {
                       // Captured by ENTITY: an entry outliving the start refuses.
                       const auto& scene = m_Workspace.ActiveScene();
                       auto&       reg   = scene->GetRegistry();
                       const auto* start =
                            reg.valid( entity ) ? reg.try_get<ECS::PlayerStartComponent>( entity ) : nullptr;
                       if ( start == nullptr )
                           return PaletteCommandOutcome( false, "that Player Start no longer exists." );
                       using SceneState   = ::Desert::Core::Scene::SceneState;
                       const bool editing = scene->GetState() == SceneState::Edit;
                       if ( editing )
                           Play( /*fromHere=*/false, std::string( start->Data.Tag ) );
                       return PaletteCommandOutcome( editing && scene->GetState() != SceneState::Edit,
                                                     "the scene is not playing; either it was already playing "
                                                     "or Play refused (the log says why)." );
                   } } );
        }
    }
} // namespace Desert::Editor
