#include <Engine/Graphic/API/D3D12/D3D12Rhi.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Settings/MachineSettings.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment( lib, "d3d12.lib" )
#pragma comment( lib, "dxgi.lib" )
#pragma comment( lib, "dxguid.lib" )

namespace Desert::Graphic::API::D3D12
{
    namespace
    {
        std::string_view HResultName( HRESULT hr ) noexcept
        {
            switch ( hr )
            {
                case DXGI_ERROR_DEVICE_REMOVED:
                    return "DXGI_ERROR_DEVICE_REMOVED";
                case DXGI_ERROR_DEVICE_HUNG:
                    return "DXGI_ERROR_DEVICE_HUNG";
                case DXGI_ERROR_DEVICE_RESET:
                    return "DXGI_ERROR_DEVICE_RESET";
                case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                    return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
                case DXGI_ERROR_UNSUPPORTED:
                    return "DXGI_ERROR_UNSUPPORTED";
                case DXGI_ERROR_NOT_FOUND:
                    return "DXGI_ERROR_NOT_FOUND";
                case E_OUTOFMEMORY:
                    return "E_OUTOFMEMORY";
                case E_INVALIDARG:
                    return "E_INVALIDARG";
                case E_NOINTERFACE:
                    return "E_NOINTERFACE";
                default:
                    return "";
            }
        }

