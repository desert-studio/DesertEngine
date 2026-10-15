#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSkinnedRayTracingGeometry.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
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

    Common::BoolResultStr RayTracingSkinCache::AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                          std::span<RayTracingPrimitive> primitives,
                                                          const uint32_t                 firstSkinned,
                                                          std::vector<RDG::BufferRef>&   blasOut )
    {
        ++m_Frame;
        const auto releaseUnused = [this]
        { std::erase_if( m_Entries, [this]( const auto& entry ) { return entry.second.LastFrame != m_Frame; } ); };

        const std::vector<RayTracingDynamicGeometryRequest> requests = SelectDynamicGeometry( primitives );
        if ( requests.empty() )
        {
            releaseUnused();
            return BOOLSUCCESS;
        }

        if ( !m_Pipeline )
        {
            Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> shader =
                 service ? service->GetByName( std::string( kSkinCacheShader ) ) : nullptr;
            if ( shader )
            {
                if ( const auto built = ComputePipeline::Create(
                          { .Shader = shader, .DebugName = std::string( kSkinCacheShader ) } ) )
                    m_Pipeline = built.GetValue();
                else if ( !m_PipelineRefusalLogged )
                {
                    LOG_ERROR(
                         "[RayTracing] the skin-cache pipeline was not built; {} skinned primitive(s) are not "
                         "in the TLAS: {}",
                         requests.size(), built.GetError() );
                    m_PipelineRefusalLogged = true;
                }
            }
            else if ( !m_PipelineRefusalLogged )
            {
                LOG_ERROR(
                     "[RayTracing] the shader '{}' did not compile - expected "
                     "Editor/Resources/Shaders/Programs/Compute/SkinCache.shader; {} skinned primitive(s) are "
                     "not in the TLAS",
                     kSkinCacheShader, requests.size() );
                m_PipelineRefusalLogged = true;
            }
            if ( !m_Pipeline )
            {
                releaseUnused();
                return BOOLSUCCESS; // every skinned primitive keeps Geometry null: counted RayTracingSkip::Skinned
            }
        }

        if ( const auto imported = Renderer::ImportBuffer( meshes.GetObjectBones(), m_BonesImport ); !imported )
            return Common::MakeError( std::format( "{}: the view's ObjectBones could not be imported: {}",
                                               kSkinCachePassName, imported.GetError() ) );
        const RDG::BufferRef bones = graph.RegisterExternal( m_BonesImport, "SkinCache.Bones" );

        const std::shared_ptr<ComputePipeline> pipeline = m_Pipeline;
        const auto&                            layout   = m_Layout.Get( pipeline->GetSpecification().Shader );
        const RDG::OtherRouteFill              fill     = Renderer::GetPipelineRouteFill( *pipeline );
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        const std::span<const GpuObjectMotion> rows = meshes.GetObjectMotionRows().Rows;

        m_RecordsPerEntity.clear();
        std::vector<DynamicBuild> builds;
        builds.reserve( requests.size() );
        uint64_t scratchBytes = 0;
        for ( const RayTracingDynamicGeometryRequest& request : requests )
        {
            const std::size_t recordIndex = request.Primitive - firstSkinned;
            if ( request.Primitive < firstSkinned || recordIndex >= records.size() )
                return Common::MakeError( std::format(
                     "{}: primitive {} is not a skinned record (first skinned {}, {} "
                     "records)",
                     kSkinCachePassName, request.Primitive, firstSkinned, records.size() ) );
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[recordIndex];
            const uint64_t                                     key =
                 ( static_cast<uint64_t>( record.Entity ) << 32u ) | m_RecordsPerEntity[record.Entity]++;
            Entry& entry    = m_Entries[key];
            entry.LastFrame = m_Frame;
            if ( !entry.Geometry )
                entry.Geometry = CreateSkinnedRayTracingGeometry();

            const Common::ResultStr<SkinnedBlasPrepared> prepared =
                 entry.Geometry->Prepare( *record.Mesh, request.HiddenSubmeshes );
            if ( !prepared )
            {
                if ( !entry.RefusalLogged )
                    LOG_ERROR( "[RayTracing] entity {}: its skinned mesh is not in the TLAS: {}", record.Entity,
                               prepared.GetError() );
                entry.RefusalLogged = true;
                continue;
            }
            if ( record.MotionRow >= rows.size() )
                return Common::MakeError( std::format( "{}: entity {} names motion row {} of {}", kSkinCachePassName,
                                                   record.Entity, record.MotionRow, rows.size() ) );
            if ( const auto imported =
                      Renderer::ImportVertexBuffer( record.Mesh->GetVertexBuffer(), entry.VerticesImport );
                 !imported )
            {
                if ( !entry.RefusalLogged )
                    LOG_ERROR(
                         "[RayTracing] entity {}: its skinned vertices could not be imported, the mesh is not "
                         "in the TLAS: {}",
                         record.Entity, imported.GetError() );
                entry.RefusalLogged = true;
                continue;
            }
            entry.RefusalLogged = false;

            // The shader's SkinCacheDispatch: x = vertex count, y = the first bone of this frame's palette.
            const uint32_t                vertexCount = record.Mesh->GetVertexCount();
            const std::array<uint32_t, 4> dispatch    = { vertexCount, rows[record.MotionRow].BoneOffset, 0u, 0u };
            const RDG::BufferRef          dispatchRef =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( dispatch ) }, "SkinCache.Dispatch" );
            graph.QueueBufferUpload( dispatchRef, std::as_bytes( std::span( dispatch ) ) );
            const RDG::BufferRef vertices = graph.RegisterExternal( entry.VerticesImport, "SkinCache.Vertices" );
            const RDG::BufferRef positions =
                 graph.RegisterExternal( entry.Geometry->GetPositions(), "SkinCache.Positions" );

            const uint32_t groups = ( vertexCount + kSkinCacheGroupSize - 1u ) / kSkinCacheGroupSize;
            graph.AddPass(
                 kSkinCachePassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( layout, fill )
                          .Storage( "SkinCacheBones", bones, RDG::Access::StorageRead )
                          .Storage( "SkinCacheVertices", vertices, RDG::Access::StorageRead )
                          .Storage( "SkinCacheDispatch", dispatchRef, RDG::Access::StorageRead )
                          .Storage( "SkinCachePositions", positions, RDG::Access::StorageWrite );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups, 1, 1 );
                 } );

            DynamicBuild& build = builds.emplace_back();
            build.Geometry      = entry.Geometry.get();
            build.Positions     = positions;
            build.Storage       = graph.RegisterExternal( entry.Geometry->GetStorage(), "SkinCache.BLAS" );
            build.ScratchOffset = scratchBytes;
            scratchBytes += prepared.GetValue().ScratchBytes;

            // Through the base: EncodeInstanceRows reads Geometry back as a RayTracingGeometryResource.
            const RayTracingGeometryResource* blas = entry.Geometry.get();
            primitives[request.Primitive].Geometry = blas;
        }
        releaseUnused();
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
