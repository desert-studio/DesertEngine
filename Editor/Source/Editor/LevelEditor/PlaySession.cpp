#include <Destruction/Public/DestructionModule.hpp>
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/Panels/WorldPartition/EditorHLODs.hpp"

#include <Engine/GameplayDebugger/GameplayDebuggerCategoryWorldPartition.hpp>

#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/PlayKeptChanges.hpp"
#include "Editor/Core/Selection/SelectionManager.hpp"
#include "Editor/Core/ShotOptions.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <Common/Core/Profiler.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/SceneLoadPhases.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Destruction/Public/DestructionFieldComponents.hpp>
#include <Destruction/Public/DestructionFields.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <Engine/ECS/PlayerStartComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>
#include <Engine/GameplayDebugger/GameplayDebugger.hpp>
#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/Common.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <glm/glm.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
#include <Common/Json/Document.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <optional>
#include <utility>
#include <vector>
#include <Common/Core/TemplateHelpers.hpp>
#include <Common/Core/Memory/Buffer.hpp>
#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <cstdio>

namespace Desert::Editor
{
    namespace
    {
        // The level's game mode prefabs (Default Pawn, Player Controller), read before Play spawns them: a prefab
        // chosen after the level opened (Details, the control socket) was never one of the open's asset roots, so
        // the manager has no loaded asset for its handle. UE resolves the GameMode's classes at BeginPlay the same
        // way; a handle nothing names is left to BeginPlay, which refuses by name.
        void LoadGameModePrefabs( const ::Desert::Core::Scene& scene, Assets::AssetManager& assets )
        {
            for ( const Assets::AssetHandle handle :
                  { scene.GetSettings().DefaultPawn, scene.GetSettings().PlayerController } )
            {
                if ( handle == 0 )
                    continue;
                if ( const auto found = assets.FindByHandle<Assets::PrefabAsset>( handle );
                     found && found->IsReadyForUse() )
                    continue;
                const std::string key = Assets::ContentRegistry::KeyForHandle( static_cast<uint64_t>( handle ) );
                if ( key.empty() )
                    continue;
                const std::filesystem::path file   = Common::AssetHandle::PathForStableKey( key );
                auto                        prefab = assets.FindByPath<Assets::PrefabAsset>( file );
                if ( !prefab )
                    prefab = assets.CreateAsset<Assets::PrefabAsset>( file.string() );
                if ( prefab && !prefab->IsReadyForUse() )
                    if ( const auto loaded = prefab->Load(); !loaded )
                        LOG_ERROR( "[Play] the game mode prefab '{}' did not load: {}", key, loaded.GetError() );
            }
        }

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
        Start( fromHere ? StartKind::PlayFromHere : StartKind::Play, playerStartTag );
    }

    void PlaySession::Simulate()
    {
        Start( StartKind::Simulate, {} );
    }

