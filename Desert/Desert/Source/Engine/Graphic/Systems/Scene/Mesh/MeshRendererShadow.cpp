// MeshRenderer's light-view passes: the cascaded shadow map (setup, cascade fit, graph pass) and the
// Reflective Shadow Map drawn from the sun.
#include "MeshRendererInternal.hpp"

#include <format>

namespace Desert::Graphic::System
{
    namespace
    {
        // A camera whose matrices are set directly — used to render the Reflective Shadow Map from the
        // sun's point of view. The cascade fitter produces a COMBINED light view-projection, so (exactly
        // like the shadow pass does with SetLightMatrix) it goes in as the projection against an identity
        // view: the vertex shader forms Projection * View * Transform, so the product is unchanged.
        class LightCamera final : public Core::Camera
        {
        public:
            LightCamera( const glm::mat4& viewProj, const glm::vec3& eye )
            {
                m_ViewMatrix       = glm::mat4( 1.0f );
                m_ProjectionMatrix = viewProj;
                m_Position         = eye;
            }
        };
    } // namespace

    void MeshRenderer::DeclareRSMDraws( RDG::PassBuilder& pass )
    {
        // The G-buffer program under DESERT_GBUFFER_RSM (no shading-word output) into the RSM framebuffer, whose
        // colour slots are ViewTargetFormats::kRSMColourSlots (slot 2 unused). The pipeline is its own
        // (standard-Z, see SetupGBufferPass) and so is the camera. Only static meshes are bounce sources: no other
        // renderer (terrain, foliage, skinned) draws into the RSM — census
        // ShaderVariantDefines.OnlyTheMeshRendererDrawsTheRSM.
        m_RSMDraws.Clear();
        if ( !m_RSMPipeline || !m_RSMMaterial || !m_RSMInstance || m_StaticQueue.empty() )
            return;
        const auto& rsm = m_SceneRenderer != nullptr ? m_SceneRenderer->GetRSMBuffer() : nullptr;
        if ( !rsm )
            return;

        // All OPAQUE static objects are bounce sources (glass transmits rather than bouncing diffusely).
        // Their effective materials go into the DEDICATED RSM material's Materials SSBO, so each texel's
        // albedo is the real per-object one — that albedo IS the flux colour, i.e. the colour bleeding.
        std::vector<const StaticMeshRenderData*> objs;
        std::vector<glm::vec4>                   gpuMats;
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
            {
                continue;
            }
            const auto [pbrInst, mat] = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Static );
            if ( pbrInst == nullptr || IsTranslucent( mat ) )
            {
                continue;
            }
            objs.push_back( &data );
            AppendRow( gpuMats, EffectiveRow( mat, pbrInst ) );
        }
        if ( objs.empty() )
            return;

        // Written ONCE, before the block is declared, so its route fill is what the draws will read.
        if ( auto* sb = m_RSMMaterial->Get<StorageBufferProperty>( "Materials" ) )
            sb->SetRawData( gpuMats.data(), static_cast<uint32_t>( gpuMats.size() * sizeof( glm::vec4 ) ) );

        // Render from the SUN. A DEDICATED material+instance (like the glass pass) keeps this camera write
        // off the opaque passes' per-frame UBs — two writes to the same UB in one frame is the hazard that
        // previously hung the GPU.
        LightCamera       lightCam( m_RSMViewProj, m_RSMEye );
        MaterialInstance* ri = m_RSMInstance.get();
        CaptureFrameState( &lightCam ).ApplyTo( ri );

        // The graph opens the render pass: colour clears to 0, depth to 1 (SceneRenderer::AddFrameRSM). The
        // per-object transform, material row and instance bind are each draw's state, written right before it.
        DataDrivenMaterial* const rsmMaterial = m_RSMMaterial.get();
        for ( uint32_t i = 0; i < static_cast<uint32_t>( objs.size() ); ++i )
        {
            const auto* obj = objs[i];
            m_RSMDraws.Add( { .Pipeline          = m_RSMPipeline.get(),
                              .Mesh              = obj->Mesh,
                              .Transform         = obj->Transform,
                              .Material          = rsmMaterial->GetMaterialExecutor(),
                              .HiddenSubmeshMask = obj->HiddenSubmeshes,
                              .BindState         = [rsmMaterial, ri, transform = obj->Transform, i]
                              {
                                  rsmMaterial->SetPushMatrix( transform );
                                  rsmMaterial->SetMaterialIndex( i );
                                  rsmMaterial->Bind( ri );
                              } } );
        }
        // The G-buffer shader's textures are the material's own (Properties): it samples no scene/view input.
        m_RSMDraws.Declare( pass, std::nullopt );
    }

    Common::BoolResultStr MeshRenderer::RenderRSMManual( const RDG::PassContext& context ) const
    {
        return m_RSMDraws.Record( context );
    }

    void MeshRenderer::LogShadowBudget( double allocMs ) const
    {
        // WHAT THIS RENDERER JUST SPENT AND WHAT THE PROCESS NOW HOLDS. The per-renderer figure alone was
        // read wrong: an empty editor prints this line TWICE — SceneRenderer::Init runs a second time when
        // the project's default scene loads — and two "320 MiB" lines were taken to mean 640 MiB held for
        // nothing. It is one renderer, and the first set is released before the second is allocated. The
        // live total is here so the log answers that directly instead of inviting a multiplication.
        //
        // MiB, spelled out. The unit was "MB" while the arithmetic divided by 1024*1024, so the same
        // quantity read as 320 here and 335 in ShadowQuality's own comment.
        LOG_INFO( "[Shadows] {} cascade(s) at {}x{} over {:.0f} m = {:.1f} MiB of attachments for this "
                  "renderer, allocated in {:.1f} ms ({:.1f} MiB live across {} renderer(s) holding "
                  "cascades).",
                  m_Shadow.CascadeCount, m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize,
                  Common::Units::ToMetres( m_Shadow.MaxDistance ),
                  static_cast<double>( ShadowAttachmentBytes( m_Shadow ) ) / ( 1024.0 * 1024.0 ), allocMs,
                  static_cast<double>( ShadowAttachmentLease::LiveBytes() ) / ( 1024.0 * 1024.0 ),
                  ShadowAttachmentLease::LiveHolders() );
    }

    void MeshRenderer::TakeShadowBudget( const ShadowQuality& budget )
    {
        m_Shadow = budget;
        // CLAMPED HERE, ON EVERY BUDGET TAKEN. ShadowQuality is a plain aggregate, so
        // `ShadowQuality{ 5, 2048, ... }` compiles; every loop of the shadow pass runs to this count while the
        // arrays it indexes are [kMaxCascades]. ComputeShadowCascades clamps its own copy, which made the
        // fitter safe and left the allocation, the material arrays and the map gather writing one past the end.
        if ( m_Shadow.CascadeCount > kMaxCascades )
        {
            LOG_WARN( "[Shadows] a budget of {} cascades was asked for; this renderer can hold {} and will "
                      "use that.",
                      m_Shadow.CascadeCount, kMaxCascades );
            m_Shadow.CascadeCount = kMaxCascades;
        }
    }

    bool MeshRenderer::RebudgetShadows( const ShadowQuality& budget )
    {
        // The maps, the caster materials (their light-matrix UBOs) and the pipelines (built against cascade
        // 0's framebuffer) may all be referenced by a frame still in flight; a quality change is rare and
        // user-driven, so the whole device is drained rather than each object's release deferred.
        Renderer::GetInstance().WaitDeviceIdle();
        for ( uint32_t i = 0; i < kMaxCascades; ++i )
        {
            m_CascadeFB[i].reset();
            m_ShadowMaterial[i].reset();
            m_ShadowInstancedMaterial[i].reset();
            m_ShadowSkinnedMaterial[i].reset();
        }
        m_ShadowPipeline.reset();
        m_ShadowInstancedPipeline.reset();
        m_ShadowSkinnedPipeline.reset();
        m_ShadowAttachments = ShadowAttachmentLease{};
        m_FittedCascades    = 0;

        TakeShadowBudget( budget );
        return SetupShadowPass();
    }

    bool MeshRenderer::SetupShadowPass()
    {
        // A ZERO BUDGET IS A LEGAL BUDGET, and the only place that has to know it is this one. A renderer
        // built with Graphic::kNoShadowQuality allocates no map, compiles no caster pipeline and loads no
        // shadow shader; everything downstream is already driven by the count it publishes
        // (RegisterShadowPass registers nothing, CaptureFrameState reports 0, and the shader's cascade
        // loop over u_ShadowParams.w selects none), so there is no second switch to keep in step.
        //
        // Returning `true` matters: Initialize treats a false here as fatal, and "this renderer was asked
        // for no shadows" is not a failure to set them up.
        if ( m_Shadow.CascadeCount == 0 )
        {
            m_ShadowAttachments = ShadowAttachmentLease{};
            LogShadowBudget( 0.0 );
            return true;
        }

        m_ShadowShader = DefaultSurfaceProgram( MeshVertexPath::Static, MeshPass::ShadowDepth );
        if ( !m_ShadowShader )
        {
            LOG_ERROR( "Failed to load shadow shader" );
            return false;
        }

        // One R32F (in RGBA32F) light-space depth map + depth attachment PER CASCADE. Each cascade also
        // gets its own MaterialShadow so the 4 shadow passes don't alias a single shared light-matrix UBO
        // (all draws recorded into one command buffer would otherwise see the last cascade's matrix).
        //
        // TIMED, because "allocate the cascades lazily, when shadows are first switched on" is a real
        // design option and the only thing that can decide it is how long this loop takes. The clock is
        // around the ALLOCATION alone — not the pipelines below, which a lazy scheme would build once at
        // startup anyway.
        const auto allocStart = std::chrono::steady_clock::now();
        for ( uint32_t i = 0; i < m_Shadow.CascadeCount; ++i )
        {
            FramebufferSpecification shadowSpec;
            shadowSpec.DebugName = std::format( "ShadowCascade{}", i );
            shadowSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowColor );
            shadowSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowDepth );
            m_CascadeFB[i] = Graphic::Framebuffer::Create( shadowSpec );
            m_CascadeFB[i]->Resize( m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize );
            m_ShadowMaterial[i] = std::make_unique<MaterialShadow>();
        }
        const double allocMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - allocStart ).count();

        // The lease is what makes the LIVE total below true, and it is taken here rather than in
        // Initialize so that it is created and destroyed with the framebuffers it accounts for.
        m_ShadowAttachments = ShadowAttachmentLease{ m_Shadow };
        LogShadowBudget( allocMs );

        GraphicsPipelineSpecification spec;
        spec.DebugName         = "ShadowPipeline";
        spec.Layout            = MeshVertexLayout( MeshVertexPath::Static );
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = true;
        // STANDARD-Z, AND THE ONLY PASS IN THE ENGINE THAT IS. Everything else renders reversed-Z
        // (Core/Projection.hpp), but a cascade's projection is ORTHOGRAPHIC — its depth is linear in
        // light-space distance, so there is no 1/z curve for a float exponent to cancel and reversing it
        // would buy exactly zero precision while inverting the compare in seven sampling shaders and the
        // sign of the shadow bias. It is spelled with a raw CompareOp, not DepthCompare::, precisely so
        // that it does not silently follow the engine convention if that is ever revisited. Its render
        // pass clears depth to 1 via PassConfig::ClearDepth in RegisterShadowPass.
        spec.DepthCompareOp = CompareOp::LessOrEqual;
        // No culling in the shadow pass: store ALL faces so the map can never come out empty (front-face
        // culling under the engine's negative-height viewport could cull the wrong set and black out the
        // scene). Self-shadow acne is handled by the normal-offset + slope bias in the PBR sampling.
        spec.CullMode = CullMode::None;
        spec.Shader   = m_ShadowShader;
        // All cascade framebuffers share the same attachment formats, so one pipeline is render-pass
        // compatible with all of them.
        spec.Framebuffer = m_CascadeFB[0];

        const auto shadow = GraphicsPipeline::Create( spec );
        if ( !shadow )
        {
            LOG_ERROR( "[MeshRenderer] nothing will cast a shadow: {}", shadow.GetError() );
            return false;
        }
        m_ShadowPipeline = shadow.GetValue();

        // Instanced shadow caster (optional): same depth-only state, but the vertex pulls per-instance model
        // matrices from the InstanceTransforms SSBO. One instanced material per cascade (each its own light
        // matrix UBO + SSBO). If the shader is missing, instanced shadows are simply disabled.
        m_ShadowInstancedShader = DefaultSurfaceProgram( MeshVertexPath::Instanced, MeshPass::ShadowDepth );
        if ( m_ShadowInstancedShader )
        {
            GraphicsPipelineSpecification ispec = spec;
            ispec.DebugName                     = "ShadowPipelineInstanced";
            ispec.Shader                        = m_ShadowInstancedShader;
            if ( const auto instanced = GraphicsPipeline::Create( ispec ) )
            {
                m_ShadowInstancedPipeline = instanced.GetValue();

                for ( uint32_t i = 0; i < m_Shadow.CascadeCount; ++i )
                    m_ShadowInstancedMaterial[i] = std::make_unique<MaterialShadowInstanced>();
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] instanced shadow casting is off: {}", instanced.GetError() );
            }
        }

        // SKINNED caster (optional): same depth-only state and the same standard-Z convention, but the
        // skinned vertex layout and a vertex stage that skins before projecting. Without this cell the
        // cascade pass had nothing it could draw a skinned mesh WITH, which is half of why a character
        // cast no shadow; the other half is the queue the pass walks (RegisterShadowPass).
        m_ShadowSkinnedShader = DefaultSurfaceProgram( MeshVertexPath::Skinned, MeshPass::ShadowDepth );
        if ( m_ShadowSkinnedShader )
        {
            GraphicsPipelineSpecification sspec = spec;
            sspec.DebugName                     = "ShadowPipelineSkinned";
            sspec.Layout                        = MeshVertexLayout( MeshVertexPath::Skinned );
            sspec.Shader                        = m_ShadowSkinnedShader;
            if ( const auto skinnedShadow = GraphicsPipeline::Create( sspec ) )
            {
                m_ShadowSkinnedPipeline = skinnedShadow.GetValue();

                for ( uint32_t i = 0; i < m_Shadow.CascadeCount; ++i )
                    m_ShadowSkinnedMaterial[i] = std::make_unique<MaterialShadowSkinned>();
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] skinned meshes will cast no shadow: {}", skinnedShadow.GetError() );
            }
        }
        else
        {
            LOG_WARN( "[MeshRenderer] Skinned shadow-depth cell missing — skinned meshes will cast no shadow." );
        }

        return true;
    }

    void MeshRenderer::UpdateCascades()
    {
        // HOW MANY MATRICES THIS FRAME ACTUALLY HAS, cleared FIRST so that every path out of this function
        // — including the two early returns below — leaves it saying the truth.
        //
        // This is the third instance of the family the budget already fixed twice (a hardwired cascade
        // index, and a count asked of the class instead of the instance): the count published to the
        // shader was the count ALLOCATED, and the count fitted is a different number. A scene with no
        // directional light returns here having written no matrix at all, and every lit draw was still
        // told u_ShadowParams.w = 4 — so the shader walked four cascades whose matrices are the array's
        // initializer, which is identity for cascade 0 and the ZERO matrix for 1..3. A zero matrix divides
        // by w = 0; identity makes light space equal world space, so fragments near the origin test as
        // "inside cascade 0" and are sampled from a map nothing rendered. Nothing reports any of it.
        m_FittedCascades = 0;

        auto* const camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return;

        const auto& dirLights = m_SceneRenderer->GetDirectionLights();
        if ( dirLights.DirectionLights.empty() )
            return;

        // The fitting itself is pure math and lives in Engine/Graphic/ShadowCascades.hpp so a test can pin
        // it down — this function only feeds it the scene's numbers and stores the result.
        CascadeSetup setup;
        setup.CameraView       = camera->GetViewMatrix();
        setup.CameraProjection = camera->GetProjectionMatrix();
        setup.CameraNear       = camera->GetNear();
        setup.CameraFar        = camera->GetFar();
        setup.LightDirection   = glm::vec3( dirLights.DirectionLights[0].Direction );
        ApplyShadowQuality( setup, m_Shadow );
        setup.SplitLambda = m_SplitLambda;

        CascadeFit     fits[kMaxShadowCascades];
        const uint32_t n = ComputeShadowCascades( setup, fits );
        // Written HERE, beside the call whose return value it is, and nowhere else. The fitter can also
        // hand back 0 without an early return of ours — a zero-length light direction, or a MaxDistance
        // that has fallen behind the camera's near plane.
        m_FittedCascades = n;

        // Cascade 1 (near-mid) doubles as the Reflective Shadow Map camera. NOT the widest cascade:
        // one-bounce GI only matters within ~tens of metres of the camera, and the widest cascade
        // squeezed the whole neighbourhood into a couple of RSM texels — the VPL gather then found
        // almost no lit surface and the GI read as zero everywhere.
        //
        // DERIVED FROM THE COUNT rather than written as 1. A one-cascade budget has no cascade 1, and a
        // literal here would have left m_RSMViewProj at identity for ever: the GI pass would have gathered
        // against a light camera looking down the world axes from the origin, which is not an error
        // anything reports — it is a frame with plausible-looking, wrong bounce light.
        const uint32_t rsmCascade = ( n > 1u ) ? 1u : 0u;

        for ( uint32_t c = 0; c < n; ++c )
        {
            m_CascadeVP[c]                                          = fits[c].ViewProj;
            m_CascadeWorldPerTexel[static_cast<glm::length_t>( c )] = fits[c].WorldPerTexel;

            if ( c == rsmCascade )
            {
                m_RSMViewProj = fits[c].ViewProj;
                // Approximate light eye: back off from the fitted sphere's centre against the sun's travel
                // direction. Only feeds cameraUB.CameraPos, which the RSM's albedo/normal/position outputs
                // do not depend on — but a sane value keeps the shared G-buffer shader well-defined.
                const float dirLen = glm::length( setup.LightDirection );
                m_RSMEye = dirLen > 1e-6f ? fits[c].Center - ( setup.LightDirection / dirLen ) * fits[c].Radius
                                          : fits[c].Center;
            }
        }
    }

    DataDrivenMaterial* MeshRenderer::MaskedCasterMaterial( const DataDrivenMaterial* material,
                                                            MeshVertexPath path, uint32_t cascade )
    {
        auto* caster = Runtime::ResourceRegistry::GetMaterialService()->GetViewVariant(
             material, path, MeshPass::ShadowDepth, cascade );
        if ( caster == nullptr )
        {
            // Not drawn through the shared program instead: that is the square shadow this route exists to end.
            static std::unordered_set<const DataDrivenMaterial*> s_Refused;
            if ( s_Refused.insert( material ).second )
                LOG_ERROR( "[Shadows] a Masked material ('{}') has no ({} x ShadowDepth) caster of its own; its "
                           "objects cast NO shadow on that path (MaterialService named the cell it refused).",
                           material->GetShaderName(), MeshVertexPathName( path ) );
        }
        return caster;
    }

    GraphicsPipeline* MeshRenderer::CellPipeline( GraphicsPipeline* passState, const DataDrivenMaterial& cell )
    {
        if ( passState == nullptr )
            return nullptr;
        MeshCellPipelineKey key{ passState, cell.GetShaderName() };
        if ( const auto found = m_CellPipelines.find( key ); found != m_CellPipelines.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_CellPipelines[key];

        auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( key.CellShader );
        if ( !shader )
        {
            LOG_ERROR( "[MeshRenderer] cell '{}' will not draw in '{}': no such shader is registered",
                       key.CellShader, passState->GetSpecification().DebugName );
            return nullptr;
        }
        GraphicsPipelineSpecification spec = passState->GetSpecification();
        spec.DebugName                     = std::format( "{} {}", spec.DebugName, key.CellShader );
        spec.Shader                        = shader;
        const auto pipeline                = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[MeshRenderer] cell '{}' will not draw in '{}': {}", key.CellShader,
                       passState->GetSpecification().DebugName, pipeline.GetError() );
            return nullptr;
        }
        slot = pipeline.GetValue();
        return slot.get();
    }

    GraphicsPipeline* MeshRenderer::MaskedCasterPipeline( const DataDrivenMaterial& caster, MeshVertexPath path )
    {
        const auto& name = caster.GetShaderName();
        if ( const auto found = m_MaskedCasterPipelines.find( name ); found != m_MaskedCasterPipelines.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_MaskedCasterPipelines[name];

        // The shared caster's state for this path (vertex layout, standard-Z depth, the cascade target) — only
        // the program differs, which is the whole difference between an opaque and a masked caster.
        const GraphicsPipeline* shared = nullptr;
        switch ( path )
        {
            case MeshVertexPath::Instanced:
                shared = m_ShadowInstancedPipeline.get();
                break;
            case MeshVertexPath::Skinned:
                shared = m_ShadowSkinnedPipeline.get();
                break;
            case MeshVertexPath::Static:
                shared = m_ShadowPipeline.get();
                break;
        }
        auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
        if ( shared == nullptr || !shader )
        {
            LOG_ERROR( "[Shadows] masked caster '{}' will not draw: {}", name,
                       !shader ? "no such shader is registered" : "the path has no shared caster pipeline" );
            return nullptr;
        }
        GraphicsPipelineSpecification spec = shared->GetSpecification();
        spec.DebugName                     = std::format( "ShadowPipelineMasked {}", name );
        spec.Shader                        = shader;
        const auto pipeline                = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[Shadows] masked caster '{}' will not draw: {}", name, pipeline.GetError() );
            return nullptr;
        }
        slot = pipeline.GetValue();
        return slot.get();
    }

    // SETUP of cascade @p c (its node's Declare): every caster of the cascade is chosen here, ONCE, into @p list,
    // and the cascade's material state (light matrix, the packed instance transforms and poses) is written here;
    // the node's exec only records the list. The cascade matrix is this frame's: UpdateCascades runs before the
    // graph's setup (SceneRenderer, "Shadow: UpdateCascades").
    void MeshRenderer::BuildShadowCascadeDraws( const uint32_t c, MeshDrawList& list )
    {
        // Shadow vert computes Projection*View*Transform; feed the combined cascade matrix as
        // Projection and identity as View, matching u_LightViewProj[c] on the PBR side.
        m_ShadowMaterial[c]->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeVP[c] );
        const MaterialExecutor* casterExecutor = m_ShadowMaterial[c]->GetMaterialExecutor();

        // Shadow casters are MATERIAL-INDEPENDENT (depth only), so batch purely by Mesh*: any
        // group of >= 2 identical meshes collapses into ONE instanced draw per cascade. This is
        // the dominant cost in the 256-mesh stress test (256x4 per-object draws -> 4 draws).
        const bool instancingOn = m_ShadowInstancedPipeline && m_ShadowInstancedMaterial[c];

        // THE CASCADE'S OWN MATRIX, NOT THE CAMERA'S — and that distinction is the whole
        // safety of culling a shadow pass at all. An object behind the camera casts into
        // the frame it is not itself in, so the camera's frustum would buy draw calls with
        // missing shadows and the draw-call detector would report it as a win.
        //
        // WHY THIS ONE CHANGES NOTHING ON SCREEN. It is derived from `m_CascadeVP[c]`, the
        // exact matrix the caster vertex shader multiplies by (SetLightMatrix above passes
        // it as Projection with an identity View, and so does this). The cascade projection
        // is a FINITE orthographic box — `glm::orthoRH_ZO(-radius, radius, -radius, radius,
        // 10 cm, 4 * radius)` in ShadowCascades.hpp — so a caster outside that box is
        // already clipped by the rasterizer today. This skips exactly the geometry the GPU
        // was going to throw away, which is why it is provable rather than plausible.
        //
        // The plane extraction is convention-agnostic here and that is not luck: the
        // cascades are deliberately STANDARD-Z while the camera is reversed-Z, which swaps
        // which of the two derived planes is "near" and which is "far" — the SET of six
        // half-spaces is the same one either way, and Intersects tests all six by value.
        const Core::Frustum cascadeFrustum( m_CascadeVP[c], glm::mat4( 1.0f ) );

        // THE LOD OF A CASTER IS ASKED FROM THE CAMERA, not from the light, and that is
        // deliberate rather than convenient: a caster drawn into the cascade at a coarser
        // level than the object the camera sees casts a silhouette that does not match the
        // object it belongs to. The per-object caster loop below has always asked it this
        // way (ComputeLOD reads the main camera); the batched paths simply did not ask.
        const auto*     lodCamera       = m_SceneRenderer->GetMainCamera();
        const glm::vec3 lodViewPosition = lodCamera != nullptr ? lodCamera->GetPosition() : glm::vec3( 0.0f );

        std::vector<std::pair<Desert::StaticMesh*, std::vector<const StaticMeshRenderData*>>> byMesh;
        const auto bucketFor = [&]( Desert::StaticMesh* mesh ) -> std::vector<const StaticMeshRenderData*>&
        {
            for ( auto& [m, v] : byMesh )
                if ( m == mesh )
                    return v;
            byMesh.emplace_back( mesh, std::vector<const StaticMeshRenderData*>{} );
            return byMesh.back().second;
        };
        // MASKED CASTERS, by the material's blend mode (ShadowCasterCellFor): each draws through its own
        // template's ShadowDepth cell on a per-cascade copy of its material, with its mask texture and clip
        // threshold. One set per copy: its rows, instance transforms or bone poses end to end, uploaded once in
        // this setup, before the list is declared.
        m_ScratchMaskedDraws.clear();
        std::size_t maskedSetCount = 0;
        const auto  maskedSetFor   = [&]( DataDrivenMaterial* caster ) -> MaskedCasterSet&
        {
            for ( std::size_t i = 0; i < maskedSetCount; ++i )
            {
                if ( m_ScratchMaskedSets[i]->Caster == caster )
                {
                    return *m_ScratchMaskedSets[i];
                }
            }
            if ( maskedSetCount == m_ScratchMaskedSets.size() )
            {
                m_ScratchMaskedSets.push_back( std::make_unique<MaskedCasterSet>() );
            }
            auto& set  = *m_ScratchMaskedSets[maskedSetCount++];
            set.Caster = caster;
            set.Rows.clear();
            set.Transforms.clear();
            return set;
        };
        const auto isMasked = []( const DataDrivenMaterial* material )
        {
            return material != nullptr &&
                   ShadowCasterCellFor( material->GetSchema().Blend ) == ShadowCasterCell::Own;
        };

        for ( const auto& rd : m_StaticQueue )
        {
            if ( rd.Mesh == nullptr || !rd.CastShadows ||
                 !IsVisibleInView( cascadeFrustum, rd.Transform, Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) ) )
            {
                continue;
            }
            const PBRSlot slot = rd.MaterialSlots != nullptr
                                      ? FirstPBRSlot( rd.MaterialSlots->Slots, MeshVertexPath::Static )
                                      : PBRSlot{};
            MaterialInstance*         inst = slot.Instance;
            const DataDrivenMaterial* mat  = slot.Surface;
            if ( !isMasked( mat ) )
            {
                bucketFor( rd.Mesh ).push_back( &rd );
                continue;
            }
            auto* caster = MaskedCasterMaterial( mat, MeshVertexPath::Static, c );
            if ( caster == nullptr )
            {
                continue; // refused by name, once
            }
            auto&            set = maskedSetFor( caster );
            MaskedCasterDraw draw;
            draw.Set       = &set;
            draw.Path      = MeshVertexPath::Static;
            draw.Mesh      = rd.Mesh;
            draw.Transform = rd.Transform;
            draw.Instance  = inst;
            draw.Row       = AppendRow( set.Rows, EffectiveRow( mat, inst ) );
            draw.LodLevel  = ComputeLOD( rd.Transform, rd.Mesh, rd.ForcedLOD, rd.LODBias );
            m_ScratchMaskedDraws.push_back( draw );
        }

        // Pack all instanced-batch transforms contiguously; each batch reads its slice via
        // firstInstance. Upload the SSBO ONCE (final size) before any instanced draw is recorded.
        // Scratch members: capacity persists across cascades/frames (4 refills per frame).
        auto& instTransforms = m_ScratchInstTransforms;
        auto& batches        = m_ScratchShadowBatches;
        auto& singles        = m_ScratchShadowSingles;
        instTransforms.clear();
        batches.clear();
        singles.clear();
        for ( auto& [mesh, bucket] : byMesh )
        {
            if ( instancingOn && bucket.size() >= 2 )
            {
                // One draw per level, exactly as the geometry pass does it, and for the
                // same reason: the per-object caster loop below passes ComputeLOD to its
                // draw while this one used to pass nothing, so a caster's silhouette
                // changed detail depending on whether it found a twin.
                const uint32_t maxLevel = Geometry::MaxAvailableLOD( mesh->GetSubmeshes() );
                auto&          levels   = m_ScratchLodLevels;
                levels.clear();
                levels.reserve( bucket.size() );
                for ( const auto* rd : bucket )
                    levels.push_back(
                         std::min( ComputeLOD( rd->Transform, rd->Mesh, rd->ForcedLOD, rd->LODBias ), maxLevel ) );

                for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                {
                    const auto first = static_cast<uint32_t>( instTransforms.size() );
                    for ( std::size_t i = 0; i < bucket.size(); ++i )
                        if ( levels[i] == level )
                            instTransforms.push_back( bucket[i]->Transform );

                    const uint32_t count = static_cast<uint32_t>( instTransforms.size() ) - first;
                    if ( count < 2 )
                    {
                        instTransforms.resize( first );
                        for ( std::size_t i = 0; i < bucket.size(); ++i )
                            if ( levels[i] == level )
                                singles.push_back( bucket[i] );
                        continue;
                    }
                    batches.push_back( ShadowBatch{ mesh, count, first, level, InstanceWindPush{} } );
                }
            }
            else
            {
                for ( const auto* rd : bucket )
                    singles.push_back( rd );
            }
        }

        // UE-style Instanced Static Meshes cast too, unless the component says otherwise —
        // each becomes its own batch (or one per LOD level).
        if ( instancingOn )
        {
            for ( const auto& ism : m_InstancedQueue )
            {
                // THE FLAG EXISTS NOW, and until it did an ISM was the one mesh kind in the
                // engine whose shadow could not be turned off: the static and skinned
                // components both carry CastShadows and this pass read both, while the ISM
                // branch had no condition at all.
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                    continue;

                // Per-instance, against this cascade. A cascade covers a slice of the view,
                // so a forest spread over the map has most of its instances outside every
                // one of the four — and the batch is a single draw whose cost is entirely
                // its instance count.
                const Common::Math::AABB localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
                const uint32_t           maxLevel    = Geometry::MaxAvailableLOD( ism.Mesh->GetSubmeshes() );

                // The cull distance from the MAIN camera, as the geometry pass measures it:
                // an instance faded out of the view casts no shadow either.
                auto& visible = m_ScratchIsmVisible;
                auto& levels  = m_ScratchLodLevels;
                CollectIsmInstances( *ism.Transforms, localBounds, cascadeFrustum, ism.CullDistance, ism.Wind,
                                     lodViewPosition, lodViewPosition, maxLevel, visible, levels );
                if ( visible.empty() )
                    continue;

                // A MASKED ISM - foliage, grass cards - casts through its own template's (Instanced x
                // ShadowDepth) cell, never the shared batch that casts whole quads.
                MaterialInstance* ismInst = ism.Material.get();
                const auto*       ismMat =
                     ismInst != nullptr ? dynamic_cast<const DataDrivenMaterial*>( ismInst->GetParentMaterial() )
                                        : nullptr;
                if ( isMasked( ismMat ) )
                {
                    auto* caster = MaskedCasterMaterial( ismMat, MeshVertexPath::Instanced, c );
                    if ( caster == nullptr )
                    {
                        continue; // refused by name, once
                    }
                    auto&          set = maskedSetFor( caster );
                    const uint32_t row = AppendRow( set.Rows, EffectiveRow( ismMat, ismInst ) );
                    for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                    {
                        const auto first = static_cast<uint32_t>( set.Transforms.size() );
                        for ( std::size_t i = 0; i < visible.size(); ++i )
                        {
                            if ( levels[i] == level )
                            {
                                set.Transforms.push_back( visible[i] );
                            }
                        }
                        MaskedCasterDraw draw;
                        draw.Set      = &set;
                        draw.Path     = MeshVertexPath::Instanced;
                        draw.Mesh     = ism.Mesh;
                        draw.Instance = ismInst;
                        draw.Row      = row;
                        draw.Count    = static_cast<uint32_t>( set.Transforms.size() ) - first;
                        draw.First    = first;
                        draw.LodLevel = level;
                        draw.Wind     = PackInstanceWind( ism.Wind );
                        m_ScratchMaskedDraws.push_back( draw );
                    }
                    continue;
                }

                for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                {
                    const auto first = static_cast<uint32_t>( instTransforms.size() );
                    for ( std::size_t i = 0; i < visible.size(); ++i )
                        if ( levels[i] == level )
                            instTransforms.push_back( visible[i] );
                    batches.push_back( ShadowBatch{ ism.Mesh,
                                                    static_cast<uint32_t>( instTransforms.size() ) - first, first,
                                                    level, PackInstanceWind( ism.Wind ) } );
                }
            }
        }

        // Per-object path (singletons).
        for ( const auto* rd : singles )
        {
            list.Add( { .Pipeline  = m_ShadowPipeline.get(),
                        .Mesh      = rd->Mesh,
                        .Transform = rd->Transform,
                        .Material  = casterExecutor,
                        .LodLevel  = ComputeLOD( rd->Transform, rd->Mesh, rd->ForcedLOD, rd->LODBias ) } );
        }

        // Meshes drawn with a data-driven material (shader graph, Shader Override, per-slot
        // custom materials) cast through the SAME pipeline as everything else: a caster is
        // depth, and depth does not care which shader would have coloured the surface. They
        // used to be absent from the cascades entirely, because this pass only ever walked
        // the PBR queues — a shader-graph object was lit like a solid and shadowed like a
        // hole in the world.
        //
        // Per-object only, no instanced batching: the batching above keys on StaticMesh*,
        // and this queue holds Mesh* (it also carries skinned and procedurally-built
        // meshes). Casters here are counted in ones and twos, not in the hundreds the
        // batching exists for.
        //
        // The whole mesh is drawn, VisibleSubmeshMask ignored — deliberately, and the
        // reason exactly one draw per entity may set CastShadows: the mask splits an
        // entity's submeshes between this queue and the PBR one, but a caster is not
        // split, so honouring the mask here would carve the PBR half out of the silhouette
        // while the PBR record was already casting the whole of it.
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh != nullptr && g.CastShadows &&
                 IsVisibleInView( cascadeFrustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
            {
                list.Add( { .Pipeline  = m_ShadowPipeline.get(),
                            .Mesh      = g.Mesh,
                            .Transform = g.Transform,
                            .Material  = casterExecutor,
                            .LodLevel  = ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 ) } );
            }
        }

        // SKINNED casters. The cascade pass walked m_StaticQueue and m_GenericQueue by name
        // and simply had no line about skinned meshes, so a character was lit by the sun,
        // outlined correctly when selected, and cast nothing on the ground it stood on.
        //
        // Parameterized by the vertex path rather than added as a fourth special case: the
        // caster is (path x ShadowDepth) and this is that cell. Every pose in the cascade is
        // packed into ONE buffer on the cascade's own material and each draw names its slice,
        // for the same reason the forward skinned path does it — a per-draw upload would
        // leave the earlier recorded draws reading the last caster's pose.
        if ( m_ShadowSkinnedPipeline && m_ShadowSkinnedMaterial[c] && !m_SkinnedQueue.empty() )
        {
            auto* skinMat = m_ShadowSkinnedMaterial[c].get();
            skinMat->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeVP[c] );

            auto& skinBones = m_ScratchBones;
            skinBones.clear();
            std::vector<std::pair<const SkinnedMeshRenderData*, uint32_t>> casters;
            for ( const auto& sd : m_SkinnedQueue )
            {
                if ( sd.Mesh == nullptr || !sd.CastShadows || sd.BoneMatrices.empty() )
                    continue;
                if ( isMasked( sd.Material ) )
                {
                    // Hair cards, cloth fringes: the (Skinned x ShadowDepth) cell of ITS template.
                    auto* caster = MaskedCasterMaterial( sd.Material, MeshVertexPath::Skinned, c );
                    if ( caster == nullptr )
                    {
                        continue; // refused by name, once
                    }
                    auto&            set = maskedSetFor( caster );
                    MaskedCasterDraw draw;
                    draw.Set       = &set;
                    draw.Path      = MeshVertexPath::Skinned;
                    draw.Mesh      = sd.Mesh;
                    draw.Transform = sd.Transform;
                    draw.Instance  = sd.Instance;
                    draw.Row       = AppendRow( set.Rows, EffectiveRow( sd.Material, sd.Instance ) );
                    draw.First     = static_cast<uint32_t>( set.Transforms.size() );
                    set.Transforms.insert( set.Transforms.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
                    m_ScratchMaskedDraws.push_back( draw );
                    continue;
                }
                casters.emplace_back( &sd, static_cast<uint32_t>( skinBones.size() ) );
                skinBones.insert( skinBones.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
            }
            if ( !casters.empty() )
            {
                skinMat->UploadBones( skinBones );
                for ( const auto& [sd, boneOffset] : casters )
                {
                    list.Add(
                         { .Pipeline  = m_ShadowSkinnedPipeline.get(),
                           .Mesh      = sd->Mesh,
                           .Transform = sd->Transform,
                           .Material  = skinMat->GetMaterialExecutor(),
                           .BindState = [skinMat, offset = boneOffset] { skinMat->SetBoneOffset( offset ); } } );
                }
            }
        }

        // Instanced path.
        if ( instancingOn && !batches.empty() )
        {
            auto* instMat = m_ShadowInstancedMaterial[c].get();
            instMat->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeVP[c] );
            if ( auto* sb = instMat->Get<StorageBufferProperty>( "InstanceTransforms" ) )
                sb->SetRawData( instTransforms.data(),
                                static_cast<uint32_t>( instTransforms.size() * sizeof( glm::mat4 ) ) );
            for ( const auto& b : batches )
            {
                // The same wind the surface pass pushed for these instances, so the shadow
                // sways with the plant (Common/FoliageWind.glslh is the one formula).
                list.Add( { .Pipeline      = m_ShadowInstancedPipeline.get(),
                            .Mesh          = b.Mesh,
                            .Transform     = glm::mat4( 1.0f ),
                            .Material      = instMat->GetMaterialExecutor(),
                            .InstanceCount = b.Count,
                            .FirstInstance = b.First,
                            .LodLevel      = b.LodLevel,
                            .BindState     = [instMat, wind = b.Wind] { instMat->SetInstancedWind( wind ); } } );
            }
        }

        // Masked casters: each copy's rows and transforms uploaded once, here in setup, its camera set to THIS
        // cascade (the copy is this cascade's alone); then its draws, in collection order.
        for ( std::size_t i = 0; i < maskedSetCount; ++i )
        {
            auto& set = *m_ScratchMaskedSets[i];
            WriteLightCamera( *set.Caster, glm::mat4( 1.0f ), m_CascadeVP[c] );
            if ( auto* sb = set.Caster->Get<StorageBufferProperty>( "Materials" ) )
            {
                sb->SetRawData( set.Rows.data(), static_cast<uint32_t>( set.Rows.size() * sizeof( glm::vec4 ) ) );
            }
            if ( set.Transforms.empty() )
            {
                continue;
            }
            // The one binding the path adds (MeshPathOwnBinding): instance matrices or bone poses.
            const auto        path = MeshCellPath( set.Caster->GetShaderName() );
            const std::string own  = path ? MeshPathOwnBufferName( *path ) : std::string();
            if ( own.empty() )
            {
                continue;
            }
            if ( auto* sb = set.Caster->Get<StorageBufferProperty>( own ) )
            {
                sb->SetRawData( set.Transforms.data(),
                                static_cast<uint32_t>( set.Transforms.size() * sizeof( glm::mat4 ) ) );
            }
        }
        for ( const MaskedCasterDraw& draw : m_ScratchMaskedDraws )
        {
            DataDrivenMaterial* material  = draw.Set->Caster;
            const bool          instanced = draw.Path == MeshVertexPath::Instanced;
            list.Add( { .Pipeline      = MaskedCasterPipeline( *material, draw.Path ),
                        .Mesh          = draw.Mesh,
                        .Transform     = draw.Transform,
                        .Material      = material->GetMaterialExecutor(),
                        .InstanceCount = instanced ? draw.Count : 1,
                        .FirstInstance = instanced ? draw.First : 0,
                        .LodLevel      = draw.LodLevel,
                        .BindState     = [material, draw]
                        {
                            material->SetPushMatrix( draw.Transform );
                            material->SetMaterialIndex( draw.Row );
                            if ( draw.Path == MeshVertexPath::Instanced )
                            {
                                material->SetInstancedWind( draw.Wind );
                            }
                            if ( draw.Path == MeshVertexPath::Skinned )
                            {
                                material->SetSkinnedBoneOffset( draw.First );
                            }
                            material->Bind( draw.Instance );
                        } } );
        }
    }
    void MeshRenderer::RegisterShadowPass( RenderGraphBuilder& builder )
    {
        // Same guard as the geometry pass: the cascade pass reads m_ShadowPipeline's spec, and
        // SetupShadowPass can now refuse. No caster pipeline means no cascade passes and an unshadowed
        // scene, not a dead editor.
        if ( !m_ShadowPipeline )
            return;

        // One depth-only pass per cascade, all in DepthPrePass (before Geometry, which depends on it).
        // Cascade matrices are computed in UpdateCascades() before the graph records (intra-phase order is
        // nondeterministic, so per-pass matrix computation can't be relied on for ordering).
        for ( uint32_t c = 0; c < m_Shadow.CascadeCount; ++c )
        {
            if ( !m_CascadeFB[c] )
                continue;

            builder
                 .AddPass(
                      std::format( "MeshShadowCascade{}", c ), RenderPhase::DepthPrePass,
                      [this, c]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
                      {
                          if ( !m_ShadowsEnabled )
                              return BOOLSUCCESS;
                          // The casters this node's Declare chose.
                          if ( auto drawn = m_CascadeDraws[c].Record( context ); !drawn.IsSuccess() )
                              return drawn;

                          // Casters that are not meshes (the tessellated terrain), recorded into THIS pass so the
                          // cascade is cleared once and holds everyone's depth (IShadowCaster): the ones the
                          // Declare declared, each through its own blocks.
                          for ( const auto& [weak, firstBlock] : m_CascadeCasters[c] )
                          {
                              const auto caster = weak.lock();
                              if ( !caster )
                              {
                                  continue; // gone since the setup: its draws went with it
                              }
                              if ( auto cast =
                                        caster->RecordShadowCascade( context, c, firstBlock, m_CascadeVP[c] );
                                   !cast.IsSuccess() )
                              {
                                  return cast;
                              }
                          }
                          return BOOLSUCCESS;
                      },
                      m_ShadowPipeline->GetSpecification(), m_CascadeFB[c], {},
                      // Clear the R32F depth target to 1.0 (far): background texels must read as "no occluder",
                      // else the default 0.1 grey clear falsely shadows receivers whose light-space depth > 0.1.
                      glm::vec4( 1.0f ), RenderPassOrder::Default,
                      // And the DEPTH ATTACHMENT to 1.0 as well, overriding the engine's reversed-Z clear of
                      // 0. This pass is standard-Z (SetupShadowPass says why); a 0 clear under its LessOrEqual
                      // test would reject every caster and hand back an empty shadow map, silently.
                      1.0f )
                 .Declare = [this, c]( RenderPassDeclaration& declared, const FrameGraphRefs& )
            {
                // Depth-only casters sample no graph resource: one block per caster material with the material's
                // own fill, so ValidatePassBindings judges every caster before anything is recorded.
                m_CascadeDraws[c].Clear();
                m_CascadeCasters[c].clear();
                if ( !m_ShadowsEnabled )
                    return;
                BuildShadowCascadeDraws( c, m_CascadeDraws[c] );
                m_CascadeDraws[c].Declare( declared, std::nullopt );

                // The non-mesh casters declare their blocks after the list's
                // (IShadowCaster::DeclareShadowCascade).
                uint32_t nextBlock = m_CascadeDraws[c].BlockCount();
                for ( const auto& weak : m_ShadowCasters )
                {
                    if ( const auto caster = weak.lock() )
                    {
                        const uint32_t declaredBlocks = caster->DeclareShadowCascade( declared, c );
                        m_CascadeCasters[c].emplace_back( weak, nextBlock );
                        nextBlock += declaredBlocks;
                    }
                }
            };
        }
    }

} // namespace Desert::Graphic::System
