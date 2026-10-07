#include <Engine/World/Foliage/Procedural/ProceduralFoliageBroadphase.hpp>

#include <algorithm>

namespace Desert::World::Foliage::Procedural
{
    namespace
    {
        // A node splits once it holds more than this many entries and is still larger than the minimum size.
        constexpr size_t kNodeCapacity = 4;

        bool CirclesTouch( glm::vec2 a, float aRadius, glm::vec2 b, float bRadius )
        {
            const glm::vec2 d   = a - b;
            const float     sum = aRadius + bRadius;
            return d.x * d.x + d.y * d.y <= sum * sum;
        }
    } // namespace

    Box2 MaxAABB( glm::vec2 location, const InstanceRadii& radii )
    {
        const glm::vec2 offset( radii.Max() );
        return Box2{ location - offset, location + offset };
    }

    ProceduralFoliageBroadphase::ProceduralFoliageBroadphase( float tileSize, float minimumQuadTreeSize )
         : m_MinimumSize( minimumQuadTreeSize )
    {
        m_Nodes.push_back( Node{ Box2{ glm::vec2( -tileSize * 2.0f ), glm::vec2( tileSize * 2.0f ) }, -1, {} } );
    }

    void ProceduralFoliageBroadphase::Empty()
    {
        if ( m_Nodes.empty() )
            return;
        const Box2 root = m_Nodes.front().Bounds;
        m_Nodes.clear();
        m_Nodes.push_back( Node{ root, -1, {} } );
        m_Count = 0;
    }

    bool ProceduralFoliageBroadphase::TestAgainstAABB( glm::vec2 location, const InstanceRadii& radii ) const
    {
        return !m_Nodes.empty() && MaxAABB( location, radii ).Intersects( m_Nodes.front().Bounds );
    }

    int32_t ProceduralFoliageBroadphase::ChildHolding( const Node& node, const Box2& box ) const
    {
        if ( node.FirstChild < 0 )
            return -1;
        for ( int32_t i = 0; i < 4; ++i )
            if ( m_Nodes[node.FirstChild + i].Bounds.Contains( box ) )
                return node.FirstChild + i;
        return -1;
    }

    void ProceduralFoliageBroadphase::Split( int32_t nodeIndex )
    {
        const Box2      bounds = m_Nodes[nodeIndex].Bounds;
        const glm::vec2 mid    = ( bounds.Min + bounds.Max ) * 0.5f;
        const int32_t   first  = static_cast<int32_t>( m_Nodes.size() );
        m_Nodes.push_back( Node{ Box2{ bounds.Min, mid }, -1, {} } );
        m_Nodes.push_back( Node{ Box2{ { mid.x, bounds.Min.y }, { bounds.Max.x, mid.y } }, -1, {} } );
        m_Nodes.push_back( Node{ Box2{ { bounds.Min.x, mid.y }, { mid.x, bounds.Max.y } }, -1, {} } );
        m_Nodes.push_back( Node{ Box2{ mid, bounds.Max }, -1, {} } );
        m_Nodes[nodeIndex].FirstChild = first;

        // Entries that fit a child whole move down, in their order; the rest stay.
        std::vector<Entry> kept;
        for ( Entry& entry : m_Nodes[nodeIndex].Entries )
        {
            const int32_t child = ChildHolding( m_Nodes[nodeIndex], entry.Box );
            if ( child >= 0 )
                m_Nodes[child].Entries.push_back( std::move( entry ) );
            else
                kept.push_back( std::move( entry ) );
        }
        m_Nodes[nodeIndex].Entries = std::move( kept );
    }

    void ProceduralFoliageBroadphase::Insert( uint32_t id, glm::vec2 location, const InstanceRadii& radii )
    {
        if ( m_Nodes.empty() )
            return;
        const Box2 box  = MaxAABB( location, radii );
        int32_t    node = 0;
        for ( ;; )
        {
            const int32_t child = ChildHolding( m_Nodes[node], box );
            if ( child < 0 )
                break;
            node = child;
        }
        m_Nodes[node].Entries.push_back( Entry{ id, location, radii, box } );
        ++m_Count;

        const Box2& bounds = m_Nodes[node].Bounds;
        const float size   = std::max( bounds.Max.x - bounds.Min.x, bounds.Max.y - bounds.Min.y );
        if ( m_Nodes[node].FirstChild < 0 && m_Nodes[node].Entries.size() > kNodeCapacity &&
             size * 0.5f >= m_MinimumSize )
            Split( node );
    }

    bool ProceduralFoliageBroadphase::Remove( uint32_t id, glm::vec2 location, const InstanceRadii& radii )
    {
        if ( m_Nodes.empty() )
            return false;
        const Box2 box  = MaxAABB( location, radii );
        int32_t    node = 0;
        while ( node >= 0 )
        {
            auto&      entries = m_Nodes[node].Entries;
            const auto it =
                 std::find_if( entries.begin(), entries.end(), [id]( const Entry& e ) { return e.Id == id; } );
            if ( it != entries.end() )
            {
                entries.erase( it );
                --m_Count;
                return true;
            }
            node = ChildHolding( m_Nodes[node], box );
        }
        return false;
    }

    template <typename Visit>
    void ProceduralFoliageBroadphase::Query( const Box2& box, Visit&& visit ) const
    {
        if ( m_Nodes.empty() )
            return;
        std::vector<int32_t> stack{ 0 };
        while ( !stack.empty() )
        {
            const Node& node = m_Nodes[stack.back()];
            stack.pop_back();
            if ( !node.Bounds.Intersects( box ) )
                continue;
            for ( const Entry& entry : node.Entries )
                if ( entry.Box.Intersects( box ) )
                    visit( entry );
            if ( node.FirstChild >= 0 )
                for ( int32_t i = 3; i >= 0; --i )
                    stack.push_back( node.FirstChild + i );
        }
    }

    void ProceduralFoliageBroadphase::GetOverlaps( uint32_t id, glm::vec2 location, const InstanceRadii& radii,
                                                   std::vector<BroadphaseOverlap>& out ) const
    {
        Query( MaxAABB( location, radii ),
               [&]( const Entry& other )
               {
                   if ( other.Id == id )
                       return;
                   // Collision wins when both circles touch.
                   const bool collision =
                        CirclesTouch( location, radii.Collision, other.Location, other.Radii.Collision );
                   const bool shade = CirclesTouch( location, radii.Shade, other.Location, other.Radii.Shade );
                   if ( collision || shade )
                       out.push_back( BroadphaseOverlap{ other.Id, collision ? OverlapKind::Collision
                                                                             : OverlapKind::Shade } );
               } );
    }

    void ProceduralFoliageBroadphase::GetInstancesInBox( const Box2& box, std::vector<uint32_t>& out ) const
    {
        Query( box, [&]( const Entry& entry ) { out.push_back( entry.Id ); } );
    }
} // namespace Desert::World::Foliage::Procedural