    void PlaySession::Start( const StartKind kind, const std::string& playerStartTag )
    {
        const bool fromHere = kind == StartKind::PlayFromHere;
        using SceneState  = ::Desert::Core::Scene::SceneState;
        const auto& scene = m_Workspace.ActiveScene();
        if ( scene->GetState() != SceneState::Edit )
            return;
        // Play is a structural change of the edit world (entities completed from disk, the pawn, a refusal's
        // restore): every pointer-based record goes now, as around every structural command (OnStructuralChange).
        CommandHistory::Get().DropVolatile();
        // Snapshot the authored scene so Stop can restore it exactly (play-time edits are discarded). Timed
        // like a load: it is the other half of the round trip Stop pays.
        Desert::Core::SceneLoadPhases       phases( "Play start" );
        // The editor's HLODs leave first (WP-EDITOR-HLOD): Play streams the world's own, which share their ids.
        Editor::EditorHLODs::ReleaseAll( *scene );
        // The Sequencer preview's bone layers leave too, before the first Play step: Play's Level Sequence
        // actors write their own (an Animator holds one writer); the preview re-scrubs after Stop.
        ECS::ReleaseLevelSequencePreviewLayers( scene->GetRegistry() );
        const Desert::Core::SceneSerializer serializer( scene.get(), m_Assets.get() );
        m_PlayWorld.Begin( serializer.SerializeToJson() );
        PlayKeptChanges::Clear(); // a new session keeps nothing an earlier one left unapplied
        phases.Lap( "write the Play snapshot", scene->GetAllEntities().size() );
        // The edit world's packages (which entities differ from their files, which records an editor region
        // left on disk) go with the snapshot: the restore at Stop is a load, and a load forgets them.
        m_EditPackages = *scene->Packages();

        // THE PLAYED WORLD IS THE WHOLE WORLD (WP20, UE's PIE over World Partition): a world the editor holds
        // in part is completed from its files - what the editor holds plays as it is in memory, the rest as
        // its files hold it - and the streamer is handed that whole world, not the edit world's part.
        auto completed = Desert::Core::EditorRegions::MakeNotLoadedForPlay( *scene, m_Assets.get() );
        if ( !completed )
        {
            LOG_ERROR( "[Scene] Play refused: {0}", completed.GetError() );
            Editor::ToastManager::Push( std::format( "Play refused: {}", completed.GetError() ),
                                        Editor::ToastLevel::Error );
            m_PlayWorld.Discard();
            m_EditPackages.reset();
            return;
        }
        const bool        completedFromDisk = completed.GetValue() > 0;
        const std::string playedWorld       = completedFromDisk ? serializer.SerializeToJson() : std::string();
        phases.Lap( "read the records the editor left on disk", completed.GetValue() );
        // A refusal below takes Play back; a world completed from disk goes back to the part the editor held.
        const auto refuse = [&]()
        {
            if ( completedFromDisk )
                RestoreEditWorld( m_PlayWorld.AuthoredSnapshot() );
            else
                m_EditPackages.reset();
            m_PlayWorld.Discard();
        };
        // The pawn is spawned AFTER the snapshot, so Stop's restore has never heard of it, and BEFORE the
        // streamer, which may unload the cell the PlayerStart stands in.
        Desert::Core::PlayRequest request;
        request.PlayerStartTag = playerStartTag;
        request.Simulate       = kind == StartKind::Simulate;
        if ( fromHere && scene->GetActiveCamera() )
        {
            // Stand where the editor camera is, facing where it faces - yaw only.
            const glm::mat4 camWorld = glm::inverse( scene->GetActiveCamera()->GetViewMatrix() );
            const glm::vec3 forward  = -glm::vec3( camWorld[2] );
            const float     yaw      = std::atan2( -forward.x, -forward.z );
            request.SpawnAt          = glm::translate( glm::mat4( 1.0f ), glm::vec3( camWorld[3] ) ) *
                              glm::rotate( glm::mat4( 1.0f ), yaw, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        }
        LoadGameModePrefabs( *scene, *m_Assets );
        if ( const auto began = Desert::Core::BeginPlay( *scene, *m_Assets, request ); !began )
        {
            LOG_ERROR( "[Scene] Play refused: {0}", began.GetError() );
            Editor::ToastManager::Push( std::format( "Play refused: {}", began.GetError() ),
                                        Editor::ToastLevel::Error );
            refuse();
            return;
        }
        phases.Lap( "spawn the player's pawn", scene->GetAllEntities().size() );
        auto streamer = Desert::Core::WorldStreamer::Begin(
             *scene, *m_Assets, completedFromDisk ? playedWorld : m_PlayWorld.AuthoredSnapshot(),
             scene->Packages()->BaselinePath().value_or( std::filesystem::path() ), InstrumentStreamingSources() );
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
            refuse();
            return;
        }
        m_WorldStreamer    = streamer.ExtractValue();
        m_WorldStreamClock = 0.0;
        // PLAY AS CLIENT (UE PIE NetMode Client): the player's pawn above becomes this connection's pawn on the
        // server; a refused connect is said and Play goes on standalone - the level itself played fine.
        if ( const auto& net = Desert::Core::ClientNetOptions::Launch(); net.Enabled() && !request.Simulate )
        {
            if ( auto connected = Desert::Core::ClientNetDriver::Connect( net ); connected )
                m_NetClient = connected.ExtractValue();
            else
            {
                LOG_ERROR( "[Net] Play as client refused: {0}", connected.GetError() );
                Editor::ToastManager::Push( std::format( "Play as client refused: {}", connected.GetError() ),
                                            Editor::ToastLevel::Error );
            }
        }
#if DESERT_DEV_INSTRUMENTS
        if ( m_WorldStreamer && ShotOptions::Get().StreamDelayTicks > 0 )
        {
            m_WorldStreamer->SetDebugLoadDelayTicks( ShotOptions::Get().StreamDelayTicks );
            LOG_WARN( "[WorldPartition] DEV: every cell read is reported {0} frame(s) late (--stream-delay-ticks)",
                      ShotOptions::Get().StreamDelayTicks );
        }
#endif
        // THE EDIT WORLD'S HISTORY IS FENCED, NOT ERASED (PIE-UNDO-KEEP; UE: SetUndoBarrier when PIE starts,
        // PlayLevel.cpp:2972): no record of it may fire into the played world, and Stop gives it back.
        CommandHistory::Get().SetUndoBarrier();
        m_State = State::Play;
        if ( request.Simulate )
            LOG_INFO( "[Play] Simulate In Editor: the world runs with no player; Stop restores the level" );
    }

