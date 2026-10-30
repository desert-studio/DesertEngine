#pragma once
// THE HLOD HIERARCHY OF A WORLD, STREAMED BY RESIDENCY (WP-FAR-8).
//
// Pattern of UE World Partition (UHLODLayer::ParentLayer / bIsSpatiallyLoaded / CellSize / LoadingRange, and
// UWorldPartitionHLODRuntimeSubsystem): the HLODs one layer builds are actors of their own runtime grid, streamed
// by the same policy as the world's cells; the layer's ParentLayer builds, per cell of that grid, an HLOD that
// stands in for the whole cell while it is not shown. The top of a chain is not spatially loaded, so there is
// always something to draw at any distance. Not UE's code: a plan over this engine's residency units.
//
// ── THE NODES ─────────────────────────────────────────────────────────────────────────────────────
//
// A NODE is one HLOD object. Depth 1: one per cell unit of the world's plan whose grid states an HLOD layer
// (the HLOD WP10/WP11 built), standing in for that cell. If its layer is spatially loaded it LIVES in the
// layer's square cell (CellSize, by the centre of the cell it stands in for); every such HLOD CELL gets one node
// of the ParentLayer that stands in for the HLOD cell as a whole, built from the records of every world cell
// beneath it - and so on up the chain. A node of an always-loaded layer lives nowhere: it is resident from the
// first frame to the last.
//
// ── WHICH ONE IS DRAWN ────────────────────────────────────────────────────────────────────────────
//
// A node is drawn while it is resident (its HLOD cell Activated, or it lives nowhere) and what it stands in for
// is NOT visible (the world cell, or the child HLOD cell, Activated). Walk any world cell C that is not visible:
// its depth-1 node is drawn if its HLOD cell is resident; if not, that HLOD cell is not visible, so the parent
// standing in for it is drawn if ITS cell is resident; ... up to the top, which is always resident. So no
// distance has a hole. What could overlap is a world cell already visible inside an HLOD cell still loading
// (the parent of that HLOD cell drawn over it until the HLOD cell activates) - UE's band. It is closed by order,
// not by range: the world cell waits for the HLOD cell above it and the HLOD cell is held while the world cell
// is drawn (HLODRuntime::WorldGate / HLODGate, WorldPartitionHLODSwitch.hpp, THE SWAP, BY RESIDENCY).
//
// ── THE RESIDENCY ─────────────────────────────────────────────────────────────────────────────────
//
// MakeHLODResidency turns the hierarchy into an ordinary WorldPartitionPlan for a second ResidencyExecutor: one
// composite per node holding its records (indices into an HLOD record space the caller fills), the nodes that
// live nowhere always loaded, and one level-0 cell per HLOD cell on a grid per spatially loaded layer (CellSize,
// LoadingRange) - the same query, hysteresis, budget and retries as the world's own cells.
//
// PURE: a function of the partition and the plan, so Build HLODs (which builds every node) and Play (which
// streams them) get one hierarchy and one set of names.

