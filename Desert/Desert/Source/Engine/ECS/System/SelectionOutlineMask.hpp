#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/HierarchyRevision.hpp>
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
    // root outlines all its mesh children. The walk runs only when its inputs moved - the resolved roots or the
    // Scene's hierarchy stamp (ECS::HierarchyRevision: attach, detach, an entity created or destroyed) - so a
    // steady frame costs O(selected) compares and no walk, whatever the size of the selected subtree.
    //
    // THE DELTA (UE: SetSelection_GameThread reaches only the proxies whose selection changed): Changed() lists
    // the entities whose flag differs from the previous Refresh - the old flagged set minus the new one, plus
    // the new minus the old - whatever moved it (a selection write, a subtree attached under a selected root, a
    // selected UUID resolving in a later frame). The retained static scene rebuilds only those
    // (MeshECSSystem::RecordStaticScene), never every slot on an editor click.
    class SelectionOutlineMask final
    {
    public:
        void Refresh( const entt::registry& registry )
        {
            // THE INPUTS FIRST: the selected UUIDs resolved to entities (one UUID scan only when the selection
            // revision or the UUID population moved, or a resolved entity stopped matching).
            const auto& selected = Runtime::SelectionContext::GetAll();
            bool        resolved = false; // m_Resolved was written this Refresh
            if ( selected.empty() )
            {
                resolved           = !m_Resolved.empty() || m_ResolvedRevision != ~0ull;
                m_Resolved.clear();
                m_ResolvedRevision = ~0ull;
            }
            else
            {
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
                    resolved            = true;
                }
            }

            // THE EVENT GATE (UE: a proxy's selection is pushed on a selection or attachment change, never
            // re-derived per frame): the flags are a function of the resolved roots and the hierarchy alone, so
            // when neither moved since the last walk the previous flags ARE this frame's and nothing changed.
            // A registry no Scene stamps (ReadHierarchyRevision: false) is walked every Refresh.
            uint64_t   hierarchy = 0;
            const bool stamped   = ReadHierarchyRevision( registry, hierarchy );
            if ( !resolved && stamped && m_Walked && hierarchy == m_WalkedHierarchy )
            {
                m_Changed.clear();
                return;
            }
            m_Walked          = stamped;
            m_WalkedHierarchy = hierarchy;

            // The previous walk's flags move aside for the diff; the one before it is cleared for reuse.
            m_Bits.swap( m_PrevBits );
            m_SetWords.swap( m_PrevSetWords );
            m_Flagged.swap( m_PrevFlagged );
            for ( const uint32_t word : m_SetWords )
                m_Bits[word] = 0;
            m_SetWords.clear();
            m_Flagged.clear();
            for ( const uint32_t word : m_RootWords )
                m_RootBits[word] = 0;
            m_RootWords.clear();

            for ( const entt::entity root : m_Resolved )
            {
                if ( root == entt::null || !registry.valid( root ) )
                    continue;
                SetBit( m_RootBits, m_RootWords, root );
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
            CollectChanged();
        }

        // The entities whose flag this Refresh changed (each once; a destroyed entity may be among the old ones).
        [[nodiscard]] const std::vector<entt::entity>& Changed() const noexcept
        {
            return m_Changed;
        }

        [[nodiscard]] bool Contains( entt::entity e ) const noexcept
        {
            if ( e == entt::null )
                return false;
            const auto   index = static_cast<uint32_t>( entt::to_integral( entt::registry::entity( e ) ) );
            const size_t word  = index >> 6u;
            return word < m_Bits.size() && ( m_Bits[word] >> ( index & 63u ) & 1ull ) != 0;
        }

        // Is @p e itself one of the selected entities (not only under one)? The skinned collector outlines the
        // selected entity alone; the flag is refreshed with the subtree's, so a draw reads no UUID.
        [[nodiscard]] bool IsSelectedRoot( entt::entity e ) const noexcept
        {
            return e != entt::null && Has( m_RootBits, e );
        }

    private:
        [[nodiscard]] static bool Has( const std::vector<uint64_t>& bits, const entt::entity e ) noexcept
        {
            const auto   index = static_cast<uint32_t>( entt::to_integral( entt::registry::entity( e ) ) );
            const size_t word  = index >> 6u;
            return word < bits.size() && ( bits[word] >> ( index & 63u ) & 1ull ) != 0;
        }

        void CollectChanged()
        {
            m_Changed.clear();
            for ( const entt::entity e : m_PrevFlagged )
                if ( !Contains( e ) )
                    m_Changed.push_back( e );
            for ( const entt::entity e : m_Flagged )
                if ( !Has( m_PrevBits, e ) )
                    m_Changed.push_back( e );
        }

        static void SetBit( std::vector<uint64_t>& bits, std::vector<uint32_t>& words, const entt::entity e )
        {
            const auto   index = static_cast<uint32_t>( entt::to_integral( entt::registry::entity( e ) ) );
            const size_t word  = index >> 6u;
            if ( word >= bits.size() )
                bits.resize( word + 1, 0 );
            if ( bits[word] == 0 )
                words.push_back( static_cast<uint32_t>( word ) );
            bits[word] |= 1ull << ( index & 63u );
        }

        void Set( entt::entity e )
        {
            SetBit( m_Bits, m_SetWords, e );
            m_Flagged.push_back( e );
        }

        std::vector<uint64_t>     m_Bits;     // one bit per entity index; only m_SetWords' words are non-zero
        std::vector<uint32_t>     m_SetWords; // the words written this frame, cleared next Refresh
        std::vector<entt::entity> m_Flagged;  // the entities flagged this frame (Set order)
        std::vector<uint64_t>     m_PrevBits; // the previous Refresh's flags (only m_PrevSetWords non-zero)
        std::vector<uint32_t>     m_PrevSetWords;
        std::vector<entt::entity> m_PrevFlagged;
        std::vector<entt::entity> m_Changed; // Changed()
        std::vector<entt::entity> m_Stack;    // subtree walk scratch, kept across frames
        std::vector<entt::entity> m_Resolved; // GetAll()[i] -> its entity (null: not in this registry)
        uint64_t                  m_ResolvedRevision  = ~0ull;
        size_t                    m_ResolvedUuidCount = 0;
        std::vector<uint64_t>     m_RootBits;               // the resolved roots themselves (IsSelectedRoot)
        std::vector<uint32_t>     m_RootWords;              // m_RootBits' non-zero words
        uint64_t                  m_WalkedHierarchy = 0;     // the hierarchy stamp the flags were walked at
        bool                      m_Walked          = false; // the flags were walked against a stamp
    };
} // namespace Desert::ECS
