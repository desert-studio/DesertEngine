#include "MeshRenderer.hpp"
#include "MeshRendererInternal.hpp"

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <optional>
#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>

namespace Desert::Graphic::System
{
    namespace MeshRendererDetail
    {
        // One object's row of `Materials[]` (MaterialPropertyBlock-style): the material's own row, then every
        // overridden instance property written into ITS slot, found by name in the shader's manifest — the
        // generic transport (Core/Formats/MaterialParamRow.hpp), no per-parameter code. Names without a slot
        // (the Transform push value, textures) are not row bytes and are skipped.
        Core::Formats::MaterialParamRow EffectiveRow( const DataDrivenMaterial* material,
                                                      MaterialInstance*         instance )
        {
            Core::Formats::MaterialParamRow row = material->GetParamRow();
            if ( instance == nullptr )
                return row;
            for ( const auto& [name, prop] : instance->GetPropertySet().GetProperties() )
            {
                if ( !prop.bIsOverridden )
                    continue;
                // The authored value as a vec4, then written as the bytes of the parameter's declared type
                // (Core::Formats::MaterialRowSlotValue, via SetMaterialParam) — an Int/Bool slot is an int.
                // A value that is not row bytes (a matrix, a texture) is skipped.
                const std::optional<glm::vec4> authored = std::visit(
                     []( const auto& v ) -> std::optional<glm::vec4>
                     {
                         using T = std::decay_t<decltype( v )>;
                         if constexpr ( std::is_same_v<T, float> )
                             return glm::vec4( v, 0.0f, 0.0f, 0.0f );
                         else if constexpr ( std::is_same_v<T, int> || std::is_same_v<T, bool> )
                             return glm::vec4( static_cast<float>( v ), 0.0f, 0.0f, 0.0f );
                         else if constexpr ( std::is_same_v<T, glm::vec2> )
                             return glm::vec4( v, 0.0f, 0.0f );
                         else if constexpr ( std::is_same_v<T, glm::vec3> )
                             return glm::vec4( v, 0.0f );
                         else if constexpr ( std::is_same_v<T, glm::vec4> )
                             return v;
                         else
                             return std::nullopt;
                     },
                     prop.Value );
                if ( authored )
                    Core::Formats::SetMaterialParam( material->GetSchema(), row, name, *authored );
            }
            return row;
        }

        // UE routes by the material's BLEND MODE: a Translucent template's objects are drawn by the translucency
        // pass and skipped by every opaque one. A property of the template the material draws with, read off its
        // program — no parameter value of any material decides a pass.
        bool IsTranslucent( const DataDrivenMaterial* material )
        {
            // Additive is a translucency-pass mode too (UE BLEND_Additive): never into depth or the G-buffer.
            return Core::Formats::IsTranslucentBlend( material->GetSchema().Blend );
        }

        // Appends one row to a buffer of rows laid end to end and returns its index there. Every lit pass
        // declares one layout, so every row in a buffer has the same length.
        uint32_t AppendRow( std::vector<glm::vec4>& rows, const Core::Formats::MaterialParamRow& row )
        {
            const auto index = row.empty() ? 0u : static_cast<uint32_t>( rows.size() / row.size() );
            rows.insert( rows.end(), row.begin(), row.end() );
            return index;
        }

        SurfaceSlot SectionSurface( const MaterialSlotBinding* slots, uint32_t sectionSlot )
        {
            if ( slots == nullptr || sectionSlot >= slots->Slots.size() || slots->Slots[sectionSlot] == nullptr )
                return {};
            MaterialInstance* inst    = slots->Slots[sectionSlot];
            auto*             surface = dynamic_cast<DataDrivenMaterial*>( inst->GetParentMaterial() );
            if ( surface != nullptr && MeshCellPath( surface->GetShaderName() ) == MeshVertexPath::Static )
                return { inst, surface };
            return {};
        }

        // THE RENDERER'S OWN DRAWS take the (path x pass) cell of the DEFAULT SURFACE template, found by that
        // role (MaterialService::DefaultSurfaceShader) and never by a name written here. Refused with the pair and
        // the registry's reason, because "shader is missing" is unactionable.
        std::optional<std::string> DefaultSurfaceShaderName( MeshVertexPath path, MeshPass pass )
        {
            const auto name = Runtime::ResourceRegistry::GetMaterialService()->DefaultSurfaceShader( path, pass );
            if ( !name )
            {
                LOG_ERROR( "[MeshRenderer] no default surface shader for vertex path '{}' in pass '{}': {}",
                           MeshVertexPathName( path ), MeshPassName( pass ), name.GetError() );
                return std::nullopt;
            }
            return name.GetValue();
        }

        std::shared_ptr<Shader> DefaultSurfaceProgram( MeshVertexPath path, MeshPass pass )
        {
            const auto name = DefaultSurfaceShaderName( path, pass );
            return name ? Runtime::ResourceRegistry::GetShaderService()->GetByName( *name ) : nullptr;
        }

        // A renderer-owned material of one (path x pass) cell of the default surface template — the same
        // DataDrivenMaterial every `.demat` builds, with the cell's default row.
        std::shared_ptr<DataDrivenMaterial> CreateCellMaterial( MeshVertexPath path, MeshPass pass )
        {
            const auto shaderName = DefaultSurfaceShaderName( path, pass );
            return shaderName ? std::make_shared<DataDrivenMaterial>( *shaderName ) : nullptr;
        }

        // The storage buffer the path adds to a caster cell (MeshPathOwnBinding): instance matrices or bone
        // poses; empty for the static path, which has none.
        std::optional<RDG::StorageBufferParameter> MeshPathOwnBuffer( MeshVertexPath path )
        {
            switch ( path )
            {
                case MeshVertexPath::Instanced:
                case MeshVertexPath::GrassBlade:
                    return RDG::StorageBufferParameter( "InstanceTransforms" );
                case MeshVertexPath::Skinned:
                    return ShaderProtocols::SkinnedUB::Parameter;
                case MeshVertexPath::Static:
                    return std::nullopt;
            }
            return std::nullopt;
        }
    } // namespace MeshRendererDetail

    static_assert( kCountedMeshPasses == GpuDriven::kMeshPassCount,
                   "DrawCounters' per-pass table is indexed by GpuDriven::MeshPass" );