    void PlaySession::Stop()
    {
        using SceneState  = ::Desert::Core::Scene::SceneState;
        const auto& scene = m_Workspace.ActiveScene();
        if ( scene->GetState() == SceneState::Edit || !m_PlayWorld.Active() )
            return;

        Desert::Core::SceneLoadPhases phases( "Stop restore" );
        // THE SELECTION OUTLIVES PLAY (UEditorEngine::EndPlayMap, PlayLevel.cpp:266-300 and 695-705: the selected
        // PIE actors are mapped to their editor counterparts and selected again after the PIE world goes). The
        // snapshot restore below rebuilds every authored entity under its own UUID, so a UUID is the counterpart;
        // one that Play spawned (the pawn) is not in the restored world and drops out.
        const std::vector<Common::UUID> selectedBeforeStop = Core::SelectionManager::GetSelection();
        EngineContext::GetInstance().GetDevice()->WaitIdle();
        // THE EDIT WORLD'S HISTORY COMES BACK (UEditorEngine::EndPlayMap -> RemoveUndoBarrier, PlayLevel.cpp:677):
        // what Play recorded against the played world goes before that world does; the edits made before Play are
        // undoable again once the snapshot below has rebuilt the entities they name, under the same UUIDs.
        if ( !CommandHistory::Get().RemoveUndoBarrier() )
            LOG_ERROR( "[Play] Stop found no undo barrier: this Play session never fenced the history" );
        EndNetClient();          // the connection closes with Play (UE: the PIE client's net driver is destroyed)
        m_WorldStreamer.reset(); // before Clear: the snapshot below brings every cell back
        phases.Lap( "wait for the GPU, close the Play undo session, drop the streamer", 0 );

        // Whatever map Play ended on -- the authored one or one a travel loaded -- the snapshot taken when Play
        // began is what comes back (UEditorEngine::EndPlayMap: the PIE world goes, the editor's level stays).
        const auto restored = m_PlayWorld.End(
             [&]( const std::string& snapshot ) -> Common::BoolResultStr
             {
                 const std::size_t outgoing = scene->GetAllEntities().size();
                 scene->Clear();
                 phases.Lap( "clear the played scene", outgoing );
                 // The played world's renderer state (distance-field brick atlas, surface cache, local shadow
                 // atlas) was queued for deletion by Clear: release it before the snapshot's world builds its
                 // own, as the opener does (SceneFiles). Without it Stop held both worlds' GPU state at once -
                 // FIELD_Barn's Stop peaked over the 6 GB cap with DistanceFieldBrickAtlas x2 (FOLIAGE-LOOK-8).
                 m_Workspace.ReleaseActiveWorld();
                 phases.Lap( "release the played world's GPU resources, drain deferred deletions", outgoing );
                 const Desert::Core::SceneSerializer serializer( scene.get(), m_Assets.get() );
                 // NOT A FILE: named "<Play snapshot>" so a failure says which of the two "loading a scene" broke.
                 return serializer.DeserializeFromJson( snapshot, "<Play snapshot>" );
             } );
        if ( !restored )
        {
            LOG_ERROR( "[Scene] Play snapshot could not be restored: {0}", restored.GetError() );
            Editor::ToastManager::Push( "Play snapshot could not be restored — see the log",
                                        Editor::ToastLevel::Error );
        }
        else if ( m_EditPackages )
            *scene->Packages() = *m_EditPackages; // the edit world is back, and so is what it knows of its files
        m_EditPackages.reset();
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
        std::vector<Common::UUID> counterparts;
        for ( const Common::UUID& uuid : selectedBeforeStop )
            if ( scene->FindEntityByID( uuid ) )
                counterparts.push_back( uuid );
        Core::SelectionManager::SetSelection( std::move( counterparts ) );
        phases.Lap( "rebuild the render registry, select the played selection's authored counterparts", incoming );
        phases.LogSummary();

        scene->SetState( SceneState::Edit );
        // What Play asked to keep (Capture current sky) lands on the restored world, undoable, now that it is
        // the edit world again.
        PlayKeptChanges::ApplyTo( *scene );
        // The session ends with the world: without this the editor kept reporting Play (MCP state "playing")
        // after every Stop — only a closed scene view used to end it (EndIfBoundTo).
        m_State = State::Paused;
    }

