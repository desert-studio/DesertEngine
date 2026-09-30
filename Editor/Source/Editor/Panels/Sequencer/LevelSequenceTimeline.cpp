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
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Track.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequenceAuthoring.hpp>

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
        namespace TL = Animation::Timeline;

        /// The entity a binding names, in @p scene — nullopt for a binding that is not an Entity binding or
        /// names nothing of this scene.
        std::optional<ECS::Entity> BoundEntity( const ::Desert::Core::Scene& scene, const TL::Binding& binding )
        {
            if ( binding.Kind != TL::BindingKind::Entity )
                return std::nullopt;
            const auto found = scene.FindEntityByID( Common::UUID( binding.Locator ) );
            if ( !found )
                return std::nullopt;
            return found->get();
        }

        /// The ticks of every Transform key on @p track (the translation's X lane carries one key per keyed
        /// pose — SetEntityTransformKey writes all ten lanes at once).
        std::vector<Animation::FrameNumber> TransformKeyTicks( const TL::Track& track )
        {
            std::vector<Animation::FrameNumber> ticks;
            for ( const auto& section : track.Sections )
            {
                const auto* channel = std::get_if<TL::Channel>( &section.Content );
                if ( channel == nullptr )
                    continue;
                if ( const auto* transform = std::get_if<TL::TransformChannel>( channel ) )
                {
                    for ( const auto& key : transform->Translation.X.Keys )
                        ticks.push_back( key.Tick );
                }
            }
            return ticks;
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
        const auto                                asset = ResolveLevelAsset();
        std::weak_ptr<Assets::LevelSequenceAsset> weak  = asset;
        SequenceOwner                             owner;
        owner.Identity = asset.get();
        // The asset may be unloaded between the edit and its undo; the step then has nothing to restore.
        owner.Resolve = [weak]() -> TL::Sequence*
        {
            const auto locked = weak.lock();
            return locked ? &locked->EditSequence() : nullptr;
        };
        // The file is the document: an undo step outlives the frame it was made in (not volatile).
        owner.Volatile = false;
        owner.Name     = "Level Sequence";
        return owner;
    }

    void SequencerPanel::PreviewLevelIfChanged( const TL::Sequence& sequence )
    {
        // NOT EVERY FRAME: a per-frame pose would fight the gizmo the user moves an actor with before keying
        // it. The scene is posed when the playhead moved or the sequence changed (every edit, undo and redo
        // bumps Revision).
        if ( m_LevelTick.Value == m_LevelTickShown && sequence.Revision == m_LevelRevisionShown )
            return;
        const auto scene = m_Scene.lock();
        if ( !scene )
            return;
        const auto step = m_LevelPreview.Scrub( scene->GetRegistry(), sequence, m_LevelTick );
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

    void SequencerPanel::KeyLevelTransform( const TL::BindingGuid& binding )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return;
        TL::Sequence& sequence = asset->EditSequence();
        const auto*   bound    = TL::FindBinding( sequence, binding );
        const auto    entity   = bound != nullptr ? BoundEntity( *scene, *bound ) : std::nullopt;
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

    void SequencerPanel::AddLevelCameraCut( const TL::BindingGuid& camera )
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset )
            return;
        TL::Sequence&            sequence = asset->EditSequence();
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto cut = ECS::AddCameraCut( sequence, camera, m_LevelTick, sequence.End ); !cut.IsSuccess() )
            ToastManager::Push( std::format( "Camera Cut refused: {}", cut.GetError() ), ToastLevel::Error, 6.0f );
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
        const TL::Sequence& sequence = asset->GetSequence();
        const double        span     = static_cast<double>( sequence.End.Value - sequence.Start.Value );
        m_LevelTick.Value = sequence.Start.Value + static_cast<int32_t>( std::llround( span * percent / 100.0 ) );
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
        TL::Sequence& sequence = asset->EditSequence();
        m_LevelTick.Value      = std::clamp( m_LevelTick.Value, sequence.Start.Value, sequence.End.Value );

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
        ImGui::SetNextItemWidth( -1.0f );
        const double tickPerDisplay =
             static_cast<double>( sequence.TickRate.Numerator ) * sequence.DisplayRate.Denominator /
             ( static_cast<double>( sequence.TickRate.Denominator ) * sequence.DisplayRate.Numerator );
        int       frame = static_cast<int>( std::floor( m_LevelTick.Value / tickPerDisplay ) );
        const int first = static_cast<int>( std::floor( sequence.Start.Value / tickPerDisplay ) );
        const int last  = static_cast<int>( std::floor( sequence.End.Value / tickPerDisplay ) );
        if ( ImGui::SliderInt( "##LevelTime", &frame, first, last, "Frame %d" ) )
            m_LevelTick.Value = static_cast<int32_t>( std::llround( frame * tickPerDisplay ) );

        // ── ROWS: one per possessable, then the Camera Cut track ─────────────────────────────────────
        const float  laneX0 = ImGui::GetCursorScreenPos().x + 320.0f;
        const float  laneW  = std::max( 40.0f, ImGui::GetContentRegionAvail().x - 330.0f );
        const double span   = std::max( 1, sequence.End.Value - sequence.Start.Value );
        const auto   xOf    = [&]( const int32_t tick )
        { return laneX0 + static_cast<float>( ( tick - sequence.Start.Value ) / span ) * laneW; };

        ImDrawList* draw = ImGui::GetWindowDrawList();
        if ( sequence.Bindings.empty() )
            ImGui::TextDisabled( "No tracks. + Track → Actor binds an entity of the scene." );

        // Copied: a button below may add a binding and reallocate the vector this loop walks.
        const std::vector<TL::Binding> bindings = sequence.Bindings;
        for ( const auto& binding : bindings )
        {
            if ( binding.Kind != TL::BindingKind::Entity )
                continue;
            ImGui::PushID( binding.Locator.c_str() );
            const auto  entity = BoundEntity( *scene, binding );
            const float rowY   = ImGui::GetCursorScreenPos().y;
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
            if ( const TL::Track* track =
                      TL::FindTrack( sequence, binding.Guid, ECS::kLevelSequenceTransformProperty ) )
            {
                const float y = rowY + ImGui::GetFrameHeight() * 0.5f;
                draw->AddLine( ImVec2( laneX0, y ), ImVec2( laneX0 + laneW, y ), IM_COL32( 90, 90, 90, 255 ) );
                for ( const auto tick : TransformKeyTicks( *track ) )
                {
                    const float x = xOf( tick.Value );
                    draw->AddQuadFilled( ImVec2( x, y - 5 ), ImVec2( x + 5, y ), ImVec2( x, y + 5 ),
                                         ImVec2( x - 5, y ), IM_COL32( 230, 190, 60, 255 ) );
                }
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
                const auto* cut = std::get_if<TL::CameraCutSectionContent>( &section.Content );
                if ( cut == nullptr )
                    continue;
                const auto* camera = TL::FindBinding( sequence, cut->Camera );
                const float x0 = xOf( section.Start.Value ), x1 = xOf( section.End.Value );
                draw->AddRectFilled( ImVec2( x0, rowY + 2 ), ImVec2( x1, rowY + ImGui::GetFrameHeight() - 2 ),
                                     IM_COL32( 70, 110, 170, 255 ), 3.0f );
                draw->AddText( ImVec2( x0 + 4, rowY + 3 ), IM_COL32( 240, 240, 240, 255 ),
                               camera != nullptr ? camera->Label.c_str() : "?" );
            }
        }

        // The playhead over the lanes.
        {
            const ImVec2 winPos = ImGui::GetWindowPos();
            const float  x      = xOf( m_LevelTick.Value );
            draw->AddLine( ImVec2( x, winPos.y + ImGui::GetFrameHeightWithSpacing() ),
                           ImVec2( x, winPos.y + ImGui::GetWindowHeight() ), IM_COL32( 220, 60, 60, 255 ), 2.0f );
        }

        PreviewLevelIfChanged( sequence );
    }

    std::vector<SequencerPanel::DocumentAction> SequencerPanel::LevelActions()
    {
        // EVERY BUTTON ABOVE, REACHABLE WITHOUT A MOUSE (see Actions for why a widget alone is not enough).
        std::vector<DocumentAction> actions;
        actions.push_back( DocumentAction{ "Save", [this] { SaveLevelSequence(); } } );
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
                                               [this, uuid, tag] { AddLevelActor( uuid, tag ); } } );
        }
        for ( const auto& binding : asset->GetSequence().Bindings )
        {
            if ( binding.Kind != TL::BindingKind::Entity )
                continue;
            const TL::BindingGuid guid = binding.Guid;
            actions.push_back( DocumentAction{ std::format( "Key Transform {}", binding.Label ),
                                               [this, guid] { KeyLevelTransform( guid ); } } );
            actions.push_back( DocumentAction{ std::format( "Camera Cut {}", binding.Label ),
                                               [this, guid] { AddLevelCameraCut( guid ); } } );
        }
        return actions;
    }

    SubjectEditorRegistry::PathOpenOutcome RequestLevelSequenceDocument( Assets::AssetManager*        assets,
                                                                         const std::string&           path,
                                                                         const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != TL::kLevelSequenceExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::LevelSequenceAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::LevelSequenceAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a level sequence — no Sequencer was opened.", path );
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