    Common::BoolResultStr MeshRenderer::Initialize()
    {
        // The lists of the binned passes, counted per pass (DesertCtl `state views.meshPasses`): the G-buffer,
        // the deferred path's depth/normal prepass, the forward opaque passes (the forward path's geometry pass
        // and the deferred path's forward-shaded generic pass), the sun's cascades (cached static and movable)
        // and the virtual shadow map's levels.
        m_GBufferDraws.CountAs( GpuDriven::MeshPass::GBuffer );
        m_DepthNormalDraws.CountAs( GpuDriven::MeshPass::DepthPrepass );
        m_ForwardDraws.CountAs( GpuDriven::MeshPass::Forward );
        m_GenericDraws.CountAs( GpuDriven::MeshPass::Forward );
        for ( uint32_t view = 0; view < kCascadeViews; ++view )
        {
            m_CascadeDraws[view].CountAs( GpuDriven::MeshPass::ShadowCascade );
            m_CascadeMovableDraws[view].CountAs( GpuDriven::MeshPass::ShadowCascade );
        }
        for ( auto& level : m_VsmDraws )
            level.CountAs( GpuDriven::MeshPass::VirtualShadow );

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return Common::MakeError( "Target framebuffer is not available" );

        // THE BUDGET, TAKEN AND HELD. Read before the first Setup* because SetupShadowPass allocates from it;
        // from here on only RebudgetShadows may change it (a Shadows quality change), and holding a copy is
        // what makes that true rather than a rule somebody has to keep.
        TakeShadowBudget( m_SceneRenderer != nullptr ? m_SceneRenderer->GetShadowQuality() : ShadowQuality{} );

        if ( !SetupGeometryPass() )
            return Common::MakeError( "Failed to setup static geometry pass" );

        // Deferred G-buffer pipeline (optional; forward path is unaffected if it fails to set up). Creating it
        // here compiles the deferred shader + validates the MRT pipeline at startup.
        if ( !SetupGBufferPass() )
            LOG_WARN( "[MeshRenderer] Deferred G-buffer pipeline not set up (deferred path unavailable)." );

        if ( !SetupSkinnedGeometryPass() )
            return Common::MakeError( "Failed to setup skinned geometry pass" );

        if ( !SetupSilhouettePass() )
            return Common::MakeError( "Failed to setup silhouette pass" );

        if ( !SetupShadowPass() )
            return Common::MakeError( "Failed to setup shadow pass" );

#if DESERT_DEV_INSTRUMENTS
        if ( !SetupDebugLinePass() )
            return Common::MakeError( "Failed to setup debug line pass" );

        // Overdraw is an optional debug view — never fatal if its shaders are missing.
        if ( !SetupOverdrawPass() )
            LOG_WARN( "[MeshRenderer] Overdraw debug view unavailable (shaders missing)." );
#endif // DESERT_DEV_INSTRUMENTS

        // Shared instanced material for auto-batching (only usable if the instanced pipeline/shader exist).
        // One instance is created up front; the per-frame scene data + the packed InstanceTransforms/Materials
        // SSBOs are written into it in DrawStaticMeshes before the instanced draws are recorded.
        if ( m_StaticInstancedPipeline )
        {
            m_StaticInstancedMaterial = CreateCellMaterial( MeshVertexPath::Instanced );
            if ( m_StaticInstancedMaterial )
                m_StaticInstancedInstance = m_StaticInstancedMaterial->CreateInstance( "StaticInstancedBatch" );
        }

        return BOOLSUCCESS;
    }

    void MeshRenderer::ClearQueues()
    {
        m_StaticQueue.clear();
        m_SkinnedQueue.clear();
        m_GenericQueue.clear();
        m_InstancedQueue.clear();
        m_GpuInstancedQueue.clear();
    }

    uint32_t MeshRenderer::ComputeLOD( const glm::mat4& transform, const Mesh* mesh, int forcedLOD,
                                       int lodBias ) const
    {
        if ( forcedLOD >= 0 )
            return static_cast<uint32_t>( forcedLOD );
        if ( !m_LODEnabled || !mesh )
            return 0;
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return 0;

        // The policy itself lives in Geometry::SelectLOD so the editor can report the SAME level it
        // draws with (Details "Mesh" section); this only resolves the renderer's camera + LOD toggle.
        return Geometry::SelectLOD( transform, mesh->GetSubmeshes(),
                                    Geometry::LODView{ camera->GetPosition(), camera->GetProjectionMatrix() },
                                    mesh->GetLODScreenSizes(), forcedLOD, lodBias );
    }

    uint64_t MeshRenderer::GetViewTrianglesDrawn() const
    {
        // The same path test the forward list's build makes: on Deferred the forward list is cleared and the
        // meshes are in the G-buffer list, the forward-shaded generics in the generic one.
        const bool deferred =
             m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred && m_StaticGBufferPipeline;
        if ( !deferred )
            return m_ForwardDraws.Triangles();
        return m_GBufferDraws.Triangles() + m_GenericDraws.Triangles();
    }

    void MeshRenderer::SubmitGenericMesh( const GenericMeshRenderData& data )
    {
        // Two valid shapes: an override draw (ShaderName set) or a per-slot draw (SlotMaterial
        // set — the shader name comes from the material at draw time).
        if ( data.Mesh && ( !data.ShaderName.empty() || data.SlotMaterial ) )
            m_GenericQueue.push_back( data );
    }

    void MeshRenderer::SubmitInstancedMesh( const InstancedMeshRenderData& data )
    {
        if ( data.Mesh && data.Material && data.Transforms && !data.Transforms->empty() )
            m_InstancedQueue.push_back( data );
    }

    void MeshRenderer::SubmitGpuInstanced( const GpuInstancedRenderData& data )
    {
        if ( data.Mesh != nullptr && data.Material && data.Instances && data.Refs != nullptr )
            m_GpuInstancedQueue.push_back( data );
    }

    namespace MeshRendererDetail
    {
        void MeshDrawList::Clear()
        {
            m_Commands.clear();
            m_Triangles = 0;
            m_BlockOf.clear();
            m_Blocks.clear();
            m_Error.reset();
            m_IndirectArguments = {};
            m_FirstRecords.clear();
            m_Layouts.DropExpired();
        }

        void MeshDrawList::SetIndirect( const RDG::BufferRef arguments, std::vector<uint32_t> firstRecords )
        {
            if ( firstRecords.size() != m_Commands.size() )
            {
                Fail( std::format( "indirect mesh draws: {} first records for {} commands", firstRecords.size(),
                                   m_Commands.size() ) );
                return;
            }
            m_IndirectArguments = arguments;
            m_FirstRecords      = std::move( firstRecords );
        }

        void MeshDrawList::Add( MeshDrawCommand command )
        {
            if ( m_Error )
                return;
            if ( command.Pipeline == nullptr || command.Mesh == nullptr || command.Material == nullptr ||
                 !command.Material->GetShader() )
            {
                const char* missing = "material shader";
                if ( command.Pipeline == nullptr )
                    missing = "pipeline";
                else if ( command.Mesh == nullptr )
                    missing = "mesh";
                else if ( command.Material == nullptr )
                    missing = "material";
                Fail( std::format( "mesh draw refused: no {}", missing ) );
                return;
            }
            const Shader* recordedWith = command.Pipeline->GetSpecification().Shader.get();
            if ( recordedWith == nullptr )
            {
                Fail( "mesh draw refused: its pipeline has no shader" );
                return;
            }
            // A block per executor AND recording shader: one executor drawn through pipelines of two shaders
            // (a material shared by two vertex paths) is two blocks, each validated against its own layout.
            const auto known =
                 std::find_if( m_Blocks.begin(), m_Blocks.end(),
                               [&]( const Block& block )
                               {
                                   return block.Material == command.Material &&
                                          block.Pipeline->GetSpecification().Shader.get() == recordedWith;
                               } );
            m_BlockOf.push_back( static_cast<uint32_t>( known - m_Blocks.begin() ) );
            if ( known == m_Blocks.end() )
            {
                m_Blocks.push_back( Block{ command.Material, command.Pipeline } );
            }
            // Per draw: the index count of the LOD level it was built with (clamped to the chain the submesh
            // has, as RenderMesh draws it), for each visible submesh, times its instances; a GPU-generated draw
            // is not counted (the GPU decides its instances).
            if ( !command.IndirectArgs.IsValid() )
            {
                const auto& submeshes = command.Mesh->GetSubmeshes();
                for ( std::size_t si = 0; si < submeshes.size(); ++si )
                {
                    if ( si < 64 && ( command.HiddenSubmeshMask & ( 1ull << si ) ) != 0 )
                        continue;
                    const auto& sm      = submeshes[si];
                    uint32_t    indices = sm.IndexCount;
                    if ( !sm.LODs.empty() )
                        indices = sm.LODs[std::min<std::size_t>( command.LodLevel, sm.LODs.size() - 1 )].IndexCount;
                    m_Triangles += static_cast<uint64_t>( indices / 3 ) * command.InstanceCount;
                }
            }
            m_Commands.push_back( std::move( command ) );
        }

