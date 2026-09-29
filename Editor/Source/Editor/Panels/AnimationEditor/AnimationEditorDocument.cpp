#include <Editor/Core/GizmoIdScope.hpp>
#include <Editor/Core/Control/PointerDrag.hpp>
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
#include <Common/Content/ContentKinds.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Animation/Animator.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>
#include <ImGuizmo.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <cctype>
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
        constexpr auto  kAddCurvePopup      = "##animaddcurve";
        constexpr auto  kAddCurveKeyPopup   = "##animaddcurvekey";
        constexpr auto  kCurveKeyPopup      = "##animcurvekey";
        constexpr float kEdgeGrip           = 4.0f; // half the width of a Notify State's resize grip
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

        // Load ONE preview mesh a registry row named, and hold the row to its word: the Rig tag is what the scan
        // read, the loaded mesh is what the file is now.
        std::shared_ptr<Assets::SkinnedMeshAsset> LoadPreviewMesh( Assets::AssetManager&        assets,
                                                                   const std::filesystem::path& path,
                                                                   const uint64_t signature, std::string& error )
        {
            auto mesh = assets.CreateAsset<Assets::SkinnedMeshAsset>( path, false );
            if ( !mesh )
            {
                error = std::format( "skeletal mesh '{}' could not be registered", path.generic_string() );
                return nullptr;
            }
            if ( const auto loaded = mesh->EnsureLoaded( assets ); !loaded )
            {
                error = std::format( "skeletal mesh '{}' would not load: {}", path.generic_string(),
                                     loaded.GetError() );
                return nullptr;
            }
            if ( mesh->GetSkeletonSignature() != signature )
            {
                error = std::format( "skeletal mesh '{}' is registered on skeleton {:016x} but its file names "
                                     "{:016x} — rescan the content",
                                     path.generic_string(), signature, mesh->GetSkeletonSignature() );
                return nullptr;
            }
            return mesh;
        }

        void CopyName( std::array<char, 128>& buffer, const std::string& name )
        {
            buffer.fill( '\0' );
            name.copy( buffer.data(), buffer.size() - 1 );
        }
    } // namespace

    AnimationEditorDocument::AnimationEditorDocument( const Assets::AssetHandle& asset,
                                                      const Core::PersonaMode mode, Assets::AssetManager* assets,
                                                      const SubjectEditorRegistry* editors )
         : AnimationEditorBase( AssetSubjectTitle( asset, assets, PersonaModeName( mode ) ), asset, mode ),
           m_Assets( assets ), m_Editors( editors ), m_ClipPin( asset, "open in the Animation Editor" )
    {
    }

    AnimationEditorDocument::~AnimationEditorDocument()
    {
        // The undo records of this window write through a raw pointer into the clip's payload, which the
        // manager may evict once m_ClipPin is gone: they leave the process-wide history with the window, so an
        // Undo after the close never writes into a freed clip (ClipEditUndo, AClosedEditorsRecords...).
        if ( m_ClipAsset )
            CommandHistory::Get().DropFor( &m_ClipAsset->GetClip() );
    }

    bool AnimationEditorDocument::DiscardEdits()
    {
        // "Don't Save" on the close question: the shared asset outlives the window, so the file's notifies and
        // curves go back into it - otherwise the next opening would show the discarded edits as unsaved.
        if ( !m_ClipAsset || !m_OnDisk )
            return false;
        auto& clip    = m_ClipAsset->GetClipForAuthoring();
        clip.Notifies = m_OnDisk->Notifies;
        clip.Curves   = m_OnDisk->Curves;
        clip.Tracks   = m_OnDisk->Tracks;
        CommandHistory::Get().DropFor( &clip );
        return true;
    }

    bool AnimationEditorDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    bool AnimationEditorDocument::ResolveRig( const std::string& name, std::filesystem::path& preferredMesh )
    {
        const Assets::AssetHandle handle( Subject().Owner );
        switch ( Mode() )
        {
            case Core::PersonaMode::Animation:
            {
                auto clip = m_Assets->FindByHandle<Assets::AnimationAsset>( handle );
                if ( !clip )
                {
                    m_Unavailable = std::format( "'{}' is not an AnimationAsset in the asset manager.", name );
                    return false;
                }
                if ( const auto loaded = clip->EnsureLoaded( *m_Assets ); !loaded )
                {
                    m_Unavailable = std::format( "'{}' would not load: {}", name, loaded.GetError() );
                    return false;
                }
                (void)ClipAsset();
                m_Signature = clip->GetSkeletonSignature();
                break;
            }
            case Core::PersonaMode::Mesh:
            {
                // The mesh opened IS the preview: Persona's Mesh mode shows its own asset, in the bind pose.
                auto mesh = m_Assets->FindByHandle<Assets::SkinnedMeshAsset>( handle );
                if ( !mesh )
                {
                    m_Unavailable = std::format( "'{}' is not a SkinnedMeshAsset in the asset manager.", name );
                    return false;
                }
                if ( const auto loaded = mesh->EnsureLoaded( *m_Assets ); !loaded )
                {
                    m_Unavailable = std::format( "'{}' would not load: {}", name, loaded.GetError() );
                    return false;
                }
                m_Signature   = mesh->GetSkeletonSignature();
                preferredMesh = mesh->GetMetadata().Filepath;
                break;
            }
            case Core::PersonaMode::Skeleton:
            {
                auto skeleton = m_Assets->FindByHandle<Assets::SkeletonAsset>( handle );
                if ( !skeleton )
                {
                    m_Unavailable = std::format( "'{}' is not a SkeletonAsset in the asset manager.", name );
                    return false;
                }
                if ( const auto loaded = skeleton->EnsureLoaded( *m_Assets ); !loaded )
                {
                    m_Unavailable = std::format( "'{}' would not load: {}", name, loaded.GetError() );
                    return false;
                }
                m_Signature = skeleton->GetSignature();
                break;
            }
        }
        if ( m_Signature == 0 )
        {
            m_Unavailable = std::format( "'{}' names no skeleton (signature 0) — no mesh can show it.", name );
            return false;
        }
        return true;
    }

    void AnimationEditorDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Unavailable.empty() )
            return;

        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets != nullptr ? m_Assets->FindMetadataByHandle( handle ) : nullptr;
        if ( meta == nullptr )
        {
            m_Unavailable = std::format( "This {} is not registered with the asset manager — nothing to show.",
                                         PersonaModeName( Mode() ) );
            return;
        }
        const std::string     name = meta->Filepath.filename().string();
        std::filesystem::path preferred;
        if ( !ResolveRig( name, preferred ) )
            return;
        const uint64_t signature = m_Signature;

        // The first `.skmesh` by path whose rig is the subject's: a stable pick, so two openings show one mesh
        // (Mesh mode shows its own). Asked of the CONTENT REGISTRY by its Rig tag, which the scan read from each
        // mesh's header: nothing is loaded to list them, and only the mesh shown is loaded (ANV1c3 loaded every
        // one, in a frame).
        std::vector<std::filesystem::path> candidates;
        for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::SkinnedMesh ) )
            if ( row.RigSignature == signature )
                candidates.push_back( row.Path );
        std::ranges::sort( candidates );
        size_t shown = 0;
        if ( !preferred.empty() )
        {
            // By the registry's spelling of the path when it lists the mesh; a mesh the scan has not seen yet
            // (created since) is still the one this window is about.
            const auto found =
                 std::ranges::find_if( candidates, [&]( const std::filesystem::path& path )
                                       { return path.lexically_normal() == preferred.lexically_normal(); } );
            if ( found == candidates.end() )
                candidates.insert( candidates.begin(), preferred );
            else
                shown = static_cast<size_t>( found - candidates.begin() );
        }
        if ( candidates.empty() )
        {
            m_Unavailable = std::format( "'{}' is on skeleton {:016x}, and no registered skeletal mesh (.skmesh) "
                                         "uses that skeleton — nothing to preview it on.",
                                         name, signature );
            return;
        }
        m_MeshCandidates = std::move( candidates );
        std::string meshError;
        m_Mesh = LoadPreviewMesh( *m_Assets, m_MeshCandidates[shown], signature, meshError );
        if ( !m_Mesh )
        {
            m_Unavailable = meshError;
            return;
        }
        m_MeshIndex = shown;

        if ( m_ClipAsset )
        {
            const auto& animClip        = m_ClipAsset->GetClip();
            m_ClipName                  = animClip.AnimationName.empty() ? name : animClip.AnimationName;
            m_Transport.DurationSeconds = animClip.DurationSeconds();
            m_Transport.DisplayRate =
                 animClip.DisplayRate.IsValid() ? animClip.DisplayRate : Animation::DEFAULT_DISPLAY_RATE;
        }

        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;

        SetPreviewMesh( shown );
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
        if ( !m_Preview || candidate >= m_MeshCandidates.size() || m_Signature == 0 )
            return;
        if ( candidate != m_MeshIndex || !m_Mesh )
        {
            std::string error;
            auto        mesh = LoadPreviewMesh( *m_Assets, m_MeshCandidates[candidate], m_Signature, error );
            if ( !mesh )
            {
                m_SaveStatus = error;
                m_SaveFailed = true;
                LOG_ERROR( "Animation Editor: {}", error );
                return;
            }
            m_Mesh = std::move( mesh );
        }
        m_MeshIndex     = candidate;
        m_MeshName      = m_MeshCandidates[candidate].filename().string();
        m_BrowserListed = false; // the rig the browser lists for may have changed
        // The preview rebuilds its animator for the new mesh: the posing and every pose record that writes
        // into the old one go first (DropPoseRecordsFor), the clip's other records stay.
        EndPosing();
        (void)DropPoseRecordsFor( m_Preview->GetAnimator() );
        const auto& mesh  = m_Mesh;
        const auto& slots = mesh->GetMaterialHandles();
        m_Preview->SetSkinnedMesh( mesh->GetMetadata().Handle,
                                   std::vector<Assets::AssetHandle>( slots.begin(), slots.end() ), m_ClipAsset );
    }

    void AnimationEditorDocument::DestroyPreview()
    {
        if ( m_Preview )
        {
            EndPosing();
            (void)DropPoseRecordsFor( m_Preview->GetAnimator() );
        }
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
        for ( const auto mode :
              { Core::PersonaMode::Skeleton, Core::PersonaMode::Mesh, Core::PersonaMode::Animation } )
            actions.push_back(
                 { std::format( "Mode {}", PersonaModeName( mode ) ), [this, mode]() { OpenMode( mode ); } } );
        // The transport, the notifies and the curves are about a clip: the Skeleton and Mesh modes have none,
        // so their palette does not offer edits that could only fail.
        if ( Mode() == Core::PersonaMode::Animation )
        {
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
                                                          NotifyTrackCount( clip.Notifies, m_AddedNotifyRows ) -
                                                               1 );
                                     } } );
            // The palette's (and DesertCtl's) way to the same edits the timeline's right-click menus make.
            for ( const int percent : kPercents )
            {
                actions.push_back( { std::format( "Add Notify State at {}%", percent ), [this, percent]()
                                     {
                                         auto* asset = ClipAsset();
                                         if ( asset == nullptr )
                                             return;
                                         const auto& clip = asset->GetClip();
                                         (void)AddNotify( std::format( "State {}", clip.Notifies.size() + 1 ),
                                                          clip.DurationSeconds() * percent / 100.0,
                                                          NotifyTrackCount( clip.Notifies, m_AddedNotifyRows ) - 1,
                                                          std::max( clip.DurationTicks.Value / 4, 1 ) );
                                     } } );
                // A key on the clip's first curve ("Curve 1" when it has none), valued by the percent.
                actions.push_back( { std::format( "Add Curve Key at {}%", percent ), [this, percent]()
                                     {
                                         auto* asset = ClipAsset();
                                         if ( asset == nullptr )
                                             return;
                                         const auto&       clip = asset->GetClip();
                                         const std::string name = clip.Curves.empty() ? std::string( "Curve 1" )
                                                                                      : clip.Curves.front().Name;
                                         (void)EditCurveKey( name, clip.DurationTicks.Value * percent / 100,
                                                             static_cast<float>( percent ) / 100.0f );
                                     } } );
            }
        }
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
        // Posing, the palette's (and DesertCtl's) way to the gizmo and the "+ Key" button.
        actions.push_back( { "Gizmo Translate", [this]() { m_GizmoRotate = false; } } );
        actions.push_back( { "Gizmo Rotate", [this]() { m_GizmoRotate = true; } } );
        actions.push_back( { "Rotate Bone Z +30", [this]()
                             {
                                 const auto* animator = BeginPosing();
                                 if ( animator == nullptr || !m_SelectedBone ||
                                      *m_SelectedBone >= animator->GetAuthoringPose().Size() )
                                 {
                                     LOG_ERROR( "Animation Editor: Rotate Bone needs a selected bone" );
                                     return;
                                 }
                                 Animation::BoneTransform pose = animator->GetAuthoringPose()[*m_SelectedBone];
                                 pose.Rotation                 = pose.Rotation *
                                                 glm::angleAxis( glm::radians( 30.0f ), glm::vec3( 0, 0, 1 ) );
                                 (void)PoseSelectedBone( pose, true );
                             } } );
        if ( Mode() == Core::PersonaMode::Animation )
            actions.push_back( { "Key Bone", [this]() { (void)KeySelectedBone(); } } );
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
        // An unkeyed pose belongs to the frame it was made on (UE drops it the same way): play or another frame
        // shows the clip again. Never mid-gesture - the transaction still holds its "before".
        if ( m_Posed && !m_PoseEdit.Open() && ( m_Transport.Playing || m_Transport.FrameIndex() != m_PosedFrame ) )
            EndPosing();
        m_Preview->SetAnimationTime( m_Transport.Time );
        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    Assets::AnimationAsset* AnimationEditorDocument::ClipAsset()
    {
        // Only Animation mode is about a clip; the Skeleton and Mesh modes' subject is not one.
        if ( m_Assets == nullptr || Mode() != Core::PersonaMode::Animation )
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
        m_ClipAsset = clip;
        m_ClipPath  = meta->Filepath;
        m_Tracked   = clip->IsReloadableFromFile();
        m_OnDisk.reset();
        if ( m_Tracked )
        {
            // READ FROM THE FILE, not copied from the asset: an edit made before this window opened (in an
            // earlier opening of it, say) is still unsaved, and the asset in memory already holds it.
            Assets::AnimationAsset fromFile( meta->Filepath );
            if ( const auto read = fromFile.LoadFromFile(); read )
                m_OnDisk = fromFile.GetClip();
            else
                LOG_ERROR( "Animation Editor: '{}' as the file holds it could not be read, so the clip counts as "
                           "unsaved: {}",
                           meta->Filepath.generic_string(), read.GetError() );
        }
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

    bool AnimationEditorDocument::AddNotify( std::string name, const double seconds, const int32_t track,
                                             const int32_t durationTicks )
    {
        if ( ClipAsset() == nullptr || name.empty() )
            return false;
        const auto& clip   = m_ClipAsset->GetClip();
        auto        edited = clip.Notifies;
        edited.push_back( { std::move( name ),
                            SnapNotifyTick( seconds, clip.TickRate, m_Transport.DisplayRate, clip.DurationTicks ),
                            std::max( track, 0 ), Animation::FrameNumber{ std::max( durationTicks, 0 ) } } );
        return EditNotifies( std::move( edited ), durationTicks > 0 ? "Add Notify State" : "Add Notify" );
    }

    bool AnimationEditorDocument::EditCurveKey( const std::string& name, const int32_t tick, const float value )
    {
        if ( ClipAsset() == nullptr )
            return false;
        return SetCurveKey( m_ClipAsset->GetClipForAuthoring(), name, Animation::FrameNumber{ tick }, value,
                            Animation::KeyInterp::Cubic, CommandHistory::Get(), {} );
    }

    ISubjectDocument::DiskState AnimationEditorDocument::GetDiskState() const
    {
        if ( !m_Tracked || !m_ClipAsset )
            return DiskState::Untracked;
        return !m_OnDisk || ClipDiffersFromFile( m_ClipAsset->GetClip(), *m_OnDisk ) ? DiskState::Dirty
                                                                                     : DiskState::Clean;
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
        m_OnDisk     = clip;
        m_SaveStatus = std::format( "Saved {}", m_ClipPath.filename().string() );
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
        if ( Mode() != Core::PersonaMode::Animation )
        {
            // The document name carries its ImGui "###" identity; a person reads only the part before it.
            const std::string_view name = GetName();
            line( 0, std::format( "Previewing {} {}", PersonaModeName( Mode() ),
                                  Mode() == Core::PersonaMode::Mesh ? std::string_view( m_MeshName )
                                                                    : name.substr( 0, name.find( "###" ) ) ) );
            line( 1, std::format( "Bind pose   mesh {}   skeleton {:016x}", m_MeshName, m_Signature ) );
            return;
        }
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
        if ( ImGui::Button( "+ Key" ) )
            (void)KeySelectedBone();
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
        ImGuiID           center  = dockId;
        const ImGuiID     left    = ImGui::DockBuilderSplitNode( center, ImGuiDir_Left, 0.22f, nullptr, &center );
        ImGuiID           right   = ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.30f, nullptr, &center );
        const ImGuiID     browser = ImGui::DockBuilderSplitNode( right, ImGuiDir_Down, 0.45f, nullptr, &right );
        const std::string assetDetails = PanelTitle( "Asset Details" );
        const std::string skeletonTree = PanelTitle( "Skeleton Tree" );
        const std::string details      = PanelTitle( "Details" );
        const std::string previewScene = PanelTitle( "Preview Scene Settings" );
        ImGui::DockBuilderDockWindow( assetDetails.c_str(), left );
        ImGui::DockBuilderDockWindow( skeletonTree.c_str(), left );
        ImGui::DockBuilderDockWindow( details.c_str(), right );
        ImGui::DockBuilderDockWindow( previewScene.c_str(), right );
        ImGui::DockBuilderDockWindow( PanelTitle( "Asset Browser" ).c_str(), browser );
        ImGui::DockBuilderDockWindow( PanelTitle( "Viewport" ).c_str(), center );
        // The tree and the bone's Details are the tabs in front, as in Persona.
        // Mesh mode is about the asset (vertices, material slots), so Asset Details leads there.
        ImGui::DockBuilderGetNode( left )->SelectedTabId =
             ImHashStr( Mode() == Core::PersonaMode::Mesh ? assetDetails.c_str() : skeletonTree.c_str() );
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

        // "AnimDock2": the Asset Browser node (ANV1d) — a layout saved before it would leave the browser floating.
        const ImGuiID        dockId = ImGui::GetID( "AnimDock2" );
        const ImGuiDockNode* node   = ImGui::DockBuilderGetNode( dockId );
        if ( node == nullptr || node->IsLeafNode() )
            BuildLayout( dockId );
        DrawModeToolbar();
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
        panel( "Asset Browser", 0, [this]() { DrawAssetBrowser(); } );
        const bool frontTabsNow = m_FrontTabsFrames == 1; // the countdown reaches 0 on this frame
        if ( m_FrontTabsFrames > 0 )
            --m_FrontTabsFrames;
        if ( frontTabsNow )
        {
            // Focusing a docked window selects its tab; Details last, so the bone's properties hold focus.
            ImGui::SetWindowFocus(
                 PanelTitle( Mode() == Core::PersonaMode::Mesh ? "Asset Details" : "Skeleton Tree" ).c_str() );
            ImGui::SetWindowFocus( PanelTitle( "Details" ).c_str() );
        }
    }

    void AnimationEditorDocument::DrawViewportPanel()
    {
        // The transport and the timeline are about a clip: the Skeleton and Mesh modes have none (UE's too).
        const bool  animation = Mode() == Core::PersonaMode::Animation;
        const float transportHeight =
             animation ? ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y : 0.0f;
        const ImVec2 avail          = ImGui::GetContentRegionAvail();
        const float  timelineHeight = animation ? TimelineHeight() : 0.0f;
        const ImVec2 view(
             std::max( avail.x, 1.0f ),
             std::max( avail.y - transportHeight - timelineHeight - ImGui::GetStyle().ItemSpacing.y, 1.0f ) );
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        if ( ImGui::BeginChild( "##animview", view, false,
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse ) )
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            Control::PointerInjection::PublishTarget( Control::Subject::Document,
                                                      { origin.x, origin.y, view.x, view.y,
                                                        ImGui::GetWindowViewport()->ID, ImGui::GetFrameCount() } );
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
                (void)m_Preview->Draw( *m_UIHelper, view,
                                       m_GizmoHovered ? PreviewInteraction::Static
                                                      : PreviewInteraction::Interactive );
            DrawBones( glm::vec2( origin.x, origin.y ), glm::vec2( view.x, view.y ) );
            // Posing keys into the clip, so the gizmo is Animation mode's.
            if ( animation )
                DrawBoneGizmo( glm::vec2( origin.x, origin.y ), glm::vec2( view.x, view.y ) );
            DrawOverlay( glm::vec2( origin.x, origin.y ) );
        }
        ImGui::EndChild();
        if ( !animation )
            return;
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

    void AnimationEditorDocument::DrawBoneDetails()
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

        // One row set per transform; `editable` rows commit on Enter, the reference pose is read-only.
        const auto section = [index]( const char* label, BoneTransformRows& rows, const bool editable )
        {
            SectionHeader( label );
            bool       changed = false;
            const auto row     = [&]( const char* name, glm::vec3& value )
            {
                ImGui::PushID( name );
                ImGui::PushID( static_cast<int>( index ) );
                ImGui::SetNextItemWidth( -80.0f );
                changed |= ImGui::InputFloat3( name, &value.x, "%.3f",
                                               editable ? ImGuiInputTextFlags_EnterReturnsTrue
                                                        : ImGuiInputTextFlags_ReadOnly );
                ImGui::PopID();
                ImGui::PopID();
            };
            row( "Location", rows.Location );
            row( "Rotation", rows.RotationDegrees );
            row( "Scale", rows.Scale );
            return changed && editable;
        };
        // The pose AT THE PLAYHEAD: the clip's evaluated pose, or the pose being made on this frame. (The
        // authoring buffer alone is the bind pose until something poses it - it is not the frame.)
        const Animation::BoneTransform& shown =
             m_Posed ? animator->GetAuthoringPose()[index] : animator->GetLocalPose()[index];
        BoneTransformRows posed{ shown.Translation, glm::degrees( glm::eulerAngles( shown.Rotation ) ),
                                 shown.Scale };
        if ( section( std::format( "Bone (frame {})", m_Transport.FrameIndex() ).c_str(), posed, true ) )
            (void)PoseSelectedBone( Animation::BoneTransform{ posed.Location,
                                                              glm::quat( glm::radians( posed.RotationDegrees ) ),
                                                              posed.Scale },
                                    true );
        BoneTransformRows reference = DecomposeBoneTransform( bone.LocalBindTransform );
        (void)section( "Reference Pose", reference, false );
    }

    Animation::FrameNumber AnimationEditorDocument::KeyTick() const
    {
        const double rate = m_ClipAsset ? m_ClipAsset->GetClip().TickRate.AsDouble() : 0.0;
        return Animation::FrameNumber{ static_cast<int32_t>( std::llround( m_Transport.Time * rate ) ) };
    }

    Animation::Animator* AnimationEditorDocument::BeginPosing()
    {
        auto*       animator = m_Preview ? m_Preview->GetAnimatorForAuthoring() : nullptr;
        const auto* asset    = ClipAsset();
        if ( animator == nullptr || asset == nullptr )
            return nullptr;
        if ( !m_Posed )
        {
            // Posing starts from the clip's pose at this frame, not from whatever the buffer last held.
            animator->SampleClipIntoLocalPose( asset->GetClip(), Animation::FrameTime{ KeyTick(), 0.0f } );
            animator->ApplyLocalPose();
            m_Transport.Playing = false;
            m_Posed             = true;
            m_PosedFrame        = m_Transport.FrameIndex();
            m_Preview->SetPoseOverride( true );
        }
        return animator;
    }

    void AnimationEditorDocument::EndPosing()
    {
        if ( m_PoseEdit.Open() )
            m_PoseEdit.Cancel();
        m_GizmoHeld = false;
        m_Posed     = false;
        if ( m_Preview )
            m_Preview->SetPoseOverride( false );
    }

    bool AnimationEditorDocument::PoseSelectedBone( const Animation::BoneTransform& pose, const bool undoable )
    {
        auto* animator = BeginPosing();
        auto* asset    = ClipAsset();
        if ( animator == nullptr || asset == nullptr || !m_SelectedBone ||
             *m_SelectedBone >= animator->GetAuthoringPose().Size() )
            return false;
        if ( undoable )
        {
            if ( const auto begun = m_PoseEdit.Begin( animator, &asset->GetClipForAuthoring() );
                 !begun.IsSuccess() )
            {
                LOG_ERROR( "Animation Editor: pose edit refused: {}", begun.GetError() );
                return false;
            }
        }
        Animation::LocalPose edited = animator->GetAuthoringPose();
        edited[*m_SelectedBone]     = pose;
        if ( const auto set = animator->SetAuthoringPose( edited ); !set.IsSuccess() )
        {
            LOG_ERROR( "Animation Editor: pose refused: {}", set.GetError() );
            if ( undoable )
                m_PoseEdit.Cancel();
            return false;
        }
        animator->ApplyLocalPose();
        if ( undoable )
        {
            if ( const auto ended = m_PoseEdit.End(); !ended.IsSuccess() )
            {
                LOG_ERROR( "Animation Editor: pose edit not recorded: {}", ended.GetError() );
                return false;
            }
        }
        return true;
    }

    bool AnimationEditorDocument::KeySelectedBone()
    {
        auto* animator = BeginPosing();
        auto* asset    = ClipAsset();
        if ( animator == nullptr || asset == nullptr || !m_SelectedBone )
        {
            LOG_ERROR( "Animation Editor: + Key needs a selected bone in a loaded preview" );
            return false;
        }
        const auto keyed =
             KeyBonePose( m_PoseEdit, animator, &asset->GetClipForAuthoring(), *m_SelectedBone, KeyTick() );
        if ( !keyed.IsSuccess() )
        {
            LOG_ERROR( "Animation Editor: + Key refused: {}", keyed.GetError() );
            return false;
        }
        return true;
    }

    void AnimationEditorDocument::DrawBoneGizmo( const glm::vec2& origin, const glm::vec2& size )
    {
        m_GizmoHovered = false;
        auto* animator = m_Preview ? m_Preview->GetAnimatorForAuthoring() : nullptr;
        if ( animator == nullptr || ClipAsset() == nullptr || !m_SelectedBone || m_Transport.Playing ||
             *m_SelectedBone >= animator->GetSkeleton().GetBones().size() )
        {
            if ( m_GizmoHeld )
                EndPosing();
            return;
        }
        // W / E as in the level viewport, while the pointer is over this window.
        if ( ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput )
        {
            if ( ImGui::IsKeyPressed( ImGuiKey_W, false ) )
                m_GizmoRotate = false;
            if ( ImGui::IsKeyPressed( ImGuiKey_E, false ) )
                m_GizmoRotate = true;
        }
        const uint32_t  bone   = *m_SelectedBone;
        const glm::mat4 target = m_Preview->GetTargetTransform();
        const glm::mat4 view   = m_Preview->GetView();
        const glm::mat4 proj   = m_Preview->GetProjection();
        glm::mat4       world  = target * animator->GetBoneModelMatrix( bone );

        const Core::GizmoIdScope gizmoId( "AnimationEditorBone" );
        ImGuizmo::SetOrthographic( false );
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect( origin.x, origin.y, size.x, size.y );
        const bool moved = ImGuizmo::Manipulate( &view[0][0], &proj[0][0],
                                                 m_GizmoRotate ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE,
                                                 ImGuizmo::LOCAL, &world[0][0] );
        const bool held  = ImGuizmo::IsUsing();
        m_GizmoHovered   = held || ImGuizmo::IsOver();

        // One undo step per drag: the transaction opens on the press, over the clip's pose at this frame.
        if ( held && !m_GizmoHeld )
        {
            Animation::Animator* posed = BeginPosing();
            if ( const auto begun = m_PoseEdit.Begin( posed, &ClipAsset()->GetClipForAuthoring() );
                 !begun.IsSuccess() )
            {
                LOG_ERROR( "Animation Editor: bone drag refused: {}", begun.GetError() );
                return;
            }
            m_GizmoHeld = true;
        }
        if ( moved && m_GizmoHeld )
        {
            // World -> the bone's parent-relative transform (GizmoController's bone branch does the same).
            glm::mat4      parentModel( 1.0f );
            const uint32_t parent = animator->GetSkeleton().ResolveParent( bone );
            if ( parent < animator->GetSkeleton().GetBones().size() )
                parentModel = animator->GetBoneModelMatrix( parent );
            const auto local = Animation::BoneTransform::FromMatrix( glm::inverse( parentModel ) *
                                                                     glm::inverse( target ) * world );
            if ( local.IsSuccess() )
                (void)PoseSelectedBone( local.GetValue(), false );
            else
            {
                LOG_ERROR( "Animation Editor: bone drag: {}", local.GetError() );
            }
        }
        if ( !held && m_GizmoHeld )
        {
            m_GizmoHeld = false;
            if ( const auto ended = m_PoseEdit.End(); !ended.IsSuccess() )
            {
                LOG_ERROR( "Animation Editor: bone drag not recorded: {}", ended.GetError() );
            }
        }
    }

    void AnimationEditorDocument::DrawAssetDetails()
    {
        if ( Mode() == Core::PersonaMode::Mesh )
        {
            DrawMeshDetails();
            return;
        }
        if ( Mode() == Core::PersonaMode::Skeleton )
        {
            DrawSkeletonDetails();
            return;
        }
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
                if ( ImGui::Selectable( m_MeshCandidates[i].generic_string().c_str(), i == m_MeshIndex ) &&
                     i != m_MeshIndex )
                    SetPreviewMesh( i );
            ImGui::EndCombo();
        }
        SectionHeader( "Environment" );
        PreviewEnvironment::DrawEnvironmentRows();
        PreviewEnvironment::DrawShowFloor( "Show Floor" );
    }

    void AnimationEditorDocument::DrawAssetBrowser()
    {
        // UE's Asset Browser in Persona: the clips that play on the previewed rig, a search, and a double click to
        // open one. Listed from the CONTENT REGISTRY by each clip's Rig tag (the scan reads a clip's stated
        // SkeletonSignature, ContentScan.cpp) - nothing is loaded to list them, as UE's browser reads the
        // registry's tags; AnimationLibrary holds only the clips something already loaded.
        const uint64_t signature = m_Signature;
        if ( signature == 0 )
        {
            ImGui::TextDisabled( "No skeleton is known yet: nothing to list clips for." );
            return;
        }
        if ( !m_BrowserListed )
        {
            m_BrowserClips.clear();
            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Animation ) )
                if ( row.RigSignature == signature )
                    m_BrowserClips.emplace_back( row.Path.filename().string(), row.Handle );
            std::ranges::sort( m_BrowserClips );
            m_BrowserListed = true;
        }
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputTextWithHint( "##browsersearch", "Search Assets", m_BrowserFilter.data(),
                                  m_BrowserFilter.size() );
        std::string filter( m_BrowserFilter.data() );
        std::ranges::transform( filter, filter.begin(),
                                []( const unsigned char c ) { return std::tolower( c ); } );
        const Assets::AssetHandle current( Subject().Owner );
        size_t                    shown = 0;
        for ( const auto& [name, handle] : m_BrowserClips )
        {
            std::string lower = name;
            std::ranges::transform( lower, lower.begin(),
                                    []( const unsigned char c ) { return std::tolower( c ); } );
            if ( !filter.empty() && lower.find( filter ) == std::string::npos )
                continue;
            ++shown;
            ImGui::Selectable( name.c_str(), handle == current, ImGuiSelectableFlags_AllowDoubleClick );
            if ( handle != current && m_Editors != nullptr && ImGui::IsItemHovered() &&
                 ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                if ( const auto opened =
                          Core::RequestOpenAsset( m_Assets->FindMetadataByHandle( handle ), handle, *m_Editors );
                     !opened.IsSuccess() )
                    LOG_ERROR( "Animation Editor: '{}' did not open: {}", name, opened.GetError() );
        }
        ImGui::TextDisabled( "%zu of %zu clips on this skeleton", shown, m_BrowserClips.size() );
    }

    float AnimationEditorDocument::TimelineHeight() const
    {
        // Ruler, "Notifies" header, the tracks, "Curves (0)" header.
        const int32_t rows   = m_ClipAsset ? NotifyTrackCount( m_ClipAsset->GetClip().Notifies, m_AddedNotifyRows )
                                           : std::max( m_AddedNotifyRows, 1 );
        const size_t  curves = m_ClipAsset ? m_ClipAsset->GetClip().Curves.size() : 0;
        return ImGui::GetFrameHeight() * static_cast<float>( 3 + rows + static_cast<int32_t>( curves ) ) +
               ImGui::GetStyle().WindowPadding.y * 2.0f;
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
        const int32_t rows         = NotifyTrackCount( clip.Notifies, m_AddedNotifyRows );
        const float   rulerTop     = at.y;
        const float   rulerBottom  = rulerTop + rowH;
        const float   rowsTop      = rulerBottom + rowH; // under the "Notifies" header
        const float   curvesTop    = rowsTop + rowH * static_cast<float>( rows );
        const float   curveRowsTop = curvesTop + rowH; // under the "Curves (N)" header
        const float   panelBottom  = curveRowsTop + rowH * static_cast<float>( clip.Curves.size() );
        const ImVec2  mouse        = ImGui::GetIO().MousePos;
        const auto    rowOfY       = [&]( const float y )
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
        text( at.x + 6.0f, curvesTop, std::format( "Curves ({})", clip.Curves.size() ).c_str(),
              IM_COL32( 230, 230, 230, 255 ) );

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
            // A Notify State (UE: AN_FootPlant's bar) is a span: its body moves it, its two edges resize it.
            const bool  state = notify.IsState();
            const float endX =
                 state ? tickX( Animation::FrameNumber{ notify.Tick.Value + notify.DurationTicks.Value } )
                       : centre.x;
            const float barTop = centre.y - rowH * 0.5f + 2.0f;
            const float barBot = centre.y + rowH * 0.5f - 2.0f;
            ImGui::PushID( static_cast<int>( i ) );
            if ( state )
            {
                ImGui::SetCursorScreenPos( ImVec2( centre.x + kEdgeGrip, barTop ) );
                ImGui::InvisibleButton(
                     "##notify", ImVec2( std::max( endX - centre.x - kEdgeGrip * 2.0f, 2.0f ), barBot - barTop ) );
            }
            else
            {
                ImGui::SetCursorScreenPos( ImVec2( centre.x - kDiamondRadius, centre.y - kDiamondRadius ) );
                ImGui::InvisibleButton( "##notify", ImVec2( kDiamondRadius * 2.0f, kDiamondRadius * 2.0f ) );
            }
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

            // The edges of a state: a drag of one is ONE edit on release (SetNotifyDuration for the end).
            bool resized = false;
            if ( state )
                for ( const NotifyStateEdge edge : { NotifyStateEdge::Begin, NotifyStateEdge::End } )
                {
                    const float x = edge == NotifyStateEdge::Begin ? centre.x : endX;
                    ImGui::SetCursorScreenPos( ImVec2( x - kEdgeGrip, barTop ) );
                    ImGui::PushID( static_cast<int>( i * 2 + ( edge == NotifyStateEdge::End ? 1 : 0 ) ) );
                    ImGui::InvisibleButton( "##stateedge", ImVec2( kEdgeGrip * 2.0f, barBot - barTop ) );
                    ImGui::PopID();
                    overNotify |= ImGui::IsItemHovered();
                    if ( ImGui::IsItemHovered() || ImGui::IsItemActive() )
                        ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeEW );
                    if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 2.0f ) )
                    {
                        m_DragState = static_cast<int32_t>( i );
                        m_DragEdge  = edge;
                    }
                    if ( ImGui::IsItemDeactivated() && m_DragState == static_cast<int32_t>( i ) &&
                         m_DragEdge == edge )
                    {
                        const auto landed =
                             DragNotifyStateEdge( notify, edge, snapped( mouse.x ), clip.DurationTicks );
                        m_DragState = -1;
                        if ( edge == NotifyStateEdge::End )
                            (void)SetNotifyDuration( clip, i, landed.DurationTicks, CommandHistory::Get(), {} );
                        else
                        {
                            auto edited = clip.Notifies;
                            edited[i]   = landed;
                            (void)EditNotifies( std::move( edited ), "Resize Notify State" );
                        }
                        resized = true;
                        break;
                    }
                }
            if ( resized )
                break; // the list was replaced; draw it next frame

            const float flash = m_Flash[i] / kNotifyFlashSeconds;
            const ImU32 fill  = dragging ? IM_COL32( 120, 120, 120, 255 )
                                         : ImGui::GetColorU32( ImVec4( 0.85f + 0.15f * flash, 0.70f + 0.30f * flash,
                                                                       0.30f + 0.55f * flash, 1.0f ) );
            if ( state )
            {
                const ImU32 bar = ImGui::GetColorU32(
                     ImVec4( 0.25f + 0.3f * flash, 0.45f + 0.3f * flash, 0.62f + 0.2f * flash, 1.0f ) );
                dl->AddRectFilled( ImVec2( centre.x, barTop ), ImVec2( endX, barBot ), bar, 3.0f );
                dl->AddRect( ImVec2( centre.x, barTop ), ImVec2( endX, barBot ), IM_COL32( 150, 200, 240, 255 ),
                             3.0f );
                dl->PushClipRect( ImVec2( centre.x, barTop ), ImVec2( endX, barBot ), true );
                text( centre.x + 5.0f, centre.y - rowH * 0.5f, notify.Name.c_str(),
                      IM_COL32( 240, 240, 240, 255 ) );
                dl->PopClipRect();
                if ( m_DragState == static_cast<int32_t>( i ) ) // the ghost: the span on release
                {
                    const auto landed =
                         DragNotifyStateEdge( notify, m_DragEdge, snapped( mouse.x ), clip.DurationTicks );
                    dl->AddRect(
                         ImVec2( tickX( landed.Tick ), barTop ),
                         ImVec2( tickX( Animation::FrameNumber{ landed.Tick.Value + landed.DurationTicks.Value } ),
                                 barBot ),
                         IM_COL32( 255, 200, 90, 255 ), 3.0f, 0, 2.0f );
                }
            }
            else
            {
                DrawDiamond( dl, centre, kDiamondRadius, fill );
                text( centre.x + kDiamondRadius + 3.0f, centre.y - rowH * 0.5f, notify.Name.c_str(),
                      flash > 0.0f ? IM_COL32( 255, 255, 220, 255 ) : IM_COL32( 220, 220, 220, 255 ) );
            }
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

        // Curves: one row per anim curve, its shape drawn across the row and its keys as diamonds. A key drags
        // along time (one edit on release); right-click a key to set its value or delete it, an empty lane to
        // key it there, the header to add a curve.
        bool overKey = false;
        for ( size_t c = 0; c < clip.Curves.size(); ++c )
        {
            const auto& curve  = clip.Curves[c];
            const float rowTop = curveRowsTop + rowH * static_cast<float>( c );
            dl->AddRectFilled( ImVec2( at.x, rowTop ), ImVec2( laneX1, rowTop + rowH ),
                               c % 2 == 0 ? IM_COL32( 30, 30, 30, 255 ) : IM_COL32( 34, 34, 34, 255 ) );
            text( at.x + 18.0f, rowTop, curve.Name.c_str(), IM_COL32( 190, 190, 190, 255 ) );
            if ( curve.Keys.empty() )
                continue;
            const auto [lowIt, highIt] =
                 std::ranges::minmax_element( curve.Keys, {}, &Animation::ScalarKey::Value );
            float low  = lowIt->Value;
            float high = highIt->Value;
            if ( high - low < 1e-6f )
            {
                low -= 0.5f;
                high += 0.5f;
            }
            const auto valueY = [&]( const float v )
            { return rowTop + rowH - 3.0f - ( v - low ) / ( high - low ) * ( rowH - 6.0f ); };
            constexpr int                    kSamples = 96;
            std::array<ImVec2, kSamples + 1> line{};
            for ( int k = 0; k <= kSamples; ++k )
            {
                const double seconds = m_Transport.DurationSeconds * k / kSamples;
                const auto   at2     = Animation::SecondsToFrameTime( seconds, clip.TickRate );
                line[static_cast<size_t>( k )] =
                     ImVec2( axis.TimeToX( seconds ), valueY( curve.Evaluate( at2, clip.TickRate ) ) );
            }
            dl->AddPolyline( line.data(), static_cast<int>( line.size() ), IM_COL32( 230, 120, 90, 255 ),
                             ImDrawFlags_None, 1.5f );
            for ( size_t k = 0; k < curve.Keys.size(); ++k )
            {
                const auto&  key = curve.Keys[k];
                const ImVec2 centre( tickX( key.Tick ), valueY( key.Value ) );
                ImGui::SetCursorScreenPos( ImVec2( centre.x - kDiamondRadius, centre.y - kDiamondRadius ) );
                ImGui::PushID( static_cast<int>( 100000 + c * 1000 + k ) );
                ImGui::InvisibleButton( "##curvekey", ImVec2( kDiamondRadius * 2.0f, kDiamondRadius * 2.0f ) );
                ImGui::PopID();
                overKey |= ImGui::IsItemHovered();
                const bool dragging =
                     m_DragCurve == static_cast<int32_t>( c ) && m_DragKey == static_cast<int32_t>( k );
                if ( ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 2.0f ) )
                {
                    m_DragCurve = static_cast<int32_t>( c );
                    m_DragKey   = static_cast<int32_t>( k );
                }
                if ( ImGui::IsItemDeactivated() && dragging )
                {
                    m_DragCurve  = -1;
                    m_DragKey    = -1;
                    auto  edited = clip.Curves;
                    auto& keys   = edited[c].Keys;
                    auto  moved  = keys[k];
                    keys.erase( keys.begin() + static_cast<std::ptrdiff_t>( k ) );
                    moved.Tick = snapped( mouse.x );
                    std::erase_if( keys,
                                   [&]( const Animation::ScalarKey& other ) { return other.Tick == moved.Tick; } );
                    keys.push_back( moved );
                    (void)ApplyCurveEdit( clip, std::move( edited ), "Move Curve Key", CommandHistory::Get(), {} );
                    break;
                }
                if ( ImGui::IsItemClicked( ImGuiMouseButton_Right ) )
                {
                    m_PopupCurve    = curve.Name;
                    m_PopupKeyTick  = key.Tick.Value;
                    m_PopupKeyValue = key.Value;
                    ImGui::OpenPopup( kCurveKeyPopup );
                }
                DrawDiamond( dl, centre, kDiamondRadius - 1.0f,
                             dragging ? IM_COL32( 120, 120, 120, 255 ) : IM_COL32( 240, 170, 120, 255 ) );
                if ( dragging )
                    DrawDiamond( dl, ImVec2( tickX( snapped( mouse.x ) ), centre.y ), kDiamondRadius - 1.0f,
                                 IM_COL32( 255, 200, 90, 255 ) );
            }
        }
        if ( !overKey && ImGui::IsWindowHovered() && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) &&
             mouse.x >= laneX0 && mouse.x < laneX1 )
        {
            if ( mouse.y >= curvesTop && mouse.y < curveRowsTop )
            {
                CopyName( m_NameBuffer, std::format( "Curve {}", clip.Curves.size() + 1 ) );
                ImGui::OpenPopup( kAddCurvePopup );
            }
            else if ( mouse.y >= curveRowsTop && mouse.y < panelBottom )
            {
                const auto  c     = static_cast<size_t>( ( mouse.y - curveRowsTop ) / rowH );
                const auto& curve = clip.Curves[std::min( c, clip.Curves.size() - 1 )];
                m_PopupCurve      = curve.Name;
                m_PopupKeyTick    = snapped( mouse.x ).Value;
                m_PopupKeyValue =
                     curve.Keys.empty()
                          ? 0.0f
                          : curve.Evaluate( Animation::FrameTime{ Animation::FrameNumber{ m_PopupKeyTick }, 0.0F },
                                            clip.TickRate );
                ImGui::OpenPopup( kAddCurveKeyPopup );
            }
        }
        DrawCurvePopups( clip );

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
            // UE's "Add Notify State": the same marker with a length — a quarter of the clip to start with.
            if ( ImGui::MenuItem( "Add Notify State" ) && ClipAsset() != nullptr )
            {
                (void)AddNotify( m_NameBuffer.data(), m_PopupSeconds, m_PopupTrack,
                                 std::max( m_ClipAsset->GetClip().DurationTicks.Value / 4, 1 ) );
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

    void AnimationEditorDocument::DrawCurvePopups( Animation::AnimationClip& clip )
    {
        if ( ImGui::BeginPopup( kAddCurvePopup ) )
        {
            ImGui::SetNextItemWidth( 180.0f );
            const bool enter = ImGui::InputText( "##curvename", m_NameBuffer.data(), m_NameBuffer.size(),
                                                 ImGuiInputTextFlags_EnterReturnsTrue );
            // A curve is its keys: it is created with one, at the playhead, valued 0.
            if ( ( ImGui::MenuItem( "Add Curve" ) || enter ) && m_NameBuffer[0] != '\0' )
            {
                (void)EditCurveKey(
                     m_NameBuffer.data(),
                     SnapNotifyTick( m_Transport.Time, clip.TickRate, m_Transport.DisplayRate, clip.DurationTicks )
                          .Value,
                     0.0f );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if ( ImGui::BeginPopup( kAddCurveKeyPopup ) )
        {
            ImGui::TextDisabled( "%s, tick %d", m_PopupCurve.c_str(), m_PopupKeyTick );
            ImGui::SetNextItemWidth( 120.0f );
            ImGui::DragFloat( "##newkeyvalue", &m_PopupKeyValue, 0.01f );
            if ( ImGui::MenuItem( "Add Key" ) )
            {
                (void)EditCurveKey( m_PopupCurve, m_PopupKeyTick, m_PopupKeyValue );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if ( ImGui::BeginPopup( kCurveKeyPopup ) )
        {
            ImGui::TextDisabled( "%s, tick %d", m_PopupCurve.c_str(), m_PopupKeyTick );
            ImGui::SetNextItemWidth( 120.0f );
            ImGui::DragFloat( "##keyvalue", &m_PopupKeyValue, 0.01f );
            if ( ImGui::MenuItem( "Set Value" ) )
            {
                (void)EditCurveKey( m_PopupCurve, m_PopupKeyTick, m_PopupKeyValue );
                ImGui::CloseCurrentPopup();
            }
            if ( ImGui::MenuItem( "Delete Key" ) )
            {
                (void)RemoveCurveKey( clip, m_PopupCurve, Animation::FrameNumber{ m_PopupKeyTick },
                                      CommandHistory::Get(), {} );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    Assets::AssetHandle AnimationEditorDocument::ModeAsset( const Core::PersonaMode mode ) const
    {
        if ( mode == Mode() )
            return { Subject().Owner };
        switch ( mode )
        {
            case Core::PersonaMode::Skeleton:
                // The previewed mesh's own skeleton dependency: the rig every mode here is on.
                return m_Mesh ? Assets::AssetHandle( m_Mesh->GetSkeletonDependency().Handle )
                              : Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            case Core::PersonaMode::Mesh:
                return m_Mesh ? m_Mesh->GetMetadata().Handle : Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            case Core::PersonaMode::Animation:
                // UE opens the last animation; this window has no history across windows, so it is the first
                // clip the Asset Browser lists for the rig (sorted by name, a stable pick).
                return m_BrowserClips.empty() ? Assets::AssetHandle( static_cast<uint64_t>( 0 ) )
                                              : m_BrowserClips.front().second;
        }
        return { static_cast<uint64_t>( 0 ) };
    }

    void AnimationEditorDocument::OpenMode( const Core::PersonaMode mode )
    {
        if ( mode == Mode() || m_Editors == nullptr || m_Assets == nullptr )
            return;
        if ( mode == Core::PersonaMode::Animation && !m_BrowserListed )
        {
            m_BrowserClips.clear();
            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Animation ) )
                if ( row.RigSignature == m_Signature && m_Signature != 0 )
                    m_BrowserClips.emplace_back( row.Path.filename().string(), row.Handle );
            std::ranges::sort( m_BrowserClips );
            m_BrowserListed = true;
        }
        const Assets::AssetHandle target = ModeAsset( mode );
        if ( static_cast<uint64_t>( target ) == 0 )
        {
            LOG_ERROR( "Animation Editor: no {} asset on skeleton {:016x} to switch to", PersonaModeName( mode ),
                       m_Signature );
            return;
        }
        if ( const auto opened =
                  Core::RequestOpenAsset( m_Assets->FindMetadataByHandle( target ), target, *m_Editors );
             !opened.IsSuccess() )
            LOG_ERROR( "Animation Editor: {} mode did not open: {}", PersonaModeName( mode ), opened.GetError() );
    }

    void AnimationEditorDocument::DrawModeToolbar()
    {
        // UE puts the mode switcher at the right of the asset editor's toolbar.
        constexpr std::array kModes = { Core::PersonaMode::Skeleton, Core::PersonaMode::Mesh,
                                        Core::PersonaMode::Animation };
        float                width  = 0.0f;
        for ( const auto mode : kModes )
            width += ImGui::CalcTextSize( PersonaModeName( mode ) ).x + ImGui::GetStyle().FramePadding.x * 2.0f +
                     ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX( std::max( ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - width ) );
        for ( const auto mode : kModes )
        {
            const bool active = mode == Mode();
            // The theme's accent (the checkmark colour): ButtonActive is darker than Button in this theme, so the
            // current mode read as the one NOT selected.
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ImGui::GetStyleColorVec4( ImGuiCol_CheckMark ) );
            if ( ImGui::Button( PersonaModeName( mode ) ) )
                OpenMode( mode );
            if ( active )
                ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    void AnimationEditorDocument::DrawMeshDetails()
    {
        if ( !m_Mesh )
        {
            ImGui::TextDisabled( "The mesh is not loaded." );
            return;
        }
        // Only what a SkinnedMeshAsset holds (UE's LOD settings and physics asset have no field here).
        const auto& mesh = *m_Mesh;
        const auto  row  = []( const char* label, const std::string& value )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled( "%s", label );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( value.c_str() );
        };
        SectionHeader( "Mesh" );
        if ( ImGui::BeginTable( "##meshdetails", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
        {
            row( "File", mesh.GetMetadata().Filepath.filename().string() );
            row( "Skeleton", std::format( "{:016x}", mesh.GetSkeletonSignature() ) );
            row( "Vertices", std::format( "{}", mesh.GetVertices().size() ) );
            row( "Triangles", std::format( "{}", mesh.GetIndices().size() / 3 ) );
            row( "Sections", std::format( "{}", mesh.GetSubmeshes().size() ) );
            row( "Morph Targets", std::format( "{}", mesh.GetMorphTargets().size() ) );
            // Each section carries its own LOD ranges; the mesh has as many levels as its richest section.
            size_t lods = 1;
            for ( const auto& section : mesh.GetSubmeshes() )
                lods = std::max( lods, section.LODs.size() );
            row( "LODs", std::format( "{}", lods ) );
            ImGui::EndTable();
        }
        SectionHeader( "Material Slots" );
        if ( ImGui::BeginTable( "##meshslots", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
        {
            const auto& slots = mesh.GetMaterialHandles();
            for ( size_t i = 0; i < slots.size(); ++i )
            {
                // Handle 0 is an empty slot, not a lost asset.
                const auto  handle = static_cast<uint64_t>( slots[i] );
                const auto* meta   = m_Assets != nullptr ? m_Assets->FindMetadataByHandle( slots[i] ) : nullptr;
                std::string name   = std::format( "{:016x} (not registered)", handle );
                if ( meta != nullptr )
                    name = meta->Filepath.filename().string();
                else if ( handle == 0 )
                    name = "None";
                row( std::format( "Slot {}", i ).c_str(), name );
            }
            ImGui::EndTable();
        }
    }

    void AnimationEditorDocument::DrawSkeletonDetails()
    {
        const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr;
        if ( !ImGui::BeginTable( "##skeldetails", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
            return;
        const auto row = []( const char* label, const std::string& value )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled( "%s", label );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( value.c_str() );
        };
        row( "Skeleton", GetName() );
        row( "Signature", std::format( "{:016x}", m_Signature ) );
        row( "Bones", animator != nullptr ? std::format( "{}", animator->GetSkeleton().GetBones().size() )
                                          : std::string( "(the preview has not built the rig yet)" ) );
        row( "Preview Mesh", m_MeshName );
        row( "Meshes on it", std::format( "{}", m_MeshCandidates.size() ) );
        ImGui::EndTable();
    }

    namespace
    {
        // Find-or-create the asset at @p path as T, load it, and open it through the one handle route.
        template <typename T>
        SubjectEditorRegistry::PathOpenOutcome
        OpenPersonaAsset( Assets::AssetManager& assets, const std::string& path,
                          const SubjectEditorRegistry& editors, const char* what )
        {
            using Outcome = SubjectEditorRegistry::PathOpenOutcome;
            auto asset    = assets.FindByPath<T>( path );
            if ( !asset )
                asset = assets.CreateAsset<T>( path );
            if ( !asset )
            {
                LOG_ERROR( "[Assets] '{}' could not be registered as {} — no editor was opened.", path, what );
                return Outcome::Failed;
            }
            if ( const auto loaded = asset->EnsureLoaded( assets ); !loaded )
            {
                LOG_ERROR( "[Assets] '{}' would not load as {} — no editor was opened: {}", path, what,
                           loaded.GetError() );
                return Outcome::Failed;
            }
            const auto handle = asset->GetMetadata().Handle;
            if ( const auto opened =
                      Core::RequestOpenAsset( assets.FindMetadataByHandle( handle ), handle, editors );
                 !opened.IsSuccess() )
            {
                LOG_ERROR( "[Assets] '{}': {}", path, opened.GetError() );
                return Outcome::Failed;
            }
            return Outcome::Requested;
        }
    } // namespace

    SubjectEditorRegistry::PathOpenOutcome RequestAnimationEditorDocument( Assets::AssetManager*        assets,
                                                                           const std::string&           path,
                                                                           const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        using Common::Content::ContentKind;
        std::error_code ec;
        if ( assets == nullptr || !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;
        const std::string extension = std::filesystem::path( path ).extension().string();
        if ( extension == kAnimationClipExtension )
            return OpenPersonaAsset<Assets::AnimationAsset>( *assets, path, editors, "an animation clip" );
        if ( extension == Common::Content::KindSpec( ContentKind::SkinnedMesh ).Extension )
            return OpenPersonaAsset<Assets::SkinnedMeshAsset>( *assets, path, editors, "a skeletal mesh" );
        if ( extension == Common::Content::KindSpec( ContentKind::Skeleton ).Extension )
            return OpenPersonaAsset<Assets::SkeletonAsset>( *assets, path, editors, "a skeleton" );
        return Outcome::NotMine;
    }
} // namespace Desert::Editor
