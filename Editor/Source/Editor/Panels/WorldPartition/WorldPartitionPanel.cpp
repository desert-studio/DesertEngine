// Ported from UE 5.8
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/SWorldPartitionEditorGrid2D.cpp :1190-1212
// (mouse: wheel zoom, right-drag pan), :1220-1262 (PaintGrid: the level's grid lines and the X/Y axes at 40 %
// opacity), :1480-1530 (PaintStreamingSources: the source's position, view direction and loading circle), and
// Engine/Source/Runtime/Engine/Private/WorldPartition/WorldPartitionRuntimeSpatialHash.cpp:1040-1075 (Draw2D:
// one filled tile per cell, outlined), adapted: Slate painting becomes an ImGui draw list; the cell tiles and
// their states come from our partition plan and residency (WorldPartitionMap.hpp) instead of a runtime hash;
// UE's unload band (the source's range times the unload margin) is drawn dashed as its own circle; the map's
// vertical axis is world Z (the ground plane of a Y-up engine) instead of UE's Y.

#include "WorldPartitionPanel.hpp"

#include <cstdint>
#include "EditorLoadedRegions.hpp"
#include "EditorHLODs.hpp"
#include <Editor/Core/EditorPreferences.hpp>

#include "../../Core/CommandHistory.hpp"
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Commands/WorldPartitionEditCommand.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliagePaintTool.hpp>

