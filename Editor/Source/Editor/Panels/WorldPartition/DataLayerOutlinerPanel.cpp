#include "DataLayerOutlinerPanel.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Commands/WorldPartitionEditCommand.hpp>
#include <Editor/Core/DataLayerEditorSubsystem.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/ECS/Components.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        namespace Rules = ::Desert::Core::Rules;

        // A layer dragged onto another nests under it (UE's Data Layer Outliner drag): the payload is its name.
        constexpr const char* kDataLayerPayload = "DATA_LAYER";

        const char* RuntimeStateName( const ::Desert::Core::DataLayerRuntimeState state )
        {
            switch ( state )
            {
                case ::Desert::Core::DataLayerRuntimeState::Unloaded:
                    return "Unloaded";
                case ::Desert::Core::DataLayerRuntimeState::Loaded:
                    return "Loaded";
                case ::Desert::Core::DataLayerRuntimeState::Activated:
                    return "Activated";
            }
            return "Unloaded";
        }

        std::string TypeText( const ::Desert::Core::DataLayerSerialized& layer )
        {
            if ( layer.Type == ::Desert::Core::DataLayerType::Editor )
                return "Editor";
            return std::format( "Runtime: {}", layer.InitialRuntimeState.has_value()
                                                    ? RuntimeStateName( *layer.InitialRuntimeState )
                                                    : "no initial state" );
        }
    } // namespace

    DataLayerOutlinerPanel::DataLayerOutlinerPanel( std::shared_ptr<::Desert::Core::Scene> scene )
         : IPanel( "Data Layers", false ), m_Scene( std::move( scene ) )
    {
        // UE's actor editor context: every entity the user creates joins the current data layers.
        Commands::SetCreatedEntitiesContext( &DataLayerEditor::ApplyCurrentLayers );
    }

    DataLayerOutlinerPanel::~DataLayerOutlinerPanel()
    {
        Commands::SetCreatedEntitiesContext( {} );
    }

    void DataLayerOutlinerPanel::SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;
        m_SelectedLayer.reset();
        m_Status.clear();
        m_ColorDirty = false;
    }

    void DataLayerOutlinerPanel::OnPreUpdate()
    {
        // Whether or not the window is open: a hidden layer's members stay undrawn, and a member that joins or
        // leaves a layer (an undo, a region load, Details) is drawn or hidden by the layer it is in now.
        if ( m_Scene )
            DataLayerEditor::UpdateEntityVisibility( *m_Scene );
    }

    void DataLayerOutlinerPanel::Commit( std::string label, ::Desert::Core::WorldPartitionSerialized after )
    {
        const auto edited = EditWorldPartition( m_Scene, std::move( label ), std::move( after ) );
        m_Status          = edited.IsSuccess() ? std::string() : edited.GetError();
    }

    void DataLayerOutlinerPanel::Report( const std::string& verb, const std::string& layer, const bool succeeded,
                                         const std::string& error, const std::size_t count )
    {
        m_Status = succeeded ? std::format( "{} '{}': {} entit{}.", verb, layer, count, count == 1 ? "y" : "ies" )
                             : error;
    }

    void DataLayerOutlinerPanel::NewLayer( const ::Desert::Core::WorldPartitionSerialized& partition,
                                           const std::optional<std::size_t>                parent )
    {
        const std::string                   name( m_NewLayer.data() );
        ::Desert::Core::DataLayerSerialized layer{ name, true };
        layer.DebugColor = Rules::SeededDataLayerColor( name );
        if ( parent.has_value() && partition.DataLayers.has_value() && *parent < partition.DataLayers->size() )
        {
            // A child starts as its parent's kind: a Runtime parent can hold only what it gates.
            const ::Desert::Core::DataLayerSerialized& above = ( *partition.DataLayers )[*parent];
            layer.Parent                                     = above.Name;
            layer.Type                                       = above.Type;
            layer.InitialRuntimeState                        = above.InitialRuntimeState;
        }
        ::Desert::Core::WorldPartitionSerialized after = partition;
        if ( !after.DataLayers.has_value() )
            after.DataLayers.emplace();
        after.DataLayers->push_back( std::move( layer ) );
        Commit( std::format( "New data layer '{}'", name ), std::move( after ) );
        if ( m_Status.empty() )
        {
            m_SelectedLayer = name;
            m_NewLayer.fill( '\0' );
        }
    }

    void DataLayerOutlinerPanel::Reparent( const ::Desert::Core::WorldPartitionSerialized& partition,
                                           const std::string& layer, std::optional<std::string> parent )
    {
        const auto index = Rules::DataLayerIndexOf( partition, layer );
        if ( !index.has_value() )
        {
            m_Status = std::format( "Data Layers: the world states no layer '{}'.", layer );
            return;
        }
        if ( parent.has_value() )
        {
            const auto above = Rules::DataLayerIndexOf( partition, *parent );
            if ( !above.has_value() || Rules::DataLayerIsWithin( partition, *above, *index ) )
            {
                m_Status = std::format( "Data Layers: '{}' cannot nest under '{}', which is itself or under it.",
                                        layer, *parent );
                return;
            }
        }
        ::Desert::Core::WorldPartitionSerialized after = partition;
        ( *after.DataLayers )[*index].Parent           = parent;
        Commit( parent.has_value() ? std::format( "Move data layer '{}' under '{}'", layer, *parent )
                                   : std::format( "Move data layer '{}' to the top level", layer ),
                std::move( after ) );
    }

    void DataLayerOutlinerPanel::DrawLayerMenu( const ::Desert::Core::WorldPartitionSerialized& partition,
                                                const std::size_t                               index )
    {
        const std::string name = ( *partition.DataLayers )[index].Name;
        if ( ImGui::MenuItem( "Select Actors" ) )
        {
            const auto selected = DataLayerEditor::SelectMembers( *m_Scene, name );
            Report( "Selected the actors of", name, selected.IsSuccess(), selected ? "" : selected.GetError(),
                    selected ? selected.GetValue() : 0 );
        }
        if ( ImGui::MenuItem( "Add Selected Actors" ) )
        {
            const auto added =
                 DataLayerEditor::AddEntities( *m_Scene, Core::SelectionManager::GetSelection(), name );
            Report( "Added to", name, added.IsSuccess(), added ? "" : added.GetError(),
                    added ? added.GetValue() : 0 );
        }
        if ( ImGui::MenuItem( "Remove Selected Actors" ) )
        {
            const auto removed =
                 DataLayerEditor::RemoveEntities( *m_Scene, Core::SelectionManager::GetSelection(), name );
            Report( "Removed from", name, removed.IsSuccess(), removed ? "" : removed.GetError(),
                    removed ? removed.GetValue() : 0 );
        }
        ImGui::Separator();
        const bool current = DataLayerEditor::IsCurrent( *m_Scene, name );
        if ( ImGui::MenuItem( current ? "Remove from Current" : "Make Current" ) )
            if ( const auto set = DataLayerEditor::SetCurrent( m_Scene, name, !current ); !set )
                m_Status = set.GetError();
        if ( ImGui::MenuItem( "New Child Data Layer", nullptr, false, m_NewLayer[0] != '\0' ) )
            NewLayer( partition, index );
        if ( ImGui::MenuItem( "Move to Top Level", nullptr, false,
                              ( *partition.DataLayers )[index].Parent.has_value() ) )
            Reparent( partition, name, std::nullopt );
    }

    void DataLayerOutlinerPanel::DrawMembers( const ::Desert::Core::DataLayerSerialized& layer,
                                              const bool                                 layerVisible )
    {
        const ImVec4 colour( layer.DebugColor[0], layer.DebugColor[1], layer.DebugColor[2], 1.0f );
        for ( const Common::UUID& uuid : DataLayerEditor::DirectMembers( *m_Scene, layer.Name ) )
        {
            const auto found = m_Scene->FindEntityByID( uuid );
            if ( !found )
                continue;
            const ECS::Entity& entity = found->get();
            const std::string  name   = entity.HasComponent<ECS::TagComponent>()
                                             ? entity.GetComponent<ECS::TagComponent>().Tag
                                             : std::format( "{}", static_cast<std::uint64_t>( uuid ) );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::ColorButton( "##member", colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                ImVec2( 6.0f, 6.0f ) );
            ImGui::SameLine();
            // A member of a hidden layer is loaded and listed, and not drawn: dimmed, as UE greys it.
            if ( !layerVisible )
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            const bool selected = Core::SelectionManager::IsSelected( uuid );
            if ( ImGui::Selectable( std::format( "{}##{}", name, static_cast<std::uint64_t>( uuid ) ).c_str(),
                                    selected, ImGuiSelectableFlags_SpanAllColumns ) )
            {
                if ( ImGui::GetIO().KeyCtrl )
                    Core::SelectionManager::Toggle( uuid );
                else
                    Core::SelectionManager::SetSelected( uuid );
            }
            if ( !layerVisible )
                ImGui::PopStyleColor();
        }
    }

    void DataLayerOutlinerPanel::DrawLayer( const ::Desert::Core::WorldPartitionSerialized& partition,
                                            const std::size_t                               index,
                                            const std::vector<std::vector<std::size_t>>&    children,
                                            const std::vector<bool>&                        visible )
    {
        const ::Desert::Core::DataLayerSerialized& layer = ( *partition.DataLayers )[index];
        ImGui::PushID( layer.Name.c_str() );
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::ColorButton(
             "##swatch", ImVec4( layer.DebugColor[0], layer.DebugColor[1], layer.DebugColor[2], 1.0f ),
             ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2( 10.0f, 10.0f ) );
        ImGui::SameLine();
        ImGuiTreeNodeFlags flags =
             ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth;
        if ( m_SelectedLayer == layer.Name )
            flags |= ImGuiTreeNodeFlags_Selected;
        const bool current = DataLayerEditor::IsCurrent( *m_Scene, layer.Name );
        const bool open =
             ImGui::TreeNodeEx( "##layer", flags, "%s%s", layer.Name.c_str(), current ? "  (current)" : "" );
        if ( ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() )
            m_SelectedLayer = layer.Name;
        if ( ImGui::BeginDragDropSource() )
        {
            ImGui::SetDragDropPayload( kDataLayerPayload, layer.Name.c_str(), layer.Name.size() + 1 );
            ImGui::TextUnformatted( layer.Name.c_str() );
            ImGui::EndDragDropSource();
        }
        if ( ImGui::BeginDragDropTarget() )
        {
            // Entities from the Outliner: the whole selection when the dragged row is in it (the Outliner's rule).
            if ( const ImGuiPayload* payload =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::EntityRelationship ) )
            {
                const Common::UUID              dragged  = *static_cast<const Common::UUID*>( payload->Data );
                const std::vector<Common::UUID> entities = Core::SelectionManager::IsSelected( dragged )
                                                                ? Core::SelectionManager::GetSelection()
                                                                : std::vector<Common::UUID>{ dragged };
                const auto added = DataLayerEditor::AddEntities( *m_Scene, entities, layer.Name );
                Report( "Added to", layer.Name, added.IsSuccess(), added ? "" : added.GetError(),
                        added ? added.GetValue() : 0 );
            }
            if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( kDataLayerPayload ) )
                Reparent( partition, std::string( static_cast<const char*>( payload->Data ) ), layer.Name );
            ImGui::EndDragDropTarget();
        }
        if ( ImGui::BeginPopupContextItem( "##layermenu" ) )
        {
            DrawLayerMenu( partition, index );
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        bool own = DataLayerEditor::IsVisible( *m_Scene, layer.Name );
        if ( ImGui::Checkbox( "##visible", &own ) )
            if ( const auto set = DataLayerEditor::SetVisible( m_Scene, layer.Name, own ); !set )
                m_Status = set.GetError();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Visible in the editor (this session). Hidden members stay loaded and listed." );
        ImGui::TableNextColumn();
        bool makeCurrent = current;
        if ( ImGui::Checkbox( "##current", &makeCurrent ) )
            if ( const auto set = DataLayerEditor::SetCurrent( m_Scene, layer.Name, makeCurrent ); !set )
                m_Status = set.GetError();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Current: every entity you add, place, paste or duplicate joins this layer." );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( TypeText( layer ).c_str() );

        if ( open )
        {
            const bool layerVisible = index < visible.size() && visible[index];
            for ( const std::size_t child : children[index] )
                DrawLayer( partition, child, children, visible );
            DrawMembers( layer, layerVisible );
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void DataLayerOutlinerPanel::DrawDetails( const ::Desert::Core::WorldPartitionSerialized& partition )
    {
        const auto index = m_SelectedLayer.has_value() ? Rules::DataLayerIndexOf( partition, *m_SelectedLayer )
                                                       : std::optional<std::size_t>();
        if ( !index.has_value() )
            return;
        const ::Desert::Core::DataLayerSerialized& layer = ( *partition.DataLayers )[*index];
        ImGui::Separator();
        ImGui::Text( "Data Layer '%s'", layer.Name.c_str() );

        if ( ImGui::Button( "Select Actors" ) )
        {
            const auto selected = DataLayerEditor::SelectMembers( *m_Scene, layer.Name );
            Report( "Selected the actors of", layer.Name, selected.IsSuccess(),
                    selected ? "" : selected.GetError(), selected ? selected.GetValue() : 0 );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Add Selected" ) )
        {
            const auto added =
                 DataLayerEditor::AddEntities( *m_Scene, Core::SelectionManager::GetSelection(), layer.Name );
            Report( "Added to", layer.Name, added.IsSuccess(), added ? "" : added.GetError(),
                    added ? added.GetValue() : 0 );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Remove Selected" ) )
        {
            const auto removed =
                 DataLayerEditor::RemoveEntities( *m_Scene, Core::SelectionManager::GetSelection(), layer.Name );
            Report( "Removed from", layer.Name, removed.IsSuccess(), removed ? "" : removed.GetError(),
                    removed ? removed.GetValue() : 0 );
        }

        // Parent: any layer that is not this one or under it.
        const std::string parentText = layer.Parent.value_or( "(top level)" );
        if ( ImGui::BeginCombo( "Parent", parentText.c_str() ) )
        {
            if ( ImGui::Selectable( "(top level)", !layer.Parent.has_value() ) && layer.Parent.has_value() )
                Reparent( partition, layer.Name, std::nullopt );
            for ( std::size_t other = 0; other < partition.DataLayers->size(); ++other )
            {
                if ( Rules::DataLayerIsWithin( partition, other, *index ) )
                    continue;
                const std::string& otherName = ( *partition.DataLayers )[other].Name;
                if ( ImGui::Selectable( otherName.c_str(), layer.Parent == otherName ) &&
                     layer.Parent != otherName )
                    Reparent( partition, layer.Name, otherName );
            }
            ImGui::EndCombo();
        }

        // Colour: dragged as a draft, written as ONE edit when the mouse is released.
        if ( m_ColorLayer != layer.Name || !m_ColorDirty )
        {
            m_ColorLayer = layer.Name;
            m_ColorDraft = layer.DebugColor;
        }
        if ( ImGui::ColorEdit3( "Color", m_ColorDraft.data(), ImGuiColorEditFlags_NoAlpha ) )
            m_ColorDirty = true;
        if ( m_ColorDirty && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
        {
            m_ColorDirty                                   = false;
            ::Desert::Core::WorldPartitionSerialized after = partition;
            ( *after.DataLayers )[*index].DebugColor       = m_ColorDraft;
            Commit( std::format( "Data layer '{}': color", layer.Name ), std::move( after ) );
        }

        // Type: Editor organises, Runtime streams (a Runtime layer starts Play Unloaded unless told otherwise).
        static constexpr const char* kTypes[] = { "Editor", "Runtime" };
        int                          type     = layer.Type == ::Desert::Core::DataLayerType::Runtime ? 1 : 0;
        if ( ImGui::Combo( "Type", &type, kTypes, 2 ) )
        {
            ::Desert::Core::WorldPartitionSerialized after   = partition;
            ::Desert::Core::DataLayerSerialized&     changed = ( *after.DataLayers )[*index];
            changed.Type =
                 type == 1 ? ::Desert::Core::DataLayerType::Runtime : ::Desert::Core::DataLayerType::Editor;
            changed.InitialRuntimeState = type == 1 ? std::optional<::Desert::Core::DataLayerRuntimeState>(
                                                           ::Desert::Core::DataLayerRuntimeState::Unloaded )
                                                    : std::nullopt;
            Commit( std::format( "Data layer '{}': {}", layer.Name, kTypes[type] ), std::move( after ) );
        }
        if ( layer.Type == ::Desert::Core::DataLayerType::Runtime && layer.InitialRuntimeState.has_value() )
        {
            static constexpr const char* kStates[] = { "Unloaded", "Loaded", "Activated" };
            int                          state     = static_cast<int>( *layer.InitialRuntimeState );
            if ( ImGui::Combo( "Initial Runtime State", &state, kStates, 3 ) )
            {
                ::Desert::Core::WorldPartitionSerialized after = partition;
                ( *after.DataLayers )[*index].InitialRuntimeState =
                     static_cast<::Desert::Core::DataLayerRuntimeState>( state );
                Commit( std::format( "Data layer '{}': Initial Runtime State {}", layer.Name, kStates[state] ),
                        std::move( after ) );
            }
        }
        bool initiallyVisible = layer.InitiallyVisible;
        if ( ImGui::Checkbox( "Initially Visible", &initiallyVisible ) )
        {
            ::Desert::Core::WorldPartitionSerialized after = partition;
            ( *after.DataLayers )[*index].InitiallyVisible = initiallyVisible;
            Commit( std::format( "Data layer '{}': Initially Visible {}", layer.Name, initiallyVisible ),
                    std::move( after ) );
        }
    }

    void DataLayerOutlinerPanel::OnUIRender()
    {
        if ( !m_Scene || !m_Scene->GetWorldPartition().has_value() )
        {
            ImGui::TextDisabled( "The scene is not a partitioned world (World Partition)." );
            return;
        }
        // A copy: an edit below replaces the block this frame is drawn from.
        const ::Desert::Core::WorldPartitionSerialized partition = *m_Scene->GetWorldPartition();

        ImGui::SetNextItemWidth( 200.0f );
        ImGui::InputTextWithHint( "##newdatalayer", "New data layer name", m_NewLayer.data(), m_NewLayer.size() );
        ImGui::SameLine();
        ImGui::BeginDisabled( m_NewLayer[0] == '\0' );
        if ( ImGui::Button( "New Data Layer" ) )
            NewLayer( partition, std::nullopt );
        ImGui::EndDisabled();

        if ( !partition.DataLayers.has_value() || partition.DataLayers->empty() )
        {
            ImGui::TextDisabled( "This world has no data layers." );
            if ( !m_Status.empty() )
                ImGui::TextWrapped( "%s", m_Status.c_str() );
            return;
        }

        const std::size_t                     count = partition.DataLayers->size();
        std::vector<std::vector<std::size_t>> children( count );
        std::vector<std::size_t>              roots;
        for ( std::size_t index = 0; index < count; ++index )
        {
            // A parent the world does not state, or a cycle CheckDataLayers refused, draws the layer at the top.
            const auto parent = Rules::DataLayerParentIndex( partition, index );
            if ( parent.has_value() && *parent != index && !Rules::DataLayerIsWithin( partition, *parent, index ) )
                children[*parent].push_back( index );
            else
                roots.push_back( index );
        }
        const std::vector<bool> visible = DataLayerEditor::EffectiveVisibility( *m_Scene );

        if ( ImGui::BeginTable( "##datalayeroutliner", 4,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_Resizable ) )
        {
            ImGui::TableSetupColumn( "Data Layer", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "Visible", ImGuiTableColumnFlags_WidthFixed );
            ImGui::TableSetupColumn( "Current", ImGuiTableColumnFlags_WidthFixed );
            ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthFixed );
            ImGui::TableHeadersRow();
            for ( const std::size_t root : roots )
                DrawLayer( partition, root, children, visible );
            ImGui::EndTable();
        }

        DrawDetails( partition );
        if ( !m_Status.empty() )
            ImGui::TextWrapped( "%s", m_Status.c_str() );
    }
} // namespace Desert::Editor
