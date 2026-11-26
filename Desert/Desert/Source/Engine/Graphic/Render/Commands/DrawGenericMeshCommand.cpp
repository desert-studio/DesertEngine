#include <Engine/Graphic/Render/Commands/DrawGenericMeshCommand.hpp>

namespace Desert::Graphic::Render
{
    uint64_t NextGenericMeshDrawParamsRevision() noexcept
    {
        static std::atomic<uint64_t> next{ 1u };
        return next.fetch_add( 1u, std::memory_order_relaxed );
    }
}
