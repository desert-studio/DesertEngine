// MeshRenderer's light-view passes: the cascaded shadow map (setup, cascade fit, graph pass).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Materials/SceneResources.hpp>

#include <format>
#include <unordered_set>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MeshShadowShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, InstanceTransforms );
        DESERT_SHADER_PARAMETER( StorageBuffer, Materials );
    };
} // namespace

namespace Desert::Graphic::System
{
    using MeshRendererDetail::SignatureHash;

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
        LOG_INFO( "[Shadows] {} cascade(s) x {} directional light(s) at {}x{} over {:.0f} m = {:.1f} MiB of "
                  "attachments for this renderer, allocated in {:.1f} ms ({:.1f} MiB live across {} renderer(s) "
                  "holding cascades).",
                  m_Shadow.CascadeCount, m_CascadeSlots, m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize,
                  Common::Units::ToMetres( m_Shadow.MaxDistance ),
                  static_cast<double>( ShadowAttachmentBytes( m_Shadow, m_CascadeSlots ) ) / ( 1024.0 * 1024.0 ),
                  allocMs,
                  static_cast<double>( ShadowAttachmentLease::LiveBytes() ) / ( 1024.0 * 1024.0 ),
                  ShadowAttachmentLease::LiveHolders() );
    }

    void MeshRenderer::InvalidateCascadeCaches()
    {
        for ( CascadeCacheState& cache : m_CascadeCache )
            cache = CascadeCacheState{};
    }

    void MeshRenderer::DropDirectionalShadow( const uint32_t slot )
    {
        if ( slot >= kDirectionalShadowSlots )
            return;
        m_DirectionalShadow[slot].Fitted = 0;
        for ( uint32_t c = 0; c < kMaxCascades; ++c )
            m_CascadeCache[slot * kMaxCascades + c] = CascadeCacheState{};
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
        // ONLY WHAT THE BUDGET SIZES IS MADE AGAIN (UE re-creates its shadow depth targets on r.Shadow.*, not its
        // shadow-depth shaders): the cascade maps, their static caches, the lease that accounts for them and the
        // local views' rows over the atlas. The caster materials (one light-matrix UBO per shadow view) and the
        // three caster pipelines depend on the depth FORMAT, never on the map size or the cascade count, so they
        // stay - the rain-occlusion view, the local views and the VSM pages keep the materials their own planning
        // made (EnsureShadowViewMaterials), and a renderer that re-budgets can never hand a view a null material
        // (SHADOW-CINE-ABORT). The caller takes the re-budget at the frame boundary, before any shadow view of the
        // frame is planned (SceneRenderer::OnUpdate, RebudgetShadowsFor).
        //
        // The maps may still be referenced by a frame in flight; a quality change is rare and user-driven, so the
        // whole device is drained rather than each object's release deferred.
        Renderer::GetInstance().WaitDeviceIdle();
        for ( uint32_t i = 0; i < kCascadeViews; ++i )
        {
            m_CascadeStaticFB[i].reset();
            m_CascadeCache[i] = CascadeCacheState{};
        }
        // The local lights' atlas is the scene's (Graphic/LocalShadowCache.hpp): it follows the viewports' new
        // budget (LocalShadowAtlasSize) at the next frame boundary. This view's table goes.
        // The array is made again at the new size by the next frame (AcquireCascadeMaps); the device is idle.
        m_CascadeMaps      = RDG::ExternalTexture{};
        m_CascadeMapsBuilt = false;
        m_LocalShadowRows.clear();
        m_LocalRedraws.clear();
        m_LocalCachePlans.clear();
        m_ShadowAttachments = ShadowAttachmentLease{};
        for ( DirectionalShadowSlot& slot : m_DirectionalShadow )
            slot = DirectionalShadowSlot{};

        // The slots held are kept (SetupShadowPass allocates each at the new budget).
        TakeShadowBudget( budget );
        return SetupShadowPass();
    }

    bool MeshRenderer::SetupShadowPass()
    {
        // A ZERO BUDGET IS A LEGAL BUDGET, and the only place that has to know it is this one. A renderer
        // built with Graphic::kNoShadowQuality allocates no map, compiles no caster pipeline and loads no
        // shadow shader; everything downstream is already driven by the count it publishes
        // (ShadowCascadePasses hands no pass, CaptureFrameState reports 0, and the shader's cascade
        // loop over each slot's fitted count selects none), so there is no second switch to keep in step.
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

        // Two D32F depth-only images PER CASCADE (kShadowImagesPerCascade): the map the receivers sample and the
        // cache of its static casters (PlanCascadeCache). Each cascade also gets its own MaterialShadow so the 4
        // shadow passes don't alias a single shared light-matrix UBO (all draws recorded into one command buffer
        // would otherwise see the last cascade's matrix).
        //
        // TIMED, because "allocate the cascades lazily, when shadows are first switched on" is a real
        // design option and the only thing that can decide it is how long this loop takes. The clock is
        // around the ALLOCATION alone — not the pipelines below, which a lazy scheme would build once at
        // startup anyway.
        const auto allocStart = std::chrono::steady_clock::now();
        for ( uint32_t slot = 0; slot < m_CascadeSlots; ++slot )
            AllocateCascadeSlot( slot );
        const double allocMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - allocStart ).count();

        // The lease is what makes the LIVE total below true, and it is taken here rather than in
        // Initialize so that it is created and destroyed with the framebuffers it accounts for.
        m_ShadowAttachments = ShadowAttachmentLease{ m_Shadow, m_CascadeSlots };
        LogShadowBudget( allocMs );

        // The caster pipelines are built once: they are render-pass compatible with every cascade framebuffer of
        // any size (one depth format), so a re-budget keeps them.
        if ( m_ShadowPipeline )
        {
            EnsureCascadeCasterMaterials();
            return true;
        }

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
        // pass clears depth to 1 via SystemRasterPass::ClearDepth in ShadowCascadePasses.
        spec.DepthCompareOp = CompareOp::LessOrEqual;
        // No culling in the shadow pass: store ALL faces so the map can never come out empty (front-face
        // culling under the engine's negative-height viewport could cull the wrong set and black out the
        // scene). Self-shadow acne is handled by the normal-offset + slope bias in the lit sampling.
        spec.CullMode = CullMode::None;
        spec.Shader   = m_ShadowShader;
        // All cascade framebuffers share the same attachment formats, so one pipeline is render-pass
        // compatible with all of them.
        spec.Framebuffer = m_CascadeStaticFB[0];

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
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] instanced shadow casting is off: {}", instanced.GetError() );
            }
        }

        // SKINNED caster (optional): same depth-only state and the same standard-Z convention, but the
        // skinned vertex layout and a vertex stage that skins before projecting. Without this cell the
        // cascade pass had nothing it could draw a skinned mesh WITH, which is half of why a character
        // cast no shadow; the other half is the queue the pass walks (ShadowCascadePasses).
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

        EnsureCascadeCasterMaterials();
        return true;
    }

    void MeshRenderer::AllocateCascadeSlot( const uint32_t slot )
    {
        for ( uint32_t c = 0; c < m_Shadow.CascadeCount; ++c )
        {
            const uint32_t           v = slot * kMaxCascades + c;
            FramebufferSpecification cacheSpec;
            cacheSpec.DebugName = std::format( "ShadowCascade{}.{}.StaticCache", slot, c );
            cacheSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowDepth );
            m_CascadeStaticFB[v] = Graphic::Framebuffer::Create( cacheSpec );
            m_CascadeStaticFB[v]->Resize( m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize );
            m_CascadeCache[v] = CascadeCacheState{};
            // A re-budget keeps the cascade's caster material (RebudgetShadows); a cascade the old budget did
            // not have gets its own.
            if ( !m_ShadowMaterial[v] )
                m_ShadowMaterial[v] = std::make_unique<MaterialShadow>( MeshPass::ShadowDepth );
        }
    }

    void MeshRenderer::EnsureCascadeCasterMaterials()
    {
        // THE CASCADES' INSTANCED AND SKINNED CASTER MATERIALS, one per cascade view held, for each optional
        // pipeline that was built; a re-budget keeps the ones it has (RebudgetShadows) and adds what a larger
        // cascade count or a new slot needs.
        for ( uint32_t slot = 0; slot < m_CascadeSlots; ++slot )
            for ( uint32_t c = 0; c < m_Shadow.CascadeCount; ++c )
            {
                const uint32_t v = slot * kMaxCascades + c;
                if ( m_ShadowInstancedPipeline && !m_ShadowInstancedMaterial[v] )
                    m_ShadowInstancedMaterial[v] =
                         std::make_unique<MaterialShadowInstanced>( MeshPass::ShadowDepth );
                if ( m_ShadowSkinnedPipeline && !m_ShadowSkinnedMaterial[v] )
                    m_ShadowSkinnedMaterial[v] = std::make_unique<MaterialShadowSkinned>( MeshPass::ShadowDepth );
            }
    }

    bool MeshRenderer::EnsureCascadeSlots( const uint32_t slots )
    {
        if ( m_Shadow.CascadeCount == 0 || !m_ShadowPipeline || slots > kDirectionalShadowSlots )
            return false;
        if ( slots <= m_CascadeSlots )
            return true;
        // The cascade array the frames in flight sample is replaced by one with the new slot's layers; a slot is
        // taken once per renderer (and kept), so the device is drained rather than the array's release deferred
        // (RebudgetShadows does the same, at the same frame boundary: SceneRenderer, "Shadow: UpdateCascades").
        Renderer::GetInstance().WaitDeviceIdle();
        const auto allocStart = std::chrono::steady_clock::now();
        for ( uint32_t slot = m_CascadeSlots; slot < slots; ++slot )
            AllocateCascadeSlot( slot );
        m_CascadeSlots = slots;
        EnsureCascadeCasterMaterials();
        const double allocMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - allocStart ).count();
        // The next AcquireCascadeMaps makes the array with every held slot's layers and rebuilds each fitted
        // cascade into it from its cache.
        m_CascadeMaps       = RDG::ExternalTexture{};
        m_CascadeMapsBuilt  = false;
        m_ShadowAttachments = ShadowAttachmentLease{ m_Shadow, m_CascadeSlots };
        LogShadowBudget( allocMs );
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
        // told ShadowParams.w = 4 — so the shader walked four cascades whose matrices are the array's
        // initializer, which is identity for cascade 0 and the ZERO matrix for 1..3. A zero matrix divides
        // by w = 0; identity makes light space equal world space, so fragments near the origin test as
        // "inside cascade 0" and are sampled from a map nothing rendered. Nothing reports any of it.
        for ( DirectionalShadowSlot& slot : m_DirectionalShadow )
            slot = DirectionalShadowSlot{};
        // This frame's cache plan, cleared with the count and for the same reason: an early return below leaves no
        // cascade with work (MapHasMovable is the map's state, not the frame's, and survives).
        for ( CascadeCacheState& state : m_CascadeCache )
        {
            state.RedrawStatic = false;
            state.Rebuild      = false;
            state.Movable      = false;
        }

        auto* const camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return;

        // EVERY SHADOWED DIRECTIONAL LIGHT, by its slot (= its Atmosphere Sun Light Index): the sun with the view's
        // resolved policy (SetShadows), atmosphere light 1 with its own component's (SceneRenderer::AtmosphereLight1
        // Frame). Each is fitted by the same call below; nothing after this list knows which light a slot is.
        struct SlotLight
        {
            glm::vec3           Direction{ 0.0f };
            bool                CastShadows        = false;
            CascadeBiasSettings Bias;
            float               SplitLambda        = 0.6f;
            float               SourceAngleDegrees = 0.0f;
        };
        std::optional<SlotLight> lights[kDirectionalShadowSlots];
        const auto&              dirLights = m_SceneRenderer->GetDirectionLights();
        if ( !dirLights.DirectionLights.empty() )
            lights[SceneResources::kDirectionalShadowSun] =
                 SlotLight{ .Direction          = glm::vec3( dirLights.DirectionLights[0].Direction ),
                            .CastShadows        = m_ShadowsEnabled,
                            .Bias               = m_ShadowBias,
                            .SplitLambda        = m_SplitLambda,
                            .SourceAngleDegrees = m_SunSourceAngle };
        static_assert( kDirectionalShadowSlots == 2, "a slot per atmosphere light: list each one's source here" );
        if ( const auto& light1 = m_SceneRenderer->GetAtmosphereLight1() )
            lights[1] = SlotLight{ .Direction          = glm::vec3( light1->Light.Direction ),
                                   .CastShadows        = light1->CastShadows,
                                   .Bias               = light1->ShadowBias,
                                   .SplitLambda        = light1->CascadeSplitLambda,
                                   .SourceAngleDegrees = light1->SourceAngleDegrees };

        for ( uint32_t slot = 0; slot < kDirectionalShadowSlots; ++slot )
        {
            const std::optional<SlotLight>& light = lights[slot];
            // A light whose shadows are off has no cascades at all (UE: no whole-scene shadow for it): it is
            // packed CastShadows 0 and lit, and its slot is never taken for it.
            if ( !light || !light->CastShadows )
                continue;
            if ( !EnsureCascadeSlots( slot + 1 ) )
                continue;
            DirectionalShadowSlot& state = m_DirectionalShadow[slot];
            state.CastShadows            = true;
            // UE's LightSourceAngle: the light's angular DIAMETER in degrees; the penumbra grows by tan(angle / 2)
            // per centimetre between the occluder and the receiver (each side of the edge).
            state.SourceTanHalf =
                 std::tan( glm::radians( std::clamp( light->SourceAngleDegrees, 0.0f, 89.0f ) ) * 0.5f );

            // The fitting itself is pure math and lives in Engine/Graphic/ShadowCascades.hpp so a test can pin
            // it down — this function only feeds it the scene's numbers and stores the result.
            CascadeSetup setup;
            setup.CameraView       = camera->GetViewMatrix();
            setup.CameraProjection = camera->GetProjectionMatrix();
            setup.CameraNear       = camera->GetNear();
            setup.CameraFar        = camera->GetFar();
            setup.LightDirection   = light->Direction;
            ApplyShadowQuality( setup, m_Shadow );
            setup.SplitLambda = light->SplitLambda;

            CascadeFit     fits[kMaxShadowCascades];
            const uint32_t n = ComputeShadowCascades( setup, fits );
            // Written HERE, beside the call whose return value it is, and nowhere else. The fitter can also
            // hand back 0 without an early return of ours — a zero-length light direction, or a MaxDistance
            // that has fallen behind the camera's near plane.
            state.Fitted = n;

            for ( uint32_t c = 0; c < n; ++c )
            {
                const uint32_t v                                     = slot * kMaxCascades + c;
                m_CascadeVP[v]                                       = fits[c].ViewProj;
                state.WorldPerTexel[static_cast<glm::length_t>( c )] = fits[c].WorldPerTexel;
                state.DepthRange[static_cast<glm::length_t>( c )]    = fits[c].DepthRange;
                // The caster's bias, in world centimetres scaled per cascade (CascadeBiasSettings), as the
                // matrix the casters draw with and the slope term their depth cell adds.
                m_CascadeCasterBias[v] = ComputeCascadeCasterBias( light->Bias, fits[c], fits[0].WorldPerTexel );
                m_CascadeCasterVP[v]   = CasterViewProj( fits[c].ViewProj, m_CascadeCasterBias[v].DepthBias );
                // The movable layer draws the same cascade with the same matrix and bias, through its own view.
                m_CascadeCasterVP[kCascadeMovableViewBase + v]   = m_CascadeCasterVP[v];
                m_CascadeCasterBias[kCascadeMovableViewBase + v] = m_CascadeCasterBias[v];
            }
        }
        // The cache plan is NOT made here: the casters have not resolved this frame's draws yet (the grass
        // sections, the terrain's cascade rows). SceneRenderer calls PlanCascadeCache once they have.
    }

    SceneResources::DirectionalShadowViews MeshRenderer::DirectionalShadows() const
    {
        SceneResources::DirectionalShadowViews views{};
        for ( uint32_t slot = 0; slot < kDirectionalShadowSlots; ++slot )
        {
            const DirectionalShadowSlot& state = m_DirectionalShadow[slot];
            // The min() is belt and braces against the fitted count and the budget ever being written from
            // different places again; a slot not held has no layers to sample.
            const uint32_t count =
                 slot < m_CascadeSlots ? std::min( state.Fitted, m_Shadow.CascadeCount ) : 0u;
            views[slot] = SceneResources::DirectionalShadowView{
                 .CascadeVP            = m_CascadeVP.data() + slot * kMaxCascades,
                 .Count                = count,
                 .FirstLayer           = CascadeLayer( slot * kMaxCascades ),
                 .CastShadows          = state.CastShadows,
                 .SourceTanHalf        = state.SourceTanHalf,
                 .CascadeWorldPerTexel = state.WorldPerTexel,
                 .CascadeDepthRange    = state.DepthRange };
        }
        return views;
    }

    void MeshRenderer::GatherShadowCasters()
    {
        ShadowCasterSetup& setup = m_ShadowCasterSetup;
        ++setup.Gather;
        setup.Statics.resize( m_StaticQueue.size() );
        setup.Generics.resize( m_GenericQueue.size() );
        setup.Skinned.resize( m_SkinnedQueue.size() );
        setup.Fields.resize( m_InstancedQueue.size() );
        setup.Instances.clear();
        setup.InstanceIndex.clear();
        // The box is the one IsVisibleInView would build per view from the same transform and local bounds, built
        // once; an empty local box stays empty (no extent: in every view).
        const auto worldBox = []( const glm::mat4& transform, const Common::Math::AABB& local )
        { return Geometry::IsEmpty( local ) ? local : Geometry::TransformBounds( transform, local ); };
        for ( std::size_t i = 0; i < m_StaticQueue.size(); ++i )
        {
            const StaticMeshRenderData& rd = m_StaticQueue[i];
            setup.Statics[i]               = ShadowCasterRow{};
            if ( rd.Mesh != nullptr && rd.CastShadows )
                setup.Statics[i].WorldBounds =
                     worldBox( rd.Transform, Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) );
        }
        for ( std::size_t i = 0; i < m_GenericQueue.size(); ++i )
        {
            const GenericMeshRenderData& g = m_GenericQueue[i];
            setup.Generics[i]              = ShadowCasterRow{};
            if ( g.Mesh != nullptr && g.CastShadows )
                setup.Generics[i].WorldBounds =
                     worldBox( g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) );
        }
        for ( std::size_t i = 0; i < m_SkinnedQueue.size(); ++i )
        {
            const SkinnedMeshRenderData& sd = m_SkinnedQueue[i];
            setup.Skinned[i]                = ShadowCasterRow{};
            if ( sd.Mesh == nullptr || !sd.CastShadows || sd.BoneMatrices.empty() )
                continue;
            // The box of this frame's pose, not the bind pose's: an arm raised out of the rest box still reaches
            // the views it shadows (UE culls a skeletal mesh by its posed bounds); the box a virtual shadow map
            // level invalidates and culls it by too.
            setup.Skinned[i].WorldBounds = SkinnedPoseBounds( *sd.Mesh, sd.Transform, sd.BoneMatrices );
        }
        // A field's instances are gathered by the first view that walks it (ShadowIsmInstances).
        for ( ShadowIsmField& field : setup.Fields )
            field = ShadowIsmField{};
    }

    bool MeshRenderer::ShadowCastersGathered() const
    {
        const ShadowCasterSetup& setup = m_ShadowCasterSetup;
        return setup.Statics.size() == m_StaticQueue.size() && setup.Generics.size() == m_GenericQueue.size() &&
               setup.Skinned.size() == m_SkinnedQueue.size() && setup.Fields.size() == m_InstancedQueue.size();
    }

    uint32_t MeshRenderer::ShadowStaticLod( const uint32_t index )
    {
        ShadowCasterRow& row = m_ShadowCasterSetup.Statics[index];
        if ( row.LodLevel == kShadowLodUnknown )
        {
            const StaticMeshRenderData& rd = m_StaticQueue[index];
            row.LodLevel                   = ComputeLOD( rd.Transform, rd.Mesh, rd.ForcedLOD, rd.LODBias );
        }
        return row.LodLevel;
    }

    uint32_t MeshRenderer::ShadowGenericLod( const uint32_t index )
    {
        ShadowCasterRow& row = m_ShadowCasterSetup.Generics[index];
        if ( row.LodLevel == kShadowLodUnknown )
        {
            const GenericMeshRenderData& g = m_GenericQueue[index];
            row.LodLevel                   = ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 );
        }
        return row.LodLevel;
    }

    const MeshRenderer::ShadowIsmField& MeshRenderer::ShadowIsmInstances( const uint32_t field )
    {
        ShadowCasterSetup& setup  = m_ShadowCasterSetup;
        ShadowIsmField&    caster = setup.Fields[field];
        if ( caster.Gather == setup.Gather )
            return caster;
        caster =
             ShadowIsmField{ .Gather = setup.Gather, .First = static_cast<uint32_t>( setup.Instances.size() ) };
        const InstancedMeshRenderData& ism = m_InstancedQueue[field];
        // THE LOD AND THE CULL DISTANCE ARE THE MAIN CAMERA'S, as the geometry pass measures them: an instance
        // faded out of the view casts no shadow either, and a caster drawn at a coarser level than the object the
        // camera sees casts a silhouette that does not match it (CollectIsmInstances' rule, asked once here).
        const auto*             camera = m_SceneRenderer->GetMainCamera();
        const Geometry::LODView lodView =
             camera != nullptr ? Geometry::LODView{ camera->GetPosition(), camera->GetProjectionMatrix() }
                               : Geometry::LODView{};
        const Common::Math::AABB      localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
        const InstanceCullDistance    lensCull    = ScaledByLens( ism.CullDistance, DrawDistanceScale( lodView ) );
        const std::vector<glm::mat4>& transforms  = *ism.Transforms;
        for ( uint32_t k = 0; k < static_cast<uint32_t>( transforms.size() ); ++k )
        {
            const std::optional<Common::Math::AABB> reach =
                 IsmInstanceReach( lensCull, k, transforms[k], localBounds, ism.Wind, lodView.Origin );
            if ( !reach )
                continue;
            setup.Instances.push_back(
                 { .WorldBounds = *reach,
                   .LodLevel    = Geometry::SelectLODFromBounds( transforms[k], localBounds, lodView,
                                                                 ism.Mesh->GetLODScreenSizes(), -1, 0 ) } );
            setup.InstanceIndex.push_back( k );
        }
        caster.Count = static_cast<uint32_t>( setup.Instances.size() ) - caster.First;
        return caster;
    }

    std::size_t MeshRenderer::ShadowBucketKeyHash::operator()( const ShadowBucketKey& key ) const noexcept
    {
        std::size_t h = std::hash<const void*>{}( key.Mesh );
        h ^= std::hash<uint64_t>{}( key.Hidden.Low ) + 0x9E3779B97F4A7C15ull + ( h << 6u ) + ( h >> 2u );
        h ^= std::hash<const void*>{}( key.Hidden.High.data() ) + 0x9E3779B97F4A7C15ull + ( h << 6u ) +
             ( h >> 2u );
        return h ^ ( key.Hidden.High.size() * 0x9E3779B97F4A7C15ull );
    }

    uint64_t MeshRenderer::CascadeStaticSignature( const uint32_t cascade, bool& movable )
    {
        // THE CASTER BOX, as BuildShadowCascadeDraws culls with: a static caster outside it is not in the cache,
        // so it moving changes nothing drawn.
        // Casters gathered for other queues: no key can be trusted, the layer is redrawn (and its draws refused,
        // BuildShadowCascadeDraws says why).
        if ( !ShadowCastersGathered() )
        {
            movable = true;
            return 0;
        }
        const Core::Frustum frustum( m_CascadeCasterVP[cascade], glm::mat4( 1.0f ) );
        // The gathered boxes (GatherShadowCasters), the ones BuildShadowCascadeDraws culls with.
        const auto reaches = [&frustum]( const ShadowCasterRow& row )
        { return IsVisibleInView( frustum, row.WorldBounds ); };
        SignatureHash hash;
        for ( std::size_t i = 0; i < m_StaticQueue.size(); ++i )
        {
            const StaticMeshRenderData& rd = m_StaticQueue[i];
            if ( rd.Mesh == nullptr || !rd.CastShadows || !reaches( m_ShadowCasterSetup.Statics[i] ) )
                continue;
            hash.Add( rd.Mesh->GetRenderRevision() );
            hash.Add( rd.Transform );
            hash.Add( rd.HiddenSubmeshes );
            hash.Add( rd.ForcedLOD );
            hash.Add( rd.LODBias );
            // The level the layer draws it at follows the main camera (ShadowStaticLod): a new one redraws.
            hash.Add( ShadowStaticLod( static_cast<uint32_t>( i ) ) );
            // The entry's section material decides the caster cell (opaque or masked) and a masked one's clip
            // (ShadingOf: the same answer the casters draw with).
            const SectionShading shading = ShadingOf( rd );
            hash.Add( rd.SectionSlot );
            hash.Add( shading.Surface );
            hash.Add( shading.Instance );
        }
        for ( std::size_t i = 0; i < m_GenericQueue.size(); ++i )
        {
            const GenericMeshRenderData& g = m_GenericQueue[i];
            if ( g.Mesh == nullptr || !g.CastShadows || !reaches( m_ShadowCasterSetup.Generics[i] ) )
                continue;
            hash.Add( g.Mesh->GetRenderRevision() );
            hash.Add( g.Transform );
            hash.Add( ShadowGenericLod( static_cast<uint32_t>( i ) ) );
        }
        // A still instanced mesh is keyed by its snapshot (m_CascadeKeySnapshots says why that is sound) and by
        // the instances the layer draws of it: the ones the main camera's cull distance keeps in the box, each
        // with its level (the gather's, as BuildShadowCascadeDraws walks them), so an instance crossing the cull
        // distance or a level redraws the layer instead of leaving its old depth cached. A swaying one makes the
        // cascade movable only when one of its kept instances reaches it (UE: a WPO primitive invalidates the
        // dynamic layer of the views its bounds touch, not of every view): a crown beyond the cascade's box
        // draws nothing into it, so it must not force its movable layer every frame.
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                continue;
            if ( ism.Wind.Sways() )
            {
                const ShadowIsmField& swaying =
                     ShadowIsmInstances( static_cast<uint32_t>( &ism - m_InstancedQueue.data() ) );
                for ( uint32_t k = swaying.First; k < swaying.First + swaying.Count && !movable; ++k )
                    movable = reaches( m_ShadowCasterSetup.Instances[k] );
                continue;
            }
            hash.Add( ism.Mesh->GetRenderRevision() );
            hash.Add( ism.Transforms.get() );
            hash.Add( ism.Transforms->size() );
            hash.Add( ism.Material.get() );
            const ShadowIsmField& kept =
                 ShadowIsmInstances( static_cast<uint32_t>( &ism - m_InstancedQueue.data() ) );
            for ( uint32_t k = kept.First; k < kept.First + kept.Count; ++k )
            {
                if ( !reaches( m_ShadowCasterSetup.Instances[k] ) )
                    continue;
                hash.Add( m_ShadowCasterSetup.InstanceIndex[k] );
                hash.Add( m_ShadowCasterSetup.Instances[k].LodLevel );
            }
        }
        for ( std::size_t i = 0; i < m_SkinnedQueue.size(); ++i )
        {
            const SkinnedMeshRenderData& sd = m_SkinnedQueue[i];
            if ( sd.Mesh != nullptr && sd.CastShadows && !sd.BoneMatrices.empty() &&
                 reaches( m_ShadowCasterSetup.Skinned[i] ) )
                movable = true;
        }
        for ( const auto& gpu : m_GpuInstancedQueue )
            if ( gpu.CastShadows && ( !gpu.WorldBounds || frustum.Intersects( *gpu.WorldBounds ) ) )
                movable = true; // regenerated around the camera every frame
        if ( m_GpuInstancedCastersExpected )
            movable = true; // submitted later this frame (ExpectGpuInstancedCasters)
        return hash.Value;
    }

    void MeshRenderer::PlanCascadeCache()
    {
        // UE's cached shadow maps for a directional light's cascades: the static casters' depth is kept per
        // cascade and drawn again only when the cascade's matrix (the sun turned, the camera moved a texel) or
        // one of the static casters it reaches changed; the movable casters are drawn over a copy of it every
        // frame they exist.
        if ( !m_ShadowPipeline )
            return;

        for ( uint32_t v = 0; v < kCascadeViews; ++v )
        {
            m_CascadeKeyedCasters[v].clear();
            m_CascadeUnkeyedCasters[v].clear();
        }

        for ( uint32_t slot = 0; slot < m_CascadeSlots; ++slot )
        {
            // A light without shadows draws none of its cascades (its slot is packed lit).
            if ( !m_DirectionalShadow[slot].CastShadows )
                continue;
            const uint32_t count = std::min( m_DirectionalShadow[slot].Fitted, m_Shadow.CascadeCount );
            for ( uint32_t c = 0; c < count; ++c )
            {
                const uint32_t     v     = slot * kMaxCascades + c;
                CascadeCacheState& state = m_CascadeCache[v];
                if ( !m_CascadeStaticFB[v] )
                    continue;
                bool          movable = false;
                SignatureHash key;
                key.Add( CascadeStaticSignature( v, movable ) );
                // The non-mesh casters (the landscape): a keyed one is drawn into the static layer and its key is
                // the layer's (a sculpted height, a streamed displacement mip, a moved camera's LOD or dice redraw
                // it); an unkeyed one makes the cascade movable. Asked of THIS frame's draws: the plan runs after
                // TerrainRenderer::PrepareLocalShadowViews.
                for ( const auto& weak : m_ShadowCasters )
                {
                    const auto caster = weak.lock();
                    if ( !caster )
                        continue;
                    if ( const std::optional<uint64_t> cast = caster->CascadeShadowSignature( v ) )
                    {
                        key.Add( *cast );
                        m_CascadeKeyedCasters[v].push_back( weak );
                    }
                    else
                    {
                        movable = true;
                        m_CascadeUnkeyedCasters[v].push_back( weak );
                    }
                }
                key.Add( m_CascadeCasterVP[v] );
                key.Add( m_CascadeCasterBias[v] );
                EnsureShadowViewMaterials( kCascadeMovableViewBase + v );

                state.Movable      = movable;
                state.RedrawStatic = state.StaticKey != key.Value;
                state.StaticKey    = key.Value;
                state.Rebuild      = state.RedrawStatic || state.Movable || state.MapHasMovable;
                if ( state.Rebuild )
                    state.MapHasMovable = state.Movable;
            }
        }

        m_CascadeKeySnapshots.clear();
        for ( const auto& ism : m_InstancedQueue )
            if ( ism.Transforms )
                m_CascadeKeySnapshots.push_back( ism.Transforms );
    }

    bool MeshRenderer::IsMaskedSurface( const DataDrivenMaterial* material )
    {
        // Own caster cell: a Masked surface (its holes) or a `.Parallax` one (its relief's depth).
        return material != nullptr &&
               ShadowCasterCellFor( material->GetSchema().Blend, IsParallaxCell( material->GetShaderName() ) ) ==
                    ShadowCasterCell::Own;
    }

    bool MeshRenderer::IsTranslucentSurface( const DataDrivenMaterial* material )
    {
        return material != nullptr && Core::Formats::IsTranslucentBlend( material->GetSchema().Blend );
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const StaticMeshRenderData& data )
    {
        const SurfaceSlot slot = SectionSurface( data.MaterialSlots.get(), data.SectionSlot );
        return { slot.Instance, slot.Surface, IsMaskedSurface( slot.Surface ),
                 IsTranslucentSurface( slot.Surface ) };
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const InstancedMeshRenderData& data )
    {
        MaterialInstance* instance = data.Material.get();
        auto*             surface =
             instance != nullptr ? dynamic_cast<DataDrivenMaterial*>( instance->GetParentMaterial() ) : nullptr;
        return { instance, surface, IsMaskedSurface( surface ), IsTranslucentSurface( surface ) };
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const GpuInstancedRenderData& data )
    {
        MaterialInstance* instance = data.Material.get();
        auto*             surface =
             instance != nullptr ? dynamic_cast<DataDrivenMaterial*>( instance->GetParentMaterial() ) : nullptr;
        return { instance, surface, IsMaskedSurface( surface ) };
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const SkinnedSection& section )
    {
        return { section.Instance, section.Material, IsMaskedSurface( section.Material ),
                 IsTranslucentSurface( section.Material ) };
    }

    DataDrivenMaterial* MeshRenderer::MaskedCasterMaterial( const DataDrivenMaterial* material,
                                                            MeshVertexPath path, uint32_t cascade ) const
    {
        const MeshPass pass =
             IsVirtualShadowView( cascade ) ? MeshPass::VirtualShadowDepth : MeshPass::ShadowDepth;
        auto* caster =
             Runtime::ResourceRegistry::GetMaterialService()->GetViewVariant( material, path, pass, cascade );
        if ( caster == nullptr )
        {
            // Not drawn through the shared program instead: that is the square shadow this route exists to end.
            static std::unordered_set<const DataDrivenMaterial*> s_Refused;
            if ( s_Refused.insert( material ).second )
                LOG_ERROR(
                     "[Shadows] a Masked material ('{}') has no ({} x {}) caster of its own; its objects cast "
                     "NO shadow on that path (MaterialService named the cell it refused).",
                     material->GetShaderName(), MeshVertexPathName( path ), MeshPassName( pass ) );
        }
        return caster;
    }

    GraphicsPipeline* MeshRenderer::CellPipeline( GraphicsPipeline* passState, const DataDrivenMaterial& cell )
    {
        // The cell's OWN program: a permutation (ShaderService::AcquireVariant - the DESERT_LOCAL_SHADOW_VSM twin
        // of a forward cell) carries its base program's name but is not registered under it, and its bindings
        // are its own; a pipeline found by the name alone would bind the base program to the twin's sets.
        const MaterialExecutor* const executor = cell.GetMaterialExecutor();
        const std::shared_ptr<Shader> program  = executor != nullptr ? executor->GetShader() : nullptr;
        if ( program == nullptr || program->GetVariant().IsDefault() )
            return CellPipeline( passState, cell.GetShaderName() );
        return CellPipeline( passState, cell.GetShaderName(), program );
    }

    GraphicsPipeline* MeshRenderer::CellPipeline( GraphicsPipeline* passState, const std::string& cellShader )
    {
        return CellPipeline( passState, cellShader, nullptr );
    }

    GraphicsPipeline* MeshRenderer::CellPipeline( GraphicsPipeline* passState, const std::string& cellShader,
                                                  const std::shared_ptr<Shader>& permutation )
    {
        if ( passState == nullptr )
            return nullptr;
        MeshCellPipelineKey key{ passState, cellShader,
                                 permutation != nullptr ? permutation->GetVariant().Hash() : 0u };
        if ( const auto found = m_CellPipelines.find( key ); found != m_CellPipelines.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_CellPipelines[key];

        auto shader =
             permutation != nullptr ? permutation : Runtime::ResourceRegistry::GetShaderService()->GetByName( key.CellShader );
        if ( !shader )
        {
            LOG_ERROR( "[MeshRenderer] cell '{}' will not draw in '{}': no such shader is registered",
                       key.CellShader, passState->GetSpecification().DebugName );
            return nullptr;
        }
        GraphicsPipelineSpecification spec = passState->GetSpecification();
        spec.DebugName = permutation != nullptr
                              ? std::format( "{} {}#{:016x}", spec.DebugName, key.CellShader, key.Variant )
                              : std::format( "{} {}", spec.DebugName, key.CellShader );
        // The pass's own program is an engine pipeline the reveal waits for; any other program is a CONTENT
        // material's cell, compiled on demand (AL1-12) and drawn only once ChooseCellDraw says it is Ready.
        const bool content = shader != spec.Shader;
        spec.Shader        = shader;
        // Reached through the one spelling the tree uses for the pipeline cache, which is how the shipping
        // pipeline register (ShippingPipelines suite) recognises a creation site; each answer is RETURNED
        // into `pipeline`, which is how the refusal scanner (GraphicsPipelineRefusal) sees it read.
        const auto pipeline = [&]
        {
            if ( content )
                return m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( spec );
            return m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        }();
        if ( !pipeline )
        {
            LOG_ERROR( "[MeshRenderer] cell '{}' will not draw in '{}': {}", key.CellShader,
                       passState->GetSpecification().DebugName, pipeline.GetError() );
            return nullptr;
        }
        slot = pipeline.GetValue();
        return slot.get();
    }

    MaterialPipelineTracker::CellDraw MeshRenderer::ChooseCellDraw( GraphicsPipeline*         passState,
                                                                    const DataDrivenMaterial& cell,
                                                                    const bool                twoSided,
                                                                    const GraphicsPipeline*   standIn )
    {
        GraphicsPipeline* own = CellPipeline( passState, cell );
        if ( own != nullptr && twoSided )
            own = CullPermutation( own, true );

        // Keyed by the PIPELINE, not the shader: one cell has a pipeline per pass state and cull permutation,
        // and they leave the driver at different moments.
        const std::string key =
             own != nullptr
                  ? own->GetSpecification().DebugName
                  : std::format( "{} {}{}", passState != nullptr ? passState->GetSpecification().DebugName : "?",
                                 cell.GetShaderName(), twoSided ? "_TwoSided" : "" );
        return ChoosePipelineDraw( own, key, standIn );
    }

    MaterialPipelineTracker::CellDraw MeshRenderer::ChoosePipelineDraw( const GraphicsPipeline* own,
                                                                        const std::string&      key,
                                                                        const GraphicsPipeline* standIn )
    {
        MaterialPipelineState reports = MaterialPipelineState::Failed; // refused: its builder logged why
        if ( own != nullptr )
        {
            switch ( own->GetReadiness() )
            {
                case PipelineReadiness::Compiling:
                    reports = MaterialPipelineState::Compiling;
                    break;
                case PipelineReadiness::Ready:
                    reports = MaterialPipelineState::Ready;
                    break;
                case PipelineReadiness::Failed:
                    reports = MaterialPipelineState::Failed;
                    break;
            }
        }
        const bool standInReady = standIn != nullptr && standIn->GetReadiness() == PipelineReadiness::Ready;
        const auto before       = m_MaterialPipelines.StateOf( key );
        const auto choice       = m_MaterialPipelines.ChooseCell( key, reports, standInReady );
        if ( choice.Draw != MaterialPipelineTracker::CellDraw::Own )
            ++m_StandInDraws;
        else if ( before && *before != MaterialPipelineState::Ready )
            LOG_INFO( "[MeshRenderer] '{}' draws its own pipeline now (it was {})", key,
                      MaterialPipelineStateName( *before ) );
        if ( choice.Announce )
            LOG_INFO( "[MeshRenderer] '{}' draws the default surface until its pipeline is ready (pipeline {}{})",
                      key, MaterialPipelineStateName( reports ),
                      choice.Draw == MaterialPipelineTracker::CellDraw::Nothing
                           ? "; the default surface is not ready either, so it is not drawn yet"
                           : "" );
        return choice.Draw;
    }

    MeshRenderer::StandInCell* MeshRenderer::StandIn( const MeshVertexPath path, const MeshPass pass )
    {
        const uint32_t key = ( static_cast<uint32_t>( path ) * 16u ) + static_cast<uint32_t>( pass );
        if ( const auto found = m_StandIns.find( key ); found != m_StandIns.end() )
            return found->second.Material ? &found->second : nullptr;
        auto& slot    = m_StandIns[key];
        slot.Material = CreateCellMaterial( path, pass );
        if ( slot.Material )
            slot.Instance = slot.Material->CreateInstance(
                 std::format( "DefaultSurfaceStandIn_{}_{}", MeshVertexPathName( path ), MeshPassName( pass ) ) );
        if ( !slot.Material || !slot.Instance )
        {
            LOG_ERROR(
                 "[MeshRenderer] no default-surface stand-in for ({} x {}): a material whose pipeline is not "
                 "ready yet is not drawn in that pass",
                 MeshVertexPathName( path ), MeshPassName( pass ) );
            slot.Material.reset();
            return nullptr;
        }
        return &slot;
    }

    MeshRenderer::TranslucentDraw* MeshRenderer::TranslucentStandIn()
    {
        const auto name = DefaultSurfaceShaderName( MeshVertexPath::Static, MeshPass::Forward );
        return name ? TranslucentDrawFor( *name ) : nullptr;
    }

    MeshRenderer::CasterPipelines MeshRenderer::CasterPipelinesOf( const uint32_t view )
    {
        if ( IsVirtualShadowView( view ) )
            return { m_VsmPipeline.get(), m_VsmInstancedPipeline.get(), m_VsmSkinnedPipeline.get(),
                     MeshPass::VirtualShadowDepth };
        return { m_ShadowPipeline.get(), m_ShadowInstancedPipeline.get(), m_ShadowSkinnedPipeline.get(),
                 MeshPass::ShadowDepth };
    }

    GraphicsPipeline* MeshRenderer::MaskedCasterPipeline( const DataDrivenMaterial& caster, MeshVertexPath path,
                                                          const MeshPass pass )
    {
        const auto& name = caster.GetShaderName();
        if ( const auto found = m_MaskedCasterPipelines.find( name ); found != m_MaskedCasterPipelines.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_MaskedCasterPipelines[name];

        // The shared caster's state for this path and pass (vertex layout, standard-Z depth, the cascade target or
        // the virtual map's render area) — only the program differs, which is the whole difference between an
        // opaque and a masked caster. The cell's name names its pass, so the name alone keys the pipeline.
        const CasterPipelines casters =
             CasterPipelinesOf( pass == MeshPass::VirtualShadowDepth ? kVsmViewBase : 0u );
        const GraphicsPipeline* shared = nullptr;
        switch ( path )
        {
            case MeshVertexPath::Instanced:
            case MeshVertexPath::GrassBlade: // the same vertex layout: the blade strip is a mesh
                shared = casters.Instanced;
                break;
            case MeshVertexPath::Skinned:
                shared = casters.Skinned;
                break;
            case MeshVertexPath::Static:
                shared = casters.Static;
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
    void MeshRenderer::BuildShadowCascadeDraws( const uint32_t c, MeshDrawList& list,
                                                const ShadowCasterLayer layer )
    {
        // The layer split is the cascade cache's (PlanCascadeCache): what stands still is drawn into the cache
        // only when it changed, what moves by itself is drawn over the copied cache every frame.
        const bool drawStatic  = layer != ShadowCasterLayer::Movable;
        const bool drawMovable = layer != ShadowCasterLayer::Static;
        if ( !ShadowCastersGathered() )
        {
            LOG_ERROR(
                 "[Shadows] shadow view {} drawn from casters gathered for other mesh queues (a queue changed "
                 "after MeshRenderer::GatherShadowCasters); it draws no mesh caster this frame.",
                 c );
            return;
        }
        // Shadow vert computes Projection*View*Transform; feed the combined cascade matrix as
        // Projection and identity as View, matching LightViewProjection[c] on the lit side.
        m_ShadowMaterial[c]->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                             m_CascadeCasterBias[c], m_ParallaxQuality );
        const MaterialExecutor* casterExecutor = m_ShadowMaterial[c]->GetMaterialExecutor();
        // The pipelines of the view's caster cells: the cascades' and local views' depth attachment, or the
        // virtual shadow map's attachment-less pool raster (IsVirtualShadowView).
        const CasterPipelines casters = CasterPipelinesOf( c );

        // Shadow casters are MATERIAL-INDEPENDENT (depth only), so batch purely by Mesh*: any
        // group of >= 2 identical meshes collapses into ONE instanced draw per cascade. This is
        // the dominant cost in the 256-mesh stress test (256x4 per-object draws -> 4 draws).
        const bool instancingOn = casters.Instanced != nullptr && m_ShadowInstancedMaterial[c];

        // THE CASCADE'S OWN MATRIX, NOT THE CAMERA'S — and that distinction is the whole
        // safety of culling a shadow pass at all. An object behind the camera casts into
        // the frame it is not itself in, so the camera's frustum would buy draw calls with
        // missing shadows and the draw-call detector would report it as a win.
        //
        // WHY THIS ONE CHANGES NOTHING ON SCREEN. It is derived from `m_CascadeCasterVP[c]`, the
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
        Core::Frustum cascadeFrustum( m_CascadeCasterVP[c], glm::mat4( 1.0f ) );
        // A virtual shadow map level keeps the casters on the sun's side of its depth range: they draw with depth
        // clamp and land on its sun-side end (EnsureVirtualShadowPipelines). That end is clip z >= 0 of the
        // level's standard-Z [0,1] projection, the plane the extraction names PLANE_FAR (row 2 alone,
        // Frustum::Rebuild).
        // A local light's mip is a perspective view from the light: its near-plane casters are clamped and
        // dropped by the raster (Common/VirtualShadowRaster.glslh), nothing lies beyond its far plane.
        if ( c >= kVsmViewBase && c < kRainOcclusionView )
            cascadeFrustum.Open( Core::PLANE_FAR );

        // THE LOD OF A CASTER IS ASKED FROM THE CAMERA, not from the light, and that is deliberate rather than
        // convenient: a caster drawn into the cascade at a coarser level than the object the camera sees casts a
        // silhouette that does not match the object it belongs to. Asked once per gather (ShadowStaticLod,
        // ShadowGenericLod, ShadowIsmInstances), the same level in every view; the box every view culls a caster
        // by is the gathered one (GatherShadowCasters).
        const ShadowCasterSetup& gathered = m_ShadowCasterSetup;
        const auto               inView   = [&cascadeFrustum]( const Common::Math::AABB& box )
        { return IsVisibleInView( cascadeFrustum, box ); };

        // Keyed by (mesh, hidden mask): a section caster batches with the same section of its twins. The buckets
        // in first-seen order; the key's bucket found by m_ScratchShadowBucketOf, not by a search.
        std::vector<std::pair<std::pair<Desert::Mesh*, SubmeshMask>, std::vector<const StaticMeshRenderData*>>>
             byMesh;
        m_ScratchShadowBucketOf.clear();
        const auto bucketFor = [&]( Desert::Mesh*     mesh,
                                    const SubmeshMask hidden ) -> std::vector<const StaticMeshRenderData*>&
        {
            const auto [it, added] =
                 m_ScratchShadowBucketOf.try_emplace( ShadowBucketKey{ mesh, hidden }, byMesh.size() );
            if ( added )
                byMesh.emplace_back( std::make_pair( mesh, hidden ), std::vector<const StaticMeshRenderData*>{} );
            return byMesh[it->second].second;
        };
        const auto staticIndex = [this]( const StaticMeshRenderData* rd )
        { return static_cast<uint32_t>( rd - m_StaticQueue.data() ); };
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
            set.Transforms.clear();
            return set;
        };
        const auto isMasked = []( const DataDrivenMaterial* material ) { return IsMaskedSurface( material ); };

        // A sun cascade's layer issued from its GPU-culled bins (AttachBinned below; the static layer at the
        // cascade view, the movable layer at CascadeMovableView). Local views and VSM levels (layer All) issue
        // none.
        // A VSM level's two layers (its still casters and its movers) are both walked: the map issues no bins.
        const bool binnedLayer = layer != ShadowCasterLayer::All && !IsVirtualShadowView( c );
        if ( binnedLayer )
            AttachBinned( list, GpuDriven::MeshPass::ShadowCascade, c );
        // THE CPU WALK of this view (CpuWalkHidden, the one rule of every view pass): a VSM level (its VG-only
        // raster), a sun cascade's static layer (its bins and their cluster raster: a VG primitive is walked for
        // every submesh the raster does not draw - a refused cut or carrier leaves it whole to the walk), a sun
        // cascade's movable layer (its bins; no cluster raster), or a view with neither (a local light):
        // everything. Empty: the walk draws none of the record.
        const ClusterWalkView walkView = IsVirtualShadowView( c ) ? VirtualShadowWalkView()
                                         : binnedLayer ? BinnedWalkView( GpuDriven::MeshPass::ShadowCascade, c )
                                                       : UnbinnedWalkView( GpuDriven::MeshPass::ShadowCascade );
        const auto            walkHidden = [&]( const Desert::Mesh*             mesh, const std::size_t /*submeshes*/,
                                     const GpuDriven::PrimitiveIndex primitive,
                                     const uint64_t                  hidden ) -> std::optional<SubmeshMask>
        { return CpuWalkHidden( walkView, mesh, primitive, hidden ); };
        for ( std::size_t i = 0; i < m_StaticQueue.size(); ++i )
        {
            const StaticMeshRenderData& rd = m_StaticQueue[i];
            if ( !drawStatic || rd.Mesh == nullptr || !rd.CastShadows )
                continue;
            // Asked before the CPU frustum test, so a record the bins hold costs this loop no culling work.
            const std::optional<SubmeshMask> walk =
                 walkHidden( rd.Mesh, rd.Mesh->GetSubmeshes().size(), rd.GpuPrimitive, rd.HiddenSubmeshes );
            if ( !walk || !inView( gathered.Statics[i].WorldBounds ) )
                continue;
            const SubmeshMask hidden = *walk;
            // The caster takes the material the LIT draw shows, by the same mapping: one entry per section
            // material (Rules::SplitMeshSlotDraws), shaded by SectionSurface, casting only its own submeshes - so
            // a masked section takes the masked route and an opaque one the opaque route.
            const SurfaceSlot         slot = SectionSurface( rd.MaterialSlots.get(), rd.SectionSlot );
            MaterialInstance*         inst = slot.Instance;
            const DataDrivenMaterial* mat  = slot.Surface;
            if ( !isMasked( mat ) )
            {
                bucketFor( rd.Mesh, hidden ).push_back( &rd );
                continue;
            }
            auto* caster = MaskedCasterMaterial( mat, MeshVertexPath::Static, c );
            if ( caster == nullptr )
            {
                continue; // refused by name, once
            }
            // The caster's row is the lit draw's, from the scene material table (SyncGpuScene: the receiving
            // row; a caster never reads ReceiveSunShadows).
            const std::optional<uint32_t> row = FindMaterialRow( mat, inst );
            if ( !row )
            {
                continue;
            }
            auto&            set = maskedSetFor( caster );
            MaskedCasterDraw draw;
            draw.Set       = &set;
            draw.Path      = MeshVertexPath::Static;
            draw.Mesh      = rd.Mesh;
            draw.Transform = rd.Transform;
            draw.Instance  = inst;
            draw.Row       = *row;
            draw.LodLevel          = ShadowStaticLod( static_cast<uint32_t>( i ) );
            draw.HiddenSubmeshMask = hidden;
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
        for ( auto& [meshKey, bucket] : byMesh )
        {
            Desert::Mesh* const mesh = meshKey.first;
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
                    levels.push_back( std::min( ShadowStaticLod( staticIndex( rd ) ), maxLevel ) );

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
                                singles.push_back( { bucket[i], meshKey.second } );
                        continue;
                    }
                    batches.push_back(
                         ShadowBatch{ mesh, count, first, level, meshKey.second, InstanceWindPush{} } );
                }
            }
            else
            {
                for ( const auto* rd : bucket )
                    singles.push_back( { rd, meshKey.second } );
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
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() ||
                     !( ism.Wind.Sways() ? drawMovable : drawStatic ) )
                    continue;
                // The field's walk (walkHidden over its primitive, the rule the static loop above reads): a still
                // VG field in a cascade or a VSM level hides exactly its clustered submeshes; a field the bins or
                // the clusters hold whole is skipped.
                SubmeshMask ismHidden{ ism.HiddenSubmeshes };
                if ( const auto field = static_cast<size_t>( &ism - m_InstancedQueue.data() );
                     field < m_FieldPrimitives.size() )
                {
                    const auto*       fieldMesh = dynamic_cast<const Desert::StaticMesh*>( ism.Mesh );
                    const std::size_t submeshes = ism.Mesh->GetSubmeshes().size();
                    const std::optional<SubmeshMask> walk =
                         walkHidden( fieldMesh, submeshes, m_FieldPrimitives[field], ism.HiddenSubmeshes );
                    if ( !walk )
                        continue;
                    ismHidden = *walk;
                }

                // Per-instance, against this cascade. A cascade covers a slice of the view,
                // so a forest spread over the map has most of its instances outside every
                // one of the four — and the batch is a single draw whose cost is entirely
                // its instance count. The instances the main camera's cull distance keeps, with the box the
                // wind can reach and the level, are the gather's (ShadowIsmInstances: asked once per field).
                const ShadowIsmField& field =
                     ShadowIsmInstances( static_cast<uint32_t>( &ism - m_InstancedQueue.data() ) );
                auto& visible = m_ScratchIsmVisible;
                auto& levels  = m_ScratchLodLevels;
                visible.clear();
                levels.clear();
                for ( uint32_t k = field.First; k < field.First + field.Count; ++k )
                {
                    if ( !inView( gathered.Instances[k].WorldBounds ) )
                        continue;
                    visible.push_back( ( *ism.Transforms )[gathered.InstanceIndex[k]] );
                    levels.push_back( gathered.Instances[k].LodLevel );
                }
                if ( visible.empty() )
                    continue;

                // A MASKED ISM - foliage, grass cards - casts through its own template's (Instanced x
                // ShadowDepth) cell, never the shared batch that casts whole quads.
                MaterialInstance* ismInst = ism.Material.get();
                const auto*       ismMat  = ismInst != nullptr
                                                 ? dynamic_cast<const DataDrivenMaterial*>( ismInst->GetParentMaterial() )
                                                 : nullptr;
                if ( isMasked( ismMat ) )
                {
                    auto* caster = MaskedCasterMaterial( ismMat, MeshVertexPath::Instanced, c );
                    if ( caster == nullptr )
                    {
                        continue; // refused by name, once
                    }
                    const std::optional<uint32_t> row = FindMaterialRow( ismMat, ismInst );
                    if ( !row )
                    {
                        continue;
                    }
                    auto& set = maskedSetFor( caster );
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
                        draw.HiddenSubmeshMask = ismHidden;
                        draw.Instance = ismInst;
                        draw.Row      = *row;
                        draw.Count    = static_cast<uint32_t>( set.Transforms.size() ) - first;
                        draw.First    = first;
                        draw.LodLevel = level;
                        draw.Wind     = PackInstanceWind( ism.Wind );
                        draw.BoundsPad         = WindBoundsPad( ism.Wind );
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
                    batches.push_back( ShadowBatch{
                         ism.Mesh, static_cast<uint32_t>( instTransforms.size() ) - first, first, level, ismHidden,
                         PackInstanceWind( ism.Wind ), WindBoundsPad( ism.Wind ) } );
                }
            }
        }

        // Per-object path (singletons).
        for ( const ShadowSingle& single : singles )
        {
            const StaticMeshRenderData* rd = single.Data;
            list.Add( { .Pipeline          = casters.Static,
                        .Mesh              = rd->Mesh,
                        .Transform         = rd->Transform,
                        .Material          = casterExecutor,
                        .HiddenSubmeshMask = single.Hidden,
                        .LodLevel          = ShadowStaticLod( staticIndex( rd ) ) } );
        }

        // Meshes drawn with a data-driven material (shader graph, Shader Override, per-slot
        // custom materials) cast through the SAME pipeline as everything else: a caster is
        // depth, and depth does not care which shader would have coloured the surface. They
        // used to be absent from the cascades entirely, because this pass only ever walked
        // the lit queues — a shader-graph object was lit like a solid and shadowed like a
        // hole in the world.
        //
        // Per-object only, no instanced batching: this queue also carries skinned and
        // procedurally-built meshes, and its casters are counted in ones and twos, not in
        // the hundreds the batching exists for.
        //
        // Each draw casts its OWN submeshes (VisibleSubmeshMask), as every static caster does: an entity's draws
        // split its sections between this queue and the batched ones, and the silhouette is their union
        // (Rules::SplitMeshSlotDraws gives each draw the entity's CastShadows).
        //
        // No masked route here, and none can apply: BlendMode Masked exists only on a Surface template
        // (DShaderParser refuses it on a shader with no Surface block), a template draws only through its mesh
        // cells, and a slot material with a mesh cell never reaches this queue (CustomSlotMaterial). The generic
        // lit draw runs the shader's own program, which clips nothing, so an opaque caster is what it shows.
        for ( std::size_t i = 0; i < m_GenericQueue.size(); ++i )
        {
            const GenericMeshRenderData& g = m_GenericQueue[i];
            if ( !drawStatic || g.Mesh == nullptr || !g.CastShadows )
                continue;
            // Asked before the CPU frustum test, so a record the bins hold costs this loop no culling work.
            const std::optional<SubmeshMask> walk =
                 walkHidden( g.Mesh, g.Mesh->GetSubmeshes().size(), g.GpuPrimitive, ~g.VisibleSubmeshMask );
            if ( walk && inView( gathered.Generics[i].WorldBounds ) )
            {
                list.Add( { .Pipeline          = casters.Static,
                            .Mesh              = g.Mesh,
                            .Transform         = g.Transform,
                            .Material          = casterExecutor,
                            .HiddenSubmeshMask = *walk,
                            .LodLevel          = ShadowGenericLod( static_cast<uint32_t>( i ) ) } );
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
        if ( drawMovable && casters.Skinned != nullptr && m_ShadowSkinnedMaterial[c] && !m_SkinnedQueue.empty() )
        {
            auto* skinMat = m_ShadowSkinnedMaterial[c].get();
            skinMat->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                     m_CascadeCasterBias[c], m_ParallaxQuality );

            auto& skinBones = m_ScratchBones;
            skinBones.clear();
            // Per section, as the static entries: a masked section through its template's caster, masked to its
            // own submeshes; the opaque sections of an entry are ONE depth draw (no material to tell apart),
            // drawing their union over one copy of the pose.
            struct OpaqueCaster
            {
                const SkinnedMeshRenderData* Data       = nullptr;
                uint32_t                     BoneOffset = 0;
                uint64_t                     Hidden     = 0;
                std::size_t                  Index      = 0; // into m_SkinnedQueue
            };
            std::vector<OpaqueCaster> opaqueCasters;
            for ( std::size_t i = 0; i < m_SkinnedQueue.size(); ++i )
            {
                const SkinnedMeshRenderData& sd = m_SkinnedQueue[i];
                // Culled by the box of this frame's pose (GatherShadowCasters), in every view.
                if ( sd.Mesh == nullptr || !sd.CastShadows || sd.BoneMatrices.empty() ||
                     !inView( gathered.Skinned[i].WorldBounds ) )
                    continue;
                uint64_t opaqueVisible = 0;
                for ( const SkinnedSection& section : sd.Sections )
                {
                    // The movable layer's bins hold the section (the cull issues it): not walked.
                    if ( !walkHidden( sd.Mesh, sd.Mesh->GetSubmeshes().size(), sd.GpuPrimitive,
                                      section.HiddenSubmeshes ) )
                        continue;
                    if ( !isMasked( section.Material ) )
                    {
                        opaqueVisible |= ~section.HiddenSubmeshes;
                        continue;
                    }
                    // Hair cards, cloth fringes: the (Skinned x ShadowDepth) cell of ITS template.
                    auto* caster = MaskedCasterMaterial( section.Material, MeshVertexPath::Skinned, c );
                    if ( caster == nullptr )
                    {
                        continue; // refused by name, once
                    }
                    const std::optional<uint32_t> row = FindMaterialRow( section.Material, section.Instance );
                    if ( !row )
                    {
                        continue;
                    }
                    auto&            set = maskedSetFor( caster );
                    MaskedCasterDraw draw;
                    draw.Set       = &set;
                    draw.Path      = MeshVertexPath::Skinned;
                    draw.Mesh      = sd.Mesh;
                    draw.Transform = sd.Transform;
                    draw.Instance  = section.Instance;
                    draw.Row       = *row;
                    draw.First     = static_cast<uint32_t>( set.Transforms.size() );
                    draw.GpuPrimitive = sd.GpuPrimitive;
                    draw.HiddenSubmeshMask = section.HiddenSubmeshes;
                    if ( IsVirtualShadowView( c ) )
                        draw.WorldBounds = gathered.Skinned[i].WorldBounds;
                    set.Transforms.insert( set.Transforms.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
                    m_ScratchMaskedDraws.push_back( draw );
                }
                if ( opaqueVisible == 0 )
                    continue;
                opaqueCasters.push_back( { &sd, static_cast<uint32_t>( skinBones.size() ), ~opaqueVisible, i } );
                skinBones.insert( skinBones.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
            }
            if ( !opaqueCasters.empty() )
            {
                skinMat->UploadBones( skinBones );
                for ( const OpaqueCaster& caster : opaqueCasters )
                {
                    // The cached pose when the skin cache holds this primitive (one skinning per frame).
                    const GpuDriven::GpuScenePrimitive row =
                         m_GpuScene != nullptr && m_GpuScene->IsValid( caster.Data->GpuPrimitive )
                              ? m_GpuScene->Primitive( caster.Data->GpuPrimitive )
                              : GpuDriven::GpuScenePrimitive{};
                    const uint64_t address =
                         ( static_cast<uint64_t>( row.SkinCacheAddressHi ) << 32u ) | row.SkinCacheAddressLo;
                    // A virtual shadow map level culls the caster by its posed box (CollectVirtualShadowCull).
                    std::optional<Common::Math::AABB> pose;
                    if ( IsVirtualShadowView( c ) )
                        pose = gathered.Skinned[caster.Index].WorldBounds;
                    list.Add( { .Pipeline          = casters.Skinned,
                                .Mesh              = caster.Data->Mesh,
                                .Transform         = caster.Data->Transform,
                                .Material          = skinMat->GetMaterialExecutor(),
                                .HiddenSubmeshMask = caster.Hidden,
                                .Push              = DrawPushFields{}
                                             .BoneOffset( caster.BoneOffset, /*required*/ false )
                                             .SkinCache( address, row.SkinCacheVertexCount ),
                                .WorldBounds = pose } );
                }
            }
        }

        // Instanced path.
        if ( instancingOn && !batches.empty() )
        {
            auto* instMat = m_ShadowInstancedMaterial[c].get();
            instMat->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                     m_CascadeCasterBias[c], m_ParallaxQuality );
            if ( auto* sb = instMat->Get<StorageBufferProperty>( kInstanceTransformsBlock ) )
                sb->SetRawData( instTransforms.data(),
                                static_cast<uint32_t>( instTransforms.size() * sizeof( glm::mat4 ) ) );
            for ( const auto& b : batches )
            {
                // The same wind the surface pass pushed for these instances, so the shadow
                // sways with the plant (Common/FoliageWind.glslh is the one formula).
                list.Add( { .Pipeline          = casters.Instanced,
                            .Mesh              = b.Mesh,
                            .Transform         = glm::mat4( 1.0f ),
                            .Material          = instMat->GetMaterialExecutor(),
                            .InstanceCount     = b.Count,
                            .FirstInstance     = b.First,
                            .HiddenSubmeshMask = b.HiddenSubmeshMask,
                            .LodLevel          = b.LodLevel,
                            .Push              = DrawPushFields{}.Wind( b.Wind ),
                            .Instances = std::span<const glm::mat4>( instTransforms ).subspan( b.First, b.Count ),
                            .BoundsPad = b.BoundsPad } );
            }
        }

        // Masked casters: each copy's rows and transforms uploaded once, here in setup, its camera set to THIS
        // cascade (the copy is this cascade's alone); then its draws, in collection order.
        for ( std::size_t i = 0; i < maskedSetCount; ++i )
        {
            auto& set = *m_ScratchMaskedSets[i];
            WriteLightCamera( *set.Caster, glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                              m_CascadeCasterBias[c], m_ParallaxQuality );
            // The draws' rows are the scene material table's (SyncGpuScene); a refusal was said by name.
            if ( !LendMaterialTable( *set.Caster ) )
            {
                continue;
            }
            if ( set.Transforms.empty() )
            {
                continue;
            }
            // The one binding the path adds (MeshPathOwnBinding): instance matrices or bone poses.
            const auto path = MeshCellPath( set.Caster->GetShaderName() );
            // A skinned caster also reads its primitive's skin-cache fields from the scene's GpuScene
            // (Vertex_Skinned.glslh).
            if ( path == MeshVertexPath::Skinned )
            {
                auto*      rows  = set.Caster->Get<StorageBufferProperty>( SceneResources::kGpuScenePrimitives );
                const bool bound = rows != nullptr && m_GpuScene != nullptr && m_GpuScene->GetPrimitivesBuffer();
                DESERT_VERIFY( bound, "a skinned masked caster reads GpuScenePrimitives but the view carries none" );
                rows->LendViewBuffer( m_GpuScene->GetPrimitivesBuffer() );
            }
            const std::optional<RDG::StorageBufferParameter> own =
                 path ? MeshPathOwnBuffer( *path ) : std::nullopt;
            if ( !own )
            {
                continue;
            }
            if ( auto* sb = set.Caster->Get<StorageBufferProperty>( *own ) )
            {
                sb->SetRawData( set.Transforms.data(),
                                static_cast<uint32_t>( set.Transforms.size() * sizeof( glm::mat4 ) ) );
            }
        }
        bool maskedPending = false;
        for ( const MaskedCasterDraw& draw : m_ScratchMaskedDraws )
        {
            DataDrivenMaterial* material  = draw.Set->Caster;
            const bool          instanced = draw.Path == MeshVertexPath::Instanced;
            // A masked caster draws through its OWN pipeline only once the driver has built it (UE: a draw whose
            // pipeline is not ready is not submitted). Handed to the list unbuilt or refused, the cascade pass
            // would fault at its bind ("could not be bound in the open render pass") and take the cascade's copy
            // and the composite reading the cascades with it. Refused (null; MaskedCasterPipeline said why, once)
            // or failed in the driver: that caster casts no shadow. Still compiling: not drawn THIS frame, and the
            // static cache is marked stale so the cascade redraws it once the pipeline is ready.
            GraphicsPipeline* const pipeline = MaskedCasterPipeline( *material, draw.Path, casters.Pass );
            if ( pipeline == nullptr || pipeline->GetReadiness() == PipelineReadiness::Failed )
                continue;
            if ( pipeline->GetReadiness() == PipelineReadiness::Compiling )
            {
                maskedPending = true;
                continue;
            }
            list.Add(
                 { .Pipeline          = pipeline,
                   .Mesh              = draw.Mesh,
                   .Transform         = draw.Transform,
                   .Material          = material->GetMaterialExecutor(),
                   .InstanceCount     = instanced ? draw.Count : 1,
                   .FirstInstance     = instanced ? draw.First : 0,
                   .HiddenSubmeshMask = draw.HiddenSubmeshMask,
                   .LodLevel          = draw.LodLevel,
                   .Push =
                        [&draw]
                   {
                       DrawPushFields push;
                       push.Transform( draw.Transform ).MaterialIndex( draw.Row );
                       if ( draw.Path == MeshVertexPath::Instanced )
                           push.Wind( draw.Wind );
                       if ( draw.Path == MeshVertexPath::Skinned )
                           push.BoneOffset( draw.First, /*required*/ true ).PrimitiveIndex( draw.GpuPrimitive );
                       return push;
                   }(),
                   .Instances =
                        instanced
                             ? std::span<const glm::mat4>( draw.Set->Transforms ).subspan( draw.First, draw.Count )
                             : std::span<const glm::mat4>{},
                   .BoundsPad   = draw.BoundsPad,
                   .WorldBounds = draw.WorldBounds } );
        }
        // The cache drawn now lacks the casters still compiling: the next frame's plan sees no key and redraws it.
        if ( maskedPending && layer == ShadowCasterLayer::Static && c < kCascadeViews )
            m_CascadeCache[c].StaticKey.reset();
        // A virtual shadow map level's cached pages lack them the same way: the next frame drops every page
        // (CollectVirtualShadowInvalidation).
        if ( maskedPending && IsVirtualShadowView( c ) )
            m_VsmCastersPending = true;

        // The GPU-generated sections move with the camera (and sway): every frame, over the cache. Cascades only:
        // their caster cells and light cameras are the cascades' (a virtual shadow map level draws none of them).
        // A cached cascade draws its movers from its movable view (kCascadeMovableViewBase + cascade view), the
        // uncached one from the cascade's own: both name the same cascade view.
        if ( drawMovable && c < kCascadeViews )
            BuildGpuInstancedShadowDraws( c, c, list );
        else if ( drawMovable && c >= kCascadeMovableViewBase && c < kCascadeMovableViewBase + kCascadeViews )
            BuildGpuInstancedShadowDraws( c - kCascadeMovableViewBase, c, list );
    }
    RDG::TextureRef MeshRenderer::AcquireCascadeMaps( RDG::Builder& graph )
    {
        if ( m_Shadow.CascadeCount == 0 || !m_CascadeStaticFB[0] )
            return {};
        if ( m_CascadeMapsBuilt )
            return graph.RegisterExternal( m_CascadeMaps, "CSM.Cascades" );

        const RDG::TextureRef created = graph.CreateTexture(
             RDG::TextureDesc{ .Size   = { .Width = m_Shadow.ShadowMapSize, .Height = m_Shadow.ShadowMapSize },
                               .Format = ViewTargetFormats::kShadowDepth,
                               .Layers = m_CascadeSlots * m_Shadow.CascadeCount,
                               .Dim    = RDG::TextureDim::Tex2DArray },
             "CSM.Cascades" );
        // A rebuilt cascade is COPIED from its cache in a later frame (SceneRenderer::AddFrameSunCascades,
        // "MeshShadowCascade{c}.FromCache"), and this frame may make no copy (no caster pipeline, no fitted
        // cascade, the ray-traced sun): the array is created for the copy all the same (UE: the TexCreate flags of
        // the pooled target), or the copy's command meets an image without TRANSFER_DST.
        graph.Extract( created, m_CascadeMaps, RDG::Access::SampledGraphics, { RDG::Access::CopyDst } );
        m_CascadeMapsBuilt = true;
        // Every layer starts at the far plane (1, standard Z: "no occluder"), written once on the frame the array
        // is made. The cascade passes write only the layers fitted this frame, and none when nothing is fitted (an
        // empty scene, no sun): without this the lighting would read, and the extraction hand on, an array no
        // pass ever wrote.
        graph.AddPass(
             "CSM.Init", RDG::PassFlags::Raster,
             [created]( RDG::PassBuilder& pass ) { pass.DepthTarget( created, RDG::LoadOp::ClearDepth( 1.0f ) ); },
             []( RDG::PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
        // A fresh array holds nothing: every cascade fitted this frame is rebuilt into it (its cache copied, its
        // movable casters drawn), and one not fitted now redraws its cache the frame it is.
        for ( uint32_t slot = 0; slot < m_CascadeSlots; ++slot )
        {
            const DirectionalShadowSlot& light  = m_DirectionalShadow[slot];
            const uint32_t               fitted = light.CastShadows ? light.Fitted : 0u;
            for ( uint32_t c = 0; c < m_Shadow.CascadeCount; ++c )
            {
                if ( c < fitted )
                    m_CascadeCache[slot * kMaxCascades + c].Rebuild = true;
                else
                    m_CascadeCache[slot * kMaxCascades + c].StaticKey.reset();
            }
        }
        return created;
    }

    MeshRenderer::ShadowCascadeFrame MeshRenderer::ShadowCascadePasses( const RDG::TextureRef maps )
    {
        // Same guard as the geometry pass: the cascade pass reads m_ShadowPipeline's spec, and
        // SetupShadowPass can now refuse. No caster pipeline means no cascade passes and an unshadowed
        // scene, not a dead editor.
        ShadowCascadeFrame frame;
        if ( !m_ShadowPipeline )
            return frame;

        // Cascade matrices and the cache plan are computed in UpdateCascades() before the graph records: a pass
        // only reads them. Each layer clears to 1 (far): the cascades are STANDARD-Z (SetupShadowPass says why),
        // and a 0 clear under its LessOrEqual test would reject every caster and hand back an empty map, silently.
        // Every held slot's cascades by the same loop; `c` below is the cascade VIEW (slot * kMaxCascades +
        // cascade), its layer of the array CascadeLayer( c ).
        for ( uint32_t c = 0; c < m_CascadeSlots * kMaxCascades; ++c )
        {
            const CascadeCacheState& state = m_CascadeCache[c];
            if ( c % kMaxCascades >= m_Shadow.CascadeCount || !maps.IsValid() || !m_CascadeStaticFB[c] ||
                 !state.Rebuild )
                continue;
            const uint32_t    layer = CascadeLayer( c );
            const std::string name  = std::format( "MeshShadowCascade{}.{}", c / kMaxCascades, c % kMaxCascades );

            if ( state.RedrawStatic )
            {
                SystemRasterPass pass{
                     .Name        = name + ".Static",
                     .ExecuteFunc = [this, c]( RDG::PassContext& context,
                                               const FrameGraphRefs& ) -> Common::BoolResultStr
                     {
                         if ( auto drawn = m_CascadeDraws[c].Record( context ); !drawn.IsSuccess() )
                             return drawn;
                         // The keyed non-mesh casters (the landscape), into the cached layer
                         // their key is part of (PlanCascadeCache).
                         for ( const auto& [weak, firstBlock] : m_CascadeStaticCasters[c] )
                         {
                             const auto caster = weak.lock();
                             if ( !caster )
                             {
                                 continue; // gone since the setup: its draws went with it
                             }
                             if ( auto cast = caster->RecordShadowView(
                                       context, c, firstBlock, m_CascadeCasterVP[c], m_CascadeCasterBias[c] );
                                  !cast.IsSuccess() )
                             {
                                 return cast;
                             }
                         }
                         return BOOLSUCCESS;
                     },
                     .TargetFramebuffer = m_CascadeStaticFB[c],
                     .ClearDepth        = 1.0f };
                pass.Declare = [this, c]( RenderPassDeclaration& declared, const FrameGraphRefs& )
                {
                    // Depth-only casters sample no graph resource: one block per caster material with the
                    // material's own fill, so ValidatePassBindings judges every caster before anything is
                    // recorded.
                    m_CascadeDraws[c].Clear();
                    BuildShadowCascadeDraws( c, m_CascadeDraws[c], ShadowCasterLayer::Static );
                    m_CascadeDraws[c].Declare( declared, std::nullopt );
                    m_CascadeStaticCasters[c].clear();
                    uint32_t nextBlock = m_CascadeDraws[c].BlockCount();
                    for ( const auto& weak : m_CascadeKeyedCasters[c] )
                    {
                        if ( const auto caster = weak.lock() )
                        {
                            const uint32_t declaredBlocks = caster->DeclareShadowView( declared, c );
                            m_CascadeStaticCasters[c].emplace_back( weak, nextBlock );
                            nextBlock += declaredBlocks;
                        }
                    }
                };
                frame.StaticPasses.push_back( std::move( pass ) );
            }

            frame.Copies.push_back( { .From  = m_CascadeStaticFB[c]->GetDepthAttachmentImage(),
                                      .Layer = layer,
                                      .Name  = name + ".FromCache" } );

            if ( !state.Movable )
                continue;
            const uint32_t   view = kCascadeMovableViewBase + c;
            SystemRasterPass pass{
                 .Name        = name + ".Movable",
                 .ExecuteFunc = [this, c]( RDG::PassContext& context,
                                           const FrameGraphRefs& ) -> Common::BoolResultStr
                 {
                     if ( auto drawn = m_CascadeMovableDraws[c].Record( context ); !drawn.IsSuccess() )
                         return drawn;

                     // Casters that are not meshes and keyed nothing (IShadowCaster::CascadeShadowSignature),
                     // recorded into THIS pass so the map holds everyone's depth: the ones the Declare declared,
                     // each through its own blocks.
                     for ( const auto& [weak, firstBlock] : m_CascadeCasters[c] )
                     {
                         const auto caster = weak.lock();
                         if ( !caster )
                         {
                             continue; // gone since the setup: its draws went with it
                         }
                         if ( auto cast = caster->RecordShadowView( context, c, firstBlock, m_CascadeCasterVP[c],
                                                                    m_CascadeCasterBias[c] );
                              !cast.IsSuccess() )
                         {
                             return cast;
                         }
                     }
                     return BOOLSUCCESS;
                 },
                 .TargetDepthArray = maps,
                 .TargetLayer      = layer };
            pass.Declare = [this, c, view]( RenderPassDeclaration& declared, const FrameGraphRefs& )
            {
                m_CascadeMovableDraws[c].Clear();
                m_CascadeCasters[c].clear();
                BuildShadowCascadeDraws( view, m_CascadeMovableDraws[c], ShadowCasterLayer::Movable );
                m_CascadeMovableDraws[c].Declare( declared, std::nullopt );
                // The skin cache's streams its skinned casters fetch (after "SkinCache: Skin" wrote them).
                for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                    declared.Read( streams, RDG::Access::StorageRead );

                // The unkeyed non-mesh casters (PlanCascadeCache) declare their blocks after the list's
                // (IShadowCaster::DeclareShadowView); the keyed ones are in the static layer.
                uint32_t nextBlock = m_CascadeMovableDraws[c].BlockCount();
                for ( const auto& weak : m_CascadeUnkeyedCasters[c] )
                {
                    if ( const auto caster = weak.lock() )
                    {
                        const uint32_t declaredBlocks = caster->DeclareShadowView( declared, c );
                        m_CascadeCasters[c].emplace_back( weak, nextBlock );
                        nextBlock += declaredBlocks;
                    }
                }
            };
            frame.MovablePasses.push_back( std::move( pass ) );
        }
        return frame;
    }

} // namespace Desert::Graphic::System