    void PlaySession::RestoreEditWorld( const std::string& snapshot )
    {
        const auto& scene = m_Workspace.ActiveScene();
        EngineContext::GetInstance().GetDevice()->WaitIdle();
        scene->Clear();
        m_Workspace.ReleaseActiveWorld(); // as Stop: the old world's GPU state goes before the snapshot loads
        const Desert::Core::SceneSerializer serializer( scene.get(), m_Assets.get() );
        if ( const auto restored = serializer.DeserializeFromJson( snapshot, "<Play snapshot>" ); !restored )
        {
            LOG_ERROR( "[Scene] Play snapshot could not be restored: {0}", restored.GetError() );
            Editor::ToastManager::Push( "Play snapshot could not be restored — see the log",
                                        Editor::ToastLevel::Error );
        }
        else if ( m_EditPackages )
            *scene->Packages() = *m_EditPackages;
        m_EditPackages.reset();
        if ( const auto inited = scene->Init(); !inited.IsSuccess() )
            LOG_ERROR( "[EditorLayer] scene failed to initialise after a refused Play: {}", inited.GetError() );
        m_Workspace.ActiveSceneReplaced();
    }

    Common::BoolResultStr PlaySession::ServiceTravel()
    {
        return m_PlayWorld.Tick( [this]( const std::string& path ) { return LoadIntoPlayWorld( path ); } );
    }

