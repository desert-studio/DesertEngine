#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Editor::SizeMap
{
    // ── WHAT AN ASSET DRAGS IN, BY SIZE (UE: the Size Map, AssetManagerEditor's SSizeMap) ─────────────────────
    //
    // The tree UE's SSizeMap::GatherDependenciesRecursively builds, over OUR asset registry: every root asset,
    // under it every asset its row names (the dependency edges the gather reads from the file's header - the
    // same edges a rename shows as referrers, Common/Content/AssetMove.hpp ReferrersOf), recursively, each asset
    // ONCE. An asset met a second time from under a DIFFERENT root asset is moved into one "Shared" box beside
    // the roots, as UE does, so the map shows what each root costs alone and what they cost together; met again
    // under the SAME root it stays where it was first placed (it is counted once, as the loader reads it once).
    //
    // THE SIZE IS THE ROW'S: the bytes the file occupies on disk, recorded by the registry gather. UE's other
    // mode, memory size, loads every asset in the tree to ask it - the cost the demand-driven model exists to
    // avoid - so it is not offered (Docs/World/05_asset_loading_model.md, cost table row 15).
    //
    // NOTHING IS DROPPED SILENTLY: an edge naming no row, and a redirector chain the registry refuses, are
    // listed in Tree::Unresolved with the referrer's key; a redirector edge is followed to the asset the
    // referrer actually loads (AssetRegistry::FollowRedirectors).
    enum class NodeRole : std::uint8_t
    {
        Asset,  // one registry row
        Self,   // an asset's own bytes, beside its dependencies (UE adds the same child to a node with children)
        Shared, // assets reached from more than one root asset
        Group,  // the map's root when it was asked for more than one asset
    };

    struct Node
    {
        NodeRole    Role = NodeRole::Asset;
        std::string Key;  // the row's stable key; empty for Shared and Group
        std::string Name; // the row's display name, else the file's stem
        std::string Kind; // the row's kind (ContentKinds); empty for Shared and Group
        uint64_t    OwnBytes   = 0;
        uint64_t    TotalBytes = 0;    // own bytes + every child's total
        std::size_t Assets     = 0;    // assets under this node, itself excluded
        int              Parent = -1; // index into Tree::Nodes; -1 for the root
        std::vector<int> Children;
    };

    struct Tree
    {
        std::vector<Node>        Nodes;
        int                      Root = -1;
        std::vector<std::string> Unresolved; // one sentence per edge the registry could not follow

        [[nodiscard]] bool Empty() const
        {
            return Root < 0;
        }
    };

    // The map of @p rootKeys (stable keys, AssetHandle::StableKeyForPath). REFUSED, naming the key: no key at
    // all, and a key with no registry row (a file the registry does not have has no edges to follow).
    [[nodiscard]] Common::ResultStr<Tree> Build( const Common::Utils::AssetRegistry& registry,
                                                 const std::vector<std::string>&     rootKeys );

    struct Rect
    {
        float X = 0.0f;
        float Y = 0.0f;
        float W = 0.0f;
        float H = 0.0f;
    };

    struct Placed
    {
        int  Node = -1;
        Rect Box;
    };

    // The children of @p parent with a non-zero total, laid out inside @p bounds by area, squarified (Bruls,
    // Huizing, van Wijk 2000 - the layout UE's FTreeMap uses): largest first, each row grown while it makes the
    // worst aspect ratio in it no worse.
    [[nodiscard]] std::vector<Placed> Squarify( const Tree& tree, int parent, const Rect& bounds );

    // "512 B", "14.25 KB", "3.10 MB", "1.02 GB" (UE's MakeBestSizeString: binary units, two decimals).
    [[nodiscard]] std::string FormatBytes( uint64_t bytes );
} // namespace Desert::Editor::SizeMap
