// MeshRenderer's light-view passes: the cascaded shadow map (setup, cascade fit, graph pass).
#include "MeshRendererInternal.hpp"

#include <Common/Core/GlslAsCpp.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/Shader.hpp>

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

    // The shadow wind distance's rule, the same text InstanceCull.shader and ClusterCut.shader run
    // (GpuCull_CastsRestPose), compiled as C++ (the shader root is on the Engine include path, premake5.lua).
    using glm::dot;
    using glm::mat4;
    using glm::vec2;
    using glm::vec3;
    using glm::vec4;
    DESERT_GLSL_AS_CPP_BEGIN
#include <Common/ShadowWind.glslh>
    DESERT_GLSL_AS_CPP_END

    // How far, relative to the shadow wind distance, the CPU walk's float rounding of the rule may differ from the
    // GPU cull's (another order of the same multiplies and adds, fused or not): within it the walk cannot know
    // which side the GPU decided, so the instance is keyed as both (MeshRenderer::ShadowCasterRow::NearWindEdge).
    // Float's relative error of a squared distance is ~1e-6; 1e-3 is 1 m at 1 km.
    constexpr float kShadowWindEdge = 1e-3f;

    // THE RING'S SIDE of a gathered swaying instance in a caster view (GPU-FRAME-7: Common/ShadowWind.glslh's
    // ShadowWindRingInner / ShadowWindInsideRing, the text GpuCull_CastsRestPose runs): Rest = inside the inner
    // distance by more than the two sides' rounding may differ (kShadowWindEdge of the ring's split, the size of
    // the terms the inner distance is made of), Edge = within it (the GPU may have taken either side: the CPU walk
    // keys and draws it as a mover, CascadeStaticSignature redraws the static layer each frame it is there),
    // Mover otherwise (and in every view with no ring).
    enum class ShadowWindRingSide : uint8_t
    {
        Mover,
        Edge,
        Rest
    };
    ShadowWindRingSide ShadowWindRingSideOf( const glm::vec3& point, const glm::vec2& casterReach,
                                             const glm::vec4& ring, const glm::vec3& viewOrigin )
    {
        const float inner = ShadowWindRingInner( ring, casterReach );
        const float band  = ring.x * kShadowWindEdge;
        if ( ShadowWindInsideRing( point, viewOrigin, inner - band ) )
            return ShadowWindRingSide::Rest;
        if ( ShadowWindInsideRing( point, viewOrigin, inner + band ) )
            return ShadowWindRingSide::Edge;
        return ShadowWindRingSide::Mover;
    }
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
        // A BUDGET THAT MOVES ONLY THE DISTANCE sizes nothing (the distance-field sun's nearer cascades,
        // ShadowQualityOf: the view's first frame that resolves Distance Field Shadows): the maps, the static
        // caches' targets, the lease and the local rows stay, no drain. The splits move, so the static caches
        // drawn at the old ones are redrawn.
        ShadowQuality sameTargets = budget;
        sameTargets.MaxDistance   = m_Shadow.MaxDistance;
        if ( sameTargets == m_Shadow )
        {
            m_Shadow.MaxDistance = budget.MaxDistance;
            InvalidateCascadeCaches();
            return true;
        }

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
            // CPU-ISM-PACK 5: the same caster in its GpuScene-id permutation (an ISM's casters by id). Refused:
            // the ISMs cast through the packed matrices (said once, here).
            const std::optional<std::string> cellName =
                 DefaultSurfaceShaderName( MeshVertexPath::Instanced, MeshPass::ShadowDepth );
            Common::ResultStr<std::shared_ptr<Shader>> sceneIds =
                 Common::ResultStr<std::shared_ptr<Shader>>::Error( "no default instanced caster cell" );
            if ( cellName )
                sceneIds = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
                     *cellName, SurfaceSceneInstancesVariant() );
            if ( sceneIds )
            {
                m_ShadowInstancedIdShader         = sceneIds.ExtractValue();
                GraphicsPipelineSpecification idspec = ispec;
                idspec.DebugName                  = "ShadowPipelineInstancedSceneIds";
                idspec.Shader                     = m_ShadowInstancedIdShader;
                if ( const auto byId = GraphicsPipeline::Create( idspec ) )
                    m_ShadowInstancedIdPipeline = byId.GetValue();
                else
                    LOG_ERROR( "[MeshRenderer] ISM shadows pack their transforms: {}", byId.GetError() );
            }
            else
                LOG_ERROR( "[MeshRenderer] ISM shadows pack their transforms: {}", sceneIds.GetError() );
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

        // THE SUN'S CASCADES PANCAKE (ComputeShadowCascades; UE: a VSM level's depth clamp, a CSM's
        // bClampToNearPlane): the same three caster states with depth clamp, so a caster nearer the sun than a
        // level's depth range writes the range's sun-side end instead of being clipped, and the range holds only
        // the level's receivers. The local lights keep the clipped states above: their near plane is placed to
        // leave the light's own fixture out (LocalShadowNear), which a clamp would flatten across the whole view.
        const auto pancaked = []( const std::shared_ptr<GraphicsPipeline>& clipped )
        {
            std::shared_ptr<GraphicsPipeline> made;
            if ( !clipped )
                return made;
            GraphicsPipelineSpecification cspec = clipped->GetSpecification();
            cspec.DebugName += " Pancaked";
            cspec.DepthClampEnable = true;
            if ( const auto created = GraphicsPipeline::Create( cspec ) )
            {
                made = created.GetValue();
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] '{}' casts into no sun cascade: {}", cspec.DebugName,
                           created.GetError() );
            }
            return made;
        };
        m_CascadeShadowPipeline          = pancaked( m_ShadowPipeline );
        m_CascadeShadowInstancedPipeline = pancaked( m_ShadowInstancedPipeline );
        m_CascadeShadowInstancedIdPipeline = pancaked( m_ShadowInstancedIdPipeline );
        m_CascadeShadowSkinnedPipeline   = pancaked( m_ShadowSkinnedPipeline );

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
                if ( m_ShadowInstancedIdPipeline && !m_ShadowInstancedIdMaterial[v] )
                    m_ShadowInstancedIdMaterial[v] =
                         std::make_unique<MaterialShadowInstanced>( m_ShadowInstancedIdShader );
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
            state.Scroll       = false;
            state.StripCount   = 0;
            state.MovableRegion.reset();
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

            CascadeFit     fits[kMaxShadowCascades];
            const uint32_t n = ComputeShadowCascades( setup, fits );
            // Written HERE, beside the call whose return value it is, and nowhere else. The fitter can also
            // hand back 0 without an early return of ours — a zero-length light direction, or a MaxDistance
            // that has fallen behind the camera's near plane.
            state.Fitted = n;
            state.Travel = light->Direction;

            for ( uint32_t c = 0; c < n; ++c )
            {
                const uint32_t v                                     = slot * kMaxCascades + c;
                m_CascadeVP[v]                                       = fits[c].ViewProj;
                m_CascadePlacement[v] = CascadePlacement{ .Travel        = light->Direction,
                                                          .WorldPerTexel = fits[c].WorldPerTexel,
                                                          .DepthOrigin   = fits[c].DepthOrigin,
                                                          .TexelOrigin   = fits[c].TexelOrigin };
                state.WorldPerTexel[static_cast<glm::length_t>( c )] = fits[c].WorldPerTexel;
                state.DepthRange[static_cast<glm::length_t>( c )]    = fits[c].DepthRange;
                state.SplitFar[static_cast<glm::length_t>( c )]      = fits[c].SplitFar;
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

    std::array<MeshRenderer::DirectionalCasterSweep, SceneResources::kMaxShadowedDirectionalLights>
    MeshRenderer::DirectionalCasterSweeps() const
    {
        std::array<DirectionalCasterSweep, SceneResources::kMaxShadowedDirectionalLights> sweeps{};
        for ( uint32_t slot = 0; slot < kDirectionalShadowSlots; ++slot )
        {
            const DirectionalShadowSlot& state = m_DirectionalShadow[slot];
            const uint32_t count = slot < m_CascadeSlots ? std::min( state.Fitted, m_Shadow.CascadeCount ) : 0u;
            if ( !state.CastShadows || count == 0u || glm::length( state.Travel ) < 1e-4f )
                continue;
            float reach = 0.0f;
            for ( uint32_t c = 0; c < count; ++c )
                reach = std::max( reach, state.DepthRange[static_cast<glm::length_t>( c )] );
            sweeps[slot] = DirectionalCasterSweep{ .Travel = glm::normalize( state.Travel ), .Reach = reach };
        }
        return sweeps;
    }

    MeshRenderer::CascadeCasterBoxes MeshRenderer::CascadeCasterBoxSet() const
    {
        CascadeCasterBoxes boxes{};
        for ( uint32_t slot = 0; slot < kDirectionalShadowSlots; ++slot )
        {
            const DirectionalShadowSlot& state = m_DirectionalShadow[slot];
            // DirectionalShadows' count: the cascades this slot's layers hold and its receivers sample.
            const uint32_t count = slot < m_CascadeSlots ? std::min( state.Fitted, m_Shadow.CascadeCount ) : 0u;
            if ( !state.CastShadows )
                continue;
            for ( uint32_t c = 0; c < count; ++c )
            {
                const uint32_t view = slot * kMaxCascades + c;
                boxes.Live |= 1u << view;
                boxes.ViewProj[view] = m_CascadeCasterVP[view];
            }
        }
        return boxes;
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
        // The ISM fields' gather is kept while its key and every field's transforms snapshot (by owner) are the
        // ones it was made from: in a still frame no instance is walked again (IsmInstanceReach, the wind point,
        // SelectLODFromBounds per instance). Any input that moved - the main camera, a field's mesh, instances,
        // material, cull distance or wind, the queue's fields - starts the whole gather over, as every frame did.
        const uint64_t ismKey  = IsmGatherKey();
        bool           keepIsm = setup.IsmRevision != 0u && setup.IsmKey == ismKey &&
                       setup.Fields.size() == m_InstancedQueue.size() &&
                       setup.IsmSources.size() == m_InstancedQueue.size();
        for ( std::size_t i = 0; keepIsm && i < m_InstancedQueue.size(); ++i )
        {
            const auto& held = setup.IsmSources[i];
            const auto& now  = m_InstancedQueue[i].Transforms;
            keepIsm          = !held.owner_before( now ) && !now.owner_before( held );
        }
        const uint64_t previousGather = setup.Gather;
        ++setup.Gather;
        setup.Statics.resize( m_StaticQueue.size() );
        setup.Generics.resize( m_GenericQueue.size() );
        setup.Skinned.resize( m_SkinnedQueue.size() );
        setup.Fields.resize( m_InstancedQueue.size() );
        // The static / generic rows are finished once this view's records are synced (FinishStaticCasterRows,
        // from SyncGpuScene): the static casters' revision they are kept by is the GpuScene stream's, known only
        // then. No reader asks them before (ShadowCastersGathered says so).
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
        // Every casting field's instances are gathered here when the gather starts over (GatherIsmCasters, on
        // the setup tasks); a kept gather's fields carry over to this gather (their rows stay in Instances).
        if ( keepIsm )
        {
            for ( ShadowIsmField& field : setup.Fields )
                if ( field.Gather == previousGather )
                    field.Gather = setup.Gather;
            return;
        }
        setup.Instances.clear();
        setup.InstanceIndex.clear();
        setup.InstanceBounds.Clear();
        for ( ShadowIsmField& field : setup.Fields )
            field = ShadowIsmField{};
        setup.IsmKey = ismKey;
        ++setup.IsmRevision;
        setup.IsmSources.resize( m_InstancedQueue.size() );
        for ( std::size_t i = 0; i < m_InstancedQueue.size(); ++i )
            setup.IsmSources[i] = m_InstancedQueue[i].Transforms;
        GatherIsmCasters();
    }

    uint64_t MeshRenderer::IsmGatherKey() const
    {
        // Every input of GatherIsmCasters but the transforms' content (held by owner, GatherShadowCasters) and
        // the kShadowWindEdge constant. The camera's EXACT position and lens: the GPU bins cull and pick levels
        // from them each frame, and a cached layer keyed by a coarser cell would keep an instance that crossed
        // its cull distance, level or wind ring inside the cell (CPU-ISM-SHADOW REMAINDER says what replaces it).
        const auto*             camera = m_SceneRenderer->GetMainCamera();
        const Geometry::LODView lodView =
             camera != nullptr ? Geometry::LODView{ camera->GetPosition(), camera->GetProjectionMatrix() }
                               : Geometry::LODView{};
        SignatureHash hash;
        hash.Add( camera != nullptr );
        if ( camera != nullptr )
        {
            hash.Add( camera->GetPosition() );
            hash.Add( camera->GetProjectionMatrix() );
        }
        hash.Add( DrawDistanceScale( lodView ) );
        hash.Add( m_ShadowWindDistance );
        hash.Add( m_InstancedQueue.size() );
        for ( const auto& ism : m_InstancedQueue )
        {
            hash.Add( ism.Mesh != nullptr ? ism.Mesh->GetRenderRevision() : uint64_t{ 0 } );
            hash.Add( ism.Transforms ? ism.Transforms->size() : std::size_t{ 0 } );
            hash.Add( static_cast<const void*>( ism.Material.get() ) );
            hash.Add( ism.CastShadows );
            hash.Add( ism.CullDistance.Min );
            hash.Add( ism.CullDistance.Max );
            hash.Add( ism.Wind.Direction );
            hash.Add( ism.Wind.Strength );
            hash.Add( ism.Wind.Speed );
            hash.Add( ism.Wind.Height );
        }
        return hash.Value;
    }

    bool MeshRenderer::ShadowCastersGathered() const
    {
        const ShadowCasterSetup& setup = m_ShadowCasterSetup;
        return setup.StaticRowsGather == setup.Gather && setup.Statics.size() == m_StaticQueue.size() &&
               setup.Generics.size() == m_GenericQueue.size() && setup.Skinned.size() == m_SkinnedQueue.size() &&
               setup.Fields.size() == m_InstancedQueue.size();
    }

    bool MeshRenderer::StaticCastersTracked() const
    {
        return m_StaticCasters.Revision != 0u && m_StaticCasters.Gather == m_ShadowCasterSetup.Gather &&
               m_ShadowCasterSetup.StaticRowsRevision == m_StaticCasters.Revision;
    }

    uint64_t MeshRenderer::StaticLodKey() const
    {
        const auto*   camera = m_SceneRenderer->GetMainCamera();
        SignatureHash hash;
        hash.Add( m_LODEnabled );
        hash.Add( camera != nullptr );
        if ( camera != nullptr )
        {
            hash.Add( camera->GetPosition() );
            hash.Add( camera->GetProjectionMatrix() );
        }
        return hash.Value;
    }

    void MeshRenderer::FinishStaticCasterRows()
    {
        DESERT_PROFILE_SCOPE( "Shadow: Static caster rows" );
        ShadowCasterSetup&       setup  = m_ShadowCasterSetup;
        const StaticCasterTrack& track  = m_StaticCasters;
        const uint64_t           lodKey = StaticLodKey();
        // Kept: the revision they were made for is this gather's (TrackStaticCasters ran for it). The queues'
        // sizes are part of the revision (its order word), so a kept row is the same entry's.
        const bool keep =
             track.Revision != 0u && track.Gather == setup.Gather && setup.StaticRowsRevision == track.Revision &&
             setup.Statics.size() == m_StaticQueue.size() && setup.Generics.size() == m_GenericQueue.size();
        if ( keep )
        {
            // The boxes hold; a level is asked again (lazily, ShadowStaticLod) only when the camera or the LOD
            // toggle moved.
            if ( setup.StaticLodKey != lodKey )
            {
                for ( ShadowCasterRow& row : setup.Statics )
                    row.LodLevel = kShadowLodUnknown;
                for ( ShadowCasterRow& row : setup.Generics )
                    row.LodLevel = kShadowLodUnknown;
            }
        }
        else
        {
            setup.Statics.resize( m_StaticQueue.size() );
            setup.Generics.resize( m_GenericQueue.size() );
            // The box is the one IsVisibleInView would build per view from the same transform and local bounds,
            // built once; an empty local box stays empty (no extent: in every view).
            const auto worldBox = []( const glm::mat4& transform, const Common::Math::AABB& local )
            { return Geometry::IsEmpty( local ) ? local : Geometry::TransformBounds( transform, local ); };
            // The dense snapshot's boxes (m_StaticSnapshot), every entry with a mesh; casters' rows copy theirs.
            m_StaticSnapshot.WorldBounds.resize( m_StaticQueue.size() );
            m_StaticSnapshot.GenericBounds.resize( m_GenericQueue.size() );
            // The same boxes as columns, in entry order (CPU-SIMD 4): refilled only here, with the revision.
            m_StaticSnapshot.WorldLanes.Clear();
            setup.StaticBounds.Clear();
            for ( std::size_t i = 0; i < m_StaticQueue.size(); ++i )
            {
                const StaticMeshRenderData& rd = m_StaticQueue[i];
                setup.Statics[i]               = ShadowCasterRow{};
                m_StaticSnapshot.WorldBounds[i] = Common::Math::AABB{};
                if ( rd.Mesh != nullptr )
                {
                    m_StaticSnapshot.WorldBounds[i] =
                         worldBox( rd.Transform, Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) );
                    if ( rd.CastShadows )
                        setup.Statics[i].WorldBounds = m_StaticSnapshot.WorldBounds[i];
                }
                m_StaticSnapshot.WorldLanes.PushBack( m_StaticSnapshot.WorldBounds[i] );
                setup.StaticBounds.PushBack( setup.Statics[i].WorldBounds );
            }
            for ( std::size_t i = 0; i < m_GenericQueue.size(); ++i )
            {
                const GenericMeshRenderData& g = m_GenericQueue[i];
                setup.Generics[i]              = ShadowCasterRow{};
                m_StaticSnapshot.GenericBounds[i] = Common::Math::AABB{};
                if ( g.Mesh == nullptr )
                    continue;
                m_StaticSnapshot.GenericBounds[i] =
                     worldBox( g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) );
                if ( g.CastShadows )
                    setup.Generics[i].WorldBounds = m_StaticSnapshot.GenericBounds[i];
            }
        }
        // Revision 0 (untracked: no stream read for this gather) never keeps them next time.
        setup.StaticRowsRevision = track.Gather == setup.Gather ? track.Revision : 0u;
        setup.StaticLodKey       = lodKey;
        setup.StaticRowsGather   = setup.Gather;
        // Kept with the rows (the same revision, the same queue sizes): of this gather either way.
        m_StaticSnapshot.Gather = setup.Gather;
    }

    bool MeshRenderer::StaticEntryInView( const Core::Frustum& frustum, const std::size_t index ) const
    {
        if ( m_StaticSnapshot.Gather == m_ShadowCasterSetup.Gather &&
             m_StaticSnapshot.WorldBounds.size() == m_StaticQueue.size() )
            return IsVisibleInView( frustum, m_StaticSnapshot.WorldBounds[index] );
        const StaticMeshRenderData& data = m_StaticQueue[index];
        return IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) );
    }

    bool MeshRenderer::StaticLanesCurrent() const noexcept
    {
        return m_StaticSnapshot.Gather == m_ShadowCasterSetup.Gather &&
               m_StaticSnapshot.WorldBounds.size() == m_StaticQueue.size() &&
               m_StaticSnapshot.WorldLanes.Size() == m_StaticQueue.size();
    }

    uint32_t MeshRenderer::StaticEntriesInView4( const Core::Frustum& frustum,
                                                 const std::size_t    base ) const noexcept
    {
        return VisibleInView4( frustum, m_StaticSnapshot.WorldLanes, base );
    }

    bool MeshRenderer::GenericEntryInView( const Core::Frustum& frustum, const std::size_t index ) const
    {
        if ( m_StaticSnapshot.Gather == m_ShadowCasterSetup.Gather &&
             m_StaticSnapshot.GenericBounds.size() == m_GenericQueue.size() )
            return IsVisibleInView( frustum, m_StaticSnapshot.GenericBounds[index] );
        const GenericMeshRenderData& g = m_GenericQueue[index];
        return IsVisibleInView( frustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) );
    }

    Common::Math::AABB MeshRenderer::StaticEntryWorldBounds( const std::size_t index ) const
    {
        // An empty snapshot box is an empty local box: TransformBounds of it, as the glass sort always read.
        if ( m_StaticSnapshot.Gather == m_ShadowCasterSetup.Gather &&
             m_StaticSnapshot.WorldBounds.size() == m_StaticQueue.size() &&
             !Geometry::IsEmpty( m_StaticSnapshot.WorldBounds[index] ) )
            return m_StaticSnapshot.WorldBounds[index];
        const StaticMeshRenderData& data = m_StaticQueue[index];
        return Geometry::TransformBounds( data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) );
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

    float MeshRenderer::ShadowWindDistanceOf( const uint32_t view ) const
    {
        // A cascade's movable layer is the cascade's: the same receivers, the same answer.
        uint32_t cascadeView = view;
        if ( cascadeView >= kCascadeMovableViewBase && cascadeView < kCascadeMovableViewBase + kCascadeViews )
            cascadeView -= kCascadeMovableViewBase;
        if ( cascadeView >= kCascadeViews )
            return m_ShadowWindDistance;
        const uint32_t               cascade = cascadeView % kMaxCascades;
        const DirectionalShadowSlot& slot    = m_DirectionalShadow[cascadeView / kMaxCascades];
        // Cascades 0 and 1 serve the receivers nearest the camera; a cascade not fitted this frame draws nothing.
        if ( cascade < 2u || cascade >= slot.Fitted )
            return m_ShadowWindDistance;
        if ( m_ShadowWindDistance <= slot.SplitFar[static_cast<glm::length_t>( cascade - 2u )] )
            return 0.0f;
        return m_ShadowWindDistance;
    }

    float MeshRenderer::ShadowWindInnerOf( const uint32_t view, const float reach ) const
    {
        // A section's instance point is its root, its every point within @p reach of it: spread = height = reach.
        const float rooted = std::max( reach, 0.0f );
        return ShadowWindRingInner( ShadowWindRingOf( view ), glm::vec2( rooted, rooted ) );
    }

    glm::vec4 MeshRenderer::ShadowWindRingOf( const uint32_t view ) const
    {
        // The cascade's receivers as ShadowWindDistanceOf reads them (a movable layer is its cascade's).
        uint32_t cascadeView = view;
        if ( cascadeView >= kCascadeMovableViewBase && cascadeView < kCascadeMovableViewBase + kCascadeViews )
            cascadeView -= kCascadeMovableViewBase;
        if ( cascadeView >= kCascadeViews )
            return glm::vec4( 0.0f );
        const uint32_t               cascade = cascadeView % kMaxCascades;
        const DirectionalShadowSlot& slot    = m_DirectionalShadow[cascadeView / kMaxCascades];
        if ( cascade < 2u || cascade >= slot.Fitted )
            return glm::vec4( 0.0f );
        // The light's fall per unit across (Travel points along the light): a point h above the ground under it
        // shadows the ground h x across / down away; no fall, no bound on how far a shadow runs.
        const float down   = -slot.Travel.y;
        const float across = glm::length( glm::vec2( slot.Travel.x, slot.Travel.z ) );
        if ( !( down > 0.0f ) )
            return glm::vec4( 0.0f );
        return glm::vec4( slot.SplitFar[static_cast<glm::length_t>( cascade - 2u )], across / down, 0.0f, 0.0f );
    }

    glm::vec4 MeshRenderer::CasterWindOrigin( const uint32_t view ) const
    {
        // The origin ShadowIsmInstances measures its Sways from (its LODView: the main camera, none = the
        // world's).
        const auto* camera = m_SceneRenderer->GetMainCamera();
        glm::vec3   origin( 0.0f );
        if ( camera != nullptr )
            origin = camera->GetPosition();
        return glm::vec4( origin, ShadowWindDistanceOf( view ) );
    }

    const MeshRenderer::ShadowIsmField& MeshRenderer::ShadowIsmInstances( const uint32_t field ) const
    {
        return m_ShadowCasterSetup.Fields[field];
    }

    void MeshRenderer::GatherIsmCasters()
    {
        ShadowCasterSetup& setup = m_ShadowCasterSetup;
        // THE LOD AND THE CULL DISTANCE ARE THE MAIN CAMERA'S, as the geometry pass measures them: an instance
        // faded out of the view casts no shadow either, and a caster drawn at a coarser level than the object the
        // camera sees casts a silhouette that does not match it (CollectIsmInstances' rule, asked once here).
        const auto*       camera = m_SceneRenderer->GetMainCamera();
        Geometry::LODView lodView{};
        if ( camera != nullptr )
            lodView = Geometry::LODView{ camera->GetPosition(), camera->GetProjectionMatrix() };
        // The runs of every field a caster view asks (each one's filter: a mesh, CastShadows, instances), in field
        // order and, within a field, in index order.
        std::size_t runs = 0;
        for ( std::size_t f = 0; f < m_InstancedQueue.size(); ++f )
        {
            const InstancedMeshRenderData& ism = m_InstancedQueue[f];
            if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                continue;
            const auto count = static_cast<uint32_t>( ism.Transforms->size() );
            for ( uint32_t begin = 0; begin < count; begin += kIsmGatherRun )
            {
                if ( runs == m_IsmGatherRuns.size() )
                    m_IsmGatherRuns.emplace_back();
                IsmGatherRun& run = m_IsmGatherRuns[runs++];
                run.Field         = static_cast<uint32_t>( f );
                run.Begin         = begin;
                run.End           = count - begin > kIsmGatherRun ? begin + kIsmGatherRun : count;
            }
        }
        if ( runs != 0 )
        {
            // About WorkerCount() + 1 contiguous slices of runs (a run costs at most kIsmGatherRun instances),
            // each one setup task; the joining thread runs one too (the ISM view culls' split,
            // BuildStaticDraws).
            DESERT_PROFILE_SCOPE( "Mesh: ISM shadow gather" );
            const std::size_t   participants = ::Common::JobSystem::Get().WorkerCount() + 1;
            const std::size_t   slices       = std::min( runs, participants );
            ::Common::TaskGroup tasks( m_SceneRenderer->ParallelSetup() );
            for ( std::size_t s = 0; s < slices; ++s )
            {
                const std::size_t first = runs * s / slices;
                const std::size_t last  = runs * ( s + 1 ) / slices;
                tasks.Launch( "Mesh: ISM shadow gather run",
                              [this, &lodView, first, last]() noexcept
                              {
                                  for ( std::size_t r = first; r < last; ++r )
                                      GatherIsmRun( m_IsmGatherRuns[r], lodView );
                              } );
            }
            tasks.Join();
        }
        // The runs appended in field and run order: the rows, their indices and their boxes' columns, as the one
        // serial walk pushed them.
        std::size_t r = 0;
        for ( std::size_t f = 0; f < m_InstancedQueue.size(); ++f )
        {
            ShadowIsmField& field = setup.Fields[f];
            field = ShadowIsmField{ .Gather = setup.Gather,
                                    .First  = static_cast<uint32_t>( setup.Instances.size() ) };
            for ( ; r < runs && m_IsmGatherRuns[r].Field == f; ++r )
            {
                const IsmGatherRun& run = m_IsmGatherRuns[r];
                setup.Instances.insert( setup.Instances.end(), run.Rows.begin(), run.Rows.end() );
                setup.InstanceIndex.insert( setup.InstanceIndex.end(), run.Index.begin(), run.Index.end() );
                for ( const ShadowCasterRow& row : run.Rows )
                    setup.InstanceBounds.PushBack( row.WorldBounds );
            }
            field.Count = static_cast<uint32_t>( setup.Instances.size() ) - field.First;
        }
    }

    void MeshRenderer::GatherIsmRun( IsmGatherRun& run, const Geometry::LODView& lodView ) const noexcept
    {
        run.Rows.clear();
        run.Index.clear();
        const InstancedMeshRenderData& ism         = m_InstancedQueue[run.Field];
        const Common::Math::AABB       localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
        const float                    lensScale   = DrawDistanceScale( lodView );
        const InstanceCullDistance     lensCull    = ScaledByLens( ism.CullDistance, lensScale );
        const std::vector<glm::mat4>&  transforms  = *ism.Transforms;
        // The run's levels eight per register (SelectLODsFromBounds: per instance SelectLODFromBounds bit for
        // bit), the culled instances' too: one pass over the run's transforms in place costs less than copying
        // the kept.
        const std::span<const glm::mat4> runTransforms( transforms.data() + run.Begin, run.End - run.Begin );
        run.Levels.resize( runTransforms.size() );
        Geometry::SelectLODsFromBounds( runTransforms, localBounds, lodView, ism.Mesh->GetLODScreenSizes(), -1, 0,
                                        run.Levels );

        // WHO SWAYS IN THE SHADOWS (UE WPO Disable Distance;
        // r.Shadow.Virtual.Cache.MaxMaterialPositionInvalidationRange):
        // a swaying instance nearer the main camera than Shadows.WindAnimationDistance is a mover; a farther one
        // casts its rest pose with the still casters and invalidates no page or view, so the wind costs what moves
        // where it is seen moving. Common/ShadowWind.glslh's one rule, the GPU cull's: the instance's bounds
        // centre in the world (GpuScenePrimitive.LocalBoundsCenter is the same (Min + Max) * 0.5) against the main
        // camera's position. An instance at the distance within the rounding the two sides may differ by
        // (kShadowWindEdge) sways for every CPU view and is NearWindEdge for the cascade key: whichever side the
        // GPU took is drawn.
        const bool      fieldSways   = ism.Wind.Sways();
        const glm::vec3 boundsCentre = ( localBounds.Min + localBounds.Max ) * 0.5f;
        const float     inner        = m_ShadowWindDistance * ( 1.0f - kShadowWindEdge );
        const float     outer        = m_ShadowWindDistance * ( 1.0f + kShadowWindEdge );
        // THE CASTER'S REACH (GPU-FRAME-7): a swaying instance's point and reach, from the same bounds and pad as
        // its GpuScene row (LocalBoundsExtent, LocalBoundsCenter.w = WindBoundsPad), for the per-cascade ring.
        const glm::vec3 boundsExtent = ( localBounds.Max - localBounds.Min ) * 0.5f;
        const float     windPad      = WindBoundsPad( ism.Wind );
        for ( uint32_t k = run.Begin; k < run.End; ++k )
        {
            const std::optional<Common::Math::AABB> reach =
                 IsmInstanceReach( lensCull, k, transforms[k], localBounds, ism.Wind, lodView.Origin );
            if ( !reach )
                continue;
            const glm::vec3 point    = ShadowWindInstancePoint( transforms[k], boundsCentre );
            const bool      nearEdge = fieldSways && ShadowWindCastsRestPose( point, lodView.Origin, inner ) &&
                                  !ShadowWindCastsRestPose( point, lodView.Origin, outer );
            glm::vec2 casterReach( 0.0f );
            if ( fieldSways )
                casterReach = ShadowWindCasterReach( transforms[k], boundsExtent, windPad );
            run.Rows.push_back(
                 { .WorldBounds  = *reach,
                   .LodLevel     = run.Levels[k - run.Begin],
                   .Sways        = fieldSways && ( nearEdge || !ShadowWindCastsRestPose( point, lodView.Origin,
                                                                                         m_ShadowWindDistance ) ),
                   .NearWindEdge = nearEdge,
                   .WindPoint    = point,
                   .CasterReach  = casterReach } );
            run.Index.push_back( k );
        }
    }

    std::size_t MeshRenderer::ShadowBucketKeyHash::operator()( const ShadowBucketKey& key ) const noexcept
    {
        std::size_t h = std::hash<const void*>{}( key.Mesh );
        h ^= std::hash<uint64_t>{}( key.Hidden.Low ) + 0x9E3779B97F4A7C15ull + ( h << 6u ) + ( h >> 2u );
        h ^= std::hash<const void*>{}( key.Hidden.High.data() ) + 0x9E3779B97F4A7C15ull + ( h << 6u ) +
             ( h >> 2u );
        return h ^ ( key.Hidden.High.size() * 0x9E3779B97F4A7C15ull );
    }

    uint64_t MeshRenderer::CascadeStaticSignature( const uint32_t cascade, const glm::mat4& casterVP,
                                                   const bool withLod, CascadeMovers& movers, bool& windSplit )
    {
        // THE CASTER BOX, as BuildShadowCascadeDraws culls with: a static caster outside it is not in the cache,
        // so it moving changes nothing drawn.
        // Casters gathered for other queues: no key can be trusted, the layer is redrawn (and its draws refused,
        // BuildShadowCascadeDraws says why).
        if ( !ShadowCastersGathered() )
        {
            movers.Unkeyed = "casters gathered for other mesh queues";
            return 0;
        }
        // Open on the sun's side, as that walk's (CascadeCasterFrustum: a sun cascade's casters draw pancaked).
        const Core::Frustum frustum = CascadeCasterFrustum( casterVP );
        // The gathered boxes (GatherShadowCasters), the ones BuildShadowCascadeDraws culls with.
        const auto reaches = [&frustum]( const ShadowCasterRow& row )
        { return IsVisibleInView( frustum, row.WorldBounds ); };
        SignatureHash hash;
        // THE STATIC / GENERIC PART (SCENE-CACHE-4), kept per cascade while the static casters' revision (every
        // field hashed below reaches it through the GpuScene stream, StaticCasterTrack says how), the rows' LOD
        // key (the levels ShadowStaticLod answers) and the cascade's caster matrix (which boxes reach it) are the
        // ones it was made from: in a still frame no static caster is walked here. An untracked gather (no
        // revision for it) makes the part every frame and never keeps it.
        if ( m_CascadeStaticParts.size() <= cascade )
            m_CascadeStaticParts.resize( static_cast<std::size_t>( cascade ) + 1u );
        CascadeStaticPart& still   = m_CascadeStaticParts[cascade];
        const bool         tracked = StaticCastersTracked();
        if ( !tracked || !still.Valid || still.Revision != m_StaticCasters.Revision ||
             still.LodKey != m_ShadowCasterSetup.StaticLodKey || still.CasterVP != casterVP ||
             still.WithLod != withLod )
        {
            SignatureHash stillHash;
            // The statics from the walk columns (this view's sync), the world as its primitive's stamp
            // (PlacementOf); the record is read only for ForcedLOD, LODBias and SectionSlot of a caster that
            // reaches the cascade (and its transform when its primitive has no stamp).
            const StaticWalkColumns& columns = m_StaticColumns;
            const std::size_t        synced  = std::min( m_StaticQueue.size(), columns.Count() );
            for ( std::size_t i = 0; i < synced; ++i )
            {
                const Desert::Mesh* const mesh = columns.Meshes[i];
                if ( mesh == nullptr || ( columns.Flags[i] & StaticWalkColumns::kCastsShadow ) == 0u ||
                     !reaches( m_ShadowCasterSetup.Statics[i] ) )
                    continue;
                const StaticMeshRenderData& rd = m_StaticQueue[i];
                stillHash.Add( mesh->GetRenderRevision() );
                if ( columns.Placements[i] != 0u )
                    stillHash.Add( columns.Placements[i] );
                else
                    stillHash.Add( rd.Transform );
                stillHash.Add( columns.Hidden[i] );
                stillHash.Add( rd.ForcedLOD );
                stillHash.Add( rd.LODBias );
                // The level the layer draws it at follows the main camera (ShadowStaticLod): a new one redraws.
                if ( withLod )
                    stillHash.Add( ShadowStaticLod( static_cast<uint32_t>( i ) ) );
                // The entry's section material decides the caster cell (opaque or masked) and a masked one's clip
                // (ShadingOf's answer, the column the casters draw with).
                stillHash.Add( rd.SectionSlot );
                stillHash.Add( columns.Surfaces[i] );
                stillHash.Add( columns.Instances[i] );
            }
            for ( std::size_t i = 0; i < m_GenericQueue.size(); ++i )
            {
                const GenericMeshRenderData& g = m_GenericQueue[i];
                if ( g.Mesh == nullptr || !g.CastShadows || !reaches( m_ShadowCasterSetup.Generics[i] ) )
                    continue;
                stillHash.Add( g.Mesh->GetRenderRevision() );
                const uint64_t placement = PlacementOf( g.GpuPrimitive ); // the world's stamp, as the statics
                if ( placement != 0u )
                    stillHash.Add( placement );
                else
                    stillHash.Add( g.Transform );
                if ( withLod )
                    stillHash.Add( ShadowGenericLod( static_cast<uint32_t>( i ) ) );
            }
            still.Valid    = tracked;
            still.Revision = m_StaticCasters.Revision;
            still.LodKey   = m_ShadowCasterSetup.StaticLodKey;
            still.CasterVP = casterVP;
            still.WithLod  = withLod;
            still.Hash     = stillHash.Value;
        }
        hash.Add( still.Hash );
        // A still instanced mesh is keyed by its snapshot (m_CascadeKeySnapshots says why that is sound) and by
        // the instances the layer draws of it: the ones the main camera's cull distance keeps in the box, each
        // with its level (the gather's, as BuildShadowCascadeDraws walks them), so an instance crossing the cull
        // distance or a level redraws the layer instead of leaving its old depth cached. A swaying field is split
        // per instance (the gather's Sways; the GPU bins' GpuCull_CastsRestPose): an instance past the shadow
        // wind distance casts its rest pose into the static layer and is keyed like a still one, so one crossing
        // the distance either way redraws the layer; a nearer one makes the cascade movable only when it reaches
        // it (UE: a WPO primitive invalidates the dynamic layer of the views its bounds touch, not of every view):
        // a crown beyond the cascade's box draws nothing into it, so it must not force its movable layer.
        // A cascade the wind does not reach (ShadowWindDistanceOf 0: no receiver near the camera samples it) holds
        // every instance in its static layer, each keyed like a still one, and is made movable by none; the
        // distance keys the layer and the kept ISM part, so a cascade the wind starts or stops reaching redraws it.
        const float cascadeWind = ShadowWindDistanceOf( cascade );
        hash.Add( cascadeWind );
        // THE CASTER'S REACH (GPU-FRAME-7, the ring GpuCull_CastsRestPose and the GPU-instanced sections read): a
        // swaying instance nearer the camera than its ring's inner distance casts its rest pose into the static
        // layer and is keyed like a still one, so the ring keys the layer and the kept ISM part too.
        const glm::vec4 cascadeRing = ShadowWindRingOf( cascade );
        const glm::vec3 ringOrigin( CasterWindOrigin( cascade ) );
        hash.Add( cascadeRing );
        // THE ISM FIELDS' PART (SCENE-CACHE-3), kept per cascade while the gather it walks (IsmRevision: the
        // gather was kept, GatherShadowCasters) and the cascade's caster matrix are the ones it was made from: in a
        // still frame no instance of any field is walked again here. A part that met an instance at the wind edge
        // keys this frame's gather, so it is made again on the next frame.
        if ( m_CascadeIsmParts.size() <= cascade )
            m_CascadeIsmParts.resize( static_cast<std::size_t>( cascade ) + 1u );
        CascadeIsmPart& part = m_CascadeIsmParts[cascade];
        if ( !part.Valid || part.Revision != m_ShadowCasterSetup.IsmRevision ||
             part.CasterVP != casterVP || part.WithLod != withLod || part.Wind != cascadeWind ||
             part.Ring != cascadeRing )
        {
            SignatureHash ismHash;
            bool          ismMovable = false;
            bool          nearEdge   = false;
            bool          ismSplit   = false;
            // The fields' headers in queue order, each field's rows in runs of kIsmGatherRun on the setup tasks
            // (SignIsmRun: the walk this loop made per row), their hashes added in field and run order.
            std::size_t runs = 0;
            for ( const auto& ism : m_InstancedQueue )
            {
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                const auto            field = static_cast<uint32_t>( &ism - m_InstancedQueue.data() );
                const ShadowIsmField& kept  = ShadowIsmInstances( field );
                for ( uint32_t first = 0; first < kept.Count; first += kIsmGatherRun )
                {
                    if ( runs == m_IsmSignatureRuns.size() )
                        m_IsmSignatureRuns.emplace_back();
                    IsmSignatureRun& run = m_IsmSignatureRuns[runs++];
                    run.Field            = field;
                    run.First            = kept.First + first;
                    run.Count            = std::min( kIsmGatherRun, kept.Count - first );
                }
            }
            const IsmSignatureView signView{ .CasterFrustum = &frustum,
                                             .Wind          = cascadeWind,
                                             .Ring          = cascadeRing,
                                             .RingOrigin    = ringOrigin,
                                             .WithLod       = withLod };
            if ( runs != 0 )
            {
                const std::size_t   participants = ::Common::JobSystem::Get().WorkerCount() + 1;
                const std::size_t   slices       = std::min( runs, participants );
                ::Common::TaskGroup tasks( m_SceneRenderer->ParallelSetup() );
                for ( std::size_t s = 0; s < slices; ++s )
                {
                    const std::size_t first = runs * s / slices;
                    const std::size_t last  = runs * ( s + 1 ) / slices;
                    tasks.Launch( "Mesh: ISM cascade signature run",
                                  [this, &signView, first, last]() noexcept
                                  {
                                      for ( std::size_t r = first; r < last; ++r )
                                          SignIsmRun( m_IsmSignatureRuns[r], signView );
                                  } );
                }
                tasks.Join();
            }
            std::size_t r = 0;
            for ( const auto& ism : m_InstancedQueue )
            {
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                const auto field = static_cast<uint32_t>( &ism - m_InstancedQueue.data() );
                ismHash.Add( ism.Mesh->GetRenderRevision() );
                ismHash.Add( ism.Transforms.get() );
                ismHash.Add( ism.Transforms->size() );
                ismHash.Add( ism.Material.get() );
                for ( ; r < runs && m_IsmSignatureRuns[r].Field == field; ++r )
                {
                    const IsmSignatureRun& run = m_IsmSignatureRuns[r];
                    ismHash.Add( run.Hash );
                    ismMovable = ismMovable || run.Movable;
                    nearEdge   = nearEdge || run.NearEdge;
                    ismSplit   = ismSplit || run.Split;
                }
            }
            part.Valid    = !nearEdge;
            part.Revision = m_ShadowCasterSetup.IsmRevision;
            part.CasterVP  = casterVP;
            part.WithLod   = withLod;
            part.WindSplit = ismSplit;
            part.Wind     = cascadeWind;
            part.Ring     = cascadeRing;
            part.Hash     = ismHash.Value;
            part.Movable  = ismMovable;
        }
        hash.Add( part.Hash );
        if ( part.Movable )
            movers.Unkeyed = "swaying instances (wind time)";
        if ( part.WindSplit )
            windSplit = true;
        for ( std::size_t i = 0; i < m_SkinnedQueue.size(); ++i )
        {
            const SkinnedMeshRenderData& sd = m_SkinnedQueue[i];
            if ( sd.Mesh == nullptr || !sd.CastShadows || sd.BoneMatrices.empty() ||
                 !reaches( m_ShadowCasterSetup.Skinned[i] ) )
                continue;
            // A skinned caster is keyed by everything its depth is drawn from (BuildShadowCascadeDraws' skinned
            // walk, the movable bins): its entry, its meshes (a displaced or morphed twin and the base it skins,
            // each with its content revision), its sections' materials and masks, its transform, its bones and its
            // morph blend (MorphTwin: a pending blend keys apart from the blended one).
            movers.Keyed = true;
            movers.Casters.Add( i );
            movers.Casters.Add( sd.Entity );
            movers.Casters.Add( sd.GpuPrimitive );
            movers.Casters.Add( sd.Mesh );
            movers.Casters.Add( sd.Mesh->GetRenderRevision() );
            movers.Casters.Add( sd.SkinSource );
            if ( sd.SkinSource != nullptr )
                movers.Casters.Add( sd.SkinSource->GetRenderRevision() );
            movers.Casters.Add( sd.Transform );
            movers.Casters.Add( sd.Sections.size() );
            for ( const SkinnedSection& section : sd.Sections )
            {
                movers.Casters.Add( section.Material );
                movers.Casters.Add( section.Instance );
                movers.Casters.Add( section.HiddenSubmeshes );
            }
            movers.Pose.Add( sd.BoneMatrices.size() );
            movers.Pose.Bytes( sd.BoneMatrices.data(), sd.BoneMatrices.size() * sizeof( glm::mat4 ) );
            if ( const auto twin = m_MorphTwins.find( sd.Entity ); twin != m_MorphTwins.end() )
            {
                movers.Pose.Add( twin->second.Twin.get() );
                movers.Pose.Add( twin->second.Blended );
                movers.Pose.Add( twin->second.Revision );
                movers.Pose.Add( twin->second.Pending );
                movers.Pose.Add( twin->second.PendingRevision );
            }
            const Common::Math::AABB& box = m_ShadowCasterSetup.Skinned[i].WorldBounds;
            if ( movers.Bounds )
                movers.Bounds = Common::Math::AABB{ glm::min( movers.Bounds->Min, box.Min ),
                                                    glm::max( movers.Bounds->Max, box.Max ) };
            else
                movers.Bounds = box;
        }
        // A GPU-instanced section (BuildGpuInstancedShadowDraws): one whose instances move by themselves makes
        // every cascade it reaches movable. A still one (the grass, GpuInstancedRenderData::Still) is keyed by its
        // generation, and split as a swaying ISM field is (WHO SWAYS IN THE SHADOWS, here on the GPU: the vertex
        // stage's CasterWindLayer.glslh): its instances past the shadow wind distance cast their rest pose into
        // the static layer, so a swaying section also keys the rule's measure (an instance crossing the distance
        // moves between the layers), and its movers make the cascade movable only when the box they can reach -
        // the distance around the camera, grown by an instance's reach - meets the cascade.
        const glm::vec4 windOrigin = CasterWindOrigin( cascade );
        for ( const auto& gpu : m_GpuInstancedQueue )
        {
            if ( !gpu.CastShadows || ( gpu.WorldBounds && !frustum.Intersects( *gpu.WorldBounds ) ) )
                continue;
            if ( !gpu.Still )
            {
                movers.Unkeyed = "instances the GPU moves";
                continue;
            }
            hash.Add( gpu.Key );
            hash.Add( gpu.Instances.get() );
            hash.Add( gpu.Still->Generation );
            hash.Add( gpu.Material.get() );
            hash.Add( gpu.HiddenSubmeshes );
            // A blade's shadow LOD and thinning in this cascade (BladeShadowSet: the texel, the bias, the view's
            // pixel): another set or another thinning draws other blades.
            const BladeCasterLod shadow = BladeShadowSet( gpu, cascade );
            hash.Add( shadow.Set );
            hash.Add( shadow.ThinFrom );
            hash.Add( gpu.ShadowLod.StartCull );
            // ...and the eye it thins from (ThinOrigin: the field's eye, which moves only from tile to tile).
            hash.Add( gpu.ShadowLod.ThinOrigin );
            const bool sways = gpu.Wind.Sways();
            hash.Add( sways );
            if ( !sways )
                continue;
            windSplit = true;
            hash.Add( windOrigin );
            if ( !( windOrigin.w > 0.0f ) )
                continue; // every instance casts its rest pose into the static layer: no mover
            // THE CASTER'S REACH (ShadowWindInnerOf): the instances nearer than the ring's inner distance cast
            // their rest pose into the static layer too (the vertex stage's split reads the same value), so the
            // distance keys the layer, and a ring past the wind distance holds no mover.
            const float inner = ShadowWindInnerOf( cascade, gpu.Still->Reach );
            hash.Add( inner );
            if ( inner >= windOrigin.w )
                continue;
            const glm::vec3 moverReach( windOrigin.w + gpu.Still->Reach );
            const glm::vec3 origin( windOrigin );
            if ( frustum.Intersects( Common::Math::AABB{ origin - moverReach, origin + moverReach } ) )
                movers.Unkeyed = "swaying GPU instances (wind time)";
        }
        if ( m_GpuInstancedCastersExpected ) // submitted later this frame (ExpectGpuInstancedCasters)
            movers.Unkeyed = "particle mesh casters (submitted after the plan)";
        return hash.Value;
    }

    void MeshRenderer::SignIsmRun( IsmSignatureRun& run, const IsmSignatureView& view ) const noexcept
    {
        // CascadeStaticSignature's per-row walk over Instances[First, First + Count): `reaches` four boxes per
        // register (VisibleInViewBits: the same verdict per row as IsVisibleInView on its WorldBounds).
        const ShadowCasterSetup& setup = m_ShadowCasterSetup;
        SignatureHash            hash;
        run.Movable  = false;
        run.NearEdge = false;
        run.Split    = false;
        run.Bits.resize( ( std::size_t{ run.Count } + 63u ) / 64u );
        VisibleInViewBits( *view.CasterFrustum, setup.InstanceBounds, run.First, run.Count, run.Bits );
        for ( uint32_t k = run.First; k < run.First + run.Count; ++k )
        {
            const ShadowCasterRow& row = setup.Instances[k];
            if ( !VisibleBit( run.Bits, k - run.First ) )
                continue;
            // One at the distance within the two sides' rounding (NearWindEdge): the GPU bins may have put it
            // in either layer, so the movable layer is drawn and the static one keyed by this gather, redrawn
            // each frame until it leaves the edge.
            if ( row.NearWindEdge && view.Wind > 0.0f )
            {
                run.Split = true;
                hash.Add( setup.Gather );
                run.NearEdge = true;
            }
            if ( row.Sways && view.Wind > 0.0f )
            {
                run.Split = true;
                const ShadowWindRingSide side =
                     ShadowWindRingSideOf( row.WindPoint, row.CasterReach, view.Ring, view.RingOrigin );
                if ( side == ShadowWindRingSide::Edge )
                {
                    hash.Add( setup.Gather );
                    run.NearEdge = true;
                }
                if ( side != ShadowWindRingSide::Rest )
                {
                    run.Movable = true;
                    continue;
                }
            }
            hash.Add( setup.InstanceIndex[k] );
            if ( view.WithLod )
                hash.Add( row.LodLevel );
        }
        run.Hash = hash.Value;
    }

    void MeshRenderer::PlanCascadeCache()
    {
        // UE's cached shadow maps for a directional light's cascades: the static casters' depth is kept per
        // cascade and drawn again only when the cascade's map moved other than by whole texels (the sun turned, a
        // depth quantum was crossed, ComputeShadowCascades) or one of the static casters it reaches changed; a map
        // that moved by whole texels SCROLLS (UE's CSM scrolling): the cache is shifted and only the strips the
        // shift uncovered are drawn. The movable casters are drawn over a copy of it every frame they exist.
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
                // The non-mesh casters (the landscape): a keyed one is drawn into the static layer and its key is
                // the layer's (a sculpted height, a streamed displacement mip, a moved camera's LOD or dice redraw
                // it); an unkeyed one makes the cascade movable. Asked of THIS frame's draws: the plan runs after
                // TerrainRenderer::PrepareLocalShadowViews. A keyed one's content (CascadeShadowContent: its key less
                // the camera's detail) is what the content key below holds of it.
                bool          unkeyedCaster = false;
                SignatureHash casterKey;
                bool          contentKnown = StaticCastersTracked();
                for ( const auto& weak : m_ShadowCasters )
                {
                    const auto caster = weak.lock();
                    if ( !caster )
                        continue;
                    if ( const std::optional<uint64_t> cast = caster->CascadeShadowSignature( v ) )
                    {
                        casterKey.Add( *cast );
                        m_CascadeKeyedCasters[v].push_back( weak );
                    }
                    else
                    {
                        unkeyedCaster = true;
                        m_CascadeUnkeyedCasters[v].push_back( weak );
                    }
                }
                EnsureShadowViewMaterials( kCascadeMovableViewBase + v );

                // THE CONTENT KEY (UE's cached CSM: a cached cascade is invalidated by its static casters
                // changing, not by the camera): which static casters the box of a caster matrix holds, what they
                // are and how they are biased - without the levels of detail the camera chose for them, a cache
                // keeps the level it was drawn at. It lets a cascade whose map moved by whole texels SCROLL: the
                // cached depth is shifted and only the strips the shift uncovered are drawn (CascadeScrollStrips),
                // once the box the cache was drawn with still holds what it held. Kept while every static caster
                // is tracked (StaticCastersTracked), every keyed caster tells its content apart and no caster in
                // the box is split by the wind ring around the camera; otherwise the full key below decides, as it
                // did. The keyed casters' part is asked OF THE BOX (IShadowCaster::CascadeShadowContent): the
                // landscape in the box the cache was drawn with, not this frame's draws, so a shifted map whose
                // old box holds the same tiles scrolls; nullopt (a caster that cannot tell its content apart) keys
                // by the full key.
                const auto contentKey = [&]( const glm::mat4& casterVP, CascadeMovers& reached,
                                             bool& windSplit ) -> std::optional<uint64_t>
                {
                    SignatureHash content;
                    content.Add( CascadeStaticSignature( v, casterVP, false, reached, windSplit ) );
                    for ( const auto& weak : m_CascadeKeyedCasters[v] )
                    {
                        const auto caster = weak.lock();
                        if ( !caster )
                            continue;
                        const std::optional<uint64_t> held = caster->CascadeShadowContent( v, casterVP );
                        if ( !held )
                            return std::nullopt;
                        content.Add( *held );
                    }
                    content.Add( m_CascadeCasterBias[v] );
                    return content.Value;
                };
                const CascadePlacement& placement    = m_CascadePlacement[v];
                const bool              samePlace    = state.Placement && state.Placement->SameMapAs( placement );
                bool                    keyIsContent = contentKnown;
                bool                    redraw       = true;
                uint64_t                newKey       = 0;
                state.Scroll                         = false;
                state.StripCount                     = 0;
                CascadeMovers movers;
                if ( keyIsContent )
                {
                    glm::ivec2 shift( 0 );
                    if ( samePlace )
                        shift = state.Placement->ShiftTo( placement );
                    // The cached box is asked FIRST: its static part is the one the last frame kept (SCENE-CACHE-4);
                    // this frame's box is asked after it and kept for the next.
                    bool cacheHolds = false;
                    if ( samePlace && shift != glm::ivec2( 0 ) && state.KeyIsContent && state.StaticKey &&
                         CascadeScrollOverlaps( shift, m_Shadow.ShadowMapSize ) )
                    {
                        CascadeMovers                 cachedMovers;
                        bool                          cachedSplit   = false;
                        const std::optional<uint64_t> cached =
                             contentKey( state.CasterVP, cachedMovers, cachedSplit );
                        cacheHolds = cached && *cached == *state.StaticKey && !cachedSplit;
                    }
                    bool                          windSplit = false;
                    const std::optional<uint64_t> content = contentKey( m_CascadeCasterVP[v], movers, windSplit );
                    if ( content )
                        newKey = *content;
                    if ( windSplit || !content )
                        keyIsContent = false;
                    else if ( samePlace && shift == glm::ivec2( 0 ) )
                        redraw = !state.KeyIsContent || state.StaticKey != newKey;
                    else if ( cacheHolds )
                    {
                        redraw           = false;
                        state.Scroll     = true;
                        state.Shift      = shift;
                        state.StripCount = CascadeScrollStrips(
                             shift, m_Shadow.ShadowMapSize, m_CascadeCasterVP[v], state.Strips, state.StripRects );
                    }
                }
                if ( !keyIsContent )
                {
                    bool          unusedSplit = false;
                    SignatureHash key;
                    movers = CascadeMovers{}; // the full key's walk: its movers, not the content key's again
                    key.Add( CascadeStaticSignature( v, m_CascadeCasterVP[v], true, movers, unusedSplit ) );
                    key.Add( casterKey.Value );
                    key.Add( m_CascadeCasterVP[v] );
                    key.Add( m_CascadeCasterBias[v] );
                    newKey = key.Value;
                    redraw = state.KeyIsContent || state.StaticKey != newKey;
                }

                if ( unkeyedCaster )
                    movers.Unkeyed = "a non-mesh caster with no key";
                state.Movable      = movers.Any();
                state.RedrawStatic = redraw || state.Scroll;
                state.StaticKey    = newKey;
                state.KeyIsContent = keyIsContent;
                state.Placement    = placement;
                state.CasterVP     = m_CascadeCasterVP[v];

                // THE MOVABLE LAYER (SHADOW-CACHE-MOTION; UE's VSM: a cached page is invalidated only by a
                // primitive that updates). Keyed movers only (CascadeMovers): their casters and poses, the caster
                // matrix and the bias the movable pass draws with. Their box in framebuffer texels (x right, y
                // down, as CascadeScrollStrips): its corners through the caster matrix, grown two texels.
                state.MovableKey.reset();
                state.MovableRect.reset();
                if ( movers.Keyed && movers.Unkeyed == nullptr )
                {
                    SignatureHash movableKey;
                    movableKey.Add( movers.Casters.Value );
                    movableKey.Add( movers.Pose.Value );
                    movableKey.Add( m_CascadeCasterVP[v] );
                    movableKey.Add( m_CascadeCasterBias[v] );
                    state.MovableKey = movableKey.Value;
                    if ( movers.Bounds )
                    {
                        const float side = static_cast<float>( m_Shadow.ShadowMapSize );
                        glm::vec2   low( side );
                        glm::vec2   high( 0.0f );
                        for ( uint32_t corner = 0; corner < 8u; ++corner )
                        {
                            const glm::vec3 pick( static_cast<float>( corner & 1u ),
                                                  static_cast<float>( ( corner >> 1u ) & 1u ),
                                                  static_cast<float>( ( corner >> 2u ) & 1u ) );
                            const glm::vec3 span  = movers.Bounds->Max - movers.Bounds->Min;
                            const glm::vec3 point = movers.Bounds->Min + span * pick;
                            const glm::vec4 clip  = m_CascadeCasterVP[v] * glm::vec4( point, 1.0f );
                            const glm::vec2 texel( ( clip.x / clip.w + 1.0f ) * 0.5f * side,
                                                   ( 1.0f - clip.y / clip.w ) * 0.5f * side );
                            low  = glm::min( low, texel );
                            high = glm::max( high, texel );
                        }
                        low  = glm::clamp( glm::floor( low ) - 2.0f, glm::vec2( 0.0f ), glm::vec2( side ) );
                        high = glm::clamp( glm::ceil( high ) + 2.0f, glm::vec2( 0.0f ), glm::vec2( side ) );
                        const glm::vec2 extent = glm::max( high - low, glm::vec2( 0.0f ) );
                        state.MovableRect      = glm::uvec4( glm::uvec2( low ), glm::uvec2( extent ) );
                    }
                }
                // Equal keys over a standing static layer: the map is last frame's, no restore, no movable draw.
                const bool movableHeld = state.MapHasMovable && state.Movable && state.MovableKey &&
                                         state.MapMovableKey == state.MovableKey;
                const bool hadMovable  = state.MapHasMovable;
                state.Rebuild = state.RedrawStatic || ( ( state.Movable || state.MapHasMovable ) && !movableHeld );
                // A rebuild over a standing static layer restores and draws only the union of the texels the
                // map's movers covered and this frame's cover, when both are known (an unkeyed mover, or a map
                // whose movers had no box: the whole layer).
                state.MovableRegion.reset();
                if ( state.Rebuild && !state.RedrawStatic &&
                     ( !state.MapHasMovable || state.MapMovableRect.has_value() ) &&
                     ( !state.Movable || state.MovableRect.has_value() ) )
                {
                    glm::uvec2 low( m_Shadow.ShadowMapSize );
                    glm::uvec2 high( 0u );
                    const auto cover = [&low, &high]( const glm::uvec4& rect )
                    {
                        low  = glm::min( low, glm::uvec2( rect.x, rect.y ) );
                        high = glm::max( high, glm::uvec2( rect.x + rect.z, rect.y + rect.w ) );
                    };
                    if ( state.MapHasMovable )
                        cover( *state.MapMovableRect );
                    if ( state.Movable )
                        cover( *state.MovableRect );
                    if ( high.x > low.x && high.y > low.y )
                        state.MovableRegion = glm::uvec4( low, high - low );
                }
                if ( state.Rebuild )
                {
                    state.MapHasMovable  = state.Movable;
                    state.MapMovableKey  = state.MovableKey;
                    state.MapMovableRect = state.MovableRect;
                }
                // One line a change of why the movable layer is drawn or kept.
                std::string reason;
                if ( movableHeld )
                    reason = "holds: no restore, no movable draw";
                else if ( state.Movable && movers.Unkeyed != nullptr )
                    reason = std::format( "unkeyed: {}", movers.Unkeyed );
                else if ( state.Movable && state.RedrawStatic )
                    reason = "static layer redrawn";
                else if ( state.Movable && !hadMovable )
                    reason = "movers arrived";
                else if ( state.Movable )
                    reason = "pose, transform or caster matrix";
                else if ( hadMovable )
                    reason = "movers left";
                if ( reason != state.MovableLogged )
                {
                    if ( !reason.empty() )
                    {
                        std::string restored = "none";
                        if ( state.MovableRegion )
                            restored =
                                 std::format( "{}x{} texels", state.MovableRegion->z, state.MovableRegion->w );
                        else if ( state.Rebuild )
                            restored = "whole layer";
                        LOG_INFO( "[Shadow] cascade {}.{} movable key changed ({}); restore {}", v / kMaxCascades,
                                  v % kMaxCascades, reason, restored );
                    }
                    state.MovableLogged = reason;
                }
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
        if ( executor == nullptr || executor->GetShader() == nullptr ||
             executor->GetShader()->GetVariant().IsDefault() )
            return CellPipeline( passState, cell.GetShaderName() );
        return CellPipeline( passState, cell.GetShaderName(), executor->GetShader() );
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
        if ( MissInSetupTask( "a cell pipeline" ) )
            return nullptr;
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
        // and they leave the driver at different moments. By identity (CellKey), so no name is built per draw.
        if ( own != nullptr )
            return ChoosePipelineDraw( own, standIn, {} );
        return ChoosePipelineDraw( nullptr, standIn, { passState, &cell, twoSided } );
    }

    std::string MeshRenderer::CellLogName( const GraphicsPipeline* own, const RefusedCell& refused )
    {
        if ( own != nullptr )
            return own->GetSpecification().DebugName;
        const char* const pass =
             refused.PassState != nullptr ? refused.PassState->GetSpecification().DebugName.c_str() : "?";
        const char* const cull = refused.TwoSided ? "_TwoSided" : "";
        if ( refused.Cell == nullptr )
            return std::format( "{} ?{}", pass, cull );
        return std::format( "{} {}{}", pass, refused.Cell->GetShaderName(), cull );
    }

    MaterialPipelineTracker::CellDraw MeshRenderer::ChoosePipelineDraw( const GraphicsPipeline* own,
                                                                        const GraphicsPipeline* standIn,
                                                                        const RefusedCell&      refused )
    {
        // The program by identity too (CellKey: serial x code generation), so an address reused after a free or
        // a hot reload is a new row and never a stale one.
        MaterialPipelineTracker::CellKey key;
        const Shader*                    program = nullptr;
        if ( own != nullptr )
        {
            key.Pipeline = own;
            program      = own->GetShader().get();
        }
        else
        {
            key = { nullptr, refused.PassState, refused.Cell, refused.TwoSided };
            const MaterialExecutor* const executor =
                 refused.Cell != nullptr ? refused.Cell->GetMaterialExecutor() : nullptr;
            if ( executor != nullptr )
                program = executor->GetShader().get();
        }
        if ( program != nullptr )
        {
            key.ShaderSerial   = program->GetSerial();
            key.CodeGeneration = program->GetCodeGeneration();
        }
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
        if ( MissInSetupTask( "the pipeline tracker" ) ) // every call writes the tracker: serial only
            return MaterialPipelineTracker::CellDraw::Nothing;
        const auto before       = m_MaterialPipelines.StateOf( key );
        const auto choice       = m_MaterialPipelines.ChooseCell( key, reports, standInReady );
        if ( choice.Draw != MaterialPipelineTracker::CellDraw::Own )
            ++m_StandInDraws;
        else if ( before && *before != MaterialPipelineState::Ready )
            LOG_INFO( "[MeshRenderer] '{}' draws its own pipeline now (it was {})", CellLogName( own, refused ),
                      MaterialPipelineStateName( *before ) );
        if ( choice.Announce )
            LOG_INFO( "[MeshRenderer] '{}' draws the default surface until its pipeline is ready (pipeline {}{})",
                      CellLogName( own, refused ), MaterialPipelineStateName( reports ),
                      choice.Draw == MaterialPipelineTracker::CellDraw::Nothing
                           ? "; the default surface is not ready either, so it is not drawn yet"
                           : "" );
        return choice.Draw;
    }

    MeshRenderer::StandInCell* MeshRenderer::StandIn( const MeshVertexPath path, const MeshPass pass )
    {
        const uint32_t key  = ( static_cast<uint32_t>( path ) * 16u ) + static_cast<uint32_t>( pass );
        if ( const auto found = m_StandIns.find( key ); found != m_StandIns.end() )
            return found->second.Material ? &found->second : nullptr;
        if ( MissInSetupTask( "a default-surface stand-in" ) )
            return nullptr;
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
        // A sun cascade's two layers draw pancaked (SetupShadowPass); the rain occlusion and the local views clip.
        if ( view < kVsmViewBase )
            return { m_CascadeShadowPipeline.get(), m_CascadeShadowInstancedPipeline.get(),
                     m_CascadeShadowSkinnedPipeline.get(), MeshPass::ShadowDepth,
                     m_CascadeShadowInstancedIdPipeline.get() };
        return { m_ShadowPipeline.get(), m_ShadowInstancedPipeline.get(), m_ShadowSkinnedPipeline.get(),
                 MeshPass::ShadowDepth, m_ShadowInstancedIdPipeline.get() };
    }

    GraphicsPipeline* MeshRenderer::MaskedCasterPipeline( const DataDrivenMaterial& caster, MeshVertexPath path,
                                                          const CasterPipelines& casters )
    {
        // GPU-COST-30 (UE FShaderParameterBindings: a draw binds what ITS compiled permutation declares): the
        // pipeline is built over the program the cell material was made from. A permutation of the cell
        // (ShaderService::AcquireVariant: the instance-id list's GpuInstanceIds at binding 18, the forward VSM
        // define) has a layout of its own that the registered default program of the same name lacks, so the
        // name alone is not the key: name # variant hash, as ShaderService keys a variant.
        const auto& name    = caster.GetShaderName();
        const auto& program = caster.GetMaterialExecutor()->GetShader();
        if ( !program )
        {
            LOG_ERROR( "[Shadows] masked caster '{}' will not draw: its material holds no program", name );
            return nullptr;
        }
        // Per shared state too: a sun cascade's caster is pancaked, a local view's clips (CasterPipelinesOf).
        // By identity (MaskedCasterKey): one program object per permutation, so its serial names name # variant.
        const MaskedCasterKey key{ program->GetSerial(), casters.Static };
        if ( const auto found = m_MaskedCasterPipelines.find( key ); found != m_MaskedCasterPipelines.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_MaskedCasterPipelines[key];
        // Its name (the log and the pipeline's DebugName), built once: on the miss that builds it.
        const std::string label = std::format( "{}#{:016x}@{}", name,
                                               static_cast<unsigned long long>( program->GetVariant().Hash() ),
                                               static_cast<const void*>( casters.Static ) );

        // The shared caster's state for this path and pass (vertex layout, standard-Z depth, the cascade target or
        // the virtual map's render area) — only the program differs, which is the whole difference between an
        // opaque and a masked caster. The cell's name names its pass, its variant hash the permutation.
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
        if ( shared == nullptr )
        {
            LOG_ERROR( "[Shadows] masked caster '{}' will not draw: the path has no shared caster pipeline",
                       label );
            return nullptr;
        }
        GraphicsPipelineSpecification spec = shared->GetSpecification();
        spec.DebugName                     = std::format( "ShadowPipelineMasked {}", label );
        spec.Shader                        = program;
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
        // The shadow wind rule's measure in this view (ShadowWindDistanceOf: 0 in a cascade no receiver near the
        // camera samples), the one every caster material of the view and the instance split below read.
        const glm::vec4 viewWind = CasterWindOrigin( c );
        // Shadow vert computes Projection*View*Transform; feed the combined cascade matrix as
        // Projection and identity as View, matching LightViewProjection[c] on the lit side.
        m_ShadowMaterial[c]->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                             m_CascadeCasterBias[c], m_ParallaxQuality, viewWind );
        const MaterialExecutor* casterExecutor = m_ShadowMaterial[c]->GetMaterialExecutor();
        // The pipelines of the view's caster cells: the cascades' and local views' depth attachment, or the
        // virtual shadow map's attachment-less pool raster (IsVirtualShadowView).
        const CasterPipelines casters = CasterPipelinesOf( c );

        // Shadow casters are MATERIAL-INDEPENDENT (depth only), so batch purely by Mesh*: any
        // group of >= 2 identical meshes collapses into ONE instanced draw per cascade. This is
        // the dominant cost in the 256-mesh stress test (256x4 per-object draws -> 4 draws).
        const bool instancingOn = casters.Instanced != nullptr && m_ShadowInstancedMaterial[c];
        // CPU-ISM-PACK 6 (UE: a shadow view draws its instances from GPU Scene by its own id list): this view's
        // ISM casters name their GpuScene rows (FirstGpuInstance + index, 4 B each) instead of packing matrices,
        // once the id caster's pipeline is built. A virtual shadow map level has none (its GPU cull reads the
        // draws' matrices); a pipeline still compiling or refused leaves the ISMs on the packed matrices.
        const bool ismIds = instancingOn && casters.InstancedSceneIds != nullptr &&
                            m_ShadowInstancedIdMaterial[c] && m_FrameSceneInstances.IsValid() &&
                            casters.InstancedSceneIds->GetReadiness() == PipelineReadiness::Ready;

        // THE CASCADE'S OWN MATRIX, NOT THE CAMERA'S — and that distinction is the whole
        // safety of culling a shadow pass at all. An object behind the camera casts into
        // the frame it is not itself in, so the camera's frustum would buy draw calls with
        // missing shadows and the draw-call detector would report it as a win.
        //
        // WHY THIS ONE CHANGES NOTHING ON SCREEN. It is derived from `m_CascadeCasterVP[c]`, the
        // exact matrix the caster vertex shader multiplies by (SetLightMatrix above passes
        // it as Projection with an identity View, and so does this). The cascade projection
        // is a FINITE orthographic box across the light — `glm::orthoRH_ZO(-radius, radius,
        // -radius, radius, ...)` in ShadowCascades.hpp — so a caster outside it sideways is
        // already clipped by the rasterizer; along the light only the far side clips, the
        // sun's side is pancaked (below). This skips exactly the geometry the GPU was going to
        // throw away, which is why it is provable rather than plausible.
        //
        // The plane extraction is convention-agnostic here and that is not luck: the
        // cascades are deliberately STANDARD-Z while the camera is reversed-Z, which swaps
        // which of the two derived planes is "near" and which is "far" — the SET of six
        // half-spaces is the same one either way, and Intersects tests all six by value.
        Core::Frustum cascadeFrustum( m_CascadeCasterVP[c], glm::mat4( 1.0f ) );
        // A sun cascade's two layers and a virtual shadow map level keep the casters on the sun's side of their
        // depth range: they draw with depth clamp and land on its sun-side end (SetupShadowPass's pancaked
        // states, EnsureVirtualShadowPipelines), so the cull opens that side (Graphic::CascadeCasterFrustum).
        // A local light's mip is a perspective view from the light: its near-plane casters are clamped and
        // dropped by the raster (Common/VirtualShadowRaster.glslh), nothing lies beyond its far plane.
        if ( c < kRainOcclusionView )
            cascadeFrustum = CascadeCasterFrustum( m_CascadeCasterVP[c] );

        // THE LOD OF A CASTER IS ASKED FROM THE CAMERA, not from the light, and that is deliberate rather than
        // convenient: a caster drawn into the cascade at a coarser level than the object the camera sees casts a
        // silhouette that does not match the object it belongs to. Asked once per gather (ShadowStaticLod,
        // ShadowGenericLod, ShadowIsmInstances), the same level in every view; the box every view culls a caster
        // by is the gathered one (GatherShadowCasters).
        const ShadowCasterSetup& gathered = m_ShadowCasterSetup;
        // A SCROLLED CASCADE'S STATIC LAYER (PlanCascadeCache) draws what reaches the strips its shift uncovered: the
        // rest of the layer is its cache, copied in shifted.
        // CPU-FRAME-ARENA 4: the strips in place (at most CascadeCache::Strips' two), no vector per view.
        std::array<Core::Frustum, 2> strips{};
        std::size_t                  stripCount = 0;
        if ( layer == ShadowCasterLayer::Static && c < kCascadeViews && m_CascadeCache[c].Scroll )
            for ( uint32_t s = 0; s < m_CascadeCache[c].StripCount && stripCount < strips.size(); ++s )
                strips[stripCount++] = CascadeCasterFrustum( m_CascadeCache[c].Strips[s] );
        const auto inView = [&cascadeFrustum, &strips, stripCount]( const Common::Math::AABB& box )
        {
            if ( !IsVisibleInView( cascadeFrustum, box ) )
                return false;
            if ( stripCount == 0 )
                return true;
            for ( std::size_t s = 0; s < stripCount; ++s )
                if ( IsVisibleInView( strips[s], box ) )
                    return true;
            return false;
        };
        // Whether gathered instance @p k sways in this view: the shadow wind distance's rule (the gather's Sways),
        // in every caster view - a sun cascade's layers (CascadeStaticSignature keys it, the GPU bins split by
        // GpuCull_CastsRestPose, the same function), a VSM level, a local light's view and the rain occlusion; none
        // in a cascade the wind does not reach (ShadowWindDistanceOf 0), whose static layer holds the rest poses;
        // nor nearer the camera than the view's ring lets its sway reach a receiver of it (ShadowWindRingOf: the
        // rule CascadeStaticSignature keys and GpuCull_CastsRestPose splits the GPU bins by; one at the ring's edge
        // sways, as the key draws it).
        const bool      viewSways     = viewWind.w > 0.0f;
        const glm::vec4 viewRing      = ShadowWindRingOf( c );
        const glm::vec3 viewOrigin    = glm::vec3( viewWind );
        const auto      instanceSways = [&gathered, viewSways, viewRing, viewOrigin]( const uint32_t k )
        {
            const auto& row = gathered.Instances[k];
            return viewSways && row.Sways &&
                   ShadowWindRingSideOf( row.WindPoint, row.CasterReach, viewRing, viewOrigin ) !=
                        ShadowWindRingSide::Rest;
        };

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
            set.Ids.clear();
            set.SceneIds = false;
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
        // raster), a sun cascade's static or movable layer (its bins and their cluster raster: a VG primitive is
        // walked for every submesh the raster does not draw - a refused cut or carrier leaves it whole to the
        // walk), or a view with neither (a local light): everything. Empty: the walk draws none of the record.
        const ClusterWalkView walkView = IsVirtualShadowView( c ) ? VirtualShadowWalkView()
                                         : binnedLayer ? BinnedWalkView( GpuDriven::MeshPass::ShadowCascade, c )
                                                       : UnbinnedWalkView( GpuDriven::MeshPass::ShadowCascade );
        const auto            walkHidden = [&]( const Desert::Mesh*             mesh, const std::size_t /*submeshes*/,
                                     const GpuDriven::PrimitiveIndex primitive,
                                     const uint64_t                  hidden ) -> std::optional<SubmeshMask>
        { return CpuWalkHidden( walkView, mesh, primitive, hidden ); };
        // The queue's hot columns (m_StaticColumns, dense by position: mesh, primitive, mask, caster flag and the
        // section's surface with its masked answer), so a record is read only once it casts in this view. A
        // record past the columns was queued after the sync: skipped.
        const StaticWalkColumns& columns = m_StaticColumns;
        const std::size_t        statics = drawStatic ? columns.Count() : 0u;
        // inView four rows at a time (CPU-SIMD 4; UE's primitive-bounds batch): the group of a record that
        // reaches the test is tested once over the rows' columns (StaticBounds, kept with the rows), the view's
        // bits AND the union of the strips' - per row the verdict inView gives its box. Rows whose columns are
        // not of these rows (another count, or rows not finished for this gather) are tested one at a time.
        const Common::Math::AABBLanes& staticLanes = gathered.StaticBounds;
        const bool                     staticBatch = gathered.StaticRowsGather == gathered.Gather &&
                                 staticLanes.Size() == gathered.Statics.size();
        const auto                     staticGroup = [&]( const std::size_t base ) noexcept
        {
            uint32_t bits = VisibleInView4( cascadeFrustum, staticLanes, base );
            if ( stripCount == 0 || bits == 0u )
                return bits;
            uint32_t stripBits = 0u;
            for ( std::size_t s = 0; s < stripCount; ++s )
                stripBits |= VisibleInView4( strips[s], staticLanes, base );
            return bits & stripBits;
        };
        VisibleInViewQuads staticQuads;
        for ( std::size_t i = 0; i < statics; ++i )
        {
            const Desert::Mesh* const mesh  = columns.Meshes[i];
            const uint8_t             flags = columns.Flags[i];
            if ( mesh == nullptr || ( flags & StaticWalkColumns::kCastsShadow ) == 0u )
                continue;
            // Asked before the CPU frustum test, so a record the bins hold costs this loop no culling work.
            const std::optional<SubmeshMask> walk =
                 walkHidden( mesh, mesh->GetSubmeshes().size(), columns.Primitives[i], columns.Hidden[i] );
            if ( !walk )
                continue;
            const bool caster = staticBatch && i < staticLanes.Size() ? staticQuads.Visible( i, staticGroup )
                                                                      : inView( gathered.Statics[i].WorldBounds );
            if ( !caster )
                continue;
            const StaticMeshRenderData& rd     = m_StaticQueue[i];
            const SubmeshMask           hidden = *walk;
            // The caster takes the material the LIT draw shows, by the same mapping: one entry per section
            // material (Rules::SplitMeshSlotDraws), shaded by SectionSurface, casting only its own submeshes - so
            // a masked section takes the masked route and an opaque one the opaque route. The columns hold that
            // surface and its IsMaskedSurface answer (ShadingOf at the sync), so no template name is asked here.
            MaterialInstance*         inst = columns.Instances[i];
            const DataDrivenMaterial* mat  = columns.Surfaces[i];
            if ( ( flags & StaticWalkColumns::kMasked ) == 0u )
            {
                bucketFor( rd.Mesh, hidden ).push_back( &rd );
                continue;
            }
            auto* masked = MaskedCasterMaterial( mat, MeshVertexPath::Static, c );
            if ( masked == nullptr )
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
            auto&            set = maskedSetFor( masked );
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
        auto& idBatches      = m_ScratchShadowIdBatches;
        auto& shadowIsmIds   = m_ScratchShadowIsmIds;
        instTransforms.clear();
        batches.clear();
        singles.clear();
        idBatches.clear();
        shadowIsmIds.clear();
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
                // A still field draws into the still layer only; a swaying one may draw into both (its instances
                // past the shadow wind distance cast their rest pose with the still casters: the gather's Sways).
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() ||
                     !( ism.Wind.Sways() && viewSways ? drawMovable || drawStatic : drawStatic ) )
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

                // Per-instance, against this cascade. A cascade covers a square around the camera,
                // so a forest spread over the map has most of its instances outside every
                // one of the four — and the batch is a single draw whose cost is entirely
                // its instance count. The instances the main camera's cull distance keeps, with the box the
                // wind can reach and the level, are the gather's (ShadowIsmInstances: asked once per field).
                const ShadowIsmField& field =
                     ShadowIsmInstances( static_cast<uint32_t>( &ism - m_InstancedQueue.data() ) );
                // inView for every instance of the field at once, four boxes per register (VisibleInViewBits:
                // per instance the verdict inView gives its gathered box): the view's bits, AND the union of the
                // scrolled strips' when there are strips.
                const std::size_t fieldWords = ( std::size_t{ field.Count } + 63u ) / 64u;
                m_ScratchCasterBits.resize( fieldWords );
                VisibleInViewBits( cascadeFrustum, gathered.InstanceBounds, field.First, field.Count,
                                   m_ScratchCasterBits );
                if ( stripCount != 0 )
                {
                    m_ScratchStripBits.assign( fieldWords, 0u );
                    for ( std::size_t stripIndex = 0; stripIndex < stripCount; ++stripIndex )
                        AddVisibleInViewBits( strips[stripIndex], gathered.InstanceBounds, field.First,
                                              field.Count, m_ScratchStripBits );
                    for ( std::size_t w = 0; w < fieldWords; ++w )
                        m_ScratchCasterBits[w] &= m_ScratchStripBits[w];
                }
                // The field's movers and its still-standing instances are two draws: the wind is pushed per draw.
                for ( const bool swaying : { false, true } )
                {
                    if ( swaying ? !ism.Wind.Sways() || !viewSways || !drawMovable : !drawStatic )
                        continue;
                    const InstanceWind wind = swaying ? ism.Wind : InstanceWind{};
                    auto& visible = m_ScratchIsmVisible;
                    auto& levels  = m_ScratchLodLevels;
                    auto&              visibleIds = m_ScratchIsmVisibleIds;
                    visible.clear();
                    levels.clear();
                    visibleIds.clear();
                    for ( uint32_t k = field.First; k < field.First + field.Count; ++k )
                    {
                        if ( instanceSways( k ) != swaying || !VisibleBit( m_ScratchCasterBits, k - field.First ) )
                            continue;
                        visible.push_back( ( *ism.Transforms )[gathered.InstanceIndex[k]] );
                        levels.push_back( gathered.Instances[k].LodLevel );
                        // Its GpuScene row (MeshRenderer.hpp InstancedDraw::FirstGpuInstance: field instance i is
                        // row FirstGpuInstance + i), the id this view's id casters read it by.
                        if ( ismIds )
                            visibleIds.push_back( ism.FirstGpuInstance + gathered.InstanceIndex[k] );
                    }
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
                        // By id: the template's (Instanced x ShadowDepth) cell in SurfaceSceneInstancesVariant, on
                        // this view's own copy, once its pipeline is built; else the packed-matrix caster.
                        DataDrivenMaterial* caster = nullptr;
                        bool                byId   = false;
                        if ( ismIds )
                        {
                            DataDrivenMaterial* const idCaster =
                                 Runtime::ResourceRegistry::GetMaterialService()->GetViewVariant(
                                      ismMat, MeshVertexPath::Instanced, casters.Pass, c,
                                      SurfaceSceneInstancesVariant(), nullptr );
                            GraphicsPipeline* idPipeline = nullptr;
                            if ( idCaster != nullptr )
                                idPipeline = MaskedCasterPipeline( *idCaster, MeshVertexPath::Instanced, casters );
                            byId = idPipeline != nullptr && idPipeline->GetReadiness() == PipelineReadiness::Ready;
                            if ( byId )
                                caster = idCaster;
                        }
                        if ( !byId )
                            caster = MaskedCasterMaterial( ismMat, MeshVertexPath::Instanced, c );
                        if ( caster == nullptr )
                        {
                            continue; // refused by name, once
                        }
                        const std::optional<uint32_t> row = FindMaterialRow( ismMat, ismInst );
                        if ( !row )
                        {
                            continue;
                        }
                        auto& set    = maskedSetFor( caster );
                        set.SceneIds = byId;
                        for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                        {
                            const std::size_t setSize = byId ? set.Ids.size() : set.Transforms.size();
                            const auto        first   = static_cast<uint32_t>( setSize );
                            for ( std::size_t i = 0; i < visible.size(); ++i )
                            {
                                if ( levels[i] != level )
                                    continue;
                                if ( byId )
                                    set.Ids.push_back( visibleIds[i] );
                                else
                                    set.Transforms.push_back( visible[i] );
                            }
                            const std::size_t setEnd = byId ? set.Ids.size() : set.Transforms.size();
                            MaskedCasterDraw draw;
                            draw.Set      = &set;
                            draw.Path     = MeshVertexPath::Instanced;
                            draw.Mesh     = ism.Mesh;
                            draw.HiddenSubmeshMask = ismHidden;
                            draw.Instance = ismInst;
                            draw.Row      = *row;
                            draw.Count             = static_cast<uint32_t>( setEnd ) - first;
                            draw.First    = first;
                            draw.LodLevel = level;
                            draw.Wind     = PackInstanceWind( wind );
                            draw.BoundsPad         = WindBoundsPad( wind );
                            m_ScratchMaskedDraws.push_back( draw );
                        }
                        continue;
                    }

                    for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                    {
                        if ( ismIds )
                        {
                            // By id: the batch's slice of this view's id list (the id material's
                            // InstanceTransforms), its rows the scene's GpuScene instances.
                            const auto first = static_cast<uint32_t>( shadowIsmIds.size() );
                            for ( std::size_t i = 0; i < visibleIds.size(); ++i )
                                if ( levels[i] == level )
                                    shadowIsmIds.push_back( visibleIds[i] );
                            idBatches.push_back( ShadowBatch{
                                 ism.Mesh, static_cast<uint32_t>( shadowIsmIds.size() ) - first, first, level,
                                 ismHidden, PackInstanceWind( wind ), WindBoundsPad( wind ) } );
                            continue;
                        }
                        const auto first = static_cast<uint32_t>( instTransforms.size() );
                        for ( std::size_t i = 0; i < visible.size(); ++i )
                            if ( levels[i] == level )
                                instTransforms.push_back( visible[i] );
                        batches.push_back( ShadowBatch{
                             ism.Mesh, static_cast<uint32_t>( instTransforms.size() ) - first, first, level, ismHidden,
                             PackInstanceWind( wind ), WindBoundsPad( wind ) } );
                    }
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
                                     m_CascadeCasterBias[c], m_ParallaxQuality, viewWind );

            auto& skinBones = m_ScratchBones;
            skinBones.clear();
            // Per section, as the static entries: a masked section through its template's caster, masked to its
            // own submeshes; the opaque sections of an entry are ONE depth draw (no material to tell apart),
            // drawing their union over one copy of the pose.
            using OpaqueCaster  = OpaqueSkinnedCaster;
            auto& opaqueCasters = m_ScratchOpaqueSkinnedCasters; // a member: its capacity persists
            opaqueCasters.clear();
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
                                     m_CascadeCasterBias[c], m_ParallaxQuality, viewWind );
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
        // The ISMs by id (CPU-ISM-PACK 6): this view's own id list, uploaded once, its rows the scene's GpuScene
        // instances (the list binds them to the id material's block). No .Instances: only a virtual shadow map
        // level's GPU cull reads them, and a level draws no id caster (ismIds).
        if ( ismIds && !idBatches.empty() )
        {
            auto* idMat = m_ShadowInstancedIdMaterial[c].get();
            idMat->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                   m_CascadeCasterBias[c], m_ParallaxQuality, viewWind );
            if ( auto* sb = idMat->Get<StorageBufferProperty>( kInstanceTransformsBlock ) )
                sb->SetRawData( shadowIsmIds.data(),
                                static_cast<uint32_t>( shadowIsmIds.size() * sizeof( uint32_t ) ) );
            list.SetSceneInstances( m_FrameSceneInstances );
            for ( const auto& b : idBatches )
            {
                list.Add( { .Pipeline          = casters.InstancedSceneIds,
                            .Mesh              = b.Mesh,
                            .Transform         = glm::mat4( 1.0f ),
                            .Material          = idMat->GetMaterialExecutor(),
                            .InstanceCount     = b.Count,
                            .FirstInstance     = b.First,
                            .HiddenSubmeshMask = b.HiddenSubmeshMask,
                            .LodLevel          = b.LodLevel,
                            .Push              = DrawPushFields{}.Wind( b.Wind ),
                            .BoundsPad         = b.BoundsPad } );
            }
        }

        // Masked casters: each copy's rows and transforms uploaded once, here in setup, its camera set to THIS
        // cascade (the copy is this cascade's alone); then its draws, in collection order.
        for ( std::size_t i = 0; i < maskedSetCount; ++i )
        {
            auto& set = *m_ScratchMaskedSets[i];
            WriteLightCamera( *set.Caster, glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                              m_CascadeCasterBias[c], m_ParallaxQuality, viewWind );
            // The draws' rows are the scene material table's (SyncGpuScene); a refusal was said by name.
            if ( !LendMaterialTable( *set.Caster ) )
            {
                continue;
            }
            if ( set.SceneIds ? set.Ids.empty() : set.Transforms.empty() )
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
                // A caster by id (SurfaceSceneInstancesVariant): its block holds GpuScene instance ids, the rows
                // the list binds (SetSceneInstances).
                if ( set.SceneIds )
                {
                    sb->SetRawData( set.Ids.data(), static_cast<uint32_t>( set.Ids.size() * sizeof( uint32_t ) ) );
                    list.SetSceneInstances( m_FrameSceneInstances );
                }
                else
                {
                    sb->SetRawData( set.Transforms.data(),
                                    static_cast<uint32_t>( set.Transforms.size() * sizeof( glm::mat4 ) ) );
                }
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
            GraphicsPipeline* const pipeline = MaskedCasterPipeline( *material, draw.Path, casters );
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
                        instanced && !draw.Set->SceneIds
                             ? std::span<const glm::mat4>( draw.Set->Transforms ).subspan( draw.First, draw.Count )
                             : std::span<const glm::mat4>{},
                   .BoundsPad   = draw.BoundsPad,
                   .WorldBounds = draw.WorldBounds } );
        }
        // The GPU-generated sections (BuildGpuInstancedShadowDraws). Cascades only: their caster cells and light
        // cameras are the cascades' (a virtual shadow map level draws none of them). A still section (the grass)
        // casts into the static layer too - its instances past the shadow wind distance, in their rest pose, or
        // every instance of one that does not sway - and its movers over the cache from the cascade's movable
        // view (kCascadeMovableViewBase + cascade view); a section whose instances move by themselves only over the
        // cache. The uncached cascade draws all of them from its own view. A still section not drawn into the
        // static layer for a pending reason (textures, pipeline) stales the cache as a compiling caster does.
        if ( c < kCascadeViews )
            maskedPending = BuildGpuInstancedShadowDraws( c, c, layer, list ) || maskedPending;
        else if ( drawMovable && c >= kCascadeMovableViewBase && c < kCascadeMovableViewBase + kCascadeViews )
            BuildGpuInstancedShadowDraws( c - kCascadeMovableViewBase, c, ShadowCasterLayer::Movable, list );

        // The cache drawn now lacks the casters still compiling: the next frame's plan sees no key and redraws it.
        if ( maskedPending && layer == ShadowCasterLayer::Static && c < kCascadeViews )
            m_CascadeCache[c].StaticKey.reset();
        // A virtual shadow map level's cached pages lack them the same way: the next frame drops every page
        // (CollectVirtualShadowInvalidation).
        if ( maskedPending && IsVirtualShadowView( c ) )
            m_VsmCastersPending = true;
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
        // A scrolled cascade's layer is also copied back into its cache ("MeshShadowCascade{c}.ToCache"): CopySrc.
        graph.Extract( created, m_CascadeMaps, RDG::Access::SampledGraphics,
                       { RDG::Access::CopyDst, RDG::Access::CopySrc } );
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
                {
                    // ...whole: a fresh layer holds no static depth around a region to keep (MovableRegion).
                    m_CascadeCache[slot * kMaxCascades + c].Rebuild = true;
                    m_CascadeCache[slot * kMaxCascades + c].MovableRegion.reset();
                }
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
                         // A SCROLLED cascade fills only the strips its shift uncovered (PlanCascadeCache): the
                         // layer around them is the cache copied in, already every static caster's depth. A
                         // strip's draws are recorded under its scissor (CascadeScrollStrips' rects), so a
                         // caster reaching a strip writes the strip only (UE's CSM scrolling renders the
                         // uncovered region alone). One strip: the mesh casters' culled draw too; two: that one
                         // draw holds the casters of both and is recorded once, at the whole map's raster.
                         const CascadeCacheState& cache  = m_CascadeCache[c];
                         uint32_t                 strips = 0u;
                         if ( cache.Scroll )
                             strips = std::min( cache.StripCount, 2u );
                         auto&      renderer = Renderer::GetInstance();
                         const auto scissor  = [&renderer]( const glm::uvec4& rect ) {
                             renderer.SetScissor( static_cast<int32_t>( rect.x ), static_cast<int32_t>( rect.y ),
                                                   rect.z, rect.w );
                         };
                         if ( strips == 1u )
                             scissor( cache.StripRects[0] );
                         if ( auto drawn = m_CascadeDraws[c].Record( context ); !drawn.IsSuccess() )
                             return drawn;
                         // The keyed non-mesh casters (the landscape), into the cached layer their key is part
                         // of (PlanCascadeCache); a scrolled cascade's one strip a call, under its scissor.
                         for ( const auto& [weak, firstBlock] : m_CascadeStaticCasters[c] )
                         {
                             const auto caster = weak.lock();
                             if ( !caster )
                             {
                                 continue; // gone since the setup: its draws went with it
                             }
                             if ( strips == 0u )
                             {
                                 if ( auto cast =
                                           caster->RecordShadowView( context, c, firstBlock, m_CascadeCasterVP[c],
                                                                     m_CascadeCasterBias[c], {} );
                                      !cast.IsSuccess() )
                                 {
                                     return cast;
                                 }
                                 continue;
                             }
                             for ( uint32_t s = 0; s < strips; ++s )
                             {
                                 scissor( cache.StripRects[s] );
                                 const std::span<const glm::mat4> region( &cache.Strips[s], 1u );
                                 if ( auto cast =
                                           caster->RecordShadowView( context, c, firstBlock, m_CascadeCasterVP[c],
                                                                     m_CascadeCasterBias[c], region );
                                      !cast.IsSuccess() )
                                 {
                                     return cast;
                                 }
                             }
                         }
                         // The render pass's own scissor back (the whole layer, SetViewportAndScissor).
                         if ( strips > 0u )
                             renderer.SetScissor( 0, 0, m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize );
                         return BOOLSUCCESS;
                     },
                };
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
                if ( state.Scroll )
                {
                    // Into the layer the scroll copy filled ("MeshShadowCascade{c}.Scroll"): LOAD, the strips drawn
                    // over it.
                    pass.TargetDepthArray = maps;
                    pass.TargetLayer      = layer;
                    frame.Scrolls.push_back( { .From  = m_CascadeStaticFB[c]->GetDepthAttachmentImage(),
                                               .Layer = layer,
                                               .Shift = state.Shift,
                                               .Name  = name + ".Scroll" } );
                    frame.ScrollPasses.push_back( std::move( pass ) );
                }
                else
                {
                    pass.TargetFramebuffer = m_CascadeStaticFB[c];
                    pass.ClearDepth        = 1.0f;
                    frame.StaticPasses.push_back( std::move( pass ) );
                }
            }

            // A scrolled cascade's layer now holds its static depth whole: it becomes the cache. Any other rebuilt
            // cascade is its cache copied in.
            if ( state.Scroll )
                frame.Copies.push_back( { .From    = m_CascadeStaticFB[c]->GetDepthAttachmentImage(),
                                          .Layer   = layer,
                                          .Name    = name + ".ToCache",
                                          .ToCache = true } );
            else
            {
                CascadeCacheCopy restore{ .From  = m_CascadeStaticFB[c]->GetDepthAttachmentImage(),
                                          .Layer = layer,
                                          .Name  = name + ".FromCache" };
                // Over a standing static layer, only the texels the movers left and reach (PlanCascadeCache).
                if ( state.MovableRegion )
                    restore.Regions.push_back( *state.MovableRegion );
                frame.Copies.push_back( std::move( restore ) );
            }

            if ( !state.Movable )
                continue;
            const uint32_t   view = kCascadeMovableViewBase + c;
            SystemRasterPass pass{
                 .Name        = name + ".Movable",
                 .ExecuteFunc = [this, c]( RDG::PassContext& context,
                                           const FrameGraphRefs& ) -> Common::BoolResultStr
                 {
                     // Under the restored region alone (PlanCascadeCache's MovableRegion): the layer around it is
                     // last frame's, its movers' depth already gone from it or never there.
                     const std::optional<glm::uvec4> region   = m_CascadeCache[c].MovableRegion;
                     auto&                           renderer = Renderer::GetInstance();
                     if ( region )
                         renderer.SetScissor( static_cast<int32_t>( region->x ), static_cast<int32_t>( region->y ),
                                              region->z, region->w );
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
                                                                    m_CascadeCasterBias[c], {} );
                              !cast.IsSuccess() )
                         {
                             return cast;
                         }
                     }
                     if ( region )
                         renderer.SetScissor( 0, 0, m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize );
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
