#include "AnimGraphPanel.hpp"

#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Editor/Panels/PanelContext.hpp>

#include <Editor/Core/GraphCanvas/GraphCanvasView.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/Animation/AnimGraphCanvasPlan.hpp>

#include <Common/Core/Logger.hpp>

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>

#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace ed = ax::NodeEditor;

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace G     = Animation::Graph;

    namespace
    {
        // A live parameter write can now be REFUSED (see Evaluator::SetBool and friends), and a panel that
        // dropped the result would be the silence those refusals exist to remove. It can only happen when
        // the parameter was renamed in the same frame the slider moved, so this is a diagnostic and not a
        // modal — but it is a diagnostic that exists.
        void ReportParamWrite( const Common::BoolResultStr& result )
        {
            if ( !result.IsSuccess() )
            {
                LOG_ERROR( "[AnimGraphPanel] {}", result.GetError() );
            }
        }

        const char* kTypeNames[] = { "Bool", "Int", "Float" };
        const char* kOpNames[]   = { ">", "<", ">=", "<=", "==", "!=", "is true", "is false" };
    } // namespace

    AnimGraphPanel::AnimGraphPanel( const SubjectId& subject, const std::string& displayName,
                                    const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Animation::AnimationLibrary*            library,
                                    Assets::AssetManager*                         assetManager )
         : ISubjectDocument( displayName, subject ), m_Scene( scene ), m_Library( library ),
           m_AssetManager( assetManager )
    {
        ed::Config config;
        config.SettingsFile = nullptr; // node positions live in the graph (State.X/Y), not a stray json
        m_Context           = ed::CreateEditor( &config );
    }

    AnimGraphPanel::~AnimGraphPanel()
    {
        if ( m_Context )
            ed::DestroyEditor( m_Context );
    }

    // THE STATIC RequestOpen INBOX IS GONE, and its absence is half the point of U7. It was a file-static
    // bool that meant "reveal the one Anim Graph window", which is all a singleton can be asked. A document
    // is asked for BY SUBJECT — Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( entity ) ) —
    // and the Details button beside the Animation component sends exactly that.

    ECS::AnimationComponent* AnimGraphPanel::ResolveComponent() const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return nullptr; // the scene this document was opened over has been closed

        const auto entOpt = scene->FindEntityByID( Subject().Owner );
        if ( !entOpt )
            return nullptr; // the entity was deleted

        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::AnimationComponent>() )
            return nullptr; // the component was removed from under the window

        return &entity.GetComponent<ECS::AnimationComponent>();
    }

    Assets::Asset<Assets::AnimGraphAsset> AnimGraphPanel::ResolveAsset() const
    {
        const ECS::AnimationComponent* anim = ResolveComponent();
        if ( !anim || !anim->GraphAsset || m_AssetManager == nullptr )
            return nullptr;
        return m_AssetManager->FindByHandle<Assets::AnimGraphAsset>( anim->GraphAsset );
    }

    void AnimGraphPanel::MarkEdited()
    {
        if ( const auto asset = ResolveAsset() )
        {
            asset->MarkEdited();
            return;
        }
        // NOT SILENT. An edit that reaches no asset is an edit no evaluator will ever be told about: the
        // canvas would show the new shape while every character on this graph went on playing the old one,
        // which is the exact class of defect — a comment or a control promising something the tree does
        // not do — that this project keeps paying for.
        m_Status        = "this graph has no asset behind it; the edit will not reach the running character";
        m_StatusIsError = true;
    }

    void AnimGraphPanel::SaveGraph()
    {
        const auto asset = ResolveAsset();
        if ( !asset )
        {
            m_Status        = "nothing to save: this window's entity names no loaded anim graph";
            m_StatusIsError = true;
            return;
        }

        const auto graph = asset->GetGraph();
        if ( !graph )
        {
            m_Status        = "nothing to save: the asset holds no graph";
            m_StatusIsError = true;
            return;
        }

        // THE SUBJECT'S OWN FILE, on the shader graph's terms: a path composed from the graph's Name would
        // be a silent Save As the first time somebody renamed one.
        if ( const auto written = Assets::AnimGraphAsset::Save( asset->GetMetadata().Filepath, *graph ); !written )
        {
            m_Status        = "NOT saved: " + written.GetError();
            m_StatusIsError = true;
            return;
        }
        m_Status        = "Saved " + asset->GetMetadata().Filepath.filename().string();
        m_StatusIsError = false;
    }

    std::vector<ISubjectDocument::DocumentAction> AnimGraphPanel::Actions()
    {
        // The SAME function the toolbar button calls. A second code path here would be a second behaviour
        // to keep in step, and the point of the entry is that what a client drives is what a person
        // presses.
        //  is here for the reason  is: this machine refuses synthetic input, so a view
        // control that exists only as a toolbar button is a view control no test and no script can reach.
        return {
             { "Save", [this] { SaveGraph(); } },
             { "Frame All", [this] { Graph::FrameAll( m_Context ); } },
             { "Frame Selection", [this] { Graph::FrameSelection( m_Context ); } },
        };
    }

    void AnimGraphPanel::OnUIRender()
    {
        // NO "SELECT AN ENTITY" EMPTY STATE any more: this window is about one entity for its whole life.
        // A null here is a subject that has just died, and the editor closes the document for it on the
        // same frame (EditorLayer::CloseDocumentsWhoseSubjectIsGone) — so the message says what happened
        // rather than asking the user to fix it.
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( !anim )
        {
            ImGui::TextDisabled( "This entity, its Animation component or its scene is gone — closing." );
            return;
        }

        if ( !anim->Graph )
        {
            // NO "Create AnimGraph" BUTTON HERE ANY MORE, and its absence is the point. A graph is a FILE
            // now, and creating one means writing it, registering it and pointing this entity's slot at
            // it — three steps that can each fail and that belong where the SLOT is, in Details. A second
            // creator here would be a second way to make a graph and the two would drift; what stood here
            // could only ever make an unsaved one, which is precisely the storage §5.1 removed.
            ImGui::TextWrapped( "This entity names no anim graph, or the file it names is not loaded. "
                                "Pick or create one in Details > Animation > AnimGraph." );
            return;
        }

        // Clip names available for this skeleton (for the clip picker) — from the (lazily built) Animator.
        std::vector<std::string> clipNames;
        if ( anim->Animator && m_Library )
        {
            // The SAME rule AnimationECSSystem resolves the chosen name with. This picker used to ask
            // tolerantly while the state machine asked exactly, so a Mixamo clip offered here resolved to
            // nothing at runtime and the state played nothing without a word.
            for ( const auto& a : m_Library->GetForSkeleton( anim->Animator->GetSkeleton() ) )
                clipNames.push_back( a->GetClip().AnimationName );
        }

        // Toolbar.
        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE "  Save" ) )
            SaveGraph();
        Utils::ImGuiUtilities::Tooltip( "Write this graph back to its .danimgraph" );
        ImGui::SameLine();
        if ( ImGui::Button( "+ State" ) )
        {
            G::State ns;
            // UNIQUE BY CONSTRUCTION. "State_" + size() collides the moment a state is deleted and
            // another added, and two states sharing a name is not cosmetic: `Entry`, `Transition::To` and
            // `Evaluator::FindState` all resolve by string and all take the FIRST match, so the second
            // one is unreachable and plays the first one's clip with nothing said.
            ns.Name = Graph::MakeUniqueStateName( *anim->Graph,
                                                  "State_" + std::to_string( anim->Graph->States.size() ), -1 );
            anim->Graph->States.push_back( ns );
            MarkEdited();
        }
        ImGui::SameLine();
        Graph::DrawViewButtons( m_Context );
        if ( const auto* cur = anim->GraphEvaluator ? anim->GraphEvaluator->CurrentState() : nullptr )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "| Active: %s", cur->Name.c_str() );
        }

        // The window's ONE error channel — a save that failed, or an edit that reached no asset. It has to
        // be on screen, because the alternative is a log line nobody authoring a graph is reading.
        Graph::DrawStatusLine( m_Status, m_StatusIsError );

        // THE CANVAS IS NOT WRAPPED IN A CHILD WINDOW, and that is the fix to "a docked Anim Graph drew an
        // empty rectangle". See `Graph::DeferredFrameAll` for the measurement: a child sized exactly to
        // the canvas gives `imgui-node-editor`'s lazy first-frame init nowhere to put its throwaway dummy
        // widget, and the canvas is refused for that frame. The shader graph never had a child here and
        // survived by four pixels. It gets an explicit WIDTH instead, which is the only thing the child
        // was really doing.
        constexpr float kSideW  = 300.0f;
        const float     canvasW = std::max( 160.0f, ImGui::GetContentRegionAvail().x - kSideW );
        DrawCanvas( *anim, canvasW );

        ImGui::SameLine();
        ImGui::BeginGroup();
        DrawSidePanel( *anim, clipNames );
        ImGui::EndGroup();
    }

    void AnimGraphPanel::DrawCanvas( ECS::AnimationComponent& anim, float width )
    {
        auto& graph = *anim.Graph;
        bool  dirty = false;

        // EVERY ID ON THIS CANVAS, AND WHAT IT NAMES — decided before a single ImGui call, by a unit with
        // no ImGui in it. `NodeId( i ) = i + 1` used to live here.
        m_Canvas = Graph::PlanAnimGraph( graph, m_Ids );

        // The size the canvas is actually drawn at, which is also what `DeferredFrameAll` waits to see
        // stop changing. Height 0 means "the rest of the window" to the node editor, so it is resolved
        // here rather than guessed at.
        const ImVec2 canvasSize( width, ImGui::GetContentRegionAvail().y );

        ed::SetCurrentEditor( m_Context );
        ed::Begin( "##animGraph", canvasSize );

        int activeIndex = -1;
        if ( anim.GraphEvaluator && anim.GraphEvaluator->CurrentState() )
        {
            const std::string& activeName = anim.GraphEvaluator->CurrentState()->Name;
            for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
                if ( graph.States[i].Name == activeName )
                {
                    activeIndex = i;
                    break;
                }
        }

        // --- State nodes ---
        for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
        {
            auto&                     s       = graph.States[i];
            const Graph::PlannedNode& planned = m_Canvas.Plan.Nodes[static_cast<size_t>( i )];

            Graph::PushNodePosition( planned );

            ed::BeginNode( ed::NodeId( Graph::Raw( planned.Id ) ) );

            const bool   isEntry  = ( graph.Entry == s.Name );
            const bool   isActive = ( i == activeIndex );
            const ImVec4 titleCol = isActive  ? ImVec4( 1.0f, 0.65f, 0.2f, 1.0f )
                                    : isEntry ? ImVec4( 0.4f, 0.85f, 1.0f, 1.0f )
                                              : ImVec4( 0.9f, 0.9f, 0.95f, 1.0f );
            ImGui::TextColored( titleCol, "%s%s", s.Name.c_str(), isEntry ? "  (entry)" : "" );
            ImGui::TextDisabled( "%s", s.Clip.empty() ? "<no clip>" : s.Clip.c_str() );

            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( Graph::Raw( m_Canvas.StateInPins[static_cast<size_t>( i )] ) ),
                          ed::PinKind::Input );
            ImGui::TextUnformatted( "-> in" );
            ed::EndPin();
            ImGui::EndGroup();

            ImGui::SameLine( 0.0f, 30.0f );

            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( Graph::Raw( m_Canvas.StateOutPins[static_cast<size_t>( i )] ) ),
                          ed::PinKind::Output );
            ImGui::TextUnformatted( "out ->" );
            ed::EndPin();
            ImGui::EndGroup();

            ed::EndNode();

            // Persist user drags back into the model, by ELEMENT and not by index. The canvas used to be
            // asked for `NodeId( i )` after a deletion had shifted every later state down one, so it
            // handed back the neighbour's position and this line wrote it into the wrong state.
            // A drag is a layout change and nothing else: it must not mark the asset edited, or merely
            // looking at a graph would ask to be saved. Parity with what stood here.
            (void)Graph::PullNodePosition( planned, s.X, s.Y );
        }

        // --- Transition links ---
        for ( const auto& link : m_Canvas.Plan.Links )
        {
            ed::Link( ed::LinkId( Graph::Raw( link.Id ) ), ed::PinId( link.FromPin ), ed::PinId( link.ToPin ),
                      ImVec4( 0.6f, 0.8f, 0.6f, 1.0f ), 2.0f );
        }

        // --- Create transitions by dragging out -> in ---
        if ( ed::BeginCreate() )
        {
            ed::PinId a, b;
            if ( ed::QueryNewLink( &a, &b ) && a && b )
            {
                const uint64_t pa  = a.Get();
                const uint64_t pb  = b.Get();
                int            src = Graph::StateOfOutPin( m_Canvas, pa );
                int            dst = Graph::StateOfInPin( m_Canvas, pb );
                if ( src < 0 && dst < 0 ) // dragged the other direction
                {
                    src = Graph::StateOfOutPin( m_Canvas, pb );
                    dst = Graph::StateOfInPin( m_Canvas, pa );
                }

                const bool valid = src >= 0 && dst >= 0 && src != dst;
                const bool dup =
                     valid &&
                     std::any_of( graph.States[src].Transitions.begin(), graph.States[src].Transitions.end(),
                                  [&]( const G::Transition& tr ) { return tr.To == graph.States[dst].Name; } );
                if ( !valid || dup )
                    ed::RejectNewItem( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), 2.0f );
                else if ( ed::AcceptNewItem( ImVec4( 0.5f, 1.0f, 0.5f, 1.0f ), 3.0f ) )
                {
                    G::Transition tr;
                    tr.To = graph.States[dst].Name;
                    graph.States[src].Transitions.push_back( tr );
                    dirty = true;
                }
            }
        }
        ed::EndCreate();

        // --- Deletion ---
        if ( ed::BeginDelete() )
        {
            ed::LinkId dl;
            while ( ed::QueryDeletedLink( &dl ) )
            {
                if ( ed::AcceptDeletedItem() )
                {
                    // WHICH TRANSITION, ASKED OF THE PLAN. `( id - kLink ) / 4096` was an index pair
                    // decoded out of an id, so deleting one transition renumbered the rest and the next
                    // deletion took a different one than the user had selected.
                    const Graph::TransitionRef ref =
                         Graph::TransitionOfLink( m_Canvas, static_cast<Graph::ElementId>( dl.Get() ) );
                    if ( ref.Valid() )
                    {
                        auto& transitions = graph.States[ref.State].Transitions;
                        transitions.erase( transitions.begin() + ref.Index );
                        dirty = true;
                    }
                }
            }
            ed::NodeId dn;
            while ( ed::QueryDeletedNode( &dn ) )
            {
                if ( ed::AcceptDeletedItem() )
                {
                    const int ni = Graph::StateOfNode( m_Canvas, static_cast<Graph::ElementId>( dn.Get() ) );
                    if ( ni >= 0 )
                    {
                        const std::string gone = graph.States[ni].Name;
                        graph.States.erase( graph.States.begin() + ni );
                        for ( auto& st : graph.States )
                            std::erase_if( st.Transitions,
                                           [&]( const G::Transition& tr ) { return tr.To == gone; } );
                        if ( graph.Entry == gone )
                            graph.Entry = graph.States.empty() ? "" : graph.States.front().Name;
                        dirty = true;
                    }
                }
            }
        }
        ed::EndDelete();

        ed::End();
        ed::SetCurrentEditor( nullptr );

        if ( m_FrameAll.Tick( canvasSize.x, canvasSize.y ) )
            Graph::FrameAll( m_Context );

        if ( dirty )
            MarkEdited();
    }

    void AnimGraphPanel::DrawSidePanel( ECS::AnimationComponent& anim, const std::vector<std::string>& clipNames )
    {
        auto& graph = *anim.Graph;
        auto* eval  = anim.GraphEvaluator.get();
        bool  dirty = false;

        ImGui::BeginChild( "##agSide", ImVec2( 290.0f, 0.0f ), true );

        // ---- Parameters (with live value controls) ----
        ImGui::TextUnformatted( "Parameters" );
        for ( int i = 0; i < static_cast<int>( graph.Parameters.size() ); ++i )
        {
            auto& p = graph.Parameters[i];
            ImGui::PushID( i );
            ImGui::SetNextItemWidth( 90 );
            dirty |= Utils::ImGuiUtilities::Property( "##pn", p.Name );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 55 );
            dirty |= ImGui::Combo( "##pt", &p.Type, kTypeNames, 3 );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 70 );
            float live = eval ? eval->GetFloat( p.Name ) : p.Default;

            // THE CONTROL NOW MATCHES THE DECLARED TYPE, all three of them. An `Int` parameter was drawn
            // as a float drag and pushed through SetFloat, which the evaluator accepted because its
            // setters did no checking at all; now it would be refused, and the honest fix is the control
            // the type always deserved. The live value is still stored as one float — that is the
            // evaluator's uniform store, not a type.
            const auto declaredType = static_cast<G::ParamType>( p.Type );
            if ( declaredType == G::ParamType::Bool )
            {
                bool b = live != 0.0f;
                if ( ImGui::Checkbox( "##pv", &b ) && eval != nullptr )
                {
                    ReportParamWrite( eval->SetBool( p.Name, b ) );
                }
            }
            else if ( declaredType == G::ParamType::Int )
            {
                auto whole = static_cast<int>( std::lround( live ) );
                if ( ImGui::DragInt( "##pv", &whole, 1.0f ) && eval != nullptr )
                {
                    ReportParamWrite( eval->SetInt( p.Name, whole ) );
                }
            }
            else if ( ImGui::DragFloat( "##pv", &live, 0.05f ) && eval != nullptr )
            {
                ReportParamWrite( eval->SetFloat( p.Name, live ) );
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x" ) )
            {
                graph.Parameters.erase( graph.Parameters.begin() + i );
                dirty = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if ( ImGui::SmallButton( "+ Parameter" ) )
        {
            graph.Parameters.push_back( { "Param", static_cast<int>( G::ParamType::Float ), 0.0f } );
            dirty = true;
        }

        ImGui::Separator();

        // ---- Selected node / link editor ----
        ed::SetCurrentEditor( m_Context );
        ed::NodeId selNode;
        ed::LinkId selLink;
        const bool haveNode = ed::GetSelectedNodes( &selNode, 1 ) > 0;
        const bool haveLink = ed::GetSelectedLinks( &selLink, 1 ) > 0;
        ed::SetCurrentEditor( nullptr );

        if ( haveNode )
        {
            // WHICH STATE, ASKED OF THE PLAN. `selNode.Get() - 1` decoded an index out of an id, so after
            // a deletion the inspector edited the state next to the one that was highlighted.
            const int si = Graph::StateOfNode( m_Canvas, static_cast<Graph::ElementId>( selNode.Get() ) );
            if ( si >= 0 )
            {
                auto& s = graph.States[si];
                ImGui::TextUnformatted( "State" );
                const std::string oldName = s.Name;
                if ( Utils::ImGuiUtilities::Property( "Name", s.Name ) )
                {
                    // A RENAME THAT COLLIDES IS A STATE THAT DISAPPEARS: every reference in this graph
                    // resolves by name and takes the first match. Renaming to an occupied name is
                    // therefore answered with a free one rather than accepted silently.
                    s.Name = Graph::MakeUniqueStateName( graph, s.Name, si );
                    if ( graph.Entry == oldName )
                        graph.Entry = s.Name;
                    for ( auto& st : graph.States )
                        for ( auto& tr : st.Transitions )
                            if ( tr.To == oldName )
                                tr.To = s.Name;
                    dirty = true;
                }
                const char* preview = s.Clip.empty() ? "Select Clip" : s.Clip.c_str();
                if ( ImGui::BeginCombo( "Clip", preview ) )
                {
                    for ( const auto& name : clipNames )
                        if ( ImGui::Selectable( name.c_str(), s.Clip == name ) )
                        {
                            s.Clip = name;
                            dirty  = true;
                        }
                    ImGui::EndCombo();
                }
                dirty |= ImGui::Checkbox( "Loop", &s.Loop );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 80 );
                dirty |= ImGui::DragFloat( "Speed", &s.Speed, 0.01f, 0.0f, 5.0f );
                if ( graph.Entry != s.Name && ImGui::SmallButton( "Set as Entry" ) )
                {
                    graph.Entry = s.Name;
                    dirty       = true;
                }
            }
        }
        else if ( haveLink )
        {
            // Same question, same answer: the plan knows which transition this link is.
            const Graph::TransitionRef ref =
                 Graph::TransitionOfLink( m_Canvas, static_cast<Graph::ElementId>( selLink.Get() ) );
            if ( ref.Valid() )
            {
                const int si = ref.State;
                auto&     tr = graph.States[si].Transitions[ref.Index];
                ImGui::Text( "Transition %s -> %s", graph.States[si].Name.c_str(), tr.To.c_str() );
                ImGui::SetNextItemWidth( 90 );
                dirty |= ImGui::DragFloat( "Blend", &tr.Blend, 0.01f, 0.0f, 2.0f );
                dirty |= ImGui::Checkbox( "Exit time", &tr.HasExitTime );
                if ( tr.HasExitTime )
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth( 80 );
                    dirty |= ImGui::DragFloat( "##exit", &tr.ExitTime, 0.01f, 0.0f, 1.0f );
                }
                ImGui::TextDisabled( "Conditions (all must hold)" );
                for ( int ci = 0; ci < static_cast<int>( tr.Conditions.size() ); ++ci )
                {
                    auto& c = tr.Conditions[ci];
                    ImGui::PushID( ci );
                    ImGui::SetNextItemWidth( 85 );
                    const char* cp = c.Parameter.empty() ? "param" : c.Parameter.c_str();
                    if ( ImGui::BeginCombo( "##cp", cp ) )
                    {
                        for ( const auto& p : graph.Parameters )
                            if ( ImGui::Selectable( p.Name.c_str(), c.Parameter == p.Name ) )
                            {
                                c.Parameter = p.Name;
                                dirty       = true;
                            }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth( 60 );
                    dirty |= ImGui::Combo( "##co", &c.Op, kOpNames, IM_ARRAYSIZE( kOpNames ) );
                    const auto op = static_cast<G::CompareOp>( c.Op );
                    if ( op != G::CompareOp::IsTrue && op != G::CompareOp::IsFalse )
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth( 55 );
                        dirty |= ImGui::DragFloat( "##cv", &c.Value, 0.05f );
                    }
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( "x" ) )
                    {
                        tr.Conditions.erase( tr.Conditions.begin() + ci );
                        dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if ( ImGui::SmallButton( "+ Condition" ) )
                {
                    tr.Conditions.push_back( {} );
                    dirty = true;
                }
            }
        }
        else
        {
            ImGui::TextDisabled( "Select a state or transition to edit it.\nDrag out -> in to connect." );
        }

        ImGui::EndChild();

        if ( dirty )
            MarkEdited();
    }

} // namespace Desert::Editor
