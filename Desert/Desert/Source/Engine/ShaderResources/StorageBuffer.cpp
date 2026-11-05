#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>

#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <numeric>

namespace Desert::ShaderResources
{

    std::shared_ptr<StorageBuffer> StorageBuffer::Create( const std::string_view debugName, uint32_t size,
                                                          uint32_t binding, bool persistent )
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
            {
                // A BUFFER THAT DID NOT BUILD DOES NOT LEAVE THIS FUNCTION. Its constructor allocates and
                // maps every copy, and since Г13 it refuses as a whole rather than skipping the copy it
                // could not make — so an object that failed has no device buffer, no mapping and no
                // descriptor to offer, and every caller in the engine already tests this result for null
                // (`if ( !m_ParamsBuffer ) return Common::MakeError( ... )`). Handing one back would put
                // the failure at the descriptor write instead of here, where the size and the name are.
                //
                // The RendererAPIType::None arm above already returns nullptr, so null is inside this
                // function's declared contract rather than a new outcome.
                auto buffer =
                     std::make_shared<API::Vulkan::VulkanStorageBuffer>( debugName, size, binding, persistent );
                const auto usable = buffer->EnsureMapped();
                if ( !usable.IsSuccess() )
                {
                    LOG_ERROR( "[StorageBuffer] '{}' ({} bytes, binding {}) is not usable and was not "
                               "created: {}",
                               debugName, size, binding, usable.GetError() );
                    return nullptr;
                }
                return buffer;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return nullptr;
    }

    std::shared_ptr<StorageBuffer> StorageBuffer::CreateReadback( const std::string_view debugName, uint32_t size,
                                                                  uint32_t binding )
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
            {
                // As Create: a buffer whose active copy cannot be made and mapped does not leave this function.
                auto       buffer = std::make_shared<API::Vulkan::VulkanStorageBuffer>( debugName, size, binding,
                                                                                        Persistence::PerFrameReadback );
                const auto usable = buffer->EnsureMapped();
                if ( !usable.IsSuccess() )
                {
                    LOG_ERROR( "[StorageBuffer] read-back '{}' ({} bytes, binding {}) is not usable and was not "
                               "created: {}",
                               debugName, size, binding, usable.GetError() );
                    return nullptr;
                }
                return buffer;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return nullptr;
    }

    std::shared_ptr<StorageBuffer> StorageBuffer::CreateDeviceLocal( const std::string_view debugName,
                                                                     uint32_t size, uint32_t binding )
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
            {
                // As Create: a buffer that did not build does not leave this function. It has no mapping to
                // ask about (EnsureMapped refuses by design), so the question is whether its one device buffer
                // was made - PrepareForRecording answers it for a buffer shared by every frame.
                auto       buffer = std::make_shared<API::Vulkan::VulkanStorageBuffer>( debugName, size, binding,
                                                                                        Persistence::DeviceOnly );
                const auto usable = buffer->PrepareForRecording();
                if ( !usable.IsSuccess() )
                {
                    LOG_ERROR(
                         "[StorageBuffer] device-local '{}' ({} bytes, binding {}) is not usable and was not "
                         "created: {}",
                         debugName, size, binding, usable.GetError() );
                    return nullptr;
                }
                return buffer;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return nullptr;
    }

} // namespace Desert::ShaderResources
