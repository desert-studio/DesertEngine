// MeshRenderer's light-view passes: the cascaded shadow map (setup, cascade fit, graph pass).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Materials/SceneResources.hpp>

#include <format>

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
        LOG_INFO( "[Shadows] {} cascade(s) at {}x{} over {:.0f} m = {:.1f} MiB of attachments for this "
                  "renderer, allocated in {:.1f} ms ({:.1f} MiB live across {} renderer(s) holding "
                  "cascades).",
                  m_Shadow.CascadeCount, m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize,
                  Common::Units::ToMetres( m_Shadow.MaxDistance ),
                  static_cast<double>( ShadowAttachmentBytes( m_Shadow ) ) / ( 1024.0 * 1024.0 ), allocMs,
                  static_cast<double>( ShadowAttachmentLease::LiveBytes() ) / ( 1024.0 * 1024.0 ),
                  ShadowAttachmentLease::LiveHolders() );
    }

    void MeshRenderer::InvalidateCascadeCaches()
    {
        for ( CascadeCacheState& cache : m_CascadeCache )
            cache = CascadeCacheState{};
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
            m_CascadeStaticFB[i].reset();
            m_CascadeCache[i] = CascadeCacheState{};
        }
        for ( uint32_t i = 0; i < kMaxShadowViews; ++i )
        {
            m_ShadowMaterial[i].reset();
            m_ShadowInstancedMaterial[i].reset();
            m_ShadowSkinnedMaterial[i].reset();
        }
        // The local lights' atlas is sized by the same budget (LocalShadowAtlasSize): it is made again, at the
        // new size, by the first light that casts after this, and every light's tiles and cache go with it.
        // The array is made again at the new size by the next frame (AcquireCascadeMaps); the device is idle.
        m_CascadeMaps      = RDG::ExternalTexture{};
        m_CascadeMapsBuilt = false;
        m_LocalAtlasFB.reset();
        m_LocalAtlas.Reset( 0 );
        m_LocalLights.clear();
        m_LocalShadowRows.clear();
        m_LocalRedraws.clear();
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
        // (ShadowCascadePasses hands no pass, CaptureFrameState reports 0, and the shader's cascade
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
        for ( uint32_t i = 0; i < m_Shadow.CascadeCount; ++i )
        {
            FramebufferSpecification cacheSpec;
            cacheSpec.DebugName = std::format( "ShadowCascade{}.StaticCache", i );
            cacheSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowDepth );
            m_CascadeStaticFB[i] = Graphic::Framebuffer::Create( cacheSpec );
            m_CascadeStaticFB[i]->Resize( m_Shadow.ShadowMapSize, m_Shadow.ShadowMapSize );
            m_CascadeCache[i]   = CascadeCacheState{};
            m_ShadowMaterial[i] = std::make_unique<MaterialShadow>( MeshPass::ShadowDepth );
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

                for ( uint32_t i = 0; i < m_Shadow.CascadeCount; ++i )
                    m_ShadowInstancedMaterial[i] = std::make_unique<MaterialShadowInstanced>( MeshPass::ShadowDepth );
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

                for ( uint32_t i = 0; i < m_Shadow.CascadeCount; ++i )
                    m_ShadowSkinnedMaterial[i] = std::make_unique<MaterialShadowSkinned>( MeshPass::ShadowDepth );
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

        for ( uint32_t c = 0; c < n; ++c )
        {
            m_CascadeVP[c]                                          = fits[c].ViewProj;
            m_CascadeWorldPerTexel[static_cast<glm::length_t>( c )] = fits[c].WorldPerTexel;
            // The caster's bias, in world centimetres scaled per cascade (CascadeBiasSettings), as the
            // matrix the casters draw with and the slope term their depth cell adds.
            m_CascadeCasterBias[c] = ComputeCascadeCasterBias( m_ShadowBias, fits[c], fits[0].WorldPerTexel );
            m_CascadeCasterVP[c]   = CasterViewProj( fits[c].ViewProj, m_CascadeCasterBias[c].DepthBias );
            m_CascadeDepthRange[static_cast<glm::length_t>( c )] = fits[c].DepthRange;
            // The movable layer draws the same cascade with the same matrix and bias, through its own view.
            m_CascadeCasterVP[kCascadeMovableViewBase + c]   = m_CascadeCasterVP[c];
            m_CascadeCasterBias[kCascadeMovableViewBase + c] = m_CascadeCasterBias[c];
        }

        PlanCascadeCache();
    }

    float MeshRenderer::GetSunSourceTanHalf() const
    {
        // UE's LightSourceAngle: the sun's angular DIAMETER in degrees; the penumbra grows by tan(angle / 2) per
        // centimetre between the occluder and the receiver (each side of the edge).
        const float degrees = std::clamp( m_SunSourceAngle, 0.0f, 89.0f );
        return std::tan( glm::radians( degrees ) * 0.5f );
    }

    uint64_t MeshRenderer::CascadeStaticSignature( const uint32_t cascade, bool& movable ) const
    {
        // THE CASTER BOX, as BuildShadowCascadeDraws culls with: a static caster outside it is not in the cache,
        // so it moving changes nothing drawn.
        const Core::Frustum frustum( m_CascadeCasterVP[cascade], glm::mat4( 1.0f ) );
        SignatureHash       hash;
        for ( const auto& rd : m_StaticQueue )
        {
            if ( rd.Mesh == nullptr || !rd.CastShadows ||
                 !IsVisibleInView( frustum, rd.Transform, Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) ) )
                continue;
            hash.Add( rd.Mesh );
            hash.Add( rd.Transform );
            hash.Add( rd.HiddenSubmeshes );
            hash.Add( rd.ForcedLOD );
            hash.Add( rd.LODBias );
            // The entry's section material decides the caster cell (opaque or masked) and a masked one's clip
            // (ShadingOf: the same answer the casters draw with).
            const SectionShading shading = ShadingOf( rd );
            hash.Add( rd.SectionSlot );
            hash.Add( shading.Surface );
            hash.Add( shading.Instance );
        }
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh == nullptr || !g.CastShadows ||
                 !IsVisibleInView( frustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
                continue;
            hash.Add( g.Mesh );
            hash.Add( g.Transform );
        }
        // A still instanced mesh is keyed by its snapshot (m_CascadeKeySnapshots says why that is sound); a
        // swaying one is movable wherever it stands.
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                continue;
            if ( ism.Wind.Sways() )
            {
                movable = true;
                continue;
            }
            hash.Add( ism.Mesh );
            hash.Add( ism.Transforms.get() );
            hash.Add( ism.Transforms->size() );
            hash.Add( ism.Material.get() );
        }
        for ( const auto& sd : m_SkinnedQueue )
        {
            if ( sd.Mesh != nullptr && sd.CastShadows && !sd.BoneMatrices.empty() &&
                 IsVisibleInView( frustum, sd.Transform, Geometry::LocalBounds( sd.Mesh->GetSubmeshes() ) ) )
                movable = true;
        }
        return hash.Value;
    }

    void MeshRenderer::PlanCascadeCache()
    {
        // UE's cached shadow maps for a directional light's cascades: the static casters' depth is kept per
        // cascade and drawn again only when the cascade's matrix (the sun turned, the camera moved a texel) or
        // one of the static casters it reaches changed; the movable casters are drawn over a copy of it every
        // frame they exist.
        if ( !m_ShadowsEnabled || !m_ShadowPipeline )
            return;

        bool castersMove = false;
        for ( const auto& weak : m_ShadowCasters )
            if ( const auto caster = weak.lock(); caster && caster->CastsShadowThisFrame() )
                castersMove = true;

        const uint32_t count = std::min( m_FittedCascades, m_Shadow.CascadeCount );
        for ( uint32_t c = 0; c < count; ++c )
        {
            CascadeCacheState& state = m_CascadeCache[c];
            if ( !m_CascadeStaticFB[c] )
                continue;
            bool          movable = castersMove;
            SignatureHash key;
            key.Add( CascadeStaticSignature( c, movable ) );
            key.Add( m_CascadeCasterVP[c] );
            key.Add( m_CascadeCasterBias[c] );
            EnsureShadowViewMaterials( kCascadeMovableViewBase + c );

            state.Movable      = movable;
            state.RedrawStatic = state.StaticKey != key.Value;
            state.StaticKey    = key.Value;
            state.Rebuild      = state.RedrawStatic || state.Movable || state.MapHasMovable;
            if ( state.Rebuild )
                state.MapHasMovable = state.Movable;
        }

        m_CascadeKeySnapshots.clear();
        for ( const auto& ism : m_InstancedQueue )
            if ( ism.Transforms )
                m_CascadeKeySnapshots.push_back( ism.Transforms );
    }

    bool MeshRenderer::IsMaskedSurface( const DataDrivenMaterial* material )
    {
        return material != nullptr && ShadowCasterCellFor( material->GetSchema().Blend ) == ShadowCasterCell::Own;
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const StaticMeshRenderData& data )
    {
        const SurfaceSlot slot = SectionSurface( data.MaterialSlots.get(), data.SectionSlot );
        return { slot.Instance, slot.Surface, IsMaskedSurface( slot.Surface ) };
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const InstancedMeshRenderData& data )
    {
        MaterialInstance* instance = data.Material.get();
        auto*             surface =
             instance != nullptr ? dynamic_cast<DataDrivenMaterial*>( instance->GetParentMaterial() ) : nullptr;
        return { instance, surface, IsMaskedSurface( surface ) };
    }

    MeshRenderer::SectionShading MeshRenderer::ShadingOf( const SkinnedSection& section )
    {
        return { section.Instance, section.Material, IsMaskedSurface( section.Material ) };
    }

    DataDrivenMaterial* MeshRenderer::MaskedCasterMaterial( const DataDrivenMaterial* material,
                                                            MeshVertexPath path, uint32_t cascade )
    {
        const MeshPass pass   = IsVirtualShadowView( cascade ) ? MeshPass::VirtualShadowDepth : MeshPass::ShadowDepth;
        auto*          caster = Runtime::ResourceRegistry::GetMaterialService()->GetViewVariant( material, path, pass,
                                                                                                cascade );
        if ( caster == nullptr )
        {
            // Not drawn through the shared program instead: that is the square shadow this route exists to end.
            static std::unordered_set<const DataDrivenMaterial*> s_Refused;
            if ( s_Refused.insert( material ).second )
                LOG_ERROR( "[Shadows] a Masked material ('{}') has no ({} x {}) caster of its own; its objects cast "
                           "NO shadow on that path (MaterialService named the cell it refused).",
                           material->GetShaderName(), MeshVertexPathName( path ), MeshPassName( pass ) );
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
        const auto choice       = m_MaterialPipelines.ChooseCell( key, reports, standInReady );
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

        // The shared caster's state for this path and pass (vertex layout, standard-Z depth, the cascade target or the
        // virtual map's render area) — only the program differs, which is the whole difference between an opaque and
        // a masked caster. The cell's name names its pass, so the name alone keys the pipeline.
        const CasterPipelines casters = CasterPipelinesOf( pass == MeshPass::VirtualShadowDepth ? kVsmViewBase : 0u );
        const GraphicsPipeline* shared = nullptr;
        switch ( path )
        {
            case MeshVertexPath::Instanced:
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
        // Shadow vert computes Projection*View*Transform; feed the combined cascade matrix as
        // Projection and identity as View, matching u_LightViewProj[c] on the lit side.
        m_ShadowMaterial[c]->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                             m_CascadeCasterBias[c] );
        const MaterialExecutor* casterExecutor = m_ShadowMaterial[c]->GetMaterialExecutor();
        // The pipelines of the view's caster cells: the cascades' and local views' depth attachment, or the virtual
        // shadow map's attachment-less pool raster (IsVirtualShadowView).
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
        // clamp and land on its sun-side end (EnsureVirtualShadowPipelines). That end is clip z >= 0 of the level's
        // standard-Z [0,1] projection, the plane the extraction names PLANE_FAR (row 2 alone, Frustum::Rebuild).
        if ( IsVirtualShadowView( c ) )
            cascadeFrustum.Open( Core::PLANE_FAR );

        // THE LOD OF A CASTER IS ASKED FROM THE CAMERA, not from the light, and that is
        // deliberate rather than convenient: a caster drawn into the cascade at a coarser
        // level than the object the camera sees casts a silhouette that does not match the
        // object it belongs to. The per-object caster loop below has always asked it this
        // way (ComputeLOD reads the main camera); the batched paths simply did not ask.
        const auto*     lodCamera       = m_SceneRenderer->GetMainCamera();
        const Geometry::LODView lodView =
             lodCamera != nullptr ? Geometry::LODView{ lodCamera->GetPosition(), lodCamera->GetProjectionMatrix() }
                                  : Geometry::LODView{};

        // Keyed by (mesh, hidden mask): a section caster batches with the same section of its twins.
        std::vector<std::pair<std::pair<Desert::StaticMesh*, uint64_t>, std::vector<const StaticMeshRenderData*>>>
                   byMesh;
        const auto bucketFor = [&]( Desert::StaticMesh* mesh,
                                    uint64_t            hidden ) -> std::vector<const StaticMeshRenderData*>&
        {
            for ( auto& [key, v] : byMesh )
                if ( key.first == mesh && key.second == hidden )
                    return v;
            byMesh.emplace_back( std::make_pair( mesh, hidden ), std::vector<const StaticMeshRenderData*>{} );
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
        const auto isMasked = []( const DataDrivenMaterial* material ) { return IsMaskedSurface( material ); };

        for ( const auto& rd : m_StaticQueue )
        {
            if ( !drawStatic || rd.Mesh == nullptr || !rd.CastShadows ||
                 !IsVisibleInView( cascadeFrustum, rd.Transform,
                                   Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) ) )
            {
                continue;
            }
            // The caster takes the material the LIT draw shows, by the same mapping: one entry per section
            // material (Rules::SplitMeshSlotDraws), shaded by SectionSurface, casting only its own submeshes - so
            // a masked section takes the masked route and an opaque one the opaque route.
            const SurfaceSlot         slot = SectionSurface( rd.MaterialSlots.get(), rd.SectionSlot );
            MaterialInstance*         inst = slot.Instance;
            const DataDrivenMaterial* mat  = slot.Surface;
            if ( !isMasked( mat ) )
            {
                bucketFor( rd.Mesh, rd.HiddenSubmeshes ).push_back( &rd );
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
            draw.HiddenSubmeshMask = rd.HiddenSubmeshes;
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
            Desert::StaticMesh* const mesh = meshKey.first;
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
                    batches.push_back(
                         ShadowBatch{ mesh, count, first, level, meshKey.second, InstanceWindPush{} } );
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
                if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() ||
                     !( ism.Wind.Sways() ? drawMovable : drawStatic ) )
                    continue;

                // Per-instance, against this cascade. A cascade covers a slice of the view,
                // so a forest spread over the map has most of its instances outside every
                // one of the four — and the batch is a single draw whose cost is entirely
                // its instance count.
                const Common::Math::AABB localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );

                // The cull distance from the MAIN camera, as the geometry pass measures it:
                // an instance faded out of the view casts no shadow either.
                auto& visible = m_ScratchIsmVisible;
                auto& levels  = m_ScratchLodLevels;
                CollectIsmInstances( *ism.Transforms, localBounds, cascadeFrustum, ism.CullDistance, ism.Wind,
                                     lodView, lodView, ism.Mesh->GetLODScreenSizes(), visible, levels );
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
                        draw.HiddenSubmeshMask = ism.HiddenSubmeshes;
                        draw.Instance = ismInst;
                        draw.Row      = row;
                        draw.Count    = static_cast<uint32_t>( set.Transforms.size() ) - first;
                        draw.First    = first;
                        draw.LodLevel = level;
                        draw.Wind     = PackInstanceWind( ism.Wind );
                        draw.BoundsPad = WindBoundsPad( ism.Wind );
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
                                                    level, ism.HiddenSubmeshes, PackInstanceWind( ism.Wind ),
                                                    WindBoundsPad( ism.Wind ) } );
                }
            }
        }

        // Per-object path (singletons).
        for ( const auto* rd : singles )
        {
            list.Add( { .Pipeline          = casters.Static,
                        .Mesh              = rd->Mesh,
                        .Transform         = rd->Transform,
                        .Material          = casterExecutor,
                        .HiddenSubmeshMask = rd->HiddenSubmeshes,
                        .LodLevel          = ComputeLOD( rd->Transform, rd->Mesh, rd->ForcedLOD, rd->LODBias ) } );
        }

        // Meshes drawn with a data-driven material (shader graph, Shader Override, per-slot
        // custom materials) cast through the SAME pipeline as everything else: a caster is
        // depth, and depth does not care which shader would have coloured the surface. They
        // used to be absent from the cascades entirely, because this pass only ever walked
        // the lit queues — a shader-graph object was lit like a solid and shadowed like a
        // hole in the world.
        //
        // Per-object only, no instanced batching: the batching above keys on StaticMesh*,
        // and this queue holds Mesh* (it also carries skinned and procedurally-built
        // meshes). Casters here are counted in ones and twos, not in the hundreds the
        // batching exists for.
        //
        // Each draw casts its OWN submeshes (VisibleSubmeshMask), as every static caster does: an entity's draws
        // split its sections between this queue and the batched ones, and the silhouette is their union
        // (Rules::SplitMeshSlotDraws gives each draw the entity's CastShadows).
        //
        // No masked route here, and none can apply: BlendMode Masked exists only on a Surface template
        // (DShaderParser refuses it on a shader with no Surface block), a template draws only through its mesh
        // cells, and a slot material with a mesh cell never reaches this queue (CustomSlotMaterial). The generic
        // lit draw runs the shader's own program, which clips nothing, so an opaque caster is what it shows.
        for ( const auto& g : m_GenericQueue )
        {
            if ( drawStatic && g.Mesh != nullptr && g.CastShadows &&
                 IsVisibleInView( cascadeFrustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
            {
                list.Add( { .Pipeline          = casters.Static,
                            .Mesh              = g.Mesh,
                            .Transform         = g.Transform,
                            .Material          = casterExecutor,
                            .HiddenSubmeshMask = ~g.VisibleSubmeshMask,
                            .LodLevel          = ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 ) } );
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
                                     m_CascadeCasterBias[c] );

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
            };
            std::vector<OpaqueCaster> skinnedCasters;
            for ( const auto& sd : m_SkinnedQueue )
            {
                if ( sd.Mesh == nullptr || !sd.CastShadows || sd.BoneMatrices.empty() )
                    continue;
                uint64_t opaqueVisible = 0;
                for ( const SkinnedSection& section : sd.Sections )
                {
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
                    auto&            set = maskedSetFor( caster );
                    MaskedCasterDraw draw;
                    draw.Set       = &set;
                    draw.Path      = MeshVertexPath::Skinned;
                    draw.Mesh      = sd.Mesh;
                    draw.Transform = sd.Transform;
                    draw.Instance  = section.Instance;
                    draw.Row       = AppendRow( set.Rows, EffectiveRow( section.Material, section.Instance ) );
                    draw.First     = static_cast<uint32_t>( set.Transforms.size() );
                    draw.MotionRow         = sd.MotionRow;
                    draw.HiddenSubmeshMask = section.HiddenSubmeshes;
                    if ( IsVirtualShadowView( c ) )
                        draw.WorldBounds = SkinnedPoseBounds( *sd.Mesh, sd.Transform, sd.BoneMatrices );
                    set.Transforms.insert( set.Transforms.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
                    m_ScratchMaskedDraws.push_back( draw );
                }
                if ( opaqueVisible == 0 )
                    continue;
                skinnedCasters.push_back( { &sd, static_cast<uint32_t>( skinBones.size() ), ~opaqueVisible } );
                skinBones.insert( skinBones.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
            }
            if ( !skinnedCasters.empty() )
            {
                skinMat->UploadBones( skinBones );
                const std::span<const GpuObjectMotion> rows = m_ScratchMotionRows.Rows;
                for ( const OpaqueCaster& caster : skinnedCasters )
                {
                    // The cached pose when the skin cache holds this primitive (one skinning per frame).
                    const GpuObjectMotion row =
                         caster.Data->MotionRow < rows.size() ? rows[caster.Data->MotionRow] : GpuObjectMotion{};
                    const uint64_t address =
                         ( static_cast<uint64_t>( row.SkinCacheAddressHi ) << 32u ) | row.SkinCacheAddressLo;
                    // A virtual shadow map level culls the caster by its posed box (CollectVirtualShadowCull).
                    std::optional<Common::Math::AABB> pose;
                    if ( IsVirtualShadowView( c ) )
                        pose = SkinnedPoseBounds( *caster.Data->Mesh, caster.Data->Transform,
                                                  caster.Data->BoneMatrices );
                    list.Add( { .Pipeline          = casters.Skinned,
                                .Mesh              = caster.Data->Mesh,
                                .Transform         = caster.Data->Transform,
                                .Material          = skinMat->GetMaterialExecutor(),
                                .HiddenSubmeshMask = caster.Hidden,
                                .BindState         = [skinMat, offset = caster.BoneOffset, address,
                                              count = row.SkinCacheVertexCount]
                                {
                                    skinMat->SetBoneOffset( offset );
                                    skinMat->SetSkinCache( address, count );
                                },
                                .WorldBounds = pose } );
                }
            }
        }

        // Instanced path.
        if ( instancingOn && !batches.empty() )
        {
            auto* instMat = m_ShadowInstancedMaterial[c].get();
            instMat->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                                     m_CascadeCasterBias[c] );
            if ( auto* sb = instMat->Get<StorageBufferProperty>( "InstanceTransforms" ) )
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
                            .BindState = [instMat, wind = b.Wind] { instMat->SetInstancedWind( wind ); },
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
                              m_CascadeCasterBias[c] );
            if ( auto* sb = set.Caster->Get<StorageBufferProperty>( "Materials" ) )
            {
                sb->SetRawData( set.Rows.data(), static_cast<uint32_t>( set.Rows.size() * sizeof( glm::vec4 ) ) );
            }
            if ( set.Transforms.empty() )
            {
                continue;
            }
            // The one binding the path adds (MeshPathOwnBinding): instance matrices or bone poses.
            const auto path = MeshCellPath( set.Caster->GetShaderName() );
            // A skinned caster also reads its row's skin-cache fields from the view's rows (Vertex_Skinned.glslh).
            if ( path == MeshVertexPath::Skinned )
            {
                auto*      rows  = set.Caster->Get<StorageBufferProperty>( SceneResources::kObjectMotionsName );
                const bool bound = rows != nullptr && m_ObjectMotions != nullptr;
                DESERT_VERIFY( bound, "a skinned masked caster reads ObjectMotions but the view carries none" );
                rows->SetBuffer( m_ObjectMotions );
            }
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
        bool maskedPending = false;
        for ( const MaskedCasterDraw& draw : m_ScratchMaskedDraws )
        {
            DataDrivenMaterial* material  = draw.Set->Caster;
            const bool          instanced = draw.Path == MeshVertexPath::Instanced;
            // A masked caster draws through its OWN pipeline only once the driver has built it (UE: a draw whose
            // pipeline is not ready is not submitted). Handed to the list unbuilt or refused, the cascade pass would
            // fault at its bind ("could not be bound in the open render pass") and take the cascade's copy and the
            // composite reading the cascades with it. Refused (null; MaskedCasterPipeline said why, once) or failed
            // in the driver: that caster casts no shadow. Still compiling: not drawn THIS frame, and the static
            // cache is marked stale so the cascade redraws it once the pipeline is ready.
            GraphicsPipeline* const pipeline = MaskedCasterPipeline( *material, draw.Path, casters.Pass );
            if ( pipeline == nullptr || pipeline->GetReadiness() == PipelineReadiness::Failed )
                continue;
            if ( pipeline->GetReadiness() == PipelineReadiness::Compiling )
            {
                maskedPending = true;
                continue;
            }
            list.Add( { .Pipeline          = pipeline,
                        .Mesh              = draw.Mesh,
                        .Transform         = draw.Transform,
                        .Material          = material->GetMaterialExecutor(),
                        .InstanceCount     = instanced ? draw.Count : 1,
                        .FirstInstance     = instanced ? draw.First : 0,
                        .HiddenSubmeshMask = draw.HiddenSubmeshMask,
                        .LodLevel          = draw.LodLevel,
                        .BindState         = [material, draw]
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
                                material->SetPrimitiveIndex( draw.MotionRow );
                            }
                            material->Bind( draw.Instance );
                        },
                        .Instances   = instanced ? std::span<const glm::mat4>( draw.Set->Transforms )
                                                        .subspan( draw.First, draw.Count )
                                                 : std::span<const glm::mat4>{},
                        .BoundsPad   = draw.BoundsPad,
                        .WorldBounds = draw.WorldBounds } );
        }
        // The cache drawn now lacks the casters still compiling: the next frame's plan sees no key and redraws it.
        if ( maskedPending && layer == ShadowCasterLayer::Static && c < kMaxCascades )
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
                               .Layers = m_Shadow.CascadeCount,
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
        const uint32_t fitted = GetValidCascadeCount();
        for ( uint32_t c = 0; c < m_Shadow.CascadeCount; ++c )
        {
            if ( c < fitted )
                m_CascadeCache[c].Rebuild = true;
            else
                m_CascadeCache[c].StaticKey.reset();
        }
        return created;
    }

    MeshRenderer::ShadowCascadeFrame MeshRenderer::ShadowCascadePasses( const RDG::TextureRef maps )
    {
        // Same guard as the geometry pass: the cascade pass reads m_ShadowPipeline's spec, and
        // SetupShadowPass can now refuse. No caster pipeline means no cascade passes and an unshadowed
        // scene, not a dead editor.
        ShadowCascadeFrame frame;
        if ( !m_ShadowPipeline || !m_ShadowsEnabled )
            return frame;

        // Cascade matrices and the cache plan are computed in UpdateCascades() before the graph records: a pass
        // only reads them. Each layer clears to 1 (far): the cascades are STANDARD-Z (SetupShadowPass says why),
        // and a 0 clear under its LessOrEqual test would reject every caster and hand back an empty map, silently.
        for ( uint32_t c = 0; c < m_Shadow.CascadeCount; ++c )
        {
            const CascadeCacheState& state = m_CascadeCache[c];
            if ( !maps.IsValid() || !m_CascadeStaticFB[c] || !state.Rebuild )
                continue;

            if ( state.RedrawStatic )
            {
                SystemRasterPass pass{ .Name        = std::format( "MeshShadowCascade{}.Static", c ),
                                       .ExecuteFunc = [this, c]( RDG::PassContext& context,
                                                                 const FrameGraphRefs& ) -> Common::BoolResultStr
                                       { return m_CascadeDraws[c].Record( context ); },
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
                };
                frame.StaticPasses.push_back( std::move( pass ) );
            }

            frame.Copies.push_back( { .From  = m_CascadeStaticFB[c]->GetDepthAttachmentImage(),
                                      .Layer = c,
                                      .Name  = std::format( "MeshShadowCascade{}.FromCache", c ) } );

            if ( !state.Movable )
                continue;
            const uint32_t   view = kCascadeMovableViewBase + c;
            SystemRasterPass pass{
                 .Name        = std::format( "MeshShadowCascade{}.Movable", c ),
                 .ExecuteFunc = [this, c]( RDG::PassContext& context,
                                           const FrameGraphRefs& ) -> Common::BoolResultStr
                 {
                     if ( auto drawn = m_CascadeMovableDraws[c].Record( context ); !drawn.IsSuccess() )
                         return drawn;

                     // Casters that are not meshes (the tessellated terrain), recorded into THIS pass so the map
                     // holds everyone's depth (IShadowCaster): the ones the Declare declared, each through its own
                     // blocks.
                     for ( const auto& [weak, firstBlock] : m_CascadeCasters[c] )
                     {
                         const auto caster = weak.lock();
                         if ( !caster )
                         {
                             continue; // gone since the setup: its draws went with it
                         }
                         if ( auto cast = caster->RecordShadowCascade(
                                   context, c, firstBlock, m_CascadeCasterVP[c], m_CascadeCasterBias[c] );
                              !cast.IsSuccess() )
                         {
                             return cast;
                         }
                     }
                     return BOOLSUCCESS;
                 },
                 .TargetDepthArray = maps,
                 .TargetLayer      = c };
            pass.Declare = [this, c, view]( RenderPassDeclaration& declared, const FrameGraphRefs& )
            {
                m_CascadeMovableDraws[c].Clear();
                m_CascadeCasters[c].clear();
                BuildShadowCascadeDraws( view, m_CascadeMovableDraws[c], ShadowCasterLayer::Movable );
                m_CascadeMovableDraws[c].Declare( declared, std::nullopt );
                // The skin cache's streams its skinned casters fetch (after "SkinCache: Skin" wrote them).
                for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                    declared.Read( streams, RDG::Access::StorageRead );

                // The non-mesh casters declare their blocks after the list's
                // (IShadowCaster::DeclareShadowCascade).
                uint32_t nextBlock = m_CascadeMovableDraws[c].BlockCount();
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
            frame.MovablePasses.push_back( std::move( pass ) );
        }
        return frame;
    }

} // namespace Desert::Graphic::System
