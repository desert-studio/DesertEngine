#include <Engine/Core/CameraSourceId.hpp>

namespace Desert::Core
{
    CameraSourceId IssueCameraSourceId()
    {
        static std::atomic<uint32_t> counter{ 0 };
        return kIssuedCameraSourceBit | ( counter.fetch_add( 1, std::memory_order_relaxed ) + 1u );
    }
}
