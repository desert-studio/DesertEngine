#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGScratch.hpp>
#include <Engine/Graphic/ViewResources.hpp>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <numeric>
#include <span>
#include <vector>
#include <memory>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Graphic::RDG
{
    namespace
    {
        // RENDER-PARALLEL-SETUP: a setup task (Common::TaskGroup) computes a plan and touches no graph; only the
        // render thread adds passes, creates and registers resources. An entry reached from a task is a race on
        // the graph, so it is named here - a log line and an assert, never RecordError (the same race again).
        void CheckNotInSetupTask( std::string_view graph, std::string_view entry )
        {
            if ( !Common::TaskGroup::InTask() )
                return;
            LOG_ERROR( "[RenderTask] graph '{}': Builder::{} was called from a setup task", graph, entry );
            assert( false && "RDG::Builder entry point called from a setup task (Common::TaskGroup)" );
        }

        bool RdgIsAttachmentAccess( Access access )
        {
            return access == Access::ColorTarget || access == Access::DepthWrite || access == Access::DepthRead;
        }

        // Final states describe what happens AFTER the graph; no pass can be doing them.
        bool RdgIsFinalOnlyAccess( Access access )
        {
            return access == Access::None || access == Access::Present || access == Access::HostRead;
        }

        std::string_view RdgPassKindName( PassFlags flags )
        {
            if ( HasFlag( flags, PassFlags::Raster ) )
                return "raster";
            if ( HasFlag( flags, PassFlags::Compute ) )
                return "compute";
            return "copy";
        }

        // Which accesses each kind of pass can issue. Anything else is a declaration the Vulkan executor
        // could only honour by synchronising stages the pass never runs.
        bool RdgPassKindAllows( PassFlags flags, Access access )
        {
            if ( HasFlag( flags, PassFlags::Copy ) )
                return access == Access::CopySrc || access == Access::CopyDst;
            if ( HasFlag( flags, PassFlags::Compute ) )
            {
                return access != Access::SampledGraphics && access != Access::VertexIndex &&
                       access != Access::CopySrc && access != Access::CopyDst && !RdgIsAttachmentAccess( access );
            }
            return access != Access::SampledCompute && access != Access::AccelStructBuildInput &&
                   access != Access::AccelStructBuildWrite && access != Access::CopySrc &&
                   access != Access::CopyDst && access != Access::ExternalWrite;
        }

        // RDG-PARALLEL-1-1 (UE FRDGBuilder::Execute with r.RDG.ParallelExecute: each parallel pass set is a task,
        // and the render thread goes on recording the serial passes; it meets a set only where the set's command
        // list joins the stream). The record phase's chunks as tasks: JobSystem helpers claim them in chunk order
        // and record them, while the calling thread walks the graph in pass order and records the serial passes.
        // At a chunk's position the calling thread takes the chunk itself when no helper has claimed it yet (the
        // pool may be busy with the game thread's work); while a helper still records it, the calling thread
        // records the next unclaimed chunks itself, and waits for the helper only once none is left
        // (FPS7-CPU-STILL-1). It never waits for a chunk nobody claimed, so a busy or empty pool only makes the
        // walk serial, as before. Held by shared_ptr: a helper the pool runs after the record phase returned finds
        // the share closed. RDG-RECORD-CHUNKS (longest-processing-time first): helpers claim in the claim order -
        // the costliest chunk of the last frame first - while the calling thread takes the chunks in position
        // order; whoever recorded a chunk, the walk replays them in position order.
        //
        // CPU-RDG-ALLOC: the recorder the helpers call - the closure on the record phase's stack and the one
        // function that calls it (a std::function would take a heap block for the closure's captures per frame).
        struct ChunkRecorder
        {
            const void* Closure                                   = nullptr;
            void ( *Call )( const void* closure, uint32_t chunk ) = nullptr;

            void operator()( uint32_t chunk ) const
            {
                Call( Closure, chunk );
            }
        };

    } // namespace

    // Outside the anonymous namespace: the builder keeps one (Builder::m_ChunkTasks, forward-declared in
    // RDGBuilder.hpp) and reuses it frame after frame.
    // CPU-RDG-ALLOC (UE: the parallel-execute task state lives with the FRDGBuilder, not one heap block per
    // frame): Reset refills the claim order and the per-chunk flags in place, so a steady frame allocates
    // nothing for them; a fresh object is made only when a helper of an earlier frame still holds the last one.
    class ChunkTasks
    {
    public:
        // A new record phase of @p count chunks: every chunk unclaimed, the claim order 0 .. count - 1 (the
        // caller may permute it through Order() before the first helper is submitted). Only on an object no
        // helper holds (Builder::Execute checks the share count).
        void Reset( uint32_t count )
        {
            m_Order.resize( count );
            std::iota( m_Order.begin(), m_Order.end(), 0u );
            m_Claimed.assign( count, 0 );
            m_Recorded.assign( count, 0 );
            m_Count     = count;
            m_OrderNext = 0;
            m_WalkNext  = 0;
            m_Running   = 0;
            m_Closed    = false;
            m_Recorder  = nullptr;
        }

        // The order helpers claim chunks in, a permutation of 0 .. count - 1; written only between Reset and
        // the first Submit.
        [[nodiscard]] std::vector<uint32_t>& Order()
        {
            return m_Order;
        }

        // The recorder every claim runs. Set once, before the first helper is submitted; it lives on the
        // record phase's stack, which Close() outlives no helper's use of.
        void SetRecorder( const ChunkRecorder* recorder )
        {
            m_Recorder = recorder;
        }

        // A helper (a JobSystem worker): records the next unclaimed chunk until none is left.
        void RunHelper()
        {
            uint32_t chunk = 0;
            while ( Claim( chunk ) )
            {
                // Finish() runs even if the recorder throws, or the calling thread waits for nothing.
                struct FinishGuard
                {
                    ChunkTasks* Tasks;
                    uint32_t    Chunk;
                    ~FinishGuard()
                    {
                        Tasks->Finish( Chunk );
                    }
                } finish{ this, chunk };
                // Per thread: the task workers' rows, apart from the calling thread's (Await).
                DESERT_PROFILE_SCOPE( "RDG: Record chunk (task)" );
                ( *m_Recorder )( chunk );
            }
        }

        // The calling thread at chunk @p chunk's position in the walk: returns once the chunk is recorded.
        // A chunk no helper claimed yet is recorded by the calling thread itself instead of waiting for one.
        // FPS7-CPU-STILL-1 (UE: a thread waiting for a task runs queued tasks - UE::Tasks::Wait /
        // FTaskGraphInterface::WaitUntilTasksComplete on a named thread - it does not sleep while work is
        // queued): a chunk a helper still records is waited for only once no chunk is left unclaimed. Until
        // then the calling thread records the next unclaimed chunk itself, as a helper would (the recorder is
        // the helpers'; a chunk is its own command buffer, replayed in pass order by the walk whoever recorded
        // it), so a pool the game thread's jobs keep busy costs the walk the helpers' missing share of the
        // chunks, never an idle wait beside them ("RDG: Wait for chunk" was 1.1-1.3 ms a frame on the still
        // camera of FPS-FIELD-BARN-TURN while chunks waited for a free worker). The chunk recorded ahead is
        // the first unclaimed one in position order: the next the walk needs (helpers take the costliest).
        // @p recordAhead false: the walk discards every chunk from here on (a late fault) and re-records their
        // passes itself, so recording one ahead is only lost time on the calling thread.
        void Await( uint32_t chunk, bool recordAhead )
        {
            std::unique_lock<std::mutex> lock( m_Lock );
            if ( m_Claimed[chunk] == 0 )
            {
                m_Claimed[chunk] = 1;
                ++m_Running;
                lock.unlock();
                {
                    DESERT_PROFILE_SCOPE( "RDG: Record chunk (render thread)" );
                    ( *m_Recorder )( chunk );
                }
                lock.lock();
                m_Recorded[chunk] = 1;
                --m_Running;
            }
            while ( recordAhead && m_Recorded[chunk] == 0 && !m_Closed )
            {
                while ( m_WalkNext < m_Count && m_Claimed[m_WalkNext] != 0 )
                    ++m_WalkNext;
                if ( m_WalkNext >= m_Count )
                    break;
                const uint32_t ahead = m_WalkNext++;
                m_Claimed[ahead]     = 1;
                ++m_Running;
                lock.unlock();
                {
                    DESERT_PROFILE_SCOPE( "RDG: Record chunk ahead (render thread)" );
                    ( *m_Recorder )( ahead );
                }
                lock.lock();
                m_Recorded[ahead] = 1;
                --m_Running;
            }
            if ( m_Recorded[chunk] == 0 )
            {
                DESERT_PROFILE_WAIT( "RDG: Wait for chunk" );
                m_Done.wait( lock, [&] { return m_Recorded[chunk] != 0; } );
            }
        }

        // Before the graph is abandoned and before the record phase returns: no helper claims a chunk any
        // more, and every chunk a helper took is finished - it records into this graph's command buffers
        // and reads the record phase's stack.
        void Close()
        {
            std::unique_lock<std::mutex> lock( m_Lock );
            m_Closed = true;
            if ( m_Running != 0 )
            {
                DESERT_PROFILE_WAIT( "RDG: Wait for chunk" );
                m_Done.wait( lock, [&] { return m_Running == 0; } );
            }
        }

    private:
        // A helper: the next unclaimed chunk in the claim order.
        bool Claim( uint32_t& chunk )
        {
            const std::lock_guard<std::mutex> lock( m_Lock );
            while ( m_OrderNext < m_Count && m_Claimed[m_Order[m_OrderNext]] != 0 )
                ++m_OrderNext;
            if ( m_Closed || m_OrderNext >= m_Count )
                return false;
            chunk            = m_Order[m_OrderNext++];
            m_Claimed[chunk] = 1;
            ++m_Running;
            return true;
        }

        void Finish( uint32_t chunk )
        {
            {
                const std::lock_guard<std::mutex> lock( m_Lock );
                m_Recorded[chunk] = 1;
                --m_Running;
            }
            m_Done.notify_all();
        }

        std::mutex                             m_Lock;
        std::condition_variable                m_Done;
        std::vector<uint32_t>                  m_Order;    // the helpers' claim order
        std::vector<uint8_t>                   m_Claimed;  // by chunk; under m_Lock
        std::vector<uint8_t>                   m_Recorded; // by chunk; under m_Lock
        uint32_t                               m_Count = 0;
        uint32_t                               m_OrderNext = 0; // no chunk before it in m_Order is unclaimed
        uint32_t                               m_WalkNext  = 0; // no chunk before it by index is unclaimed
        uint32_t                               m_Running   = 0; // claimed, not finished
        bool                                   m_Closed    = false;
        const ChunkRecorder*                   m_Recorder  = nullptr;
    };

    namespace
    {

        // Closes the record phase's chunk tasks on every way out of it (ChunkTasks::Close).
        struct ChunkTasksCloser
        {
            std::shared_ptr<ChunkTasks>& Tasks;
            ~ChunkTasksCloser()
            {
                if ( Tasks )
                    Tasks->Close();
            }
        };

        // RDG-RECORD-CHUNKS: on every way out of one pass's recording, the CPU time it took, in microseconds,
        // into @p slot (CompileResult::RecordMicros at the pass's position); nothing when @p slot is null.
        class RecordCostTimer
        {
        public:
            explicit RecordCostTimer( float* slot ) : m_Slot( slot )
            {
                if ( m_Slot != nullptr )
                    m_Start = std::chrono::steady_clock::now();
            }
            RecordCostTimer( const RecordCostTimer& )            = delete;
            RecordCostTimer& operator=( const RecordCostTimer& ) = delete;
            ~RecordCostTimer()
            {
                if ( m_Slot == nullptr )
                    return;
                const std::chrono::duration<float, std::micro> spent = std::chrono::steady_clock::now() - m_Start;
                *m_Slot                                              = spent.count();
            }

        private:
            float*                                m_Slot = nullptr;
            std::chrono::steady_clock::time_point m_Start;
        };
    } // namespace

    // ── Builder: resources ─────────────────────────────────────────────────────────────────────────────

    ImportedFramebuffer Builder::ImportFramebuffer( std::span<ExternalTexture* const> colors,
                                                    ExternalTexture* depth, std::string_view name,
                                                    std::span<ExternalTexture* const> resolves )
    {
        CheckNotInSetupTask( m_Name, "ImportFramebuffer" );
        ImportedFramebuffer imported;
        if ( !resolves.empty() && resolves.size() != colors.size() )
            RecordError( std::format( "graph '{}': ImportFramebuffer('{}') has {} colour(s) and {} resolve(s)",
                                      m_Name, name, colors.size(), resolves.size() ) );
        for ( size_t i = 0; i < resolves.size(); ++i )
        {
            if ( resolves[i] == nullptr )
            {
                RecordError(
                     std::format( "graph '{}': ImportFramebuffer('{}') resolve {} is null", m_Name, name, i ) );
                imported.Resolves.push_back( {} );
                continue;
            }
            imported.Resolves.push_back(
                 RegisterExternal( *resolves[i], std::format( "{}.Resolve{}", name, i ) ) );
        }
        for ( size_t i = 0; i < colors.size(); ++i )
        {
            if ( colors[i] == nullptr )
            {
                RecordError(
                     std::format( "graph '{}': ImportFramebuffer('{}') colour {} is null", m_Name, name, i ) );
                imported.Colors.push_back( {} );
                continue;
            }
            imported.Colors.push_back( RegisterExternal( *colors[i], std::format( "{}.Color{}", name, i ) ) );
        }
        if ( depth != nullptr )
            imported.Depth = RegisterExternal( *depth, std::format( "{}.Depth", name ) );
        return imported;
    }

    TextureRef Builder::CreateTexture( const TextureDesc& desc, std::string_view name )
    {
        CheckNotInSetupTask( m_Name, "CreateTexture" );
        const uint32_t maxMips = Core::Formats::MipChainLength( std::max(
             { desc.Size.Width, desc.Size.Height, desc.Dim == TextureDim::Tex3D ? desc.Size.Depth : 1u } ) );
        if ( desc.Size.Width == 0 || desc.Size.Height == 0 || desc.Size.Depth == 0 || desc.Mips == 0 ||
             desc.Layers == 0 )
            RecordError( fmt::format( "graph '{}': texture '{}' has a zero extent ({}x{}x{}, {} mips, {} layers)",
                                      m_Name, name, desc.Size.Width, desc.Size.Height, desc.Size.Depth, desc.Mips,
                                      desc.Layers ) );
        else if ( desc.Mips > maxMips )
            RecordError( fmt::format( "graph '{}': texture '{}' asks for {} mips, a {}x{} chain has {}", m_Name,
                                      name, desc.Mips, desc.Size.Width, desc.Size.Height, maxMips ) );
        else if ( ( desc.Dim == TextureDim::Cube || desc.Dim == TextureDim::CubeArray ) && desc.Layers % 6 != 0 )
            RecordError( fmt::format( "graph '{}': cube texture '{}' has {} layers, not a multiple of 6", m_Name,
                                      name, desc.Layers ) );
        else if ( desc.Dim == TextureDim::Tex3D && desc.Layers != 1 )
            RecordError( fmt::format( "graph '{}': volume texture '{}' has {} layers; a volume has one", m_Name,
                                      name, desc.Layers ) );
        else if ( desc.Dim != TextureDim::Tex3D && desc.Size.Depth != 1 )
            RecordError( fmt::format( "graph '{}': texture '{}' is not a volume but has depth {}", m_Name, name,
                                      desc.Size.Depth ) );

        ResourceRecord record( m_Arena );
        record.Name.assign( name.data(), name.size() );
        record.Kind    = ResourceKind::Texture;
        record.Texture = desc;
        m_Resources.push_back( std::move( record ) );
        return TextureRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    Common::ResultStr<TextureDesc> Builder::GetTextureDesc( TextureRef texture ) const
    {
        const ResourceRecord* resource = FindResource( texture.Index, ResourceKind::Texture );
        if ( resource == nullptr )
            return Common::MakeFormattedError<TextureDesc>(
                 "graph '{}': GetTextureDesc of invalid texture handle {}", m_Name, texture.Index );
        return Common::MakeSuccess( resource->Texture );
    }

    Common::ResultStr<std::string_view> Builder::GetTextureName( TextureRef texture ) const
    {
        const ResourceRecord* resource = FindResource( texture.Index, ResourceKind::Texture );
        if ( resource == nullptr )
            return Common::MakeFormattedError<std::string_view>(
                 "graph '{}': GetTextureName of invalid texture handle {}", m_Name, texture.Index );
        return Common::MakeSuccess( std::string_view( resource->Name ) );
    }

    BufferRef Builder::CreateBuffer( const BufferDesc& desc, std::string_view name )
    {
        CheckNotInSetupTask( m_Name, "CreateBuffer" );
        if ( desc.Bytes == 0 )
            RecordError( fmt::format( "graph '{}': buffer '{}' has zero bytes", m_Name, name ) );

        ResourceRecord record( m_Arena );
        record.Name.assign( name.data(), name.size() );
        record.Kind   = ResourceKind::Buffer;
        record.Buffer = desc;
        m_Resources.push_back( std::move( record ) );
        return BufferRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    void Builder::QueueBufferUpload( BufferRef buffer, std::span<const std::byte> bytes )
    {
        CheckNotInSetupTask( m_Name, "QueueBufferUpload" );
        // Copied at the call: the exec runs after the caller's storage is gone.
        auto payload = std::make_shared<const std::vector<std::byte>>( bytes.begin(), bytes.end() );
        const std::span<const std::byte> copied( *payload );
        AddUploadPass( buffer, copied, std::move( payload ) );
    }

    void Builder::QueueStaticBufferUpload( BufferRef buffer, std::span<const std::byte> bytes )
    {
        CheckNotInSetupTask( m_Name, "QueueStaticBufferUpload" );
        AddUploadPass( buffer, bytes, nullptr );
    }

    void Builder::AddUploadPass( BufferRef buffer, std::span<const std::byte> bytes,
                                 std::shared_ptr<const std::vector<std::byte>> owned )
    {
        const ResourceRecord* record = FindResource( buffer.Index, ResourceKind::Buffer );
        std::string           name;
        if ( record != nullptr )
            name.assign( record->Name.data(), record->Name.size() );
        else
            name = fmt::format( "#{}", buffer.Index );
        std::string           error;
        if ( record == nullptr )
            error = fmt::format( "graph '{}': upload into invalid buffer handle {}", m_Name, buffer.Index );
        else if ( bytes.empty() )
            error = fmt::format( "graph '{}': upload into buffer '{}' carries no bytes", m_Name, name );
        else if ( bytes.size() > record->Buffer.Bytes )
            error = fmt::format( "graph '{}': upload of {} bytes into buffer '{}' of {} bytes", m_Name,
                                 bytes.size(), name, record->Buffer.Bytes );
        else if ( bytes.size() % 4 != 0 )
            error = fmt::format( "graph '{}': upload of {} bytes into buffer '{}' is not a multiple of 4 bytes",
                                 m_Name, bytes.size(), name );

        AddPass(
             std::format( "Upload: {}", name ), PassFlags::Copy,
             [&]( PassBuilder& pass )
             {
                 // Declared even when the upload is refused: the faulted pass's write is what makes its readers
                 // Dependency faults rooted on it, not "read before any pass writes it".
                 if ( record != nullptr )
                     pass.Write( buffer, Access::CopyDst );
             },
             [buffer, bytes, owned = std::move( owned )]( PassContext& context ) -> Common::BoolResultStr
             {
                 const Common::ResultStr<BufferBinding> target = context.GetBuffer( buffer, Access::CopyDst );
                 if ( !target )
                     return Common::MakeFormattedError( "{}", target.GetError() );
                 return context.GetBackend().UploadBuffer( target.GetValue().Resource, bytes );
             } );
        m_Passes.back().QueuedUpload = true;
        if ( !error.empty() )
            RecordPassError( static_cast<uint32_t>( m_Passes.size() - 1 ), std::move( error ) );
    }

    TextureRef Builder::RegisterExternal( ExternalTexture& texture, std::string_view name )
    {
        CheckNotInSetupTask( m_Name, "RegisterExternal" );
        if ( texture.SubresourceStates.size() != texture.Desc.SubresourceCount() )
            RecordError( fmt::format( "graph '{}': external texture '{}' carries {} subresource states for {} "
                                      "subresources ({} mips x {} layers)",
                                      m_Name, name, texture.SubresourceStates.size(),
                                      texture.Desc.SubresourceCount(), texture.Desc.Mips, texture.Desc.Layers ) );

        ResourceRecord record( m_Arena );
        record.Name.assign( name.data(), name.size() );
        record.Kind        = ResourceKind::Texture;
        record.Texture     = texture.Desc;
        record.ExternalTex = &texture;
        m_Resources.push_back( std::move( record ) );
        return TextureRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    BufferRef Builder::RegisterExternal( ExternalBuffer& buffer, std::string_view name )
    {
        CheckNotInSetupTask( m_Name, "RegisterExternal" );
        ResourceRecord record( m_Arena );
        record.Name.assign( name.data(), name.size() );
        record.Kind        = ResourceKind::Buffer;
        record.Buffer      = buffer.Desc;
        record.ExternalBuf = &buffer;
        m_Resources.push_back( std::move( record ) );
        return BufferRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    void Builder::Extract( TextureRef texture, ExternalTexture& into, Access final,
                           std::initializer_list<Access> later )
    {
        CheckNotInSetupTask( m_Name, "Extract" );
        if ( FindResource( texture.Index, ResourceKind::Texture ) == nullptr )
        {
            RecordError(
                 fmt::format( "graph '{}': Extract of invalid texture handle {}", m_Name, texture.Index ) );
            return;
        }

        ResourceRecord& record = m_Resources[texture.Index];
        if ( record.HasFinalAccess )
        {
            RecordError( fmt::format( "graph '{}': texture '{}' is extracted twice", m_Name, record.Name ) );
            return;
        }
        if ( ( GetAccessInfo( final ).Targets & AccessTarget_Texture ) == 0 )
        {
            RecordError( fmt::format( "graph '{}': texture '{}' extracted into {}, a buffer-only access", m_Name,
                                      record.Name, GetAccessName( final ) ) );
            return;
        }
        if ( record.ExternalTex != nullptr && record.ExternalTex != &into )
        {
            RecordError( fmt::format( "graph '{}': external texture '{}' extracted into a different "
                                      "ExternalTexture than it was registered from",
                                      m_Name, record.Name ) );
            return;
        }
        record.ExtractTex     = &into;
        record.HasFinalAccess = true;
        record.FinalAccess    = final;
        for ( const Access access : later )
        {
            if ( ( GetAccessInfo( access ).Targets & AccessTarget_Texture ) == 0 )
            {
                RecordError(
                     fmt::format( "graph '{}': texture '{}' extracted for a later {}, a buffer-only access",
                                  m_Name, record.Name, GetAccessName( access ) ) );
                return;
            }
            record.LaterAccessMask |= 1u << static_cast<uint32_t>( access );
        }
        record.ExtractOwner = CurrentResourceAttribution();
    }

    void Builder::Extract( BufferRef buffer, ExternalBuffer& into, Access final )
    {
        CheckNotInSetupTask( m_Name, "Extract" );
        if ( FindResource( buffer.Index, ResourceKind::Buffer ) == nullptr )
        {
            RecordError( fmt::format( "graph '{}': Extract of invalid buffer handle {}", m_Name, buffer.Index ) );
            return;
        }

        ResourceRecord& record = m_Resources[buffer.Index];
        if ( record.HasFinalAccess )
        {
            RecordError( fmt::format( "graph '{}': buffer '{}' is extracted twice", m_Name, record.Name ) );
            return;
        }
        if ( ( GetAccessInfo( final ).Targets & AccessTarget_Buffer ) == 0 )
        {
            RecordError( fmt::format( "graph '{}': buffer '{}' extracted into {}, a texture-only access", m_Name,
                                      record.Name, GetAccessName( final ) ) );
            return;
        }
        if ( record.ExternalBuf != nullptr && record.ExternalBuf != &into )
        {
            RecordError( fmt::format( "graph '{}': external buffer '{}' extracted into a different "
                                      "ExternalBuffer than it was registered from",
                                      m_Name, record.Name ) );
            return;
        }
        record.ExtractBuf     = &into;
        record.HasFinalAccess = true;
        record.FinalAccess    = final;
        record.ExtractOwner   = CurrentResourceAttribution();
    }

    void Builder::Reset() noexcept
    {
        // The records first: their arena containers give nothing back, and the arena reset below then runs the
        // execs' destructors and rewinds every block (CPU-FRAME-ARENA). Every member a new builder starts with
        // is set back to that value here; m_Name and the arena's blocks are what the owner keeps.
        m_Passes.clear();
        m_Resources.clear();
        m_Prerequisites.clear();
        std::fill( m_PrerequisiteSlots.begin(), m_PrerequisiteSlots.end(), PrerequisiteSlot{} );
        m_DeclarationError.clear();
        m_Executed        = false;
        m_PassCulling     = true;
        m_AsyncCompute    = true;
        m_ParallelExecute = true;
        m_FaultDefaults   = FaultDefaults{};
        m_Report          = ExecuteReport{};
        m_LateSubstitutions.clear();
        m_Arena.Reset();
    }

    PassBuilder Builder::BeginPass( std::string_view name, PassFlags flags )
    {
        CheckNotInSetupTask( m_Name, "BeginPass" );
        const int kinds = ( HasFlag( flags, PassFlags::Raster ) ? 1 : 0 ) +
                          ( HasFlag( flags, PassFlags::Compute ) ? 1 : 0 ) +
                          ( HasFlag( flags, PassFlags::Copy ) ? 1 : 0 );
        PassRecord record( m_Arena );
        record.Name.assign( name.data(), name.size() );
        // SetAsyncCompute( false ): the request is dropped here, before Compile sees it (the declaration check
        // below still reads what the caller declared).
        record.Flags = m_AsyncCompute ? flags
                                      : static_cast<PassFlags>( static_cast<uint8_t>( flags ) &
                                                                ~static_cast<uint8_t>( PassFlags::AsyncCompute ) );
        m_Passes.push_back( std::move( record ) );
        const auto pass = static_cast<uint32_t>( m_Passes.size() - 1 );
        // A malformed pass faults itself (RDG-FAULT1), the graph goes on without it.
        if ( kinds != 1 )
            RecordPassError(
                 pass,
                 fmt::format( "graph '{}': pass '{}' names {} of Raster/Compute/Copy; exactly one is required",
                              m_Name, name, kinds ) );
        if ( HasFlag( flags, PassFlags::AsyncCompute ) && !HasFlag( flags, PassFlags::Compute ) )
            RecordPassError(
                 pass, std::format( "graph '{}': pass '{}' declares AsyncCompute without Compute; only a compute "
                                    "pass can run on the async compute queue",
                                    m_Name, name ) );
        return { *this, pass };
    }

    void Builder::RecordError( std::string message )
    {
        if ( m_DeclarationError.empty() )
            m_DeclarationError = std::move( message );
    }

    void Builder::RecordPassError( uint32_t pass, std::string message )
    {
        if ( m_Passes[pass].DeclarationError.empty() )
            m_Passes[pass].DeclarationError = std::move( message );
    }

    void Builder::SetFaultDefault( TextureRef texture, FaultDefault value )
    {
        const ResourceRecord* record = FindResource( texture.Index, ResourceKind::Texture );
        if ( record == nullptr || record->IsExternal() )
        {
            RecordError( std::format( "graph '{}': SetFaultDefault of handle {}, which is not a transient "
                                      "texture of this graph",
                                      m_Name, texture.Index ) );
            return;
        }
        m_Resources[texture.Index].Default = value;
    }

    void Builder::SetFaultPolicy( TextureRef external, ExternalFaultPolicy policy )
    {
        const ResourceRecord* record = FindResource( external.Index, ResourceKind::Texture );
        if ( record == nullptr || !record->IsExternal() )
        {
            RecordError( std::format( "graph '{}': SetFaultPolicy of texture handle {}, which is not an "
                                      "external of this graph",
                                      m_Name, external.Index ) );
            return;
        }
        m_Resources[external.Index].Policy = policy;
    }

    void Builder::SetFaultPolicy( BufferRef external, ExternalFaultPolicy policy )
    {
        const ResourceRecord* record = FindResource( external.Index, ResourceKind::Buffer );
        if ( record == nullptr || !record->IsExternal() )
        {
            RecordError( std::format( "graph '{}': SetFaultPolicy of buffer handle {}, which is not an "
                                      "external of this graph",
                                      m_Name, external.Index ) );
            return;
        }
        m_Resources[external.Index].Policy = policy;
    }

    FaultDefaults& Builder::GetFaultDefaults()
    {
        return m_FaultDefaults;
    }

    const FaultDefaults& Builder::GetFaultDefaults() const
    {
        return m_FaultDefaults;
    }

    const ExecuteReport& Builder::GetExecuteReport() const
    {
        return m_Report;
    }

    ExternalTexture* Builder::FindExternalTexture( uint32_t resource ) const
    {
        return resource < m_Resources.size() ? m_Resources[resource].ExternalTex : nullptr;
    }

    FrameFaultExternal Builder::MakeFrameFaultExternal( uint32_t resource ) const
    {
        const ResourceRecord& record = m_Resources[resource];
        return { resource, record.HasFinalAccess ? std::optional<Access>( record.FinalAccess ) : std::nullopt };
    }

    const Builder::ResourceRecord* Builder::FindResource( uint32_t index, ResourceKind kind ) const
    {
        if ( index >= m_Resources.size() || m_Resources[index].Kind != kind )
            return nullptr;
        return &m_Resources[index];
    }

    // ── PassBuilder ────────────────────────────────────────────────────────────────────────────────────

    void PassBuilder::Read( TextureRef texture, Access access, SubresourceRange range )
    {
        DeclareTexture( texture, access, range, false, "Read" );
    }

    void PassBuilder::Write( TextureRef texture, Access access, SubresourceRange range )
    {
        DeclareTexture( texture, access, range, true, "Write" );
    }

    void PassBuilder::Read( BufferRef buffer, Access access )
    {
        DeclareBuffer( buffer, access, false, "Read" );
    }

    void PassBuilder::Write( BufferRef buffer, Access access )
    {
        DeclareBuffer( buffer, access, true, "Write" );
    }

    void PassBuilder::ColorTarget( uint32_t slot, TextureRef texture, const LoadOp& load, uint32_t mip,
                                   uint32_t layer, StoreAction store )
    {
        DeclareAttachment( slot, false, texture, Access::ColorTarget, load, mip, layer, store );
    }

    void PassBuilder::ResolveTarget( uint32_t slot, TextureRef texture )
    {
        DeclareAttachment( slot, false, texture, Access::ColorTarget, LoadOp::DontCare(), 0, 0, StoreAction::Store,
                           true );
    }

    void PassBuilder::DepthTarget( TextureRef texture, const LoadOp& load, bool write, uint32_t layer,
                                   StoreAction store )
    {
        DeclareAttachment( 0, true, texture, write ? Access::DepthWrite : Access::DepthRead, load, 0, layer,
                           store );
    }

    void PassBuilder::RenderArea( uint32_t width, uint32_t height )
    {
        Builder::PassRecord& pass  = m_Builder.m_Passes[m_Pass];
        const std::string&   graph = m_Builder.m_Name;
        if ( !HasFlag( pass.Flags, PassFlags::Raster ) )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': RenderArea({}x{}) in a {} pass", graph,
                                                    pass.Name, width, height, RdgPassKindName( pass.Flags ) ) );
            return;
        }
        if ( width == 0 || height == 0 )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': RenderArea({}x{}) has a zero side",
                                                    graph, pass.Name, width, height ) );
            return;
        }
        if ( !pass.Attachments.empty() )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': RenderArea({}x{}) in a pass with "
                                                    "{} attachment(s); the area of such a pass is theirs",
                                                    graph, pass.Name, width, height, pass.Attachments.size() ) );
            return;
        }
        pass.AreaWidth  = width;
        pass.AreaHeight = height;
    }

    void PassBuilder::Prepares( const IRecordPrerequisite& prerequisite )
    {
        // One graph-wide index per distinct object (O(1) here; Execute prepares by index).
        const uint32_t index    = m_Builder.IndexPrerequisite( prerequisite );
        auto&          declared = m_Builder.m_Passes[m_Pass].Prerequisites;
        if ( std::find( declared.begin(), declared.end(), index ) == declared.end() )
            declared.push_back( index );
    }

    uint32_t Builder::IndexPrerequisite( const IRecordPrerequisite& prerequisite )
    {
        // Grown (and every key re-placed) before the insert that would make it more than half full.
        if ( ( m_Prerequisites.size() + 1 ) * 2 > m_PrerequisiteSlots.size() )
        {
            constexpr size_t kFirstSlots = 16;
            m_PrerequisiteSlots.assign( std::max( kFirstSlots, m_PrerequisiteSlots.size() * 2 ), {} );
            const size_t mask = m_PrerequisiteSlots.size() - 1;
            for ( uint32_t i = 0; i < m_Prerequisites.size(); ++i )
            {
                size_t slot = std::hash<const void*>{}( m_Prerequisites[i] ) & mask;
                while ( m_PrerequisiteSlots[slot].Key != nullptr )
                    slot = ( slot + 1 ) & mask;
                m_PrerequisiteSlots[slot] = { m_Prerequisites[i], i };
            }
        }
        const size_t mask = m_PrerequisiteSlots.size() - 1;
        size_t       slot = std::hash<const void*>{}( &prerequisite ) & mask;
        while ( m_PrerequisiteSlots[slot].Key != nullptr )
        {
            if ( m_PrerequisiteSlots[slot].Key == &prerequisite )
                return m_PrerequisiteSlots[slot].Index;
            slot = ( slot + 1 ) & mask;
        }
        const uint32_t index      = static_cast<uint32_t>( m_Prerequisites.size() );
        m_PrerequisiteSlots[slot] = { &prerequisite, index };
        m_Prerequisites.push_back( &prerequisite );
        return index;
    }

    void PassBuilder::ExternalQueue( const ExternalQueueSync& sync )
    {
        Builder::PassRecord& pass  = m_Builder.m_Passes[m_Pass];
        const std::string&   graph = m_Builder.m_Name;
        std::string          refusal;
        if ( !HasFlag( pass.Flags, PassFlags::Copy ) || HasFlag( pass.Flags, PassFlags::AsyncCompute ) )
            refusal = fmt::format( "in a {} pass; only a graphics-queue Copy pass hands work to another queue",
                                   RdgPassKindName( pass.Flags ) );
        else if ( sync.Signal.Semaphore == 0 || sync.Wait.Semaphore == 0 )
            refusal = "names no semaphore";
        else if ( sync.Signal.Semaphore != sync.Wait.Semaphore )
            refusal = "signals one semaphore and waits another";
        else if ( sync.Wait.Value <= sync.Signal.Value )
            refusal = fmt::format( "waits value {} that is not after the signalled {}", sync.Wait.Value,
                                   sync.Signal.Value );
        else if ( pass.External.has_value() )
            refusal = "is declared twice";
        if ( !refusal.empty() )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': ExternalQueue {}", graph, pass.Name, refusal ) );
            return;
        }
        pass.External = sync;
    }

    void PassBuilder::DeclareTexture( TextureRef texture, Access access, SubresourceRange range, bool asWrite,
                                      std::string_view call )
    {
        Builder::PassRecord&           pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( texture.Index, ResourceKind::Texture );
        const std::string&             graph    = m_Builder.m_Name;
        if ( resource == nullptr )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': {}() of invalid texture handle {}",
                                                    graph, pass.Name, call, texture.Index ) );
            return;
        }
        if ( RdgIsFinalOnlyAccess( access ) )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': {}('{}', {}) - {} is a state after the "
                                                    "graph (Extract), not something a pass does",
                                                    graph, pass.Name, call, resource->Name,
                                                    GetAccessName( access ), GetAccessName( access ) ) );
            return;
        }
        if ( ( GetAccessInfo( access ).Targets & AccessTarget_Texture ) == 0 )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) - a buffer-only access on a "
                                      "texture",
                                      graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
            return;
        }
        if ( RdgIsAttachmentAccess( access ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) - attachments are declared "
                                      "with ColorTarget()/DepthTarget(), which carry the load op",
                                      graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
            return;
        }
        if ( asWrite != IsWriteAccess( access ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) - {} is a {} access", graph, pass.Name,
                                      call, resource->Name, GetAccessName( access ), GetAccessName( access ),
                                      asWrite ? "read" : "write" ) );
            return;
        }
        if ( !RdgPassKindAllows( pass.Flags, access ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) in a {} pass", graph, pass.Name, call,
                                      resource->Name, GetAccessName( access ), RdgPassKindName( pass.Flags ) ) );
            return;
        }

        const TextureDesc& desc     = resource->Texture;
        SubresourceRange   resolved = range;
        if ( resolved.BaseMip >= desc.Mips || resolved.BaseLayer >= desc.Layers )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': {}('{}') mip {} layer {} is outside {} "
                                                    "mips x {} layers",
                                                    graph, pass.Name, call, resource->Name, resolved.BaseMip,
                                                    resolved.BaseLayer, desc.Mips, desc.Layers ) );
            return;
        }
        if ( resolved.MipCount == kAllRemaining )
            resolved.MipCount = desc.Mips - resolved.BaseMip;
        if ( resolved.LayerCount == kAllRemaining )
            resolved.LayerCount = desc.Layers - resolved.BaseLayer;
        if ( resolved.MipCount == 0 || resolved.LayerCount == 0 ||
             resolved.BaseMip + resolved.MipCount > desc.Mips ||
             resolved.BaseLayer + resolved.LayerCount > desc.Layers )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}') mips [{}, +{}) layers [{}, +{}) "
                                      "do not fit {} mips x {} layers",
                                      graph, pass.Name, call, resource->Name, resolved.BaseMip, resolved.MipCount,
                                      resolved.BaseLayer, resolved.LayerCount, desc.Mips, desc.Layers ) );
            return;
        }

        pass.Uses.push_back( { texture.Index, access, resolved, -1 } );
    }

    void PassBuilder::DeclareBuffer( BufferRef buffer, Access access, bool asWrite, std::string_view call )
    {
        Builder::PassRecord&           pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( buffer.Index, ResourceKind::Buffer );
        const std::string&             graph    = m_Builder.m_Name;
        if ( resource == nullptr )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': {}() of invalid buffer handle {}",
                                                    graph, pass.Name, call, buffer.Index ) );
            return;
        }
        if ( RdgIsFinalOnlyAccess( access ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) - a state after the graph "
                                      "(Extract), not something a pass does",
                                      graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
            return;
        }
        if ( ( GetAccessInfo( access ).Targets & AccessTarget_Buffer ) == 0 )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) - a texture-only access on a "
                                      "buffer",
                                      graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
            return;
        }
        if ( asWrite != IsWriteAccess( access ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) - {} is a {} access", graph, pass.Name,
                                      call, resource->Name, GetAccessName( access ), GetAccessName( access ),
                                      asWrite ? "read" : "write" ) );
            return;
        }
        if ( !RdgPassKindAllows( pass.Flags, access ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}', {}) in a {} pass", graph, pass.Name, call,
                                      resource->Name, GetAccessName( access ), RdgPassKindName( pass.Flags ) ) );
            return;
        }

        pass.Uses.push_back( { buffer.Index, access, SubresourceRange{ 0, 1, 0, 1 }, -1 } );
    }

    void PassBuilder::DeclareAttachment( uint32_t slot, bool isDepth, TextureRef texture, Access access,
                                         const LoadOp& load, uint32_t mip, uint32_t layer, StoreAction store,
                                         bool isResolve )
    {
        Builder::PassRecord&           pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( texture.Index, ResourceKind::Texture );
        const std::string&             graph    = m_Builder.m_Name;
        std::string_view               call     = "ColorTarget";
        if ( isResolve )
            call = "ResolveTarget";
        else if ( isDepth )
            call = "DepthTarget";
        if ( resource == nullptr )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': {}() of invalid texture handle {}",
                                                    graph, pass.Name, call, texture.Index ) );
            return;
        }
        if ( !HasFlag( pass.Flags, PassFlags::Raster ) )
        {
            m_Builder.RecordPassError( m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}') in a {} pass", graph,
                                                            pass.Name, call, resource->Name,
                                                            RdgPassKindName( pass.Flags ) ) );
            return;
        }
        if ( pass.AreaWidth != 0 )
        {
            m_Builder.RecordPassError( m_Pass, fmt::format( "graph '{}' pass '{}': {}('{}') in a pass with a "
                                                            "RenderArea; such a pass has no attachment",
                                                            graph, pass.Name, call, resource->Name ) );
            return;
        }
        if ( access == Access::DepthRead && load.Action == LoadAction::Clear )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': DepthTarget('{}') clears a read-only "
                                                    "depth attachment",
                                                    graph, pass.Name, resource->Name ) );
            return;
        }
        for ( const Builder::AttachmentRecord& existing : pass.Attachments )
        {
            if ( existing.IsDepth == isDepth && existing.IsResolve == isResolve &&
                 ( isDepth || existing.Slot == slot ) )
            {
                m_Builder.RecordPassError( m_Pass,
                                           fmt::format( "graph '{}' pass '{}': {}('{}') binds slot {} twice",
                                                        graph, pass.Name, call, resource->Name, slot ) );
                return;
            }
        }
        // One render pass has one sample count: every colour and depth attachment shares it. A resolve target is
        // single-sample and matches the multisampled colour of its slot in format and size.
        const TextureDesc& self = resource->Texture;
        for ( const Builder::AttachmentRecord& existing : pass.Attachments )
        {
            const Builder::ResourceRecord& otherRecord = m_Builder.m_Resources[existing.Resource];
            const TextureDesc&             other       = otherRecord.Texture;
            if ( isResolve && !existing.IsResolve && !existing.IsDepth && existing.Slot == slot &&
                 ( other.Samples <= 1 || self.Samples != 1 || other.Format != self.Format ||
                   other.Size.Width != self.Size.Width || other.Size.Height != self.Size.Height ) )
            {
                m_Builder.RecordPassError(
                     m_Pass,
                     fmt::format(
                          "graph '{}' pass '{}': ResolveTarget('{}') cannot resolve colour slot {} ('{}'): the "
                          "colour "
                          "has {} sample(s), format {}, {}x{}; the resolve target {} sample(s), format {}, {}x{}",
                          graph, pass.Name, resource->Name, slot, otherRecord.Name, other.Samples,
                          static_cast<uint32_t>( other.Format ), other.Size.Width, other.Size.Height, self.Samples,
                          static_cast<uint32_t>( self.Format ), self.Size.Width, self.Size.Height ) );
                return;
            }
            if ( !isResolve && !existing.IsResolve && other.Samples != self.Samples )
            {
                m_Builder.RecordPassError(
                     m_Pass,
                     fmt::format( "graph '{}' pass '{}': {}('{}') has {} sample(s), '{}' in the same pass has {}",
                                  graph, pass.Name, call, resource->Name, self.Samples, otherRecord.Name,
                                  other.Samples ) );
                return;
            }
        }
        if ( isResolve &&
             std::none_of( pass.Attachments.begin(), pass.Attachments.end(),
                           [slot]( const Builder::AttachmentRecord& existing )
                           { return !existing.IsDepth && !existing.IsResolve && existing.Slot == slot; } ) )
        {
            m_Builder.RecordPassError(
                 m_Pass, fmt::format( "graph '{}' pass '{}': ResolveTarget('{}') for colour slot {}, which has no "
                                      "ColorTarget declared before it",
                                      graph, pass.Name, resource->Name, slot ) );
            return;
        }

        const TextureDesc& desc       = resource->Texture;
        const uint32_t     baseLayer  = layer == kAllRemaining ? 0 : layer;
        const uint32_t     layerCount = layer == kAllRemaining ? desc.Layers : 1;
        if ( mip >= desc.Mips || baseLayer >= desc.Layers )
        {
            m_Builder.RecordPassError( m_Pass,
                                       fmt::format( "graph '{}' pass '{}': {}('{}') mip {} layer {} is outside {} "
                                                    "mips x {} layers",
                                                    graph, pass.Name, call, resource->Name, mip, baseLayer,
                                                    desc.Mips, desc.Layers ) );
            return;
        }

        Builder::AttachmentRecord attachment;
        attachment.Slot       = slot;
        attachment.IsDepth    = isDepth;
        attachment.IsResolve  = isResolve;
        attachment.Resource   = texture.Index;
        attachment.Load       = load;
        attachment.Mip        = mip;
        attachment.BaseLayer  = baseLayer;
        attachment.LayerCount = layerCount;
        attachment.Store      = store;
        pass.Attachments.push_back( attachment );
        pass.Uses.push_back( { texture.Index, access, SubresourceRange{ mip, 1, baseLayer, layerCount },
                               static_cast<int32_t>( pass.Attachments.size() - 1 ) } );
    }

    // ── PassContext ────────────────────────────────────────────────────────────────────────────────────

    std::string_view PassContext::GetPassName() const
    {
        return m_Builder.m_Passes[m_Pass].Name;
    }

    BindingBlockRef PassContext::GetBindingBlock( const uint32_t index ) const
    {
        return BindingBlockRef{ m_Pass, index };
    }

    Pipe PassContext::GetPipe() const
    {
        for ( const CompiledPass& pass : m_Result.Passes )
        {
            if ( pass.Pass == m_Pass )
                return pass.OnPipe;
        }
        return Pipe::Graphics; // a context exists only for an executed pass, which is always listed above
    }

    Common::ResultStr<TextureDesc> PassContext::GetTextureDesc( TextureRef texture ) const
    {
        return m_Builder.GetTextureDesc( texture );
    }

    Common::ResultStr<TextureBinding> PassContext::GetTexture( TextureRef texture, Access access,
                                                               SubresourceRange range ) const
    {
        const Builder::PassRecord&     pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( texture.Index, ResourceKind::Texture );
        if ( resource == nullptr )
            return Common::MakeFormattedError<TextureBinding>(
                 "graph '{}' pass '{}': GetTexture of invalid handle {}", m_Builder.m_Name, pass.Name,
                 texture.Index );
#if DESERT_DEV_INSTRUMENTS
        const TextureDesc& desc   = resource->Texture;
        const uint32_t     mipEnd = range.MipCount == kAllRemaining ? desc.Mips : range.BaseMip + range.MipCount;
        const uint32_t     layerEnd =
             range.LayerCount == kAllRemaining ? desc.Layers : range.BaseLayer + range.LayerCount;
        for ( uint32_t layer = range.BaseLayer; layer < layerEnd; ++layer )
        {
            for ( uint32_t mip = range.BaseMip; mip < mipEnd; ++mip )
            {
                bool declared = false;
                for ( const Builder::ResourceUse& use : pass.Uses )
                {
                    declared =
                         declared ||
                         ( use.Resource == texture.Index && use.Usage == access && mip >= use.Range.BaseMip &&
                           mip < use.Range.BaseMip + use.Range.MipCount && layer >= use.Range.BaseLayer &&
                           layer < use.Range.BaseLayer + use.Range.LayerCount );
                }
                if ( !declared )
                    return Common::MakeFormattedError<TextureBinding>(
                         "graph '{}' pass '{}' uses texture '{}' mip {} layer {} as {} without declaring it in "
                         "its setup",
                         m_Builder.m_Name, pass.Name, resource->Name, mip, layer, GetAccessName( access ) );
            }
        }
#endif
        // RDG-FAULT1: a read of a transient whose producer was removed by a fault reads the FaultDefault the
        // producer declared - the system texture - under the handle the pass declared (DefaultSubstitution).
        uint32_t bound = texture.Index;
        for ( const std::vector<DefaultSubstitution>* list :
              { &m_Result.Substitutions, &m_Late } )
        {
            for ( const DefaultSubstitution& substitution : *list )
            {
                if ( substitution.ReaderPass == m_Pass && substitution.Original == texture.Index &&
                     !substitution.AttachmentCleared )
                    bound = substitution.Replacement;
            }
        }
        resource = &m_Builder.m_Resources[bound];
        TextureBinding binding;
        binding.Resource = bound;
        binding.Name     = resource->Name;
        binding.Desc     = &resource->Texture;
        binding.External = resource->ExternalTex != nullptr ? resource->ExternalTex : resource->ExtractTex;
        binding.Memory   = m_Result.FindAllocation( bound );
        binding.Physical = m_Backend.FindPhysicalTexture( bound );
        return Common::MakeSuccess( binding );
    }

    Common::ResultStr<BufferBinding> PassContext::GetBuffer( BufferRef buffer, Access access ) const
    {
        const Builder::PassRecord&     pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( buffer.Index, ResourceKind::Buffer );
        if ( resource == nullptr )
            return Common::MakeFormattedError<BufferBinding>(
                 "graph '{}' pass '{}': GetBuffer of invalid handle {}", m_Builder.m_Name, pass.Name,
                 buffer.Index );
#if DESERT_DEV_INSTRUMENTS
        bool declared = false;
        for ( const Builder::ResourceUse& use : pass.Uses )
            declared = declared || ( use.Resource == buffer.Index && use.Usage == access );
        if ( !declared )
            return Common::MakeFormattedError<BufferBinding>(
                 "graph '{}' pass '{}' uses buffer '{}' as {} without declaring it in its setup", m_Builder.m_Name,
                 pass.Name, resource->Name, GetAccessName( access ) );
#endif
        BufferBinding binding;
        binding.Resource = buffer.Index;
        binding.Name     = resource->Name;
        binding.Desc     = &resource->Buffer;
        binding.External = resource->ExternalBuf != nullptr ? resource->ExternalBuf : resource->ExtractBuf;
        binding.Memory   = m_Result.FindAllocation( buffer.Index );
        binding.Physical = m_Backend.FindPhysicalBuffer( buffer.Index );
        return Common::MakeSuccess( binding );
    }

    // ── Execute ────────────────────────────────────────────────────────────────────────────────────────

    // Mirrors one barrier batch into the imported textures it touches: each covered subresource takes the
    // barrier's After state, then the owner's record hook sees the new states at once. The image's own layout
    // record (read by descriptor binds and by code outside the graph) therefore follows the GPU barrier by
    // barrier instead of catching up at the end of Execute.
    Common::BoolResultStr Builder::RecordExternalStates( std::span<const Barrier> barriers )
    {
        ScratchVector<uint32_t> touched; // resources, one per external texture
        for ( const Barrier& barrier : barriers )
        {
            if ( barrier.Kind != ResourceKind::Texture )
                continue;
            const ResourceRecord&  record   = m_Resources[barrier.Resource];
            ExternalTexture* const external = record.ExternalTex;
            if ( external == nullptr )
                continue;
            const TextureDesc& desc   = external->Desc;
            const uint32_t     mips   = barrier.Range.MipCount == kAllRemaining ? desc.Mips - barrier.Range.BaseMip
                                                                                : barrier.Range.MipCount;
            const uint32_t     layers = barrier.Range.LayerCount == kAllRemaining
                                             ? desc.Layers - barrier.Range.BaseLayer
                                             : barrier.Range.LayerCount;
            for ( uint32_t layer = barrier.Range.BaseLayer; layer < barrier.Range.BaseLayer + layers; ++layer )
                for ( uint32_t mip = barrier.Range.BaseMip; mip < barrier.Range.BaseMip + mips; ++mip )
                    external->SubresourceStates[desc.SubresourceIndex( mip, layer )] = barrier.After;
            if ( std::find( touched.begin(), touched.end(), barrier.Resource ) == touched.end() )
                touched.push_back( barrier.Resource );
        }
        for ( const uint32_t resource : touched )
        {
            const ResourceRecord& record = m_Resources[resource];
            if ( !record.ExternalTex->RecordStates )
                continue;
            const Common::BoolResultStr recorded =
                 record.ExternalTex->RecordStates( record.ExternalTex->SubresourceStates, false );
            if ( !recorded )
                return Common::MakeFormattedError( "texture '{}': {}", record.Name, recorded.GetError() );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Builder::Execute( IBackend& backend )
    {
        if ( m_Executed )
            return Common::MakeFormattedError( "graph '{}' was already executed; a graph is built per frame",
                                               m_Name );
        m_Executed = true;
        m_Report   = {};
        m_LateSubstitutions.clear();
        // CPU-RDG-ALLOC: the execute's working arrays (ScratchVector) come from the graph's scratch arena and are
        // released together when Execute returns. A worker never grows one of them (RDGScratch.hpp): what a chunk
        // appends to on a worker (its faults, late substitutions) stays a std::vector.
        const ScratchScope scratch( m_Scratch );

        ScratchVector<std::string_view> added;
        added.reserve( m_Passes.size() );
        for ( const PassRecord& pass : m_Passes )
            added.push_back( pass.Name );
        // RDG-FAULT1: every way this execute ends goes through one of these two, so the reporter hears each
        // execute exactly once and an error is exactly a FrameFault.
        const auto finish = [&]() -> Common::BoolResultStr
        {
            std::stable_sort( m_Report.Faults.begin(), m_Report.Faults.end(),
                              []( const PassFault& a, const PassFault& b ) { return a.Pass < b.Pass; } );
            std::sort( m_Report.FaultCulled.begin(), m_Report.FaultCulled.end() );
            backend.GetPassFaultReporter().Report( m_Name, added, m_Report );
            if ( m_Report.Frame )
                return Common::MakeError( m_Report.Frame->Reason );
            return Common::MakeSuccess( true );
        };
        const auto frameFault = [&]( std::string reason, std::vector<uint32_t> roots ) -> Common::BoolResultStr
        {
            FrameFault fault{ std::move( reason ), {}, std::move( roots ) };
            for ( uint32_t r = 0; r < m_Resources.size(); ++r )
            {
                if ( m_Resources[r].ExternalTex != nullptr &&
                     m_Resources[r].Policy == ExternalFaultPolicy::FrameFatal )
                    fault.Externals.push_back( MakeFrameFaultExternal( r ) );
            }
            m_Report.Frame = std::move( fault );
            return finish();
        };

        DESERT_PROFILE_PHASES( phases );
        DESERT_PROFILE_PHASE( phases, "RDG: Compile" );
        // RDG-RECORD-COMPILE-CACHE: the backend's stored plan for this declaration, rebound to this graph, or a
        // fresh compile. Held to the end of Execute: a stored plan serves no other graph while this one records.
        Common::ResultStr<std::shared_ptr<CompileResult>> compiled = CompileThroughCache( backend );
        if ( !compiled )
            return frameFault( compiled.GetError(), {} );
        const std::shared_ptr<CompileResult> plan   = compiled.ExtractValue();
        const CompileResult&                 result = *plan;
        m_Report.Faults               = result.Faults;
        m_Report.FaultCulled          = result.FaultCulledPasses;
        m_Report.InvalidatedExternals = result.InvalidatedExternals;
        if ( result.Frame )
        {
            m_Report.Frame = result.Frame; // nothing recorded: the caller clears Frame->Externals and presents
            return finish();
        }

        // The decided fallback, announced once per backend (the log keeps the count).
        if ( !result.DemotedAsyncPasses.empty() )
        {
            ScratchVector<std::string_view> demoted;
            demoted.reserve( result.DemotedAsyncPasses.size() );
            for ( const uint32_t pass : result.DemotedAsyncPasses )
                demoted.push_back( m_Passes[pass].Name );
            backend.GetAsyncComputeFallbackLog().Report( demoted );
        }

        const auto substituted = [&]( uint32_t pass, uint32_t resource )
        {
            return std::any_of( result.Substitutions.begin(), result.Substitutions.end(),
                                [&]( const DefaultSubstitution& s )
                                { return s.ReaderPass == pass && s.Original == resource; } );
        };
        ScratchVector<ResourceView> views( m_Resources.size() );
        bool                        anyFaultDefault = false;
        for ( uint32_t r = 0; r < m_Resources.size(); ++r )
        {
            const ResourceRecord& record = m_Resources[r];
            ResourceView&         view   = views[r];
            view.Resource                = r;
            view.Name                    = record.Name;
            view.Kind                    = record.Kind;
            view.Texture                 = record.Kind == ResourceKind::Texture ? &record.Texture : nullptr;
            view.Buffer                  = record.Kind == ResourceKind::Buffer ? &record.Buffer : nullptr;
            view.ExternalTex             = record.ExternalTex;
            view.ExternalBuf             = record.ExternalBuf;
            view.Extracted               = record.IsExtracted() && !record.IsExternal();
            view.ExtractOwner            = record.ExtractOwner;
            anyFaultDefault              = anyFaultDefault || record.Default != FaultDefault::None;
        }
        for ( const DerivedUsage& usage : result.Usages )
            views[usage.Resource].AccessMask = usage.AccessMask;
        for ( const CompiledPass& compiledPass : result.Passes )
        {
            // A read replaced by its FaultDefault does not touch the lost transient (it has no allocation).
            for ( const ResourceUse& use : m_Passes[compiledPass.Pass].Uses )
                views[use.Resource].Used =
                     views[use.Resource].Used || !substituted( compiledPass.Pass, use.Resource );
        }
        // UE (FRDGBuilder::Execute, the epilogue's BeginResourceRHI of every extracted texture): an extraction or
        // a final access is a use at the end of the graph, so the resource has a physical image even when no
        // executed pass touched it (its producers were never added - an empty scene - or were removed by a
        // fault). CollectFinalBarriers transitions it to its final access, and the extraction hands it out.
        for ( uint32_t r = 0; r < m_Resources.size(); ++r )
        {
            const ResourceRecord& record = m_Resources[r];
            if ( !record.IsExtracted() || views[r].Used )
                continue;
            views[r].Used = true;
            views[r].AccessMask |= ( 1u << static_cast<uint32_t>( record.FinalAccess ) ) | record.LaterAccessMask;
        }
        // A LATE fault substitutes through PassContext at record time, so the system textures are bound whenever
        // a FaultDefault exists, not only when Compile already substituted one.
        for ( const FaultDefault value : { FaultDefault::Black, FaultDefault::White, FaultDefault::BlackCube } )
        {
            const uint32_t source = m_FaultDefaults.GetSource( value );
            if ( anyFaultDefault && source < views.size() )
                views[source].Used = true;
        }

        DESERT_PROFILE_PHASE( phases, "RDG: BeginGraph" );
        const GraphView             graph{ m_Name, views, &result };
        const Common::BoolResultStr begun = backend.BeginGraph( graph );
        if ( !begun )
            return frameFault( std::format( "graph '{}': {}", m_Name, begun.GetError() ), {} );

        // RDG-FAULT1 late faults. lostRoot[p] >= 0: pass p did not produce its outputs this frame (its exec
        // failed, or it was skipped for a lost input); the value is the pass the chain starts at.
        ScratchVector<int32_t> lostRoot( m_Passes.size(), -1 );
        // RDG-PARALLEL-2: where one run of passes keeps its late faults. The calling thread's run writes the
        // graph's own lists; a chunk recorded on a worker writes the chunk's (RecordChunk), and the replay merges
        // them in pass order. A worker never ends the frame: it keeps a backend failure, the replay ends it.
        struct RecordState
        {
            ScratchVector<int32_t>&           LostRoot; // by pass
            std::vector<PassFault>&           Faults;
            std::vector<uint32_t>&            FaultCulled;
            std::vector<DefaultSubstitution>& Late;
            bool                              Worker         = false;
            bool                              RenderPassOpen = false;
            std::string                       BackendFailure;
        };
        RecordState graphState{ lostRoot, m_Report.Faults, m_Report.FaultCulled, m_LateSubstitutions };
        const auto  lateFault = [&]( RecordState& state, uint32_t pass, PassFaultStage stage, std::string reason,
                                    std::optional<uint32_t> root )
        {
            state.LostRoot[pass] = static_cast<int32_t>( root ? *root : pass );
            state.Faults.push_back(
                 { pass, std::string( m_Passes[pass].Name ), stage, std::move( reason ), root } );
            state.FaultCulled.push_back( pass );
        };
        // The record phase's chunk tasks (ChunkTasks), made once the chunks are begun. Closed - every chunk a
        // helper took finished - before the calling thread abandons the graph, and on every way out of the record
        // phase.
        std::shared_ptr<ChunkTasks> chunkTasks;
        const auto                  abandonGraph = [&]()
        {
            if ( chunkTasks )
                chunkTasks->Close();
            backend.AbandonGraph();
        };
        // A backend failure abandons the graph on the calling thread; a worker only keeps it.
        const auto backendFailure = [&]( RecordState& state, std::string reason ) -> Common::BoolResultStr
        {
            if ( state.Worker )
            {
                state.BackendFailure = std::move( reason );
                return Common::MakeFormattedError( "{}", state.BackendFailure );
            }
            abandonGraph();
            return frameFault( std::move( reason ), {} );
        };

        // RENDER-THREADS M1: why a pass's declared prerequisite was not prepared. CPU-RDG-PREPARE: by pass, an
        // index into the sparse prepareReasons (kPrepared = prepared, or none declared): no string per pass.
        constexpr uint32_t       kPrepared = ~uint32_t{ 0 };
        ScratchVector<uint32_t>  prepareFailures( m_Passes.size(), kPrepared );
        std::vector<std::string> prepareReasons;

        // The incoming read-after-write edges of each pass, in result.Edges order (CSR: rawIn[rawBegin[p] ..
        // rawBegin[p + 1]) are pass p's). A late fault walks only the pass's own edges instead of every edge of
        // the graph per pass.
        ScratchVector<uint32_t> rawBegin( m_Passes.size() + 1, 0 );
        ScratchVector<uint32_t> rawIn;
        {
            for ( const DependencyEdge& edge : result.Edges )
            {
                if ( edge.Kind == DependencyKind::ReadAfterWrite && edge.To < m_Passes.size() )
                    ++rawBegin[edge.To + 1];
            }
            for ( size_t p = 0; p < m_Passes.size(); ++p )
                rawBegin[p + 1] += rawBegin[p];
            rawIn.resize( rawBegin.back() );
            ScratchVector<uint32_t> fill( rawBegin.begin(), rawBegin.end() - 1 );
            for ( uint32_t e = 0; e < result.Edges.size(); ++e )
            {
                const DependencyEdge& edge = result.Edges[e];
                if ( edge.Kind == DependencyKind::ReadAfterWrite && edge.To < m_Passes.size() )
                    rawIn[fill[edge.To]++] = e;
            }
        }

        // RDG-RECORD-CHUNKS: the plan's RecordMicros while this frame measures them (the record phase cuts
        // chunks), else null. Each position is recorded by one thread at a time - a discarded chunk's passes
        // record again only after its Await - so every pass writes its own slot without a lock.
        float* recordMicros = nullptr;

        // One pass inside its segment: barriers, render pass, exec, releases. Only a BACKEND failure abandons
        // the graph; a pass that fails or loses an input is a late fault and the graph goes on.
        auto recordPass = [&]( uint32_t position, RecordState& state ) -> Common::BoolResultStr
        {
            const CompiledPass& planned = result.Passes[position];
            // THE CPU RECORD COST OF EACH PASS, one row per pass (UE: RDG's per-pass CPU event under the pass's
            // name). Prefixed so it never lands in the row the backend fills with the same pass's GPU time, nor
            // in a DESERT_PROFILE_PASS row the pass's own lambda opens under that name. The row is interned once
            // per name and thread; no "RDG Record: <pass>" string is built per pass per frame.
            DESERT_PROFILE_SCOPE_PREFIXED( "RDG Record: ", planned.Name );
            float* costSlot = nullptr;
            if ( recordMicros != nullptr )
                costSlot = recordMicros + position;
            const RecordCostTimer   cost( costSlot );
            const uint32_t          p = planned.Pass;
            CompiledPass            adjusted;
            bool                    useAdjusted = false;
            std::optional<uint32_t> skipRoot;
            std::string             skipReason;
            for ( uint32_t incoming = rawBegin[p]; incoming < rawBegin[p + 1]; ++incoming )
            {
                const DependencyEdge& edge = result.Edges[rawIn[incoming]];
                if ( state.LostRoot[edge.From] < 0 || skipRoot )
                    continue;
                const ResourceRecord& record     = m_Resources[edge.Resource];
                const uint32_t        source     = record.Kind == ResourceKind::Texture && !record.IsExternal()
                                                        ? m_FaultDefaults.GetSource( record.Default )
                                                        : kInvalidResource;
                bool                  attachment = false;
                bool                  sampled    = false;
                bool                  other      = false;
                for ( const ResourceUse& use : m_Passes[p].Uses )
                {
                    if ( use.Resource != edge.Resource )
                        continue;
                    const bool isSampled =
                         use.Usage == Access::SampledGraphics || use.Usage == Access::SampledCompute;
                    attachment = attachment || use.Attachment >= 0;
                    sampled    = sampled || ( use.Attachment < 0 && isSampled );
                    other      = other || ( use.Attachment < 0 && !isSampled );
                }
                if ( source == kInvalidResource || other )
                {
                    skipRoot   = static_cast<uint32_t>( state.LostRoot[edge.From] );
                    skipReason = std::format( "reads '{}', which pass '{}' did not produce", record.Name,
                                              m_Passes[edge.From].Name );
                    continue;
                }
                if ( sampled )
                    state.Late.push_back( { p, edge.Resource, source, record.Default, false } );
                if ( attachment )
                {
                    if ( !useAdjusted )
                        adjusted = planned;
                    useAdjusted = true;
                    for ( AttachmentDecision& decision : adjusted.Attachments )
                    {
                        if ( decision.Resource == edge.Resource && decision.Load == LoadAction::Load )
                        {
                            decision.Load  = LoadAction::Clear;
                            decision.Clear = FaultDefaults::GetClear( record.Default );
                        }
                    }
                    state.Late.push_back( { p, edge.Resource, kInvalidResource, record.Default, true } );
                }
            }
            const CompiledPass& pass = useAdjusted ? adjusted : planned;

            backend.BeginPass( pass );
            if ( !pass.Barriers.empty() )
            {
                // Recorded even for a skipped pass: the layout chain the plan computed must stay true.
                backend.RecordBarriers( pass.Barriers );
                // A worker's chunk sets the external states at its replay, on the calling thread, in pass order.
                if ( !state.Worker )
                {
                    const Common::BoolResultStr recorded = RecordExternalStates( pass.Barriers );
                    if ( !recorded )
                        return backendFailure( state, std::format( "graph '{}' pass '{}': {}", m_Name, pass.Name,
                                                                   recorded.GetError() ) );
                }
            }
            const bool rendering =
                 HasFlag( pass.Flags, PassFlags::Raster ) && ( !pass.Attachments.empty() || pass.AreaWidth != 0 );
            if ( skipRoot )
            {
                if ( state.RenderPassOpen )
                    backend.EndRenderPass();
                state.RenderPassOpen = false;
                lateFault( state, p, PassFaultStage::Dependency, std::move( skipReason ), skipRoot );
            }
            else
            {
                // A pass merged into a render pass that a faulted or skipped pass closed opens its own; merged
                // passes load every attachment, so the plan's decisions hold.
                if ( rendering && ( !pass.ContinuesRenderPass || !state.RenderPassOpen || useAdjusted ) )
                {
                    if ( state.RenderPassOpen )
                        backend.EndRenderPass();
                    const Common::BoolResultStr started = backend.BeginRenderPass( pass );
                    if ( !started )
                        return backendFailure( state, std::format( "graph '{}' pass '{}': {}", m_Name, pass.Name,
                                                                   started.GetError() ) );
                    state.RenderPassOpen = true;
                }
                // RENDER-THREADS M1: a pass whose prerequisite could not be prepared does not record; a pass
                // that declared any records sealed - its exec looks up what the prepare made and creates nothing.
                const Common::BoolResultStr outcome = [&]() -> Common::BoolResultStr
                {
                    if ( prepareFailures[p] != kPrepared )
                        return Common::MakeFormattedError( "a prerequisite was not prepared: {}",
                                                           prepareReasons[prepareFailures[p]] );
                    PassContext                  context( *this, result, backend, p, state.Late );
                    std::optional<RecordingSeal> seal;
                    if ( !m_Passes[p].Prerequisites.empty() )
                        seal.emplace();
                    return m_Passes[p].Exec( context );
                }();
                if ( !outcome )
                    lateFault( state, p, PassFaultStage::Execution, outcome.GetError(), std::nullopt );
                if ( rendering && state.RenderPassOpen && ( !outcome || !pass.KeepsRenderPassOpen ) )
                {
                    backend.EndRenderPass();
                    state.RenderPassOpen = false;
                }
            }
            if ( !pass.EpilogueBarriers.empty() )
                backend.RecordEpilogueBarriers( pass.EpilogueBarriers );
            backend.EndPass( pass );
            return Common::MakeSuccess( true );
        };

        // RENDER-THREADS M1 (UE: everything a pass lambda binds exists before the graph records). Every
        // prerequisite a surviving pass declared is prepared once, in pass order, before the first pass records;
        // a failure faults only the passes that declared it.
        DESERT_PROFILE_PHASE( phases, "RDG: Prepare recording" );
        {
            // RENDER-CPU-COST-2: the phase's two parts read apart (it was one self row, 0.58-0.70 ms on the turn).
            DESERT_PROFILE_PHASES( prepareRows );
            DESERT_PROFILE_PHASE( prepareRows, "RDG Prepare: prerequisites" );
            // By graph-wide index: kNotPrepared until the first surviving pass that declared it prepares it.
            // CPU-RDG-PREPARE: kPrepared, or the index of its failure in prepareReasons.
            constexpr uint32_t      kNotPrepared = kPrepared - 1;
            ScratchVector<uint32_t> prepared( m_Prerequisites.size(), kNotPrepared );
            for ( const CompiledPass& planned : result.Passes )
            {
                for ( const uint32_t index : m_Passes[planned.Pass].Prerequisites )
                {
                    if ( prepared[index] == kNotPrepared )
                    {
                        const Common::BoolResultStr ready = m_Prerequisites[index]->PrepareForRecording();
                        prepared[index]                   = kPrepared;
                        if ( !ready )
                        {
                            prepared[index] = static_cast<uint32_t>( prepareReasons.size() );
                            prepareReasons.push_back( ready.GetError() );
                        }
                    }
                    if ( prepared[index] != kPrepared && prepareFailures[planned.Pass] == kPrepared )
                        prepareFailures[planned.Pass] = prepared[index];
                }
            }
            // UE FRDGBuilder::SetupParallelExecute: what the backend would otherwise create lazily while a pass
            // records (Vulkan: the render pass, framebuffer and attachment views of every pass that opens one)
            // exists before the first pass records. A failure here is not reported: the pass's BeginRenderPass
            // resolves it again and fails at the same point it always did.
            DESERT_PROFILE_PHASE( prepareRows, "RDG Prepare: render passes" );
            backend.PrepareRecording( result.Passes );
            DESERT_PROFILE_PHASE_END( prepareRows );
        }

        DESERT_PROFILE_PHASE( phases, "RDG: Record passes" );
        // RDG-PARALLEL-2 (UE FRDGBuilder::Execute, r.RDG.ParallelExecute). A backend that records in parallel gets
        // each graphics segment cut into chunks at render pass boundaries; task workers record the chunks, and the
        // calling thread replays them into the segment's submission in pass order. RDG-PARALLEL-1-1: the chunks
        // record WHILE the calling thread walks the graph and records the serial passes - a NeverParallel pass,
        // the async compute segments, every pass after the first late fault - in order, as before; it meets a
        // chunk only at the chunk's position (ChunkTasks::Await). A graph with a prerequisite that was not
        // prepared records entirely on the calling thread.
        struct RecordChunk
        {
            RecordChunkDesc                  Desc;
            // By pass; only the chunk's own passes are ever set. Sized where the chunk is made (the calling
            // thread): the worker that records the chunk only refills it.
            ScratchVector<int32_t>           LostRoot;
            std::vector<PassFault>           Faults;
            std::vector<uint32_t>            FaultCulled;
            std::vector<DefaultSubstitution> Late;
            std::string                      BackendFailure;
        };
        ScratchVector<RecordChunk> chunks;
        ScratchVector<int32_t>     chunkAt;     // by position: the chunk starting there, -1 when none
        ScratchVector<float>       chunkMicros; // by chunk: its passes' record cost last frame (0 when unmeasured)
        bool                     chunksByCost = false;
        // Every failed prerequisite was declared by a surviving pass, which carries it.
        const bool               anyPrepareFailure = !prepareReasons.empty();
        if ( m_ParallelExecute && backend.SupportsParallelRecording() && !anyPrepareFailure )
        {
            // UE: r.RDG.ParallelExecute.PassMin 1 / PassMax. Every segment is cut, however short: the calling
            // thread no longer waits for the chunks before it walks (RDG-PARALLEL-1-1), so a hand-off costs it a
            // claim, not a stall - the segment of a few heavy passes between an async fork and its join is what
            // the walk overlaps. RDG-RECORD-CHUNKS (UE cuts by pass count, which put the turn's heavy passes in
            // one 24-pass chunk the walk then waited 0.79 ms for): a chunk grows while its passes cost at most
            // kChunkMicros as the last frame that recorded this plan measured them (CompileResult::RecordMicros).
            // A plan with no costs yet (a cache miss, or no cache) grows a chunk to about kChunkPasses passes.
            // A run between two render pass boundaries is never split, so a run dearer than kChunkMicros is one
            // chunk of its own.
            constexpr uint32_t kChunkPasses = 24;
            constexpr float    kChunkMicros = 200.0f;
            chunksByCost                    = plan->RecordMicros.size() == result.Passes.size();
            chunkAt.assign( result.Passes.size(), -1 );
            const auto cuttable = [&]( uint32_t position )
            {
                return !result.Passes[position].ContinuesRenderPass &&
                       !result.Passes[position - 1].KeepsRenderPassOpen;
            };
            for ( const PipeSegment& segment : result.Segments )
            {
                // RDG-RECORD-CHUNKS: AsyncCompute segments are cut as the graphics ones are. A chunk is on its own
                // pipe: the backend records it into a command buffer of that pipe's queue family and hangs its
                // GPU scopes on that pipe's timeline from BeginChunks on (VulkanRdgBackend::BeginChunks,
                // ChunkRoot), whatever segment the walk is in while a helper records it. The prologue has no
                // passes; an NGX evaluation is NeverParallel and stays in the walk.
                if ( segment.FirstPosition == CrossPipeSync::kForkAtGraphStart )
                    continue;
                bool chunkOpen = false;
                for ( uint32_t position = segment.FirstPosition; position <= segment.LastPosition; )
                {
                    // One run: the passes up to the next render pass boundary.
                    uint32_t runEnd = position;
                    while ( runEnd < segment.LastPosition && !cuttable( runEnd + 1 ) )
                        ++runEnd;
                    bool  neverParallel = false;
                    float runMicros     = 0.0f;
                    for ( uint32_t inRun = position; inRun <= runEnd; ++inRun )
                    {
                        neverParallel =
                             neverParallel || HasFlag( result.Passes[inRun].Flags, PassFlags::NeverParallel );
                        if ( chunksByCost )
                            runMicros += plan->RecordMicros[inRun];
                    }
                    bool grows = chunkOpen && chunks.back().Desc.LastPosition + 1 == position;
                    if ( grows && chunksByCost )
                        grows = chunkMicros.back() + runMicros <= kChunkMicros;
                    else if ( grows )
                        grows =
                             chunks.back().Desc.LastPosition + 1 - chunks.back().Desc.FirstPosition < kChunkPasses;
                    if ( neverParallel )
                        chunkOpen = false;
                    else if ( grows )
                    {
                        chunks.back().Desc.LastPosition = runEnd;
                        chunkMicros.back() += runMicros;
                    }
                    else
                    {
                        RecordChunk chunk;
                        chunk.Desc = { segment.OnPipe, position, runEnd };
                        chunk.LostRoot.assign( m_Passes.size(), -1 );
                        chunkAt[position] = static_cast<int32_t>( chunks.size() );
                        chunks.push_back( std::move( chunk ) );
                        chunkMicros.push_back( runMicros );
                        chunkOpen = true;
                    }
                    position = runEnd + 1;
                }
            }
            // Measured this frame for the next one: only a plan the cache keeps is ever read again.
            if ( !chunks.empty() && backend.GetCompileCache() != nullptr )
            {
                plan->RecordMicros.resize( result.Passes.size(), 0.0f );
                recordMicros = plan->RecordMicros.data();
            }
        }
        // The active view is per recording thread (ViewResources.cpp t_ActiveView): a chunk recorded on a worker
        // reads the same view's descriptor sets and block copies as the thread that runs the graph (UE: a parallel
        // command list set records for its view), not the frame context.
        ViewResources&                        recordingView = ViewResourceRegistry::Active();
        const auto                            recordChunk   = [&]( uint32_t c )
        {
            const ActiveViewScope viewScope( recordingView );
            RecordChunk&          chunk = chunks[c];
            std::fill( chunk.LostRoot.begin(), chunk.LostRoot.end(), -1 ); // a discarded chunk records again
            RecordState state{ chunk.LostRoot, chunk.Faults, chunk.FaultCulled, chunk.Late };
            state.Worker = true;
            backend.BeginChunk( c );
            for ( uint32_t position = chunk.Desc.FirstPosition; position <= chunk.Desc.LastPosition; ++position )
            {
                if ( !recordPass( position, state ) )
                    break;
            }
            backend.EndChunk( c );
            chunk.BackendFailure = std::move( state.BackendFailure );
        };
        const ChunkRecorder chunkRecorder{ &recordChunk, []( const void* closure, uint32_t c )
                                           { ( *static_cast<decltype( &recordChunk )>( closure ) )( c ); } };
        // After everything a chunk task reads (the chunks, the recorder, the plan): on every way out of the record
        // phase the tasks are closed before any of it goes.
        const ChunkTasksCloser closeChunkTasks{ chunkTasks };
        if ( !chunks.empty() )
        {
            ScratchVector<RecordChunkDesc> descs;
            descs.reserve( chunks.size() );
            for ( const RecordChunk& chunk : chunks )
                descs.push_back( chunk.Desc );
            const Common::BoolResultStr chunksBegun = backend.BeginChunks( descs );
            if ( !chunksBegun )
            {
                backend.AbandonGraph();
                return frameFault( std::format( "graph '{}' chunks: {}", m_Name, chunksBegun.GetError() ), {} );
            }
            // UE: the parallel pass sets are launched as tasks before the render thread executes the serial
            // passes. One helper per worker at most; a chunk no helper claimed by the time the walk reaches it is
            // recorded by the calling thread (ChunkTasks::Await), so a refused Submit costs only parallelism.
            // High (UE: the parallel command list sets are high-priority tasks): the frame waits on these
            // chunks ("RDG: Wait for chunk"), so a free worker takes one before any Normal or Background job.
            // RDG-RECORD-CHUNKS: helpers claim the costliest chunk of the last frame first (longest processing
            // time first: the dearest chunk starts earliest, the cheap ones fill in); ties, and a plan with no
            // costs, in position order.
            // CPU-RDG-ALLOC: the builder's kept tasks, reset in place; a new one only on the first frame or
            // while a helper submitted by an earlier frame still holds the kept one (it runs late, finds that
            // share closed and drops it). The share count of 1 is read after every helper's release of its copy,
            // and the acquire fence orders that helper's last access before the reset.
            if ( m_ChunkTasks == nullptr || m_ChunkTasks.use_count() != 1 )
                m_ChunkTasks = std::make_shared<ChunkTasks>();
            std::atomic_thread_fence( std::memory_order_acquire );
            m_ChunkTasks->Reset( static_cast<uint32_t>( chunks.size() ) );
            if ( chunksByCost )
            {
                std::vector<uint32_t>& claimOrder = m_ChunkTasks->Order();
                std::stable_sort( claimOrder.begin(), claimOrder.end(),
                                  [&]( uint32_t a, uint32_t b ) { return chunkMicros[a] > chunkMicros[b]; } );
            }
            chunkTasks = m_ChunkTasks;
            chunkTasks->SetRecorder( &chunkRecorder );
            Common::JobSystem& jobs    = Common::JobSystem::Get();
            const size_t       helpers = std::min( chunks.size(), jobs.WorkerCount() );
            for ( size_t h = 0; h < helpers; ++h )
            {
                std::shared_ptr<ChunkTasks> share = chunkTasks;
                if ( !jobs.Submit( [share]() noexcept { share->RunHelper(); }, Common::JobPriority::High ) )
                    break;
            }
        }
        // Set once any pass faulted late: a chunk recorded on a worker did not see that fault, so every later
        // chunk is discarded and its passes record again on the calling thread (the rare path).
        bool discardChunks = false;
        // The calling thread's replay of one chunk: its passes' external states, its faults, then its commands.
        const auto replayChunk = [&]( RecordChunk& chunk, uint32_t index ) -> Common::BoolResultStr
        {
            DESERT_PROFILE_SCOPE( "RDG: Replay chunk" );
            for ( uint32_t position = chunk.Desc.FirstPosition; position <= chunk.Desc.LastPosition; ++position )
            {
                const CompiledPass& pass = result.Passes[position];
                if ( pass.Barriers.empty() )
                    continue;
                const Common::BoolResultStr recorded = RecordExternalStates( pass.Barriers );
                if ( !recorded )
                {
                    abandonGraph();
                    return frameFault(
                         std::format( "graph '{}' pass '{}': {}", m_Name, pass.Name, recorded.GetError() ), {} );
                }
            }
            for ( uint32_t position = chunk.Desc.FirstPosition; position <= chunk.Desc.LastPosition; ++position )
            {
                const uint32_t p = result.Passes[position].Pass;
                if ( chunk.LostRoot[p] >= 0 )
                    lostRoot[p] = chunk.LostRoot[p];
            }
            m_Report.Faults.insert( m_Report.Faults.end(), chunk.Faults.begin(), chunk.Faults.end() );
            m_Report.FaultCulled.insert( m_Report.FaultCulled.end(), chunk.FaultCulled.begin(),
                                         chunk.FaultCulled.end() );
            m_LateSubstitutions.insert( m_LateSubstitutions.end(), chunk.Late.begin(), chunk.Late.end() );
            if ( !chunk.BackendFailure.empty() )
            {
                abandonGraph();
                return frameFault( std::move( chunk.BackendFailure ), {} );
            }
            const Common::BoolResultStr executed = backend.ExecuteChunk( index );
            if ( !executed )
            {
                abandonGraph();
                return frameFault( std::format( "graph '{}' chunk at pass '{}': {}", m_Name,
                                                result.Passes[chunk.Desc.FirstPosition].Name, executed.GetError() ),
                                   {} );
            }
            discardChunks = discardChunks || !chunk.Faults.empty();
            return Common::MakeSuccess( true );
        };

        for ( const PipeSegment& segment : result.Segments )
        {
            // Amendment B: the prologue has no passes; it records the releases of a fork from the start.
            const bool                  prologue     = segment.FirstPosition == CrossPipeSync::kForkAtGraphStart;
            const std::string           segmentName  = prologue ? std::string( "the graph start" )
                                                                : std::string( result.Passes[segment.FirstPosition].Name );
            const Common::BoolResultStr segmentBegun = backend.BeginPipeSegment( segment );
            if ( !segmentBegun )
            {
                abandonGraph();
                return frameFault( std::format( "graph '{}' segment at pass '{}': {}", m_Name, segmentName,
                                                segmentBegun.GetError() ),
                                   {} );
            }
            // Empty when the only external a fork from the graph start opens is a held read group already in its
            // read state (RDGCompile TrackUse): RecordEpilogueBarriers is never called empty.
            if ( prologue && !result.PrologueBarriers.empty() )
                backend.RecordEpilogueBarriers( result.PrologueBarriers );
            for ( uint32_t position = segment.FirstPosition; !prologue && position <= segment.LastPosition; )
            {
                const int32_t chunk = chunkAt.empty() ? -1 : chunkAt[position];
                // The chunk is recorded (by a helper, or here when none claimed it) before it is replayed or
                // discarded: either reads what its recording wrote.
                if ( chunk >= 0 )
                    chunkTasks->Await( static_cast<uint32_t>( chunk ), !discardChunks );
                if ( chunk >= 0 && !discardChunks )
                {
                    RecordChunk&          replayed = chunks[static_cast<uint32_t>( chunk )];
                    Common::BoolResultStr executed = replayChunk( replayed, static_cast<uint32_t>( chunk ) );
                    if ( !executed )
                        return executed;
                    position = replayed.Desc.LastPosition + 1;
                    continue;
                }
                if ( chunk >= 0 )
                    backend.DiscardChunk( static_cast<uint32_t>( chunk ) );
                const size_t          faultsBefore = m_Report.Faults.size();
                Common::BoolResultStr recorded     = recordPass( position, graphState );
                if ( !recorded )
                    return recorded;
                discardChunks = discardChunks || m_Report.Faults.size() != faultsBefore;
                ++position;
            }
            const Common::BoolResultStr segmentEnded = backend.EndPipeSegment( segment );
            if ( !segmentEnded )
            {
                abandonGraph();
                return frameFault( std::format( "graph '{}' segment at pass '{}': {}", m_Name, segmentName,
                                                segmentEnded.GetError() ),
                                   {} );
            }
        }

        if ( !chunks.empty() )
        {
            backend.EndChunks();
            // RDG-RECORD-CHUNKS: every chunk is recorded (the walk awaited each); the costliest as recorded now.
            chunkTasks->Close();
            CompileCache* const cache = backend.GetCompileCache();
            if ( cache != nullptr && recordMicros != nullptr )
            {
                float maxChunk = 0.0f;
                for ( const RecordChunk& chunk : chunks )
                {
                    float spent = 0.0f;
                    for ( uint32_t at = chunk.Desc.FirstPosition; at <= chunk.Desc.LastPosition; ++at )
                        spent += recordMicros[at];
                    maxChunk = std::max( maxChunk, spent );
                }
                cache->CountRecordChunks( static_cast<uint32_t>( chunks.size() ), chunksByCost, maxChunk );
            }
        }

        // An external whose every writer this frame was lost late: FrameFatal ends the frame (after EndGraph
        // below, so the graph's command buffers stay well formed), InvalidateHistory is listed for its owner.
        ScratchVector<uint32_t> lateFatal;
        std::vector<uint32_t> lateRoots;
        for ( uint32_t r = 0; r < m_Resources.size(); ++r )
        {
            const ResourceRecord& record = m_Resources[r];
            if ( !record.IsExternal() || record.Policy == ExternalFaultPolicy::KeepsContents )
                continue;
            bool                  anyWriter   = false;
            bool                  anySurvivor = false;
            std::vector<uint32_t> roots;
            for ( const CompiledPass& compiledPass : result.Passes )
            {
                for ( const ResourceUse& use : m_Passes[compiledPass.Pass].Uses )
                {
                    if ( use.Resource != r || !IsWriteAccess( use.Usage ) )
                        continue;
                    anyWriter = true;
                    if ( lostRoot[compiledPass.Pass] < 0 )
                        anySurvivor = true;
                    else
                        roots.push_back( static_cast<uint32_t>( lostRoot[compiledPass.Pass] ) );
                }
            }
            if ( !anyWriter || anySurvivor )
                continue;
            if ( record.Policy == ExternalFaultPolicy::InvalidateHistory )
                m_Report.InvalidatedExternals.push_back( r );
            else
            {
                lateFatal.push_back( r );
                lateRoots.insert( lateRoots.end(), roots.begin(), roots.end() );
            }
        }

        // An extracted transient's image is taken before EndGraph releases the graph's hold on it.
        ScratchVector<std::shared_ptr<IPhysicalTexture>> extractedTextures( m_Resources.size() );
        ScratchVector<std::shared_ptr<IPhysicalBuffer>>  extractedBuffers( m_Resources.size() );
        for ( const ExternalFinalState& final : result.ExternalFinalStates )
        {
            const ResourceRecord& record = m_Resources[final.Resource];
            if ( record.IsExternal() )
                continue;
            if ( record.Kind == ResourceKind::Texture )
                extractedTextures[final.Resource] = backend.GetPhysicalTexture( final.Resource );
            else
                extractedBuffers[final.Resource] = backend.GetPhysicalBuffer( final.Resource );
        }

        DESERT_PROFILE_PHASE( phases, "RDG: EndGraph" );
        const Common::BoolResultStr ended = backend.EndGraph( result.FinalBarriers );
        if ( !ended )
            return frameFault( std::format( "graph '{}': {}", m_Name, ended.GetError() ), {} );

        std::string recordError; // the first failed layout write-back; every other one still runs
        for ( const ExternalFinalState& final : result.ExternalFinalStates )
        {
            const ResourceRecord& record = m_Resources[final.Resource];
            if ( record.Kind == ResourceKind::Texture )
            {
                ExternalTexture* target   = record.ExternalTex != nullptr ? record.ExternalTex : record.ExtractTex;
                target->Desc              = record.Texture;
                target->SubresourceStates = final.SubresourceStates;
                // The layouts match the last barrier's (already recorded); the final states add the stages and
                // accesses of the reads merged after it, and the owner checks the image can record them.
                if ( target->RecordStates )
                {
                    const Common::BoolResultStr recorded = target->RecordStates( target->SubresourceStates, true );
                    if ( !recorded && recordError.empty() )
                        recordError = std::format( "graph '{}' texture '{}': {}", m_Name, record.Name,
                                                   recorded.GetError() );
                }
                if ( !record.IsExternal() )
                {
                    target->Physical          = std::move( extractedTextures[final.Resource] );
                    target->CreatedAccessMask = views[final.Resource].AccessMask;
                }
            }
            else
            {
                ExternalBuffer* target = record.ExternalBuf != nullptr ? record.ExternalBuf : record.ExtractBuf;
                target->Desc           = record.Buffer;
                target->State          = final.SubresourceStates.front();
                if ( target->RecordFinalState )
                {
                    const Common::BoolResultStr recorded = target->RecordFinalState( target->State );
                    if ( !recorded && recordError.empty() )
                        recordError =
                             std::format( "graph '{}' buffer '{}': {}", m_Name, record.Name, recorded.GetError() );
                }
                if ( !record.IsExternal() )
                    target->Physical = std::move( extractedBuffers[final.Resource] );
            }
        }
        if ( !recordError.empty() )
            return frameFault( std::move( recordError ), {} );
        if ( !lateFatal.empty() )
        {
            std::string names;
            for ( const uint32_t r : lateFatal )
                std::format_to( std::back_inserter( names ), "{}'{}'", names.empty() ? "" : ", ",
                                m_Resources[r].Name );
            std::sort( lateRoots.begin(), lateRoots.end() );
            lateRoots.erase( std::unique( lateRoots.begin(), lateRoots.end() ), lateRoots.end() );
            FrameFault fault{ std::format( "{} lost every writer to a fault during execution", names ),
                              {},
                              std::move( lateRoots ) };
            for ( const uint32_t r : lateFatal )
                fault.Externals.push_back( MakeFrameFaultExternal( r ) );
            m_Report.Frame = std::move( fault );
        }
        return finish();
    }
} // namespace Desert::Graphic::RDG
