#include "MeshRenderer.hpp"
#include "MeshRendererInternal.hpp"

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

        // Appends one row to a buffer of rows laid end to end and returns its index there. Every PBR pass
        // declares one layout, so every row in a buffer has the same length.
        uint32_t AppendRow( std::vector<glm::vec4>& rows, const Core::Formats::MaterialParamRow& row )
        {
            const auto index = row.empty() ? 0u : static_cast<uint32_t>( rows.size() / row.size() );
            rows.insert( rows.end(), row.begin(), row.end() );
            return index;
        }

        PBRSlot FirstPBRSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path )
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
        TakeShadowBudget( m_SceneRenderer ? m_SceneRenderer->GetShadowQuality() : ShadowQuality{} );

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
                           // Forward path only. In Deferred, meshes are drawn into the G-buffer by
                           // MeshGBufferPass instead (this target keeps sky/grid/terrain for compositing).
                           if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred &&
                                m_StaticGBufferPipeline )
                               return BOOLSUCCESS;

                           const auto camera = m_SceneRenderer->GetMainCamera();
                           if ( !camera )
                               return BOOLSUCCESS;

                           // `UpdateGlobalUniforms( camera, points, directionals )` used to be called
                           // here. Its entire body was `if ( !camera ) return;` — it read neither light
                           // set, which is what `-Wunused-parameter` reported about both. The lights
                           // reach the shaders through the material executors' uniform blocks, and the
                           // two `GetXLights()` calls that fed this one were a per-frame walk of the
                           // scene's light components for nothing.
                           // The cloud layer's shadow map is a pass parameter of this node (declared below).
                           const MeshPassBindings pass( context, SceneViewInputsOf( refs ) );
                           if ( auto drawn = DrawStaticMeshes( pass ); !drawn.IsSuccess() )
                               return drawn;
                           if ( auto drawn = DrawSkinnedMeshes( /*useLoadPass*/ false, pass ); !drawn.IsSuccess() )
                               return drawn;
                           return DrawGenericMeshes( /*useLoadPass*/ false, pass );
                       },
                       m_StaticPipeline->GetSpecification(), targetFb,
                       { RenderPassDependency( RenderPhase::DepthPrePass ) } )
             .Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            // The scene/view inputs the lit draws sample (SceneViewInputs: cascades, environment cubes, BRDF LUT,
            // cloud shadow map), each a pass parameter the body binds.
            for ( const RDG::TextureRef input : SceneViewInputsOf( refs ).Refs() )
                declared.Read( input, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() );
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
                staticData.TranslucencySortPriority = data.TranslucencySortPriority;

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
                    // a second loop hunting a different C++ CLASS, and since the material build could not
                    // produce that class from an asset under any circumstances, an imported character
                    // with its own materials matched nothing and was dropped without drawing.
                    if ( const auto slot = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Skinned ) )
                    {
                        skinnedData.Instance = slot.Instance;
                        skinnedData.Material = slot.Surface;
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
