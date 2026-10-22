#pragma once

#include <atomic>

namespace Desert::Geometry
{
    // UE FProgressCancel (GeometryCore/Public/Util/ProgressCancel.h): the cancel token a long geometry operation
    // polls between its units of work. The owner (a tool's background compute) raises it from the main thread
    // when the result is no longer wanted; the operation sees it on the worker and returns early with a refusal,
    // so a superseded compute stops spending the worker instead of finishing a mesh nobody shows.
    class ProgressCancel
    {
    public:
        void Cancel() { m_Requested.store( true, std::memory_order_relaxed ); }

        [[nodiscard]] bool Cancelled() const { return m_Requested.load( std::memory_order_relaxed ); }

    private:
        std::atomic<bool> m_Requested{ false };
    };
} // namespace Desert::Geometry
