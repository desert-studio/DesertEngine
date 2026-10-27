#include <Engine/Graphic/GpuDriven/GpuScene.hpp>

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <string>

namespace
{
    // The slot names the upload pass binds, spelled once (RDGShaderParameters.hpp).
    struct GpuSceneUploadShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, GpuSceneUploadSource );
        DESERT_SHADER_PARAMETER( StorageBuffer, GpuSceneUploadDestination );
        static constexpr std::array kAll{ Desert::Graphic::RDG::Erase( GpuSceneUploadSource ),
                                          Desert::Graphic::RDG::Erase( GpuSceneUploadDestination ) };
    };

    constexpr const char* kUploadShader      = "GpuSceneUpload"; // Programs/GpuDriven/GpuSceneUpload.shader
    constexpr uint32_t    kUploadGroupSize   = 64u;
    constexpr uint32_t    kMaxGroupsPerAxis  = 65535u;
    constexpr uint32_t    kUploadHeaderWords = 4u;
    constexpr uint32_t    kMinCapacityRows   = 64u;

    template <typename Row>
    constexpr uint32_t WordsOf()
    {
        static_assert( sizeof( Row ) % 4u == 0u, "a table row is a whole number of 32-bit words" );
        return static_cast<uint32_t>( sizeof( Row ) / 4u );
    }

    // First fit in @p free, else the end of @p storage (grown by @p count rows).
    template <typename Row>
    uint32_t AllocateRange( std::vector<glm::uvec2>& free, std::vector<Row>& storage, const uint32_t count )
    {
        for ( auto it = free.begin(); it != free.end(); ++it )
        {
            if ( it->y < count )
                continue;
            const uint32_t first = it->x;
            it->x += count;
            it->y -= count;
            if ( it->y == 0u )
                free.erase( it );
            return first;
        }
        const auto first = static_cast<uint32_t>( storage.size() );
        storage.resize( storage.size() + count );
        return first;
    }

    void FreeRange( std::vector<glm::uvec2>& free, const uint32_t first, const uint32_t count )
    {
        if ( count == 0u )
            return;
        free.push_back( { first, count } );
        std::sort( free.begin(), free.end(),
                   []( const glm::uvec2& a, const glm::uvec2& b ) { return a.x < b.x; } );
        std::vector<glm::uvec2> merged;
        merged.reserve( free.size() );
        for ( const glm::uvec2& range : free )
        {
            if ( !merged.empty() && merged.back().x + merged.back().y == range.x )
                merged.back().y += range.y;
            else
                merged.push_back( range );
        }
        free = std::move( merged );
    }

    void SortUnique( std::vector<uint32_t>& rows )
    {
        std::sort( rows.begin(), rows.end() );
        rows.erase( std::unique( rows.begin(), rows.end() ), rows.end() );
    }
} // namespace

namespace Desert::Graphic::GpuDriven
{
    struct GpuScene::Table
    {
        const char* Name     = "";
        uint32_t    Binding  = 0;
        uint32_t    RowWords = 0;

        std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
        RDG::ExternalBuffer                             Import;
        uint32_t                                        CapacityRows = 0;
        // The buffer was (re)created: every row of the CPU image goes up, not only the dirty ones.
        bool Full = false;
    };

    struct GpuSceneUploadPipeline
    {
        std::shared_ptr<ComputePipeline> Pipeline;
        ShaderBindingLayoutCache         Layout;
    };

    namespace
    {
        std::unique_ptr<GpuScene::Table> MakeTable( const char* name, const uint32_t binding,
                                                    const uint32_t rowWords )
        {
            auto table      = std::make_unique<GpuScene::Table>();
            table->Name     = name;
            table->Binding  = binding;
            table->RowWords = rowWords;
            return table;
        }