#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace Desert::Core::Rules
{
    struct HLODNode
    {
        std::size_t Layer = kNoRecord; // index into WorldPartitionSerialized::HLODLayers
        // What it stands in for: a cell unit of the world's plan, or (ForHLODCell) a cell of HLODHierarchy::Cells.
        std::size_t StandsFor   = kNoRecord;
        bool        ForHLODCell = false;
        // The HLOD cell it lives in (index into HLODHierarchy::Cells); kNoRecord = resident the whole session.
        std::size_t Cell = kNoRecord;
        // The world's cell units whose records it is built from, ascending.
        std::vector<std::size_t> SourceUnits;
        // The union of the squares of what it stands in for.
        CellBounds Bounds;
        // Its key in the saved HLOD index: a depth-1 node is its cell unit's DescribeResidencyUnit (the WP-FAR-6B
        // row, unchanged); a parent is "<child layer>(X,Z)" of the HLOD cell it stands in for.
        std::string Name;
    };

    struct HLODCell
    {
        std::size_t              Layer = kNoRecord;
        CellCoord                Coord;
        CellBounds               Square;
        CellBounds               Bounds; // Square widened by its nodes' bounds
        std::vector<std::size_t> Nodes;  // ascending
    };

    struct HLODHierarchy
    {
        std::vector<HLODNode> Nodes; // depth-1 nodes in cell unit order, then parents in the order made
        // Ordered by layer grid, then X, then Z (the order the streaming query reads cells in).
        std::vector<HLODCell> Cells;
        // Per layer: its residency grid (MakeHLODResidency) when spatially loaded, else kNoRecord.
        std::vector<std::size_t> GridOfLayer;

        // Whether any HLOD is spatially loaded, i.e. whether a second residency streams HLOD cells.
        [[nodiscard]] bool Streams() const
        {
            return !Cells.empty();
        }
    };

    namespace Detail
    {
        [[nodiscard]] inline CellBounds UnionBounds( const CellBounds& a, const CellBounds& b )
        {
            return { std::min( a.MinX, b.MinX ), std::min( a.MinZ, b.MinZ ), std::max( a.MaxX, b.MaxX ),
                     std::max( a.MaxZ, b.MaxZ ) };
        }

        [[nodiscard]] inline std::size_t HLODLayerIndex( const std::vector<HLODLayerSerialized>& layers,
                                                         const std::string&                      name )
        {
            for ( std::size_t i = 0; i < layers.size(); ++i )
                if ( layers[i].Name == name )
                    return i;
            return kNoRecord;
        }
    } // namespace Detail

    // The hierarchy of @p partition over @p plan. Call after ValidateHLODLayers (a refused partition is refused
    // here too, by its message). A world whose layers are all always loaded gets only depth-1 nodes, none
    // living in a cell: exactly the HLODs WP11 kept resident.
    [[nodiscard]] inline Common::ResultStr<HLODHierarchy>
    PlanHLODHierarchy( const WorldPartitionSerialized& partition, const WorldPartitionPlan& plan )
    {
        if ( auto valid = ValidateHLODLayers( partition ); !valid )
            return Common::MakeError<HLODHierarchy>( valid.GetError() );
        HLODHierarchy out;
        if ( !partition.HLODLayers.has_value() )
            return Common::MakeSuccess( std::move( out ) );
        const std::vector<HLODLayerSerialized>& layers = *partition.HLODLayers;

        out.GridOfLayer.assign( layers.size(), kNoRecord );
        std::size_t grids = 0;
        for ( std::size_t i = 0; i < layers.size(); ++i )
            if ( layers[i].CellSize.has_value() )
                out.GridOfLayer[i] = grids++;

        // Depth 1: one node per world cell whose grid states a layer.
        for ( std::size_t c = 0; c < plan.Cells.size(); ++c )
        {
            const PlannedCell& cell = plan.Cells[c];
            const auto&        grid = partition.Grids.at( cell.Grid );
            if ( !grid.HLODLayer.has_value() )
                continue;
            HLODNode node;
            node.Layer       = Detail::HLODLayerIndex( layers, *grid.HLODLayer );
            node.StandsFor   = plan.AlwaysLoaded.size() + c;
            node.SourceUnits = { node.StandsFor };
            node.Bounds      = cell.Square;
            node.Name        = DescribeResidencyUnit( plan, node.StandsFor );
            out.Nodes.push_back( std::move( node ) );
        }

        // A layer's level is how many layers hang beneath it; children are gathered before their parent.
        std::vector<std::size_t> level( layers.size(), 0 );
        for ( std::size_t pass = 0; pass < layers.size(); ++pass )
            for ( std::size_t i = 0; i < layers.size(); ++i )
                if ( layers[i].ParentLayer.has_value() )
                {
                    const std::size_t parent = Detail::HLODLayerIndex( layers, *layers[i].ParentLayer );
                    level[parent]            = std::max( level[parent], level[i] + 1 );
                }
        std::vector<std::size_t> order( layers.size() );
        for ( std::size_t i = 0; i < order.size(); ++i )
            order[i] = i;
        std::stable_sort( order.begin(), order.end(),
                          [&]( std::size_t a, std::size_t b ) { return level[a] < level[b]; } );

        using Key = std::tuple<std::size_t, std::int32_t, std::int32_t>; // grid, X, Z
        std::map<Key, std::vector<std::size_t>> cellNodes;
        std::vector<std::optional<Key>>         livesIn;   // per node
        std::vector<std::optional<Key>>         standsKey; // per parent node
        for ( const std::size_t layer : order )
        {
            if ( out.GridOfLayer[layer] == kNoRecord )
                continue;
            const double size = *layers[layer].CellSize;
            std::map<std::pair<std::int32_t, std::int32_t>, std::vector<std::size_t>> mine;
            for ( std::size_t n = 0; n < out.Nodes.size(); ++n )
            {
                if ( out.Nodes[n].Layer != layer )
                    continue;
                const CellBounds& b  = out.Nodes[n].Bounds;
                const double      cx = 0.5 * ( static_cast<double>( b.MinX ) + b.MaxX );
                const double      cz = 0.5 * ( static_cast<double>( b.MinZ ) + b.MaxZ );
                mine[{ static_cast<std::int32_t>( std::floor( cx / size ) ),
                       static_cast<std::int32_t>( std::floor( cz / size ) ) }]
                     .push_back( n );
            }
            const std::size_t parent = Detail::HLODLayerIndex( layers, *layers[layer].ParentLayer );
            for ( const auto& [coord, members] : mine )
            {
                const Key key{ out.GridOfLayer[layer], coord.first, coord.second };
                cellNodes[key] = members;
                livesIn.resize( out.Nodes.size() );
                for ( const std::size_t n : members )
                    livesIn[n] = key;
                HLODNode up;
                up.Layer       = parent;
                up.ForHLODCell = true;
                up.Bounds      = out.Nodes[members.front()].Bounds;
                for ( const std::size_t n : members )
                {
                    up.Bounds = Detail::UnionBounds( up.Bounds, out.Nodes[n].Bounds );
                    up.SourceUnits.insert( up.SourceUnits.end(), out.Nodes[n].SourceUnits.begin(),
                                           out.Nodes[n].SourceUnits.end() );
                }
                std::sort( up.SourceUnits.begin(), up.SourceUnits.end() );
                up.SourceUnits.erase( std::unique( up.SourceUnits.begin(), up.SourceUnits.end() ),
                                      up.SourceUnits.end() );
                up.Name = std::format( "{}({},{})", layers[layer].Name, coord.first, coord.second );
                out.Nodes.push_back( std::move( up ) );
                standsKey.resize( out.Nodes.size() );
                standsKey.back() = key;
            }
        }
        livesIn.resize( out.Nodes.size() );
        standsKey.resize( out.Nodes.size() );

        // The cells in key order, which is the order the query needs (grid, then X, then Z).
        std::map<Key, std::size_t> cellIndex;
        for ( const auto& [key, members] : cellNodes )
        {
            const auto [grid, x, z] = key;
            std::size_t layer       = kNoRecord;
            for ( std::size_t i = 0; i < out.GridOfLayer.size(); ++i )
                if ( out.GridOfLayer[i] == grid )
                    layer = i;
            const float size = *layers[layer].CellSize;
            HLODCell    cell;
            cell.Layer  = layer;
            cell.Coord  = { x, z };
            cell.Square = { static_cast<float>( static_cast<double>( x ) * size ),
                            static_cast<float>( static_cast<double>( z ) * size ),
                            static_cast<float>( static_cast<double>( x + 1 ) * size ),
                            static_cast<float>( static_cast<double>( z + 1 ) * size ) };
            cell.Bounds = cell.Square;
            for ( const std::size_t n : members )
                cell.Bounds = Detail::UnionBounds( cell.Bounds, out.Nodes[n].Bounds );
            cell.Nodes = members;
            std::sort( cell.Nodes.begin(), cell.Nodes.end() );
            cellIndex.emplace( key, out.Cells.size() );
            out.Cells.push_back( std::move( cell ) );
        }
        for ( std::size_t n = 0; n < out.Nodes.size(); ++n )
        {
            if ( livesIn[n].has_value() )
                out.Nodes[n].Cell = cellIndex.at( *livesIn[n] );
            if ( standsKey[n].has_value() )
                out.Nodes[n].StandsFor = cellIndex.at( *standsKey[n] );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    // THE RESIDENCY OF THE HIERARCHY: a plan, its grids, and per node its residency unit and its records'
    // place in the HLOD record space (node n holds records [FirstRecord[n], FirstRecord[n] + its count)).
    struct HLODResidency
    {
        WorldPartitionPlan       Plan;
        WorldPartitionSerialized Partition;
        std::vector<std::size_t> UnitOfNode;
        std::vector<std::size_t> FirstRecord;
        std::size_t              Records = 0;
        // Per grid of Partition: the WORLD grids (by WorldPartitionGridSerialized::Name) whose cells that HLOD layer
        // stands in for - every grid whose HLODLayer chain passes through it. A source's TargetGrids names world
        // grids; HLODSources reads it through this.
        std::vector<std::vector<std::string>> StandsForGrids;
    };

    // @p recordsOfNode: per node of @p hierarchy, how many records its saved HLOD holds (0 for one that has none:
    // it stays in the hierarchy, standing in for nothing, so the chain above it is unchanged).
    [[nodiscard]] inline Common::ResultStr<HLODResidency>
    MakeHLODResidency( const WorldPartitionSerialized& partition, const HLODHierarchy& hierarchy,
                       std::span<const std::size_t> recordsOfNode )
    {
        if ( recordsOfNode.size() != hierarchy.Nodes.size() )
            return Common::MakeError<HLODResidency>(
                 std::format( "the HLOD residency was given {} record count(s) "
                              "for {} HLOD(s)",
                              recordsOfNode.size(), hierarchy.Nodes.size() ) );
        HLODResidency out;
        if ( partition.HLODLayers.has_value() )
            for ( std::size_t i = 0; i < partition.HLODLayers->size(); ++i )
            {
                if ( hierarchy.GridOfLayer.at( i ) == kNoRecord )
                    continue;
                const HLODLayerSerialized&   layer = ( *partition.HLODLayers )[i];
                WorldPartitionGridSerialized grid;
                grid.Name         = "HLOD " + layer.Name;
                grid.CellSize     = *layer.CellSize;
                grid.LoadingRange = *layer.LoadingRange;
                out.Partition.Grids.push_back( std::move( grid ) );
                std::vector<std::string> standsFor;
                for ( const WorldPartitionGridSerialized& world : partition.Grids )
                    for ( const HLODLayerSerialized* at =
                               world.HLODLayer.has_value() ? FindHLODLayer( partition, *world.HLODLayer ) : nullptr;
                          at != nullptr;
                          at = at->ParentLayer.has_value() ? FindHLODLayer( partition, *at->ParentLayer ) : nullptr )
                        if ( at == &layer )
                        {
                            standsFor.push_back( world.Name );
                            break;
                        }
                out.StandsForGrids.push_back( std::move( standsFor ) );
            }
        out.FirstRecord.resize( hierarchy.Nodes.size() );
        for ( std::size_t n = 0; n < hierarchy.Nodes.size(); ++n )
        {
            const HLODNode&  node = hierarchy.Nodes[n];
            PlannedComposite composite;
            out.FirstRecord[n] = out.Records;
            for ( std::size_t r = 0; r < recordsOfNode[n]; ++r )
                composite.Members.push_back( out.Records + r );
            out.Records += recordsOfNode[n];
            composite.Anchor    = composite.Members.empty() ? kNoRecord : composite.Members.front();
            composite.Footprint = node.Bounds;
            if ( node.Cell != kNoRecord )
            {
                const HLODCell& cell = hierarchy.Cells.at( node.Cell );
                composite.Grid       = hierarchy.GridOfLayer.at( cell.Layer );
                composite.Cell       = cell.Coord;
            }
            out.Plan.Composites.push_back( std::move( composite ) );
            if ( node.Cell == kNoRecord )
                out.Plan.AlwaysLoaded.push_back( n );
        }
        for ( const HLODCell& cell : hierarchy.Cells )
        {
            PlannedCell planned;
            planned.Grid       = hierarchy.GridOfLayer.at( cell.Layer );
            planned.Level      = 0;
            planned.Cell       = cell.Coord;
            planned.Square     = cell.Square;
            planned.Bounds     = cell.Bounds;
            planned.Composites = cell.Nodes;
            out.Plan.Cells.push_back( std::move( planned ) );
        }
        out.Plan.LevelCount = out.Plan.Cells.empty() ? 0 : 1;
        out.UnitOfNode.assign( hierarchy.Nodes.size(), kNoRecord );
        for ( std::size_t a = 0; a < out.Plan.AlwaysLoaded.size(); ++a )
            out.UnitOfNode[out.Plan.AlwaysLoaded[a]] = a;
        for ( std::size_t n = 0; n < hierarchy.Nodes.size(); ++n )
            if ( hierarchy.Nodes[n].Cell != kNoRecord )
                out.UnitOfNode[n] = out.Plan.AlwaysLoaded.size() + hierarchy.Nodes[n].Cell;
        return Common::MakeSuccess( std::move( out ) );
    }

    // THE SOURCES THE HLOD CELLS STREAM FOR. A source's TargetGrids names WORLD grids; the HLOD cells of a layer
    // stand in for the cells of the world grids beneath it, so a source streams an HLOD grid exactly when it
    // streams one of those. In UE the source carries a second list, TargetHLODLayers, set apart from TargetGrids;
    // here it is derived, because the swap (HLODRuntime::WorldGate) makes a world cell WAIT for the HLOD cell above
    // it, so a source naming "Main" read literally (never "HLOD <layer>") would hold its Main cells forever. A source
    // naming none streams every grid, HLOD grids included; one targeting no HLOD grid is left out (an emptied list
    // would read as "every grid").
    [[nodiscard]] inline std::vector<StreamingSource> HLODSources( const HLODResidency&             residency,
                                                                   std::span<const StreamingSource> sources )
    {
        std::vector<StreamingSource> out;
        out.reserve( sources.size() );
        for ( const StreamingSource& source : sources )
        {
            if ( source.TargetGrids.empty() )
            {
                out.push_back( source );
                continue;
            }
            StreamingSource mapped = source;
            mapped.TargetGrids.clear();
            for ( std::size_t g = 0; g < residency.Partition.Grids.size(); ++g )
                for ( const std::string& world : residency.StandsForGrids.at( g ) )
                    if ( std::find( source.TargetGrids.begin(), source.TargetGrids.end(), world ) !=
                         source.TargetGrids.end() )
                    {
                        mapped.TargetGrids.push_back( residency.Partition.Grids[g].Name );
                        break;
                    }
            if ( !mapped.TargetGrids.empty() )
                out.push_back( std::move( mapped ) );
        }
        return out;
    }

    // An HLOD residency unit's name for a report: the grid's ("HLOD <layer>") and the cell, or the node's name
    // for one resident the whole session.
    [[nodiscard]] inline std::string DescribeHLODUnit( const HLODHierarchy& hierarchy,
                                                       const HLODResidency& residency, std::size_t unit )
    {
        if ( unit < residency.Plan.AlwaysLoaded.size() )
            return "HLOD " + hierarchy.Nodes.at( residency.Plan.AlwaysLoaded[unit] ).Name;
        const PlannedCell& cell = residency.Plan.Cells.at( unit - residency.Plan.AlwaysLoaded.size() );
        return std::format( "{}({},{})", residency.Partition.Grids.at( cell.Grid ).Name, cell.Cell.X,
                            cell.Cell.Z );
    }
} // namespace Desert::Core::Rules
