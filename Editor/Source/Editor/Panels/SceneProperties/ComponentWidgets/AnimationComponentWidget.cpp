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
#include <Editor/Core/SubjectOpenRequest.hpp>
#include <Editor/Panels/PanelContext.hpp>
#include <Editor/Panels/Sequencer/SequencerPanel.hpp>
#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/DetailsNavigation.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Widgets/AssetFieldOpen.hpp>

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

        Utils::ImGuiUtilities::BeginPropertyRow( "Update in Editor",
                                                 "Advance this animation in Edit mode (UE: Update Animation "
                                                 "in Editor). Play always advances it." );
        ImGui::Checkbox( "##updateInEditor", &animation.UpdateAnimationInEditor );
        Utils::ImGuiUtilities::EndPropertyRow();

        // ============================================================
        // SOURCE — ONE CHOICE (design 07 §14.1)
        // ============================================================

        // The clips this entity's mesh can play: the mesh's skeleton reference (GUID) under ClipPlaysOnMesh, the
        // SAME rule AnimationECSSystem resolves the name this slot writes (SkeletonReference.hpp). Asked when the
        // widget draws, not cached: a cache keyed by anything but the library's contents offered stale clips.
        const Assets::AssetHandle meshHandle = entity.HasComponent<ECS::SkinnedMeshComponent>()
                                                    ? entity.GetComponent<ECS::SkinnedMeshComponent>().MeshHandle
                                                    : Assets::AssetHandle{};
        const auto clips = m_AnimationLibrary->GetForMesh( m_AnimationLibrary->IdentifyMeshHandle( meshHandle ) );

        // NOT A SECOND FIELD: the source IS whether the graph slot names a file (AnimationECSSystem plays the
        // graph when it does and CurrentClip when it does not). The radio states that one fact instead of
        // leaving two silent paths; "Graph" with an empty slot is the picker's transient state, kept in ImGui's
        // storage because it is a UI step and not something the scene says.
        ImGuiStorage* const storage      = ImGui::GetStateStorage();
        const ImGuiID       pickingGraph = ImGui::GetID( "##pickingGraph" );
        const bool graphSource = static_cast<bool>( animation.GraphAsset ) || storage->GetBool( pickingGraph );
        Utils::ImGuiUtilities::BeginPropertyRow( "Source", "What this component plays: one clip, or an Anim Graph "
                                                           "that picks clips from live parameters" );
        if ( ImGui::RadioButton( "Clip", !graphSource ) && graphSource )
        {
            // THE SLOT IS CLEARED, THE FILE IS NOT DELETED (and the object goes with the handle, see below).
            animation.GraphAsset = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            animation.Graph.reset();
            animation.GraphEvaluator.reset();
            storage->SetBool( pickingGraph, false );
        }
        ImGui::SameLine();
        if ( ImGui::RadioButton( "Anim Graph", graphSource ) && !graphSource )
            storage->SetBool( pickingGraph, true );
        Utils::ImGuiUtilities::EndPropertyRow();

        const auto currentClip =
             std::ranges::find_if( clips, [&animation]( const auto& asset )
                                   { return asset && asset->GetClip().AnimationName == animation.CurrentClip; } );
        const bool hasClip = currentClip != clips.end();

        if ( !graphSource )
        {
            const auto playClip = [&]( const Assets::Asset<Assets::AnimationAsset>& asset )
            {
                animation.CurrentClip = asset->GetClip().AnimationName;
                animator->Play( asset->GetClip() );
            };

            Utils::ImGuiUtilities::BeginPropertyRow( "Clip", "The clip this component plays. Drag one from the "
                                                             "Content Browser, or pick from the clips this "
                                                             "skeleton can play" );
            const bool emptyClip = animation.CurrentClip.empty();
            const bool clicked   = Utils::ImGuiUtilities::AssetSlot(
                 "ClipSlot", emptyClip ? "None" : animation.CurrentClip.c_str(), emptyClip );
            if ( clicked )
                ImGui::OpenPopup( "clip_selector" );
            // Drag & drop from the Content Browser: the file must be one of the clips THIS skeleton plays — a
            // clip for another rig is refused in the log, not silently bound to a name that resolves to nothing.
            if ( ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( DragPayloads::AssetFile ) )
                {
                    const std::filesystem::path dropped( static_cast<const char*>( p->Data ) );
                    std::error_code             ec;
                    const auto                  match = std::ranges::find_if(
                         clips,
                         [&]( const auto& asset ) {
                             return asset &&
                                    std::filesystem::equivalent( asset->GetMetadata().Filepath, dropped, ec );
                         } );
                    if ( match != clips.end() )
                        playClip( *match );
                    else
                        LOG_WARN( "[Animation] '{}' is not a clip this entity's skeleton can play; the Clip slot "
                                  "is unchanged.",
                                  dropped.string() );
                }
                ImGui::EndDragDropTarget();
            }
            const uint64_t clipHandle =
                 hasClip ? static_cast<uint64_t>( ( *currentClip )->GetMetadata().Handle ) : uint64_t{ 0 };
            DrawAssetFieldOpen( clipHandle );
            DrawAssetFieldButtons( clipHandle );

            if ( ImGui::BeginPopup( "clip_selector" ) )
            {
                static ImGuiTextFilter filter;
                filter.Draw( "##Search", 200 );
                ImGui::Separator();
                for ( const auto& animAsset : clips )
                {
                    const auto& name = animAsset->GetClip().AnimationName;
                    if ( filter.PassFilter( name.c_str() ) &&
                         ImGui::Selectable( name.c_str(), animation.CurrentClip == name ) )
                        playClip( animAsset );
                }
                if ( clips.empty() )
                    ImGui::TextDisabled( "No clip in the project plays on this skeleton" );
                ImGui::EndPopup();
            }
            Utils::ImGuiUtilities::EndPropertyRow();
        }

        // ============================================================
        // PLAYBACK
        // ============================================================

        Utils::ImGuiUtilities::BeginPropertyRow( "Playing" );
        ImGui::Checkbox( "##playing", &animation.Playing );
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Loop" );
        ImGui::Checkbox( "##loop", &animation.Loop );
        Utils::ImGuiUtilities::EndPropertyRow();

        Utils::ImGuiUtilities::BeginPropertyRow( "Speed" );
        ImGui::DragFloat( "##speed", &animation.PlaybackSpeed, 0.01f, 0.0f, 3.0f, "%.2fx" );
        Utils::ImGuiUtilities::EndPropertyRow();

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

        if ( graphSource )
            RenderAnimGraph( entity, animation, clips );

        // The authoring tools, opened EXPLICITLY: clicking a character is not a request to author it. Only
        // tools with a reader are drawn (§14.1 rule №1): the Sequencer keys this rig's bones, the Anim Graph
        // button sits with the graph slot. Rig / Control Rig have no per-entity subject to open yet — they
        // are not drawn rather than drawn dead. "Anim Layers" is gone with the panel it opened (§9.4).
        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        const bool hasRig = entity.HasComponent<ECS::SkinnedMeshComponent>();
        ImGui::BeginDisabled( !hasRig );
        if ( ImGui::Button( ICON_MDI_CHART_TIMELINE "  Sequencer",
                            ImVec2( ImGui::GetContentRegionAvail().x, 0.0f ) ) )
            Core::SubjectOpenRequests::Request( SequencerPanel::SkeletalSubjectFor( EntityId( entity ) ) );
        ImGui::EndDisabled();
        Utils::ImGuiUtilities::Tooltip( hasRig ? "Author clips on a timeline (keyframes per bone)"
                                               : "Needs a Skinned Mesh: the timeline keys BONES, and the "
                                                 "bones are that component's skeleton" );

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

        const bool emptyGraph = !animation.GraphAsset;
        if ( emptyGraph )
            preview = "None";
        const ImGuiID pickingGraph = ImGui::GetID( "##pickingGraph" );
        // A slot like the mesh's and the material's (§14.1): drag a .danimgraph from the Content Browser,
        // or pick; double-click / the buttons open it or show it in the browser.
        const bool clickedGraph = Utils::ImGuiUtilities::AssetSlot( "AnimGraphSlot", preview.c_str(), emptyGraph );
        if ( clickedGraph )
            ImGui::OpenPopup( "animgraph_selector" );
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( DragPayloads::AssetFile ) )
            {
                const std::filesystem::path dropped( static_cast<const char*>( p->Data ) );
                std::error_code             ec;
                bool                        bound = false;
                for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::AnimGraph ) )
                {
                    if ( std::filesystem::equivalent( row.Path, dropped, ec ) )
                    {
                        animation.GraphAsset = row.Handle; // the object is AnimationECSSystem's to hand over
                        animation.GraphEvaluator.reset();
                        bound = true;
                        break;
                    }
                }
                if ( !bound )
                    LOG_WARN(
                         "[Animation] '{}' is not an Anim Graph of this project; the graph slot is unchanged.",
                         dropped.string() );
            }
            ImGui::EndDragDropTarget();
        }
        DrawAssetFieldOpen( static_cast<uint64_t>( animation.GraphAsset ) );
        DrawAssetFieldButtons( static_cast<uint64_t>( animation.GraphAsset ) );
        if ( animation.GraphAsset )
            ImGui::GetStateStorage()->SetBool( pickingGraph, false );

        if ( ImGui::BeginPopup( "animgraph_selector" ) )
        {
            if ( ImGui::Selectable( "None (plays the Clip)", !animation.GraphAsset ) )
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
                }
            }
            ImGui::EndPopup();
        }

        if ( !animation.GraphAsset )
        {
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextDisabled( "A state machine that picks the clip from live parameters "
                                 "(e.g. Speed, IsJumping). Pick one above, or make a new one." );
            ImGui::PopTextWrapPos();
            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            // The palette's "New AnimGraph" presses THIS button (DetailsNavigation): one handler, two routes.
            const bool commanded = TakeDetailsActionRequest( "New AnimGraph" );
            if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_PLUS_CIRCLE "  New AnimGraph", 28.0f ) ||
                 commanded )
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
            ImGui::TextDisabled( "%zu pose nodes  \xc2\xb7  %zu parameters", animation.Graph->Nodes.size(),
                                 animation.Graph->Parameters.size() );
        }
        else
        {
            // NAMED, NOT BLANK. The slot holds a handle and the object is not here — either the file is
            // gone or the entity has not been through AnimationECSSystem yet (no skinned mesh, no
            // animator). Both are states an author can act on; an empty line is not.
            ImGui::TextDisabled( "not resolved yet — needs a Skinned Mesh, or the file is missing" );
        }

        // ── LIVE PARAMETERS (§14.1: moved here from the graph — they are what one looks at on THIS
        // character). Written through the evaluator's DECLARED setters, so a type the graph disagrees with is
        // refused out loud; with no evaluator the row says why instead of moving nothing.
        ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );
        ImGui::TextUnformatted( "Live parameters" );
        if ( eval == nullptr )
        {
            ImGui::TextDisabled( "no evaluator yet — the graph is built once a Skinned Mesh with an animator "
                                 "has been through AnimationECSSystem" );
        }
        else if ( eval->Graph().Parameters.empty() )
        {
            ImGui::TextDisabled( "the graph declares no parameters" );
        }
        else
        {
            Utils::ImGuiUtilities::ResetPropertyRows();
            for ( const auto& param : eval->Graph().Parameters )
            {
                const float           live    = eval->GetFloat( param.Name );
                Common::BoolResultStr written = Common::MakeSuccess( true );
                Utils::ImGuiUtilities::BeginPropertyRow( param.Name.c_str(),
                                                         G::TypeName( static_cast<G::ParamType>( param.Type ) ) );
                ImGui::PushID( param.Name.c_str() );
                switch ( static_cast<G::ParamType>( param.Type ) )
                {
                    case G::ParamType::Bool:
                    {
                        bool value = live != 0.0f;
                        if ( ImGui::Checkbox( "##p", &value ) )
                            written = eval->SetBool( param.Name, value );
                        break;
                    }
                    case G::ParamType::Int:
                    {
                        int value = static_cast<int>( live );
                        if ( ImGui::DragInt( "##p", &value ) )
                            written = eval->SetInt( param.Name, value );
                        break;
                    }
                    case G::ParamType::Float:
                    {
                        float value = live;
                        if ( ImGui::DragFloat( "##p", &value, 0.01f ) )
                            written = eval->SetFloat( param.Name, value );
                        break;
                    }
                }
                ImGui::PopID();
                Utils::ImGuiUtilities::EndPropertyRow();
                if ( !written )
                    LOG_ERROR( "[Animation] live parameter '{}' refused: {}", param.Name, written.GetError() );
            }
        }

        ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
        // THE BUTTON THE OWNER ASKED FOR, and the one that could not be built before U7: it opens a
        // document FROM the component in front of the user. Open-or-focus falls out of the subject — a
        // second press brings the window that is already on this GRAPH forward instead of making a second
        // one (EditorLayer::ServiceSubjectOpenRequests). The subject is the `.danimgraph` the slot names
        // (ANIM-FIX8), so two characters on one graph share one window; an empty slot has nothing to open.
        ImGui::BeginDisabled( !animation.GraphAsset );
        if ( Utils::ImGuiUtilities::AccentButton( ICON_MDI_STATE_MACHINE "  Open in Anim Graph", 28.0f ) )
            Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( animation.GraphAsset ) );
        ImGui::EndDisabled();

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

        G::AnimGraph graph = G::MakeStateMachineGraph();
        // NAMED AFTER THE ENTITY, because the FILE is named after the graph (the migration does the same)
        // and a file called "AnimGraph.danimgraph" would be claimed by the first character and then
        // silently shared by every one after it — sharing is the feature, but it has to be CHOSEN.
        graph.Name = entity.GetComponent<ECS::TagComponent>().Tag + "_Graph";
        G::State idle;
        idle.Name = "Idle";
        if ( !clips.empty() )
            idle.Clip = clips.front()->GetClip().AnimationName;
        G::OutputMachine( graph )->States.push_back( idle );
        G::OutputMachine( graph )->Entry = "Idle";

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

        auto asset = assets->CreateAsset<Assets::AnimGraphAsset>( path );
        if ( !asset || !asset->IsReadyForUse() )
        {
            LOG_ERROR( "[Animation] '{}' was written but could not be registered as an asset; the entity's "
                       "slot is left empty.",
                       path.string() );
            return;
        }

        animation.GraphAsset = asset->GetMetadata().Handle;
        animation.GraphEvaluator.reset();

        // Straight into the visual editor, over the file just made. The subject is what the request carries,
        // so the window that opens is this graph's and not "whatever is selected".
        Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( animation.GraphAsset ) );
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