        std::wstring WidenUtf8( std::string_view text )
        {
            if ( text.empty() )
                return {};
            const int count =
                 MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
            std::wstring wide( static_cast<size_t>( std::max( count, 0 ) ), L'\0' );
            if ( count > 0 )
                MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), wide.data(),
                                     count );
            return wide;
        }

        void NameObject( ID3D12Object* object, std::string_view name )
        {
            if ( object && !name.empty() )
                object->SetName( WidenUtf8( name ).c_str() );
        }

        std::string_view QueueName( QueueKind kind ) noexcept
        {
            switch ( kind )
            {
                case QueueKind::Graphics:
                    return "Graphics";
                case QueueKind::Compute:
                    return "Compute";
                case QueueKind::Copy:
                    return "Copy";
                case QueueKind::Count:
                    break;
            }
            return "<not a queue>";
        }

        // D3D12 limits that are constants of the API rather than queries (d3d12.h D3D12_REQ_* / D3D12_CS_*).
        constexpr uint64_t kMaxRawBufferBytes        = 1ull << 31; // a raw SRV/UAV addresses 2^31 bytes
        constexpr uint32_t kComputeSharedMemoryBytes = D3D12_CS_TGSM_REGISTER_COUNT * 4u;
        constexpr uint32_t kStagingCbvSrvUavPerPage  = 4096;
        constexpr uint32_t kStagingSmallPerPage      = 256;
    } // namespace

    std::string DescribeHResult( std::string_view what, HRESULT hr )
    {
        char code[16] = {};
        std::snprintf( code, sizeof( code ), "0x%08X", static_cast<unsigned>( hr ) );
        std::string            text = std::string( what ) + " failed: HRESULT " + code;
        const std::string_view name = HResultName( hr );
        if ( !name.empty() )
            text += " (" + std::string( name ) + ")";
        return text;
    }

    std::string NarrowUtf16( const wchar_t* text )
    {
        if ( !text || !*text )
            return {};
        const int count = WideCharToMultiByte( CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr );
        if ( count <= 1 )
            return {};
        std::string narrow( static_cast<size_t>( count - 1 ), '\0' );
        WideCharToMultiByte( CP_UTF8, 0, text, -1, narrow.data(), count, nullptr, nullptr );
        return narrow;
    }

    D3D12_COMMAND_LIST_TYPE ToCommandListType( QueueKind kind ) noexcept
    {
        switch ( kind )
        {
            case QueueKind::Graphics:
                return D3D12_COMMAND_LIST_TYPE_DIRECT;
            case QueueKind::Compute:
                return D3D12_COMMAND_LIST_TYPE_COMPUTE;
            case QueueKind::Copy:
                return D3D12_COMMAND_LIST_TYPE_COPY;
            case QueueKind::Count:
                break;
        }
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    QueueKind QueueForPipe( RDG::Pipe pipe ) noexcept
    {
        if ( pipe == RDG::Pipe::AsyncCompute )
            return QueueKind::Compute;
        return QueueKind::Graphics;
    }

    // --- ADAPTER ------------------------------------------------------------------------------------------------

    Common::ResultStr<ComPtr<IDXGIFactory6>> CreateDxgiFactory( bool debugLayer ) noexcept
    {
        ComPtr<IDXGIFactory6> factory;
        UINT                  flags = 0;
        if ( debugLayer )
            flags |= DXGI_CREATE_FACTORY_DEBUG;
        HRESULT hr = CreateDXGIFactory2( flags, IID_PPV_ARGS( &factory ) );
        if ( FAILED( hr ) && debugLayer )
        {
            // The DXGI debug layer ships with the Graphics Tools optional feature; a machine without it still
            // runs.
            LOG_WARN( "[D3D12] DXGI debug factory unavailable ({}); continuing without it",
                      DescribeHResult( "CreateDXGIFactory2(DEBUG)", hr ) );
            hr = CreateDXGIFactory2( 0, IID_PPV_ARGS( &factory ) );
        }
        if ( FAILED( hr ) )
            return Common::MakeError<ComPtr<IDXGIFactory6>>(
                 DescribeHResult( "CreateDXGIFactory2 (IDXGIFactory6, Windows 10 1803+)", hr ) );
        return Common::MakeSuccess( std::move( factory ) );
    }

    Common::ResultStr<AdapterChoice> SelectAdapter( IDXGIFactory6& factory ) noexcept
    {
        std::string refused;
        for ( UINT index = 0;; ++index )
        {
            ComPtr<IDXGIAdapter4> adapter;
            const HRESULT hr = factory.EnumAdapterByGpuPreference( index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                   IID_PPV_ARGS( &adapter ) );
            if ( hr == DXGI_ERROR_NOT_FOUND )
                break;
            if ( FAILED( hr ) )
                return Common::MakeError<AdapterChoice>( DescribeHResult( "EnumAdapterByGpuPreference", hr ) );

            AdapterChoice choice;
            if ( FAILED( adapter->GetDesc3( &choice.Desc ) ) )
                continue;
            const std::string name = NarrowUtf16( choice.Desc.Description );
            if ( choice.Desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE )
            {
                refused += " [" + name + ": software adapter]";
                continue;
            }
            // Probe with a null device pointer: answers whether the device would be created, creates nothing.
            const HRESULT probe =
                 D3D12CreateDevice( adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof( ID3D12Device ), nullptr );
            if ( FAILED( probe ) )
            {
                refused += " [" + name + ": " + DescribeHResult( "D3D12CreateDevice(12_0)", probe ) + "]";
                continue;
            }
            choice.Adapter      = std::move( adapter );
            choice.FeatureLevel = D3D_FEATURE_LEVEL_12_0;
            return Common::MakeSuccess( std::move( choice ) );
        }
        return Common::MakeError<AdapterChoice>( "no hardware adapter creates a feature level 12_0 D3D12 device:" +
                                                 refused );
    }

    // --- QUEUE
    // ----------------------------------------------------------------------------------------------------

    D3D12Queue::~D3D12Queue()
    {
        Release();
    }

    Common::BoolResultStr D3D12Queue::Init( ID3D12Device& device, QueueKind kind ) noexcept
    {
        m_Kind = kind;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type     = ToCommandListType( kind );
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        HRESULT hr    = device.CreateCommandQueue( &desc, IID_PPV_ARGS( &m_Queue ) );
        if ( FAILED( hr ) )
            return Common::MakeError(
                 DescribeHResult( "CreateCommandQueue(" + std::string( QueueName( kind ) ) + ")", hr ) );
        hr = device.CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( &m_Fence ) );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "CreateFence", hr ) );
        m_Event = CreateEventExW( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
        if ( !m_Event )
            return Common::MakeError( "CreateEventExW for the " + std::string( QueueName( kind ) ) +
                                      " queue fence failed" );
        const std::string name = "D3D12 " + std::string( QueueName( kind ) ) + " queue";
        NameObject( m_Queue.Get(), name );
        NameObject( m_Fence.Get(), name + " fence" );
        return BOOLSUCCESS;
    }

    void D3D12Queue::Release() noexcept
    {
        if ( m_Event )
        {
            CloseHandle( m_Event );
            m_Event = nullptr;
        }
        m_Fence.Reset();
        m_Queue.Reset();
    }

    Common::ResultStr<uint64_t> D3D12Queue::Submit( std::span<ID3D12CommandList* const> lists ) noexcept
    {
        std::lock_guard lock( m_SubmitMutex );
        if ( !lists.empty() )
            m_Queue->ExecuteCommandLists( static_cast<UINT>( lists.size() ), lists.data() );
        const uint64_t value = m_NextValue++;
        const HRESULT  hr    = m_Queue->Signal( m_Fence.Get(), value );
        if ( FAILED( hr ) )
            return Common::MakeError<uint64_t>( DescribeHResult( "ID3D12CommandQueue::Signal", hr ) );
        m_LastSignalled.store( value, std::memory_order_release );
        return Common::MakeSuccess( value );
    }

    Common::ResultStr<uint64_t> D3D12Queue::Signal() noexcept
    {
        return Submit( {} );
    }

    Common::BoolResultStr D3D12Queue::WaitOnQueue( const D3D12Queue& other, uint64_t value ) noexcept
    {
        const HRESULT hr = m_Queue->Wait( other.Fence(), value );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "ID3D12CommandQueue::Wait", hr ) );
        return BOOLSUCCESS;
    }

    bool D3D12Queue::IsComplete( uint64_t value ) const noexcept
    {
        return CompletedValue() >= value;
    }

    uint64_t D3D12Queue::CompletedValue() const noexcept
    {
        return m_Fence ? m_Fence->GetCompletedValue() : 0;
    }

    Common::BoolResultStr D3D12Queue::WaitCpu( uint64_t value ) noexcept
    {
        const uint64_t completed = CompletedValue();
        // A removed device reports UINT64_MAX on every fence: "complete", and the caller must hear why.
        if ( completed == UINT64_MAX )
            return Common::MakeError( "the D3D12 device was removed (fence of the " +
                                      std::string( QueueName( m_Kind ) ) + " queue reads UINT64_MAX)" );
        if ( completed >= value )
            return BOOLSUCCESS;
        std::lock_guard lock( m_WaitMutex );
        const HRESULT   hr = m_Fence->SetEventOnCompletion( value, m_Event );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "ID3D12Fence::SetEventOnCompletion", hr ) );
        if ( WaitForSingleObject( m_Event, INFINITE ) != WAIT_OBJECT_0 )
            return Common::MakeError( "WaitForSingleObject on the " + std::string( QueueName( m_Kind ) ) +
                                      " queue fence failed" );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr D3D12Queue::Flush() noexcept
    {
        auto value = Signal();
        if ( !value )
            return Common::MakeError( value.GetError() );
        return WaitCpu( value.GetValue() );
    }

    // --- COMMAND POOL
    // ---------------------------------------------------------------------------------------------

    Common::BoolResultStr D3D12CommandPool::Init( ID3D12Device10& device, QueueKind kind ) noexcept
    {
        m_Device = &device;
        m_Kind   = kind;
        return BOOLSUCCESS;
    }

    void D3D12CommandPool::Release() noexcept
    {
        std::lock_guard lock( m_Mutex );
        m_Entries.clear();
    }

    Common::ResultStr<CommandContext> D3D12CommandPool::Acquire( const D3D12Queue& queue ) noexcept
    {
        std::lock_guard lock( m_Mutex );
        const uint64_t  completed = queue.CompletedValue();
        for ( uint32_t i = 0; i < static_cast<uint32_t>( m_Entries.size() ); ++i )
        {
            Entry& entry = m_Entries[i];
            if ( entry.InUse || entry.RetireValue > completed )
                continue;
            HRESULT hr = entry.Allocator->Reset();
            if ( FAILED( hr ) )
                return Common::MakeError<CommandContext>( DescribeHResult( "ID3D12CommandAllocator::Reset", hr ) );
            hr = entry.List->Reset( entry.Allocator.Get(), nullptr );
            if ( FAILED( hr ) )
                return Common::MakeError<CommandContext>(
                     DescribeHResult( "ID3D12GraphicsCommandList::Reset", hr ) );
            entry.InUse = true;
            return Common::MakeSuccess( CommandContext{ entry.Allocator.Get(), entry.List.Get(), i } );
        }

        const D3D12_COMMAND_LIST_TYPE type = ToCommandListType( m_Kind );
        Entry                         entry;
        HRESULT hr = m_Device->CreateCommandAllocator( type, IID_PPV_ARGS( &entry.Allocator ) );
        if ( FAILED( hr ) )
            return Common::MakeError<CommandContext>( DescribeHResult( "CreateCommandAllocator", hr ) );
        // CreateCommandList1 creates the list CLOSED and without an allocator; Reset opens it on ours.
        hr = m_Device->CreateCommandList1( 0, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS( &entry.List ) );
        if ( FAILED( hr ) )
            return Common::MakeError<CommandContext>(
                 DescribeHResult( "CreateCommandList1 (ID3D12GraphicsCommandList7)", hr ) );
        hr = entry.List->Reset( entry.Allocator.Get(), nullptr );
        if ( FAILED( hr ) )
            return Common::MakeError<CommandContext>( DescribeHResult( "ID3D12GraphicsCommandList::Reset", hr ) );
        const std::string name =
             "D3D12 " + std::string( QueueName( m_Kind ) ) + " list " + std::to_string( m_Entries.size() );
        NameObject( entry.Allocator.Get(), name + " allocator" );
        NameObject( entry.List.Get(), name );
        entry.InUse = true;
        m_Entries.push_back( std::move( entry ) );
        const uint32_t index = static_cast<uint32_t>( m_Entries.size() - 1 );
        return Common::MakeSuccess(
             CommandContext{ m_Entries[index].Allocator.Get(), m_Entries[index].List.Get(), index } );
    }

    void D3D12CommandPool::Recycle( const CommandContext& context, uint64_t retireValue ) noexcept
    {
        std::lock_guard lock( m_Mutex );
        if ( context.Entry >= m_Entries.size() )
            return;
        Entry& entry      = m_Entries[context.Entry];
        entry.RetireValue = retireValue;
        entry.InUse       = false;
    }

    // --- CPU STAGING DESCRIPTORS
    // ----------------------------------------------------------------------------------

    Common::BoolResultStr D3D12CpuDescriptorHeap::Init( ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                        uint32_t descriptorsPerPage ) noexcept
    {
        m_Device    = &device;
        m_Type      = type;
        m_PerPage   = descriptorsPerPage;
        m_Increment = device.GetDescriptorHandleIncrementSize( type );
        std::lock_guard lock( m_Mutex );
        return AddPage();
    }

    void D3D12CpuDescriptorHeap::Release() noexcept
    {
        std::lock_guard lock( m_Mutex );
        m_Pages.clear();
    }

    Common::BoolResultStr D3D12CpuDescriptorHeap::AddPage() noexcept
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = m_Type;
        desc.NumDescriptors = m_PerPage;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Page          page;
        const HRESULT hr = m_Device->CreateDescriptorHeap( &desc, IID_PPV_ARGS( &page.Heap ) );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "CreateDescriptorHeap (CPU staging)", hr ) );
        page.Start = page.Heap->GetCPUDescriptorHandleForHeapStart();
        page.FreeList.reserve( m_PerPage );
        for ( uint32_t i = m_PerPage; i > 0; --i )
            page.FreeList.push_back( i - 1 );
        m_Pages.push_back( std::move( page ) );
        return BOOLSUCCESS;
    }

    Common::ResultStr<CpuDescriptor> D3D12CpuDescriptorHeap::Allocate() noexcept
    {
        std::lock_guard lock( m_Mutex );
        for ( uint32_t p = 0; p < static_cast<uint32_t>( m_Pages.size() ); ++p )
        {
            Page& page = m_Pages[p];
            if ( page.FreeList.empty() )
                continue;
            const uint32_t index = page.FreeList.back();
            page.FreeList.pop_back();
            CpuDescriptor descriptor;
            descriptor.Handle.ptr = page.Start.ptr + static_cast<SIZE_T>( index ) * m_Increment;
            descriptor.Page       = p;
            descriptor.Index      = index;
            return Common::MakeSuccess( descriptor );
        }
        auto added = AddPage();
        if ( !added )
            return Common::MakeError<CpuDescriptor>( added.GetError() );
        Page&          page  = m_Pages.back();
        const uint32_t index = page.FreeList.back();
        page.FreeList.pop_back();
        CpuDescriptor descriptor;
        descriptor.Handle.ptr = page.Start.ptr + static_cast<SIZE_T>( index ) * m_Increment;
        descriptor.Page       = static_cast<uint32_t>( m_Pages.size() - 1 );
        descriptor.Index      = index;
        return Common::MakeSuccess( descriptor );
    }

    void D3D12CpuDescriptorHeap::Free( const CpuDescriptor& descriptor ) noexcept
    {
        if ( !descriptor.IsValid() )
            return;
        std::lock_guard lock( m_Mutex );
        if ( descriptor.Page < m_Pages.size() )
            m_Pages[descriptor.Page].FreeList.push_back( descriptor.Index );
    }

    // --- BINDLESS (SHADER-VISIBLE) DESCRIPTORS
    // --------------------------------------------------------------------

    Common::BoolResultStr D3D12BindlessHeap::Init( ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                   uint32_t capacity ) noexcept
    {
        m_Device    = &device;
        m_Type      = type;
        m_Capacity  = capacity;
        m_Increment = device.GetDescriptorHandleIncrementSize( type );
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = type;
        desc.NumDescriptors = capacity;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT hr    = device.CreateDescriptorHeap( &desc, IID_PPV_ARGS( &m_Heap ) );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "CreateDescriptorHeap (shader-visible bindless, " +
                                                            std::to_string( capacity ) + " descriptors)",
                                                       hr ) );
        m_CpuStart = m_Heap->GetCPUDescriptorHandleForHeapStart();
        m_GpuStart = m_Heap->GetGPUDescriptorHandleForHeapStart();
        if ( type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER )
            NameObject( m_Heap.Get(), "D3D12 bindless samplers" );
        else
            NameObject( m_Heap.Get(), "D3D12 bindless resources" );
        return BOOLSUCCESS;
    }

    void D3D12BindlessHeap::Release() noexcept
    {
        std::lock_guard lock( m_Mutex );
        m_Heap.Reset();
        m_Free.clear();
        m_Pending.clear();
        m_HighWater = 0;
    }

    Common::ResultStr<uint32_t> D3D12BindlessHeap::Allocate() noexcept
    {
        std::lock_guard lock( m_Mutex );
        if ( !m_Free.empty() )
        {
            const uint32_t slot = m_Free.back();
            m_Free.pop_back();
            return Common::MakeSuccess( slot );
        }
        if ( m_HighWater >= m_Capacity )
            return Common::MakeError<uint32_t>(
                 "the D3D12 bindless heap is full (" + std::to_string( m_Capacity ) + " descriptors, " +
                 std::to_string( m_Pending.size() ) + " waiting for their frame to retire)" );
        return Common::MakeSuccess( m_HighWater++ );
    }

    void D3D12BindlessHeap::FreeAfter( uint32_t slot, uint64_t graphicsFenceValue ) noexcept
    {
        std::lock_guard lock( m_Mutex );
        m_Pending.push_back( Pending{ slot, graphicsFenceValue } );
    }

    void D3D12BindlessHeap::Retire( uint64_t completedGraphicsFenceValue ) noexcept
    {
        std::lock_guard lock( m_Mutex );
        size_t          retired = 0;
        while ( retired < m_Pending.size() && m_Pending[retired].Fence <= completedGraphicsFenceValue )
        {
            m_Free.push_back( m_Pending[retired].Slot );
            ++retired;
        }
        if ( retired > 0 )
            m_Pending.erase( m_Pending.begin(), m_Pending.begin() + static_cast<std::ptrdiff_t>( retired ) );
    }

    void D3D12BindlessHeap::Write( uint32_t slot, D3D12_CPU_DESCRIPTOR_HANDLE source ) noexcept
    {
        m_Device->CopyDescriptorsSimple( 1, CpuHandle( slot ), source, m_Type );
    }

    D3D12_GPU_DESCRIPTOR_HANDLE D3D12BindlessHeap::GpuHandle( uint32_t slot ) const noexcept
    {
        return D3D12_GPU_DESCRIPTOR_HANDLE{ m_GpuStart.ptr + static_cast<UINT64>( slot ) * m_Increment };
    }

    D3D12_CPU_DESCRIPTOR_HANDLE D3D12BindlessHeap::CpuHandle( uint32_t slot ) const noexcept
    {
        return D3D12_CPU_DESCRIPTOR_HANDLE{ m_CpuStart.ptr + static_cast<SIZE_T>( slot ) * m_Increment };
    }

    // --- FORMATS AND RESOURCES
    // ------------------------------------------------------------------------------------

    DXGI_FORMAT ToDxgiFormat( Core::Formats::ImageFormat format, bool srgb ) noexcept
    {
        using Core::Formats::ImageFormat;
        switch ( format )
        {
            case ImageFormat::RGBA8F:
                if ( srgb )
                    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case ImageFormat::RGBA16F:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case ImageFormat::RGBA32F:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case ImageFormat::BGRA8F:
                if ( srgb )
                    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            case ImageFormat::DEPTH24STENCIL8:
                return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case ImageFormat::DEPTH32F:
                return DXGI_FORMAT_D32_FLOAT;
            case ImageFormat::BC7_UNORM:
                if ( srgb )
                    return DXGI_FORMAT_BC7_UNORM_SRGB;
                return DXGI_FORMAT_BC7_UNORM;
            case ImageFormat::BC6H_UFLOAT:
                return DXGI_FORMAT_BC6H_UF16;
            case ImageFormat::BC4_UNORM:
                return DXGI_FORMAT_BC4_UNORM;
            case ImageFormat::BC5_UNORM:
                return DXGI_FORMAT_BC5_UNORM;
            case ImageFormat::R16_UNORM:
                return DXGI_FORMAT_R16_UNORM;
            case ImageFormat::R32F:
                return DXGI_FORMAT_R32_FLOAT;
            case ImageFormat::R8_UNORM:
                return DXGI_FORMAT_R8_UNORM;
            case ImageFormat::RG16F:
                return DXGI_FORMAT_R16G16_FLOAT;
            case ImageFormat::R32_UINT:
                return DXGI_FORMAT_R32_UINT;
            case ImageFormat::RG8_UNORM:
                return DXGI_FORMAT_R8G8_UNORM;
            case ImageFormat::RG32_UINT:
                return DXGI_FORMAT_R32G32_UINT;
            case ImageFormat::R11G11B10F:
                return DXGI_FORMAT_R11G11B10_FLOAT;
            case ImageFormat::Count:
                break;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    bool IsDepthFormat( Core::Formats::ImageFormat format ) noexcept
    {
        return format == Core::Formats::ImageFormat::DEPTH24STENCIL8 ||
               format == Core::Formats::ImageFormat::DEPTH32F;
    }

    uint32_t PlaneCount( Core::Formats::ImageFormat format ) noexcept
    {
        if ( format == Core::Formats::ImageFormat::DEPTH24STENCIL8 )
            return 2;
        return 1;
    }

    namespace
    {
        // A depth texture is created TYPELESS so one resource carries both its DSV and its SRV (UE does the same).
        DXGI_FORMAT ResourceFormat( const TextureDesc& desc ) noexcept
        {
            if ( desc.Format == Core::Formats::ImageFormat::DEPTH24STENCIL8 )
                return DXGI_FORMAT_R24G8_TYPELESS;
            if ( desc.Format == Core::Formats::ImageFormat::DEPTH32F )
                return DXGI_FORMAT_R32_TYPELESS;
            return ToDxgiFormat( desc.Format, desc.Srgb );
        }

        Common::ResultStr<D3D12Resource> FinishBuffer( ComPtr<ID3D12Resource> resource, const BufferDesc& desc )
        {
            D3D12Resource out;
            out.Resource   = std::move( resource );
            out.GpuAddress = out.Resource->GetGPUVirtualAddress();
            if ( desc.Heap == D3D12_HEAP_TYPE_UPLOAD || desc.Heap == D3D12_HEAP_TYPE_READBACK )
            {
                // An upload buffer is never read by the CPU: an empty read range says so to the driver.
                D3D12_RANGE        nothingRead{ 0, 0 };
                const D3D12_RANGE* readRange = nullptr;
                if ( desc.Heap == D3D12_HEAP_TYPE_UPLOAD )
                    readRange = &nothingRead;
                const HRESULT hr = out.Resource->Map( 0, readRange, &out.Mapped );
                if ( FAILED( hr ) )
                    return Common::MakeError<D3D12Resource>(
                         DescribeHResult( "ID3D12Resource::Map(" + std::string( desc.Name ) + ")", hr ) );
            }
            NameObject( out.Resource.Get(), desc.Name );
            return Common::MakeSuccess( std::move( out ) );
        }

        D3D12Resource FinishTexture( ComPtr<ID3D12Resource> resource, const TextureDesc& desc )
        {
            D3D12Resource out;
            out.Resource  = std::move( resource );
            out.MipLevels = desc.MipLevels;
            out.ArraySize = desc.ArrayLayers;
            out.Planes    = PlaneCount( desc.Format );
            NameObject( out.Resource.Get(), desc.Name );
            return out;
        }
    } // namespace

    D3D12_RESOURCE_DESC1 MakeBufferDesc( const BufferDesc& desc ) noexcept
    {
        D3D12_RESOURCE_DESC1 out{};
        out.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        out.Width            = std::max<uint64_t>( desc.Size, 1 );
        out.Height           = 1;
        out.DepthOrArraySize = 1;
        out.MipLevels        = 1;
        out.Format           = DXGI_FORMAT_UNKNOWN;
        out.SampleDesc.Count = 1;
        out.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        out.Flags            = D3D12_RESOURCE_FLAG_NONE;
        if ( desc.AllowUnordered )
            out.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return out;
    }

    D3D12_RESOURCE_DESC1 MakeTextureDesc( const TextureDesc& desc ) noexcept
    {
        D3D12_RESOURCE_DESC1 out{};
        out.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        out.Width            = desc.Width;
        out.Height           = desc.Height;
        out.DepthOrArraySize = static_cast<UINT16>( desc.ArrayLayers );
        out.MipLevels        = static_cast<UINT16>( desc.MipLevels );
        out.Format           = ResourceFormat( desc );
        out.SampleDesc.Count = desc.Samples;
        out.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        out.Flags            = D3D12_RESOURCE_FLAG_NONE;
        if ( desc.RenderTarget )
            out.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if ( desc.DepthStencil )
            out.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if ( desc.AllowUnordered )
            out.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return out;
    }

    Common::ResultStr<D3D12Resource> CreateCommittedBuffer( ID3D12Device10&   device,
                                                            const BufferDesc& desc ) noexcept
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type                     = desc.Heap;
        const D3D12_RESOURCE_DESC1 rd = MakeBufferDesc( desc );
        ComPtr<ID3D12Resource>     resource;
        // Buffers have no layout under enhanced barriers: their initial layout is UNDEFINED by rule.
        const HRESULT hr =
             device.CreateCommittedResource3( &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                              nullptr, nullptr, 0, nullptr, IID_PPV_ARGS( &resource ) );
        if ( FAILED( hr ) )
            return Common::MakeError<D3D12Resource>( DescribeHResult( "CreateCommittedResource3(buffer " +
                                                                           std::string( desc.Name ) + ", " +
                                                                           std::to_string( desc.Size ) + " B)",
                                                                      hr ) );
        return FinishBuffer( std::move( resource ), desc );
    }

    Common::ResultStr<D3D12Resource> CreateCommittedTexture( ID3D12Device10&    device,
                                                             const TextureDesc& desc ) noexcept
    {
        const D3D12_RESOURCE_DESC1 rd = MakeTextureDesc( desc );
        if ( rd.Format == DXGI_FORMAT_UNKNOWN )
            return Common::MakeError<D3D12Resource>( "texture " + std::string( desc.Name ) +
                                                     ": the format has no DXGI equivalent" );
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> resource;
        const HRESULT          hr =
             device.CreateCommittedResource3( &heap, D3D12_HEAP_FLAG_NONE, &rd, desc.InitialLayout, nullptr,
                                              nullptr, 0, nullptr, IID_PPV_ARGS( &resource ) );
        if ( FAILED( hr ) )
            return Common::MakeError<D3D12Resource>(
                 DescribeHResult( "CreateCommittedResource3(texture " + std::string( desc.Name ) + ", " +
                                       std::to_string( desc.Width ) + "x" + std::to_string( desc.Height ) + ")",
                                  hr ) );
        return Common::MakeSuccess( FinishTexture( std::move( resource ), desc ) );
    }

    Common::ResultStr<D3D12Heap> CreatePlacementHeap( ID3D12Device10& device, uint64_t size, D3D12_HEAP_TYPE type,
                                                      D3D12_HEAP_FLAGS flags ) noexcept
    {
        D3D12_HEAP_DESC desc{};
        desc.SizeInBytes     = size;
        desc.Properties.Type = type;
        // 4 MiB: the placement alignment of an MSAA texture, so every resource class may be placed anywhere.
        desc.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
        desc.Flags     = flags;
        D3D12Heap     out;
        const HRESULT hr = device.CreateHeap( &desc, IID_PPV_ARGS( &out.Heap ) );
        if ( FAILED( hr ) )
            return Common::MakeError<D3D12Heap>(
                 DescribeHResult( "CreateHeap(" + std::to_string( size ) + " B)", hr ) );
        out.Size = size;
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<D3D12Resource> CreatePlacedBuffer( ID3D12Device10& device, const D3D12Heap& heap,
                                                         uint64_t offset, const BufferDesc& desc ) noexcept
    {
        const D3D12_RESOURCE_DESC1 rd = MakeBufferDesc( desc );
        ComPtr<ID3D12Resource>     resource;
        const HRESULT              hr =
             device.CreatePlacedResource2( heap.Heap.Get(), offset, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr,
                                           0, nullptr, IID_PPV_ARGS( &resource ) );
        if ( FAILED( hr ) )
            return Common::MakeError<D3D12Resource>( DescribeHResult( "CreatePlacedResource2(buffer " +
                                                                           std::string( desc.Name ) + " at " +
                                                                           std::to_string( offset ) + ")",
                                                                      hr ) );
        return FinishBuffer( std::move( resource ), desc );
    }

    Common::ResultStr<D3D12Resource> CreatePlacedTexture( ID3D12Device10& device, const D3D12Heap& heap,
                                                          uint64_t offset, const TextureDesc& desc ) noexcept
    {
        const D3D12_RESOURCE_DESC1 rd = MakeTextureDesc( desc );
        if ( rd.Format == DXGI_FORMAT_UNKNOWN )
            return Common::MakeError<D3D12Resource>( "texture " + std::string( desc.Name ) +
                                                     ": the format has no DXGI equivalent" );
        ComPtr<ID3D12Resource> resource;
        const HRESULT hr = device.CreatePlacedResource2( heap.Heap.Get(), offset, &rd, desc.InitialLayout, nullptr,
                                                         0, nullptr, IID_PPV_ARGS( &resource ) );
        if ( FAILED( hr ) )
            return Common::MakeError<D3D12Resource>( DescribeHResult( "CreatePlacedResource2(texture " +
                                                                           std::string( desc.Name ) + " at " +
                                                                           std::to_string( offset ) + ")",
                                                                      hr ) );
        return Common::MakeSuccess( FinishTexture( std::move( resource ), desc ) );
    }

    D3D12_RESOURCE_ALLOCATION_INFO QueryAllocation( ID3D12Device10&             device,
                                                    const D3D12_RESOURCE_DESC1& desc ) noexcept
    {
        return device.GetResourceAllocationInfo2( 0, 1, &desc, nullptr );
    }

    // --- DEVICE
    // ---------------------------------------------------------------------------------------------------

    Common::ResultStr<std::shared_ptr<D3D12Device>> D3D12Device::Create() noexcept
    {
        std::shared_ptr<D3D12Device> device( new D3D12Device() );
        auto                         init = device->Init();
        if ( !init )
            return Common::MakeError<std::shared_ptr<D3D12Device>>( "D3D12 device refused: " + init.GetError() );
        LOG_INFO( "[D3D12] {} — feature level 12_0, shader model {}.{}, raytracing tier {}, tearing {}",
                  device->m_Caps.Name, ( static_cast<int>( device->m_ShaderModel ) >> 4 ) & 0xF,
                  static_cast<int>( device->m_ShaderModel ) & 0xF, static_cast<int>( device->m_RaytracingTier ),
                  device->m_AllowTearing );
        return Common::MakeSuccess( std::move( device ) );
    }

    D3D12Device::~D3D12Device()
    {
        WaitIdle();
        m_BindlessSamplers.Release();
        m_BindlessResources.Release();
        for ( D3D12CpuDescriptorHeap& heap : m_Staging )
            heap.Release();
        for ( D3D12CommandPool& pool : m_Pools )
            pool.Release();
        for ( D3D12Queue& queue : m_Queues )
            queue.Release();
    }

    Common::BoolResultStr D3D12Device::Init() noexcept
    {
        bool debugLayer = false;
#if defined( DESERT_CONFIG_DEBUG )
        {
            ComPtr<ID3D12Debug> debug;
            if ( SUCCEEDED( D3D12GetDebugInterface( IID_PPV_ARGS( &debug ) ) ) )
            {
                debug->EnableDebugLayer();
                debugLayer = true;
            }
        }
#endif
        auto factory = CreateDxgiFactory( debugLayer );
        if ( !factory )
            return Common::MakeError( factory.GetError() );
        m_Factory = factory.ExtractValue();

        auto chosen = SelectAdapter( *m_Factory.Get() );
        if ( !chosen )
            return Common::MakeError( chosen.GetError() );
        AdapterChoice choice = chosen.ExtractValue();
        m_Adapter            = choice.Adapter;
        m_AdapterDesc        = choice.Desc;

        // ID3D12Device10: CreateCommittedResource3 / CreatePlacedResource2 with an initial barrier layout, and the
        // runtime that records ID3D12GraphicsCommandList7::Barrier (enhanced barriers).
        HRESULT hr = D3D12CreateDevice( m_Adapter.Get(), choice.FeatureLevel, IID_PPV_ARGS( &m_Device ) );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "D3D12CreateDevice (ID3D12Device10: the D3D12 runtime of "
                                                       "Windows 11 24H2, or an Agility SDK 1.706+)",
                                                       hr ) );
        NameObject( m_Device.Get(), "D3D12 device" );

        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        hr = m_Device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof( options ) );
        if ( FAILED( hr ) )
            return Common::MakeError( DescribeHResult( "CheckFeatureSupport(OPTIONS)", hr ) );
        m_ResourceHeapTier = options.ResourceHeapTier;
        if ( options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3 )
            return Common::MakeError( "resource binding tier 3 is required (bindless descriptor tables); the "
                                      "adapter reports tier " +
                                      std::to_string( static_cast<int>( options.ResourceBindingTier ) ) );

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        if ( FAILED( m_Device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                                    sizeof( options12 ) ) ) ||
             !options12.EnhancedBarriersSupported )
            return Common::MakeError( "enhanced barriers are required (D3D12_OPTIONS12::EnhancedBarriersSupported "
                                      "is false: update the GPU driver)" );

        // The highest shader model the runtime knows is asked from the top down: a runtime older than a model
        // answers E_INVALIDARG for it rather than the highest it supports.
        bool smKnown = false;
        for ( int sm = 0x69; sm >= 0x60; --sm )
        {
            D3D12_FEATURE_DATA_SHADER_MODEL model{ static_cast<D3D_SHADER_MODEL>( sm ) };
            if ( SUCCEEDED(
                      m_Device->CheckFeatureSupport( D3D12_FEATURE_SHADER_MODEL, &model, sizeof( model ) ) ) )
            {
                m_ShaderModel = model.HighestShaderModel;
                smKnown       = true;
                break;
            }
        }
        if ( !smKnown || m_ShaderModel < D3D_SHADER_MODEL_6_6 )
            return Common::MakeError( "shader model 6.6 is required (ResourceDescriptorHeap bindless indexing)" );

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if ( SUCCEEDED(
                  m_Device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof( options5 ) ) ) )
            m_RaytracingTier = options5.RaytracingTier;

        ComPtr<IDXGIFactory5> factory5;
        if ( SUCCEEDED( m_Factory.As( &factory5 ) ) )
        {
            BOOL tearing = FALSE;
            if ( SUCCEEDED( factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                           sizeof( tearing ) ) ) )
                m_AllowTearing = tearing == TRUE;
        }

        for ( uint32_t i = 0; i < kQueueKindCount; ++i )
        {
            const QueueKind kind  = static_cast<QueueKind>( i );
            auto            queue = m_Queues[i].Init( *m_Device.Get(), kind );
            if ( !queue )
                return queue;
            auto pool = m_Pools[i].Init( *m_Device.Get(), kind );
            if ( !pool )
                return pool;
        }

        const std::array<uint32_t, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> perPage{
             kStagingCbvSrvUavPerPage, kStagingSmallPerPage, kStagingSmallPerPage, kStagingSmallPerPage };
        for ( uint32_t t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++t )
        {
            auto heap =
                 m_Staging[t].Init( *m_Device.Get(), static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>( t ), perPage[t] );
            if ( !heap )
                return heap;
        }
        // Tier 3 binding: the full shader-visible CBV/SRV/UAV heap the API allows (UE BindlessResourcesHeapSize
        // defaults lower and grows; one fixed heap keeps every slot's GPU handle valid for the device's life).
        auto resources = m_BindlessResources.Init( *m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                   D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2 );
        if ( !resources )
            return resources;
        auto samplers = m_BindlessSamplers.Init( *m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                 D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE );
        if ( !samplers )
            return samplers;

        // An HDR10 output on this adapter: DisplayOutput HDR10_PQ / scRGB_Linear are offered.
        for ( UINT o = 0;; ++o )
        {
            ComPtr<IDXGIOutput> output;
            if ( m_Adapter->EnumOutputs( o, &output ) == DXGI_ERROR_NOT_FOUND )
                break;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1    outputDesc{};
            if ( SUCCEEDED( output.As( &output6 ) ) && SUCCEEDED( output6->GetDesc1( &outputDesc ) ) &&
                 outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 )
                m_HdrOutputAvailable = true;
        }

        FillCapabilities( choice );
        FillCatalog( choice );
        return BOOLSUCCESS;
    }

    void D3D12Device::FillCapabilities( const AdapterChoice& choice ) noexcept
    {
        Engine::DeviceCapabilities& caps = m_Caps;
        caps.Name                        = NarrowUtf16( choice.Desc.Description );
        caps.VendorId                    = choice.Desc.VendorId;
        caps.DeviceId                    = choice.Desc.DeviceId;
        switch ( choice.Desc.VendorId )
        {
            case 0x10DE:
                caps.VendorName = "NVIDIA";
                break;
            case 0x1002:
                caps.VendorName = "AMD";
                break;
            case 0x8086:
                caps.VendorName = "Intel";
                break;
            default:
                caps.VendorName = "Unknown";
                break;
        }
        LARGE_INTEGER umd{};
        if ( SUCCEEDED( m_Adapter->CheckInterfaceSupport( __uuidof( IDXGIDevice ), &umd ) ) )
        {
            const uint64_t version = static_cast<uint64_t>( umd.QuadPart );
            caps.DriverVersion     = static_cast<uint32_t>( version ^ ( version >> 32 ) );
        }
        static_assert( sizeof( LUID ) == 8 );
        std::memcpy( caps.DeviceLuid.data(), &choice.Desc.AdapterLuid, sizeof( LUID ) );
        caps.DeviceLuidValid = true;

        D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
        bool                             uma = false;
        if ( SUCCEEDED( m_Device->CheckFeatureSupport( D3D12_FEATURE_ARCHITECTURE1, &architecture,
                                                       sizeof( architecture ) ) ) )
            uma = architecture.UMA == TRUE;
        caps.Type        = Engine::DeviceType::Discrete;
        caps.VideoMemory = choice.Desc.DedicatedVideoMemory;
        if ( uma )
        {
            caps.Type = Engine::DeviceType::Integrated;
            caps.VideoMemory =
                 std::max<uint64_t>( choice.Desc.DedicatedVideoMemory, choice.Desc.SharedSystemMemory );
        }

        caps.MaxStorageBufferSize         = kMaxRawBufferBytes;
        caps.StorageBufferAlignment       = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
        caps.SupportsWideLines            = false;
        caps.MaxLineWidth                 = 1.0f;
        caps.SupportsAnisotropy           = true;
        caps.SupportsNonSolidFill         = true;
        caps.MaxTexture2DSize             = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        caps.MaxTextureArrayLayers        = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        caps.MaxColorAttachments          = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
        caps.MaxComputeSharedMemorySize   = kComputeSharedMemoryBytes;
        caps.SupportsTextureCompressionBC = true; // mandatory at feature level 11_0 and above
        // ExecuteIndirect with a count buffer is core D3D12.
        caps.SupportsMultiDrawIndirect = true;
        caps.MaxDrawIndirectCount      = UINT32_MAX;
        caps.SupportsDrawIndirectCount = true;
        // SV_StartVertexLocation / SV_StartInstanceLocation arrived with shader model 6.8.
        caps.SupportsShaderDrawParameters = m_ShaderModel >= static_cast<D3D_SHADER_MODEL>( 0x68 );

        UINT64 frequency = 0;
        if ( SUCCEEDED( m_Queues[0].Native()->GetTimestampFrequency( &frequency ) ) && frequency > 0 )
        {
            caps.SupportsTimestampQueries = true;
            caps.TimestampPeriodNs        = static_cast<float>( 1.0e9 / static_cast<double>( frequency ) );
        }
        caps.SupportsFloatRenderTargets  = true;
        caps.SupportsBufferDeviceAddress = true; // every buffer has a GPU virtual address

        D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
        if ( SUCCEEDED(
                  m_Device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof( options9 ) ) ) )
            caps.SupportsVisibilityBuffer64 = options9.AtomicInt64OnTypedResourceSupported == TRUE;

        caps.RayTracingHidden = Common::Settings::MachineSettings::Get().HideRayTracing;
    }

    void D3D12Device::FillCatalog( const AdapterChoice& choice ) noexcept
    {
        namespace S                   = Common::Scalability;
        S::CapabilityCatalog& catalog = m_Caps.Catalog;
        const bool            rayTracing =
             m_RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED && !m_Caps.RayTracingHidden;
        const bool rayQuery = rayTracing && m_RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;

        catalog.MSAACounts = { 1 };
        for ( UINT samples : { 2u, 4u, 8u } )
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
            levels.Format      = DXGI_FORMAT_R16G16B16A16_FLOAT;
            levels.SampleCount = samples;
            if ( SUCCEEDED( m_Device->CheckFeatureSupport( D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                                           sizeof( levels ) ) ) &&
                 levels.NumQualityLevels > 0 )
                catalog.MSAACounts.push_back( static_cast<int>( samples ) );
        }
        // The engine's own methods; the vendor upscalers (DLSS, FSR, XeSS) are integrated on Vulkan only, so this
        // backend offers none of them until their D3D12 paths exist.
        catalog.AntiAliasingMethods = { S::AntiAliasingMethod::None, S::AntiAliasingMethod::FXAA,
                                        S::AntiAliasingMethod::SMAA };
        if ( catalog.MSAACounts.size() > 1 )
            catalog.AntiAliasingMethods.push_back( S::AntiAliasingMethod::MSAA );
        catalog.AntiAliasingMethods.push_back( S::AntiAliasingMethod::TAA );
        catalog.AntiAliasingMethods.push_back( S::AntiAliasingMethod::TSR );

        catalog.RayTracingModes = { S::RayTracingMode::None };
        catalog.RayTracingAccel = S::RayTracingAcceleration::None;
        if ( rayTracing )
        {
            catalog.RayTracingAccel = S::RayTracingAcceleration::Hardware;
            if ( rayQuery )
                catalog.RayTracingModes.push_back( S::RayTracingMode::RayQuery );
            catalog.RayTracingModes.push_back( S::RayTracingMode::RayTracingPipeline );
        }
        catalog.MaxRenderArea    = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        catalog.FragmentStores   = true; // UAVs from pixel shaders are core D3D12
        catalog.AnisotropyLevels = { 1, 2, 4, 8, 16 };
        catalog.DisplayOutputs   = { S::DisplayOutput::SDR_sRGB };
        if ( m_HdrOutputAvailable )
        {
            catalog.DisplayOutputs.push_back( S::DisplayOutput::HDR10_PQ );
            catalog.DisplayOutputs.push_back( S::DisplayOutput::scRGB_Linear );
        }
        // Flip model: sync interval 1 = Fifo; sync interval 0 shows the newest frame at the next vblank
        // (Mailbox); with ALLOW_TEARING it presents at once (Immediate).
        catalog.PresentModes = { S::PresentMode::Fifo, S::PresentMode::Mailbox };
        if ( m_AllowTearing )
            catalog.PresentModes.push_back( S::PresentMode::Immediate );
        catalog.TextureCompression = { S::TextureCompressionFamily::BC };
        catalog.AsyncCompute       = true; // a COMPUTE queue always exists in D3D12
        catalog.Timing             = S::GpuTiming::None;
        if ( m_Caps.SupportsTimestampQueries )
            catalog.Timing = S::GpuTiming::AnyStage;
        catalog.Class = S::DeviceClass::Discrete;
        if ( m_Caps.Type == Engine::DeviceType::Integrated )
            catalog.Class = S::DeviceClass::Integrated;
        catalog.VideoMemory  = m_Caps.VideoMemory;
        catalog.Architecture = S::GpuArchitecture::Other;
        // Memory shared with another API is CreateSharedBuffer's, on the RendererAPI; none exists here yet.
        catalog.Sharing = S::GpuSharing::None;
        (void)choice;
    }

    void D3D12Device::WaitIdle() const
    {
        for ( D3D12Queue& queue : m_Queues )
        {
            if ( !queue.Native() )
                continue;
            auto flushed = queue.Flush();
            if ( !flushed )
            {
                LOG_ERROR( "[D3D12] WaitIdle: {}", flushed.GetError() );
            }
        }
    }

    Engine::DeviceMemoryReport D3D12Device::QueryMemory() const
    {
        Engine::DeviceMemoryReport   report;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
        const bool                   localKnown =
             SUCCEEDED( m_Adapter->QueryVideoMemoryInfo( 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local ) );
        const bool sharedKnown =
             SUCCEEDED( m_Adapter->QueryVideoMemoryInfo( 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared ) );
        report.BudgetKnown = localKnown && sharedKnown;
        Engine::DeviceMemoryHeap deviceHeap;
        deviceHeap.Size        = m_AdapterDesc.DedicatedVideoMemory;
        deviceHeap.DeviceLocal = true;
        Engine::DeviceMemoryHeap systemHeap;
        systemHeap.Size        = m_AdapterDesc.SharedSystemMemory;
        systemHeap.DeviceLocal = false;
        if ( report.BudgetKnown )
        {
            deviceHeap.Budget = local.Budget;
            deviceHeap.Usage  = local.CurrentUsage;
            systemHeap.Budget = shared.Budget;
            systemHeap.Usage  = shared.CurrentUsage;
        }
        report.Heaps = { deviceHeap, systemHeap };
        return report;
    }

    Common::BoolResultStr D3D12Device::PersistPipelineCache()
    {
        return BOOLSUCCESS;
    }

} // namespace Desert::Graphic::API::D3D12
