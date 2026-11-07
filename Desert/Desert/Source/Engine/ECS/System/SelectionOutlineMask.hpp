#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/SelectionContext.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace Desert::ECS
{
    // The editor selection as a per-entity FLAG the mesh collectors read in O(1) (UE: the selection is pushed to
    // the primitive once, FPrimitiveSceneProxy::SetSelection_GameThread, never re-derived per draw). Before, every
    // static mesh scanned the selected UUID list for itself and for every ancestor, every frame:
    // O(meshes x depth x selected).
    //
    // Refresh() runs once per frame on the collector's own thread and only READS the registry (the collector runs
    // in parallel with the other systems, so it may not stamp a component). It resolves the selected UUIDs to
    // entities - by one UUID scan only when the selection revision or the UUID population changed, or a resolved
    // entity stopped matching - then flags each selected entity and its whole child subtree, so a selected prefab
    // root outlines all its mesh children. Cost: O(selected subtree) per frame, nothing with an empty selection.
    class SelectionOutlineMask final
    {
    public:
        void Refresh( const entt::registry& registry )
        {
            for ( const uint32_t word : m_SetWords )
                m_Bits[word] = 0;
            m_SetWords.clear();

            const auto& selected = Runtime::SelectionContext::GetAll();
            if ( selected.empty() )
            {
                m_Resolved.clear();
                m_ResolvedRevision = ~0ull;
                return;
            }

            auto         uuids     = registry.view<const UUIDComponent>();
            const size_t uuidCount = uuids.size();
            bool         stale     = m_ResolvedRevision != Runtime::SelectionContext::GetRevision() ||
                         m_ResolvedUuidCount != uuidCount || m_Resolved.size() != selected.size();
            for ( size_t i = 0; !stale && i < m_Resolved.size(); ++i )
            {
                const entt::entity e = m_Resolved[i];
                if ( e == entt::null )
                    continue;
                stale = !registry.valid( e ) || !registry.has<UUIDComponent>( e ) ||
                        !( registry.get<UUIDComponent>( e ).UUID == selected[i] );
            }
            if ( stale )
            {
                m_Resolved.assign( selected.size(), entt::null );
                for ( const entt::entity e : uuids )
                {
                    const auto& id = uuids.get<const UUIDComponent>( e ).UUID;
                    for ( size_t i = 0; i < selected.size(); ++i )
                        if ( selected[i] == id )
                            m_Resolved[i] = e;
                }
                m_ResolvedRevision  = Runtime::SelectionContext::GetRevision();
                m_ResolvedUuidCount = uuidCount;
            }

            for ( const entt::entity root : m_Resolved )
            {
                if ( root == entt::null )
                    continue;
                m_Stack.clear();
                m_Stack.push_back( root );
                while ( !m_Stack.empty() )
                {
                    const entt::entity e = m_Stack.back();
                    m_Stack.pop_back();
                    if ( !registry.valid( e ) || Contains( e ) )
                        continue; // a shared subtree (a selected child of a selected root) is walked once
                    Set( e );
                    if ( registry.has<RelationshipComponent>( e ) )
                        for ( const entt::entity child : registry.get<RelationshipComponent>( e ).Children )
                            m_Stack.push_back( child );
                }
            }
        }

        [[nodiscard]] bool Contains( entt::entity e ) const noexcept
        {
            if ( e == entt::null )
                return false;
            const auto   index = static_cast<uint32_t>( entt::to_integral( entt::registry::entity( e ) ) );
            const size_t word  = index >> 6u;
            return word < m_Bits.size() && ( m_Bits[word] >> ( index & 63u ) & 1ull ) != 0;
        }

    private:
        void Set( entt::entity e )
        {
            const auto   index = static_cast<uint32_t>( entt::to_integral( entt::registry::entity( e ) ) );
            const size_t word  = index >> 6u;
            if ( word >= m_Bits.size() )
                m_Bits.resize( word + 1, 0 );
            if ( m_Bits[word] == 0 )
                m_SetWords.push_back( static_cast<uint32_t>( word ) );
            m_Bits[word] |= 1ull << ( index & 63u );
        }

        std::vector<uint64_t>     m_Bits;     // one bit per entity index; only m_SetWords' words are non-zero
        std::vector<uint32_t>     m_SetWords; // the words written this frame, cleared next Refresh
        std::vector<entt::entity> m_Stack;    // subtree walk scratch, kept across frames
        std::vector<entt::entity> m_Resolved; // GetAll()[i] -> its entity (null: not in this registry)
        uint64_t                  m_ResolvedRevision  = ~0ull;
        size_t                    m_ResolvedUuidCount = 0;
    };
} // namespace Desert::ECS
