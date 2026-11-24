#include <Engine/Graphic/Systems/RayTracing/RayTracingFoliage.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <unordered_set>

namespace
{
    // The slot names "RayTracingScene: Foliage Wind" binds, spelled once (RDGShaderParameters.hpp).
    struct FoliageWindShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, CameraUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, FoliageWindJobs );
        DESERT_SHADER_PARAMETER( StorageBuffer, FoliageWindCopies );
        DESERT_SHADER_PARAMETER( StorageBuffer, FoliageWindVertexData );
        DESERT_SHADER_PARAMETER( StorageBuffer, FoliageWindIndexData );
    };

    [[nodiscard]] uint64_t InstanceKey( const uint32_t entity, const uint32_t instance )
    {
        return ( static_cast<uint64_t>( entity ) << 32u ) | instance;
    }

    [[nodiscard]] glm::uvec4 Bits( const glm::vec4& value )
    {
        return { std::bit_cast<uint32_t>( value.x ), std::bit_cast<uint32_t>( value.y ),
                 std::bit_cast<uint32_t>( value.z ), std::bit_cast<uint32_t>( value.w ) };
    }

    [[nodiscard]] float BoundsRadius( const Desert::Submesh& submesh )
    {
        return 0.5f * glm::length( submesh.BoundingBox.Max - submesh.BoundingBox.Min );
    }
} // namespace

namespace Desert::Graphic::RayTracing
{
    std::vector<float> RayTracingLodErrors( const Mesh& mesh, const uint64_t hiddenSubmeshes )
    {
        const std::vector<Submesh>& submeshes = mesh.GetSubmeshes();
        std::size_t                 levels    = 0;
        for ( std::size_t index = 0; index < submeshes.size(); ++index )
            if ( !IsSubmeshHidden( hiddenSubmeshes, index ) && HasRayTracingSegment( submeshes[index] ) )
                levels = std::max( levels, submeshes[index].LODs.size() );
        std::vector<float> errors;
        for ( std::size_t level = 1; level < levels && level <= kRayTracingMaxLod; ++level )
        {
            float error = 0.0f;
            for ( std::size_t index = 0; index < submeshes.size(); ++index )
            {
                const Submesh& submesh = submeshes[index];
                if ( IsSubmeshHidden( hiddenSubmeshes, index ) || !HasRayTracingSegment( submesh ) ||
                     submesh.LODs.size() <= 1u )
                    continue; // a submesh without a chain traces its LOD 0 at every level: no error
                // The level RayTracingLodRange cuts at this RT-LOD; its error is LODCutErrors[cut - 1].
                const std::size_t cut = std::min( level, submesh.LODs.size() - 1u );
                if ( submesh.LODCutErrors.size() < cut )
                    return errors; // no measured error for this level: never chosen
                error = std::max( error, submesh.LODCutErrors[cut - 1u] * BoundsRadius( submesh ) );
            }
            errors.push_back( error );
        }
        return errors;
    }

    uint8_t SelectRayTracingLod( const std::span<const float> errors, const float distance, const float scale,
                                 const uint8_t previous, const float errorPerCm )
    {
        uint8_t lod = 0;
        for ( std::size_t level = 1; level <= errors.size(); ++level )
        {
            const float reach = errorPerCm * distance * ( level <= previous ? 1.0f : kRayTracingLodHysteresis );
            if ( !( errors[level - 1u] * scale <= reach ) )
                break;
            lod = static_cast<uint8_t>( level );
        }
        return lod;
    }

    float RayTracingInstanceScale( const glm::mat4& transform )
    {
        return std::max( { glm::length( glm::vec3( transform[0] ) ), glm::length( glm::vec3( transform[1] ) ),
                           glm::length( glm::vec3( transform[2] ) ) } );
    }

    uint8_t RayTracingFoliageLod::Select( const uint32_t entity, const uint32_t instance,
                                          const std::span<const float> errors, const float distance,
                                          const float scale, const float errorPerCm )
    {
        const uint64_t key      = InstanceKey( entity, instance );
        const auto     found    = m_Previous.find( key );
        const uint8_t  previous = found != m_Previous.end() ? found->second : 0u;
        const uint8_t  lod      = SelectRayTracingLod( errors, distance, scale, previous, errorPerCm );
        m_Current[key]          = lod;
        return lod;
    }

