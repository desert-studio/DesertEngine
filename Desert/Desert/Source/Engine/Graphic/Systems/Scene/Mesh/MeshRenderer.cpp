#include "MeshRenderer.hpp"
#include "MeshRendererInternal.hpp"

namespace Desert::Graphic::System
{
    namespace MeshRendererDetail
    {
        // Per-instance material override (MaterialPropertyBlock-style): start from the material's
        // reflected data and apply any overridden instance properties on top — generically, by name,
        // through reflection. Each drawn object thus gets its own effective material in the SSBO.
        PBRGpuMaterial BuildEffectiveMaterial( MaterialPBR* material, MaterialInstance* instance )
        {
            Assets::PBRSurfaceParams data = material->Data();

            if ( instance )
            {
                // Apply instance overrides by schema name onto the typed hot-path view. The names
                // are the StaticMeshPBR schema (single material protocol) — same ones the tint
                // path (MeshECSSystem) and the material canon use.
                const auto vec4Of = []( const auto& v, const glm::vec4& current ) -> glm::vec4
                {
                    if ( auto* v4 = std::get_if<glm::vec4>( &v ) )
                        return *v4;
                    if ( auto* v3 = std::get_if<glm::vec3>( &v ) )
                        return glm::vec4( *v3, current.w );
                    return current;
                };
                const auto floatOf = []( const auto& v, float current ) -> float
                {
                    if ( auto* f = std::get_if<float>( &v ) )
                        return *f;
                    // A bare-instance override (no pre-existing typed property) is stored as a vec4; a scalar
                    // param authored that way (e.g. RoughnessFactor from MaterialComponent) rides in .x.
                    if ( auto* v4 = std::get_if<glm::vec4>( &v ) )
                        return v4->x;
                    return current;
                };

                for ( const auto& [name, prop] : instance->GetPropertySet().GetProperties() )
                {
                    if ( !prop.bIsOverridden )
                        continue;
                    const auto& v = prop.Value;

                    if ( name == "AlbedoColor" )
                        data.AlbedoColor = vec4Of( v, data.AlbedoColor );
                    else if ( name == "MetallicFactor" )
                        data.MetallicFactor = floatOf( v, data.MetallicFactor );
                    else if ( name == "RoughnessFactor" )
                        data.RoughnessFactor = floatOf( v, data.RoughnessFactor );
                    else if ( name == "AOStrength" )
                        data.AOStrength = floatOf( v, data.AOStrength );
                    else if ( name == "EmissiveColor" )
                        data.EmissiveColor = vec4Of( v, data.EmissiveColor );
                    else if ( name == "EmissiveIntensity" )
                        data.EmissiveIntensity = floatOf( v, data.EmissiveIntensity );
                    else if ( name == "AlphaCutoff" )
                        data.AlphaCutoff = floatOf( v, data.AlphaCutoff );
                    else if ( name == "Transmission" )
                        data.Transmission = floatOf( v, data.Transmission );
                    else if ( name == "IOR" )
                        data.IOR = floatOf( v, data.IOR );
                    else if ( name == "GlassTint" )
                        data.GlassTint = vec4Of( v, data.GlassTint );
                    else if ( name == "UVTiling" )
                    {
                        const glm::vec4 t =
                             vec4Of( v, glm::vec4( data.UVTiling.value_or( glm::vec2( 1.0f ) ), 0, 0 ) );
                        data.UVTiling = glm::vec2( t );
                    }
                    // Textures are per-material descriptors, not SSBO data — not overridable here.
                }
            }

            return BuildPBRGpuMaterial( data );
        }

