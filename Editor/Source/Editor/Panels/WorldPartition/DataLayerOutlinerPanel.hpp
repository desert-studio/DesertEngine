#pragma once

// THE DATA LAYER OUTLINER (WP-DATA-LAYER-OUTLINER) - UE's SDataLayerBrowser / Data Layer Outliner
// (FDataLayerTreeItem, FDataLayerActorTreeItem), the pattern and not the letter, as its own panel; the World
// Partition panel's Data Layers section stays the summary (Loaded, Initially Loaded, New, Delete).
//
// One tree of the world's data layers, nested by Parent (WorldPartitionSerialized::DataLayers), each layer with
// its colour, its loaded members under it, and per row: Visible (this session, DataLayerEditor::SetVisible),
// Current (the layers new entities join, DataLayerEditor::SetCurrent) and the layer's type with its initial
// runtime state. A row's menu: Select Actors, Add Selected Actors, Remove Selected Actors, Make Current, New Child
// Data Layer, Move to Top Level. Entities dragged from the Outliner onto a layer join it (the whole selection when
// the dragged row is selected, as the Outliner's own drop); a layer dragged onto another nests under it. The
// selected layer's details - parent, colour, type, initial runtime state, initially visible - are edited below the
// tree through EditWorldPartition, one undo step each and refused by Rules::CheckDataLayers as the planner would
// refuse them.
//
// The panel also keeps the session state working while it is closed: every frame (OnPreUpdate) it brings the
// entities' data-layer hiding up to date (DataLayerEditor::UpdateEntityVisibility), and it installs the current
// layers as the editor's created-entities context (Commands::SetCreatedEntitiesContext) for its lifetime.

#include <Editor/Core/IPanel.hpp>

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    class DataLayerOutlinerPanel final : public IPanel
    {
    public:
        explicit DataLayerOutlinerPanel( std::shared_ptr<::Desert::Core::Scene> scene );
        ~DataLayerOutlinerPanel() override;

        DataLayerOutlinerPanel( const DataLayerOutlinerPanel& )            = delete;
        DataLayerOutlinerPanel& operator=( const DataLayerOutlinerPanel& ) = delete;

        void OnPreUpdate() override;
        void OnUIRender() override;
        void SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene ) override;

    private:
        // One layer's row, its members and its children's rows, recursively.
        void DrawLayer( const ::Desert::Core::WorldPartitionSerialized& partition, std::size_t index,
                        const std::vector<std::vector<std::size_t>>& children, const std::vector<bool>& visible );
        void DrawLayerMenu( const ::Desert::Core::WorldPartitionSerialized& partition, std::size_t index );
        void DrawMembers( const ::Desert::Core::DataLayerSerialized& layer, bool layerVisible );
        void DrawDetails( const ::Desert::Core::WorldPartitionSerialized& partition );
        // A new layer named by the name field: top-level, or nested under @p parent with the parent's type.
        void NewLayer( const ::Desert::Core::WorldPartitionSerialized& partition,
                       std::optional<std::size_t>                      parent );
        // @p layer nests under @p parent (nullopt: top level), unless that would put it under itself.
        void Reparent( const ::Desert::Core::WorldPartitionSerialized& partition, const std::string& layer,
                       std::optional<std::string> parent );
        // One edit of the world's block through EditWorldPartition; a refusal is the status line's.
        void Commit( std::string label, ::Desert::Core::WorldPartitionSerialized after );
        // The status line for a membership or selection verb's outcome.
        void Report( const std::string& verb, const std::string& layer, bool succeeded, const std::string& error,
                     std::size_t count );

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        std::optional<std::string>             m_SelectedLayer; ///< the layer whose details are drawn
        std::string                            m_Status;        ///< the last refusal or verb outcome
        std::array<char, 128>                  m_NewLayer{};    ///< the name field of New Data Layer
        // The colour picker's value while it is being dragged: committed as ONE edit when the mouse is released.
        std::string          m_ColorLayer;
        std::array<float, 3> m_ColorDraft{};
        bool                 m_ColorDirty = false;
    };
} // namespace Desert::Editor
