#include <Engine/Graphic/API/Vulkan/VulkanSharedGpuMemory.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <format>
#include <functional>
#include <string>

#if defined( _WIN32 )
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using Common::Scalability::GpuSharing;

        struct DeviceHandles
        {
            VkDevice         Device   = VK_NULL_HANDLE;
            VkPhysicalDevice Physical = VK_NULL_HANDLE;
        };

        [[nodiscard]] DeviceHandles CurrentDevice()
        {
            const auto device =
                 std::static_pointer_cast<VulkanLogicalDevice>( EngineContext::GetInstance().GetDevice() );
            if ( !device )
                return {};
            return { .Device   = device->GetVulkanLogicalDevice(),
                     .Physical = device->GetPhysicalDevice()->GetVulkanPhysicalDevice() };
        }

        // The catalog's handle type, or the refusal that names why there is none.
        [[nodiscard]] Common::ResultStr<GpuSharing> CatalogSharing( const std::string_view what,
                                                                    const std::string_view name )
        {
            const GpuSharing sharing = EngineContext::GetInstance().GetCapabilities().Catalog.Sharing;
            if ( sharing == GpuSharing::Win32 || sharing == GpuSharing::Fd )
                return Common::MakeSuccess( sharing );
            return Common::MakeError<GpuSharing>( std::format(
                 "{} '{}': CapabilityCatalog::Sharing is {} (the device exports no {} by a Win32 or POSIX handle)",
                 what, name, Common::Scalability::kGpuSharingNames[static_cast<std::size_t>( sharing )],
                 what == "shared buffer" ? "memory" : "timeline semaphore" ) );
        }

        // Runs @p destroy once the frames recorded so far have retired (the allocator's ring).
        void DestroyLater( std::function<void()> destroy )
        {
            const auto context =
                 std::static_pointer_cast<VulkanContext>( EngineContext::GetInstance().GetRendererContext() );
            if ( context && context->GetVulkanAllocator() )
                context->GetVulkanAllocator()->RT_DestroyLater( std::move( destroy ) );
            else
                destroy(); // no renderer left: nothing can be in flight
        }

        [[nodiscard]] PFN_vkVoidFunction Load( const VkDevice device, const char* name )
        {
            return vkGetDeviceProcAddr( device, name );
        }

        class VulkanSharedBuffer final : public SharedGpuBuffer
        {
        public:
            VulkanSharedBuffer( std::string name, const GpuSharing kind, const VkDevice device,
                                const VkBuffer buffer, const VkDeviceMemory memory, const uint64_t bytes,
                                const uint64_t allocationBytes )
                 : m_Name( std::move( name ) ), m_Kind( kind ), m_Device( device ), m_Buffer( buffer ),
                   m_Memory( memory ), m_Bytes( bytes ), m_AllocationBytes( allocationBytes ),
                   m_Physical( VulkanRdgBuffer::Wrap( buffer, bytes ) ),
                   m_State( std::make_shared<RDG::AccessState>( RDG::GetAccessState( RDG::Access::None ) ) )
            {
            }

            ~VulkanSharedBuffer() override
            {
                DestroyLater(
                     [device = m_Device, buffer = m_Buffer, memory = m_Memory, win32 = m_Win32]()
                     {
#if defined( _WIN32 )
                         if ( win32 )
                             CloseHandle( static_cast<HANDLE>( win32 ) );
#else
                         (void)win32;
#endif
                         vkDestroyBuffer( device, buffer, nullptr );
                         vkFreeMemory( device, memory, nullptr );
                     } );
            }

            [[nodiscard]] uint64_t Bytes() const override
            {
                return m_Bytes;
            }

            [[nodiscard]] Common::ResultStr<SharedHandle> ExportHandle() override
            {
                SharedHandle handle{ .Kind = m_Kind, .AllocationBytes = m_AllocationBytes, .Dedicated = true };
#if defined( _WIN32 )
                if ( m_Kind == GpuSharing::Win32 )
                {
                    if ( !m_Win32 )
                    {
                        const auto get = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
                             Load( m_Device, "vkGetMemoryWin32HandleKHR" ) );
                        if ( !get )
                            return Common::MakeError<SharedHandle>( std::format(
                                 "shared buffer '{}': vkGetMemoryWin32HandleKHR is not loaded", m_Name ) );
                        VkMemoryGetWin32HandleInfoKHR info{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
                        info.memory           = m_Memory;
                        info.handleType       = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                        HANDLE         win32  = nullptr;
                        const VkResult result = get( m_Device, &info, &win32 );
                        if ( result != VK_SUCCESS || !win32 )
                            return Common::MakeError<SharedHandle>(
                                 std::format( "shared buffer '{}': vkGetMemoryWin32HandleKHR returned {}", m_Name,
                                              static_cast<int>( result ) ) );
                        m_Win32 = win32;
                    }
                    handle.Win32 = m_Win32;
                    return Common::MakeSuccess( handle );
                }
#endif
                if ( m_Kind == GpuSharing::Fd )
                {
                    const auto get =
                         reinterpret_cast<PFN_vkGetMemoryFdKHR>( Load( m_Device, "vkGetMemoryFdKHR" ) );
                    if ( !get )
                        return Common::MakeError<SharedHandle>(
                             std::format( "shared buffer '{}': vkGetMemoryFdKHR is not loaded", m_Name ) );
                    VkMemoryGetFdInfoKHR info{ VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
                    info.memory           = m_Memory;
                    info.handleType       = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
                    int            fd     = -1;
                    const VkResult result = get( m_Device, &info, &fd );
                    if ( result != VK_SUCCESS || fd < 0 )
                        return Common::MakeError<SharedHandle>(
                             std::format( "shared buffer '{}': vkGetMemoryFdKHR returned {}", m_Name,
                                          static_cast<int>( result ) ) );
                    handle.Fd = fd;
                    return Common::MakeSuccess( handle );
                }
                return Common::MakeError<SharedHandle>(
                     std::format( "shared buffer '{}': handle type {} is not exported on this platform", m_Name,
                                  Common::Scalability::kGpuSharingNames[static_cast<std::size_t>( m_Kind )] ) );
            }

            [[nodiscard]] Common::BoolResultStr Import( RDG::ExternalBuffer& into ) override
            {
                into.Desc.Bytes = m_Bytes;
                into.Physical   = m_Physical;
                into.State      = *m_State;
                // The state lives beside the buffer and outlives it in the hook: a graph that ends after the
                // buffer was dropped records into a state nobody reads.
                into.RecordFinalState = [state = m_State]( const RDG::AccessState& final ) -> Common::BoolResultStr
                {
                    *state = final;
                    return Common::MakeSuccess( true );
                };
                return Common::MakeSuccess( true );
            }

        private:
            std::string                       m_Name;
            GpuSharing                        m_Kind;
            VkDevice                          m_Device;
            VkBuffer                          m_Buffer;
            VkDeviceMemory                    m_Memory;
            uint64_t                          m_Bytes;
            uint64_t                          m_AllocationBytes;
            std::shared_ptr<VulkanRdgBuffer>  m_Physical;
            std::shared_ptr<RDG::AccessState> m_State;
            void*                             m_Win32 = nullptr; // HANDLE, owned
        };

        class VulkanSharedTimeline final : public SharedGpuTimeline
        {
        public:
            VulkanSharedTimeline( std::string name, const GpuSharing kind, const VkDevice device,
                                  const VkSemaphore semaphore )
                 : m_Name( std::move( name ) ), m_Kind( kind ), m_Device( device ), m_Semaphore( semaphore )
            {
            }

            ~VulkanSharedTimeline() override
            {
                DestroyLater(
                     [device = m_Device, semaphore = m_Semaphore, win32 = m_Win32]()
                     {
#if defined( _WIN32 )
                         if ( win32 )
                             CloseHandle( static_cast<HANDLE>( win32 ) );
#else
                         (void)win32;
#endif
                         vkDestroySemaphore( device, semaphore, nullptr );
                     } );
            }

            [[nodiscard]] uint64_t Semaphore() const override
            {
                return reinterpret_cast<uint64_t>( m_Semaphore );
            }

            [[nodiscard]] Common::ResultStr<SharedHandle> ExportHandle() override
            {
                SharedHandle handle{ .Kind = m_Kind };
#if defined( _WIN32 )
                if ( m_Kind == GpuSharing::Win32 )
                {
                    if ( !m_Win32 )
                    {
                        const auto get = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
                             Load( m_Device, "vkGetSemaphoreWin32HandleKHR" ) );
                        if ( !get )
                            return Common::MakeError<SharedHandle>( std::format(
                                 "shared timeline '{}': vkGetSemaphoreWin32HandleKHR is not loaded", m_Name ) );
                        VkSemaphoreGetWin32HandleInfoKHR info{
                             VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
                        info.semaphore        = m_Semaphore;
                        info.handleType       = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                        HANDLE         win32  = nullptr;
                        const VkResult result = get( m_Device, &info, &win32 );
                        if ( result != VK_SUCCESS || !win32 )
                            return Common::MakeError<SharedHandle>(
                                 std::format( "shared timeline '{}': vkGetSemaphoreWin32HandleKHR returned {}",
                                              m_Name, static_cast<int>( result ) ) );
                        m_Win32 = win32;
                    }
                    handle.Win32 = m_Win32;
                    return Common::MakeSuccess( handle );
                }
#endif
                if ( m_Kind == GpuSharing::Fd )
                {
                    const auto get =
                         reinterpret_cast<PFN_vkGetSemaphoreFdKHR>( Load( m_Device, "vkGetSemaphoreFdKHR" ) );
                    if ( !get )
                        return Common::MakeError<SharedHandle>(
                             std::format( "shared timeline '{}': vkGetSemaphoreFdKHR is not loaded", m_Name ) );
                    VkSemaphoreGetFdInfoKHR info{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
                    info.semaphore        = m_Semaphore;
                    info.handleType       = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                    int            fd     = -1;
                    const VkResult result = get( m_Device, &info, &fd );
                    if ( result != VK_SUCCESS || fd < 0 )
                        return Common::MakeError<SharedHandle>(
                             std::format( "shared timeline '{}': vkGetSemaphoreFdKHR returned {}", m_Name,
                                          static_cast<int>( result ) ) );
                    handle.Fd = fd;
                    return Common::MakeSuccess( handle );
                }
                return Common::MakeError<SharedHandle>(
                     std::format( "shared timeline '{}': handle type {} is not exported on this platform", m_Name,
                                  Common::Scalability::kGpuSharingNames[static_cast<std::size_t>( m_Kind )] ) );
            }

        private:
            std::string m_Name;
            GpuSharing  m_Kind;
            VkDevice    m_Device;
            VkSemaphore m_Semaphore;
            void*       m_Win32 = nullptr; // HANDLE, owned
        };

        [[nodiscard]] VkExternalMemoryHandleTypeFlagBits MemoryHandleType( const GpuSharing kind )
        {
            return kind == GpuSharing::Win32 ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
                                             : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        }
        [[nodiscard]] VkExternalSemaphoreHandleTypeFlagBits SemaphoreHandleType( const GpuSharing kind )
        {
            return kind == GpuSharing::Win32 ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
                                             : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        }
    } // namespace

    Common::ResultStr<std::shared_ptr<SharedGpuBuffer>> CreateVulkanSharedBuffer( const std::string_view name,
                                                                                  const uint64_t         bytes )
    {
        using Result       = std::shared_ptr<SharedGpuBuffer>;
        const auto sharing = CatalogSharing( "shared buffer", name );
        if ( !sharing )
            return Common::MakeError<Result>( sharing.GetError() );
        const DeviceHandles handles = CurrentDevice();
        if ( handles.Device == VK_NULL_HANDLE || bytes == 0u )
            return Common::MakeError<Result>(
                 std::format( "shared buffer '{}': {}", name, bytes == 0u ? "zero bytes" : "no Vulkan device" ) );
        const VkExternalMemoryHandleTypeFlagBits handleType = MemoryHandleType( sharing.GetValue() );

        VkExternalMemoryBufferCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = handleType;
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.pNext = &external;
        bufferInfo.size  = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer        = VK_NULL_HANDLE;
        if ( const VkResult result = vkCreateBuffer( handles.Device, &bufferInfo, nullptr, &buffer );
             result != VK_SUCCESS )
            return Common::MakeError<Result>( std::format( "shared buffer '{}': vkCreateBuffer returned {}", name,
                                                           static_cast<int>( result ) ) );

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements( handles.Device, buffer, &requirements );
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties( handles.Physical, &memory );
        uint32_t typeIndex = UINT32_MAX;
        for ( uint32_t i = 0; i < memory.memoryTypeCount; ++i )
            if ( ( requirements.memoryTypeBits & ( 1u << i ) ) != 0u &&
                 ( memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) != 0u )
            {
                typeIndex = i;
                break;
            }
        if ( typeIndex == UINT32_MAX )
        {
            vkDestroyBuffer( handles.Device, buffer, nullptr );
            return Common::MakeError<Result>( std::format(
                 "shared buffer '{}': no device-local memory type takes an exportable buffer", name ) );
        }

        VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = buffer;
        VkExportMemoryAllocateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exportInfo.pNext       = &dedicated;
        exportInfo.handleTypes = handleType;
        VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.pNext           = &exportInfo;
        allocateInfo.allocationSize  = requirements.size;
        allocateInfo.memoryTypeIndex = typeIndex;
        VkDeviceMemory deviceMemory  = VK_NULL_HANDLE;
        if ( const VkResult result = vkAllocateMemory( handles.Device, &allocateInfo, nullptr, &deviceMemory );
             result != VK_SUCCESS )
        {
            vkDestroyBuffer( handles.Device, buffer, nullptr );
            return Common::MakeError<Result>(
                 std::format( "shared buffer '{}': vkAllocateMemory ({} bytes) returned {}", name,
                              requirements.size, static_cast<int>( result ) ) );
        }
        if ( const VkResult result = vkBindBufferMemory( handles.Device, buffer, deviceMemory, 0 );
             result != VK_SUCCESS )
        {
            vkDestroyBuffer( handles.Device, buffer, nullptr );
            vkFreeMemory( handles.Device, deviceMemory, nullptr );
            return Common::MakeError<Result>( std::format( "shared buffer '{}': vkBindBufferMemory returned {}",
                                                           name, static_cast<int>( result ) ) );
        }
        return Common::MakeSuccess<Result>(
             std::make_shared<VulkanSharedBuffer>( std::string( name ), sharing.GetValue(), handles.Device, buffer,
                                                   deviceMemory, bytes, requirements.size ) );
    }

    Common::ResultStr<std::shared_ptr<SharedGpuTimeline>> CreateVulkanSharedTimeline( const std::string_view name )
    {
        using Result       = std::shared_ptr<SharedGpuTimeline>;
        const auto sharing = CatalogSharing( "shared timeline", name );
        if ( !sharing )
            return Common::MakeError<Result>( sharing.GetError() );
        const DeviceHandles handles = CurrentDevice();
        if ( handles.Device == VK_NULL_HANDLE )
            return Common::MakeError<Result>( std::format( "shared timeline '{}': no Vulkan device", name ) );

        VkExportSemaphoreCreateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        exportInfo.handleTypes = SemaphoreHandleType( sharing.GetValue() );
        VkSemaphoreTypeCreateInfo timeline{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        timeline.pNext         = &exportInfo;
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline.initialValue  = 0;
        VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        info.pNext            = &timeline;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if ( const VkResult result = vkCreateSemaphore( handles.Device, &info, nullptr, &semaphore );
             result != VK_SUCCESS )
            return Common::MakeError<Result>( std::format( "shared timeline '{}': vkCreateSemaphore returned {}",
                                                           name, static_cast<int>( result ) ) );
        return Common::MakeSuccess<Result>( std::make_shared<VulkanSharedTimeline>(
             std::string( name ), sharing.GetValue(), handles.Device, semaphore ) );
    }
} // namespace Desert::Graphic::API::Vulkan
