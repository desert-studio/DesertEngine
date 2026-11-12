#include "AssetReferenceGraph.hpp"

#include <Common/Content/ContentKinds.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <utility>

namespace Desert::Editor::ReferenceGraph
{
    namespace
    {
        using Common::Utils::AssetRegistry;
        using Common::Utils::AssetRegistryEntry;

        bool IsRedirector( const AssetRegistryEntry& row )
        {
            return row.Kind == Common::Content::KindName( Common::Content::ContentKind::Redirector );
        }

        void SortUnique( std::vector<Edge>& edges )
        {
            std::sort( edges.begin(), edges.end(),
                       []( const Edge& a, const Edge& b ) { return a.Row < b.Row; } );
            edges.erase( std::unique( edges.begin(), edges.end(),
                                      []( const Edge& a, const Edge& b ) { return a.Row == b.Row; } ),
                         edges.end() );
        }

        const char* SideName( const Side side )
        {
            switch ( side )
            {
                case Side::Root:
                    return "root";
                case Side::Referencers:
                    return "referencers";
                case Side::Dependencies:
                    return "dependencies";
            }
            return "unknown";
        }
    } // namespace

    std::string ExpansionKey( const Side side, const std::string& key )
    {
        return std::format( "{}|{}", SideName( side ), key );
    }

    std::string DisplayName( const AssetRegistryEntry& row )
    {
        if ( !row.DisplayName.empty() )
            return row.DisplayName;
        // "assets:Materials/M_Rock.demat" -> "M_Rock"; a key with no tag is a path as it stands.
        const std::size_t tag = row.Key.find( ':' );
        const std::string path = tag == std::string::npos ? row.Key : row.Key.substr( tag + 1 );
        return std::filesystem::path( path ).stem().string();
    }

    Index BuildIndex( const AssetRegistry& registry )
    {
        const std::vector<AssetRegistryEntry>& rows = registry.Entries();
        Index                                  index;
        index.Dependencies.resize( rows.size() );
        index.Referencers.resize( rows.size() );
        index.Unresolved.resize( rows.size() );
        for ( std::size_t i = 0; i < rows.size(); ++i )
        {
            index.RowByKey.emplace( rows[i].Key, i );
            index.Kinds.push_back( rows[i].Kind );
        }
        std::sort( index.Kinds.begin(), index.Kinds.end() );
        index.Kinds.erase( std::unique( index.Kinds.begin(), index.Kinds.end() ), index.Kinds.end() );

        for ( std::size_t i = 0; i < rows.size(); ++i )
        {
            const AssetRegistryEntry& row = rows[i];
            for ( const uint64_t handle : row.Dependencies )
            {
                const AssetRegistryEntry* named = registry.FindByHandle( handle );
                if ( named == nullptr )
                {
                    index.Unresolved[i].push_back(
                         std::format( "'{}' names asset {:016x}, which no registry row states", row.Key, handle ) );
                    continue;
                }
                std::string via;
                if ( IsRedirector( *named ) )
                {
                    const auto followed = registry.FollowRedirectors( *named );
                    if ( !followed )
                    {
                        index.Unresolved[i].push_back( std::format( "'{}': {}", row.Key, followed.GetError() ) );
                        continue;
                    }
                    via   = named->Key;
                    named = followed.GetValue();
                }
                const auto target = index.RowByKey.find( named->Key );
                if ( target == index.RowByKey.end() || target->second == i )
                    continue; // a row names itself: not a reference to draw
                index.Dependencies[i].push_back( { target->second, via } );
                // A redirector is the trace of a move, not an asset anyone edits (AssetMove.hpp ReferrersOf).
                if ( !IsRedirector( row ) )
                    index.Referencers[target->second].push_back( { i, std::move( via ) } );
            }
        }
        // Rows are held in key order, so row order is key order.
        for ( std::vector<Edge>& edges : index.Dependencies )
            SortUnique( edges );
        for ( std::vector<Edge>& edges : index.Referencers )
            SortUnique( edges );
        return index;
    }

    namespace
    {
        class Builder
        {
        public:
            Builder( const AssetRegistry& registry, const Index& index, const Settings& settings )
                 : m_Rows( registry.Entries() ), m_Index( index ), m_Settings( settings )
            {
            }

            Common::BoolResultStr AddRoots( const std::vector<std::string>& rootKeys )
            {
                if ( rootKeys.empty() )
                    return Common::MakeError<bool>( "Reference Viewer: no asset to look at" );
                for ( const std::string& key : rootKeys )
                {
                    const auto row = m_Index.RowByKey.find( key );
                    if ( row == m_Index.RowByKey.end() )
                        return Common::MakeError<bool>(
                             std::format( "Reference Viewer: '{}' has no asset registry row", key ) );
                    if ( m_PlacedReferencers.contains( row->second ) )
                        continue; // named twice in the selection
                    const int node = AddAsset( row->second, Side::Root, 0 );
                    m_Graph.Roots.push_back( node );
                    m_PlacedReferencers.emplace( row->second, node );
                    m_PlacedDependencies.emplace( row->second, node );
                }
                return Common::MakeSuccess( true );
            }

