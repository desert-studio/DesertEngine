#include "WorldPartitionDetails.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <format>
#include <functional>
#include <span>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        namespace Rules = ::Desert::Core::Rules;

        const ImVec4 kMissingColour( 0.95f, 0.35f, 0.30f, 1.0f );

        // The reason a composite is in no cell, as the World Partition panel's legend names it.
        const char* AlwaysLoadedReasonName( const Rules::AlwaysLoadedReason reason )
        {
            switch ( reason )
            {
                case Rules::AlwaysLoadedReason::None:
                    return "in a cell";
                case Rules::AlwaysLoadedReason::Author:
                    return "a member is marked always loaded (Is Spatially Loaded off)";
                case Rules::AlwaysLoadedReason::Component:
                    return "a member has a component that keeps it loaded everywhere";
                case Rules::AlwaysLoadedReason::NoFit:
                    return "its footprint is wider than the grid's top level";
                case Rules::AlwaysLoadedReason::NoGrid:
                    return "its grid is not one of the world's usable grids";
            }
            return "unknown reason";
        }

        // One undoable edit of the entity: a whole-entity snapshot around @p change (adding or removing a
        // component included), so Ctrl+Z restores it exactly.
        void EditEntity( const Common::UUID uuid, const std::function<void()>& change )
        {
            Commands::MutateEntityUndoable( uuid, change );
        }
    } // namespace

    void WorldPartitionDetails::Draw( ::Desert::ECS::Entity& entity, ::Desert::Core::Scene& scene,
                                      const std::shared_ptr<::Desert::Assets::AssetManager>& assets )
    {
        const std::optional<::Desert::Core::WorldPartitionSerialized>& stated = scene.GetWorldPartition();
        if ( !stated.has_value() || !entity.HasComponent<::Desert::ECS::UUIDComponent>() )
            return;
        if ( !ImGui::CollapsingHeader( "World Partition", ImGuiTreeNodeFlags_DefaultOpen ) )
            return;
        // A copy: an edit below replaces nothing of the block, but the entity's edits relocate component pools.
        const ::Desert::Core::WorldPartitionSerialized partition = *stated;
        const Common::UUID                   uuid      = entity.GetComponent<::Desert::ECS::UUIDComponent>().UUID;
        Utils::ImGuiUtilities::ResetPropertyRows();

        // ── Is Spatially Loaded (UE AActor::bIsSpatiallyLoaded): off = the AlwaysLoaded marker ──
        {
            const bool alwaysLoaded = entity.HasComponent<::Desert::ECS::AlwaysLoadedComponent>();
            bool       spatial      = !alwaysLoaded;
            Utils::ImGuiUtilities::BeginPropertyRow(
                 "Is Spatially Loaded",
                 "On: the entity streams with the cell its position puts it in. Off: it and "
                 "everything it is attached to stay loaded everywhere (AlwaysLoaded)." );
            const bool changed = ImGui::Checkbox( "##SpatiallyLoaded", &spatial );
            Utils::ImGuiUtilities::EndPropertyRow();
            if ( changed )
            {
                EditEntity( uuid,
                            [&entity, alwaysLoaded]
                            {
                                if ( alwaysLoaded )
                                    entity.RemoveComponent<::Desert::ECS::AlwaysLoadedComponent>();
                                else
                                    entity.AddComponent<::Desert::ECS::AlwaysLoadedComponent>();
                            } );
                return; // the entity changed under this frame's reads; drawn again next frame
            }
        }

        // ── Runtime Grid (UE AActor::RuntimeGrid): absent = the world's first grid, UE's main grid ──
        std::optional<std::size_t> gridIndex;
        if ( !partition.RuntimePartitions.empty() )
        {
            const bool                 stating = entity.HasComponent<::Desert::ECS::RuntimeGridComponent>();
            std::optional<std::string> named;
            if ( stating )
                named = entity.GetComponent<::Desert::ECS::RuntimeGridComponent>().Data.Grid;
            gridIndex                 = Rules::GridIndexOf( partition, named );
            const std::string preview = !named.has_value()      ? partition.RuntimePartitions.front().Name + " (main grid)"
                                        : gridIndex.has_value() ? *named
                                                                : *named + " (no such grid)";
            Utils::ImGuiUtilities::BeginPropertyRow(
                 "Runtime Grid",
                 "The World Partition grid this entity streams on; its cell size and loading range "
                 "apply to it. A composite streams on its anchor's grid." );
            std::optional<std::size_t> picked;
            if ( !gridIndex.has_value() )
                ImGui::PushStyleColor( ImGuiCol_Text, kMissingColour );
            const bool open = ImGui::BeginCombo( "##RuntimeGrid", preview.c_str() );
            if ( !gridIndex.has_value() )
                ImGui::PopStyleColor();
            if ( open )
            {
                for ( std::size_t g = 0; g < partition.RuntimePartitions.size(); ++g )
                    if ( ImGui::Selectable( partition.RuntimePartitions[g].Name.c_str(), gridIndex == g ) && gridIndex != g )
                        picked = g;
                ImGui::EndCombo();
            }
            Utils::ImGuiUtilities::EndPropertyRow();
            if ( picked.has_value() )
            {
                const std::string name = partition.RuntimePartitions[*picked].Name;
                const bool        main = *picked == 0;
                EditEntity( uuid,
                            [&entity, &name, main, stating]
                            {
                                // The first grid is the absent block: one spelling of "the main grid".
                                if ( main )
                                    entity.RemoveComponent<::Desert::ECS::RuntimeGridComponent>();
                                else if ( stating )
                                    entity.GetComponent<::Desert::ECS::RuntimeGridComponent>().Data.Grid = name;
                                else
                                    entity.AddComponent<::Desert::ECS::RuntimeGridComponent>().Data.Grid = name;
                            } );
                return;
            }
        }

        // ── HLOD Layer (UE AActor::HLODLayer): absent = the grid's layer ──
        {
            static const std::vector<::Desert::Core::HLODLayerSerialized> kNoLayers;
            const std::vector<::Desert::Core::HLODLayerSerialized>& layers = partition.HLODLayers.value_or( kNoLayers );
            const ::Desert::Core::HLODLayerSerialized* const        gridLayer =
                 gridIndex.has_value() ? Rules::GridHLODLayer( partition, partition.RuntimePartitions[*gridIndex] ) : nullptr;
            const std::string gridDefault = std::format(
                 "Partition's layer ({})", gridLayer != nullptr ? gridLayer->Name : std::string( "None" ) );
            const bool                 stating = entity.HasComponent<::Desert::ECS::HLODLayerComponent>();
            std::optional<std::string> named;
            if ( stating )
                named = entity.GetComponent<::Desert::ECS::HLODLayerComponent>().Data.Layer;
            const bool known = !named.has_value() || Rules::HLODLayerIndexOf( partition, *named ).has_value();
            // A layer named in a grid that builds no HLOD refuses Build HLODs (Rules::CellHLODParts): said here.
            const bool        noCellHLOD = named.has_value() && gridLayer == nullptr;
            const std::string preview    = !named.has_value() ? gridDefault
                                           : !known           ? *named + " (no such layer)"
                                           : noCellHLOD       ? *named + " (its partition builds no HLOD)"
                                                              : *named;
            Utils::ImGuiUtilities::BeginPropertyRow(
                 "HLOD Layer", "The HLOD layer that builds this entity's part of its cell's HLOD, instead of its "
                               "grid's. A composite is built by its anchor's layer." );
            std::optional<std::optional<std::string>> picked;
            if ( !known || noCellHLOD )
                ImGui::PushStyleColor( ImGuiCol_Text, kMissingColour );
            const bool open = ImGui::BeginCombo( "##HLODLayer", preview.c_str() );
            if ( !known || noCellHLOD )
                ImGui::PopStyleColor();
            if ( open )
            {
                if ( ImGui::Selectable( gridDefault.c_str(), !named.has_value() ) && named.has_value() )
                    picked = std::optional<std::string>();
                for ( const ::Desert::Core::HLODLayerSerialized& layer : layers )
                    if ( ImGui::Selectable( layer.Name.c_str(), named == layer.Name ) && named != layer.Name )
                        picked = std::optional<std::string>( layer.Name );
                if ( layers.empty() )
                    ImGui::TextDisabled( "This world states no HLOD layer (World Partition > HLOD layers)." );
                ImGui::EndCombo();
            }
            Utils::ImGuiUtilities::EndPropertyRow();
            if ( picked.has_value() )
            {
                const std::optional<std::string> layer = *picked;
                EditEntity( uuid,
                            [&entity, &layer, stating]
                            {
                                if ( !layer.has_value() )
                                    entity.RemoveComponent<::Desert::ECS::HLODLayerComponent>();
                                else if ( stating )
                                    entity.GetComponent<::Desert::ECS::HLODLayerComponent>().Data.Layer = *layer;
                                else
                                    entity.AddComponent<::Desert::ECS::HLODLayerComponent>().Data.Layer = *layer;
                            } );
                return;
            }
        }

        // ── Data Layers (UE AActor::DataLayerAssets): absent = no layer ──
        {
            static const std::vector<::Desert::Core::DataLayerSerialized> kNoLayers;
            const std::vector<::Desert::Core::DataLayerSerialized>& layers = partition.DataLayers.value_or( kNoLayers );
            const bool               stating                     = entity.HasComponent<::Desert::ECS::DataLayersComponent>();
            std::vector<std::string> member;
            if ( stating )
                member = entity.GetComponent<::Desert::ECS::DataLayersComponent>().Data.Layers;
            std::string preview;
            for ( const std::string& name : member )
                preview += std::format(
                     "{}{}{}", preview.empty() ? "" : ", ", name,
                     Rules::DataLayerIndexOf( partition, name ).has_value() ? "" : " (no such layer)" );
            if ( preview.empty() )
                preview = "No layer";
            Utils::ImGuiUtilities::BeginPropertyRow(
                 "Data Layers",
                 "The data layers this entity belongs to. A composite belongs to its anchor's layers." );
            std::optional<std::string> toggled;
            if ( ImGui::BeginCombo( "##DataLayers", preview.c_str() ) )
            {
                if ( layers.empty() )
                    ImGui::TextDisabled( "This world has no data layers (World Partition > Data Layers)." );
                for ( const ::Desert::Core::DataLayerSerialized& layer : layers )
                {
                    const bool on = std::find( member.begin(), member.end(), layer.Name ) != member.end();
                    if ( ImGui::Selectable( layer.Name.c_str(), on ) )
                        toggled = layer.Name;
                }
                // A name read from the file that the world has no layer of: listed so it can be removed.
                for ( const std::string& name : member )
                    if ( !Rules::DataLayerIndexOf( partition, name ).has_value() )
                    {
                        ImGui::PushStyleColor( ImGuiCol_Text, kMissingColour );
                        if ( ImGui::Selectable(
                                  std::format( "{}  (no such layer - click to remove)", name ).c_str(), true ) )
                            toggled = name;
                        ImGui::PopStyleColor();
                    }
                ImGui::EndCombo();
            }
            Utils::ImGuiUtilities::EndPropertyRow();
            if ( toggled.has_value() )
            {
                std::vector<std::string> after = member;
                const auto               found = std::find( after.begin(), after.end(), *toggled );
                if ( found != after.end() )
                    after.erase( found );
                else
                    after.push_back( *toggled );
                EditEntity( uuid,
                            [&entity, &after, stating]
                            {
                                // No layer is no block, never an empty list.
                                if ( after.empty() )
                                    entity.RemoveComponent<::Desert::ECS::DataLayersComponent>();
                                else if ( stating )
                                    entity.GetComponent<::Desert::ECS::DataLayersComponent>().Data.Layers = after;
                                else
                                    entity.AddComponent<::Desert::ECS::DataLayersComponent>().Data.Layers = after;
                            } );
                return;
            }
        }

        // ── Cell: where the composite lands, the planner's own answer ──
        const std::uint64_t id       = static_cast<std::uint64_t>( uuid );
        const std::uint64_t revision = CommandHistory::Get().Revision();
        const bool          marker   = entity.HasComponent<::Desert::ECS::AlwaysLoadedComponent>();
        if ( !m_CellValid || m_CellEntity != id || m_CellRevision != revision || m_CellScene != &scene ||
             m_CellPartition != partition || m_CellAlwaysLoaded != marker )
        {
            m_CellLine         = PlanCellLine( entity, scene, assets );
            m_CellEntity       = id;
            m_CellRevision     = revision;
            m_CellScene        = &scene;
            m_CellPartition    = partition;
            m_CellAlwaysLoaded = marker;
            m_CellValid        = true;
        }
        Utils::ImGuiUtilities::BeginPropertyRow(
             "Cell",
             "Where World Partition puts this entity's composite: the cell (grid, level, X, Z, runtime data "
             "layers) whose file and residency it shares, or why it stays loaded everywhere." );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( m_CellLine.c_str() );
        Utils::ImGuiUtilities::EndPropertyRow();
    }

    std::string WorldPartitionDetails::PlanCellLine( const ::Desert::ECS::Entity& entity, ::Desert::Core::Scene& scene,
                                                     const std::shared_ptr<::Desert::Assets::AssetManager>& assets ) const
    {
        // The loader's own path from a scene to a plan (as the World Partition panel's Edit plan): what Play and
        // the cook would decide for this composite.
        const std::string json   = ::Desert::Core::SceneSerializer( &scene, assets.get() ).SerializeToJson();
        auto              parsed = ::Desert::Core::ParseLoadableScene( "<Details: World Partition>", json );
        if ( !parsed )
            return "not planned: " + parsed.GetError();
        const ::Desert::Core::SceneSerialized& serialized = parsed.GetValue().Scene;
        if ( !serialized.WorldPartition.has_value() || serialized.WorldPartition->RuntimePartitions.empty() )
            return "not planned: the world states no runtime partition";
        const ::Desert::Core::WorldPartitionSerialized& partition = *serialized.WorldPartition;
        const Rules::WorldPartitionPlan       plan =
             Rules::PlanWorldPartition( serialized.Entities, partition, ::Desert::Core::RegistryMeshBounds(),
                                       ::Desert::Core::WorldCells::ScriptReferenceKinds() );

        const Common::UUID uuid   = entity.GetComponent<::Desert::ECS::UUIDComponent>().UUID;
        std::size_t        record = Rules::kNoRecord;
        for ( std::size_t r = 0; r < serialized.Entities.size(); ++r )
            if ( serialized.Entities[r].id.has_value() && static_cast<std::uint64_t>( *serialized.Entities[r].id ) == static_cast<std::uint64_t>( uuid ) )
                record = r;
        if ( record == Rules::kNoRecord )
            return "not planned: the entity is not in the scene's saved form";
        for ( std::size_t c = 0; c < plan.Composites.size(); ++c )
        {
            const Rules::PlannedComposite& composite = plan.Composites[c];
            if ( std::find( composite.Members.begin(), composite.Members.end(), record ) ==
                 composite.Members.end() )
                continue;
            if ( composite.Reason != Rules::AlwaysLoadedReason::None )
                return std::format( "always loaded: {}", AlwaysLoadedReasonName( composite.Reason ) );
            for ( const Rules::PlannedCell& cell : plan.Cells )
                if ( std::find( cell.Composites.begin(), cell.Composites.end(), c ) != cell.Composites.end() )
                    return std::format(
                         "{} - grid {}, level {}, cell {:.0f} cm", Rules::CellStem( partition, cell ),
                         partition.RuntimePartitions.at( cell.Grid ).Name, cell.Level,
                         Rules::LevelCellSize( partition.RuntimePartitions.at( cell.Grid ).LHGrid->CellSize, cell.Level ) );
            return "not planned: its composite is in no cell of the plan";
        }
        return "no place: a prefab instance or landscape tile the plan could not place (World Partition panel)";
    }
} // namespace Desert::Editor
