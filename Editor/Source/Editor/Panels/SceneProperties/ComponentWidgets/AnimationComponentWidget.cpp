#include "AnimationComponentWidget.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <ImGui/imgui.h>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <filesystem>

#include <Editor/Panels/Animation/AnimGraphPanel.hpp>
#include <Editor/Core/PanelRequests.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>
#include <Editor/Panels/PanelContext.hpp>
#include <Editor/Panels/Sequencer/SequencerPanel.hpp>
#include <Editor/Core/AssetPickerRows.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    AnimationComponentWidget::AnimationComponentWidget( const Animation::AnimationLibrary* animationLibrary,
                                                        Assets::AssetManager*              assetManager )
         : IComponentWidget( "Animation" ), m_AnimationLibrary( animationLibrary ), m_AssetManager( assetManager )
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

        // ── THE SLOT ──────────────────────────────────────────────────────────────────────────────────
        //
        // A graph is a FILE now, so this is a picker over the project's graphs and not a "create it inside
        // this entity" button. That is the whole of §5.1's animation half on screen: two characters point
        // at one `.danimgraph` and share it, where the old blob made every copy its own divergent island.
        auto* const assets = m_AssetManager;

        std::string preview = "None (plays CurrentClip)";
        if ( animation.GraphAsset )
        {
            // "(missing)" is a REAL and DIFFERENT state from "None" — a handle whose file the asset scan
            // did not find — and the two look identical on screen unless they are named apart. The rig
            // slot next door states the same pair for the same reason.
            preview = "(missing)";
            if ( assets != nullptr )
            {
                if ( auto graph = assets->FindByHandle<Assets::AnimGraphAsset>( animation.GraphAsset ) )
                {
                    preview = graph->GetDisplayName();
                }
            }
        }

        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##animgraphslot", preview.c_str() ) )
        {
            if ( ImGui::Selectable( "None (plays CurrentClip)", !animation.GraphAsset ) )
            {
                // THE SLOT IS CLEARED, THE FILE IS NOT DELETED. A picker that removed content from disk
                // would make "I picked the wrong one" unrecoverable; the graph object goes too, because
                // AnimationECSSystem hands it over from the asset and an entity with no handle that kept
                // the old pointer would be evaluating a graph its scene file no longer names.
                animation.GraphAsset = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
                animation.Graph.reset();
                animation.GraphEvaluator.reset();
            }
            if ( assets != nullptr )
            {
                for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::AnimGraph ) )
                {
                    const bool selected = ( row.Handle == animation.GraphAsset );
                    if ( ImGui::Selectable( ::Desert::Editor::PickerDisplayName( row ).c_str(), selected ) )
                    {
                        // ONLY THE HANDLE. The object is AnimationECSSystem's to hand over, from the
                        // asset, so that every entity naming one file ends up pointing at ONE object —
                        // assigning `graph->GetGraph()` here as well would be a second writer of the same
                        // fact and the two would disagree the first time a load replaced it.
                        animation.GraphAsset = row.Handle;
                        animation.GraphEvaluator.reset();
                    }
                    if ( selected )
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }
            ImGui::EndCombo();
        }

        if ( !animation.GraphAsset )
        {
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextDisabled( "A state machine that picks the clip from live parameters "
                                 "(e.g. Speed, IsJumping). Pick one above, or make a new one." );
            ImGui::PopTextWrapPos();
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_PLUS_CIRCLE "  New AnimGraph", 28.0f ) )
            {
                CreateAnimGraphAsset( entity, animation, clips, assets );
            }
            Utils::ImGuiUtilities::Tooltip( "Writes a new .danimgraph under the project's AnimGraphs/ "
                                            "folder and points this entity at it" );
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
        if ( animation.Graph )
        {
            ImGui::TextDisabled( "%zu states  \xc2\xb7  %zu parameters", animation.Graph->States.size(),
                                 animation.Graph->Parameters.size() );
        }
        else
        {
            // NAMED, NOT BLANK. The slot holds a handle and the object is not here — either the file is
            // gone or the entity has not been through AnimationECSSystem yet (no skinned mesh, no
            // animator). Both are states an author can act on; an empty line is not.
            ImGui::TextDisabled( "not resolved yet — needs a Skinned Mesh, or the file is missing" );
        }

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        // THE BUTTON THE OWNER ASKED FOR, and the one that could not be built before U7: it opens a
        // document FROM the component in front of the user. Open-or-focus falls out of the subject — a
        // second press brings the window that is already on this entity forward instead of making a
        // second one (EditorLayer::ServiceSubjectOpenRequests).
        if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_STATE_MACHINE "  Open in Anim Graph", 28.0f ) )
            Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( EntityId( entity ) ) );

        ImGui::Unindent( 6.0f );
    }

    void AnimationComponentWidget::CreateAnimGraphAsset(
         ECS::Entity& entity, ECS::AnimationComponent& animation,
         const std::vector<Assets::Asset<Assets::AnimationAsset>>& clips, Assets::AssetManager* assets )
    {
        namespace G = Animation::Graph;

        if ( assets == nullptr )
        {
            LOG_ERROR( "[Animation] a new anim graph cannot be created without an asset manager." );
            return;
        }

        G::AnimGraph graph;
        // NAMED AFTER THE ENTITY, because the FILE is named after the graph (the migration does the same)
        // and a file called "AnimGraph.danimgraph" would be claimed by the first character and then
        // silently shared by every one after it — sharing is the feature, but it has to be CHOSEN.
        graph.Name = entity.GetComponent<ECS::TagComponent>().Tag + "_Graph";
        G::State idle;
        idle.Name = "Idle";
        if ( !clips.empty() )
            idle.Clip = clips.front()->GetClip().AnimationName;
        graph.States.push_back( idle );
        graph.Entry = "Idle";

        // The same sanitisation the migration applies, and for the same reason: an entity tag is anything
        // the author typed, and a '/' in it would put the file outside AnimGraphs/.
        std::string stem;
        for ( const char c : graph.Name )
        {
            const bool safe = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ||
                              c == '_' || c == '-';
            stem.push_back( safe ? c : '_' );
        }
        if ( stem.empty() )
            stem = "AnimGraph";

        // A name already on disk is NOT overwritten: the author pressed "New", and taking another
        // character's graph away from it is the opposite of that.
        std::filesystem::path path;
        for ( int i = 0; i < 256; ++i )
        {
            const std::string candidate = i == 0 ? stem : stem + std::to_string( i );
            path                        = Common::Constants::Path::ANIM_GRAPH_PATH /
                   ( candidate + std::string( Animation::Graph::kAnimGraphExtension ) );
            if ( !std::filesystem::exists( path ) )
            {
                graph.Name = candidate;
                break;
            }
        }

        if ( const auto written = Assets::AnimGraphAsset::Save( path, graph ); !written )
        {
            LOG_ERROR( "[Animation] the new anim graph '{}' was not written: {} — the entity's slot is "
                       "left empty rather than pointed at a file that does not exist.",
                       path.string(), written.GetError() );
            return;
        }

        auto asset = assets->CreateAsset<Assets::AnimGraphAsset>( Assets::AssetPriority::Medium, path );
        if ( !asset || !asset->IsReadyForUse() )
        {
            LOG_ERROR( "[Animation] '{}' was written but could not be registered as an asset; the entity's "
                       "slot is left empty.",
                       path.string() );
            return;
        }

        animation.GraphAsset = asset->GetMetadata().Handle;
        animation.GraphEvaluator.reset();

        // Straight into the visual editor, ON THIS ENTITY. The subject is what the request carries, so the
        // window that opens is this graph's and not "whatever is selected".
        Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( EntityId( entity ) ) );
    }

    DESERT_REGISTER_CUSTOM_COMPONENT(
         ECS::AnimationComponent, "Animation", false,
         (
              []( ECS::Entity& e, ::Desert::Core::Scene* s, const ComponentEditContext& ctx )
              {
                  // LOCKED HERE AND NOT STORED AS A weak_ptr: the widget lives
                  // for exactly this call, so a pointer that is valid now is
                  // valid for all of it, and the lock is what makes that true.
                  const auto assets = ctx.AssetManager.lock();
                  AnimationComponentWidget( ctx.AnimationLibrary, assets.get() ).Render( e, s );
              } ) )
} // namespace Desert::Editor
