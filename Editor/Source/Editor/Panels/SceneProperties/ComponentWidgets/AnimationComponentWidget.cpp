#include "AnimationComponentWidget.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <ImGui/imgui.h>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <Editor/Panels/Animation/AnimGraphPanel.hpp>
#include <Editor/Core/PanelRequests.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>
#include <Editor/Panels/PanelContext.hpp>
#include <Editor/Panels/Sequencer/SequencerPanel.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    AnimationComponentWidget::AnimationComponentWidget( const Animation::AnimationLibrary* animationLibrary )
         : IComponentWidget( "Animation" ), m_AnimationLibrary( animationLibrary )
    {
    }

    void AnimationComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* /*scene*/ )
    {
        auto& animation = entity.GetComponent<ECS::AnimationComponent>();

        Utils::ImGuiUtilities::PushID();

        if ( !animation.Animator )
        {
            ImGui::TextDisabled( "No animator assigned" );
            Utils::ImGuiUtilities::PopID();
            return;
        }

        auto* animator = animation.Animator.get();

        // ============================================================
        // BASIC CONTROLS
        // ============================================================

        // Playback, on the panel's shared rows.
        Utils::ImGuiUtilities::ResetPropertyRows();

        Utils::ImGuiUtilities::BeginPropertyRow( "Playing" );
        ImGui::Checkbox( "##playing", &animation.Playing );
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Loop" );
        ImGui::Checkbox( "##loop", &animation.Loop );
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Speed" );
        ImGui::DragFloat( "##speed", &animation.PlaybackSpeed, 0.01f, 0.0f, 3.0f, "%.2fx" );
        Utils::ImGuiUtilities::EndPropertyRow();

        // ============================================================
        // CLIP SELECTION
        // ============================================================

        const auto&    skeleton = animator->GetSkeleton();
        const uint64_t sig      = skeleton.GetSignature();

        static std::vector<Assets::Asset<Assets::AnimationAsset>> cached;
        static uint64_t                                           cachedSig = 0;

        if ( cachedSig != sig )
        {
            // ONE rule, shared with AnimationECSSystem's resolution of the very name this combo writes into
            // the component (see ClipSkeletonMatch.hpp). The picker asking one question and the runtime
            // another is what made a chosen clip fail to play in silence.
            cached    = m_AnimationLibrary->GetForSkeleton( skeleton );
            cachedSig = sig;
        }

        const char* preview = animation.CurrentClip.empty() ? "Select Clip" : animation.CurrentClip.c_str();

        Utils::ImGuiUtilities::BeginPropertyRow( "Clip" );
        if ( ImGui::BeginCombo( "##ClipSelect", preview ) )
        {
            for ( const auto& animAsset : cached )
            {
                const auto& clip     = animAsset->GetClip();
                bool        selected = ( animation.CurrentClip == clip.AnimationName );

                if ( ImGui::Selectable( clip.AnimationName.c_str(), selected ) )
                {
                    animation.CurrentClip = clip.AnimationName;
                    animator->Play( clip );
                }

                if ( selected )
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }
        Utils::ImGuiUtilities::EndPropertyRow();

        // ============================================================
        // TIMELINE
        // ============================================================

        if ( animator->GetDuration() > 0.0f )
        {
            float currentTime = animator->GetCurrentTime();
            float duration    = animator->GetDuration();

            Utils::ImGuiUtilities::BeginPropertyRow( "Time" );
            if ( ImGui::SliderFloat( "##Timeline", &currentTime, 0.0f, duration, "%.2f s" ) )
            {
                animation.Playing = false;
                animator->SetTime( currentTime );
            }
            Utils::ImGuiUtilities::EndPropertyRow();
        }

        // Keyframe/track editing lives in the Sequencer (a proper timeline); Details stays focused on playback
        // + the AnimGraph summary.
        // The authoring tools, opened EXPLICITLY. None of these panels shows up on selection any more:
        // clicking a character is not a request to author its animation.
        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        const float half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        // THE RIG'S OWN TIMELINE, ON THIS ENTITY. It used to be SequencerPanel::RequestOpen() — a static
        // inbox with no payload, so this button could only say "reveal the one Sequencer window" and the
        // window then had to guess which rig it was about from the selection. It sends a SUBJECT now, so
        // two characters can be keyed side by side and a second press focuses the window that is already
        // on this one. Disabled without a rig, because the subject is the SkinnedMeshComponent: a button
        // that opened nothing and said nothing is the dead control this project's contract forbids.
        const bool hasRig = entity.HasComponent<ECS::SkinnedMeshComponent>();
        ImGui::BeginDisabled( !hasRig );
        if ( ImGui::Button( ICON_MDI_CHART_TIMELINE "  Sequencer", ImVec2( half, 0.0f ) ) )
            Core::SubjectOpenRequests::Request( SequencerPanel::SkeletalSubjectFor( EntityId( entity ) ) );
        ImGui::EndDisabled();
        Utils::ImGuiUtilities::Tooltip( hasRig ? "Author clips on a timeline (keyframes per bone)"
                                               : "Needs a Skinned Mesh: the timeline keys BONES, and the "
                                                 "bones are that component's skeleton" );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_LAYERS "  Anim Layers", ImVec2( ImGui::GetContentRegionAvail().x, 0.0f ) ) )
            Core::PanelRequests::Open( "Anim Layers" );
        Utils::ImGuiUtilities::Tooltip( "Additive layers on top of the base clip" );

        RenderAnimGraph( entity, animation, cached );

        Utils::ImGuiUtilities::PopID();
    }

    // Compact AnimGraph summary in Details: create / active-badge / counts + an "Open in Anim Graph" button.
    // Full authoring (states / transitions / parameters) lives in the visual Anim Graph node panel — no triple
    // UI.
    void
    AnimationComponentWidget::RenderAnimGraph( ECS::Entity& entity, ECS::AnimationComponent& animation,
                                               const std::vector<Assets::Asset<Assets::AnimationAsset>>& clips )
    {
        namespace G = Animation::Graph;

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_STATE_MACHINE "  AnimGraph (State Machine)" ) )
            return;

        ImGui::Indent( 6.0f );
        ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );

        if ( !animation.Graph )
        {
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextDisabled( "A state machine that picks the clip from live parameters "
                                 "(e.g. Speed, IsJumping). Author it visually in the Anim Graph panel." );
            ImGui::PopTextWrapPos();
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_PLUS_CIRCLE "  Create AnimGraph", 28.0f ) )
            {
                auto     graph = std::make_shared<G::AnimGraph>();
                G::State idle;
                idle.Name = "Idle";
                if ( !clips.empty() )
                    idle.Clip = clips.front()->GetClip().AnimationName;
                graph->States.push_back( idle );
                graph->Entry    = "Idle";
                animation.Graph = graph;
                animation.GraphRevision++;
                // Straight into the visual editor, ON THIS ENTITY. The subject is what the request
                // carries, so the window that opens is this graph's and not "whatever is selected".
                Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( EntityId( entity ) ) );
            }
            ImGui::Unindent( 6.0f );
            return;
        }

        auto* eval = animation.GraphEvaluator.get();

        // Active-state badge.
        if ( eval && eval->CurrentState() )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.65f, 0.2f, 1.0f ), ICON_MDI_PLAY );
            ImGui::SameLine( 0.0f, 6.0f );
            ImGui::Text( "Active: %s", eval->CurrentState()->Name.c_str() );
        }
        else
        {
            ImGui::TextDisabled( ICON_MDI_PAUSE " Active state shows in Play/Preview" );
        }
        ImGui::TextDisabled( "%zu states  \xc2\xb7  %zu parameters", animation.Graph->States.size(),
                             animation.Graph->Parameters.size() );

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        // THE BUTTON THE OWNER ASKED FOR, and the one that could not be built before U7: it opens a
        // document FROM the component in front of the user. Open-or-focus falls out of the subject — a
        // second press brings the window that is already on this entity forward instead of making a
        // second one (EditorLayer::ServiceSubjectOpenRequests).
        if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_STATE_MACHINE "  Open in Anim Graph", 28.0f ) )
            Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( EntityId( entity ) ) );

        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.46f, 0.19f, 0.19f, 1.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.62f, 0.24f, 0.24f, 1.0f ) );
        const bool remove = ImGui::Button( ICON_MDI_DELETE "  Remove AnimGraph" );
        ImGui::PopStyleColor( 2 );
        if ( remove )
        {
            animation.Graph.reset();
            animation.GraphEvaluator.reset();
        }

        ImGui::Unindent( 6.0f );
    }

    DESERT_REGISTER_CUSTOM_COMPONENT( ECS::AnimationComponent, "Animation", false,
                                      ( []( ECS::Entity& e, ::Desert::Core::Scene* s,
                                            const ComponentEditContext& ctx )
                                        { AnimationComponentWidget( ctx.AnimationLibrary ).Render( e, s ); } ) )
} // namespace Desert::Editor
