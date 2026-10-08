#include "MeshRenderer.hpp"
#include "MeshRendererInternal.hpp"

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/View/ObjectMotionRows.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

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
                const auto slot = Core::Formats::MaterialParamSlot( material->GetSchema(), name );
                if ( !slot || *slot >= row.size() )
                    continue;
                glm::vec4& dst = row[*slot];
                std::visit(
                     [&dst]( const auto& v )
                     {
                         using T = std::decay_t<decltype( v )>;
                         if constexpr ( std::is_same_v<T, float> )
                             dst = glm::vec4( v, 0.0f, 0.0f, 0.0f );
                         else if constexpr ( std::is_same_v<T, glm::vec2> )
                             dst = glm::vec4( v, 0.0f, 0.0f );
                         else if constexpr ( std::is_same_v<T, glm::vec3> )
                             dst = glm::vec4( v, dst.w );
                         else if constexpr ( std::is_same_v<T, glm::vec4> )
                             dst = v;
                     },
                     prop.Value );
            }
            return row;
        }

        // UE routes by the material's BLEND MODE: a Translucent template's objects are drawn by the translucency
        // pass and skipped by every opaque one. A property of the template the material draws with, read off its
        // program — no parameter value of any material decides a pass.
        bool IsTranslucent( const DataDrivenMaterial* material )
        {
            return material->GetSchema().Blend == Core::Formats::SurfaceBlendMode::Translucent;
        }

        // Appends one row to a buffer of rows laid end to end and returns its index there. Every lit pass
        // declares one layout, so every row in a buffer has the same length.
        uint32_t AppendRow( std::vector<glm::vec4>& rows, const Core::Formats::MaterialParamRow& row )
        {
            const auto index = row.empty() ? 0u : static_cast<uint32_t>( rows.size() / row.size() );
            rows.insert( rows.end(), row.begin(), row.end() );
            return index;
        }

        SurfaceSlot FirstSurfaceSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path )
        {
            for ( auto* inst : slots )
            {
                if ( inst == nullptr )
                    continue;
                // The batched paths draw a material allocated from a cell of the mesh-shader table, on the
                // path the cell belongs to (MeshCellPath) — the vertex factory's question, asked of the shader.
                auto* surface = dynamic_cast<DataDrivenMaterial*>( inst->GetParentMaterial() );
                if ( surface != nullptr && MeshCellPath( surface->GetShaderName() ) == path )
                    return { inst, surface };
            }
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
        return Geometry::SelectLOD( transform, mesh->GetSubmeshes(), camera->GetPosition(), forcedLOD, lodBias );
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

    namespace MeshRendererDetail
    {
        void MeshDrawList::Clear()
        {
            m_Commands.clear();
            m_BlockOf.clear();
            m_Blocks.clear();
            m_Error.reset();
            m_Layouts.DropExpired();
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
                if ( auto drawn = Renderer::RenderMesh(
                          *blocks[m_BlockOf[i]], *draw.Pipeline, *draw.Mesh, draw.Transform, *draw.Material,
                          draw.InstanceCount, draw.FirstInstance, draw.HiddenSubmeshMask, draw.LodLevel );
                     !drawn.IsSuccess() )
                    return drawn;
            }
            return BOOLSUCCESS;
        }
    } // namespace MeshRendererDetail

    void MeshRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return;

        // A PASS WITHOUT ITS PIPELINE IS NOT A PASS, and this guard is what makes the refusal above
        // survivable. Before Г22 `m_StaticPipeline` could not be null (Create returned a make_shared),
        // so `m_StaticPipeline->GetSpecification()` two lines down was safe by accident; now that
        // SetupStaticPass can honestly refuse, the same line is a null dereference — the refusal became
        // expressible and its first reader crashed on it. Registering nothing is the right answer: the
        // graph simply has no geometry pass, and the sky, terrain and post chain still draw.
        if ( !m_StaticPipeline )
        {
            LOG_ERROR( "[MeshRenderer] no geometry pass this scene: the static-mesh pipeline was never "
                       "built." );
            return;
        }

        builder
             .AddPass( "MeshGeometryPass", RenderPhase::Geometry,
                       [this]( RDG::PassContext& context, const FrameGraphRefs& refs ) -> Common::BoolResultStr
                       {
                           // The draw list this node's Declare built (empty in Deferred, where the meshes go
                           // to the G-buffer, or without a camera).
                           (void)refs;
                           return m_ForwardDraws.Record( context );
                       },
                       m_StaticPipeline->GetSpecification(), targetFb,
                       { RenderPassDependency( RenderPhase::DepthPrePass ) } )
             .Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
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
            BuildGenericDraws( m_ForwardDraws );
            m_ForwardDraws.Declare( declared, SceneViewInputsOf( refs ) );
        };

        // NOTE: the deferred G-buffer geometry is NOT a graph pass — it's rendered MANUALLY via
        // RenderGBufferManual() (called from SceneRenderer when Deferred). A graph pass targeting the G-buffer
        // would sit between the forward-target passes and break the graph's "consecutive same-framebuffer =
        // clear once" grouping, causing a spurious re-clear that wipes the sky/meshes in the scene target.

        // The silhouette mask is always produced (and cleared) so the Jump Flood outline has a
        // fresh input every frame. Outline visibility is controlled by JumpFloodOutlineRenderer.
        RegisterSilhouettePass( builder );
        RegisterShadowPass( builder );