    Common::BoolResultStr PlaySession::LoadIntoPlayWorld( const std::string& path )
    {
        using SceneState  = ::Desert::Core::Scene::SceneState;
        const auto& scene = m_Workspace.ActiveScene();

        // ASKED BEFORE ANYTHING IS TORN DOWN: a level that cannot load leaves the played world running.
        auto text = Desert::Core::ExternalEntities::ReadSceneFileText( path );
        if ( !text )
            return Common::MakeFormattedError<bool>( "Travel refused, the played level is untouched: {}",
                                                     text.GetError() );
        const std::string json     = text.ExtractValue();
        auto              loadable = Desert::Core::ParseLoadableScene( path, json );
        if ( !loadable )
            return Common::MakeFormattedError<bool>( "Travel refused, the played level is untouched: {}",
                                                     loadable.GetError() );

        Desert::Core::SceneLoadPhases phases( std::format( "PIE travel '{}'", path ) );
        EngineContext::GetInstance().GetDevice()->WaitIdle(); // scene teardown frees GPU resources
        if ( m_NetClient ) // a client does not travel on its own (UE: ServerTravel moves the clients); the session
                           // ends
            LOG_WARN( "[Net] a level change on a client ends its session with the server" );
        EndNetClient();
        m_WorldStreamer.reset();                              // streams the world about to be cleared
        // The level left takes its Play records with it - they name the world about to be cleared (UE's travel in
        // PIE makes a new PIE world) - and the edit world's history stays parked behind a fresh barrier.
        if ( CommandHistory::Get().RemoveUndoBarrier() )
            CommandHistory::Get().SetUndoBarrier();
        else
            LOG_ERROR( "[Play] travel found no undo barrier: this Play session never fenced the history" );
        const std::size_t outgoing = scene->GetAllEntities().size();
        scene->Clear();
        // A notification of the level being left names one of ITS overlay canvases (RuntimeLayer's switch).
        Desert::UI::UIOverlayRequests::Get().Clear();
        phases.Lap( "clear the played world", outgoing );

        // Whatever fails from here on, the scene stays in Play: Stop restores the authored level only from a
        // world that is not in Edit, and a half-travelled world is still the game's, never the document's.
        const auto stayInPlay = [&]( const std::string& error ) -> Common::BoolResultStr
        {
            scene->SetState( SceneState::Play );
            m_Workspace.ActiveSceneReplaced();
            return Common::MakeError( error );
        };
        const Desert::Core::SceneSerializer serializer( scene.get(), m_Assets.get() );
        if ( const auto loaded = serializer.Deserialize( loadable.ExtractValue(), path ); !loaded )
            return stayInPlay(
                 std::format( "Travel to '{}' failed after teardown: {}", path, loaded.GetError() ) );
        (void)Runtime::AwaitSceneClosure( *scene ); // the new level's dependency closure, read by the workers
        if ( const auto inited = scene->Init(); !inited.IsSuccess() )
            return stayInPlay( std::format( "Travel to '{}': init failed: {}", path, inited.GetError() ) );
        phases.Lap( "load the travelled level", scene->GetAllEntities().size() );

        // Play first: the spawned pawn is the source the streamer begins around (PawnIsAStreamingSource).
        LoadGameModePrefabs( *scene, *m_Assets );
        if ( const auto began = Desert::Core::BeginPlay( *scene, *m_Assets, {} ); !began )
            return stayInPlay( std::format( "Play refused for '{}': {}", path, began.GetError() ) );
        auto streamer = Desert::Core::WorldStreamer::Begin( *scene, *m_Assets, json, std::filesystem::path( path ),
                                                            InstrumentStreamingSources() );
        if ( !streamer )
            return stayInPlay(
                 std::format( "Travel to '{}' could not stream the world: {}", path, streamer.GetError() ) );
        m_WorldStreamer    = streamer.ExtractValue();
        m_WorldStreamClock = 0.0;
        m_Workspace.ActiveSceneReplaced();
        phases.Lap( "begin Play and the world streamer", scene->GetAllEntities().size() );
        phases.LogSummary();
        LOG_INFO( "[Play] travelled to '{}' -- Stop returns to the authored level", path );
        return BOOLSUCCESS;
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
        m_PlayWorld.Discard();
        m_EditPackages.reset();
        EndNetClient(); // the closed view's world is still the active one here and goes after this returns
        m_WorldStreamer.reset();
        // The session ends here, not in Stop: its records go with the closed view's world, the parked history
        // comes back.
        if ( !CommandHistory::Get().RemoveUndoBarrier() )
            LOG_ERROR( "[Play] the closed view's Play session found no undo barrier: it never fenced the history" );
    }

