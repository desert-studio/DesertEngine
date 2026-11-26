#include "Device.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Engine
{
    // The backend chose and built it from the RhiSelection (adapter, device profile); a refusal names why.
    Common::ResultStr<std::shared_ptr<Device>> Device::Create()
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateDevice();
    }
} // namespace Desert::Engine
