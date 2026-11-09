#include "Device.hpp"
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#if defined( DESERT_PLATFORM_WINDOWS )
#include <Engine/Graphic/API/D3D12/D3D12Rhi.hpp>
#endif

namespace Desert::Engine
{
    Common::ResultStr<std::shared_ptr<Device>> Device::Create()
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return Common::MakeError<std::shared_ptr<Device>>( "no rendering API is selected" );
            case Graphic::RendererAPIType::D3D12:
            {
#if defined( DESERT_PLATFORM_WINDOWS )
                auto device = Graphic::API::D3D12::D3D12Device::Create();
                if ( !device )
                    return Common::MakeError<std::shared_ptr<Device>>( device.GetError() );
                return Common::MakeSuccess<std::shared_ptr<Device>>( device.ExtractValue() );
#else
                return Common::MakeError<std::shared_ptr<Device>>( "the D3D12 backend is Windows only" );
#endif
            }
            case Graphic::RendererAPIType::Vulkan:
            {
                auto device = Graphic::API::Vulkan::VulkanLogicalDevice::Create();
                if ( !device )
                    return Common::MakeError<std::shared_ptr<Device>>( device.GetError() );
                return Common::MakeSuccess<std::shared_ptr<Device>>( device.ExtractValue() );
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return Common::MakeError<std::shared_ptr<Device>>( "unknown rendering API" );
    }

} // namespace Desert::Engine