    void PlaySession::EndNetClient()
    {
        if ( !m_NetClient )
            return;
        m_NetClient->Shutdown( *m_Workspace.ActiveScene() );
        m_NetClient
             .reset(); // releases the predicted pawn's character from the physics world (PhysicsPawnMoveWorld)
    }

    void PlaySession::EndForExit()
    {
        EndNetClient();
        m_WorldStreamer.reset();
        m_PendingStop = false;
    }

    bool PlaySession::TickStreaming( Desert::Core::Scene& scene, const Common::Timestep& ts )
    {
        // The network first: this frame's replicated poses are what streaming and every system see.
        if ( m_NetClient )
        {
            DESERT_PROFILE_SCOPE( "ClientNetDriver::Tick" );
            m_NetClient->Tick( scene, *m_Assets ); // real time (ClientNetDriver::Tick)
        }
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
            case Command::Simulate:
                if ( state == SceneState::Edit )
                    Simulate();
                return PaletteCommandOutcome( state == SceneState::Edit && scene->GetState() != SceneState::Edit,
                                              "the scene is not simulating; either it was already playing or "
                                              "Simulate refused (the log says why)." );
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
            if ( ImGui::MenuItem( ICON_MDI_COGS " Simulate", nullptr, false ) )
                run( Command::Simulate );
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
        // RUNTIME DATA LAYERS (WP-DATA-LAYERS-RUNTIME): UE's Data Layer Outliner in PIE - a Runtime layer's state set
        // by hand, one entry per layer and state; the same call a game makes (WorldStreamer::SetDataLayerRuntimeState).
        if ( const auto& partition = m_Workspace.ActiveScene()->GetWorldPartition();
             partition.has_value() && partition->DataLayers.has_value() )
            for ( const ::Desert::Core::DataLayerSerialized& layer : *partition->DataLayers )
            {
                if ( layer.Type != ::Desert::Core::DataLayerType::Runtime )
                    continue;
                for ( const auto state : { ::Desert::Core::DataLayerRuntimeState::Unloaded,
                                           ::Desert::Core::DataLayerRuntimeState::Loaded,
                                           ::Desert::Core::DataLayerRuntimeState::Activated } )
                    commands.push_back(
                         { "Scene",
                           std::format( "World Partition: Data layer {} -> {}", layer.Name,
                                        ::Desert::Core::Rules::DataLayerRuntimeStateName( state ) ),
                           [this, name = layer.Name, state]
                           {
                               ::Desert::Core::WorldStreamer* const streamer = Streamer();
                               if ( streamer == nullptr || !streamer->Streams( *m_Workspace.ActiveScene() ) )
                                   return PaletteCommandOutcome( false, "nothing is streaming: a runtime data layer "
                                                                        "has a state in Play only." );
                               return streamer->SetDataLayerRuntimeState( name, state );
                           } } );
            }
#if DESERT_DEV_INSTRUMENTS
        // THE WORLD PARTITION DEBUG VIEW (WP-DEBUG-DRAW; UE wp.Runtime.ToggleDrawRuntimeHash2D / 3D,
        // ToggleDrawStreamingSources, ToggleDrawRuntimeCellsDetails, ShowRuntimeSpatialHashGridLevel): the
        // gameplay debugger's World Partition category shown and switched - what Shift+M/B/O/I/G/C do in a
        // packaged game.
        {
            using WorldPartitionView = ::Desert::Debug::GameplayDebuggerCategoryWorldPartition;
            const auto viewCommand = [this]( std::string label, std::function<void( WorldPartitionView& )> change )
            {
                return PaletteCommand{
                     "Scene", std::move( label ),
                     [this, change = std::move( change )]() -> Common::BoolResultStr
                     {
                         ::Desert::Debug::GameplayDebugger& debugger = m_Workspace.ActiveScene()->GetGameplayDebugger();
                         auto* category = dynamic_cast<WorldPartitionView*>( debugger.FindCategory( "World Partition" ) );
                         if ( category == nullptr )
                             return Common::MakeError<bool>(
                                  std::string( "the gameplay debugger has no World Partition category" ) );
                         debugger.SetActive( true );
                         if ( !debugger.SetCategoryEnabled( "World Partition", true ) )
                             return PaletteCommandOutcome( false, "the gameplay debugger did not turn its World "
                                                                  "Partition category on" );
                         change( *category );
                         return PaletteCommandDone();
                     } };
            };
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition 3D",
                                             []( WorldPartitionView& c )
                                             {
                                                 c.GetView().Hash3D = true;
                                                 c.GetView().Hash2D = false;
                                             } ) );
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition 2D",
                                             []( WorldPartitionView& c )
                                             {
                                                 c.GetView().Hash2D = true;
                                                 c.GetView().Hash3D = false;
                                             } ) );
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition 2D + 3D",
                                             []( WorldPartitionView& c )
                                             {
                                                 c.GetView().Hash2D = true;
                                                 c.GetView().Hash3D = true;
                                             } ) );
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition toggle streaming sources",
                                             []( WorldPartitionView& c ) { c.GetView().Sources = !c.GetView().Sources; } ) );
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition toggle cell details",
                                             []( WorldPartitionView& c )
                                             { c.GetView().CellDetails = !c.GetView().CellDetails; } ) );
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition next grid level",
                                             []( WorldPartitionView& c ) { c.NextGridLevel(); } ) );
            commands.push_back( viewCommand( "Gameplay Debugger: World Partition next colour mode",
                                             []( WorldPartitionView& c ) { c.NextColorMode(); } ) );
        }
