#include <Engine/Graphic/Materials/Properties/PropertyVersion.hpp>

namespace Desert::Graphic::PropertyVersion
{
    uint64_t Next() noexcept
    {
        static std::atomic<uint64_t> counter{ kNeverWritten };
        return counter.fetch_add( 1, std::memory_order_relaxed ) + 1;
    }
}
