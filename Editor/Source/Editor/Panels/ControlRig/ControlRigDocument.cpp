#include "ControlRigDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Core/GraphCanvas/GraphCanvasView.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>

#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Rig/ControlShape.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ControlRigAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>

namespace ed = ax::NodeEditor;

namespace Desert::Editor
{
    namespace
    {
        using namespace Assets::Serialization;

        constexpr std::array<const char*, 3> kSpaceKinds = { "Component", "Bone", "Control" };

        uintptr_t IdOf( std::string_view a, std::string_view b = {}, std::string_view c = {} )
        {
            // imgui-node-editor ids are opaque non-zero integers; the name triple IS the identity, so the
            // id survives undo, reorder and reload instead of being an index that moves under the canvas.
            const size_t h = std::hash<std::string>{}( std::format( "{}\x1f{}\x1f{}", a, b, c ) );
            return static_cast<uintptr_t>( h | 1u );
        }

        bool EditTransform( const char* label, RigTransformData& t, bool& done )
        {
            ImGui::PushID( label );
            ImGui::TextDisabled( "%s", label );
            bool changed = ImGui::DragFloat3( "T", &t.Translation.x, 0.1f );
            done |= ImGui::IsItemDeactivatedAfterEdit();
            glm::vec3 euler = glm::degrees( glm::eulerAngles( t.Rotation ) );
            if ( ImGui::DragFloat3( "R", &euler.x, 0.5f ) )
            {
                t.Rotation = glm::quat( glm::radians( euler ) );
                changed    = true;
            }
            done |= ImGui::IsItemDeactivatedAfterEdit();
            changed |= ImGui::DragFloat3( "S", &t.Scale.x, 0.01f, 0.001f, 1000.0f );
            done |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
            return changed;
        }
    } // namespace

