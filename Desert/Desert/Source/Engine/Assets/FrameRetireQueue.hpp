#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Desert::Assets
{
    /**
     * @brief GPU OBJECTS THAT NOTHING NAMES ANY MORE, KEPT ALIVE UNTIL NO FRAME IN FLIGHT CAN STILL READ THEM.
     *
     * Eviction used to run only on a scene change, where the device is idled anyway (SceneRenderer::RebindScene),
     * so dropping a built mesh destroyed its vertex and index buffers at once and nothing could be reading them.
     * World streaming asks for a sweep whenever a cell leaves (WP13), and then the frames recorded just before the
     * cell was destroyed are still executing on the GPU: a buffer released at the sweep would be freed under them.
     *
     * The material path answers the same question with a device idle (MaterialService::CollectGarbage). That is
     * affordable once per scene change and not once per cell departure, so an evicted mesh is PARKED instead, with
     * the absolute frame number of the sweep, and released once enough frames have been begun that every frame
     * recorded before the sweep has had its fence waited on.
     *
     * THE MARGIN. A frame's slot is reused, and its fence waited on, `framesInFlight` frames later; the sweep runs
     * at the start of frame F before that frame begins, so the last frame that can have drawn the object is F-1
     * and its fence has been waited on once frame F-1+framesInFlight has begun. Releasing at F+framesInFlight
     * satisfies that; one more frame is added so the rule does not depend on where in the frame the absolute
     * counter is advanced (FrameManager::NextFrame).
     *
     * Header-only and free of Vulkan so the eviction suite can hold the rule against a stand-in object.
     */
    template <typename T>
    class FrameRetireQueue final
    {
    public:
        /// Keep @p item alive; @p frame is the absolute frame number at which it stopped being named.
        void Park( T item, std::uint64_t frame )
        {
            m_Parked.push_back( Parked{ std::move( item ), frame } );
        }

        /// Release every item no frame in flight can still read at absolute frame @p frame. Returns how many.
        std::size_t Collect( std::uint64_t frame, std::uint32_t framesInFlight )
        {
            const auto due = [frame, framesInFlight]( const Parked& parked )
            { return frame >= parked.Frame + framesInFlight + 1; };
            const auto first    = std::remove_if( m_Parked.begin(), m_Parked.end(), due );
            const auto released = static_cast<std::size_t>( std::distance( first, m_Parked.end() ) );
            m_Parked.erase( first, m_Parked.end() );
            return released;
        }

        /// Release everything now. Only for a caller that has idled the device (shutdown, service reset).
        void Clear()
        {
            m_Parked.clear();
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_Parked.size();
        }

    private:
        struct Parked
        {
            T             Item;
            std::uint64_t Frame = 0;
        };
        std::vector<Parked> m_Parked;
    };
} // namespace Desert::Assets
