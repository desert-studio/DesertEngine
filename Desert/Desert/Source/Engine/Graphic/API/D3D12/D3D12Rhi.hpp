#pragma once

// THE DIRECT3D 12 BACKEND, PART 1 (DX12-RHI). UE is the reference: FD3D12DynamicRHI owns one FD3D12Adapter, which
// owns the ID3D12Device and one FD3D12Queue per ED3D12QueueType (Direct, Async compute, Copy), each with its own
// fence; command allocators are recycled behind their queue's sync point (FD3D12CommandAllocator), descriptors
// live in a CPU staging manager (FD3D12OfflineDescriptorManager) and one shader-visible bindless heap per type
// (FD3D12BindlessDescriptorManager), and render-graph transitions are recorded as enhanced barriers
// (FD3D12EnhancedBarriersForAdapter). This file is that layer: device, queues, allocators, descriptors,
// resources, barrier mapping and the flip-model swap chain. The frame recording that consumes it (a
// RendererAPI and an RDG::IBackend over this device) is part 2.
//
// Windows only: Desert/Desert/CMakeLists.txt adds every /D3D12/ source to the Windows list, so non-Windows hosts
// and the default Vulkan path compile without it. Enhanced barriers are REQUIRED (no legacy ResourceBarrier
// path): the device is refused by name without them, as UE's SM6 D3D12 path refuses below its feature floor.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Device.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RDG/RDGCompileResult.hpp>
#include <Engine/Graphic/SwapChain.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic::API::D3D12
{
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // "<what> failed: HRESULT 0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)" — every refusal of this backend names the
    // call.
    [[nodiscard]] std::string DescribeHResult( std::string_view what, HRESULT hr );
    // UTF-16 (DXGI adapter / output names) to UTF-8.
    [[nodiscard]] std::string NarrowUtf16( const wchar_t* text );

    // UE ED3D12QueueType. One queue and one fence per kind; RDG::Pipe::AsyncCompute runs on Compute.
    enum class QueueKind : uint8_t
    {
        Graphics,
        Compute,
        Copy,
        Count
    };
    inline constexpr uint32_t kQueueKindCount = static_cast<uint32_t>( QueueKind::Count );

    [[nodiscard]] D3D12_COMMAND_LIST_TYPE ToCommandListType( QueueKind kind ) noexcept;
    [[nodiscard]] QueueKind               QueueForPipe( RDG::Pipe pipe ) noexcept;

    // --- ADAPTER (UE FD3D12DynamicRHIModule::FindAdapter) ------------------------------------------------------

    struct AdapterChoice
    {
        ComPtr<IDXGIAdapter4> Adapter;
        DXGI_ADAPTER_DESC3    Desc{};
        D3D_FEATURE_LEVEL     FeatureLevel = D3D_FEATURE_LEVEL_12_0;
    };

    // The factory, with the DXGI debug layer in Debug builds (D3D12GetDebugInterface beside it, in the device).
    [[nodiscard]] Common::ResultStr<ComPtr<IDXGIFactory6>> CreateDxgiFactory( bool debugLayer ) noexcept;
    // The first hardware adapter in DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE order that creates a feature level 12_0
    // device. Software adapters (WARP, the Basic Render Driver) are skipped. Refused by name when none qualifies.
    [[nodiscard]] Common::ResultStr<AdapterChoice> SelectAdapter( IDXGIFactory6& factory ) noexcept;

    // --- QUEUES AND FENCES (UE FD3D12Queue + FD3D12Fence) -------------------------------------------------------

    class D3D12Queue
    {
    public:
        D3D12Queue() = default;
        ~D3D12Queue();
        D3D12Queue( const D3D12Queue& )            = delete;
        D3D12Queue& operator=( const D3D12Queue& ) = delete;

        [[nodiscard]] Common::BoolResultStr Init( ID3D12Device& device, QueueKind kind ) noexcept;
        void                                Release() noexcept;

        // Executes @p lists and signals the queue's fence behind them. Answers the signalled value: the sync
        // point a command allocator, a deferred descriptor free or a CPU wait is keyed by.
        [[nodiscard]] Common::ResultStr<uint64_t> Submit( std::span<ID3D12CommandList* const> lists ) noexcept;
        [[nodiscard]] Common::ResultStr<uint64_t> Signal() noexcept;
        // GPU-side wait of this queue on @p other reaching @p value (cross-queue RDG sync, async compute).
        [[nodiscard]] Common::BoolResultStr WaitOnQueue( const D3D12Queue& other, uint64_t value ) noexcept;
        // CPU wait; returns at once when the value has completed.
        [[nodiscard]] Common::BoolResultStr WaitCpu( uint64_t value ) noexcept;
        // Signal + CPU wait: everything submitted so far has retired.
        [[nodiscard]] Common::BoolResultStr Flush() noexcept;

        [[nodiscard]] bool     IsComplete( uint64_t value ) const noexcept;
        [[nodiscard]] uint64_t CompletedValue() const noexcept;
        [[nodiscard]] uint64_t LastSignalled() const noexcept
        {
            return m_LastSignalled.load( std::memory_order_acquire );
        }
        [[nodiscard]] ID3D12CommandQueue* Native() const noexcept
        {
            return m_Queue.Get();
        }
        [[nodiscard]] ID3D12Fence* Fence() const noexcept
        {
            return m_Fence.Get();
        }
        [[nodiscard]] QueueKind Kind() const noexcept
        {
            return m_Kind;
        }

    private:
        ComPtr<ID3D12CommandQueue> m_Queue;
        ComPtr<ID3D12Fence>        m_Fence;
        HANDLE                     m_Event = nullptr;
        std::mutex                 m_SubmitMutex; // ExecuteCommandLists + Signal are one ordered act
        std::mutex                 m_WaitMutex;   // one SetEventOnCompletion on the shared event at a time
        uint64_t                   m_NextValue = 1;
        std::atomic<uint64_t>      m_LastSignalled{ 0 };
        QueueKind                  m_Kind = QueueKind::Graphics;
    };

    // --- COMMAND ALLOCATORS AND LISTS (UE FD3D12CommandAllocator / FD3D12CommandList pools) ---------------------

    struct CommandContext
    {
        ID3D12CommandAllocator*     Allocator = nullptr;
        ID3D12GraphicsCommandList7* List      = nullptr;
        uint32_t                    Entry     = UINT32_MAX;
    };

    // One pool per queue kind. Acquire hands out an allocator whose last submission has retired on its queue (or
    // creates one) with its list reset onto it; Release returns it keyed by the fence value of that submission.
    // The pool grows to the number of contexts in flight and then recycles: no allocation in steady state.
    class D3D12CommandPool
    {
    public:
        [[nodiscard]] Common::BoolResultStr Init( ID3D12Device10& device, QueueKind kind ) noexcept;
        void                                Release() noexcept;

        [[nodiscard]] Common::ResultStr<CommandContext> Acquire( const D3D12Queue& queue ) noexcept;
        // @p retireValue: the queue's fence value of the submission that carried the list.
        void Recycle( const CommandContext& context, uint64_t retireValue ) noexcept;

    private:
        struct Entry
        {
            ComPtr<ID3D12CommandAllocator>     Allocator;
            ComPtr<ID3D12GraphicsCommandList7> List;
            uint64_t                           RetireValue = 0;
            bool                               InUse       = false;
        };
        std::mutex         m_Mutex;
        std::vector<Entry> m_Entries;
        ID3D12Device10*    m_Device = nullptr;
        QueueKind          m_Kind   = QueueKind::Graphics;
    };

    // --- DESCRIPTORS
    // ----------------------------------------------------------------------------------------------

    struct CpuDescriptor
    {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle{};
        uint32_t                    Page  = UINT32_MAX;
        uint32_t                    Index = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return Page != UINT32_MAX;
        }
    };

    // CPU STAGING (UE FD3D12OfflineDescriptorManager): non-shader-visible pages of one descriptor type, a free
    // list per page. RTV / DSV live here only; CBV/SRV/UAV and samplers are written here and copied into the
    // bindless heap below.
    class D3D12CpuDescriptorHeap
    {
    public:
        [[nodiscard]] Common::BoolResultStr Init( ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                  uint32_t descriptorsPerPage ) noexcept;
        void                                Release() noexcept;

        [[nodiscard]] Common::ResultStr<CpuDescriptor> Allocate() noexcept;
        void                                           Free( const CpuDescriptor& descriptor ) noexcept;

    private:
        struct Page
        {
            ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_CPU_DESCRIPTOR_HANDLE  Start{};
            std::vector<uint32_t>        FreeList;
        };
        [[nodiscard]] Common::BoolResultStr AddPage() noexcept;

        std::mutex                 m_Mutex;
        std::vector<Page>          m_Pages;
        ID3D12Device*              m_Device    = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE m_Type      = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uint32_t                   m_PerPage   = 0;
        uint32_t                   m_Increment = 0;
    };

    // SHADER-VISIBLE BINDLESS (UE FD3D12BindlessDescriptorManager, SM 6.6 ResourceDescriptorHeap /
    // SamplerDescriptorHeap). One heap per type for the device's life; a slot is the index a shader reads.
    // A freed slot is reused only after the graphics fence value given at free time has completed — a frame in
    // flight may still index it (UE: deferred deletion behind the frame fence).
    class D3D12BindlessHeap
    {
    public:
        [[nodiscard]] Common::BoolResultStr Init( ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                  uint32_t capacity ) noexcept;
        void                                Release() noexcept;

        [[nodiscard]] Common::ResultStr<uint32_t> Allocate() noexcept;
        void                                      FreeAfter( uint32_t slot, uint64_t graphicsFenceValue ) noexcept;
        // Returns every deferred slot whose fence value has completed to the free list.
        void Retire( uint64_t completedGraphicsFenceValue ) noexcept;
        // Copies one staging descriptor into @p slot (CopyDescriptorsSimple).
        void Write( uint32_t slot, D3D12_CPU_DESCRIPTOR_HANDLE source ) noexcept;

        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle( uint32_t slot ) const noexcept;
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle( uint32_t slot ) const noexcept;
        [[nodiscard]] ID3D12DescriptorHeap*       Native() const noexcept
        {
            return m_Heap.Get();
        }
        [[nodiscard]] uint32_t Capacity() const noexcept
        {
            return m_Capacity;
        }

    private:
        struct Pending
        {
            uint32_t Slot  = 0;
            uint64_t Fence = 0;
        };
        std::mutex                   m_Mutex;
        ComPtr<ID3D12DescriptorHeap> m_Heap;
        ID3D12Device*                m_Device = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE   m_Type   = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        D3D12_CPU_DESCRIPTOR_HANDLE  m_CpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE  m_GpuStart{};
        uint32_t                     m_Increment = 0;
        uint32_t                     m_Capacity  = 0;
        uint32_t                     m_HighWater = 0; // slots [0, m_HighWater) were handed out at least once
        std::vector<uint32_t>        m_Free;
        std::vector<Pending>         m_Pending; // in free order, so fence values are non-decreasing
    };

    // --- RESOURCES (committed, or placed into an explicit heap: no D3D12MA) ------------------------------------

    // The engine format as DXGI sees it; DXGI_FORMAT_UNKNOWN for ImageFormat::Count. @p srgb selects the _SRGB
    // twin of the formats that have one (Core::Formats::HasSRGBVariant).
    [[nodiscard]] DXGI_FORMAT ToDxgiFormat( Core::Formats::ImageFormat format, bool srgb ) noexcept;
    [[nodiscard]] bool        IsDepthFormat( Core::Formats::ImageFormat format ) noexcept;
    // 2 for depth-stencil formats (depth plane + stencil plane), else 1.
    [[nodiscard]] uint32_t PlaneCount( Core::Formats::ImageFormat format ) noexcept;

    struct BufferDesc
    {
        uint64_t         Size           = 0;
        D3D12_HEAP_TYPE  Heap           = D3D12_HEAP_TYPE_DEFAULT; // UPLOAD / READBACK are persistently mapped
        bool             AllowUnordered = false;
        std::string_view Name;
    };

    struct TextureDesc
    {
        uint32_t                   Width          = 1;
        uint32_t                   Height         = 1;
        uint32_t                   ArrayLayers    = 1; // 6 per cube
        uint32_t                   MipLevels      = 1;
        uint32_t                   Samples        = 1;
        Core::Formats::ImageFormat Format         = Core::Formats::ImageFormat::RGBA8F;
        bool                       Srgb           = false;
        bool                       RenderTarget   = false;
        bool                       DepthStencil   = false;
        bool                       AllowUnordered = false;
        D3D12_BARRIER_LAYOUT       InitialLayout  = D3D12_BARRIER_LAYOUT_COMMON;
        std::string_view           Name;
    };

    struct D3D12Resource
    {
        ComPtr<ID3D12Resource>    Resource;
        D3D12_GPU_VIRTUAL_ADDRESS GpuAddress = 0;       // buffers only
        void*                     Mapped     = nullptr; // UPLOAD / READBACK buffers: mapped for their whole life
        uint32_t                  MipLevels  = 1;
        uint32_t                  ArraySize  = 1;
        uint32_t                  Planes     = 1;
    };

    struct D3D12Heap
    {
        ComPtr<ID3D12Heap> Heap;
        uint64_t           Size = 0;
    };

    [[nodiscard]] D3D12_RESOURCE_DESC1 MakeBufferDesc( const BufferDesc& desc ) noexcept;
    [[nodiscard]] D3D12_RESOURCE_DESC1 MakeTextureDesc( const TextureDesc& desc ) noexcept;

    [[nodiscard]] Common::ResultStr<D3D12Resource> CreateCommittedBuffer( ID3D12Device10&   device,
                                                                          const BufferDesc& desc ) noexcept;
    [[nodiscard]] Common::ResultStr<D3D12Resource> CreateCommittedTexture( ID3D12Device10&    device,
                                                                           const TextureDesc& desc ) noexcept;
    // A heap for placed resources (UE FD3D12TransientHeap): RDG's transient allocator aliases textures and buffers
    // inside one. Tier 2 heaps hold every resource class; on tier 1 @p flags must name one class.
    [[nodiscard]] Common::ResultStr<D3D12Heap>     CreatePlacementHeap( ID3D12Device10& device, uint64_t size,
                                                                        D3D12_HEAP_TYPE  type,
                                                                        D3D12_HEAP_FLAGS flags ) noexcept;
    [[nodiscard]] Common::ResultStr<D3D12Resource> CreatePlacedBuffer( ID3D12Device10&  device,
                                                                       const D3D12Heap& heap, uint64_t offset,
                                                                       const BufferDesc& desc ) noexcept;
    [[nodiscard]] Common::ResultStr<D3D12Resource> CreatePlacedTexture( ID3D12Device10&  device,
                                                                        const D3D12Heap& heap, uint64_t offset,
                                                                        const TextureDesc& desc ) noexcept;
    // Size + alignment a placed texture needs (GetResourceAllocationInfo2): RDG's IMemoryRequirementsProvider.
    [[nodiscard]] D3D12_RESOURCE_ALLOCATION_INFO QueryAllocation( ID3D12Device10&             device,
                                                                  const D3D12_RESOURCE_DESC1& desc ) noexcept;

    // --- RDG BARRIERS AS ENHANCED BARRIERS (UE FD3D12EnhancedBarriersForAdapter) --------------------------------

    struct BarrierState
    {
        D3D12_BARRIER_SYNC   Sync   = D3D12_BARRIER_SYNC_NONE;
        D3D12_BARRIER_ACCESS Access = D3D12_BARRIER_ACCESS_NO_ACCESS;
        D3D12_BARRIER_LAYOUT Layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
    };

    // PURE: RDG's API-neutral access state (RDGAccess.hpp) on @p queue. Buffers have no layout (UNDEFINED).
    [[nodiscard]] D3D12_BARRIER_SYNC   ToBarrierSync( RDG::PipelineStageFlags stages ) noexcept;
    [[nodiscard]] D3D12_BARRIER_ACCESS ToBarrierAccess( RDG::MemoryAccessFlags  memory,
                                                        RDG::PipelineStageFlags stages ) noexcept;
    [[nodiscard]] D3D12_BARRIER_LAYOUT ToBarrierLayout( const RDG::AccessState& state, QueueKind queue ) noexcept;
    [[nodiscard]] BarrierState         MapAccessState( const RDG::AccessState& state, bool texture,
                                                       QueueKind queue ) noexcept;

    // What a barrier needs to know about the resource RDG names by index (the backend resolves the index).
    struct BarrierTarget
    {
        ID3D12Resource* Resource  = nullptr;
        uint32_t        MipLevels = 1;
        uint32_t        ArraySize = 1;
        uint32_t        Planes    = 1;
    };

    // One RDG barrier batch as D3D12_BARRIER_GROUPs. Fixed capacity, no allocation: Add flushes into @p list
    // when full, Flush records whatever is left. The queue is the one the batch is recorded on: an
    // OwnershipRelease / OwnershipAcquire pair (Vulkan queue-family transfer) becomes a transition to / from the
    // COMMON layout, which every queue may access, and the fence RDG already waits on orders the two halves.
    class D3D12BarrierBatch
    {
    public:
        explicit D3D12BarrierBatch( QueueKind queue ) noexcept : m_Queue( queue )
        {
        }
        void Add( ID3D12GraphicsCommandList7& list, const RDG::Barrier& barrier,
                  const BarrierTarget& target ) noexcept;
        void Flush( ID3D12GraphicsCommandList7& list ) noexcept;

    private:
        static constexpr uint32_t                    kCapacity = 64;
        std::array<D3D12_TEXTURE_BARRIER, kCapacity> m_Textures{};
        std::array<D3D12_BUFFER_BARRIER, kCapacity>  m_Buffers{};
        uint32_t                                     m_TextureCount = 0;
        uint32_t                                     m_BufferCount  = 0;
        QueueKind                                    m_Queue;
    };

    // --- THE DEVICE (UE FD3D12Adapter + FD3D12Device) ---------------------------------------------------------

    class D3D12Device final : public Engine::Device
    {
    public:
        // Refused by name below the backend's floor: feature level 12_0, Shader Model 6.6 (bindless descriptor
        // heap indexing), resource binding tier 3, enhanced barriers.
        [[nodiscard]] static Common::ResultStr<std::shared_ptr<D3D12Device>> Create() noexcept;
        ~D3D12Device() override;

        [[nodiscard]] const Engine::DeviceCapabilities& GetCapabilities() const override
        {
            return m_Caps;
        }
        void                                     WaitIdle() const override;
        [[nodiscard]] Engine::DeviceMemoryReport QueryMemory() const override;
        [[nodiscard]] std::string                GetName() const override
        {
            return m_Caps.Name;
        }
        // No pipeline state object is created through this device before its RendererAPI (part 2) exists, so
        // there is no ID3D12PipelineLibrary content to write; answers success without touching the disk.
        [[nodiscard]] Common::BoolResultStr PersistPipelineCache() override;

        [[nodiscard]] ID3D12Device10& Native() const noexcept
        {
            return *m_Device.Get();
        }
        [[nodiscard]] IDXGIFactory6& Factory() const noexcept
        {
            return *m_Factory.Get();
        }
        [[nodiscard]] IDXGIAdapter4& Adapter() const noexcept
        {
            return *m_Adapter.Get();
        }
        [[nodiscard]] D3D12Queue& Queue( QueueKind kind ) const noexcept
        {
            return m_Queues[static_cast<uint32_t>( kind )];
        }
        [[nodiscard]] D3D12CommandPool& CommandPool( QueueKind kind ) noexcept
        {
            return m_Pools[static_cast<uint32_t>( kind )];
        }
        [[nodiscard]] D3D12CpuDescriptorHeap& StagingHeap( D3D12_DESCRIPTOR_HEAP_TYPE type ) noexcept
        {
            return m_Staging[static_cast<uint32_t>( type )];
        }
        [[nodiscard]] D3D12BindlessHeap& BindlessResources() noexcept
        {
            return m_BindlessResources;
        }
        [[nodiscard]] D3D12BindlessHeap& BindlessSamplers() noexcept
        {
            return m_BindlessSamplers;
        }
        [[nodiscard]] bool SupportsTearing() const noexcept
        {
            return m_AllowTearing;
        }
        [[nodiscard]] D3D_SHADER_MODEL ShaderModel() const noexcept
        {
            return m_ShaderModel;
        }

    private:
        D3D12Device() = default;
        [[nodiscard]] Common::BoolResultStr Init() noexcept;
        void                                FillCapabilities( const AdapterChoice& choice ) noexcept;
        void                                FillCatalog( const AdapterChoice& choice ) noexcept;

        ComPtr<IDXGIFactory6>  m_Factory;
        ComPtr<IDXGIAdapter4>  m_Adapter;
        ComPtr<ID3D12Device10> m_Device;
        DXGI_ADAPTER_DESC3     m_AdapterDesc{};

        mutable std::array<D3D12Queue, kQueueKindCount>                          m_Queues;
        std::array<D3D12CommandPool, kQueueKindCount>                            m_Pools;
        std::array<D3D12CpuDescriptorHeap, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_Staging;
        D3D12BindlessHeap                                                        m_BindlessResources;
        D3D12BindlessHeap                                                        m_BindlessSamplers;

        Engine::DeviceCapabilities m_Caps;
        D3D_SHADER_MODEL           m_ShaderModel        = D3D_SHADER_MODEL_6_0;
        D3D12_RAYTRACING_TIER      m_RaytracingTier     = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        D3D12_RESOURCE_HEAP_TIER   m_ResourceHeapTier   = D3D12_RESOURCE_HEAP_TIER_1;
        bool                       m_AllowTearing       = false;
        bool                       m_HdrOutputAvailable = false;
    };

    // --- THE SWAP CHAIN (UE FD3D12Viewport: flip model, HDR10 capable) ------------------------------------------

    // FLIP_DISCARD with kBackBufferCount buffers and a frame-latency waitable object. Display().Output picks the
    // back buffer: SDR_sRGB = R8G8B8A8_UNORM (sRGB RTV), HDR10_PQ = R10G10B10A2_UNORM in
    // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, scRGB_Linear = R16G16B16A16_FLOAT in G10_NONE_P709 — the HDR
    // two only when the window's output reports that colour space, else refused by name. Display().VSync off
    // presents with ALLOW_TEARING when the device supports it.
    class D3D12SwapChain final : public SwapChain
    {
    public:
        static constexpr uint32_t kBackBufferCount = 3;

        explicit D3D12SwapChain( const GLFWwindow* window ) : SwapChain( window )
        {
        }
        ~D3D12SwapChain() override;

        Common::ResultStr<bool> CreateSwapChain( const std::shared_ptr<Engine::Device>& device, uint32_t* width,
                                                 uint32_t* height ) override;
        uint32_t                GetBackBufferCount() const override
        {
            return kBackBufferCount;
        }
        uint32_t GetWidth() const override
        {
            return m_Width;
        }
        uint32_t GetHeight() const override
        {
            return m_Height;
        }
        void               RequestRebuild( uint32_t width, uint32_t height ) override;
        [[nodiscard]] bool RebuildPending() const override
        {
            return m_RebuildPending;
        }
        void Release() override;

        // A capture copies the back buffer inside the frame's recording, which part 2's RendererAPI owns.
        [[nodiscard]] bool SupportsFrameReadback() const noexcept override
        {
            return false;
        }
        [[nodiscard]] Common::BoolResultStr RecordFrameCapture() override;
        [[nodiscard]] Common::ResultStr<std::vector<uint8_t>>
        TakeCapturedFrameRGBA8( uint32_t& outWidth, uint32_t& outHeight ) override;

        // Resizes the buffers to the pending size. Every queue must be idle (the caller's frame boundary).
        [[nodiscard]] Common::BoolResultStr ApplyRebuild() noexcept;
        // Blocks on the frame-latency waitable object (UE: the viewport's frame pacing).
        [[nodiscard]] Common::BoolResultStr WaitForFrameLatency() noexcept;
        [[nodiscard]] uint32_t              CurrentBackBuffer() const noexcept;
        [[nodiscard]] ID3D12Resource*       BackBuffer( uint32_t index ) const noexcept
        {
            return m_Buffers[index].Get();
        }
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv( uint32_t index ) const noexcept
        {
            return m_Rtvs[index].Handle;
        }
        [[nodiscard]] DXGI_FORMAT BackBufferFormat() const noexcept
        {
            return m_Format;
        }
        [[nodiscard]] Common::BoolResultStr Present() noexcept;

    private:
        [[nodiscard]] Common::BoolResultStr AcquireBuffers() noexcept;
        void                                ReleaseBuffers() noexcept;
        [[nodiscard]] Common::BoolResultStr ApplyColorSpace() noexcept;

        std::shared_ptr<D3D12Device>                         m_Device;
        ComPtr<IDXGISwapChain4>                              m_SwapChain;
        HANDLE                                               m_LatencyWait = nullptr;
        std::array<ComPtr<ID3D12Resource>, kBackBufferCount> m_Buffers;
        std::array<CpuDescriptor, kBackBufferCount>          m_Rtvs;
        DXGI_FORMAT                                          m_Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT                                          m_RtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        DXGI_COLOR_SPACE_TYPE m_ColorSpace                               = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        uint32_t              m_Width                                    = 0;
        uint32_t              m_Height                                   = 0;
        uint32_t              m_PendingWidth                             = 0;
        uint32_t              m_PendingHeight                            = 0;
        bool                  m_RebuildPending                           = false;
        bool                  m_Tearing                                  = false;
    };

} // namespace Desert::Graphic::API::D3D12