#if DESERT_DEV_INSTRUMENTS
        RegisterDebugPass( builder );
#endif
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
        frame.CascadeCount = GetValidCascadeCount();
        for ( uint32_t c = 0; c < frame.CascadeCount; ++c )
            frame.CascadeMaps[c] = m_CascadeFB[c] ? m_CascadeFB[c]->GetColorAttachmentImage().get() : nullptr;
        frame.CascadeTexelWorld = m_CascadeWorldPerTexel;
        frame.ShadowBias        = m_ShadowBias;
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

        // The view's motion rows and both frames' bone palettes (BuildObjectMotions, this frame). Carried by
        // every capture: only the view-pass cells declare them (a light view — shadow depth, RSM — reads none),
        // and ApplyTo binds them by name only into a template that does.
        frame.ObjectMotions = m_ObjectMotions;
        frame.ObjectBones   = m_ObjectBones;

        return frame;
    }

    Common::BoolResultStr MeshRenderer::BuildObjectMotions( MotionHistory& motion )
    {
        // The rows are numbered on the CPU (Graphic/View/ObjectMotionRows.hpp: one row per primitive, submesh
        // records of one object share it, slots stable per submission); this function only gathers the queued
        // records, writes each one's row back and uploads.
        auto& records = m_ScratchMotionRecords;
        records.clear();
        records.reserve( m_StaticQueue.size() + m_GenericQueue.size() + m_SkinnedQueue.size() );
        for ( const auto& data : m_StaticQueue )
            records.push_back( { .Entity = data.Entity, .World = data.Transform } );
        for ( const auto& data : m_GenericQueue )
            records.push_back( { .Entity = data.Entity, .World = data.Transform } );
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
        for ( auto& data : m_SkinnedQueue )
            data.MotionRow = built.RecordRows[record++];
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
        if ( const auto uploaded = upload( m_ObjectMotions, SceneResources::kObjectMotionsName, Core::kObjectMotionsBinding,
                                           rows.data(), rows.size() * sizeof( GpuObjectMotion ) );
             !uploaded )
            return uploaded;
        return upload( m_ObjectBones, SceneResources::kObjectBonesName, Core::kObjectBonesBinding, palettes.data(),
                       palettes.size() * sizeof( glm::mat4 ) );
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
                staticData.Mesh            = static_cast<StaticMesh*>( data.Mesh );
                staticData.Transform       = data.Transform;
                staticData.MaterialSlots   = data.MaterialSlots;
                staticData.Outlined        = data.Outlined;
                staticData.HiddenSubmeshes = data.HiddenSubmeshes;
                staticData.ForcedLOD       = data.ForcedLOD;
                staticData.LODBias         = data.LODBias;
                staticData.CastShadows     = data.CastShadows;
                staticData.ReceiveShadows  = data.ReceiveShadows;
                staticData.TranslucencySortPriority = data.TranslucencySortPriority;

                m_StaticQueue.push_back( staticData );
                break;
            }

            case MeshType::Skinned:
            {
                SkinnedMeshRenderData skinnedData;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): MeshType::Skinned is set only
                // for a SkinnedMesh
                skinnedData.Mesh          = static_cast<SkinnedMesh*>( data.Mesh );
                skinnedData.Entity        = data.Entity;
                skinnedData.Transform     = data.Transform;
                skinnedData.BoneMatrices  = data.BoneMatrices;
                skinnedData.Outlined      = data.Outlined;
                skinnedData.CastShadows   = data.CastShadows;
                skinnedData.MaterialSlots = data.MaterialSlots; // keeps Instance below alive (A8-3)
                if ( data.MaterialSlots && !data.MaterialSlots->Slots.empty() )
                {
                    // THE SAME selector the static queue uses, asked for the SKINNED path. It used to be
                    // a second loop hunting a different C++ CLASS, and since the material build could not
                    // produce that class from an asset under any circumstances, an imported character
                    // with its own materials matched nothing and was dropped without drawing.
                    if ( const auto slot = FirstSurfaceSlot( data.MaterialSlots->Slots, MeshVertexPath::Skinned ) )
                    {
                        skinnedData.Instance = slot.Instance;
                        skinnedData.Material = slot.Surface;
                    }
                    else
                    {
                        // A consistency guard, not the custom-shader case: MeshECSSystem substitutes its
                        // default skinned lit material for any slot that fails to resolve, so every slot
                        // reaching here should already carry a skinned-path parent. If one does not, the
                        // producer and this queue disagree about what a skinned slot IS, and drawing it
                        // through the skinned pipeline with a static material's descriptor sets is a
                        // layout mismatch — so the mesh is dropped and the disagreement is named.
                        static bool s_WarnedNoSkinnedSlot = false;
                        if ( !s_WarnedNoSkinnedSlot )
                        {
                            LOG_WARN( "[MeshRenderer] A skinned mesh arrived with slots but none whose parent "
                                      "is a lit material on the SKINNED vertex path; the mesh is dropped. "
                                      "MeshECSSystem is expected to have substituted its default skinned "
                                      "material, so this means the two disagree." );
                            s_WarnedNoSkinnedSlot = true;
                        }
                    }
                }
                if ( skinnedData.Material && skinnedData.Instance )
                    m_SkinnedQueue.push_back( std::move( skinnedData ) );
                break;
            }
        }
    }

} // namespace Desert::Graphic::System
