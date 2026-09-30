#pragma once

/**
 * LINKED ANIM LAYERS — SWAPPABLE SUB-GRAPHS BEHIND A DECLARED INTERFACE (UE: Anim Layer Interface +
 * FAnimNode_LinkedAnimLayer + LinkAnimClassLayers).
 *
 * An INTERFACE asset declares layer functions by name, each with named input poses. A graph calls a layer
 * through a `LinkedAnimLayerNode` naming the function, never the implementation. At runtime an
 * IMPLEMENTING graph (an AnimGraph asset, by GUID) is linked for the interface — "armed with a rifle"
 * links the rifle graph, "unarmed" the default — and every node naming that interface's functions now
 * evaluates the implementing graph's function. Unlinked, a layer is the interface's DEFAULT: its first
 * input pose passed through, so a character with nothing linked still animates.
 *
 * INVARIANTS: an implementation is linked only if it implements EVERY function of the interface (refused
 * by the missing name otherwise — a half-implemented interface is a T-pose on the first call); one
 * implementation per interface per instance at a time; linking is a runtime switch and stores nothing
 * in the graph asset.
 */

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <string>
#include <vector>

namespace Desert::Animation::Graph
{
    struct AnimLayerFunction
    {
        std::string              Name;
        std::vector<std::string> InputPoses; ///< named pose pins; empty = the layer takes no input
        std::string              Group;      ///< UE's layer group: layers of one group share an instance
    };

    struct AnimLayerInterface
    {
        Common::Content::AssetGuid     Guid;
        std::string                    Name;
        std::vector<AnimLayerFunction> Functions;
    };

    /// The graph node. Stores names, resolves through the instance's `LinkedLayerTable`.
    struct LinkedAnimLayerNode
    {
        Common::Content::AssetGuid Interface;
        std::string                Function;
    };

    /// What an implementing graph declares it implements: the interface + its function names.
    struct LayerImplementation
    {
        Common::Content::AssetGuid Graph;
        Common::Content::AssetGuid Interface;
        std::vector<std::string>   ImplementedFunctions;
    };

    /// Per anim-instance runtime table (UE: the linked instances of one UAnimInstance).
    class LinkedLayerTable
    {
    public:
        /// Refuses an implementation of another interface, and one missing a function (named).
        [[nodiscard]] Common::BoolResultStr Link( const AnimLayerInterface&  anInterface,
                                                  const LayerImplementation& implementation );
        void                                Unlink( const Common::Content::AssetGuid& anInterface );

        /// The graph implementing @p node's function, or a null GUID = the interface default (pass-through).
        [[nodiscard]] Common::Content::AssetGuid Resolve( const LinkedAnimLayerNode& node ) const;

    private:
        struct Entry
        {
            Common::Content::AssetGuid Interface;
            Common::Content::AssetGuid Graph;
        };
        std::vector<Entry> m_Links;
    };
} // namespace Desert::Animation::Graph
