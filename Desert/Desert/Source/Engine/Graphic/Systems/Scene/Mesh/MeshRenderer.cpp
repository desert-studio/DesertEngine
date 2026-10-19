#include "MeshRenderer.hpp"
#include "MeshRendererInternal.hpp"

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/View/ObjectMotionRows.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <optional>

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

        // The (path x pass) cell of the default surface compiled under @p variant (a permutation's defines). The
        // caller holds the program (ShaderService::AcquireVariant keeps only a weak reference).
        std::shared_ptr<Shader> DefaultSurfaceProgramVariant( MeshVertexPath path, MeshPass pass,
                                                              const ShaderVariant& variant )
        {
            const auto name = DefaultSurfaceShaderName( path, pass );
            return name ? Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( *name, variant )
                        : nullptr;
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
        std::string MeshPathOwnBufferName( MeshVertexPath path )
        {
            switch ( path )
            {
                case MeshVertexPath::Instanced:
                case MeshVertexPath::GrassBlade:
                    return "InstanceTransforms";
                case MeshVertexPath::Skinned:
                    return ShaderProtocols::SkinnedUB::Name;
                case MeshVertexPath::Static:
                    return {};
            }
            return {};
        }
    } // namespace MeshRendererDetail

    Common::BoolResultStr MeshRenderer::Initialize()
    {
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
                          draw.InstanceCount, draw.FirstInstance, draw.HiddenSubmeshMask, draw.LodLevel );
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

        frame.CascadeViewProj = m_CascadeVP;
        // THE COUNT TRAVELS WITH THE MAPS. Without it the applier bound four maps and told the shader to
        // walk four cascades whatever this renderer had allocated, so a one-cascade renderer would have
        // had three identity matrices tested and three unbound samplers read — the shader's own loop is
        // driven by u_ShadowParams.w and would have found "the fragment is inside cascade 1" everywhere.
        //
        // And it is the count FITTED THIS FRAME, not the count allocated — GetValidCascadeCount, see
        // UpdateCascades. The two differ whenever the fit does not run or degenerates (no sun in the
        // scene, most commonly), and publishing the allocation then sends the shader through matrices no
        // frame ever wrote.
        frame.CascadeCount      = GetValidCascadeCount();
        frame.CascadeTexelWorld = m_CascadeWorldPerTexel;
        frame.CascadeDepthRange = m_CascadeDepthRange;
        frame.SunSourceTanHalf  = GetSunSourceTanHalf();
        frame.ShadowsEnabled    = m_ShadowsEnabled;
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
        // and the glass march the opaque scene depth (u_SceneDepth): the deferred path has it (the G-buffer
        // depth, finished before any of them draws); the forward path has no prepass, so it marches nothing.
        if ( view != nullptr )
            frame.ContactShadow = m_SceneRenderer->GetContactShadowSettings(
                 *view, m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred );
        // The sun's PCSS taps: the SAME value the deferred composite and the surface-cache cards pack.
        frame.ShadowFilter = m_SceneRenderer->GetShadowFilterSettings();

        // The view's motion rows and both frames' bone palettes (BuildObjectMotions, this frame). Carried by
        // every capture: only the view-pass cells declare them (a light view — shadow depth, RSM — reads none),
        // and ApplyTo binds them by name only into a template that does.
        frame.ObjectMotions = m_ObjectMotions;
        frame.ObjectBones   = m_ObjectBones;

        return frame;
    }

    Common::BoolResultStr MeshRenderer::BuildObjectMotions( MotionHistory&                         motion,
                                                            std::span<const MotionRecord>          otherRigid,
                                                            RayTracing::RayTracingSkinCache* const skinCache,
                                                            const uint64_t                         frame )
    {
        // The rows are numbered on the CPU (Graphic/View/ObjectMotionRows.hpp: one row per primitive, submesh
        // records of one object share it, slots stable per submission); this function only gathers the queued
        // records, writes each one's row back and uploads.
        auto& records = m_ScratchMotionRecords;
        records.clear();
        size_t instanceCount = 0;
        for ( const auto& data : m_InstancedQueue )
            instanceCount += data.Transforms->size();
        records.reserve( m_StaticQueue.size() + m_GenericQueue.size() + instanceCount + otherRigid.size() +
                         m_SkinnedQueue.size() );
        for ( const auto& data : m_StaticQueue )
            records.push_back( { .Entity = data.Entity, .Part = data.MotionPart, .World = data.Transform } );
        for ( const auto& data : m_GenericQueue )
            records.push_back( { .Entity = data.Entity, .Part = data.MotionPart, .World = data.Transform } );
        // Every ISM instance is a primitive row of its own (UE instance scene data; the GI card table and the TLAS
        // custom index are keyed per instance). Part = instance + 1 keeps each instance's previous world its own.
        for ( const auto& data : m_InstancedQueue )
        {
            const std::vector<glm::mat4>& transforms = *data.Transforms;
            for ( size_t instance = 0; instance < transforms.size(); ++instance )
                records.push_back( { .Entity = data.Entity,
                                     .Part   = static_cast<uint32_t>( instance + 1 ),
                                     .World  = transforms[instance] } );
        }
        records.insert( records.end(), otherRigid.begin(), otherRigid.end() );
        const size_t rigidCount = records.size();
        for ( const auto& data : m_SkinnedQueue )
            records.push_back( { .Entity = data.Entity, .World = data.Transform, .Bones = data.BoneMatrices } );

        auto& built = m_ScratchMotionRows;
        BuildObjectMotionRows( motion, std::span<const MotionRecord>( records ).first( rigidCount ),
                               std::span<const MotionRecord>( records ).subspan( rigidCount ), built );
        size_t record = 0;
        for ( auto& data : m_StaticQueue )
            data.MotionRow = built.RecordRows[record++];
        for ( auto& data : m_GenericQueue )
            data.MotionRow = built.RecordRows[record++];
        m_InstancedMotionRows.clear();
        m_InstancedMotionRows.reserve( instanceCount );
        for ( auto& data : m_InstancedQueue )
        {
            data.FirstMotionRow = static_cast<uint32_t>( m_InstancedMotionRows.size() );
            for ( size_t instance = 0; instance < data.Transforms->size(); ++instance )
                m_InstancedMotionRows.push_back( built.RecordRows[record++] );
        }
        m_OtherMotionRows.clear();
        m_OtherMotionRows.reserve( otherRigid.size() );
        for ( size_t other = 0; other < otherRigid.size(); ++other )
            m_OtherMotionRows.push_back( built.RecordRows[record++] );
        for ( auto& data : m_SkinnedQueue )
            data.MotionRow = built.RecordRows[record++];
        // The GPU skin cache: each skinned row learns where its posed vertices are, before the rows are uploaded.
        if ( skinCache != nullptr )
            skinCache->PrepareRows( *this, built.Rows, frame );
        const auto& rows     = built.Rows;
        const auto& palettes = built.Palettes;

        // Both buffers at FINAL size before any pass is declared: the descriptor a draw records points at the
        // buffer it reads (a later grow would reallocate it under recorded draws).
        const auto upload = []( std::shared_ptr<ShaderResources::StorageBuffer>& buffer, const char* name,
                                const uint32_t binding, const void* data,
                                const size_t bytes ) -> Common::BoolResultStr
        {
            if ( !buffer )
                buffer = ShaderResources::StorageBuffer::Create(
                     name, static_cast<uint32_t>( std::max<size_t>( bytes, sizeof( glm::mat4 ) ) ), binding );
            if ( !buffer )
                return Common::MakeFormattedError( "the view's {} buffer could not be created", name );
            if ( bytes == 0 )
                return BOOLSUCCESS;
            if ( const auto wrote = buffer->SetData( data, static_cast<uint32_t>( bytes ) ); !wrote )
                return Common::MakeFormattedError( "the view's {} ({} bytes) could not be uploaded: {}", name,
                                                   bytes, wrote.GetError() );
            return BOOLSUCCESS;
        };
        if ( const auto uploaded =
                  upload( m_ObjectBones, SceneResources::kObjectBonesName, Core::kObjectBonesBinding,
                          palettes.data(), palettes.size() * sizeof( glm::mat4 ) );
             !uploaded )
            return uploaded;
        // The scene's skin step, after the palette it skins from is uploaded and before the rows are: its own
        // graph is submitted here, ahead of every view graph (each only reads the streams), and a failure strips
        // the address from the rows it was to fill before they reach the GPU.
        if ( skinCache != nullptr )
            if ( const Common::BoolResultStr skinned =
                      skinCache->SkinFrame( *this, built.Rows, m_SkinCacheBonesImport, frame );
                 !skinned )
                LOG_ERROR( "[MeshRenderer] the skin cache's step failed: {}", skinned.GetError() );
        return upload( m_ObjectMotions, SceneResources::kObjectMotionsName, Core::kObjectMotionsBinding,
                       rows.data(), rows.size() * sizeof( GpuObjectMotion ) );
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
