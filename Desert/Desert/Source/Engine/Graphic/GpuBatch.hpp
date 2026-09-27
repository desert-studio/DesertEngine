#pragma once

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <vector>

namespace Desert::Graphic
{
    /// ONE GRAPHICS-QUEUE COMMAND BUFFER: recorded now, submitted once, its completion polled — never
    /// waited for on the path it was made for.
    ///
    /// WHY IT EXISTS. An HDR skybox's IBL chain was three convolutions, a mip chain and eight prefilter
    /// levels, each its own one-off buffer that the calling thread WAITED on: 211 ms of the main thread
    /// on a cold open, spent sleeping on fences inside a scene load (AL1-3c). Recording all of it into
    /// one buffer and polling that buffer's fence is what moves the cost to the GPU's own time. Compute
    /// is recorded on the GRAPHICS queue on purpose: the cache readbacks and the frames that later sample
    /// the cubes go to the same queue, so submission order is the whole cross-batch dependency and no
    /// semaphore is needed.
    ///
    /// WHAT OUTLIVES THE CALL. A recorded command references pipelines, descriptor sets and images that
    /// the recording function would otherwise drop on return; `Retain` hands them to the batch, which
    /// lets go of them only once the GPU has finished (or, at teardown, after waiting for it).
    class GpuBatch
    {
    public:
        virtual ~GpuBatch() = default;

        GpuBatch()                             = default;
        GpuBatch( const GpuBatch& )            = delete;
        GpuBatch& operator=( const GpuBatch& ) = delete;

        /// A batch in the recording state. Refused, by name, when the device has no one-off buffer to give.
        static Common::ResultStr<std::unique_ptr<GpuBatch>> Begin();

        /// Keeps @p object alive until the GPU is done with the batch.
        void Retain( std::shared_ptr<const void> object )
        {
            m_Retained.push_back( std::move( object ) );
        }

        /// Ends and submits the buffer WITHOUT waiting. Called once; a second call is refused.
        virtual Common::BoolResultStr Submit() = 0;

        /// True once the submitted buffer has finished on the GPU. Never blocks; false before Submit.
        [[nodiscard]] virtual bool IsComplete() const = 0;

        /// Blocks until the submitted buffer has finished — for a caller whose result must exist when it
        /// returns (the procedural sky, re-baked on a sun rotation inside the frame that asked for it).
        virtual void Wait() = 0;

    protected:
        /// Released by the derived destructor's end, i.e. after it has made sure the GPU is done.
        std::vector<std::shared_ptr<const void>> m_Retained;
    };
} // namespace Desert::Graphic
