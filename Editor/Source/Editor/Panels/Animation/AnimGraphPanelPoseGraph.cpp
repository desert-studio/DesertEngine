// The AnimGraph tab of AnimGraphPanel: the pose graph's canvas, its side panel and its document actions.
// A translation unit of its own so the state-machine half and the pose half of the panel are read apart;
// the deciding lives in PoseGraphEdit (no ImGui), and every edit here is one call into it.
#include "AnimGraphPanel.hpp"

#include <Editor/Core/GraphCanvas/GraphCanvasView.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/Animation/PoseGraphEdit.hpp>

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/LayeredBlendPerBone.hpp>
#include <Engine/Animation/Graph/LinkedAnimLayer.hpp>
#include <Engine/ECS/Components.hpp>

#include <imgui-node-editor/imgui_node_editor.h>

#include <format>

namespace ed = ax::NodeEditor;

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace G     = Animation::Graph;

    namespace
    {
        const char* kCurveBlendNames[] = { "Override",        "Do Not Override", "Normalize By Weight",
                                           "Blend By Weight", "Use Base Pose",   "Use Max Value",
                                           "Use Min Value" };

        bool IsOutputMachineNode( const G::AnimGraph& graph, const G::PoseNode& node )
        {
            const G::StateMachine* machine = G::OutputMachine( graph );
            return machine != nullptr && node.Machine && &*node.Machine == machine;
        }
    } // namespace

    bool AnimGraphPanel::Report( const Common::BoolResultStr& result )
    {
        if ( result.IsSuccess() )
            return true;
        m_Status        = result.GetError();
        m_StatusIsError = true;
        return false;
    }

    void AnimGraphPanel::AddPoseNode( G::PoseNodeKind kind, const std::optional<glm::vec2>& where )
    {
        AnimGraphEditTransaction::Scope transaction( m_GraphEdit, GraphOwner() );
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( anim == nullptr || !anim->Graph )
        {
            m_Status        = "no graph to add a node to";
            m_StatusIsError = true;
            return;
        }
        G::AnimGraph&                  graph = *anim->Graph;
        const std::vector<std::string> clips = ResolveClipNames( *anim );
        const std::pair<float, float>  cell  = Graph::NextPoseNodePosition( graph.Nodes );
        const glm::vec2                at    = where.value_or( glm::vec2( cell.first, cell.second ) );
        // This panel edits the graph's own AnimGraph (GraphScope::Host); a layer graph's canvas is where a
        // Linked Input Pose may be added, and the unit refuses it here with the reason.
        const auto added = Graph::AddPoseNode( graph, graph.Nodes, kind, G::GraphScope::Host, at.x, at.y,
                                               clips.empty() ? std::string() : clips.front() );
        if ( !added.IsSuccess() )
        {
            m_Status        = added.GetError();
            m_StatusIsError = true;
            return;
        }
        m_SelectedPoseNode  = added.GetValue();
        m_PoseSelectPending = true;
        MarkEdited();
    }

    void AnimGraphPanel::WirePose( const std::string& from, const std::string& to, int pin )
    {
        AnimGraphEditTransaction::Scope transaction( m_GraphEdit, GraphOwner() );
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( anim == nullptr || !anim->Graph )
            return;
        G::AnimGraph& graph = *anim->Graph;
        if ( to.empty() ? Report( Graph::ConnectOutput( graph.Nodes, graph.OutputPose, from ) )
                        : Report( Graph::ConnectPose( graph.Nodes, from, to, pin ) ) )
            MarkEdited();
    }

    void AnimGraphPanel::RemovePoseNode( const std::string& name )
    {
        AnimGraphEditTransaction::Scope transaction( m_GraphEdit, GraphOwner() );
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( anim == nullptr || !anim->Graph )
            return;
        if ( Report( Graph::RemovePoseNode( anim->Graph->Nodes, anim->Graph->OutputPose, name ) ) )
        {
            if ( m_SelectedPoseNode == name )
                m_SelectedPoseNode.clear();
            MarkEdited();
        }
    }

    void AnimGraphPanel::AppendPoseActions( ECS::AnimationComponent& anim, std::vector<DocumentAction>& actions )
    {
        // THE CONTEXT MENU'S ENTRIES AND THE CANVAS DRAG, AS DOCUMENT ACTIONS: this machine refuses synthetic
        // input, so an edit only a mouse can make is an edit no client and no check can make.
        actions.push_back( { "Show AnimGraph", [this] { m_EditingMachine = false; } } );
        actions.push_back( { "Show State Machine", [this] { m_EditingMachine = true; } } );
        for ( const G::PoseNodeKind kind : Graph::AddableKinds( G::GraphScope::Host ) )
            actions.push_back( { std::format( "Add {}", Graph::PoseNodeTitle( kind ) ),
                                 [this, kind] { AddPoseNode( kind, std::nullopt ); } } );

        const G::AnimGraph& graph = *anim.Graph;
        for ( const G::PoseNode& node : graph.Nodes )
        {
            const std::string name = node.Name;
            actions.push_back( { std::format( "Select Node '{}'", name ), [this, name]
                                 {
                                     m_EditingMachine    = false;
                                     m_SelectedPoseNode  = name;
                                     m_PoseSelectPending = true;
                                 } } );
            actions.push_back(
                 { std::format( "Delete Node '{}'", name ), [this, name] { RemovePoseNode( name ); } } );
            if ( graph.OutputPose != name )
                actions.push_back( { std::format( "Wire '{}' Into Output Pose", name ),
                                     [this, name] { WirePose( name, std::string(), 0 ); } } );
            // Only the wires the unit would accept: an unwired pin of another node, no loop closed.
            for ( const G::PoseNode& into : graph.Nodes )
            {
                for ( int pin = 0; pin < static_cast<int>( into.PoseInputs.size() ); ++pin )
                {
                    if ( !into.PoseInputs[static_cast<size_t>( pin )].empty() )
                        continue;
                    std::vector<G::PoseNode> trial = graph.Nodes;
                    if ( !Graph::ConnectPose( trial, name, into.Name, pin ).IsSuccess() )
                        continue;
                    const std::string to = into.Name;
                    actions.push_back(
                         { std::format( "Wire '{}' Into '{}' {}", name, to, Graph::PosePinLabel( into, pin ) ),
                           [this, name, to, pin] { WirePose( name, to, pin ); } } );
                }
            }
        }
    }

    void AnimGraphPanel::DrawPoseCanvas( ECS::AnimationComponent& anim, float width, float height )
    {
        G::AnimGraph& graph = *anim.Graph;
        bool          dirty = false;

        m_PoseCanvas = Graph::PlanPoseCanvas( graph.Nodes, graph.OutputPose, m_PoseIds );

        const ImVec2 canvasSize( width, height );
        ed::SetCurrentEditor( m_PoseContext );
        ed::Begin( "##poseGraph", canvasSize );

        for ( size_t i = 0; i < graph.Nodes.size(); ++i )
        {
            G::PoseNode&              node    = graph.Nodes[i];
            const Graph::PlannedNode& planned = m_PoseCanvas.Plan.Nodes[i];
            const auto                kind    = static_cast<G::PoseNodeKind>( node.Kind );
            Graph::PushNodePosition( planned );

            ed::BeginNode( ed::NodeId( Graph::Raw( planned.Id ) ) );
            ImGui::TextColored( ImVec4( 0.55f, 0.8f, 1.0f, 1.0f ), "%s", Graph::PoseNodeTitle( kind ) );
            ImGui::TextUnformatted( node.Name.c_str() );
            if ( node.Sequence )
                ImGui::TextDisabled( "%s",
                                     node.Sequence->Clip.empty() ? "<no clip>" : node.Sequence->Clip.c_str() );
            if ( node.LinkedLayer )
                ImGui::TextDisabled( "%s.%s", node.LinkedLayer->Interface.c_str(),
                                     node.LinkedLayer->Layer.c_str() );
            if ( node.Machine )
                ImGui::TextDisabled( "%zu state(s)", node.Machine->States.size() );

            ImGui::BeginGroup();
            for ( size_t p = 0; p < m_PoseCanvas.InPins[i].size(); ++p )
            {
                ed::BeginPin( ed::PinId( Graph::Raw( m_PoseCanvas.InPins[i][p] ) ), ed::PinKind::Input );
                ImGui::Text( ICON_MDI_ARROW_RIGHT " %s",
                             Graph::PosePinLabel( node, static_cast<int>( p ) ).c_str() );
                ed::EndPin();
            }
            ImGui::EndGroup();
            ImGui::SameLine( 0.0f, 24.0f );
            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( Graph::Raw( m_PoseCanvas.OutPins[i] ) ), ed::PinKind::Output );
            ImGui::TextUnformatted( "Pose " ICON_MDI_ARROW_RIGHT );
            ed::EndPin();
            ImGui::EndGroup();
            ed::EndNode();

            dirty |= Graph::PullNodePosition( planned, node.X, node.Y );
        }

        // Output Pose, the sink: its place is the canvas's, not the file's.
        {
            const Graph::PlannedNode& sink = m_PoseCanvas.Plan.Nodes.back();
            Graph::PushNodePosition( sink );
            ed::BeginNode( ed::NodeId( Graph::Raw( sink.Id ) ) );
            ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.4f, 1.0f ), "Output Pose" );
            ed::BeginPin( ed::PinId( Graph::Raw( m_PoseCanvas.SinkPin ) ), ed::PinKind::Input );
            ImGui::TextUnformatted( ICON_MDI_ARROW_RIGHT " Result" );
            ed::EndPin();
            ed::EndNode();
        }

        for ( const Graph::PlannedLink& link : m_PoseCanvas.Plan.Links )
            ed::Link( ed::LinkId( Graph::Raw( link.Id ) ), ed::PinId( link.FromPin ), ed::PinId( link.ToPin ),
                      ImVec4( 0.85f, 0.85f, 0.9f, 1.0f ), 2.0f );

        // --- Wire by dragging Pose -> a Pose pin: the unit decides, on a copy, before the drop ---
        if ( ed::BeginCreate() )
        {
            ed::PinId a, b;
            if ( ed::QueryNewLink( &a, &b ) && a && b )
            {
                Graph::PosePinRef out = Graph::PinOf( m_PoseCanvas, a.Get() );
                Graph::PosePinRef in  = Graph::PinOf( m_PoseCanvas, b.Get() );
                if ( in.Output && !out.Output )
                    std::swap( out, in );
                std::string refusal;
                if ( !out.Valid() || !in.Valid() || !out.Output || in.Output )
                    refusal = "a wire runs from a node's Pose output into a Pose input";
                std::vector<G::PoseNode> trial      = graph.Nodes;
                std::string              outputPose = graph.OutputPose;
                if ( refusal.empty() )
                {
                    const std::string& from = graph.Nodes[static_cast<size_t>( out.Node )].Name;
                    const auto         wired =
                         in.Node == Graph::kOutputSink
                                      ? Graph::ConnectOutput( trial, outputPose, from )
                                      : Graph::ConnectPose( trial, from, graph.Nodes[static_cast<size_t>( in.Node )].Name,
                                                            in.Pin );
                    if ( !wired.IsSuccess() )
                        refusal = wired.GetError();
                }
                if ( !refusal.empty() )
                {
                    ed::RejectNewItem( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), 2.0f );
                    ed::Suspend();
                    ImGui::SetTooltip( "%s", refusal.c_str() );
                    ed::Resume();
                }
                else if ( ed::AcceptNewItem( ImVec4( 0.5f, 1.0f, 0.5f, 1.0f ), 3.0f ) )
                {
                    graph.Nodes      = std::move( trial );
                    graph.OutputPose = std::move( outputPose );
                    dirty            = true;
                }
            }
        }
        ed::EndCreate();

        // --- Deletion: a wire unwires its pin, a node takes its wires with it, the sink stays ---
        if ( ed::BeginDelete() )
        {
            ed::LinkId deletedLink;
            while ( ed::QueryDeletedLink( &deletedLink ) )
            {
                const Graph::PoseWireRef wire =
                     Graph::WireOf( m_PoseCanvas, static_cast<Graph::ElementId>( deletedLink.Get() ) );
                if ( wire.From < 0 || !ed::AcceptDeletedItem() )
                    continue;
                if ( wire.To == Graph::kOutputSink )
                    graph.OutputPose.clear();
                else
                    Report( Graph::DisconnectPose(
                         graph.Nodes, m_PoseCanvas.Plan.Nodes[static_cast<size_t>( wire.To )].Key, wire.Pin ) );
                dirty = true;
            }
            ed::NodeId deletedNode;
            while ( ed::QueryDeletedNode( &deletedNode ) )
            {
                const int index =
                     Graph::PoseNodeOf( m_PoseCanvas, static_cast<Graph::ElementId>( deletedNode.Get() ) );
                if ( index < 0 )
                {
                    ed::RejectDeletedItem(); // the Output Pose sink is the graph's, not a node
                    continue;
                }
                if ( !ed::AcceptDeletedItem() )
                    continue;
                // BY NAME, from the plan: an earlier deletion this frame has already moved the indices.
                const std::string gone = m_PoseCanvas.Plan.Nodes[static_cast<size_t>( index )].Key;
                if ( Report( Graph::RemovePoseNode( graph.Nodes, graph.OutputPose, gone ) ) )
                {
                    if ( m_SelectedPoseNode == gone )
                        m_SelectedPoseNode.clear();
                    dirty = true;
                }
            }
        }
        ed::EndDelete();

        // --- Selection follows the canvas; a document action's pick is pushed into it ---
        if ( m_PoseSelectPending )
        {
            m_PoseSelectPending = false;
            if ( const Graph::ElementId id = m_PoseIds.Lookup( Graph::ElementKind::Node, m_SelectedPoseNode );
                 id != Graph::ElementId::Invalid )
                ed::SelectNode( ed::NodeId( Graph::Raw( id ) ) );
        }
        else if ( ed::HasSelectionChanged() )
        {
            ed::NodeId selected;
            const int  count = ed::GetSelectedNodes( &selected, 1 );
            const int  index =
                 count > 0 ? Graph::PoseNodeOf( m_PoseCanvas, static_cast<Graph::ElementId>( selected.Get() ) )
                            : -1;
            m_SelectedPoseNode =
                 index >= 0 ? m_PoseCanvas.Plan.Nodes[static_cast<size_t>( index )].Key : std::string();
        }

        // --- Double-click a state machine: into it, as UE opens its graph ---
        if ( const ed::NodeId clicked = ed::GetDoubleClickedNode() )
        {
            const int index = Graph::PoseNodeOf( m_PoseCanvas, static_cast<Graph::ElementId>( clicked.Get() ) );
            const G::PoseNode* node =
                 index >= 0 ? G::FindNode( graph, m_PoseCanvas.Plan.Nodes[static_cast<size_t>( index )].Key )
                            : nullptr;
            if ( node != nullptr && node->Machine )
            {
                if ( IsOutputMachineNode( graph, *node ) )
                    m_EditingMachine = true;
                else
                {
                    m_Status = "the State Machine tab edits the machine at the bottom of Output Pose's base "
                               "chain; wire this one there to open it";
                    m_StatusIsError = true;
                }
            }
        }

        // --- The context menu: add a node where the menu was opened ---
        const ImVec2 mouse = ImGui::GetMousePos(); // canvas space while the editor is not suspended
        ed::Suspend();
        if ( ed::ShowBackgroundContextMenu() )
        {
            m_PoseMenuAt = glm::vec2( mouse.x, mouse.y );
            ImGui::OpenPopup( "##addPoseNode" );
        }
        if ( ImGui::BeginPopup( "##addPoseNode" ) )
        {
            ImGui::TextDisabled( "Add node" );
            ImGui::Separator();
            for ( const G::PoseNodeKind kind : Graph::AddableKinds( G::GraphScope::Host ) )
                if ( ImGui::MenuItem( Graph::PoseNodeTitle( kind ) ) )
                    AddPoseNode( kind, m_PoseMenuAt );
            ImGui::EndPopup();
        }
        ed::Resume();

        ed::End();
        ed::SetCurrentEditor( nullptr );

        if ( m_PoseFrameAll.Tick( canvasSize.x, canvasSize.y ) )
            Graph::FrameAll( m_PoseContext );

        if ( dirty )
            MarkEdited();
    }

    bool AnimGraphPanel::DrawPinBinding( G::AnimGraph& graph, G::PoseNode& node, const std::string& pin )
    {
        const std::string bound   = Graph::BoundParameter( node, pin );
        const std::string preview = bound.empty() ? std::string( "unbound (1.0)" ) : bound;
        bool              changed = false;
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( std::format( "##bind{}", pin ).c_str(), preview.c_str() ) )
        {
            if ( ImGui::Selectable( "unbound (1.0)", bound.empty() ) )
                changed = Report( Graph::BindParameterPin( graph, node, pin, std::string() ) );
            for ( const G::Parameter& parameter : graph.Parameters )
                if ( ImGui::Selectable( parameter.Name.c_str(), parameter.Name == bound ) )
                    changed = Report( Graph::BindParameterPin( graph, node, pin, parameter.Name ) );
            ImGui::EndCombo();
        }
        return changed;
    }

    void AnimGraphPanel::DrawPoseSidePanel( ECS::AnimationComponent&        anim,
                                            const std::vector<std::string>& clipNames, float height )
    {
        G::AnimGraph& graph = *anim.Graph;
        bool          dirty = false;

        ImGui::BeginChild( "##poseSide", ImVec2( 290.0f, height ), true );
        ImGui::Text( "Output Pose: %s", graph.OutputPose.empty() ? "<nothing wired>" : graph.OutputPose.c_str() );
        ImGui::Separator();

        int index = -1;
        for ( size_t i = 0; i < graph.Nodes.size(); ++i )
            if ( graph.Nodes[i].Name == m_SelectedPoseNode )
                index = static_cast<int>( i );
        if ( index < 0 )
        {
            ImGui::TextWrapped( "Select a node. Right-click the canvas to add one; drag from a Pose output "
                                "to a Pose input to wire it." );
            ImGui::EndChild();
            return;
        }

        G::PoseNode& node = graph.Nodes[static_cast<size_t>( index )];
        ImGui::TextColored( ImVec4( 0.55f, 0.8f, 1.0f, 1.0f ), "%s",
                            Graph::PoseNodeTitle( static_cast<G::PoseNodeKind>( node.Kind ) ) );
        std::string typed = node.Name;
        if ( Utils::ImGuiUtilities::InputText( typed, "##poseName" ) && !typed.empty() )
        {
            m_SelectedPoseNode = Graph::RenamePoseNode( graph.Nodes, graph.OutputPose, index, typed );
            dirty              = true;
        }
        if ( graph.OutputPose != node.Name && ImGui::Button( "Wire Into Output Pose" ) )
            dirty |= Report( Graph::ConnectOutput( graph.Nodes, graph.OutputPose, node.Name ) );

        if ( node.Sequence )
        {
            ImGui::TextUnformatted( "Sequence" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo( "##clip",
                                    node.Sequence->Clip.empty() ? "<no clip>" : node.Sequence->Clip.c_str() ) )
            {
                for ( const std::string& clip : clipNames )
                    if ( ImGui::Selectable( clip.c_str(), clip == node.Sequence->Clip ) )
                    {
                        node.Sequence->Clip = clip;
                        dirty               = true;
                    }
                ImGui::EndCombo();
            }
            dirty |= ImGui::Checkbox( "Loop", &node.Sequence->Loop );
        }

        if ( node.LayeredBlend )
        {
            G::LayeredBlendPerBoneNode& blend  = *node.LayeredBlend;
            int                         layers = static_cast<int>( blend.Layers.size() );
            ImGui::SetNextItemWidth( 120.0f );
            if ( ImGui::InputInt( "Layers", &layers ) && layers >= 0 )
            {
                Graph::SetLayerCount( node, static_cast<size_t>( layers ) );
                dirty = true;
            }
            for ( size_t layer = 0; layer < blend.Layers.size(); ++layer )
            {
                ImGui::PushID( static_cast<int>( layer ) );
                ImGui::Separator();
                ImGui::Text( "Layer %zu", layer );
                ImGui::TextUnformatted( "Weight" );
                dirty |= DrawPinBinding( graph, node, G::LayerWeightPin( layer ) );
                auto& filters = blend.Layers[layer].Filters;
                for ( size_t f = 0; f < filters.size(); ++f )
                {
                    ImGui::PushID( static_cast<int>( f ) );
                    ImGui::SetNextItemWidth( 150.0f );
                    dirty |= Utils::ImGuiUtilities::InputText( filters[f].BoneName, "##bone" );
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth( 60.0f );
                    dirty |= ImGui::DragInt( "##depth", &filters[f].BlendDepth, 0.1f, 0, 64 );
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                    {
                        filters.erase( filters.begin() + static_cast<std::ptrdiff_t>( f ) );
                        dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if ( ImGui::SmallButton( "+ Branch Filter" ) )
                {
                    filters.push_back( {} );
                    dirty = true;
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::SetNextItemWidth( -1.0f );
            dirty |= ImGui::Combo( "##curveBlend", &blend.CurveBlend, kCurveBlendNames,
                                   IM_ARRAYSIZE( kCurveBlendNames ) );
            dirty |= ImGui::Checkbox( "Mesh Space Rotation Blend", &blend.MeshSpaceRotationBlend );
            dirty |= ImGui::Checkbox( "Mesh Space Scale Blend", &blend.MeshSpaceScaleBlend );
            dirty |= ImGui::Checkbox( "Root Motion From Root Bone Weight", &blend.BlendRootMotionBasedOnRootBone );
        }

        if ( static_cast<G::PoseNodeKind>( node.Kind ) == G::PoseNodeKind::ApplyAdditive )
        {
            ImGui::TextUnformatted( "Alpha" );
            dirty |= DrawPinBinding( graph, node, std::string( G::kApplyAdditiveAlphaPin ) );
        }

        if ( node.LinkedLayer )
        {
            ImGui::TextUnformatted( "Interface" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo( "##iface", node.LinkedLayer->Interface.empty()
                                                    ? "<none>"
                                                    : node.LinkedLayer->Interface.c_str() ) )
            {
                if ( graph.Layers )
                    for ( const G::AnimLayerInterface& declared : graph.Layers->Interfaces )
                        if ( ImGui::Selectable( declared.Name.c_str(),
                                                declared.Name == node.LinkedLayer->Interface ) )
                        {
                            node.LinkedLayer->Interface = declared.Name;
                            node.LinkedLayer->Layer =
                                 declared.Layers.empty() ? std::string() : declared.Layers.front();
                            dirty = true;
                        }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted( "Layer" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo(
                      "##layer", node.LinkedLayer->Layer.empty() ? "<none>" : node.LinkedLayer->Layer.c_str() ) )
            {
                if ( const G::AnimLayerInterface* declared =
                          G::FindLayerInterface( graph, node.LinkedLayer->Interface ) )
                    for ( const std::string& layer : declared->Layers )
                        if ( ImGui::Selectable( layer.c_str(), layer == node.LinkedLayer->Layer ) )
                        {
                            node.LinkedLayer->Layer = layer;
                            dirty                   = true;
                        }
                ImGui::EndCombo();
            }
        }

        if ( node.Machine )
        {
            ImGui::Text( "%zu state(s), entry '%s'", node.Machine->States.size(), node.Machine->Entry.c_str() );
            if ( IsOutputMachineNode( graph, node ) && ImGui::Button( "Open State Machine" ) )
                m_EditingMachine = true;
        }

        ImGui::Separator();
        std::string remove;
        if ( ImGui::Button( ICON_MDI_DELETE "  Delete Node" ) )
            remove = node.Name;
        ImGui::EndChild();

        if ( !remove.empty() )
        {
            RemovePoseNode( remove ); // marks the edit itself
            return;
        }
        if ( dirty )
            MarkEdited();
    }
} // namespace Desert::Editor
