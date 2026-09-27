// MeshRenderer's overlays: the selection-silhouette mask and the developer instruments (debug lines,
// overdraw) that only DESERT_DEV_INSTRUMENTS builds carry.
#include "MeshRendererInternal.hpp"

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
        spec.Layout    = { { Graphic::ShaderDataType::Float3, "a_Position" },
                           { Graphic::ShaderDataType::Float3, "a_Normal" },
                           { Graphic::ShaderDataType::Float3, "a_Tangent" },
                           { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                           { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };

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
            sspec.Layout                        = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                                    { Graphic::ShaderDataType::Float3, "a_Normal" },
                                                    { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                                    { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                                    { Graphic::ShaderDataType::Float2, "a_TextureCoord" },
                                                    { Graphic::ShaderDataType::Int4, "a_BoneIndices" },
                                                    { Graphic::ShaderDataType::Float4, "a_BoneWeights" } };
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
    // StaticMeshWireframe shares StaticMeshPBR's modules and therefore costs 0.2 ms cold; DebugLine,
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
        spec.Framebuffer       = targetFb;
        spec.Topology          = PrimitiveTopology::Lines;
        spec.LineWidth         = 1.0f; // dynamic line width is set to 1.0 in SubmitLines (no wideLines feature)
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
        accumSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F );
        m_OverdrawFB = Graphic::Framebuffer::Create( accumSpec );
        m_OverdrawFB->Resize( targetFb->GetFramebufferWidth(), targetFb->GetFramebufferHeight() );

        // Geometry accumulation pipeline: static-mesh layout (same as silhouette), ADDITIVE blend, and NO
        // depth test — every fragment (even occluded ones) must count, which is exactly what overdraw measures.
        GraphicsPipelineSpecification spec;
        spec.DebugName           = "OverdrawPipeline";
        spec.Layout              = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                     { Graphic::ShaderDataType::Float3, "a_Normal" },
                                     { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                     { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                     { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };
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
        rspec.Framebuffer          = targetFb;
        rspec.DepthTestEnabled     = false;
        rspec.DepthWriteEnabled    = false;
        rspec.UseLoadRenderPass    = true;
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

    void MeshRenderer::RenderOverdrawManual()
    {
        if ( !m_OverdrawPipeline || !m_OverdrawFB || !m_OverdrawResolvePipeline )
            return;
        const auto& target = m_SceneRenderer ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        const auto  camera = m_SceneRenderer ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( !target || !camera )
            return;

        auto& renderer = Renderer::GetInstance();

        // 1) Accumulate: clear to 0, then draw every opaque mesh additively (static + generic; both use the
        //    static vertex layout). Skinned meshes are skipped — they'd need the skinned layout + bone SSBO.
        {
            RenderPassSpecification rpSpec;
            rpSpec.TargetFramebuffer = m_OverdrawFB;
            rpSpec.DebugName         = "OverdrawAccumPass";
            rpSpec.ClearColor.Color  = glm::vec4( 0.0f );
            auto rp                  = RenderPass::Create( rpSpec );

            renderer.BeginRenderPass( rp.get() );
            m_OverdrawMaterial->UpdateCamera( camera );

            // CULLED LIKE THE PASS IT REPORTS ON. This view exists to answer "how many times was this
            // pixel shaded", and an uncalled re-rasterization would answer it about a frame the engine
            // does not draw — an instrument disagreeing with the thing it measures, which is the defect
            // shape this repository keeps finding rather than a conservative choice.
            const Core::Frustum overdrawFrustum = camera->GetFrustum();
            for ( const auto& rd : m_StaticQueue )
                if ( rd.Mesh != nullptr && IsVisibleInView( overdrawFrustum, rd.Transform,
                                                            Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) ) )
                    renderer.RenderMesh( m_OverdrawPipeline.get(), rd.Mesh, rd.Transform,
                                         m_OverdrawMaterial->GetMaterialExecutor() );
            for ( const auto& g : m_GenericQueue )
                if ( g.Mesh != nullptr && IsVisibleInView( overdrawFrustum, g.Transform,
                                                           Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
                    renderer.RenderMesh( m_OverdrawPipeline.get(), g.Mesh, g.Transform,
                                         m_OverdrawMaterial->GetMaterialExecutor() );
            renderer.EndRenderPass();
        }

        // 2) Resolve: heat-map the accumulation over the scene colour (LOAD; the resolve discards empty texels).
        {
            RenderPassSpecification rpSpec;
            rpSpec.TargetFramebuffer = target;
            rpSpec.DebugName         = "OverdrawResolvePass";
            auto rp                  = RenderPass::Create( rpSpec );

            renderer.BeginRenderPass( rp.get(), false );
            m_OverdrawResolveMaterial->BindInputs( m_OverdrawFB->GetColorAttachmentImage( 0 ) );
            renderer.SubmitFullscreenQuad( m_OverdrawResolvePipeline.get(),
                                           m_OverdrawResolveMaterial->GetMaterialExecutor() );
            renderer.EndRenderPass();
        }
    }

    void MeshRenderer::RegisterDebugPass( RenderGraphBuilder& builder )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_DebugLinePipeline )
            return;

        // Overlay debug lines (AABB wireframes) over the lit scene; runs after Geometry, depth-tested.
        builder.AddPass(
             "DebugLinesPass", RenderPhase::Debug,
             [this]()
             {
                 if ( !m_ShowBoundingBoxes )
                     return;
                 const auto camera = m_SceneRenderer->GetMainCamera();
                 if ( !camera )
                     return;

                 // 12 box edges as index pairs into the 8 AABB corners (index bits = x|y<<1|z<<2).
                 static const int kEdges[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 },
                                                    { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
                 const glm::vec4  color( m_BoundingBoxColor, 1.0f );

                 std::vector<MaterialDebugLine::LineVertex> lines;
                 for ( const auto& rd : m_StaticQueue )
                 {
                     if ( !rd.Mesh )
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

                 m_DebugLineMaterial->Update( camera, lines );
                 Renderer::GetInstance().SubmitLines(
                      m_DebugLinePipeline.get(), static_cast<uint32_t>( lines.size() ), m_BoundingBoxLineWidth,
                      m_DebugLineMaterial->GetMaterialExecutor() );
             },
             m_DebugLinePipeline->GetSpecification(), targetFb,
             { RenderPassDependency( RenderPhase::Geometry ) } );
    }
#endif // DESERT_DEV_INSTRUMENTS

    void MeshRenderer::RegisterSilhouettePass( RenderGraphBuilder& builder )
    {
        // The mask target AND the pipeline that writes it: the second half is new, because
        // SetupSilhouettePass can now refuse and this function reads the pipeline's spec.
        if ( !m_SilhouetteMaskFramebuffer || !m_SilhouettePipeline )
            return;

        builder.AddPass(
             "MeshSilhouettePass", RenderPhase::Outline,
             [this]()
             {
                 const auto camera = m_SceneRenderer->GetMainCamera();
                 if ( !camera )
                     return;

                 auto& renderer = Renderer::GetInstance();
                 m_SilhouetteMaterial->UpdateCamera( camera );

                 // ===== Static =====
                 for ( const auto& renderData : m_StaticQueue )
                 {
                     if ( !renderData.Outlined || !renderData.Mesh )
                         continue;

                     renderer.RenderMesh( m_SilhouettePipeline.get(), renderData.Mesh, renderData.Transform,
                                          m_SilhouetteMaterial->GetMaterialExecutor() );
                 }

                 // ===== Generic (data-driven materials) — same Silhouette pipeline, the
                 // material's shader is irrelevant for the mask (just geometry + transform).
                 for ( const auto& g : m_GenericQueue )
                 {
                     if ( !g.Outlined || !g.Mesh )
                         continue;
                     renderer.RenderMesh( m_SilhouettePipeline.get(), g.Mesh, g.Transform,
                                          m_SilhouetteMaterial->GetMaterialExecutor() );
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
                         if ( !sd.Outlined || !sd.Mesh || sd.BoneMatrices.empty() )
                             continue;
                         outlined.emplace_back( &sd, static_cast<uint32_t>( outlineBones.size() ) );
                         outlineBones.insert( outlineBones.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
                     }
                     if ( !outlined.empty() )
                     {
                         m_SilhouetteSkinnedMaterial->UpdateCamera( camera );
                         m_SilhouetteSkinnedMaterial->UploadBones( outlineBones );
                         for ( const auto& [sd, boneOffset] : outlined )
                         {
                             m_SilhouetteSkinnedMaterial->SetBoneOffset( boneOffset );
                             renderer.RenderMesh( m_SilhouetteSkinnedPipeline.get(), sd->Mesh, sd->Transform,
                                                  m_SilhouetteSkinnedMaterial->GetMaterialExecutor() );
                         }
                     }
                 }
             },
             m_SilhouettePipeline->GetSpecification(), m_SilhouetteMaskFramebuffer,
             { RenderPassDependency( RenderPhase::Geometry ) } );
    }

} // namespace Desert::Graphic::System
