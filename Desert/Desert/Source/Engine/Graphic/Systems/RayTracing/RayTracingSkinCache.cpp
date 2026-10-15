#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSkinnedRayTracingGeometry.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <Common/Core/Logger.hpp>

#include <array>
#include <format>
#include <string>

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<SkinnedRayTracingGeometry> CreateSkinnedRayTracingGeometry()
    {
        return std::make_unique<API::Vulkan::VulkanSkinnedRayTracingGeometry>();
    }

    std::unique_ptr<SkinCacheStreams> CreateSkinCacheStreams()
    {
        return std::make_unique<API::Vulkan::VulkanSkinCacheStreams>();
    }

    namespace
    {
        // One primitive's build inside "RayTracingScene.DynamicGeometry".
        struct DynamicBuild
        {
            SkinnedRayTracingGeometry* Geometry = nullptr;
            RDG::BufferRef             Positions;
            RDG::BufferRef             Storage;
            uint64_t                   ScratchOffset = 0;
        };
    } // namespace

    void RayTracingSkinCache::SetEnabled( const bool enabled )
    {
        if ( enabled == m_Enabled )
            return;
        m_Enabled = enabled;
        if ( !enabled )
        {
            // Retired on the frame ring by the API objects: a frame in flight still reads them safely.
            m_Entries.clear();
            LOG_INFO(
                 "[SkinCache] off (Scalability Effects.SkinCache = 0): skinned meshes skin in the vertex stage "
                 "and the ray-tracing scene has no skinned geometry" );
        }
        m_OffLogged = false;
    }

    void RayTracingSkinCache::KeyRecords( const System::MeshRenderer& meshes )
    {
        m_RecordsPerEntity.clear();
        m_RecordKeys.clear();
        for ( const System::MeshRenderer::SkinnedMeshRenderData& record : meshes.GetSkinnedQueue() )
            m_RecordKeys.push_back( ( static_cast<uint64_t>( record.Entity ) << 32u ) |
                                    m_RecordsPerEntity[record.Entity]++ );
    }

    bool RayTracingSkinCache::EnsurePipeline( const std::size_t skinnedCount )
    {
        if ( m_Pipeline )
            return true;
        Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
        const std::shared_ptr<Shader> shader =
             service ? service->GetByName( std::string( kSkinCacheShader ) ) : nullptr;
        if ( !shader )
        {
            if ( !m_PipelineRefusalLogged )
                LOG_ERROR(
                     "[SkinCache] the shader '{}' did not compile - expected "
                     "Editor/Resources/Shaders/Programs/Compute/SkinCache.shader; {} skinned primitive(s) skin "
                     "in the vertex stage and are not in the TLAS",
                     kSkinCacheShader, skinnedCount );
            m_PipelineRefusalLogged = true;
            return false;
        }
        const auto built =
             ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( kSkinCacheShader ) } );
        if ( !built )
        {
            if ( !m_PipelineRefusalLogged )
                LOG_ERROR(
                     "[SkinCache] the pipeline was not built; {} skinned primitive(s) skin in the vertex stage "
                     "and are not in the TLAS: {}",
                     skinnedCount, built.GetError() );
            m_PipelineRefusalLogged = true;
            return false;
        }
        m_Pipeline = built.GetValue();
        return true;
    }

    void RayTracingSkinCache::Touch( Entry& entry ) const
    {
        if ( entry.GraphSerial == m_GraphSerial )
            return;
        entry.GraphSerial = m_GraphSerial;
        entry.StreamsRef.reset();
        entry.StorageRef.reset();
    }

    RDG::BufferRef RayTracingSkinCache::StreamsIn( RDG::Builder& graph, Entry& entry )
    {
        Touch( entry );
        if ( !entry.StreamsRef )
            entry.StreamsRef = graph.RegisterExternal( entry.Streams->GetBuffer(), "SkinCache.Streams" );
        return *entry.StreamsRef;
    }

    RDG::BufferRef RayTracingSkinCache::StorageIn( RDG::Builder& graph, Entry& entry )
    {
        Touch( entry );
        if ( !entry.StorageRef )
            entry.StorageRef = graph.RegisterExternal( entry.Geometry->GetStorage(), "SkinCache.BLAS" );
        return *entry.StorageRef;
    }

    void RayTracingSkinCache::PrepareRows( const System::MeshRenderer&      meshes,
                                           const std::span<GpuObjectMotion> rows, const uint64_t frame )
    {
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        if ( !m_Enabled || records.empty() || !EnsurePipeline( records.size() ) )
            return; // rows keep no address: the raster skins in the vertex stage

        KeyRecords( meshes );
        for ( std::size_t i = 0; i < records.size(); ++i )
        {
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[i];
            if ( record.Mesh == nullptr || record.MotionRow >= rows.size() )
                continue;
            Entry& entry = m_Entries[m_RecordKeys[i]];
            if ( entry.LastFrame != frame || !entry.Streams )
            {
                // The first view of the frame that draws it: fit the streams to this frame's mesh.
                entry.LastFrame    = frame;
                entry.StreamsReady = false;
                if ( !entry.Streams )
                    entry.Streams = CreateSkinCacheStreams();
                const Common::ResultStr<bool> prepared = entry.Streams->Prepare( record.Mesh->GetVertexCount() );
                if ( !prepared )
                {
                    if ( !entry.RefusalLogged )
                        LOG_ERROR(
                             "[SkinCache] entity {}: no cached streams, it skins in the vertex stage and is not "
                             "in the TLAS: {}",
                             record.Entity, prepared.GetError() );
                    entry.RefusalLogged = true;
                    entry.HasSkinned    = false;
                    continue;
                }
                if ( prepared.GetValue() || entry.Mesh != record.Mesh )
                    entry.HasSkinned = false; // new buffer or another mesh: no previous pose to rotate
                entry.Mesh = record.Mesh;
                if ( const auto imported =
                          Renderer::ImportVertexBuffer( record.Mesh->GetVertexBuffer(), entry.VerticesImport );
                     !imported )
                {
                    if ( !entry.RefusalLogged )
                        LOG_ERROR(
                             "[SkinCache] entity {}: its skinned vertices could not be imported, it skins in "
                             "the vertex stage and is not in the TLAS: {}",
                             record.Entity, imported.GetError() );
                    entry.RefusalLogged = true;
                    continue;
                }
                entry.RefusalLogged = false;
                entry.StreamsReady  = true;
            }
            if ( !entry.StreamsReady )
                continue;
            const uint64_t   address = entry.Streams->GetDeviceAddress();
            GpuObjectMotion& row     = rows[record.MotionRow];
            row.SkinCacheAddressLo   = static_cast<uint32_t>( address & 0xffffffffu );
            row.SkinCacheAddressHi   = static_cast<uint32_t>( address >> 32u );
            row.SkinCacheVertexCount = record.Mesh->GetVertexCount();
        }
    }

    Common::BoolResultStr RayTracingSkinCache::AddSkinPasses( RDG::Builder&               graph,
                                                              const System::MeshRenderer& meshes,
                                                              RDG::ExternalBuffer& viewBones, const uint64_t frame,
                                                              std::vector<RDG::BufferRef>& rasterReads )
    {
        ++m_GraphSerial;
        m_DeclaringGraph                                                           = &graph;
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        if ( !m_Enabled || !m_Pipeline || records.empty() )
            return BOOLSUCCESS;

        const std::span<const GpuObjectMotion> rows     = meshes.GetObjectMotionRows().Rows;
        const std::shared_ptr<ComputePipeline> pipeline = m_Pipeline;
        const auto&                            layout   = m_Layout.Get( pipeline->GetSpecification().Shader );
        const RDG::OtherRouteFill              fill     = Renderer::GetPipelineRouteFill( *pipeline );
        std::optional<RDG::BufferRef>          bones;

        KeyRecords( meshes );
        for ( std::size_t i = 0; i < records.size(); ++i )
        {
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[i];
            const auto                                         found  = m_Entries.find( m_RecordKeys[i] );
            if ( found == m_Entries.end() )
                continue;
            Entry& entry = found->second;
            if ( entry.LastFrame != frame || !entry.StreamsReady || record.MotionRow >= rows.size() )
                continue;
            const RDG::BufferRef streams = StreamsIn( graph, entry );
            rasterReads.push_back( streams );
            if ( entry.HasSkinned && entry.SkinnedFrame == frame )
                continue; // an earlier view of this frame skinned it: one skinning per frame

            if ( !bones )
            {
                if ( const auto imported = Renderer::ImportBuffer( meshes.GetObjectBones(), viewBones );
                     !imported )
                    return Common::MakeError( std::format( "{}: the view's ObjectBones could not be imported: {}",
                                                           kSkinCachePassName, imported.GetError() ) );
                bones = graph.RegisterExternal( viewBones, "SkinCache.Bones" );
            }

            // The shader's SkinCacheDispatch: x = vertex count, y = the first bone of this frame's palette, z = 1
            // when the streams' current positions are last frame's pose (rotate them into the previous positions).
            const bool                    history     = entry.HasSkinned && entry.SkinnedFrame + 1u == frame;
            const uint32_t                vertexCount = record.Mesh->GetVertexCount();
            const std::array<uint32_t, 4> dispatch    = { vertexCount, rows[record.MotionRow].BoneOffset,
                                                       history ? 1u : 0u, 0u };
            const RDG::BufferRef          dispatchRef =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( dispatch ) }, "SkinCache.Dispatch" );
            graph.QueueBufferUpload( dispatchRef, std::as_bytes( std::span( dispatch ) ) );
            const RDG::BufferRef vertices = graph.RegisterExternal( entry.VerticesImport, "SkinCache.Vertices" );
            const RDG::BufferRef bonesRef = *bones;
            const uint32_t       groups   = ( vertexCount + kSkinCacheGroupSize - 1u ) / kSkinCacheGroupSize;
            graph.AddPass(
                 kSkinCachePassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( layout, fill )
                          .Storage( "SkinCacheBones", bonesRef, RDG::Access::StorageRead )
                          .Storage( "SkinCacheVertices", vertices, RDG::Access::StorageRead )
                          .Storage( "SkinCacheDispatch", dispatchRef, RDG::Access::StorageRead )
                          .Storage( "SkinCacheStreams", streams, RDG::Access::StorageWrite );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups, 1, 1 );
                 } );
            entry.SkinnedFrame = frame;
            entry.HasSkinned   = true;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr RayTracingSkinCache::AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                          std::span<RayTracingPrimitive> primitives,
                                                          const uint32_t firstSkinned, const uint64_t frame,
                                                          std::vector<RDG::BufferRef>& blasOut )
    {
        const std::vector<RayTracingDynamicGeometryRequest> requests = SelectDynamicGeometry( primitives );
        if ( requests.empty() )
            return BOOLSUCCESS;
        if ( !m_Enabled || !m_Pipeline )
        {
            // Every skinned primitive keeps Geometry null: counted RayTracingSkip::Skinned in the view's stats.
            if ( !m_OffLogged )
                LOG_WARN( "[SkinCache] {} skinned primitive(s) are not in the TLAS: the skin cache is {}",
                             requests.size(),
                             m_Enabled ? "without its pipeline (logged above)"
                                       : "off (Scalability Effects.SkinCache)" );
            m_OffLogged = true;
            return BOOLSUCCESS;
        }
        if ( m_DeclaringGraph != &graph )
            return Common::MakeError( std::format( "{}: AddSkinPasses was not declared into this graph before the "
                                                   "ray-tracing scene - the BLAS would "
                                                   "read positions no pass of this graph skinned",
                                                   kDynamicGeometryPassName ) );

        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        KeyRecords( meshes );
        std::vector<DynamicBuild> builds;
        builds.reserve( requests.size() );
        uint64_t scratchBytes = 0;
        for ( const RayTracingDynamicGeometryRequest& request : requests )
        {
            const std::size_t recordIndex = request.Primitive - firstSkinned;
            if ( request.Primitive < firstSkinned || recordIndex >= records.size() )
                return Common::MakeError(
                     std::format( "{}: primitive {} is not a skinned record (first skinned {}, {} records)",
                                  kDynamicGeometryPassName, request.Primitive, firstSkinned, records.size() ) );
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[recordIndex];
            const auto                                         found = m_Entries.find( m_RecordKeys[recordIndex] );
            if ( found == m_Entries.end() || !found->second.StreamsReady || !found->second.HasSkinned ||
                 found->second.SkinnedFrame != frame )
                continue; // its streams were refused (logged by PrepareRows): counted RayTracingSkip::Skinned
            Entry& entry = found->second;

            if ( entry.BuiltFrame == frame && entry.Geometry )
            {
                // An earlier view of this frame built it (or was refused, already logged): every view instances
                // the same BLAS. Read by this view's TLAS pass too.
                if ( entry.BlasReady )
                {
                    const RayTracingGeometryResource* shared = entry.Geometry.get();
                    primitives[request.Primitive].Geometry   = shared;
                    blasOut.push_back( StorageIn( graph, entry ) );
                }
                continue;
            }
            entry.BuiltFrame = frame;
            entry.BlasReady  = false;
            if ( !entry.Geometry )
                entry.Geometry = CreateSkinnedRayTracingGeometry();
            const Common::ResultStr<SkinnedBlasPrepared> prepared =
                 entry.Geometry->Prepare( *record.Mesh, request.HiddenSubmeshes );
            if ( !prepared )
            {
                if ( !entry.RefusalLogged )
                    LOG_ERROR( "[SkinCache] entity {}: its skinned mesh is not in the TLAS: {}", record.Entity,
                               prepared.GetError() );
                entry.RefusalLogged = true;
                continue;
            }
            entry.BlasReady = true;

            DynamicBuild& build = builds.emplace_back();
            build.Geometry      = entry.Geometry.get();
            build.Positions     = StreamsIn( graph, entry );
            build.Storage       = StorageIn( graph, entry );
            build.ScratchOffset = scratchBytes;
            scratchBytes += prepared.GetValue().ScratchBytes;

            // Through the base: EncodeInstanceRows reads Geometry back as a RayTracingGeometryResource.
            const RayTracingGeometryResource* blas = entry.Geometry.get();
            primitives[request.Primitive].Geometry = blas;
        }
        if ( builds.empty() )
            return BOOLSUCCESS;

        const RDG::BufferRef scratch = graph.CreateBuffer( RDG::BufferDesc{ scratchBytes }, "SkinCache.Scratch" );
        graph.AddPass(
             kDynamicGeometryPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Write( scratch, RDG::Access::AccelStructBuildWrite );
                 for ( const DynamicBuild& build : builds )
                 {
                     pass.Read( build.Positions, RDG::Access::AccelStructBuildInput );
                     pass.Write( build.Storage, RDG::Access::AccelStructBuildWrite );
                 }
             },
             [builds, scratch]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto scratchBinding = context.GetBuffer( scratch, RDG::Access::AccelStructBuildWrite );
                 if ( !scratchBinding )
                     return Common::MakeError( scratchBinding.GetError() );
                 for ( const DynamicBuild& build : builds )
                 {
                     const auto positions =
                          context.GetBuffer( build.Positions, RDG::Access::AccelStructBuildInput );
                     if ( !positions )
                         return Common::MakeError( positions.GetError() );
                     if ( const auto storage =
                               context.GetBuffer( build.Storage, RDG::Access::AccelStructBuildWrite );
                          !storage )
                         return Common::MakeError( storage.GetError() );
                     if ( const auto recorded = build.Geometry->RecordBuild(
                               context, positions.GetValue(), scratchBinding.GetValue(), build.ScratchOffset );
                          !recorded )
                         return recorded;
                 }
                 return BOOLSUCCESS;
             } );
        for ( const DynamicBuild& build : builds )
            blasOut.push_back( build.Storage );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
