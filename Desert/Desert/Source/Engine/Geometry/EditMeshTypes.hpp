#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    // The vocabulary EditMesh (topology) and EditMeshAttributes (overlays, polygroups, material IDs) share.
    // It lives apart from both because the attribute layer's element IDs follow exactly the same rules as
    // the mesh's vertex/triangle/edge IDs - stable, holes left by removal, reused by later appends - and
    // two copies of that pool would be two places for the rule to drift.

    inline constexpr int InvalidId = -1;

    enum class EditResult : uint8_t
    {
        Ok,
        InvalidVertex,           // an argument names a vertex that does not exist
        InvalidTriangle,         // ... a triangle that does not exist
        InvalidEdge,             // ... an edge that does not exist (or the two vertices share none)
        InvalidElement,          // ... an overlay element that does not exist
        DegenerateTriangle,      // two corners of a new triangle are the same vertex
        DuplicateTriangle,       // a triangle over the same three vertices already exists
        NonManifoldEdge,         // the edit would put a third triangle on an edge
        InconsistentOrientation, // the edit would put two triangles on an edge in the SAME direction
        BoundaryEdge,            // FlipEdge on an edge with one triangle: there is nothing to flip towards
        FlipCreatesExistingEdge, // FlipEdge: the other diagonal already exists, the flip would duplicate it
        CollapseBreaksTopology,  // CollapseEdge: link condition / pinch / ear / tetrahedron - see CollapseEdge
        AttributeSeam,           // the edit would mix the two sides of a seam - see FlipEdge / CollapseEdge
        ElementOnOtherVertex,    // an overlay element is already bound to another vertex than this corner
    };

    [[nodiscard]] const char* ToString( EditResult result );

    // Iterates the LIVE IDs of one element kind, skipping holes. Invalidated by any edit.
    class IdRange
    {
    public:
        class Iterator
        {
        public:
            Iterator( std::span<const uint8_t> alive, int id ) : m_Alive( alive ), m_Id( id )
            {
                SkipDead();
            }
            int operator*() const
            {
                return m_Id;
            }
            Iterator& operator++()
            {
                ++m_Id;
                SkipDead();
                return *this;
            }
            bool operator==( const Iterator& other ) const
            {
                return m_Id == other.m_Id;
            }
            bool operator!=( const Iterator& other ) const
            {
                return m_Id != other.m_Id;
            }

        private:
            void SkipDead()
            {
                while ( m_Id < static_cast<int>( m_Alive.size() ) && m_Alive[m_Id] == 0 )
                    ++m_Id;
            }
            std::span<const uint8_t> m_Alive;
            int                      m_Id;
        };

        explicit IdRange( std::span<const uint8_t> alive ) : m_Alive( alive )
        {
        }
        [[nodiscard]] Iterator begin() const
        {
            return { m_Alive, 0 };
        }
        [[nodiscard]] Iterator end() const
        {
            return { m_Alive, static_cast<int>( m_Alive.size() ) };
        }

    private:
        std::span<const uint8_t> m_Alive;
    };

    // Stable IDs with holes: a freed ID is remembered and handed out again by a later Allocate, nothing
    // is ever renumbered except by an explicit compaction (which builds the old -> new map itself).
    struct IdPool
    {
        std::vector<uint8_t> Alive;
        std::vector<int>     Free;
        int                  Live = 0;

        // Returns the new ID; when it equals the old size() the caller must grow its payload arrays.
        int  Allocate();
        void Release( int id );

        [[nodiscard]] bool Contains( int id ) const
        {
            return id >= 0 && id < static_cast<int>( Alive.size() ) && Alive[id] != 0;
        }
        [[nodiscard]] int Size() const
        {
            return static_cast<int>( Alive.size() );
        }
        [[nodiscard]] IdRange Ids() const
        {
            return IdRange( Alive );
        }

        // old -> new, holes -> InvalidId, ascending order kept; then the pool is dense with that many IDs.
        std::vector<int> Compact();

        // Live count, free list and payload size agree with the alive flags.
        [[nodiscard]] Common::BoolResultStr Check( const char* kind, size_t payloadSize ) const;
    };

    // Old ID -> new ID for every element kind Compact() renumbers; a removed/hole ID maps to InvalidId.
    // Overlay maps are empty when that overlay is disabled.
    struct CompactMaps
    {
        std::vector<int>              Vertices;
        std::vector<int>              Triangles;
        std::vector<int>              Edges;
        std::vector<int>              NormalElements;
        std::vector<int>              TangentElements;
        std::vector<int>              ColorElements;
        std::vector<std::vector<int>> UVElements; // one map per UV layer
    };
} // namespace Desert::Geometry
