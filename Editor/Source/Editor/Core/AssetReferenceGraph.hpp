#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor::ReferenceGraph
{
    // ── WHO NAMES THIS ASSET, AND WHAT IT NAMES (UE: the Reference Viewer, EdGraph_ReferenceViewer) ─────────────
    //
    // The graph UE's UEdGraph_ReferenceViewer::RebuildGraph builds, over OUR asset registry: the root assets in the
    // middle, every asset whose row names them to the LEFT (referencers, IAssetRegistry::GetReferencers), every
    // asset their rows name to the RIGHT (dependencies, GetDependencies), each side walked breadth first to its own
    // depth limit. The edges are the registry rows' dependency edges - the header's dependency GUIDs, read without
    // loading anything (Common/Content/ContentScan.cpp) - the same edges a rename lists as referrers
    // (Common/Content/AssetMove.hpp ReferrersOf) and the Size Map walks (Editor/Core/AssetSizeMap.hpp). It is not
    // the Asset References panel's text scan, which is a search aid; this is what the loader will follow.
    //
    // UE's knobs, with UE's defaults: Search Referencers Depth 1, Search Dependencies Depth 1, Search Breadth Limit
    // 20 - a node with more neighbours than the limit shows the first ones (key order) and one collapsed "+N" node
    // that opens the rest - and a kind filter that prunes a kind out of the walk. "Show Duplicates" is UE's off
    // state: an asset is drawn once per side, and a second path to it is a second link to the same node.
    //
    // NOTHING IS DROPPED SILENTLY: an edge naming no row, and a redirector chain the registry refuses, are drawn as
    // a "Missing" node on the dependency side with the registry's sentence. A redirector is followed to the asset
    // the referrer actually loads (AssetRegistry::FollowRedirectors) and the link remembers the redirector's key;
    // a redirector row is never a referencer node (it is the move's trace, as ReferrersOf has it).

    struct Edge
    {
        std::size_t Row = 0; // index into AssetRegistry::Entries()
        std::string Via;     // the redirector key the edge was followed through; empty = it names the row itself
    };

    // Both directions of every row's edges, built once per registry snapshot (the reverse direction is a walk over
    // every row, which a rebuild on each depth change must not repeat).
    struct Index
    {
        std::unordered_map<std::string, std::size_t> RowByKey;
        std::vector<std::vector<Edge>>               Dependencies; // per row, in the target's key order
        std::vector<std::vector<Edge>>               Referencers;  // per row, in the referrer's key order
        std::vector<std::vector<std::string>>        Unresolved;   // per row: one sentence per edge naming no row
        std::vector<std::string>                     Kinds;        // every kind a row states, sorted
    };

    [[nodiscard]] Index BuildIndex( const Common::Utils::AssetRegistry& registry );

    enum class Side : std::uint8_t
    {
        Root,
        Referencers,  // left of the roots
        Dependencies, // right of the roots
    };

    enum class NodeRole : std::uint8_t
    {
        Asset,      // one registry row
        Overflow,   // the neighbours past the breadth limit, collapsed into one node
        Unresolved, // an edge the registry could not follow
    };

    struct Settings
    {
        int                   ReferencerDepth = 1;
        int                   DependencyDepth = 1;
        int                   BreadthLimit    = 20;
        std::set<std::string> HiddenKinds;
        std::set<std::string> Expanded; // ExpansionKey of every node whose collapsed neighbours were opened
    };

    inline constexpr int kMaxDepth   = 16;
    inline constexpr int kMaxBreadth = 1000;

    // The Settings::Expanded entry of the node @p key on @p side ("<side>|<key>": one asset may be expanded on the
    // dependency side and collapsed on the referencer side).
    [[nodiscard]] std::string ExpansionKey( Side side, const std::string& key );

    struct Node
    {
        NodeRole    Role  = NodeRole::Asset;
        Side        Where = Side::Root;
        int         Depth = 0; // 0 for a root, then 1.. outward
        int         Column = 0; // -Depth on the referencer side, +Depth on the dependency side
        int         Slot   = 0; // the node's place in its column, top to bottom
        std::size_t Row    = 0; // Asset only
        std::string Key;        // Asset: the row's stable key
        std::string Name;       // Asset: the row's display name, else the file's stem; Overflow: "+N more"
        std::string Kind;       // Asset: the row's kind
        uint64_t    Bytes = 0;  // Asset: the row's size on disk
        std::size_t ReferencerCount = 0; // Asset: every referencer the registry knows, shown or not
        std::size_t DependencyCount = 0; // Asset: every dependency edge, unresolved ones included
        std::size_t Hidden = 0;          // Overflow: the neighbours it stands for
        std::string ExpandKey;           // Overflow: the Settings::Expanded entry that opens it
        std::string Detail;              // Unresolved: the registry's sentence
    };

    // `From` names `To` (the referencer is always From), so every link runs left to right.
    struct Link
    {
        int         From = -1;
        int         To   = -1;
        std::string Via;
    };

    struct Graph
    {
        std::vector<Node>  Nodes;
        std::vector<Link>  Links;
        std::vector<int>   Roots;
        std::map<int, int> ColumnSizes; // column -> nodes in it
    };

    // The graph of @p rootKeys (stable keys, AssetHandle::StableKeyForPath). REFUSED, naming the key: no key at
    // all, and a key with no registry row.
    [[nodiscard]] Common::ResultStr<Graph> Build( const Common::Utils::AssetRegistry& registry, const Index& index,
                                                  const std::vector<std::string>& rootKeys,
                                                  const Settings&                 settings );

    // The row's display name, else its file's stem.
    [[nodiscard]] std::string DisplayName( const Common::Utils::AssetRegistryEntry& row );

    // The keys of the asset nodes drawn on @p side, sorted (UE: Copy Referencing / Referenced Objects List).
    [[nodiscard]] std::vector<std::string> KeysOn( const Graph& graph, Side side );
} // namespace Desert::Editor::ReferenceGraph