        void MeshDrawList::Fail( std::string error )
        {
            if ( !m_Error )
                m_Error = std::move( error );
        }

        // One block per (executor, recording shader), in block-index order: the layout kept for the shader the
        // block's draws record with, the executor's route fill, and the scene/view inputs where the layout has
        // their slots.
        template <typename Declaration, typename PerBlock>
        void MeshDrawList::DeclareBlocks( Declaration& declaration, const std::optional<SceneViewInputs>& view,
                                          const PerBlock& perBlock ) const
        {
            for ( const Block& declared : m_Blocks )
            {
                const std::shared_ptr<const RDG::ShaderBindingLayout>& layout =
                     m_Layouts.Get( declared.Pipeline->GetSpecification().Shader );
                auto block = declaration.Bindings( layout, declared.Material->GetRouteFill() );
                if ( view && SamplesSceneViewInputs( *layout ) )
                {
                    BindSceneViewInputs( block, *view, *layout );
                }
                perBlock( block, *layout );
            }
            // A GPU-instanced draw's buffers: the graph places the compute-write -> vertex / indirect barrier from
            // these (its material binds the transforms; the record reads the arguments). Each buffer once.
            std::vector<RDG::BufferRef> read;
            for ( const MeshDrawCommand& command : m_Commands )
            {
                if ( !command.IndirectArgs.IsValid() ||
                     std::find( read.begin(), read.end(), command.IndirectArgs ) != read.end() )
                    continue;
                read.push_back( command.IndirectArgs );
                declaration.Read( command.IndirectArgs, RDG::Access::IndirectArgs );
                declaration.Read( command.IndirectInstances, RDG::Access::StorageRead );
            }
        }

        void MeshDrawList::Declare( RDG::PassBuilder& pass, const std::optional<SceneViewInputs>& view ) const
        {
            DeclareBlocks( pass, view, []( auto&, const auto& ) {} );
        }

        void MeshDrawList::Declare( RDG::PassBuilder& pass, const std::optional<SceneViewInputs>& view,
                                    const std::function<void( RDG::BindingBlockBuilder&,
                                                              const RDG::ShaderBindingLayout& )>& perBlock ) const
        {
            DeclareBlocks( pass, view, perBlock );
        }

        void MeshDrawList::Declare( RenderPassDeclaration&                declared,
                                    const std::optional<SceneViewInputs>& view ) const
        {
            DeclareBlocks( declared, view, []( auto&, const auto& ) {} );
        }

        Common::BoolResultStr MeshDrawList::Record( const RDG::PassContext& context ) const
        {
            if ( m_Error )
                return Common::MakeFormattedError( "{}", *m_Error );
            std::optional<MeshPassScope> counted;
            if ( m_CountedPass )
                counted.emplace( static_cast<uint32_t>( *m_CountedPass ) );
            std::vector<std::unique_ptr<RDG::PassBindings>> blocks;
            blocks.reserve( m_Blocks.size() );
            for ( uint32_t index = 0; index < m_Blocks.size(); ++index )
                blocks.push_back(
                     std::make_unique<RDG::PassBindings>( context, context.GetBindingBlock( index ) ) );
            for ( size_t i = 0; i < m_Commands.size(); ++i )
            {
                const MeshDrawCommand& draw = m_Commands[i];
                if ( draw.BindState )
                {
                    draw.BindState();
                }
                // A GPU-instanced draw (GpuInstances.hpp): its own argument records, one per submesh of the mesh
                // after the header, the instance counts its generator wrote.
                if ( draw.IndirectArgs.IsValid() )
                {
                    static_assert( kGpuInstanceArgsRecordBytes == kMeshDrawArgumentsBytes );
                    if ( auto drawn = Renderer::RenderMeshIndirect(
                              *blocks[m_BlockOf[i]], *draw.Pipeline, *draw.Mesh, draw.Transform, *draw.Material,
                              draw.IndirectArgs, kGpuInstanceArgsHeaderBytes, draw.HiddenSubmeshMask,
                              MeshDrawArgumentsOrder::Submesh );
                         !drawn.IsSuccess() )
                        return drawn;
                    continue;
                }
                if ( m_IndirectArguments.IsValid() )
                {
                    if ( auto drawn = Renderer::RenderMeshIndirect(
                              *blocks[m_BlockOf[i]], *draw.Pipeline, *draw.Mesh, draw.Transform, *draw.Material,
                              m_IndirectArguments, uint64_t{ m_FirstRecords[i] } * kMeshDrawArgumentsBytes,
                              draw.HiddenSubmeshMask, MeshDrawArgumentsOrder::Drawn );
                         !drawn.IsSuccess() )
                        return drawn;
                    continue;
                }
                if ( auto drawn = Renderer::RenderMesh(
                          *blocks[m_BlockOf[i]], *draw.Pipeline, *draw.Mesh, draw.Transform, *draw.Material,
                          draw.InstanceCount, draw.FirstInstance, draw.HiddenSubmeshMask, draw.LodLevel,
                          draw.SubmeshFirstRows );
                     !drawn.IsSuccess() )
                    return drawn;
            }
            return BOOLSUCCESS;
        }
    } // namespace MeshRendererDetail

    SystemRasterPass MeshRenderer::GeometryPass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return {};

        // A PASS WITHOUT ITS PIPELINE IS NOT A PASS, and this guard is what makes the refusal above
        // survivable. Before Г22 `m_StaticPipeline` could not be null (Create returned a make_shared),
        // so `m_StaticPipeline->GetSpecification()` two lines down was safe by accident; now that
        // SetupStaticPass can honestly refuse, the same line is a null dereference — the refusal became
        // expressible and its first reader crashed on it. Handing back no pass is the right answer: the
        // graph simply has no geometry pass, and the sky, terrain and post chain still draw.
        if ( !m_StaticPipeline )
        {
            return {};
        }

        SystemRasterPass pass{ .Name        = "MeshGeometryPass",
                               .ExecuteFunc = [this]( RDG::PassContext&     context,
                                                      const FrameGraphRefs& refs ) -> Common::BoolResultStr
                               {
                                   // The draw list this node's Declare built (empty in Deferred, where the meshes
                                   // go to the G-buffer, or without a camera).
                                   (void)refs;
                                   return m_ForwardDraws.Record( context );
                               },
                               .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            // The frame's forward draw list - built HERE, before any command is recorded - and one binding block
            // per material of it, the scene/view inputs bound where its shader has slots for them. Forward path
            // only: in Deferred the meshes are drawn into the G-buffer by "Deferred: GBuffer" instead (this
            // target keeps sky/grid/terrain for compositing). The cloud layer's shadow map is one of the inputs.
            m_ForwardDraws.Clear();
            const bool deferred =
                 m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred && m_StaticGBufferPipeline;
            if ( deferred || m_SceneRenderer->GetMainCamera() == nullptr )
                return;
            BuildStaticDraws( m_ForwardDraws );
            BuildSkinnedDraws( m_ForwardDraws );
            BuildGenericDraws( m_ForwardDraws, GenericSlice::Forward );
            m_ForwardDraws.Declare( declared, SceneViewInputsOf( refs ) );
            // The skin cache's streams the forward skinned cell fetches by address (Vertex_Skinned.glslh).
            for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                declared.Read( streams, RDG::Access::StorageRead );
        };