#include <algorithm>
#include <ranges>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Projects/WorldPartitionSettings.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Common/Json/Document.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <numbers>
#include <optional>
#include <system_error>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace Map   = WorldPartitionMap;
    using ::Desert::Core::Rules::WorldPartitionPlan;

    namespace
    {
        // The action's one name, from the rules header, so the button and the palette cannot drift apart.
        const std::string kConvertButtonLabel{ ::Desert::Core::Rules::kConvertToWorldPartitionLabel };

        // UE's minimum on-screen grid cell (SWorldPartitionEditorGrid2D PaintGrid): finer levels would be noise.
        constexpr double kMinGridPixels = 24.0;

        ImU32 ToU32( glm::vec4 rgba )
        {
            return ImGui::ColorConvertFloat4ToU32( ImVec4( rgba.r, rgba.g, rgba.b, rgba.a ) );
        }

        // The entity ids of an editor selection, as the loaders name records.
        std::vector<std::uint64_t> IdsOf( const std::vector<Common::UUID>& selected )
        {
            std::vector<std::uint64_t> ids;
            ids.reserve( selected.size() );
            for ( const Common::UUID& id : selected )
                ids.push_back( static_cast<std::uint64_t>( id ) );
            return ids;
        }

        // One row of the Loaders section: its name and what it is (UE's loader list shows the same).
        std::string LoaderLabel( const ::Desert::Core::EditorRegions::LoaderRow& row )
        {
            using Kind              = ::Desert::Core::EditorRegions::LoaderKind;
            const char* const state = row.Loaded ? "Loaded" : "Unloaded";
            switch ( row.Kind )
            {
                case Kind::Whole:
                    return std::format( "{} (Load All: every record of the world)", row.Name );
                case Kind::Volume:
                    return std::format( "{} (Location Volume, {})", row.Name, state );
                case Kind::Region:
                    if ( row.Box )
                        return std::format( "{} ({:.0f}..{:.0f} x {:.0f}..{:.0f} m, {})", row.Name,
                                            static_cast<double>( row.Box->MinX ) / 100.0,
                                            static_cast<double>( row.Box->MaxX ) / 100.0,
                                            static_cast<double>( row.Box->MinZ ) / 100.0,
                                            static_cast<double>( row.Box->MaxZ ) / 100.0, state );
                    return std::format( "{} ({})", row.Name, state );
                case Kind::EntityList:
                    return std::format( "{} ({} entities, {})", row.Name, row.Count, state );
                case Kind::Pinned:
                    return std::format( "{} ({})", row.Name, row.Count );
            }
            return row.Name;
        }

        ImVec2 ToScreen( const Map::View& view, glm::dvec2 size, ImVec2 origin, glm::dvec2 world )
        {
            const glm::dvec2 local = Map::WorldToScreen( view, size, world );
            return { origin.x + static_cast<float>( local.x ), origin.y + static_cast<float>( local.y ) };
        }

        // A source's shape on the map (Rules::SourceSector), in world space so the map's axes stay its own:
        // the circle, or the slice's arc and its two sides. @p dashed draws every other segment (the unload band).
        void PaintSector( ImDrawList& list, const Map::View& view, glm::dvec2 size, ImVec2 origin,
                          const ::Desert::Core::Rules::SourceSector& sector, double scale, double angleScale,
                          ImU32 colour, bool dashed )
        {
            const double radius = sector.Radius * scale;
            const double angle  = std::min( 360.0, sector.Angle * angleScale );
            const int    pieces = std::max( 8, static_cast<int>( 96.0 * angle / 360.0 ) );
            const auto   rim    = [&]( double degrees )
            {
                const double r = degrees * std::numbers::pi / 180.0;
                // The same turn as the rules (about +Y): +X goes to −Z.
                const glm::dvec2 axis = sector.Axis;
                const glm::dvec2 dir( axis.x * std::cos( r ) + axis.y * std::sin( r ),
                                      -axis.x * std::sin( r ) + axis.y * std::cos( r ) );
                return ToScreen( view, size, origin, sector.Centre + dir * radius );
            };
            for ( int i = 0; i < pieces; ++i )
            {
                if ( dashed && ( i % 2 ) != 0 )
                    continue;
                const double a0 = -angle * 0.5 + angle * i / pieces;
                const double a1 = -angle * 0.5 + angle * ( i + 1 ) / pieces;
                list.AddLine( rim( a0 ), rim( a1 ), colour, 1.5f );
            }
            if ( angle < 360.0 )
            {
                const ImVec2 apex = ToScreen( view, size, origin, sector.Centre );
                list.AddLine( apex, rim( -angle * 0.5 ), colour, 1.5f );
                list.AddLine( apex, rim( angle * 0.5 ), colour, 1.5f );
            }
        }

        // Below this the legend (five rows in Play) covers the cells; a smaller window scrolls instead.
        constexpr float kMinMapPixels = 240.0f;

    } // namespace

    WorldPartitionPanel::WorldPartitionPanel( std::shared_ptr<::Desert::Core::Scene> scene,
                                              ::Desert::Assets::AssetManager* assets, StreamerGetter streamer )
         : IPanel( "World Partition", false ), m_Scene( std::move( scene ) ), m_Assets( assets ),
           m_Streamer( std::move( streamer ) )
    {
    }

    void WorldPartitionPanel::SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        if ( scene == m_Scene )
            return;
        // The editor HLODs belong to the scene they were made in: they leave it with the window's attention.
        if ( m_Scene )
            m_EditorHLODs.Release( *m_Scene );
        m_EditorHLODsStale = true;
        m_Scene         = scene;
        m_EditPlanStale = true;
        m_FocusPending  = true;
        m_Selection.reset();
        m_DragFrom.reset();
        m_Held     = {};
        m_HeldRead = false;
        m_RowIds.clear();
        m_Volumes.clear();
        m_VolumesKnown = false;
        m_DataLayerStatus.clear();
        // A prompt asked about the scene that is gone: its answer must not change the new one.
        m_PendingUnload.reset();
    }

    const ::Desert::Core::EditorRegions::HeldRegions& WorldPartitionPanel::Held()
    {
        if ( m_HeldRead || !m_Scene )
            return m_Held;
        m_HeldRead = true;
        if ( const auto& baseline = m_Scene->Packages()->BaselinePath() )
        {
            auto remembered = EditorLoadedRegions::Remembered( *baseline );
            if ( remembered )
                m_Held = remembered.ExtractValue();
            else
                m_DataLayerStatus = "The loaded regions could not be read: " + remembered.GetError();
        }
        return m_Held;
    }

    void WorldPartitionPanel::RebuildEditPlan()
    {
        m_HLODStatusStale = true; // what the plan is remade for (a save, a region load, Refresh) may move it
        m_EditPlanStale = false;
        m_EditPlan.reset();
        m_PlanSource.reset();
        if ( !m_Scene )
        {
            m_EditPlanStatus = "No active scene.";
            return;
        }
        m_PlanSource    = m_Scene->GetWorldPartition();
        m_PlanFromIndex = false;
        m_EditCells.clear();
        m_RowIds.clear();
        m_Volumes.clear();
        m_EditTags.clear();
        m_HLODCells.clear();
        m_HLODOverlayStatus.clear();
        // A PARTITIONED WORLD OPENED FROM ITS FILES IS PLANNED OVER ITS DESCRIPTOR INDEX (WP20, UE's World
        // Partition editor draws every actor descriptor, loaded or not): the scene may hold only a region of it,
        // and the map is where the next region is chosen. An entity's unsaved move shows after its save.
        if ( m_PlanSource && m_Scene->Packages()->BaselinePath() )
        {
            if ( m_PlanSource->RuntimePartitions.empty() )
            {
                m_EditPlanStatus = "This scene's WorldPartition block states no runtime partition.";
                return;
            }
            auto index = ::Desert::Core::DescriptorIndex::Refresh( *m_Scene->Packages()->BaselinePath() );
            if ( !index )
            {
                m_EditPlanStatus = "The world's descriptor index could not be read: " + index.GetError();
                return;
            }
            const std::vector<::Desert::Core::Rules::EntityDescriptor> descriptors =
                 ::Desert::Core::DescriptorIndex::Descriptors( index.GetValue().Index );
            m_EditPartition = *m_PlanSource;
            m_EditPlan      = ::Desert::Core::Rules::PlanWorldPartition(
                 std::span<const ::Desert::Core::Rules::EntityDescriptor>( descriptors ), m_EditPartition,
                 ::Desert::Core::RegistryMeshBounds(), ::Desert::Core::WorldCells::ScriptReferenceKinds() );
            m_PlanFromIndex = true;
            // What of each cell the editor holds now: a region load marks the plan stale, so this is remade
            // whenever the held set changes.
            std::vector<std::uint64_t> rowIds;
            rowIds.reserve( index.GetValue().Index.Entities.size() );
            for ( const ::Desert::Core::DescriptorIndex::DescriptorRow& row : index.GetValue().Index.Entities )
                rowIds.push_back( row.Id );
            const ::Desert::Core::EntityPackages& packages = *m_Scene->Packages();
            m_EditCells =
                 WorldPartitionMap::EditorCellStates( *m_EditPlan, rowIds, [&packages]( std::uint64_t id )
                                                      { return packages.IsLoaded( Common::UUID( id ) ); } );
            m_RowIds  = std::move( rowIds );
            m_Volumes = ::Desert::Core::EditorRegions::LocationVolumesOf( descriptors );
            // The world's Location Volumes join this user's loaders (EditorRegions::WithWorldVolumes): with this
            // user's state, else their Auto Load - which the open applied, so only the first reading of this scene
            // may take it; a volume saved after the open joins unloaded, since nothing loaded it.
            ::Desert::Core::EditorRegions::HeldRegions held = Held();
            if ( m_VolumesKnown )
                for ( const ::Desert::Core::EditorRegions::LocationVolume& volume : m_Volumes )
                    if ( std::none_of( held.Volumes.begin(), held.Volumes.end(),
                                       [&volume]( const ::Desert::Core::EditorRegions::VolumeLoader& named )
                                       { return named.Volume == volume.Id; } ) )
                        held.Volumes.push_back( ::Desert::Core::EditorRegions::VolumeLoader{ volume.Id, false } );
            m_Held         = ::Desert::Core::EditorRegions::WithWorldVolumes( held, m_Volumes );
            m_VolumesKnown = true;
            m_EditTags.reserve( index.GetValue().Index.Entities.size() );
            for ( const ::Desert::Core::DescriptorIndex::DescriptorRow& row : index.GetValue().Index.Entities )
                m_EditTags.push_back( row.Descriptor.Tag );
            ReadHLODOverlay( *m_Scene->Packages()->BaselinePath(), descriptors );
            m_EditPlanStatus.clear(); // the view stays: a region load replans the same world
            return;
        }
        // The loader's own path from a scene to a plan, so the map shows the partition Play and the cook make.
        const std::string json   = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets ).SerializeToJson();
        auto              parsed = ::Desert::Core::ParseLoadableScene( "<World Partition panel>", json );
        if ( !parsed )
        {
            m_EditPlanStatus = "The scene could not be planned: " + parsed.GetError();
            return;
        }
        const ::Desert::Core::SceneSerialized& scene = parsed.GetValue().Scene;
        if ( !scene.WorldPartition )
        {
            m_EditPlanStatus = "This scene is not partitioned: it states no WorldPartition block.";
            return;
        }
        if ( scene.WorldPartition->RuntimePartitions.empty() )
        {
            m_EditPlanStatus = "This scene's WorldPartition block states no runtime partition.";
            return;
        }
        m_EditPartition = *scene.WorldPartition;
        m_EditPlan      = ::Desert::Core::Rules::PlanWorldPartition( scene.Entities, m_EditPartition,
                                                                     ::Desert::Core::RegistryMeshBounds(),
                                                                     ::Desert::Core::WorldCells::ScriptReferenceKinds() );
        m_EditPlanStatus.clear();
        m_FocusPending = true;
    }

    void WorldPartitionPanel::ReadHLODOverlay( const std::filesystem::path&                             scenePath,
                                               std::span<const ::Desert::Core::Rules::EntityDescriptor> descriptors )
    {
        namespace WC = ::Desert::Core::WorldCells;
        m_HLODCells.clear();
        m_HLODOverlayStatus.clear();
        // No Build HLODs yet: the world has no HLOD to show, built or stale.
        std::error_code ec;
        if ( !std::filesystem::exists(
                  WC::BuiltHLODDirectory( scenePath ) / std::filesystem::path( WC::kBuiltHLODIndexName ), ec ) )
            return;
        auto stored = WC::ReadBuiltHLODs( scenePath );
        if ( !stored )
        {
            m_HLODOverlayStatus = "HLOD overlay: " + stored.GetError();
            return;
        }
        // Judged against the SAVED world, read exactly as Build HLODs reads it, so a cell is stale here exactly when
        // the cook would refuse it.
        auto text = ::Desert::Core::ExternalEntities::ReadSceneFileText( scenePath );
        if ( !text )
        {
            m_HLODOverlayStatus = "HLOD overlay: " + text.GetError();
            return;
        }
        auto saved = ::Desert::Core::ParseLoadableScene( scenePath.string(), text.GetValue() );
        if ( !saved )
        {
            m_HLODOverlayStatus = "HLOD overlay: " + saved.GetError();
            return;
        }
        const std::span registries( &::Desert::Assets::ContentRegistry::Get(), 1 );
        // Against the world the build cut: prefab instances expanded as Build HLODs and the cook expand them (WP21);
        // over the unexpanded records every cell holding an instance read stale here while the cook found it fresh.
        auto cut = WC::ExpandWorldForCook( saved.GetValue().Scene, descriptors, WC::PrefabFileReader() );
        if ( !cut )
        {
            m_HLODOverlayStatus = "HLOD overlay: " + cut.GetError();
            return;
        }
        auto stale = WC::StaleHLODCells( stored.GetValue().Index, cut.GetValue().World.Scene,
                                         cut.GetValue().Descriptors, registries );
        if ( !stale )
        {
            m_HLODOverlayStatus = "HLOD overlay: " + stale.GetError();
            return;
        }
        std::vector<std::string> built;
        for ( const WC::BuiltHLODCell& row : stored.GetValue().Index.Cells )
            if ( row.File )
                built.push_back( row.Unit );
        m_HLODCells = Map::HLODCellStates( *m_EditPlan, built, stale.GetValue() );
    }

    void WorldPartitionPanel::OnUIRender()
    {
        const ::Desert::Core::WorldStreamer* streamer = ( m_Streamer && m_Scene ) ? m_Streamer() : nullptr;
        if ( streamer != nullptr && !streamer->Streams( *m_Scene ) )
            streamer = nullptr; // it streams another scene view's world, not the one this panel follows
        const bool playing = streamer != nullptr;
        if ( playing != m_WasPlaying )
        {
            m_WasPlaying   = playing;
            m_FocusPending = true;
            if ( !playing )
                m_EditPlanStale = true; // Stop rebuilt the scene: the old plan's indices describe nothing now
        }

        // The scene's partition changed under the panel - a conversion, or the undo of one. Cheap enough
        // to ask every frame (two floats per grid); rebuilding the plan is not, which is why it is asked.
        if ( !playing && m_Scene && m_Scene->GetWorldPartition() != m_PlanSource )
            m_EditPlanStale = true;

        if ( !playing && m_EditPlanStale )
            RebuildEditPlan();

        // ── toolbar ──
        if ( playing )
        {
            ImGui::TextUnformatted( "Play: streaming" );
            ImGui::SameLine();
            ImGui::Checkbox( "Follow camera", &m_Follow );
        }
        else
        {
            ImGui::TextUnformatted( m_PlanFromIndex ? "Edit: the world on disk" : "Edit: partition plan" );
            ImGui::SameLine();
            if ( ImGui::Button( "Refresh" ) )
                RebuildEditPlan();
            // Offered only where it can do something. On a world that is already partitioned the action
            // still EXISTS in the command palette and still refuses by name there - hiding the button is
            // a statement about the button, not about the rule.
            if ( m_Scene && !m_Scene->GetWorldPartition().has_value() )
            {
                ImGui::SameLine();
                if ( ImGui::Button( kConvertButtonLabel.c_str() ) )
                {
                    const Common::BoolResultStr converted = ConvertSceneToWorldPartition();
                    m_ConvertStatus = converted.IsSuccess() ? std::string() : converted.GetError();
                }
            }
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Focus" ) )
        {
            m_FocusPending = true;
            m_Follow       = false;
            m_FollowZoomed = false;
        }

        const WorldPartitionPlan*                       plan      = nullptr;
        const ::Desert::Core::WorldPartitionSerialized* partition = nullptr;
        const ::Desert::Core::Rules::ResidencyState*    residency = nullptr;
        if ( playing )
        {
            plan      = &streamer->Plan();
            partition = &streamer->Partition();
            residency = &streamer->Residency();
        }
        else if ( m_EditPlan )
        {
            plan      = &*m_EditPlan;
            partition = &m_EditPartition;
        }

        if ( !m_ConvertStatus.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.5f, 0.35f, 1.0f ), "%s", m_ConvertStatus.c_str() );

        // ── Load Region (Edit, a partitioned world opened from its files) ──
        if ( !playing && m_Scene && m_Scene->GetWorldPartition().has_value() &&
             m_Scene->Packages()->BaselinePath().has_value() )
        {
            ImGui::BeginDisabled( !m_Selection.has_value() );
            if ( ImGui::Button( "Load Region" ) )
                if ( const auto loaded = LoadSelectedRegion(); !loaded )
                    m_RegionStatus = loaded.GetError();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button( "Unload All" ) )
                if ( const auto loaded = LoadRegion( RegionRequest{} ); !loaded )
                    m_RegionStatus = loaded.GetError();
            ImGui::SameLine();
            if ( ImGui::Button( "Load All" ) )
                if ( const auto loaded = LoadRegion( RegionRequest{ {}, true } ); !loaded )
                    m_RegionStatus = loaded.GetError();
            ImGui::SameLine();
            if ( m_Selection )
                ImGui::TextDisabled( "selection %.0f..%.0f x %.0f..%.0f m; %zu record(s) on disk",
                                     static_cast<double>( m_Selection->MinX ) / 100.0,
                                     static_cast<double>( m_Selection->MaxX ) / 100.0,
                                     static_cast<double>( m_Selection->MinZ ) / 100.0,
                                     static_cast<double>( m_Selection->MaxZ ) / 100.0,
                                     m_Scene->Packages()->NotLoadedCount() );
            else
                ImGui::TextDisabled( "drag on the map to select a region; %zu record(s) on disk",
                                     m_Scene->Packages()->NotLoadedCount() );
            if ( !m_RegionStatus.empty() )
                ImGui::TextWrapped( "%s", m_RegionStatus.c_str() );
            DrawLoaders();
        }

        // ── HLOD per grid (Edit, a partitioned world) ──
        if ( !playing && m_Scene && m_Scene->GetWorldPartition().has_value() )
        {
            // What the HLOD builders say of the world's layers (UE: a layer whose builder is not loaded or refuses
            // its settings), red and above every section: Build HLODs refuses with the first of them.
            for ( const std::string& refused : ::Desert::Core::Rules::HLODLayerBuilderIssues(
                       *m_Scene->GetWorldPartition(), ::Desert::Core::HLODBuilders() ) )
                ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.35f, 1.0f ), "%s", refused.c_str() );
            DrawHLODBuild();
            DrawRuntimeGrids();
            DrawHLODLayers();
            DrawGridHLODLayers();
            DrawEditorHLODs();
            DrawDataLayers();
        }

        // A loader change above read the world on disk again (RebuildEditPlan remade m_EditPlan): the map draws
        // the plan as it is now, or none.
        if ( !playing )
        {
            plan      = m_EditPlan ? &*m_EditPlan : nullptr;
            partition = m_EditPlan ? &m_EditPartition : nullptr;
        }

        // The map shows one partition that streams by cells (UE's runtime hash grid picker); a Persistent
        // partition has no cell to draw - its content is in the always-loaded count.
        const auto makesCells = [&]( std::size_t index )
        { return ::Desert::Core::Rules::CellGridOf( *partition, index ) != nullptr; };
        std::optional<std::size_t> firstCellPartition;
        for ( std::size_t g = 0; partition != nullptr && g < partition->RuntimePartitions.size() && !firstCellPartition;
              ++g )
            if ( makesCells( g ) )
                firstCellPartition = g;
        if ( plan == nullptr || !firstCellPartition.has_value() )
        {
            ImGui::TextDisabled( "%s", playing
                                            ? "The streamed partition states no partition that streams by cells."
                                       : plan == nullptr ? m_EditPlanStatus.c_str()
                                                         : "This world states no partition that streams by "
                                                           "cells: everything in it is always loaded." );
            return;
        }

        // ── the partition shown: every partition of the world that makes cells reachable ──
        if ( m_Grid >= partition->RuntimePartitions.size() || !makesCells( m_Grid ) )
        {
            m_Grid         = *firstCellPartition;
            m_FocusPending = true;
        }
        ImGui::SetNextItemWidth( 140.0f );
        if ( ImGui::BeginCombo( "Partition", partition->RuntimePartitions[m_Grid].Name.c_str() ) )
        {
            for ( std::size_t g = 0; g < partition->RuntimePartitions.size(); ++g )
            {
                if ( !makesCells( g ) )
                    continue;
                ImGui::PushID( static_cast<int>( g ) );
                const bool picked = g == m_Grid;
                if ( ImGui::Selectable( partition->RuntimePartitions[g].Name.c_str(), picked ) && !picked )
                {
                    m_Grid         = g;
                    m_FocusPending = true;
                    m_Fitted       = false;
                }
                if ( picked )
                    ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        const ::Desert::Core::RuntimePartitionLHGridSerialized& shown =
             *::Desert::Core::Rules::CellGridOf( *partition, m_Grid );
        // A plan without per-partition levels (none planned yet) shows the world's level count.
        const int gridLevels =
             m_Grid < plan->GridLevelCount.size() ? plan->GridLevelCount[m_Grid] : plan->LevelCount;
        const std::size_t gridCells = static_cast<std::size_t>(
             std::ranges::count( plan->Cells, m_Grid, &::Desert::Core::Rules::PlannedCell::Grid ) );
        const int maxLevel = std::max( gridLevels - 1, 0 );
        m_Level            = std::min( m_Level, maxLevel );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::SliderInt( "Level", &m_Level, -1, maxLevel, m_Level < 0 ? "all" : "L%d" );
        ImGui::SameLine();
        ImGui::TextDisabled( "%zu cells, %zu always-loaded, %d level(s), cell %.0f m", gridCells,
                             plan->AlwaysLoaded.size(), gridLevels,
                             static_cast<double>( shown.CellSize ) / 100.0 );

        if ( !playing && !m_HLODOverlayStatus.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.5f, 0.35f, 1.0f ), "%s", m_HLODOverlayStatus.c_str() );
        if ( m_MiniMapBuild )
            ImGui::Text( "Build Minimap: tile %zu of %zu", m_MiniMapBuild->TilesDone() + 1, m_MiniMapBuild->TileCount() );
        else if ( !m_MiniMapStatus.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.5f, 0.35f, 1.0f ), "%s", m_MiniMapStatus.c_str() );

        DrawMap( *plan, *partition, residency, streamer );
    }

    // UE FRuntimePartitionDesc::HLODSetups, shown where UE shows them - under the runtime partition they belong
    // to: per partition whose content can be away, its setups in hierarchy order (setup 0 stands in for the
    // cells, each next one for the setup before - RuntimePartitionHLODSetupSerialized), each a name and a layer
    // picked from the world's HLODLayers; added at the end, removed from the end (removing from the middle would
    // re-chain the rest silently). Every edit is ONE history entry through EditWorldPartition, which refuses what
    // ValidateHLODLayers refuses.
    void WorldPartitionPanel::DrawGridHLODLayers()
    {
        const ::Desert::Core::WorldPartitionSerialized current = *m_Scene->GetWorldPartition();
        if ( current.RuntimePartitions.empty() || !ImGui::CollapsingHeader( "HLOD setups per partition" ) )
            return;
        static const std::vector<::Desert::Core::HLODLayerSerialized> kNoLayers;
        const std::vector<::Desert::Core::HLODLayerSerialized>& layers = current.HLODLayers.value_or( kNoLayers );
        if ( layers.empty() )
            ImGui::TextDisabled(
                 "This world states no HLOD layer; a setup can only name one of the world's layers." );

        std::optional<::Desert::Core::WorldPartitionSerialized> after;
        std::string                                             label;
        for ( std::size_t g = 0; g < current.RuntimePartitions.size(); ++g )
        {
            const ::Desert::Core::RuntimePartitionSerialized& runtime = current.RuntimePartitions[g];
            ImGui::PushID( static_cast<int>( g ) );
            ImGui::TextUnformatted( std::format( "{} ({})", runtime.Name,
                                                 ::Desert::Core::Rules::RuntimePartitionTypeName( runtime.Type ) )
                                         .c_str() );
            if ( !::Desert::Core::Rules::RuntimePartitionClass( runtime.Type ).CanBeAway() )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "always loaded: no HLOD stands in for it" );
                ImGui::PopID();
                continue;
            }
            static const std::vector<::Desert::Core::RuntimePartitionHLODSetupSerialized> kNoSetups;
            const std::vector<::Desert::Core::RuntimePartitionHLODSetupSerialized>&       setups =
                 runtime.HLODSetups.has_value() ? *runtime.HLODSetups : kNoSetups;
            if ( setups.empty() )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "no HLOD: its far cells draw nothing" );
            }
            ImGui::Indent();
            for ( std::size_t k = 0; k < setups.size(); ++k )
            {
                const ::Desert::Core::RuntimePartitionHLODSetupSerialized& setup = setups[k];
                ImGui::PushID( static_cast<int>( k ) );
                ImGui::SetNextItemWidth( 200.0f );
                if ( ImGui::BeginCombo( std::format( "HLOD{} '{}'", k, setup.Name ).c_str(),
                                        setup.HLODLayer.c_str() ) )
                {
                    for ( const ::Desert::Core::HLODLayerSerialized& layer : layers )
                        if ( ImGui::Selectable( layer.Name.c_str(), setup.HLODLayer == layer.Name ) &&
                             setup.HLODLayer != layer.Name )
                        {
                            after                                                    = current;
                            ( *after->RuntimePartitions[g].HLODSetups )[k].HLODLayer = layer.Name;
                            label = std::format( "Set HLOD setup {} of partition {} to layer {}", setup.Name,
                                                 runtime.Name, layer.Name );
                        }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            // A new setup builds with the first layer this partition's chain does not name yet (a layer twice in
            // one chain is refused, ValidateHLODLayers); none left, no button.
            const auto unused = std::find_if(
                 layers.begin(), layers.end(),
                 [&]( const ::Desert::Core::HLODLayerSerialized& layer )
                 {
                     return std::none_of( setups.begin(), setups.end(),
                                          [&]( const ::Desert::Core::RuntimePartitionHLODSetupSerialized& setup )
                                          { return setup.HLODLayer == layer.Name; } );
                 } );
            if ( unused != layers.end() && ImGui::SmallButton( "Add HLOD setup" ) )
            {
                after = current;
                std::optional<std::vector<::Desert::Core::RuntimePartitionHLODSetupSerialized>>& edited =
                     after->RuntimePartitions[g].HLODSetups;
                if ( !edited.has_value() )
                    edited.emplace();
                ::Desert::Core::RuntimePartitionHLODSetupSerialized setup;
                std::size_t                                         n     = edited->size();
                const auto                                          taken = [&]( const std::string& name )
                {
                    return std::any_of( edited->begin(), edited->end(),
                                        [&]( const ::Desert::Core::RuntimePartitionHLODSetupSerialized& other )
                                        { return other.Name == name; } );
                };
                do
                    setup.Name = std::format( "HLOD{}", n++ );
                while ( taken( setup.Name ) );
                setup.HLODLayer = unused->Name;
                label           = std::format( "Add HLOD setup {} to partition {}", setup.Name, runtime.Name );
                edited->push_back( std::move( setup ) );
            }
            if ( !setups.empty() )
            {
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Remove last setup" ) )
                {
                    after = current;
                    std::optional<std::vector<::Desert::Core::RuntimePartitionHLODSetupSerialized>>& edited =
                         after->RuntimePartitions[g].HLODSetups;
                    label =
                         std::format( "Remove HLOD setup {} of partition {}", edited->back().Name, runtime.Name );
                    edited->pop_back();
                    if ( edited->empty() )
                        edited.reset(); // none is stated as no key, never as an empty list
                }
            }
            ImGui::Unindent();
            ImGui::PopID();
        }
        if ( after )
        {
            const auto edited = EditWorldPartition( m_Scene, std::move( label ), std::move( *after ) );
            m_GridEditStatus  = edited ? std::string() : edited.GetError();
        }
        // A refused edit is named here too: the Runtime partitions section above may be collapsed.
        if ( !m_GridEditStatus.empty() )
            ImGui::TextWrapped( "%s", m_GridEditStatus.c_str() );
    }

    // UE World Settings -> Runtime Hash Set -> Runtime Partitions (FRuntimePartitionDesc): the world's partitions
    // as a table - name, type, and the LHGrid's own settings where the type has them - added, removed and edited
    // here. Every commit (Enter in a cell, a button) is ONE history entry through EditWorldPartition, which
    // refuses what the planner would refuse. Sizes are world units (centimetres).
    void WorldPartitionPanel::DrawRuntimeGrids()
    {
        if ( !ImGui::CollapsingHeader( "Runtime partitions" ) )
            return;
        const ::Desert::Core::WorldPartitionSerialized          current = *m_Scene->GetWorldPartition();
        std::optional<::Desert::Core::WorldPartitionSerialized> after;
        std::string                                             label;
        if ( ImGui::BeginTable( "##runtimepartitions", 7,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp ) )
        {
            ImGui::TableSetupColumn( "Name" );
            ImGui::TableSetupColumn( "Type" );
            ImGui::TableSetupColumn( "Cell size (cm)" );
            ImGui::TableSetupColumn( "Loading range (cm)" );
            ImGui::TableSetupColumn( "Priority" );
            ImGui::TableSetupColumn( "Debug colour" );
            ImGui::TableSetupColumn( "" );
            ImGui::TableHeadersRow();
            constexpr ImGuiInputTextFlags kCommit = ImGuiInputTextFlags_EnterReturnsTrue;
            for ( std::size_t g = 0; g < current.RuntimePartitions.size(); ++g )
            {
                const ::Desert::Core::RuntimePartitionSerialized& runtime = current.RuntimePartitions[g];
                ImGui::PushID( static_cast<int>( g ) );
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char name[128];
                std::snprintf( name, sizeof( name ), "%s", runtime.Name.c_str() );
                ImGui::SetNextItemWidth( -FLT_MIN );
                if ( ImGui::InputText( "##name", name, sizeof( name ), kCommit ) && runtime.Name != name )
                {
                    after                            = current;
                    after->RuntimePartitions[g].Name = name;
                    label = std::format( "Rename partition {} to {}", runtime.Name, name );
                }
                ImGui::TableNextColumn();
                const std::string_view typeName = ::Desert::Core::Rules::RuntimePartitionTypeName( runtime.Type );
                ImGui::TextUnformatted( typeName.data(), typeName.data() + typeName.size() );
                if ( runtime.LHGrid.has_value() )
                {
                    const ::Desert::Core::RuntimePartitionLHGridSerialized& grid = *runtime.LHGrid;
                    ImGui::TableNextColumn();
                    float cell = grid.CellSize;
                    ImGui::SetNextItemWidth( -FLT_MIN );
                    if ( ImGui::InputFloat( "##cell", &cell, 0.0f, 0.0f, "%.0f", kCommit ) &&
                         cell != grid.CellSize )
                    {
                        after                                        = current;
                        after->RuntimePartitions[g].LHGrid->CellSize = cell;
                        label = std::format( "Set cell size of partition {}", runtime.Name );
                    }
                    ImGui::TableNextColumn();
                    float range = grid.LoadingRange;
                    ImGui::SetNextItemWidth( -FLT_MIN );
                    if ( ImGui::InputFloat( "##range", &range, 0.0f, 0.0f, "%.0f", kCommit ) &&
                         range != grid.LoadingRange )
                    {
                        after                                            = current;
                        after->RuntimePartitions[g].LHGrid->LoadingRange = range;
                        label = std::format( "Set loading range of partition {}", runtime.Name );
                    }
                    ImGui::TableNextColumn();
                    int priority = grid.Priority;
                    ImGui::SetNextItemWidth( -FLT_MIN );
                    if ( ImGui::InputInt( "##priority", &priority, 0, 0, kCommit ) && priority != grid.Priority )
                    {
                        after                                        = current;
                        after->RuntimePartitions[g].LHGrid->Priority = priority;
                        label = std::format( "Set priority of partition {}", runtime.Name );
                    }
                }
                else
                {
                    // A type without a grid states none of the three (RuntimePartitionSerialized::LHGrid).
                    for ( int column = 0; column < 3; ++column )
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled( "-" );
                    }
                }
                ImGui::TableNextColumn();
                // UE FSpatialHashRuntimeGrid::DebugColor: what the gameplay debugger paints this grid's cells in.
                if ( const std::optional<glm::vec3> debugColor = DebugColorEdit( false, g, runtime.DebugColor ) )
                {
                    after                                  = current;
                    after->RuntimePartitions[g].DebugColor = *debugColor;
                    label = std::format( "Set debug colour of partition {}", runtime.Name );
                }
                ImGui::TableNextColumn();
                if ( ImGui::SmallButton( "Remove" ) )
                {
                    after = current;
                    after->RuntimePartitions.erase( after->RuntimePartitions.begin() +
                                                    static_cast<std::ptrdiff_t>( g ) );
                    label = std::format( "Remove partition {}", runtime.Name );
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // UE's new partition: a name no partition states; an LHGrid gets the main grid's defaults (128 m cell,
        // 256 m range), a Persistent one nothing to set.
        const auto addPartition = [&]( ::Desert::Core::RuntimePartitionType type, std::string_view stem )
        {
            after = current;
            ::Desert::Core::RuntimePartitionSerialized runtime;
            runtime.Type  = type;
            std::size_t n = after->RuntimePartitions.size() + 1;
            do
                runtime.Name = std::format( "{}{}", stem, n++ );
            while ( ::Desert::Core::Rules::GridIndexOf( *after, runtime.Name ).has_value() );
            if ( type == ::Desert::Core::RuntimePartitionType::LHGrid )
                runtime.LHGrid.emplace();
            runtime.DebugColor = ::Desert::Core::SeededDebugColor( runtime.Name ); // UE seeds a new grid's colour
            label = std::format( "Add {} partition {}", ::Desert::Core::Rules::RuntimePartitionTypeName( type ),
                                 runtime.Name );
            after->RuntimePartitions.push_back( std::move( runtime ) );
        };
        if ( ImGui::Button( "Add LHGrid partition" ) )
            addPartition( ::Desert::Core::RuntimePartitionType::LHGrid, "Grid" );
        ImGui::SameLine();
        if ( ImGui::Button( "Add Persistent partition" ) )
            addPartition( ::Desert::Core::RuntimePartitionType::Persistent, "Persistent" );
        if ( after )
        {
            const auto edited = EditWorldPartition( m_Scene, std::move( label ), std::move( *after ) );
            m_GridEditStatus  = edited ? std::string() : edited.GetError();
        }
        if ( !m_GridEditStatus.empty() )
            ImGui::TextWrapped( "%s", m_GridEditStatus.c_str() );
    }

    std::optional<glm::vec3> WorldPartitionPanel::DebugColorEdit( const bool layer, const std::size_t index,
                                                                  const glm::vec3& stated )
    {
        const auto mine = [&] {
            return m_PendingDebugColor && m_PendingDebugColor->Layer == layer &&
                   m_PendingDebugColor->Index == index;
        };
        glm::vec3 color = mine() ? m_PendingDebugColor->Color : stated;
        if ( ImGui::ColorEdit3( "##debugcolor", &color.x, ImGuiColorEditFlags_NoInputs ) )
            m_PendingDebugColor = PendingDebugColor{ layer, index, color };
        // ColorEdit reports its picker popup's active widget as its own (IsItemActive), so the drag is one edit.
        if ( !mine() || ImGui::IsItemActive() )
            return std::nullopt;
        const glm::vec3 chosen = m_PendingDebugColor->Color;
        m_PendingDebugColor.reset();
        return chosen != stated ? std::optional<glm::vec3>( chosen ) : std::nullopt;
    }

    namespace
    {
        // The settings object a layer starts with when its builder is picked (UE: picking UHLODLayer::
        // HLODBuilderClass creates that class's UHLODBuilderSettings subclass with its defaults): MeshSimplify's
        // starting triangle share (its Settings require one), MeshApproximate's UE defaults; none for Instancing
        // (reads none), MeshMerge (none = one section per source material) and a project's builder (its settings
        // are its own struct).
        std::optional<Common::Json::Value> DefaultBuilderSettings( const ::Desert::Core::HLODLayerType type )
        {
            namespace Core = ::Desert::Core;
            if ( type == Core::HLODLayerType::MeshSimplify )
            {
                Core::HLODMeshSimplifySettings settings;
                settings.TrianglePercent = 0.5f; // UE UHLODBuilderMeshSimplifySettings' starting share
                return Common::Json::FromStruct( settings );
            }
            if ( type == Core::HLODLayerType::MeshApproximate )
                return Common::Json::FromStruct( Core::HLODApproximationSettings{} );
            return std::nullopt;
        }

        // A number committed on Enter - one history entry per commit, never one per keystroke. @p scale shows it
        // in other units (a share as percent).
        bool CommitFloat( const char* label, float& value, const char* format, const float scale = 1.0f )
        {
            float shown = value * scale;
            ImGui::SetNextItemWidth( 140.0f );
            if ( !ImGui::InputFloat( label, &shown, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue ) ||
                 shown / scale == value )
                return false;
            value = shown / scale;
            return true;
        }

        bool CommitInt( const char* label, int32_t& value )
        {
            int shown = value;
            ImGui::SetNextItemWidth( 140.0f );
            if ( !ImGui::InputInt( label, &shown, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue ) || shown == value )
                return false;
            value = shown;
            return true;
        }

        // UE FMeshMergingSettings::bMergeMaterials + FMaterialProxySettings, as the mesh builders' settings show
        // them: true once a change is committed to @p merge.
        bool DrawMergeMaterials( std::optional<::Desert::Core::HLODMaterialMergeSerialized>& merge )
        {
            bool changed = false;
            bool merged  = merge.has_value();
            if ( ImGui::Checkbox( "Merge Materials", &merged ) )
            {
                if ( merged )
                    merge.emplace();
                else
                    merge.reset();
                changed = true;
            }
            if ( !merge.has_value() )
                return changed;
            ImGui::Indent();
            changed |= CommitInt( "Texture size", merge->TextureSize );
            changed |= ImGui::Checkbox( "Normal map", &merge->NormalMap );
            changed |= ImGui::Checkbox( "ORM map (AO, roughness, metallic)", &merge->ORMMap );
            changed |= ImGui::Checkbox( "Emissive map", &merge->EmissiveMap );
            ImGui::Unindent();
            return changed;
        }

        // THE LAYER'S BUILDER SETTINGS, drawn as the fields of its builder's own object (UE's HLOD layer Details:
        // HLODBuilderSettings is the picked builder class's UHLODBuilderSettings subclass - MeshMerge's and
        // MeshSimplify's merged material, MeshSimplify's triangle share, MeshApproximate's approximation), never
        // as fields of the layer. Read as the builder reads them (Rules::ReadHLODBuilderSettings, strict): what it
        // refuses is shown as its refusal and not edited over. @return the whole new Settings object once a field
        // is committed.
        std::optional<Common::Json::Value> DrawBuilderSettings( const ::Desert::Core::HLODLayerSerialized& layer )
        {
            namespace Core                    = ::Desert::Core;
            namespace Rules                   = ::Desert::Core::Rules;
            const Common::Json::Value* stated = layer.Settings.has_value() ? &*layer.Settings : nullptr;
            const auto                 refuse = []( const std::string& why )
            { ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.35f, 1.0f ), "%s", why.c_str() ); };
            switch ( layer.Type )
            {
                case Core::HLODLayerType::Instancing:
                    ImGui::TextDisabled(
                         "Instancing reads no settings: each distinct mesh is one instanced batch." );
                    return std::nullopt;
                case Core::HLODLayerType::MeshMerge:
                {
                    Core::HLODMeshMergeSettings settings;
                    if ( stated != nullptr )
                    {
                        auto read = Rules::ReadHLODBuilderSettings<Core::HLODMeshMergeSettings>( stated );
                        if ( !read )
                        {
                            refuse( "MeshMerge: " + read.GetError() );
                            return std::nullopt;
                        }
                        settings = read.ExtractValue();
                    }
                    if ( DrawMergeMaterials( settings.MergeMaterials ) )
                        return Common::Json::FromStruct( settings );
                    return std::nullopt;
                }
                case Core::HLODLayerType::MeshSimplify:
                {
                    Core::HLODMeshSimplifySettings settings;
                    if ( stated != nullptr )
                    {
                        auto read = Rules::ReadHLODBuilderSettings<Core::HLODMeshSimplifySettings>( stated );
                        if ( !read )
                        {
                            refuse( "MeshSimplify: " + read.GetError() );
                            return std::nullopt;
                        }
                        settings = read.ExtractValue();
                    }
                    if ( !settings.TrianglePercent.has_value() )
                        refuse(
                             "MeshSimplify states no TrianglePercent: Build HLODs refuses the layer until one is "
                             "set." );
                    float percent = settings.TrianglePercent.value_or( 0.0f );
                    bool  changed = CommitFloat( "Triangles kept", percent, "%.1f %%", 100.0f );
                    if ( changed )
                        settings.TrianglePercent = percent;
                    changed |= DrawMergeMaterials( settings.MergeMaterials );
                    if ( changed )
                        return Common::Json::FromStruct( settings );
                    return std::nullopt;
                }
                case Core::HLODLayerType::MeshApproximate:
                {
                    auto read = Rules::ReadHLODBuilderSettings<Core::HLODApproximationSettings>( stated );
                    if ( !read )
                    {
                        refuse( "MeshApproximate: " + read.GetError() );
                        return std::nullopt;
                    }
                    Core::HLODApproximationSettings settings = read.ExtractValue();
                    bool                            changed  = false;
                    changed |= CommitFloat( "Accuracy (cm)", settings.WorldSpaceApproximationAccuracy, "%.1f" );
                    changed |= CommitInt( "Max voxel dimension", settings.ClampVoxelDimension );
                    changed |= CommitFloat( "Winding threshold", settings.WindingThreshold, "%.2f" );
                    changed |= ImGui::Checkbox( "Fill gaps", &settings.FillGaps );
                    if ( settings.FillGaps )
                        changed |= CommitFloat( "Gap distance (cm)", settings.GapDistance, "%.1f" );
                    changed |= CommitFloat( "Geometric deviation (cm)", settings.GeometricDeviation, "%.1f" );
                    changed |= CommitInt( "Texture size", settings.TextureSize );
                    changed |= CommitInt( "Capture resolution", settings.CaptureResolution );
                    constexpr const char* kNormals[] = { "Smooth", "Hard edges" };
                    int                   normals    = static_cast<int>( settings.NormalMode );
                    ImGui::SetNextItemWidth( 140.0f );
                    if ( ImGui::Combo( "Normals", &normals, kNormals, IM_ARRAYSIZE( kNormals ) ) &&
                         normals != static_cast<int>( settings.NormalMode ) )
                    {
                        settings.NormalMode = static_cast<Core::HLODApproximationNormals>( normals );
                        changed             = true;
                    }
                    if ( settings.NormalMode == Core::HLODApproximationNormals::HardEdges )
                        changed |= CommitFloat( "Hard normal angle (deg)", settings.HardNormalAngle, "%.0f" );
                    if ( changed )
                        return Common::Json::FromStruct( settings );
                    return std::nullopt;
                }
                case Core::HLODLayerType::Custom:
                {
                    // A project builder's settings are its own struct, which the editor does not know: shown as
                    // stated, set with `set world WorldPartition.HLODLayers` (ControlService).
                    const std::string text =
                         stated != nullptr ? Common::Json::Write( *stated ) : std::string( "none" );
                    ImGui::TextWrapped( "Settings of builder '%s': %s", layer.Builder.value_or( "" ).c_str(),
                                        text.c_str() );
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }
    } // namespace

    // UE's UHLODLayer, edited where UE's World Settings > World Partition Setup edits the partition: one row per
    // layer of the world (Name, Builder = UE HLODBuilderClass, Spatially Loaded + Cell Size + Loading Range, who
    // names it), New and Delete, under the table each layer's BUILDER SETTINGS (UE
    // UHLODLayer::HLODBuilderSettings: the picked builder's own settings object, DrawBuilderSettings), and the
    // world's ChunkRegionSize. Which layers a partition's cells are built with is its HLOD setups' choice
    // (DrawGridHLODLayers). Every commit is ONE history entry through EditWorldPartition - refused whole, by name,
    // by Rules::ValidateHLODLayers; what a builder says of its settings is the red list above the sections
    // (Rules::HLODLayerBuilderIssues).
    void WorldPartitionPanel::DrawHLODLayers()
    {
        if ( !ImGui::CollapsingHeader( "HLOD layers" ) )
            return;
        namespace Core                                              = ::Desert::Core;
        const Core::WorldPartitionSerialized                current = *m_Scene->GetWorldPartition();
        static const std::vector<Core::HLODLayerSerialized> kNoLayers;
        const std::vector<Core::HLODLayerSerialized>&       layers = current.HLODLayers.value_or( kNoLayers );
        // UE UHLODLayer's defaults for a spatially loaded layer (cm).
        constexpr float kDefaultCellSize     = 25600.0f;
        constexpr float kDefaultLoadingRange = 51200.0f;
        // UE's builder class picker: every builder this process registered (Core::HLODBuilders).
        const std::vector<std::string> builderNames = Core::HLODBuilders().Names();

        // Who names each layer: the partitions' HLOD setups, and entities (their HLODLayer block - the plan's
        // composites, which over the descriptor index include those not loaded, and the loaded ones).
        std::vector<std::size_t> setupUsers( layers.size(), 0 ), entityUsers( layers.size(), 0 ),
             liveUsers( layers.size(), 0 );
        for ( std::size_t i = 0; i < layers.size(); ++i )
            for ( const Core::RuntimePartitionSerialized& runtime : current.RuntimePartitions )
                if ( runtime.HLODSetups.has_value() )
                    for ( const Core::RuntimePartitionHLODSetupSerialized& setup : *runtime.HLODSetups )
                        if ( setup.HLODLayer == layers[i].Name )
                            ++setupUsers[i];
        if ( m_EditPlan && m_EditPartition == current )
            for ( const Core::Rules::PlannedComposite& composite : m_EditPlan->Composites )
                if ( composite.HLODLayer.has_value() && *composite.HLODLayer < entityUsers.size() )
                    entityUsers[*composite.HLODLayer] += composite.Members.size();
        {
            auto& registry = m_Scene->GetRegistry();
            auto  view     = registry.view<::Desert::ECS::HLODLayerComponent>();
            for ( const auto handle : view )
                if ( const auto index = Core::Rules::HLODLayerIndexOf(
                          current, view.get<::Desert::ECS::HLODLayerComponent>( handle ).Data.Layer );
                     index.has_value() )
                    ++liveUsers[*index];
        }
        const auto entitiesNaming = [&]( std::size_t i ) { return std::max( entityUsers[i], liveUsers[i] ); };
        const auto usedBy         = [&]( std::size_t i )
        {
            return std::format( "{} HLOD setup(s), {} entit{}", setupUsers[i], entitiesNaming( i ),
                                entitiesNaming( i ) == 1 ? "y" : "ies" );
        };

        std::optional<Core::WorldPartitionSerialized> after;
        std::string                                   label;
        const auto edit = [&]( std::string what ) -> std::vector<Core::HLODLayerSerialized>&
        {
            after = current;
            label = std::move( what );
            return *after->HLODLayers;
        };
        if ( layers.empty() )
            ImGui::TextDisabled( "This world states no HLOD layer." );
        else if ( ImGui::BeginTable( "##hlodlayers", 7,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp ) )
        {
            ImGui::TableSetupColumn( "Name" );
            ImGui::TableSetupColumn( "Builder" );
            ImGui::TableSetupColumn( "Spatially Loaded" );
            ImGui::TableSetupColumn( "Cell size (cm)" );
            ImGui::TableSetupColumn( "Loading range (cm)" );
            ImGui::TableSetupColumn( "Used by" );
            ImGui::TableSetupColumn( "" );
            ImGui::TableHeadersRow();
            constexpr ImGuiInputTextFlags kCommit = ImGuiInputTextFlags_EnterReturnsTrue;
            for ( std::size_t i = 0; i < layers.size() && !after; ++i )
            {
                const Core::HLODLayerSerialized& layer = layers[i];
                ImGui::PushID( static_cast<int>( i ) );
                ImGui::TableNextRow();

                // Name: a rename carries every HLOD setup naming the layer; refused while an entity names it (an
                // entity on disk cannot be renamed with the block, so its name would dangle).
                ImGui::TableNextColumn();
                char name[128];
                std::snprintf( name, sizeof( name ), "%s", layer.Name.c_str() );
                ImGui::SetNextItemWidth( -FLT_MIN );
                if ( ImGui::InputText( "##name", name, sizeof( name ), kCommit ) && layer.Name != name )
                {
                    if ( entitiesNaming( i ) > 0 )
                        m_HLODLayerStatus = std::format( "Rename '{}': {} entit{} name it as their HLOD Layer - "
                                                         "give them another layer first.",
                                                         layer.Name, entitiesNaming( i ),
                                                         entitiesNaming( i ) == 1 ? "y" : "ies" );
                    else
                    {
                        const std::string renamed( name );
                        edit( std::format( "Rename HLOD layer {} to {}", layer.Name, renamed ) )[i].Name = renamed;
                        for ( Core::RuntimePartitionSerialized& runtime : after->RuntimePartitions )
                            if ( runtime.HLODSetups.has_value() )
                                for ( Core::RuntimePartitionHLODSetupSerialized& setup : *runtime.HLODSetups )
                                    if ( setup.HLODLayer == layer.Name )
                                        setup.HLODLayer = renamed;
                    }
                }

                // Builder (UE UHLODLayer::HLODBuilderClass): a built-in one is the layer's Type, a project's a
                // Custom layer naming it. A new pick starts that builder's settings object
                // (DefaultBuilderSettings), as UE creates the class's settings when the class is picked - one
                // builder's settings never carry over to another's.
                ImGui::TableNextColumn();
                const std::string builder( Core::Rules::HLODBuilderNameOf( layer ) );
                ImGui::SetNextItemWidth( -FLT_MIN );
                if ( ImGui::BeginCombo( "##builder", builder.empty() ? "(none)" : builder.c_str() ) )
                {
                    std::optional<std::string> picked;
                    for ( const std::string& candidate : builderNames )
                        if ( ImGui::Selectable( candidate.c_str(), candidate == builder ) && candidate != builder )
                            picked = candidate;
                    ImGui::EndCombo();
                    if ( picked )
                    {
                        Core::HLODLayerSerialized& changed =
                             edit( std::format( "Set builder of HLOD layer {} to {}", layer.Name, *picked ) )[i];
                        changed.Type    = Core::HLODLayerType::Custom;
                        changed.Builder = *picked;
                        for ( const auto& [type, builtIn] : Core::Rules::kBuiltInHLODBuilders )
                            if ( builtIn == *picked )
                            {
                                changed.Type = type;
                                changed.Builder.reset();
                            }
                        changed.Settings = DefaultBuilderSettings( changed.Type );
                    }
                }

                // Spatially Loaded (UE UHLODLayer::bIsSpatiallyLoaded): both numbers or neither. Whether a setup
                // chain may end on it is the partition's rule (Rules::ValidateHLODLayers names why it refuses).
                ImGui::TableNextColumn();
                bool spatial = layer.CellSize.has_value();
                if ( ImGui::Checkbox( "##spatial", &spatial ) )
                {
                    Core::HLODLayerSerialized& changed = edit( std::format(
                         "{} HLOD layer {} spatially loaded", spatial ? "Make" : "Stop making", layer.Name ) )[i];
                    if ( spatial )
                    {
                        changed.CellSize     = kDefaultCellSize;
                        changed.LoadingRange = kDefaultLoadingRange;
                    }
                    else
                    {
                        changed.CellSize.reset();
                        changed.LoadingRange.reset();
                    }
                }
                ImGui::TableNextColumn();
                if ( layer.CellSize.has_value() )
                {
                    float cell = *layer.CellSize;
                    ImGui::SetNextItemWidth( -FLT_MIN );
                    if ( ImGui::InputFloat( "##cell", &cell, 0.0f, 0.0f, "%.0f", kCommit ) &&
                         cell != *layer.CellSize )
                        edit( std::format( "Set cell size of HLOD layer {}", layer.Name ) )[i].CellSize = cell;
                }
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                if ( layer.LoadingRange.has_value() )
                {
                    float range = *layer.LoadingRange;
                    ImGui::SetNextItemWidth( -FLT_MIN );
                    if ( ImGui::InputFloat( "##range", &range, 0.0f, 0.0f, "%.0f", kCommit ) &&
                         range != *layer.LoadingRange )
                        edit( std::format( "Set loading range of HLOD layer {}", layer.Name ) )[i].LoadingRange =
                             range;
                }
                else
                    ImGui::TextDisabled( "-" );

                ImGui::TableNextColumn();
                ImGui::TextUnformatted( usedBy( i ).c_str() );
                ImGui::TableNextColumn();
                if ( ImGui::SmallButton( "Delete" ) )
                {
                    if ( setupUsers[i] + entitiesNaming( i ) > 0 )
                        m_HLODLayerStatus =
                             std::format( "Delete '{}': it is named by {} - name another layer there "
                                          "first.",
                                          layer.Name, usedBy( i ) );
                    else
                    {
                        std::vector<Core::HLODLayerSerialized>& list =
                             edit( std::format( "Delete HLOD layer {}", layer.Name ) );
                        list.erase( list.begin() + static_cast<std::ptrdiff_t>( i ) );
                        if ( list.empty() )
                            after->HLODLayers.reset(); // no layers is no key, never an empty list
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Each layer's builder settings, under the builder that reads them.
        for ( std::size_t i = 0; i < layers.size() && !after; ++i )
        {
            const Core::HLODLayerSerialized& layer = layers[i];
            const std::string_view           named = Core::Rules::HLODBuilderNameOf( layer );
            ImGui::PushID( static_cast<int>( i ) );
            if ( ImGui::TreeNode( "##buildersettings", "%s - %s settings", layer.Name.c_str(),
                                  std::string( named ).c_str() ) )
            {
                if ( std::optional<Common::Json::Value> settings = DrawBuilderSettings( layer );
                     settings.has_value() )
                    edit( std::format( "Set {} settings of HLOD layer {}", named, layer.Name ) )[i].Settings =
                         std::move( settings );
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        ImGui::SetNextItemWidth( 200.0f );
        ImGui::InputTextWithHint( "##newhlodlayer", "New HLOD layer name", m_NewHLODLayer.data(),
                                  m_NewHLODLayer.size() );
        ImGui::SameLine();
        if ( ImGui::Button( "New HLOD Layer" ) && !after )
        {
            // UE's new UHLODLayer: the Instancing builder, not spatially loaded (the last setup of a chain until
            // it is given a cell size and range).
            after = current;
            if ( !after->HLODLayers )
                after->HLODLayers.emplace();
            Core::HLODLayerSerialized layer;
            layer.Name = std::string( m_NewHLODLayer.data() );
            label      = std::format( "New HLOD layer {}", layer.Name );
            after->HLODLayers->push_back( std::move( layer ) );
        }
        // ChunkRegionSize: the side of the square region (cm) whose cells a packaged build ships in one content
        // chunk; off = the world ships in the base chunk.
        bool  chunked = current.ChunkRegionSize.has_value();
        float region  = current.ChunkRegionSize.value_or( 0.0f );
        if ( ImGui::Checkbox( "Chunked packaging", &chunked ) && !after )
        {
            after = current;
            // A 1 km region to start from (the Protocol world's); the field below sets the real one.
            after->ChunkRegionSize = chunked ? std::optional<float>( 100000.0f ) : std::nullopt;
            label                  = chunked ? "Ship the world in chunks" : "Ship the world in the base chunk";
        }
        if ( current.ChunkRegionSize.has_value() )
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 160.0f );
            if ( ImGui::InputFloat( "Chunk region (cm)", &region, 0.0f, 0.0f, "%.0f",
                                    ImGuiInputTextFlags_EnterReturnsTrue ) &&
                 region != *current.ChunkRegionSize && !after )
            {
                if ( !( std::isfinite( region ) && region > 0.0f ) )
                    m_HLODLayerStatus =
                         std::format( "Chunk region {:f} cm: it must be positive and finite", region );
                else
                {
                    after                  = current;
                    after->ChunkRegionSize = region;
                    label                  = "Set chunk region size";
                }
            }
        }

        if ( after )
        {
            const bool created = label.starts_with( "New HLOD layer" );
            const auto edited  = EditWorldPartition( m_Scene, std::move( label ), std::move( *after ) );
            m_HLODLayerStatus  = edited ? std::string() : edited.GetError();
            m_EditPlanStale    = true; // the Used by counts are the plan's
            if ( edited && created )
                m_NewHLODLayer.fill( '\0' );
        }
        if ( !m_HLODLayerStatus.empty() )
            ImGui::TextWrapped( "%s", m_HLODLayerStatus.c_str() );
    }

    Common::BoolResultStr WorldPartitionPanel::LoadSelectedRegion()
    {
        if ( !m_Selection )
            return Common::MakeError<bool>(
                 std::string( "Load Region: no region is selected - drag on the World Partition map." ) );
        RegionRequest request;
        request.Regions.push_back( *m_Selection );
        return LoadRegion( request );
    }

    Common::BoolResultStr WorldPartitionPanel::LoadRegion( const RegionRequest& request )
    {
        if ( !m_Scene )
            return Common::MakeError<bool>( std::string( "No active scene." ) );
        namespace Regions = ::Desert::Core::EditorRegions;
        // The world's Location Volumes join the loaders first (RebuildEditPlan: EditorRegions::WithWorldVolumes),
        // so Unload All also switches off an Auto Load volume this user never named.
        if ( auto onDisk = ReadWorldOnDisk( request.Whole ? "Load All" : "Load Region" ); !onDisk )
            return onDisk;
        Regions::HeldRegions next = Held();
        if ( request.Whole )
            next.Whole = true;
        else if ( request.Regions.empty() )
        {
            // Unload All: every loader off but the pins, which hold their entities whatever the others do (UE
            // FLoaderAdapterPinnedActors); the rectangles and lists made by hand go.
            next.Whole = false;
            next.Regions.clear();
            next.EntityLists.clear();
            for ( Regions::VolumeLoader& volume : next.Volumes )
                volume.Loaded = false;
        }
        else
            for ( const ::Desert::Core::Rules::CellBounds& region : request.Regions )
            {
                // The same rectangle again is the listed loader switched on, not a second row naming one place.
                const auto listed = std::find_if( next.Regions.begin(), next.Regions.end(),
                                                  [&region]( const Regions::RegionLoader& loader )
                                                  {
                                                      return loader.Region.MinX == region.MinX &&
                                                             loader.Region.MaxX == region.MaxX &&
                                                             loader.Region.MinZ == region.MinZ &&
                                                             loader.Region.MaxZ == region.MaxZ;
                                                  } );
                if ( listed != next.Regions.end() )
                    listed->Loaded = true;
                else
                    next.Regions.push_back( Regions::RegionLoader{ region, true } );
            }
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    Common::BoolResultStr WorldPartitionPanel::Reload( ::Desert::Core::EditorRegions::HeldRegions         next,
                                                       const ::Desert::Core::EditorRegions::UnsavedPolicy policy )
    {
        if ( !m_Scene )
            return Common::MakeError<bool>( std::string( "No active scene." ) );
        auto changed = ::Desert::Core::EditorRegions::LoadHeld( *m_Scene, m_Assets, next, policy );
        if ( !changed )
            return Common::MakeError<bool>( changed.GetError() );
        const auto& outcome = changed.GetValue();
        if ( !outcome.Unsaved.empty() )
        {
            // Nothing changed: the prompt asks (DrawUnloadPrompt), and its answer calls this again.
            const std::size_t count = outcome.Unsaved.size();
            m_PendingUnload         = PendingUnload{ std::move( next ), outcome.Unsaved, true };
            m_RegionStatus = std::format( "{} unsaved {} would be unloaded: Save, Discard or Cancel.", count,
                                          count == 1 ? "entity" : "entities" );
            return BOOLSUCCESS;
        }
        if ( outcome.Loaded > 0 || outcome.Unloaded > 0 )
            CommandHistory::Get().Clear();
        m_RegionStatus = std::format(
             "{} record(s) loaded, {} unloaded; {} of the world left on disk ({} composite(s) in the regions and "
             "volumes, {} held by entity lists and pins, {} always loaded, {} without a place, {} in unloaded data "
             "layers).",
             outcome.Loaded, outcome.Unloaded, outcome.NotLoaded, outcome.Selection.InRegions,
             outcome.Selection.ByEntities, outcome.Selection.AlwaysLoaded, outcome.Selection.Unplaced,
             outcome.Selection.UnloadedByDataLayers );
        m_EditPlanStale = true;
        // The HLODs of the cells that arrived hide and those of the cells that left show, before the next frame.
        if ( const auto synced = m_EditorHLODs.SyncHeld( *m_Scene, EditorLayersLoaded() ); !synced )
            m_RegionStatus += " " + synced.GetError();
        // The next open of this world holds what it holds now (UE's per-user loaded regions and volumes). The change
        // itself stands either way; a refusal here is the status line's, so the user knows the next open will differ.
        m_Held     = std::move( next );
        m_HeldRead = true;
        if ( const auto& baseline = m_Scene->Packages()->BaselinePath() )
            if ( const auto kept = EditorLoadedRegions::Remember( *baseline, m_Held ); !kept )
                m_RegionStatus += " The loaders were not kept for the next open: " + kept.GetError();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldPartitionPanel::ReadWorldOnDisk( const std::string& what )
    {
        if ( !m_Scene )
            return Common::MakeError<bool>( std::format( "{}: no active scene.", what ) );
        RebuildEditPlan();
        if ( !m_PlanFromIndex || !m_EditPlan.has_value() )
            return Common::MakeError<bool>( std::format(
                 "{}: the loaders hold a partitioned world opened from its files, and this scene is not one{}{}",
                 what, m_EditPlanStatus.empty() ? "" : " - ", m_EditPlanStatus ) );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldPartitionPanel::AddLocationVolume()
    {
        if ( !m_Scene || !m_Scene->GetWorldPartition().has_value() )
            return Common::MakeError<bool>( std::string(
                 "Add Location Volume: the scene is not a partitioned world, and a Location Volume is a loader of "
                 "one." ) );
        auto volume = m_Scene->CreateNewEntity( "Location Volume" );
        volume.AddComponent<::Desert::ECS::LocationVolumeComponent>();
        const Common::UUID id = volume.GetComponent<::Desert::ECS::UUIDComponent>().UUID;
        Commands::NotifyCreated( { id } );
        ::Desert::Editor::Core::SelectionManager::SetSelected( id );
        m_RegionStatus =
             "Location Volume added at the origin: set its Tag, place and Extent, save the scene, then "
             "Load it in Loaders.";
        return BOOLSUCCESS;
    }

    std::vector<std::string> WorldPartitionPanel::LocationVolumeNames() const
    {
        std::vector<std::string> names;
        if ( !m_Scene )
            return names;
        // Every volume is always loaded in the editor, so the scene names each one; a volume with no tag has no
        // name to address (its row is "Location Volume <id>").
        auto view =
             m_Scene->GetRegistry().view<::Desert::ECS::LocationVolumeComponent, ::Desert::ECS::TagComponent>();
        for ( const auto entity : view )
        {
            const std::string& tag = view.get<::Desert::ECS::TagComponent>( entity ).Tag;
            if ( !tag.empty() && std::find( names.begin(), names.end(), tag ) == names.end() )
                names.push_back( tag );
        }
        return names;
    }

    Common::BoolResultStr WorldPartitionPanel::SetLocationVolumeLoaded( const std::string& name,
                                                                        const bool         loaded )
    {
        namespace Regions      = ::Desert::Core::EditorRegions;
        const std::string what = std::format( "{} location volume '{}'", loaded ? "Load" : "Unload", name );
        if ( auto onDisk = ReadWorldOnDisk( what ); !onDisk )
            return onDisk;
        std::vector<std::uint64_t> tagged;
        for ( const Regions::LocationVolume& volume : m_Volumes )
            if ( volume.Name == name )
                tagged.push_back( volume.Id );
        if ( tagged.empty() )
            return Common::MakeError<bool>(
                 std::format( "{}: the saved world has no Location Volume tagged '{}' - a "
                              "new or renamed volume is a loader once the scene is saved.",
                              what, name ) );
        if ( tagged.size() > 1 )
            return Common::MakeError<bool>(
                 std::format( "{}: {} saved Location Volumes are tagged '{}'; give each its own tag.", what,
                              tagged.size(), name ) );
        Regions::HeldRegions next = Held();
        for ( Regions::VolumeLoader& volume : next.Volumes )
            if ( volume.Volume == tagged.front() )
                volume.Loaded = loaded;
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    Common::BoolResultStr WorldPartitionPanel::LoadRegionFromSelection( const std::vector<Common::UUID>& selected )
    {
        namespace Regions = ::Desert::Core::EditorRegions;
        if ( selected.empty() )
            return Common::MakeError<bool>( std::string( "Load Region from Selection: nothing is selected." ) );
        if ( auto onDisk = ReadWorldOnDisk( "Load Region from Selection" ); !onDisk )
            return onDisk;
        const std::vector<std::uint64_t> ids     = IdsOf( selected );
        std::vector<std::uint64_t>       records = Regions::RecordsAroundSelection( *m_EditPlan, m_RowIds, ids );
        if ( records.empty() )
            return Common::MakeError<bool>(
                 std::format( "Load Region from Selection: none of the {} selected entities is a placed record of "
                              "the saved world (save new entities first; the always-loaded part needs no loader).",
                              selected.size() ) );
        Regions::HeldRegions next = Held();
        next.EntityLists.push_back( Regions::EntityListLoader{ std::move( records ), true } );
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    Common::BoolResultStr WorldPartitionPanel::UnloadSelected( const std::vector<Common::UUID>& selected )
    {
        namespace Regions = ::Desert::Core::EditorRegions;
        if ( selected.empty() )
            return Common::MakeError<bool>( std::string( "Unload Selected: nothing is selected." ) );
        if ( auto onDisk = ReadWorldOnDisk( "Unload Selected" ); !onDisk )
            return onDisk;
        const std::vector<std::uint64_t> ids = IdsOf( selected );
        if ( auto reloaded = Reload( Regions::WithoutEntities( Held(), *m_EditPlan, m_RowIds, ids ),
                                     Regions::UnsavedPolicy::Ask );
             !reloaded )
            return reloaded;
        const std::size_t held = static_cast<std::size_t>(
             std::count_if( selected.begin(), selected.end(),
                            [this]( const Common::UUID& id ) { return m_Scene->Packages()->IsLoaded( id ); } ) );
        if ( held > 0 )
            m_RegionStatus +=
                 std::format( " {} of the selected entities stay loaded: a region, a Location Volume, a "
                              "pin, Load All or the always-loaded part holds them.",
                              held );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldPartitionPanel::SetSelectedPinned( const std::vector<Common::UUID>& selected,
                                                                  const bool                       pinned )
    {
        namespace Regions      = ::Desert::Core::EditorRegions;
        const std::string what = pinned ? "Pin Selected" : "Unpin Selected";
        if ( selected.empty() )
            return Common::MakeError<bool>( std::format( "{}: nothing is selected.", what ) );
        if ( auto onDisk = ReadWorldOnDisk( what ); !onDisk )
            return onDisk;
        const std::vector<std::uint64_t> ids  = IdsOf( selected );
        Regions::HeldRegions             next = Held();
        if ( pinned )
        {
            const std::unordered_set<std::uint64_t> records( m_RowIds.begin(), m_RowIds.end() );
            for ( const std::uint64_t id : ids )
            {
                if ( !records.contains( id ) )
                    return Common::MakeError<bool>( std::format(
                         "{}: entity {} is not a record of the saved world - save it first.", what, id ) );
                if ( std::find( next.Pinned.begin(), next.Pinned.end(), id ) == next.Pinned.end() )
                    next.Pinned.push_back( id );
            }
        }
        else
            std::erase_if( next.Pinned, [&ids]( const std::uint64_t id )
                           { return std::find( ids.begin(), ids.end(), id ) != ids.end(); } );
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    Common::BoolResultStr WorldPartitionPanel::UnpinAll()
    {
        if ( auto onDisk = ReadWorldOnDisk( "Unpin All" ); !onDisk )
            return onDisk;
        ::Desert::Core::EditorRegions::HeldRegions next = Held();
        next.Pinned.clear();
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    std::vector<std::string> WorldPartitionPanel::LoaderNames() const
    {
        std::vector<std::string> names;
        for ( ::Desert::Core::EditorRegions::LoaderRow& row :
              ::Desert::Core::EditorRegions::LoaderRowsOf( m_Held, m_Volumes ) )
            names.push_back( std::move( row.Name ) );
        return names;
    }

    std::optional<::Desert::Core::EditorRegions::LoaderRow>
    WorldPartitionPanel::FindLoader( const std::string& name ) const
    {
        for ( ::Desert::Core::EditorRegions::LoaderRow& row :
              ::Desert::Core::EditorRegions::LoaderRowsOf( m_Held, m_Volumes ) )
            if ( row.Name == name )
                return std::move( row );
        return std::nullopt;
    }

    Common::BoolResultStr WorldPartitionPanel::SetLoaderLoaded( const std::string& name, const bool loaded )
    {
        namespace Regions      = ::Desert::Core::EditorRegions;
        using Kind             = Regions::LoaderKind;
        const std::string what = std::format( "{} loader '{}'", loaded ? "Load" : "Unload", name );
        if ( auto onDisk = ReadWorldOnDisk( what ); !onDisk )
            return onDisk;
        const std::optional<Regions::LoaderRow> row = FindLoader( name );
        if ( !row )
            return Common::MakeError<bool>( std::format( "{}: no loader is named '{}'.", what, name ) );
        Regions::HeldRegions next = Held();
        switch ( row->Kind )
        {
            case Kind::Whole:
                next.Whole = loaded;
                break;
            case Kind::Volume:
                next.Volumes[row->Index].Loaded = loaded;
                break;
            case Kind::Region:
                next.Regions[row->Index].Loaded = loaded;
                break;
            case Kind::EntityList:
                next.EntityLists[row->Index].Loaded = loaded;
                break;
            case Kind::Pinned:
                return Common::MakeError<bool>( std::format( "{}: the pins hold their entities whatever the other "
                                                             "loaders do - Unpin All, or Unpin Selected.",
                                                             what ) );
        }
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    Common::BoolResultStr WorldPartitionPanel::RemoveLoader( const std::string& name )
    {
        namespace Regions      = ::Desert::Core::EditorRegions;
        using Kind             = Regions::LoaderKind;
        const std::string what = std::format( "Remove loader '{}'", name );
        if ( auto onDisk = ReadWorldOnDisk( what ); !onDisk )
            return onDisk;
        const std::optional<Regions::LoaderRow> row = FindLoader( name );
        if ( !row )
            return Common::MakeError<bool>( std::format( "{}: no loader is named '{}'.", what, name ) );
        Regions::HeldRegions next = Held();
        switch ( row->Kind )
        {
            case Kind::Whole: // Load All has no state but on: removing it is switching it off
                next.Whole = false;
                break;
            case Kind::Region:
                next.Regions.erase( next.Regions.begin() + static_cast<std::ptrdiff_t>( row->Index ) );
                break;
            case Kind::EntityList:
                next.EntityLists.erase( next.EntityLists.begin() + static_cast<std::ptrdiff_t>( row->Index ) );
                break;
            case Kind::Volume:
                return Common::MakeError<bool>( std::format(
                     "{}: a Location Volume is the world's - Unload it, or delete its entity and save.", what ) );
            case Kind::Pinned:
                return Common::MakeError<bool>( std::format( "{}: the pins go with Unpin All.", what ) );
        }
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    // UE's World Partition editor loader list, the pattern and not the letter: one row per loader of this user
    // (the world's Location Volumes, the regions, the entity lists, the pins, Load All), each with its Loaded
    // switch, and the selection actions that make or change one.
    void WorldPartitionPanel::DrawLoaders()
    {
        namespace Regions = ::Desert::Core::EditorRegions;
        if ( !ImGui::CollapsingHeader( "Loaders", ImGuiTreeNodeFlags_DefaultOpen ) )
            return;
        const std::vector<Common::UUID> selected = ::Desert::Editor::Core::SelectionManager::GetSelection();
        const auto                      report   = [this]( const Common::BoolResultStr& done )
        {
            if ( !done )
                m_RegionStatus = done.GetError();
        };
        if ( ImGui::Button( "Add Location Volume" ) )
            report( AddLocationVolume() );
        ImGui::SameLine();
        ImGui::BeginDisabled( selected.empty() );
        if ( ImGui::Button( "Load Region from Selection" ) )
            report( LoadRegionFromSelection( selected ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Unload Selected" ) )
            report( UnloadSelected( selected ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Pin" ) )
            report( SetSelectedPinned( selected, true ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Unpin" ) )
            report( SetSelectedPinned( selected, false ) );
        ImGui::EndDisabled();

        // This frame's rows: an action reloads and rewrites what they were made from, so the list stops after one.
        const std::vector<Regions::LoaderRow> rows = Regions::LoaderRowsOf( m_Held, m_Volumes );
        if ( rows.empty() )
            ImGui::TextDisabled( "No loader: the editor holds the always-loaded part only." );
        for ( const Regions::LoaderRow& row : rows )
        {
            ImGui::PushID( row.Name.c_str() );
            bool acted  = false;
            bool loaded = row.Loaded;
            ImGui::BeginDisabled( row.Kind == Regions::LoaderKind::Pinned );
            if ( ImGui::Checkbox( "##loaded", &loaded ) )
            {
                report( SetLoaderLoaded( row.Name, loaded ) );
                acted = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted( LoaderLabel( row ).c_str() );
            if ( !acted &&
                 ( row.Kind == Regions::LoaderKind::Region || row.Kind == Regions::LoaderKind::EntityList ) )
            {
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Remove" ) )
                {
                    report( RemoveLoader( row.Name ) );
                    acted = true;
                }
            }
            if ( !acted && row.Kind == Regions::LoaderKind::Pinned )
            {
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Unpin All" ) )
                {
                    report( UnpinAll() );
                    acted = true;
                }
            }
            ImGui::PopID();
            if ( acted )
                break;
        }
    }

    bool WorldPartitionPanel::IsPinned( const std::uint64_t record )
    {
        const std::vector<std::uint64_t>& pinned = Held().Pinned;
        return std::find( pinned.begin(), pinned.end(), record ) != pinned.end();
    }

    Common::BoolResultStr WorldPartitionPanel::SetPinned( const std::uint64_t record, const bool pinned )
    {
        return SetPinned( std::span<const std::uint64_t>( &record, 1 ), pinned );
    }

    Common::BoolResultStr WorldPartitionPanel::SetPinned( const std::span<const std::uint64_t> records,
                                                          const bool                           pinned )
    {
        if ( !m_Scene || !m_Scene->GetWorldPartition().has_value() )
            return Common::MakeError<bool>( std::string( "Pin: the world is not partitioned." ) );
        if ( Playing() )
            return Common::MakeError<bool>(
                 std::string( "Pin: not during Play - the played world is all loaded." ) );
        ::Desert::Core::EditorRegions::HeldRegions next  = Held();
        bool                                       moved = false;
        for ( const std::uint64_t record : records )
        {
            if ( IsPinned( record ) == pinned )
                continue;
            moved = true;
            if ( pinned )
                next.Pinned.push_back( record );
            else
                std::erase( next.Pinned, record );
        }
        if ( !moved )
            return Common::MakeSuccess( false );
        // The same loaders again, with the record held or left to them (the pins loader, WP-LOADER-ADAPTERS).
        return Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask );
    }

    void WorldPartitionPanel::DrawUnloadPrompt()
    {
        constexpr const char* kPrompt = "Unsaved Entities##WorldPartitionUnload";
        if ( m_PendingUnload && m_PendingUnload->Open )
        {
            ImGui::OpenPopup( kPrompt );
            m_PendingUnload->Open = false;
        }
        if ( !ImGui::BeginPopupModal( kPrompt, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;
        if ( !m_PendingUnload )
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const std::vector<::Desert::Core::EditorRegions::UnsavedEntity>& unsaved = m_PendingUnload->Unsaved;
        // Named up to a screenful; the rest is a count, as UE's Save Content dialog scrolls.
        constexpr std::size_t kNamed = 8;
        std::string           names;
        for ( std::size_t i = 0; i < unsaved.size() && i < kNamed; ++i )
            names += i == 0 ? unsaved[i].Name : std::format( ", {}", unsaved[i].Name );
        if ( unsaved.size() > kNamed )
            names += std::format( " and {} more", unsaved.size() - kNamed );
        ImGui::TextUnformatted(
             std::format( "{} unsaved {}: {}", unsaved.size(), unsaved.size() == 1 ? "entity" : "entities", names )
                  .c_str() );
        ImGui::TextDisabled( "They differ from their files (edited or deleted since the last save), and the "
                             "region change would drop that." );
        ImGui::TextDisabled( "Save writes the world's unsaved entities, then changes the regions. Discard changes "
                             "them and drops those edits." );
        ImGui::Spacing();
        bool save    = false;
        bool discard = false;
        bool cancel  = false;
        ImGui::BeginDisabled( !m_SaveScene );
        save = ImGui::Button( "Save", ImVec2( 100.0f, 0.0f ) );
        ImGui::EndDisabled();
        ImGui::SameLine();
        discard = ImGui::Button( "Discard", ImVec2( 100.0f, 0.0f ) );
        ImGui::SameLine();
        cancel =
             ImGui::Button( "Cancel", ImVec2( 100.0f, 0.0f ) ) || ImGui::IsKeyPressed( ImGuiKey_Escape, false );
        if ( save || discard || cancel )
        {
            PendingUnload pending = std::move( *m_PendingUnload );
            m_PendingUnload.reset();
            ImGui::CloseCurrentPopup();
            if ( cancel )
                m_RegionStatus = "Cancelled: nothing was loaded or unloaded.";
            else if ( save && !m_SaveScene() )
                m_RegionStatus = "The world was not saved (see the log): nothing was loaded or unloaded.";
            else if ( const auto changed =
                           Reload( std::move( pending.Next ), save ? ::Desert::Core::EditorRegions::UnsavedPolicy::Ask
                                                                   : ::Desert::Core::EditorRegions::UnsavedPolicy::Discard );
                      !changed )
                m_RegionStatus = changed.GetError();
        }
        ImGui::EndPopup();
    }

    void WorldPartitionPanel::TickEditorHLODs()
    {
        if ( !m_Scene )
            return;
        // Play streams the world's own HLODs (PlaySession destroyed these before its snapshot), and Stop's restore
        // is a load: they are made again once the editor is back in Edit.
        if ( m_Scene->GetState() != ::Desert::Core::Scene::SceneState::Edit )
        {
            m_EditorHLODs.Forget();
            m_EditorHLODsStale = true;
            return;
        }
        if ( m_EditorHLODs.Count() > 0 && !m_EditorHLODs.IsInScene( *m_Scene ) )
        {
            m_EditorHLODs.Forget(); // a load, or a Play refused after PlaySession released them
            m_EditorHLODsStale = true;
        }
        if ( m_EditorHLODsStale && !IsBuildingHLODs() )
        {
            m_EditorHLODsStale = false;
            if ( const auto made = m_EditorHLODs.Make( *m_Scene, m_Assets, EditorLayersLoaded() ); !made )
                LOG_ERROR( "[WorldPartition] {0}", made.GetError() );
        }
        const EditorPreferences& prefs = EditorPreferences::Get();
        std::optional<glm::vec3> eye;
        if ( const auto camera = m_Scene->GetActiveCamera() )
            eye = glm::vec3( glm::inverse( camera->GetViewMatrix() )[3] );
        m_EditorHLODs.Apply( *m_Scene,
                             EditorHLODView{ prefs.ShowHLODsInEditor, prefs.HLODInEditorMinDrawDistance,
                                             prefs.HLODInEditorMaxDrawDistance },
                             eye );
    }

    std::vector<bool> WorldPartitionPanel::EditorLayersLoaded()
    {
        if ( !m_Scene || !m_Scene->GetWorldPartition().has_value() )
            return {};
        return ::Desert::Core::EditorRegions::DataLayersLoadedInEditor( *m_Scene->GetWorldPartition(),
                                                                        Held().DataLayers );
    }

    void WorldPartitionPanel::SetShowHLODs( const bool show )
    {
        EditorPreferences::Get().ShowHLODsInEditor = show;
        EditorPreferences::Save();
    }

    Common::BoolResultStr WorldPartitionPanel::SetEditorProperty( const std::string& property,
                                                                  const std::string& text )
    {
        float values[4] = {};
        if ( property == "WorldPartitionEditor.Selection" )
        {
            if ( std::sscanf( text.c_str(), "%f,%f,%f,%f", &values[0], &values[1], &values[2], &values[3] ) != 4 ||
                 values[0] > values[2] || values[1] > values[3] )
                return Common::MakeError( std::format(
                     "WorldPartitionEditor.Selection: '{}' is not minX,minZ,maxX,maxZ (cm, min <= max)", text ) );
            m_Selection = ::Desert::Core::Rules::CellBounds{ values[0], values[1], values[2], values[3] };
            return BOOLSUCCESS;
        }
        if ( property == "WorldPartitionEditor.ShowHLODs" )
        {
            if ( text != "true" && text != "false" )
                return Common::MakeError(
                     std::format( "WorldPartitionEditor.ShowHLODs: '{}' is not true or false", text ) );
            SetShowHLODs( text == "true" );
            return BOOLSUCCESS;
        }
        if ( property == "WorldPartitionEditor.HLODMinDrawDistance" ||
             property == "WorldPartitionEditor.HLODMaxDrawDistance" )
        {
            if ( std::sscanf( text.c_str(), "%f", &values[0] ) != 1 || values[0] < 0.0f )
                return Common::MakeError(
                     std::format( "{}: '{}' is not a distance in cm (0 or more)", property, text ) );
            EditorPreferences& prefs = EditorPreferences::Get();
            if ( property == "WorldPartitionEditor.HLODMinDrawDistance" )
                prefs.HLODInEditorMinDrawDistance = values[0];
            else
                prefs.HLODInEditorMaxDrawDistance = values[0];
            EditorPreferences::Save();
            return BOOLSUCCESS;
        }
        return Common::MakeError( std::format( "'{}' is not WorldPartitionEditor.Selection, .ShowHLODs, "
                                               ".HLODMinDrawDistance or .HLODMaxDrawDistance",
                                               property ) );
    }

    // UE's Show HLODs and HLOD-in-editor draw distances (UWorldPartitionEditorPerProjectUserSettings), beside the
    // grids whose HLODs they show. This user's, not the world's: no undo entry.
    void WorldPartitionPanel::DrawEditorHLODs()
    {
        if ( !ImGui::CollapsingHeader( "HLODs in the editor" ) )
            return;
        EditorPreferences& prefs = EditorPreferences::Get();
        bool               show  = prefs.ShowHLODsInEditor;
        if ( ImGui::Checkbox( "Show HLODs", &show ) )
            SetShowHLODs( show );
        ImGui::DragFloat( "Min draw distance", &prefs.HLODInEditorMinDrawDistance, 100.0f, 0.0f, FLT_MAX,
                          "%.0f cm" );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
            EditorPreferences::Save();
        ImGui::DragFloat( "Max draw distance (0 = no limit)", &prefs.HLODInEditorMaxDrawDistance, 100.0f, 0.0f,
                          FLT_MAX, "%.0f cm" );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
            EditorPreferences::Save();
        const std::string line = std::format( "{} of {} saved HLOD(s) drawn. {}", m_EditorHLODs.DrawnCount(),
                                              m_EditorHLODs.Count(), m_EditorHLODs.Status() );
        ImGui::TextWrapped( "%s", line.c_str() );
    }

    std::vector<std::string> WorldPartitionPanel::DataLayerNames() const
    {
        std::vector<std::string> names;
        if ( m_Scene && m_Scene->GetWorldPartition().has_value() && m_Scene->GetWorldPartition()->DataLayers )
            for ( const ::Desert::Core::DataLayerSerialized& layer : *m_Scene->GetWorldPartition()->DataLayers )
                names.push_back( layer.Name );
        return names;
    }

    Common::BoolResultStr WorldPartitionPanel::SetDataLayerLoaded( const std::string& name, const bool loaded )
    {
        if ( !m_Scene || !m_Scene->GetWorldPartition().has_value() )
            return Common::MakeError<bool>( std::string( "Data Layers: the world is not partitioned." ) );
        const ::Desert::Core::WorldPartitionSerialized& partition = *m_Scene->GetWorldPartition();
        if ( !::Desert::Core::Rules::DataLayerIndexOf( partition, name ).has_value() )
            return Common::MakeError<bool>( std::format( "Data Layers: the world states no layer '{}'.", name ) );
        const ::Desert::Core::EditorRegions::DataLayerOverrides before = Held().DataLayers;
        ::Desert::Core::EditorRegions::HeldRegions              next   = Held();
        next.DataLayers = ::Desert::Core::EditorRegions::WithDataLayerLoaded( partition, before, name, loaded );
        if ( next.DataLayers == before )
            return Common::MakeSuccess( false );
        // The same loaders again, under the new layer state; Reload remembers it (and keeps the old on a refusal).
        if ( auto reloaded = Reload( std::move( next ), ::Desert::Core::EditorRegions::UnsavedPolicy::Ask ); !reloaded )
            return reloaded;
        m_DataLayerStatus = std::format( "{} '{}': {}", loaded ? "Loaded" : "Unloaded", name, m_RegionStatus );
        return BOOLSUCCESS;
    }

    // UE's Data Layer Outliner, the pattern and not the letter: one row per layer of the world, its Loaded state
    // for this user and the world's Initially Loaded; New and Delete edit the world's list through
    // EditWorldPartition (one undo step, refused by Rules::CheckDataLayers as the planner would refuse it).
    void WorldPartitionPanel::DrawDataLayers()
    {
        if ( !m_Scene || !m_Scene->GetWorldPartition().has_value() || !ImGui::CollapsingHeader( "Data Layers" ) )
            return;
        const ::Desert::Core::WorldPartitionSerialized current = *m_Scene->GetWorldPartition();
        static const std::vector<::Desert::Core::DataLayerSerialized> kNoLayers;
        const std::vector<::Desert::Core::DataLayerSerialized>&       layers =
             current.DataLayers ? *current.DataLayers : kNoLayers;
        const bool       heldFromFiles = m_Scene->Packages()->BaselinePath().has_value();
        const std::vector<bool> loaded =
             ::Desert::Core::EditorRegions::DataLayersLoadedInEditor( current, Held().DataLayers );

        // Members per layer, from the plan the map draws (the world on disk, or the scene): what Delete refuses.
        std::vector<std::size_t> members( layers.size(), 0 );
        if ( m_EditPlan )
            for ( const ::Desert::Core::Rules::PlannedComposite& composite : m_EditPlan->Composites )
                for ( const std::size_t layer : composite.DataLayers )
                    if ( layer < members.size() )
                        members[layer] += composite.Members.size();

        const auto commit = [this]( std::string label, ::Desert::Core::WorldPartitionSerialized after )
        {
            const auto edited = EditWorldPartition( m_Scene, std::move( label ), std::move( after ) );
            m_DataLayerStatus = edited.IsSuccess() ? std::string() : edited.GetError();
            m_EditPlanStale   = true;
        };

        // The summary: the tree, visibility, the current layer and membership are the Data Layers panel's.
        ImGui::TextDisabled( "Tree, visibility, current layer and membership: the Data Layers panel." );
        if ( layers.empty() )
            ImGui::TextDisabled( "This world has no data layers." );
        else if ( ImGui::BeginTable( "##datalayers", 7,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_SizingStretchProp ) )
        {
            ImGui::TableSetupColumn( "Data Layer" );
            ImGui::TableSetupColumn( "Loaded" );
            ImGui::TableSetupColumn( "Initially Loaded" );
            ImGui::TableSetupColumn( "Type" );
            ImGui::TableSetupColumn( "Initial Runtime State" );
            ImGui::TableSetupColumn( "Debug colour" );
            ImGui::TableSetupColumn( "Entities" );
            ImGui::TableHeadersRow();
            for ( std::size_t index = 0; index < layers.size(); ++index )
            {
                const ::Desert::Core::DataLayerSerialized& layer = layers[index];
                ImGui::PushID( static_cast<int>( index ) );
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( layer.Name.c_str() );
                ImGui::TableNextColumn();
                bool on = index < loaded.size() && loaded[index];
                ImGui::BeginDisabled( !heldFromFiles );
                if ( ImGui::Checkbox( "##loaded", &on ) )
                    if ( const auto set = SetDataLayerLoaded( layer.Name, on ); !set )
                        m_DataLayerStatus = set.GetError();
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                bool initially = layer.InitiallyLoadedInEditor;
                if ( ImGui::Checkbox( "##initially", &initially ) )
                {
                    ::Desert::Core::WorldPartitionSerialized after = current;
                    ( *after.DataLayers )[index].InitiallyLoadedInEditor = initially;
                    commit( std::format( "Data layer '{}': Initially Loaded {}", layer.Name, initially ), after );
                }
                // UE UDataLayerAsset::DataLayerType (WP v3): a Runtime layer splits the cook and streams in Play and
                // a packaged game by its runtime state (WorldStreamer::SetDataLayerRuntimeState).
                using DataLayerType         = ::Desert::Core::DataLayerType;
                using DataLayerRuntimeState = ::Desert::Core::DataLayerRuntimeState;
                ImGui::TableNextColumn();
                static constexpr const char* kTypes[] = { "Editor", "Runtime" };
                int type = layer.Type == DataLayerType::Runtime ? 1 : 0;
                ImGui::SetNextItemWidth( -1.0f );
                if ( ImGui::Combo( "##type", &type, kTypes, 2 ) )
                {
                    ::Desert::Core::WorldPartitionSerialized after  = current;
                    ::Desert::Core::DataLayerSerialized&     edited = ( *after.DataLayers )[index];
                    edited.Type = type == 1 ? DataLayerType::Runtime : DataLayerType::Editor;
                    // A Runtime layer states the state it starts Play in and an Editor one none (Rules::
                    // CheckDataLayers): a layer made Runtime starts Unloaded, UE's InitialRuntimeState default.
                    if ( edited.Type == DataLayerType::Runtime )
                        edited.InitialRuntimeState = DataLayerRuntimeState::Unloaded;
                    else
                        edited.InitialRuntimeState.reset();
                    commit( std::format( "Data layer '{}': Type {}", layer.Name, kTypes[type] ), std::move( after ) );
                }
                // UE UDataLayerInstance::InitialRuntimeState: what Play and a packaged game start the layer in.
                ImGui::TableNextColumn();
                if ( layer.Type == DataLayerType::Runtime && layer.InitialRuntimeState.has_value() )
                {
                    static constexpr const char* kStates[] = { "Unloaded", "Loaded", "Activated" };
                    int state = static_cast<int>( *layer.InitialRuntimeState );
                    ImGui::SetNextItemWidth( -1.0f );
                    if ( ImGui::Combo( "##initialstate", &state, kStates, 3 ) )
                    {
                        ::Desert::Core::WorldPartitionSerialized after = current;
                        ( *after.DataLayers )[index].InitialRuntimeState = static_cast<DataLayerRuntimeState>( state );
                        commit( std::format( "Data layer '{}': Initial Runtime State {}", layer.Name, kStates[state] ),
                                std::move( after ) );
                    }
                }
                else
                    ImGui::TextDisabled( "%s", layer.Type == DataLayerType::Runtime ? "none stated" : "Editor layer" );
                ImGui::TableNextColumn();
                // UE UDataLayerAsset::DebugColor: what the gameplay debugger paints this layer's cells in.
                // The layer's colour is the v4 sRGB triple the Data Layers outliner swatches too (one value).
                if ( const std::optional<glm::vec3> debugColor = DebugColorEdit(
                          true, index, glm::vec3( layer.DebugColor[0], layer.DebugColor[1], layer.DebugColor[2] ) ) )
                {
                    ::Desert::Core::WorldPartitionSerialized after = current;
                    ( *after.DataLayers )[index].DebugColor        = { debugColor->x, debugColor->y, debugColor->z };
                    commit( std::format( "Set debug colour of data layer '{}'", layer.Name ), std::move( after ) );
                }
                ImGui::TableNextColumn();
                ImGui::Text( "%zu", members[index] );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Delete" ) )
                {
                    // A parent goes after its children are moved (WP v4): a child naming a gone layer is an Issue.
                    const auto children = std::count_if( layers.begin(), layers.end(),
                                                         [&]( const ::Desert::Core::DataLayerSerialized& other )
                                                         { return other.Parent == layer.Name; } );
                    if ( children > 0 )
                        m_DataLayerStatus = std::format( "Delete '{}': {} data layer(s) nest under it - move them "
                                                         "first (the Data Layers panel).",
                                                         layer.Name, children );
                    else if ( members[index] > 0 )
                        m_DataLayerStatus = std::format(
                             "Delete '{}': {} entit{} of the world belong to it - remove it from their Data "
                             "Layers first.",
                             layer.Name, members[index], members[index] == 1 ? "y" : "ies" );
                    else
                    {
                        ::Desert::Core::WorldPartitionSerialized after = current;
                        after.DataLayers->erase( after.DataLayers->begin() + static_cast<std::ptrdiff_t>( index ) );
                        if ( after.DataLayers->empty() )
                            after.DataLayers.reset(); // no layers is no key, never an empty list
                        commit( std::format( "Delete data layer '{}'", layer.Name ), std::move( after ) );
                        ImGui::PopID();
                        break; // the list moved under `layers`; drawn again next frame
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::SetNextItemWidth( 200.0f );
        ImGui::InputTextWithHint( "##newdatalayer", "New data layer name", m_NewDataLayer.data(),
                                  m_NewDataLayer.size() );
        ImGui::SameLine();
        if ( ImGui::Button( "New Data Layer" ) )
        {
            ::Desert::Core::WorldPartitionSerialized after = current;
            if ( !after.DataLayers )
                after.DataLayers.emplace();
            const std::string name( m_NewDataLayer.data() );
            ::Desert::Core::DataLayerSerialized layer{ name, true };
            layer.DebugColor = ::Desert::Core::Rules::SeededDataLayerColor( name );
            after.DataLayers->push_back( std::move( layer ) );
            commit( std::format( "New data layer '{}'", name ), std::move( after ) );
            if ( m_DataLayerStatus.empty() )
                m_NewDataLayer.fill( '\0' );
        }
        if ( !m_DataLayerStatus.empty() )
            ImGui::TextWrapped( "%s", m_DataLayerStatus.c_str() );
    }

    Common::BoolResultStr WorldPartitionPanel::BuildHLODs( HLODBuildOptions options )
    {
        if ( m_Builder )
            return Common::MakeError( "Build HLODs: a build is already running (its landscape bake)." );
        if ( !m_Scene )
            return Common::MakeError( "Build HLODs: there is no active scene." );
        const auto& baseline = m_Scene->Packages()->BaselinePath();
        // The build reads the saved files: a world with unsaved changes asks to Save and build (UE prompts to save
        // the map before it builds HLODs) instead of being refused.
        if ( !baseline || m_Scene->Packages()->HasUnsavedChanges(
                               ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets ).LiveEntities() ) )
        {
            m_SavePrompt = options;
            const std::string ask =
                 std::format( "Build HLODs: '{}' has unsaved changes - Save and build? (World Partition window)",
                              m_Scene->GetSceneName() );
            LOG_INFO( "[Build HLODs] {0}", ask );
            ToastManager::Push( ask, ToastLevel::Warning, 6.0f );
            return BOOLSUCCESS;
        }
        auto builder = std::make_unique<WorldPartitionHLODBuilder>( *baseline, options );
        if ( auto started = builder->Start( m_Assets ); !started )
            return Common::MakeError( started.GetError() );
        m_Builder = std::move( builder );
        m_SavePrompt.reset();
        m_HLODBuildStatus.clear();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldPartitionPanel::SaveAndBuildHLODs()
    {
        const HLODBuildOptions options = m_SavePrompt.value_or( HLODBuildOptions{} );
        m_SavePrompt.reset();
        if ( !m_Scene )
            return Common::MakeError( "Save and build: there is no active scene." );
        if ( !m_SaveScene )
            return Common::MakeError( "Save and build: the editor gave the World Partition window no scene save" );
        if ( !m_SaveScene() )
            return Common::MakeError( "Save and build: the scene was NOT saved; the log line above says why." );
        m_HLODStatusStale = true;
        return BuildHLODs( options );
    }

    Common::BoolResultStr WorldPartitionPanel::DeleteHLODs()
    {
        if ( m_Builder )
            return Common::MakeError( "Delete HLODs: a build is running; delete once it has finished." );
        if ( !m_Scene )
            return Common::MakeError( "Delete HLODs: there is no active scene." );
        const auto& baseline = m_Scene->Packages()->BaselinePath();
        if ( !baseline )
            return Common::MakeError( "Delete HLODs: '" + m_Scene->GetSceneName() +
                                      "' is not saved, so it has no built HLODs" );
        auto removed = ::Desert::Editor::DeleteHLODs( *baseline );
        if ( !removed )
            return Common::MakeError( removed.GetError() );
        m_HLODStatusStale  = true;
        m_EditorHLODsStale = true; // the editor shows what is on disk (EditorHLODs)
        ToastManager::Push( std::format( "Delete HLODs: {} file(s) removed", removed.GetValue() ),
                            ToastLevel::Info );
        return BOOLSUCCESS;
    }

    // The running build (WorldPartitionHLODBuilder): the landscape bake, then the mesh HLODs' merged materials,
    // then the approximated cells' capture, then the build that binds them.
    void WorldPartitionPanel::TickBuild()
    {
        if ( !m_Builder || !m_Builder->Tick() )
            return;
        const std::unique_ptr<WorldPartitionHLODBuilder> builder = std::move( m_Builder );
        m_HLODStatusStale                                        = true;
        // The editor shows the HLODs on disk, whatever this build wrote or refused (EditorHLODs).
        m_EditorHLODsStale = true;
        if ( builder->Error() )
        {
            LOG_ERROR( "[Build HLODs] {0}", *builder->Error() );
            m_HLODBuildStatus = *builder->Error();
            ToastManager::Push( *builder->Error(), ToastLevel::Error, 8.0f );
            return;
        }
        ToastManager::Push( "Build HLODs: done", ToastLevel::Success );
    }

    void WorldPartitionPanel::RefreshHLODStatus()
    {
        m_HLODStatusStale = false;
        m_RegistrySerial  = ::Desert::Assets::ContentRegistry::WriteSerial();
        m_HLODStatus.reset();
        m_HLODStatusError.clear();
        const auto& baseline = m_Scene->Packages()->BaselinePath();
        if ( !baseline )
        {
            m_HLODStatusError = "the scene is not saved, so it has no built HLODs";
            return;
        }
        if ( *baseline != m_StatusScene )
        {
            m_StatusScene   = *baseline;
            m_StaleNotified = 0;
        }
        auto status = HLODStatusOf( *baseline );
        if ( !status )
        {
            m_HLODStatusError = status.GetError();
            return;
        }
        const std::size_t stale = status.GetValue().Stale.size();
        // The notification (UE: "HLODs need to be rebuilt"): more rows stale than the last count said.
        if ( stale > m_StaleNotified )
            ToastManager::Push( std::format( "World Partition: {} stale HLOD - Build HLODs", stale ),
                                ToastLevel::Warning, 6.0f );
        m_StaleNotified = stale;
        m_HLODStatus    = status.ExtractValue();
    }

    void WorldPartitionPanel::DrawHLODBuild()
    {
        constexpr const char* kSaveAndBuildPopup = "Save and build HLODs";
        if ( ::Desert::Core::Rules::AnyGridHLODLayer( *m_Scene->GetWorldPartition() ) )
        {
            // A mesh, material or texture saved since the last status may have made rows stale.
            if ( ::Desert::Assets::ContentRegistry::WriteSerial() != m_RegistrySerial )
                m_HLODStatusStale = true;
            if ( m_HLODStatusStale && !m_Builder )
                RefreshHLODStatus();
            if ( ImGui::CollapsingHeader( "HLODs", ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                if ( m_Builder )
                    ImGui::TextUnformatted( "Building HLODs (landscape bake)..." );
                else if ( !m_HLODStatusError.empty() )
                    ImGui::TextWrapped( "HLOD status: %s", m_HLODStatusError.c_str() );
                else if ( m_HLODStatus && !m_HLODStatus->Built )
                    ImGui::Text( "HLODs not built: %zu stale HLOD", m_HLODStatus->Stale.size() );
                else if ( m_HLODStatus )
                    ImGui::Text( "%zu stale HLOD", m_HLODStatus->Stale.size() );
                if ( m_HLODStatus && !m_HLODStatus->Stale.empty() && ImGui::IsItemHovered() )
                {
                    ImGui::BeginTooltip();
                    for ( const std::string& unit : m_HLODStatus->Stale )
                        ImGui::TextUnformatted( unit.c_str() );
                    ImGui::EndTooltip();
                }
                ImGui::BeginDisabled( m_Builder != nullptr );
                if ( ImGui::Button( std::string( ::Desert::Core::Rules::kBuildHLODsLabel ).c_str() ) )
                    if ( auto started = BuildHLODs(); !started )
                        m_HLODBuildStatus = started.GetError();
                ImGui::SameLine();
                if ( ImGui::Button( "Force all" ) )
                    if ( auto started = BuildHLODs( HLODBuildOptions{ true } ); !started )
                        m_HLODBuildStatus = started.GetError();
                ImGui::SameLine();
                if ( ImGui::Button( std::string( ::Desert::Core::Rules::kDeleteHLODsLabel ).c_str() ) )
                    if ( auto removed = DeleteHLODs(); !removed )
                        m_HLODBuildStatus = removed.GetError();
                ImGui::EndDisabled();
                if ( !m_HLODBuildStatus.empty() )
                    ImGui::TextWrapped( "%s", m_HLODBuildStatus.c_str() );
            }
        }
        if ( m_SavePrompt && !ImGui::IsPopupOpen( kSaveAndBuildPopup ) )
            ImGui::OpenPopup( kSaveAndBuildPopup );
        if ( ImGui::BeginPopupModal( kSaveAndBuildPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "The world has unsaved changes; HLODs are built from its saved files." );
            if ( ImGui::Button( "Save and build" ) )
            {
                ImGui::CloseCurrentPopup();
                if ( auto started = SaveAndBuildHLODs(); !started )
                    m_HLODBuildStatus = started.GetError();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel" ) )
            {
                m_SavePrompt.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    Common::BoolResultStr WorldPartitionPanel::BuildMiniMap()
    {
        if ( m_MiniMapBuild )
            return Common::MakeError( "Build Minimap: a build is already running" );
        if ( m_Builder )
            return Common::MakeError( "Build Minimap: Build HLODs is still running; build the minimap after it" );
        if ( !m_Scene )
            return Common::MakeError( "Build Minimap: there is no active scene." );
        if ( !m_Scene->GetWorldPartition() )
            return Common::MakeError( "Build Minimap: '" + m_Scene->GetSceneName() +
                                      "' is not partitioned (it states no WorldPartition block)" );
        const auto& baseline = m_Scene->Packages()->BaselinePath();
        if ( !baseline )
            return Common::MakeError( "Build Minimap: '" + m_Scene->GetSceneName() +
                                      "' is not saved; save it first (the minimap is captured from its files)" );
        if ( m_Scene->Packages()->HasUnsavedChanges(
                  ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets ).LiveEntities() ) )
            return Common::MakeError( "Build Minimap: '" + m_Scene->GetSceneName() +
                                      "' has unsaved changes; save the scene first (the minimap is captured from "
                                      "its files)" );
        // The world rectangle: every placed composite of the world on disk, loaded or not (UE: the editor world
        // bounds of every actor descriptor).
        if ( m_EditPlanStale || !m_PlanFromIndex )
            RebuildEditPlan();
        if ( !m_EditPlan || !m_PlanFromIndex )
            return Common::MakeError( "Build Minimap: the world on disk could not be planned: " + m_EditPlanStatus );
        std::optional<::Desert::Core::Rules::CellBounds> bounds;
        for ( const ::Desert::Core::Rules::PlannedComposite& composite : m_EditPlan->Composites )
        {
            if ( !composite.Footprint )
                continue;
            const ::Desert::Core::Rules::CellBounds& box = *composite.Footprint;
            if ( !bounds )
            {
                bounds = box;
                continue;
            }
            bounds->MinX = std::min( bounds->MinX, box.MinX );
            bounds->MinZ = std::min( bounds->MinZ, box.MinZ );
            bounds->MaxX = std::max( bounds->MaxX, box.MaxX );
            bounds->MaxZ = std::max( bounds->MaxZ, box.MaxZ );
        }
        if ( !bounds )
            return Common::MakeError( "Build Minimap: '" + m_Scene->GetSceneName() +
                                      "' places nothing (no entity of the world has a position)" );
        ::Desert::Core::WorldPartitionMiniMapSerialized settings;
        if ( const auto& previous = m_Scene->GetWorldPartition()->MiniMap )
        {
            settings.WorldUnitsPerPixel = previous->WorldUnitsPerPixel;
            settings.BuilderCellSize    = previous->BuilderCellSize;
        }
        // The rectangle starts on a whole texel of the world's texel grid, so a rebuild lands on the same texels.
        settings.MinX = std::floor( bounds->MinX / settings.WorldUnitsPerPixel ) * settings.WorldUnitsPerPixel;
        settings.MinZ = std::floor( bounds->MinZ / settings.WorldUnitsPerPixel ) * settings.WorldUnitsPerPixel;
        settings.MaxX = bounds->MaxX;
        settings.MaxZ = bounds->MaxZ;
        auto started  = MiniMapBuilder::Start( *baseline, settings, DataLayerNames(), m_Assets );
        if ( !started )
            return Common::MakeError( "Build Minimap: " + started.GetError() );
        m_MiniMapBuild = started.ExtractValue();
        m_MiniMapScene = *baseline;
        m_MiniMapStatus.clear();
        LOG_INFO( "[Build Minimap] '{0}': {1} tile(s) to capture", m_Scene->GetSceneName(),
                  m_MiniMapBuild->TileCount() );
        return BOOLSUCCESS;
    }

    void WorldPartitionPanel::TickMiniMap()
    {
        if ( !m_MiniMapBuild || !m_MiniMapBuild->Tick() )
            return;
        const std::unique_ptr<MiniMapBuilder> build = std::move( m_MiniMapBuild );
        if ( build->Error() )
        {
            m_MiniMapStatus = "Build Minimap: " + *build->Error();
            LOG_ERROR( "[Build Minimap] {0}", *build->Error() );
            return;
        }
        // The texture is on disk; the world it was captured from now references it, as one undoable edit (UE marks
        // the minimap actor's package dirty the same way: Save keeps it).
        if ( !m_Scene || !m_Scene->GetWorldPartition() || m_Scene->Packages()->BaselinePath() != m_MiniMapScene )
        {
            m_MiniMapStatus = std::format( "Build Minimap: the scene changed during the build; '{}' is written but "
                                           "no world references it",
                                           MiniMapBuilder::TexturePath( m_MiniMapScene ).generic_string() );
            LOG_ERROR( "[Build Minimap] {0}", m_MiniMapStatus );
            return;
        }
        ::Desert::Core::WorldPartitionSerialized after = *m_Scene->GetWorldPartition();
        after.MiniMap                                  = *build->Result();
        if ( auto edited = EditWorldPartition( m_Scene, "Build Minimap", std::move( after ) ); !edited )
        {
            m_MiniMapStatus = "Build Minimap: " + edited.GetError();
            LOG_ERROR( "[Build Minimap] {0}", edited.GetError() );
            return;
        }
        // The texture was rewritten under the handle the map holds: its GPU copy is built again on the next ask.
        if ( m_MiniMapHandle )
            static_cast<void>( ::Desert::Runtime::ResourceRegistry::GetTextureService()->EvictBuilt( *m_MiniMapHandle ) );
        m_MiniMapPath.clear();
        m_MiniMapHandle.reset();
    }

    Common::BoolResultStr WorldPartitionPanel::ConvertSceneToWorldPartition()
    {
        if ( auto converted = ConvertToWorldPartition( m_Scene ); !converted )
            return converted;
        m_EditPlanStale = true;
        m_FocusPending  = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ConvertToWorldPartition( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        if ( !scene )
            return Common::MakeError( kConvertButtonLabel + ": there is no active scene." );

        const auto converted =
             ::Desert::Core::Rules::ConvertToWorldPartition( scene->GetSceneName(), scene->GetWorldPartition() );
        if ( !converted.IsSuccess() )
            return Common::MakeError( converted.GetError() );

        std::optional<::Desert::Core::WorldPartitionSerialized> before = scene->GetWorldPartition();
        const ::Desert::Core::WorldPartitionSerialized&         after  = converted.GetValue();
        scene->SetWorldPartition( after );
        // The entry, not a dirty flag: the editor reads CommandHistory's revision to decide whether the
        // scene has unsaved changes, so pushing IS marking it dirty (EditorLayer.cpp, s_SavedRevision).
        CommandHistory::Get().PushCommand( std::make_unique<WorldPartitionEditCommand>(
             "Convert to World Partition", scene, std::move( before ), after ) );
        // FO-6: the world's foliage filed by the new grid, one field per type per cell (UE converts a level's
        // InstancedFoliageActor into partitioned ones the same way). Its own undo step, after the conversion's.
        if ( auto filed = Tools::FoliagePaintTool::RepartitionFoliage( *scene ); !filed )
            return Common::MakeFormattedError<bool>( "{}: the world is partitioned but its foliage is not: {}",
                                                     kConvertButtonLabel, filed.GetError() );

        return BOOLSUCCESS;
    }

    void WorldPartitionPanel::DrawMap( const WorldPartitionPlan&                       plan,
                                       const ::Desert::Core::WorldPartitionSerialized& partition,
                                       const ::Desert::Core::Rules::ResidencyState*    residency,
                                       const ::Desert::Core::WorldStreamer*            streamer )
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2       avail  = ImGui::GetContentRegionAvail();
        avail.x             = std::max( avail.x, kMinMapPixels );
        avail.y             = std::max( avail.y, kMinMapPixels );
        const glm::dvec2 size( avail.x, avail.y );
        // Only the picked partition is drawn, and OnUIRender keeps m_Grid on one that makes cells.
        const ::Desert::Core::RuntimePartitionSerialized& shownPartition = partition.RuntimePartitions[m_Grid];
        const ::Desert::Core::RuntimePartitionLHGridSerialized& grid =
             *::Desert::Core::Rules::CellGridOf( partition, m_Grid );
        const float cellSize = grid.CellSize;

        ImGui::InvisibleButton( "##wpmap", avail,
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                     ImGuiButtonFlags_MouseButtonMiddle );
        const bool     hovered = ImGui::IsItemHovered();
        const ImGuiIO& io      = ImGui::GetIO();

        // Where the streaming sources are: the streamer's last ones in Play (every enabled Streaming Source and
        // the pawn's, WorldStreamer.hpp), the editor's camera in Edit — UE's editor streams around its view.
        std::vector<::Desert::Core::Rules::StreamingSource> sources;
        const std::shared_ptr<::Desert::Core::Camera> camera = m_Scene ? m_Scene->GetActiveCamera() : nullptr;
        if ( streamer != nullptr )
            sources = streamer->LastSources();
        else if ( camera )
        {
            ::Desert::Core::Rules::StreamingSource source;
            source.Position = camera->GetPosition();
            // The view's forward is the third row of the view rotation, negated.
            const glm::mat4 view = camera->GetViewMatrix();
            source.Facing        = glm::vec3( -view[0][2], -view[1][2], -view[2][2] );
            sources.push_back( source );
        }
        // The map follows the first: the pawn's when the level has one and nothing else is a source.
        const std::optional<glm::vec3> sourcePos =
             sources.empty() ? std::nullopt : std::optional<glm::vec3>( sources.front().Position );

        // ── view: focus, follow, wheel, drag ──
        // Follow centres the source; its scale is UE's PlayerExtent until a wheel sets one (m_FollowZoomed).
        const auto FollowView = [&]( glm::dvec2 mapSize, const glm::vec3& source )
        {
            Map::View followed = Map::Follow( mapSize, { source.x, source.z } );
            if ( m_FollowZoomed )
                followed.Scale = m_View.Scale;
            return followed;
        };
        // The map grows with the window: a resize refits a view nobody has zoomed or dragged since the last
        // fit. The scale stays uniform, so a cell stays square at every size.
        if ( size != m_MapSize )
        {
            m_MapSize = size;
            if ( m_Fitted )
                m_FocusPending = true;
        }
        if ( m_FocusPending )
        {
            m_FocusPending = false;
            m_Fitted       = true;
            if ( streamer != nullptr && m_Follow && sourcePos )
                m_View = FollowView( size, *sourcePos );
            else if ( const auto bounds = Map::PlanBounds( plan, m_Grid ) )
                Map::Focus( m_View, size, *bounds );
        }
        if ( streamer != nullptr && m_Follow && sourcePos )
            m_View = FollowView( size, *sourcePos );
        if ( hovered && io.MouseWheel != 0.0f )
        {
            Map::Zoom( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y }, io.MouseWheel );
            m_FollowZoomed = m_Follow;
            m_Fitted       = false;
        }
        if ( ImGui::IsItemActive() && ( ImGui::IsMouseDragging( ImGuiMouseButton_Right ) ||
                                        ImGui::IsMouseDragging( ImGuiMouseButton_Middle ) ) )
        {
            Map::Pan( m_View, { io.MouseDelta.x, io.MouseDelta.y } );
            m_Follow = false;
            m_Fitted = false;
        }

        // ── region selection (Edit, a world opened from its files): UE's box selection on the grid. A left drag
        // spans a rectangle; a click takes the cell under the cursor, or clears the selection over empty ground.
        if ( streamer == nullptr && m_PlanFromIndex )
        {
            const glm::dvec2 mouseWorld =
                 Map::ScreenToWorld( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y } );
            if ( ImGui::IsItemActivated() && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                m_DragFrom = mouseWorld;
            if ( m_DragFrom && ImGui::IsItemActive() && ImGui::IsMouseDragging( ImGuiMouseButton_Left ) )
                m_Selection = Map::RegionBetween( *m_DragFrom, mouseWorld );
            if ( m_DragFrom && ImGui::IsMouseReleased( ImGuiMouseButton_Left ) )
            {
                const float threshold = io.MouseDragThreshold;
                if ( io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < threshold * threshold )
                {
                    if ( const auto index = Map::CellAt( plan, *m_DragFrom, m_Level, m_Grid ) )
                        m_Selection = plan.Cells[*index].Square;
                    else
                        m_Selection.reset();
                }
                m_DragFrom.reset();
            }
        }
        else
            m_DragFrom.reset();

        ImDrawList&  list = *ImGui::GetWindowDrawList();
        const ImVec2 corner( origin.x + avail.x, origin.y + avail.y );
        list.PushClipRect( origin, corner, true );
        list.AddRectFilled( origin, corner, IM_COL32( 22, 22, 24, 255 ) );

        // ── the minimap under everything (UE SWorldPartitionEditorGrid2D paints AWorldPartitionMiniMap's texture over
        // its MiniMapWorldBounds before the grid): row 0 of the texture lies along MinZ, column 0 along MinX ──
        if ( partition.MiniMap && !partition.MiniMap->Texture.Path.empty() )
        {
            auto* textures = ::Desert::Runtime::ResourceRegistry::GetTextureService();
            if ( partition.MiniMap->Texture.Path != m_MiniMapPath )
            {
                m_MiniMapPath = partition.MiniMap->Texture.Path;
                m_MiniMapHandle.reset();
                auto handle = textures->HandleForPath( m_MiniMapPath );
                if ( handle )
                    m_MiniMapHandle = handle.GetValue();
                else
                    m_MiniMapStatus = "Minimap: " + handle.GetError();
            }
            if ( m_MiniMapHandle )
            {
                static_cast<void>( textures->Require( *m_MiniMapHandle ) ); // starts the read; drawn once ready
                const auto* texture = textures->Get( *m_MiniMapHandle );
                auto*       image   = texture ? static_cast<Graphic::Image2D*>(
                                               ::Desert::Runtime::ResourceRegistry::GetImageService()->Resolve(
                                                    texture->GetImageHandle() ) )
                                              : nullptr;
                if ( image != nullptr )
                {
                    static UI::UIHelper s_MiniMapUI = []
                    {
                        UI::UIHelper ui;
                        ui.Init();
                        return ui;
                    }();
                    const std::shared_ptr<Graphic::Image2D> shared( image, []( Graphic::Image2D* ) {} );
                    if ( const ImTextureID id = s_MiniMapUI.GetTextureID( shared ) )
                        list.AddImage( id,
                                       ToScreen( m_View, size, origin,
                                                 { partition.MiniMap->MinX, partition.MiniMap->MinZ } ),
                                       ToScreen( m_View, size, origin,
                                                 { partition.MiniMap->MaxX, partition.MiniMap->MaxZ } ) );
                }
            }
        }

        // ── background grid of the level legible at this zoom ──
        {
            const int        level = Map::GridLineLevel( m_View, cellSize, kMinGridPixels );
            const double     edge  = ::Desert::Core::Rules::LevelCellSize( cellSize, level );
            const glm::dvec2 w0    = Map::ScreenToWorld( m_View, size, { 0.0, 0.0 } );
            const glm::dvec2 w1    = Map::ScreenToWorld( m_View, size, size );
            const ImU32      line  = IM_COL32( 255, 255, 255, 20 );
            for ( double x = std::floor( std::min( w0.x, w1.x ) / edge ) * edge; x <= std::max( w0.x, w1.x );
                  x += edge )
            {
                const float sx = ToScreen( m_View, size, origin, { x, 0.0 } ).x;
                list.AddLine( ImVec2( sx, origin.y ), ImVec2( sx, corner.y ), line );
            }
            for ( double z = std::floor( std::min( w0.y, w1.y ) / edge ) * edge; z <= std::max( w0.y, w1.y );
                  z += edge )
            {
                const float sy = ToScreen( m_View, size, origin, { 0.0, z } ).y;
                list.AddLine( ImVec2( origin.x, sy ), ImVec2( corner.x, sy ), line );
            }
        }

        // ── cells, coarse levels first ──
        for ( const std::size_t index : Map::VisibleCells( plan, m_View, size, m_Level, m_Grid ) )
        {
            const auto&          cell  = plan.Cells[index];
            const Map::CellState state = Map::StateOf( plan, residency, index, m_EditCells );
            const glm::vec4 fill = Map::ColorOf( state );
            const ImVec2    a    = ToScreen( m_View, size, origin, { cell.Square.MinX, cell.Square.MinZ } );
            const ImVec2    b    = ToScreen( m_View, size, origin, { cell.Square.MaxX, cell.Square.MaxZ } );
            const ImVec2    lo( std::min( a.x, b.x ), std::min( a.y, b.y ) );
            const ImVec2    hi( std::max( a.x, b.x ), std::max( a.y, b.y ) );
            list.AddRectFilled( lo, hi, ToU32( fill ) );
            list.AddRect( lo, hi, ToU32( { fill.r, fill.g, fill.b, 0.9f } ) );
        }

        // ── HLOD overlay (Edit, a world whose HLODs were built): a built cell outlined, a stale one bolder ──
        if ( streamer == nullptr && !m_HLODCells.empty() )
            for ( const std::size_t index : Map::VisibleCells( plan, m_View, size, m_Level, m_Grid ) )
            {
                if ( index >= m_HLODCells.size() || m_HLODCells[index] == Map::HLODState::None )
                    continue;
                const auto&  cell = plan.Cells[index];
                const ImVec2 a    = ToScreen( m_View, size, origin, { cell.Square.MinX, cell.Square.MinZ } );
                const ImVec2 b    = ToScreen( m_View, size, origin, { cell.Square.MaxX, cell.Square.MaxZ } );
                // Inset, so the cell's own state outline stays visible beside it.
                const ImVec2 lo( std::min( a.x, b.x ) + 2.0f, std::min( a.y, b.y ) + 2.0f );
                const ImVec2 hi( std::max( a.x, b.x ) - 2.0f, std::max( a.y, b.y ) - 2.0f );
                const bool   stale = m_HLODCells[index] == Map::HLODState::Stale;
                list.AddRect( lo, hi, ToU32( Map::ColorOf( m_HLODCells[index] ) ), 0.0f, 0, stale ? 3.0f : 1.5f );
            }

        // ── the held entities' footprints and labels, once legible (UE ShowActors / DrawActorLabels) ──
        if ( streamer == nullptr && m_PlanFromIndex && m_Scene )
        {
            const ::Desert::Core::EntityPackages& packages = *m_Scene->Packages();
            const auto held = [this, &packages]( std::size_t record )
            { return record < m_RowIds.size() && packages.IsLoaded( Common::UUID( m_RowIds[record] ) ); };
            for ( const Map::FootprintOnScreen& footprint : Map::VisibleFootprints( plan, m_View, size, held ) )
            {
                const ImVec2 lo( origin.x + static_cast<float>( footprint.Min.x ),
                                 origin.y + static_cast<float>( footprint.Min.y ) );
                const ImVec2 hi( origin.x + static_cast<float>( footprint.Max.x ),
                                 origin.y + static_cast<float>( footprint.Max.y ) );
                list.AddRect( lo, hi, ToU32( { 1.0f, 1.0f, 1.0f, 0.75f * footprint.Opacity } ) );
                const std::size_t anchor = plan.Composites[footprint.Composite].Anchor;
                if ( anchor >= m_EditTags.size() || m_EditTags[anchor].empty() )
                    continue;
                const ImVec2 label = ImGui::CalcTextSize( m_EditTags[anchor].c_str() );
                if ( Map::LabelFits( footprint, { label.x, label.y } ) )
                    list.AddText( ImVec2( lo.x + static_cast<float>( Map::kLabelPaddingPixels ),
                                          lo.y + static_cast<float>( Map::kLabelPaddingPixels ) ),
                                  ToU32( { 1.0f, 1.0f, 1.0f, footprint.Opacity } ), m_EditTags[anchor].c_str() );
            }
        }

        // ── world axes: X red, Z green, at UE's 40 % ──
        {
            const ImVec2 zero = ToScreen( m_View, size, origin, { 0.0, 0.0 } );
            list.AddLine( ImVec2( origin.x, zero.y ), ImVec2( corner.x, zero.y ), IM_COL32( 255, 0, 0, 102 ) );
            list.AddLine( ImVec2( zero.x, origin.y ), ImVec2( zero.x, corner.y ), IM_COL32( 0, 255, 0, 102 ) );
        }

        // ── the selected region ──
        if ( streamer == nullptr && m_Selection )
        {
            const ImVec2 a = ToScreen( m_View, size, origin, { m_Selection->MinX, m_Selection->MinZ } );
            const ImVec2 b = ToScreen( m_View, size, origin, { m_Selection->MaxX, m_Selection->MaxZ } );
            const ImVec2 lo( std::min( a.x, b.x ), std::min( a.y, b.y ) );
            const ImVec2 hi( std::max( a.x, b.x ), std::max( a.y, b.y ) );
            list.AddRectFilled( lo, hi, IM_COL32( 64, 140, 255, 48 ) );
            list.AddRect( lo, hi, IM_COL32( 96, 170, 255, 230 ), 0.0f, 0, 2.0f );
        }

        // ── the loaders' boxes (WP-LOADER-ADAPTERS): Location Volumes amber, regions green; filled and solid
        // while loaded, a thin outline while not; each named as its row in Loaders ──
        if ( streamer == nullptr )
            for ( const ::Desert::Core::EditorRegions::LoaderRow& row :
                  ::Desert::Core::EditorRegions::LoaderRowsOf( m_Held, m_Volumes ) )
            {
                if ( !row.Box )
                    continue;
                const ImVec2 a = ToScreen( m_View, size, origin, { row.Box->MinX, row.Box->MinZ } );
                const ImVec2 b = ToScreen( m_View, size, origin, { row.Box->MaxX, row.Box->MaxZ } );
                const ImVec2 lo( std::min( a.x, b.x ), std::min( a.y, b.y ) );
                const ImVec2 hi( std::max( a.x, b.x ), std::max( a.y, b.y ) );
                const bool   volume = row.Kind == ::Desert::Core::EditorRegions::LoaderKind::Volume;
                const ImU32  edge   = volume ? IM_COL32( 255, 196, 64, row.Loaded ? 230 : 110 )
                                             : IM_COL32( 120, 220, 140, row.Loaded ? 230 : 110 );
                if ( row.Loaded )
                    list.AddRectFilled( lo, hi,
                                        volume ? IM_COL32( 255, 196, 64, 32 ) : IM_COL32( 120, 220, 140, 32 ) );
                list.AddRect( lo, hi, edge, 0.0f, 0, row.Loaded ? 2.0f : 1.0f );
                list.AddText( ImVec2( lo.x + 4.0f, lo.y + 2.0f ), edge, row.Name.c_str() );
            }

        // ── every streaming source: its loading circle, its unload band; where the view looks, in Edit ──
        for ( const ::Desert::Core::Rules::StreamingSource& source : sources )
        {
            const ImVec2 at     = ToScreen( m_View, size, origin, { source.Position.x, source.Position.z } );
            const double loadCm = ::Desert::Core::Rules::SourceRadius( source, grid );
            // Outside Play the band is the project's (Config/Engine.json + this platform's layer), the value the
            // next Play streams with, not the engine's default.
            const float  margin = streamer != nullptr
                                       ? streamer->Settings().UnloadMargin
                                       : ::Desert::Project::CurrentWorldPartitionSettings().UnloadMargin;
            for ( const auto& sector : ::Desert::Core::Rules::SourceSectors( source, shownPartition ) )
            {
                PaintSector( list, m_View, size, origin, sector, 1.0, 1.0, IM_COL32( 255, 255, 255, 200 ), false );
                PaintSector( list, m_View, size, origin, sector, 1.0 + margin, 1.0 + margin,
                             IM_COL32( 255, 255, 255, 120 ), true );
                // THE LOOK-AHEAD: the same shape where the source will be, joined by its path; the query wants
                // every cell the shape passes over between the two.
                if ( sector.Sweep != glm::dvec2( 0.0 ) )
                {
                    auto ahead   = sector;
                    ahead.Centre = sector.Centre + sector.Sweep;
                    ahead.Sweep  = glm::dvec2( 0.0 );
                    PaintSector( list, m_View, size, origin, ahead, 1.0, 1.0, IM_COL32( 120, 200, 255, 200 ),
                                 false );
                    // The tip's unload band: StepResidency widens the sweep by the same margin as the radius.
                    auto keptAhead   = ahead;
                    keptAhead.Centre = sector.Centre + sector.Sweep * ( 1.0 + margin );
                    PaintSector( list, m_View, size, origin, keptAhead, 1.0 + margin, 1.0 + margin,
                                 IM_COL32( 120, 200, 255, 120 ), true );
                    list.AddLine( ToScreen( m_View, size, origin, sector.Centre ),
                                  ToScreen( m_View, size, origin, ahead.Centre ), IM_COL32( 120, 200, 255, 200 ),
                                  2.0f );
                }
            }
            // Where the source faces (UE's view direction): the camera's in Edit, the entity's forward in Play.
            if ( source.Facing )
            {
                const glm::dvec2 forward( source.Facing->x, source.Facing->z );
                if ( const double length = glm::length( forward ); length > 1e-6 )
                {
                    const glm::dvec2 dir = forward / length;
                    const glm::dvec2 tip = glm::dvec2( source.Position.x, source.Position.z ) + dir * loadCm * 0.5;
                    list.AddLine( at, ToScreen( m_View, size, origin, tip ), IM_COL32( 255, 220, 64, 255 ), 2.0f );
                }
            }
            list.AddCircleFilled( at, 5.0f, IM_COL32( 255, 220, 64, 255 ) );
        }

        // ── legend: every state, every cell at the shown level (on screen or not) ──
        {
            const std::vector<Map::LegendRow> legend =
                 Map::Legend( plan, residency, m_Level, m_EditCells, m_Grid );
            const float                       h      = ImGui::GetTextLineHeight();
            ImVec2                            row( origin.x + 8.0f, corner.y - 8.0f );
            for ( const Map::LegendRow& entry : std::views::reverse( legend ) )
            {
                row.y -= h + 2.0f;
                list.AddRectFilled( row, ImVec2( row.x + h, row.y + h ), ToU32( Map::ColorOf( entry.State ) ) );
                const std::string text = std::format( "{} ({})", Map::NameOf( entry.State ), entry.Count );
                list.AddText( ImVec2( row.x + h + 6.0f, row.y ), IM_COL32( 230, 230, 230, 255 ), text.c_str() );
            }
            if ( streamer == nullptr && !m_HLODCells.empty() )
                for ( const Map::HLODState hlod : { Map::HLODState::Built, Map::HLODState::Stale } )
                {
                    row.y -= h + 2.0f;
                    list.AddRect( row, ImVec2( row.x + h, row.y + h ), ToU32( Map::ColorOf( hlod ) ), 0.0f, 0, 2.0f );
                    const std::string text = std::format( "{} ({})", Map::NameOf( hlod ),
                                                          std::ranges::count( m_HLODCells, hlod ) );
                    list.AddText( ImVec2( row.x + h + 6.0f, row.y ), IM_COL32( 230, 230, 230, 255 ), text.c_str() );
                }
        }
        list.PopClipRect();

        // ── tooltip: the cell under the cursor ──
        if ( hovered )
        {
            const glm::dvec2 world =
                 Map::ScreenToWorld( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y } );
            if ( const auto index = Map::CellAt( plan, world, m_Level, m_Grid ) )
            {
                const auto& cell = plan.Cells[*index];
                ImGui::BeginTooltip();
                ImGui::Text( "%s  (%d, %d)", Map::LevelLabel( cellSize, cell.Level ).c_str(), cell.Cell.X,
                             cell.Cell.Z );
                ImGui::Text( "%s, %zu composite(s)",
                             Map::NameOf( Map::StateOf( plan, residency, *index, m_EditCells ) ),
                             cell.Composites.size() );
                ImGui::EndTooltip();
            }
        }
    }
} // namespace Desert::Editor
