#include "RetargetDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Retarget/RetargetSource.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/RetargetAsset.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        constexpr float kSidePanelWidth = 360.0f;

        std::optional<Common::Content::AssetGuid> GuidOf( const Assets::AssetGuidRef& ref )
        {
            auto guid = Common::Content::AssetGuidFromText( ref.Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return std::nullopt;
            return guid.GetValue();
        }

        // One rig and its preview mesh, from the content registry's tags (nothing scanned is loaded to choose):
        // the `.skeleton` whose GUID the file states, and the first skinned mesh on it by path — the mesh the
        // Animation Editor opens that rig on.
        void LoadSide( Assets::AssetManager& assets, const Assets::AssetGuidRef& ref, const char* which,
                       std::shared_ptr<Assets::SkeletonAsset>&    skeleton,
                       std::shared_ptr<Assets::SkinnedMeshAsset>& mesh, std::string& problem )
        {
            const auto guid = GuidOf( ref );
            if ( !guid )
            {
                problem = std::format( "the {} skeleton GUID '{}' is not well-formed", which, ref.Guid );
                return;
            }
            const auto rig = Assets::ContentRegistry::RigRow( *guid );
            if ( !rig )
            {
                problem = std::format( "the {} skeleton '{}' is not in the content registry", which, ref.Path );
                return;
            }
            skeleton = assets.CreateAsset<Assets::SkeletonAsset>( rig->Path, false );
            if ( !skeleton )
            {
                problem = std::format( "the {} skeleton '{}' could not be registered", which,
                                       rig->Path.generic_string() );
                return;
            }
            if ( const auto loaded = skeleton->EnsureLoaded( assets );
                 !loaded || skeleton->GetSkeleton() == nullptr )
            {
                problem =
                     std::format( "the {} skeleton '{}' would not load: {}", which, rig->Path.generic_string(),
                                  loaded ? std::string( "no bones" ) : loaded.GetError() );
                skeleton.reset();
                return;
            }

            const auto row = Assets::ContentRegistry::PreviewMeshRow( *guid );
            if ( !row )
            {
                problem = std::format( "no skeletal mesh references the {} skeleton '{}' — nothing to show it on",
                                       which, ref.Path );
                return;
            }
            mesh = assets.CreateAsset<Assets::SkinnedMeshAsset>( row->Path, false );
            if ( !mesh )
            {
                problem =
                     std::format( "the {} mesh '{}' could not be registered", which, row->Path.generic_string() );
                return;
            }
            if ( const auto loaded = mesh->EnsureLoaded( assets ); !loaded )
            {
                problem = std::format( "the {} mesh '{}' would not load: {}", which, row->Path.generic_string(),
                                       loaded.GetError() );
                mesh.reset();
            }
        }

        const Animation::Skeleton* RigOf( const std::shared_ptr<Assets::SkeletonAsset>& asset )
        {
            return asset ? asset->GetSkeleton() : nullptr;
        }

        std::string ChainSpan( const std::string& start, const std::string& end )
        {
            return std::format( "{} > {}", start.empty() ? "?" : start, end.empty() ? "?" : end );
        }
    } // namespace

    RetargetDocument::RetargetDocument( const Assets::AssetHandle& retarget, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( retarget, assets, "Retarget" ),
                             AssetSubject( retarget, static_cast<uint32_t>( Assets::AssetTypeID::Retarget ) ) ),
           m_Assets( assets )
    {
        LoadModel();
        if ( m_Model )
            LoadSides();
    }

    RetargetDocument::~RetargetDocument() = default;

    std::filesystem::path RetargetDocument::FilePath() const
    {
        const auto* meta = m_Assets != nullptr
                                ? m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) )
                                : nullptr;
        return meta != nullptr ? Common::Constants::Path::FullPath( meta->Filepath ) : std::filesystem::path();
    }

    bool RetargetDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void RetargetDocument::LoadModel()
    {
        const auto path = FilePath();
        if ( path.empty() )
        {
            m_Unavailable = "This retarget is not registered with the asset manager — nothing to edit.";
            return;
        }
        auto data = Assets::Serialization::LoadRetargetFile( path );
        if ( !data )
        {
            m_Unavailable = std::format( "'{}' would not read: {}", path.filename().string(), data.GetError() );
            return;
        }
        m_Model = std::make_unique<RetargetDocumentModel>( data.ExtractValue(), CommandHistory::Get() );
    }

    void RetargetDocument::LoadSides()
    {
        const auto& data = m_Model->GetData();
        LoadSide( *m_Assets, data.SourceSkeleton, "source", m_Source.Skeleton, m_Source.Mesh, m_Source.Problem );
        LoadSide( *m_Assets, data.TargetSkeleton, "target", m_Target.Skeleton, m_Target.Mesh, m_Target.Problem );

        // The source rig's clips: every Animation row whose Rig tag is the source skeleton (UE's retarget editor
        // lists the source IK Rig's sequences). Read from the scan's tags; only the one shown is loaded.
        if ( const auto guid = GuidOf( data.SourceSkeleton ) )
        {
            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Animation ) )
                if ( row.Skeleton == *guid )
                    m_Clips.push_back( row.Path );
            std::ranges::sort( m_Clips );
        }
    }

    void RetargetDocument::EnsurePreviews()
    {
        if ( m_SourcePreview || !m_Unavailable.empty() || !m_Model )
            return;

        m_SourcePreview = std::make_unique<PreviewViewport>();
        m_TargetPreview = std::make_unique<PreviewViewport>();
        m_SourceUI      = std::make_unique<UI::UIHelper>();
        m_TargetUI      = std::make_unique<UI::UIHelper>();
        m_SourceUI->Init();
        m_TargetUI->Init();

        // Both posed by this window, never by a clip clock of their own: the source by the clip sampled at the
        // window's time, the target by the retarget of that pose — one time, so the two cannot drift.
        const auto show = []( PreviewViewport& preview, const std::shared_ptr<Assets::SkinnedMeshAsset>& mesh )
        {
            if ( !mesh )
                return;
            const auto& slots = mesh->GetMaterialHandles();
            preview.SetSkinnedMesh( mesh->GetMetadata().Handle,
                                    std::vector<Assets::AssetHandle>( slots.begin(), slots.end() ), nullptr );
            preview.SetPoseOverride( true );
        };
        show( *m_SourcePreview, m_Source.Mesh );
        show( *m_TargetPreview, m_Target.Mesh );
        if ( const auto* animator = m_TargetPreview->GetAnimatorForAuthoring() )
        {
            m_TargetRest = animator->GetAuthoringPose(); // Init = bind (Animator.hpp)
            m_TargetPose = m_TargetRest;
        }

        if ( !m_Clips.empty() && !m_Clip )
            SelectClip( m_Clips.front() );
    }

    void RetargetDocument::ReleaseView()
    {
        m_SourcePreview.reset();
        m_TargetPreview.reset();
        m_SourceUI.reset();
        m_TargetUI.reset();
    }

    void RetargetDocument::SelectClip( const std::filesystem::path& path )
    {
        auto clip = m_Assets->CreateAsset<Assets::AnimationAsset>( path, false );
        if ( !clip )
        {
            m_Status       = std::format( "clip '{}' could not be registered", path.generic_string() );
            m_StatusFailed = true;
            return;
        }
        if ( const auto loaded = clip->EnsureLoaded( *m_Assets ); !loaded )
        {
            m_Status = std::format( "clip '{}' would not load: {}", path.generic_string(), loaded.GetError() );
            m_StatusFailed = true;
            return;
        }
        m_Clip     = std::move( clip );
        m_ClipName = path.filename().string();
        m_Time     = 0.0;
    }

    void RetargetDocument::RebuildRetarget()
    {
        if ( m_BuiltRevision == m_Model->GetRevision() )
            return;
        m_BuiltRevision = m_Model->GetRevision();
        m_Retarget.reset();
        m_RetargetError.clear();

        const auto* source = RigOf( m_Source.Skeleton );
        const auto* target = RigOf( m_Target.Skeleton );
        if ( source == nullptr || target == nullptr )
        {
            m_RetargetError = !m_Source.Problem.empty() ? m_Source.Problem : m_Target.Problem;
            return;
        }
        auto setup = Assets::Serialization::BuildRetargetSetup( m_Model->GetData() );
        if ( !setup )
        {
            m_RetargetError = setup.GetError();
            return;
        }
        auto built = Animation::Retarget::RetargetSource::Create( *source, *target, setup.ExtractValue(),
                                                                  Subject().Owner, 0u );
        if ( !built )
        {
            m_RetargetError = built.GetError();
            return;
        }
        m_Retarget = built.ExtractValue();
    }

    void RetargetDocument::RecomputeVerdicts()
    {
        if ( m_VerdictRevision == m_Model->GetRevision() )
            return;
        m_VerdictRevision  = m_Model->GetRevision();
        const auto* source = RigOf( m_Source.Skeleton );
        const auto* target = RigOf( m_Target.Skeleton );
        if ( source == nullptr || target == nullptr )
        {
            m_ChainProblems.assign( m_Model->GetData().Chains.size(), "no rig to resolve against" );
            m_Verdict = !m_Source.Problem.empty() ? m_Source.Problem : m_Target.Problem;
            return;
        }
        m_ChainProblems  = m_Model->ChainProblems( *source, *target );
        const auto valid = m_Model->Validate( *source, *target );
        m_Verdict        = valid.IsSuccess() ? std::string() : valid.GetError();
    }

    void RetargetDocument::Pose()
    {
        auto* sourceAnimator = m_SourcePreview ? m_SourcePreview->GetAnimatorForAuthoring() : nullptr;
        auto* targetAnimator = m_TargetPreview ? m_TargetPreview->GetAnimatorForAuthoring() : nullptr;
        if ( sourceAnimator == nullptr || !m_Clip )
            return;

        const auto&  clip     = m_Clip->GetClip();
        const double duration = clip.DurationSeconds();
        if ( m_Playing )
            m_Time += static_cast<double>( ImGui::GetIO().DeltaTime );
        m_Time = duration > 0.0 ? std::fmod( std::max( m_Time, 0.0 ), duration ) : 0.0;

        // The clip's time on its own sequence: the playback range starts at Sequence.Start.
        auto at = Animation::SecondsToFrameTime( m_Time, clip.Sequence.TickRate );
        at.Frame.Value += clip.Sequence.Start.Value;
        sourceAnimator->SampleClipIntoLocalPose( clip, at );
        sourceAnimator->ApplyLocalPose();

        if ( targetAnimator == nullptr )
            return;
        const auto* target = RigOf( m_Target.Skeleton );
        const bool  posed =
             m_Retarget != nullptr && target != nullptr &&
             sourceAnimator->GetAuthoringPose().Size() == m_Retarget->GetSourceSkeleton().GetBones().size() &&
             m_TargetPose.Size() == target->GetBones().size() &&
             m_Retarget->Run( *target, sourceAnimator->GetAuthoringPose(), m_TargetPose );
        if ( const auto set = targetAnimator->SetAuthoringPose( posed ? m_TargetPose : m_TargetRest ); !set )
            m_RetargetError = set.GetError();
        else if ( m_Retarget != nullptr && !posed )
            m_RetargetError = m_Retarget->GetLastError().empty()
                                   ? std::string( "the preview meshes' rigs are not the retarget's skeletons" )
                                   : m_Retarget->GetLastError();
        targetAnimator->ApplyLocalPose();
    }

    void RetargetDocument::OnPreUpdate()
    {
        // The slot is not claimed until the window has been drawn (StaticMeshViewerDocument's gate).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreviews();
        if ( !m_Model || !m_SourcePreview || m_RenderSize.x == 0u || m_RenderSize.y == 0u )
            return;

        RebuildRetarget();
        Pose();
        m_SourcePreview->Update( m_RenderSize.x, m_RenderSize.y );
        m_TargetPreview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    ISubjectDocument::DiskState RetargetDocument::GetDiskState() const
    {
        if ( !m_Model )
            return DiskState::Untracked;
        return m_Model->IsDirty() ? DiskState::Dirty : DiskState::Clean;
    }

    void RetargetDocument::Save()
    {
        const auto path = FilePath();
        if ( const auto saved = m_Model->Save( path ); !saved )
        {
            m_Status       = std::format( "Save failed: {}", saved.GetError() );
            m_StatusFailed = true;
            LOG_ERROR( "Retarget: {}", m_Status );
            return;
        }
        // A resident RetargetAsset re-reads the file it was built from; its revision moves, so every entity
        // retargeting through it rebuilds its retargeter (AnimationECSSystem's stamp) on the next frame.
        if ( const auto asset =
                  m_Assets->FindByHandle<Assets::RetargetAsset>( Assets::AssetHandle( Subject().Owner ) );
             asset && asset->IsReadyForUse() )
        {
            if ( const auto reloaded = asset->LoadFromFile(); !reloaded )
            {
                LOG_ERROR( "Retarget: '{}' was saved but the resident asset would not re-read it: {}",
                           path.generic_string(), reloaded.GetError() );
            }
            else
            {
                asset->ResolveDependencies( *m_Assets );
            }
        }
        m_Status       = std::format( "Saved {}", path.filename().string() );
        m_StatusFailed = false;
    }

    bool RetargetDocument::SaveDocument()
    {
        if ( !m_Model )
            return false;
        Save();
        return !m_StatusFailed;
    }

    bool RetargetDocument::DiscardEdits()
    {
        if ( !m_Model )
            return false;
        m_Model->Discard();
        return true;
    }

    void RetargetDocument::RunAutoMap()
    {
        const auto* source = RigOf( m_Source.Skeleton );
        const auto* target = RigOf( m_Target.Skeleton );
        if ( !m_Model || source == nullptr || target == nullptr )
        {
            m_Status       = "Auto-map needs both rigs loaded";
            m_StatusFailed = true;
            return;
        }
        auto mapped = m_Model->AutoMap( *source, *target );
        if ( !mapped )
        {
            m_Status       = std::format( "Auto-map: {}", mapped.GetError() );
            m_StatusFailed = true;
            return;
        }
        m_Status =
             std::format( "Auto-map mapped {} of {} chains", mapped.GetValue(), m_Model->GetData().Chains.size() );
        m_StatusFailed = false;
    }

    std::vector<ISubjectDocument::DocumentAction> RetargetDocument::Actions()
    {
        return { { "Auto-map", [this]() { RunAutoMap(); } },
                 { "Save", [this]() { (void)SaveDocument(); } },
                 { "Play", [this]() { m_Playing = true; } },
                 { "Pause", [this]() { m_Playing = false; } } };
    }

    bool RetargetDocument::BoneCombo( const char* label, const Animation::Skeleton* skeleton, std::string& bone )
    {
        bool changed = false;
        if ( ImGui::BeginCombo( label, bone.empty() ? "(none)" : bone.c_str() ) )
        {
            if ( skeleton == nullptr )
                ImGui::TextDisabled( "the rig is not loaded" );
            else
                for ( const auto& info : skeleton->GetBones() )
                {
                    if ( ImGui::Selectable( info.Name.c_str(), info.Name == bone ) && info.Name != bone )
                    {
                        bone    = info.Name;
                        changed = true;
                    }
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    void RetargetDocument::DrawToolbar()
    {
        if ( ImGui::Button( ICON_MDI_SWAP_HORIZONTAL " Auto-map" ) )
            RunAutoMap();
        ImGui::SameLine();
        ImGui::BeginDisabled( !m_Model->IsDirty() );
        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( ImGui::Button( m_Playing ? ICON_MDI_PAUSE : ICON_MDI_PLAY ) )
            m_Playing = !m_Playing;
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 260.0f );
        if ( ImGui::BeginCombo( "Clip", m_ClipName.empty() ? "(no clip on the source rig)" : m_ClipName.c_str() ) )
        {
            for ( const auto& path : m_Clips )
            {
                const std::string name = path.filename().string();
                if ( ImGui::Selectable( name.c_str(), name == m_ClipName ) )
                    SelectClip( path );
            }
            ImGui::EndCombo();
        }
        if ( !m_Status.empty() )
        {
            ImGui::SameLine();
            if ( m_StatusFailed )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Status.c_str() );
            else
                ImGui::TextDisabled( "%s", m_Status.c_str() );
        }
    }

    void RetargetDocument::DrawChainTable()
    {
        const auto& chains = m_Model->GetData().Chains;
        ImGui::TextUnformatted( "Chains" );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "+ Chain" ) )
        {
            std::string name = "Chain";
            for ( size_t n = chains.size() + 1;
                  std::ranges::any_of( chains, [&]( const auto& chain ) { return chain.Name == name; } ); ++n )
                name = std::format( "Chain{}", n );
            if ( const auto added = m_Model->AddChain( name ); !added )
            {
                m_Status       = added.GetError();
                m_StatusFailed = true;
            }
            else
                m_Selected = static_cast<int>( m_Model->GetData().Chains.size() ) - 1;
        }
        ImGui::Separator();

        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if ( !ImGui::BeginTable( "##chains", 4, flags, ImVec2( 0.0f, ImGui::GetContentRegionAvail().y * 0.5f ) ) )
            return;
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 22.0f );
        ImGui::TableSetupColumn( "Chain" );
        ImGui::TableSetupColumn( "Source" );
        ImGui::TableSetupColumn( "Target" );
        ImGui::TableHeadersRow();
        for ( size_t i = 0; i < chains.size(); ++i )
        {
            const auto&        chain   = chains[i];
            const std::string& problem = i < m_ChainProblems.size() ? m_ChainProblems[i] : std::string();
            ImGui::PushID( static_cast<int>( i ) );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            if ( problem.empty() )
                ImGui::TextColored( ImVec4( 0.45f, 0.85f, 0.45f, 1.0f ), ICON_MDI_CHECK );
            else
                ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.25f, 1.0f ), ICON_MDI_ALERT );
            if ( !problem.empty() && ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", problem.c_str() );
            ImGui::TableSetColumnIndex( 1 );
            if ( ImGui::Selectable( chain.Name.c_str(), m_Selected == static_cast<int>( i ),
                                    ImGuiSelectableFlags_SpanAllColumns ) )
                m_Selected = static_cast<int>( i );
            ImGui::TableSetColumnIndex( 2 );
            ImGui::TextUnformatted( ChainSpan( chain.SourceStartBone, chain.SourceEndBone ).c_str() );
            ImGui::TableSetColumnIndex( 3 );
            ImGui::TextUnformatted( ChainSpan( chain.TargetStartBone, chain.TargetEndBone ).c_str() );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void RetargetDocument::DrawInspector()
    {
        const auto& chains = m_Model->GetData().Chains;
        ImGui::Separator();
        if ( m_Selected < 0 || static_cast<size_t>( m_Selected ) >= chains.size() )
        {
            ImGui::TextDisabled( "Select a chain to edit it." );
            return;
        }
        const size_t                 index = static_cast<size_t>( m_Selected );
        RetargetDocumentModel::Chain chain = chains[index];
        ImGui::Text( "Chain '%s'", chain.Name.c_str() );
        if ( index < m_ChainProblems.size() && !m_ChainProblems[index].empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.25f, 1.0f ), "%s", m_ChainProblems[index].c_str() );

        bool changed = false;
        ImGui::TextUnformatted( "Source" );
        changed |= BoneCombo( "Start##src", RigOf( m_Source.Skeleton ), chain.SourceStartBone );
        changed |= BoneCombo( "End##src", RigOf( m_Source.Skeleton ), chain.SourceEndBone );
        ImGui::TextUnformatted( "Target" );
        changed |= BoneCombo( "Start##tgt", RigOf( m_Target.Skeleton ), chain.TargetStartBone );
        changed |= BoneCombo( "End##tgt", RigOf( m_Target.Skeleton ), chain.TargetEndBone );
        changed |= ImGui::Checkbox( "Drive with IK", &chain.DriveWithIK );
        if ( changed )
        {
            if ( const auto edited = m_Model->EditChain( index, chain ); !edited )
            {
                m_Status       = edited.GetError();
                m_StatusFailed = true;
            }
        }

        // What the runtime does with the chain, stated rather than offered: it has one mode of each.
        ImGui::Spacing();
        ImGui::TextDisabled( "Rotation: interpolated FK along the chain" );
        ImGui::TextDisabled( "Translation: pelvis only, scaled by the rigs' height" );
        if ( chain.DriveWithIK )
            ImGui::TextDisabled( "IK goal: the chain's end bone (%s)", chain.TargetEndBone.c_str() );

        ImGui::Spacing();
        if ( ImGui::Button( "Remove chain" ) )
        {
            if ( const auto removed = m_Model->RemoveChain( index ); !removed )
            {
                m_Status       = removed.GetError();
                m_StatusFailed = true;
            }
            else
                m_Selected = -1;
        }
    }

    void RetargetDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;

        if ( !m_Unavailable.empty() || !m_Model )
        {
            ImGui::TextWrapped( "%s", m_Unavailable.c_str() );
            return;
        }

        RecomputeVerdicts();
        DrawToolbar();
        if ( !m_Verdict.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.25f, 1.0f ), ICON_MDI_ALERT " %s", m_Verdict.c_str() );
        else if ( !m_RetargetError.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.25f, 1.0f ), ICON_MDI_ALERT " %s",
                                m_RetargetError.c_str() );

        const ImVec2 avail   = ImGui::GetContentRegionAvail();
        const float  spacing = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 pane( std::max( ( avail.x - kSidePanelWidth - 2.0f * spacing ) * 0.5f, 1.0f ),
                           std::max( avail.y, 1.0f ) );
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( pane.x ), static_cast<uint32_t>( pane.y ) );

        const auto drawPane = [&]( const char* id, const char* caption, PreviewViewport* preview, UI::UIHelper* ui,
                                   const Side& side )
        {
            if ( ImGui::BeginChild( id, pane ) )
            {
                if ( !side.Problem.empty() )
                    ImGui::TextWrapped( "%s", side.Problem.c_str() );
                else if ( preview == nullptr || ui == nullptr )
                    ImGui::TextDisabled( "Starting the preview..." );
                else
                {
                    (void)preview->Draw( *ui, ImGui::GetContentRegionAvail(), PreviewInteraction::Interactive );
                    ImGui::SetCursorPos( ImVec2( 8.0f, 6.0f ) );
                    ImGui::TextUnformatted( caption );
                }
            }
            ImGui::EndChild();
        };
        drawPane( "##source", "Source", m_SourcePreview.get(), m_SourceUI.get(), m_Source );
        ImGui::SameLine();
        drawPane( "##target", "Target", m_TargetPreview.get(), m_TargetUI.get(), m_Target );
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##retargetside", ImVec2( kSidePanelWidth, pane.y ) ) )
        {
            DrawChainTable();
            DrawInspector();
        }
        ImGui::EndChild();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestRetargetDocument( Assets::AssetManager*        assets,
                                                                    const std::string&           path,
                                                                    const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension().string() !=
                  std::string( Common::Content::KindSpec( Common::Content::ContentKind::Retarget ).Extension ) ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::RetargetAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::RetargetAsset>( path, false );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a retarget — no editor was opened.", path );
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
