#include "AnimationEditorDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp>
#include <Editor/Panels/Sequencer/TimelineRuler.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        constexpr float kNotifyFlashSeconds = 0.35f; // how long a crossed notify stays lit
        constexpr float kTrackLabelWidth    = 150.0f;
        constexpr float kDiamondRadius      = 6.0f;
        constexpr auto  kAddNotifyPopup     = "##animaddnotify";
        constexpr auto  kEditNotifyPopup    = "##animeditnotify";

        void DrawDiamond( ImDrawList* dl, const ImVec2 c, const float r, const ImU32 fill )
        {
            dl->AddQuadFilled( ImVec2( c.x, c.y - r ), ImVec2( c.x + r, c.y ), ImVec2( c.x, c.y + r ),
                               ImVec2( c.x - r, c.y ), fill );
            dl->AddQuad( ImVec2( c.x, c.y - r ), ImVec2( c.x + r, c.y ), ImVec2( c.x, c.y + r ),
                         ImVec2( c.x - r, c.y ), IM_COL32( 20, 20, 20, 255 ) );
        }

        void CopyName( std::array<char, 128>& buffer, const std::string& name )
        {
            buffer.fill( '\0' );
            name.copy( buffer.data(), buffer.size() - 1 );
        }
    } // namespace

    AnimationEditorDocument::AnimationEditorDocument( const Assets::AssetHandle& clip,
                                                      Assets::AssetManager*      assets )
         : AnimationEditorBase( AssetSubjectTitle( clip, assets, "Animation" ), clip ), m_Assets( assets ),
           m_ClipPin( clip, "open in the Animation Editor" )
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
        (void)ClipAsset();
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
        static constexpr std::array kPercents = { 0, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90, 100 };
        for ( const int percent : kPercents )
            actions.push_back( { std::format( "Set Time {}%", percent ), [this, percent]()
                                 {
                                     m_Transport.Playing = false;
                                     m_Transport.SetTime( m_Transport.DurationSeconds * percent / 100.0 );
                                 } } );
        actions.push_back( { "Add Notify Track", [this]()
                             {
                                 if ( auto* asset = ClipAsset() )
                                     m_AddedNotifyRows =
                                          NotifyTrackCount( asset->GetClip().Notifies, m_AddedNotifyRows ) + 1;
                             } } );
        for ( const int percent : kPercents )
            actions.push_back( { std::format( "Add Notify at {}%", percent ), [this, percent]()
                                 {
                                     auto* asset = ClipAsset();
                                     if ( asset == nullptr )
                                         return;
                                     const auto& clip = asset->GetClip();
                                     (void)AddNotify( std::format( "Notify {}", clip.Notifies.size() + 1 ),
                                                      clip.DurationSeconds() * percent / 100.0,
                                                      NotifyTrackCount( clip.Notifies, m_AddedNotifyRows ) - 1 );
                                 } } );
        actions.push_back( { "Save", [this]() { (void)SaveDocument(); } } );
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
        const float  dt         = ImGui::GetIO().DeltaTime;
        const bool   wasPlaying = m_Transport.Playing;
        const double before     = m_Transport.Time;
        m_Transport.Advance( static_cast<double>( dt ) );

        // A notify the playhead crossed lights up (UE flashes it), by the Animator's own crossing rule.
        if ( ClipAsset() != nullptr )
        {
            const auto& clip = m_ClipAsset->GetClip();
            m_Flash.resize( clip.Notifies.size(), 0.0f );
            for ( float& flash : m_Flash )
                flash = std::max( flash - dt, 0.0f );
            if ( wasPlaying )
            {
                const bool looped = m_Transport.Time < before;
                for ( const size_t i :
                      CrossedNotifies( clip.Notifies, clip.TickRate, before, m_Transport.Time, looped ) )
                    m_Flash[i] = kNotifyFlashSeconds;
            }
        }
        m_Preview->SetAnimationTime( m_Transport.Time );
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    Assets::AnimationAsset* AnimationEditorDocument::ClipAsset()
    {
        if ( m_Assets == nullptr )
            return nullptr;
        // The sweep keeps this clip while the window is open (m_ClipPin). The reload below is for a clip some
        // other path unloaded (a reimport); it re-reads the file.
        if ( m_ClipAsset )
        {
            if ( !m_ClipAsset->IsReadyForUse() && !m_ClipAsset->EnsureLoaded( *m_Assets ) )
                return nullptr;
            return m_ClipAsset.get();
        }
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets->FindMetadataByHandle( handle );
        auto                      clip = m_Assets->FindByHandle<Assets::AnimationAsset>( handle );
        if ( meta == nullptr || !clip || !clip->EnsureLoaded( *m_Assets ) )
            return nullptr;
        m_ClipAsset      = clip;
        m_ClipPath       = meta->Filepath;
        m_OnDiskNotifies = clip->GetClip().Notifies;
        m_Tracked        = clip->IsReloadableFromFile();
        return m_ClipAsset.get();
    }

    bool AnimationEditorDocument::EditNotifies( std::vector<Animation::AnimationNotify> edited, std::string label )
    {
        if ( ClipAsset() == nullptr )
            return false;
        // No change hook: the dirty state is derived (GetDiskState compares with the file's notifies), so an
        // undo that outlives this window has nothing of the window to call.
        return ApplyNotifyEdit( m_ClipAsset->GetClipForAuthoring(), std::move( edited ), std::move( label ),
                                CommandHistory::Get(), {} );
    }

    bool AnimationEditorDocument::AddNotify( std::string name, const double seconds, const int32_t track )
    {
        if ( ClipAsset() == nullptr || name.empty() )
            return false;
        const auto& clip   = m_ClipAsset->GetClip();
        auto        edited = clip.Notifies;
        edited.push_back( { std::move( name ),
                            SnapNotifyTick( seconds, clip.TickRate, m_Transport.DisplayRate, clip.DurationTicks ),
                            std::max( track, 0 ) } );
        return EditNotifies( std::move( edited ), "Add Notify" );
    }

    ISubjectDocument::DiskState AnimationEditorDocument::GetDiskState() const
    {
        if ( !m_Tracked || !m_ClipAsset )
            return DiskState::Untracked;
        const auto same = []( const Animation::AnimationNotify& a, const Animation::AnimationNotify& b )
        { return a.Name == b.Name && a.Tick.Value == b.Tick.Value && a.Track == b.Track; };
        return std::ranges::equal( m_ClipAsset->GetClip().Notifies, m_OnDiskNotifies, same ) ? DiskState::Clean
                                                                                             : DiskState::Dirty;
    }

    bool AnimationEditorDocument::SaveDocument()
    {
        // A clip no file produced (a procedural one) has nowhere to go; false, never an invented path.
        if ( ClipAsset() == nullptr || !m_Tracked || m_ClipPath.empty() )
            return false;
        const auto& clip    = m_ClipAsset->GetClip();
        const auto  written = Assets::Serialization::SaveClipToFile( m_ClipPath, clip );
        m_SaveFailed        = !written;
        if ( !written )
        {
            m_SaveStatus = std::format( "Save failed: {}", written.GetError() );
            LOG_ERROR( "Animation Editor: '{}': {}", m_ClipPath.generic_string(), m_SaveStatus );
            return false;
        }
        m_OnDiskNotifies = clip.Notifies;
        m_SaveStatus     = std::format( "Saved {}", m_ClipPath.filename().string() );
        return true;
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
        if ( ImGui::Button( GetDiskState() == DiskState::Dirty ? "Save*" : "Save" ) )
            (void)SaveDocument();
        ImGui::SameLine();
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
        if ( !m_SaveStatus.empty() )
        {
            ImGui::SameLine();
            ImGui::TextColored( m_SaveFailed ? ImVec4( 1.0f, 0.4f, 0.35f, 1.0f )
                                             : ImVec4( 0.6f, 0.8f, 0.6f, 1.0f ),
                                "%s", m_SaveStatus.c_str() );
        }

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
        const float     timelineHeight  = TimelineHeight();
        const ImVec2    view(
             std::max( avail.x - kSideWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f ),
             std::max( avail.y - transportHeight - timelineHeight - ImGui::GetStyle().ItemSpacing.y, 1.0f ) );

        // Ctrl+S on this window writes the clip (UE's Save Asset in the asset editor).
        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && ImGui::GetIO().KeyCtrl &&
             ImGui::IsKeyPressed( ImGuiKey_S, false ) )
            (void)SaveDocument();
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
        DrawTimeline( view.x, timelineHeight );
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

    float AnimationEditorDocument::TimelineHeight() const
    {
        // Ruler, "Notifies" header, the tracks, "Curves (0)" header.
        const int32_t rows = m_ClipAsset ? NotifyTrackCount( m_ClipAsset->GetClip().Notifies, m_AddedNotifyRows )
                                         : std::max( m_AddedNotifyRows, 1 );
        return ImGui::GetFrameHeight() * static_cast<float>( 3 + rows ) + ImGui::GetStyle().WindowPadding.y * 2.0f;
    }

    void AnimationEditorDocument::DrawTimeline( const float width, const float height )
    {
        if ( ClipAsset() == nullptr )
            return;
        if ( !ImGui::BeginChild( "##animtimeline", ImVec2( width, height ), false,
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse ) )
        {
            ImGui::EndChild();
            return;
        }
        Animation::AnimationClip& clip   = m_ClipAsset->GetClipForAuthoring();
        ImDrawList*               dl     = ImGui::GetWindowDrawList();
        const ImVec2              at     = ImGui::GetCursorScreenPos();
        const float               rowH   = ImGui::GetFrameHeight();
        const float               laneX0 = at.x + kTrackLabelWidth;
        const float               laneW  = std::max( ImGui::GetContentRegionAvail().x - kTrackLabelWidth, 1.0f );
        const float               laneX1 = laneX0 + laneW;
        const auto axis = Sequencer::TimeAxis( laneX0, laneW, static_cast<float>( m_Transport.DurationSeconds ) );
        const int32_t rows        = NotifyTrackCount( clip.Notifies, m_AddedNotifyRows );
        const float   rulerTop    = at.y;
        const float   rulerBottom = rulerTop + rowH;
        const float   rowsTop     = rulerBottom + rowH; // under the "Notifies" header
        const float   curvesTop   = rowsTop + rowH * static_cast<float>( rows );
        const float   panelBottom = curvesTop + rowH;
        const ImVec2  mouse       = ImGui::GetIO().MousePos;
        const auto    rowOfY      = [&]( const float y )
        { return std::clamp( static_cast<int32_t>( std::floor( ( y - rowsTop ) / rowH ) ), 0, rows - 1 ); };
        const auto snapped = [&]( const float x ) {
            return SnapNotifyTick( axis.XToTime( x ), clip.TickRate, m_Transport.DisplayRate, clip.DurationTicks );
        };
        const auto tickX = [&]( const Animation::FrameNumber tick ) {
            return axis.TimeToX(
                 Animation::FrameTimeToSeconds( Animation::FrameTime{ tick, 0.0F }, clip.TickRate ) );
        };
        const auto text = [&]( const float x, const float rowTop, const char* label, const ImU32 colour )
        { dl->AddText( ImVec2( x, rowTop + ( rowH - ImGui::GetTextLineHeight() ) * 0.5f ), colour, label ); };

        // Backgrounds: ruler, the two section headers, alternating track rows.
        dl->AddRectFilled( ImVec2( at.x, rulerTop ), ImVec2( laneX1, rulerBottom ), IM_COL32( 36, 36, 36, 255 ) );
        dl->AddRectFilled( ImVec2( at.x, rulerBottom ), ImVec2( laneX1, rowsTop ), IM_COL32( 48, 48, 48, 255 ) );
        for ( int32_t r = 0; r < rows; ++r )
        {
            const float y = rowsTop + rowH * static_cast<float>( r );
            dl->AddRectFilled( ImVec2( at.x, y ), ImVec2( laneX1, y + rowH ),
                               r % 2 == 0 ? IM_COL32( 30, 30, 30, 255 ) : IM_COL32( 34, 34, 34, 255 ) );
            text( at.x + 18.0f, y, std::format( "Track {}", r + 1 ).c_str(), IM_COL32( 190, 190, 190, 255 ) );
        }
        dl->AddRectFilled( ImVec2( at.x, curvesTop ), ImVec2( laneX1, panelBottom ), IM_COL32( 48, 48, 48, 255 ) );
        dl->AddLine( ImVec2( laneX0, rulerTop ), ImVec2( laneX0, panelBottom ), IM_COL32( 20, 20, 20, 255 ) );
        Sequencer::DrawFrameGrid( dl, axis, rulerTop, rulerBottom, clip.DurationTicks, clip.TickRate,
                                  m_Transport.DisplayRate, true, IM_COL32( 120, 120, 120, 255 ) );
        Sequencer::DrawFrameGrid( dl, axis, rowsTop, curvesTop, clip.DurationTicks, clip.TickRate,
                                  m_Transport.DisplayRate, false, IM_COL32( 55, 55, 55, 255 ) );
        text( at.x + 6.0f, rulerBottom, "Notifies", IM_COL32( 230, 230, 230, 255 ) );
        // No float curves in AnimationClip yet: the header says so by its count, as UE's does for none.
        text( at.x + 6.0f, curvesTop, "Curves (0)", IM_COL32( 230, 230, 230, 255 ) );

        ImGui::SetCursorScreenPos( ImVec2( laneX0 - 64.0f, rulerBottom + 1.0f ) );
        if ( ImGui::SmallButton( "+ Track" ) )
            m_AddedNotifyRows = rows + 1;

        // The ruler scrubs: a click or drag pauses and moves the playhead, like UE's.
        ImGui::SetCursorScreenPos( ImVec2( laneX0, rulerTop ) );
        ImGui::InvisibleButton( "##animruler", ImVec2( laneW, rowH ) );
        if ( ImGui::IsItemActive() )
        {
            m_Transport.Playing = false;
            m_Transport.SetTime( std::clamp( axis.XToTime( mouse.x ), 0.0, m_Transport.DurationSeconds ) );
        }

        // Notifies: a diamond on its tick, on its track; drag moves (one edit on release), right-click edits.
        m_Flash.resize( clip.Notifies.size(), 0.0f );
        bool overNotify = false;
        for ( size_t i = 0; i < clip.Notifies.size(); ++i )
        {
            const auto&  notify = clip.Notifies[i];
            const ImVec2 centre( tickX( notify.Tick ),
                                 rowsTop + rowH * ( static_cast<float>( notify.Track ) + 0.5f ) );
            ImGui::SetCursorScreenPos( ImVec2( centre.x - kDiamondRadius, centre.y - kDiamondRadius ) );
            ImGui::PushID( static_cast<int>( i ) );
            ImGui::InvisibleButton( "##notify", ImVec2( kDiamondRadius * 2.0f, kDiamondRadius * 2.0f ) );
            ImGui::PopID();
            overNotify |= ImGui::IsItemHovered();
            const bool dragging = m_DragNotify == static_cast<int32_t>( i );
            if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 2.0f ) )
                m_DragNotify = static_cast<int32_t>( i );
            if ( ImGui::IsItemDeactivated() && dragging )
            {
                auto edited     = clip.Notifies;
                edited[i].Tick  = snapped( mouse.x );
                edited[i].Track = rowOfY( mouse.y );
                m_DragNotify    = -1;
                (void)EditNotifies( std::move( edited ), "Move Notify" );
                break; // the list was replaced (and re-sorted); draw it next frame
            }
            if ( ImGui::IsItemClicked( ImGuiMouseButton_Right ) )
            {
                m_PopupNotify = static_cast<int32_t>( i );
                CopyName( m_NameBuffer, notify.Name );
                ImGui::OpenPopup( kEditNotifyPopup );
            }

            const float flash = m_Flash[i] / kNotifyFlashSeconds;
            const ImU32 fill  = dragging ? IM_COL32( 120, 120, 120, 255 )
                                         : ImGui::GetColorU32( ImVec4( 0.85f + 0.15f * flash, 0.70f + 0.30f * flash,
                                                                       0.30f + 0.55f * flash, 1.0f ) );
            DrawDiamond( dl, centre, kDiamondRadius, fill );
            text( centre.x + kDiamondRadius + 3.0f, centre.y - rowH * 0.5f, notify.Name.c_str(),
                  flash > 0.0f ? IM_COL32( 255, 255, 220, 255 ) : IM_COL32( 220, 220, 220, 255 ) );
            if ( dragging ) // the ghost: where the notify lands on release, snapped to the frame grid
                DrawDiamond( dl,
                             ImVec2( tickX( snapped( mouse.x ) ),
                                     rowsTop + rowH * ( static_cast<float>( rowOfY( mouse.y ) ) + 0.5f ) ),
                             kDiamondRadius, IM_COL32( 255, 200, 90, 255 ) );
        }

        // Right-click on a track's empty lane: Add Notify there.
        const bool overRows = mouse.x >= laneX0 && mouse.x < laneX1 && mouse.y >= rowsTop && mouse.y < curvesTop;
        if ( !overNotify && overRows && ImGui::IsWindowHovered() &&
             ImGui::IsMouseClicked( ImGuiMouseButton_Right ) )
        {
            m_PopupTrack   = rowOfY( mouse.y );
            m_PopupSeconds = axis.XToTime( mouse.x );
            CopyName( m_NameBuffer, "New Notify" );
            ImGui::OpenPopup( kAddNotifyPopup );
        }
        DrawNotifyPopups( clip );

        Sequencer::DrawPlayhead( dl, axis, m_Transport.Time, rulerTop, panelBottom, rulerBottom );
        ImGui::SetCursorScreenPos( ImVec2( at.x, panelBottom ) );
        ImGui::Dummy( ImVec2( 1.0f, 1.0f ) );
        ImGui::EndChild();
    }

    void AnimationEditorDocument::DrawNotifyPopups( Animation::AnimationClip& clip )
    {
        if ( ImGui::BeginPopup( kAddNotifyPopup ) )
        {
            ImGui::TextDisabled( "Track %d, %.3f s", m_PopupTrack + 1, m_PopupSeconds );
            ImGui::SetNextItemWidth( 180.0f );
            const bool enter = ImGui::InputText( "##name", m_NameBuffer.data(), m_NameBuffer.size(),
                                                 ImGuiInputTextFlags_EnterReturnsTrue );
            if ( ImGui::MenuItem( "Add Notify" ) || enter )
            {
                (void)AddNotify( m_NameBuffer.data(), m_PopupSeconds, m_PopupTrack );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if ( ImGui::BeginPopup( kEditNotifyPopup ) )
        {
            const auto index = static_cast<size_t>( m_PopupNotify );
            if ( m_PopupNotify < 0 || index >= clip.Notifies.size() )
                ImGui::CloseCurrentPopup();
            else
            {
                ImGui::SetNextItemWidth( 180.0f );
                const bool enter = ImGui::InputText( "##rename", m_NameBuffer.data(), m_NameBuffer.size(),
                                                     ImGuiInputTextFlags_EnterReturnsTrue );
                if ( ( ImGui::MenuItem( "Rename" ) || enter ) && m_NameBuffer[0] != '\0' )
                {
                    auto edited        = clip.Notifies;
                    edited[index].Name = m_NameBuffer.data();
                    (void)EditNotifies( std::move( edited ), "Rename Notify" );
                    ImGui::CloseCurrentPopup();
                }
                if ( ImGui::MenuItem( "Delete" ) )
                {
                    auto edited = clip.Notifies;
                    edited.erase( edited.begin() + static_cast<std::ptrdiff_t>( index ) );
                    (void)EditNotifies( std::move( edited ), "Delete Notify" );
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
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
