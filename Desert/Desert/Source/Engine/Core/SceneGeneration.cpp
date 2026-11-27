#include <Engine/Core/SceneGeneration.hpp>

namespace Desert::Core
{
    uint64_t NextSceneGeneration()
    {
        static std::atomic<uint64_t> counter{ 0 };
        return counter.fetch_add( 1, std::memory_order_relaxed ) + 1;
    }
}
