#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/Render/RayTracingMeshInput.hpp>
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
    Common::BoolResultStr RayTracingSkinCache::AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                          RDG::ExternalBuffer&           viewBones,
                                                          std::span<RayTracingPrimitive> primitives,
                                                          const uint32_t firstSkinned, const uint64_t frame,
                                                          std::vector<RayTracingDynamicGeometryBuild>& buildsOut,
                                                          std::vector<RDG::BufferRef>&                 blasOut )
    {
        const std::vector<RayTracingDynamicGeometryRequest> requests = SelectDynamicGeometry( primitives );
        if ( requests.empty() )
            return BOOLSUCCESS;

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
                return BOOLSUCCESS; // every skinned primitive keeps Geometry null: counted RayTracingSkip::Skinned
            }
        }

        if ( const auto imported = Renderer::ImportBuffer( meshes.GetObjectBones(), viewBones ); !imported )
            return Common::MakeError( std::format( "{}: the view's ObjectBones could not be imported: {}",
                                                   kSkinCachePassName, imported.GetError() ) );
        const RDG::BufferRef bones = graph.RegisterExternal( viewBones, "SkinCache.Bones" );

        const std::shared_ptr<ComputePipeline> pipeline = m_Pipeline;
        const auto&                            layout   = m_Layout.Get( pipeline->GetSpecification().Shader );
        const RDG::OtherRouteFill              fill     = Renderer::GetPipelineRouteFill( *pipeline );
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> records = meshes.GetSkinnedQueue();
        const std::span<const GpuObjectMotion> rows = meshes.GetObjectMotionRows().Rows;

        m_RecordsPerEntity.clear();
        for ( const RayTracingDynamicGeometryRequest& request : requests )
        {
            const std::size_t recordIndex = request.Primitive - firstSkinned;
            if ( request.Primitive < firstSkinned || recordIndex >= records.size() )
                return Common::MakeError(
                     std::format( "{}: primitive {} is not a skinned record (first skinned {}, {} "
                                  "records)",
                                  kSkinCachePassName, request.Primitive, firstSkinned, records.size() ) );
            const System::MeshRenderer::SkinnedMeshRenderData& record = records[recordIndex];
            const uint64_t                                     key =
                 ( static_cast<uint64_t>( record.Entity ) << 32u ) | m_RecordsPerEntity[record.Entity]++;
            Entry& entry = m_Entries[key];
            if ( entry.LastFrame == frame && entry.Geometry )
            {
                // An earlier view of this frame skinned and built it (or was refused, already logged): one
                // skinning per frame, every view instances the same BLAS. Read by this view's TLAS pass too.
                if ( entry.Ready )
                {
                    const RayTracingGeometryResource* shared = entry.Geometry.get();
                    primitives[request.Primitive].Geometry   = shared;
                    blasOut.push_back( graph.RegisterExternal( entry.Geometry->GetStorage(), "SkinCache.BLAS" ) );
                }
                continue;
            }
            entry.LastFrame = frame;
            entry.Ready     = false;
            if ( !entry.Geometry )
                entry.Geometry = CreateSkinnedRayTracingGeometry();

            const Common::ResultStr<SkinnedBlasPrepared> prepared =
                 entry.Geometry->Prepare( DescribeDeformedMeshBlas( *record.Mesh, request.HiddenSubmeshes ) );
            if ( !prepared )
            {
                if ( !entry.RefusalLogged )
                    LOG_ERROR( "[RayTracing] entity {}: its skinned mesh is not in the TLAS: {}", record.Entity,
                               prepared.GetError() );
                entry.RefusalLogged = true;
                continue;
            }
            if ( record.MotionRow >= rows.size() )
                return Common::MakeError( std::format( "{}: entity {} names motion row {} of {}",
                                                       kSkinCachePassName, record.Entity, record.MotionRow,
                                                       rows.size() ) );
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
            entry.Ready         = true;

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

            buildsOut.push_back(
                 { .Target       = entry.Geometry.get(),
                   .Vertices     = positions,
                   .Storage      = graph.RegisterExternal( entry.Geometry->GetStorage(), "SkinCache.BLAS" ),
                   .ScratchBytes = prepared.GetValue().ScratchBytes } );

            // Through the base: EncodeInstanceRows reads Geometry back as a RayTracingGeometryResource.
            const RayTracingGeometryResource* blas = entry.Geometry.get();
            primitives[request.Primitive].Geometry = blas;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
