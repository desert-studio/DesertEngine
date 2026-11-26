#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::ShaderResources
{
    // A BUFFER THAT DID NOT BUILD DOES NOT LEAVE THE BACKEND: null = refused, with its name, size and binding
    // in the log (IRhi::CreateStorageBuffer). Every caller already tests the result for null.
    std::shared_ptr<StorageBuffer> StorageBuffer::Create( const std::string_view debugName, uint32_t size,
                                                          uint32_t binding, bool persistent )
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateStorageBuffer( debugName, size, binding, persistent );
    }

    std::shared_ptr<StorageBuffer> StorageBuffer::CreateReadback( const std::string_view debugName, uint32_t size,
                                                                  uint32_t binding )
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateReadbackStorageBuffer( debugName, size, binding );
    }

    std::shared_ptr<StorageBuffer> StorageBuffer::CreateDeviceLocal( const std::string_view debugName,
                                                                     uint32_t size, uint32_t binding )
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateDeviceLocalStorageBuffer( debugName, size, binding );
    }

} // namespace Desert::ShaderResources
