#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace Desert::Graphic::GpuDriven
{
    // One persistent device table of the cache: re-uploaded WHOLE when its host mirror was rebuilt (a scene
    // change, never a frame), registered into every graph and left readable (GpuScene's tables do the same).
    struct DrawCommandCache::Table
    {
        std::string                                     Name;
        std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
        RDG::ExternalBuffer                             Import;
        uint64_t                                        CapacityBytes = 0;
    };

    namespace
    {
        constexpr uint64_t                                kMinTableBytes = 4096u;
        constexpr std::array<const char*, 3>              kPassTableNames{ "PrimitiveDraws", "DrawSlots", "Bins" };
        constexpr std::array<const char*, kMeshPassCount> kPassNames{ "DepthPrepass", "GBuffer", "Forward",
                                                                      "ShadowCascade", "VirtualShadow" };

        std::unique_ptr<DrawCommandCache::Table> MakeTable( std::string name )
        {
            auto table  = std::make_unique<DrawCommandCache::Table>();
            table->Name = std::move( name );
            return table;
        }

        // Registers @p table in @p graph; when @p write, first (re)creates it big enough for @p bytes and queues
        // their upload. Returns the table's ref, left in StorageRead for every later reader.
        Common::ResultStr<RDG::BufferRef> UploadTable( RDG::Builder& graph, DrawCommandCache::Table& table,
                                                       std::span<const std::byte> bytes, const bool write )
        {
            if ( !table.Buffer || ( write && table.CapacityBytes < bytes.size() ) )
            {
                const uint64_t capacity = std::max(
                     { static_cast<uint64_t>( bytes.size() ), table.CapacityBytes * 2u, kMinTableBytes } );
                if ( capacity > std::numeric_limits<uint32_t>::max() )
                    return Common::MakeError<RDG::BufferRef>( std::format(
                         "DrawCommands {}: {} bytes pass the 4 GiB a buffer is sized by", table.Name, capacity ) );
                auto buffer = ShaderResources::StorageBuffer::Create(
                     table.Name, static_cast<uint32_t>( capacity ), 0u, /*persistent=*/true );
                if ( !buffer )
                    return Common::MakeError<RDG::BufferRef>(
                         std::format( "DrawCommands {}: the device buffer of {} bytes could not be created",
                                      table.Name, capacity ) );
                // The old buffer is retired on the frame ring by its API object: a frame in flight still reads it.
                table.Buffer        = std::move( buffer );
                table.CapacityBytes = capacity;
            }
            if ( const auto imported = Renderer::ImportBuffer( table.Buffer, table.Import ); !imported )
                return Common::MakeError<RDG::BufferRef>( std::format( "DrawCommands {}: the table could not be imported: {}",
                                                       table.Name, imported.GetError() ) );
            const RDG::BufferRef ref = graph.RegisterExternal( table.Import, "DrawCommands." + table.Name );
            if ( write && !bytes.empty() )
                graph.QueueBufferUpload( ref, bytes );
            graph.Extract( ref, table.Import, RDG::Access::StorageRead );
            return Common::MakeSuccess( ref );
        }

        template <typename Row>
        std::span<const std::byte> BytesOf( const std::vector<Row>& rows )
        {
            static_assert( sizeof( Row ) % 4u == 0u, "a table row is a whole number of 32-bit words" );
            return std::as_bytes( std::span( rows ) );
        }

        bool IsHidden( const uint64_t mask, const size_t submesh )
        {
            return submesh < 64 && ( ( mask >> submesh ) & 1ull ) != 0;
        }
    } // namespace

    const char* BinFallbackName( const BinFallback fallback )
    {
        switch ( fallback )
        {
            case BinFallback::None:
                return "None";
            case BinFallback::PassNotBinned:
                return "PassNotBinned";
            case BinFallback::NoBinnedCell:
                return "NoBinnedCell";
            case BinFallback::PipelineUnavailable:
                return "PipelineUnavailable";
            case BinFallback::NoMaterialRow:
                return "NoMaterialRow";
            case BinFallback::BoundTextures:
                return "BoundTextures";
            case BinFallback::Count:
                break;
        }
        return "Unknown";
    }

    DrawCommandCache::DrawCommandCache( PassPipelineResolver resolver )
         : m_Resolver( std::move( resolver ) ), m_TransformTable( MakeTable( "SubmeshTransforms" ) )
    {
        for ( uint32_t pass = 0; pass < kMeshPassCount; ++pass )
            for ( size_t table = 0; table < kPassTableNames.size(); ++table )
                m_PassTables[pass][table] =
                     MakeTable( std::format( "{}.{}", kPassNames[pass], kPassTableNames[table] ) );
    }

    DrawCommandCache::~DrawCommandCache() = default;

    void DrawCommandCache::ResolvePipelines( const PrimitiveIndex primitive, PrimitiveRecord& record ) const
    {
        const size_t submeshes = record.Mesh->GetSubmeshes().size();
        for ( uint32_t pass = 0; pass < kMeshPassCount; ++pass )
        {
            auto& pipelines = record.Pipelines[pass];
            auto& fallbacks = record.Fallbacks[pass];
            pipelines.assign( submeshes, nullptr );
            fallbacks.assign( submeshes, BinFallback::None );
            // UE bCastShadow: a primitive that does not cast is no shadow pass's draw at all.
            if ( IsShadowPass( static_cast<MeshPass>( pass ) ) &&
                 ( record.PrimitiveFlags & PrimitiveFlags::kCastsShadow ) == 0u )
                continue;
            for ( size_t si = 0; si < submeshes; ++si )
            {
                if ( IsHidden( record.HiddenSubmeshMask, si ) )
                    continue;
                const PassPipelineResolution resolved =
                     m_Resolver( static_cast<MeshPass>( pass ), primitive, *record.Mesh, static_cast<uint32_t>( si ),
                                 record.MaterialRows[si] );
                pipelines[si] = resolved.Pipeline;
                fallbacks[si] = resolved.Pipeline != nullptr ? BinFallback::None : resolved.Fallback;
            }
        }
    }

    void DrawCommandCache::MarkPassesOf( const PrimitiveRecord& record )
    {
        for ( uint32_t pass = 0; pass < kMeshPassCount; ++pass )
            if ( std::any_of( record.Pipelines[pass].begin(), record.Pipelines[pass].end(),
                              []( const GraphicsPipeline* pipeline ) { return pipeline != nullptr; } ) ||
                 std::any_of( record.Fallbacks[pass].begin(), record.Fallbacks[pass].end(),
                              []( const BinFallback fallback ) { return fallback != BinFallback::None; } ) )
                m_Dirty[pass] = true;
    }

    void DrawCommandCache::AcquireTransforms( const Mesh& mesh )
    {
        if ( auto it = m_MeshTransforms.find( &mesh ); it != m_MeshTransforms.end() )
        {
            ++it->second.Users;
            return;
        }
        const auto& submeshes = mesh.GetSubmeshes();
        const auto  count     = static_cast<uint32_t>( submeshes.size() );
        uint32_t    first     = static_cast<uint32_t>( m_SubmeshTransforms.size() );
        // First fit in the free ranges, else the end of the table.
        auto fit = std::find_if( m_FreeTransformRanges.begin(), m_FreeTransformRanges.end(),
                                 [count]( const glm::uvec2& range ) { return range.y >= count; } );
        if ( fit != m_FreeTransformRanges.end() )
        {
            first = fit->x;
            fit->x += count;
            fit->y -= count;
            if ( fit->y == 0u )
                m_FreeTransformRanges.erase( fit );
        }
        else
            m_SubmeshTransforms.resize( m_SubmeshTransforms.size() + count );
        for ( uint32_t si = 0; si < count; ++si )
            m_SubmeshTransforms[first + si] = submeshes[si].Transform;
        m_MeshTransforms.emplace( &mesh, MeshTransformRows{ .FirstRow = first, .RowCount = count, .Users = 1 } );
        m_TransformsUnuploaded = true;
    }

    void DrawCommandCache::ReleaseTransforms( const Mesh& mesh )
    {
        const auto it = m_MeshTransforms.find( &mesh );
        if ( it == m_MeshTransforms.end() || --it->second.Users > 0u )
            return;
        const glm::uvec2 freed{ it->second.FirstRow, it->second.RowCount };
        m_MeshTransforms.erase( it );
        if ( freed.y == 0u )
            return;
        m_FreeTransformRanges.push_back( freed );
        std::sort( m_FreeTransformRanges.begin(), m_FreeTransformRanges.end(),
                   []( const glm::uvec2& a, const glm::uvec2& b ) { return a.x < b.x; } );
        std::vector<glm::uvec2> merged;
        merged.reserve( m_FreeTransformRanges.size() );
        for ( const glm::uvec2& range : m_FreeTransformRanges )
        {
            if ( !merged.empty() && merged.back().x + merged.back().y == range.x )
                merged.back().y += range.y;
            else
                merged.push_back( range );
        }
        m_FreeTransformRanges = std::move( merged );
    }

    Common::BoolResultStr DrawCommandCache::AddPrimitive( const PrimitiveIndex     primitive,
                                                          const PrimitiveDrawDesc& desc )
    {
        if ( primitive == kInvalidIndex )
            return Common::MakeError( "DrawCommands: AddPrimitive of the invalid primitive index" );
        if ( desc.Mesh == nullptr )
            return Common::MakeFormattedError( "DrawCommands: primitive {} has no mesh", primitive );
        const Mesh& mesh = *desc.Mesh;
        const MeshGeometry& geometry = mesh.GetGeometry();
        if ( !geometry.IsResident() || geometry.IndexCount == 0u )
            return Common::MakeFormattedError(
                 "DrawCommands: primitive {}: the mesh has no {} on the GPU (a binned draw is indexed)", primitive,
                 geometry.IsResident() ? "indices" : "vertices" );
        const auto& submeshes = mesh.GetSubmeshes();
        if ( submeshes.empty() )
            return Common::MakeFormattedError( "DrawCommands: primitive {}: the mesh has no submesh", primitive );
        if ( desc.MaterialRows.empty() )
            return Common::MakeFormattedError( "DrawCommands: primitive {} has no material slot", primitive );
        if ( desc.InstanceCount == 0u )
            return Common::MakeFormattedError( "DrawCommands: primitive {} has no instance", primitive );
        if ( desc.FirstInstance == kInvalidIndex ||
             uint64_t{ desc.FirstInstance } + desc.InstanceCount > uint64_t{ kInvalidIndex } )
            return Common::MakeFormattedError(
                 "DrawCommands: primitive {} names no GpuScene instance range (first {}, count {})", primitive,
                 desc.FirstInstance, desc.InstanceCount );

        PrimitiveRecord record;
        record.Mesh              = &mesh;
        record.FirstInstance     = desc.FirstInstance;
        record.InstanceCount     = desc.InstanceCount;
        record.HiddenSubmeshMask = desc.HiddenSubmeshMask;
        record.PrimitiveFlags    = desc.PrimitiveFlags;
        record.MaterialRows.resize( submeshes.size() );
        const uint32_t indexCount = geometry.IndexCount;
        for ( size_t si = 0; si < submeshes.size(); ++si )
        {
            record.MaterialRows[si] = desc.MaterialRows[SectionMaterialSlot( si, desc.MaterialRows.size() )];
            const auto levels       = static_cast<uint32_t>( std::max<size_t>( submeshes[si].LODs.size(), 1u ) );
            record.LodCount         = std::max( record.LodCount, levels );
            // RenderMesh's check, made once here because the GPU never reports an index read past the buffer.
            for ( uint32_t lod = 0; lod < levels; ++lod )
            {
                const SubmeshIndexRange range = SubmeshIndices( submeshes[si], lod );
                if ( uint64_t{ range.First } + range.Count > indexCount )
                    return Common::MakeFormattedError(
                         "DrawCommands: primitive {}: submesh {} LOD {} reads indices [{}, {}) of a {}-index "
                         "buffer",
                         primitive, si, lod, range.First, uint64_t{ range.First } + range.Count, indexCount );
            }
        }
        ResolvePipelines( primitive, record );

        RemovePrimitive( primitive );
        if ( m_Primitives.size() <= primitive )
            m_Primitives.resize( size_t{ primitive } + 1u );
        AcquireTransforms( mesh );
        MarkPassesOf( record );
        m_Primitives[primitive] = std::move( record );
        return Common::MakeSuccess( true );
    }

    void DrawCommandCache::RemovePrimitive( const PrimitiveIndex primitive )
    {
        if ( primitive >= m_Primitives.size() || !m_Primitives[primitive] )
            return;
        MarkPassesOf( *m_Primitives[primitive] );
        ReleaseTransforms( *m_Primitives[primitive]->Mesh );
        m_Primitives[primitive].reset();
    }

    void DrawCommandCache::InvalidatePipelines()
    {
        for ( size_t primitive = 0; primitive < m_Primitives.size(); ++primitive )
        {
            auto& record = m_Primitives[primitive];
            if ( !record )
                continue;
            const auto before         = record->Pipelines;
            const auto fallbacksBefore = record->Fallbacks;
            ResolvePipelines( static_cast<PrimitiveIndex>( primitive ), *record );
            for ( uint32_t pass = 0; pass < kMeshPassCount; ++pass )
                if ( before[pass] != record->Pipelines[pass] || fallbacksBefore[pass] != record->Fallbacks[pass] )
                    m_Dirty[pass] = true;
        }
    }

    bool DrawCommandCache::NeedsRebuild() const
    {
        return std::any_of( m_Dirty.begin(), m_Dirty.end(), []( const bool dirty ) { return dirty; } );
    }

    void DrawCommandCache::Rebuild()
    {
        for ( uint32_t pass = 0; pass < kMeshPassCount; ++pass )
        {
            if ( !m_Dirty[pass] )
                continue;
            RebuildPass( static_cast<MeshPass>( pass ) );
            m_Dirty[pass]      = false;
            m_Unuploaded[pass] = true;
        }
    }

    void DrawCommandCache::RebuildPass( const MeshPass pass )
    {
        const auto     passIndex = static_cast<uint32_t>( pass );
        PassDrawTable& table     = m_Passes[passIndex];
        table                    = PassDrawTable{};
        table.PrimitiveDraws.assign( m_Primitives.size(), GpuPrimitivePassDraws{} );

        // A bin is one index range of the bound geometry under one PSO: every slot that draws exactly that shares
        // it, across primitives (the instancing of the binned path) and across LOD levels a submesh clamps to the
        // same range. The range is pool-absolute (the mesh's span base added), so two pooled meshes never share a
        // bin while all of them share their arena's group.
        using BindingKey = std::tuple<const VertexBuffer*, uint64_t, const VertexBuffer*, uint64_t,
                                      const IndexBuffer*, uint64_t>;
        const auto keyOf = []( const MeshGeometryBinding& binding )
        {
            return BindingKey{ binding.Vertices, binding.VertexOffset, binding.Streams,
                               binding.StreamOffset, binding.Indices, binding.IndexOffset };
        };
        using BinKey = std::tuple<const GraphicsPipeline*, BindingKey, uint32_t, uint32_t, uint32_t>;
        struct BinDraft
        {
            GpuDrawBin              Row{};
            uint32_t                GroupOrdinal = 0; // order of first appearance of its (pipeline, binding)
            uint32_t                Ordinal      = 0; // order of first appearance of the bin
            const GraphicsPipeline* Pipeline     = nullptr;
            MeshGeometryBinding     Binding;
            uint64_t                Capacity     = 0;
        };
        std::map<BinKey, uint32_t>                                         binOf;
        std::map<std::pair<const GraphicsPipeline*, BindingKey>, uint32_t> groupOf;
        std::vector<BinDraft>                                               drafts;

        for ( size_t primitive = 0; primitive < m_Primitives.size(); ++primitive )
        {
            const auto& record = m_Primitives[primitive];
            if ( !record )
                continue;
            const auto& pipelines = record->Pipelines[passIndex];
            for ( const BinFallback fallback : record->Fallbacks[passIndex] )
                if ( fallback != BinFallback::None )
                    ++table.Fallbacks[static_cast<uint32_t>( fallback )];
            if ( std::none_of( pipelines.begin(), pipelines.end(),
                               []( const GraphicsPipeline* pipeline ) { return pipeline != nullptr; } ) )
                continue;
            const auto&    submeshes        = record->Mesh->GetSubmeshes();
            const auto     count            = static_cast<uint32_t>( submeshes.size() );
            const uint32_t transforms       = m_MeshTransforms.at( record->Mesh ).FirstRow;
            table.PrimitiveDraws[primitive] = GpuPrimitivePassDraws{
                 .FirstDrawSlot = static_cast<uint32_t>( table.DrawSlots.size() ),
                 .SubmeshCount  = count,
                 .LodCount      = record->LodCount,
            };
            for ( uint32_t lod = 0; lod < record->LodCount; ++lod )
            {
                for ( uint32_t si = 0; si < count; ++si )
                {
                    GpuDrawSlot slot{};
                    slot.Bin              = kInvalidIndex;
                    slot.MaterialRow      = record->MaterialRows[si];
                    slot.SubmeshTransform = transforms + si;
                    if ( const GraphicsPipeline* pipeline = pipelines[si]; pipeline != nullptr )
                    {
                        const Submesh&            submesh    = submeshes[si];
                        const SubmeshIndexRange   range      = SubmeshIndices( submesh, lod );
                        const MeshGeometry&       geometry   = record->Mesh->GetGeometry();
                        const MeshGeometryBinding binding    = geometry.SharedBinding();
                        const uint32_t            firstIndex = range.First + geometry.SharedFirstIndex();
                        const uint32_t vertexOffset = submesh.VertexOffset + geometry.SharedBaseVertex();
                        const BinKey   key{ pipeline, keyOf( binding ), firstIndex, range.Count, vertexOffset };
                        auto [it, added] = binOf.try_emplace( key, static_cast<uint32_t>( drafts.size() ) );
                        if ( added )
                        {
                            const auto [group, newGroup] = groupOf.try_emplace(
                                 std::pair{ pipeline, keyOf( binding ) }, static_cast<uint32_t>( groupOf.size() ) );
                            BinDraft draft;
                            draft.Row.IndexCount   = range.Count;
                            draft.Row.FirstIndex   = firstIndex;
                            draft.Row.VertexOffset = static_cast<int32_t>( vertexOffset );
                            draft.GroupOrdinal     = group->second;
                            draft.Ordinal          = static_cast<uint32_t>( drafts.size() );
                            draft.Pipeline         = pipeline;
                            draft.Binding          = binding;
                            drafts.push_back( draft );
                        }
                        drafts[it->second].Capacity += record->InstanceCount;
                        slot.Bin = it->second; // the draft's ordinal; remapped to the sorted row below
                    }
                    table.DrawSlots.push_back( slot );
                }
            }
        }

        // Sort the bins by group (first appearance, so the order is the scene's, not the allocator's), keeping
        // first appearance inside a group: one group is one contiguous run, ONE multi-draw.
        std::vector<uint32_t> order( drafts.size() );
        for ( uint32_t i = 0; i < order.size(); ++i )
            order[i] = i;
        std::sort( order.begin(), order.end(),
                   [&]( const uint32_t a, const uint32_t b )
                   {
                       return std::tie( drafts[a].GroupOrdinal, drafts[a].Ordinal ) <
                              std::tie( drafts[b].GroupOrdinal, drafts[b].Ordinal );
                   } );
        std::vector<uint32_t> rowOf( drafts.size() );
        uint64_t              visible = 0;
        table.Bins.reserve( drafts.size() );
        for ( uint32_t row = 0; row < order.size(); ++row )
        {
            BinDraft& draft   = drafts[order[row]];
            rowOf[order[row]] = row;
            if ( table.Groups.empty() || table.Groups.back().Pipeline != draft.Pipeline ||
                 table.Groups.back().Binding != draft.Binding )
                table.Groups.push_back( IndirectGroup{
                     .Pipeline = draft.Pipeline, .Binding = draft.Binding, .FirstBin = row, .BinCount = 0 } );
            ++table.Groups.back().BinCount;
            draft.Row.Group        = static_cast<uint32_t>( table.Groups.size() - 1u );
            draft.Row.FirstVisible = static_cast<uint32_t>( visible );
            draft.Row.Capacity     = static_cast<uint32_t>( std::min<uint64_t>( draft.Capacity, UINT32_MAX ) );
            visible += draft.Capacity;
            table.Bins.push_back( draft.Row );
        }
        for ( GpuDrawSlot& slot : table.DrawSlots )
            if ( slot.Bin != kInvalidIndex )
                slot.Bin = rowOf[slot.Bin];
        // gl_InstanceIndex = FirstVisible + instance is a uint32 the GPU computes: a list past it cannot be drawn.
        // Clamped, not wrapped; AddUploadPass refuses the pass so the overflow is named, not drawn wrong.
        table.VisibleCapacity = static_cast<uint32_t>( std::min<uint64_t>( visible, UINT32_MAX ) );
        if ( table.VisibleCapacity == UINT32_MAX )
            return;
    }

    const PassDrawTable& DrawCommandCache::Pass( const MeshPass pass ) const
    {
        return m_Passes[static_cast<uint32_t>( pass )];
    }

    bool DrawCommandCache::SubmeshesBinned( const MeshPass pass, const PrimitiveIndex primitive,
                                            const uint64_t hiddenSubmeshMask ) const
    {
        if ( primitive >= m_Primitives.size() || !m_Primitives[primitive] )
            return false;
        const PrimitiveRecord& record    = *m_Primitives[primitive];
        const auto&            pipelines = record.Pipelines[static_cast<uint32_t>( pass )];
        bool                   any       = false;
        for ( size_t si = 0; si < pipelines.size(); ++si )
        {
            if ( IsHidden( hiddenSubmeshMask, si ) )
                continue;
            // A submesh the primitive hides has no bin: the CPU record must not leave it to one.
            if ( pipelines[si] == nullptr || IsHidden( record.HiddenSubmeshMask, si ) )
                return false;
            any = true;
        }
        return any;
    }

    Common::ResultStr<DrawCommandBuffers> DrawCommandCache::AddUploadPass( RDG::Builder& graph )
    {
        if ( NeedsRebuild() )
            return Common::MakeError<DrawCommandBuffers>( "DrawCommands: Upload with a Rebuild pending" );
        DrawCommandBuffers out;
        for ( uint32_t pass = 0; pass < kMeshPassCount; ++pass )
        {
            const PassDrawTable& table = m_Passes[pass];
            if ( table.VisibleCapacity == UINT32_MAX )
                return Common::MakeError<DrawCommandBuffers>( std::format(
                     "DrawCommands {}: the pass's bins hold 4G or more visible instances", kPassNames[pass] ) );
            const bool write = m_Unuploaded[pass];
            auto&      gpu   = m_PassTables[pass];
            auto       draws = UploadTable( graph, *gpu[0], BytesOf( table.PrimitiveDraws ), write );
            if ( !draws )
                return Common::MakeError<DrawCommandBuffers>( draws.GetError() );
            auto slots = UploadTable( graph, *gpu[1], BytesOf( table.DrawSlots ), write );
            if ( !slots )
                return Common::MakeError<DrawCommandBuffers>( slots.GetError() );
            auto bins = UploadTable( graph, *gpu[2], BytesOf( table.Bins ), write );
            if ( !bins )
                return Common::MakeError<DrawCommandBuffers>( bins.GetError() );
            out.Passes[pass] = PassDrawBuffers{ .PrimitiveDraws  = draws.GetValue(),
                                                .DrawSlots       = slots.GetValue(),
                                                .Bins            = bins.GetValue() };
        }
        auto transforms =
             UploadTable( graph, *m_TransformTable, BytesOf( m_SubmeshTransforms ), m_TransformsUnuploaded );
        if ( !transforms )
            return Common::MakeError<DrawCommandBuffers>( transforms.GetError() );
        out.SubmeshTransforms = transforms.GetValue();
        // Cleared only once every table is in the graph: a refused upload re-sends all of them next frame.
        m_Unuploaded.fill( false );
        m_TransformsUnuploaded = false;
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Graphic::GpuDriven
