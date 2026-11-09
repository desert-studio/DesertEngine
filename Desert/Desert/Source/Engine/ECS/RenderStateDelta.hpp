#pragma once

#include <Common/Core/DirtySet.hpp>

#include <entt/entt.hpp>

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

    // THE RENDER-STATE DELTA (UE: the primitives UPrimitiveComponent::MarkRenderStateDirty queued for the next
    // SendAllEndOfFrameUpdates). The Scene's render-state dirty set (Scene::MarkRenderStateDirty) is latched and
    // emptied at the top of every frame for the control state; this is the SAME feed kept for a retained render
    // cache: every entity the set receives is appended here, and the one consumer (MeshECSSystem's retained static
    // scene) drains it at its update. Marks made after the drain (a script past the collectors, an editor edit
    // between frames) are drained by the next update, so nothing is lost across the Scene's frame boundary.
    //
    // ONE CONSUMER, BOUNDED: a frame whose boundary finds the queue not drained since the previous boundary (a
    // scene whose systems did not run) drops the marks and asks for a full rebuild instead, so the queue never
    // holds more than one frame of marks and a consumer that missed marks never trusts its cache.
    class RenderStateDelta
    {
    public:
        // A mark of an entity already queued since the last drain is absorbed (an entity whose transform a
        // script, the physics write-back and the animation all set in one frame is rebuilt once).
        void Mark( const entt::entity entity ) noexcept
        {
            std::lock_guard lock( m_Mutex );
            (void)m_Pending.Mark( entity );
        }

        // Everything is to be rebuilt (a scene reload, a cleared registry): the queued marks are moot.
        void RequestFullRebuild() noexcept
        {
            std::lock_guard lock( m_Mutex );
            m_Pending.Clear();
            m_Full = true;
        }

        // The Scene's frame boundary (Scene::OnUpdate, where the dirty set is latched). See the class comment.
        void FrameBoundary() noexcept
        {
            std::lock_guard lock( m_Mutex );
            if ( !m_DrainedSinceBoundary )
            {
                m_Pending.Clear();
                m_Full = true;
            }
            m_DrainedSinceBoundary = false;
        }

        // The consumer's half: @p out receives the queued marks, each entity once (an index recycled within the
        // frame: both versions), in mark order; both buffers keep their capacity, so a steady state allocates
        // nothing. Returns whether a full rebuild was requested since the last drain.
        [[nodiscard]] bool Drain( std::vector<entt::entity>& out ) noexcept
        {
            std::lock_guard lock( m_Mutex );
            out.clear();
            m_Pending.DrainInto( out );
            const bool full        = m_Full;
            m_Full                 = false;
            m_DrainedSinceBoundary = true;
            return full;
        }

    private:
        std::mutex                m_Mutex;
        EntityDirtySet            m_Pending;
        bool                      m_Full                 = true; // a new consumer starts from the whole scene
        bool                      m_DrainedSinceBoundary = true;
    };

    // The registry context's link to its Scene's RenderStateDelta (registry.ctx: a pointer, so the context holds
    // a copyable value; the Scene owns the queue and outlives its registry's systems).
    struct RenderStateDeltaLink
    {
        RenderStateDelta* Delta = nullptr;
    };
} // namespace Desert::ECS
