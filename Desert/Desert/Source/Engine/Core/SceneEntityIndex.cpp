#include <Engine/Core/SceneEntityIndex.hpp>

#include <Engine/ECS/Components.hpp>

#include <algorithm>
#include <unordered_set>

namespace Desert::Core
{
    ECS::Entity& SceneEntityIndex::Add( const Common::UUID& uuid, entt::entity handle, entt::registry& registry )
    {
        m_SlotOf[handle] = m_Entities.size();
        m_ByUuid[uuid]   = handle;
        m_Ids.push_back( uuid );
        return m_Entities.emplace_back( handle, registry );
    }

    bool SceneEntityIndex::Remove( entt::entity handle )
    {
        const auto slotIt = m_SlotOf.find( handle );
        if ( slotIt == m_SlotOf.end() )
            return false;

        const std::size_t  slot = slotIt->second;
        const std::size_t  last = m_Entities.size() - 1;
        const Common::UUID uuid = m_Ids[slot];
        m_SlotOf.erase( slotIt );

        // Only drop the UUID lookup if it still names THIS entity: with a duplicated UUID the lookup belongs
        // to the newer one, and removing the older must not orphan it.
        if ( const auto byId = m_ByUuid.find( uuid ); byId != m_ByUuid.end() && byId->second == handle )
            m_ByUuid.erase( byId );

        if ( slot != last )
        {
            m_Entities[slot]                       = m_Entities[last];
            m_Ids[slot]                            = m_Ids[last];
            m_SlotOf[m_Entities[slot].GetHandle()] = slot;
        }
        m_Entities.pop_back();
        m_Ids.pop_back();
        return true;
    }

    const ECS::Entity* SceneEntityIndex::Find( const Common::UUID& uuid ) const
    {
        const auto byId = m_ByUuid.find( uuid );
        if ( byId == m_ByUuid.end() )
            return nullptr;
        const auto slot = m_SlotOf.find( byId->second );
        return slot == m_SlotOf.end() ? nullptr : &m_Entities[slot->second];
    }

    void SceneEntityIndex::Arrange( const std::vector<entt::entity>& order )
    {
        std::vector<std::size_t>        from;
        std::unordered_set<std::size_t> seen;
        from.reserve( order.size() );
        for ( const entt::entity handle : order )
            if ( const auto slot = m_SlotOf.find( handle );
                 slot != m_SlotOf.end() && seen.insert( slot->second ).second )
                from.push_back( slot->second );

        std::vector<std::size_t> slots = from;
        std::sort( slots.begin(), slots.end() );

        std::vector<ECS::Entity>  entities;
        std::vector<Common::UUID> ids;
        entities.reserve( from.size() );
        ids.reserve( from.size() );
        for ( const std::size_t slot : from )
        {
            entities.push_back( m_Entities[slot] );
            ids.push_back( m_Ids[slot] );
        }
        for ( std::size_t i = 0; i < from.size(); ++i )
        {
            m_Entities[slots[i]]              = entities[i];
            m_Ids[slots[i]]                   = ids[i];
            m_SlotOf[entities[i].GetHandle()] = slots[i];
        }
    }

    void SceneEntityIndex::Clear()
    {
        m_Entities.clear();
        m_Ids.clear();
        m_SlotOf.clear();
        m_ByUuid.clear();
    }

    void DestroyEntityTree( entt::registry& registry, SceneEntityIndex& index, entt::entity root )
    {
        if ( root == entt::null || !registry.valid( root ) )
            return;

        // Unlink the ROOT from its parent — the one parent that survives this call.
        if ( const auto* rel = registry.try_get<ECS::RelationshipComponent>( root );
             rel && rel->Parent != entt::null )
        {
            if ( auto* parentRel = registry.try_get<ECS::RelationshipComponent>( rel->Parent ) )
            {
                auto& siblings = parentRel->Children;
                if ( const auto it = std::find( siblings.begin(), siblings.end(), root ); it != siblings.end() )
                    siblings.erase( it );
            }
        }

        // Collect the subtree first, then destroy: destroying while walking would read the Children of an
        // entity whose storage the registry has already recycled. An explicit stack rather than recursion,
        // because a streamed cell can be a deep chain and the recursion this replaced had no bound.
        std::vector<entt::entity> doomed;
        std::vector<entt::entity> stack{ root };
        while ( !stack.empty() )
        {
            const entt::entity e = stack.back();
            stack.pop_back();
            if ( !registry.valid( e ) )
                continue;
            doomed.push_back( e );
            if ( const auto* rel = registry.try_get<ECS::RelationshipComponent>( e ) )
                stack.insert( stack.end(), rel->Children.begin(), rel->Children.end() );
        }

        // Children before parents, as the recursive version did, so an on_destroy listener that looks at
        // the parent of a dying entity still finds it alive.
        for ( auto it = doomed.rbegin(); it != doomed.rend(); ++it )
        {
            index.Remove( *it );
            registry.destroy( *it );
        }
    }
} // namespace Desert::Core
