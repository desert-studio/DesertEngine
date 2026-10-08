#pragma once

#include <Engine/ECS/Components.hpp>

#include <entt/entt.hpp>

#include <cstddef>
#include <unordered_map>

namespace Desert::Graphic::System
{
    // Drops every cached emitter whose entity no longer carries an emitter in @p registry — destroyed (a
    // recycled id comes back with a new version, so the raw value never matches again) or stripped of its
    // ParticleEmitterComponent. A disabled emitter keeps its state: it is still the scene's emitter.
    //
    // Erasing an entry is safe while frames are in flight: the entry's storage buffers release their copies
    // through MappedBufferCopy -> VulkanAllocator::RT_DestroyBuffer, and its billboard material releases its
    // descriptor sets and pools through RT_FreeDescriptorSets / RT_DestroyDescriptorPool and its dummy buffer
    // through RT_DestroyBuffer — the allocator's deletion ring, which destroys them only once the frame that
    // last used them has finished. Answers how many entries were dropped.
    //
    // Kept device-free (any map keyed by the entity's integer value) so the EntityDestroy suite drives the
    // same function ParticleRenderer::PrepareFrame calls.
    template <class Value>
    std::size_t RetireDestroyedEmitters( std::unordered_map<uint32_t, Value>& emitters,
                                         const entt::registry&                registry )
    {
        return std::erase_if( emitters,
                              [&registry]( const auto& entry )
                              {
                                  const auto entity = static_cast<entt::entity>( entry.first );
                                  return !registry.valid( entity ) ||
                                         !registry.all_of<ECS::ParticleEmitterComponent>( entity );
                              } );
    }
} // namespace Desert::Graphic::System
