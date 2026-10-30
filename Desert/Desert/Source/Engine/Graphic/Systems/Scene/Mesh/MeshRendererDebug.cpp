// MeshRenderer's overlays: the selection-silhouette mask and the developer instruments (debug lines,
// overdraw) that only DESERT_DEV_INSTRUMENTS builds carry.
#include "MeshRendererInternal.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>
#endif

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MeshDebugOverdrawResolveShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, OverdrawTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
    };
} // namespace

namespace Desert::Graphic::System
{
    bool MeshRenderer::SetupSilhouettePass()
    {
        m_SilhouetteShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Silhouette" );
        if ( !m_SilhouetteShader )
        {
            LOG_ERROR( "Failed to load silhouette shader" );
            return false;
        }

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        // Dedicated single-channel-ish mask target. Selected meshes are drawn white; the rest stays
        // at the framebuffer clear color (~0.1), which JFA_Init separates with a 0.5 threshold.
        FramebufferSpecification maskSpec;
        maskSpec.DebugName = "SilhouetteMask";
        maskSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSilhouetteMask );

        m_SilhouetteMaskFramebuffer = Graphic::Framebuffer::Create( maskSpec );
        m_SilhouetteMaskFramebuffer->Resize( targetFb->GetFramebufferWidth(), targetFb->GetFramebufferHeight() );

        GraphicsPipelineSpecification spec;
        spec.DebugName = "SilhouettePipeline";
        spec.Layout    = MeshVertexLayout( MeshVertexPath::Static );

        spec.DepthTestEnabled   = false;
        spec.DepthWriteEnabled  = false;
        spec.StencilTestEnabled = false;
        spec.CullMode           = CullMode::None;
        spec.Shader             = m_SilhouetteShader;
        spec.Framebuffer        = m_SilhouetteMaskFramebuffer;

        const auto silhouette = GraphicsPipeline::Create( spec );
        if ( !silhouette )
        {
            LOG_ERROR( "[MeshRenderer] selection outlines are off: {}", silhouette.GetError() );
            return false;
        }
        m_SilhouettePipeline = silhouette.GetValue();

        m_SilhouetteMaterial = std::make_unique<MaterialSilhouette>();

        // Skinned silhouette variant (optional): same mask target/state, but the skinned vertex layout +
        // the Silhouette_Skinned shader (skins by the Bones SSBO). Used to outline selected skinned meshes.
        m_SilhouetteSkinnedShader =
             Runtime::ResourceRegistry::GetShaderService()->GetByName( "Silhouette_Skinned" );
        if ( m_SilhouetteSkinnedShader )
        {
            GraphicsPipelineSpecification sspec = spec;
            sspec.DebugName                     = "SilhouetteSkinnedPipeline";
            sspec.Layout                        = MeshVertexLayout( MeshVertexPath::Skinned );
            sspec.Shader                        = m_SilhouetteSkinnedShader;
            // Optional variant: a refusal costs the outline on skinned meshes, not the pass.
            if ( const auto skinnedSilhouette = GraphicsPipeline::Create( sspec ) )
            {
                m_SilhouetteSkinnedPipeline = skinnedSilhouette.GetValue();
                m_SilhouetteSkinnedMaterial = std::make_unique<MaterialSilhouetteSkinned>();
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] skinned meshes get no selection outline: {}",
                           skinnedSilhouette.GetError() );
            }
        }

        return true;
    }