#endif
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
        // Fire one TAGGED field placed in the scene, now (UE: the event that calls ApplyPhysicsField on a Field
        // System Actor). The control channel's way to break something at a chosen step of a scenario instead of
        // racing a script timer against the shots; the same firing path as Destruction.applyField.
        for ( const auto entity : registry.view<ECS::TagComponent>() )
        {
            if ( registry.try_get<ECS::RadialImpulseFieldComponent>( entity ) == nullptr &&
                 registry.try_get<ECS::StrainFieldComponent>( entity ) == nullptr &&
                 registry.try_get<ECS::KillFieldComponent>( entity ) == nullptr &&
                 registry.try_get<ECS::AnchorFieldComponent>( entity ) == nullptr )
                continue;
            const std::string tag = registry.get<ECS::TagComponent>( entity ).Tag;
            if ( tag.empty() )
                continue;
            commands.push_back(
                 { "Action", std::format( "Fire Field '{}'", tag ), [this, entity]
                   {
                       const auto& scene = m_Workspace.ActiveScene();
                       auto&       reg   = scene->GetRegistry();
                       if ( !reg.valid( entity ) )
                           return PaletteCommandOutcome( false, "that field no longer exists." );
                       const auto*                    physics = scene->FindSystem<ECS::PhysicsECSSystem>();
                       Destruction::DestructionWorld* world =
                            Destruction::FindDestructionWorld( physics );
                       if ( world == nullptr )
                           return PaletteCommandOutcome(
                                false, "fields fire in Play or Simulate: the scene has no destruction world." );
                       const auto fired = ECS::FireDestructionField( reg, entity, *world );
                       if ( !fired )
                           return PaletteCommandOutcome( false, fired.GetError() );
                       LOG_INFO( "[Play] Fire Field: acted on {} bodies/pieces", fired.GetValue() );
                       return PaletteCommandOutcome( true, "" );
                   } } );
        }
    }
} // namespace Desert::Editor
