#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Render/RayTracingMeshInput.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/DisplacementCache.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <Common/Core/Logger.hpp>

#include <array>
#include <format>
#include <string>
#include <unordered_map>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct RayTracingSkinCacheShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, SkinCacheBones );
        DESERT_SHADER_PARAMETER( StorageBuffer, SkinCacheVertices );
        DESERT_SHADER_PARAMETER( StorageBuffer, SkinCacheDispatch );
        DESERT_SHADER_PARAMETER( StorageBuffer, SkinCacheStreams );
    };
} // namespace

namespace Desert::Graphic::RayTracing
{
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
            entry.StreamsRef = graph.RegisterExternal( DrawnStreams( entry ).GetBuffer(), "SkinCache.Streams" );
        return *entry.StreamsRef;
    }

    RDG::BufferRef RayTracingSkinCache::StorageIn( RDG::Builder& graph, Entry& entry )
    {
        Touch( entry );
        if ( !entry.StorageRef )
            entry.StorageRef = graph.RegisterExternal( entry.Geometry->GetStorage(), "SkinCache.BLAS" );
        return *entry.StorageRef;
    }

    void RayTracingSkinCache::PrepareRows( const System::MeshRenderer& meshes, GpuDriven::GpuScene& scene,
                                           const uint64_t frame )
    {
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        if ( !m_Enabled || records.empty() || !EnsurePipeline( records.size() ) )
            return; // the primitives keep no address: the raster skins in the vertex stage

        KeyRecords( meshes );
        for ( std::size_t i = 0; i < records.size(); ++i )
        {
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[i];
            if ( record.Mesh == nullptr || !scene.IsValid( record.GpuPrimitive ) )
                continue;
            Entry& entry = m_Entries[m_RecordKeys[i]];
            if ( entry.LastFrame != frame || !entry.Streams )
            {
                // The first view of the frame that draws it: fit the streams to this frame's mesh.
                entry.LastFrame    = frame;
                entry.StreamsReady = false;
                if ( !entry.Streams )
                    entry.Streams = CreateSkinCacheStreams();
                // A displaced record skins its base (SkinSource) and draws the twin (Mesh): two stream sets.
                const SkinnedMesh*            source   = record.SkinSource != nullptr ? record.SkinSource : record.Mesh;
                const Common::ResultStr<bool> prepared = entry.Streams->Prepare( source->GetVertexCount() );
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
                if ( prepared.GetValue() || entry.Source != source )
                    entry.HasSkinned = false; // new buffer or another mesh: no previous pose to rotate
                if ( record.SkinSource == nullptr )
                    entry.Displaced.reset();
                else
                {
                    if ( !entry.Displaced )
                        entry.Displaced = CreateSkinCacheStreams();
                    const Common::ResultStr<bool> twin = entry.Displaced->Prepare( record.Mesh->GetVertexCount() );
                    if ( !twin )
                    {
                        if ( !entry.RefusalLogged )
                            LOG_ERROR( "[SkinCache] entity {}: no displaced streams, it skins in the vertex stage "
                                       "and is not in the TLAS: {}",
                                       record.Entity, twin.GetError() );
                        entry.RefusalLogged = true;
                        entry.HasSkinned    = false;
                        continue;
                    }
                    if ( twin.GetValue() || entry.Mesh != record.Mesh )
                        entry.DisplacedFresh = true;
                }
                entry.Mesh   = record.Mesh;
                entry.Source = source;
                if ( const auto imported =
                          Renderer::ImportVertexBuffer( source->GetVertexBuffer(), entry.VerticesImport );
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
            scene.SetSkinCache( record.GpuPrimitive, DrawnStreams( entry ).GetDeviceAddress(),
                                record.Mesh->GetVertexCount() );
        }
    }

    Common::BoolResultStr RayTracingSkinCache::SkinFrame( System::MeshRenderer& meshes, GpuDriven::GpuScene& scene,
                                                          RDG::ExternalBuffer& viewBones, const uint64_t frame )
    {
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        if ( !m_Enabled || !m_Pipeline || records.empty() )
            return BOOLSUCCESS;

        const std::shared_ptr<ComputePipeline> pipeline = m_Pipeline;
        const auto&                            layout   = m_Layout.Get( pipeline->GetSpecification().Shader );
        const RDG::OtherRouteFill              fill     = Renderer::GetPipelineRouteFill( *pipeline );
        RDG::Builder                           graph( "SkinCache" );
        std::optional<RDG::BufferRef>          bones;
        // A vertex external is registered once per graph: two records of one entity may share a morph twin.
        std::unordered_map<RDG::ExternalBuffer*, RDG::BufferRef> vertexRefs;
        std::vector<Entry*>                                      skinned;
        std::vector<std::size_t>                                 skinnedRecords;

        KeyRecords( meshes );
        for ( std::size_t i = 0; i < records.size(); ++i )
        {
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[i];
            const auto                                         found  = m_Entries.find( m_RecordKeys[i] );
            if ( found == m_Entries.end() )
                continue;
            Entry& entry = found->second;
            if ( entry.LastFrame != frame || !entry.StreamsReady || !scene.IsValid( record.GpuPrimitive ) )
                continue;
            if ( entry.HasSkinned && entry.SkinnedFrame == frame )
                continue; // an earlier view of this frame skinned it: one skinning per frame

            if ( !bones )
            {
                if ( const auto imported = Renderer::ImportBuffer( scene.GetBonesBuffer(), viewBones );
                     !imported )
                    return Common::MakeError( std::format( "{}: the scene's ObjectBones could not be imported: {}",
                                                           kSkinCachePassName, imported.GetError() ) );
                bones = graph.RegisterExternal( viewBones, "SkinCache.Bones" );
            }

            // A morphed record reads the twin's output through the external the morph graph wrote it through
            // (MeshRenderer::UpdateMorphTargets): one resource, so its carried VertexIndex state barriers this
            // read.
            RDG::ExternalBuffer* const morphed = meshes.FindMorphOutput( record.Entity, entry.Source );
            RDG::ExternalBuffer* const source  = morphed != nullptr ? morphed : &entry.VerticesImport;
            auto [vertexRef, fresh]            = vertexRefs.try_emplace( source );
            if ( fresh )
                vertexRef->second = graph.RegisterExternal( *source, "SkinCache.Vertices" );

            // The shader's SkinCacheDispatch: x = vertex count, y = the first bone of this frame's palette, z = 1
            // when the streams' current positions are last frame's pose (rotate them into the previous positions).
            const bool                    history     = entry.HasSkinned && entry.SkinnedFrame + 1u == frame;
            const uint32_t                vertexCount = entry.Source->GetVertexCount();
            const std::array<uint32_t, 4> dispatch    = { vertexCount, scene.Primitive( record.GpuPrimitive ).BoneOffset,
                                                       history ? 1u : 0u, 0u };
            const RDG::BufferRef          dispatchRef =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( dispatch ) }, "SkinCache.Dispatch" );
            graph.QueueBufferUpload( dispatchRef, std::as_bytes( std::span( dispatch ) ) );
            const RDG::BufferRef streams =
                 graph.RegisterExternal( entry.Streams->GetBuffer(), "SkinCache.Streams" );
            const RDG::BufferRef vertices = vertexRef->second;
            const RDG::BufferRef bonesRef = *bones;
            const uint32_t       groups   = ( vertexCount + kSkinCacheGroupSize - 1u ) / kSkinCacheGroupSize;
            graph.AddPass(
                 kSkinCachePassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( layout, fill )
                          .Storage( RayTracingSkinCacheShaderParameters::SkinCacheBones, bonesRef,
                                    RDG::Access::StorageRead )
                          .Storage( RayTracingSkinCacheShaderParameters::SkinCacheVertices, vertices,
                                    RDG::Access::StorageRead )
                          .Storage( RayTracingSkinCacheShaderParameters::SkinCacheDispatch, dispatchRef,
                                    RDG::Access::StorageRead )
                          .Storage( RayTracingSkinCacheShaderParameters::SkinCacheStreams, streams,
                                    RDG::Access::StorageWrite );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups, 1, 1 );
                 } );
            if ( entry.Displaced )
            {
                // The displaced twin's pose from the base just skinned, in this graph (CONTRACT-DISPLACE §9.8).
                const RDG::BufferRef displaced =
                     graph.RegisterExternal( entry.Displaced->GetBuffer(), "SkinCache.DisplacedStreams" );
                Common::BoolResultStr added = BOOLSUCCESS;
                if ( m_Displacement == nullptr )
                    added = Common::MakeError( "the skin cache has no displacement cache (SetDisplacement)" );
                else
                {
                    const auto slot             = static_cast<uint32_t>( m_RecordKeys[i] & 0xffffffffu );
                    const bool displacedHistory = history && !entry.DisplacedFresh;
                    added = m_Displacement->AddSkinnedPasses( graph, record.Entity, slot, streams, vertexCount,
                                                              displaced, displacedHistory, vertexRefs );
                }
                if ( !added )
                {
                    // Nothing writes the drawn streams: the row loses its address (the vertex stage skins the
                    // twin's rest vertices) and the entry is not marked skinned this frame.
                    if ( !entry.RefusalLogged )
                        LOG_ERROR( "[SkinCache] entity {}: its displaced pose is not written, it skins in the vertex "
                                   "stage and is not in the TLAS: {}",
                                   record.Entity, added.GetError() );
                    entry.RefusalLogged      = true;
                    entry.StreamsReady       = false;
                    scene.SetSkinCache( record.GpuPrimitive, 0u, 0u );
                    continue;
                }
            }
            skinned.push_back( &entry );
            skinnedRecords.push_back( i );
        }
        if ( skinned.empty() )
            return BOOLSUCCESS;

        if ( const Common::BoolResultStr executed = Renderer::ExecuteGraph( graph ); !executed )
        {
            // Nothing of this graph reached the GPU: the streams hold no pose of this frame, so their rows lose
            // the address (the raster skins those draws in the vertex stage) and the entries are not marked
            // skinned.
            for ( Entry* entry : skinned )
            {
                entry->StreamsReady = false;
                entry->HasSkinned   = false;
            }
            for ( const std::size_t i : skinnedRecords )
                scene.SetSkinCache( records[i].GpuPrimitive, 0u, 0u );
            return Common::MakeError( std::format( "{}: the skin graph of {} primitive(s) did not execute; they "
                                                   "skin in the vertex stage this frame: {}",
                                                   kSkinCachePassName, skinned.size(), executed.GetError() ) );
        }
        for ( Entry* entry : skinned )
        {
            entry->SkinnedFrame   = frame;
            entry->HasSkinned     = true;
            entry->DisplacedFresh = false;
        }
        return BOOLSUCCESS;
    }

    void RayTracingSkinCache::DeclareReads( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                            const uint64_t frame, std::vector<RDG::BufferRef>& rasterReads )
    {
        ++m_GraphSerial;
        m_DeclaringGraph                                                           = &graph;
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        if ( !m_Enabled || !m_Pipeline || records.empty() )
            return;

        KeyRecords( meshes );
        for ( std::size_t i = 0; i < records.size(); ++i )
        {
            const auto found = m_Entries.find( m_RecordKeys[i] );
            if ( found == m_Entries.end() )
                continue;
            Entry& entry = found->second;
            // Only what a skin graph of this frame wrote: an entry whose graph failed lost its rows' address.
            if ( entry.LastFrame != frame || !entry.StreamsReady || !entry.HasSkinned ||
                 entry.SkinnedFrame != frame )
                continue;
            rasterReads.push_back( StreamsIn( graph, entry ) );
        }
    }

    Common::BoolResultStr RayTracingSkinCache::AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                          std::span<RayTracingPrimitive> primitives,
                                                          const uint32_t firstSkinned, const uint64_t frame,
                                                          std::vector<RayTracingDynamicGeometryBuild>& buildsOut,
                                                          std::vector<RDG::BufferRef>&                 blasOut )
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
            return Common::MakeError( std::format( "{}: DeclareReads did not register into this graph before the "
                                                   "ray-tracing scene - the BLAS would read positions no pass of "
                                                   "this graph declared",
                                                   kDynamicGeometryPassName ) );

        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        KeyRecords( meshes );
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
                    RayTracingPrimitive&              target = primitives[request.Primitive];
                    target.Geometry                          = shared;
                    target.DeformedPositionsAddress          = DrawnStreams( entry ).GetDeviceAddress();
                    target.DeformedPositions                 = StreamsIn( graph, entry );
                    blasOut.push_back( StorageIn( graph, entry ) );
                }
                continue;
            }
            entry.BuiltFrame = frame;
            entry.BlasReady  = false;
            if ( !entry.Geometry )
                entry.Geometry = CreateSkinnedRayTracingGeometry();
            const Common::ResultStr<SkinnedBlasPrepared> prepared =
                 entry.Geometry->Prepare( DescribeDeformedMeshBlas( *record.Mesh, request.Submeshes ) );
            if ( !prepared )
            {
                if ( !entry.RefusalLogged )
                    LOG_ERROR( "[SkinCache] entity {}: its skinned mesh is not in the TLAS: {}", record.Entity,
                               prepared.GetError() );
                entry.RefusalLogged = true;
                continue;
            }
            entry.BlasReady = true;

            // The vertex input is the entry's streams: the BLAS reads the positions SkinFrame wrote this frame,
            // the same bytes the raster fetches - no skinning on the ray-tracing side.
            buildsOut.push_back( { .Target       = entry.Geometry.get(),
                                   .Vertices     = StreamsIn( graph, entry ),
                                   .Storage      = StorageIn( graph, entry ),
                                   .ScratchBytes = prepared.GetValue().ScratchBytes } );

            // Through the base: EncodeInstanceRows reads Geometry back as a RayTracingGeometryResource.
            const RayTracingGeometryResource* blas   = entry.Geometry.get();
            RayTracingPrimitive&              target = primitives[request.Primitive];
            target.Geometry                          = blas;
            target.DeformedPositionsAddress          = DrawnStreams( entry ).GetDeviceAddress();
            target.DeformedPositions                 = StreamsIn( graph, entry );
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
