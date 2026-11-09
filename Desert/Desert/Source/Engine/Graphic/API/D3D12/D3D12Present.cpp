#include <Engine/Graphic/API/D3D12/D3D12Rhi.hpp>

#include <Common/Core/Logger.hpp>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace Desert::Graphic::API::D3D12
{
    // --- RDG ACCESS STATE -> ENHANCED BARRIER -------------------------------------------------------------------

    D3D12_BARRIER_SYNC ToBarrierSync( RDG::PipelineStageFlags stages ) noexcept
    {
        uint32_t sync = D3D12_BARRIER_SYNC_NONE;
        if ( stages & RDG::PipelineStage_DrawIndirect )
            sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
        if ( stages & RDG::PipelineStage_VertexInput )
            sync |= D3D12_BARRIER_SYNC_INDEX_INPUT | D3D12_BARRIER_SYNC_VERTEX_SHADING;
        if ( stages & RDG::PipelineStage_VertexShader )
            sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
        if ( stages & RDG::PipelineStage_FragmentShader )
            sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
        if ( stages & ( RDG::PipelineStage_EarlyFragmentTests | RDG::PipelineStage_LateFragmentTests ) )
            sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
        if ( stages & RDG::PipelineStage_ColorAttachmentOutput )
            sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
        if ( stages & RDG::PipelineStage_ComputeShader )
            sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        if ( stages & RDG::PipelineStage_Copy )
            sync |= D3D12_BARRIER_SYNC_COPY;
        if ( stages & RDG::PipelineStage_AccelStructBuild )
            sync |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
        // PipelineStage_Host has no GPU scope in D3D12: the host reads after the frame's fence, and the barrier
        // into it only has to finish the GPU writes (SyncAfter NONE).
        return static_cast<D3D12_BARRIER_SYNC>( sync );
    }

    D3D12_BARRIER_ACCESS ToBarrierAccess( RDG::MemoryAccessFlags memory, RDG::PipelineStageFlags stages ) noexcept
    {
        uint32_t access = D3D12_BARRIER_ACCESS_COMMON;
        if ( memory & RDG::MemoryAccess_IndirectCommandRead )
            access |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
        if ( memory & RDG::MemoryAccess_IndexRead )
            access |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
        if ( memory & RDG::MemoryAccess_VertexAttributeRead )
            access |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
        if ( memory & RDG::MemoryAccess_UniformRead )
            access |= D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
        if ( memory & RDG::MemoryAccess_ShaderSampledRead )
            access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        if ( memory & ( RDG::MemoryAccess_ShaderStorageRead | RDG::MemoryAccess_ShaderStorageWrite ) )
        {
            // An acceleration-structure build reads its vertex / index / instance inputs as shader resources.
            const bool buildInput = ( stages & RDG::PipelineStage_AccelStructBuild ) &&
                                    !( memory & RDG::MemoryAccess_ShaderStorageWrite );
            if ( buildInput )
                access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            else
                access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        }
        if ( memory & ( RDG::MemoryAccess_ColorAttachmentRead | RDG::MemoryAccess_ColorAttachmentWrite ) )
            access |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
        // DEPTH_STENCIL_WRITE already grants the read; D3D12 refuses the two combined.
        if ( memory & RDG::MemoryAccess_DepthStencilWrite )
            access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        else if ( memory & RDG::MemoryAccess_DepthStencilRead )
            access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
        if ( memory & RDG::MemoryAccess_TransferRead )
            access |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
        if ( memory & RDG::MemoryAccess_TransferWrite )
            access |= D3D12_BARRIER_ACCESS_COPY_DEST;
        if ( memory & RDG::MemoryAccess_AccelStructRead )
            access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
        if ( memory & RDG::MemoryAccess_AccelStructWrite )
            access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
        if ( access == D3D12_BARRIER_ACCESS_COMMON )
            return D3D12_BARRIER_ACCESS_NO_ACCESS;
        return static_cast<D3D12_BARRIER_ACCESS>( access );
    }

    D3D12_BARRIER_LAYOUT ToBarrierLayout( const RDG::AccessState& state, QueueKind queue ) noexcept
    {
        switch ( state.Layout )
        {
            case RDG::ImageLayout::Undefined:
                return D3D12_BARRIER_LAYOUT_UNDEFINED;
            case RDG::ImageLayout::General:
            {
                // Storage only: the UAV layout (compressible on some hardware). Storage mixed with copies
                // (Access::ExternalWrite): the queue's COMMON layout, the one that admits UAV and copy together.
                const RDG::MemoryAccessFlags storage =
                     RDG::MemoryAccess_ShaderStorageRead | RDG::MemoryAccess_ShaderStorageWrite;
                if ( ( state.Memory & ~storage ) == 0 )
                    return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                if ( queue == QueueKind::Compute )
                    return D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON;
                if ( queue == QueueKind::Graphics )
                    return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON;
                return D3D12_BARRIER_LAYOUT_COMMON;
            }
            case RDG::ImageLayout::ColorAttachment:
                return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            case RDG::ImageLayout::DepthStencilAttachment:
                return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
            case RDG::ImageLayout::DepthStencilReadOnly:
                return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
            case RDG::ImageLayout::ShaderReadOnly:
                // The queue-neutral layout: async compute samples what graphics wrote without a layout change.
                return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            case RDG::ImageLayout::TransferSrc:
                return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
            case RDG::ImageLayout::TransferDst:
                return D3D12_BARRIER_LAYOUT_COPY_DEST;
            case RDG::ImageLayout::Present:
                return D3D12_BARRIER_LAYOUT_PRESENT;
        }
        return D3D12_BARRIER_LAYOUT_COMMON;
    }

    BarrierState MapAccessState( const RDG::AccessState& state, bool texture, QueueKind queue ) noexcept
    {
        BarrierState out;
        out.Sync   = ToBarrierSync( state.Stages );
        out.Access = ToBarrierAccess( state.Memory, state.Stages );
        out.Layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
        if ( texture )
            out.Layout = ToBarrierLayout( state, queue );
        // A synchronisation scope with no access is legal; an access with no scope is not.
        if ( out.Sync == D3D12_BARRIER_SYNC_NONE )
            out.Access = D3D12_BARRIER_ACCESS_NO_ACCESS;
        return out;
    }

    namespace
    {
        D3D12_BARRIER_SUBRESOURCE_RANGE ToSubresources( const RDG::SubresourceRange& range,
                                                        const BarrierTarget&         target ) noexcept
        {
            D3D12_BARRIER_SUBRESOURCE_RANGE out{};
            const bool                      whole = range.BaseMip == 0 && range.BaseLayer == 0 &&
                               ( range.MipCount == RDG::kAllRemaining || range.MipCount >= target.MipLevels ) &&
                               ( range.LayerCount == RDG::kAllRemaining || range.LayerCount >= target.ArraySize );
            if ( whole )
            {
                // 0xFFFFFFFF with NumMipLevels 0: every subresource of the resource.
                out.IndexOrFirstMipLevel = 0xFFFFFFFFu;
                return out;
            }
            uint32_t mips = range.MipCount;
            if ( mips == RDG::kAllRemaining )
                mips = target.MipLevels - range.BaseMip;
            uint32_t layers = range.LayerCount;
            if ( layers == RDG::kAllRemaining )
                layers = target.ArraySize - range.BaseLayer;
            out.IndexOrFirstMipLevel = range.BaseMip;
            out.NumMipLevels         = mips;
            out.FirstArraySlice      = range.BaseLayer;
            out.NumArraySlices       = layers;
            out.FirstPlane           = 0;
            out.NumPlanes            = target.Planes;
            return out;
        }
    } // namespace

    void D3D12BarrierBatch::Add( ID3D12GraphicsCommandList7& list, const RDG::Barrier& barrier,
                                 const BarrierTarget& target ) noexcept
    {
        const bool   texture = barrier.Kind == RDG::ResourceKind::Texture;
        BarrierState before  = MapAccessState( barrier.Before, texture, m_Queue );
        BarrierState after   = MapAccessState( barrier.After, texture, m_Queue );
        bool         discard = barrier.DiscardContents;

        switch ( barrier.BarrierType )
        {
            case RDG::BarrierKind::Transition:
                break;
            case RDG::BarrierKind::AliasAcquire:
                // The aliased memory's previous tenant is not this resource: nothing to wait on, nothing to keep.
                before  = BarrierState{};
                discard = true;
                break;
            case RDG::BarrierKind::OwnershipRelease:
                // Finish this queue's work and leave the texture in COMMON, which the other queue may access.
                after = BarrierState{};
                if ( texture )
                    after.Layout = D3D12_BARRIER_LAYOUT_COMMON;
                break;
            case RDG::BarrierKind::OwnershipAcquire:
                before = BarrierState{};
                if ( texture )
                    before.Layout = D3D12_BARRIER_LAYOUT_COMMON;
                break;
        }
        if ( discard && texture )
            before.Layout = D3D12_BARRIER_LAYOUT_UNDEFINED;

        if ( texture )
        {
            if ( m_TextureCount == kCapacity )
                Flush( list );
            D3D12_TEXTURE_BARRIER& out = m_Textures[m_TextureCount++];
            out.SyncBefore             = before.Sync;
            out.SyncAfter              = after.Sync;
            out.AccessBefore           = before.Access;
            out.AccessAfter            = after.Access;
            out.LayoutBefore           = before.Layout;
            out.LayoutAfter            = after.Layout;
            out.pResource              = target.Resource;
            out.Subresources           = ToSubresources( barrier.Range, target );
            out.Flags                  = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            if ( discard )
                out.Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
            return;
        }
        if ( m_BufferCount == kCapacity )
            Flush( list );
        D3D12_BUFFER_BARRIER& out = m_Buffers[m_BufferCount++];
        out.SyncBefore            = before.Sync;
        out.SyncAfter             = after.Sync;
        out.AccessBefore          = before.Access;
        out.AccessAfter           = after.Access;
        out.pResource             = target.Resource;
        out.Offset                = 0;
        out.Size                  = UINT64_MAX; // buffer barriers cover the whole resource
    }

    void D3D12BarrierBatch::Flush( ID3D12GraphicsCommandList7& list ) noexcept
    {
        std::array<D3D12_BARRIER_GROUP, 2> groups{};
        uint32_t                           count = 0;
        if ( m_TextureCount > 0 )
        {
            groups[count].Type             = D3D12_BARRIER_TYPE_TEXTURE;
            groups[count].NumBarriers      = m_TextureCount;
            groups[count].pTextureBarriers = m_Textures.data();
            ++count;
        }
        if ( m_BufferCount > 0 )
        {
            groups[count].Type            = D3D12_BARRIER_TYPE_BUFFER;
            groups[count].NumBarriers     = m_BufferCount;
            groups[count].pBufferBarriers = m_Buffers.data();
            ++count;
        }
        if ( count > 0 )
            list.Barrier( count, groups.data() );
        m_TextureCount = 0;
        m_BufferCount  = 0;
    }

    // --- SWAP CHAIN
    // -----------------------------------------------------------------------------------------------

    D3D12SwapChain::~D3D12SwapChain()
    {
        Release();
    }

    Common::ResultStr<bool> D3D12SwapChain::CreateSwapChain( const std::shared_ptr<Engine::Device>& device,
                                                             uint32_t* width, uint32_t* height )
    {
        m_Device = std::dynamic_pointer_cast<D3D12Device>( device );
        if ( !m_Device )
            return Common::MakeError<bool>( "D3D12SwapChain::CreateSwapChain: the device is not a D3D12Device" );
        HWND window = glfwGetWin32Window( const_cast<GLFWwindow*>( m_Window ) );
        if ( !window )
            return Common::MakeError<bool>( "D3D12SwapChain::CreateSwapChain: the GLFW window has no HWND" );

        const Common::Scalability::DisplayOutput output = Display().Output;
        m_Format                                        = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_RtvFormat                                     = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        m_ColorSpace                                    = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if ( output == Common::Scalability::DisplayOutput::HDR10_PQ )
        {
            m_Format     = DXGI_FORMAT_R10G10B10A2_UNORM;
            m_RtvFormat  = DXGI_FORMAT_R10G10B10A2_UNORM;
            m_ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
        else if ( output == Common::Scalability::DisplayOutput::scRGB_Linear )
        {
            m_Format     = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_RtvFormat  = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }
        m_Tearing = m_Device->SupportsTearing() && !Display().VSync;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width            = *width;
        desc.Height           = *height;
        desc.Format           = m_Format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount      = kBackBufferCount;
        desc.Scaling          = DXGI_SCALING_STRETCH;
        desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if ( m_Tearing )
            desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = m_Device->Factory().CreateSwapChainForHwnd( m_Device->Queue( QueueKind::Graphics ).Native(),
                                                                 window, &desc, nullptr, nullptr, &swapChain1 );
        if ( FAILED( hr ) )
            return Common::MakeError<bool>( DescribeHResult( "CreateSwapChainForHwnd", hr ) );
        hr = swapChain1.As( &m_SwapChain );
        if ( FAILED( hr ) )
            return Common::MakeError<bool>(
                 DescribeHResult( "IDXGISwapChain1::QueryInterface(IDXGISwapChain4)", hr ) );
        // GLFW owns the window's fullscreen state; DXGI's Alt+Enter would fight it.
        m_Device->Factory().MakeWindowAssociation( window, DXGI_MWA_NO_ALT_ENTER );
        m_SwapChain->SetMaximumFrameLatency( kBackBufferCount - 1 );
        m_LatencyWait = m_SwapChain->GetFrameLatencyWaitableObject();

        auto colorSpace = ApplyColorSpace();
        if ( !colorSpace )
            return Common::MakeError<bool>( colorSpace.GetError() );
        m_Width      = *width;
        m_Height     = *height;
        auto buffers = AcquireBuffers();
        if ( !buffers )
            return Common::MakeError<bool>( buffers.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr D3D12SwapChain::ApplyColorSpace() noexcept
    {
        UINT          support = 0;
        const HRESULT hr      = m_SwapChain->CheckColorSpaceSupport( m_ColorSpace, &support );
        if ( FAILED( hr ) || !( support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT ) )
            return Common::MakeError( "the window's output cannot present colour space " +
                                      std::to_string( static_cast<int>( m_ColorSpace ) ) +
                                      " (Display.Output asks for HDR on an SDR output)" );
        const HRESULT set = m_SwapChain->SetColorSpace1( m_ColorSpace );
        if ( FAILED( set ) )
            return Common::MakeError( DescribeHResult( "IDXGISwapChain3::SetColorSpace1", set ) );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr D3D12SwapChain::AcquireBuffers() noexcept
    {
        D3D12CpuDescriptorHeap& rtvHeap = m_Device->StagingHeap( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
        for ( uint32_t i = 0; i < kBackBufferCount; ++i )
        {
            const HRESULT hr = m_SwapChain->GetBuffer( i, IID_PPV_ARGS( &m_Buffers[i] ) );
            if ( FAILED( hr ) )
                return Common::MakeError( DescribeHResult( "IDXGISwapChain::GetBuffer", hr ) );
            const std::wstring name = L"D3D12 back buffer " + std::to_wstring( i );
            m_Buffers[i]->SetName( name.c_str() );
            if ( !m_Rtvs[i].IsValid() )
            {
                auto rtv = rtvHeap.Allocate();
                if ( !rtv )
                    return Common::MakeError( rtv.GetError() );
                m_Rtvs[i] = rtv.GetValue();
            }
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format        = m_RtvFormat;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            m_Device->Native().CreateRenderTargetView( m_Buffers[i].Get(), &view, m_Rtvs[i].Handle );
        }
        return BOOLSUCCESS;
    }

    void D3D12SwapChain::ReleaseBuffers() noexcept
    {
        for ( ComPtr<ID3D12Resource>& buffer : m_Buffers )
            buffer.Reset();
    }

    void D3D12SwapChain::RequestRebuild( uint32_t width, uint32_t height )
    {
        m_PendingWidth   = width;
        m_PendingHeight  = height;
        m_RebuildPending = true;
    }

    Common::BoolResultStr D3D12SwapChain::ApplyRebuild() noexcept
    {
        if ( !m_RebuildPending )
            return BOOLSUCCESS;
        m_RebuildPending = false;
        // A minimised window reports 0x0: keep the buffers, the next non-zero size rebuilds.
        if ( m_PendingWidth == 0 || m_PendingHeight == 0 )
            return BOOLSUCCESS;
        ReleaseBuffers();
        UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if ( m_Tearing )
            flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        const HRESULT hr =
             m_SwapChain->ResizeBuffers( kBackBufferCount, m_PendingWidth, m_PendingHeight, m_Format, flags );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "IDXGISwapChain::ResizeBuffers", hr ) );
        m_Width         = m_PendingWidth;
        m_Height        = m_PendingHeight;
        auto colorSpace = ApplyColorSpace();
        if ( !colorSpace )
            return colorSpace;
        return AcquireBuffers();
    }

    Common::BoolResultStr D3D12SwapChain::WaitForFrameLatency() noexcept
    {
        if ( !m_LatencyWait )
            return BOOLSUCCESS;
        if ( WaitForSingleObjectEx( m_LatencyWait, 1000, TRUE ) == WAIT_FAILED )
            return Common::MakeError( "waiting on the swap chain's frame-latency object failed" );
        return BOOLSUCCESS;
    }

    uint32_t D3D12SwapChain::CurrentBackBuffer() const noexcept
    {
        return m_SwapChain->GetCurrentBackBufferIndex();
    }

    Common::BoolResultStr D3D12SwapChain::Present() noexcept
    {
        UINT sync  = 0;
        UINT flags = 0;
        if ( Display().VSync )
            sync = 1;
        else if ( m_Tearing )
            flags = DXGI_PRESENT_ALLOW_TEARING;
        const HRESULT hr = m_SwapChain->Present( sync, flags );
        if ( FAILED( hr ) )
        {
            std::string error = DescribeHResult( "IDXGISwapChain::Present", hr );
            if ( hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET )
            {
                const HRESULT reason = m_Device->Native().GetDeviceRemovedReason();
                error += "; " + DescribeHResult( "removed reason", reason );
            }
            return Common::MakeError( error );
        }
        return BOOLSUCCESS;
    }

    void D3D12SwapChain::Release()
    {
        if ( !m_SwapChain )
            return;
        if ( m_Device )
            m_Device->WaitIdle();
        ReleaseBuffers();
        if ( m_Device )
        {
            D3D12CpuDescriptorHeap& rtvHeap = m_Device->StagingHeap( D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
            for ( CpuDescriptor& rtv : m_Rtvs )
            {
                rtvHeap.Free( rtv );
                rtv = CpuDescriptor{};
            }
        }
        if ( m_LatencyWait )
        {
            CloseHandle( m_LatencyWait );
            m_LatencyWait = nullptr;
        }
        m_SwapChain.Reset();
    }

    Common::BoolResultStr D3D12SwapChain::RecordFrameCapture()
    {
        return Common::MakeError( "D3D12SwapChain: frame capture is recorded by the D3D12 RendererAPI, which this "
                                  "build does not have (SupportsFrameReadback is false)" );
    }

    Common::ResultStr<std::vector<uint8_t>> D3D12SwapChain::TakeCapturedFrameRGBA8( uint32_t& outWidth,
                                                                                    uint32_t& outHeight )
    {
        outWidth  = 0;
        outHeight = 0;
        return Common::MakeError<std::vector<uint8_t>>(
             "D3D12SwapChain: no frame was captured (SupportsFrameReadback is false)" );
    }

} // namespace Desert::Graphic::API::D3D12
