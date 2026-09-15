#include "AnimGraphPanel.hpp"
#include <Editor/Panels/PanelContext.hpp>

#include <Editor/Core/ImGuiUtilities.hpp>

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

        // Disjoint id ranges so nodes / pins / links never collide in the node editor.
        constexpr uint64_t kOutPin = 0x2000'0000ULL;
        constexpr uint64_t kInPin  = 0x4000'0000ULL;
        constexpr uint64_t kLink   = 0x8000'0000ULL;

        uint64_t NodeId( int i )
        {
            return static_cast<uint64_t>( i ) + 1;
        }
        uint64_t OutPinId( int i )
        {
            return kOutPin + static_cast<uint64_t>( i );
        }
        uint64_t InPinId( int i )
        {
            return kInPin + static_cast<uint64_t>( i );
        }
        uint64_t LinkId( int state, int transition )
        {
            return kLink + static_cast<uint64_t>( state ) * 4096ull + static_cast<uint64_t>( transition );
        }

        // pin -> state index; returns -1 if the pin isn't a state pin of the given kind.
        int OutPinState( uint64_t pin )
        {
            return ( pin >= kOutPin && pin < kInPin ) ? static_cast<int>( pin - kOutPin ) : -1;
        }
        int InPinState( uint64_t pin )
        {
            return ( pin >= kInPin && pin < kLink ) ? static_cast<int>( pin - kInPin ) : -1;
        }

        const char* kTypeNames[] = { "Bool", "Int", "Float" };
        const char* kOpNames[]   = { ">", "<", ">=", "<=", "==", "!=", "is true", "is false" };
    } // namespace

    AnimGraphPanel::AnimGraphPanel( const SubjectId& subject, const std::string& displayName,
                                    const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Animation::AnimationLibrary*            library )
         : ISubjectDocument( displayName, subject ), m_Scene( scene ), m_Library( library )
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
            ImGui::TextWrapped( "This entity has no AnimGraph yet." );
            if ( ImGui::Button( "Create AnimGraph" ) )
            {
                anim->Graph = std::make_shared<G::AnimGraph>();
                G::State idle;
                idle.Name = "Idle";
                anim->Graph->States.push_back( idle );
                anim->Graph->Entry = "Idle";
                anim->GraphRevision++;
                m_ApplyPositions = true;
            }
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
        if ( ImGui::Button( "+ State" ) )
        {
            G::State ns;
            ns.Name = "State_" + std::to_string( anim->Graph->States.size() );
            anim->Graph->States.push_back( ns );
            anim->GraphRevision++;
        }
        ImGui::SameLine();
        if ( const auto* cur = anim->GraphEvaluator ? anim->GraphEvaluator->CurrentState() : nullptr )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "| Active: %s", cur->Name.c_str() );
        }

        constexpr float kSideW = 300.0f;
        ImGui::BeginChild( "##agCanvas", ImVec2( -kSideW, 0.0f ) );
        DrawCanvas( *anim, clipNames );
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginGroup();
        DrawSidePanel( *anim, clipNames );
        ImGui::EndGroup();
    }

    void AnimGraphPanel::DrawCanvas( ECS::AnimationComponent& anim, const std::vector<std::string>& )
    {
        auto& graph = *anim.Graph;
        bool  dirty = false;

        ed::SetCurrentEditor( m_Context );
        ed::Begin( "##animGraph", ImVec2( 0.0f, 0.0f ) );

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
            auto& s = graph.States[i];

            if ( m_ApplyPositions )
                ed::SetNodePosition( ed::NodeId( NodeId( i ) ), ImVec2( s.X, s.Y ) );

            ed::BeginNode( ed::NodeId( NodeId( i ) ) );

            const bool   isEntry  = ( graph.Entry == s.Name );
            const bool   isActive = ( i == activeIndex );
            const ImVec4 titleCol = isActive  ? ImVec4( 1.0f, 0.65f, 0.2f, 1.0f )
                                    : isEntry ? ImVec4( 0.4f, 0.85f, 1.0f, 1.0f )
                                              : ImVec4( 0.9f, 0.9f, 0.95f, 1.0f );
            ImGui::TextColored( titleCol, "%s%s", s.Name.c_str(), isEntry ? "  (entry)" : "" );
            ImGui::TextDisabled( "%s", s.Clip.empty() ? "<no clip>" : s.Clip.c_str() );

            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( InPinId( i ) ), ed::PinKind::Input );
            ImGui::TextUnformatted( "-> in" );
            ed::EndPin();
            ImGui::EndGroup();

            ImGui::SameLine( 0.0f, 30.0f );

            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( OutPinId( i ) ), ed::PinKind::Output );
            ImGui::TextUnformatted( "out ->" );
            ed::EndPin();
            ImGui::EndGroup();

            ed::EndNode();

            // Persist user drags back into the model (so positions save with the scene).
            if ( !m_ApplyPositions )
            {
                const ImVec2 p = ed::GetNodePosition( ed::NodeId( NodeId( i ) ) );
                if ( p.x != s.X || p.y != s.Y )
                {
                    s.X = p.x;
                    s.Y = p.y;
                }
            }
        }
        if ( m_ApplyPositions )
            ed::NavigateToContent( 0.0f );
        m_ApplyPositions = false;

        // --- Transition links ---
        for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
        {
            const auto& s = graph.States[i];
            for ( int t = 0; t < static_cast<int>( s.Transitions.size() ); ++t )
            {
                const int target = [&]
                {
                    for ( int j = 0; j < static_cast<int>( graph.States.size() ); ++j )
                        if ( graph.States[j].Name == s.Transitions[t].To )
                            return j;
                    return -1;
                }();
                if ( target < 0 )
                    continue;
                ed::Link( ed::LinkId( LinkId( i, t ) ), ed::PinId( OutPinId( i ) ), ed::PinId( InPinId( target ) ),
                          ImVec4( 0.6f, 0.8f, 0.6f, 1.0f ), 2.0f );
            }
        }

        // --- Create transitions by dragging out -> in ---
        if ( ed::BeginCreate() )
        {
            ed::PinId a, b;
            if ( ed::QueryNewLink( &a, &b ) && a && b )
            {
                uint64_t pa = a.Get(), pb = b.Get();
                int      src = OutPinState( pa );
                int      dst = InPinState( pb );
                if ( src < 0 && dst < 0 ) // dragged the other direction
                {
                    src = OutPinState( pb );
                    dst = InPinState( pa );
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
                    const uint64_t id = dl.Get() - kLink;
                    const int      si = static_cast<int>( id / 4096ull );
                    const int      ti = static_cast<int>( id % 4096ull );
                    if ( si >= 0 && si < static_cast<int>( graph.States.size() ) && ti >= 0 &&
                         ti < static_cast<int>( graph.States[si].Transitions.size() ) )
                    {
                        graph.States[si].Transitions.erase( graph.States[si].Transitions.begin() + ti );
                        dirty = true;
                    }
                }
            }
            ed::NodeId dn;
            while ( ed::QueryDeletedNode( &dn ) )
            {
                if ( ed::AcceptDeletedItem() )
                {
                    const int ni = static_cast<int>( dn.Get() ) - 1;
                    if ( ni >= 0 && ni < static_cast<int>( graph.States.size() ) )
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

        if ( dirty )
            anim.GraphRevision++;
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
            const int si = static_cast<int>( selNode.Get() ) - 1;
            if ( si >= 0 && si < static_cast<int>( graph.States.size() ) )
            {
                auto& s = graph.States[si];
                ImGui::TextUnformatted( "State" );
                const std::string oldName = s.Name;
                if ( Utils::ImGuiUtilities::Property( "Name", s.Name ) )
                {
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
            const uint64_t id = selLink.Get() - kLink;
            const int      si = static_cast<int>( id / 4096ull );
            const int      ti = static_cast<int>( id % 4096ull );
            if ( si >= 0 && si < static_cast<int>( graph.States.size() ) && ti >= 0 &&
                 ti < static_cast<int>( graph.States[si].Transitions.size() ) )
            {
                auto& tr = graph.States[si].Transitions[ti];
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
            anim.GraphRevision++;
    }

} // namespace Desert::Editor