            void Walk( const Side side )
            {
                const bool                             left  = side == Side::Referencers;
                const int                              limit = std::clamp( left ? m_Settings.ReferencerDepth
                                                                                : m_Settings.DependencyDepth,
                                                                           0, kMaxDepth );
                const std::size_t                      breadth =
                     static_cast<std::size_t>( std::clamp( m_Settings.BreadthLimit, 1, kMaxBreadth ) );
                const std::vector<std::vector<Edge>>& edges  = left ? m_Index.Referencers : m_Index.Dependencies;
                std::map<std::size_t, int>&           placed = left ? m_PlacedReferencers : m_PlacedDependencies;

                std::vector<int> frontier = m_Graph.Roots;
                for ( int depth = 1; depth <= limit && !frontier.empty(); ++depth )
                {
                    std::vector<int> next;
                    for ( const int parent : frontier )
                    {
                        const std::size_t parentRow = m_Graph.Nodes[static_cast<std::size_t>( parent )].Row;
                        const std::string expandKey =
                             ExpansionKey( side, m_Graph.Nodes[static_cast<std::size_t>( parent )].Key );
                        const bool expanded = m_Settings.Expanded.contains( expandKey );

                        if ( !left )
                            for ( const std::string& sentence : m_Index.Unresolved[parentRow] )
                            {
                                Node missing;
                                missing.Role   = NodeRole::Unresolved;
                                missing.Name   = "Missing";
                                missing.Detail = sentence;
                                Link( parent, Place( std::move( missing ), side, depth ), side, {} );
                            }

                        std::size_t shown  = 0;
                        std::size_t hidden = 0;
                        for ( const Edge& edge : edges[parentRow] )
                        {
                            if ( m_Settings.HiddenKinds.contains( m_Rows[edge.Row].Kind ) )
                                continue;
                            if ( const auto at = placed.find( edge.Row ); at != placed.end() )
                            {
                                Link( parent, at->second, side, edge.Via );
                                continue;
                            }
                            if ( !expanded && shown >= breadth )
                            {
                                ++hidden;
                                continue;
                            }
                            ++shown;
                            const int node = AddAsset( edge.Row, side, depth );
                            placed.emplace( edge.Row, node );
                            next.push_back( node );
                            Link( parent, node, side, edge.Via );
                        }
                        if ( hidden > 0 )
                        {
                            Node more;
                            more.Role      = NodeRole::Overflow;
                            more.Name      = std::format( "+{} more", hidden );
                            more.Hidden    = hidden;
                            more.ExpandKey = expandKey;
                            Link( parent, Place( std::move( more ), side, depth ), side, {} );
                        }
                    }
                    frontier = std::move( next );
                }
            }

            Graph Take()
            {
                return std::move( m_Graph );
            }

        private:
            int AddAsset( const std::size_t row, const Side side, const int depth )
            {
                const AssetRegistryEntry& entry = m_Rows[row];
                Node                      node;
                node.Row             = row;
                node.Key             = entry.Key;
                node.Name            = DisplayName( entry );
                node.Kind            = entry.Kind;
                node.Bytes           = entry.Size;
                node.ReferencerCount = m_Index.Referencers[row].size();
                node.DependencyCount = m_Index.Dependencies[row].size() + m_Index.Unresolved[row].size();
                return Place( std::move( node ), side, depth );
            }

            int Place( Node node, const Side side, const int depth )
            {
                node.Where  = side;
                node.Depth  = depth;
                node.Column = side == Side::Referencers ? -depth : depth;
                node.Slot   = m_Graph.ColumnSizes[node.Column]++;
                m_Graph.Nodes.push_back( std::move( node ) );
                return static_cast<int>( m_Graph.Nodes.size() ) - 1;
            }

            // The referencer is always From: on the left side the new node names its parent.
            void Link( const int parent, const int child, const Side side, std::string via )
            {
                const int from = side == Side::Referencers ? child : parent;
                const int to   = side == Side::Referencers ? parent : child;
                for ( const ReferenceGraph::Link& existing : m_Graph.Links )
                    if ( existing.From == from && existing.To == to )
                        return;
                m_Graph.Links.push_back( ReferenceGraph::Link{ from, to, std::move( via ) } );
            }

            const std::vector<AssetRegistryEntry>& m_Rows;
            const Index&                           m_Index;
            const Settings&                        m_Settings;
            Graph                                  m_Graph;
            std::map<std::size_t, int>             m_PlacedReferencers;
            std::map<std::size_t, int>             m_PlacedDependencies;
        };
    } // namespace

    Common::ResultStr<Graph> Build( const AssetRegistry& registry, const Index& index,
                                    const std::vector<std::string>& rootKeys, const Settings& settings )
    {
        Builder builder( registry, index, settings );
        if ( const auto roots = builder.AddRoots( rootKeys ); !roots )
            return Common::MakeError<Graph>( roots.GetError() );
        builder.Walk( Side::Referencers );
        builder.Walk( Side::Dependencies );
        return Common::MakeSuccess( builder.Take() );
    }

    std::vector<std::string> KeysOn( const Graph& graph, const Side side )
    {
        std::vector<std::string> keys;
        for ( const Node& node : graph.Nodes )
            if ( node.Role == NodeRole::Asset && node.Where == side )
                keys.push_back( node.Key );
        std::sort( keys.begin(), keys.end() );
        return keys;
    }
} // namespace Desert::Editor::ReferenceGraph
