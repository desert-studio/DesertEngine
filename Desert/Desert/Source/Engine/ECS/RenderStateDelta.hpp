#pragma once

#include <entt/entt.hpp>

#include <mutex>
#include <vector>

namespace Desert::ECS
{
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
        void Mark( const entt::entity entity ) noexcept
        {
            std::lock_guard lock( m_Mutex );
            m_Pending.push_back( entity );
        }

        // Everything is to be rebuilt (a scene reload, a cleared registry): the queued marks are moot.
        void RequestFullRebuild() noexcept
        {
            std::lock_guard lock( m_Mutex );
            m_Pending.clear();
            m_Full = true;
        }

        // The Scene's frame boundary (Scene::OnUpdate, where the dirty set is latched). See the class comment.
        void FrameBoundary() noexcept
        {
            std::lock_guard lock( m_Mutex );
            if ( !m_DrainedSinceBoundary )
            {
                m_Pending.clear();
                m_Full = true;
            }
            m_DrainedSinceBoundary = false;
        }

        // The consumer's half: @p out receives the queued marks (duplicates possible; the consumer dedupes) and
        // hands its buffer back for the next frame (capacities kept, no allocation in a steady state). Returns
        // whether a full rebuild was requested since the last drain.
        [[nodiscard]] bool Drain( std::vector<entt::entity>& out ) noexcept
        {
            std::lock_guard lock( m_Mutex );
            out.clear();
            out.swap( m_Pending );
            const bool full        = m_Full;
            m_Full                 = false;
            m_DrainedSinceBoundary = true;
            return full;
        }

    private:
        std::mutex                m_Mutex;
        std::vector<entt::entity> m_Pending;
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