#if DESERT_DEV_INSTRUMENTS
    // ── THE DEVELOPER'S THREE PIPELINES, AND WHY THEY ARE NOT IN A PLAYER'S BUILD ────────────────────────
    //
    // MEASURED, Shipping Runtime on this tree: a boot creates 67 Vulkan pipelines (39 graphics, 28 compute —
    // the 28 were never counted before because only the graphics side logs itself). Four of the 67 are
    // reachable ONLY through Graphic::DebugViewState, and nothing in the player's source set writes one:
    // SceneRenderer::SetDebugView has no caller in Runtime/Source, Desert/Desert/Source or
    // Desert/Common/Source, and the fields it would carry cannot come from a .desce either (they left the
    // scene format with К2 — Desert/Tests/Engine/SceneDebugFields derives that ban from the struct). So the
    // four were built at every player's startup, held for the session, and could not be bound by anything.
    //
    // WHAT CUTTING THEM IS WORTH, and the number is small on purpose rather than by accident: on a warm
    // machine 1.1 ms of an 82 ms pipeline phase; on a COLD one 127 ms of 9 611 ms, because the cost is not
    // the pipeline object — it is the driver compiling that pipeline's shader for the first time.
    // StaticMeshWireframe shares StaticMeshLit's modules and therefore costs 0.2 ms cold; DebugLine,
    // Overdraw and OverdrawResolve own theirs and cost 43.3, 5.7 and 77.9 ms. Time is not the whole
    // argument: an instrument a player's binary cannot use is surface it should not carry.
    //
    // NOT CUT, and this is the half the note in DevInstruments.hpp got wrong: the selection-outline family
    // (Silhouette*, JFA_*) DRAWS IN A PLAYER. `MeshECSSystem` ORs a serialized per-mesh field into the
    // outline flag (`outlined = isSelected || mesh.OutlineDraw`), so any author who ticks "Draw outline" in
    // the Materials panel ships an outlined mesh; and JFA_Init and JFA_Final run on EVERY frame regardless,
    // because the composite is what hands the scene colour to tonemap.
    bool MeshRenderer::SetupDebugLinePass()
    {
        m_DebugLineShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DebugLine" );
        if ( !m_DebugLineShader )
        {
            LOG_ERROR( "Failed to load DebugLine shader" );
            return false;
        }

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName         = "DebugLinePipeline";
        spec.Shader            = m_DebugLineShader;
        spec.TargetLayout      = SceneTargetLayout();
        spec.Topology          = PrimitiveTopology::Lines;
        spec.LineWidth         = 1.0f; // dynamic line width is set to 1.0 in SubmitPulled (no wideLines feature)
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = false;
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.CullMode          = CullMode::None;
        // No vertex Layout: the DebugLine shader pulls endpoints from the Lines storage buffer by index.

        // `return m_DebugLinePipeline != nullptr;` stood at the end of this function and could not be
        // false: Create returned a make_shared. The refusal is here now, where it can happen.
        const auto debugLine = GraphicsPipeline::Create( spec );
        if ( !debugLine )
        {
            LOG_ERROR( "[MeshRenderer] debug lines will not draw: {}", debugLine.GetError() );
            return false;
        }
        m_DebugLinePipeline = debugLine.GetValue();

        m_DebugLineMaterial = std::make_unique<MaterialDebugLine>();
        return true;
    }

    bool MeshRenderer::EnsureOverdrawPass()
    {
        if ( m_OverdrawPipeline && m_OverdrawFB && m_OverdrawResolvePipeline )
            return true;
        if ( m_OverdrawRefused )
            return false;
        if ( SetupOverdrawPass() )
            return true;
        m_OverdrawRefused = true;
        LOG_WARN( "[MeshRenderer] Overdraw debug view unavailable (shaders missing)." );
        return false;
    }

    bool MeshRenderer::SetupOverdrawPass()
    {
        m_OverdrawShader        = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Overdraw" );
        m_OverdrawResolveShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "OverdrawResolve" );
        if ( !m_OverdrawShader || !m_OverdrawResolveShader )
            return false;

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        // Accumulation target: RGBA32F so many additive fragments don't clip. Cleared to 0 each frame; each
        // drawn fragment adds a small constant, so the .r channel ends up holding overdraw-count * step.
        FramebufferSpecification accumSpec;
        accumSpec.DebugName = "OverdrawAccum";
        accumSpec.Attachments.Attachments.emplace_back( Core::Formats::ImageFormat::RGBA32F );
        m_OverdrawFB = Graphic::Framebuffer::Create( accumSpec );
        m_OverdrawFB->Resize( targetFb->GetFramebufferWidth(), targetFb->GetFramebufferHeight() );

        // Geometry accumulation pipeline: static-mesh layout (same as silhouette), ADDITIVE blend, and NO
        // depth test — every fragment (even occluded ones) must count, which is exactly what overdraw measures.
        GraphicsPipelineSpecification spec;
        spec.DebugName           = "OverdrawPipeline";
        spec.Layout              = MeshVertexLayout( MeshVertexPath::Static );
        spec.Shader              = m_OverdrawShader;
        spec.Framebuffer         = m_OverdrawFB;
        spec.DepthTestEnabled    = false;
        spec.DepthWriteEnabled   = false;
        spec.CullMode            = CullMode::None;
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::One;
        spec.DstColorBlendFactor = BlendFactor::One;
        const auto overdraw      = GraphicsPipeline::Create( spec );
        if ( !overdraw )
        {
            LOG_ERROR( "[MeshRenderer] the overdraw view is off: {}", overdraw.GetError() );
            return false;
        }
        m_OverdrawPipeline = overdraw.GetValue();
        m_OverdrawMaterial = std::make_unique<MaterialOverdraw>();

        // Fullscreen resolve: heat-map the accumulation over the scene colour (LOAD so the scene shows through).
        GraphicsPipelineSpecification rspec;
        rspec.DebugName            = "OverdrawResolvePipeline";
        rspec.Shader               = m_OverdrawResolveShader;
        rspec.TargetLayout         = SceneTargetLayout();
        rspec.DepthTestEnabled     = false;
        rspec.DepthWriteEnabled    = false;
        const auto overdrawResolve = GraphicsPipeline::Create( rspec );
        if ( !overdrawResolve )
        {
            LOG_ERROR( "[MeshRenderer] the overdraw view is off: {}", overdrawResolve.GetError() );
            return false;
        }
        m_OverdrawResolvePipeline = overdrawResolve.GetValue();
        m_OverdrawResolveMaterial = std::make_unique<MaterialOverdrawResolve>();

        return true;
    }

    void MeshRenderer::DeclareOverdrawDraws( RDG::PassBuilder& pass )
    {
        m_OverdrawDraws.Clear();
        auto* const      camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        const ViewFrame* view   = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        if ( !m_OverdrawPipeline || !m_OverdrawFB || !m_OverdrawResolvePipeline || camera == nullptr ||
             view == nullptr )
            return;

        // 1) Accumulate into m_OverdrawFB (the graph opens it cleared to 0): every opaque mesh additively
        //    (static + generic; both use the static vertex layout). Skinned meshes are skipped — they'd need
        //    the skinned layout + bone SSBO.
        m_OverdrawMaterial->UpdateCamera( *view );

        // CULLED LIKE THE PASS IT REPORTS ON. This view exists to answer "how many times was this
        // pixel shaded", and an uncalled re-rasterization would answer it about a frame the engine
        // does not draw — an instrument disagreeing with the thing it measures, which is the defect
        // shape this repository keeps finding rather than a conservative choice.
        const Core::Frustum     overdrawFrustum = camera->GetFrustum();
        const MaterialExecutor* executor        = m_OverdrawMaterial->GetMaterialExecutor();
        const GraphicsPipeline* pipeline        = m_OverdrawPipeline.get();
        for ( const auto& rd : m_StaticQueue )
        {
            if ( rd.Mesh != nullptr && IsVisibleInView( overdrawFrustum, rd.Transform,
                                                        Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) ) )
            {
                m_OverdrawDraws.Add(
                     { .Pipeline = pipeline, .Mesh = rd.Mesh, .Transform = rd.Transform, .Material = executor } );
            }
        }
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh != nullptr &&
                 IsVisibleInView( overdrawFrustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
            {
                m_OverdrawDraws.Add(
                     { .Pipeline = pipeline, .Mesh = g.Mesh, .Transform = g.Transform, .Material = executor } );
            }
        }
        // The accumulation shader samples no scene/view input.
        m_OverdrawDraws.Declare( pass, std::nullopt );
    }

    Common::BoolResultStr MeshRenderer::RenderOverdrawAccumManual( const RDG::PassContext& context ) const
    {
        return m_OverdrawDraws.Record( context );
    }

    void MeshRenderer::DeclareOverdrawResolve( RDG::PassBuilder& pass, const RDG::TextureRef overdraw )
    {
        if ( !m_OverdrawPipeline || !m_OverdrawFB || !m_OverdrawResolvePipeline )
            return;
        // 2) Resolve: heat-map the accumulation over the scene colour (the graph opens the target with LOAD;
        //    the resolve discards empty texels). The resolve's one block: the shader's reflected layout, the
        //    material as the other route, and the accumulation as OverdrawTexture with the sampler the material route
        //    sampled it with (the image's own: linear, REPEAT).
        const MaterialExecutor& executor = *m_OverdrawResolveMaterial->GetMaterialExecutor();
        pass.Prepares( executor );
        pass.Bindings( m_OverdrawResolveLayout.Get( m_OverdrawResolvePipeline->GetSpecification().Shader ),
                       executor.GetRouteFill() )
             .Texture( MeshDebugOverdrawResolveShaderParameters::OverdrawTexture, overdraw,
                       RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Sampler( MeshDebugOverdrawResolveShaderParameters::LinearRepeatSampler,
                       RDG::SamplerDesc::LinearRepeat() );
    }

    Common::BoolResultStr MeshRenderer::RecordOverdrawResolve( const RDG::PassContext& context ) const
    {
        if ( !m_OverdrawPipeline || !m_OverdrawFB || !m_OverdrawResolvePipeline )
            return BOOLSUCCESS;
        // Through the block DeclareOverdrawResolve declared.
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_OverdrawResolvePipeline,
                                                       m_OverdrawResolveMaterial->GetMaterialExecutor() );
    }

    SystemRasterPass MeshRenderer::DebugLinesPass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_DebugLinePipeline )
            return {};

        // Overlay debug lines (AABB wireframes) over the lit scene; runs after Geometry, depth-tested.
        // RENDER-THREADS M1 (UE: a pass's buffers are filled before the graph records): the lines are built and
        // written into the line material in the node's setup - the write may grow its storage buffer, which drops
        // the view copies - and the material is prepared, so the exec only draws.
        SystemRasterPass pass{
             .Name        = "DebugLinesPass",
             .ExecuteFunc = [this]( RDG::PassContext&, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 if ( m_DebugLineVertices == 0 )
                     return BOOLSUCCESS;
                 Renderer::GetInstance().SubmitPulled( m_DebugLinePipeline.get(), m_DebugLineVertices,
                                                       m_BoundingBoxLineWidth,
                                                       m_DebugLineMaterial->GetMaterialExecutor() );
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            m_DebugLineVertices = 0;
            if ( !m_ShowBoundingBoxes && !m_ShowCardPlacement )
                return;
            auto* const      camera = m_SceneRenderer->GetMainCamera();
            const ViewFrame* view   = m_SceneRenderer->GetViewFrame();
            if ( camera == nullptr || view == nullptr )
                return;

            // 12 box edges as index pairs into the 8 AABB corners (index bits = x|y<<1|z<<2).
            static const int kEdges[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 },
                                               { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            const glm::vec4  color( m_BoundingBoxColor, 1.0f );

            std::vector<MaterialDebugLine::LineVertex> lines;
            const auto addBox = [&lines]( const glm::vec3& mn, const glm::vec3& mx, const glm::mat4& world,
                                          const glm::vec4& boxColor )
            {
                glm::vec3 c[8] = { { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mn.x, mx.y, mn.z },
                                   { mx.x, mx.y, mn.z }, { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
                                   { mn.x, mx.y, mx.z }, { mx.x, mx.y, mx.z } };
                for ( auto& corner : c )
                    corner = glm::vec3( world * glm::vec4( corner, 1.0f ) );
                for ( const auto& e : kEdges )
                {
                    lines.push_back( { glm::vec4( c[e[0]], 1.0f ), boxColor } );
                    lines.push_back( { glm::vec4( c[e[1]], 1.0f ), boxColor } );
                }
            };
            // CARD PLACEMENT: each card's box in its mesh's space (the card build applied every submesh's
            // transform, as the distance field does), coloured by the axis it faces - X red, Y green, Z
            // blue, the negative direction darker.
            const auto addCards = [&addBox]( const Mesh& mesh, const glm::mat4& world )
            {
                const auto cards = mesh.GetCards();
                if ( !cards )
                    return;
                for ( const Assets::CardBuildData& card : cards->Cards )
                {
                    const uint32_t direction = static_cast<uint32_t>( card.Direction );
                    glm::vec4      cardColor( 0.0f, 0.0f, 0.0f, 1.0f );
                    cardColor[direction / 2] = ( direction & 1u ) != 0u ? 1.0f : 0.45f;
                    addBox( card.Center - card.Extent, card.Center + card.Extent, world, cardColor );
                }
            };
            if ( m_ShowCardPlacement )
            {
                for ( const auto& rd : m_StaticQueue )
                    if ( rd.Mesh != nullptr )
                        addCards( *rd.Mesh, rd.Transform );
                for ( const auto& ism : m_InstancedQueue )
                    if ( ism.Mesh != nullptr && ism.Transforms )
                        for ( const glm::mat4& world : *ism.Transforms )
                            addCards( *ism.Mesh, world );
            }
            for ( const auto& rd : m_StaticQueue )
            {
                if ( rd.Mesh == nullptr || !m_ShowBoundingBoxes )
                    continue;
                for ( const auto& sm : rd.Mesh->GetSubmeshes() )
                {
                    const glm::vec3 mn   = sm.BoundingBox.Min;
                    const glm::vec3 mx   = sm.BoundingBox.Max;
                    glm::vec3       c[8] = { { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mn.x, mx.y, mn.z },
                                             { mx.x, mx.y, mn.z }, { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
                                             { mn.x, mx.y, mx.z }, { mx.x, mx.y, mx.z } };
                    const glm::mat4 world = rd.Transform * sm.Transform;
                    for ( auto& corner : c )
                        corner = glm::vec3( world * glm::vec4( corner, 1.0f ) );
                    for ( const auto& e : kEdges )
                    {
                        lines.push_back( { glm::vec4( c[e[0]], 1.0f ), color } );
                        lines.push_back( { glm::vec4( c[e[1]], 1.0f ), color } );
                    }
                }
            }
            if ( lines.empty() )
                return;

            m_DebugLineMaterial->Update( *view, lines );
            declared.Prepares( *m_DebugLineMaterial->GetMaterialExecutor() );
            m_DebugLineVertices = static_cast<uint32_t>( lines.size() );
        };
        return pass;
    }

    namespace
    {
        // The program's shading model by name, from the one loaded set (ShaderRootShadingModels, the set the
        // parser resolved it from); empty for a program that is no surface template.
        std::string DescribeShadingModelName( const Core::Formats::ShaderProgramMeta& meta )
        {
            if ( meta.ShadingModel.IsNull() )
                return {};
            const Core::ShadingModels::ShaderRootSet held = Core::ShadingModels::ShaderRootShadingModels();
            if ( !held->IsSuccess() )
                return "<shading models did not load>";
            const Core::ShadingModels::ShadingModelEntry* entry =
                 held->GetValue().Registry.FindByGuid( meta.ShadingModel );
            return entry != nullptr ? std::string( entry->Manifest.Name ) : std::string( "<unknown model>" );
        }

        // What ShadingOf answered, as the readout's values.
        void DescribeShading( const MeshRenderer::SectionShading& shading, PrimitiveDrawReadout& out )
        {
            if ( shading.Instance != nullptr )
                out.MaterialInstance = shading.Instance->GetName();
            if ( shading.Surface == nullptr )
                return;
            const Core::Formats::ShaderProgramMeta& meta = shading.Surface->GetSchema();
            out.Template                                 = shading.Surface->GetShaderName();
            out.Blend                                    = meta.Blend;
            out.ShadingModel                             = DescribeShadingModelName( meta );
        }
    } // namespace

    std::vector<PrimitiveDrawReadout> MeshRenderer::DescribePrimitive( const uint32_t entity ) const
    {
        using Pass = PrimitiveDrawReadout::Pass;
        std::vector<PrimitiveDrawReadout> draws;

        const Core::Camera* camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        const std::optional<Core::Frustum> frustum =
             camera != nullptr ? std::optional<Core::Frustum>( camera->GetFrustum() ) : std::nullopt;
        // The path test the passes make (GetViewTrianglesDrawn, DeclareGBufferDraws): on Deferred the lit, skinned
        // and instanced queues are drawn into the G-buffer (UE's base pass), else by the forward pass.
        const bool deferred = m_SceneRenderer != nullptr &&
                              m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred &&
                              m_StaticGBufferPipeline != nullptr;
        const Pass opaquePass = deferred ? Pass::BasePass : Pass::Forward;

        // Bounds, frustum and level count of one placement of @p mesh (the authored box every cull reads).
        const auto place =
             [&frustum]( PrimitiveDrawReadout& out, const glm::mat4& transform, const Desert::Mesh& mesh )
        {
            const Common::Math::AABB local = Geometry::LocalBounds( mesh.GetSubmeshes() );
            out.WorldBounds                = Geometry::TransformBounds( transform, local );
            out.InFrustum                  = frustum.has_value() && IsVisibleInView( *frustum, transform, local );
            out.LodCount = std::max<uint32_t>( 1u, static_cast<uint32_t>( mesh.GetLODScreenSizes().size() ) );
        };

        for ( const StaticMeshRenderData& data : m_StaticQueue )
        {
            if ( data.Entity != entity || data.Mesh == nullptr )
                continue;
            PrimitiveDrawReadout& out    = draws.emplace_back();
            out.Kind                     = PrimitiveDrawReadout::Queue::Static;
            out.SectionSlot              = data.SectionSlot;
            out.ForcedLod                = data.ForcedLOD;
            out.LodBias                  = data.LODBias;
            out.Lod                      = ComputeLOD( data.Transform, data.Mesh, data.ForcedLOD, data.LODBias );
            const SectionShading shading = ShadingOf( data );
            DescribeShading( shading, out );
            out.ShadingPass  = shading.Translucent ? Pass::Translucency : opaquePass;
            out.CastShadows  = data.CastShadows;
            out.GpuPrimitive = data.GpuPrimitive;
            place( out, data.Transform, *data.Mesh );
        }

        for ( const SkinnedMeshRenderData& data : m_SkinnedQueue )
        {
            if ( data.Entity != entity || data.Mesh == nullptr )
                continue;
            // One draw per section, each with its own material (BuildSkinnedDraws); the skin pass draws LOD 0.
            for ( uint32_t section = 0; section < data.Sections.size(); ++section )
            {
                PrimitiveDrawReadout& out    = draws.emplace_back();
                out.Kind                     = PrimitiveDrawReadout::Queue::Skinned;
                out.SectionSlot              = section;
                const SectionShading shading = ShadingOf( data.Sections[section] );
                DescribeShading( shading, out );
                out.ShadingPass  = shading.Translucent ? Pass::Translucency : opaquePass;
                out.CastShadows  = data.CastShadows;
                out.GpuPrimitive = data.GpuPrimitive;
                place( out, data.Transform, *data.Mesh );
            }
        }

        auto* const shaders      = Runtime::ResourceRegistry::GetShaderService();
        const auto  isRegistered = [shaders]( std::string_view name )
        { return shaders->GetByName( std::string( name ) ) != nullptr; };
        for ( const GenericMeshRenderData& data : m_GenericQueue )
        {
            if ( data.Entity != entity || data.Mesh == nullptr )
                continue;
            PrimitiveDrawReadout& out = draws.emplace_back();
            out.Kind                  = PrimitiveDrawReadout::Queue::Generic;
            // The program BuildGenericDraws resolves: the per-slot material's shader, else the Shader Override's.
            const auto* slotSurface = dynamic_cast<const DataDrivenMaterial*>( data.SlotMaterial );
            out.Template            = slotSurface != nullptr ? slotSurface->GetShaderName() : data.ShaderName;
            if ( const auto shader = shaders->GetByName( out.Template ) )
            {
                const Core::Formats::ShaderProgramMeta& meta = shader->GetProgramMeta();
                out.Blend                                    = meta.Blend;
                out.ShadingModel                             = DescribeShadingModelName( meta );
            }
            // A Shader Override naming a surface template is drawn in the G-buffer on Deferred (the slice
            // DeferredSurfaceGBuffer); every other generic draw shades forward over the composite.
            const bool surfaceOverride =
                 data.SlotMaterial == nullptr &&
                 TemplateCellShader( out.Template, MeshVertexPath::Static, MeshPass::GBuffer, isRegistered )
                      .has_value();
            out.ShadingPass  = deferred && surfaceOverride ? Pass::BasePass : Pass::Forward;
            out.Lod          = ComputeLOD( data.Transform, data.Mesh, /*forced*/ -1 );
            out.CastShadows  = data.CastShadows;
            out.GpuPrimitive = data.GpuPrimitive;
            place( out, data.Transform, *data.Mesh );
        }

        for ( const InstancedMeshRenderData& data : m_InstancedQueue )
        {
            if ( data.Entity != entity || data.Mesh == nullptr || !data.Transforms )
                continue;
            PrimitiveDrawReadout& out    = draws.emplace_back();
            out.Kind                     = PrimitiveDrawReadout::Queue::Instanced;
            out.LodPerInstance           = true;
            const SectionShading shading = ShadingOf( data );
            DescribeShading( shading, out );
            out.ShadingPass = shading.Translucent ? Pass::Translucency : opaquePass;
            out.CastShadows = data.CastShadows;
            out.Instances   = static_cast<uint32_t>( data.Transforms->size() );
            // The field's box is the union of its instances'; it is in the frustum when any instance is.
            out.WorldBounds = Common::Math::AABB{ glm::vec3( Geometry::kNoBoundsSentinel ),
                                                  glm::vec3( -Geometry::kNoBoundsSentinel ) };
            for ( const glm::mat4& transform : *data.Transforms )
            {
                PrimitiveDrawReadout one;
                place( one, transform, *data.Mesh );
                if ( !Geometry::IsEmpty( one.WorldBounds ) )
                {
                    out.WorldBounds.Min = glm::min( out.WorldBounds.Min, one.WorldBounds.Min );
                    out.WorldBounds.Max = glm::max( out.WorldBounds.Max, one.WorldBounds.Max );
                }
                out.InFrustum = out.InFrustum || one.InFrustum;
                out.LodCount  = one.LodCount;
            }
        }
        return draws;
    }
#endif // DESERT_DEV_INSTRUMENTS

    SystemRasterPass MeshRenderer::SilhouettePass()
    {
        // The mask target AND the pipeline that writes it: the second half is new, because
        // SetupSilhouettePass can now refuse and this function reads the pipeline's spec.
        if ( !m_SilhouetteMaskFramebuffer || !m_SilhouettePipeline )
            return {};

        SystemRasterPass pass{
             .Name        = "MeshSilhouettePass",
             .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 // The masks this node's Declare chose.
                 return m_SilhouetteDraws.Record( context );
             },
             .TargetFramebuffer = m_SilhouetteMaskFramebuffer };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            // SETUP: every outlined mesh is chosen here, ONCE, into the node's draw list (the mask cameras and the
            // packed poses written before any command is recorded); one block per mask material.
            m_SilhouetteDraws.Clear();
            auto* const      camera = m_SceneRenderer->GetMainCamera();
            const ViewFrame* view   = m_SceneRenderer->GetViewFrame();
            if ( camera == nullptr || view == nullptr )
                return;
            m_SilhouetteMaterial->UpdateCamera( *view );
            const MaterialExecutor* mask         = m_SilhouetteMaterial->GetMaterialExecutor();
            const GraphicsPipeline* maskPipeline = m_SilhouettePipeline.get();

            // ===== Static =====
            for ( const auto& renderData : m_StaticQueue )
            {
                if ( !renderData.Outlined || renderData.Mesh == nullptr )
                    continue;
                m_SilhouetteDraws.Add( { .Pipeline  = maskPipeline,
                                         .Mesh      = renderData.Mesh,
                                         .Transform = renderData.Transform,
                                         .Material  = mask } );
            }

            // ===== Generic (data-driven materials) — same Silhouette pipeline, the
            // material's shader is irrelevant for the mask (just geometry + transform).
            for ( const auto& g : m_GenericQueue )
            {
                if ( !g.Outlined || g.Mesh == nullptr )
                    continue;
                m_SilhouetteDraws.Add(
                     { .Pipeline = maskPipeline, .Mesh = g.Mesh, .Transform = g.Transform, .Material = mask } );
            }

            // ===== Skinned ===== — skin the mask by the SAME bone matrices the mesh is
            // rendered with (animated or bind) so the outline tracks the posed shape.
            if ( m_SilhouetteSkinnedPipeline && m_SilhouetteSkinnedMaterial )
            {
                // Poses packed once, sliced per draw — the shape every skinned path in
                // this renderer now shares. Uploading inside the loop meant a
                // multi-selection of skinned meshes outlined them all in the last
                // one's pose.
                auto& outlineBones = m_ScratchBones;
                outlineBones.clear();
                std::vector<std::pair<const SkinnedMeshRenderData*, uint32_t>> outlined;
                for ( const auto& sd : m_SkinnedQueue )
                {
                    if ( !sd.Outlined || sd.Mesh == nullptr || sd.BoneMatrices.empty() )
                        continue;
                    outlined.emplace_back( &sd, static_cast<uint32_t>( outlineBones.size() ) );
                    outlineBones.insert( outlineBones.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
                }
                if ( !outlined.empty() )
                {
                    m_SilhouetteSkinnedMaterial->UpdateCamera( *view );
                    m_SilhouetteSkinnedMaterial->UploadBones( outlineBones );
                    auto* const skinnedMask = m_SilhouetteSkinnedMaterial.get();
                    for ( const auto& [sd, boneOffset] : outlined )
                    {
                        // The pose slice (or the skin cache's streams) is the draw's own state, set right before
                        // it.
                        const GpuDriven::GpuScenePrimitive row =
                             m_GpuScene != nullptr && m_GpuScene->IsValid( sd->GpuPrimitive )
                                  ? m_GpuScene->Primitive( sd->GpuPrimitive )
                                  : GpuDriven::GpuScenePrimitive{};
                        const uint64_t address =
                             ( static_cast<uint64_t>( row.SkinCacheAddressHi ) << 32u ) | row.SkinCacheAddressLo;
                        m_SilhouetteDraws.Add( { .Pipeline  = m_SilhouetteSkinnedPipeline.get(),
                                                 .Mesh      = sd->Mesh,
                                                 .Transform = sd->Transform,
                                                 .Material  = skinnedMask->GetMaterialExecutor(),
                                                 .Push      = DrawPushFields{}
                                                             .BoneOffset( boneOffset, /*required*/ false )
                                                             .SkinCache( address, row.SkinCacheVertexCount ) } );
                    }
                }
            }
            // The mask shaders sample no scene/view input.
            m_SilhouetteDraws.Declare( declared, std::nullopt );
            // The skin cache's streams the skinned masks fetch (after "SkinCache: Skin" wrote them).
            for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                declared.Read( streams, RDG::Access::StorageRead );
        };
        return pass;
    }

} // namespace Desert::Graphic::System
