#pragma once

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <type_traits>

// THE DEVICE ADDRESS OF A MESH'S DATA IN A GEOMETRY BUFFER, READ WHEN IT IS USED. A GeometryPool arena keeps its
// VertexBuffer / IndexBuffer object across a growth but changes its VkBuffer (Graphic/GeometryPool.hpp), so a
// ray-tracing geometry holds the buffer and the byte offset and asks for the address at every build, refit and
// metadata write - never a VkDeviceAddress cached across frames.
namespace Desert::Graphic::API::Vulkan
{
    // A mesh's span in one geometry buffer: the buffer (held, so it outlives every read) and where the mesh
    // begins.
    template <typename TBuffer>
    struct GeometryBufferRange
    {
        std::shared_ptr<TBuffer> Buffer;
        uint64_t                 ByteOffset = 0;

        [[nodiscard]] bool operator==( const GeometryBufferRange& ) const = default;
    };
    using VertexBufferRange = GeometryBufferRange<VertexBuffer>;
    using IndexBufferRange  = GeometryBufferRange<IndexBuffer>;

    // The current device address of @p range's first byte (the buffer's own, VertexBuffer::GetDeviceAddress, plus
    // the mesh's offset); 0 when the buffer is missing, not resident, or the device has no buffer-device-address
    // functions.
    template <typename TBuffer>
    [[nodiscard]] inline VkDeviceAddress CurrentGeometryAddress( const GeometryBufferRange<TBuffer>& range )
    {
        if ( !range.Buffer )
            return 0;
        const uint64_t address = range.Buffer->GetDeviceAddress();
        return address == 0u ? 0u : address + range.ByteOffset;
    }
} // namespace Desert::Graphic::API::Vulkan
