#include "AssetSizeMap.hpp"

#include <Common/Content/ContentKinds.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Desert::Editor::SizeMap
{
    namespace
    {
        using SizeMapRow = Common::Utils::AssetRegistryEntry;

        std::string SizeMapNameOf( const SizeMapRow& row )
        {
            if ( !row.DisplayName.empty() )
                return row.DisplayName;
            const std::size_t colon    = row.Key.find( ':' );
            const std::string relative = colon == std::string::npos ? row.Key : row.Key.substr( colon + 1 );
            return std::filesystem::path( relative ).stem().generic_string();
        }

        // UE SSizeMap::GatherDependenciesRecursively, over indices into Tree::Nodes instead of shared pointers.
        struct SizeMapGather
        {
            const Common::Utils::AssetRegistry&  Rows;
            Tree&                                Out;
            std::unordered_set<std::string>      RootKeys;
            std::unordered_map<std::string, int> Visited; // row key -> node
            int                                  SharedNode = -1;

            int Add( Node node, const int parent )
            {
                node.Parent = parent;
                Out.Nodes.push_back( std::move( node ) );
                const int index = static_cast<int>( Out.Nodes.size() ) - 1;
                if ( parent >= 0 )
                    Out.Nodes[static_cast<std::size_t>( parent )].Children.push_back( index );
                return index;
            }

            Node& At( const int index )
            {
                return Out.Nodes[static_cast<std::size_t>( index )];
            }

            // The child of the map's root that holds @p node; -1 when @p node is the map's root itself.
            int RootLevelAncestor( const int node )
            {
                int at = node;
                while ( At( at ).Parent >= 0 && At( At( at ).Parent ).Parent >= 0 )
                    at = At( at ).Parent;
                return At( at ).Parent < 0 ? -1 : at;
            }

            void MoveToShared( const int node )
            {
                if ( SharedNode < 0 )
                {
                    Node shared;
                    shared.Role = NodeRole::Shared;
                    shared.Name = "Shared";
                    SharedNode  = Add( std::move( shared ), Out.Root );
                }
                std::vector<int>& siblings = At( At( node ).Parent ).Children;
                siblings.erase( std::remove( siblings.begin(), siblings.end(), node ), siblings.end() );
                At( node ).Parent = SharedNode;
                At( SharedNode ).Children.push_back( node );
            }

            // The rows @p row names, a redirector edge followed to the asset it leads to.
            std::vector<const SizeMapRow*> DependenciesOf( const SizeMapRow& row )
            {
                std::vector<const SizeMapRow*> rows;
                for ( const uint64_t edge : row.Dependencies )
                {
                    const SizeMapRow* named = Rows.FindByHandle( edge );
                    if ( named == nullptr )
                    {
                        Out.Unresolved.push_back(
                             std::format( "{} names {:016x}, which no registry row states", row.Key, edge ) );
                        continue;
                    }
                    if ( named->Kind == Common::Content::KindName( Common::Content::ContentKind::Redirector ) )
                    {
                        const auto followed = Rows.FollowRedirectors( *named );
                        if ( !followed )
                        {
                            Out.Unresolved.push_back(
                                 std::format( "{} names the redirector {}: {}", row.Key, named->Key,
                                              followed.GetError() ) );
                            continue;
                        }
                        named = followed.GetValue();
                    }
                    if ( named->Key != row.Key )
                        rows.push_back( named );
                }
                return rows;
            }

            void Gather( const std::vector<const SizeMapRow*>& rows, const int parent )
            {
                for ( const SizeMapRow* row : rows )
                {
                    if ( const auto seen = Visited.find( row->Key ); seen != Visited.end() )
                    {
                        const int existing = seen->second;
                        if ( SharedNode >= 0 && At( existing ).Parent == SharedNode )
                            continue;
                        // A root asset, or one placed straight under the map's root, is never moved: the map
                        // shows what the roots cost once all of them are loaded, and a root another root names
                        // is part of that already.
                        const int  existingParent = At( existing ).Parent;
                        const bool atRootLevel    = existingParent < 0 || At( existingParent ).Parent < 0 ||
                                                 RootKeys.contains( row->Key );
                        if ( atRootLevel )
                            continue;
                        // Named again from under the same root asset: counted once, where it was first met.
                        if ( RootLevelAncestor( parent ) == RootLevelAncestor( existing ) )
                            continue;
                        MoveToShared( existing );
                        continue;
                    }

                    Node node;
                    node.Key        = row->Key;
                    node.Name       = SizeMapNameOf( *row );
                    node.Kind       = row->Kind;
                    node.OwnBytes   = row->Size;
                    const int index = Add( std::move( node ), parent );
                    Visited.emplace( row->Key, index );

                    const std::vector<const SizeMapRow*> dependencies = DependenciesOf( *row );
                    if ( !dependencies.empty() )
                        Gather( dependencies, index );
                }
            }
        };

        // UE SSizeMap::FinalizeNodesRecursively: totals bottom-up, and an asset with children gains a Self child
        // carrying its own bytes, so the box of a subdivided asset still shows what the asset itself weighs.
        void SizeMapFinalize( Tree& tree, const int index )
        {
            const std::vector<int> children = tree.Nodes[static_cast<std::size_t>( index )].Children;
            uint64_t               total    = 0;
            std::size_t            assets   = 0;
            for ( const int child : children )
            {
                SizeMapFinalize( tree, child );
                const Node& finished = tree.Nodes[static_cast<std::size_t>( child )];
                total += finished.TotalBytes;
                assets += finished.Assets + ( finished.Role == NodeRole::Asset ? 1 : 0 );
            }

            Node& node = tree.Nodes[static_cast<std::size_t>( index )];
            if ( node.Role != NodeRole::Asset || children.empty() )
            {
                node.TotalBytes = node.OwnBytes + total;
                node.Assets     = assets;
                return;
            }

            Node self;
            self.Role       = NodeRole::Self;
            self.Key        = node.Key;
            self.Name       = node.Name;
            self.Kind       = node.Kind;
            self.OwnBytes   = node.OwnBytes;
            self.TotalBytes = node.OwnBytes;
            self.Parent     = index;
            const uint64_t own = node.OwnBytes;
            tree.Nodes.push_back( std::move( self ) );
            // `node` may dangle after the push: index again.
            Node& owner = tree.Nodes[static_cast<std::size_t>( index )];
            owner.Children.push_back( static_cast<int>( tree.Nodes.size() ) - 1 );
            owner.TotalBytes = own + total;
            owner.Assets     = assets;
        }

        double SizeMapWorstRatio( const double rowArea, const double largest, const double smallest,
                                  const double side )
        {
            if ( rowArea <= 0.0 || smallest <= 0.0 || side <= 0.0 )
                return 1.0e30;
            const double sideSq = side * side;
            const double areaSq = rowArea * rowArea;
            return std::max( sideSq * largest / areaSq, areaSq / ( sideSq * smallest ) );
        }
    } // namespace

    Common::ResultStr<Tree> Build( const Common::Utils::AssetRegistry& registry,
                                   const std::vector<std::string>&     rootKeys )
    {
        if ( rootKeys.empty() )
            return Common::MakeError<Tree>( "Size Map: no asset was given" );

        std::vector<const SizeMapRow*> roots;
        for ( const std::string& key : rootKeys )
        {
            const SizeMapRow* row = registry.FindByKey( key );
            if ( row == nullptr )
                return Common::MakeError<Tree>( std::format(
                     "Size Map: '{}' has no row in the asset registry, so nothing is known of what it names", key ) );
            if ( std::find( roots.begin(), roots.end(), row ) == roots.end() )
                roots.push_back( row );
        }

        Tree tree;
        Node group;
        group.Role = NodeRole::Group;
        tree.Nodes.push_back( std::move( group ) );
        tree.Root = 0;

        SizeMapGather gather{ registry, tree, {}, {}, -1 };
        for ( const SizeMapRow* row : roots )
            gather.RootKeys.insert( row->Key );
        gather.Gather( roots, tree.Root );
        SizeMapFinalize( tree, tree.Root );

        // ONE ASSET ASKED FOR AND NOTHING SHARED: that asset is the map (UE shows a single asset as the root).
        Node& root = tree.Nodes[0];
        if ( root.Children.size() == 1 )
        {
            tree.Root                                                   = root.Children.front();
            tree.Nodes[static_cast<std::size_t>( tree.Root )].Parent = -1;
        }
        else
            tree.Nodes[0].Name = std::format( "{} assets", roots.size() );
        return Common::MakeSuccess( std::move( tree ) );
    }

    std::vector<Placed> Squarify( const Tree& tree, const int parent, const Rect& bounds )
    {
        std::vector<Placed> placed;
        if ( parent < 0 || bounds.W <= 0.0f || bounds.H <= 0.0f )
            return placed;

        std::vector<int> items;
        double           total = 0.0;
        for ( const int child : tree.Nodes[static_cast<std::size_t>( parent )].Children )
        {
            const uint64_t bytes = tree.Nodes[static_cast<std::size_t>( child )].TotalBytes;
            if ( bytes == 0 )
                continue;
            items.push_back( child );
            total += static_cast<double>( bytes );
        }
        if ( items.empty() )
            return placed;
        std::stable_sort( items.begin(), items.end(),
                          [&tree]( const int a, const int b )
                          {
                              return tree.Nodes[static_cast<std::size_t>( a )].TotalBytes >
                                     tree.Nodes[static_cast<std::size_t>( b )].TotalBytes;
                          } );

        const double scale = static_cast<double>( bounds.W ) * static_cast<double>( bounds.H ) / total;
        const auto   area  = [&]( const std::size_t i )
        { return static_cast<double>( tree.Nodes[static_cast<std::size_t>( items[i] )].TotalBytes ) * scale; };

        double      freeX = bounds.X, freeY = bounds.Y, freeW = bounds.W, freeH = bounds.H;
        std::size_t next  = 0;
        while ( next < items.size() )
        {
            const double side    = std::min( freeW, freeH );
            std::size_t  end     = next + 1;
            double       rowArea = area( next );
            double       worst   = SizeMapWorstRatio( rowArea, area( next ), area( next ), side );
            while ( end < items.size() )
            {
                const double grown      = rowArea + area( end );
                const double grownWorst = SizeMapWorstRatio( grown, area( next ), area( end ), side );
                if ( grownWorst > worst )
                    break;
                worst   = grownWorst;
                rowArea = grown;
                ++end;
            }

            // The row is a strip along the SHORT side: a column at the left of a wide space, a band at the top
            // of a tall one.
            const bool   column    = freeW >= freeH;
            const double thickness = side > 0.0 ? rowArea / side : 0.0;
            double       offset    = 0.0;
            for ( std::size_t i = next; i < end; ++i )
            {
                const double length = thickness > 0.0 ? area( i ) / thickness : 0.0;
                Rect         box;
                if ( column )
                    box = Rect{ static_cast<float>( freeX ), static_cast<float>( freeY + offset ),
                                static_cast<float>( thickness ), static_cast<float>( length ) };
                else
                    box = Rect{ static_cast<float>( freeX + offset ), static_cast<float>( freeY ),
                                static_cast<float>( length ), static_cast<float>( thickness ) };
                placed.push_back( Placed{ items[i], box } );
                offset += length;
            }
            if ( column )
            {
                freeX += thickness;
                freeW = std::max( 0.0, freeW - thickness );
            }
            else
            {
                freeY += thickness;
                freeH = std::max( 0.0, freeH - thickness );
            }
            next = end;
        }
        return placed;
    }

    std::string FormatBytes( const uint64_t bytes )
    {
        constexpr double kKilo = 1024.0;
        const double     value = static_cast<double>( bytes );
        if ( value < kKilo )
            return std::format( "{} B", bytes );
        if ( value < kKilo * kKilo )
            return std::format( "{:.2f} KB", value / kKilo );
        if ( value < kKilo * kKilo * kKilo )
            return std::format( "{:.2f} MB", value / ( kKilo * kKilo ) );
        return std::format( "{:.2f} GB", value / ( kKilo * kKilo * kKilo ) );
    }
} // namespace Desert::Editor::SizeMap
