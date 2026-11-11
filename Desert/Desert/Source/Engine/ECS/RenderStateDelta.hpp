#pragma once

#include <Common/Core/DirtySet.hpp>

#include <entt/entt.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Desert::ECS
{
    // An entity's dense index (its version dropped) for Common::DirtySet: a recycled index re-marked with its new
    // version is listed beside the old one, so a destroy + create within a frame reaches the consumer as both.
    struct EntityIndexOf
    {
        [[nodiscard]] uint32_t operator()( const entt::entity entity ) const noexcept
        {
            return static_cast<uint32_t>( entt::to_integral( entt::registry::entity( entity ) ) );
        }
    };

    using EntityDirtySet = Common::DirtySet<entt::entity, EntityIndexOf>;

    // The retained render caches fed by the delta, one CHANNEL each: every channel receives every mark and is
    // drained by its own consumer, so one consumer's drain never hides a mark from another.
    enum class RenderStateConsumer : uint8_t
    {
        StaticScene, // MeshECSSystem::RecordStaticScene
        Ism,         // MeshECSSystem::RecordInstancedScene (the retained ISM records)
        Skinned,     // MeshECSSystem::InvalidateSkinnedAdoption (the skinned slots' adoption keys)
        Count
    };

    // THE RENDER-STATE DELTA (UE: the primitives UPrimitiveComponent::MarkRenderStateDirty queued for the next
    // SendAllEndOfFrameUpdates). The Scene's render-state dirty set (Scene::MarkRenderStateDirty) is latched and
    // emptied at the top of every frame for the control state; this is the SAME feed kept for the retained render
    // caches: every entity the set receives is appended to every channel, and each consumer drains its channel at
    // its update. Marks made after a drain (a script past the collectors, an editor edit between frames) are
    // drained by the next update, so nothing is lost across the Scene's frame boundary.
    //
    // ONE CONSUMER PER CHANNEL, BOUNDED: a frame whose boundary finds a channel not drained since the previous
    // boundary (a scene whose systems did not run, a consumer that skipped its update) drops that channel's marks
    // and asks it for a full rebuild instead, so no channel holds more than one frame of marks and a consumer
    // that missed marks never trusts its cache.
    class RenderStateDelta
    {
    public:
        static constexpr size_t kChannels = static_cast<size_t>( RenderStateConsumer::Count );

        // A mark of an entity already queued in a channel since its last drain is absorbed (an entity whose
        // transform a script, the physics write-back and the animation all set in one frame is rebuilt once).
        void Mark( const entt::entity entity ) noexcept
        {
            std::lock_guard lock( m_Mutex );
            for ( Channel& channel : m_Channels )
                (void)channel.Pending.Mark( entity );
        }

        // Everything is to be rebuilt (a scene reload, a cleared registry): the queued marks are moot.
        void RequestFullRebuild() noexcept
        {
            std::lock_guard lock( m_Mutex );
            for ( Channel& channel : m_Channels )
            {
                channel.Pending.Clear();
                channel.Full = true;
            }
        }

        // The Scene's frame boundary (Scene::OnUpdate, where the dirty set is latched). See the class comment.
        void FrameBoundary() noexcept
        {
            std::lock_guard lock( m_Mutex );
            for ( Channel& channel : m_Channels )
            {
                if ( !channel.DrainedSinceBoundary )
                {
                    channel.Pending.Clear();
                    channel.Full = true;
                }
                channel.DrainedSinceBoundary = false;
            }
        }

        // @p consumer's half: @p out receives its channel's queued marks, each entity once (an index recycled
        // within the frame: both versions), in mark order; both buffers keep their capacity, so a steady state
        // allocates nothing. Returns whether a full rebuild was requested for the channel since its last drain.
        [[nodiscard]] bool Drain( const RenderStateConsumer consumer, std::vector<entt::entity>& out ) noexcept
        {
            std::lock_guard lock( m_Mutex );
            Channel&        channel = m_Channels[static_cast<size_t>( consumer )];
            out.clear();
            channel.Pending.DrainInto( out );
            const bool full              = channel.Full;
            channel.Full                 = false;
            channel.DrainedSinceBoundary = true;
            return full;
        }

    private:
        struct Channel
        {
            EntityDirtySet Pending;
            bool           Full                 = true; // a new consumer starts from the whole scene
            bool           DrainedSinceBoundary = true;
        };

        std::mutex                      m_Mutex;
        std::array<Channel, kChannels> m_Channels;
    };

    // The registry context's link to its Scene's RenderStateDelta (registry.ctx: a pointer, so the context holds
    // a copyable value; the Scene owns the queue and outlives its registry's systems).
    struct RenderStateDeltaLink
    {
        RenderStateDelta* Delta = nullptr;
    };
} // namespace Desert::ECS
