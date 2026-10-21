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
#include "EditorLoadedRegions.hpp"

#include "../../Core/CommandHistory.hpp"
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

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <span>
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

        // One history entry for "the world's partition changed": the block before and the block after.
        //
        // WEAK, not shared. A history entry that owned the scene would keep a closed world alive for as
        // long as the undo stack remembered it; ICommand's contract is that an entry whose target is
        // gone returns false and is discarded, and a weak pointer is how that is DETECTED rather than
        // assumed. The scene is addressed as a whole object because the partition is a property of the
        // world itself and has no id of its own.
        class ConvertToWorldPartitionCommand final : public ICommand
        {
        public:
            ConvertToWorldPartitionCommand( std::weak_ptr<::Desert::Core::Scene>                    scene,
                                            std::optional<::Desert::Core::WorldPartitionSerialized> before,
                                            ::Desert::Core::WorldPartitionSerialized                after )
                 : m_Scene( std::move( scene ) ), m_Before( std::move( before ) ), m_After( std::move( after ) )
            {
            }

            [[nodiscard]] std::string GetLabel() const override
            {
                return "Convert to World Partition";
            }

            bool Undo() override
            {
                return Apply( m_Before );
            }

            bool Redo() override
            {
                return Apply( m_After );
            }

        private:
            bool Apply( const std::optional<::Desert::Core::WorldPartitionSerialized>& partition )
            {
                const std::shared_ptr<::Desert::Core::Scene> scene = m_Scene.lock();
                if ( !scene )
                    return false;
                scene->SetWorldPartition( partition );
                return true;
            }

            std::weak_ptr<::Desert::Core::Scene>                    m_Scene;
            std::optional<::Desert::Core::WorldPartitionSerialized> m_Before;
            ::Desert::Core::WorldPartitionSerialized                m_After;
        };
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
        m_Scene         = scene;
        m_EditPlanStale = true;
        m_FocusPending  = true;
        m_Selection.reset();
        m_DragFrom.reset();
    }

    void WorldPartitionPanel::RebuildEditPlan()
    {
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
        // A PARTITIONED WORLD OPENED FROM ITS FILES IS PLANNED OVER ITS DESCRIPTOR INDEX (WP20, UE's World
        // Partition editor draws every actor descriptor, loaded or not): the scene may hold only a region of it,
        // and the map is where the next region is chosen. An entity's unsaved move shows after its save.
        if ( m_PlanSource && m_Scene->Packages()->BaselinePath() )
        {
            if ( m_PlanSource->Grids.empty() )
            {
                m_EditPlanStatus = "This scene's WorldPartition block states no grid.";
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
                 ::Desert::Core::RegistryMeshBounds() );
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
        if ( scene.WorldPartition->Grids.empty() )
        {
            m_EditPlanStatus = "This scene's WorldPartition block states no grid.";
            return;
        }
        m_EditPartition = *scene.WorldPartition;
        m_EditPlan      = ::Desert::Core::Rules::PlanWorldPartition( scene.Entities, m_EditPartition,
                                                                     ::Desert::Core::RegistryMeshBounds() );
        m_EditPlanStatus.clear();
        m_FocusPending = true;
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
        }

        if ( plan == nullptr || partition->Grids.empty() )
        {
            ImGui::TextDisabled( "%s",
                                 playing ? "The streamed partition states no grid." : m_EditPlanStatus.c_str() );
            return;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        const int maxLevel = std::max( plan->LevelCount - 1, 0 );
        ImGui::SliderInt( "Level", &m_Level, -1, maxLevel, m_Level < 0 ? "all" : "L%d" );
        ImGui::SameLine();
        ImGui::TextDisabled( "%zu cells, %zu always-loaded, %d level(s), cell %.0f m", plan->Cells.size(),
                             plan->AlwaysLoaded.size(), plan->LevelCount,
                             static_cast<double>( partition->Grids[0].CellSize ) / 100.0 );

        DrawMap( *plan, *partition, residency, streamer );
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
        auto* assets = m_Assets;
        auto  changed =
             request.Whole
                   ? ::Desert::Core::EditorRegions::LoadWholeWorld( *m_Scene, assets )
                   : ::Desert::Core::EditorRegions::LoadRegions(
                         *m_Scene, assets, std::span<const ::Desert::Core::Rules::CellBounds>( request.Regions ) );
        if ( !changed )
            return Common::MakeError<bool>( changed.GetError() );
        const auto& outcome = changed.GetValue();
        if ( outcome.Loaded > 0 || outcome.Unloaded > 0 )
            CommandHistory::Get().Clear();
        m_RegionStatus =
             std::format( "{} record(s) loaded, {} unloaded; {} of the world left on disk "
                          "({} composite(s) in the region, {} always loaded, {} without a place).",
                          outcome.Loaded, outcome.Unloaded, outcome.NotLoaded, outcome.Selection.InRegions,
                          outcome.Selection.AlwaysLoaded, outcome.Selection.Unplaced );
        m_EditPlanStale = true;
        // The next open of this world holds what it holds now (UE's EditorLoadedRegions). The change itself
        // stands either way; a refusal here is the status line's, so the user knows the next open will differ.
        if ( const auto& baseline = m_Scene->Packages()->BaselinePath() )
            if ( const auto kept = EditorLoadedRegions::Remember(
                      *baseline, ::Desert::Core::EditorRegions::HeldRegions{ request.Regions, request.Whole } );
                 !kept )
                m_RegionStatus += " The loaded regions were not kept for the next open: " + kept.GetError();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldPartitionPanel::BuildHLODs()
    {
        namespace WC = ::Desert::Core::WorldCells;
        if ( m_Bake )
            return Common::MakeError( "Build HLODs: a build is already running (its landscape bake)." );
        if ( !m_Scene )
            return Common::MakeError( "Build HLODs: there is no active scene." );
        const auto& baseline = m_Scene->Packages()->BaselinePath();
        if ( !baseline )
            return Common::MakeError( "Build HLODs: '" + m_Scene->GetSceneName() +
                                      "' is not saved; save it first (the HLODs are built from its files)" );
        if ( m_Scene->Packages()->HasUnsavedChanges(
                  ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets ).LiveEntities() ) )
            return Common::MakeError( "Build HLODs: '" + m_Scene->GetSceneName() +
                                      "' has unsaved changes; save the scene first (the HLODs are built from its "
                                      "files)" );
        const std::filesystem::path& path = *baseline;
        // The saved world, read exactly as WorldCook reads it, so the checksums the build stores are the ones
        // a cook of the same files computes.
        auto text = ::Desert::Core::ExternalEntities::ReadSceneFileText( path );
        if ( !text )
            return Common::MakeError( "Build HLODs: " + text.GetError() );
        auto scene = ::Desert::Core::ParseLoadableScene( path.string(), text.GetValue() );
        if ( !scene )
            return Common::MakeError( "Build HLODs: " + scene.GetError() );
        auto descriptors = ::Desert::Core::DescriptorIndex::Refresh( path );
        if ( !descriptors )
            return Common::MakeError( "Build HLODs: " + descriptors.GetError() );
        const auto      planned = ::Desert::Core::DescriptorIndex::Descriptors( descriptors.GetValue().Index );
        const std::span registries( &::Desert::Assets::ContentRegistry::Get(), 1 );
        const ::Desert::Core::HLODBuilders builders = ::Desert::Core::MakeHLODBuilders( registries );
        // Phase 1: the landscape HLODs' areas, from the same request the build makes for each.
        std::vector<::Desert::Core::LandscapeHLODTileArea> areas;
        decltype( builders.Landscape )                     collect;
        if ( builders.Landscape )
            collect = [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
            {
                auto area = ::Desert::Core::LandscapeHLODTileAreaOf( request );
                if ( !area )
                    return Common::MakeError<::Desert::Assets::StaticMeshComponentSer>( area.GetError() );
                const bool known = std::any_of( areas.begin(), areas.end(),
                                                [&]( const auto& seen ) {
                                                    return static_cast<uint64_t>( seen.Tile ) ==
                                                           static_cast<uint64_t>( area.GetValue().Tile );
                                                } );
                if ( !known )
                    areas.push_back( area.GetValue() );
                return builders.Landscape( request );
            };
        if ( auto probe =
                  WC::BuildWorldHLODs( scene.GetValue().Scene, planned, registries, builders.Mesh, collect );
             !probe )
            return Common::MakeError( "Build HLODs: " + probe.GetError() );
        if ( areas.empty() )
            return FinishBuildHLODs( {} );
        LOG_INFO( "[Build HLODs] '{0}': baking the BaseColor of {1} landscape tile(s)", m_Scene->GetSceneName(),
                  areas.size() );
        ::Desert::Core::LoadableScene saved = std::move( scene.GetValue() );
        m_Bake = std::make_unique<LandscapeHLODBake>( path, std::move( saved ), std::move( areas ), m_Assets );
        return BOOLSUCCESS;
    }

    void WorldPartitionPanel::TickBuild()
    {
        if ( !m_Bake || !m_Bake->Tick() )
            return;
        const std::unique_ptr<LandscapeHLODBake> bake = std::move( m_Bake );
        if ( bake->Error() )
        {
            LOG_ERROR( "[Build HLODs] the landscape bake was refused: {0}", *bake->Error() );
            return;
        }
        if ( auto done = FinishBuildHLODs( bake->Baked() ); !done )
            LOG_ERROR( "[Build HLODs] {0}", done.GetError() );
    }

    Common::BoolResultStr WorldPartitionPanel::FinishBuildHLODs( const std::vector<LandscapeHLODBaked>& baked )
    {
        namespace WC = ::Desert::Core::WorldCells;
        if ( !m_Scene )
            return Common::MakeError( "Build HLODs: there is no active scene." );
        const auto& baseline = m_Scene->Packages()->BaselinePath();
        if ( !baseline )
            return Common::MakeError( "Build HLODs: '" + m_Scene->GetSceneName() +
                                      "' is not saved; save it first (the HLODs are built from its files)" );
        if ( m_Scene->Packages()->HasUnsavedChanges(
                  ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets ).LiveEntities() ) )
            return Common::MakeError( "Build HLODs: '" + m_Scene->GetSceneName() +
                                      "' has unsaved changes; save the scene first (the HLODs are built from its "
                                      "files)" );
        const std::filesystem::path& path = *baseline;
        // The saved world, read exactly as WorldCook reads it, so the checksums the build stores are the ones
        // a cook of the same files computes.
        auto text = ::Desert::Core::ExternalEntities::ReadSceneFileText( path );
        if ( !text )
            return Common::MakeError( "Build HLODs: " + text.GetError() );
        auto scene = ::Desert::Core::ParseLoadableScene( path.string(), text.GetValue() );
        if ( !scene )
            return Common::MakeError( "Build HLODs: " + scene.GetError() );
        auto descriptors = ::Desert::Core::DescriptorIndex::Refresh( path );
        if ( !descriptors )
            return Common::MakeError( "Build HLODs: " + descriptors.GetError() );
        const auto      planned = ::Desert::Core::DescriptorIndex::Descriptors( descriptors.GetValue().Index );
        const std::span registries( &::Desert::Assets::ContentRegistry::Get(), 1 );
        const ::Desert::Core::HLODBuilders builders = ::Desert::Core::MakeHLODBuilders( registries );
        // Each landscape HLOD bound to the material its tile's bake wrote (UE ULandscapeHLODBuilder). The slot
        // does not enter HLODBuildChecksum (it hashes the member records), so binding changes no staleness.
        decltype( builders.Landscape ) landscape;
        if ( builders.Landscape )
            landscape = [&]( const auto& request ) -> Common::ResultStr<::Desert::Assets::StaticMeshComponentSer>
            {
                auto mesh = builders.Landscape( request );
                if ( !mesh )
                    return mesh;
                const auto& id = request.Records[request.Tile].id;
                for ( const LandscapeHLODBaked& tile : baked )
                    if ( id && static_cast<uint64_t>( tile.Tile ) == static_cast<uint64_t>( *id ) )
                    {
                        ::Desert::Assets::StaticMeshComponentSer block = mesh.GetValue();
                        block.MaterialGuids = std::vector<std::string>{ tile.MaterialGuid };
                        block.MaterialPaths = std::vector<std::string>{ tile.MaterialPath };
                        return Common::MakeSuccess( std::move( block ) );
                    }
                return mesh;
            };
        auto built = WC::BuildWorldHLODs( scene.GetValue().Scene, planned, registries, builders.Mesh, landscape );
        if ( !built )
            return Common::MakeError( "Build HLODs: " + built.GetError() );
        if ( auto written = WC::WriteBuiltHLODs( path, built.GetValue() ); !written )
            return Common::MakeError( written.GetError() );
        const std::filesystem::path dir   = WC::BuiltHLODDirectory( path );
        std::size_t                 bytes = 0;
        for ( const WC::CookedFile& file : built.GetValue().Files )
        {
            bytes += file.Bytes.size();
            LOG_INFO( "[Build HLODs] {0}: {1} bytes", ( dir / file.Name ).string(), file.Bytes.size() );
        }
        // Read back what was written and check it against the world it was built from.
        auto stored = WC::ReadBuiltHLODs( path );
        if ( !stored )
            return Common::MakeError( "Build HLODs: " + stored.GetError() );
        std::size_t records = 0;
        for ( const WC::BuiltHLODCell& cell : stored.GetValue().Index.Cells )
        {
            auto cellRecords = WC::StoredCellRecords( stored.GetValue(), cell );
            if ( !cellRecords )
                return Common::MakeError( "Build HLODs: read back: " + cellRecords.GetError() );
            records += cellRecords.GetValue().size();
        }
        auto stale = WC::StaleHLODCells( stored.GetValue().Index, scene.GetValue().Scene, planned, registries );
        if ( !stale )
            return Common::MakeError( "Build HLODs: read back: " + stale.GetError() );
        for ( const std::string& unit : stale.GetValue() )
            LOG_ERROR( "[Build HLODs] {0}: the HLOD read back is stale", unit );
        LOG_INFO(
             "[Build HLODs] '{0}': {1} cell(s), {2} file(s), {3} bytes, {4} HLOD record(s) in {5}; read back: "
             "{6} stale cell(s)",
             m_Scene->GetSceneName(), stored.GetValue().Index.Cells.size(), built.GetValue().Files.size(), bytes,
             records, dir.string(), stale.GetValue().size() );
        if ( !stale.GetValue().empty() )
            return Common::MakeError(
                 std::format( "Build HLODs: {} cell(s) read back stale", stale.GetValue().size() ) );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldPartitionPanel::ConvertSceneToWorldPartition()
    {
        if ( !m_Scene )
            return Common::MakeError( kConvertButtonLabel + ": there is no active scene." );

        const auto converted = ::Desert::Core::Rules::ConvertToWorldPartition( m_Scene->GetSceneName(),
                                                                               m_Scene->GetWorldPartition() );
        if ( !converted.IsSuccess() )
            return Common::MakeError( converted.GetError() );

        std::optional<::Desert::Core::WorldPartitionSerialized> before = m_Scene->GetWorldPartition();
        const ::Desert::Core::WorldPartitionSerialized&         after  = converted.GetValue();
        m_Scene->SetWorldPartition( after );
        // The entry, not a dirty flag: the editor reads CommandHistory's revision to decide whether the
        // scene has unsaved changes, so pushing IS marking it dirty (EditorLayer.cpp, s_SavedRevision).
        CommandHistory::Get().PushCommand(
             std::make_unique<ConvertToWorldPartitionCommand>( m_Scene, std::move( before ), after ) );
        // FO-6: the world's foliage filed by the new grid, one field per type per cell (UE converts a level's
        // InstancedFoliageActor into partitioned ones the same way). Its own undo step, after the conversion's.
        if ( auto filed = Tools::FoliagePaintTool::RepartitionFoliage( *m_Scene ); !filed )
            return Common::MakeFormattedError<bool>( "{}: the world is partitioned but its foliage is not: {}",
                                                     kConvertButtonLabel, filed.GetError() );

        m_EditPlanStale = true;
        m_FocusPending  = true;
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
        const float      cellSize = partition.Grids[0].CellSize;

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
                m_View = Map::Follow( size, { sourcePos->x, sourcePos->z } );
            else if ( const auto bounds = Map::PlanBounds( plan ) )
                Map::Focus( m_View, size, *bounds );
        }
        if ( streamer != nullptr && m_Follow && sourcePos )
            m_View = Map::Follow( size, { sourcePos->x, sourcePos->z } );
        if ( hovered && io.MouseWheel != 0.0f )
        {
            Map::Zoom( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y }, io.MouseWheel );
            m_Follow = false;
            m_Fitted = false;
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
                    if ( const auto index = Map::CellAt( plan, *m_DragFrom, m_Level ) )
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
        for ( const std::size_t index : Map::VisibleCells( plan, m_View, size, m_Level ) )
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

        // ── every streaming source: its loading circle, its unload band; where the view looks, in Edit ──
        for ( const ::Desert::Core::Rules::StreamingSource& source : sources )
        {
            const ImVec2 at     = ToScreen( m_View, size, origin, { source.Position.x, source.Position.z } );
            const double loadCm = ::Desert::Core::Rules::SourceRadius( source, partition.Grids[0] );
            const float  margin = streamer != nullptr ? streamer->Settings().UnloadMargin
                                                      : ::Desert::Core::Rules::ResidencySettings{}.UnloadMargin;
            for ( const auto& sector : ::Desert::Core::Rules::SourceSectors( source, partition.Grids[0] ) )
            {
                PaintSector( list, m_View, size, origin, sector, 1.0, 1.0, IM_COL32( 255, 255, 255, 200 ), false );
                PaintSector( list, m_View, size, origin, sector, 1.0 + margin, 1.0 + margin,
                             IM_COL32( 255, 255, 255, 120 ), true );
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
            const std::vector<Map::LegendRow> legend = Map::Legend( plan, residency, m_Level, m_EditCells );
            const float                       h      = ImGui::GetTextLineHeight();
            ImVec2                            row( origin.x + 8.0f, corner.y - 8.0f );
            for ( const Map::LegendRow& entry : std::views::reverse( legend ) )
            {
                row.y -= h + 2.0f;
                list.AddRectFilled( row, ImVec2( row.x + h, row.y + h ), ToU32( Map::ColorOf( entry.State ) ) );
                const std::string text = std::format( "{} ({})", Map::NameOf( entry.State ), entry.Count );
                list.AddText( ImVec2( row.x + h + 6.0f, row.y ), IM_COL32( 230, 230, 230, 255 ), text.c_str() );
            }
        }
        list.PopClipRect();

        // ── tooltip: the cell under the cursor ──
        if ( hovered )
        {
            const glm::dvec2 world =
                 Map::ScreenToWorld( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y } );
            if ( const auto index = Map::CellAt( plan, world, m_Level ) )
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
