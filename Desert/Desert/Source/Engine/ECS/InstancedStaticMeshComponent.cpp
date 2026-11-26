#include <Engine/ECS/InstancedStaticMeshComponent.hpp>

namespace Desert::ECS
{
    auto InstancedStaticMeshComponent::NextInstanceRevision() -> uint64_t
    {
        static std::atomic<uint64_t> s_Revision{ 0 };
        return s_Revision.fetch_add( 1, std::memory_order_relaxed ) + 1;
    }
}
