#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <Common/Core/Profiler.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/GpuDriven/GpuScene.hpp>
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
                 .Skinned               = meshes.GetSkinnedQueue(),
                 .GpuInstanced          = meshes.GetGpuInstancedQueue(),
                 .ViewOrigin            = viewOrigin,
                 .DrawDistanceScale     = drawDistanceScale };
        }
    } // namespace

    std::optional<std::span<const GpuDriven::PrimitiveChange>>
    RayTracingSceneRenderer::KeptGatherChanges( const GpuDriven::GpuScene& scene, const RayTracingGatherInputs& inputs )
    {
        MakeRayTracingSceneUnsyncedKey( inputs, m_SceneViewCulls, m_SceneUnsyncedScratch );
        const std::optional<std::span<const GpuDriven::PrimitiveChange>> changes =
             m_GatherIsScene && m_SceneStream == scene.GetStreamId() ? scene.ChangesSince( m_SceneCursor )
                                                                     : std::nullopt;
        const auto structural = []( const GpuDriven::PrimitiveChange& change )
        {
            // A meshless primitive (a landscape tile) is no record of the queues the gather reads.
            return change.Kind == GpuDriven::PrimitiveChangeKind::Discarded ||
                   ( change.Meshed && change.Kind != GpuDriven::PrimitiveChangeKind::Moved );
        };
        if ( changes && m_SceneUnsyncedScratch == m_SceneUnsyncedKey &&
             std::none_of( changes->begin(), changes->end(), structural ) )
            return changes;
        return std::nullopt;
    }

    void RayTracingSceneRenderer::FollowGather( const GpuDriven::GpuScene& scene, const RayTracingGatherInputs& inputs )
    {
        // The primitives a Moved change moves: one per row for every kind with a CPU world of its own (a static
        // row merges its records; an ISM instance's world is its field's instances, an Updated change).
        m_ScenePrimitivesOfRow.clear();
        for ( std::size_t p = 0; p < m_Gather.Primitives.size(); ++p )
        {
            const RayTracingPrimitiveKind kind = m_Gather.Primitives[p].Kind;
            if ( kind == RayTracingPrimitiveKind::Static || kind == RayTracingPrimitiveKind::Generic ||
                 kind == RayTracingPrimitiveKind::Skinned )
                m_ScenePrimitivesOfRow.emplace( m_Gather.Primitives[p].GpuSceneRow, static_cast<uint32_t>( p ) );
        }
        // The gather read the queues of this frame's sync: every change up to now is in it.
        m_SceneStream    = scene.GetStreamId();
        m_SceneCursor    = scene.GetChangeSerial();
        m_SceneViewCulls = RayTracingGatherCullsByView( inputs );
        MakeRayTracingSceneUnsyncedKey( inputs, m_SceneViewCulls, m_SceneUnsyncedKey );
        m_GatherIsScene = true;
        ++m_SceneGathers;
        ++m_LayoutSerial;
    }

    Common::BoolResultStr RayTracingSceneRenderer::GatherScene()
    {
        if ( m_Frame.Built )
            return BOOLSUCCESS; // this frame's TLAS gather is the scene's rows already
        if ( m_SceneMeshes == nullptr ||
             m_SceneGatherFrame != Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() )
            return Common::MakeError( "RayTracingScene: a scene gather before this frame's AddPasses" );
        // Kept current by the GpuScene change stream (GatherScene in the header): a frame reads the changes since
        // its cursor - none on a still scene, one Moved per moving primitive - and gathers in full only on a
        // structure change. No key over the records, no compare over them.
        m_SceneMoved.clear();
        const GpuDriven::GpuScene* scene = m_SceneMeshes->GetGpuScene();
        if ( scene == nullptr )
            return Common::MakeError( "RayTracingScene: a scene gather before this frame's GPU-scene sync" );
        const RayTracingGatherInputs inputs = GatherInputsOf( *m_SceneMeshes, m_SceneViewOrigin, m_SceneLensScale );
        // A TLAS gather resolved BLASes and carries the external tail: a frame without a TLAS gathers anew.
        if ( !m_GatherHasBlas )
        {
            DESERT_PROFILE_SCOPE( "RayTracingScene: SceneGatherChanges" );
            if ( const auto changes = KeptGatherChanges( *scene, inputs ) )
            {
                m_SceneCursor = scene->GetChangeSerial();
                if ( !changes->empty() && MoveScenePrimitives( *scene, *changes ) )
                    ++m_SceneGatherMoves;
                return BOOLSUCCESS;
            }
        }
        {
            // In the profile only on the frames that regather (a still scene: none after the first; a moving
            // primitive: none either, "RayTracingScene: SceneGatherMove" instead).
            DESERT_PROFILE_SCOPE( "RayTracingScene: SceneGather" );
            GatherRayTracingPrimitives( inputs, nullptr, m_Gather );
            m_GatherHasBlas = false;
            m_ListValid     = false; // the instance list indexes another primitive list now
            FollowGather( *scene, inputs );
        }
        return BOOLSUCCESS;
    }

    bool RayTracingSceneRenderer::MoveScenePrimitives( const GpuDriven::GpuScene&                     scene,
                                                       const std::span<const GpuDriven::PrimitiveChange> changes )
    {
        // Only Moved changes reach here (GatherScene). Applied in stream order, so a primitive moved twice since
        // the cursor ends at its last world; the row's records share that world (the registry merges rigid
        // records of one entity by world), so every kept primitive of the row takes it.
        DESERT_PROFILE_SCOPE( "RayTracingScene: SceneGatherMove" );
        for ( const GpuDriven::PrimitiveChange& change : changes )
        {
            if ( !scene.IsValid( change.Primitive ) )
                continue;
            const uint32_t row            = scene.Primitive( change.Primitive ).FirstInstance;
            const auto [first, last]      = m_ScenePrimitivesOfRow.equal_range( row );
            for ( auto it = first; it != last; ++it )
            {
                RayTracingPrimitive& primitive = m_Gather.Primitives[it->second];
                if ( primitive.World == change.World )
                    continue;
                primitive.World = change.World;
                m_SceneMoved.push_back( it->second );
            }
        }
        std::sort( m_SceneMoved.begin(), m_SceneMoved.end() );
        m_SceneMoved.erase( std::unique( m_SceneMoved.begin(), m_SceneMoved.end() ), m_SceneMoved.end() );
        return !m_SceneMoved.empty();
    }

    bool RayTracingSceneRenderer::KeepTlasGather( const System::MeshRenderer& meshes, RayTracingSceneCache& cache,
                                                  const RayTracingGatherInputs&                               inputs,
                                                  std::optional<std::span<const GpuDriven::PrimitiveChange>>& changes )
    {
        if ( !m_GatherIsScene || !m_GatherHasBlas )
            return false;
        const GpuDriven::GpuScene* scene = meshes.GetGpuScene();
        if ( scene == nullptr )
            return false;
        changes = KeptGatherChanges( *scene, inputs );
        if ( !changes )
            return false;
        // The skin cache fills the gather's skinned primitives in queue order (they are its last ones): the same
        // records, rows and meshes, and no more skinned primitives than the queue has.
        const std::span<const System::MeshRenderer::SkinnedMeshRenderData> skinned = meshes.GetSkinnedQueue();
        if ( skinned.size() > m_GatherSceneCount )
            return false;
        const std::size_t firstSkinned = m_GatherSceneCount - skinned.size();
        if ( firstSkinned > 0 && m_Gather.Primitives[firstSkinned - 1].Kind == RayTracingPrimitiveKind::Skinned )
            return false;
        for ( std::size_t i = 0; i < skinned.size(); ++i )
        {
            const RayTracingPrimitive& primitive = m_Gather.Primitives[firstSkinned + i];
            if ( primitive.Kind != RayTracingPrimitiveKind::Skinned ||
                 primitive.GpuSceneRow != skinned[i].GpuInstance ||
                 primitive.DeformedMesh != static_cast<const void*>( skinned[i].Mesh ) )
                return false;
        }
        // The variant cache drops a variant no Resolve asked for in a frame: every variant of the kept gather is
        // asked again, and a different answer (a re-uploaded mesh, a dropped and rebuilt variant) regathers.
        for ( const RayTracingVariantUse& use : m_Gather.VariantUses )
        {
            const Common::ResultStr<const RayTracingGeometryResource*> resolved =
                 cache.GetVariants().Resolve( *use.MeshPtr, use.Masks );
            if ( !resolved || static_cast<const void*>( resolved.GetValue() ) != use.Geometry )
                return false;
        }
        return true;
    }

    bool RayTracingSceneRenderer::SameExternal( const RayTracingExternalPrimitives& external ) const
    {
        if ( external.Primitives.size() != m_LastExternalPrimitives.size() ||
             !std::equal( external.SegmentRecords.begin(), external.SegmentRecords.end(),
                          m_LastExternalRecords.begin(), m_LastExternalRecords.end() ) ||
             !std::equal( external.LandscapeOpacity.begin(), external.LandscapeOpacity.end(),
                          m_LastLandscapeOpacity.begin(), m_LastLandscapeOpacity.end() ) )
            return false;
        for ( std::size_t i = 0; i < external.Primitives.size(); ++i )
        {
            const RayTracingPrimitive& now  = external.Primitives[i];
            const RayTracingPrimitive& then = m_LastExternalPrimitives[i];
            if ( now.Geometry != then.Geometry || now.GpuSceneRow != then.GpuSceneRow ||
                 now.CastShadows != then.CastShadows || now.Translucent != then.Translucent ||
                 now.FirstSegmentRecord != then.FirstSegmentRecord ||
                 now.SegmentRecordCount != then.SegmentRecordCount || now.Kind != then.Kind ||
                 now.ProceduralMesh != then.ProceduralMesh || now.SourceMesh != then.SourceMesh )
                return false;
        }
        return true;
    }

    Common::BoolResultStr RayTracingSceneRenderer::AddPasses(
         RDG::Builder& graph, bool required, const System::MeshRenderer& meshes, RayTracingSceneCache& cache,
         const glm::vec3& viewOrigin, const float drawDistanceScale, const RayTracingExternalPrimitives& external )
    {
        // m_Instances, m_SceneRows and m_Metadata are kept across frames: a frame whose primitives only moved
        // patches them (THE TLAS FOLLOWS THE CHANGE STREAM in the header).
        m_Frame = {};
        m_SceneMoved.clear();
        m_Stats         = {};
        m_FrameDeformed = false;
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

        m_DynamicBlas.clear();
        m_DynamicBuilds.clear();
        const RayTracingGatherInputs inputs   = GatherInputsOf( meshes, viewOrigin, drawDistanceScale );
        const GpuDriven::GpuScene*   scene    = meshes.GetGpuScene();
        bool                         relisted = !m_ListValid; // the instance list is laid out anew this frame
        {
            // Every device object the scene's cache makes below (BLAS variants, skin-cache BLASes and positions,
            // procedural BLASes) is a row of the SCENE, not of this view (RT-LEDGER-2): shared by every view, so
            // in no view's budget. The view's own scope (its TLAS below, its landscape tiles) resumes after.
            const ResourceAttributionScope sceneRows = cache.OpenLedgerScope();

            std::optional<std::span<const GpuDriven::PrimitiveChange>> changes;
            if ( KeepTlasGather( meshes, cache, inputs, changes ) )
            {
                // Only moves since the kept gather: its scene part stays, the moved primitives take their world,
                // the external tail and the per-frame BLASes are this frame's again.
                DESERT_PROFILE_SCOPE( "RayTracingScene: TlasGatherMove" );
                m_Gather.Primitives.resize( m_GatherSceneCount );
                m_Gather.SegmentRecords.resize( m_GatherSceneRecords );
                m_Gather.Errors.clear();
                m_SceneCursor = scene->GetChangeSerial();
                if ( !changes->empty() && MoveScenePrimitives( *scene, *changes ) )
                    ++m_SceneGatherMoves;
                for ( const uint32_t p : m_DynamicPrimitives )
                {
                    RayTracingPrimitive& primitive     = m_Gather.Primitives[p];
                    primitive.Geometry                 = nullptr;
                    primitive.DeformedPositionsAddress = 0;
                    primitive.DeformedPositions        = {};
                }
            }
            else
            {
                // Everything the raster queued, frustum-independent (RayTracingGather.hpp).
                DESERT_PROFILE_SCOPE( "RayTracingScene: TlasGather" );
                GatherRayTracingPrimitives( inputs, &cache.GetVariants(), m_Gather );
                m_GatherSceneCount   = m_Gather.Primitives.size();
                m_GatherSceneRecords = m_Gather.SegmentRecords.size();
                m_DynamicPrimitives.clear();
                for ( std::size_t p = 0; p < m_Gather.Primitives.size(); ++p )
                    if ( m_Gather.Primitives[p].Kind == RayTracingPrimitiveKind::Skinned ||
                         m_Gather.Primitives[p].ProceduralMesh != nullptr )
                        m_DynamicPrimitives.push_back( static_cast<uint32_t>( p ) );
                m_GatherHasBlas = true;
                relisted        = true;
                if ( scene != nullptr )
                    FollowGather( *scene, inputs );
                else
                {
                    // No stream to follow: the next frame gathers anew.
                    m_GatherIsScene = false;
                    ++m_SceneGathers;
                    ++m_LayoutSerial;
                }
            }
            // Skinned primitives trace their per-frame BLAS, built from the positions the scene's skin step
            // (RayTracingSkinCache::SkinFrame) already wrote this frame - no skinning here; an earlier view's
            // build is reused. The gather appends one skinned primitive per skinned record, last.
            const auto firstSkinned =
                 static_cast<uint32_t>( m_GatherSceneCount - meshes.GetSkinnedQueue().size() );
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
        for ( const uint32_t p : m_DynamicPrimitives )
            if ( m_Gather.Primitives[p].DeformedPositions.IsValid() )
                m_FrameDeformed = true;

        // The rows a kept list re-encodes this frame: the moved primitives, the dynamic ones (their BLAS is this
        // frame's) and the external tail.
        const auto forEachPatched = [&]( const auto& visit )
        {
            for ( const uint32_t p : m_SceneMoved )
                visit( p );
            for ( const uint32_t p : m_DynamicPrimitives )
                visit( p );
            for ( std::size_t p = firstExternal; p < m_Gather.Primitives.size(); ++p )
                visit( static_cast<uint32_t>( p ) );
        };
        if ( !relisted )
        {
            // A row whose primitive enters or leaves the list, or traces another BLAS, is no move: the list is
            // laid out anew (BuildRayTracingInstances) and the TLAS built.
            relisted =
                 m_Instances.InstanceOfPrimitive.size() != m_Gather.Primitives.size() || !SameExternal( external );
            if ( !relisted )
                forEachPatched(
                     [&]( const uint32_t p )
                     {
                         const RayTracingPrimitive& primitive = m_Gather.Primitives[p];
                         const uint32_t             instance  = m_Instances.InstanceOfPrimitive[p];
                         const bool traced = primitive.Kind != RayTracingPrimitiveKind::GpuInstanced &&
                                             !RayTracingInstanceSkipOf( primitive ).has_value();
                         if ( ( instance != kNoRayTracingInstance ) != traced ||
                              ( instance != kNoRayTracingInstance &&
                                m_Instances.Instances[instance].Geometry != primitive.Geometry ) )
                             relisted = true;
                     } );
        }
        if ( relisted )
        {
            DESERT_PROFILE_SCOPE( "RayTracingScene: InstanceList" );
            m_Instances = BuildRayTracingInstances( m_Gather.Primitives );
            for ( std::size_t reason = 0; reason < m_Instances.Skipped.size(); ++reason )
                m_Instances.Skipped[reason] += m_Gather.Skipped[reason];
            m_ListSkipped         = m_Instances.Skipped;
            m_ListInstancesByKind = m_Instances.InstancesByKind;
            m_SceneRows           = EncodeRayTracingSceneRows( m_Instances );
            m_ListValid           = true;
            m_BuildPending        = true;
            ++m_LayoutSerial;
            m_LastExternalPrimitives.assign( external.Primitives.begin(), external.Primitives.end() );
            m_LastExternalRecords.assign( external.SegmentRecords.begin(), external.SegmentRecords.end() );
            m_LastLandscapeOpacity.assign( external.LandscapeOpacity.begin(), external.LandscapeOpacity.end() );
        }
        else
        {
            DESERT_PROFILE_SCOPE( "RayTracingScene: InstancePatch" );
            m_Instances.Skipped         = m_ListSkipped;
            m_Instances.InstancesByKind = m_ListInstancesByKind;
            forEachPatched(
                 [&]( const uint32_t p )
                 {
                     const uint32_t instance = m_Instances.InstanceOfPrimitive[p];
                     if ( instance == kNoRayTracingInstance )
                         return;
                     m_Instances.Instances[instance] = MakeRayTracingInstance( m_Gather.Primitives[p] );
                     if ( PatchRayTracingSceneRow( m_SceneRows, instance, m_Instances.Instances[instance] ) )
                         ++m_TlasMovedRows;
                 } );
        }
        // The GPU-instanced primitives' rows (the landscape grass), after the CPU rows: their BLAS is final now
        // (variants, procedural builds above).
        if ( !m_GpuInstanceRows && !m_Gather.GpuInstanced.empty() )
            m_GpuInstanceRows = CreateRayTracingGpuInstanceRows();
        bool build = relisted || m_BuildPending;
        if ( m_GpuInstanceRows )
        {
            if ( const Common::BoolResultStr prepared =
                      m_GpuInstanceRows->Prepare( m_Gather.GpuInstanced, m_Gather.Primitives, m_Instances );
                 !prepared )
                return Common::MakeError( std::format( "{}: {}", kGpuInstancesPassName, prepared.GetError() ) );
            // GPU-written rows that trace other BLASes are no move either.
            const uint64_t key = m_GpuInstanceRows->GetKey();
            build              = build || key != m_LastGpuRowsKey;
            m_LastGpuRowsKey   = key;
        }
        // BVH quality: an update refits the last build's tree, never re-sorts it; after the set number of updates
        // in a row the TLAS is built again (0: every frame).
        build = build || m_TlasUpdatesBeforeRebuild == 0 || m_UpdatesSinceBuild >= m_TlasUpdatesBeforeRebuild;

        // The GPU twin of the segment records: record i of SegmentRecords becomes geometry record i, the row an
        // instance's SBT offset (its FirstSegmentRecord) + a hit's geometry index names. A landscape tile's masked
        // segment tests its holes (the producer's LandscapeOpacity). No mesh material declares an opacity source
        // the metadata can read yet (REMAINDER RT-UNIFY): every alpha-tested mesh segment is counted in
        // SegmentsWithoutOpacity, and its candidates are confirmed as hits. Kept with the list: made again when
        // the list was, or when a row of it is this frame's (a skinned stream's address, a procedural or a
        // landscape BLAS).
        if ( relisted || !m_Metadata || !m_DynamicPrimitives.empty() || m_Gather.Primitives.size() > firstExternal )
        {
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
            {
                m_Metadata.reset();
                return Common::MakeError( metadata.GetError() );
            }
            m_Metadata = metadata.GetValue();
            m_GeometryRecords.assign( m_Metadata->Metadata.Geometry.begin(), m_Metadata->Metadata.Geometry.end() );
        }

        if ( !m_Target )
            m_Target = CreateRayTracingSceneTarget();
        const Common::ResultStr<RayTracingSceneFrame> frame =
             AddRayTracingScenePass( graph, required, m_SceneRows, *m_Target, m_DynamicBlas, !m_DynamicBuilds.empty(),
                                     m_GpuInstanceRows.get(), !build );
        if ( !frame )
            return Common::MakeError( frame.GetError() );
        m_Frame = frame.GetValue();
        if ( m_Frame.Built )
        {
            if ( m_Frame.Updated )
            {
                ++m_TlasUpdates;
                ++m_UpdatesSinceBuild;
            }
            else
            {
                ++m_TlasFullBuilds;
                m_UpdatesSinceBuild = 0;
                m_BuildPending      = false;
            }
            m_Frame.Metadata = AddRayTracingMetadataUpload( graph, m_Metadata->Metadata );
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
        m_Stats.SegmentsWithoutOpacity              = m_Metadata->SegmentsWithoutOpacity;
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