        // NOTE: the deferred G-buffer geometry is NOT a graph pass — it's rendered MANUALLY via
        // RenderGBufferManual() (called from SceneRenderer when Deferred). A graph pass targeting the G-buffer
        // would sit between the forward-target passes and break the graph's "consecutive same-framebuffer =
        // clear once" grouping, causing a spurious re-clear that wipes the sky/meshes in the scene target.
        return pass;
    }

    SceneFrameBinding MeshRenderer::CaptureFrameState( const ViewFrame* view ) const
    {
        SceneFrameBinding frame;
        frame.View        = view;
        frame.TimeSeconds = m_WorldTimeSeconds;

        frame.PointLights     = &m_SceneRenderer->GetPointLights();
        frame.SpotLights      = &m_SceneRenderer->GetSpotLights();
        frame.DirectionLights = &m_SceneRenderer->GetDirectionLights();

        // THE COUNTS TRAVEL WITH THE MAPS, per shadowed directional light, and they are the counts FITTED THIS
        // FRAME, not the counts allocated (DirectionalShadows): the shader's loop is driven by them, and
        // publishing the allocation sends it through matrices no frame ever wrote.
        frame.DirectionalShadows = DirectionalShadows();
        frame.ShadowDebugMode   = m_ShadowDebugMode;
        frame.ShowNormals       = m_ShowNormals;
        frame.LightingDebug     = m_LightingDebug;

        // The active IBL environment (diffuse irradiance + prefiltered specular) and the split-sum BRDF
        // LUT, resolved once so each lit object samples real ambient/reflections instead of the dummy cube.
        auto* imageService = Runtime::ResourceRegistry::GetImageService();
        if ( const auto& env = m_SceneRenderer->GetEnvironment(); env.has_value() )
        {
            frame.EnvironmentLook = env->Look;
            if ( env->IrradianceMap.IsValid() )
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the handle names this exact
                // type
                frame.IrradianceMap = static_cast<ImageCube*>( imageService->Resolve( env->IrradianceMap ) );
            if ( env->PreFilteredMap.IsValid() )
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the handle names this exact
                // type
                frame.PrefilteredMap = static_cast<ImageCube*>( imageService->Resolve( env->PreFilteredMap ) );
        }
        if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
             brdf && brdf->GetImageHandle().IsValid() )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the handle names this exact type
            frame.BrdfLut = static_cast<Image2D*>( imageService->Resolve( brdf->GetImageHandle() ) );

        // The cloud layer's shadow, from the SAME gather the deferred composite reads
        // (SceneRenderer::GetCloudShadowInput). Filled by ExecuteCloudShadowMap() before the render graph
        // records, so it is final by the time any mesh pass runs — in both render paths.
        frame.CloudShadow = m_SceneRenderer->GetCloudShadowInput();

        // The view's contact shadows, from the SAME settings the deferred composite packs. The forward meshes
        // and the glass march the opaque scene depth (SceneDepthTexture): the deferred path has it (the G-buffer
        // depth, finished before any of them draws); the forward path has no prepass, so it marches nothing.
        if ( view != nullptr )
            frame.ContactShadow = m_SceneRenderer->GetContactShadowSettings(
                 *view, m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred );
        // The sun's PCSS taps: the SAME value the deferred composite and the surface-cache cards pack.
        frame.ShadowFilter = m_SceneRenderer->GetShadowFilterSettings();

        // The scene's GPU-scene primitives and both frames' bone palettes (SyncGpuScene, this frame). Carried
        // by every capture: only the view-pass cells declare them (a light view — shadow depth, RSM — reads
        // none), and ApplyTo binds them by name only into a template that does.
        if ( m_GpuScene != nullptr )
        {
            frame.GpuScenePrimitives = m_GpuScene->GetPrimitivesBuffer();
            frame.ObjectBones        = m_GpuScene->GetBonesBuffer();
        }
        // The world's parameters, the view's one snapshot of this frame (SceneRenderer::BeginScene).
        frame.WorldParameters = m_SceneRenderer->GetWorldParametersBuffer();
        frame.RainOcclusion   = GetRainOcclusionUniforms();

        return frame;
    }

    uint32_t MeshRenderer::RegisterMaterialRow( GpuDriven::GpuSceneRegistry& registry,
                                                const DataDrivenMaterial*    material,
                                                MaterialInstance*            instance,
                                                const bool                   receiveSunShadows )
    {
        const GpuDriven::MaterialRowKey key{ .Material = material,
                                             .Instance = instance,
                                             .Variant = receiveSunShadows ? 0u : GpuDriven::kMaterialRowNoSunShadows };
        if ( const auto found = m_FrameMaterialRows.find( key ); found != m_FrameMaterialRows.end() )
            return found->second;
        Core::Formats::MaterialParamRow row = MeshRendererDetail::EffectiveRow( material, instance );
        if ( !receiveSunShadows )
            Core::Formats::SetMaterialParam( material->GetSchema(), row, "ReceiveSunShadows", glm::vec4( 0.0f ) );
        const uint32_t index = registry.SyncMaterialRow( key, row );
        m_FrameMaterialRows.emplace( key, index );
        if ( index >= m_RowSurfaces.size() )
            m_RowSurfaces.resize( index + 1 );
        m_RowSurfaces[index] = { material, instance };
        return index;
    }

    std::optional<uint32_t> MeshRenderer::FindMaterialRow( const DataDrivenMaterial* material,
                                                           const MaterialInstance*   instance,
                                                           const bool                receiveSunShadows )
    {
        const GpuDriven::MaterialRowKey key{ .Material = material,
                                             .Instance = instance,
                                             .Variant = receiveSunShadows ? 0u : GpuDriven::kMaterialRowNoSunShadows };
        if ( const auto found = m_FrameMaterialRows.find( key ); found != m_FrameMaterialRows.end() )
            return found->second;
        if ( m_MissingMaterialRowReported.insert( material ).second )
            LOG_ERROR( "[MeshRenderer] '{}' has no row in the scene material table this frame (SyncGpuScene registers "
                       "every queued draw's row before the upload); its draws are not recorded.",
                       material != nullptr ? material->GetShaderName() : std::string( "<null>" ) );
        return std::nullopt;
    }

    bool MeshRenderer::LendMaterialTable( DataDrivenMaterial& material ) const
    {
        // Find: a surface template with no parameters declares no Materials row block.
        auto* const block = material.Find<StorageBufferProperty>( kMaterialRowBlock );
        if ( block == nullptr )
            return true;
        if ( m_GpuScene == nullptr || m_GpuScene->GetMaterialsBuffer() == nullptr )
        {
            LOG_ERROR( "[MeshRenderer] the scene material table is not on the device (SyncGpuScene did not upload "
                       "it); '{}' is not drawn.",
                       material.GetShaderName() );
            return false;
        }
        block->LendViewBuffer( m_GpuScene->GetMaterialsBuffer() );
        return true;
    }

    void MeshRenderer::FeedDrawCommandCache( const GpuDriven::GpuScene&            scene,
                                             const GpuDriven::GpuSceneRecordRows& rows )
    {
        if ( !m_DrawCommandCache )
            m_DrawCommandCache = std::make_unique<GpuDriven::DrawCommandCache>(
                 [this]( const GpuDriven::MeshPass pass, const GpuDriven::PrimitiveIndex primitive, const Desert::Mesh&,
                         uint32_t, const uint32_t materialRow ) -> const GraphicsPipeline*
                 { return ResolveBinnedPipeline( pass, primitive, materialRow ); } );
        // Answers resolved before a recompile or a wireframe toggle are stale: a reload can add or drop a cell
        // (PipelineCache's InvalidateByShader point, Graphic::PipelineRebuildEpoch), and the Forward pass state is
        // the line twin while the Wireframe view is on.
        if ( const uint64_t epoch = Graphic::PipelineRebuildEpoch();
             epoch != m_BinnedPipelineEpoch || WireframeView() != m_BinnedWireframe )
        {
            m_BinnedPipelineEpoch = epoch;
            m_BinnedWireframe     = WireframeView();
            m_BinnedRefused.clear(); // a reload may have fixed what was refused; say a remaining refusal again
            m_DrawCommandCache->InvalidatePipelines();
        }

        // One entry per primitive, gathered across its records: a record draws the submeshes it does not hide with
        // its row; the primitive hides what every record hides (the other records' sections, the component's mask).
        auto& feed = m_ScratchBinnedFeed;
        feed.clear();
        const auto claim = [&feed, &scene]( const GpuDriven::PrimitiveIndex primitive, const Desert::Mesh* mesh,
                                            const MeshVertexPath path, const uint32_t firstInstance,
                                            const uint32_t instanceCount, const uint64_t hidden, const uint32_t row,
                                            const bool                   generic  = false,
                                            const GpuDriven::BinFallback fallback = GpuDriven::BinFallback::None )
        {
            if ( mesh == nullptr || !scene.IsValid( primitive ) )
                return;
            const size_t submeshes = mesh->GetSubmeshes().size();
            auto [it, inserted]    = feed.try_emplace( primitive );
            BinnedPrimitiveFeed& f = it->second;
            if ( inserted )
            {
                f.Mesh              = mesh;
                f.Geometry          = mesh->GetGeometry().MeshBinding();
                f.Path              = path;
                f.FirstInstance     = firstInstance;
                f.InstanceCount     = instanceCount;
                f.Flags             = scene.Primitive( primitive ).Flags;
                f.HiddenSubmeshMask = ~0ull;
                f.MaterialRows.assign( submeshes, row );
            }
            // A primitive shared by a generic record and a batched one (one per entity slot) takes the generic's
            // rules: it cannot be half binned through two vertex contracts.
            f.Generic = f.Generic || generic;
            if ( fallback != GpuDriven::BinFallback::None )
                f.Fallback = fallback;
            f.HiddenSubmeshMask &= hidden;
            for ( size_t s = 0; s < submeshes && s < 64; ++s )
                if ( ( hidden & ( 1ull << s ) ) == 0 )
                    f.MaterialRows[s] = row;
        };
        for ( const auto& data : m_StaticQueue )
            if ( data.MaterialSlots &&
                 MeshRendererDetail::SectionSurface( data.MaterialSlots.get(), data.SectionSlot ).Surface != nullptr )
                claim( data.GpuPrimitive, data.Mesh, MeshVertexPath::Static, data.GpuInstance, 1, data.HiddenSubmeshes,
                       data.MaterialRow );
        for ( size_t f = 0; f < m_InstancedQueue.size() && f < rows.FieldPrimitives.size(); ++f )
        {
            const auto& data = m_InstancedQueue[f];
            const auto* mat =
                 data.Material ? dynamic_cast<const DataDrivenMaterial*>( data.Material->GetParentMaterial() ) : nullptr;
            if ( mat == nullptr || !data.Transforms )
                continue;
            if ( const auto row = FindMaterialRow( mat, data.Material.get(), true ) )
                claim( rows.FieldPrimitives[f], data.Mesh, MeshVertexPath::Instanced, rows.FieldInstances[f],
                       static_cast<uint32_t>( data.Transforms->size() ), data.HiddenSubmeshes, *row );
        }
        for ( const auto& data : m_SkinnedQueue )
            for ( const auto& section : data.Sections )
                if ( section.Material != nullptr && section.Instance != nullptr )
                    if ( const auto row = FindMaterialRow( section.Material, section.Instance, true ) )
                        claim( data.GpuPrimitive, data.Mesh, MeshVertexPath::Skinned, data.GpuInstance, 1,
                               section.HiddenSubmeshes, *row );

        // The generic queue (Shader Override and custom-program slot draws), as UE caches every mesh batch a pass
        // processor accepts: a Shader Override naming a SURFACE template draws in the G-buffer through its
        // (Static x GBuffer) cell (BuildGenericDraws' surfaceOverride), so its `.Binned` twin is its bin; any other
        // program shades forward through its own passes (GenericProgram) and casts through the shared caster.
        {
            auto* const shaders      = Runtime::ResourceRegistry::GetShaderService();
            const auto  isRegistered = [shaders]( std::string_view name )
            { return shaders->GetByName( std::string( name ) ) != nullptr; };
            for ( const auto& g : m_GenericQueue )
            {
                if ( g.Mesh == nullptr || !g.MaterialRow )
                    continue;
                const bool surfaceOverride =
                     g.SlotMaterial == nullptr &&
                     TemplateCellShader( g.ShaderName, MeshVertexPath::Static, MeshPass::GBuffer, isRegistered )
                          .has_value();
                claim( g.GpuPrimitive, g.Mesh, MeshVertexPath::Static, g.GpuInstance, 1, ~g.VisibleSubmeshMask,
                       *g.MaterialRow, true,
                       surfaceOverride ? GpuDriven::BinFallback::None : GpuDriven::BinFallback::GenericProgram );
            }
        }

        // Only the changed primitives reach the cache (UE: a cached command is rebuilt when its primitive is).
        for ( const auto& [primitive, entry] : feed )
        {
            if ( const auto held = m_BinnedFeed.find( primitive ); held != m_BinnedFeed.end() && held->second == entry )
                continue;
            const GpuDriven::PrimitiveDrawDesc desc{ .Mesh              = entry.Mesh,
                                                     .MaterialRows      = entry.MaterialRows,
                                                     .FirstInstance     = entry.FirstInstance,
                                                     .InstanceCount     = entry.InstanceCount,
                                                     .HiddenSubmeshMask = entry.HiddenSubmeshMask,
                                                     .PrimitiveFlags    = entry.Flags };
            if ( const Common::BoolResultStr added = m_DrawCommandCache->AddPrimitive( primitive, desc ); !added )
            {
                m_DrawCommandCache->RemovePrimitive( primitive );
                LOG_ERROR( "[MeshRenderer] primitive {} has no binned draws: {}", primitive, added.GetError() );
            }
        }
        for ( const auto& [primitive, entry] : m_BinnedFeed )
            if ( !feed.contains( primitive ) )
                m_DrawCommandCache->RemovePrimitive( primitive );
        m_BinnedFeed.swap( feed );
        m_DrawCommandCache->Rebuild();
    }

    const GraphicsPipeline* MeshRenderer::BinnedCellPipeline( const std::string&      cellShader,
                                                              const GraphicsPipeline* passState, const bool twoSided )
    {
        if ( passState == nullptr )
            return nullptr;
        auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellShader );
        if ( !shader )
        {
            if ( m_BinnedRefused.insert( cellShader ).second )
                LOG_ERROR( "[MeshRenderer] binned cell '{}' is not registered; its draws are not binned.", cellShader );
            return nullptr;
        }
        GraphicsPipelineSpecification spec = passState->GetSpecification();
        spec.Shader                        = shader;
        spec.DebugName                     = std::format( "{} {}", spec.DebugName, cellShader );
        if ( twoSided )
            spec.CullMode = CullMode::None; // CullPermutation's rule
        const auto built = m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( spec );
        if ( !built )
        {
            if ( m_BinnedRefused.insert( cellShader ).second )
                LOG_ERROR( "[MeshRenderer] binned cell '{}' has no pipeline: {}", cellShader, built.GetError() );
            return nullptr;
        }
        return built.GetValue().get();
    }

    GpuDriven::PassPipelineResolution MeshRenderer::ResolveBinnedPipeline( const GpuDriven::MeshPass       pass,
                                                                           const GpuDriven::PrimitiveIndex primitive,
                                                                           const uint32_t                  materialRow )
    {
        using GpuDriven::BinFallback;
        const auto fallback = []( const BinFallback reason )
        { return GpuDriven::PassPipelineResolution{ nullptr, reason }; };
        const auto binned   = []( const GraphicsPipeline* pipeline )
        {
            return GpuDriven::PassPipelineResolution{ pipeline, pipeline != nullptr ? BinFallback::None
                                                                                    : BinFallback::PipelineUnavailable };
        };
        const BinnedPrimitiveFeed* feed = nullptr;
        if ( const auto it = m_ScratchBinnedFeed.find( primitive ); it != m_ScratchBinnedFeed.end() )
            feed = &it->second;
        else if ( const auto held = m_BinnedFeed.find( primitive ); held != m_BinnedFeed.end() )
            feed = &held->second;
        if ( feed == nullptr || materialRow >= m_RowSurfaces.size() || m_RowSurfaces[materialRow].Surface == nullptr )
            return fallback( BinFallback::NoMaterialRow );
        const RowSurface& row = m_RowSurfaces[materialRow];
        const std::array<const GraphicsPipeline*, 3> casterStates{ m_ShadowPipeline.get(),
                                                                   m_ShadowInstancedPipeline.get(),
                                                                   m_ShadowSkinnedPipeline.get() };
        if ( feed->Generic && pass == GpuDriven::MeshPass::ShadowCascade )
        {
            // The generic casters' rule: the default surface's opaque Static caster, never the program's own.
            const auto defaultCell =
                 MeshRendererDetail::DefaultSurfaceShaderName( MeshVertexPath::Static, MeshPass::ShadowDepth );
            if ( !defaultCell )
                return fallback( BinFallback::NoBinnedCell );
            return binned( BinnedCellPipeline( BinnedCellOf( *defaultCell ), casterStates[0], false ) );
        }
        if ( IsTranslucentSurface( row.Surface ) )
            return {}; // translucency is sorted per view, never binned
        if ( feed->Fallback != BinFallback::None )
            return fallback( feed->Fallback );
        // A bin's draws share one PSO and one descriptor set: a texture bound per material (a TextureBinding) would
        // be the first draw's for all of them. Row textures (BindlessTexture slots) travel with the material row.
        if ( !row.Surface->GetMaterialLayout().Textures.empty() )
        {
            if ( m_BinnedRefused.insert( row.Surface->GetShaderName() + "|bound" ).second )
                LOG_ERROR( "[MeshRenderer] '{}' binds {} texture(s) per material (TextureBinding); its draws stay on "
                           "the CPU path. Its textures belong in the material row (BindlessTexture).",
                           row.Surface->GetShaderName(), row.Surface->GetMaterialLayout().Textures.size() );
            return fallback( BinFallback::BoundTextures );
        }
        const MeshVertexPath path = feed->Path;
        const size_t pathIndex = path == MeshVertexPath::Static ? 0 : path == MeshVertexPath::Instanced ? 1 : 2;
        const std::string&     surface  = row.Surface->GetShaderName();
        const std::string_view templ    = std::string_view( surface ).substr( 0, surface.find( '/' ) );
        const bool             parallax = IsParallaxCell( surface );
        const bool twoSided = row.Instance != nullptr ? row.Instance->IsTwoSided() : row.Surface->IsTwoSided();
        const auto viewCell = [&]( const MeshPass meshPass ) -> std::optional<std::string>
        {
            auto cell = MeshShaderFor( templ, path, meshPass );
            if ( cell && parallax )
                return ParallaxCellOf( *cell );
            return cell;
        };
        switch ( pass )
        {
            case GpuDriven::MeshPass::GBuffer:
            {
                const auto cell = viewCell( MeshPass::GBuffer );
                if ( !cell )
                    return fallback( BinFallback::NoBinnedCell );
                const std::array<const GraphicsPipeline*, 3> states{ m_StaticGBufferPipeline.get(),
                                                                     m_InstancedGBufferPipeline.get(),
                                                                     m_SkinnedGBufferPipeline.get() };
                return binned( BinnedCellPipeline( BinnedCellOf( *cell ), states[pathIndex], twoSided ) );
            }
            case GpuDriven::MeshPass::Forward:
            {
                const auto cell = viewCell( MeshPass::Forward );
                if ( !cell )
                    return fallback( BinFallback::NoBinnedCell );
                // The Wireframe view's line twin, as the CPU recording takes it (WireframePipelineOr): Static and
                // Instanced have one, Skinned answers its own state.
                const std::array<const GraphicsPipeline*, 3> states{
                     WireframePipelineOr( m_StaticPipeline.get() ),
                     WireframePipelineOr( m_StaticInstancedPipeline.get() ), m_SkinnedPipeline.get() };
                return binned( BinnedCellPipeline( BinnedCellOf( *cell ), states[pathIndex], twoSided ) );
            }
            case GpuDriven::MeshPass::ShadowCascade:
            {
                const auto defaultCell = MeshRendererDetail::DefaultSurfaceShaderName( path, MeshPass::ShadowDepth );
                if ( !defaultCell )
                    return fallback( BinFallback::NoBinnedCell );
                const std::string_view defaultTemplate =
                     std::string_view( *defaultCell ).substr( 0, defaultCell->find( '/' ) );
                const auto cell =
                     ShadowCasterShaderFor( templ, defaultTemplate, row.Surface->GetSchema().Blend, parallax, path );
                if ( !cell )
                    return fallback( BinFallback::NoBinnedCell );
                // The casters' state culls nothing already: a two-sided material changes no state there.
                return binned( BinnedCellPipeline( BinnedCellOf( *cell ), casterStates[pathIndex], false ) );
            }
            case GpuDriven::MeshPass::DepthPrepass:
                // Draws only generic programs' DepthNormal today (BuildGenericDraws), which answered GenericProgram
                // above: a surface is not in this pass.
                return {};
            case GpuDriven::MeshPass::VirtualShadow: // its own per-page remap until the cull feeds it (REMAINDER C)
                return fallback( BinFallback::PassNotBinned );
            case GpuDriven::MeshPass::Count:
                break;
        }
        return {};
    }

    Common::BoolResultStr MeshRenderer::SyncGpuScene( GpuDriven::GpuSceneRegistry&           registry,
                                                      std::span<const MotionRecord>          otherRigid,
                                                      RayTracing::RayTracingSkinCache* const skinCache,
                                                      const uint64_t                         frame )
    {
        namespace Flags       = GpuDriven::PrimitiveFlags;
        const auto rigidFlags = []( const bool castShadows, const bool castContactShadow ) -> uint32_t
        {
            return ( castShadows ? Flags::kCastsShadow : 0u ) | ( castContactShadow ? 0u : Flags::kNoContactShadow );
        };

        registry.BeginFrame( frame );
        GpuDriven::GpuScene& scene = registry.Scene();

        // The queued records as the registry keys them (GpuSceneRegistry::Sync: one primitive per entity slot,
        // submesh records of one object share it, slots stable per submission); this function only gathers them,
        // writes each one's rows back and uploads the scene's deltas.
        auto& rigid = m_ScratchRigidRecords;
        rigid.clear();
        rigid.reserve( m_StaticQueue.size() + m_GenericQueue.size() + otherRigid.size() );
        for ( const auto& data : m_StaticQueue )
            rigid.push_back( { .Entity = data.Entity,
                               .Part   = data.MotionPart,
                               .World  = data.Transform,
                               .Mesh      = data.Mesh,
                               .Flags     = rigidFlags( data.CastShadows, data.CastContactShadow ),
                               .ForcedLod = data.ForcedLOD,
                               .LodBias   = data.LODBias } );
        for ( const auto& data : m_GenericQueue )
            rigid.push_back( { .Entity = data.Entity,
                               .Part   = data.MotionPart,
                               .World  = data.Transform,
                               .Mesh   = data.Mesh,
                               .Flags  = rigidFlags( data.CastShadows, true ) } );
        rigid.insert( rigid.end(), otherRigid.begin(), otherRigid.end() );

        auto& skinned = m_ScratchSkinnedRecords;
        skinned.clear();
        skinned.reserve( m_SkinnedQueue.size() );
        for ( const auto& data : m_SkinnedQueue )
            skinned.push_back( { .Entity = data.Entity,
                                 .World  = data.Transform,
                                 .Bones  = data.BoneMatrices,
                                 .Mesh   = data.Mesh,
                                 .Flags  = Flags::kSkinned | rigidFlags( data.CastShadows, true ) } );

        // A field's wind as its row carries it: PackViewInstanceWind's values (zeros for one that does not sway), which
        // the binned instanced vertex stage reads from the row instead of the push block.
        const auto primitiveWind = []( const InstanceWind& wind ) -> GpuDriven::PrimitiveWind
        {
            if ( !wind.Sways() )
                return {};
            return { .Wind        = glm::vec4( wind.Direction.x, wind.Direction.y, wind.Strength, wind.Height ),
                     .Speed       = wind.Speed,
                     .Seconds     = wind.Seconds,
                     .PrevSeconds = wind.PrevSeconds };
        };
        // Every ISM / foliage field is ONE primitive whose GpuScene instances are its instances (UE instance
        // scene data; the GI card table and the TLAS custom index are keyed per instance).
        auto& fields = m_ScratchFieldRecords;
        fields.clear();
        fields.reserve( m_InstancedQueue.size() );
        for ( const auto& data : m_InstancedQueue )
            fields.push_back( { .Entity            = data.Entity,
                                .Mesh              = data.Mesh,
                                .Transforms        = *data.Transforms,
                                .CullDistanceStart = data.CullDistance.Min,
                                .CullDistanceEnd   = data.CullDistance.Max,
                                .Flags             = rigidFlags( data.CastShadows, data.CastContactShadow ),
                                .BoundsPad         = WindBoundsPad( data.Wind ),
                                .Wind              = primitiveWind( data.Wind ) } );

        auto& rows = m_ScratchGpuSceneRows;
        registry.Sync( rigid, skinned, fields, rows );

        size_t record = 0;
        for ( auto& data : m_StaticQueue )
        {
            data.GpuPrimitive = rows.RigidPrimitives[record];
            data.GpuInstance  = rows.RigidInstances[record++];
        }
        for ( auto& data : m_GenericQueue )
        {
            data.GpuPrimitive = rows.RigidPrimitives[record];
            data.GpuInstance  = rows.RigidInstances[record++];
        }
        m_OtherGpuInstances.clear();
        m_OtherGpuInstances.reserve( otherRigid.size() );
        for ( size_t other = 0; other < otherRigid.size(); ++other )
            m_OtherGpuInstances.push_back( rows.RigidInstances[record++] );
        for ( size_t i = 0; i < m_SkinnedQueue.size(); ++i )
        {
            m_SkinnedQueue[i].GpuPrimitive = rows.SkinnedPrimitives[i];
            m_SkinnedQueue[i].GpuInstance  = rows.SkinnedInstances[i];
        }
        m_InstancedGpuInstances.clear();
        for ( size_t f = 0; f < m_InstancedQueue.size(); ++f )
        {
            auto& data            = m_InstancedQueue[f];
            data.FirstGpuInstance = static_cast<uint32_t>( m_InstancedGpuInstances.size() );
            for ( size_t instance = 0; instance < data.Transforms->size(); ++instance )
                m_InstancedGpuInstances.push_back( rows.FieldInstances[f] + static_cast<uint32_t>( instance ) );
        }
        // The scene material table: the row of every (template, instance) a queued draw shades with, registered
        // here so the upload below puts it on the device before any pass names it (the draw paths only look rows
        // up). Rows are keyed by the template whose layout they have — the section's surface; the cell a pass
        // records with (G-buffer twin, instanced variant, masked caster) is a cell of the same template and reads
        // the same layout. The component's Receive Shadows off zeroes the row's ReceiveSunShadows (either one opts
        // out), so a static draw that opts out has a second row; its shadow caster and glass draw name the first.
        m_FrameMaterialRows.clear();
        for ( auto& data : m_StaticQueue )
        {
            if ( !data.MaterialSlots )
                continue;
            if ( const auto slot = MeshRendererDetail::SectionSurface( data.MaterialSlots.get(), data.SectionSlot ) )
            {
                const uint32_t receiving = RegisterMaterialRow( registry, slot.Surface, slot.Instance, true );
                data.MaterialRow         = data.ReceiveShadows
                                                ? receiving
                                                : RegisterMaterialRow( registry, slot.Surface, slot.Instance, false );
            }
        }
        for ( const auto& data : m_InstancedQueue )
            if ( data.Material )
                if ( const auto* mat = dynamic_cast<const DataDrivenMaterial*>( data.Material->GetParentMaterial() ) )
                    (void)RegisterMaterialRow( registry, mat, data.Material.get(), true );
        for ( const auto& data : m_SkinnedQueue )
            for ( const auto& section : data.Sections )
                if ( section.Material != nullptr && section.Instance != nullptr )
                    (void)RegisterMaterialRow( registry, section.Material, section.Instance, true );
        for ( const auto& gpu : m_GpuInstancedQueue )
            if ( gpu.Material )
                if ( const auto* mat = dynamic_cast<const DataDrivenMaterial*>( gpu.Material->GetParentMaterial() ) )
                    (void)RegisterMaterialRow( registry, mat, gpu.Material.get(), true );
        // The generic queue's rows (per-slot assets, Shader Override records) and its stand-in's.
        RegisterGenericRows( registry );
        // The default surface's rows: what every stand-in draw of this frame names.
        const bool anyMeshDraw = !m_StaticQueue.empty() || !m_InstancedQueue.empty() || !m_SkinnedQueue.empty() ||
                                 !m_GpuInstancedQueue.empty();
        if ( anyMeshDraw )
            if ( StandInCell* const standIn = StandIn( MeshVertexPath::Static, MeshPass::Forward ) )
            {
                m_StandInMaterialRows[0] =
                     RegisterMaterialRow( registry, standIn->Material.get(), standIn->Instance.get(), true );
                m_StandInMaterialRows[1] =
                     RegisterMaterialRow( registry, standIn->Material.get(), standIn->Instance.get(), false );
            }
        m_GpuScene = &scene;
        FeedDrawCommandCache( scene, rows );

        // The scene's deltas reach the device in a graph of their own, submitted ahead of this view's graph: its
        // passes bind the tables by descriptor (CaptureFrameState), so the tables are at their final size and
        // content before any pass is declared.
        const auto upload = [&scene]() -> Common::BoolResultStr
        {
            if ( !scene.HasPendingUpload() )
                return BOOLSUCCESS;
            RDG::Builder graph( "GPUScene" );
            if ( const auto added = scene.AddUploadPass( graph ); !added )
                return Common::MakeFormattedError( "the GPU scene's upload could not be declared: {}",
                                                   added.GetError() );
            if ( const Common::BoolResultStr executed = Renderer::ExecuteGraph( graph ); !executed )
                return Common::MakeFormattedError( "the GPU scene's upload did not execute: {}",
                                                   executed.GetError() );
            return BOOLSUCCESS;
        };

        // The GPU skin cache: each skinned primitive learns where its posed vertices are, before the upload.
        if ( skinCache != nullptr )
            skinCache->PrepareRows( *this, scene, frame );
        if ( const Common::BoolResultStr uploaded = upload(); !uploaded )
            return uploaded;
        // The scene's skin step, after the palettes it skins from are on the device: its own graph is submitted
        // here, ahead of every view graph (each only reads the streams); a failure strips the address from the
        // primitives it was to fill, which then go up again before any view pass reads them.
        if ( skinCache != nullptr )
        {
            if ( const Common::BoolResultStr skinnedFrame =
                      skinCache->SkinFrame( *this, scene, m_SkinCacheBonesImport, frame );
                 !skinnedFrame )
                LOG_ERROR( "[MeshRenderer] the skin cache's step failed: {}", skinnedFrame.GetError() );
            if ( const Common::BoolResultStr uploaded = upload(); !uploaded )
                return uploaded;
        }
        return BOOLSUCCESS;
    }

    void MeshRenderer::SubmitMesh( const MeshRenderData& data )
    {
        if ( !data.Mesh )
        {
            return;
        }

        switch ( data.Mesh->GetType() )
        {
            case MeshType::Static:
            {
                StaticMeshRenderData staticData;
                staticData.Entity                   = data.Entity;
                staticData.MotionPart               = data.MotionPart;
                staticData.Mesh            = static_cast<StaticMesh*>( data.Mesh );
                staticData.Transform       = data.Transform;
                staticData.MaterialSlots   = data.MaterialSlots;
                staticData.SectionSlot              = data.SectionSlot;
                staticData.Outlined        = data.Outlined;
                staticData.HiddenSubmeshes = data.HiddenSubmeshes;
                staticData.ForcedLOD       = data.ForcedLOD;
                staticData.LODBias         = data.LODBias;
                staticData.CastShadows     = data.CastShadows;
                staticData.ReceiveShadows  = data.ReceiveShadows;
                staticData.CastContactShadow        = data.CastContactShadow;
                staticData.TranslucencySortPriority = data.TranslucencySortPriority;

                m_StaticQueue.push_back( staticData );
                break;
            }

            case MeshType::Skinned:
            {
                SkinnedMeshRenderData skinnedData;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): MeshType::Skinned is set only
                // for a SkinnedMesh
                skinnedData.Mesh          = MorphedMeshFor( data, static_cast<SkinnedMesh*>( data.Mesh ) );
                skinnedData.Entity        = data.Entity;
                skinnedData.Transform     = data.Transform;
                skinnedData.BoneMatrices  = data.BoneMatrices;
                skinnedData.Outlined      = data.Outlined;
                skinnedData.CastShadows   = data.CastShadows;
                skinnedData.MaterialSlots = data.MaterialSlots; // keeps the sections' instances alive (A8-3)
                if ( data.MaterialSlots && !data.MaterialSlots->Slots.empty() )
                {
                    // Section si shades with slot SectionMaterialSlot( si, slots ), the static rule; the sections
                    // of one slot are one draw, masked to them. Up to 64 submeshes, the masks' width.
                    using SlotSections = std::pair<size_t, uint64_t>; // slot, the submeshes it shades
                    const auto&  slots = data.MaterialSlots->Slots;
                    const size_t count = std::min<size_t>( skinnedData.Mesh->GetSubmeshes().size(), 64 );
                    std::vector<SlotSections> visibleBySlot;
                    for ( size_t si = 0; si < count; ++si )
                    {
                        const size_t slot = Desert::SectionMaterialSlot( si, slots.size() );
                        const auto   it   = std::ranges::find( visibleBySlot, slot, &SlotSections::first );
                        if ( it != visibleBySlot.end() )
                            it->second |= 1ull << si;
                        else
                            visibleBySlot.emplace_back( slot, 1ull << si );
                    }
                    for ( const SlotSections& group : visibleBySlot )
                    {
                        const size_t        slot    = group.first;
                        const uint64_t      visible = group.second;
                        MaterialInstance*   inst    = slots[slot];
                        DataDrivenMaterial* surface = nullptr;
                        if ( inst != nullptr )
                            surface = dynamic_cast<DataDrivenMaterial*>( inst->GetParentMaterial() );
                        if ( surface != nullptr &&
                             MeshCellPath( surface->GetShaderName() ) == MeshVertexPath::Skinned )
                        {
                            skinnedData.Sections.push_back(
                                 { .Material = surface, .Instance = inst, .HiddenSubmeshes = ~visible } );
                            continue;
                        }
                        // A consistency guard, not the custom-shader case: MeshECSSystem substitutes its default
                        // skinned lit material for any slot that fails to resolve, so every slot reaching here
                        // should already carry a skinned-path parent. If one does not, the producer and this
                        // queue disagree about what a skinned slot IS, and drawing it through the skinned
                        // pipeline with a static material's descriptor sets is a layout mismatch - so that
                        // section is dropped and the disagreement is named.
                        static bool s_WarnedNoSkinnedSlot = false;
                        if ( !s_WarnedNoSkinnedSlot )
                        {
                            LOG_WARN(
                                 "[MeshRenderer] A skinned mesh's slot {} is not a lit material on the SKINNED "
                                 "vertex path; its sections are dropped. MeshECSSystem is expected to have "
                                 "substituted its default skinned material, so this means the two disagree.",
                                 slot );
                            s_WarnedNoSkinnedSlot = true;
                        }
                    }
                }
                if ( !skinnedData.Sections.empty() )
                    m_SkinnedQueue.push_back( std::move( skinnedData ) );
                break;
            }
        }
    }

} // namespace Desert::Graphic::System
