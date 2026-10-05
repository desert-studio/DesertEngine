// THE LEVEL SEQUENCE DOCUMENT (ANIM-LSEQ) — SequencerPanel's third timeline, UE's Sequencer over a
// ULevelSequence: the rows are the sequence's POSSESSABLES (entity bindings) and its Camera Cut track, the
// ruler scrubs a PREVIEW of the scene (ECS::LevelSequencePreview, the same host the placed component plays
// with), and closing the document gives the scene back (the panel's destructor).
//
// Every edit is one undo step through the SAME transaction the other timelines use (SequenceEditTransaction
// over a SequenceOwner), and every structural rule lives in the engine half (LevelSequenceAuthoring): this
// file draws and routes, it does not decide.

#include "SequencerPanel.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/GizmoState.hpp>
#include <Editor/Panels/Sequencer/TimelineRuler.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Track.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>
#include <Engine/ECS/System/LevelSequenceSystem.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        namespace LevelTL = Animation::Timeline;

        /// The entity a binding names, in @p scene — nullopt for a binding that is not an Entity binding or
        /// names nothing of this scene.
        std::optional<ECS::Entity> BoundEntity( const ::Desert::Core::Scene& scene,
                                                const LevelTL::Binding&      binding )
        {
            if ( binding.Kind != LevelTL::BindingKind::Entity )
                return std::nullopt;
            const auto found = scene.FindEntityByID( Common::UUID( binding.Locator ) );
            if ( !found )
                return std::nullopt;
            return found->get();
        }

        double SecondsAt( const LevelTL::Sequence& sequence, const int32_t tick )
        {
            return Animation::FrameTimeToSeconds( Animation::FrameTime{ Animation::FrameNumber{ tick }, 0.0F },
                                                  sequence.TickRate );
        }

        /// Ticks per DISPLAY frame — the grid a scrubbed playhead and a dragged key land on.
        double TicksPerDisplayFrame( const LevelTL::Sequence& sequence )
        {
            return static_cast<double>( sequence.TickRate.Numerator ) * sequence.DisplayRate.Denominator /
                   ( static_cast<double>( sequence.TickRate.Denominator ) * sequence.DisplayRate.Numerator );
        }

        int32_t SnapToDisplayFrame( const LevelTL::Sequence& sequence, const double ticks )
        {
            const double perFrame = TicksPerDisplayFrame( sequence );
            return static_cast<int32_t>( std::llround( std::round( ticks / perFrame ) * perFrame ) );
        }
    } // namespace

    std::shared_ptr<Assets::LevelSequenceAsset> SequencerPanel::ResolveLevelAsset() const
    {
        if ( m_AssetManager == nullptr )
            return nullptr;
        auto asset =
             m_AssetManager->FindByHandle<Assets::LevelSequenceAsset>( Assets::AssetHandle( Subject().Owner ) );
        if ( asset && !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->EnsureLoaded( *m_AssetManager ); !loaded )
            {
                LOG_ERROR( "[Sequencer] level sequence would not load: {}", loaded.GetError() );
                return nullptr;
            }
        }
        return asset;
    }

    SequenceOwner SequencerPanel::LevelOwner() const
    {
        const auto                                      asset = ResolveLevelAsset();
        const std::weak_ptr<Assets::LevelSequenceAsset> weak  = asset;
        SequenceOwner                                   owner;
        owner.Identity = asset.get();
        // The asset may be unloaded between the edit and its undo; the step then has nothing to restore.
        owner.Resolve = [weak]() -> LevelTL::Sequence*
        {
            const auto locked = weak.lock();
            return locked ? &locked->EditSequence() : nullptr;
        };
        // The file is the document: an undo step outlives the frame it was made in (not volatile).
        owner.Volatile = false;
        owner.Name     = "Level Sequence";
        return owner;
    }

    void SequencerPanel::PreviewLevelIfChanged( const LevelTL::Sequence& sequence )
    {
        // NOT EVERY FRAME: a per-frame pose would fight the gizmo the user moves an actor with before keying
        // it. The scene is posed when the playhead moved or the sequence changed (every edit, undo and redo
        // bumps Revision).
        if ( m_LevelTick.Value == m_LevelTickShown && sequence.Revision == m_LevelRevisionShown )
            return;
        const auto scene = m_Scene.lock();
        if ( !scene )
            return;
        const auto step =
             m_LevelPreview.Scrub( scene->GetRegistry(), sequence, m_LevelTick,
                                   m_AssetManager != nullptr ? ECS::LevelSequenceClips( *m_AssetManager )
                                                             : ECS::LevelSequenceClipSource{} );
        for ( const auto& refusal : step.Refusals )
            LOG_WARN( "[Sequencer] level preview: {}", refusal );
        m_LevelTickShown     = m_LevelTick.Value;
        m_LevelRevisionShown = sequence.Revision;
    }

    void SequencerPanel::AddLevelActor( const Common::UUID& entity, const std::string& label )
    {
        if ( !ResolveLevelAsset() )
            return;
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        const auto added = ECS::AddEntityBinding( ResolveLevelAsset()->EditSequence(), entity, label );
        if ( !added.IsSuccess() )
            ToastManager::Push( std::format( "+ Track → Actor refused: {}", added.GetError() ), ToastLevel::Error,
                                6.0f );
    }

    void SequencerPanel::KeyLevelTransform( const LevelTL::BindingGuid& binding )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return;
        LevelTL::Sequence& sequence = asset->EditSequence();
        const auto*        bound    = LevelTL::FindBinding( sequence, binding );
        const auto         entity   = bound != nullptr ? BoundEntity( *scene, *bound ) : std::nullopt;
        if ( !entity || !entity->HasComponent<ECS::TransformComponent>() )
        {
            ToastManager::Push( "Key Transform: the binding names no entity with a Transform in this scene",
                                ToastLevel::Error, 6.0f );
            return;
        }
        // The LIVE transform at the playhead is what gets keyed — UE's "key what you see".
        const auto               pose = ECS::EntityPose( entity->GetComponent<ECS::TransformComponent>() );
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto keyed = ECS::SetEntityTransformKey( sequence, binding, m_LevelTick, pose );
             !keyed.IsSuccess() )
            ToastManager::Push( std::format( "Key Transform refused: {}", keyed.GetError() ), ToastLevel::Error,
                                6.0f );
    }

    void SequencerPanel::AddLevelCameraCut( const LevelTL::BindingGuid& camera )
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset )
            return;
        LevelTL::Sequence&       sequence = asset->EditSequence();
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto cut = ECS::AddCameraCut( sequence, camera, m_LevelTick, sequence.End ); !cut.IsSuccess() )
            ToastManager::Push( std::format( "Camera Cut refused: {}", cut.GetError() ), ToastLevel::Error, 6.0f );
    }

    void SequencerPanel::AddLevelVisibilityTrack( const LevelTL::BindingGuid& binding )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return;
        LevelTL::Sequence& sequence = asset->EditSequence();
        const auto*        bound    = LevelTL::FindBinding( sequence, binding );
        const auto         entity   = bound != nullptr ? BoundEntity( *scene, *bound ) : std::nullopt;
        // What the actor shows now is the track's first key (UE keys the current value when a property track
        // is added); an entity with no VisibilityComponent renders, so it is visible.
        const bool current = !entity || !entity->HasComponent<ECS::VisibilityComponent>() ||
                             entity->GetComponent<ECS::VisibilityComponent>().Visible;
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto added = ECS::AddVisibilityTrack( sequence, binding, current ); !added.IsSuccess() )
            ToastManager::Push( std::format( "+ Track → Visibility refused: {}", added.GetError() ),
                                ToastLevel::Error, 6.0f );
    }

    void SequencerPanel::KeyLevelVisibility( const LevelTL::BindingGuid& binding, const bool visible )
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset )
            return;
        LevelTL::Sequence&       sequence = asset->EditSequence();
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto keyed = ECS::SetVisibilityKey( sequence, binding, m_LevelTick, visible );
             !keyed.IsSuccess() )
            ToastManager::Push( std::format( "Key Visibility refused: {}", keyed.GetError() ), ToastLevel::Error,
                                6.0f );
    }

    std::vector<std::shared_ptr<Assets::AnimationAsset>>
    SequencerPanel::LevelAnimationClips( const LevelTL::BindingGuid& binding ) const
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene || m_Library == nullptr )
            return {};
        const auto* bound  = LevelTL::FindBinding( asset->GetSequence(), binding );
        const auto  entity = bound != nullptr ? BoundEntity( *scene, *bound ) : std::nullopt;
        if ( !entity || !entity->HasComponent<ECS::SkinnedMeshComponent>() ||
             !entity->HasComponent<ECS::AnimationComponent>() )
            return {};
        // The SAME skeleton identity the skeletal timeline's clip picker lists by: a clip is offered only where
        // it plays on this mesh's skeleton.
        return m_Library->GetForMesh(
             m_Library->IdentifyMeshHandle( entity->GetComponent<ECS::SkinnedMeshComponent>().MeshHandle ) );
    }

    void SequencerPanel::AddLevelAnimation( const LevelTL::BindingGuid&                    binding,
                                            const std::shared_ptr<Assets::AnimationAsset>& clip )
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset || !clip )
            return;
        // A section names its clip by the asset's header GUID (the section's source resolves it back through
        // HandleForGuid); a clip with no cooked registry row has no GUID a section could keep.
        const auto guid =
             Assets::ContentRegistry::GuidForHandle( static_cast<uint64_t>( clip->GetMetadata().Handle ) );
        if ( !guid )
        {
            ToastManager::Push(
                 std::format( "+ Track → Animation refused: '{}' has no asset GUID (not in the cooked "
                              "registry) — a section could not name it",
                              clip->GetClip().AnimationName ),
                 ToastLevel::Error, 6.0f );
            return;
        }
        LevelTL::Sequence& sequence = asset->EditSequence();
        // UE: the section starts at the playhead and spans the clip once, in the SEQUENCE's ticks.
        const auto& clipSequence = clip->GetClip().Sequence;
        const auto  length =
             Animation::ConvertTick( clip->GetClip().DurationTicks(), clipSequence.TickRate, sequence.TickRate );
        const Animation::FrameNumber end{ m_LevelTick.Value + std::max<int32_t>( 1, length.Ticks.Value ) };
        const ScopedSequenceEdit     undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto added = ECS::AddAnimationSection( sequence, binding, *guid, m_LevelTick, end, true );
             !added.IsSuccess() )
            ToastManager::Push( std::format( "+ Track → Animation refused: {}", added.GetError() ),
                                ToastLevel::Error, 6.0f );
    }

    void SequencerPanel::SaveLevelSequence()
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset )
            return;
        const auto saved = Assets::LevelSequenceAsset::Save( asset->GetMetadata().Filepath, asset->GetSequence(),
                                                             asset->Guid() );
        if ( saved.IsSuccess() )
            ToastManager::Push( std::format( "Saved {}", asset->GetDisplayName() ), ToastLevel::Info, 3.0f );
        else
            ToastManager::Push( std::format( "Save refused: {}", saved.GetError() ), ToastLevel::Error, 6.0f );
    }

    void SequencerPanel::SetLevelTimePercent( const int percent )
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset )
            return;
        const LevelTL::Sequence& sequence = asset->GetSequence();
        const auto               span     = static_cast<double>( sequence.End.Value - sequence.Start.Value );
        JumpLevel( sequence,
                   sequence.Start.Value + static_cast<int32_t>( std::llround( span * percent / 100.0 ) ) );
    }

    void SequencerPanel::DrawLevelTimeline()
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
        {
            ImGui::TextDisabled( "The level sequence could not be loaded; see the log." );
            return;
        }
        LevelTL::Sequence& sequence = asset->EditSequence();
        // THE PLAYER IS THE CLOCK: it advances while playing, and every scrub is a JumpTo on it.
        LevelTL::Player& player = LevelPlayer( sequence );
        if ( player.State() == LevelTL::PlayState::Playing )
            (void)player.Advance( static_cast<double>( ImGui::GetIO().DeltaTime ) );
        m_LevelTick.Value =
             std::clamp( player.Current().Frame.Value, sequence.Start.Value, sequence.End.Value );

        // ── TOOLBAR: Save, + Track, the playhead ─────────────────────────────────────────────────────
        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            SaveLevelSequence();
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_PLUS " Track" ) )
            ImGui::OpenPopup( "##LevelAddTrack" );
        if ( ImGui::BeginPopup( "##LevelAddTrack" ) )
        {
            if ( ImGui::BeginMenu( "Actor" ) )
            {
                // The scene's entities, by tag (UE: the actor list of "+ Track → Actor To Sequencer").
                auto& registry = scene->GetRegistry();
                for ( const auto handle : registry.view<ECS::UUIDComponent, ECS::TagComponent>() )
                {
                    const auto& uuid = registry.get<ECS::UUIDComponent>( handle ).UUID;
                    const auto& tag  = registry.get<ECS::TagComponent>( handle ).Tag;
                    if ( ImGui::MenuItem( std::format( "{}##{}", tag, uuid.ToString() ).c_str() ) )
                        AddLevelActor( uuid, tag );
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        DrawLevelTransport( sequence );

        // ── THE RULER: the display-frame grid, scrubbed by a click or a drag on it ──────────────────
        constexpr float gutter       = 320.0f;
        const float     contentX0    = ImGui::GetCursorScreenPos().x;
        const float     laneX0       = contentX0 + gutter;
        const float     laneW        = std::max( 40.0f, ImGui::GetContentRegionAvail().x - gutter - 10.0f );
        const float     endSeconds   = static_cast<float>( SecondsAt( sequence, sequence.End.Value ) );
        const Sequencer::CurveViewport axis = Sequencer::TimeAxis( laneX0, laneW, endSeconds );
        const auto      xOf          = [&]( const int32_t tick )
        { return axis.TimeToX( SecondsAt( sequence, tick ) ); };
        ImDrawList*     draw         = ImGui::GetWindowDrawList();
        {
            const float rulerY = ImGui::GetCursorScreenPos().y;
            const float rulerH = 22.0f;
            draw->AddRectFilled( ImVec2( laneX0, rulerY ), ImVec2( laneX0 + laneW, rulerY + rulerH ),
                                 IM_COL32( 36, 36, 40, 255 ) );
            Sequencer::DrawFrameGrid( draw, axis, rulerY, rulerY + rulerH, sequence.End, sequence.TickRate,
                                      sequence.DisplayRate, true, IM_COL32( 255, 255, 255, 60 ) );
            ImGui::SetCursorScreenPos( ImVec2( laneX0, rulerY ) );
            ImGui::InvisibleButton( "##LevelRuler", ImVec2( laneW, rulerH ) );
            if ( ImGui::IsItemActive() )
            {
                const double seconds = axis.XToTime( ImGui::GetIO().MousePos.x );
                const double ticks   = seconds * sequence.TickRate.Numerator / sequence.TickRate.Denominator;
                JumpLevel( sequence, SnapToDisplayFrame( sequence, ticks ) );
            }
            ImGui::SetCursorScreenPos( ImVec2( contentX0, rulerY + rulerH + 4.0f ) );
        }

        // ── ROWS: one per possessable, then the Camera Cut track ─────────────────────────────────────
        if ( sequence.Bindings.empty() )
            ImGui::TextDisabled( "No tracks. + Track → Actor binds an entity of the scene." );
        const float lanesTop = ImGui::GetCursorScreenPos().y;

        // Copied: a button below may add a binding and reallocate the vector this loop walks.
        const std::vector<LevelTL::Binding> bindings = sequence.Bindings;
        for ( const auto& binding : bindings )
        {
            if ( binding.Kind != LevelTL::BindingKind::Entity )
                continue;
            ImGui::PushID( binding.Locator.c_str() );
            const auto  entity = BoundEntity( *scene, binding );
            const float rowY   = ImGui::GetCursorScreenPos().y;
            ImGui::SetCursorScreenPos( ImVec2( contentX0, rowY ) );
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( entity ? binding.Label.c_str()
                                           : std::format( "{} (not in this scene)", binding.Label ).c_str() );
            ImGui::SameLine( 160.0f );
            if ( ImGui::SmallButton( ICON_MDI_KEY " Transform" ) )
                KeyLevelTransform( binding.Guid );
            if ( entity && entity->HasComponent<ECS::CameraComponent>() )
            {
                ImGui::SameLine();
                if ( ImGui::SmallButton( ICON_MDI_VIDEO " Cut" ) )
                    AddLevelCameraCut( binding.Guid );
            }
            // "+ Track" on the actor's row (UE: the binding's "+ Track" menu): Visibility for ANY actor,
            // Animation <clip> for one with a skeletal mesh.
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_PLUS " Track" ) )
                ImGui::OpenPopup( "##LevelBindingTrack" );
            if ( ImGui::BeginPopup( "##LevelBindingTrack" ) )
            {
                if ( ImGui::MenuItem( ICON_MDI_EYE " Visibility", nullptr, false,
                                      !ECS::HasVisibilityTrack( sequence, binding.Guid ) ) )
                    AddLevelVisibilityTrack( binding.Guid );
                if ( entity && entity->HasComponent<ECS::SkinnedMeshComponent>() &&
                     entity->HasComponent<ECS::AnimationComponent>() && ImGui::BeginMenu( "Animation" ) )
                {
                    const auto clips = LevelAnimationClips( binding.Guid );
                    if ( clips.empty() )
                        ImGui::TextDisabled( "No clip plays on this mesh's skeleton." );
                    for ( const auto& clip : clips )
                        if ( clip && ImGui::MenuItem( clip->GetClip().AnimationName.c_str() ) )
                            AddLevelAnimation( binding.Guid, clip );
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
            const float rowH  = ImGui::GetFrameHeight();
            const float nextY = ImGui::GetCursorScreenPos().y;
            // The Animation track: one bar per section, labelled with the clip it plays (under the keys).
            if ( const LevelTL::Track* track =
                      LevelTL::FindTrack( sequence, binding.Guid, ECS::kLevelSequenceAnimationProperty ) )
            {
                const auto clips = LevelAnimationClips( binding.Guid );
                for ( const auto& section : track->Sections )
                {
                    const auto* anim = std::get_if<LevelTL::AnimationSectionContent>( &section.Content );
                    if ( anim == nullptr )
                        continue;
                    std::string label = "?";
                    for ( const auto& clip : clips )
                        if ( clip && Assets::ContentRegistry::GuidForHandle(
                                          static_cast<uint64_t>( clip->GetMetadata().Handle ) ) == anim->Clip )
                            label = clip->GetClip().AnimationName;
                    const float x0 = xOf( section.Start.Value );
                    const float x1 = xOf( section.End.Value );
                    draw->AddRectFilled( ImVec2( x0, rowY + 2 ), ImVec2( x1, rowY + rowH - 2 ),
                                         IM_COL32( 80, 150, 90, 255 ), 3.0f );
                    draw->AddText( ImVec2( x0 + 4, rowY + 3 ), IM_COL32( 240, 240, 240, 255 ), label.c_str() );
                }
            }
            if ( LevelTL::FindTrack( sequence, binding.Guid, ECS::kLevelSequenceTransformProperty ) != nullptr )
                DrawLevelKeyLane( sequence, binding.Guid, laneX0, laneW, rowY, rowH );
            ImGui::SetCursorScreenPos( ImVec2( contentX0, std::max( nextY, rowY + rowH + 4.0f ) ) );

            // The Visibility track, a row under its actor (UE: the property track nested in the binding): the
            // checkbox shows the value at the playhead and toggling it keys the new value there; the lane
            // shades where the actor is hidden and draws each key (filled = visible, hollow = hidden).
            if ( const auto shown = ECS::VisibilityAt( sequence, binding.Guid, m_LevelTick ) )
            {
                const float visY = ImGui::GetCursorScreenPos().y;
                ImGui::SetCursorScreenPos( ImVec2( contentX0 + 16.0f, visY ) );
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( ICON_MDI_EYE " Visibility" );
                ImGui::SameLine( 160.0f );
                bool visible = *shown;
                if ( ImGui::Checkbox( "##LevelVisible", &visible ) )
                    KeyLevelVisibility( binding.Guid, visible );
                const auto  keys = ECS::VisibilityKeys( sequence, binding.Guid );
                const float y0   = visY + 2.0f;
                const float y1   = visY + rowH - 2.0f;
                for ( size_t i = 0; i < keys.size(); ++i )
                {
                    if ( keys[i].Visible )
                        continue;
                    // A bool holds its first key before it: a hidden first key hides from the range start.
                    const int32_t from = i == 0 ? sequence.Start.Value : keys[i].Tick.Value;
                    const int32_t to   = i + 1 < keys.size() ? keys[i + 1].Tick.Value : sequence.End.Value;
                    draw->AddRectFilled( ImVec2( xOf( from ), y0 ), ImVec2( xOf( to ), y1 ),
                                         IM_COL32( 20, 20, 24, 200 ) );
                }
                for ( const auto& key : keys )
                {
                    const float  x         = xOf( key.Tick.Value );
                    const float  r         = 5.0f;
                    const float  c         = ( y0 + y1 ) * 0.5f;
                    const ImVec2 diamond[] = { ImVec2( x, c - r ), ImVec2( x + r, c ), ImVec2( x, c + r ),
                                               ImVec2( x - r, c ) };
                    if ( key.Visible )
                        draw->AddConvexPolyFilled( diamond, 4, IM_COL32( 230, 230, 230, 255 ) );
                    else
                        draw->AddPolyline( diamond, 4, IM_COL32( 230, 230, 230, 255 ), ImDrawFlags_Closed, 1.5f );
                }
                ImGui::SetCursorScreenPos( ImVec2( contentX0, visY + rowH + 4.0f ) );
            }
            ImGui::PopID();
        }

        // The Camera Cut track (sequence level): one bar per cut, labelled with the camera binding.
        for ( const auto& track : sequence.Tracks )
        {
            if ( track.Property != ECS::kLevelSequenceCameraCutProperty )
                continue;
            const float rowY = ImGui::GetCursorScreenPos().y;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( ICON_MDI_VIDEO " Camera Cuts" );
            for ( const auto& section : track.Sections )
            {
                const auto* cut = std::get_if<LevelTL::CameraCutSectionContent>( &section.Content );
                if ( cut == nullptr )
                    continue;
                const auto* camera = LevelTL::FindBinding( sequence, cut->Camera );
                const float x0     = xOf( section.Start.Value );
                const float x1     = xOf( section.End.Value );
                draw->AddRectFilled( ImVec2( x0, rowY + 2 ), ImVec2( x1, rowY + ImGui::GetFrameHeight() - 2 ),
                                     IM_COL32( 70, 110, 170, 255 ), 3.0f );
                draw->AddText( ImVec2( x0 + 4, rowY + 3 ), IM_COL32( 240, 240, 240, 255 ),
                               camera != nullptr ? camera->Label.c_str() : "?" );
            }
        }

        // The marquee closes on any release; a key drag closes in the lane that holds it.
        if ( m_LevelMarquee )
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            draw->AddRectFilled( ImVec2( m_LevelMarqueeFrom.x, m_LevelMarqueeFrom.y ), mouse,
                                 IM_COL32( 120, 160, 230, 40 ) );
            draw->AddRect( ImVec2( m_LevelMarqueeFrom.x, m_LevelMarqueeFrom.y ), mouse,
                           IM_COL32( 120, 160, 230, 200 ) );
            if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
                m_LevelMarquee = false;
        }

        // The playhead over the ruler and the lanes.
        Sequencer::DrawPlayhead( draw, axis, SecondsAt( sequence, m_LevelTick.Value ), lanesTop - 26.0f,
                                 ImGui::GetCursorScreenPos().y, lanesTop - 4.0f );

        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && !ImGui::GetIO().WantTextInput &&
             ImGui::IsKeyPressed( ImGuiKey_Delete ) && !m_LevelSelKeys.empty() )
            DeleteSelectedLevelKeys();

        // ── THE CURVE VIEW of a Transform track: the one curve editor the skeletal timeline draws with ──
        ImGui::Separator();
        ImGui::Checkbox( "Curves", &m_LevelCurveView );
        if ( m_LevelCurveView )
        {
            ImGui::SameLine();
            ImGui::RadioButton( "Location", &m_LevelCurvePart, 0 );
            ImGui::SameLine();
            ImGui::RadioButton( "Scale", &m_LevelCurvePart, 2 );
            DrawLevelCurve( sequence, contentX0, gutter, laneW );
        }

        UpdateLevelAutoKey( sequence );
        PreviewLevelIfChanged( sequence );
    }

    LevelTL::Player& SequencerPanel::LevelPlayer( const LevelTL::Sequence& sequence )
    {
        if ( !m_LevelPlayer || m_LevelPlayerStart != sequence.Start || m_LevelPlayerEnd != sequence.End )
        {
            const int32_t at = std::clamp( m_LevelTick.Value, sequence.Start.Value, sequence.End.Value );
            m_LevelPlayer.emplace( sequence.TickRate, sequence.Start, sequence.End );
            m_LevelPlayer->SetLoopMode( m_LevelLoop );
            (void)m_LevelPlayer->JumpTo( Animation::FrameTime{ Animation::FrameNumber{ at }, 0.0F } );
            m_LevelPlayerStart = sequence.Start;
            m_LevelPlayerEnd   = sequence.End;
        }
        return *m_LevelPlayer;
    }

    void SequencerPanel::JumpLevel( const LevelTL::Sequence& sequence, const int32_t tick )
    {
        const int32_t at = std::clamp( tick, sequence.Start.Value, sequence.End.Value );
        (void)LevelPlayer( sequence ).JumpTo( Animation::FrameTime{ Animation::FrameNumber{ at }, 0.0F } );
        m_LevelTick.Value = at;
    }

    void SequencerPanel::DrawLevelTransport( const LevelTL::Sequence& sequence )
    {
        LevelTL::Player& player  = LevelPlayer( sequence );
        const bool       playing = player.State() == LevelTL::PlayState::Playing;
        if ( ImGui::SmallButton( ICON_MDI_SKIP_PREVIOUS "##LevelStart" ) )
            JumpLevel( sequence, sequence.Start.Value );
        ImGui::SameLine();
        if ( ImGui::SmallButton( playing ? ICON_MDI_PAUSE "##LevelPlay" : ICON_MDI_PLAY "##LevelPlay" ) )
            playing ? player.Pause() : player.Play();
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_STOP "##LevelStop" ) )
        {
            player.Stop();
            m_LevelTick.Value = player.Current().Frame.Value;
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_SKIP_NEXT "##LevelEnd" ) )
            JumpLevel( sequence, sequence.End.Value );
        ImGui::SameLine();
        bool loop = m_LevelLoop == LevelTL::LoopMode::Loop;
        if ( ImGui::Checkbox( "Loop##Level", &loop ) )
        {
            m_LevelLoop = loop ? LevelTL::LoopMode::Loop : LevelTL::LoopMode::Once;
            player.SetLoopMode( m_LevelLoop );
        }
        ImGui::SameLine();
        // REC: the light IS the mode (one bool, read by UpdateLevelAutoKey) — red while it records.
        if ( m_LevelRecord )
            ImGui::PushStyleColor( ImGuiCol_Button, IM_COL32( 190, 40, 40, 255 ) );
        const bool pressed = ImGui::SmallButton( ICON_MDI_RECORD " Auto Key##Level" );
        if ( m_LevelRecord )
            ImGui::PopStyleColor();
        if ( pressed )
            SetLevelRecord( !m_LevelRecord );
        ImGui::SameLine();
        const double perFrame = TicksPerDisplayFrame( sequence );
        ImGui::Text( "Frame %d", static_cast<int>( std::floor( m_LevelTick.Value / perFrame ) ) );
    }

    void SequencerPanel::SetLevelRecord( const bool on )
    {
        m_LevelRecord = on;
        if ( !on )
            m_LevelAutoKey.Reset();
    }

    void SequencerPanel::UpdateLevelAutoKey( LevelTL::Sequence& sequence )
    {
        const auto scene = m_Scene.lock();
        if ( !scene || !m_LevelRecord )
            return;
        const bool held      = Core::GizmoState::EntityInteraction();
        const bool releasing = m_LevelAutoKey.Releasing( held );
        // The history's revision as the key's undo step opens: the gizmo's move entry must be the last thing
        // recorded for the two to become one transaction.
        const uint64_t opened = CommandHistory::Get().Revision();
        // The key's undo step closes (pushes its entry) when this scope ends, before the join below.
        const auto keyed = [&]
        {
            // The undo step opens around exactly the frame that writes: the release.
            std::optional<ScopedSequenceEdit> undoStep;
            if ( releasing )
                undoStep.emplace( m_LevelEdit, LevelOwner() );
            return m_LevelAutoKey.Observe( scene->GetRegistry(), sequence, m_LevelTick, held );
        }();
        if ( !keyed.IsSuccess() )
            ToastManager::Push( std::format( "Auto Key refused: {}", keyed.GetError() ), ToastLevel::Error, 6.0f );
        else if ( releasing && keyed.GetValue() > 0U )
        {
            // UE: one FScopedTransaction holds the actor's move AND its auto-key — one Ctrl+Z takes both back.
            if ( const auto move = Core::GizmoState::EntityGestureEntry() )
                (void)CommandHistory::Get().JoinFollowUp( *move, opened );
        }
        if ( releasing )
            Core::GizmoState::SetEntityGestureEntry( std::nullopt );
    }

    void SequencerPanel::DrawLevelKeyLane( LevelTL::Sequence& sequence, const LevelTL::BindingGuid& binding,
                                           const float laneX0, const float laneW, const float rowY,
                                           const float rowH )
    {
        ImDrawList*                     draw = ImGui::GetWindowDrawList();
        const Sequencer::CurveViewport  axis =
             Sequencer::TimeAxis( laneX0, laneW, static_cast<float>( SecondsAt( sequence, sequence.End.Value ) ) );
        const auto  xOf = [&]( const int32_t tick ) { return axis.TimeToX( SecondsAt( sequence, tick ) ); };
        const float y   = rowY + rowH * 0.5f;
        draw->AddLine( ImVec2( laneX0, y ), ImVec2( laneX0 + laneW, y ), IM_COL32( 90, 90, 90, 255 ) );

        const auto selectedAt = [&]( const Animation::FrameNumber tick )
        {
            return std::ranges::find_if( m_LevelSelKeys, [&]( const LevelKeyRef& key )
                                         { return key.Binding == binding && key.Tick == tick; } );
        };
        const auto   ticks = ECS::EntityTransformKeyTicks( sequence, binding );
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        ImGui::SetCursorScreenPos( ImVec2( laneX0, rowY ) );
        ImGui::InvisibleButton( "##LevelKeys", ImVec2( laneW, rowH ) );
        if ( ImGui::IsItemClicked( ImGuiMouseButton_Left ) )
        {
            const bool shift = ImGui::GetIO().KeyShift;
            std::optional<Animation::FrameNumber> hit;
            for ( const auto tick : ticks )
                if ( std::abs( mouse.x - xOf( tick.Value ) ) < 6.0f )
                    hit = tick;
            if ( hit )
            {
                const auto found = selectedAt( *hit );
                if ( shift && found != m_LevelSelKeys.end() )
                    m_LevelSelKeys.erase( found );
                else if ( found == m_LevelSelKeys.end() )
                {
                    if ( !shift )
                        m_LevelSelKeys.clear();
                    m_LevelSelKeys.push_back( LevelKeyRef{ binding, *hit } );
                }
                m_LevelKeyDrag   = selectedAt( *hit ) != m_LevelSelKeys.end();
                m_LevelDragX0    = mouse.x;
                m_LevelDragDelta = 0;
            }
            else
            {
                if ( !shift )
                    m_LevelSelKeys.clear();
                m_LevelMarquee     = true;
                m_LevelMarqueeFrom = glm::vec2( mouse.x, mouse.y );
            }
        }
        // A key drag: the selection follows on the display grid; written once, on release (one undo step).
        if ( m_LevelKeyDrag && ImGui::IsItemActive() )
        {
            const double ticksPerPixel =
                 ( axis.TimeEnd - axis.TimeStart ) / std::max( 1.0f, laneW ) * sequence.TickRate.Numerator /
                 sequence.TickRate.Denominator;
            m_LevelDragDelta = SnapToDisplayFrame( sequence, ( mouse.x - m_LevelDragX0 ) * ticksPerPixel );
        }
        if ( m_LevelKeyDrag && ImGui::IsItemDeactivated() )
        {
            m_LevelKeyDrag = false;
            if ( m_LevelDragDelta != 0 )
            {
                const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
                std::vector<LevelTL::BindingGuid> owners;
                for ( const auto& key : m_LevelSelKeys )
                    if ( std::ranges::find( owners, key.Binding ) == owners.end() )
                        owners.push_back( key.Binding );
                for ( const auto& owner : owners )
                {
                    std::vector<Animation::FrameNumber> from;
                    for ( const auto& key : m_LevelSelKeys )
                        if ( key.Binding == owner )
                            from.push_back( key.Tick );
                    if ( const auto moved = ECS::MoveEntityTransformKeys( sequence, owner, from, m_LevelDragDelta );
                         !moved.IsSuccess() )
                    {
                        ToastManager::Push( std::format( "Move keys refused: {}", moved.GetError() ),
                                            ToastLevel::Error, 6.0f );
                        continue;
                    }
                    for ( auto& key : m_LevelSelKeys )
                        if ( key.Binding == owner )
                            key.Tick.Value += m_LevelDragDelta;
                }
            }
            m_LevelDragDelta = 0;
        }
        // The marquee adds every key of this lane inside it.
        if ( m_LevelMarquee )
        {
            const float x0 = std::min( m_LevelMarqueeFrom.x, mouse.x ), x1 = std::max( m_LevelMarqueeFrom.x, mouse.x );
            const float y0 = std::min( m_LevelMarqueeFrom.y, mouse.y ), y1 = std::max( m_LevelMarqueeFrom.y, mouse.y );
            if ( y >= y0 && y <= y1 )
                for ( const auto tick : ticks )
                    if ( const float x = xOf( tick.Value ); x >= x0 && x <= x1 && selectedAt( tick ) == m_LevelSelKeys.end() )
                        m_LevelSelKeys.push_back( LevelKeyRef{ binding, tick } );
        }

        for ( const auto tick : ticks )
        {
            const bool  selected = selectedAt( tick ) != m_LevelSelKeys.end();
            const float x        = xOf( tick.Value + ( selected && m_LevelKeyDrag ? m_LevelDragDelta : 0 ) );
            draw->AddQuadFilled( ImVec2( x, y - 5 ), ImVec2( x + 5, y ), ImVec2( x, y + 5 ), ImVec2( x - 5, y ),
                                 selected ? IM_COL32( 255, 245, 200, 255 ) : IM_COL32( 230, 190, 60, 255 ) );
        }
    }

    void SequencerPanel::DeleteSelectedLevelKeys()
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset || m_LevelSelKeys.empty() )
            return;
        LevelTL::Sequence&       sequence = asset->EditSequence();
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        std::vector<LevelTL::BindingGuid> owners;
        for ( const auto& key : m_LevelSelKeys )
            if ( std::ranges::find( owners, key.Binding ) == owners.end() )
                owners.push_back( key.Binding );
        for ( const auto& owner : owners )
        {
            std::vector<Animation::FrameNumber> ticks;
            for ( const auto& key : m_LevelSelKeys )
                if ( key.Binding == owner )
                    ticks.push_back( key.Tick );
            if ( const auto removed = ECS::RemoveEntityTransformKeys( sequence, owner, ticks ); !removed.IsSuccess() )
                ToastManager::Push( std::format( "Delete keys refused: {}", removed.GetError() ), ToastLevel::Error,
                                    6.0f );
        }
        m_LevelSelKeys.clear();
    }

    void SequencerPanel::DrawLevelCurve( LevelTL::Sequence& sequence, const float contentX0, const float gutter,
                                         const float laneW )
    {
        // Which track: the selected key's binding, else the first actor with Transform keys.
        int                                   trackIndex = -1;
        std::optional<Animation::FrameNumber> selectedTick;
        for ( int ti = 0; ti < static_cast<int>( sequence.Tracks.size() ) && trackIndex < 0; ++ti )
        {
            const LevelTL::Track& track = sequence.Tracks[static_cast<size_t>( ti )];
            if ( track.Property != ECS::kLevelSequenceTransformProperty )
                continue;
            if ( !m_LevelSelKeys.empty() )
            {
                if ( track.Binding == m_LevelSelKeys.front().Binding )
                {
                    trackIndex   = ti;
                    selectedTick = m_LevelSelKeys.front().Tick;
                }
            }
            else if ( !ECS::EntityTransformKeyTicks( sequence, track.Binding ).empty() )
                trackIndex = ti;
        }
        if ( trackIndex < 0 )
        {
            ImGui::TextDisabled( "No Transform keys yet — Key Transform, or Auto Key and move the actor." );
            return;
        }
        LevelTL::Track&               track = sequence.Tracks[static_cast<size_t>( trackIndex )];
        const Animation::TrackChannel part =
             m_LevelCurvePart == 2 ? Animation::TrackChannel::Scale : Animation::TrackChannel::Position;
        LevelTL::TransformChannel* shown = nullptr;
        for ( auto& section : track.Sections )
        {
            auto* channel = std::get_if<LevelTL::Channel>( &section.Content );
            auto* pose    = channel != nullptr ? std::get_if<LevelTL::TransformChannel>( channel ) : nullptr;
            if ( pose == nullptr || Animation::LiftChannel( *pose, part, 0 ).empty() )
                continue;
            const auto keys = Animation::LiftChannel( *pose, part, 0 );
            if ( shown == nullptr || ( selectedTick && std::ranges::any_of( keys, [&]( const Animation::ScalarKey& k )
                                                                            { return k.Tick == *selectedTick; } ) ) )
                shown = pose;
        }
        const LevelTL::Binding* bound = LevelTL::FindBinding( sequence, track.Binding );
        const LevelTL::BindingGuid binding = track.Binding;

        CurvePlot plot;
        plot.Sequence        = &sequence;
        plot.Shown           = shown;
        plot.Channel         = part;
        plot.FitTrack        = trackIndex;
        plot.FitChannel      = m_LevelCurvePart;
        plot.Label           = bound != nullptr ? bound->Label : std::string( "actor" );
        plot.ContentX0       = contentX0;
        plot.Gutter          = gutter;
        plot.LaneW           = laneW;
        plot.DurationSeconds = static_cast<float>( SecondsAt( sequence, sequence.End.Value ) );
        plot.DurationTicks   = sequence.End;
        plot.PlayheadSeconds = SecondsAt( sequence, m_LevelTick.Value );
        plot.SelectedTick    = selectedTick;
        plot.Select          = [this, binding]( const Animation::FrameNumber tick )
        { m_LevelSelKeys.assign( 1, LevelKeyRef{ binding, tick } ); };
        plot.BeginEdit = [this]
        {
            if ( const auto began = m_LevelEdit.Begin( LevelOwner() ); !began.IsSuccess() )
                LOG_ERROR( "[Sequencer] level curve edit not undoable: {}", began.GetError() );
        };
        plot.EndEdit = [this]
        {
            if ( !m_LevelEdit.OpenExplicitly() )
                return;
            if ( const auto ended = m_LevelEdit.End(); !ended.IsSuccess() )
                LOG_ERROR( "[Sequencer] level curve edit not undoable: {}", ended.GetError() );
        };
        // In place on the track (the pointer `Shown` stays valid), the part's key only — the skeletal rule.
        plot.Retime = [&sequence, &track, label = plot.Label, part]( const Animation::FrameNumber from,
                                                                     const Animation::FrameNumber to )
        { return Animation::MoveTrackKey( sequence, track, label, part, from, to ).IsSuccess(); };
        DrawTransformCurve( plot );
    }

    std::vector<SequencerPanel::DocumentAction> SequencerPanel::LevelActions()
    {
        // EVERY BUTTON ABOVE, REACHABLE WITHOUT A MOUSE (see Actions for why a widget alone is not enough).
        std::vector<DocumentAction> actions;
        actions.push_back( DocumentAction{ "Save", [this] { SaveLevelSequence(); } } );
        actions.push_back( DocumentAction{ "Play", [this] {
                                               if ( const auto a = ResolveLevelAsset() )
                                                   LevelPlayer( a->GetSequence() ).Play();
                                           } } );
        actions.push_back( DocumentAction{ "Pause", [this] {
                                               if ( const auto a = ResolveLevelAsset() )
                                                   LevelPlayer( a->GetSequence() ).Pause();
                                           } } );
        actions.push_back( DocumentAction{ "Auto Key On", [this] { SetLevelRecord( true ); } } );
        actions.push_back( DocumentAction{ "Auto Key Off", [this] { SetLevelRecord( false ); } } );
        actions.push_back( DocumentAction{ "Delete Selected Keys", [this] { DeleteSelectedLevelKeys(); } } );
        actions.push_back( DocumentAction{ "Select All Keys", [this] {
                                               const auto a = ResolveLevelAsset();
                                               if ( !a )
                                                   return;
                                               m_LevelSelKeys.clear();
                                               for ( const auto& b : a->GetSequence().Bindings )
                                                   for ( const auto t : ECS::EntityTransformKeyTicks( a->GetSequence(), b.Guid ) )
                                                       m_LevelSelKeys.push_back( LevelKeyRef{ b.Guid, t } );
                                           } } );
        actions.push_back( DocumentAction{ "Toggle Curves", [this] { m_LevelCurveView = !m_LevelCurveView; } } );
        static constexpr std::array kTimePercents = { 0, 25, 50, 75, 100 };
        for ( const int percent : kTimePercents )
            actions.push_back( DocumentAction{ std::format( "Set Time {}%", percent ),
                                               [this, percent] { SetLevelTimePercent( percent ); } } );

        const auto scene = m_Scene.lock();
        const auto asset = ResolveLevelAsset();
        if ( !scene || !asset )
            return actions;
        auto& registry = scene->GetRegistry();
        for ( const auto handle : registry.view<ECS::UUIDComponent, ECS::TagComponent>() )
        {
            const Common::UUID uuid = registry.get<ECS::UUIDComponent>( handle ).UUID;
            const std::string  tag  = registry.get<ECS::TagComponent>( handle ).Tag;
            actions.push_back( DocumentAction{ std::format( "Add Actor {}", tag ),
                                               [this, uuid, label = tag] { AddLevelActor( uuid, label ); } } );
        }
        for ( const auto& binding : asset->GetSequence().Bindings )
        {
            if ( binding.Kind != LevelTL::BindingKind::Entity )
                continue;
            const LevelTL::BindingGuid guid = binding.Guid;
            actions.push_back( DocumentAction{ std::format( "Key Transform {}", binding.Label ),
                                               [this, guid] { KeyLevelTransform( guid ); } } );
            actions.push_back( DocumentAction{ std::format( "Camera Cut {}", binding.Label ),
                                               [this, guid] { AddLevelCameraCut( guid ); } } );
            if ( !ECS::HasVisibilityTrack( asset->GetSequence(), guid ) )
                actions.push_back( DocumentAction{ std::format( "Add Visibility Track {}", binding.Label ),
                                                   [this, guid] { AddLevelVisibilityTrack( guid ); } } );
            else
            {
                actions.push_back( DocumentAction{ std::format( "Key Visible {}", binding.Label ),
                                                   [this, guid] { KeyLevelVisibility( guid, true ); } } );
                actions.push_back( DocumentAction{ std::format( "Key Hidden {}", binding.Label ),
                                                   [this, guid] { KeyLevelVisibility( guid, false ); } } );
            }
            for ( const auto& clip : LevelAnimationClips( guid ) )
            {
                if ( !clip )
                    continue;
                actions.push_back( DocumentAction{
                     std::format( "Add Animation {} {}", binding.Label, clip->GetClip().AnimationName ),
                     [this, guid, clip] { AddLevelAnimation( guid, clip ); } } );
            }
        }
        return actions;
    }

    SubjectEditorRegistry::PathOpenOutcome RequestLevelSequenceDocument( Assets::AssetManager*        assets,
                                                                         const std::string&           path,
                                                                         const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != LevelTL::kLevelSequenceExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::LevelSequenceAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::LevelSequenceAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a level sequence — no Sequencer was opened.",
                       path );
            return Outcome::Failed;
        }
        if ( const auto loaded = asset->EnsureLoaded( *assets ); !loaded )
        {
            LOG_ERROR( "[Assets] '{}' would not load as a level sequence — no Sequencer was opened: {}", path,
                       loaded.GetError() );
            return Outcome::Failed;
        }
        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[Assets] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
