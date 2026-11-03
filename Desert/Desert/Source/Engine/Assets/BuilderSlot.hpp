#pragma once

#include <mutex>
#include <utility>

namespace Desert::Assets
{
    /// The one builder a derived-data kind is built with, set by the editor / cook host (Set*Builder) and read on
    /// a cache miss from any loader thread. Get() hands out a copy, so a builder is never called under the lock.
    template <typename Builder>
    class BuilderSlot
    {
    public:
        void Set( Builder builder )
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Builder = std::move( builder );
        }

        Builder Get() const
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Builder;
        }

    private:
        mutable std::mutex m_Mutex;
        Builder            m_Builder;
    };
} // namespace Desert::Assets
