#include <Engine/Graphic/GpuBatch.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    Common::ResultStr<std::unique_ptr<GpuBatch>> GpuBatch::Begin()
    {
        return Renderer::GetInstance().Rhi().BeginGpuBatch();
    }
} // namespace Desert::Graphic
