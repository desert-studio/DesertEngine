#include <Engine/Animation/AnimationUpdateRate.hpp>

namespace Desert::Animation
{
    namespace
    {
        std::atomic<float> g_AnimationCpuMsPerFrame{ -1.0f };
    } // namespace

    void SetAnimationCpuMsPerFrame( float ms ) noexcept
    {
        g_AnimationCpuMsPerFrame.store( ms, std::memory_order_relaxed );
    }

    float AnimationCpuMsPerFrame() noexcept
    {
        return g_AnimationCpuMsPerFrame.load( std::memory_order_relaxed );
    }
} // namespace Desert::Animation
