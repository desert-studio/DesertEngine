#include <Engine/Graphic/Rhi/Rhi.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanRhi.hpp>

namespace Desert::Graphic
{
    Common::ResultStr<std::unique_ptr<IRhi>> CreateRhi( const RhiSelection& selection )
    {
        switch ( selection.Kind )
        {
            case RhiKind::Vulkan:
                return Common::MakeSuccess<std::unique_ptr<IRhi>>(
                     std::make_unique<API::Vulkan::VulkanRhi>( selection ) );
            case RhiKind::D3D12:
                // An explicit refusal, not a fallback: the D3D12 renderer has no frame yet, and a process that
                // asked for d3d12 and silently ran Vulkan would prove nothing about either.
                return Common::MakeError<std::unique_ptr<IRhi>>(
                     "rendering backend d3d12: renderer not finished (resources, render graph and frame are not "
                     "implemented); run without --rhi d3d12" );
        }
        return Common::MakeFormattedError<std::unique_ptr<IRhi>>( "rendering backend {} is not a backend",
                                                                  static_cast<int>( selection.Kind ) );
    }
} // namespace Desert::Graphic
