#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/{Public,Private}/ProceduralFoliageBroadphase.{h,cpp} and the
// TQuadTree it wraps (Engine/Public/GenericQuadTree.h).
// Adapted: an entry carries the instance's id, location and radii instead of a pointer to it, so the tree answers
// overlap queries on its own; elements live at the deepest node that holds their box whole (a loose split, no
// copies).

#include <Engine/World/Foliage/Procedural/ProceduralFoliageInstance.hpp>

#include <cstdint>
#include <vector>

namespace Desert::World::Foliage::Procedural
{
    /// An overlap the broadphase found for a queried instance (UE FProceduralFoliageOverlap).
    struct BroadphaseOverlap
    {
        uint32_t    Other = 0;
        OverlapKind Kind  = OverlapKind::Collision;
    };

    /**
     * @brief The quadtree a tile files its living instances in, by the box that holds both their circles.
     *
     * The tree covers [-2 TileSize, 2 TileSize] on both axes (UE: seeds spread past the tile, and a composite tile
     * holds its neighbours' overlap). Answers are in tree order, which depends only on the order of inserts and
     * removals, so a simulation that issues them in a fixed order gets the same answers on every run.
     */
    class ProceduralFoliageBroadphase
    {
    public:
        ProceduralFoliageBroadphase() = default;
        ProceduralFoliageBroadphase( float tileSize, float minimumQuadTreeSize );

        void Empty();

        /// Whether the box around the instance's circles touches the tree at all (UE TestAgainstAABB).
        [[nodiscard]] bool TestAgainstAABB( glm::vec2 location, const InstanceRadii& radii ) const;
        void               Insert( uint32_t id, glm::vec2 location, const InstanceRadii& radii );
        /// Removes what Insert filed under @p id with the same location and radii; false if it is not there.
        bool Remove( uint32_t id, glm::vec2 location, const InstanceRadii& radii );

        /// Every filed instance other than @p id whose collision or shade circle touches this one's.
        void GetOverlaps( uint32_t id, glm::vec2 location, const InstanceRadii& radii,
                          std::vector<BroadphaseOverlap>& out ) const;
        /// Every filed instance whose box touches @p box.
        void GetInstancesInBox( const Box2& box, std::vector<uint32_t>& out ) const;

        [[nodiscard]] size_t Size() const
        {
            return m_Count;
        }

    private:
        struct Entry
        {
            uint32_t      Id = 0;
            glm::vec2     Location{ 0.0f };
            InstanceRadii Radii;
            Box2          Box;
        };
        struct Node
        {
            Box2               Bounds;
            int32_t            FirstChild = -1; ///< four consecutive nodes, or -1 for a leaf
            std::vector<Entry> Entries;
        };

        [[nodiscard]] int32_t ChildHolding( const Node& node, const Box2& box ) const;
        void                  Split( int32_t nodeIndex );
        template <typename Visit>
        void Query( const Box2& box, Visit&& visit ) const;

        std::vector<Node> m_Nodes;
        float             m_MinimumSize = 100.0f;
        size_t            m_Count       = 0;
    };

    /// The box that holds both of an instance's circles (UE GetMaxAABB).
    [[nodiscard]] Box2 MaxAABB( glm::vec2 location, const InstanceRadii& radii );
} // namespace Desert::World::Foliage::Procedural
