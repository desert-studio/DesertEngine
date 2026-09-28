#include "AnimationEditorDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace Desert::Editor
{
    AnimationEditorDocument::AnimationEditorDocument( const Assets::AssetHandle& clip,
                                                      Assets::AssetManager*      assets )
         : AnimationEditorBase( AssetSubjectTitle( clip, assets, "Animation" ), clip ), m_Assets( assets )
    {
    }

    AnimationEditorDocument::~AnimationEditorDocument() = default;

    bool AnimationEditorDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void AnimationEditorDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Unavailable.empty() )
            return;

        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets != nullptr ? m_Assets->FindMetadataByHandle( handle ) : nullptr;
        if ( meta == nullptr )
        {
            m_Unavailable = "This clip is not registered with the asset manager — nothing to show.";
            return;
        }
        const std::string name = meta->Filepath.filename().string();
        auto              clip = m_Assets->FindByHandle<Assets::AnimationAsset>( handle );
        if ( !clip )
        {
            m_Unavailable = std::format( "'{}' is not an AnimationAsset in the asset manager.", name );
            return;
        }
        if ( const auto loaded = clip->EnsureLoaded( *m_Assets ); !loaded )
        {
            m_Unavailable = std::format( "'{}' would not load: {}", name, loaded.GetError() );
            return;
        }
        const uint64_t signature = clip->GetSkeletonSignature();
        if ( signature == 0 )
        {
            m_Unavailable = std::format( "'{}' names no skeleton (signature 0) — no mesh can play it.", name );
            return;
        }

        // The first `.skmesh` by path whose rig is the clip's: a stable pick, so two openings show one mesh.
        // Asked of the CONTENT REGISTRY, not of the asset manager's cache: skinned meshes are registered
        // lazily, on first reference, so a cache census found none in a fresh session (ANV1a2 frame).
        std::vector<std::pair<std::string, Assets::Asset<Assets::SkinnedMeshAsset>>> candidates;
        for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::SkinnedMesh ) )
        {
            const auto mesh = m_Assets->CreateAsset<Assets::SkinnedMeshAsset>( row.Path, false );
            if ( !mesh )
                continue;
            if ( const auto loaded = mesh->EnsureLoaded( *m_Assets ); !loaded )
            {
                LOG_WARN( "Animation Editor: skeletal mesh '{}' would not load: {}", row.Path.generic_string(),
                          loaded.GetError() );
                continue;
            }
            if ( mesh->GetSkeletonSignature() == signature )
                candidates.emplace_back( mesh->GetMetadata().Filepath.generic_string(), mesh );
        }
        if ( candidates.empty() )
        {
            m_Unavailable =
                 std::format( "'{}' plays on skeleton {:016x}, and no registered skeletal mesh (.skmesh) "
                              "uses that skeleton — nothing to preview it on.",
                              name, signature );
            return;
        }
        std::ranges::sort( candidates, {}, &decltype( candidates )::value_type::first );
        const auto& mesh = candidates.front().second;

        const auto& animClip        = clip->GetClip();
        m_ClipName                  = animClip.AnimationName.empty() ? name : animClip.AnimationName;
        m_MeshName                  = std::filesystem::path( candidates.front().first ).filename().string();
        m_Transport.DurationSeconds = animClip.DurationSeconds();
        m_Transport.DisplayRate =
             animClip.DisplayRate.IsValid() ? animClip.DisplayRate : Animation::DEFAULT_DISPLAY_RATE;

        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;

        const auto& slots = mesh->GetMaterialHandles();
        m_Preview->SetSkinnedMesh( mesh->GetMetadata().Handle,
                                   std::vector<Assets::AssetHandle>( slots.begin(), slots.end() ), clip );
        // The pose changes every frame the clip plays; the gate still skips a paused, unmoved pane.
        if ( m_PendingOrbitDegrees )
        {
            m_Preview->SetOrbit( glm::radians( m_PendingOrbitDegrees->x ),
                                 glm::radians( m_PendingOrbitDegrees->y ) );
            m_PendingOrbitDegrees.reset();
        }
    }

    void AnimationEditorDocument::DestroyPreview()
    {
        m_Preview.reset();
        m_UIHelper.reset();
    }

    void AnimationEditorDocument::SetPreviewViewpoint( const PreviewViewpoint& viewpoint )
    {
        if ( !m_Preview )
        {
            m_PendingOrbitDegrees = std::make_unique<glm::vec2>( viewpoint.YawDegrees, viewpoint.PitchDegrees );
            return;
        }
        m_Preview->SetOrbit( glm::radians( viewpoint.YawDegrees ), glm::radians( viewpoint.PitchDegrees ) );
    }

    std::vector<ISubjectDocument::DocumentAction> AnimationEditorDocument::Actions()
    {
        std::vector<DocumentAction> actions;
        actions.push_back( { "Play/Pause", [this]() { m_Transport.TogglePlay(); } } );
        actions.push_back( { "Next Frame", [this]() { m_Transport.StepFrames( 1 ); } } );
        actions.push_back( { "Previous Frame", [this]() { m_Transport.StepFrames( -1 ); } } );
        for ( const int percent : { 0, 25, 50, 75, 100 } )
            actions.push_back( { std::format( "Set Time {}%", percent ), [this, percent]()
                                 {
                                     m_Transport.Playing = false;
                                     m_Transport.SetTime( m_Transport.DurationSeconds * percent / 100.0 );
                                 } } );
        PreviewEnvironment::AppendActions( actions );
        return actions;
    }

    void AnimationEditorDocument::OnPreUpdate()
    {
        // The slot is not claimed until the window has been drawn (StaticMeshViewerDocument has the argument).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_RenderSize.x == 0u || m_RenderSize.y == 0u )
            return;

        // Real seconds, scaled by the transport's speed: playback runs at clip speed whatever the frame rate.
        m_Transport.Advance( static_cast<double>( ImGui::GetIO().DeltaTime ) );
        m_Preview->SetAnimationTime( m_Transport.Time );
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    void AnimationEditorDocument::DrawOverlay( const glm::vec2& origin ) const
    {
        // UE's viewport notice, top-left: what is previewed, on which frame, at which rate.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto  line = [&]( const int row, const std::string& text )
        {
            const ImVec2 at( origin.x + 10.0f,
                             origin.y + 8.0f + static_cast<float>( row ) * ImGui::GetTextLineHeight() );
            draw->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), IM_COL32( 0, 0, 0, 200 ), text.c_str() );
            draw->AddText( at, IM_COL32( 235, 235, 235, 255 ), text.c_str() );
        };
        line( 0, std::format( "Previewing Animation {}", m_ClipName ) );
        line( 1, std::format( "Frame {} / {}   {:.3f} s / {:.3f} s", m_Transport.FrameIndex(),
                              m_Transport.LastFrame(), m_Transport.Time, m_Transport.DurationSeconds ) );
        line( 2, std::format( "{:.2f} fps   mesh {}", m_Transport.DisplayRate.AsDouble(), m_MeshName ) );
    }

    void AnimationEditorDocument::DrawTransport()
    {
        AnimationTransport& t = m_Transport;
        if ( ImGui::Button( "|<" ) )
            t.ToStart();
        ImGui::SameLine();
        if ( ImGui::Button( "<|" ) )
            t.StepFrames( -1 );
        ImGui::SameLine();
        if ( ImGui::Button( t.Playing ? "Pause" : "Play", ImVec2( 60.0f, 0.0f ) ) )
            t.TogglePlay();
        ImGui::SameLine();
        if ( ImGui::Button( "|>" ) )
            t.StepFrames( 1 );
        ImGui::SameLine();
        if ( ImGui::Button( ">|" ) )
            t.ToEnd();
        ImGui::SameLine();
        ImGui::Checkbox( "Loop", &t.Loop );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 80.0f );
        const std::string speed = std::format( "x{:g}", t.Speed );
        if ( ImGui::BeginCombo( "##speed", speed.c_str() ) )
        {
            for ( const float option : AnimationTransport::kSpeeds )
                if ( ImGui::Selectable( std::format( "x{:g}", option ).c_str(), option == t.Speed ) )
                    t.Speed = option;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text( "%d / %d  (%.3f / %.3f s)", t.FrameIndex(), t.LastFrame(), t.Time, t.DurationSeconds );

        // The scrubber, in frames on the display grid; dragging pauses, like UE's.
        int frame = t.FrameIndex();
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::SliderInt( "##scrub", &frame, 0, std::max( t.LastFrame(), 0 ), "frame %d" ) )
        {
            t.Playing = false;
            t.SetTime( static_cast<double>( frame ) * t.FrameSeconds() );
        }
    }

    void AnimationEditorDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;

        if ( !m_Unavailable.empty() )
        {
            ImGui::TextWrapped( "%s", m_Unavailable.c_str() );
            return;
        }

        // Room on the right for the Skeleton Tree / Details that follow (ANV1c); the preview takes the rest.
        constexpr float kSideWidth   = 260.0f;
        const float  transportHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 avail           = ImGui::GetContentRegionAvail();
        const ImVec2 view( std::max( avail.x - kSideWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f ),
                           std::max( avail.y - transportHeight, 1.0f ) );
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        ImGui::BeginGroup();
        if ( ImGui::BeginChild( "##animview", view ) )
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
            DrawOverlay( glm::vec2( origin.x, origin.y ) );
        }
        ImGui::EndChild();
        DrawTransport();
        ImGui::EndGroup();

        ImGui::SameLine();
        if ( ImGui::BeginChild( "##animside", ImVec2( kSideWidth, avail.y ) ) )
        {
            ImGui::TextUnformatted( "Preview Scene" );
            ImGui::Separator();
            PreviewEnvironment::DrawEnvironmentRows();
            PreviewEnvironment::DrawShowFloor( "Show Floor" );
        }
        ImGui::EndChild();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestAnimationEditorDocument( Assets::AssetManager*        assets,
                                                                           const std::string&           path,
                                                                           const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != kAnimationClipExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::AnimationAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::AnimationAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as an animation clip — no editor was opened.",
                       path );
            return Outcome::Failed;
        }
        if ( const auto loaded = asset->EnsureLoaded( *assets ); !loaded )
        {
            LOG_ERROR( "[Assets] '{}' would not load as an animation clip — no editor was opened: {}", path,
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
