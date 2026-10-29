#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <Common/Core/Profiler.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Render/RayTracingMeshInput.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGather.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <unordered_set>

namespace Desert::Graphic::RayTracing
{
    RayTracingGeometryVariants::BuildFn MakeVariantBuild()
    {
        return []( Mesh& mesh, const RayTracingSubmeshMasks& masks )
                    -> Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>
        {
            auto built =
                 API::Vulkan::VulkanRayTracingGeometry::BuildStatic( DescribeStaticMeshBlas( mesh, masks ) );
            if ( !built )
                return Common::MakeError<std::shared_ptr<RayTracingGeometryResource>>( built.GetError() );
            return Common::MakeSuccess( std::shared_ptr<RayTracingGeometryResource>( built.GetValue() ) );
        };
    }

    namespace
    {
        // Programs/RayTracing/RayTracingGpuInstances.shader's push constants, one dispatch per source.
        struct GpuInstanceRowsPush
        {
            uint32_t RowBase      = 0; // the source's first row in the instance buffer
            uint32_t Rows         = 0; // its reserved rows
            uint32_t Record       = 0; // the indexed-indirect record stating the drawn count and first instance
            uint32_t IndexAndMask = 0; // customIndex:24 | mask:8 of every active row
            uint32_t SbtAndFlags  = 0; // sbtOffset:24 | flags:8 (the primitive's first segment record)
            uint32_t AddressLo    = 0; // the BLAS device address
            uint32_t AddressHi    = 0;
            uint32_t Pad          = 0;
        };
        constexpr uint32_t kGpuInstanceRowsGroupSize = 64; // the program's LocalSize