        // First slot instance whose parent is a PBR material ON THE GIVEN VERTEX PATH. Slots holding a
        // custom-shader material (DataDrivenMaterial, v3 per-slot shaders) belong to the generic path —
        // they must never be fed into the PBR SSBO machinery. nullptr when the object has no such slot.
        //
        // The PATH argument is what makes one function serve both queues. Its skinned half used to be a
        // second, hand-written loop inside SubmitMesh looking for a different CLASS, and the two answered
        // differently about the same `.demat`: the static loop found a material, the skinned one found
        // nothing, and a character with authored materials was silently dropped.
        MaterialInstance* FirstPBRSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path )
        {
            for ( auto* inst : slots )
            {
                if ( !inst )
                    continue;
                auto* pbr = dynamic_cast<MaterialPBR*>( inst->GetParentMaterial() );
                if ( pbr && pbr->VertexPath() == path )
                    return inst;
            }
            return nullptr;
        }
    } // namespace MeshRendererDetail

    Common::BoolResultStr MeshRenderer::Initialize()
    {
        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return Common::MakeError( "Target framebuffer is not available" );

        // THE BUDGET, TAKEN ONCE AND HELD. Read before the first Setup* because SetupShadowPass allocates
        // from it; from here on nothing may change it, and holding a copy is what makes that true rather
        // than a rule somebody has to keep.
        m_Shadow = m_SceneRenderer ? m_SceneRenderer->GetShadowQuality() : ShadowQuality{};
        // CLAMPED ONCE, HERE. ShadowQuality is a plain aggregate, so `ShadowQuality{ 5, 2048, ... }`
        // compiles; every loop below runs to this count while the arrays it indexes are [kMaxCascades].
        // ComputeShadowCascades clamps its own copy, which made the fitter safe and left the allocation,
        // the material arrays and the map gather writing one past the end.
        if ( m_Shadow.CascadeCount > kMaxCascades )
        {
            LOG_WARN( "[Shadows] a budget of {} cascades was asked for; this renderer can hold {} and will "
                      "use that.",
                      m_Shadow.CascadeCount, kMaxCascades );
            m_Shadow.CascadeCount = kMaxCascades;
        }

        if ( !SetupGeometryPass() )
            return Common::MakeError( "Failed to setup static geometry pass" );

        // Deferred G-buffer pipeline (optional; forward path is unaffected if it fails to set up). Creating it
        // here compiles the deferred shader + validates the MRT pipeline at startup.
        if ( !SetupGBufferPass() )
            LOG_WARN( "[MeshRenderer] Deferred G-buffer pipeline not set up (deferred path unavailable)." );
        if ( !SetupGlassPass() )
            LOG_WARN( "[MeshRenderer] Glass pipeline not set up (transparent materials won't draw)." );

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
            m_StaticInstancedMaterial = MaterialPBR::Create( MeshVertexPath::Instanced );
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
                Fail( std::format( "mesh draw refused: no {}", command.Pipeline == nullptr ? "pipeline"
                                                               : command.Mesh == nullptr   ? "mesh"
                                                               : command.Material == nullptr
                                                                    ? "material"
                                                                    : "material shader" ) );
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
        template <typename Declaration>
        void MeshDrawList::DeclareBlocks( Declaration&                          declaration,
                                          const std::optional<SceneViewInputs>& view ) const
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
            }
        }

        void MeshDrawList::Declare( RDG::PassBuilder& pass, const std::optional<SceneViewInputs>& view ) const
        {
            DeclareBlocks( pass, view );
        }

        void MeshDrawList::Declare( RenderPassDeclaration&                declared,
                                    const std::optional<SceneViewInputs>& view ) const
        {
            DeclareBlocks( declared, view );
        }

        Common::BoolResultStr MeshDrawList::Record( const RDG::PassContext& context ) const
        {
            if ( m_Error )
                return Common::MakeFormattedError( "{}", *m_Error );
            std::vector<std::unique_ptr<RDG::PassBindings>> blocks;
            blocks.reserve( m_Blocks.size() );
            for ( uint32_t index = 0; index < m_Blocks.size(); ++index )
                blocks.push_back( std::make_unique<RDG::PassBindings>( context, context.GetBindingBlock( index ) ) );
            for ( size_t i = 0; i < m_Commands.size(); ++i )
            {
                const MeshDrawCommand& draw = m_Commands[i];
                if ( draw.BindState )
                {
                    draw.BindState();
                }
                if ( auto drawn = Renderer::GetInstance().RenderMesh(
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
            BuildSkinnedDraws( /*useLoadPass*/ false, m_ForwardDraws );
            BuildGenericDraws( /*useLoadPass*/ false, m_ForwardDraws );
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

    PBRSceneFrame MeshRenderer::CaptureFrameState( const Core::Camera* camera ) const
    {
        PBRSceneFrame frame;
        frame.Camera      = camera;
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
        // LUT, resolved once so each PBR object samples real ambient/reflections instead of the dummy cube.
        auto* imageService = Runtime::ResourceRegistry::GetImageService();
        if ( const auto& env = m_SceneRenderer->GetEnvironment(); env.has_value() )
        {
            frame.EnvironmentLook = env->Look;
            if ( env->IrradianceMap.IsValid() )
                frame.IrradianceMap = static_cast<ImageCube*>( imageService->Resolve( env->IrradianceMap ) );
            if ( env->PreFilteredMap.IsValid() )
                frame.PrefilteredMap = static_cast<ImageCube*>( imageService->Resolve( env->PreFilteredMap ) );
        }
        if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
             brdf && brdf->GetImageHandle().IsValid() )
            frame.BrdfLut = static_cast<Image2D*>( imageService->Resolve( brdf->GetImageHandle() ) );

        // The cloud layer's shadow, from the SAME gather the deferred composite reads
        // (SceneRenderer::GetCloudShadowInput). Filled by ExecuteCloudShadowMap() before the render graph
        // records, so it is final by the time any mesh pass runs — in both render paths.
        frame.CloudShadow = m_SceneRenderer->GetCloudShadowInput();

        return frame;
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
                staticData.Mesh            = static_cast<StaticMesh*>( data.Mesh );
                staticData.Transform       = data.Transform;
                staticData.MaterialSlots   = data.MaterialSlots;
                staticData.Outlined        = data.Outlined;
                staticData.HiddenSubmeshes = data.HiddenSubmeshes;
                staticData.ForcedLOD       = data.ForcedLOD;
                staticData.LODBias         = data.LODBias;
                staticData.CastShadows     = data.CastShadows;
                staticData.ReceiveShadows  = data.ReceiveShadows;

                m_StaticQueue.push_back( staticData );
                break;
            }

            case MeshType::Skinned:
            {
                SkinnedMeshRenderData skinnedData;
                skinnedData.Mesh          = static_cast<SkinnedMesh*>( data.Mesh );
                skinnedData.Transform     = data.Transform;
                skinnedData.BoneMatrices  = data.BoneMatrices;
                skinnedData.Outlined      = data.Outlined;
                skinnedData.CastShadows   = data.CastShadows;
                skinnedData.MaterialSlots = data.MaterialSlots; // keeps Instance below alive (A8-3)
                if ( data.MaterialSlots && !data.MaterialSlots->Slots.empty() )
                {
                    // THE SAME selector the static queue uses, asked for the SKINNED path. It used to be
                    // a second loop hunting a different C++ CLASS, and since MaterialFactory could not
                    // produce that class from an asset under any circumstances, an imported character
                    // with its own materials matched nothing and was dropped without drawing.
                    if ( auto* inst = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Skinned ) )
                    {
                        skinnedData.Instance = inst;
                        skinnedData.Material = static_cast<MaterialPBR*>( inst->GetParentMaterial() );
                    }
                    else
                    {
                        // A consistency guard, not the custom-shader case: MeshECSSystem substitutes its
                        // default skinned PBR material for any slot that fails to resolve, so every slot
                        // reaching here should already carry a skinned-path parent. If one does not, the
                        // producer and this queue disagree about what a skinned slot IS, and drawing it
                        // through the skinned pipeline with a static material's descriptor sets is a
                        // layout mismatch — so the mesh is dropped and the disagreement is named.
                        static bool s_WarnedNoSkinnedSlot = false;
                        if ( !s_WarnedNoSkinnedSlot )
                        {
                            LOG_WARN( "[MeshRenderer] A skinned mesh arrived with slots but none whose parent "
                                      "is a PBR material on the SKINNED vertex path; the mesh is dropped. "
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
