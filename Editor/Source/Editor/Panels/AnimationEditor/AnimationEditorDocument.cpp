#include "AnimationEditorDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonTree.hpp>
#include <Editor/Panels/Sequencer/TimelineRuler.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Animation/Animator.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>

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

        // A titled rule (ImGui 1.89.4's SeparatorText; this tree has 1.89 WIP).
        void SectionHeader( const char* label )
        {
            ImGui::Spacing();
            ImGui::TextUnformatted( label );
            ImGui::Separator();
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
        m_MeshCandidates = std::move( candidates );
        m_MeshIndex      = 0;

        const auto& animClip        = clip->GetClip();
        m_ClipName                  = animClip.AnimationName.empty() ? name : animClip.AnimationName;
        m_Transport.DurationSeconds = animClip.DurationSeconds();
        m_Transport.DisplayRate =
             animClip.DisplayRate.IsValid() ? animClip.DisplayRate : Animation::DEFAULT_DISPLAY_RATE;

        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;

        SetPreviewMesh( 0 );
        // The pose changes every frame the clip plays; the gate still skips a paused, unmoved pane.
        if ( m_PendingOrbitDegrees )
        {
            m_Preview->SetOrbit( glm::radians( m_PendingOrbitDegrees->x ),
                                 glm::radians( m_PendingOrbitDegrees->y ) );
            m_PendingOrbitDegrees.reset();
        }
    }

    void AnimationEditorDocument::SetPreviewMesh( const size_t candidate )
    {
        if ( !m_Preview || candidate >= m_MeshCandidates.size() || !m_ClipAsset )
            return;
        const auto& [path, mesh] = m_MeshCandidates[candidate];
        m_MeshIndex              = candidate;
        m_MeshName               = std::filesystem::path( path ).filename().string();
        const auto& slots        = mesh->GetMaterialHandles();
        m_Preview->SetSkinnedMesh( mesh->GetMetadata().Handle,
                                   std::vector<Assets::AssetHandle>( slots.begin(), slots.end() ), m_ClipAsset );
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
        actions.push_back( { "Show Bones On", [this]() { m_ShowBones = true; } } );
        actions.push_back( { "Show Bones Off", [this]() { m_ShowBones = false; } } );
        // One entry per bone of the rig the preview plays: "<clip>: Select Bone <name>" in the palette.
        if ( const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr )
            for ( const auto& bone : animator->GetSkeleton().GetBones() )
                actions.push_back( { std::format( "Select Bone {}", bone.Name ), [this, name = bone.Name]()
                                     {
                                         if ( const auto* rig = m_Preview ? m_Preview->GetAnimator() : nullptr )
                                             m_SelectedBone = BoneByName( rig->GetSkeleton(), name );
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

    std::string AnimationEditorDocument::PanelTitle( const char* name ) const
    {
        // "###" + the subject: two open clips must not share one "Skeleton Tree" window.
        return std::format( "{}###{}{}", name, name, static_cast<uint64_t>( Subject().Owner ) );
    }

    void AnimationEditorDocument::BuildLayout( const unsigned int dockId ) const
    {
        // UE Persona: Asset Details | Skeleton Tree left, the viewport centre, Details | Preview Scene Settings
        // right. The shares are Persona's defaults at a 1200 px window, not measured from a screenshot.
        ImGui::DockBuilderRemoveNode( dockId );
        ImGui::DockBuilderAddNode( dockId, ImGuiDockNodeFlags_DockSpace );
        ImGui::DockBuilderSetNodeSize( dockId, ImGui::GetContentRegionAvail() );
        ImGuiID           center = dockId;
        const ImGuiID     left   = ImGui::DockBuilderSplitNode( center, ImGuiDir_Left, 0.22f, nullptr, &center );
        const ImGuiID     right  = ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.30f, nullptr, &center );
        const std::string assetDetails = PanelTitle( "Asset Details" );
        const std::string skeletonTree = PanelTitle( "Skeleton Tree" );
        const std::string details      = PanelTitle( "Details" );
        const std::string previewScene = PanelTitle( "Preview Scene Settings" );
        ImGui::DockBuilderDockWindow( assetDetails.c_str(), left );
        ImGui::DockBuilderDockWindow( skeletonTree.c_str(), left );
        ImGui::DockBuilderDockWindow( details.c_str(), right );
        ImGui::DockBuilderDockWindow( previewScene.c_str(), right );
        ImGui::DockBuilderDockWindow( PanelTitle( "Viewport" ).c_str(), center );
        // The tree and the bone's Details are the tabs in front, as in Persona.
        ImGui::DockBuilderGetNode( left )->SelectedTabId  = ImHashStr( skeletonTree.c_str() );
        ImGui::DockBuilderGetNode( right )->SelectedTabId = ImHashStr( details.c_str() );
        ImGui::DockBuilderGetNode( center )->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
        ImGui::DockBuilderFinish( dockId );
    }

    void AnimationEditorDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End. The panels are windows of
        // their own, docked into this window's DockSpace (a nested Begin is legal).
        m_DrewThisFrame = true;

        if ( !m_Unavailable.empty() )
        {
            ImGui::TextWrapped( "%s", m_Unavailable.c_str() );
            return;
        }

        const ImGuiID        dockId = ImGui::GetID( "AnimDock" );
        const ImGuiDockNode* node   = ImGui::DockBuilderGetNode( dockId );
        if ( node == nullptr || node->IsLeafNode() )
            BuildLayout( dockId );
        ImGui::DockSpace( dockId, ImVec2( 0.0f, 0.0f ) );

        const auto panel = [this]( const char* name, const ImGuiWindowFlags flags, auto&& draw )
        {
            if ( ImGui::Begin( PanelTitle( name ).c_str(), nullptr, flags ) )
                draw();
            ImGui::End();
        };
        panel( "Viewport", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
               [this]() { DrawViewportPanel(); } );
        panel( "Asset Details", 0, [this]() { DrawAssetDetails(); } );
        panel( "Skeleton Tree", 0, [this]() { DrawSkeletonTree(); } );
        panel( "Details", 0, [this]() { DrawBoneDetails(); } );
        panel( "Preview Scene Settings", 0, [this]() { DrawPreviewSceneSettings(); } );
    }

    void AnimationEditorDocument::DrawViewportPanel()
    {
        const float  transportHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 avail           = ImGui::GetContentRegionAvail();
        const float  timelineHeight  = TimelineHeight();
        const ImVec2 view(
             std::max( avail.x, 1.0f ),
             std::max( avail.y - transportHeight - timelineHeight - ImGui::GetStyle().ItemSpacing.y, 1.0f ) );
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        if ( ImGui::BeginChild( "##animview", view, false,
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse ) )
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
            DrawBones( glm::vec2( origin.x, origin.y ), glm::vec2( view.x, view.y ) );
            DrawOverlay( glm::vec2( origin.x, origin.y ) );
        }
        ImGui::EndChild();
        DrawTransport();
        DrawTimeline( view.x, timelineHeight );
    }

    void AnimationEditorDocument::DrawBones( const glm::vec2& origin, const glm::vec2& size ) const
    {
        const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr;
        if ( animator == nullptr || ( !m_ShowBones && !m_SelectedBone ) )
            return;
        const auto&     skeleton = animator->GetSkeleton();
        const auto      count    = static_cast<uint32_t>( skeleton.GetBones().size() );
        const glm::mat4 viewProj = m_Preview->GetViewProjection();
        const glm::mat4 model    = m_Preview->GetTargetTransform();
        // The same projection the level viewport's tools use (CreateShapeTool's WorldToScreen): NDC y up.
        const auto project = [&]( const uint32_t bone, ImVec2& out )
        {
            const glm::vec4 world = model * animator->GetBoneModelMatrix( bone ) * glm::vec4( 0, 0, 0, 1 );
            const glm::vec4 clip  = viewProj * world;
            if ( clip.w <= 0.0001f )
                return false;
            const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
            out                 = ImVec2( origin.x + ( ndc.x * 0.5f + 0.5f ) * size.x,
                                          origin.y + ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * size.y );
            return true;
        };
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto  bone = [&]( const uint32_t i, const ImU32 colour, const float thickness )
        {
            ImVec2 at;
            if ( !project( i, at ) )
                return;
            draw->AddCircleFilled( at, thickness + 1.5f, colour );
            ImVec2         parentAt;
            const uint32_t parent = skeleton.ResolveParent( i );
            if ( parent < count && project( parent, parentAt ) )
                draw->AddLine( parentAt, at, colour, thickness );
        };
        if ( m_ShowBones )
            for ( uint32_t i = 0; i < count; ++i )
                bone( i, IM_COL32( 220, 220, 220, 200 ), 1.5f );
        if ( m_SelectedBone && *m_SelectedBone < count )
        {
            bone( *m_SelectedBone, IM_COL32( 255, 170, 30, 255 ), 3.0f );
            if ( ImVec2 at; project( *m_SelectedBone, at ) )
            {
                const std::string& name = skeleton.GetBones()[*m_SelectedBone].Name;
                draw->AddText( ImVec2( at.x + 9.0f, at.y - 7.0f ), IM_COL32( 0, 0, 0, 220 ), name.c_str() );
                draw->AddText( ImVec2( at.x + 8.0f, at.y - 8.0f ), IM_COL32( 255, 200, 90, 255 ), name.c_str() );
            }
        }
    }

    void AnimationEditorDocument::DrawSkeletonTree()
    {
        const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr;
        if ( animator == nullptr )
        {
            ImGui::TextDisabled( "Starting the preview..." );
            return;
        }
        const auto& skeleton = animator->GetSkeleton();
        const auto& bones    = skeleton.GetBones();
        m_CollapsedBones.resize( bones.size(), false );

        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputTextWithHint( "##bonefilter", "Search Bones", m_BoneFilter.data(), m_BoneFilter.size() );
        ImGui::Checkbox( "Show Bones", &m_ShowBones );
        ImGui::Separator();

        if ( !ImGui::BeginChild( "##bonerows" ) )
        {
            ImGui::EndChild();
            return;
        }
        const float indent = ImGui::GetStyle().IndentSpacing;
        const float arrow  = ImGui::GetFontSize();
        for ( const SkeletonTreeRow& row :
              BuildSkeletonTreeRows( skeleton, m_BoneFilter.data(), m_CollapsedBones ) )
        {
            ImGui::PushID( static_cast<int>( row.Bone ) );
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + indent * static_cast<float>( row.Depth ) );
            if ( row.HasChildren )
            {
                const bool collapsed = m_CollapsedBones[row.Bone];
                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0, 0, 0, 0 ) );
                if ( ImGui::ArrowButtonEx( "##fold", collapsed ? ImGuiDir_Right : ImGuiDir_Down,
                                           ImVec2( arrow, arrow ) ) )
                    m_CollapsedBones[row.Bone] = !collapsed;
                ImGui::PopStyleColor();
            }
            else
                ImGui::Dummy( ImVec2( arrow, arrow ) );
            ImGui::SameLine();
            // A row shown only as the ancestor of a search hit is dimmed, as in UE's filtered tree.
            if ( !row.Matches )
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            if ( ImGui::Selectable( bones[row.Bone].Name.c_str(), m_SelectedBone == row.Bone ) )
                m_SelectedBone = row.Bone;
            if ( !row.Matches )
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void AnimationEditorDocument::DrawBoneDetails() const
    {
        const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr;
        if ( animator == nullptr || !m_SelectedBone ||
             *m_SelectedBone >= animator->GetSkeleton().GetBones().size() )
        {
            ImGui::TextDisabled( "Select a bone in the Skeleton Tree." );
            return;
        }
        const auto&    skeleton = animator->GetSkeleton();
        const uint32_t index    = *m_SelectedBone;
        const auto&    bone     = skeleton.GetBones()[index];
        const uint32_t parent   = skeleton.ResolveParent( index );
        ImGui::Text( "Bone  %s", bone.Name.c_str() );
        ImGui::TextDisabled( "Index %u   Parent %s", index,
                             parent < skeleton.GetBones().size() ? skeleton.GetBones()[parent].Name.c_str()
                                                                 : "(root)" );

        // Read-only: the pose is the clip's at the playhead, the reference is the rig's rest pose.
        const auto section = [index]( const char* label, const BoneTransformRows& rows )
        {
            SectionHeader( label );
            const auto row = [index]( const char* name, glm::vec3 value )
            {
                ImGui::PushID( name );
                ImGui::PushID( static_cast<int>( index ) );
                ImGui::SetNextItemWidth( -80.0f );
                ImGui::InputFloat3( name, &value.x, "%.3f", ImGuiInputTextFlags_ReadOnly );
                ImGui::PopID();
                ImGui::PopID();
            };
            row( "Location", rows.Location );
            row( "Rotation", rows.RotationDegrees );
            row( "Scale", rows.Scale );
        };
        section( std::format( "Bone (frame {})", m_Transport.FrameIndex() ).c_str(),
                 DecomposeBoneTransform( animator->GetBoneLocalPose( index ) ) );
        section( "Reference Pose", DecomposeBoneTransform( bone.LocalBindTransform ) );
    }

    void AnimationEditorDocument::DrawAssetDetails()
    {
        const auto* asset = ClipAsset();
        if ( asset == nullptr )
        {
            ImGui::TextDisabled( "The clip is not loaded." );
            return;
        }
        // Only what an AnimationClip holds: UE's Rate Scale, compression and additive settings have no field
        // here, so they are not shown.
        const auto& clip = asset->GetClip();
        size_t      keys = 0;
        for ( const auto& track : clip.Tracks )
            keys += track.PositionKeys.size() + track.RotationKeys.size() + track.ScaleKeys.size();
        if ( !ImGui::BeginTable( "##clipdetails", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
            return;
        const auto row = []( const char* label, const std::string& value )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled( "%s", label );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( value.c_str() );
        };
        row( "Animation", clip.AnimationName.empty() ? m_ClipName : clip.AnimationName );
        row( "File", m_ClipPath.empty() ? std::string( "(none)" ) : m_ClipPath.filename().string() );
        row( "Skeleton", std::format( "{:016x}", clip.SkeletonSignature ) );
        row( "Length", std::format( "{:.3f} s", clip.DurationSeconds() ) );
        row( "Frames", std::format( "{}", m_Transport.LastFrame() + 1 ) );
        row( "Display Rate",
             std::format( "{}/{} fps", clip.DisplayRate.Numerator, clip.DisplayRate.Denominator ) );
        row( "Tick Rate", std::format( "{}/{} ({} ticks)", clip.TickRate.Numerator, clip.TickRate.Denominator,
                                       clip.DurationTicks.Value ) );
        row( "Bone Tracks", std::format( "{}", clip.Tracks.size() ) );
        row( "Keys", std::format( "{}", keys ) );
        row( "Notifies", std::format( "{}", clip.Notifies.size() ) );
        row( "Sections", std::format( "{}", clip.Sections.size() ) );
        ImGui::EndTable();
    }

    void AnimationEditorDocument::DrawPreviewSceneSettings()
    {
        // UE's Preview Mesh: any registered skeletal mesh on the clip's rig (same skeleton signature).
        SectionHeader( "Mesh" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##previewmesh", m_MeshName.c_str() ) )
        {
            for ( size_t i = 0; i < m_MeshCandidates.size(); ++i )
                if ( ImGui::Selectable( m_MeshCandidates[i].first.c_str(), i == m_MeshIndex ) && i != m_MeshIndex )
                    SetPreviewMesh( i );
            ImGui::EndCombo();
        }
        SectionHeader( "Environment" );
        PreviewEnvironment::DrawEnvironmentRows();
        PreviewEnvironment::DrawShowFloor( "Show Floor" );
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