        // RayTracingGpuInstances.shader's buffers: bound through these members, checked against the shader's
        // reflected layout when the program is created.
        struct GpuInstanceRowsShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Transforms );
            DESERT_SHADER_PARAMETER( StorageBuffer, Args );
            DESERT_SHADER_PARAMETER( StorageBuffer, TlasRows );
            static constexpr std::array kAll{ RDG::Erase( Transforms ), RDG::Erase( Args ), RDG::Erase( TlasRows ) };
        };

        class GpuInstanceRows final : public RayTracingGpuInstanceRows
        {
        public:
            Common::BoolResultStr Prepare( std::span<const RayTracingGpuInstanceSource> sources,
                                           std::span<const RayTracingPrimitive>         primitives,
                                           RayTracingInstanceList&                      list ) override
            {
                m_Sources.clear();
                m_Rows = 0;
                m_Key  = 14695981039346656037ull;
                if ( sources.empty() )
                    return BOOLSUCCESS;
                if ( !m_Program )
                {
                    const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( kProgramName );
                    if ( !shader )
                        return Common::MakeError( std::format( "missing shader '{}'", kProgramName ) );
                    const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = kProgramName } );
                    if ( !made )
                        return Common::MakeError( made.GetError() );
                    if ( auto checked = m_Layout.Validate( shader, GpuInstanceRowsShaderParameters::kAll ); !checked )
                        return Common::MakeError( checked.GetError() );
                    m_Program = made.GetValue();
                }
                const auto skip = [&list]( RayTracingSkip reason )
                { ++list.Skipped[static_cast<std::size_t>( reason )]; };
                for ( const RayTracingGpuInstanceSource& source : sources )
                {
                    const RayTracingPrimitive& primitive = primitives[source.Primitive];
                    if ( primitive.Geometry == nullptr )
                    {
                        skip( RayTracingSkip::NoGeometry );
                        continue;
                    }
                    if ( primitive.FirstSegmentRecord > kMaxSegmentRecord )
                    {
                        skip( RayTracingSkip::SegmentRecordOutOfRange );
                        continue;
                    }
                    // A producer with no room this frame generated nothing: no row to trace.
                    if ( source.Refs == nullptr || source.Rows == 0 )
                        continue;
                    const uint64_t address =
                         static_cast<const RayTracingGeometryResource*>( primitive.Geometry )->GetDeviceAddress();
                    const auto mask =
                         static_cast<uint32_t>( kMaskOpaque | ( primitive.CastShadows ? kMaskShadowCaster : 0u ) );
                    GpuInstanceRowsPush push;
                    push.RowBase      = m_Rows;
                    push.Rows         = source.Rows;
                    push.Record       = source.Record;
                    push.IndexAndMask = kNoGpuSceneRow | ( mask << 24u );
                    push.SbtAndFlags  = primitive.FirstSegmentRecord & kMaxSegmentRecord;
                    push.AddressLo    = static_cast<uint32_t>( address );
                    push.AddressHi    = static_cast<uint32_t>( address >> 32u );
                    m_Sources.push_back( { *source.Refs, push } );
                    m_Rows += source.Rows;
                    for ( const uint64_t value : { address, uint64_t{ source.Rows }, uint64_t{ push.Record },
                                                   uint64_t{ push.IndexAndMask }, uint64_t{ push.SbtAndFlags } } )
                        m_Key = ( m_Key ^ value ) * 1099511628211ull;
                    list.InstancesByKind[static_cast<std::size_t>( RayTracingPrimitiveKind::GpuInstanced )] +=
                         source.Rows;
                }
                return BOOLSUCCESS;
            }

            uint32_t GetRows() const override
            {
                return m_Rows;
            }
            uint64_t GetKey() const override
            {
                return m_Key;
            }

            Common::BoolResultStr AddPass( RDG::Builder& graph, const RDG::BufferRef instances,
                                           const uint32_t firstRow ) override
            {
                std::vector<Source> sources = m_Sources;
                for ( Source& source : sources )
                    source.Push.RowBase += firstRow;
                graph.AddPass(
                     kGpuInstancesPassName, RDG::PassFlags::Compute,
                     [this, &sources, instances]( RDG::PassBuilder& pass )
                     {
                         const auto& layout = m_Layout.Get( m_Program->GetSpecification().Shader );
                         for ( const Source& source : sources )
                             pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_Program ) )
                                  .Storage( GpuInstanceRowsShaderParameters::Transforms, source.Refs.Instances, RDG::Access::StorageRead )
                                  .Storage( GpuInstanceRowsShaderParameters::Args, source.Refs.Args, RDG::Access::StorageRead )
                                  .Storage( GpuInstanceRowsShaderParameters::TlasRows, instances, RDG::Access::StorageWrite )
                                  .PushConstantBytes( static_cast<uint32_t>( sizeof( GpuInstanceRowsPush ) ) );
                     },
                     [program = m_Program, sources]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         auto& renderer = Renderer::GetInstance();
                         for ( uint32_t i = 0; i < sources.size(); ++i )
                         {
                             RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                             bindings.PushConstants( &sources[i].Push, sizeof( GpuInstanceRowsPush ) );
                             const uint32_t groups = ( sources[i].Push.Rows + kGpuInstanceRowsGroupSize - 1 ) /
                                                     kGpuInstanceRowsGroupSize;
                             if ( auto dispatched = renderer.DispatchCompute( bindings, *program, groups, 1, 1 );
                                  !dispatched )
                                 return dispatched;
                         }
                         return BOOLSUCCESS;
                     } );
                return BOOLSUCCESS;
            }

        private:
            static constexpr const char* kProgramName = "RayTracingGpuInstances";
            struct Source
            {
                GpuInstanceRefs     Refs;
                GpuInstanceRowsPush Push;
            };
            std::shared_ptr<ComputePipeline> m_Program;
            mutable ShaderBindingLayoutCache m_Layout;
            std::vector<Source>              m_Sources;
            uint32_t                         m_Rows = 0;
            uint64_t                         m_Key  = 0;
        };
    } // namespace

    std::unique_ptr<RayTracingGpuInstanceRows> CreateRayTracingGpuInstanceRows()
    {
        return std::make_unique<GpuInstanceRows>();
    }

    std::string FormatRayTracingSceneMemory( const RayTracingSceneStats& stats )
    {
        const auto mib = []( const uint64_t bytes ) { return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 ); };
        std::string text;
        auto        out = std::back_inserter( text );
        std::format_to( out, "ray tracing scene\n" );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "TLAS", mib( stats.TlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "TLAS instance rows", mib( stats.TlasInstanceBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "primitive instance ranges",
                        mib( stats.PrimitiveInstanceBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "TLAS build scratch", mib( stats.TlasScratchBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "dynamic BLAS scratch", mib( stats.DynamicScratchBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "traced BLAS", mib( stats.TracedBlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "variant BLAS (scene)", mib( stats.VariantBlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "procedural BLAS (scene)",
                        mib( stats.ProceduralBlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB  {} entries\n", "skin cache BLAS (scene)",
                        mib( stats.SkinCacheBlasBytes ), stats.SkinCacheEntries );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "skin cache positions",
                        mib( stats.SkinCachePositionsBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB ({} entries, {} geometries, {} levels lowered)\n",
                        "displaced geometry", mib( stats.DisplacedBufferBytes ), stats.DisplacedEntries,
                        stats.DisplacedGeometries, stats.DisplacedLevelsHeld );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "displacement topology",
                        mib( stats.DisplacedTopologyBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "landscape vertices", mib( stats.LandscapeVertexBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "landscape BLAS", mib( stats.LandscapeBlasBytes ) );
        return text;
    }

    namespace
    {
        // The gather's inputs: the queues the raster drew and the view's ISM distance cull.
        RayTracingGatherInputs GatherInputsOf( const System::MeshRenderer& meshes, const glm::vec3& viewOrigin,
                                               const float drawDistanceScale )
        {
        return { .Static                = meshes.GetStaticQueue(),
                 .Generic               = meshes.GetGenericQueue(),
                 .Instanced             = meshes.GetInstancedQueue(),
                 .InstancedGpuInstances = meshes.GetInstancedGpuInstances(),
                 .Skinned               = meshes.GetSkinnedQueue(),
                 .GpuInstanced          = meshes.GetGpuInstancedQueue(),
                 .ViewOrigin            = viewOrigin,
                 .DrawDistanceScale     = drawDistanceScale };
        }
    } // namespace

    Common::BoolResultStr RayTracingSceneRenderer::GatherScene()
    {
        if ( m_Frame.Built )
            return BOOLSUCCESS; // this frame's TLAS gather is the scene's rows already
        if ( m_SceneMeshes == nullptr ||
             m_SceneGatherFrame != Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() )
            return Common::MakeError( "RayTracingScene: a scene gather before this frame's AddPasses" );
        // Cached per primitive: the gather runs again only when what it reads changed IN STRUCTURE
        // (RayTracingSceneGatherKey::SameStructure, O(records)) or a TLAS gather overwrote it since. A key that
        // differs only in its records' worlds moves the kept primitives of those rows in place
        // (GetSceneMovedPrimitives). A still scene costs the key, not the per-instance ISM walk.
        m_SceneMoved.clear();
        const RayTracingGatherInputs inputs = GatherInputsOf( *m_SceneMeshes, m_SceneViewOrigin, m_SceneLensScale );
        {
            DESERT_PROFILE_SCOPE( "RayTracingScene: SceneGatherKey" );
            MakeRayTracingSceneGatherKey( inputs, m_SceneGatherScratchKey );
            if ( m_GatherIsScene && m_SceneGatherScratchKey.SameStructure( m_SceneGatherKey ) )
            {
                if ( m_SceneGatherScratchKey.Transforms == m_SceneGatherKey.Transforms )
                    return BOOLSUCCESS;
                MoveScenePrimitives();
                std::swap( m_SceneGatherKey, m_SceneGatherScratchKey );
                ++m_SceneGatherMoves;
                return BOOLSUCCESS;
            }
        }
        {
            // In the profile only on the frames that regather (a still scene: none after the first; a moving
            // primitive: none either, "RayTracingScene: SceneGatherMove" instead).
            DESERT_PROFILE_SCOPE( "RayTracingScene: SceneGather" );
            GatherRayTracingPrimitives( inputs, nullptr, m_Gather );
            // The primitives a world of the key moves: one per row for every kind with a CPU world of its own
            // (a static row merges its records; an ISM instance's world is its field snapshot, structure).
            m_ScenePrimitivesOfRow.clear();
            for ( std::size_t p = 0; p < m_Gather.Primitives.size(); ++p )
            {
                const RayTracingPrimitiveKind kind = m_Gather.Primitives[p].Kind;
                if ( kind == RayTracingPrimitiveKind::Static || kind == RayTracingPrimitiveKind::Generic ||
                     kind == RayTracingPrimitiveKind::Skinned )
                    m_ScenePrimitivesOfRow.emplace( m_Gather.Primitives[p].GpuSceneRow, static_cast<uint32_t>( p ) );
            }
        }
        std::swap( m_SceneGatherKey, m_SceneGatherScratchKey );
        m_GatherIsScene = true;
        ++m_SceneGathers;
        return BOOLSUCCESS;
    }

    void RayTracingSceneRenderer::MoveScenePrimitives()
    {
        // Same structure: the same primitives in the same order (RayTracingSceneGatherKey). Only the records whose
        // world differs are applied, each to the primitives of its row; a row's records share one world (the
        // gather takes the row's first), so a primitive moves once.
        DESERT_PROFILE_SCOPE( "RayTracingScene: SceneGatherMove" );
        const std::vector<glm::mat4>& now  = m_SceneGatherScratchKey.Transforms;
        const std::vector<glm::mat4>& kept = m_SceneGatherKey.Transforms;
        for ( std::size_t record = 0; record < now.size(); ++record )
        {
            if ( now[record] == kept[record] )
                continue;
            const auto [first, last] = m_ScenePrimitivesOfRow.equal_range( m_SceneGatherScratchKey.TransformRows[record] );
            for ( auto it = first; it != last; ++it )
            {
                RayTracingPrimitive& primitive = m_Gather.Primitives[it->second];
                if ( primitive.World == now[record] )
                    continue;
                primitive.World = now[record];
                m_SceneMoved.push_back( it->second );
            }
        }
    }

    Common::BoolResultStr RayTracingSceneRenderer::AddPasses(
         RDG::Builder& graph, bool required, const System::MeshRenderer& meshes, RayTracingSceneCache& cache,
         const glm::vec3& viewOrigin, const float drawDistanceScale, const RayTracingExternalPrimitives& external )
    {
        m_Frame     = {};
        m_Instances = {};
        m_SceneMoved.clear();
        m_Stats     = {};
        // What a scene gather of this frame reads (GatherScene), kept whether or not the TLAS is built.
        m_SceneMeshes      = &meshes;
        m_SceneViewOrigin  = viewOrigin;
        m_SceneLensScale   = drawDistanceScale;
        m_SceneGatherFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( !required )
            return BOOLSUCCESS;

        // The scene's shared geometry ends its previous frame on the first view of this one
        // (RayTracingSceneCache).
        cache.BeginViewFrame( Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
        // m_Gather becomes this frame's TLAS gather below: a later frame without a TLAS gathers the scene anew.
        m_GatherIsScene = false;

        m_DynamicBlas.clear();
        m_DynamicBuilds.clear();
        {
            // Every device object the scene's cache makes below (BLAS variants, skin-cache BLASes and positions,
            // procedural BLASes) is a row of the SCENE, not of this view (RT-LEDGER-2): shared by every view, so
            // in no view's budget. The view's own scope (its TLAS below, its landscape tiles) resumes after.
            const ResourceAttributionScope sceneRows = cache.OpenLedgerScope();

            // Everything the raster queued, frustum-independent (RayTracingGather.hpp).
            GatherRayTracingPrimitives( GatherInputsOf( meshes, viewOrigin, drawDistanceScale ),
                                        &cache.GetVariants(), m_Gather );
            // Skinned primitives trace their per-frame BLAS, built from the positions the scene's skin step
            // (RayTracingSkinCache::SkinFrame) already wrote this frame - no skinning here; an earlier view's
            // build is reused. The gather appends one skinned primitive per skinned record, last.
            const auto firstSkinned =
                 static_cast<uint32_t>( m_Gather.Primitives.size() - meshes.GetSkinnedQueue().size() );
            if ( const Common::BoolResultStr skinned =
                      cache.GetSkinCache().AddPasses( graph, meshes, m_Gather.Primitives, firstSkinned,
                                                      cache.GetFrame(), m_DynamicBuilds, m_DynamicBlas );
                 !skinned )
                return skinned;
            // Meshes the uploader built no BLAS for (procedural): built in the same pass, once per buffer upload.
            if ( const Common::BoolResultStr procedural = cache.GetProcedural().AddBuilds(
                      graph, m_Gather.Primitives, cache.GetFrame(), m_DynamicBuilds, m_DynamicBlas );
                 !procedural )
                return procedural;
        }
        // Primitives other renderers produced (landscape tiles), after the skinned ones (firstSkinned above counts
        // from the end): their BLAS requests join the same pass, and their BLASes built in an earlier frame are
        // read by the TLAS like every other dynamic BLAS.
        const std::size_t firstExternal      = m_Gather.Primitives.size();
        const auto        externalRecordBase = static_cast<uint32_t>( m_Gather.SegmentRecords.size() );
        m_Gather.Primitives.insert( m_Gather.Primitives.end(), external.Primitives.begin(),
                                    external.Primitives.end() );
        m_Gather.SegmentRecords.insert( m_Gather.SegmentRecords.end(), external.SegmentRecords.begin(),
                                        external.SegmentRecords.end() );
        for ( std::size_t i = firstExternal; i < m_Gather.Primitives.size(); ++i )
            m_Gather.Primitives[i].FirstSegmentRecord += externalRecordBase;
        m_DynamicBuilds.insert( m_DynamicBuilds.end(), external.Builds.begin(), external.Builds.end() );
        m_DynamicBlas.insert( m_DynamicBlas.end(), external.Resident.begin(), external.Resident.end() );
        // ONE pass builds every dynamic BLAS of the view (skin cache, procedural, landscape tiles).
        const Common::ResultStr<uint64_t> dynamicScratch =
             AddRayTracingDynamicGeometryPass( graph, m_DynamicBuilds, m_DynamicBlas );
        if ( !dynamicScratch )
            return Common::MakeError( dynamicScratch.GetError() );
        m_Instances = BuildRayTracingInstances( m_Gather.Primitives );
        for ( std::size_t reason = 0; reason < m_Instances.Skipped.size(); ++reason )
            m_Instances.Skipped[reason] += m_Gather.Skipped[reason];
        // The GPU-instanced primitives' rows (the landscape grass), after the CPU rows: their BLAS is final now
        // (variants, procedural builds above).
        if ( !m_GpuInstanceRows && !m_Gather.GpuInstanced.empty() )
            m_GpuInstanceRows = CreateRayTracingGpuInstanceRows();
        if ( m_GpuInstanceRows )
            if ( const Common::BoolResultStr prepared =
                      m_GpuInstanceRows->Prepare( m_Gather.GpuInstanced, m_Gather.Primitives, m_Instances );
                 !prepared )
                return Common::MakeError( std::format( "{}: {}", kGpuInstancesPassName, prepared.GetError() ) );

        // The GPU twin of the segment records: record i of SegmentRecords becomes geometry record i, the row an
        // instance's SBT offset (its FirstSegmentRecord) + a hit's geometry index names. A landscape tile's masked
        // segment tests its holes (the producer's LandscapeOpacity). No mesh material declares an opacity source
        // the metadata can read yet (REMAINDER RT-UNIFY): every alpha-tested mesh segment is counted in
        // SegmentsWithoutOpacity, and its candidates are confirmed as hits.
        const Common::ResultStr<RayTracingMetadataBuild> metadata = BuildRayTracingMetadata(
             m_Gather.Primitives, m_Gather.SegmentRecords,
             [&external]( const RayTracingSegmentRecord& segment ) -> std::optional<RayTracingMaterialRecord>
             {
                 // A generic record's own alpha test (3D text's glyph coverage, RayTracingProgramHit::Opacity).
                 if ( segment.ProgramHit != nullptr )
                     return segment.ProgramHit->Opacity;
                 if ( segment.LandscapeTile == kNoLandscapeTile ||
                      segment.LandscapeTile >= external.LandscapeOpacity.size() )
                     return std::nullopt;
                 return external.LandscapeOpacity[segment.LandscapeTile];
             } );
        if ( !metadata )
            return Common::MakeError( metadata.GetError() );

        if ( !m_Target )
            m_Target = CreateRayTracingSceneTarget();
        const Common::ResultStr<RayTracingSceneFrame> frame =
             AddRayTracingScenePass( graph, required, m_Instances, *m_Target, m_DynamicBlas,
                                     !m_DynamicBuilds.empty(), m_GpuInstanceRows.get() );
        if ( !frame )
            return Common::MakeError( frame.GetError() );
        m_Frame = frame.GetValue();
        m_GeometryRecords.clear();
        if ( m_Frame.Built )
        {
            m_Frame.Metadata = AddRayTracingMetadataUpload( graph, metadata.GetValue().Metadata );
            m_GeometryRecords.assign( metadata.GetValue().Metadata.Geometry.begin(),
                                      metadata.GetValue().Metadata.Geometry.end() );
        }

        m_Stats.Built           = m_Frame.Built;
        m_Stats.InstancesByKind = m_Instances.InstancesByKind;
        m_Stats.SkippedByReason = m_Instances.Skipped;
        std::unordered_set<const void*> traced;
        for ( const RayTracingInstance& instance : m_Instances.Instances )
            if ( traced.insert( instance.Geometry ).second )
                m_Stats.TracedBlasBytes +=
                     static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetMemoryBytes();
        for ( const RayTracingGpuInstanceSource& source : m_Gather.GpuInstanced )
            if ( const void* geometry = m_Gather.Primitives[source.Primitive].Geometry;
                 geometry != nullptr && traced.insert( geometry ).second )
                m_Stats.TracedBlasBytes +=
                     static_cast<const RayTracingGeometryResource*>( geometry )->GetMemoryBytes();
        m_Stats.VariantBlasBytes                    = cache.GetVariants().GetMemoryBytes();
        m_Stats.ProceduralBlasBytes                 = cache.GetProcedural().GetMemoryBytes();
        const RayTracingSkinCache::MemoryBytes skin = cache.GetSkinCache().GetMemoryBytes();
        m_Stats.SkinCacheBlasBytes                  = skin.Blas;
        m_Stats.SkinCachePositionsBytes             = skin.Positions;
        m_Stats.TlasBytes                           = m_Target->GetMemoryBytes();
        m_Stats.TlasInstanceBytes                   = m_Frame.InstanceBytes;
        m_Stats.PrimitiveInstanceBytes              = m_Frame.PrimitiveInstanceBytes;
        m_Stats.TlasScratchBytes                    = m_Frame.ScratchBytes;
        m_Stats.SegmentsWithoutOpacity              = metadata.GetValue().SegmentsWithoutOpacity;
        m_Stats.DynamicScratchBytes                 = dynamicScratch.GetValue();
        m_Stats.SkinCacheEntries     = static_cast<uint32_t>( cache.GetSkinCache().GetEntryCount() );
        const System::DisplacementStats displaced = cache.GetDisplacement().GetStats();
        m_Stats.DisplacedBufferBytes   = displaced.BufferBytes;
        m_Stats.DisplacedTopologyBytes = displaced.TopologyBytes;
        m_Stats.DisplacedEntries       = displaced.Entries;
        m_Stats.DisplacedGeometries    = displaced.Geometries;
        m_Stats.DisplacedLevelsLowered = displaced.LevelsLowered;
        m_Stats.DisplacedLevelsHeld    = displaced.LevelsHeld;
        m_Stats.LandscapeBuilt       = external.Built;
        m_Stats.LandscapeRefit       = external.Refit;
        m_Stats.LandscapeVertexBytes = external.VertexBytes;
        m_Stats.LandscapeBlasBytes   = external.BlasBytes;
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