    void RayTracingFoliageLod::EndGather()
    {
        m_Previous.swap( m_Current );
        m_Current.clear();
    }

    bool RayTracingFoliageWind::EnsurePipeline()
    {
        if ( m_Pipeline )
            return true;
        Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
        const std::shared_ptr<Shader> shader =
             service ? service->GetByName( std::string( kFoliageWindShader ) ) : nullptr;
        if ( !shader )
        {
            if ( !m_RefusalLogged )
                LOG_ERROR( "[RT] the shader '{}' did not compile - expected "
                           "Editor/Resources/Shaders/Programs/Compute/{}.shader; swaying foliage traces its "
                           "rest pose",
                           kFoliageWindShader, kFoliageWindShader );
            m_RefusalLogged = true;
            return false;
        }
        const auto built =
             ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( kFoliageWindShader ) } );
        if ( !built )
        {
            if ( !m_RefusalLogged )
                LOG_ERROR( "[RT] {}: {}; swaying foliage traces its rest pose", kFoliageWindPassName,
                           built.GetError() );
            m_RefusalLogged = true;
            return false;
        }
        m_Pipeline = built.GetValue();
        return true;
    }

    Common::BoolResultStr RayTracingFoliageWind::AddPasses(
         RDG::Builder& graph, const System::MeshRenderer& meshes, const std::span<RayTracingPrimitive> primitives,
         const std::span<const uint32_t> windPrimitives, const RDG::BufferRef camera, const glm::vec3& viewOrigin,
         const uint64_t frame, const RayTracingFoliageSettings& settings,
         std::vector<RayTracingDynamicGeometryBuild>& buildsOut, std::vector<RDG::BufferRef>& blasOut,
         RayTracingFoliageStats& stats )
    {
        if ( m_BudgetFrame != frame )
        {
            m_BudgetFrame = frame;
            m_BudgetUsed  = 0;
        }
        stats.Budget = settings.RefitBudget;
        // No budget or no distance (the Scalability level turns the wind off in RT): every swaying instance below
        // traces its rest pose.
        const bool refits = settings.RefitBudget > 0u && settings.WindDistance > 0.0f;

        // Every swaying instance traces its rest pose unless taken below; the nearest are the candidates.
        struct Candidate
        {
            uint32_t Primitive = 0;
            float    Distance  = 0.0f;
        };
        std::vector<Candidate> candidates;
        for ( const uint32_t p : windPrimitives )
        {
            if ( p >= primitives.size() )
                return Common::MakeFormattedError<bool>( "{}: wind primitive {} of {}", kFoliageWindPassName, p,
                                                         primitives.size() );
            RayTracingPrimitive& primitive = primitives[p];
            if ( primitive.Wind.MeshPtr == nullptr )
                continue;
            primitive.Geometry = primitive.Wind.Rest;
            ++stats.Swaying;
            const float distance = glm::length( glm::vec3( primitive.World[3] ) - viewOrigin );
            if ( refits && distance <= settings.WindDistance )
                candidates.push_back( { p, distance } );
        }
        if ( candidates.empty() )
        {
            stats.RestPose = stats.Swaying;
            return BOOLSUCCESS;
        }
        if ( !camera.IsValid() || !EnsurePipeline() )
        {
            if ( !camera.IsValid() && !m_RefusalLogged )
                LOG_WARN( "[RT] {}: the view gave no camera block; swaying foliage traces its rest pose",
                             kFoliageWindPassName );
            m_RefusalLogged = m_RefusalLogged || !camera.IsValid();
            stats.RestPose  = stats.Swaying;
            return BOOLSUCCESS;
        }
        std::ranges::sort( candidates, {}, &Candidate::Distance );

        // This frame's wind of each swaying field (the queue's record: Seconds moves every frame, a kept gather's
        // primitives do not carry it).
        std::unordered_map<uint64_t, InstanceWindPush> windOf;
        for ( const System::MeshRenderer::InstancedMeshRenderData& data : meshes.GetInstancedQueue() )
            if ( data.Mesh != nullptr && data.Wind.Sways() )
                windOf.try_emplace( InstanceKey( data.Entity, 0u ) ^
                                         static_cast<uint64_t>( reinterpret_cast<std::uintptr_t>( data.Mesh ) ),
                                    PackInstanceWind( data.Wind ) );

        struct Job
        {
            Entry*                        Item = nullptr;
            RDG::BufferRef                Vertices;
            RDG::BufferRef                Indices;
            RDG::BufferRef                Storage;
            RayTracingDynamicGeometryPlan Plan;
            uint32_t                      Invocations = 0;
        };
        std::vector<Job>        jobs;
        std::vector<glm::uvec4> jobRows;
        std::vector<glm::uvec4> copies;
        for ( const Candidate& candidate : candidates )
        {
            RayTracingPrimitive& primitive = primitives[candidate.Primitive];
            const uint64_t       key       = InstanceKey( primitive.Wind.Entity, primitive.Wind.Instance );
            Entry&               entry     = m_Entries[key];
            if ( entry.LastFrame == frame )
            {
                // An earlier view of this frame took it into the budget: instance the same BLAS.
                if ( entry.Refit && entry.Rest == primitive.Wind.Rest )
                {
                    primitive.Geometry = static_cast<const RayTracingGeometryResource*>( entry.Target.get() );
                    blasOut.push_back( graph.RegisterExternal( entry.Target->GetStorage(), "FoliageWind.BLAS" ) );
                    ++stats.Shared;
                }
                continue;
            }
            if ( m_BudgetUsed >= settings.RefitBudget )
                break;
            const auto wind = windOf.find(
                 InstanceKey( primitive.Wind.Entity, 0u ) ^
                 static_cast<uint64_t>( reinterpret_cast<std::uintptr_t>( primitive.Wind.MeshPtr ) ) );
            if ( wind == windOf.end() )
                continue; // the field stopped swaying this frame: its rest pose is its pose
            const auto* rest = static_cast<const RayTracingGeometryResource*>( primitive.Wind.Rest );
            const RayTracingGeometryLayout source = rest->GetMetadataLayout();
            if ( source.Segments.empty() || source.VertexAddress == 0 || source.IndexAddress == 0 ||
                 source.VertexStride != sizeof( Vertex ) )
                continue;

            // The target's layout: the vertex range the rest BLAS's segments address (its RT-LOD's visible
            // submeshes, [lowest FirstVertex, highest MaxVertex]) as Desert::Vertex - not the mesh's every vertex:
            // a hidden or trailing submesh is never copied - and its segments with their indices packed one after
            // another. Indices are submesh-local (added to FirstVertex), so the range's first vertex moves the
            // source address and every segment's FirstVertex / MaxVertex, never an index.
            // Segments is not empty (checked above) and each MaxVertex >= its FirstVertex (DescribeMeshGeometry).
            uint32_t firstVertex = std::numeric_limits<uint32_t>::max();
            uint32_t lastVertex  = 0u;
            for ( const RayTracingGeometrySegment& segment : source.Segments )
            {
                firstVertex = std::min( firstVertex, segment.FirstVertex );
                lastVertex  = std::max( lastVertex, segment.MaxVertex );
            }
            RayTracingDynamicMeshLayout layout;
            const uint32_t              vertexCount = lastVertex - firstVertex + 1u;
            const uint64_t              vertexAddress =
                 source.VertexAddress + uint64_t{ firstVertex } * uint64_t{ source.VertexStride };
            layout.VertexBytes   = uint64_t{ vertexCount } * sizeof( Vertex );
            const auto firstCopy = static_cast<uint32_t>( copies.size() );
            uint32_t   indexCount = 0;
            for ( RayTracingGeometrySegment segment : source.Segments )
            {
                const uint32_t count = segment.PrimitiveCount * 3u;
                copies.emplace_back( segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) ),
                                     indexCount, count, 0u );
                segment.VertexStride    = static_cast<uint32_t>( sizeof( Vertex ) );
                segment.PositionOffset  = static_cast<uint32_t>( offsetof( Vertex, Position ) );
                segment.FirstVertex    -= firstVertex;
                segment.MaxVertex      -= firstVertex;
                segment.IndexByteOffset = indexCount * static_cast<uint32_t>( sizeof( uint32_t ) );
                layout.Segments.push_back( segment );
                indexCount += count;
            }
            layout.IndexBytes = uint64_t{ indexCount } * sizeof( uint32_t );
            if ( vertexCount == 0 || indexCount == 0 )
            {
                copies.resize( firstCopy );
                continue;
            }

            if ( !entry.Target )
                entry.Target = CreateRayTracingDynamicMeshGeometry( "FoliageWind" );
            const Common::ResultStr<RayTracingDynamicGeometryPlan> plan = entry.Target->Prepare( layout, true );
            if ( !plan )
            {
                LOG_ERROR( "[RT] entity {} instance {}: its wind BLAS could not be prepared, it traces its rest "
                           "pose: {}",
                           primitive.Wind.Entity, primitive.Wind.Instance, plan.GetError() );
                copies.resize( firstCopy );
                continue;
            }
            entry.LastFrame = frame;
            entry.Refit     = true;
            entry.Rest      = primitive.Wind.Rest;
            ++m_BudgetUsed;

            const auto  job = static_cast<uint32_t>( jobs.size() );
            const auto& m   = primitive.World;
            jobRows.emplace_back( static_cast<uint32_t>( vertexAddress ),
                                  static_cast<uint32_t>( vertexAddress >> 32u ),
                                  static_cast<uint32_t>( source.IndexAddress ),
                                  static_cast<uint32_t>( source.IndexAddress >> 32u ) );
            jobRows.emplace_back( vertexCount, firstCopy, static_cast<uint32_t>( copies.size() ) - firstCopy,
                                  indexCount );
            for ( int column = 0; column < 4; ++column )
                jobRows.push_back( Bits( m[column] ) );
            jobRows.push_back( Bits( wind->second.A ) );
            jobRows.push_back( Bits( glm::vec4( wind->second.B.x, wind->second.B.y, 0.0f, 0.0f ) ) );
            static_assert( kFoliageWindJobRows == 8u );

            Job& made        = jobs.emplace_back();
            made.Item        = &entry;
            made.Plan        = plan.GetValue();
            made.Invocations = std::max( vertexCount, indexCount );
            made.Vertices    = graph.RegisterExternal( entry.Target->GetVertices(),
                                                       std::format( "FoliageWind.Vertices.{}", job ) );
            made.Indices     = graph.RegisterExternal( entry.Target->GetIndices(),
                                                       std::format( "FoliageWind.Indices.{}", job ) );
            made.Storage =
                 graph.RegisterExternal( entry.Target->GetStorage(), std::format( "FoliageWind.BLAS.{}", job ) );
            primitive.Geometry = static_cast<const RayTracingGeometryResource*>( entry.Target.get() );
            if ( made.Plan.Update )
                ++stats.Refit;
            else
                ++stats.Built;
        }
        stats.RestPose = stats.Swaying - stats.Refit - stats.Built - stats.Shared;
        if ( jobs.empty() )
            return BOOLSUCCESS;

        // The jobs' rows and copy ranges, then ONE pass: a binding block and a dispatch per job.
        const RDG::BufferRef jobBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ jobRows.size() * sizeof( glm::uvec4 ) }, "FoliageWind.Jobs" );
        graph.QueueBufferUpload( jobBuffer, std::as_bytes( std::span( jobRows ) ) );
        const RDG::BufferRef copyBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ copies.size() * sizeof( glm::uvec4 ) }, "FoliageWind.Copies" );
        graph.QueueBufferUpload( copyBuffer, std::as_bytes( std::span( copies ) ) );

        const std::shared_ptr<ComputePipeline> pipeline = m_Pipeline;
        const auto&                            layout   = m_Layout.Get( pipeline->GetSpecification().Shader );
        const std::shared_ptr<const RDG::OtherRouteFill> fill = Renderer::GetPipelineRouteFill( *pipeline );
        std::vector<ShaderPush::FoliageWindRayTracing>   pushes;
        std::vector<uint32_t>                            groups;
        for ( uint32_t job = 0; job < jobs.size(); ++job )
        {
            pushes.push_back( ShaderPush::FoliageWindRayTracing{
                 .Job = glm::uvec4( job, jobs[job].Plan.Update ? 0u : 1u, 0u, 0u ) } );
            groups.push_back( ( jobs[job].Invocations + kFoliageWindGroupSize - 1u ) / kFoliageWindGroupSize );
        }
        graph.AddPass(
             kFoliageWindPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 for ( const Job& job : jobs )
                     pass.Bindings( layout, fill )
                          .PushConstants<ShaderPush::FoliageWindRayTracing>()
                          .Uniform( FoliageWindShaderParameters::CameraUB, camera )
                          .Storage( FoliageWindShaderParameters::FoliageWindJobs, jobBuffer,
                                    RDG::Access::StorageRead )
                          .Storage( FoliageWindShaderParameters::FoliageWindCopies, copyBuffer,
                                    RDG::Access::StorageRead )
                          .Storage( FoliageWindShaderParameters::FoliageWindVertexData, job.Vertices,
                                    RDG::Access::StorageWrite )
                          .Storage( FoliageWindShaderParameters::FoliageWindIndexData, job.Indices,
                                    job.Plan.Update ? RDG::Access::StorageRead : RDG::Access::StorageWrite );
             },
             [pipeline, pushes, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 for ( uint32_t block = 0; block < pushes.size(); ++block )
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( block ) );
                     bindings.PushConstants( pushes[block] );
                     if ( const Common::BoolResultStr dispatched =
                               Renderer::DispatchCompute( bindings, *pipeline, groups[block], 1, 1 );
                          !dispatched )
                         return dispatched;
                 }
                 return BOOLSUCCESS;
             } );

        // Each job's refit (or first build): a request of the RT scene's one dynamic-geometry pass, declared after
        // this one (it reads the vertices this pass wrote).
        for ( const Job& job : jobs )
        {
            buildsOut.push_back( { .Target       = job.Item->Target.get(),
                                   .Vertices     = job.Vertices,
                                   .Indices      = job.Indices,
                                   .Storage      = job.Storage,
                                   .ScratchBytes = job.Plan.ScratchBytes } );
            stats.WindBytes += job.Item->Target->GetMemoryBytes() + job.Item->Target->GetBuildInputBytes();
        }
        return BOOLSUCCESS;
    }

    void CountRayTracingFoliage( const std::span<const RayTracingPrimitive> primitives,
                                 const RayTracingGeometryVariants& variants, RayTracingFoliageStats& stats )
    {
        std::unordered_set<const void*> shared;
        std::unordered_set<const void*> lod0;
        for ( const RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.Kind != RayTracingPrimitiveKind::Instanced )
                continue;
            ++stats.Instances;
            if ( primitive.Submeshes.Lod > 0u )
                ++stats.Lowered;
            const void* rest = primitive.LayoutOwner();
            if ( rest != nullptr && shared.insert( rest ).second )
                stats.SharedBytes += static_cast<const RayTracingGeometryResource*>( rest )->GetMemoryBytes();
            const auto* mesh = static_cast<const Mesh*>( primitive.SourceMesh );
            if ( mesh == nullptr )
                continue;
            RayTracingSubmeshMasks masks = primitive.Submeshes;
            masks.Lod                    = 0u;
            // Each resident LOD-0 BLAS once, however many instances (and masks sets) share it.
            const RayTracingGeometryResource* resident = variants.FindResident( *mesh, masks );
            if ( resident != nullptr && lod0.insert( resident ).second )
                stats.Lod0ResidentBytes += resident->GetMemoryBytes();
        }
        stats.Unique = static_cast<uint32_t>( shared.size() );
    }

    void RayTracingFoliageWind::EndFrame( const uint64_t frame )
    {
        std::erase_if( m_Entries, [frame]( const auto& entry ) { return entry.second.LastFrame != frame; } );
        for ( auto& item : m_Entries )
            item.second.Refit = false;
    }

    uint64_t RayTracingFoliageWind::GetMemoryBytes() const
    {
        uint64_t bytes = 0;
        for ( const auto& item : m_Entries )
            if ( item.second.Target )
                bytes += item.second.Target->GetMemoryBytes() + item.second.Target->GetBuildInputBytes();
        return bytes;
    }
} // namespace Desert::Graphic::RayTracing