    ControlRigDocument::ControlRigDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( subject, assets, "Control Rig" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::ControlRig ) ) ),
           m_Assets( assets )
    {
        const Assets::AssetMetadata* metadata = assets ? assets->FindMetadataByHandle( subject ) : nullptr;
        if ( metadata == nullptr )
            m_OpenError = "the rig is not registered with the asset manager";
        else if ( auto opened = ControlRigDocumentModel::Open( metadata->Filepath, CommandHistory::Get() );
                  !opened )
            m_OpenError = opened.GetError();
        else
            m_Model = opened.ExtractValue();

        if ( auto library = Animation::ControlShapeLibrary::BuiltIn() )
            m_ShapeNames = library.GetValue().Names();

        ed::Config config;
        config.SettingsFile = nullptr; // node positions are view state; the .derig carries none
        m_Canvas            = ed::CreateEditor( &config );
        LoadSkeleton();
    }

    ControlRigDocument::~ControlRigDocument()
    {
        if ( m_Canvas )
            ed::DestroyEditor( m_Canvas );
    }

    bool ControlRigDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState ControlRigDocument::GetDiskState() const
    {
        if ( !m_Model )
            return DiskState::Untracked;
        return m_Model->IsDirty() ? DiskState::Dirty : DiskState::Clean;
    }

    bool ControlRigDocument::SaveDocument()
    {
        if ( !m_Model )
            return false;
        const auto saved = m_Model->Save();
        Report( saved );
        if ( !saved )
            return false;
        // The scenes that name this rig read the ASSET; reload it so they rebuild from what was written.
        if ( m_Assets )
            if ( auto asset = m_Assets->FindByPath<Assets::ControlRigAsset>( m_Model->GetPath() ) )
                if ( const auto reloaded = asset->LoadFromFile(); !reloaded )
                    LOG_ERROR( "[ControlRig] '{}' was saved and would not reload: {}", m_Model->GetPath().string(),
                               reloaded.GetError() );
        return true;
    }

    void ControlRigDocument::Report( const Common::BoolResultStr& result )
    {
        m_LastRefusal = result ? std::string() : result.GetError();
    }

    void ControlRigDocument::LoadSkeleton()
    {
        m_Skeleton.reset();
        m_SkeletonError.clear();
        if ( !m_Model || !m_Assets )
            return;
        const auto guid = Common::Content::AssetGuidFromText( m_Model->GetData().TargetSkeleton.Guid );
        if ( !guid )
        {
            m_SkeletonError = guid.GetError();
            return;
        }
        if ( auto loaded = SkeletonSlots::LoadSkeleton( *m_Assets, guid.GetValue() ) )
            m_Skeleton = loaded.ExtractValue();
        else
            m_SkeletonError = loaded.GetError();
    }

    std::vector<std::string> ControlRigDocument::BoneNames() const
    {
        std::vector<std::string> names;
        if ( m_Skeleton )
            if ( const Animation::Skeleton* skeleton = m_Skeleton->GetSkeleton() )
                for ( const auto& bone : skeleton->GetBones() )
                    names.push_back( bone.Name );
        return names;
    }

    void ControlRigDocument::OnUIRender()
    {
        // No ImGui::Begin: the document loop wraps this in Begin/End.
        if ( !m_Model )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "This rig could not be opened: %s",
                                m_OpenError.c_str() );
            return;
        }
        DrawToolbar();

        const float statusHeight = ImGui::GetFrameHeightWithSpacing() * ( m_LastRefusal.empty() ? 1.0f : 2.0f );
        const float height       = std::max( ImGui::GetContentRegionAvail().y - statusHeight, 100.0f );
        const float width        = ImGui::GetContentRegionAvail().x;
        const float side         = std::clamp( width * 0.2f, 180.0f, 320.0f );

        ImGui::BeginChild( "##elements", ImVec2( side, height ), true );
        DrawElements();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild( "##canvas",
                           ImVec2( width - side * 2.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f, height ),
                           true );
        DrawCanvas();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild( "##inspector", ImVec2( side, height ), true );
        DrawInspector();
        ImGui::EndChild();
        DrawStatus();
    }

    void ControlRigDocument::DrawToolbar()
    {
        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            SaveDocument();
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_FIT_TO_PAGE_OUTLINE " Frame" ) )
            Graph::FrameAll( m_Canvas );
        ImGui::SameLine();
        ImGui::TextUnformatted( "Event:" );
        for ( const Animation::RigEvent event : { Animation::RigEvent::Construction, Animation::RigEvent::Forwards,
                                                  Animation::RigEvent::Backwards } )
        {
            ImGui::SameLine();
            if ( ImGui::RadioButton( std::string( Animation::ToString( event ) ).c_str(),
                                     m_Model->GetEvent() == event ) &&
                 m_Model->GetEvent() != event )
            {
                m_Model->SetEvent( event );
                m_SelectedNode.clear();
                m_LiteralDraft.reset();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "  Skeleton: %s", m_Model->GetData().TargetSkeleton.Path.empty()
                                                    ? "(none)"
                                                    : m_Model->GetData().TargetSkeleton.Path.c_str() );
    }

    void ControlRigDocument::DrawElements()
    {
        const auto& data = m_Model->GetData();
        if ( ImGui::TreeNodeEx( "Bones", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            if ( !m_SkeletonError.empty() )
                ImGui::TextDisabled( "%s", m_SkeletonError.c_str() );
            for ( const auto& bone : BoneNames() )
                ImGui::BulletText( "%s", bone.c_str() );
            ImGui::TreePop();
        }
        if ( ImGui::TreeNodeEx( "Controls", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            for ( const auto& control : data.Controls )
            {
                const bool selected = control.Name == m_SelectedControl;
                if ( ImGui::Selectable( std::format( ICON_MDI_RHOMBUS " {}", control.Name ).c_str(), selected ) )
                {
                    m_SelectedControl = control.Name;
                    m_SelectedNode.clear();
                }
                if ( ImGui::BeginPopupContextItem() )
                {
                    if ( ImGui::MenuItem( "Delete" ) )
                        Report( m_Model->RemoveControl( control.Name ) );
                    if ( ImGui::MenuItem( "Add Get Control node" ) )
                        if ( auto added = m_Model->AddNode( Animation::RigNodeKind::GetControl, control.Name,
                                                            NextFreePosition() );
                             !added )
                            m_LastRefusal = added.GetError();
                    if ( ImGui::MenuItem( "Add Set Control node" ) )
                        if ( auto added = m_Model->AddNode( Animation::RigNodeKind::SetControl, control.Name,
                                                            NextFreePosition() );
                             !added )
                            m_LastRefusal = added.GetError();
                    ImGui::EndPopup();
                }
            }
            ImGui::TreePop();
        }
        ImGui::Separator();
        char buffer[128] = {};
        std::snprintf( buffer, sizeof( buffer ), "%s", m_NewControlName.c_str() );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::InputTextWithHint( "##newControl", "new control name", buffer, sizeof( buffer ) ) )
            m_NewControlName = buffer;
        if ( ImGui::Button( "+ Control" ) )
        {
            const auto added = m_Model->AddControl( m_NewControlName );
            Report( added );
            if ( added )
            {
                m_SelectedControl = m_NewControlName;
                m_NewControlName.clear();
            }
        }
    }

    RigNodePositionData ControlRigDocument::NextFreePosition() const
    {
        RigNodePositionData at{ 40.0f, 40.0f };
        if ( const auto* graph = m_Model->GetGraph() )
            for ( const auto& node : graph->Nodes )
                at.Y = std::max( at.Y, node.Position.Y + 180.0f );
        return at;
    }

    void ControlRigDocument::DrawCanvas()
    {
        const auto& data = m_Model->GetData();
        ed::SetCurrentEditor( m_Canvas );
        ed::Begin( "##rigGraph" );
        m_Pins.clear();

        // A COPY: an edit below (wire, cut, move, remove) installs a new value and frees the model's vector.
        const auto*                         graph = m_Model->GetGraph();
        const std::vector<RigGraphNodeData> nodes = graph ? graph->Nodes : std::vector<RigGraphNodeData>{};
        // Positions are the FILE's (CRIG 4): taken whenever the model moved (open, undo, redo, event switch),
        // left to the canvas between, so a drag in progress is not snapped back.
        const bool place = m_PlacedRevision != m_Model->GetRevision();
        m_PlacedRevision = m_Model->GetRevision();
        for ( const auto& node : nodes )
        {
            const auto kind = Animation::RigNodeKindFromText( node.Kind );
            if ( !kind )
                continue;
            const auto&      desc = Animation::DescribeRigNode( *kind );
            const ed::NodeId id( IdOf( node.Name ) );
            if ( place )
                ed::SetNodePosition( id, ImVec2( node.Position.X, node.Position.Y ) );
            ed::BeginNode( id );
            ImGui::TextUnformatted( node.Kind.c_str() );
            if ( !node.Target.empty() )
                ImGui::TextDisabled( "%s", node.Target.c_str() );
            for ( const auto& pin : desc.Inputs )
            {
                const uintptr_t pinId = IdOf( node.Name, "in", pin.Name );
                m_Pins[pinId]         = PinRef{ node.Name, std::string( pin.Name ), true };
                ed::BeginPin( ed::PinId( pinId ), ed::PinKind::Input );
                ImGui::Text( "> %.*s", int( pin.Name.size() ), pin.Name.data() );
                ed::EndPin();
            }
            for ( const auto& pin : desc.Outputs )
            {
                const uintptr_t pinId = IdOf( node.Name, "out", pin.Name );
                m_Pins[pinId]         = PinRef{ node.Name, std::string( pin.Name ), false };
                ed::BeginPin( ed::PinId( pinId ), ed::PinKind::Output );
                ImGui::Text( "%.*s >", int( pin.Name.size() ), pin.Name.data() );
                ed::EndPin();
            }
            ed::EndNode();
        }
        for ( const auto& node : nodes )
            for ( const auto& input : node.Inputs )
                if ( input.Link )
                    ed::Link( ed::LinkId( IdOf( node.Name, "link", input.Pin ) ),
                              ed::PinId( IdOf( input.Link->Node, "out", input.Link->Pin ) ),
                              ed::PinId( IdOf( node.Name, "in", input.Pin ) ) );

        // A drag ends on release: each node the canvas moved off its authored position is one MoveNode
        // record (undoable, dirties the document), like UE's graph model committing on drop.
        if ( !place && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            for ( const auto& node : nodes )
            {
                const ImVec2 at = ed::GetNodePosition( ed::NodeId( IdOf( node.Name ) ) );
                if ( std::abs( at.x - node.Position.X ) > 0.5f || std::abs( at.y - node.Position.Y ) > 0.5f )
                    Report( m_Model->MoveNode( node.Name, RigNodePositionData{ at.x, at.y } ) );
            }

        if ( ed::BeginCreate() )
        {
            ed::PinId a;
            ed::PinId b;
            if ( ed::QueryNewLink( &a, &b ) && a && b )
            {
                auto pa = m_Pins.find( a.Get() );
                auto pb = m_Pins.find( b.Get() );
                if ( pa != m_Pins.end() && pb != m_Pins.end() && pa->second.Input != pb->second.Input )
                {
                    const PinRef& out = pa->second.Input ? pb->second : pa->second;
                    const PinRef& in  = pa->second.Input ? pa->second : pb->second;
                    if ( ed::AcceptNewItem() )
                        Report( m_Model->Connect( out.Node, out.Pin, in.Node, in.Pin ) );
                }
                else
                    ed::RejectNewItem( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), 2.0f );
            }
        }
        ed::EndCreate();

        if ( ed::BeginDelete() )
        {
            ed::LinkId link;
            while ( ed::QueryDeletedLink( &link ) )
            {
                for ( const auto& node : nodes )
                    for ( const auto& input : node.Inputs )
                        if ( input.Link && IdOf( node.Name, "link", input.Pin ) == link.Get() &&
                             ed::AcceptDeletedItem() )
                        {
                            Report( m_Model->Disconnect( node.Name, input.Pin ) );
                            break;
                        }
            }
            ed::NodeId node;
            while ( ed::QueryDeletedNode( &node ) )
                for ( const auto& candidate : nodes )
                    if ( IdOf( candidate.Name ) == node.Get() && ed::AcceptDeletedItem() )
                    {
                        Report( m_Model->RemoveNode( candidate.Name ) );
                        break;
                    }
        }
        ed::EndDelete();

        if ( ed::HasSelectionChanged() )
        {
            ed::NodeId selected;
            if ( ed::GetSelectedNodes( &selected, 1 ) == 1 )
                for ( const auto& candidate : nodes )
                    if ( IdOf( candidate.Name ) == selected.Get() )
                    {
                        m_SelectedNode = candidate.Name;
                        m_SelectedControl.clear();
                    }
        }

        const ImVec2 mouseOnCanvas = ImGui::GetMousePos(); // canvas space while the editor is not suspended
        ed::Suspend();
        if ( ed::ShowBackgroundContextMenu() )
        {
            m_DropPoint = glm::vec2( mouseOnCanvas.x, mouseOnCanvas.y );
            ImGui::OpenPopup( "##addRigNode" );
        }
        if ( ImGui::BeginPopup( "##addRigNode" ) )
        {
            const std::string control =
                 !m_SelectedControl.empty()
                      ? m_SelectedControl
                      : ( data.Controls.empty() ? std::string() : data.Controls.front().Name );
            const auto bones = BoneNames();
            for ( const auto& desc : Animation::RigNodeDescriptors() )
            {
                if ( !ImGui::MenuItem( std::string( desc.Name ).c_str() ) )
                    continue;
                std::string target;
                if ( desc.Target == Animation::RigNodeTargetKind::Control )
                    target = control;
                else if ( desc.Target == Animation::RigNodeTargetKind::Bone )
                    target = bones.empty() ? std::string() : bones.front();
                if ( auto added = m_Model->AddNode( desc.Kind, target,
                                                    RigNodePositionData{ m_DropPoint.x, m_DropPoint.y } );
                     added )
                {
                    m_SelectedNode = added.GetValue();
                    m_SelectedControl.clear();
                    m_LastRefusal.clear();
                }
                else
                    m_LastRefusal = added.GetError();
            }
            ImGui::EndPopup();
        }
        ed::Resume();
        ed::End();
        ed::SetCurrentEditor( nullptr );
    }

    void ControlRigDocument::DrawInspector()
    {
        if ( !m_SelectedControl.empty() )
            DrawControlInspector( m_SelectedControl );
        else if ( !m_SelectedNode.empty() )
            DrawNodeInspector( m_SelectedNode );
        else
            ImGui::TextDisabled( "Select a control or a node." );
    }

    void ControlRigDocument::DrawControlInspector( const std::string& name )
    {
        const auto& data  = m_Model->GetData();
        const auto  found = std::ranges::find( data.Controls, name, &ControlElementData::Name );
        if ( found == data.Controls.end() )
        {
            m_SelectedControl.clear();
            return;
        }
        if ( !m_Draft || m_DraftOf != name || m_DraftRevision != m_Model->GetRevision() )
        {
            m_Draft         = *found;
            m_DraftOf       = name;
            m_DraftRevision = m_Model->GetRevision();
        }
        ControlElementData& c       = *m_Draft;
        bool                commit  = false; // a discrete widget: write now
        bool                release = false; // a held widget let go: write the drag as one record

        ImGui::Text( "Control \"%s\"", name.c_str() );
        char buffer[128] = {};
        std::snprintf( buffer, sizeof( buffer ), "%s", c.Name.c_str() );
        if ( ImGui::InputText( "Name", buffer, sizeof( buffer ) ) )
            c.Name = buffer;
        release |= ImGui::IsItemDeactivatedAfterEdit();

        if ( ImGui::BeginCombo( "Shape", c.ShapeName.empty() ? "(none)" : c.ShapeName.c_str() ) )
        {
            for ( const auto& shape : m_ShapeNames )
                if ( ImGui::Selectable( shape.c_str(), shape == c.ShapeName ) )
                {
                    c.ShapeName = shape;
                    commit      = true;
                }
            ImGui::EndCombo();
        }
        glm::vec3 colour = c.Color.value_or( glm::vec3( 1.0f, 0.85f, 0.1f ) );
        if ( ImGui::ColorEdit3( "Colour", &colour.x ) )
            c.Color = colour;
        release |= ImGui::IsItemDeactivatedAfterEdit();
        float size = c.ShapeTransform ? c.ShapeTransform->Scale.x : 1.0f;
        if ( ImGui::DragFloat( "Size", &size, 0.01f, 0.01f, 100.0f ) )
        {
            if ( !c.ShapeTransform )
                c.ShapeTransform.emplace();
            c.ShapeTransform->Scale = glm::vec3( size );
        }
        release |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Separator();
        ImGui::TextDisabled( "%s", "Offset" );
        EditTransform( "Offset", c.Offset, release );
        ImGui::Separator();
        ImGui::TextDisabled( "%s", "Initial value" );
        EditTransform( "Pose", c.Pose, release );

        ImGui::Separator();
        ImGui::TextDisabled( "%s", "Parent" );
        const auto bones = BoneNames();
        for ( size_t i = 0; i < c.Parents.size(); ++i )
        {
            auto& space = c.Parents[i];
            ImGui::PushID( static_cast<int>( i ) );
            if ( ImGui::BeginCombo( "Kind", space.Kind.c_str() ) )
            {
                for ( const char* kind : kSpaceKinds )
                    if ( ImGui::Selectable( kind, space.Kind == kind ) )
                    {
                        space.Kind   = kind;
                        space.Target = space.Kind == "Bone" && !bones.empty() ? bones.front() : std::string();
                        commit       = true;
                    }
                ImGui::EndCombo();
            }
            if ( space.Kind != "Component" && ImGui::BeginCombo( "Target", space.Target.c_str() ) )
            {
                if ( space.Kind == "Bone" )
                {
                    for ( const auto& bone : bones )
                        if ( ImGui::Selectable( bone.c_str(), bone == space.Target ) )
                        {
                            space.Target = bone;
                            commit       = true;
                        }
                }
                else
                {
                    for ( const auto& other : data.Controls )
                        if ( other.Name != name &&
                             ImGui::Selectable( other.Name.c_str(), other.Name == space.Target ) )
                        {
                            space.Target = other.Name;
                            commit       = true;
                        }
                }
                ImGui::EndCombo();
            }
            ImGui::DragFloat( "Weight", &space.Weight, 0.01f, 0.0f, 1.0f );
            release |= ImGui::IsItemDeactivatedAfterEdit();
            if ( ImGui::SmallButton( "Remove parent" ) )
            {
                c.Parents.erase( c.Parents.begin() + static_cast<std::ptrdiff_t>( i ) );
                commit = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if ( ImGui::SmallButton( "+ Parent" ) )
        {
            c.Parents.push_back( ControlSpaceData{} );
            commit = true;
        }

        ImGui::Separator();
        ImGui::TextDisabled( "%s", "Limits (rotations in degrees)" );
        for ( size_t channel = 0; channel < 9; ++channel )
        {
            const std::string spelled(
                 Animation::ToString( static_cast<Animation::ControlLimitChannel>( channel ) ) );
            auto limit = std::ranges::find( c.Limits, spelled, &ControlLimitData::Channel );
            bool on    = limit != c.Limits.end();
            ImGui::PushID( spelled.c_str() );
            if ( ImGui::Checkbox( spelled.c_str(), &on ) )
            {
                if ( on )
                {
                    // A fresh range: a full turn for rotation, a metre either side for translation, 0..10 for
                    // scale.
                    const float extent = channel >= 6 ? 10.0f : ( channel >= 3 ? 180.0f : 100.0f );
                    c.Limits.push_back( ControlLimitData{ spelled, channel >= 6 ? 0.0f : -extent, extent } );
                }
                else
                    c.Limits.erase( limit );
                commit = true;
                limit  = std::ranges::find( c.Limits, spelled, &ControlLimitData::Channel );
            }
            if ( limit != c.Limits.end() )
            {
                ImGui::SameLine();
                float range[2] = { limit->Min, limit->Max };
                if ( ImGui::DragFloat2( "Min / Max", range, 0.1f ) )
                {
                    limit->Min = range[0];
                    limit->Max = range[1];
                }
                release |= ImGui::IsItemDeactivatedAfterEdit();
            }
            ImGui::PopID();
        }

        if ( commit || release )
        {
            Report( m_Model->SetControl( name, c ) );
            if ( c.Name != name && m_LastRefusal.empty() )
                m_SelectedControl = c.Name;
            m_Draft.reset();
        }

        ImGui::Separator();
        ImGui::TextDisabled( "%s", "Drives bone" );
        const auto  drive   = std::ranges::find( data.Drives, name, &ControlDriveData::Control );
        const char* current = drive == data.Drives.end() ? "(none)" : drive->Bone.c_str();
        if ( ImGui::BeginCombo( "Bone", current ) )
        {
            if ( ImGui::Selectable( "(none)", drive == data.Drives.end() ) )
                Report( m_Model->SetDrive( name, {} ) );
            for ( const auto& bone : bones )
                if ( ImGui::Selectable( bone.c_str(), drive != data.Drives.end() && drive->Bone == bone ) )
                    Report( m_Model->SetDrive( name, bone ) );
            ImGui::EndCombo();
        }
    }

    void ControlRigDocument::DrawNodeInspector( const std::string& name )
    {
        const auto& data  = m_Model->GetData();
        const auto* graph = m_Model->GetGraph();
        if ( !graph )
        {
            m_SelectedNode.clear();
            return;
        }
        const auto found = std::ranges::find( graph->Nodes, name, &RigGraphNodeData::Name );
        if ( found == graph->Nodes.end() )
        {
            m_SelectedNode.clear();
            return;
        }
        const RigGraphNodeData node = *found; // a copy: an edit below replaces the model's value
        const auto             kind = Animation::RigNodeKindFromText( node.Kind );
        if ( !kind )
            return;
        const auto& desc = Animation::DescribeRigNode( *kind );
        ImGui::Text( "%s  (%s)", node.Name.c_str(), node.Kind.c_str() );

        if ( desc.Target != Animation::RigNodeTargetKind::None &&
             ImGui::BeginCombo( "Target", node.Target.c_str() ) )
        {
            std::vector<std::string> choices;
            if ( desc.Target == Animation::RigNodeTargetKind::Bone )
                choices = BoneNames();
            else
                for ( const auto& control : data.Controls )
                    choices.push_back( control.Name );
            for ( const auto& choice : choices )
                if ( ImGui::Selectable( choice.c_str(), choice == node.Target ) )
                    Report( m_Model->SetNodeTarget( node.Name, choice ) );
            ImGui::EndCombo();
        }
        if ( desc.UsesSpace )
        {
            for ( const auto space : { Animation::RigControlSpace::Global, Animation::RigControlSpace::Local } )
            {
                const std::string label( Animation::ToString( space ) );
                if ( ImGui::RadioButton( label.c_str(), node.Space == label ) )
                    Report( m_Model->SetNodeSpace( node.Name, space ) );
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }

        ImGui::Separator();
        ImGui::TextDisabled( "%s", "Inputs" );
        for ( const auto& input : node.Inputs )
        {
            ImGui::PushID( input.Pin.c_str() );
            if ( input.Link )
            {
                ImGui::Text( "%s <- %s.%s", input.Pin.c_str(), input.Link->Node.c_str(), input.Link->Pin.c_str() );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Cut" ) )
                    Report( m_Model->Disconnect( node.Name, input.Pin ) );
            }
            else
            {
                // The held literal lives in a DRAFT until the widget is let go: the model's value is re-read
                // every frame, so without it a drag would restart from the stored value each frame.
                const bool        drafted = m_LiteralDraft && m_LiteralDraftNode == node.Name &&
                                     m_LiteralDraft->Pin == input.Pin;
                RigGraphInputData edited  = drafted ? *m_LiteralDraft : input;
                bool              done    = false;
                bool              held    = false;
                if ( edited.Float )
                {
                    ImGui::DragFloat( input.Pin.c_str(), &*edited.Float, 0.01f );
                    held = ImGui::IsItemActive();
                    done = ImGui::IsItemDeactivatedAfterEdit();
                }
                else if ( edited.Vec3 )
                {
                    ImGui::DragFloat3( input.Pin.c_str(), &edited.Vec3->x, 0.1f );
                    held = ImGui::IsItemActive();
                    done = ImGui::IsItemDeactivatedAfterEdit();
                }
                else if ( edited.Quat )
                {
                    glm::vec3 euler = glm::degrees( glm::eulerAngles( *edited.Quat ) );
                    if ( ImGui::DragFloat3( input.Pin.c_str(), &euler.x, 0.5f ) )
                        edited.Quat = glm::quat( glm::radians( euler ) );
                    held = ImGui::IsItemActive();
                    done = ImGui::IsItemDeactivatedAfterEdit();
                }
                else if ( edited.Transform )
                {
                    EditTransform( input.Pin.c_str(), *edited.Transform, done );
                    held = !done && !( edited == input );
                }
                if ( held && !done )
                {
                    m_LiteralDraft     = edited;
                    m_LiteralDraftNode = node.Name;
                }
                if ( done )
                {
                    m_LiteralDraft.reset();
                    if ( !( edited == input ) )
                        Report( m_Model->SetLiteral( node.Name, edited ) );
                }
            }
            ImGui::PopID();
        }
        if ( ImGui::Button( "Delete node" ) )
        {
            Report( m_Model->RemoveNode( node.Name ) );
            m_SelectedNode.clear();
        }
    }

    void ControlRigDocument::DrawStatus()
    {
        if ( m_StatusRevision != m_Model->GetRevision() )
        {
            m_StatusRevision   = m_Model->GetRevision();
            const auto&  data  = m_Model->GetData();
            const auto*  graph = m_Model->GetGraph();
            const size_t nodes = graph ? graph->Nodes.size() : 0;
            const auto   event = Animation::ToString( m_Model->GetEvent() );
            const auto   valid = m_Model->Validate();
            m_StatusError      = !valid;
            if ( !valid )
                m_Status = std::format( "Not savable: {}", valid.GetError() );
            else if ( m_Skeleton && m_Skeleton->GetSkeleton() )
            {
                Animation::ControlRigStage stage;
                const auto                 built = BuildControlRig( data, *m_Skeleton->GetSkeleton(), stage );
                m_StatusError                    = !built;
                m_Status = built ? std::format( "{} solve: {} node(s), {} control(s), binds to the "
                                                "skeleton",
                                                event, nodes, data.Controls.size() )
                                 : std::format( "Does not bind to its skeleton: {}", built.GetError() );
            }
            else
                m_Status = std::format( "{} solve: {} node(s), {} control(s); skeleton not loaded", event, nodes,
                                        data.Controls.size() );
        }
        Graph::DrawStatusLine( m_Status, m_StatusError );
        if ( !m_LastRefusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ), "%s", m_LastRefusal.c_str() );
    }

    SubjectEditorRegistry::PathOpenOutcome RequestControlRigDocument( Assets::AssetManager*        assets,
                                                                      const std::string&           path,
                                                                      const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension() != Assets::Serialization::kControlRigExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::ControlRigAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::ControlRigAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[ControlRig] '{}' could not be registered as a control rig — no editor was opened.",
                       path );
            return Outcome::Failed;
        }
        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[ControlRig] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
