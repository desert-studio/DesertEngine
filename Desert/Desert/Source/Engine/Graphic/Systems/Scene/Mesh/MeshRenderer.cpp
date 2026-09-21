#include "MeshRenderer.hpp"
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/PBRPush.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
// MeshShaderFor / MeshVertexPath / MeshPass — the (path x pass) table this file asks for its pipelines.
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <variant>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <unordered_set>

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
                        const glm::vec4 t = vec4Of( v, glm::vec4( data.UVTiling.value_or( glm::vec2( 1.0f ) ), 0, 0 ) );
                        data.UVTiling     = glm::vec2( t );
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
        // The texture half of a shared generic material's identity.
        //
        // Parameters became per-draw rows; SAMPLERS did not and cannot, because a sampler is a descriptor
        // and a descriptor set belongs to the material every draw of the group binds. So two draws may
        // share a material only if they want the same textures, and this spells "the same textures" as a
        // key: the sorted asset handles of the overrides plus the address of any runtime-owned texture
        // (the text system's font atlas, which has no handle). Sorted, because two entities that named
        // the same textures in a different order are the same texture set and must batch as one.
        std::string GenericTextureKey( const MeshRenderer::GenericMeshRenderData& g )
        {
            if ( g.SlotMaterial )
                return {}; // the material IS the asset; it is already its own key

            std::vector<std::string> parts;
            parts.reserve( g.Overrides.Textures.size() + 1 );
            for ( const auto& [name, handle] : g.Overrides.Textures )
                if ( handle != 0 )
                    parts.push_back( name + "=" + std::to_string( handle ) );
            if ( g.DirectTexture && !g.DirectTextureSampler.empty() )
                parts.push_back( g.DirectTextureSampler + "=@" +
                                 std::to_string( reinterpret_cast<uintptr_t>( g.DirectTexture ) ) );
            std::sort( parts.begin(), parts.end() );

            std::string key;
            for ( const auto& part : parts )
                key += "|" + part;
            return key;
        }

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
    } // namespace

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

        if ( !SetupDebugLinePass() )
            return Common::MakeError( "Failed to setup debug line pass" );

        // Overdraw is an optional debug view — never fatal if its shaders are missing.
        if ( !SetupOverdrawPass() )
            LOG_WARN( "[MeshRenderer] Overdraw debug view unavailable (shaders missing)." );

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

    void MeshRenderer::DrawGenericMeshes( bool useLoadPass )
    {
        if ( m_GenericQueue.empty() )
            return;

        const auto  targetFb = m_TargetFramebuffer.lock();
        const auto* camera   = m_SceneRenderer->GetMainCamera();
        if ( !targetFb || !camera )
            return;

        // THE SAME per-frame scene snapshot the PBR queue is drawn with — camera, lights, cascades, the
        // baked environment and the cloud shadow — gathered once here as it is there.
        //
        // What stood in its place was three hand-written fills (CameraUB, TimeUB, DirectionLightsUB) and
        // nothing else, so a custom-shader mesh could not receive the environment cubes, the light counts,
        // the point and spot buffers or the cloud shadow map at all. A lit shader-graph material was
        // therefore obliged to invent a lighting model out of the one light payload it could see, which is
        // exactly what it did: a flat ambient constant, an unnormalized Lambert and full sun under a
        // cloud. Every one of those blocks is bound by NAME and guarded, so this costs the shaders that
        // do not declare them nothing.
        const PBRSceneFrame frameState = CaptureFrameState( camera );

        const Core::Frustum frustum = camera->GetFrustum();

        const VertexBufferLayout meshLayout = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                                { Graphic::ShaderDataType::Float3, "a_Normal" },
                                                { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                                { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                                { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };

        // Reused across frames like every other accumulator in this file — this runs once per frame per
        // submesh group, and two fresh vectors a frame is the allocation churn the optimization workflow
        // says not to write.
        auto& draws          = m_ScratchGenericDraws;
        auto& rowsByMaterial = m_ScratchGenericRows;
        draws.clear();
        rowsByMaterial.clear();

        const auto rowsFor = [&rowsByMaterial]( DataDrivenMaterial* mat ) -> MaterialRows&
        {
            for ( auto& [m, r] : rowsByMaterial )
                if ( m == mat )
                    return r;
            rowsByMaterial.emplace_back( mat, MaterialRows{} );
            return rowsByMaterial.back().second;
        };

        for ( const auto& g : m_GenericQueue )
        {
            // CULLED ON THE AUTHORED BOX, and that is only sound because no vertex stage in this tree
            // moves a vertex off it. A data-driven material whose vertex stage displaced geometry — a
            // world-position offset, the thing UE hands a "bounds scale" knob for — would be culled on
            // a box it is allowed to leave, and would pop out of existence for reasons invisible in the
            // scene. The shader graph emits a FRAGMENT body only; its vertex stage is one shared
            // generated include (Common/GraphVertex.glslh) that transforms a_Position and nothing else,
            // and Desert/Tests/Engine/FrustumCulling asserts that over every Surface-domain shader in
            // the tree. The day a vertex-offset node exists, that census goes red before this does.
            if ( g.Mesh != nullptr &&
                 !IsVisibleInView( frustum, g.Transform, Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
                continue;

            // Per-slot draws carry their own material (asset params already applied at build);
            // Shader Override draws use a shader-keyed shared material + per-frame overrides.
            DataDrivenMaterial* material   = nullptr;
            std::string         shaderName = g.ShaderName;
            if ( g.SlotMaterial )
            {
                material   = dynamic_cast<DataDrivenMaterial*>( g.SlotMaterial );
                if ( material )
                    shaderName = material->GetShaderName();
            }

            auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
            // A shader whose first compile failed is still registered under its name, so GetByName hands
            // it back like any other. Skip its draws silently: the compilation error was already logged
            // once, with the file and line, and repeating it every frame for every mesh would bury it.
            if ( !shader || !shader->IsCompiled() || !g.Mesh || ( g.SlotMaterial && !material ) )
                continue;

            // ── The domain gate ────────────────────────────────────────────────────────────────
            //
            // This path rasterizes ONE domain. A material naming a shader of any other domain used to
            // draw here, silently and wrongly, and NOTHING below this point was ever going to object.
            //
            // What was measured, with a `.demat` naming the Terrain shader in a mesh slot: the submesh
            // reached this loop, left the batched PBR path (so it was masked out of the PBR draw), and
            // then drew nothing, with no log line and no validation error -- validation layers were
            // active and reported only an unrelated teardown leak. No pipeline was built for it either:
            // the spec assembled below differs from the terrain's own only in DebugName and Layout, and
            // PipelineCache::MakeKey hashes neither, so GetOrCreate returned the pipeline TerrainRenderer
            // had already cached. The mesh was therefore drawn with the terrain's pipeline -- whose vertex
            // shader ignores the vertex buffer entirely and reads gl_VertexIndex -- against a TerrainUB
            // that this function never fills, because it engine-fills CameraUB, TimeUB and
            // DirectionLightsUB by name and nothing else.
            //
            // The cache collision is a separate hazard and is NOT what this gate fixes; it is simply why
            // the failure was even quieter than "a wrong pipeline". This gate stops the draw before any
            // of that, on the one fact that is always true: the domains do not match.
            //
            // WHY HERE, and not at material creation. A Terrain-domain material is a legitimate object:
            // the terrain draws with one, the Material Editor edits one, and MaterialService builds one
            // for any `.demat` the File Explorer thumbnails. Creation does not know its consumer, so a
            // refusal there would refuse the correct uses too. This is the narrowest point that knows
            // BOTH facts -- the material's domain and that the consumer is the mesh path -- and it is
            // the single place every producer converges: per-slot draws, MaterialComponent shader
            // overrides and the text system all arrive in this one queue, as does every editor entry
            // point (scene slot, thumbnail, Collections). One gate covers them.
            //
            // NAMED ONCE PER SHADER, because this runs per frame per submesh group and an unguarded
            // LOG_ERROR here would be a flood that buries the message it is trying to deliver. The set
            // is a log throttle and nothing else; the refusal itself is unconditional. Same shape as
            // the s_Warned* guards this file already uses for the skinned and instanced paths, with a
            // finer key so a second offending shader is not silenced by the first.
            //
            // The entity may still cast a shadow: the shadow pass draws depth with its own shader and
            // never executes this material, and the caster belongs to the ENTITY (see
            // Rules::RouteMeshShadowCaster), whose other submeshes may be drawing correctly.
            if ( const auto domain = shader->GetProgramMeta().Domain; !Core::Formats::DrawnByMeshPath( domain ) )
            {
                static std::unordered_set<std::string> s_RefusedShaders;
                if ( s_RefusedShaders.insert( shaderName ).second )
                {
                    LOG_ERROR( "[MeshRenderer] Material shader '{}' declares Domain {}, but a mesh "
                               "material slot draws Domain {}. REFUSED: this submesh is not drawn. "
                               "Nothing lower down would have objected -- the mesh path hands a "
                               "{}-domain shader geometry and uniform blocks it does not read, and "
                               "neither Vulkan validation nor the pipeline cache can see that is wrong, "
                               "so the draw produced garbage or nothing at all with no error. Assign a "
                               "{}-domain material to this slot; a {}-domain material belongs on the "
                               "component that draws that domain.",
                               shaderName, Core::Formats::ShaderDomainName( domain ),
                               Core::Formats::ShaderDomainName( Core::Formats::kMeshPathDomain ),
                               Core::Formats::ShaderDomainName( domain ),
                               Core::Formats::ShaderDomainName( Core::Formats::kMeshPathDomain ),
                               Core::Formats::ShaderDomainName( domain ) );
                }
                continue;
            }

            if ( !material )
            {
                // ONE MATERIAL PER (SHADER x TEXTURE SET), not per shader. The parameters of the draws
                // that share it are separate rows now, so they no longer collide — but a SAMPLER is a
                // descriptor and a descriptor set belongs to the material, so two entities wanting
                // different textures still cannot share one. Keying them apart is what stops the row fix
                // from leaving half the defect standing: two labels in one font share a material and
                // batch; two labels in different fonts get one material each and both are right.
                auto& shared = m_GenericMaterials[shaderName + GenericTextureKey( g )];
                if ( !shared )
                    shared = std::make_unique<DataDrivenMaterial>( shaderName );
                material = shared.get();
            }

            GraphicsPipelineSpecification spec;
            spec.DebugName   = "GenericMesh_" + shaderName;
            spec.Shader      = shader;
            spec.Framebuffer       = targetFb;
            spec.Layout            = meshLayout;
            spec.UseLoadRenderPass = useLoadPass; // deferred manual pass begins with LOAD
            ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
            // NAMED ONCE PER SHADER, for the reason the domain refusal above gives: this runs per frame
            // per submesh group. The cache remembers the refusal itself, so the rebuild happens once;
            // this set is only about the log line.
            const auto built = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
            if ( !built )
            {
                static std::unordered_set<std::string> s_RefusedPipelines;
                if ( s_RefusedPipelines.insert( shaderName ).second )
                    LOG_ERROR( "[MeshRenderer] this submesh is not drawn: {}", built.GetError() );
                continue;
            }
            const auto& pipeline = built.GetValue();

            // ── This draw's ROW ─────────────────────────────────────────────────────────────────────
            //
            // A shader-override draw does not own its material: several entities share one within a
            // frame, so each restates its own values. A per-slot draw is the opposite — the material IS
            // the asset, its values were applied when the asset loaded, and restating them here would
            // overwrite them with schema defaults.
            //
            // Either way the values end up as a row rather than in the material's own block, which is
            // the whole change: the block WAS the parameters, so the last draw to write it decided the
            // colours of every draw recorded before it, and three spheres differing only in a graph
            // parameter rendered as one (MAT_ProbeSharedBlock.desce).
            if ( !g.SlotMaterial )
            {
                material->ApplyDefaults();
                for ( const auto& [name, value] : g.Overrides.Params )
                    material->SetParamRaw( name, value );

                // Texture overrides: resolve asset handle -> runtime Image2D and bind by sampler name.
                // Unset samplers keep the backend fallback texture, so this is purely additive. Bound on
                // the material and not per draw, because the key above guarantees every draw sharing
                // this material asked for the same set.
                for ( const auto& [name, handle] : g.Overrides.Textures )
                {
                    if ( handle == 0 )
                        continue;
                    auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
                    if ( !tex )
                        continue;
                    auto* img = static_cast<Image2D*>(
                         Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
                    if ( img )
                        material->SetTexture( name, img );
                }

                // Runtime-owned texture (no asset handle) bound straight to its sampler — the text
                // SDF atlas takes this path.
                if ( g.DirectTexture && !g.DirectTextureSampler.empty() )
                    material->SetTexture( g.DirectTextureSampler, g.DirectTexture );
            }

            auto&       rows = rowsFor( material );
            GenericDraw draw;
            draw.Data     = &g;
            draw.Material = material;
            draw.Pipeline = pipeline;
            draw.Row      = rows.Count;
            ++rows.Count;
            rows.Bytes.insert( rows.Bytes.end(), material->GetParamRow().begin(), material->GetParamRow().end() );
            draws.push_back( draw );
        }

        if ( draws.empty() )
            return;

        // ── Upload every row BEFORE recording any draw ──────────────────────────────────────────────
        //
        // At final size, and once, for the same reason DrawStaticMeshes does it: growing a storage
        // buffer reallocates the VkBuffer, and a draw recorded against the old one would read freed
        // memory. The scene snapshot goes on per MATERIAL rather than per draw — it depends only on
        // scene-global state, and a shader that declares only CameraUB still receives only CameraUB.
        for ( auto& [material, rows] : rowsByMaterial )
        {
            frameState.ApplyTo( material );
            if ( rows.Bytes.empty() )
                continue; // a shader with no parameters declares no Materials block to fill
            if ( auto* sb = material->Get<StorageBufferProperty>( Core::Formats::kMaterialRowBlockName ) )
                sb->SetRawData( rows.Bytes.data(),
                                static_cast<uint32_t>( rows.Bytes.size() * sizeof( glm::vec4 ) ) );
        }

        // ── Record, IN QUEUE ORDER ──────────────────────────────────────────────────────────────────
        //
        // Order is preserved deliberately rather than incidentally: TextSDF blends with ZWrite off, so
        // grouping the draws by material — the obvious way to write this loop — would reorder overlapping
        // labels and change the composite. Nothing above needs the draws grouped; the rows are already
        // uploaded, and what a draw carries is one push constant.
        //
        // ONE DRAW PER OBJECT, AND THE ROW TRANSPORT IS NOT WHAT STOPS THAT. Measured 2026-09-05 in Debug
        // on Resources/Assets/Scenes/MAT_ProbeGraphBatchStress.desce — 1025 cubes on one graph material,
        // the exact scene MAT_ProbeBatchStress is except that its material is a `MatProbe` graph rather
        // than a `.demat` PBR surface. Minimum of six interleaved runs across two builds, reading the
        // pass's own profiler line; the machine was shared with another agent, and the two builds' minima
        // agreed to 0.001 ms:
        //
        //   scene (1025 cubes)     RenderMesh calls   MeshGeometryPass CPU   frame (wall)
        //   PBR material           6                  0.742 ms               11.254 ms  (89 FPS)
        //   graph material         5125               12.417 ms              56.178 ms  (18 FPS)
        //
        // Moving the parameters onto rows halved this pass (26.647 -> 12.417 ms, and 71.029 -> 56.178 ms
        // of frame) by deleting the per-draw uniform-field writes and flushes. It did NOT change the draw
        // count, and it could not have: what collapses 1025 objects into 6 draws is INSTANCING, and
        // instancing needs a vertex stage that reads its transform from `InstanceTransforms[]` instead of
        // the push constant. `MeshShaderFor(Instanced, Forward)` names a whole second .shader for the PBR
        // surface; a data-driven shader has no such variant and the DSL has no way to express one, so the
        // vertex-path axis of Materials/Mesh/MeshVertexPath.hpp has exactly one cell filled for every
        // material that is not MaterialPBR.
        //
        // That is the next piece of work and it is a shader-permutation feature, not a renderer change:
        // the DSL (or the graph generator) has to emit an instanced variant, ShaderService has to register
        // it, the pipeline cache has to hold both, and this loop then groups by (material x mesh) exactly
        // as DrawStaticMeshes does — including its rule that an object with per-instance overrides leaves
        // the batch, which here means "a row that differs from the batch's". The row transport is what
        // makes that rule expressible at all; before it, two objects sharing a material could not differ.
        for ( const auto& d : draws )
        {
            const auto& g = *d.Data;
            d.Material->SetMaterialIndex( d.Row );
            Renderer::GetInstance().RenderMesh( d.Pipeline.get(), g.Mesh, g.Transform,
                                                d.Material->GetMaterialExecutor(), 1, 0, ~g.VisibleSubmeshMask,
                                                ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 ) );
        }
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

        builder.AddPass( "MeshGeometryPass", RenderPhase::Geometry,
                         [this]()
                         {
                             // Forward path only. In Deferred, meshes are drawn into the G-buffer by
                             // MeshGBufferPass instead (this target keeps sky/grid/terrain for compositing).
                             if ( m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred &&
                                  m_StaticGBufferPipeline )
                                 return;

                             const auto camera = m_SceneRenderer->GetMainCamera();
                             if ( !camera )
                                 return;

                             // `UpdateGlobalUniforms( camera, points, directionals )` used to be called
                             // here. Its entire body was `if ( !camera ) return;` — it read neither light
                             // set, which is what `-Wunused-parameter` reported about both. The lights
                             // reach the shaders through the material executors' uniform blocks, and the
                             // two `GetXLights()` calls that fed this one were a per-frame walk of the
                             // scene's light components for nothing.
                             DrawStaticMeshes();
                             DrawSkinnedMeshes();
                             DrawGenericMeshes();
                         },
                         m_StaticPipeline->GetSpecification(), targetFb,
                         { RenderPassDependency( RenderPhase::DepthPrePass ) } );

        // NOTE: the deferred G-buffer geometry is NOT a graph pass — it's rendered MANUALLY via
        // RenderGBufferManual() (called from SceneRenderer when Deferred). A graph pass targeting the G-buffer
        // would sit between the forward-target passes and break the graph's "consecutive same-framebuffer =
        // clear once" grouping, causing a spurious re-clear that wipes the sky/meshes in the scene target.

        // The silhouette mask is always produced (and cleared) so the Jump Flood outline has a
        // fresh input every frame. Outline visibility is controlled by JumpFloodOutlineRenderer.
        RegisterSilhouettePass( builder );
        RegisterShadowPass( builder );
        RegisterDebugPass( builder );
    }

    void MeshRenderer::RenderGBufferManual()
    {
        if ( !m_StaticGBufferPipeline )
            return;
        const auto& gbuffer = m_SceneRenderer ? m_SceneRenderer->GetGBuffer() : nullptr;
        if ( !gbuffer || !m_SceneRenderer->GetMainCamera() )
            return;

        auto& renderer = Renderer::GetInstance();
        // Clear the G-buffer to ZERO (not the default 0.1 grey) so empty texels have a zero normal — the
        // lighting pass uses dot(normal,normal) to tell geometry from sky, and 0.1 would read as "geometry".
        RenderPassSpecification rpSpec;
        rpSpec.TargetFramebuffer = gbuffer;
        rpSpec.DebugName         = "DeferredGBufferPass";
        rpSpec.ClearColor.Color  = glm::vec4( 0.0f, 0.0f, 0.0f, 0.0f );
        auto rp                  = RenderPass::Create( rpSpec );

        renderer.BeginRenderPass( rp.get() );
        m_DeferredGeometry = true;
        DrawStaticMeshes();
        m_DeferredGeometry = false;
        renderer.EndRenderPass();
    }

    void MeshRenderer::RenderGenericManual()
    {
        if ( m_GenericQueue.empty() )
            return;
        const auto& target = m_SceneRenderer ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        if ( !target || !m_SceneRenderer->GetMainCamera() )
            return;

        auto& renderer = Renderer::GetInstance();

        RenderPassSpecification rpSpec;
        rpSpec.TargetFramebuffer = target;
        rpSpec.DebugName         = "GenericForwardPass";
        auto rp                  = RenderPass::Create( rpSpec );

        renderer.BeginRenderPass( rp.get(), false ); // LOAD: over the deferred lighting composite
        DrawGenericMeshes( /*useLoadPass*/ true );
        renderer.EndRenderPass();
    }

    void MeshRenderer::RenderGlassManual( const std::shared_ptr<Image2D>& sceneColor )
    {
        if ( !m_StaticGlassPipeline || !m_GlassMaterial || !m_GlassInstance || m_StaticQueue.empty() )
            return;
        const auto& target = m_SceneRenderer ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        const auto  camera = m_SceneRenderer ? m_SceneRenderer->GetMainCamera() : nullptr;
        if ( !target || !camera )
            return;

        // Collect the transparent (Transmission > 0) objects + their effective GPU material entries. Uses a
        // DEDICATED material so the opaque passes' per-frame UBs are untouched (the double-write-per-frame that
        // hung the GPU).
        const Core::Frustum frustum = camera->GetFrustum();

        std::vector<const StaticMeshRenderData*> glassObjs;
        std::vector<PBRGpuMaterial>              gpuMats;
        for ( const auto& data : m_StaticQueue )
        {
            if ( !data.Mesh || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
                continue;
            if ( !IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) ) )
                continue;
            MaterialInstance* pbrInst = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Static );
            if ( !pbrInst )
                continue;
            auto*          mat = static_cast<MaterialPBR*>( pbrInst->GetParentMaterial() );
            PBRGpuMaterial gm = BuildEffectiveMaterial( mat, pbrInst );
            if ( gm.GlassTint.a <= 0.001f )
                continue; // opaque -> drawn by the opaque pass, not here
            glassObjs.push_back( &data );
            gpuMats.push_back( gm );
        }
        if ( glassObjs.empty() )
            return;

        auto& renderer = Renderer::GetInstance();

        // --- One-time shared setup on the dedicated glass material (written ONCE per frame) ---
        if ( auto* sb = m_GlassMaterial->Get<StorageBufferProperty>( "Materials" ) )
            sb->SetRawData( gpuMats.data(),
                            static_cast<uint32_t>( gpuMats.size() * sizeof( PBRGpuMaterial ) ) );

        // The whole scene contribution in one snapshot (see Graphic::PBRSceneFrame) — the glass pass
        // needs every part of it, including the env cube + BRDF bindings it epsilon-touches.
        MaterialInstance* gi = m_GlassInstance.get();
        CaptureFrameState( camera ).ApplyTo( gi );

        // Bind the scene snapshot the glass samples for refraction (binding 19, glass-shader-only).
        if ( sceneColor )
            if ( auto tex = m_GlassMaterial->GetMaterialExecutor()->GetTexture2DProperty( "u_SceneColor" ) )
                tex->SetImage( sceneColor.get() );

        // --- Draw the glass over the composited scene (LOAD + blend) ---
        RenderPassSpecification rpSpec;
        rpSpec.TargetFramebuffer = target;
        rpSpec.DebugName         = "GlassPass";
        auto rp                  = RenderPass::Create( rpSpec );

        renderer.BeginRenderPass( rp.get(), false ); // LOAD: composite over the opaque scene
        for ( uint32_t i = 0; i < static_cast<uint32_t>( glassObjs.size() ); ++i )
        {
            const auto* obj = glassObjs[i];
            MaterialPBR::UpdateTransform( gi, obj->Transform );
            m_GlassMaterial->SetMaterialIndex( i );
            m_GlassMaterial->Bind( gi );
            renderer.RenderMesh( m_StaticGlassPipeline.get(), obj->Mesh, obj->Transform,
                                 m_GlassMaterial->GetMaterialExecutor(), 1, 0, obj->HiddenSubmeshes,
                                 ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias ) );
        }
        renderer.EndRenderPass();
    }

    void MeshRenderer::RenderRSMManual()
    {
        // Reuses the G-buffer SHADER and attachment layout — the RSM framebuffer is created to match, so
        // the two are render-pass compatible and the shader's four outputs line up. The pipeline is its
        // own (standard-Z, see SetupDeferredPass) and so is the camera.
        if ( !m_RSMPipeline || !m_RSMMaterial || !m_RSMInstance || m_StaticQueue.empty() )
            return;
        const auto& rsm = m_SceneRenderer ? m_SceneRenderer->GetRSMBuffer() : nullptr;
        if ( !rsm )
            return;

        // All OPAQUE static objects are bounce sources (glass transmits rather than bouncing diffusely).
        // Their effective materials go into the DEDICATED RSM material's Materials SSBO, so each texel's
        // albedo is the real per-object one — that albedo IS the flux colour, i.e. the colour bleeding.
        std::vector<const StaticMeshRenderData*> objs;
        std::vector<PBRGpuMaterial>              gpuMats;
        for ( const auto& data : m_StaticQueue )
        {
            if ( !data.Mesh || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
                continue;
            MaterialInstance* pbrInst = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Static );
            if ( !pbrInst )
                continue;
            PBRGpuMaterial gm =
                 BuildEffectiveMaterial( static_cast<MaterialPBR*>( pbrInst->GetParentMaterial() ), pbrInst );
            if ( gm.GlassTint.a > 0.001f )
                continue;
            objs.push_back( &data );
            gpuMats.push_back( gm );
        }
        if ( objs.empty() )
            return;

        auto& renderer = Renderer::GetInstance();

        if ( auto* sb = m_RSMMaterial->Get<StorageBufferProperty>( "Materials" ) )
            sb->SetRawData( gpuMats.data(), static_cast<uint32_t>( gpuMats.size() * sizeof( PBRGpuMaterial ) ) );

        // Render from the SUN. A DEDICATED material+instance (like the glass pass) keeps this camera write
        // off the opaque passes' per-frame UBs — two writes to the same UB in one frame is the hazard that
        // previously hung the GPU.
        LightCamera       lightCam( m_RSMViewProj, m_RSMEye );
        MaterialInstance* ri = m_RSMInstance.get();
        CaptureFrameState( &lightCam ).ApplyTo( ri );

        RenderPassSpecification rpSpec;
        rpSpec.TargetFramebuffer = rsm;
        rpSpec.DebugName         = "RSMPass";
        rpSpec.ClearColor.Color  = glm::vec4( 0.0f ); // zero normal = "no caster here" for the VPL gather
        // Standard-Z pass (it is drawn through a cascade matrix), so its depth clears to 1 = far, not to
        // the engine's reversed-Z 0. With 0 the LessOrEqual test rejects everything and the RSM is empty.
        rpSpec.ClearColor.DepthStencil.x = 1.0f;
        auto rp                          = RenderPass::Create( rpSpec );

        renderer.BeginRenderPass( rp.get() );
        for ( uint32_t i = 0; i < static_cast<uint32_t>( objs.size() ); ++i )
        {
            const auto* obj = objs[i];
            MaterialPBR::UpdateTransform( ri, obj->Transform );
            m_RSMMaterial->SetMaterialIndex( i );
            m_RSMMaterial->Bind( ri );
            renderer.RenderMesh( m_RSMPipeline.get(), obj->Mesh, obj->Transform,
                                 m_RSMMaterial->GetMaterialExecutor(), 1, 0, obj->HiddenSubmeshes );
        }
        renderer.EndRenderPass();
    }

    // SUPPRESSED, NAMED, AND NOT FIXED HERE: cognitive complexity 153 against a threshold of 19. That is
    // TRUE and PRE-EXISTING -- this function has bucketed by mesh, chosen a pass variant, packed two
    // SSBOs and recorded three kinds of draw since long before the analyser gate landed (И15) -- and the
    // gate reports a function-level finding for ANY edit anywhere inside the function, so a one-line fix
    // in here cannot land without either this line or a split that is a task of its own. Named in Г26's
    // report as debt rather than hidden.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void MeshRenderer::DrawStaticMeshes()
    {
        // BOTH QUEUES, AND THE SECOND ONE WAS MISSING. The instanced batches at the bottom of this
        // function are drawn INSIDE it, so `if ( m_StaticQueue.empty() ) return;` meant an Instanced
        // Static Mesh was drawn only in a scene that also held at least one ordinary static mesh —
        // silently, with no message, because the "these entities do not appear" refusal below only
        // fires when the instanced CELL is missing, not when the function returned before reaching it.
        //
        // Measured 2026-09-21 on a scene of one directional light and one ISM of 4 000 cubes: mean
        // pixel 109.3 against 160.0 for the same 4 000 as separate entities, and the frame was
        // BYTE-IDENTICAL whether the camera stood inside the field or nine thousand units above it —
        // the tell that nothing was being drawn at all rather than drawn wrongly. Adding one ordinary
        // static mesh anywhere in the scene made all 4 000 appear.
        //
        // It is reachable from the editor in one gesture now: "Collapse selection into Instanced Static
        // Mesh" DESTROYS the entities it folds, so folding the last static meshes in a scene used to
        // empty it. The shadow pass has its own loop and never had this guard, which is why the draw
        // counter still reported thousands of instances while the colour frame held none.
        if ( m_StaticQueue.empty() && m_InstancedQueue.empty() )
            return;

        auto&      renderer = Renderer::GetInstance();
        const auto camera   = m_SceneRenderer->GetMainCamera();
        if ( !camera )
            return;

        // The scene's whole contribution to a lit draw, gathered ONCE (camera, lights, shadow cascades and
        // the resolved IBL cubes + BRDF LUT). Applied per material GROUP below, not per object: only the
        // transform is per-object, and it rides a push constant.
        const PBRSceneFrame frameState = CaptureFrameState( camera );

        // FRUSTUM CULLING, AND IT LIVES IN THE PASS RATHER THAN AT SUBMIT. The queues this pass reads are
        // read by FIVE passes, and three of them look at the scene from somewhere else: the four shadow
        // cascades and the RSM rasterize from the SUN. An object behind the camera casts a shadow into
        // the frame it is not itself in, so dropping it at submit would delete shadows to save draws —
        // a "culling win" measured as a picture that is missing something. The camera's frustum is only
        // ever applied where the camera is what rasterizes.
        //
        // Rebuilt per pass rather than cached on the renderer: it is six normalized planes out of two
        // matrices the camera already holds, and a cached frustum is a second answer to "where is the
        // camera" that can disagree with the matrices the same pass draws with.
        const Core::Frustum frustum = camera->GetFrustum();

        // Group draws by material so each material's per-object data fills ONE storage buffer, indexed
        // per draw (GPU-scene style). Objects of the same material that wrote a shared buffer per-draw
        // would otherwise collapse to the last writer.
        std::vector<std::pair<MaterialPBR*, std::vector<const StaticMeshRenderData*>>> groups;
        const auto groupFor = [&]( MaterialPBR* mat ) -> std::vector<const StaticMeshRenderData*>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, std::vector<const StaticMeshRenderData*>{} );
            return groups.back().second;
        };

        for ( const auto& data : m_StaticQueue )
        {
            if ( !data.Mesh || !data.MaterialSlots || data.MaterialSlots->Slots.empty() ||
                 !data.MaterialSlots->Slots[0] )
                continue;

            if ( !IsVisibleInView( frustum, data.Transform, Geometry::LocalBounds( data.Mesh->GetSubmeshes() ) ) )
                continue;

            // First PBR slot drives the batch. Slots holding a custom-shader material
            // (DataDrivenMaterial) are not PBR — their submeshes were routed to the generic
            // path at submit and are masked out of this draw; an object with NO PBR slot at
            // all has nothing for this path to do.
            if ( MaterialInstance* pbrInst = FirstPBRSlot( data.MaterialSlots->Slots, MeshVertexPath::Static ) )
                groupFor( static_cast<MaterialPBR*>( pbrInst->GetParentMaterial() ) ).push_back( &data );
        }

        // Accumulators for the auto-instanced path (shared across ALL material groups). The shared instanced
        // material owns ONE InstanceTransforms / Materials SSBO per frame, so every batch's data is packed
        // contiguously and uploaded ONCE: a per-batch refill of the same buffer would be overwritten by the
        // next batch before the GPU executes the recorded draws (last-write-wins). Each instanced draw then
        // reads its own transform slice via firstInstance (gl_InstanceIndex) and its own material via the
        // MaterialIndex push constant (push constants ARE snapshotted per draw, so they stay correct).
        //
        // THE CELL OF THE PASS BEING DRAWN, AND THAT IS THE WHOLE OF Г26's FIRST FIX. This used to read
        // `... && !m_DeferredGeometry`, justified by "instancing is disabled in the deferred G-buffer
        // pass (its instanced variant isn't built yet)". The justification was true for the AUTO-BATCHED
        // statics below, which fall back to per-object draws, and false for the ISM queue, which is one
        // entity carrying N transforms and has no per-object path: it was dropped whole, with no log
        // line, in the render path most of this repository's scenes state. The G-buffer pass has its own
        // instanced cell now, so the gate asks whether THIS pass has one instead of asking which pass it
        // is.
        auto* instancedPipeline =
             m_DeferredGeometry ? m_InstancedGBufferPipeline.get() : m_StaticInstancedPipeline.get();
        auto* instancedMaterial =
             m_DeferredGeometry ? m_InstancedGBufferMaterial.get() : m_StaticInstancedMaterial.get();
        auto* instancedInstance =
             m_DeferredGeometry ? m_InstancedGBufferInstance.get() : m_StaticInstancedInstance.get();
        const bool instancingOn = instancedPipeline != nullptr && instancedMaterial != nullptr &&
                                  instancedInstance != nullptr && !m_Wireframe;
        auto& instTransforms = m_ScratchInstTransforms;
        auto& instMaterials  = m_ScratchInstMaterials;
        auto& instDraws      = m_ScratchInstDraws;
        instTransforms.clear();
        instMaterials.clear();
        instDraws.clear();

        // The one renderer-owned forward material the G-buffer pass's spare material is serving in this
        // pass, so a second distinct one is noticed instead of silently sharing the spare's Materials[]
        // buffer. Local, because the question is per pass and not per frame.
        const MaterialPBR* unownedServed = nullptr;

        for ( auto& [mat, objects] : groups )
        {
            if ( objects.empty() )
                continue;

            // Sub-group this material's objects by Mesh*; a sub-group of >= 2 identical meshes collapses into
            // one instanced draw. Singletons (and everything when instancing is off / wireframe) take the
            // classic per-object path below — which also preserves their individual material overrides.
            std::vector<std::pair<Desert::StaticMesh*, std::vector<ObjDraw>>> byMesh;
            const auto bucketFor = [&]( Desert::StaticMesh* mesh ) -> std::vector<ObjDraw>&
            {
                for ( auto& [m, v] : byMesh )
                    if ( m == mesh )
                        return v;
                byMesh.emplace_back( mesh, std::vector<ObjDraw>{} );
                return byMesh.back().second;
            };

            // The effective material is built ONCE per object here and reused for the glass split,
            // the batch entry and the per-object SSBO (it used to be rebuilt up to three times).
            // Transparency split (per-object so instance-level Transmission overrides are honoured): a material
            // with Transmission > 0 is GLASS — skipped by every opaque pass (forward + deferred G-buffer) and
            // drawn ONLY by RenderGlassManual, which holds its own (Static x Glass) material and composites
            // forward over the scene with blending.
            for ( const auto* obj : objects )
            {
                ObjDraw od;
                od.Obj  = obj;
                od.Inst = FirstPBRSlot( obj->MaterialSlots->Slots, MeshVertexPath::Static );
                od.Gm   = BuildEffectiveMaterial( mat, od.Inst );
                if ( od.Gm.GlassTint.a > 0.001f )
                    continue;
                if ( od.Inst )
                    for ( const auto& [pname, prop] : od.Inst->GetPropertySet().GetProperties() )
                        if ( prop.bIsOverridden )
                        {
                            od.HasOverrides = true;
                            break;
                        }
                bucketFor( obj->Mesh ).push_back( od );
            }

            auto& singles = m_ScratchSingles;
            singles.clear();
            for ( auto& [mesh, bucket] : byMesh )
            {
                // Per-object state a shared batch entry can't carry: hidden submeshes, the
                // shadow-receive opt-out, and INSTANCE OVERRIDES — a batch shares one material
                // entry, so an overridden instance in it would silently render with someone
                // else's values. All of those take the per-object path.
                std::vector<ObjDraw> batchable;
                for ( auto& od : bucket )
                {
                    if ( od.Obj->HiddenSubmeshes != 0 || !od.Obj->ReceiveShadows || od.HasOverrides )
                        singles.push_back( od );
                    else
                        batchable.push_back( od );
                }

                if ( instancingOn && batchable.size() >= 2 )
                {
                    // PER-INSTANCE LOD, AND THE DEFECT IT CLOSES IS THAT BATCHING DROPPED THE LEVEL.
                    // The per-object path below computes a LOD and passes it to the draw; this path
                    // recorded its draw with the default, level 0. So the SAME object rendered at a
                    // different detail depending on whether it happened to find a twin to batch with —
                    // both ends of the chain (the policy and the draw) looked right and the link
                    // between them silently lost a property.
                    //
                    // Clamped to what the mesh HAS: a chain-less mesh answers 0 for every instance, so
                    // its batch stays one draw instead of splitting into four identical ones.
                    const uint32_t maxLevel = Geometry::MaxAvailableLOD( mesh->GetSubmeshes() );
                    auto&          levels   = m_ScratchLodLevels;
                    levels.clear();
                    levels.reserve( batchable.size() );
                    for ( const auto& od : batchable )
                        levels.push_back( std::min(
                             ComputeLOD( od.Obj->Transform, od.Obj->Mesh, od.Obj->ForcedLOD, od.Obj->LODBias ),
                             maxLevel ) );

                    for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                    {
                        const auto firstInstance = static_cast<uint32_t>( instTransforms.size() );
                        for ( std::size_t i = 0; i < batchable.size(); ++i )
                            if ( levels[i] == level )
                                instTransforms.push_back( batchable[i].Obj->Transform );

                        const uint32_t count = static_cast<uint32_t>( instTransforms.size() ) - firstInstance;
                        if ( count < 2 )
                        {
                            // A level with a single member is cheaper as a per-object draw, and that is
                            // the same threshold the batch itself is chosen by.
                            instTransforms.resize( firstInstance );
                            for ( std::size_t i = 0; i < batchable.size(); ++i )
                                if ( levels[i] == level )
                                    singles.push_back( batchable[i] );
                            continue;
                        }

                        InstancedDraw d;
                        d.Mesh          = mesh;
                        d.InstanceCount = count;
                        d.FirstInstance = firstInstance;
                        d.MaterialIndex = static_cast<uint32_t>( instMaterials.size() );
                        d.LodLevel      = level;
                        // No batchable object carries overrides (filtered above), so every instance of
                        // the batch genuinely shares the parent material's effective values.
                        instMaterials.push_back( batchable[0].Gm );
                        instDraws.push_back( d );
                    }
                }
                else
                {
                    for ( const auto& od : batchable )
                        singles.push_back( od );
                }
            }

            if ( singles.empty() )
                continue;

            // WHICH MATERIAL RECORDS THE DRAW. The group is keyed by the (surface x Static x Forward)
            // material a mesh slot resolved to, but the deferred pass rasterizes with the G-buffer
            // pipeline — whose layout comes from StaticMeshGBuffer's reflection, not StaticMeshPBR's. A
            // material is one shader's descriptor sets plus a payload, so the sets have to come from the
            // shader that is about to be bound: this asks the service for the SAME `.demat` on the SAME
            // vertex path in the G-buffer pass, and the twin carries the same parameters and the same
            // textures because it is built from the same asset.
            //
            // Until this existed the pass borrowed the forward material's sets, and the borrow was legal
            // only because StaticMeshGBuffer.shader declared — and multiplied by 1e-20 — fourteen
            // descriptors it never reads. That dummy is gone with this.
            MaterialPBR* drawMat = mat;
            if ( m_DeferredGeometry )
            {
                auto* materials = Runtime::ResourceRegistry::GetMaterialService();
                if ( materials->Owns( mat ) )
                {
                    drawMat = materials->GetPassVariant( mat, MeshPass::GBuffer );
                }
                else
                {
                    // A material a RENDERER built rather than a `.demat` — in practice only
                    // MeshECSSystem's default, standing in for a mesh whose slot did not resolve. It has
                    // no asset, so the service has no sibling of it; this pass keeps one of its own. See
                    // SetupGBufferPass for why one is enough, and this is the check that says so.
                    if ( unownedServed && unownedServed != mat )
                    {
                        static bool s_WarnedSecondUnowned = false;
                        if ( !s_WarnedSecondUnowned )
                        {
                            s_WarnedSecondUnowned = true;
                            LOG_ERROR( "[MeshRenderer] A second renderer-owned mesh material reached the "
                                       "deferred pass in one frame. Both groups would fill ONE Materials[] "
                                       "buffer and the last one recorded would decide the colours of both. "
                                       "The G-buffer pass keeps a single spare material because the engine "
                                       "had exactly one such material (MeshECSSystem's default); it now has "
                                       "more, and this pass needs one spare per material." );
                        }
                    }
                    unownedServed = mat;
                    drawMat       = m_GBufferUnownedMaterial.get();
                }

                if ( !drawMat )
                {
                    // Not a quiet skip: the objects in this group simply would not appear in a deferred
                    // scene, which reads as "my mesh is invisible" and not as "one material has no
                    // G-buffer variant". Once per material, because this runs every frame.
                    static std::unordered_set<const MaterialPBR*> s_WarnedNoGBufferVariant;
                    if ( s_WarnedNoGBufferVariant.insert( mat ).second )
                        LOG_ERROR( "[MeshRenderer] No (Static x GBuffer) material for a mesh material; its "
                                   "{} object(s) are NOT drawn into the G-buffer and will be missing from "
                                   "the deferred scene. MaterialFactory logged which shader refused.",
                                   singles.size() );
                    continue;
                }
            }

            // ---- Classic per-object path (singletons / wireframe) ----
            // Fill this material's per-object storage buffer (one GpuMaterial per drawn object).
            auto& gpuMaterials = m_ScratchGpuMaterials;
            gpuMaterials.clear();
            gpuMaterials.reserve( singles.size() );
            for ( const auto& od : singles )
            {
                PBRGpuMaterial gm = od.Gm;
                // ExtraParams.w rides the per-mesh Receive Shadows toggle (1 = skip sun shadows);
                // the batched path only ever carries receivers, so it stays 0 there.
                gm.ExtraParams.w = od.Obj->ReceiveShadows ? 0.0f : 1.0f;
                gpuMaterials.push_back( gm );
            }

            if ( auto* sb = drawMat->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( gpuMaterials.data(),
                                static_cast<uint32_t>( gpuMaterials.size() * sizeof( PBRGpuMaterial ) ) );

            // SHARED per-frame scene data (camera / lights / shadow / env) is written ONCE per material
            // group, NOT per mesh: these Update* all write the drawn material's buffers (shared by every
            // instance) and depend only on scene-global state. Only the transform is per-object (push
            // constant), so it stays in the draw loop below.
            //
            // The MATERIAL overload, because the material that records the draw is not always the
            // instance's parent — in the G-buffer pass it is the pass's own twin. Every write is by block
            // NAME and guarded, so a G-buffer material that declares only the camera receives only the
            // camera, and the fourteen blocks it no longer has cost it nothing.
            {
                DESERT_PROFILE_SCOPE( "Mesh: SharedSceneSetup (1x/group)" );
                frameState.ApplyTo( static_cast<Material*>( drawMat ) );
            }

            for ( uint32_t i = 0; i < static_cast<uint32_t>( singles.size() ); ++i )
            {
                const auto*       obj  = singles[i].Obj;
                MaterialInstance* inst = singles[i].Inst;

                {
                    // Per-object work: transform (push constant) + material index + descriptor bind.
                    DESERT_PROFILE_SCOPE( "Mesh: PerObject Setup" );
                    MaterialPBR::UpdateTransform( inst, obj->Transform );
                    drawMat->SetMaterialIndex( i );
                    // The INSTANCE still comes from the forward slot, and that is correct rather than
                    // convenient: an instance carries the per-object Transform this Bind pushes plus the
                    // overrides, and both are looked up by NAME in whichever material is binding. What the
                    // instance must NOT be is a second descriptor set — it never was.
                    drawMat->Bind( inst );
                }

                {
                    // The actual draw call (bind pipeline + descriptor sets + vkCmdDrawIndexed).
                    DESERT_PROFILE_SCOPE( "Mesh: RenderMesh (draw)" );
                    // Deferred: the G-buffer twin's sets bind against the G-buffer pipeline, which writes
                    // the MRT instead of shading. Otherwise forward (wireframe variant when enabled).
                    auto* pipeline =
                         ( m_DeferredGeometry && m_StaticGBufferPipeline ) ? m_StaticGBufferPipeline.get()
                         : ( m_Wireframe && m_StaticWireframePipeline )    ? m_StaticWireframePipeline.get()
                                                                           : m_StaticPipeline.get();
                    const uint32_t lod = ComputeLOD( obj->Transform, obj->Mesh, obj->ForcedLOD, obj->LODBias );
                    renderer.RenderMesh( pipeline, obj->Mesh, obj->Transform, drawMat->GetMaterialExecutor(), 1, 0,
                                         obj->HiddenSubmeshes, lod );
                }
            }
        }

        // UE-style Instanced Static Meshes (one entity = N instances): fold into the same instanced
        // accumulation as the auto-batched static meshes. Each ISM is one batch; its transforms come
        // straight from the component array (no per-instance ECS cost), appended to the shared SSBO.
        //
        // AND IT IS NOT SILENT WHEN IT CANNOT. The queue has no per-object fallback, so "instancing is
        // unavailable" means "these entities do not appear". §1.4: that is refused out loud, with the
        // pass it happened in and how many entities it cost, rather than substituted with nothing.
        if ( !instancingOn && !m_InstancedQueue.empty() )
        {
            static bool s_WarnedNoInstancedCell = false;
            if ( !s_WarnedNoInstancedCell )
            {
                s_WarnedNoInstancedCell = true;
                LOG_ERROR( "[MeshRenderer] {} Instanced Static Mesh entit(ies) are NOT drawn in the {} "
                           "pass: it has no usable instanced cell (pipeline {}, material {}, instance {}"
                           "{}). An ISM is one entity holding N transforms and has no per-object path to "
                           "fall back to, so nothing of it reaches the frame.",
                           m_InstancedQueue.size(), m_DeferredGeometry ? "G-buffer" : "forward",
                           instancedPipeline ? "ok" : "MISSING", instancedMaterial ? "ok" : "MISSING",
                           instancedInstance ? "ok" : "MISSING", m_Wireframe ? ", wireframe view on" : "" );
            }
        }

        if ( instancingOn )
        {
            for ( const auto& ism : m_InstancedQueue )
            {
                if ( !ism.Mesh || !ism.Material || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                auto* mat = static_cast<MaterialPBR*>( ism.Material->GetParentMaterial() );
                if ( !mat )
                    continue;

                // PER-INSTANCE, and that is the whole point of culling an ISM at all. A batch is ONE draw
                // call carrying N transforms, so a batch-level test would answer "some of it is on screen"
                // and then submit every instance — a forest whose one visible tree costs the vertex stage
                // all forty thousand of them. The draw count does not move here; the INSTANCE count does,
                // which is why the detector prints the two apart.
                //
                // Only the visible transforms are appended, so the slice named by firstInstance is exactly
                // the instances that survived: nothing downstream needs to know culling happened.
                const Common::Math::AABB localBounds = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
                const uint32_t           maxLevel    = Geometry::MaxAvailableLOD( ism.Mesh->GetSubmeshes() );

                // Visible instances and their levels, gathered in ONE pass over the component's array.
                // The LOD question is per instance for the same reason the visibility question is: the
                // batch is one entity but forty thousand placements, and the near ones and the far ones
                // of a single ISM are not the same object to a renderer.
                auto& visible = m_ScratchIsmVisible;
                auto& levels  = m_ScratchLodLevels;
                visible.clear();
                levels.clear();
                for ( const auto& instanceTransform : *ism.Transforms )
                {
                    if ( !IsVisibleInView( frustum, instanceTransform, localBounds ) )
                        continue;
                    visible.push_back( instanceTransform );
                    levels.push_back( std::min( Geometry::SelectLODFromBounds( instanceTransform, localBounds,
                                                                               camera->GetPosition(), -1, 0 ),
                                                maxLevel ) );
                }
                if ( visible.empty() )
                    continue;

                // ONE material row for the whole ISM, named by every one of its per-level draws: the
                // level splits the geometry, not the material.
                const auto materialIndex = static_cast<uint32_t>( instMaterials.size() );
                instMaterials.push_back( BuildEffectiveMaterial( mat, ism.Material.get() ) );

                for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                {
                    InstancedDraw d;
                    d.Mesh          = ism.Mesh;
                    d.FirstInstance = static_cast<uint32_t>( instTransforms.size() );
                    d.MaterialIndex = materialIndex;
                    d.LodLevel      = level;
                    for ( std::size_t i = 0; i < visible.size(); ++i )
                        if ( levels[i] == level )
                            instTransforms.push_back( visible[i] );
                    d.InstanceCount = static_cast<uint32_t>( instTransforms.size() ) - d.FirstInstance;
                    instDraws.push_back( d );
                }
            }
        }

        // ---- Instanced path: upload the packed SSBOs ONCE (at final size) before recording any instanced
        // draw, so the descriptor points at the final VkBuffer (a later grow reallocates it). Scene data is
        // uploaded a single time for the whole frame; each batch is then one instanced draw call. ----
        if ( instancingOn && !instDraws.empty() )
        {
            DESERT_PROFILE_SCOPE( "Mesh: Instanced Batches" );
            auto* instMat  = instancedMaterial;
            auto* instInst = instancedInstance;

            if ( auto* sb = instMat->Get<StorageBufferProperty>( "InstanceTransforms" ) )
                sb->SetRawData( instTransforms.data(),
                                static_cast<uint32_t>( instTransforms.size() * sizeof( glm::mat4 ) ) );
            if ( auto* sb = instMat->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( instMaterials.data(),
                                static_cast<uint32_t>( instMaterials.size() * sizeof( PBRGpuMaterial ) ) );

            frameState.ApplyTo( instInst );
            MaterialPBR::UpdateTransform( instInst, glm::mat4( 1.0f ) ); // unused by the instanced VS

            // The instanced vertex stage reads its model matrix from the InstanceTransforms SSBO, so the
            // per-draw transform is unused; it is named rather than repeated as a literal.
            const glm::mat4 unusedModelTransform( 1.0F );
            for ( const auto& d : instDraws )
            {
                instMat->SetMaterialIndex( d.MaterialIndex );
                instMat->Bind( instInst );
                renderer.RenderMesh( instancedPipeline, d.Mesh, unusedModelTransform,
                                     instMat->GetMaterialExecutor(), d.InstanceCount, d.FirstInstance,
                                     /*hiddenSubmeshMask*/ 0, d.LodLevel );
            }
        }
    }

    void MeshRenderer::DrawSkinnedMeshes( bool useLoadPass )
    {
        if ( m_SkinnedQueue.empty() )
            return;

        auto&      renderer = Renderer::GetInstance();
        const auto camera   = m_SceneRenderer->GetMainCamera();

        // The SAME snapshot, from the SAME gather, that lights every static mesh in this frame — the
        // cascades and the environment cubes included. Skinned meshes have no G-buffer variant, so in a
        // deferred scene they are drawn FORWARD over the composite and receive nothing the composite
        // computed: this is the only route by which the sun's shadows, the baked sky and the cloud
        // layer's shadow reach them at all.
        const PBRSceneFrame frameState = CaptureFrameState( camera );

        // Deferred forward-over-composite: a LOAD-render-pass variant of the skinned pipeline (built once via
        // the pipeline cache), so skinned meshes draw OVER the deferred scene instead of clearing it. Same
        // mechanism the generic + glass passes use. Forward path keeps the plain pipeline (no load).
        GraphicsPipeline* pipeline = m_SkinnedPipeline.get();
        if ( useLoadPass && m_SkinnedPipeline )
        {
            GraphicsPipelineSpecification spec = m_SkinnedPipeline->GetSpecification();
            spec.UseLoadRenderPass             = true;
            spec.DebugName                     = "SkinnedMesh_Load";
            // A refusal here is not fatal: `pipeline` still holds the non-LOAD skinned pipeline, which
            // draws over a cleared target instead of the composited one. Named once, because this runs
            // every frame.
            const auto loadVariant = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
            if ( loadVariant )
            {
                pipeline = loadVariant.GetValue().get();
            }
            else
            {
                static bool s_Warned = false;
                if ( !std::exchange( s_Warned, true ) )
                    LOG_ERROR( "[MeshRenderer] skinned meshes draw through the non-LOAD pipeline in the "
                               "deferred path: {}",
                               loadVariant.GetError() );
            }
        }

        // Grouped by material, exactly like DrawStaticMeshes — and for the same two reasons, which the
        // skinned path did not have before and paid for twice:
        //
        //   * the per-object GPU materials fill ONE Materials[] buffer indexed per draw, so instance
        //     overrides survive. The old path built its entry from the parent material's data and never
        //     read the instance, so a tinted character rendered untinted;
        //   * every pose in the group is packed end to end into ONE Bones buffer and each draw names its
        //     slice with a push constant. The old path uploaded a pose per draw into a buffer the
        //     already-recorded draws still pointed at, so two skinned meshes sharing a material both
        //     rendered in the pose of whichever was submitted last.
        std::vector<std::pair<MaterialPBR*, std::vector<const SkinnedMeshRenderData*>>> groups;
        const auto groupFor = [&]( MaterialPBR* mat ) -> std::vector<const SkinnedMeshRenderData*>&
        {
            for ( auto& [m, v] : groups )
                if ( m == mat )
                    return v;
            groups.emplace_back( mat, std::vector<const SkinnedMeshRenderData*>{} );
            return groups.back().second;
        };
        for ( const auto& data : m_SkinnedQueue )
            if ( data.Mesh && data.Material && data.Instance )
                groupFor( data.Material ).push_back( &data );

        auto& bones        = m_ScratchBones;
        auto& gpuMaterials = m_ScratchGpuMaterials;

        for ( auto& [mat, objects] : groups )
        {
            bones.clear();
            gpuMaterials.clear();
            gpuMaterials.reserve( objects.size() );

            std::vector<uint32_t> boneOffsets;
            boneOffsets.reserve( objects.size() );
            for ( const auto* obj : objects )
            {
                boneOffsets.push_back( static_cast<uint32_t>( bones.size() ) );
                bones.insert( bones.end(), obj->BoneMatrices.begin(), obj->BoneMatrices.end() );
                gpuMaterials.push_back( BuildEffectiveMaterial( mat, obj->Instance ) );
            }

            // Both buffers at FINAL size before any draw is recorded, so the descriptor points at the
            // buffer the draws will actually read (a later grow reallocates it).
            mat->UploadBones( bones.data(), bones.size() );
            if ( auto* sb = mat->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( gpuMaterials.data(),
                                static_cast<uint32_t>( gpuMaterials.size() * sizeof( PBRGpuMaterial ) ) );

            // Shared per-frame scene state once per group, as the static path does.
            frameState.ApplyTo( objects[0]->Instance );

            for ( uint32_t i = 0; i < static_cast<uint32_t>( objects.size() ); ++i )
            {
                const auto* obj = objects[i];
                MaterialPBR::UpdateTransform( obj->Instance, obj->Transform );
                mat->SetMaterialIndex( i );
                mat->SetBoneOffset( boneOffsets[i] );
                mat->Bind( obj->Instance );

                renderer.RenderMesh( pipeline, obj->Mesh, obj->Transform, mat->GetMaterialExecutor() );
            }
        }
    }

    void MeshRenderer::RenderSkinnedManual()
    {
        if ( m_SkinnedQueue.empty() )
            return;
        const auto& target = m_SceneRenderer ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        if ( !target || !m_SceneRenderer->GetMainCamera() )
            return;

        auto& renderer = Renderer::GetInstance();

        RenderPassSpecification rpSpec;
        rpSpec.TargetFramebuffer = target;
        rpSpec.DebugName         = "SkinnedForwardPass";
        auto rp                  = RenderPass::Create( rpSpec );

        renderer.BeginRenderPass( rp.get(), false ); // LOAD: over the deferred lighting composite
        DrawSkinnedMeshes( /*useLoadPass*/ true );
        renderer.EndRenderPass();
    }

    bool MeshRenderer::SetupGeometryPass()
    {
        m_GeometryShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "StaticMeshPBR" );

        if ( !m_GeometryShader )
            return false;

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName = "StaticMeshGeometry";

        spec.Layout = { { Graphic::ShaderDataType::Float3, "a_Position" },
                        { Graphic::ShaderDataType::Float3, "a_Normal" },
                        { Graphic::ShaderDataType::Float3, "a_Tangent" },
                        { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                        { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };

        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        spec.Shader         = m_GeometryShader;
        spec.Framebuffer    = targetFb;

        // Pipelines come from the shared cache (deduped by shader + target + state). The mesh keeps its
        // explicit state for now; PBR's render-state moves to the shader's #pragma state in Phase 2.
        const auto staticPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !staticPipeline )
        {
            LOG_ERROR( "[MeshRenderer] static meshes will not draw: {}", staticPipeline.GetError() );
            return false;
        }
        m_StaticPipeline = staticPipeline.GetValue();

        // Wireframe variant — identical spec, line polygon mode (device feature fillModeNonSolid is on).
        // Selected per-frame by the SceneSettings debug toggle; shares the same framebuffer/render pass.
        // A debug view, so a refusal costs the view and not the pass.
        spec.DebugName   = "StaticMeshWireframe";
        spec.PolygonMode = PrimitivePolygonMode::Wireframe;
        if ( const auto wireframe = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec ) )
            m_StaticWireframePipeline = wireframe.GetValue();
        else
            LOG_ERROR( "[MeshRenderer] the wireframe view is off: {}", wireframe.GetError() );

        // Instanced variant: same vertex layout + state, but the vertex shader pulls the per-instance model
        // matrix from the InstanceTransforms SSBO (binding 16) by gl_InstanceIndex. Drawn via one instanced
        // draw call (RenderMeshInstanced). Optional — if the shader is missing, instancing is just disabled.
        m_InstancedGeometryShader =
             Runtime::ResourceRegistry::GetShaderService()->GetByName( "StaticMeshPBR_Instanced" );
        if ( m_InstancedGeometryShader )
        {
            GraphicsPipelineSpecification ispec;
            ispec.DebugName      = "StaticMeshGeometryInstanced";
            ispec.Layout         = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                     { Graphic::ShaderDataType::Float3, "a_Normal" },
                                     { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                     { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                     { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };
            ispec.DepthCompareOp      = DepthCompare::CloserOrEqual;
            ispec.CullMode       = CullMode::Back;
            ispec.Shader         = m_InstancedGeometryShader;
            ispec.Framebuffer    = targetFb;
            if ( const auto instanced = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
                m_StaticInstancedPipeline = instanced.GetValue();
            else
                LOG_ERROR( "[MeshRenderer] instanced static drawing is off: {}", instanced.GetError() );
        }

        return true;
    }

    bool MeshRenderer::SetupGBufferPass()
    {
        // Optional: only present when the deferred G-buffer shader exists and the scene renderer has a
        // G-buffer. Failure here does NOT fail Initialize — the forward path stays fully functional.
        m_StaticGBufferShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "StaticMeshGBuffer" );
        if ( !m_StaticGBufferShader )
            return false;

        const auto& gbuffer = m_SceneRenderer ? m_SceneRenderer->GetGBuffer() : nullptr;
        if ( !gbuffer )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName      = "StaticMeshGBuffer";
        spec.Layout         = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                { Graphic::ShaderDataType::Float3, "a_Normal" },
                                { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };
        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        spec.Shader         = m_StaticGBufferShader;
        spec.Framebuffer    = gbuffer; // 2 color attachments -> the shader's 2 MRT outputs

        const auto gbufferPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !gbufferPipeline )
        {
            LOG_ERROR( "[MeshRenderer] the deferred path is off, the forward one still draws: {}",
                       gbufferPipeline.GetError() );
            return false;
        }
        m_StaticGBufferPipeline = gbufferPipeline.GetValue();

        // The RSM renders the same shader, layout and attachment set from the sun's POV, with a DEDICATED
        // material — but NOT the same pipeline, because it is drawn through a CASCADE matrix, and the
        // cascades are standard-Z (SetupShadowPass says why). Sharing the G-buffer pipeline would test
        // reversed-Z depth against standard-Z fragments, which is not "slightly wrong": it keeps the
        // FARTHEST surface per texel, so every VPL would be a back face and the bounce light would come
        // out of the wrong geometry. One extra pipeline is the price of the two conventions coexisting,
        // and the cache hands back a shared object anyway if some other pass ever asks for the same state.
        GraphicsPipelineSpecification rsmSpec = spec;
        rsmSpec.DebugName                     = "StaticMeshRSM";
        rsmSpec.DepthCompareOp                = CompareOp::LessOrEqual;
        const auto rsmPipeline                = m_SceneRenderer->GetPipelineCache().GetOrCreate( rsmSpec );
        if ( !rsmPipeline )
        {
            LOG_ERROR( "[MeshRenderer] the deferred path is off, the forward one still draws: {}",
                       rsmPipeline.GetError() );
            return false;
        }
        m_RSMPipeline = rsmPipeline.GetValue();

        // (Static x GBuffer): the RSM reuses the G-buffer shader and its pipeline, rasterized from the
        // sun. A DEDICATED material (rather than the objects' own) because this pass writes a camera UB
        // holding the SUN's matrices, and two writes to one per-frame UB in a frame is the hazard the
        // glass pass was split out to avoid.
        m_RSMMaterial = MaterialPBR::Create( MeshVertexPath::Static, MeshPass::GBuffer );
        if ( !m_RSMMaterial )
            return false;
        m_RSMInstance = m_RSMMaterial->CreateInstance();

        // (Static x GBuffer) for the meshes whose FORWARD material is not service-owned. There is exactly
        // one such material in the engine — MeshECSSystem::m_DefaultMaterial, which stands in for every
        // mesh whose slot does not resolve — and it has no `.demat`, so MaterialService has no sibling of
        // it to hand out. Without this the deferred pass drew nothing for those meshes: measured on
        // Resources/Assets/Scenes/MAT_ProbeDeferredNoSlot.desce, a Cornell box with the material stripped
        // off one cube, and the cube vanished.
        //
        // ONE material is enough and that is a property of the engine, not an assumption: a MeshRenderer
        // draws one scene's queue, a scene has one MeshECSSystem, and a MeshECSSystem has one default
        // material. DrawStaticMeshes checks it — a second distinct unowned material in one pass would
        // share this material's Materials[] buffer and the last group to fill it would win.
        //
        // It needs no textures: the default material has none either, so both sample the backend's
        // fallbacks and the surface is identical. What it supplies is the descriptor SETS, allocated from
        // the G-buffer shader's own reflection.
        m_GBufferUnownedMaterial = MaterialPBR::Create( MeshVertexPath::Static, MeshPass::GBuffer );
        if ( !m_GBufferUnownedMaterial )
            return false;

        // (Instanced x GBuffer). Without it the deferred pass had no instanced cell, and since the ISM
        // queue is the one queue with NO per-object fallback, every InstancedStaticMesh entity was
        // dropped there in silence -- in the render path most of this repository's scenes state. Optional
        // like the rest of this pass: a refusal costs instancing in the G-buffer, and DrawStaticMeshes
        // logs what that costs rather than dropping the queue without a word.
        m_InstancedGBufferShader = Runtime::ResourceRegistry::GetShaderService()->GetByName(
             MeshShaderFor( MeshVertexPath::Instanced, MeshPass::GBuffer ) );
        if ( m_InstancedGBufferShader )
        {
            GraphicsPipelineSpecification ispec;
            ispec.DebugName      = "StaticMeshGBufferInstanced";
            ispec.Layout         = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                     { Graphic::ShaderDataType::Float3, "a_Normal" },
                                     { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                     { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                     { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };
            ispec.DepthCompareOp = DepthCompare::CloserOrEqual;
            ispec.CullMode       = CullMode::Back;
            ispec.Shader         = m_InstancedGBufferShader;
            ispec.Framebuffer    = gbuffer;

            if ( const auto instanced = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
            {
                m_InstancedGBufferPipeline = instanced.GetValue();
                m_InstancedGBufferMaterial = MaterialPBR::Create( MeshVertexPath::Instanced, MeshPass::GBuffer );
                if ( m_InstancedGBufferMaterial )
                {
                    m_InstancedGBufferInstance =
                         m_InstancedGBufferMaterial->CreateInstance( "StaticInstancedGBufferBatch" );
                }
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] instanced drawing is off in the DEFERRED path: {}. Instanced "
                           "Static Meshes will not be drawn while the scene renders deferred.",
                           instanced.GetError() );
            }
        }
        else
        {
            LOG_ERROR( "[MeshRenderer] shader '{}' is missing; Instanced Static Meshes will not be drawn "
                       "while the scene renders deferred.",
                       MeshShaderFor( MeshVertexPath::Instanced, MeshPass::GBuffer ) );
        }
        return true;
    }

    bool MeshRenderer::SetupGlassPass()
    {
        // Optional (like the G-buffer pass): needs the glass shader + the scene target. Failure leaves the
        // rest fully functional — glass just won't draw.
        m_StaticGlassShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "StaticMeshGlass" );
        if ( !m_StaticGlassShader )
            return false;

        const auto& target = m_SceneRenderer ? m_SceneRenderer->GetTargetFramebuffer() : nullptr;
        if ( !target )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName         = "StaticMeshGlass";
        spec.Layout            = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                   { Graphic::ShaderDataType::Float3, "a_Normal" },
                                   { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                   { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                   { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };
        spec.Shader            = m_StaticGlassShader;
        spec.Framebuffer       = target;
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.DepthWriteEnabled = false;      // transparent: don't occlude later fragments / itself
        spec.CullMode          = CullMode::Back;
        spec.BlendEnable       = true;       // src-alpha over the composited scene
        spec.UseLoadRenderPass = true;       // begun with clearFrame=false to preserve the opaque scene

        const auto glassPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !glassPipeline )
        {
            LOG_ERROR( "[MeshRenderer] glass materials will not draw: {}", glassPipeline.GetError() );
            return false;
        }
        m_StaticGlassPipeline = glassPipeline.GetValue();

        // A dedicated material (+ one instance) owns the glass pass's per-frame UBs / Materials SSBO. It is
        // descriptor-layout-compatible with the glass pipeline because Glass.glsl.frag declares the same
        // bindings as StaticMeshPBR. Being separate from every opaque material, its per-frame ring is written
        // exactly once per frame (here) — no double-update hang.
        // (Static x Glass): same surface, same plumbing, a shader whose fragment stage refracts the
        // composited scene. Dedicated for the per-frame-UB reason above.
        m_GlassMaterial = MaterialPBR::Create( MeshVertexPath::Static, MeshPass::Glass );
        if ( !m_GlassMaterial )
            return false;
        m_GlassInstance = m_GlassMaterial->CreateInstance();
        return m_GlassMaterial && m_GlassInstance;
    }

    bool MeshRenderer::SetupSkinnedGeometryPass()
    {
        m_SkinnedShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SkinnedMeshPBR" );

        if ( !m_SkinnedShader )
            return false;

        const auto& targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName = "SkinnedMeshGeometry";

        spec.Layout = { { Graphic::ShaderDataType::Float3, "a_Position" },
                        { Graphic::ShaderDataType::Float3, "a_Normal" },
                        { Graphic::ShaderDataType::Float3, "a_Tangent" },
                        { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                        { Graphic::ShaderDataType::Float2, "a_TextureCoord" },
                        { Graphic::ShaderDataType::Int4, "a_BoneIndices" },
                        { Graphic::ShaderDataType::Float4, "a_BoneWeights" } };

        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        spec.Shader         = m_SkinnedShader;
        spec.Framebuffer    = targetFb;

        const auto skinned = GraphicsPipeline::Create( spec );
        if ( !skinned )
        {
            LOG_ERROR( "[MeshRenderer] skinned meshes will not draw: {}", skinned.GetError() );
            return false;
        }
        m_SkinnedPipeline = skinned.GetValue();

        return true;
    }

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
        maskSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA8F );

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
            sspec.DebugName = "SilhouetteSkinnedPipeline";
            sspec.Layout    = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                { Graphic::ShaderDataType::Float3, "a_Normal" },
                                { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                { Graphic::ShaderDataType::Float2, "a_TextureCoord" },
                                { Graphic::ShaderDataType::Int4, "a_BoneIndices" },
                                { Graphic::ShaderDataType::Float4, "a_BoneWeights" } };
            sspec.Shader            = m_SilhouetteSkinnedShader;
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

        m_ShadowShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Shadow" );
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
            shadowSpec.DebugName = "ShadowCascade" + std::to_string( i );
            shadowSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::RGBA32F );
            shadowSpec.Attachments.Attachments.push_back( Core::Formats::ImageFormat::DEPTH24STENCIL8 );
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
        spec.DebugName = "ShadowPipeline";
        spec.Layout    = { { Graphic::ShaderDataType::Float3, "a_Position" },
                           { Graphic::ShaderDataType::Float3, "a_Normal" },
                           { Graphic::ShaderDataType::Float3, "a_Tangent" },
                           { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                           { Graphic::ShaderDataType::Float2, "a_TextureCoord" } };
        spec.DepthTestEnabled  = true;
        spec.DepthWriteEnabled = true;
        // STANDARD-Z, AND THE ONLY PASS IN THE ENGINE THAT IS. Everything else renders reversed-Z
        // (Core/Projection.hpp), but a cascade's projection is ORTHOGRAPHIC — its depth is linear in
        // light-space distance, so there is no 1/z curve for a float exponent to cancel and reversing it
        // would buy exactly zero precision while inverting the compare in seven sampling shaders and the
        // sign of the shadow bias. It is spelled with a raw CompareOp, not DepthCompare::, precisely so
        // that it does not silently follow the engine convention if that is ever revisited. Its render
        // pass clears depth to 1 via PassConfig::ClearDepth in RegisterShadowPass.
        spec.DepthCompareOp    = CompareOp::LessOrEqual;
        // No culling in the shadow pass: store ALL faces so the map can never come out empty (front-face
        // culling under the engine's negative-height viewport could cull the wrong set and black out the
        // scene). Self-shadow acne is handled by the normal-offset + slope bias in the PBR sampling.
        spec.CullMode          = CullMode::None;
        spec.Shader            = m_ShadowShader;
        // All cascade framebuffers share the same attachment formats, so one pipeline is render-pass
        // compatible with all of them.
        spec.Framebuffer       = m_CascadeFB[0];

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
        m_ShadowInstancedShader =
             Runtime::ResourceRegistry::GetShaderService()->GetByName( "Shadow_Instanced" );
        if ( m_ShadowInstancedShader )
        {
            GraphicsPipelineSpecification ispec = spec;
            ispec.DebugName = "ShadowPipelineInstanced";
            ispec.Shader    = m_ShadowInstancedShader;
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
        m_ShadowSkinnedShader = Runtime::ResourceRegistry::GetShaderService()->GetByName(
             MeshShaderFor( MeshVertexPath::Skinned, MeshPass::ShadowDepth ) );
        if ( m_ShadowSkinnedShader )
        {
            GraphicsPipelineSpecification sspec = spec;
            sspec.DebugName                     = "ShadowPipelineSkinned";
            sspec.Layout                        = { { Graphic::ShaderDataType::Float3, "a_Position" },
                                                    { Graphic::ShaderDataType::Float3, "a_Normal" },
                                                    { Graphic::ShaderDataType::Float3, "a_Tangent" },
                                                    { Graphic::ShaderDataType::Float3, "a_Bitangent" },
                                                    { Graphic::ShaderDataType::Float2, "a_TextureCoord" },
                                                    { Graphic::ShaderDataType::Int4, "a_BoneIndices" },
                                                    { Graphic::ShaderDataType::Float4, "a_BoneWeights" } };
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
            LOG_WARN( "[MeshRenderer] Shadow_Skinned shader missing — skinned meshes will cast no shadow." );
        }

        return true;
    }

    PBRSceneFrame MeshRenderer::CaptureFrameState( const Core::Camera* camera ) const
    {
        PBRSceneFrame frame;
        frame.Camera = camera;

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

        const auto camera = m_SceneRenderer->GetMainCamera();
        if ( !camera )
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
            m_CascadeVP[c]            = fits[c].ViewProj;
            m_CascadeWorldPerTexel[c] = fits[c].WorldPerTexel;

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

            builder.AddPass(
                 "MeshShadowCascade" + std::to_string( c ), RenderPhase::DepthPrePass,
                 [this, c]()
                 {
                     if ( !m_ShadowsEnabled )
                         return;

                     // Shadow vert computes Projection*View*Transform; feed the combined cascade matrix as
                     // Projection and identity as View, matching u_LightViewProj[c] on the PBR side.
                     m_ShadowMaterial[c]->SetLightMatrix( glm::mat4( 1.0f ), m_CascadeVP[c] );

                     auto& renderer = Renderer::GetInstance();

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
                     const auto*     lodCamera = m_SceneRenderer->GetMainCamera();
                     const glm::vec3 lodViewPosition =
                          lodCamera != nullptr ? lodCamera->GetPosition() : glm::vec3( 0.0f );

                     std::vector<std::pair<Desert::StaticMesh*, std::vector<const StaticMeshRenderData*>>> byMesh;
                     const auto bucketFor =
                          [&]( Desert::StaticMesh* mesh ) -> std::vector<const StaticMeshRenderData*>&
                     {
                         for ( auto& [m, v] : byMesh )
                             if ( m == mesh )
                                 return v;
                         byMesh.emplace_back( mesh, std::vector<const StaticMeshRenderData*>{} );
                         return byMesh.back().second;
                     };
                     for ( const auto& rd : m_StaticQueue )
                         if ( rd.Mesh != nullptr && rd.CastShadows &&
                              IsVisibleInView( cascadeFrustum, rd.Transform,
                                               Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ) ) )
                             bucketFor( rd.Mesh ).push_back( &rd );

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
                                      std::min( ComputeLOD( rd->Transform, rd->Mesh, rd->ForcedLOD, rd->LODBias ),
                                                maxLevel ) );

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
                                 batches.push_back( ShadowBatch{ mesh, count, first, level } );
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
                             if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms ||
                                  ism.Transforms->empty() )
                                 continue;

                             // Per-instance, against this cascade. A cascade covers a slice of the view,
                             // so a forest spread over the map has most of its instances outside every
                             // one of the four — and the batch is a single draw whose cost is entirely
                             // its instance count.
                             const Common::Math::AABB localBounds =
                                  Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
                             const uint32_t maxLevel = Geometry::MaxAvailableLOD( ism.Mesh->GetSubmeshes() );

                             auto& visible = m_ScratchIsmVisible;
                             auto& levels  = m_ScratchLodLevels;
                             visible.clear();
                             levels.clear();
                             for ( const auto& instanceTransform : *ism.Transforms )
                             {
                                 if ( !IsVisibleInView( cascadeFrustum, instanceTransform, localBounds ) )
                                     continue;
                                 visible.push_back( instanceTransform );
                                 levels.push_back(
                                      std::min( Geometry::SelectLODFromBounds( instanceTransform, localBounds,
                                                                               lodViewPosition, -1, 0 ),
                                                maxLevel ) );
                             }
                             if ( visible.empty() )
                                 continue;

                             for ( const uint32_t level : Geometry::DistinctLODs( levels ) )
                             {
                                 const auto first = static_cast<uint32_t>( instTransforms.size() );
                                 for ( std::size_t i = 0; i < visible.size(); ++i )
                                     if ( levels[i] == level )
                                         instTransforms.push_back( visible[i] );
                                 batches.push_back( ShadowBatch{
                                      ism.Mesh, static_cast<uint32_t>( instTransforms.size() ) - first, first,
                                      level } );
                             }
                         }
                     }

                     // Per-object path (singletons).
                     for ( const auto* rd : singles )
                         renderer.RenderMesh( m_ShadowPipeline.get(), rd->Mesh, rd->Transform,
                                              m_ShadowMaterial[c]->GetMaterialExecutor(), 1, 0, 0,
                                              ComputeLOD( rd->Transform, rd->Mesh, rd->ForcedLOD,
                                                          rd->LODBias ) );

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
                         if ( g.Mesh != nullptr && g.CastShadows &&
                              IsVisibleInView( cascadeFrustum, g.Transform,
                                               Geometry::LocalBounds( g.Mesh->GetSubmeshes() ) ) )
                             renderer.RenderMesh( m_ShadowPipeline.get(), g.Mesh, g.Transform,
                                                  m_ShadowMaterial[c]->GetMaterialExecutor(), 1, 0, 0,
                                                  ComputeLOD( g.Transform, g.Mesh, /*forced*/ -1 ) );

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
                             if ( !sd.Mesh || !sd.CastShadows || sd.BoneMatrices.empty() )
                                 continue;
                             casters.emplace_back( &sd, static_cast<uint32_t>( skinBones.size() ) );
                             skinBones.insert( skinBones.end(), sd.BoneMatrices.begin(), sd.BoneMatrices.end() );
                         }
                         if ( !casters.empty() )
                         {
                             skinMat->UploadBones( skinBones );
                             for ( const auto& [sd, boneOffset] : casters )
                             {
                                 skinMat->SetBoneOffset( boneOffset );
                                 renderer.RenderMesh( m_ShadowSkinnedPipeline.get(), sd->Mesh, sd->Transform,
                                                      skinMat->GetMaterialExecutor() );
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
                             renderer.RenderMesh( m_ShadowInstancedPipeline.get(), b.Mesh, glm::mat4( 1.0f ),
                                                  instMat->GetMaterialExecutor(), b.Count, b.First,
                                                  /*hiddenSubmeshMask*/ 0, b.LodLevel );
                     }
                 },
                 m_ShadowPipeline->GetSpecification(), m_CascadeFB[c], {},
                 // Clear the R32F depth target to 1.0 (far): background texels must read as "no occluder",
                 // else the default 0.1 grey clear falsely shadows receivers whose light-space depth > 0.1.
                 glm::vec4( 1.0f ), RenderPassOrder::Default,
                 // And the DEPTH ATTACHMENT to 1.0 as well, overriding the engine's reversed-Z clear of
                 // 0. This pass is standard-Z (SetupShadowPass says why); a 0 clear under its LessOrEqual
                 // test would reject every caster and hand back an empty shadow map, silently.
                 1.0f );
        }
    }

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
        rspec.DebugName           = "OverdrawResolvePipeline";
        rspec.Shader              = m_OverdrawResolveShader;
        rspec.Framebuffer         = targetFb;
        rspec.DepthTestEnabled    = false;
        rspec.DepthWriteEnabled   = false;
        rspec.UseLoadRenderPass   = true;
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
                 static const int kEdges[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
                                                    { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
                                                    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
                 const glm::vec4 color( m_BoundingBoxColor, 1.0f );

                 std::vector<MaterialDebugLine::LineVertex> lines;
                 for ( const auto& rd : m_StaticQueue )
                 {
                     if ( !rd.Mesh )
                         continue;
                     for ( const auto& sm : rd.Mesh->GetSubmeshes() )
                     {
                         const glm::vec3 mn = sm.BoundingBox.Min;
                         const glm::vec3 mx = sm.BoundingBox.Max;
                         glm::vec3       c[8] = { { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
                                                  { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z },
                                                  { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
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
                 Renderer::GetInstance().SubmitLines( m_DebugLinePipeline.get(),
                                                      static_cast<uint32_t>( lines.size() ),
                                                      m_BoundingBoxLineWidth,
                                                      m_DebugLineMaterial->GetMaterialExecutor() );
             },
             m_DebugLinePipeline->GetSpecification(), targetFb,
             { RenderPassDependency( RenderPhase::Geometry ) } );
    }

    void MeshRenderer::RegisterSilhouettePass( RenderGraphBuilder& builder )
    {
        // The mask target AND the pipeline that writes it: the second half is new, because
        // SetupSilhouettePass can now refuse and this function reads the pipeline's spec.
        if ( !m_SilhouetteMaskFramebuffer || !m_SilhouettePipeline )
            return;

        builder.AddPass( "MeshSilhouettePass", RenderPhase::Outline,
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

                                 renderer.RenderMesh( m_SilhouettePipeline.get(), renderData.Mesh,
                                                      renderData.Transform,
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
                                     outlineBones.insert( outlineBones.end(), sd.BoneMatrices.begin(),
                                                          sd.BoneMatrices.end() );
                                 }
                                 if ( !outlined.empty() )
                                 {
                                     m_SilhouetteSkinnedMaterial->UpdateCamera( camera );
                                     m_SilhouetteSkinnedMaterial->UploadBones( outlineBones );
                                     for ( const auto& [sd, boneOffset] : outlined )
                                     {
                                         m_SilhouetteSkinnedMaterial->SetBoneOffset( boneOffset );
                                         renderer.RenderMesh( m_SilhouetteSkinnedPipeline.get(), sd->Mesh,
                                                              sd->Transform,
                                                              m_SilhouetteSkinnedMaterial->GetMaterialExecutor() );
                                     }
                                 }
                             }
                         },
                         m_SilhouettePipeline->GetSpecification(), m_SilhouetteMaskFramebuffer,
                         { RenderPassDependency( RenderPhase::Geometry ) } );
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
                skinnedData.Mesh         = static_cast<SkinnedMesh*>( data.Mesh );
                skinnedData.Transform    = data.Transform;
                skinnedData.BoneMatrices = data.BoneMatrices;
                skinnedData.Outlined     = data.Outlined;
                skinnedData.CastShadows  = data.CastShadows;
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
