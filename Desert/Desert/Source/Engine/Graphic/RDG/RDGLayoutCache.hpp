#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace Desert::Graphic::RDG
{
    // RDG-FAULT1. A binding layout kept for one source (a shader object) and re-derived only when that source
    // changes: either it is ANOTHER object (a renderer swapped its shader) or the same object recompiled (its
    // reload generation moved). The source's identity is its ownership (weak_ptr owner equality, not the address):
    // the control block outlives the object while this cache observes it, so a new object allocated where a
    // destroyed one lived is never mistaken for it. The layout is handed out as a shared const value, so a block
    // declared this frame keeps the layout it validated against even if a later Get re-derives it, and a node's
    // per-frame setup copies a pointer, not the slot list. Pure (no device): the derivation is the caller's.
    class LayoutCache
    {
    public:
        template <class Source, class Derive>
        [[nodiscard]] const std::shared_ptr<const ShaderBindingLayout>& Get( const std::shared_ptr<Source>& source,
                                                                             uint64_t generation, Derive&& derive )
        {
            const std::weak_ptr<const void> observed = source;
            const bool                      sameSource =
                 !m_Source.expired() && !m_Source.owner_before( observed ) && !observed.owner_before( m_Source );
            if ( !m_Layout || !sameSource || m_Generation != generation )
            {
                m_Layout =
                     std::make_shared<const ShaderBindingLayout>( std::forward<Derive>( derive )( *source ) );
                m_Source     = observed;
                m_Generation = generation;
            }
            return m_Layout;
        }

    private:
        std::shared_ptr<const ShaderBindingLayout> m_Layout;
        std::weak_ptr<const void>                  m_Source;     // the object m_Layout was derived from
        std::optional<uint64_t>                    m_Generation; // its reload generation at that time
    };

    // One LayoutCache PER SOURCE OBJECT, for a node that declares one block per shader its draws record with (a
    // mesh draw list: one block per material executor and recording pipeline). Keyed on the source's ownership
    // (owner order of the weak_ptr, like LayoutCache's identity), so a new object allocated where a destroyed one
    // lived gets its own cache, never the dead one's layout. DropExpired forgets the caches of destroyed sources
    // (the owner of the set calls it once per frame, before the frame's Gets).
    class LayoutCacheSet
    {
    public:
        template <class Source, class Derive>
        [[nodiscard]] const std::shared_ptr<const ShaderBindingLayout>& Get( const std::shared_ptr<Source>& source,
                                                                             uint64_t generation, Derive&& derive )
        {
            return m_Caches[std::weak_ptr<const void>( source )].Get( source, generation,
                                                                      std::forward<Derive>( derive ) );
        }

        void DropExpired()
        {
            std::erase_if( m_Caches, []( const auto& cached ) { return cached.first.expired(); } );
        }

        [[nodiscard]] std::size_t Size() const
        {
            return m_Caches.size();
        }

    private:
        std::map<std::weak_ptr<const void>, LayoutCache, std::owner_less<>> m_Caches;
    };
} // namespace Desert::Graphic::RDG