        Common::ResultStr<std::shared_ptr<ComputePipeline>> EnsurePipeline( GpuSceneUploadPipeline& upload )
        {
            if ( upload.Pipeline )
                return upload.Pipeline;
            Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> shader =
                 service ? service->GetByName( std::string( kUploadShader ) ) : nullptr;
            if ( !shader )
                return Common::MakeError( std::format(
                     "the shader '{}' did not compile - expected Editor/Resources/Shaders/Programs/GpuDriven/"
                     "GpuSceneUpload.shader",
                     kUploadShader ) );
            if ( const Common::BoolResultStr valid =
                      upload.Layout.Validate( shader, GpuSceneUploadShaderParameters::kAll );
                 !valid )
                return Common::MakeError( std::format( "'{}': {}", kUploadShader, valid.GetError() ) );
            const auto built =
                 ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( kUploadShader ) } );
            if ( !built )
                return Common::MakeError(
                     std::format( "the '{}' pipeline was not built: {}", kUploadShader, built.GetError() ) );
            upload.Pipeline = built.GetValue();
            return upload.Pipeline;
        }

        // Scatters @p rows (indices into the CPU image @p image; empty + @p contiguous = rows [0, rowCount)) of
        // one table into its persistent buffer, growing it first when @p rowCount passes the capacity (then every
        // row goes up). Returns the table's ref in @p graph, left in StorageRead.
        Common::ResultStr<RDG::BufferRef> UploadTable( RDG::Builder& graph, GpuScene::Table& table,
                                                       GpuSceneUploadPipeline& upload, const uint32_t rowCount,
                                                       const std::byte* image, std::span<const uint32_t> rows,
                                                       bool contiguous )
        {
            const uint32_t needed = std::max( rowCount, 1u );
            if ( !table.Buffer || table.CapacityRows < needed )
            {
                const uint32_t capacity = std::max( { needed, table.CapacityRows * 2u, kMinCapacityRows } );
                const uint64_t bytes    = static_cast<uint64_t>( capacity ) * table.RowWords * 4u;
                if ( bytes > std::numeric_limits<uint32_t>::max() )
                    return Common::MakeError(
                         std::format( "GPUScene {}: {} rows ({} bytes) pass the 4 GiB a buffer "
                                      "is sized by",
                                      table.Name, capacity, bytes ) );
                auto buffer = ShaderResources::StorageBuffer::Create( table.Name, static_cast<uint32_t>( bytes ),
                                                                      table.Binding, /*persistent=*/true );
                if ( !buffer )
                    return Common::MakeError( std::format( "GPUScene {}: the device buffer of {} rows ({} bytes) "
                                                           "could not be created",
                                                           table.Name, capacity, bytes ) );
                // The old buffer is retired on the frame ring by its API object: a frame in flight still reads it.
                table.Buffer       = std::move( buffer );
                table.CapacityRows = capacity;
                table.Full         = true;
            }
            if ( const auto imported = Renderer::ImportBuffer( table.Buffer, table.Import ); !imported )
                return Common::MakeError( std::format( "GPUScene {}: the table could not be imported: {}",
                                                       table.Name, imported.GetError() ) );
            const RDG::BufferRef ref =
                 graph.RegisterExternal( table.Import, std::string( "GPUScene." ) + table.Name );

            if ( table.Full )
            {
                contiguous = true;
                rows       = {};
            }
            const uint32_t count = contiguous ? rowCount : static_cast<uint32_t>( rows.size() );
            if ( count > 0u )
            {
                const auto pipeline = EnsurePipeline( upload );
                if ( !pipeline )
                    return Common::MakeError( std::format( "GPUScene {}: {}", table.Name, pipeline.GetError() ) );

                const uint32_t        rowBytes = table.RowWords * 4u;
                std::vector<uint32_t> source( kUploadHeaderWords + ( contiguous ? 0u : count ) +
                                              static_cast<size_t>( count ) * table.RowWords );
                source[0]      = count;
                source[1]      = table.RowWords;
                source[2]      = contiguous ? 0u : 1u;
                uint32_t* data = source.data() + kUploadHeaderWords;
                if ( contiguous )
                    std::memcpy( data, image, static_cast<size_t>( count ) * rowBytes );
                else
                {
                    std::memcpy( data, rows.data(), rows.size() * sizeof( uint32_t ) );
                    data += count;
                    for ( const uint32_t row : rows )
                    {
                        std::memcpy( data, image + static_cast<size_t>( row ) * rowBytes, rowBytes );
                        data += table.RowWords;
                    }
                }
                const RDG::BufferRef staging =
                     graph.CreateBuffer( RDG::BufferDesc{ source.size() * sizeof( uint32_t ) },
                                         std::string( "GPUScene.Upload." ) + table.Name );
                graph.QueueBufferUpload( staging, std::as_bytes( std::span( source ) ) );

                const std::shared_ptr<ComputePipeline> compute = pipeline.GetValue();
                const auto&               layout = upload.Layout.Get( compute->GetSpecification().Shader );
                const RDG::OtherRouteFill fill   = Renderer::GetPipelineRouteFill( *compute );
                const uint64_t            words  = static_cast<uint64_t>( count ) * table.RowWords;
                const auto groups = static_cast<uint32_t>( ( words + kUploadGroupSize - 1u ) / kUploadGroupSize );
                const uint32_t groupsX = std::min( groups, kMaxGroupsPerAxis );
                const uint32_t groupsY = ( groups + groupsX - 1u ) / groupsX;
                graph.AddPass(
                     std::string( "GPUScene: Upload " ) + table.Name, RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( layout, fill )
                              .Storage( GpuSceneUploadShaderParameters::GpuSceneUploadSource, staging,
                                        RDG::Access::StorageRead )
                              .Storage( GpuSceneUploadShaderParameters::GpuSceneUploadDestination, ref,
                                        RDG::Access::StorageWrite );
                     },
                     [compute, groupsX, groupsY]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::DispatchCompute( bindings, *compute, groupsX, groupsY, 1 );
                     } );
            }
            // Every later reader binds the table by descriptor (or imports it into its own graph): the graph
            // leaves it readable, which orders those reads after this scatter and the next scatter after them.
            graph.Extract( ref, table.Import, RDG::Access::StorageRead );
            table.Full = false;
            return ref;
        }
    } // namespace

    GpuScene::GpuScene()
         : m_PrimitiveTable(
                MakeTable( "Primitives", Core::kGpuScenePrimitivesBinding, WordsOf<GpuScenePrimitive>() ) ),
           m_InstanceTable( MakeTable( "Instances", 0u, WordsOf<GpuSceneInstance>() ) ),
           m_LodTable( MakeTable( "LodScreenSizes", 0u, 1u ) ),
           m_WorldBoundsTable( MakeTable( "WorldBounds", 0u, WordsOf<GpuSceneWorldBounds>() ) ),
           m_BonesTable( MakeTable( "ObjectBones", Core::kObjectBonesBinding, WordsOf<glm::mat4>() ) ),
           m_Upload( std::make_unique<GpuSceneUploadPipeline>() )
    {
    }

    GpuScene::~GpuScene() = default;

    void GpuScene::MarkPrimitive( const PrimitiveIndex primitive )
    {
        m_DirtyPrimitives.push_back( primitive );
    }

    void GpuScene::WriteInstances( const PrimitiveIndex primitive, const PrimitiveDesc& desc )
    {
        GpuScenePrimitive& row = m_Primitives[primitive];
        const auto count       = static_cast<uint32_t>( std::max<size_t>( desc.InstanceToPrimitive.size(), 1u ) );
        if ( row.InstanceCount != count )
        {
            for ( uint32_t i = 0; i < row.InstanceCount; ++i )
            {
                m_Instances[row.FirstInstance + i] = GpuSceneInstance{};
                m_DirtyInstances.push_back( row.FirstInstance + i );
            }
            FreeRange( m_FreeInstanceRanges, row.FirstInstance, row.InstanceCount );
            row.FirstInstance = AllocateRange( m_FreeInstanceRanges, m_Instances, count );
            row.InstanceCount = count;
        }
        const uint32_t boundsRow = ( desc.Flags & PrimitiveFlags::kWorldBoundsOverride ) != 0u
                                        ? m_WorldBoundsRow[primitive]
                                        : kInvalidIndex;
        for ( uint32_t i = 0; i < count; ++i )
        {
            GpuSceneInstance& instance = m_Instances[row.FirstInstance + i];
            instance.InstanceToPrimitive =
                 desc.InstanceToPrimitive.empty() ? glm::mat4( 1.0f ) : desc.InstanceToPrimitive[i];
            instance.Primitive         = primitive;
            instance.CullDistanceStart = desc.CullDistanceStart;
            instance.CullDistanceEnd   = desc.CullDistanceEnd;
            instance.WorldBoundsRow    = boundsRow;
            m_DirtyInstances.push_back( row.FirstInstance + i );
        }
    }

    void GpuScene::WriteLodScreenSizes( const PrimitiveIndex primitive, const std::span<const float> sizes )
    {
        // No sizes = no LOD chain: LodCount 1 and no LodScreenSizes range (FirstLodScreenSize kInvalidIndex).
        GpuScenePrimitive& row  = m_Primitives[primitive];
        const uint32_t     held = row.FirstLodScreenSize != kInvalidIndex ? row.LodCount : 0u;
        const auto         want = static_cast<uint32_t>( sizes.size() );
        if ( held != want )
        {
            if ( held != 0u )
                FreeRange( m_FreeLodRanges, row.FirstLodScreenSize, held );
            row.FirstLodScreenSize = want != 0u ? AllocateRange( m_FreeLodRanges, m_LodScreenSizes, want )
                                                : kInvalidIndex;
        }
        row.LodCount = std::max( want, 1u );
        for ( uint32_t i = 0; i < want; ++i )
        {
            m_LodScreenSizes[row.FirstLodScreenSize + i] = sizes[i];
            m_DirtyLodScreenSizes.push_back( row.FirstLodScreenSize + i );
        }
    }

    PrimitiveIndex GpuScene::AddPrimitive( const PrimitiveDesc& desc )
    {
        PrimitiveIndex primitive;
        if ( !m_FreePrimitives.empty() )
        {
            primitive = m_FreePrimitives.back();
            m_FreePrimitives.pop_back();
        }
        else
        {
            primitive = static_cast<PrimitiveIndex>( m_Primitives.size() );
            m_Primitives.emplace_back();
            m_Live.push_back( 0u );
            m_WorldBoundsRow.push_back( kInvalidIndex );
            m_MovedMark.push_back( 0u );
            m_UndoMark.push_back( 0u );
        }
        m_Live[primitive]      = 1u;
        GpuScenePrimitive& row = m_Primitives[primitive];
        row                    = GpuScenePrimitive{};
        row.FirstLodScreenSize = kInvalidIndex;
        row.LocalToWorld       = desc.LocalToWorld;
        row.PrevLocalToWorld   = desc.LocalToWorld; // added this frame: no object motion
        UpdatePrimitive( primitive, desc );
        return primitive;
    }

    void GpuScene::UpdatePrimitive( const PrimitiveIndex primitive, const PrimitiveDesc& desc )
    {
        if ( !IsValid( primitive ) )
            return;
        SetTransform( primitive, desc.LocalToWorld );
        GpuScenePrimitive& row    = m_Primitives[primitive];
        const glm::vec3    centre = ( desc.LocalBoundsMin + desc.LocalBoundsMax ) * 0.5f;
        const glm::vec3    extent = ( desc.LocalBoundsMax - desc.LocalBoundsMin ) * 0.5f;
        row.LocalBoundsCenter     = glm::vec4( centre, desc.BoundsPad );
        row.LocalBoundsExtent     = glm::vec4( extent, 0.0f );
        row.LodBias               = desc.LodBias;
        row.ForcedLod             = desc.ForcedLod;
        row.Flags                 = desc.Flags;
        row.Entity                = desc.Entity;
        WriteLodScreenSizes( primitive, desc.LodScreenSizes );
        WriteInstances( primitive, desc );
        MarkPrimitive( primitive );
    }

    void GpuScene::RemovePrimitive( const PrimitiveIndex primitive )
    {
        if ( !IsValid( primitive ) )
            return;
        GpuScenePrimitive& row = m_Primitives[primitive];
        for ( uint32_t i = 0; i < row.InstanceCount; ++i )
        {
            m_Instances[row.FirstInstance + i] = GpuSceneInstance{};
            m_DirtyInstances.push_back( row.FirstInstance + i );
        }
        FreeRange( m_FreeInstanceRanges, row.FirstInstance, row.InstanceCount );
        if ( row.FirstLodScreenSize != kInvalidIndex )
            FreeRange( m_FreeLodRanges, row.FirstLodScreenSize, row.LodCount );
        if ( m_WorldBoundsRow[primitive] != kInvalidIndex )
        {
            m_FreeWorldBounds.push_back( m_WorldBoundsRow[primitive] );
            m_WorldBoundsRow[primitive] = kInvalidIndex;
        }
        row               = GpuScenePrimitive{};
        m_Live[primitive] = 0u;
        m_FreePrimitives.push_back( primitive );
        MarkPrimitive( primitive );
    }

    bool GpuScene::IsValid( const PrimitiveIndex primitive ) const
    {
        return primitive < m_Live.size() && m_Live[primitive] != 0u;
    }

    void GpuScene::SetTransform( const PrimitiveIndex primitive, const glm::mat4& world )
    {
        if ( !IsValid( primitive ) )
            return;
        GpuScenePrimitive& row = m_Primitives[primitive];
        if ( row.LocalToWorld == world )
            return;
        JournalTransform( primitive );
        row.PrevLocalToWorld = row.LocalToWorld;
        row.LocalToWorld     = world;
        MarkPrimitive( primitive );
        if ( m_MovedMark[primitive] == 0u )
        {
            m_MovedMark[primitive] = 1u;
            m_MovedLastFrame.push_back( primitive );
        }
    }

    void GpuScene::SetSkinning( const PrimitiveIndex primitive, const PrimitiveSkinning& skinning )
    {
        if ( !IsValid( primitive ) )
            return;
        GpuScenePrimitive& row = m_Primitives[primitive];
        const auto         lo  = static_cast<uint32_t>( skinning.SkinCacheAddress & 0xffffffffu );
        const auto         hi  = static_cast<uint32_t>( skinning.SkinCacheAddress >> 32u );
        if ( row.BoneOffset != skinning.BoneOffset || row.PrevBoneOffset != skinning.PrevBoneOffset ||
             row.SkinCacheAddressLo != lo || row.SkinCacheAddressHi != hi ||
             row.SkinCacheVertexCount != skinning.SkinCacheVertexCount )
        {
            row.BoneOffset           = skinning.BoneOffset;
            row.PrevBoneOffset       = skinning.PrevBoneOffset;
            row.SkinCacheAddressLo   = lo;
            row.SkinCacheAddressHi   = hi;
            row.SkinCacheVertexCount = skinning.SkinCacheVertexCount;
            MarkPrimitive( primitive );
        }

        uint32_t& boundsRow = m_WorldBoundsRow[primitive];
        if ( skinning.PoseBounds )
        {
            if ( boundsRow == kInvalidIndex )
            {
                if ( !m_FreeWorldBounds.empty() )
                {
                    boundsRow = m_FreeWorldBounds.back();
                    m_FreeWorldBounds.pop_back();
                }
                else
                {
                    boundsRow = static_cast<uint32_t>( m_WorldBounds.size() );
                    m_WorldBounds.emplace_back();
                }
            }
            m_WorldBounds[boundsRow] = *skinning.PoseBounds;
            m_DirtyWorldBounds.push_back( boundsRow );
        }
        else if ( boundsRow != kInvalidIndex )
        {
            m_FreeWorldBounds.push_back( boundsRow );
            boundsRow = kInvalidIndex;
        }
        // The instances name the pose box only while the primitive culls by it.
        const uint32_t named =
             ( row.Flags & PrimitiveFlags::kWorldBoundsOverride ) != 0u ? boundsRow : kInvalidIndex;
        for ( uint32_t i = 0; i < row.InstanceCount; ++i )
        {
            GpuSceneInstance& instance = m_Instances[row.FirstInstance + i];
            if ( instance.WorldBoundsRow == named )
                continue;
            instance.WorldBoundsRow = named;
            m_DirtyInstances.push_back( row.FirstInstance + i );
        }
    }

    void GpuScene::SetSkinCache( const PrimitiveIndex primitive, const uint64_t address,
                                 const uint32_t vertexCount )
    {
        if ( !IsValid( primitive ) )
            return;
        GpuScenePrimitive& row = m_Primitives[primitive];
        const auto         lo  = static_cast<uint32_t>( address & 0xffffffffu );
        const auto         hi  = static_cast<uint32_t>( address >> 32u );
        if ( row.SkinCacheAddressLo == lo && row.SkinCacheAddressHi == hi &&
             row.SkinCacheVertexCount == vertexCount )
            return;
        row.SkinCacheAddressLo   = lo;
        row.SkinCacheAddressHi   = hi;
        row.SkinCacheVertexCount = vertexCount;
        MarkPrimitive( primitive );
    }

    void GpuScene::SetBonePalettes( const std::span<const glm::mat4> palettes )
    {
        if ( palettes.size() == m_BonePalettes.size() &&
             ( palettes.empty() ||
               std::memcmp( palettes.data(), m_BonePalettes.data(), palettes.size_bytes() ) == 0 ) )
            return;
        m_BonePalettes.assign( palettes.begin(), palettes.end() );
        m_BonesDirty = true;
    }

    void GpuScene::JournalTransform( const PrimitiveIndex primitive )
    {
        if ( m_UndoMark[primitive] != 0u )
            return;
        m_UndoMark[primitive]        = 1u;
        const GpuScenePrimitive& row = m_Primitives[primitive];
        m_FrameUndo.push_back( TransformUndo{ .Primitive        = primitive,
                                              .LocalToWorld     = row.LocalToWorld,
                                              .PrevLocalToWorld = row.PrevLocalToWorld } );
    }

    void GpuScene::DiscardFrame()
    {
        for ( const TransformUndo& undo : m_FrameUndo )
        {
            m_UndoMark[undo.Primitive] = 0u;
            if ( !IsValid( undo.Primitive ) )
                continue;
            GpuScenePrimitive& row = m_Primitives[undo.Primitive];
            row.LocalToWorld       = undo.LocalToWorld;
            row.PrevLocalToWorld   = undo.PrevLocalToWorld;
            MarkPrimitive( undo.Primitive );
        }
        m_FrameUndo.clear();
        for ( const PrimitiveIndex primitive : m_MovedLastFrame )
            m_MovedMark[primitive] = 0u;
        m_MovedLastFrame = std::move( m_UndoMovedLastFrame );
        m_UndoMovedLastFrame.clear();
        for ( const PrimitiveIndex primitive : m_MovedLastFrame )
            m_MovedMark[primitive] = 1u;
    }

    void GpuScene::BeginFrame()
    {
        // A new undo: the last frame was kept (or discarded already).
        for ( const TransformUndo& undo : m_FrameUndo )
            m_UndoMark[undo.Primitive] = 0u;
        m_FrameUndo.clear();
        m_UndoMovedLastFrame = m_MovedLastFrame;
        // Moved in the frame that just ended: this frame starts from where they stopped (UE: the velocity
        // list's "moved last frame" pass). A primitive that moves again this frame overwrites Prev with the same
        // value in SetTransform.
        for ( const PrimitiveIndex primitive : m_MovedLastFrame )
        {
            m_MovedMark[primitive] = 0u;
            if ( !IsValid( primitive ) )
                continue;
            GpuScenePrimitive& row = m_Primitives[primitive];
            if ( row.PrevLocalToWorld == row.LocalToWorld )
                continue;
            JournalTransform( primitive );
            row.PrevLocalToWorld = row.LocalToWorld;
            MarkPrimitive( primitive );
        }
        m_MovedLastFrame.clear();
    }

    bool GpuScene::HasPendingUpload() const
    {
        return !m_DirtyPrimitives.empty() || !m_DirtyInstances.empty() || !m_DirtyLodScreenSizes.empty() ||
               !m_DirtyWorldBounds.empty() || m_BonesDirty || !m_PrimitiveTable->Buffer || !m_BonesTable->Buffer;
    }

    Common::ResultStr<GpuSceneBuffers> GpuScene::AddUploadPass( RDG::Builder& graph )
    {
        SortUnique( m_DirtyPrimitives );
        SortUnique( m_DirtyInstances );
        SortUnique( m_DirtyLodScreenSizes );
        SortUnique( m_DirtyWorldBounds );

        GpuSceneBuffers out;
        out.PrimitiveCount = static_cast<uint32_t>( m_Primitives.size() );
        out.InstanceCount  = static_cast<uint32_t>( m_Instances.size() );

        const auto upload = [&]( Table& table, const uint32_t rowCount, const void* image,
                                 std::vector<uint32_t>& dirty, const bool contiguous,
                                 RDG::BufferRef& into ) -> Common::BoolResultStr
        {
            auto ref = UploadTable( graph, table, *m_Upload, rowCount, static_cast<const std::byte*>( image ),
                                    dirty, contiguous && !dirty.empty() );
            if ( !ref )
                return Common::MakeError( ref.GetError() );
            into = ref.GetValue();
            dirty.clear();
            return BOOLSUCCESS;
        };

        // The palettes are rewritten whole every frame they change: one contiguous upload, no row list.
        std::vector<uint32_t> bones;
        if ( m_BonesDirty )
            bones.push_back( 0u ); // a non-empty "dirty" list switches the contiguous upload on
        if ( const auto done = upload( *m_PrimitiveTable, out.PrimitiveCount, m_Primitives.data(),
                                       m_DirtyPrimitives, false, out.Primitives );
             !done )
            return Common::MakeError( done.GetError() );
        if ( const auto done = upload( *m_InstanceTable, out.InstanceCount, m_Instances.data(), m_DirtyInstances,
                                       false, out.Instances );
             !done )
            return Common::MakeError( done.GetError() );
        if ( const auto done = upload( *m_LodTable, static_cast<uint32_t>( m_LodScreenSizes.size() ),
                                       m_LodScreenSizes.data(), m_DirtyLodScreenSizes, false, out.LodScreenSizes );
             !done )
            return Common::MakeError( done.GetError() );
        if ( const auto done = upload( *m_WorldBoundsTable, static_cast<uint32_t>( m_WorldBounds.size() ),
                                       m_WorldBounds.data(), m_DirtyWorldBounds, false, out.WorldBounds );
             !done )
            return Common::MakeError( done.GetError() );
        if ( const auto done = upload( *m_BonesTable, static_cast<uint32_t>( m_BonePalettes.size() ),
                                       m_BonePalettes.data(), bones, true, out.ObjectBones );
             !done )
            return Common::MakeError( done.GetError() );
        m_BonesDirty = false;
        return out;
    }

    const GpuScenePrimitive& GpuScene::Primitive( const PrimitiveIndex primitive ) const
    {
        return m_Primitives[primitive];
    }

    std::span<const GpuSceneInstance> GpuScene::Instances( const PrimitiveIndex primitive ) const
    {
        const GpuScenePrimitive& row = m_Primitives[primitive];
        return std::span<const GpuSceneInstance>( m_Instances ).subspan( row.FirstInstance, row.InstanceCount );
    }

    Common::ResultStr<GpuSceneBuffers> GpuScene::ImportTables( RDG::Builder& graph ) const
    {
        if ( HasPendingUpload() )
            return Common::MakeError<GpuSceneBuffers>(
                 "GPUScene: rows wait for an upload; the tables are imported only after AddUploadPass ran" );
        GpuSceneBuffers out;
        out.PrimitiveCount = static_cast<uint32_t>( m_Primitives.size() );
        out.InstanceCount  = static_cast<uint32_t>( m_Instances.size() );
        const auto import  = [&graph]( Table& table, RDG::BufferRef& into ) -> Common::BoolResultStr
        {
            if ( !table.Buffer )
                return Common::MakeError( std::format( "GPUScene {}: no device table yet (no upload ran)", table.Name ) );
            if ( const auto imported = Renderer::ImportBuffer( table.Buffer, table.Import ); !imported )
                return Common::MakeError( std::format( "GPUScene {}: the table could not be imported: {}",
                                                       table.Name, imported.GetError() ) );
            into = graph.RegisterExternal( table.Import, std::string( "GPUScene." ) + table.Name );
            return BOOLSUCCESS;
        };
        for ( const auto& [table, into] :
              { std::pair<Table*, RDG::BufferRef*>{ m_PrimitiveTable.get(), &out.Primitives },
                std::pair<Table*, RDG::BufferRef*>{ m_InstanceTable.get(), &out.Instances },
                std::pair<Table*, RDG::BufferRef*>{ m_LodTable.get(), &out.LodScreenSizes },
                std::pair<Table*, RDG::BufferRef*>{ m_WorldBoundsTable.get(), &out.WorldBounds },
                std::pair<Table*, RDG::BufferRef*>{ m_BonesTable.get(), &out.ObjectBones } } )
            if ( const Common::BoolResultStr done = import( *table, *into ); !done )
                return Common::MakeError<GpuSceneBuffers>( done.GetError() );
        return Common::MakeSuccess( std::move( out ) );
    }

    const std::shared_ptr<ShaderResources::StorageBuffer>& GpuScene::GetPrimitivesBuffer() const
    {
        return m_PrimitiveTable->Buffer;
    }

    const std::shared_ptr<ShaderResources::StorageBuffer>& GpuScene::GetBonesBuffer() const
    {
        return m_BonesTable->Buffer;
    }
} // namespace Desert::Graphic::GpuDriven
